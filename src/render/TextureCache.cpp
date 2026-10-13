#include "render/TextureCache.hpp"
#include "render/ShaderMeshRenderer.hpp"
#include <material/ProcedureStorage.hpp>
#include <model/EngineArithmetic.hpp>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

namespace neomdl::render {
namespace {
namespace g=gl;
constexpr g::Enum luminance=0x1909,rgb=0x1907,rgba=0x1908,
    ubyte=0x1401,unpackBuffer=0x88EC,unpackBinding=0x88EF,
    bc1=0x83F0,bc3=0x83F3;
bool samplerSpace(char ch) {
    return ch==' '||ch=='\t'||ch=='\r'||ch=='\n'||ch=='\v'||ch=='\f';
}
void engineSamplerFields(neoshared::material::TxiMetadata& metadata) {
    auto& properties=metadata.properties;
    properties.mipmap=true;properties.filter=true;properties.clamp=0;
    const auto& text=metadata.sourceText;
    for(std::size_t begin=0;begin<text.size();) {
        const auto end=text.find('\n',begin);
        auto line=std::string_view(text).substr(begin,end==std::string::npos?end:end-begin);
        line=line.substr(0,line.find('\0'));
        while(!line.empty()&&(line.front()==' '||line.front()=='\t'))line.remove_prefix(1);
        const auto keyEnd=line.find_first_of(" \t\r\n");
        auto key=std::string(line.substr(0,keyEnd));
        for(auto& ch:key)if(ch>='A'&&ch<='Z')ch=char(ch+('a'-'A'));
        auto value=line.substr(keyEnd==std::string_view::npos?line.size():keyEnd);
        while(!value.empty()&&samplerSpace(value.front()))value.remove_prefix(1);
        if(key=="mipmap"||key=="filter") {
            const auto stop=std::find_if(value.begin(),value.end(),samplerSpace);
            auto token=std::string(value.begin(),stop);
            // Parse(bool&) scans into an eight-byte temporary. Undefined engine
            // overflows are rejected; bounded unrecognized tokens retain state.
            if(token.size()>7u)throw std::invalid_argument("Engine texture boolean token exceeds its bounded sampler domain");
            for(auto& ch:token)if(ch>='A'&&ch<='Z')ch=char(ch+('a'-'A'));
            auto& field=key=="mipmap"?properties.mipmap:properties.filter;
            if(token=="1"||token=="true")field=true;
            else if(token=="0"||token=="false")field=false;
        } else if(key=="clamp") {
            bool negative=false;
            if(!value.empty()&&(value.front()=='-'||value.front()=='+')) {
                negative=value.front()=='-';value.remove_prefix(1);
            }
            std::uint64_t number=0;std::size_t digits=0;
            const auto limit=negative?std::uint64_t(2147483648u):std::uint64_t(2147483647u);
            for(const char ch:value) {
                if(ch<'0'||ch>'9')break;
                number=number*10u+unsigned(ch-'0');++digits;
                if(number>limit)throw std::invalid_argument("Engine texture clamp exceeds its defined 32-bit sampler domain");
            }
            // Failed Parse(short&) scans copy an uninitialized temporary.
            if(!digits)throw std::invalid_argument("Engine texture clamp has no initialized numeric value");
            const auto signedValue=negative?-static_cast<std::int64_t>(number):static_cast<std::int64_t>(number);
            const auto low=static_cast<std::uint16_t>(signedValue);
            properties.clamp=static_cast<std::int16_t>(low>=0x8000u?int(low)-0x10000:int(low));
        }
        if(end==std::string::npos)break;
        begin=end+1u;
    }
}
std::uint16_t engineTextureHash(const neoshared::texture::TextureData& texture) {
    // AurTextureCalculateHashID, zero palette-scheme entries: K1 Desktop 0x36cfdc,
    // K2 Desktop 0xbbf7f. Resources use ASCII ResRefs.
    const auto name=neoshared::material::textureResourceKey(texture.sourcePath.stem().u8string());
    std::uint32_t hash=0;
    for(const unsigned char c:name)hash=(hash?hash*c:c)%65101u;
    return static_cast<std::uint16_t>(hash);
}
std::size_t processedLevelBytes(const neoshared::texture::ProcessedTextureLevel& level,
        neoshared::texture::ProcessedCompressionCodec compression,unsigned bytesPerPixel) {
    if(compression==neoshared::texture::ProcessedCompressionCodec::None)
        return std::size_t(level.width)*std::size_t(level.height)*bytesPerPixel;
    const auto blocksWide=std::max<std::uint32_t>(1u,(level.width+3u)/4u);
    const auto blocksHigh=std::max<std::uint32_t>(1u,(level.height+3u)/4u);
    const auto blockBytes=compression==neoshared::texture::ProcessedCompressionCodec::Bc1?8u:16u;
    return std::size_t(blocksWide)*std::size_t(blocksHigh)*blockBytes;
}
g::Enum processedFormat(neoshared::texture::ProcessedSourceEncoding encoding) {
    return encoding==neoshared::texture::ProcessedSourceEncoding::Gray8?luminance:
           encoding==neoshared::texture::ProcessedSourceEncoding::Rgb8?rgb:rgba;
}
g::Enum processedInternal(neoshared::texture::ProcessedCompressionCodec compression,
        neoshared::texture::ProcessedSourceEncoding encoding) {
    return compression==neoshared::texture::ProcessedCompressionCodec::Bc1?bc1:
           compression==neoshared::texture::ProcessedCompressionCodec::Bc3?bc3:
           processedFormat(encoding);
}
struct UploadState {
    g::Api& a;g::Int active{},twoD{},cube{},threeD{},unpack{},alignment{},row{},skipRows{},skipPixels{},imageHeight{},skipImages{},swapBytes{},lsbFirst{};
    explicit UploadState(g::Api& api):a(api) {
        a.GetIntegerv(g::ActiveTexture,&active);a.ActiveTexture(g::Texture0);
        a.GetIntegerv(g::TextureBinding2D,&twoD);a.GetIntegerv(g::TextureBindingCube,&cube);
        a.GetIntegerv(0x806A,&threeD);a.GetIntegerv(0x806E,&imageHeight);a.GetIntegerv(0x806D,&skipImages);
        a.GetIntegerv(0x0CF0,&swapBytes);a.GetIntegerv(0x0CF1,&lsbFirst);
        a.PixelStorei(0x0CF0,0);a.PixelStorei(0x0CF1,0);
        a.GetIntegerv(unpackBinding,&unpack);a.GetIntegerv(0x0CF5,&alignment);
        a.GetIntegerv(0x0CF2,&row);a.GetIntegerv(0x0CF3,&skipRows);a.GetIntegerv(0x0CF4,&skipPixels);
        a.BindBuffer(unpackBuffer,0);a.PixelStorei(0x0CF5,1);a.PixelStorei(0x0CF2,0);
        a.PixelStorei(0x0CF3,0);a.PixelStorei(0x0CF4,0);a.PixelStorei(0x806E,0);a.PixelStorei(0x806D,0);
    }
    ~UploadState() {
        a.PixelStorei(0x0CF0,swapBytes);a.PixelStorei(0x0CF1,lsbFirst);
        a.BindBuffer(unpackBuffer,static_cast<g::UInt>(unpack));a.PixelStorei(0x0CF5,alignment);
        a.PixelStorei(0x0CF2,row);a.PixelStorei(0x0CF3,skipRows);a.PixelStorei(0x0CF4,skipPixels);
        a.PixelStorei(0x806E,imageHeight);a.PixelStorei(0x806D,skipImages);
        a.BindTexture(0x806F,static_cast<g::UInt>(threeD));
        a.BindTexture(g::Texture2D,static_cast<g::UInt>(twoD));a.BindTexture(g::TextureCube,static_cast<g::UInt>(cube));
        a.ActiveTexture(static_cast<g::Enum>(active));
    }
};
}
bool TextureCache::initialize(const gl::Loader& loader,std::string& error) {
    if(ready_) return true;
    ready_=api_.load(loader,error);
    if(ready_) {
        const auto image3D=loader("glTexImage3D");
        static_assert(sizeof(image3D)==sizeof(texImage3D_),"Unexpected GL function-pointer ABI");
        std::memcpy(&texImage3D_,&image3D,sizeof(image3D));
        // The K2 processed callers dispatch this exact ARB entry point.
        // Absence is reported only when a block-compressed processed payload is
        // actually selected; no core-name or decoded-pixel fallback is used.
        const auto compressed2D=loader("glCompressedTexImage2DARB");
        static_assert(sizeof(compressed2D)==sizeof(compressedTexImage2D_),"Unexpected GL function-pointer ABI");
        std::memcpy(&compressedTexImage2D_,&compressed2D,sizeof(compressed2D));
    }
    return ready_;
}
GpuTexture TextureCache::uploadProcessed(const neoshared::texture::TextureData& texture,
                            const neoshared::material::TextureMetadata* metadata,
                            std::size_t layer,std::string& error,bool engineProcedure) {
    error.clear();
    if(!ready_){error="Texture upload context is not initialized";return {};}
    if(!texture.processedPayload){error="Processed upload requested without retained engine payload";return {};}
    const auto& payload=*texture.processedPayload;
    const bool cube=payload.target==neoshared::texture::ProcessedTextureTarget::CubeMap;
    if(cube)layer=0;
    const auto found=entries_.find(&texture);
    if(found!=entries_.end()) {
        const auto cached=found->second.find(layer);if(cached!=found->second.end())return cached->second;
    }
    if((cube&&payload.surfaces.size()!=6u)||(!cube&&layer>=payload.surfaces.size())) {
        error=cube?"Processed cubemap does not contain six direct face surfaces":
                   "Processed texture frame is outside retained source surfaces";
        return {};
    }
    if(payload.compression!=neoshared::texture::ProcessedCompressionCodec::None&&!compressedTexImage2D_) {
        error="Engine processed block upload requires glCompressedTexImage2DARB";return {};
    }
    if(api_.GetError()!=g::NoError){error="Pending OpenGL error before processed texture upload";return {};}
    const auto& firstSurface=payload.surfaces[cube?0u:layer];
    if(firstSurface.levels.empty()){error="Processed source contains no base level";return {};}
    const auto& first=firstSurface.levels.front();
    g::Int maximum{};api_.GetIntegerv(cube?0x851Cu:0x0D33u,&maximum);
    if(maximum<=0||!first.width||!first.height||first.width>unsigned(maximum)||first.height>unsigned(maximum)) {
        error="Processed source dimensions exceed the current GL context";return {};
    }
    const auto checkSurface=[&](const neoshared::texture::ProcessedTextureSurface& surface,
                                std::uint32_t baseWidth,std::uint32_t baseHeight) {
        if(surface.levels.empty())return false;
        std::uint32_t width=baseWidth,height=baseHeight;
        for(const auto& level:surface.levels) {
            if(level.width!=width||level.height!=height||
               level.bytes.size()!=processedLevelBytes(level,payload.compression,payload.bytesPerPixel))return false;
            width=std::max<std::uint32_t>(1u,width/2u);
            height=std::max<std::uint32_t>(1u,height/2u);
        }
        return true;
    };
    if(cube) {
        if(first.width!=first.height){error="Processed cubemap faces are not square";return {};}
        for(const auto& surface:payload.surfaces)
            if(surface.levels.size()!=firstSurface.levels.size()||!checkSurface(surface,first.width,first.height)) {
                error="Processed cubemap face/mipmap boundaries are inconsistent";return {};
            }
    } else if(!checkSurface(firstSurface,first.width,first.height)) {
        error="Processed texture mipmap boundaries are inconsistent";return {};
    }
    UploadState restore(api_);
    GpuTexture result;result.cube=cube;result.width=first.width;result.height=first.height;
    result.order=orderFor(texture,layer);
    api_.GenTextures(1,&result.id);
    if(!result.id){error="OpenGL could not allocate a processed texture";return {};}
    const auto target=cube?g::TextureCube:g::Texture2D;
    api_.BindTexture(target,result.id);
    const auto txi=metadata?metadata->txi:neoshared::material::makeTextureMetadata(texture).txi;
    const bool filter=txi.properties.filter.value_or(true);
    const bool mipmap=txi.properties.mipmap.value_or(true);
    if(cube) {
        api_.TexParameteri(target,0x2802,0x812F);api_.TexParameteri(target,0x2803,0x812F);
        api_.TexParameteri(target,0x8072,0x812F);
    } else {
        const auto clamp=txi.properties.clamp.value_or(0);
        api_.TexParameteri(target,0x2802,(clamp&1)?0x2900:0x2901);
        api_.TexParameteri(target,0x2803,(clamp&2)?0x2900:0x2901);
    }
    api_.TexParameteri(target,0x2800,filter?0x2601:0x2600);
    api_.TexParameteri(target,0x2801,mipmap?(filter?0x2703:0x2700):(filter?0x2601:0x2600));
    const auto uploadSurface=[&](const neoshared::texture::ProcessedTextureSurface& surface,g::Enum imageTarget) {
        const auto levelCount=mipmap?surface.levels.size():std::size_t(1);
        for(std::size_t mip=0;mip<levelCount;++mip) {
            const auto& level=surface.levels[mip];
            if(payload.compression==neoshared::texture::ProcessedCompressionCodec::None) {
                const auto format=processedFormat(payload.encoding);
                api_.TexImage2D(imageTarget,gl::Int(mip),gl::Int(format),gl::Size(level.width),gl::Size(level.height),
                                0,format,ubyte,level.bytes.data());
            } else {
                compressedTexImage2D_(imageTarget,gl::Int(mip),processedInternal(payload.compression,payload.encoding),
                    gl::Size(level.width),gl::Size(level.height),0,gl::Size(level.bytes.size()),level.bytes.data());
            }
        }
    };
    try {
        if(cube)for(unsigned face=0;face<6u;++face) {
            // CreateProcessedCubeTexture advances compressed face spans even
            // with mipmap 0. Its raw branch omits that advance and repeats the
            // first face for all six targets.
            const auto& surface=!mipmap&&payload.compression==neoshared::texture::ProcessedCompressionCodec::None?
                payload.surfaces.front():payload.surfaces[face];
            uploadSurface(surface,0x8515u+face);
        }
        else uploadSurface(firstSurface,g::Texture2D);
    } catch(...) {api_.DeleteTextures(1,&result.id);throw;}
    if(api_.GetError()!=g::NoError) {
        api_.DeleteTextures(1,&result.id);error="OpenGL processed texture upload failed";return {};
    }
    (void)engineProcedure;
    entries_[&texture].emplace(layer,result);return result;
}

GpuTexture TextureCache::upload(const neoshared::texture::TextureData& texture,
                            const neoshared::material::TextureMetadata* metadata,
                            std::size_t layer,std::string& error,bool engineProcedure) {
    error.clear();
    if(!ready_) {error="Texture upload context is not initialized";return {};}
    if(const auto found=entries_.find(&texture);found!=entries_.end()) {
        const auto first=found->second.find(0u);
        if(first!=found->second.end()&&first->second.cube)return first->second;
        if(const auto cached=found->second.find(layer);cached!=found->second.end())return cached->second;
    }
    if(texture.processedPayload)return uploadProcessed(texture,metadata,layer,error,engineProcedure);
    if(engineRuntime_) {
        // GLRender::CreateTexture and CreateCubeTexture consume the engine
        // source component layout. Decoded RGBA
        // is the editor representation, not a replacement upload encoding.
        const auto txi=metadata?metadata->txi:neoshared::material::parseMaterialTxi(texture.txi);
        const bool cube=texture.cubeMap||txi.properties.cube.value_or(false);
        if(cube)layer=0;
        if(layer>=texture.layers.size()){error="Texture frame is outside decoded layers";return {};}
        if(api_.GetError()!=g::NoError){error="Pending OpenGL error before texture upload";return {};}
        g::Int maximum{},gluMaximum{};
        api_.GetIntegerv(cube?0x851cu:0x0d33u,&maximum);
        // gluBuild2DMipmaps 0x8fe505 queries MAX_TEXTURE_SIZE even for cube
        // targets, then independently limits each nearest-power dimension.
        api_.GetIntegerv(0x0d33u,&gluMaximum);
        if(maximum<=0){error="Texture dimensions/storage exceed the current GL context";return {};}
        const bool mip=txi.properties.mipmap.value_or(true);
        const bool normal=!cube&&material::procedureDefinition(txi,engineGame_).bumpTexture;
        const bool automatic=mip&&!normal&&!cube&&automaticMipmaps();
        std::vector<std::vector<material::ProcedureImage>> plans;
        std::vector<bool> gluPlans;
        try {
            material::ProcedureDefinition sourceDefinition;
            material::ProcedureImage source;
            if(cube)source=material::procedureSourceImage(texture,sourceDefinition,true);
            else {
                // The decoder/controller has already selected this frame's
                // engine TID. Keep its source facts while selecting its pixels.
                auto frame=texture;
                frame.layers={texture.layers[layer]};frame.animated=false;
                source=material::procedureSourceImage(frame,sourceDefinition,false);
            }
            auto faces=material::procedureTidImages(source,1,1,cube).front();
            for(auto& face:faces) {
                const bool glu=mip&&!normal&&(cube||(!automatic&&face.bytesPerPixel!=2));
                if(normal)plans.push_back(material::procedureNormalLevels(face,
                    txi.properties.bumpMapScaling.value_or(1.0f),
                    txi.properties.bumpMapKind==neoshared::material::BumpMapKind::AuthoredNormal));
                else if(glu)plans.push_back(material::procedureMipLevels(face,
                    material::ProcedureMipKernel::GluByte,unsigned(gluMaximum)));
                else if(mip&&!automatic)plans.push_back(material::procedureMipLevels(face,material::ProcedureMipKernel::AurUpdate));
                else plans.push_back({std::move(face)});
                gluPlans.push_back(glu);
                const auto& base=plans.back().front();
                if(base.width>unsigned(maximum)||base.height>unsigned(maximum)) {
                    error="Texture dimensions/storage exceed the current GL context";return {};
                }
            }
        } catch(const std::exception& exception){error=exception.what();return {};}
        UploadState restore(api_);
        GpuTexture result;result.cube=cube;result.engineNormal=normal;
        result.width=plans.front().front().width;result.height=plans.front().front().height;
        result.order=orderFor(texture,layer);
        api_.GenTextures(1,&result.id);
        if(!result.id){error="OpenGL could not allocate a texture";return {};}
        const auto target=cube?g::TextureCube:g::Texture2D;
        api_.BindTexture(target,result.id);
        const bool filter=txi.properties.filter.value_or(true);
        if(!cube)api_.TexParameteri(target,0x8191,automatic?1:0);
        if(normal) {
            // BuildTextureNormalMap returns before the ordinary TXI sampler.
            api_.TexParameteri(target,0x2802,0x2901);api_.TexParameteri(target,0x2803,0x2901);
            api_.TexParameteri(target,0x2801,0x2703);api_.TexParameteri(target,0x2800,0x2601);
        } else {
            const auto clamp=txi.properties.clamp.value_or(0);
            api_.TexParameteri(target,0x2802,cube?0x812f:((clamp&1)?0x2900:0x2901));
            api_.TexParameteri(target,0x2803,cube?0x812f:((clamp&2)?0x2900:0x2901));
            if(cube)api_.TexParameteri(target,0x8072,0x812f);
            api_.TexParameteri(target,0x2800,filter?0x2601:0x2600);
            api_.TexParameteri(target,0x2801,mip?(filter?0x2703:0x2700):(filter?0x2601:0x2600));
        }
        for(std::size_t face=0;face<plans.size();++face)for(std::size_t level=0;level<plans[face].size();++level) {
            const auto& p=plans[face][level];
            const bool packed=!normal&&!cube&&p.bytesPerPixel==2;
            const auto format=normal?0x80e1u:p.bytesPerPixel==4?0x1908u:p.bytesPerPixel==3?0x1907u:
                p.bytesPerPixel==2?(cube?0x190au:0x1908u):0x1909u;
            // The embedded GLU entry point converts internal component counts
            // three/four to RGB/RGBA (0x8fe475..0x8fe4ae). Counts one/two survive.
            const auto internal=normal?0x8058u:packed?4u:
                gluPlans[face]&&p.bytesPerPixel>=3?format:p.bytesPerPixel;
            api_.TexImage2D(cube?0x8515u+g::Enum(face):target,g::Int(level),g::Int(internal),
                g::Size(p.width),g::Size(p.height),0,format,packed?0x8033u:0x1401u,p.bytes.data());
        }
        if(api_.GetError()!=g::NoError) {
            api_.DeleteTextures(1,&result.id);error="OpenGL texture upload failed";return {};
        }
        entries_[&texture].emplace(layer,result);return result;
    }
    if(texture.cubeMap) layer=0;
    if(layer>=texture.layers.size()) {error="Texture frame is outside decoded layers";return {};}
    auto& a=api_;
    if(a.GetError()!=g::NoError) {error="Pending OpenGL error before texture upload";return {};}
    g::Int maximum=0;a.GetIntegerv(texture.cubeMap ? 0x851Cu : 0x0D33u,&maximum);
    const auto& first=texture.layers[layer];
    const auto valid=[&](const neoshared::texture::TextureLayer& image) {
        return image.width>0 && image.height>0 && maximum>0 &&
            image.width<=static_cast<unsigned>(maximum) && image.height<=static_cast<unsigned>(maximum) &&
            static_cast<std::uint64_t>(image.width)*image.height*4u==image.rgba.size();
    };
    if(!valid(first)) {error="Texture dimensions/storage exceed the current GL context";return {};}
    if(texture.cubeMap) {
        if(texture.layers.size()!=6 || texture.cubeFaces.size()!=6 || first.width!=first.height) {
            error="Environment cubemap requires six named square faces";return {};
        }
        std::array<bool,6> faces{};
        for(std::size_t n=0;n<6;++n) {
            const auto index=static_cast<std::size_t>(texture.cubeFaces[n]);const auto& image=texture.layers[n];
            if(index>=6 || faces[index] || !valid(image) || image.width!=first.width || image.height!=first.height) {
                error="Environment cubemap has duplicate/missing/mismatched faces";return {};
            }
            faces[index]=true;
        }
    }
    UploadState restore(a);
    GpuTexture result;result.cube=texture.cubeMap;result.width=first.width;result.height=first.height;
    result.order=orderFor(texture,layer);
    a.GenTextures(1,&result.id);
    if(!result.id) {error="OpenGL could not allocate a texture";return {};}
    const auto target=texture.cubeMap ? g::TextureCube : g::Texture2D;
    a.BindTexture(target,result.id);
    const bool filter=!metadata || metadata->txi.properties.filter.value_or(true);
    const bool clamp=texture.cubeMap || (metadata && metadata->txi.properties.clamp.value_or(0)!=0);
    if(engineProcedure) {
        // GLRender::CreateTexture (Desktop 0x8cfe6 / 0x8d1ba): the supported
        // compatibility-GL upload takes its capability-gated SGI mip branch.
        // A context rejecting GL_GENERATE_MIPMAP is diagnosed, not replaced
        // with an unverified CPU/GLU downsampler. Clamp bits select S/T alone.
        const bool mipmap=metadata && metadata->txi.properties.mipmap.value_or(true);
        const auto bits=metadata?metadata->txi.properties.clamp.value_or(0):0;
        a.TexParameteri(target,0x8191,mipmap?1:0);
        a.TexParameteri(target,0x2801,mipmap?(filter?0x2703:0x2700):(filter?0x2601:0x2600));
        a.TexParameteri(target,0x2800,filter?0x2601:0x2600);
        a.TexParameteri(target,0x2802,(bits&1)?0x2900:0x2901);
        a.TexParameteri(target,0x2803,(bits&2)?0x2900:0x2901);
    } else {
        a.TexParameteri(target,0x2801,filter?0x2601:0x2600);a.TexParameteri(target,0x2800,filter?0x2601:0x2600);
        a.TexParameteri(target,0x2802,clamp?0x812F:0x2901);a.TexParameteri(target,0x2803,clamp?0x812F:0x2901);
    }
    if(texture.cubeMap) a.TexParameteri(target,0x8072,0x812F);
    const auto upload=[&](const neoshared::texture::TextureLayer& image,g::Enum imageTarget) {
        // Shared images use top-left editing orientation; our GL UVs are lower-left.
        std::vector<std::uint8_t> flipped(image.rgba.size());
        const auto rowBytes=static_cast<std::size_t>(image.width)*4;
        for(std::uint32_t y=0;y<image.height;++y)
            std::copy_n(image.rgba.data()+static_cast<std::size_t>(y)*rowBytes,rowBytes,
                flipped.data()+static_cast<std::size_t>(image.height-1-y)*rowBytes);
        a.TexImage2D(imageTarget,0,rgba,static_cast<g::Size>(image.width),static_cast<g::Size>(image.height),0,rgba,ubyte,flipped.data());
    };
    try {
        if(texture.cubeMap) for(std::size_t n=0;n<6;++n)
            upload(texture.layers[n],0x8515u+static_cast<unsigned>(texture.cubeFaces[n]));
        else upload(first,target);
    } catch(...) {a.DeleteTextures(1,&result.id);throw;}
    if(a.GetError()!=g::NoError) {a.DeleteTextures(1,&result.id);error="OpenGL texture upload failed";return {};}
    entries_[&texture].emplace(layer,result);return result;
}
void TextureCache::setEngineK2(bool enabled) noexcept {
    engineRuntime_=enabled;engineGame_=material::EngineMaterialGame::Kotor2;
}
void TextureCache::setEngineRuntime(material::EngineMaterialGame game) noexcept {
    engineGame_=game;
    engineRuntime_=true;
}
const material::ProcedureDefinition& TextureCache::definitionFor(
        const neoshared::texture::TextureData& texture,
        const neoshared::material::TextureMetadataPtr& metadata,material::EngineMaterialGame game) {
    const auto found=definitions_.find(&texture);
    if(found!=definitions_.end()&&found->second.metadata==metadata&&found->second.game==game)
        return found->second.definition;
    DefinitionRecord record{metadata,game,material::procedureDefinition(metadata->txi,game)};
    return definitions_.insert_or_assign(&texture,std::move(record)).first->second.definition;
}
bool TextureCache::makeCurrent(const std::vector<ProcedureRegistration>& requests,
                               std::string& error) {
    error.clear();
    std::unordered_set<const neoshared::texture::TextureData*> seen;
    std::vector<const ProcedureRegistration*> pending;
    pending.reserve(requests.size());
    for(const auto& request:requests)if(seen.insert(request.source).second) {
        orderFor(*request.source);
        if(procedures_.find(request.source)==procedures_.end())
            pending.push_back(&request);
    }
    // BuildAndStoreAll: K1 Desktop 0x370630; K2 Desktop 0xbe5af.
    // This list differs from ForceControllerUpdate's ascending hash list.
    for(auto i=pending.rbegin();i!=pending.rend();++i) {
        const auto& request=**i;std::string failure;
        get(*request.source,request.metadata.get(),0,failure);
        if(error.empty()&&!failure.empty())
            error=request.source->sourcePath.stem().u8string()+": "+failure;
    }
    return error.empty();
}
GpuTexture TextureCache::get(const neoshared::texture::TextureData& texture,
                            const neoshared::material::TextureMetadata* metadata,
                            std::size_t layer,std::string& error) {
    error.clear();
    if(!ready_){error="Texture upload context is not initialized";return {};}
    if(const auto failed=failures_.find(&texture);failed!=failures_.end()){error=failed->second;return {};}
    if(const auto found=procedures_.find(&texture);found!=procedures_.end()) {
        return selected(*found->second,error);
    }
    if(const auto found=entries_.find(&texture);found!=entries_.end()) {
        const auto first=found->second.find(0u);
        if(first!=found->second.end()&&first->second.cube)return first->second;
        if(const auto cached=found->second.find(layer);cached!=found->second.end())return cached->second;
    }
    try {
        // A supplied metadata object is the caller's authoritative state.
        // Implicit resource metadata needs CAurTextureBasic's physical-line
        // sampler scan, which differs from the shared editor's CR tokenizer.
        std::optional<neoshared::material::TextureMetadata> implicitMetadata;
        if(!metadata&&engineRuntime_) {
            implicitMetadata=neoshared::material::makeTextureMetadata(texture);
            engineSamplerFields(implicitMetadata->txi);
            metadata=&*implicitMetadata;
        }
        const auto definition=material::procedureDefinition(metadata?metadata->txi:neoshared::material::parseMaterialTxi(texture.txi),engineGame_);
        if(definition.volumeTexture && (!definition.type ||
           (*definition.type!=neoshared::material::ProcedureType::Dirty && *definition.type!=neoshared::material::ProcedureType::Dirty2 && *definition.type!=neoshared::material::ProcedureType::Dirty3)))
            throw std::invalid_argument("A prior Dirty procedure left a engine 3D texture flag with no matching volume consumer");
        if(definition.volumeTexture)dirtySources_.insert(&texture);
        if(!definition.type)return upload(texture,metadata,layer,error);
        if(!engineRuntime_)throw std::invalid_argument("Procedural runtime requires a verified game and platform controller");
        const bool cube=texture.cubeMap || (metadata && metadata->txi.properties.cube.value_or(false)) ||
            (!metadata && neoshared::material::parseMaterialTxi(texture.txi).properties.cube.value_or(false));
        const auto stream=neoshared::texture::scanTxiStream(metadata?metadata->txi.sourceText:texture.txi);
        if(std::any_of(stream.begin(),stream.end(),[](const auto& row){return row.binary;}) &&
           texture.txiStorage!=neoshared::texture::TxiStorage::Sidecar)
            throw std::invalid_argument("Engine packed channel arrays require a first-load sidecar AurRes cursor; embedded/cached TXI is not that stream");
        if(cube&&definition.volumeTexture)
            throw std::invalid_argument("Dirty volume and procedural cube flags select incompatible engine storage");
        if(definition.volumeTexture&&(definition.numX!=1||definition.numY!=1))
            throw std::invalid_argument("Engine multi-TID volume crops have no established complete 3D source span");
        const bool cycle=*definition.type==neoshared::material::ProcedureType::Cycle;
        const auto storage=material::procedureStorageDescriptor(texture,definition,cube);
        const bool processed=storage.provenance==material::ProcedureStorageProvenance::ProcessedPayload;
        const bool blockCompressed=storage.compression!=material::ProcedureCompressionCodec::None;
        const bool sourceCompressed=texture.compressed;
        const unsigned nx=unsigned(definition.numX),ny=unsigned(definition.numY),total=nx*ny;
        if(processed&&((storage.target==material::ProcedureTextureTarget::CubeMap)!=cube))
            throw std::invalid_argument("Processed source target disagrees with the engine 2D/cubemap caller");
        if(sourceCompressed&&!processed&&!cycle)
            throw std::invalid_argument("Engine block-compressed texture skips controller Init; no decoded-pixel substitute is executed");
        if(processed&&blockCompressed&&!cycle)
            throw std::invalid_argument("Engine processed block upload has no writable controller output encoding");
        if(texture.layers.empty())throw std::invalid_argument("Procedural source has no initial image");
        if(sourceCompressed&&!processed&&(cube||definition.bumpTexture))
            throw std::invalid_argument("Compressed decoded cube/normal controller dispatch has no retained engine source payload");
        if(processed&&cube&&total!=1u)
            throw std::invalid_argument("Processed cubemap caller has one texture ID; multi-ID face packing is not established");
        if(processed&&cube&&definition.bumpTexture)
            throw std::invalid_argument("Processed cubemap caller has no engine normal-conversion dispatch");
        const auto processedDirectCycle=processed&&cycle&&(
            (cube&&total==1u)||(!cube&&texture.processedPayload->surfaces.size()==total));
        const auto decodedCompressedCycle=!processed&&sourceCompressed&&cycle;
        if(processed&&cycle&&!processedDirectCycle)
            throw std::invalid_argument("Processed Cycle TID count disagrees with the retained engine source surfaces");
        const bool directCycle=processedDirectCycle||decodedCompressedCycle;
        auto record=std::make_unique<ProcedureRecord>();record->source=&texture;
        record->metadata=metadata?*metadata:neoshared::material::makeTextureMetadata(texture);
        auto& r=*record;r.storage=storage;r.engineHash=engineTextureHash(texture);
        r.metadata.txi.properties.mipmap=r.metadata.txi.properties.mipmap.value_or(true);
        if(processed&&!cycle&&!definition.volumeTexture&&r.metadata.txi.properties.mipmap.value_or(true)) {
            for(const auto& surface:texture.processedPayload->surfaces)
                if(surface.levels.size()!=1u)
                    throw std::invalid_argument("Processed controller override supplies only the base image; a source-provided mip chain would overrun that replacement span");
        }
        if(definition.volumeTexture && r.metadata.txi.properties.mipmap.value_or(true))
            throw std::invalid_argument("Dirty 3D upload requires mipmap 0: both engine mipmapped branches dispatch to 2D upload/mipmap helpers, not 3D storage");
        r.cube=cube;r.cubeLayout=processed&&cube?material::ProcedureCubeLayout::ProcessedDirect:
                                                    material::ProcedureCubeLayout::OrdinaryMapped;
        r.pixelStorage=!blockCompressed&&!definition.volumeTexture&&!directCycle;
        r.normalUpload=definition.bumpTexture&&!cube; // Cube helper never calls the normal converter.
        if(r.normalUpload && definition.volumeTexture)
            throw std::invalid_argument("Dirty volume cannot also use the engine 2D normal-map upload consumer");
        if(r.normalUpload||r.pixelStorage)r.uploadedRevision=std::numeric_limits<std::uint64_t>::max();
        orderFor(texture);
        material::ProcedureImage initial;
        const auto& first=texture.layers.front();
        if(!sourceCompressed || (processed && !blockCompressed))initial=material::procedureSourceImage(texture,definition,cube);
        else {
            initial.width=first.width;initial.height=first.height;
            const auto count=std::uint64_t(first.width)*first.height;
            if(!first.width||!first.height||count>first.rgba.max_size()/4u||first.rgba.size()!=count*4u)
                throw std::invalid_argument("Invalid block-compressed cycle source dimensions/storage");
            initial.bytes.resize(std::size_t(count)*4);
            for(unsigned y=0;y<first.height;++y)std::copy_n(first.rgba.data()+std::size_t(first.height-1-y)*first.width*4,
                std::size_t(first.width)*4,initial.bytes.data()+std::size_t(y)*first.width*4);
        }
        if(r.pixelStorage) {
            const bool mip=r.metadata.txi.properties.mipmap.value_or(true);
            const bool runtimeMips=r.storage.mipOwnership==material::ProcedureMipOwnership::RuntimeGenerated;
            if(initial.bytesPerPixel==2&&!cube&&!cycle&&!r.normalUpload&&!mip)
                throw std::invalid_argument("Engine non-mipmapped packed-4444 update passes RGBA/UBYTE over a two-byte image (overread)");
            if(*definition.type==neoshared::material::ProcedureType::Arturo&&r.normalUpload&&
               r.metadata.txi.properties.bumpMapKind==neoshared::material::BumpMapKind::AuthoredNormal)
                throw std::invalid_argument("Engine Arturo supplies one height byte per pixel to an authored-normal reader that requires four (overread)");
            if(*definition.type==neoshared::material::ProcedureType::Arturo&&definition.bumpTexture&&initial.bytesPerPixel!=1&&
               (cube||definition.numX!=1||definition.numY!=1))
                throw std::invalid_argument("Engine Arturo height pointer is shorter than the tiled/cube BPP source span");
            // All crop, normal and explicit mip domains are validated BEFORE
            // controller initialization can consume the shared scene RNG.
            const auto tids=material::procedureTidImages(initial,nx,ny,cube,r.cubeLayout);
            for(const auto& faces:tids)for(const auto& face:faces) {
                if(r.normalUpload)(void)material::procedureNormalLevels(face,r.metadata.txi.properties.bumpMapScaling.value_or(1.0f),
                    r.metadata.txi.properties.bumpMapKind==neoshared::material::BumpMapKind::AuthoredNormal);
                else if(runtimeMips&&mip&&(cube||!automaticMipmaps())) {
                    (void)material::procedureMipLevels(face,cube||initial.bytesPerPixel!=2?material::ProcedureMipKernel::GluByte:material::ProcedureMipKernel::AurUpdate);
                    if(!cycle&&!cube)(void)material::procedureMipLevels(face,material::ProcedureMipKernel::AurUpdate);
                }
            }
            if(r.normalUpload)r.storage.initialUpload=r.storage.updateUpload=material::ProcedureUploadAlgorithm::HeightToNormal;
            else if(processed)r.storage.initialUpload=r.storage.updateUpload=material::ProcedureUploadAlgorithm::DirectPixels;
            else if(runtimeMips&&mip&&cube)r.storage.initialUpload=r.storage.updateUpload=material::ProcedureUploadAlgorithm::PortableGluByte;
            else if(runtimeMips&&mip&&!automaticMipmaps())
                r.storage.initialUpload=r.storage.updateUpload=initial.bytesPerPixel==2?
                    material::ProcedureUploadAlgorithm::AurImageScale:material::ProcedureUploadAlgorithm::PortableGluByte;
            else if(runtimeMips&&mip)r.storage.initialUpload=r.storage.updateUpload=material::ProcedureUploadAlgorithm::SgiAutomatic;
        }
        if(directCycle) {
            r.tiled=true;r.frames=texture;r.frames.animated=total>1;
            if(cube) {
                // The processed cube caller owns one complete six-face texture.
                if(total!=1u)throw std::invalid_argument("Processed cubemap cycle has more than one texture ID");
            } else {
                r.frames.layers.clear();
                if(r.frames.processedPayload)r.frames.processedPayload->surfaces.clear();
                if(texture.layers.size()==total && (texture.animated||total==1)) {
                    for(unsigned frame=0;frame<total;++frame) {
                        // Strip decoders store top-left cells. Engine ImageGetSubImage
                        // uses the lower-left raw row origin. TPC frame blocks already
                        // have their own engine TID order and must not be reordered.
                        const bool strip=texture.kind==neoshared::texture::TextureFileKind::Tga ||
                                         texture.kind==neoshared::texture::TextureFileKind::Dds;
                        const unsigned src=strip?(ny-1-frame/nx)*nx+frame%nx:frame;
                        r.frames.layers.push_back(texture.layers[src]);
                        if(r.frames.processedPayload)
                            r.frames.processedPayload->surfaces.push_back(texture.processedPayload->surfaces.at(src));
                    }
                } else if(texture.layers.size()==1&&!processed) {
                    if(first.width%nx||first.height%ny||first.width<nx||first.height<ny)
                        throw std::invalid_argument("Cycle tile grid does not exactly divide the source image");
                    const auto w=first.width/nx,h=first.height/ny;
                    for(unsigned frame=0;frame<total;++frame) {
                        neoshared::texture::TextureLayer tile;tile.width=w;tile.height=h;tile.rgba.resize(std::size_t(w)*h*4);
                        const auto x=(frame%nx)*w,y=(ny-1-frame/nx)*h;
                        for(unsigned row=0;row<h;++row)
                            std::copy_n(first.rgba.data()+((std::size_t(y+row)*first.width)+x)*4,std::size_t(w)*4,tile.rgba.data()+std::size_t(row)*w*4);
                        r.frames.layers.push_back(std::move(tile));
                    }
                } else throw std::invalid_argument("Cycle TID count disagrees with retained source-frame storage");
            }
            // A private frame view is NOT a newly registered engine texture.
        }
        if(!r.controller.initialize(definition,initial,procedureContext_,error))throw std::invalid_argument(error);
        if(r.tiled)orders_[&r.frames]=orders_.at(&texture);
        auto* stored=record.get();procedures_.emplace(&texture,std::move(record));
        if(stored->controller.definition().volumeTexture || stored->normalUpload || stored->pixelStorage) {
            if(!uploadProcedure(*stored,error))return {};
        }
        return selected(*stored,error);
    } catch(const std::exception& e) {error=e.what();failures_[&texture]=error;return {};}
}
GpuTexture TextureCache::selected(ProcedureRecord& r,std::string& error) {
    if(!r.fault.empty()){error=r.fault;return {};}
    if(!r.controller.validTexture()) {error="Engine Life controller invalidated its TID without uploading its private cells";return {};}
    GpuTexture result;
    if(r.controller.definition().volumeTexture || r.normalUpload || r.pixelStorage) {
        if(!uploadProcedure(r,error))return {};
        result=entries_[r.source].at(r.pixelStorage?r.controller.frame():0u);
    } else {
        const auto& source=r.tiled?r.frames:*r.source;
        const auto layer=r.tiled?r.controller.frame():0u;
        result=upload(source,&r.metadata,layer,error,true);
        if(result.id) {
            result.order=orderFor(*r.source,layer);
            procedureIds_[result.id]=&r;
            if(!r.tiled&&r.uploadedRevision!=r.controller.revision()) {
                if(!uploadProcedure(r,error))return {};
            }
        }
    }
    return result;
}
bool TextureCache::uploadProcedure(ProcedureRecord& r,std::string& error) {
    if(r.pixelStorage)return uploadPixelTids(r,error);
    const auto& image=r.controller.image();const bool volume=r.controller.definition().volumeTexture;
    if(!volume && r.uploadedRevision==r.controller.revision())return true;
    auto found=entries_[r.source].find(0);bool created=found==entries_[r.source].end();
    if(!created&&r.uploadedRevision==r.controller.revision())return true;
    std::vector<material::ProcedureImage> normal;
    if(r.normalUpload) {
        try {normal=material::procedureNormalLevels(image,r.metadata.txi.properties.bumpMapScaling.value_or(1.0f),
            r.metadata.txi.properties.bumpMapKind==neoshared::material::BumpMapKind::AuthoredNormal);}
        catch(const std::exception& exception){error=exception.what();return false;}
    }
    if(api_.GetError()!=gl::NoError){error="Pending OpenGL error before procedural upload";return false;}
    gl::Int maximum{};api_.GetIntegerv(volume?0x8073u:0x0D33u,&maximum);
    if(maximum<=0||image.width>unsigned(maximum)||image.height>unsigned(maximum)||image.depth>unsigned(maximum)) {
        error="Procedural image exceeds current GL texture limits";return false;
    }
    if(volume&&!texImage3D_){error="Dirty volume requires glTexImage3D";return false;}
    GpuTexture target;
    if(!created)target=found->second;
    else {target.width=image.width;target.height=image.height;target.volume=volume;target.order=orderFor(*r.source);}
    if(volume) {
        target.depth=image.depth;target.dirtyInputs=r.controller.initializedDirtyInputs();
        const auto kind=*r.controller.definition().type;
        target.dirtySlot=kind==neoshared::material::ProcedureType::Dirty?0u:kind==neoshared::material::ProcedureType::Dirty2?1u:2u;
    }
    if(r.normalUpload) {target.engineNormal=true;target.width=normal[0].width;target.height=normal[0].height;}
    UploadState restore(api_);
    if(created){api_.GenTextures(1,&target.id);if(!target.id){error="Cannot allocate procedural texture";return false;}}
    const auto textureTarget=volume?0x806Fu:gl::Texture2D;
    api_.BindTexture(textureTarget,target.id);
    const bool filter=r.metadata.txi.properties.filter.value_or(true);
    if(!volume && !r.normalUpload) {
        const bool mipmap=r.metadata.txi.properties.mipmap.value_or(true);
        const auto clamp=r.metadata.txi.properties.clamp.value_or(0);
        api_.TexParameteri(textureTarget,0x8191,mipmap?1:0);
        api_.TexParameteri(textureTarget,0x2801,mipmap?(filter?0x2703:0x2700):(filter?0x2601:0x2600));
        api_.TexParameteri(textureTarget,0x2800,filter?0x2601:0x2600);
        api_.TexParameteri(textureTarget,0x2802,(clamp&1)?0x2900:0x2901);
        api_.TexParameteri(textureTarget,0x2803,(clamp&2)?0x2900:0x2901);
    }
    // The engine 3D branch's sampler writes target TEXTURE_2D, not its new
    // volume. Retain the volume's defaults; do not fabricate 3D sampler state
    // or reproduce unrelated 2D-binding side effects in this host cache.
    const auto format=image.bytesPerPixel==4?0x1908u:image.bytesPerPixel==3?0x1907u:0x1909u;
    if(r.normalUpload) {
        // The normal converter returns before ordinary TXI sampler setup.
        api_.TexParameteri(textureTarget,0x8191,0);
        api_.TexParameteri(textureTarget,0x2802,0x2901);api_.TexParameteri(textureTarget,0x2803,0x2901);
        api_.TexParameteri(textureTarget,0x2801,0x2703);api_.TexParameteri(textureTarget,0x2800,0x2601);
        for(std::size_t level=0;level<normal.size();++level) {
            const auto& n=normal[level];
            api_.TexImage2D(textureTarget,gl::Int(level),0x8058,gl::Size(n.width),gl::Size(n.height),0,0x80E1u,0x1401,n.bytes.data());
        }
    }
    else if(volume)texImage3D_(textureTarget,0,gl::Int(format),gl::Size(image.width),gl::Size(image.height),gl::Size(image.depth),0,format,0x1401,image.bytes.data());
    else api_.TexImage2D(textureTarget,0,gl::Int(format),gl::Size(image.width),gl::Size(image.height),0,format,0x1401,image.bytes.data());
    if(api_.GetError()!=gl::NoError) {
        if(created)api_.DeleteTextures(1,&target.id);
        error="Procedural texture upload failed; controller revision retained for retry";return false;
    }
    entries_[r.source][0]=target;procedureIds_[target.id]=&r;
    r.uploadedRevision=r.controller.revision();return true;
}
bool TextureCache::setProcedureMipPath(ProcedureMipPath path,std::string& error) {
    error.clear();
    if(path!=ProcedureMipPath::SgiAutomatic&&path!=ProcedureMipPath::LegacyByteKernels) {
        error="Unknown procedural mip capability";return false;
    }
    if(path!=mipPath_&&!procedures_.empty()) {
        error="Clear the texture context before changing procedural mip capability";return false;
    }
    if(path!=mipPath_)failures_.clear();
    mipPath_=path;return true;
}
bool TextureCache::setProcedureGluPath(ProcedureGluPath path,std::string& error) {
    error.clear();
    if(path!=ProcedureGluPath::Unspecified&&path!=ProcedureGluPath::EmbeddedByte) {
        error="Unknown legacy GLU selection";return false;
    }
    if(path!=gluPath_&&!procedures_.empty()) {
        error="Clear the texture context before changing legacy GLU selection";return false;
    }
    if(path!=gluPath_)failures_.clear();
    gluPath_=path;return true;
}
bool TextureCache::uploadPixelTids(ProcedureRecord& r,std::string& error) {
    error.clear();
    if(r.uploadedRevision==r.controller.revision())return true;
    struct FacePlan {
        std::vector<material::ProcedureImage> levels;
        const material::ProcedureImage* source{};
        bool glu{};
        const material::ProcedureImage& image(std::size_t level) const {
            return source?*source:levels[level];
        }
        std::size_t size() const {return source?1u:levels.size();}
    };
    std::vector<std::vector<FacePlan>> plans;
    std::vector<std::vector<material::ProcedureImage>> tids;
    const bool updating=r.uploadedRevision!=std::numeric_limits<std::uint64_t>::max();
    const bool mip=r.metadata.txi.properties.mipmap.value_or(true);
    const bool runtimeMips=r.storage.mipOwnership==material::ProcedureMipOwnership::RuntimeGenerated;
    const bool automatic=runtimeMips&&mip&&!r.normalUpload&&!r.cube&&automaticMipmaps();
    const bool explicitMips=runtimeMips&&mip&&!r.normalUpload&&!automatic;
    gl::Int gluMaximum{};
    if(explicitMips&&(r.cube||(!updating&&r.controller.image().bytesPerPixel!=2)))
        api_.GetIntegerv(0x0d33u,&gluMaximum);
    try {
        const auto& d=r.controller.definition();
        const auto prepare=[&](const material::ProcedureImage& face) {
            FacePlan p;
            p.glu=explicitMips&&(r.cube||(!updating&&face.bytesPerPixel!=2));
            if(r.normalUpload)p.levels=material::procedureNormalLevels(face,
                r.metadata.txi.properties.bumpMapScaling.value_or(1.0f),
                r.metadata.txi.properties.bumpMapKind==neoshared::material::BumpMapKind::AuthoredNormal);
            else if(explicitMips) p.levels=material::procedureMipLevels(face,
                r.cube||(!updating&&face.bytesPerPixel!=2)?material::ProcedureMipKernel::GluByte:material::ProcedureMipKernel::AurUpdate,
                unsigned(gluMaximum));
            else p.source=&face;
            return p;
        };
        if(!r.cube&&d.numX==1&&d.numY==1) {
            plans.resize(1);
            plans[0].push_back(prepare(r.controller.image()));
        } else {
            tids=material::procedureTidImages(r.controller.image(),unsigned(d.numX),unsigned(d.numY),r.cube,r.cubeLayout);
            plans.resize(tids.size());
            for(std::size_t tid=0;tid<tids.size();++tid)for(const auto& face:tids[tid])
                plans[tid].push_back(prepare(face));
        }
    } catch(const std::exception& e) {error=e.what();return false;}
    auto& targets=entries_[r.source];
    const bool subImage=updating&&!r.normalUpload&&!automatic&&!(r.cube&&explicitMips);
    UploadState restore(api_);
    for(std::size_t tid=0;tid<plans.size();++tid) {
        auto& target=targets[tid];
        if(!updating) {
            api_.GenTextures(1,&target.id);
            target.order=orderFor(*r.source,tid);
            procedureIds_[target.id]=&r;
        }
        target.cube=r.cube;target.engineNormal=r.normalUpload;
        target.width=plans[tid][0].image(0).width;target.height=plans[tid][0].image(0).height;
        const auto binding=r.cube?gl::TextureCube:gl::Texture2D;
        api_.BindTexture(binding,target.id);
        const bool filter=r.metadata.txi.properties.filter.value_or(true);
        if(!r.cube)api_.TexParameteri(binding,0x8191,automatic?1:0);
        if(r.normalUpload) {
            api_.TexParameteri(binding,0x2802,0x2901);api_.TexParameteri(binding,0x2803,0x2901);
            api_.TexParameteri(binding,0x2801,0x2703);api_.TexParameteri(binding,0x2800,0x2601);
        } else {
            const auto clamp=r.metadata.txi.properties.clamp.value_or(0);
            api_.TexParameteri(binding,0x2802,r.cube?0x812f:((clamp&1)?0x2900:0x2901));
            api_.TexParameteri(binding,0x2803,r.cube?0x812f:((clamp&2)?0x2900:0x2901));
            if(r.cube)api_.TexParameteri(binding,0x8072,0x812f);
            api_.TexParameteri(binding,0x2801,mip?(filter?0x2703:0x2700):(filter?0x2601:0x2600));
            api_.TexParameteri(binding,0x2800,filter?0x2601:0x2600);
        }
        for(std::size_t face=0;face<plans[tid].size();++face)for(std::size_t l=0;l<plans[tid][face].size();++l) {
            const auto& p=plans[tid][face].image(l);
            const auto imageTarget=r.cube?0x8515u+gl::Enum(face):gl::Texture2D;
            const auto format=r.normalUpload?0x80e1u:p.bytesPerPixel==4?0x1908u:p.bytesPerPixel==3?0x1907u:
                p.bytesPerPixel==2?(r.cube?0x190au:0x1908u):0x1909u;
            const auto type=(!r.normalUpload&&!r.cube&&p.bytesPerPixel==2)?0x8033u:0x1401u;
            const auto internal=r.normalUpload?0x8058u:
                plans[tid][face].glu&&p.bytesPerPixel<3?p.bytesPerPixel:format;
            if(subImage)
                api_.TexSubImage2D(imageTarget,gl::Int(l),0,0,gl::Size(p.width),gl::Size(p.height),format,type,p.bytes.data());
            else
                api_.TexImage2D(imageTarget,gl::Int(l),gl::Int(internal),gl::Size(p.width),gl::Size(p.height),0,format,type,p.bytes.data());
        }
    }
    r.uploadedRevision=r.controller.revision();return true;
}

GpuTexture TextureCache::bind(ProcedureRecord& r,std::string& error) {
    auto result=selected(r,error);if(!result.id)return {};
    r.used=true;
    const auto change=r.controller.control(procedureContext_,error);
    if(!error.empty()){r.fault=error;return {};}
    if(change==material::ProcedureChange::Image) {
        if(!uploadProcedure(r,error))return {};
    }
    // Frame changes and InvalidTexture become visible to the NEXT Bind. This
    // bind selected its TID before the engine Control call (CAurTexture::Bind).
    return result;
}
GpuTexture TextureCache::bind(gl::UInt id,std::string& error) {
    error.clear();const auto found=procedureIds_.find(id);
    if(found==procedureIds_.end()){GpuTexture result;result.id=id;return result;}
    return bind(*found->second,error);
}
bool TextureCache::beginFrame(float elapsed,std::string& error) {
    error.clear();
    if(!ready_){error="Texture context is not initialized";return false;}
    // AurTextureMakeCurrent always increments its 32-bit token. Controller
    // callers consume the frame elapsed value directly; Wave/Water apply their
    // own engine .04 ceiling after their token and quality admission checks.
    procedureContext_.token=model::engineIntAdd(procedureContext_.token,1);
    procedureContext_.elapsed=elapsed;
    if(!engineRuntime_)return true;
    std::vector<ProcedureRecord*> order;order.reserve(procedures_.size());
    for(auto& p:procedures_)if(p.second->used)order.push_back(p.second.get());
    std::sort(order.begin(),order.end(),[&](const auto* a,const auto* b){
        return a->engineHash!=b->engineHash ? a->engineHash<b->engineHash :
            orders_.at(a->source)<orders_.at(b->source);
    });
    for(auto* r:order) {
        bind(*r,error);r->used=false;
        if(!error.empty())return false;
    }
    return true;
}
GpuTexture TextureCache::current(gl::UInt id,std::string& error) {
    error.clear();
    const auto found=procedureIds_.find(id);
    if(found==procedureIds_.end()) {GpuTexture result;result.id=id;return result;}
    return selected(*found->second,error);
}
bool TextureCache::bind(MeshDrawParameters& p,std::string& error) {
    error.clear();
    const auto use=[&](gl::UInt& id,material::UvState* uv,bool allowCube=false) {
        if(!id || procedureIds_.find(id)==procedureIds_.end())return true;
        const auto result=bind(id,error);if(!result.id)return false;
        if(result.volume){error="A engine Dirty 3D texture cannot be consumed by this 2D material path";return false;}
        if(result.cube&&!allowCube){error="Cubemap used by a 2D procedural texture consumer";return false;}
        id=result.id;
        if(uv) {
            uv->texel={{1.0f/float(result.width),1.0f/float(result.height)}};
            uv->scale={{1,1}};uv->offset={{0,0}};uv->atlas=false;
            uv->frame=unsigned((result.order&0xffffffffu)-1u);uv->layer=uv->frame;
        }
        return true;
    };
    if(p.dirty.settings.enabled && !p.outlineOnly && !p.inspectMaterial) {
        // Validate the pinned controller and complete selector BEFORE even the
        // outer PartTriMesh::Mat Bind. No synthetic replacement for missing 3D.
        const auto volume=current(p.dirty.texture,error);
        if(!volume.id||!volume.volume||!volume.dirtyInputs||volume.dirtySlot!=p.dirty.settings.slot) {
            error="Dirty diagnostic requires a registered matching Dirty/Dirty2/Dirty3 volume";return false;
        }
        p.dirty.initializedFormula=volume.dirtyInputs->parameter;
        material::DirtyMaterialPlan plan;
        if(!material::makeDirtyMaterialPlan(p.dirty,p.material.useLightmap,p.material.useEnvironmentMap,plan,error,!p.material.engineHardware.arbFragmentPrograms))return false;
        if(!use(p.diffuseTexture,&p.material.diffuseUv))return false; // outer Mat
        const auto volumeId=p.dirty.texture;
        p.rebindEngineTexture=[this,volumeId](gl::UInt id,std::string& failure) {
            const auto result=bind(id,failure);
            if(!result.id)return gl::UInt{};
            if(result.volume!=(id==volumeId)) {failure="Wrong target at a Dirty texture Bind event";return gl::UInt{};}
            return result.id;
        };
        return true; // remaining Binds execute in the actual dispatcher order
    }
    if(!use(p.diffuseTexture,&p.material.diffuseUv))return false;
    if(p.ownerFragment==OwnerFragmentEffect::Hologram)return use(p.hologramFilterTexture,nullptr);
    if(p.material.k2PartTriMeshState &&
       (p.exactK2SkinVertexProgram || p.material.engineGame==material::EngineMaterialGame::Kotor1) &&
       !p.inspectMaterial &&
       p.ownerFragment==OwnerFragmentEffect::None && p.skinEffect==SkinRenderEffect::None) {
        // PartTriMesh::Mat binds diffuse first (K2 Desktop 0xdb216, K1 0x341290).
        // Skin and K1 rigid consumers perform the remaining/repeated Binds in
        // engine pass order. Eager binding would advance procedural state too
        // early and erase within-draw differences.
        p.rebindEngineTexture=[this](gl::UInt id,std::string& failure) {
            const auto texture=bind(id,failure);
            if(texture.volume) {
                failure="A engine material sampler cannot consume a Dirty volume";
                return gl::UInt{};
            }
            return texture.id;
        };
        if(p.material.engineGame==material::EngineMaterialGame::Kotor1)
            p.queryEngineTexture=[this](gl::UInt id,std::string& failure) {
                return current(id,failure).id;
            };
        return true;
    }
    if(p.material.useLightmap&&!use(p.lightmapTexture,nullptr))return false;
    if(p.material.useBumpMap) {
        if(!use(p.bumpTexture,&p.material.bumpUv))return false;
        const auto found=procedureIds_.find(p.bumpTexture);
        if(found!=procedureIds_.end() && found->second->normalUpload)
            p.material.bumpKind=neoshared::material::BumpMapKind::AuthoredNormal;
    }
    if(p.material.useEnvironmentMap&&!use(p.environmentTexture,nullptr,true))return false;
    return true;
}
std::uint64_t TextureCache::orderFor(
        const neoshared::texture::TextureData& texture, std::size_t layer) {
    auto found=orders_.find(&texture);
    if(found==orders_.end()) {
        const auto order=nextOrder_++;
        found=orders_.emplace(&texture,order).first;
    }
    // AurTextureGetOrdering emits every nonzero TID of each static texture in
    // texture-registration order. NeoMDL uploads decoded frames separately, so
    // reserve the high word for the texture and the low word for that frame.
    const auto frame=std::min<std::uint64_t>(
        static_cast<std::uint64_t>(layer),
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()-1u));
    return (found->second<<32u)|(frame+1u);
}
void TextureCache::setDirtyAreaInputs(const std::array<material::DirtyTextureInputs,3>& inputs,
        const std::string& scope) {
    bool changed=scope!=dirtyScope_;
    for(unsigned i=0;i<3;++i) {
        const auto& a=inputs[i];const auto& b=procedureContext_.dirty[i];
        changed=changed||a.color!=b.color||a.size!=b.size||a.parameter!=b.parameter||a.algorithm!=b.algorithm;
    }
    if(!changed)return;
    // These are context-owned names. Call with this cache's GL context current,
    // exactly like clear(); all borrowers re-resolve at the next submission.
    for(const auto* source:dirtySources_) {
        const auto entry=entries_.find(source);
        if(entry!=entries_.end()) {
            for(const auto& frame:entry->second) {
                const auto id=frame.second.id;
                procedureIds_.erase(id);if(textureDeleted_)textureDeleted_(id);
                if(ready_)api_.DeleteTextures(1,&id);
            }
            entries_.erase(entry);
        }
        procedures_.erase(source);failures_.erase(source);
    }
    dirtySources_.clear();dirtyScope_=scope;procedureContext_.dirty=inputs;
}
void TextureCache::retain(const void* owner,const neoshared::texture::TextureData& source) {
    if(borrowers_[owner].insert(&source).second)++borrowCounts_[&source];
}
void TextureCache::release(const void* owner) {
    const auto borrower=borrowers_.find(owner);
    if(borrower==borrowers_.end())return;
    for(const auto* source:borrower->second) {
        const auto count=borrowCounts_.find(source);
        if(--count->second)continue;
        borrowCounts_.erase(count);
        const auto retire=[&](const neoshared::texture::TextureData* texture) {
            if(const auto entry=entries_.find(texture);entry!=entries_.end()) {
                for(const auto& frame:entry->second) {
                    const auto id=frame.second.id;
                    procedureIds_.erase(id);
                    if(textureDeleted_)textureDeleted_(id);
                    retiredTextures_.push_back(id);
                }
                entries_.erase(entry);
            }
            orders_.erase(texture);
        };
        retire(source);
        if(const auto procedure=procedures_.find(source);procedure!=procedures_.end()) {
            if(procedure->second->tiled)retire(&procedure->second->frames);
            procedures_.erase(procedure);
        }
        failures_.erase(source);dirtySources_.erase(source);definitions_.erase(source);
    }
    borrowers_.erase(borrower);
}
void TextureCache::flushRetiredTextures() {
    for(const auto id:retiredTextures_)api_.DeleteTextures(1,&id);
    retiredTextures_.clear();
}
void TextureCache::clear() {
    flushRetiredTextures();
    if(ready_)
        for(const auto& texture:entries_) for(const auto& frame:texture.second) {
            if(textureDeleted_)textureDeleted_(frame.second.id);
            api_.DeleteTextures(1,&frame.second.id);
        }
    entries_.clear();dirtySources_.clear();dirtyScope_.clear();
    procedures_.clear();failures_.clear();procedureIds_.clear();definitions_.clear();
    procedureContext_=material::ProcedureContext{};
    orders_.clear();
    nextOrder_=1u;borrowers_.clear();borrowCounts_.clear();
}
std::size_t TextureCache::size() const noexcept {
    std::size_t result=0;for(const auto& texture:entries_) result+=texture.second.size();return result;
}
} // namespace neomdl::render
