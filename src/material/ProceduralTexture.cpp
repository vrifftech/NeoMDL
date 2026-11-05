#include <material/ProceduralTexture.hpp>
#include <model/EngineArithmetic.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace neomdl::material {
namespace {
constexpr float pi=3.1415927410125732421875f, twoPi=6.283185482025146484375f;
std::string lower(std::string s) {
    for(auto& c:s) if(c>='A' && c<='Z') c=char(c-'A'+'a');
    return s;
}
std::string firstWord(const std::string& text) { std::istringstream s(text);std::string r;s>>r;return r; }
bool scan(const std::string& s,float& v) {float f{};if(std::sscanf(s.c_str(),"%f",&f)!=1)return false;v=f;return true;}
bool scan(const std::string& s,int& v) {int n{};if(std::sscanf(s.c_str(),"%d",&n)!=1)return false;v=n;return true;}
std::optional<neoshared::material::ProcedureType> type(const std::string& s,EngineMaterialGame game) {
    static const std::pair<const char*,neoshared::material::ProcedureType> types[]{
        {"water",neoshared::material::ProcedureType::Water},{"life",neoshared::material::ProcedureType::Life},{"perlin",neoshared::material::ProcedureType::Perlin},
        {"arturo",neoshared::material::ProcedureType::Arturo},{"wave",neoshared::material::ProcedureType::Wave},{"cycle",neoshared::material::ProcedureType::Cycle},
        {"random",neoshared::material::ProcedureType::Random},{"ringtexdistort",neoshared::material::ProcedureType::RingTexDistort},
        {"dirty",neoshared::material::ProcedureType::Dirty},{"dirty2",neoshared::material::ProcedureType::Dirty2},{"dirty3",neoshared::material::ProcedureType::Dirty3}};
    for(const auto& p:types) if(s==p.first) {
        if(game==EngineMaterialGame::Kotor1 &&
           (p.second==neoshared::material::ProcedureType::Dirty||p.second==neoshared::material::ProcedureType::Dirty2||p.second==neoshared::material::ProcedureType::Dirty3))
            return {}; // Engine K1 factory leaves its destroyed controller null.
        return p.second;
    }
    return {};
}
bool isDirty(neoshared::material::ProcedureType t) {return t==neoshared::material::ProcedureType::Dirty||t==neoshared::material::ProcedureType::Dirty2||t==neoshared::material::ProcedureType::Dirty3;}
std::size_t checkedProduct(std::size_t a,std::size_t b) {
    if(b&&a>std::numeric_limits<std::size_t>::max()/b)
        throw std::length_error("Procedural image storage size overflows");
    return a*b;
}
std::size_t checkedPixels(std::int64_t w,std::int64_t h,std::int64_t d=1) {
    if(w<=0||h<=0||d<=0||w>std::numeric_limits<int>::max()||
       h>std::numeric_limits<int>::max()||d>std::numeric_limits<int>::max())
        throw std::invalid_argument("Procedural dimensions must be positive engine integers");
    return checkedProduct(checkedProduct(std::size_t(w),std::size_t(h)),std::size_t(d));
}
// cvttss2si's indefinite result is defined by the inspected x86 instruction.
// Avoid C++ undefined float-to-integer conversions without saturating pixels.
std::int32_t truncEngine(float f) {
    return model::engineFloatToInt(f);
}
std::uint8_t byte(float f) {return static_cast<std::uint8_t>(truncEngine(f));}
float bits(std::uint32_t u) {float f;std::memcpy(&f,&u,4);return f;}
float trigValue(std::uint32_t index) {
    // K2 cosineLUT/sineLUT 0x313223/0x313243, table 0x54a0d0;
    // K1 0x3a4442/0x3a4466, tables 0x47d4e0/0x47d560 contain identical bits.
    static constexpr std::uint32_t table[]{
        0x3f800000,0x3f7b14be,0x3f6c835e,0x3f54db31,0x3f3504f3,0x3f0e39da,0x3ec3ef15,0x3e47c5c2,
        0,0xbe47c5c2,0xbec3ef15,0xbf0e39da,0xbf3504f3,0xbf54db31,0xbf6c835e,0xbf7b14be,
        0xbf800000,0xbf7b14be,0xbf6c835e,0xbf54db31,0xbf3504f3,0xbf0e39da,0xbec3ef15,0xbe47c5c2,
        0,0x3e47c5c2,0x3ec3ef15,0x3f0e39da,0x3f3504f3,0x3f54db31,0x3f6c835e,0x3f7b14be};
    return bits(table[index&31u]);
}
float cosLut(float angle) {
    return trigValue(std::uint32_t(truncEngine(angle/bits(0x3e490fdb))));
}
float sinLut(float angle) {
    // Index offset, not angle subtraction: truncation differs for negatives.
    const auto i=(std::uint32_t(truncEngine(angle/bits(0x3e490fdb)))+24u)&31u;
    return trigValue(i);
}
int mod(int x,int n) {const auto r=x%n;return r<0?r+n:r;}
float mix(float a,float b,float f) {return (1.0f-f)*a+f*b;}
float solverElapsed(float elapsed,const ProcedureDefinition& definition) noexcept {
    // K2 Desktop MINSS has the live elapsed value as its source operand, so an
    // unordered value survives. K1 selects .04 instead.
    if(definition.game==EngineMaterialGame::Kotor2)
        return 0.04f<elapsed?0.04f:elapsed;
    return elapsed<0.04f?elapsed:0.04f;
}
float sample(const std::vector<float>& grid,int w,int h,float x,float y) {
    const int ix=truncEngine(x-0.5f),iy=truncEngine(y-0.5f);
    const float fx=x-float(ix),fy=y-float(iy);
    const auto at=[&](int a,int b){return grid[std::size_t(mod(b,h))*std::size_t(w)+std::size_t(mod(a,w))];};
    return mix(mix(at(ix,iy),at(ix+1,iy),fx),mix(at(ix,iy+1),at(ix+1,iy+1),fx),fy);
}
std::uint8_t interpolate(const std::vector<std::uint8_t>& a,int n,float x,float y) {
    const int ix=truncEngine(std::floor(x)),iy=truncEngine(std::floor(y));
    const float fx=x-float(ix),fy=y-float(iy);
    const auto at=[&](int xx,int yy){return float(a[std::size_t(mod(yy,n))*std::size_t(n)+std::size_t(mod(xx,n))]);};
    return byte(mix(mix(at(ix,iy),at(ix+1,iy),fx),mix(at(ix,iy+1),at(ix+1,iy+1),fx),fy));
}
void validateDefinition(const ProcedureDefinition& d) {
    for(float f:{d.distortionAmplitude,d.speed,d.fps,d.forceCycleSpeed,d.angleCycleSpeed})
        if(!std::isfinite(f))throw std::invalid_argument("Nonfinite engine procedural parameter");
    for(const auto* a:{&d.channelScale,&d.channelTranslate})
        for(float f:*a)if(!std::isfinite(f))throw std::invalid_argument("Nonfinite channel parameter");
    if(d.numX<=0||d.numY<=0||std::uint64_t(d.numX)*unsigned(d.numY)>std::numeric_limits<unsigned>::max())
        throw std::invalid_argument("Engine texture tile count cannot be represented");
}
}

ProcedureDefinition procedureDefinition(const neoshared::material::TxiMetadata& metadata,EngineMaterialGame game) {
    // Only a live controller consumes Parse<float>'s packed payload. With no
    // controller the engine outer reader would interpret those bytes as text.
    std::optional<neoshared::material::ProcedureType> active;
    for(const auto& row:neoshared::texture::scanTxiStream(metadata.sourceText)) {
        if(row.listData)continue;
        if(row.key=="proceduretype"&&!firstWord(row.value).empty())active=type(firstWord(row.value),game);
        if(row.binary&&!active)
            throw std::invalid_argument("Packed TXI array precedes a live engine procedural controller");
    }
    std::vector<std::string> lines;
    std::istringstream input(neoshared::texture::txiTextView(metadata.sourceText).text);std::string line;
    while(std::getline(input,line))lines.push_back(line);
    ProcedureDefinition d;d.game=game;
    for(std::size_t i=0;i<lines.size();++i) {
        std::istringstream row(lines[i]);std::string key,value;row>>key;std::getline(row,value);key=lower(key);
        if(key.empty()||key[0]=='#'||key[0]==';'||key.rfind("//",0)==0)continue;
        if(key=="proceduretype") {
            const auto word=firstWord(value);
            if(!word.empty()) {
                const auto w=d.defaultWidth,h=d.defaultHeight,nx=d.numX,ny=d.numY;
                const bool volume=d.volumeTexture,bump=d.bumpTexture;
                d=ProcedureDefinition{};d.game=game;d.defaultWidth=w;d.defaultHeight=h;d.numX=nx;d.numY=ny;
                d.volumeTexture=volume;d.bumpTexture=bump;
                d.type=type(word,game);
                if(d.type && isDirty(*d.type))d.volumeTexture=true; // Basic +0xf0 is sticky
                // unknown nonempty token destroys the previous controller
            }
        } else if(key=="isbumpmap") {int n{};if(scan(value,n))d.bumpTexture=n!=0;}
        else if(key=="defaultwidth"||key=="defaultheight"||key=="numx"||key=="numy") {
            int v{};
            if(scan(value,v)) {
                // Engine storage is signed short; don't use implementation-defined casts.
                const auto u=std::uint32_t(v)&65535u;v=u>=32768u?int(u)-65536:int(u);
                if(key=="defaultwidth")d.defaultWidth=v;else if(key=="defaultheight")d.defaultHeight=v;
                else if(key=="numx")d.numX=v;else d.numY=v;
            }
        }
        if(!d.type)continue; // fields before construction do not configure the next object
        if(key=="distort"||key=="distortangle") {int v{};if(scan(value,v)){if(key=="distort")d.distort=v!=0;else d.distortAngle=v!=0;}}
        else if(key=="distortionamplitude")scan(value,d.distortionAmplitude);
        else if(key=="speed")scan(value,d.speed);
        else if(key=="fps" && *d.type==neoshared::material::ProcedureType::Cycle)scan(value,d.fps);
        else if(key=="waterwidth" && *d.type==neoshared::material::ProcedureType::Water)scan(value,d.waterWidth);
        else if(key=="waterheight" && *d.type==neoshared::material::ProcedureType::Water)scan(value,d.waterHeight);
        else if(key=="forcecyclespeed" && *d.type==neoshared::material::ProcedureType::Water)scan(value,d.forceCycleSpeed);
        else if(key=="anglecyclespeed" && *d.type==neoshared::material::ProcedureType::Water)scan(value,d.angleCycleSpeed);
        else if(key=="arturowidth" && *d.type==neoshared::material::ProcedureType::Arturo)scan(value,d.arturoWidth);
        else if(key=="arturoheight" && *d.type==neoshared::material::ProcedureType::Arturo)scan(value,d.arturoHeight);
        else if(key=="channelscale"||key=="channeltranslate") {
            auto& a=key=="channelscale"?d.channelScale:d.channelTranslate;
            int n{};char packed{};const int fields=std::sscanf(value.c_str(),"%d %c",&n,&packed);
            if(fields==2)throw std::invalid_argument("Packed channel-array TXI requires the engine packed stream, not inline text values");
            if(fields==1&&n<0)throw std::invalid_argument("Invalid engine channel-array count");
            // Parse<float> allocates the new positive count, then copies the
            // OLD count before loading values. Shrinking therefore overruns.
            if(fields==1&&n>0&&std::size_t(n)<a.size())
                throw std::invalid_argument("Engine channel-list shrink copies beyond the replacement allocation");
            if(fields==1&&std::size_t(n)>lines.size()-i-1)
                throw std::invalid_argument("Truncated engine channel array");
            std::size_t index=0;
            if(fields==1)a.resize(std::size_t(n));
            const auto add=[&](const std::string& text){float f{};if(!scan(text,f)||!std::isfinite(f))throw std::invalid_argument("Invalid channel-array element");
                if(index<a.size())a[index]=f;else a.push_back(f);++index;};
            if(fields==1) {
                for(int j=0;j<n;++j)add(lines[++i]);
            } else {
                bool ended=false;
                while(++i<lines.size()) {
                    if(firstWord(lines[i]).rfind("endlist",0)==0){ended=true;break;}
                    add(lines[i]);
                }
                if(!ended)throw std::invalid_argument("Unterminated engine channel array");
            }
        } else {
            for(const auto* prefix:{"channelscale","channeltranslate"}) {
                const auto length=std::strlen(prefix);
                if(key.size()==length+1 && key.compare(0,length,prefix)==0 && key.back()>='0'&&key.back()<='3') {
                    float f=-1.0f;scan(value,f);
                    if(f>=0) {
                        auto& a=prefix[7]=='s'?d.channelScale:d.channelTranslate;
                        if(a.empty())a.assign(4,prefix[7]=='s'?1.0f:0.0f);
                        const auto index=std::size_t(key.back()-'0');
                        if(index>=a.size())throw std::invalid_argument("Indexed channel write exceeds the preceding engine channel list");
                        a[index]=f;
                    }
                }
            }
        }
    }
    if(d.type)validateDefinition(d);
    return d;
}
std::uint32_t ProcedureContext::nextRandom() {
    if(random)return random();
    randomState=randomState*214013u+2531011u;
    return (randomState>>16u)&32767u;
}

bool ProceduralTexture::initialize(const ProcedureDefinition& d,const ProcedureImage& source,
                                  ProcedureContext& context,std::string& error) {
    error.clear();
    if(initialized_){error="Procedural texture already initialized; replace it on resource reload";return false;}
    try {
        if(!d.type)throw std::invalid_argument("No engine procedural controller was constructed");
        validateDefinition(d);
        if(d.volumeTexture && !isDirty(*d.type))
            throw std::invalid_argument("A prior Dirty directive leaves the engine 3D flag set; replacement by a 2D controller is unsupported");
        const auto count=checkedPixels(source.width,source.height,source.depth);
        if(source.bytesPerPixel<1||source.bytesPerPixel>4||source.bytes.size()!=checkedProduct(count,source.bytesPerPixel))
            throw std::invalid_argument("Engine procedural source has inconsistent byte storage");
        if(source.depth!=1)throw std::invalid_argument("Controller input must be a engine 2D image before Dirty initialization");
        if(*d.type==neoshared::material::ProcedureType::Perlin && source.width!=source.height)
            throw std::invalid_argument("Engine Perlin writes width squared; nonsquare images are unsupported rather than overrun");
        if(*d.type==neoshared::material::ProcedureType::Water) {
            checkedPixels(d.waterWidth,d.waterHeight);
            if(d.waterWidth!=d.waterHeight)throw std::invalid_argument("Engine water solver uses width for both axes; nonsquare grids are unsupported");
        }
        if(*d.type==neoshared::material::ProcedureType::Arturo) {
            checkedPixels(d.arturoWidth,d.arturoHeight);
            if(!d.distort)throw std::invalid_argument("Engine non-distorted Arturo uploads an unwritten height buffer; no synthetic output is supplied");
        }
        if((*d.type==neoshared::material::ProcedureType::Life||*d.type==neoshared::material::ProcedureType::RingTexDistort||isDirty(*d.type)) && source.bytesPerPixel!=4)
            throw std::invalid_argument("This engine controller writes four bytes per pixel; a non-RGBA source is unsupported");
        dirtyInputs_.reset();
        definition_=d;definition_.volumeTexture=isDirty(*d.type);image_=source;snapshot_=source;
        height_.assign(count,0);lastToken_=*d.type==neoshared::material::ProcedureType::Arturo?-1:0;
        phase_=forceAngle_=waveAngle_=0;frame_=0;waterCounter_=0;waterPhase_=2;
        revision_=0;validTexture_=true;
        switch(*d.type) {
        case neoshared::material::ProcedureType::Perlin: buildPerlin(from_,context);buildPerlin(target_,context);break;
        case neoshared::material::ProcedureType::Wave:
            current_.resize(count);previous_.assign(count,127.0f);scratch_.resize(count);
            for(auto& f:current_)f=float(context.nextRandom()%256u);
            break;
        case neoshared::material::ProcedureType::Water:
            current_.assign(std::size_t(d.waterWidth)*std::size_t(d.waterHeight),127.0f);
            previous_.assign(current_.size(),0);scratch_.resize(current_.size());target_.resize(count);break;
        case neoshared::material::ProcedureType::Life:
            life_.resize(checkedProduct(count,4));
            for(std::size_t p=0;p<count;++p) {
                life_[4*p]=context.nextRandom()%100u>=61u?255:0;
                life_[4*p+1]=life_[4*p+2]=0;life_[4*p+3]=255;
            }
            // Init writes RAM after the original upload. Control later calls
            // glImage(true,NULL), NOT glImage(true,privateLifeBuffer).
            break;
        case neoshared::material::ProcedureType::Arturo: {
            const auto n=std::size_t(d.arturoWidth)*std::size_t(d.arturoHeight);
            current_.resize(n);previous_.assign(n,0);scratch_.resize(n);rowsWritten_.assign(source.height,0);
            for(int y=0;y<d.arturoHeight;++y)for(int x=0;x<d.arturoWidth;++x) {
                const float ux=float(x)/float(d.arturoWidth),uy=float(y)/float(d.arturoHeight);
                const float px=(ux+ux)-1.0f,py=(uy+uy)-1.0f;
                const float argument=(1.0f-std::sqrt(px*px+py*py))*pi;
                // K1 calls sinf at 0x36c1b4; K2 widens the float argument
                // before its sin call at 0x87552, then rounds back to float.
                const float value=d.game==EngineMaterialGame::Kotor1?std::sin(argument):float(std::sin(double(argument)));
                current_[std::size_t(y)*std::size_t(d.arturoWidth)+std::size_t(x)]=127.0f*value;
            }
            buildArturo(context);buildArturo(context);break;
        }
        case neoshared::material::ProcedureType::Dirty:case neoshared::material::ProcedureType::Dirty2:case neoshared::material::ProcedureType::Dirty3:dirty(context);++revision_;break;
        default:break;
        }
        initialized_=true;return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
std::int32_t ProceduralTexture::integerValue(float value) const noexcept {
    return truncEngine(value);
}
std::uint8_t ProceduralTexture::byteValue(float value) const noexcept {
    return static_cast<std::uint8_t>(integerValue(value));
}
float ProceduralTexture::cosine(float value) const noexcept {return cosLut(value);}
float ProceduralTexture::sine(float value) const noexcept {return sinLut(value);}
void ProceduralTexture::heightToImage() {
    const auto bpp=image_.bytesPerPixel;
    const auto parameter=[](const std::vector<float>& a,std::size_t i,float fallback){return i<a.size()?a[i]:fallback;};
    for(std::size_t p=0;p<height_.size();++p)for(unsigned c=0;c<bpp;++c) {
        if(bpp==4) {
            const float s=parameter(definition_.channelScale,c,1),t=parameter(definition_.channelTranslate,c,0);
            auto v=s>0?byteValue(float(height_[p])*s):std::uint8_t(0);
            if(t>0)v=std::uint8_t(v+byteValue(t*255.0f));
            image_.bytes[p*4+c]=v;
        } else {
            const float h=float(height_[p]>>4u);
            const float s=parameter(definition_.channelScale,2*c,1),t=parameter(definition_.channelTranslate,2*c,0);
            std::uint8_t v=s>0?std::uint8_t(byteValue(h*s)&15u):0;
            if(t>0)v=std::uint8_t((v+byteValue(t*15.0f))&15u);
            const float s2=parameter(definition_.channelScale,2*c+1,1),t2=parameter(definition_.channelTranslate,2*c+1,0);
            // The inspected high-nibble writes clear the low nibble. Do not
            // replace this with a conventional two-channel packed conversion.
            if(s2>0)v=std::uint8_t((v+(unsigned(byteValue(h*s2))<<4u))&240u);
            if(t2>0)v=std::uint8_t((v+(unsigned(byteValue(t2*15.0f))<<4u))&240u);
            image_.bytes[p*bpp+c]=v;
        }
    }
}
void ProceduralTexture::distortImage() {
    const unsigned w=image_.width,h=image_.height,bpp=image_.bytesPerPixel;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const auto n=std::size_t(y)*w+x;std::uint32_t sx,sy;
        if(definition_.distortAngle) {
            const float a=float(height_[n])*bits(0x3cc90fdb);
            const auto dx=integerValue(definition_.distortionAmplitude*sine(a)-0.5f);
            const auto dy=integerValue(definition_.distortionAmplitude*cosine(a)-0.5f);
            sx=(x+w+std::uint32_t(dx))&(w-1u);sy=(y+h+std::uint32_t(dy))&(h-1u);
        } else {
            const auto offset=integerValue((float(height_[n])-127.0f)*0.0078125f*definition_.distortionAmplitude-0.5f);
            sx=(x+std::uint32_t(offset))&(w-1u);sy=(y+std::uint32_t(offset))&(h-1u);
        }
        std::copy_n(snapshot_.bytes.data()+(std::size_t(sy)*w+sx)*bpp,bpp,image_.bytes.data()+n*bpp);
    }
}
void ProceduralTexture::buildPerlin(std::vector<std::uint8_t>& destination,ProcedureContext& context,int maximum) {
    const int width=int(image_.width);std::vector<std::vector<std::uint8_t>> octaves;float weight=1;
    for(int n=4;;n*=2,weight*=0.5f) {
        auto& a=octaves.emplace_back(checkedProduct(std::size_t(n),std::size_t(n)));
        for(auto& v:a) {
            if(maximum<0)v=byteValue(float(int(context.nextRandom()%128u)-64)*weight+127.0f);
            else v=maximum?byteValue(float(context.nextRandom()%unsigned(maximum))*weight):0;
        }
        if(n>width/2)break;
    }
    destination.resize(checkedProduct(std::size_t(width),std::size_t(width)));
    for(int y=0;y<width;++y)for(int x=0;x<width;++x) {
        int sum=127;unsigned n=4;
        for(const auto& a:octaves) {
            sum+=int(interpolate(a,int(n),float(y)/float(width)*float(n),float(x)/float(width)*float(n)))-127;n*=2;
        }
        destination[std::size_t(y)*std::size_t(width)+std::size_t(x)]=std::uint8_t(sum);
    }
}
void ProceduralTexture::buildArturo(ProcedureContext& context) {
    const int w=definition_.arturoWidth,h=definition_.arturoHeight;
    const auto chosenX=int(context.nextRandom()%unsigned(w)),chosenY=int(context.nextRandom()%unsigned(h));
    const auto at=[&](int x,int y){return current_[std::size_t(mod(y,h))*std::size_t(w)+std::size_t(mod(x,w))];};
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const auto p=std::size_t(y)*std::size_t(w)+std::size_t(x);
        float v=((at(x-1,y)+at(x+1,y))+at(x,y-1))+at(x,y+1);
        v=v*0.5f-previous_[p]+current_[p]*0.0f;
        if(context.randomImpulses&&x==chosenX&&y==chosenY)v+=float(int(context.nextRandom()%256u)-128);
        scratch_[p]=std::clamp(v+127.0f,0.0f,255.0f)-127.0f;
    }
    previous_=current_;current_=scratch_;
}
void ProceduralTexture::wave(ProcedureContext& context) {
    const int w=int(image_.width),h=int(image_.height);const float dt=solverElapsed(context.elapsed,definition_),t2=dt*dt;
    const std::array<float,9> kernel{{0,t2,0,t2,2.0f-4.0f*t2,t2,0,t2,0}};
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        float v=0;
        for(int yy=0;yy<3;++yy)for(int xx=0;xx<3;++xx) {
            const auto row=mod(y+w-1+yy,h),col=mod(x+h-1+xx,w);
            v+=current_[std::size_t(row)*std::size_t(w)+std::size_t(col)]*kernel[std::size_t(yy*3+xx)];
        }
        const auto p=std::size_t(y)*std::size_t(w)+std::size_t(x);previous_[p]=v-previous_[p];
    }
    current_.swap(previous_);
    if(context.randomImpulses) {
        const auto y=context.nextRandom()%image_.height,x=context.nextRandom()%image_.width;
        previous_[std::size_t(y)*image_.width+x]+=float(context.nextRandom()%32u);
    }
    for(std::size_t i=0;i<height_.size();++i){current_[i]=std::clamp(current_[i],40.0f,215.0f);height_[i]=byteValue(current_[i]);}
}
bool ProceduralTexture::water(ProcedureContext& context) {
    if(!context.lowQualityWater) {
        if(waterCounter_&1u){++waterCounter_;return false;}
        waterCounter_=0;
    }
    // Both engine builds advance the phase on every admitted solve, including
    // full-quality solves (K1 0x36b359; K2 0x86310).
    waterPhase_=(waterPhase_+1)%3;
    const int w=definition_.waterWidth,h=definition_.waterHeight;
    if(!context.lowQualityWater||waterPhase_==0) {
        const float dt=solverElapsed(context.elapsed,definition_)*definition_.speed;
        forceAngle_+=(definition_.forceCycleSpeed*dt)/bits(0x42652ee0);
        waveAngle_+=(definition_.angleCycleSpeed*dt)/bits(0x42652ee0);
        if(forceAngle_>twoPi)forceAngle_-=twoPi;
        if(waveAngle_>twoPi)waveAngle_-=twoPi;
        const float amplitude=cosine(forceAngle_)*0.25f+0.2f,direction=cosine(waveAngle_);
        std::array<float,9> kernel{{0.5f,1,0.5f,1,0,1,0.5f,1,0.5f}};
        kernel[0]=cosine(direction-pi*0.25f)*amplitude+0.5f;
        kernel[2]=cosine(direction-pi*0.75f)*amplitude+0.5f;
        kernel[8]=cosine(direction-pi*1.25f)*amplitude+0.5f;
        kernel[6]=cosine(direction-pi*1.75f)*amplitude+0.5f;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
            const auto p=std::size_t(y)*std::size_t(w)+std::size_t(x);const float offset=current_[p]-127.0f;float force=0;
            for(int yy=0;yy<3;++yy)for(int xx=0;xx<3;++xx) {
                const auto neighbor=std::size_t(mod(y-1+yy,h))*std::size_t(w)+std::size_t(mod(x-1+xx,w));
                const float term=(xx==1&&yy==1)?offset:offset-(current_[neighbor]-127.0f);
                force-=term*kernel[std::size_t(yy*3+xx)];
            }
            force+=previous_[p]*-0.025f;
            scratch_[p]=(dt*previous_[p]+offset)+((force*0.5f)*dt)*dt;
            previous_[p]+=force*dt;
        }
        if(context.randomImpulses) {
            const auto y=context.nextRandom()%unsigned(w),x=context.nextRandom()%unsigned(w);
            previous_[std::size_t(y)*std::size_t(w)+x]+=float(context.nextRandom()%127u);
        }
        for(std::size_t p=0;p<current_.size();++p) {
            const float v=scratch_[p]+127.0f;current_[p]=std::clamp(v,40.0f,215.0f);
            if(v<40.0f||v>215.0f)previous_[p]=0;
        }
        auto& output=context.lowQualityWater?target_:height_;
        const float sx=float(w)/float(image_.width),sy=float(h)/float(image_.height);
        for(unsigned y=0;y<image_.height;++y)for(unsigned x=0;x<image_.width;++x)
            output[std::size_t(y)*image_.width+x]=byteValue(sample(current_,w,h,float(x)*sx,float(y)*sy));
        if(context.lowQualityWater)std::fill(height_.begin(),height_.end(),0);
    }
    const auto width=int(image_.width),height=int(image_.height);
    if(context.lowQualityWater&&context.filterWater) {
        const int start=waterPhase_*height/3,end=(waterPhase_+1)*height/3;
        for(int y=start;y<end;++y)for(int x=0;x<width;++x) {
            std::uint8_t v=0;
            for(int yy=-1;yy<=1;++yy)for(int xx=-1;xx<=1;++xx) {
                if(xx&&yy)continue;
                const auto a=target_[std::size_t(mod(y+yy,height))*std::size_t(width)+std::size_t(mod(x+xx,width))];
                v=std::uint8_t(v+byteValue(float(a)*((xx==0&&yy==0)?0.5f:0.1f)));
            }
            height_[std::size_t(y)*std::size_t(width)+std::size_t(x)]=v;
        }
    } else if(!context.lowQualityWater&&context.filterWater) {
        target_=height_;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
            std::uint8_t v=0;
            for(int yy=-1;yy<=1;++yy)for(int xx=-1;xx<=1;++xx) {
                const auto a=target_[std::size_t(mod(y+yy,height))*std::size_t(width)+std::size_t(mod(x+xx,width))];
                if(context.integerWaterFilter) {
                    if(xx&&yy)continue;
                    v=std::uint8_t(v+(a>>((xx==0&&yy==0)?1u:3u)));
                } else v=std::uint8_t(v+byteValue(float(a)*((xx&&yy)?0.025f:(xx==0&&yy==0)?0.5f:0.1f)));
            }
            height_[std::size_t(y)*std::size_t(width)+std::size_t(x)]=v;
        }
    }
    // The partial-row return precedes the counter increment in both callers.
    if(context.lowQualityWater&&waterPhase_!=2)return false;
    ++waterCounter_;return true;
}
bool ProceduralTexture::arturo(ProcedureContext& context,std::string& error) {
    context.arturoPhase=(context.arturoPhase+1)%3;
    if(context.arturoPhase==0) {
        phase_+=context.elapsed*definition_.speed*1.5f;
        if(phase_>1) {buildArturo(context);while(phase_>1)phase_-=1;}
    }
    if(context.arturoPhase<2) {
        const unsigned start=context.arturoPhase==0?0:image_.height/2;
        const unsigned end=context.arturoPhase==0?image_.height/2:image_.height;
        const float dx=float(definition_.arturoWidth)/float(image_.width),dy=float(definition_.arturoHeight)/float(image_.height);
        float fy=float(start)*dy;
        for(unsigned y=start;y<end;++y,fy+=dy) {
            float fx=0;
            for(unsigned x=0;x<image_.width;++x,fx+=dx) {
                const auto old=sample(previous_,definition_.arturoWidth,definition_.arturoHeight,fx,fy);
                const auto now=sample(current_,definition_.arturoWidth,definition_.arturoHeight,fx,fy);
                height_[std::size_t(y)*image_.width+x]=byteValue(mix(old,now,phase_)+127.0f);
            }
            rowsWritten_[y]=1;
        }
        return false;
    }
    if(std::find(rowsWritten_.begin(),rowsWritten_.end(),0)!=rowsWritten_.end()) {
        error="Engine shared Arturo phase would upload rows this controller never initialized";return false;
    }
    return true;
}
void ProceduralTexture::life() {
    const auto old=life_;const int w=int(image_.width),h=int(image_.height);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        unsigned neighbors=0;
        for(int yy=-1;yy<=1;++yy)for(int xx=-1;xx<=1;++xx)if(xx||yy)
            neighbors+=old[(std::size_t(mod(y+yy,h))*std::size_t(w)+std::size_t(mod(x+xx,w)))*4]!=0;
        const auto p=(std::size_t(y)*std::size_t(w)+std::size_t(x))*4;const bool alive=old[p]!=0;
        if(neighbors==3||(alive&&neighbors==2)) {
            life_[p]=255;life_[p+1]=life_[p+2]=std::uint8_t(std::min(255u,unsigned(old[p+2])+4u));
        } else {
            life_[p]=life_[p+1]=0;life_[p+2]=alive?128:std::uint8_t(old[p+2]>4?old[p+2]-4:0);
        }
    }
}
void ProceduralTexture::ring(ProcedureContext& context) {
    const auto w=image_.width,h=image_.height;
    const float inverseWidth=1.0f/float(w); // K1 0x36bece / K2 0x883c2.
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const float xx=float(x+1u)*inverseWidth-0.5f,yy=float(y+1u)*inverseWidth-0.5f;
        const float a=std::sqrt(xx*xx+yy*yy)*context.ringFrequency+phase_*context.ringPhaseScale;
        const float component=(cosine(a)*context.ringAmplitude+0.5f)*255.0f;
        const auto value=byteValue(std::clamp(component,0.0f,255.0f));
        const auto p=(std::size_t(y)*w+x)*4;image_.bytes[p]=image_.bytes[p+1]=value;image_.bytes[p+2]=image_.bytes[p+3]=255;
    }
    phase_+=context.elapsed*context.ringSpeed;while(phase_>twoPi)phase_-=twoPi;
}
void ProceduralTexture::dirty(ProcedureContext& context) {
    const auto kind=*definition_.type;const auto index=kind==neoshared::material::ProcedureType::Dirty?0u:kind==neoshared::material::ProcedureType::Dirty2?1u:2u;
    auto inputs=context.dirty[index];
    // Init stores its formula at +0x40 as a signed byte. Preserve the pinned
    // controller value; later area changes do not rewrite a live controller.
    const auto low=static_cast<std::uint32_t>(inputs.parameter)&255u;
    inputs.parameter=low<128u?static_cast<int>(low):static_cast<int>(low)-256;
    dirtyInputs_=inputs;
    const auto pixels=checkedPixels(inputs.size,inputs.size,inputs.size);
    image_.width=image_.height=image_.depth=unsigned(inputs.size);image_.bytes.resize(checkedProduct(pixels,4));
    const std::array<int,4> color{{int((inputs.color>>16)&255u),int((inputs.color>>8)&255u),int(inputs.color&255u),
        (inputs.color&0x80000000u)?int(inputs.color>>24)-256:int(inputs.color>>24)}};
    std::array<std::vector<std::uint8_t>,3> planes;
    if(inputs.algorithm==1)for(auto& p:planes)buildPerlin(p,context,std::max({color[0],color[1],color[2]}));
    const auto n=std::size_t(inputs.size);
    for(std::size_t z=0;z<n;++z)for(std::size_t y=0;y<n;++y)for(std::size_t x=0;x<n;++x) {
        const int value=inputs.algorithm==1?int(planes[0][z*n+y])+int(planes[1][z*n+x])+int(planes[2][y*n+x]):int(context.nextRandom()&255u);
        const auto p=((z*n+y)*n+x)*4;
        for(unsigned c=0;c<4;++c)image_.bytes[p+c]=color[c]?std::uint8_t(value%color[c]):0;
    }
}
ProcedureChange ProceduralTexture::control(ProcedureContext& context,std::string& error) {
    error.clear();
    if(!initialized_){error="Procedural controller is not initialized";return ProcedureChange::None;}
    if(!context.enabled)return ProcedureChange::None;
    const auto kind=*definition_.type;
    // Arturo accumulates BEFORE comparing the update token. Multiple binds
    // can therefore affect another instance even within one rendered frame.
    if(kind==neoshared::material::ProcedureType::Arturo) {
        context.arturoElapsed+=context.elapsed;

        if(!(context.arturoElapsed>0.03f))return ProcedureChange::None;
        context.arturoElapsed-=std::floor(context.arturoElapsed/0.03f)*0.03f;
    }
    if(kind==neoshared::material::ProcedureType::Life) {
        // Engine LEA/SUB wraps in 32 bits before the signed comparison.
        if(lastToken_>model::engineIntAdd(context.token,-5))return ProcedureChange::None;
    } else if(lastToken_==context.token)return ProcedureChange::None;
    lastToken_=context.token;
    try {
        if(kind==neoshared::material::ProcedureType::Cycle) {
            phase_+=context.elapsed;
            // Engine division also permits zero/negative fps. Zero -> +inf;
            // negative rates advance once per token, not reverse playback.
            const float interval=1.0f/definition_.fps;
            if(phase_>interval) {
                phase_=0;frame_=(frame_+1u)%(unsigned(definition_.numX)*unsigned(definition_.numY));
                return ProcedureChange::Frame;
            }
            return ProcedureChange::None;
        }
        if(isDirty(kind))return ProcedureChange::None;
        if(kind==neoshared::material::ProcedureType::Life) {
            life();validTexture_=false;return ProcedureChange::InvalidTexture;
        }
        if(kind==neoshared::material::ProcedureType::Random)for(auto& b:height_)b=std::uint8_t(context.nextRandom());
        else if(kind==neoshared::material::ProcedureType::Perlin) {
            phase_+=context.elapsed*4.0f;
            if(phase_>1) {from_=target_;buildPerlin(target_,context);while(phase_>=1)phase_-=1;}
            for(std::size_t p=0;p<height_.size();++p)height_[p]=byteValue(mix(float(from_[p]),float(target_[p]),phase_));
        } else if(kind==neoshared::material::ProcedureType::Wave)wave(context);
        else if(kind==neoshared::material::ProcedureType::Water) {if(!water(context))return ProcedureChange::None;}
        else if(kind==neoshared::material::ProcedureType::Arturo) {
            if(!arturo(context,error))return ProcedureChange::None;
        } else if(kind==neoshared::material::ProcedureType::RingTexDistort) {
            ring(context);++revision_;return ProcedureChange::Image;
        }
        if((kind==neoshared::material::ProcedureType::Water&&image_.bytesPerPixel==1) ||
           (kind==neoshared::material::ProcedureType::Arturo&&definition_.bumpTexture)) {
            // Arturo supplies its one-byte height pointer to the bump loader.
            // The texture's stored BPP is unchanged; only the active upload span differs.
            std::copy(height_.begin(),height_.end(),image_.bytes.begin());
        }
        else if(definition_.distort)distortImage();else heightToImage();
        ++revision_;return ProcedureChange::Image;
    } catch(const std::exception& e){error=e.what();return ProcedureChange::None;}
}

std::vector<ProcedureImage> procedureNormalLevels(const ProcedureImage& image,float scale,bool authoredNormal) {


    const auto count=checkedPixels(image.width,image.height);
    if(image.depth!=1 || !std::isfinite(scale) || image.bytes.size()<count)
        throw std::invalid_argument("Invalid engine normal-map upload image/scale");
    const auto extent=[](unsigned n) {
        if((n&(n-1u))==0)return n;
        if(n>1 && ((n-1u)&(n-2u))==0)return n-1u;
        throw std::invalid_argument("Engine normal-map dimensions are neither power-of-two nor power-of-two plus one");
    };
    const auto w=extent(image.width),h=extent(image.height);
    ProcedureImage first;first.width=w;first.height=h;first.bytes.resize(checkedProduct(checkedPixels(w,h),4));
    if(authoredNormal) {
        if(image.bytes.size()<first.bytes.size())throw std::invalid_argument("Authored normal upload has fewer than four bytes per output pixel");
        for(unsigned y=0;y<h;++y)
            std::copy_n(image.bytes.begin()+std::size_t(y)*image.width*4,w*4,
                first.bytes.begin()+std::size_t(y)*w*4);
        for(std::size_t texel=0;texel<first.bytes.size();texel+=4)
            std::swap(first.bytes[texel],first.bytes[texel+2]);
    } else {
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
            const float v=float(image.bytes[std::size_t(y)*image.width+x])*bits(0x3b808081);
            const float right=float(image.bytes[std::size_t(y)*image.width+(x+1u)%image.width])*bits(0x3b808081);
            const float above=float(image.bytes[std::size_t((y+1u)%image.height)*image.width+x])*bits(0x3b808081);
            const float dx=(v-right)*scale,dy=(v-above)*scale;
            const float inv=1.0f/std::sqrt((dy*dy+dx*dx)+1.0f);
            const auto p=(std::size_t(y)*w+x)*4;
            first.bytes[p]=byte(inv*127.0f+128.0f);
            first.bytes[p+1]=byte((dx*inv)*-127.0f+128.0f);
            first.bytes[p+2]=byte((dy*inv)*127.0f+128.0f);
            first.bytes[p+3]=255;
        }
    }
    std::vector<ProcedureImage> levels;levels.push_back(std::move(first));
    while(levels.back().width>1||levels.back().height>1) {
        const auto& src=levels.back();ProcedureImage dst;
        dst.width=std::max(1u,src.width/2);dst.height=std::max(1u,src.height/2);
        dst.bytes.resize(std::size_t(dst.width)*dst.height*4);
        for(unsigned y=0;y<src.height;y+=2)for(unsigned x=0;x<src.width;x+=2) {
            const auto p00=(std::size_t(y)*src.width+x)*4;
            const auto p10=(std::size_t(y)*src.width+(x+1u)%src.width)*4;
            const auto p01=(std::size_t((y+1u)%src.height)*src.width+x)*4;
            const auto p11=(std::size_t((y+1u)%src.height)*src.width+(x+1u)%src.width)*4;
            std::array<float,3> v{};
            for(unsigned c=0;c<3;++c) {
                const auto term=[&](std::size_t p) {
                    return (float(src.bytes[p+c])*bits(0x3c010204)-1.0f)*
                           (float(src.bytes[p+3])*bits(0x3b808081));
                };
                v[c]=((term(p00)+term(p10))+term(p01))+term(p11);
            }
            if(!authoredNormal) {
                if(dst.width==1||dst.height==1||(dst.width==2&&dst.height==2))v={{1,0,0}};
                else if(dst.width==4&&dst.height==4){v[1]*=0.5f;v[2]*=0.5f;}
            }
            const float length=std::sqrt((v[1]*v[1]+v[2]*v[2])+v[0]*v[0]);
            const auto p=(std::size_t(y/2)*dst.width+x/2)*4;
            if(length==0){dst.bytes[p]=255;dst.bytes[p+1]=dst.bytes[p+2]=128;}
            else {
                const float inv=1.0f/length;
                for(unsigned c=0;c<3;++c)dst.bytes[p+c]=byte((v[c]*inv)*127.0f+128.0f);
            }
            dst.bytes[p+3]=byte(std::min(1.0f,length*0.25f)*255.0f);
        }
        levels.push_back(std::move(dst));
    }
    return levels;
}

neoshared::texture::TextureData constructProcedureSource(const std::string& txi,const std::string& resref,
                                                        EngineMaterialGame game) {
    const auto definition=procedureDefinition(neoshared::material::parseMaterialTxi(txi),game);
    if(!definition.type)throw std::invalid_argument("TXI-only resource did not construct a engine texture controller");
    const auto count=checkedPixels(definition.defaultWidth,definition.defaultHeight);
    neoshared::texture::TextureData result;
    result.kind=neoshared::texture::TextureFileKind::Txi;
    result.sourcePath=std::filesystem::u8path(resref+".txi");
    result.canvasWidth=unsigned(definition.defaultWidth);result.canvasHeight=unsigned(definition.defaultHeight);
    result.sourceEncoding=game==EngineMaterialGame::Kotor1?"K1 procedural RGBA":"K2 procedural RGBA";
    result.txi=txi;result.hasAlpha=true;
    result.txiStorage=neoshared::texture::TxiStorage::Sidecar;
    neoshared::texture::TextureLayer image;image.width=result.canvasWidth;image.height=result.canvasHeight;image.rgba.assign(checkedProduct(count,4),127);
    result.layers.push_back(std::move(image));return result;
}
} // namespace neomdl::material
