#include <material/ProcedureStorage.hpp>
#include <neoshared/glu/ByteMipmaps.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace neomdl::material {
namespace {
constexpr std::array<unsigned,6> targets{{1,0,2,3,4,5}},turns{{3,1,0,2,2,0}};
std::size_t checkedProduct(std::size_t a,std::size_t b) {
    if(b&&a>std::numeric_limits<std::size_t>::max()/b)
        throw std::length_error("Procedural image storage size overflows");
    return a*b;
}
std::size_t imageBytes(unsigned width,unsigned height,unsigned bpp) {
    return checkedProduct(checkedProduct(width,height),bpp);
}
unsigned extentProduct(unsigned a,unsigned b) {
    if(b&&a>std::numeric_limits<unsigned>::max()/b)
        throw std::length_error("Procedural image dimensions overflow");
    return a*b;
}
unsigned tileCount(unsigned nx,unsigned ny) {
    if(!nx||!ny)throw std::invalid_argument("Invalid engine TID grid");
    return extentProduct(nx,ny);
}
void validate(const ProcedureImage& p) {
    if(!p.width||!p.height||p.depth!=1||p.bytesPerPixel<1||p.bytesPerPixel>4||
       imageBytes(p.width,p.height,p.bytesPerPixel)!=p.bytes.size())
        throw std::invalid_argument("Invalid engine procedural source storage");
}
ProcedureImage crop(const ProcedureImage& p,unsigned x,unsigned y,unsigned w,unsigned h) {
    if(!w||!h||x>p.width||y>p.height||w>p.width-x||h>p.height-y)
        throw std::invalid_argument("Engine procedural crop is outside its source image");
    ProcedureImage out;out.width=w;out.height=h;out.bytesPerPixel=p.bytesPerPixel;
    const auto row=checkedProduct(w,out.bytesPerPixel);out.bytes.resize(checkedProduct(row,h));
    for(unsigned j=0;j<h;++j)std::copy_n(p.bytes.data()+((std::size_t(y+j)*p.width+x)*p.bytesPerPixel),
        row,out.bytes.data()+std::size_t(j)*row);
    return out;
}
ProcedureImage rotate(const ProcedureImage& p,unsigned q) {
    q&=3u;if(!q)return p;
    ProcedureImage out;out.width=(q&1u)?p.height:p.width;out.height=(q&1u)?p.width:p.height;
    out.bytesPerPixel=p.bytesPerPixel;out.bytes.resize(p.bytes.size());
    for(unsigned y=0;y<p.height;++y)for(unsigned x=0;x<p.width;++x) {
        unsigned dx,dy;


        if(q==1){dx=p.height-1u-y;dy=x;}
        else if(q==2){dx=p.width-1u-x;dy=p.height-1u-y;}
        else {dx=y;dy=p.width-1u-x;}
        std::copy_n(p.bytes.data()+(std::size_t(y)*p.width+x)*p.bytesPerPixel,p.bytesPerPixel,
            out.bytes.data()+(std::size_t(dy)*out.width+dx)*out.bytesPerPixel);
    }
    return out;
}
void put(ProcedureImage& to,const ProcedureImage& from,unsigned x,unsigned y) {
    if(to.bytesPerPixel!=from.bytesPerPixel||x>to.width||y>to.height||
       from.width>to.width-x||from.height>to.height-y)
        throw std::invalid_argument("Inconsistent procedural source atlas");
    for(unsigned row=0;row<from.height;++row)
        std::copy_n(from.bytes.data()+std::size_t(row)*from.width*from.bytesPerPixel,
            std::size_t(from.width)*from.bytesPerPixel,
            to.bytes.data()+((std::size_t(y+row)*to.width+x)*to.bytesPerPixel));
}
unsigned sourceBpp(const neoshared::texture::TextureData& t) {
    if(t.tgaSourceLayout) {
        const auto& f=*t.tgaSourceLayout;
        if((f.imageType==2||f.imageType==10)&&(f.pixelBits==24||f.pixelBits==32))return f.pixelBits/8;
        if(f.imageType==3&&(f.pixelBits==8||f.pixelBits==16))return f.pixelBits/8;
        // CResTGA swaps byte 0 and 2 with a pixelBits/8 stride for every
        // non-type-3 image, including unsupported narrow/RLE-gray layouts.
        throw std::invalid_argument("Engine TGA source has an unsafe/unestablished color-swap stride or palette expansion");
    }
    const auto& e=t.sourceEncoding;
    if(e=="K1 procedural RGBA"||e=="K2 procedural RGBA"||e=="raw RGBA"||e.rfind("32-bit TGA",0)==0)return 4;
    if(e=="raw RGB"||e.rfind("24-bit TGA",0)==0)return 3;
    if(e=="raw grayscale"||e=="8-bit TGA")return 1;
    throw std::invalid_argument("Procedural source has no established engine byte layout: "+e);
}
ProcedureSourceEncoding sourceEncoding(const neoshared::texture::TextureData& t,unsigned bpp) {
    if(t.tgaSourceLayout&&t.tgaSourceLayout->imageType==3&&t.tgaSourceLayout->pixelBits==16)
        return ProcedureSourceEncoding::Gray16;
    if(bpp==1)return ProcedureSourceEncoding::Gray8;
    if(bpp==2)return ProcedureSourceEncoding::PackedRgba4444;
    if(bpp==3)return ProcedureSourceEncoding::Rgb8;
    return ProcedureSourceEncoding::Rgba8;
}
ProcedureImage layerImage(const neoshared::texture::TextureLayer& l,unsigned bpp) {
    if(!l.width||!l.height||imageBytes(l.width,l.height,4)!=l.rgba.size())
        throw std::invalid_argument("Invalid decoded procedural layer storage");
    ProcedureImage p;p.width=l.width;p.height=l.height;p.bytesPerPixel=bpp;
    p.bytes.resize(imageBytes(l.width,l.height,bpp));
    for(unsigned y=0;y<l.height;++y)for(unsigned x=0;x<l.width;++x) {
        const auto s=(std::size_t(l.height-1-y)*l.width+x)*4;
        const auto d=(std::size_t(y)*l.width+x)*bpp;
        if(bpp==2){p.bytes[d]=l.rgba[s];p.bytes[d+1]=l.rgba[s+3];}
        else std::copy_n(l.rgba.data()+s,bpp,p.bytes.data()+d);
    }
    return p;
}
ProcedureImage processedImage(const neoshared::texture::ProcessedTextureLevel& level,unsigned bpp) {
    ProcedureImage result;result.width=level.width;result.height=level.height;result.bytesPerPixel=bpp;
    if(!level.width||!level.height||bpp<1||bpp>4||
       imageBytes(level.width,level.height,bpp)!=level.bytes.size())
        throw std::invalid_argument("Uncompressed processed level has an inconsistent engine byte span");
    result.bytes=level.bytes;return result;
}
ProcedureImage aurImageScale(const ProcedureImage& source,unsigned width,unsigned height) {


    validate(source);
    ProcedureImage destination;destination.width=width;destination.height=height;
    destination.bytesPerPixel=source.bytesPerPixel;
    destination.bytes.resize(imageBytes(width,height,source.bytesPerPixel));
    const std::uint64_t denominator=std::uint64_t(source.width)*source.height;
    if(denominator>std::numeric_limits<std::uint64_t>::max()/256u)
        throw std::length_error("Procedural scaling accumulator overflows");
    for(unsigned dy=0;dy<height;++dy) {
        const std::uint64_t y0=std::uint64_t(dy)*source.height;
        const std::uint64_t y1=std::uint64_t(dy+1u)*source.height;
        const unsigned firstY=unsigned(y0/height),lastY=unsigned((y1-1u)/height);
        for(unsigned dx=0;dx<width;++dx) {
            const std::uint64_t x0=std::uint64_t(dx)*source.width;
            const std::uint64_t x1=std::uint64_t(dx+1u)*source.width;
            const unsigned firstX=unsigned(x0/width),lastX=unsigned((x1-1u)/width);
            for(unsigned c=0;c<source.bytesPerPixel;++c) {
                std::uint64_t total=0;
                for(unsigned sy=firstY;sy<=lastY;++sy) {
                    const auto overlapY=std::min<std::uint64_t>(y1,std::uint64_t(sy+1u)*height)-
                                        std::max<std::uint64_t>(y0,std::uint64_t(sy)*height);
                    for(unsigned sx=firstX;sx<=lastX;++sx) {
                        const auto overlapX=std::min<std::uint64_t>(x1,std::uint64_t(sx+1u)*width)-
                                            std::max<std::uint64_t>(x0,std::uint64_t(sx)*width);
                        total+=std::uint64_t(source.bytes[(std::size_t(sy)*source.width+sx)*source.bytesPerPixel+c])*
                               overlapX*overlapY;
                    }
                }
                destination.bytes[(std::size_t(dy)*width+dx)*source.bytesPerPixel+c]=
                    std::uint8_t((total+denominator/2u)/denominator);
            }
        }
    }
    return destination;
}

}
ProcedureStorageDescriptor procedureStorageDescriptor(
        const neoshared::texture::TextureData& texture,const ProcedureDefinition& definition,bool cube) {
    ProcedureStorageDescriptor result;
    if(definition.numX<=0||definition.numY<=0)throw std::invalid_argument("Invalid engine procedural tile count");
    result.textureIdCount=tileCount(unsigned(definition.numX),unsigned(definition.numY));
    result.faceCount=cube?6u:1u;
    result.target=definition.volumeTexture?ProcedureTextureTarget::Volume3D:
                  cube?ProcedureTextureTarget::CubeMap:ProcedureTextureTarget::Texture2D;
    if(texture.processedPayload) {
        const auto& p=*texture.processedPayload;
        result.provenance=ProcedureStorageProvenance::ProcessedPayload;
        result.encoding=p.encoding==neoshared::texture::ProcessedSourceEncoding::Gray8?ProcedureSourceEncoding::Gray8:
                        p.encoding==neoshared::texture::ProcessedSourceEncoding::Rgb8?ProcedureSourceEncoding::Rgb8:
                        ProcedureSourceEncoding::Rgba8;
        result.compression=p.compression==neoshared::texture::ProcessedCompressionCodec::Bc1?ProcedureCompressionCodec::Bc1:
                           p.compression==neoshared::texture::ProcessedCompressionCodec::Bc3?ProcedureCompressionCodec::Bc3:
                           ProcedureCompressionCodec::None;
        result.target=p.target==neoshared::texture::ProcessedTextureTarget::CubeMap?
            ProcedureTextureTarget::CubeMap:ProcedureTextureTarget::Texture2D;
        result.mipOwnership=ProcedureMipOwnership::SourceProvided;
        result.initialUpload=ProcedureUploadAlgorithm::DirectSourceLevels;
        result.updateUpload=result.compression==ProcedureCompressionCodec::None?
            ProcedureUploadAlgorithm::DirectPixels:ProcedureUploadAlgorithm::DirectSourceLevels;
    } else {
        const auto bpp=sourceBpp(texture);
        result.encoding=sourceEncoding(texture,bpp);
        result.mipOwnership=ProcedureMipOwnership::RuntimeGenerated;
        result.initialUpload=result.updateUpload=definition.volumeTexture?ProcedureUploadAlgorithm::Volume3D:
            definition.bumpTexture&&!cube?ProcedureUploadAlgorithm::HeightToNormal:
            ProcedureUploadAlgorithm::DirectPixels;
    }
    return result;
}
ProcedureImage procedureSourceImage(const neoshared::texture::TextureData& t,
                                   const ProcedureDefinition& d,bool cube) {
    if(d.numX<=0||d.numY<=0)
        throw std::invalid_argument("Invalid engine procedural tile count");
    const auto nx=unsigned(d.numX),ny=unsigned(d.numY),n=tileCount(nx,ny);
    ProcedureImage result;
    if(t.processedPayload) {
        const auto& payload=*t.processedPayload;
        if(payload.compression!=neoshared::texture::ProcessedCompressionCodec::None)
            throw std::invalid_argument("Block-compressed processed storage has no writable engine controller pixel span");
        const auto bpp=unsigned(payload.bytesPerPixel);
        if(payload.target==neoshared::texture::ProcessedTextureTarget::CubeMap) {
            if(!cube||payload.surfaces.size()!=6)
                throw std::invalid_argument("Processed cubemap requires six direct engine face surfaces");
            if(payload.surfaces.front().levels.empty())
                throw std::invalid_argument("Processed cubemap face has no base level");
            auto first=processedImage(payload.surfaces[0].levels.at(0),bpp);
            if(first.width!=first.height)throw std::invalid_argument("Processed cubemap faces must be square");
            result=first;result.height=extentProduct(first.height,6u);
            result.bytes.resize(imageBytes(result.width,result.height,bpp));
            for(unsigned face=0;face<6u;++face) {
                if(payload.surfaces[face].levels.empty())
                    throw std::invalid_argument("Processed cubemap face has no base level");
                auto image=processedImage(payload.surfaces[face].levels.front(),bpp);
                if(image.width!=first.width||image.height!=first.height)
                    throw std::invalid_argument("Processed cubemap face dimensions differ");
                put(result,image,0,face*first.height);
            }
        } else if(cube) {
            throw std::invalid_argument("Processed 2D storage does not establish a cubemap face layout");
        } else if(payload.surfaces.size()==1u) {
            if(payload.surfaces.front().levels.empty())
                throw std::invalid_argument("Processed texture has no base level");
            result=processedImage(payload.surfaces.front().levels.front(),bpp);
        } else if(d.type==neoshared::material::ProcedureType::Cycle&&payload.surfaces.size()==n) {
            if(payload.surfaces.front().levels.empty())
                throw std::invalid_argument("Processed cycle frame has no base level");
            auto first=processedImage(payload.surfaces.front().levels.front(),bpp);
            result=first;result.width=extentProduct(first.width,nx);result.height=extentProduct(first.height,ny);
            result.bytes.resize(imageBytes(result.width,result.height,bpp));
            for(unsigned frame=0;frame<n;++frame) {
                if(payload.surfaces[frame].levels.empty())
                    throw std::invalid_argument("Processed cycle frame has no base level");
                auto image=processedImage(payload.surfaces[frame].levels.front(),bpp);
                if(image.width!=first.width||image.height!=first.height)
                    throw std::invalid_argument("Processed cycle frame dimensions differ");
                put(result,image,(frame%nx)*first.width,(frame/nx)*first.height);
            }
        } else {
            throw std::invalid_argument("Processed surfaces do not establish one engine controller image");
        }
        validate(result);
        (void)procedureTidImages(result,nx,ny,cube,ProcedureCubeLayout::ProcessedDirect);
        return result;
    }
    if(t.layers.empty())throw std::invalid_argument("Procedural source has no image");
    const auto bpp=sourceBpp(t);
    auto first=layerImage(t.layers.front(),bpp);
    if(cube&&t.cubeMap) {
        if(t.layers.size()!=6||t.cubeFaces.size()!=6||first.width!=first.height)
            throw std::invalid_argument("Procedural cubemap requires six complete square named faces");
        result=first;result.height=extentProduct(first.height,6u);
        result.bytes.resize(imageBytes(result.width,result.height,bpp));
        std::array<bool,6> seen{};
        for(std::size_t j=0;j<6;++j) {
            const auto target=unsigned(t.cubeFaces[j]);
            if(target>=6||seen[target])throw std::invalid_argument("Procedural cube has duplicate/missing targets");
            seen[target]=true;auto face=layerImage(t.layers[j],bpp);
            if(face.width!=first.width||face.height!=first.height)throw std::invalid_argument("Procedural cube face dimensions differ");
            const auto i=unsigned(std::find(targets.begin(),targets.end(),target)-targets.begin());
            put(result,rotate(face,(4u-turns[i])&3u),0,i*first.height);
        }
    } else if(t.layers.size()==1)result=std::move(first);
    else if(!cube&&d.type==neoshared::material::ProcedureType::Cycle&&t.layers.size()==n) {
        result=first;result.width=extentProduct(first.width,nx);result.height=extentProduct(first.height,ny);
        result.bytes.resize(imageBytes(result.width,result.height,bpp));
        const bool strip=t.kind==neoshared::texture::TextureFileKind::Tga||t.kind==neoshared::texture::TextureFileKind::Dds;
        for(unsigned i=0;i<n;++i) {
            auto tile=layerImage(t.layers[strip?(ny-1-i/nx)*nx+i%nx:i],bpp);
            if(tile.width!=first.width||tile.height!=first.height)throw std::invalid_argument("Procedural frame dimensions differ");
            put(result,tile,(i%nx)*tile.width,(i/nx)*tile.height);
        }
    } else throw std::invalid_argument("Decoded layers do not establish one engine controller image");
    validate(result);
    // The game resource loader ignores TGA origin flags. Undo the shared
    // display correction, rather than silently rendering a repaired source.
    if(t.tgaSourceLayout&&(t.tgaSourceLayout->descriptor&0x30u)) {
        auto original=result;const auto flags=t.tgaSourceLayout->descriptor;
        for(unsigned y=0;y<result.height;++y)for(unsigned x=0;x<result.width;++x) {
            const auto sx=(flags&0x10u)?result.width-1-x:x;
            const auto sy=(flags&0x20u)?result.height-1-y:y;
            std::copy_n(original.bytes.data()+(std::size_t(sy)*result.width+sx)*bpp,bpp,
                result.bytes.data()+(std::size_t(y)*result.width+x)*bpp);
        }
    }
    // Validate every crop before controller Init can consume shared RNG.
    (void)procedureTidImages(result,nx,ny,cube);return result;
}
std::vector<std::vector<ProcedureImage>> procedureTidImages(
        const ProcedureImage& p,unsigned nx,unsigned ny,bool cube,ProcedureCubeLayout layout) {
    validate(p);
    const auto count=tileCount(nx,ny);
    if(cube&&p.height%6u)throw std::invalid_argument("Engine cube strip height is not six complete faces");
    const auto h=cube?p.height/6u:p.height;
    if(p.width%nx||h%ny||p.width<nx||h<ny)
        throw std::invalid_argument("Engine TID grid leaves a remainder or empty image; no implicit padding");
    const auto w=p.width/nx,th=h/ny;
    if(cube&&w!=th)throw std::invalid_argument("Engine cubemap TID faces must be square after tiling");
    std::vector<std::vector<ProcedureImage>> out(count);
    for(unsigned tid=0;tid<count;++tid) {
        out[tid].resize(cube?6u:1u);
        if(cube)for(unsigned face=0;face<6;++face) {
            auto image=crop(p,(tid%nx)*w,face*h+(tid/nx)*th,w,th);
            if(layout==ProcedureCubeLayout::ProcessedDirect)out[tid][face]=std::move(image);
            else out[tid][targets[face]]=rotate(image,turns[face]);
        }
        else out[tid][0]=crop(p,(tid%nx)*w,(tid/nx)*th,w,th);
    }
    return out;
}
std::vector<ProcedureImage> procedureMipLevels(const ProcedureImage& p,ProcedureMipKernel kernel,unsigned maxTextureSize) {
    validate(p);
    if(kernel==ProcedureMipKernel::GluByte) {
        auto images=neoshared::glu::buildByteMipmaps({p.width,p.height,p.bytesPerPixel,p.bytes},maxTextureSize);
        std::vector<ProcedureImage> levels;levels.reserve(images.size());
        for(auto& image:images) {
            ProcedureImage level;level.width=image.width;level.height=image.height;
            level.bytesPerPixel=image.components;level.bytes=std::move(image.bytes);
            levels.push_back(std::move(level));
        }
        return levels;
    }
    const auto extent=std::max(p.width,p.height);
    ProcedureImage base=p.width!=p.height?aurImageScale(p,extent,extent):p;
    std::vector<ProcedureImage> levels;
    levels.push_back(std::move(base));
    while(levels.back().width>1||levels.back().height>1) {
        const auto& s=levels.back();ProcedureImage d;
        d.width=std::max(1u,s.width/2);d.height=std::max(1u,s.height/2);d.bytesPerPixel=s.bytesPerPixel;
        d.bytes.resize(std::size_t(d.width)*d.height*d.bytesPerPixel);
        const auto cols=s.width>1?2u:1u,rows=s.height>1?2u:1u;
        for(unsigned y=0;y<d.height;++y)for(unsigned x=0;x<d.width;++x)for(unsigned c=0;c<d.bytesPerPixel;++c) {
            unsigned sum=0,lo=0,hi=0;
            for(unsigned yy=0;yy<rows;++yy)for(unsigned xx=0;xx<cols;++xx) {
                const auto v=s.bytes[((std::size_t(y*rows+yy)*s.width+x*cols+xx)*s.bytesPerPixel)+c];
                sum+=v;lo+=v&15u;hi+=v>>4u;
            }
            unsigned v;
            if(s.bytesPerPixel==2)v=(lo/4u)|((hi/4u)<<4u);
            else v=sum/(cols*rows);
            d.bytes[(std::size_t(y)*d.width+x)*d.bytesPerPixel+c]=std::uint8_t(v);
        }
        levels.push_back(std::move(d));
    }
    return levels;
}
} // namespace neomdl::material
