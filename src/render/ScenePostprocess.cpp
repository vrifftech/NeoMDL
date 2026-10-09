// desktop K1 ATI and K2 ARB/ATI framebuffer graphs.
// Desktop ARB: 0x92856 / 0x9592e.
#include "render/ScenePostprocess.hpp"
#include "render/SoftShadowPass.hpp"
#include "render/MaterialPass.hpp"
#include "render/DesktopAtiPostprocess.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neomdl::render {
namespace {
namespace g=gl;
constexpr g::Enum Fbo=0x8d40,DrawFbo=0x8ca9,ReadFbo=0x8ca8,FboBinding=0x8ca6,
    ReadFboBinding=0x8caa,Color0=0x8ce0,Complete=0x8cd5,
    Rgba8=0x8058,Rgba=0x1908,UByte=0x1401,
    Rect=0x84f5,RectBinding=0x84f6,Linear=0x2601,Clamp=0x2900,Repeat=0x2901,
    MinFilter=0x2801,MagFilter=0x2800,WrapS=0x2802,WrapT=0x2803,
    Viewport=0x0ba2,ClearColor=0x0c22,ClearDepth=0x0b73,ClearStencil=0x0b91,
    ColorBit=0x4000,Scissor=0x0c11,Srgb=0x8db9,Extensions=0x1f03,
    DrawBuffer=0x0c01,ReadBuffer=0x0c02,Unpack=0x88ec,UnpackBinding=0x88ef,ActiveTexture=0x84e0;
bool extension(const char* text,const char* token) {
    if(!text)return false;
    const std::string list(text),word(token);std::size_t at=0;
    while((at=list.find(word,at))!=std::string::npos) {
        const auto end=at+word.size();
        if((at==0||list[at-1]==' ')&&(end==list.size()||list[end]==' '))return true;
        at=end;
    }return false;
}
std::string objectLog(g::Api& a,g::UInt id,bool program) {
    g::Int n=0;if(program)a.GetProgramiv(id,g::InfoLogLength,&n);else a.GetShaderiv(id,g::InfoLogLength,&n);
    std::vector<char> bytes(static_cast<std::size_t>(std::max(n,1)),0);g::Size used=0;
    if(program)a.GetProgramInfoLog(id,static_cast<g::Size>(bytes.size()),&used,bytes.data());
    else a.GetShaderInfoLog(id,static_cast<g::Size>(bytes.size()),&used,bytes.data());
    return std::string(bytes.data(),static_cast<std::size_t>(std::clamp(used,0,static_cast<int>(bytes.size()))));
}
g::UInt compile(g::Api& a,g::Enum kind,const char* source) {
    const auto id=a.CreateShader(kind);if(!id)throw std::runtime_error("Cannot allocate scene-effect shader");
    a.ShaderSource(id,1,&source,nullptr);a.CompileShader(id);g::Int okay=0;a.GetShaderiv(id,g::CompileStatus,&okay);
    if(!okay) {const auto e=objectLog(a,id,false);a.DeleteShader(id);throw std::runtime_error(e);}return id;
}
// K2 ARB programs 0x528143..0x528e17 and the matching K1 ATI instruction
// streams at 0x302992 share arithmetic. K1 retains its normalized 2D capture
// and fixed fog stage; K2 retains rectangle sampling and its dream consumer.
const char* desktopVertex=R"GLSL(
attribute vec3 aPosition; attribute vec4 aColor; attribute vec2 aUv;
uniform mat4 uMvp;
varying vec2 vUv; varying vec4 vColor;
void main(){vUv=aUv;vColor=aColor;gl_Position=uMvp*vec4(aPosition,1.0);}
)GLSL";
const char* desktopFragment=R"GLSL(
#ifdef K1_DESKTOP
uniform sampler2D uScene;
uniform int uFogMode;
uniform vec3 uFogData, uFogColor;
#define sceneSample texture2D
#else
#extension GL_ARB_texture_rectangle : require
uniform sampler2DRect uScene;
#define sceneSample texture2DRect
#endif
uniform sampler2D uAux; uniform sampler2D uOutline; uniform sampler2D uLut;
uniform int uMode;
uniform vec3 uSize; uniform vec3 uRows[3]; uniform vec3 uTap[4];
uniform vec4 uParameters;
varying vec2 vUv; varying vec4 vColor;
vec4 postColor(vec4 c){
#ifdef K1_DESKTOP
    if(uFogMode!=0){
        // Only post-flare saturation can inherit fog: the first framebuffer
        // wrapper unconditionally disables it. This quad has eye-space z=0.
        float f=uFogMode==1?uFogData.y/(uFogData.y-uFogData.x):1.0;
        c.rgb=mix(uFogColor,c.rgb,clamp(f,0.0,1.0));
    }
#endif
    return c;
}
void main(){
    if(uMode==8){gl_FragColor=texture2D(uAux,vUv);
        gl_FragColor.a=1.0; // K1 ATI command 4 and K2 ARB Decal2D
        gl_FragColor=postColor(gl_FragColor);return;}
    if(uMode==6){
        vec3 r=texture2D(uAux,vUv+uTap[0].xy).rgb*uTap[0].z;
        r+=texture2D(uAux,vUv+uTap[1].xy).rgb*uTap[1].z;
        r+=texture2D(uAux,vUv+uTap[2].xy).rgb*uTap[2].z;
        r+=texture2D(uAux,vUv+uTap[3].xy).rgb*uTap[3].z;
        gl_FragColor=postColor(vec4(r,vColor.a));return;
    }
    if(uMode==7){
        vec3 d=2.0*texture2D(uAux,vUv).rgb-1.0;
        vec2 uv=vec2(dot(d,vec3(.15625,0.0,vUv.x)),dot(d,vec3(0.0,.15625,vUv.y)));
        vec4 outline=texture2D(uOutline,vUv);
        float gate=step(.500001,outline.a+.5);
        gl_FragColor=postColor(vec4(texture2D(uLut,uv).rgb*gate+outline.rgb,outline.a));return;
    }
    if(uMode==3){
        vec4 field=texture2D(uAux,vec2(vUv.x,1.0-vUv.y));
        vec3 d=field.rgb*2.0-1.0;
        vec2 pixel=vUv*uSize.xy;
        vec2 uv=vec2(dot(d,vec3(uSize.x/10.0,0.0,pixel.x)),dot(d,vec3(0.0,uSize.y/10.0,pixel.y)));
        gl_FragColor=postColor(vec4(sceneSample(uScene,uv).rgb,field.a));return;
    }
    vec4 c=sceneSample(uScene,vUv*uSize.xy);
    if(uMode==0)c.a=1.0; // RenderFrameToTexture selects Decal in both games
    if(uMode==1)c.rgb=vec3(dot(c.rgb,uRows[0]),dot(c.rgb,uRows[1]),dot(c.rgb,uRows[2]));
    if(uMode==2){
        vec2 uv=vec2(uParameters.x*(1.0-vUv.x),7.5*(1.0-vUv.y));
        c.rgb+=texture2D(uAux,uv).rgb;c.a=1.0;
    }
    if(uMode==4)c=vec4(texture2D(uAux,vUv).rgb*uParameters.x+c.rgb*(1.0-uParameters.x),1.0);
    if(uMode==5 || uMode==9){
        c.rgb=vec3(texture2D(uLut,vec2(c.r,.5)).r,texture2D(uLut,vec2(c.g,.5)).g,texture2D(uLut,vec2(c.b,.5)).b);
#ifdef K1_DESKTOP
        c.a=vColor.a;
#else
        c.a=1.0;
#endif
    }
    gl_FragColor=postColor(c);
}
)GLSL";
struct QuadVertex {float x,y,z,r,g,b,a,u,v;};
model::Mat4 ortho(float w,float h) {
    auto m=model::identityMatrix();m.values[0]=2/w;m.values[5]=2/h;m.values[10]=-1;
    m.values[12]=-1;m.values[13]=-1;return m;
}
}
struct ScenePostprocess::Impl {
    g::Api* api{};g::Loader resolver;
    using Gen=void(NEOMDL_GL_CALL*)(g::Size,g::UInt*);
    using Delete=void(NEOMDL_GL_CALL*)(g::Size,const g::UInt*);
    using Bind=void(NEOMDL_GL_CALL*)(g::Enum,g::UInt);
    using Attach=void(NEOMDL_GL_CALL*)(g::Enum,g::Enum,g::Enum,g::UInt,g::Int);
    using Check=g::Enum(NEOMDL_GL_CALL*)(g::Enum);
    using DrawBuffers=void(NEOMDL_GL_CALL*)(g::Size,const g::Enum*);
    Gen genFbo{};Delete deleteFbo{};Bind bindFbo{};
    Attach attach{};Check check{};DrawBuffers drawBuffers{};
    bool loaded{},separate{},srgb{},rectangle{};
    struct Target {g::UInt fbo{},texture{};int width{},height{};};
    Target history[2],desktopMips[4],desktopFiltered;
    g::UInt capture{},lut{},overbrightLut{},program{},vbo{},ibo{};
    g::Int mode{-1},mvp{-1},size{-1},parameters{-1},rows{-1},taps{-1},fogMode{-1},fogData{-1},fogColor{-1};
    int targetWidth{},targetHeight{},historyFront{};
    bool targetDesktopArbShaders{true};std::uint64_t epoch{};
    model::GameVersion targetGame{model::GameVersion::Kotor2},programGame{model::GameVersion::Kotor2};
    bool validHistory{},previousSpeed{},distorting{},pendingHistory{},pendingSpeed{};
    SceneEffectsSettings settings;std::size_t producers{};
    model::FogSettings sceneFog;bool desktopFogEnabled{};
    SceneEffectTextureBinding bindTexture;
    model::GameVersion runtimeGame{model::GameVersion::Kotor2};
    SceneEffectDimensions effectDimensions;
    EngineMaterialState* desktopState{};
    bool desktopShaderRan{};
    DesktopAtiPostprocessApi ati;
    std::array<g::UInt,3> atiLut{},atiOverbrightLut{};
    bool atiMatrices{};g::Int atiMatrixMode{},atiVertexProgram{},atiFragmentProgram{};
    std::array<std::array<float,3>,4> atiTaps{};
    struct Frame {
        g::Int viewport[4]{},draw{},read{},drawBuffer{},readBuffer{},drawFboReadBuffer{},unpack{},active{};
        std::vector<g::Enum> drawBuffers;
        g::Int unpackValues[4]{},rectBindings[4]{};
        g::Boolean rectEnabled[4]{},scissor{},srgb{};
        float clearColor[4]{},clearDepth{};g::Int clearStencil{};
    };
    std::unique_ptr<Frame> frame;
    bool k1Desktop() const {return runtimeGame==model::GameVersion::Kotor1;}
    bool k2Ati() const {return !k1Desktop()&&!settings.desktopArbShaders;}
    bool normalizedCapture() const {return k1Desktop()||k2Ati();}
    g::Enum captureTarget() const {return normalizedCapture()?g::Texture2D:Rect;}
    void load() {
        if(loaded)return;
        if(!resolver)throw std::runtime_error("Missing scene-effect GL resolver");
        auto& a=*api;int major=0,minor=0;const char* version=reinterpret_cast<const char*>(a.GetString(g::Version));
        if(version)std::sscanf(version,"%d.%d",&major,&minor);
        const char* ext=reinterpret_cast<const char*>(a.GetString(Extensions));
        separate=major>=3||extension(ext,"GL_ARB_framebuffer_object");
        if(!separate && !extension(ext,"GL_EXT_framebuffer_object"))throw std::runtime_error("Scene effects require framebuffer objects");
        srgb=major>=3||extension(ext,"GL_ARB_framebuffer_sRGB")||extension(ext,"GL_EXT_framebuffer_sRGB");
        rectangle=major>=3||extension(ext,"GL_ARB_texture_rectangle");
        const auto fn=[&](const char* name,const char* legacy){return resolver(separate?name:legacy);};
        genFbo=reinterpret_cast<Gen>(fn("glGenFramebuffers","glGenFramebuffersEXT"));
        deleteFbo=reinterpret_cast<Delete>(fn("glDeleteFramebuffers","glDeleteFramebuffersEXT"));
        bindFbo=reinterpret_cast<Bind>(fn("glBindFramebuffer","glBindFramebufferEXT"));
        attach=reinterpret_cast<Attach>(fn("glFramebufferTexture2D","glFramebufferTexture2DEXT"));
        check=reinterpret_cast<Check>(fn("glCheckFramebufferStatus","glCheckFramebufferStatusEXT"));
        if(!genFbo||!deleteFbo||!bindFbo||!attach||!check)
            throw std::runtime_error("Missing scene-effect framebuffer entry points");
        drawBuffers=reinterpret_cast<DrawBuffers>(resolver("glDrawBuffers"));
        if(!drawBuffers)drawBuffers=reinterpret_cast<DrawBuffers>(resolver("glDrawBuffersARB"));
        if(!drawBuffers)throw std::runtime_error("Missing scene-effect draw-buffer entry point");
        loaded=true;
    }
    void save() {
        frame=std::make_unique<Frame>();auto& f=*frame;auto& a=*api;
        a.GetIntegerv(Viewport,f.viewport);a.GetFloatv(ClearColor,f.clearColor);a.GetFloatv(ClearDepth,&f.clearDepth);
        a.GetIntegerv(ClearStencil,&f.clearStencil);a.GetIntegerv(FboBinding,&f.draw);
        if(separate)a.GetIntegerv(ReadFboBinding,&f.read);else f.read=f.draw;
        a.GetIntegerv(DrawBuffer,&f.drawBuffer);a.GetIntegerv(ReadBuffer,&f.readBuffer);
        if(f.draw) {
            g::Int count=0;a.GetIntegerv(0x8824 /* GL_MAX_DRAW_BUFFERS */,&count);
            if(count<1)throw std::runtime_error("Invalid host draw-buffer limit");
            f.drawBuffers.resize(static_cast<std::size_t>(count));
            for(int n=0;n<count;++n){g::Int v=0;a.GetIntegerv(0x8825u+unsigned(n),&v);f.drawBuffers[static_cast<std::size_t>(n)]=static_cast<g::Enum>(v);}
        }
        f.drawFboReadBuffer=f.readBuffer;
        if(separate&&f.draw!=f.read) {
            bindFbo(ReadFbo,f.draw);a.GetIntegerv(ReadBuffer,&f.drawFboReadBuffer);bindFbo(ReadFbo,f.read);
        }
        a.GetIntegerv(UnpackBinding,&f.unpack);a.GetIntegerv(ActiveTexture,&f.active);
        constexpr g::Enum keys[]={0x0cf5,0x0cf2,0x0cf3,0x0cf4};
        for(unsigned n=0;n<4;++n)a.GetIntegerv(keys[n],&f.unpackValues[n]);
        f.scissor=a.IsEnabled(Scissor);if(srgb)f.srgb=a.IsEnabled(Srgb);
        if(rectangle)for(unsigned n=0;n<4;++n){a.ActiveTexture(g::Texture0+n);a.GetIntegerv(RectBinding,&f.rectBindings[n]);f.rectEnabled[n]=a.IsEnabled(Rect);}
        a.ActiveTexture(static_cast<g::Enum>(f.active));
    }
    void restore() noexcept {
        if(!api||!frame)return;
        auto& a=*api;auto& f=*frame;
        if(atiMatrices) {
            for(unsigned n=0;n<6;++n){a.ActiveTexture(g::Texture0+n);ati.x.MatrixMode(0x1702);ati.x.PopMatrix();}
            ati.x.MatrixMode(0x1700);ati.x.PopMatrix();ati.x.MatrixMode(0x1701);ati.x.PopMatrix();
            ati.x.PopAttrib();ati.x.BindProgramARB(desktop_programs::VP,static_cast<g::UInt>(atiVertexProgram));
            ati.x.BindProgramARB(desktop_programs::FP,static_cast<g::UInt>(atiFragmentProgram));
            ati.x.MatrixMode(atiMatrixMode);atiMatrices=false;
        }
        bindFbo(Fbo,f.draw);
        if(f.draw&&!f.drawBuffers.empty())drawBuffers(static_cast<g::Size>(f.drawBuffers.size()),f.drawBuffers.data());
        else a.DrawBuffer(f.drawBuffer);
        // Read-buffer selection belongs to its FBO, including the temporarily
        // used draw surface when the host has distinct draw/read framebuffers.
        a.ReadBuffer(f.drawFboReadBuffer);
        if(separate)bindFbo(ReadFbo,f.read);
        a.ReadBuffer(f.readBuffer);
        a.Viewport(f.viewport[0],f.viewport[1],f.viewport[2],f.viewport[3]);
        a.ClearColor(f.clearColor[0],f.clearColor[1],f.clearColor[2],f.clearColor[3]);a.ClearDepth(f.clearDepth);a.ClearStencil(f.clearStencil);
        if(f.scissor)a.Enable(Scissor);else a.Disable(Scissor);
        if(srgb){if(f.srgb)a.Enable(Srgb);else a.Disable(Srgb);}
        constexpr g::Enum keys[]={0x0cf5,0x0cf2,0x0cf3,0x0cf4};
        a.BindBuffer(Unpack,f.unpack);for(unsigned n=0;n<4;++n)a.PixelStorei(keys[n],f.unpackValues[n]);
        if(rectangle)for(unsigned n=0;n<4;++n){a.ActiveTexture(g::Texture0+n);a.BindTexture(Rect,f.rectBindings[n]);if(f.rectEnabled[n])a.Enable(Rect);else a.Disable(Rect);}
        a.ActiveTexture(f.active);frame.reset();distorting=false;bindTexture={};
    }
    void error(const char* where) {
        const auto e=api->GetError();if(e!=g::NoError)throw std::runtime_error(std::string("Scene effects ")+where+" (GL "+std::to_string(e)+")");
    }
    void bindTarget(const Target& t) {
        bindFbo(Fbo,t.fbo);api->DrawBuffer(Color0);api->ReadBuffer(Color0);api->Viewport(0,0,t.width,t.height);
    }
    void bindHost() {
        // Engine framebuffer zero is the caller's scene surface in an embedded
        // viewer. Never accidentally render into a different window's default FBO.
        bindFbo(Fbo,frame->draw);api->DrawBuffer(frame->drawBuffer);api->ReadBuffer(frame->drawBuffer);
        api->Viewport(frame->viewport[0],frame->viewport[1],frame->viewport[2],frame->viewport[3]);
    }
    void makeTexture(g::UInt& id,g::Enum target,int w,int h,const void* pixels=nullptr) {
        auto& a=*api;a.ActiveTexture(g::Texture0);a.GenTextures(1,&id);
        if(!id)throw std::runtime_error("Cannot allocate scene-effect texture");
        a.BindTexture(target,id);a.TexParameteri(target,MinFilter,Linear);a.TexParameteri(target,MagFilter,Linear);
        a.TexParameteri(target,WrapS,Clamp);a.TexParameteri(target,WrapT,Clamp);
        a.TexImage2D(target,0,Rgba8,w,h,0,Rgba,UByte,pixels);
        error("texture allocation");
    }
    void makeTarget(Target& t,int w,int h) {
        t.width=w;t.height=h;makeTexture(t.texture,g::Texture2D,w,h);
        genFbo(1,&t.fbo);if(!t.fbo)throw std::runtime_error("Cannot allocate scene-effect framebuffer");
        bindTarget(t);attach(Fbo,Color0,g::Texture2D,t.texture,0);
        if(check(Fbo)!=Complete)throw std::runtime_error("Incomplete scene-effect color framebuffer");
    }
    void clearColorTarget() {auto& a=*api;a.ColorMask(1,1,1,1);a.Disable(Scissor);a.ClearColor(0,0,0,0);a.Clear(ColorBit);}
    void releaseTargets() noexcept {
        if(!api)return;
        auto& a=*api;
        const auto destroy=[&](Target& t){if(t.fbo&&deleteFbo)deleteFbo(1,&t.fbo);if(t.texture)a.DeleteTextures(1,&t.texture);t={};};
        for(auto& t:history)destroy(t);
        for(auto& t:desktopMips)destroy(t);
        destroy(desktopFiltered);
        for(auto& id:atiLut){if(id)a.DeleteTextures(1,&id);id=0;}
        for(auto& id:atiOverbrightLut){if(id)a.DeleteTextures(1,&id);id=0;}
        for(auto* id:{&capture,&lut,&overbrightLut}){if(*id)a.DeleteTextures(1,id);*id=0;}
        targetWidth=targetHeight=0;validHistory=previousSpeed=false;
    }
    void createDesktopProgram() {
        if(program&&programGame==runtimeGame)return;
        auto& a=*api;
        if(program){a.DeleteProgram(program);program=0;}
        g::UInt vs=0,fs=0,next=0;
        try {
            const std::string prefix=std::string("#version 120\n")+(k1Desktop()?"#define K1_DESKTOP\n":"");
            const std::string vertex=prefix+desktopVertex,fragment=prefix+desktopFragment;
            vs=compile(a,g::VertexShader,vertex.c_str());
            fs=compile(a,g::FragmentShader,fragment.c_str());
            next=a.CreateProgram();a.AttachShader(next,vs);a.AttachShader(next,fs);
            a.BindAttribLocation(next,0,"aPosition");a.BindAttribLocation(next,2,"aColor");a.BindAttribLocation(next,3,"aUv");a.LinkProgram(next);
            g::Int okay=0;a.GetProgramiv(next,g::LinkStatus,&okay);if(!okay)throw std::runtime_error(objectLog(a,next,true));
            a.DeleteShader(vs);vs=0;a.DeleteShader(fs);fs=0;program=next;programGame=runtimeGame;next=0;
            a.UseProgram(program);a.Uniform1i(a.GetUniformLocation(program,"uScene"),0);a.Uniform1i(a.GetUniformLocation(program,"uAux"),1);
            a.Uniform1i(a.GetUniformLocation(program,"uOutline"),2);a.Uniform1i(a.GetUniformLocation(program,"uLut"),3);
            mode=a.GetUniformLocation(program,"uMode");mvp=a.GetUniformLocation(program,"uMvp");size=a.GetUniformLocation(program,"uSize");
            parameters=a.GetUniformLocation(program,"uParameters");rows=a.GetUniformLocation(program,"uRows[0]");taps=a.GetUniformLocation(program,"uTap[0]");
            fogMode=a.GetUniformLocation(program,"uFogMode");fogData=a.GetUniformLocation(program,"uFogData");fogColor=a.GetUniformLocation(program,"uFogColor");
        }catch(...){if(vs)a.DeleteShader(vs);if(fs)a.DeleteShader(fs);if(next)a.DeleteProgram(next);throw;}
    }
    void ensure() {
        auto& a=*api;const int w=frame->viewport[2],h=frame->viewport[3];
        const auto dimensions=sceneEffectDimensions(w,h,runtimeGame,settings.desktopArbShaders);effectDimensions=dimensions;
        if(frame->drawBuffer==0)throw std::runtime_error("Scene effects require a writable primary host draw buffer");
        if(!k1Desktop()&&settings.desktopDreamLetterbox&&(*settings.desktopDreamLetterbox)[1]>h)
            throw std::runtime_error("Desktop dream letterbox exceeds the scene viewport");
        if(!normalizedCapture()&&!rectangle)throw std::runtime_error("Desktop ARB scene effects require rectangle textures");
        g::Int max=0,samples=0;a.GetIntegerv(0x0d33,&max);a.GetIntegerv(0x80a8,&samples);
        if(samples!=0)throw std::runtime_error("Engine scene-effect capture requires a single-sample host target");
        if(dimensions.sceneWidth>max||dimensions.sceneHeight>max||dimensions.captureWidth>max||dimensions.captureHeight>max||max<512)throw std::runtime_error("Scene-effect viewport exceeds texture limits");
        if(producers) {g::Int bits=0;a.GetIntegerv(g::StencilBits,&bits);
            if(bits==0)throw std::runtime_error("Desktop distortion requires a stencil attachment");}
        if(!normalizedCapture()){g::Int rectMax=0;a.GetIntegerv(0x84f8,&rectMax);if(w>rectMax||h>rectMax)throw std::runtime_error("Desktop capture exceeds rectangle texture limits");}
        if(k2Ati())ati.load(resolver);else createDesktopProgram();
        a.BindBuffer(Unpack,0);a.PixelStorei(0x0cf5,1);a.PixelStorei(0x0cf2,0);a.PixelStorei(0x0cf3,0);a.PixelStorei(0x0cf4,0);
        if(!vbo)a.GenBuffers(1,&vbo);
        if(!ibo){a.GenBuffers(1,&ibo);const unsigned indices[]={0,1,2,0,2,3};a.BindBuffer(g::ElementArrayBuffer,ibo);a.BufferData(g::ElementArrayBuffer,sizeof(indices),indices,g::StaticDraw);}
        if(!vbo||!ibo)throw std::runtime_error("Cannot allocate scene-effect quad buffers");
        if(w==targetWidth&&h==targetHeight&&targetGame==runtimeGame&&targetDesktopArbShaders==settings.desktopArbShaders)return;
        releaseTargets();
        try {
            for(auto& t:history){
                makeTarget(t,dimensions.historyWidth,dimensions.historyHeight);
                // K1 ATI initialization 0x313ac7..0x313b02 clears its history
                // pbuffer to transparent black before any speed frame.
                if(k1Desktop())clearColorTarget();
            }
            makeTexture(capture,captureTarget(),dimensions.captureWidth,dimensions.captureHeight);
            if(k2Ati()) {
                // Init0x98e45..98ec4 only allocates/copies this texture.
                // History0x9358c does not set filtering; retain GL defaults
                // until a later engine helper explicitly sets clamp/linear.
                a.TexParameteri(g::Texture2D,WrapS,Repeat);a.TexParameteri(g::Texture2D,WrapT,Repeat);
                a.TexParameteri(g::Texture2D,MinFilter,0x2702);a.TexParameteri(g::Texture2D,MagFilter,Linear);
            }
            for(unsigned n=0;n<4;++n)makeTarget(desktopMips[n],64<<n,64<<n);
            makeTarget(desktopFiltered,256,256);
            // The engine 1D lookup tables are sampled at the exact same
            // centers using a single-row 2D texture; byte quantization is
            // performed before filtering, not approximated in the shader.
            std::array<unsigned char,256*4> bytes{};
            for(unsigned n=0;n<256;++n){
                bytes[4*n]=n<=233?0:static_cast<unsigned char>((float(n)-234)/21*255*.7f);
                bytes[4*n+1]=bytes[4*n+2]=n<=215?0:static_cast<unsigned char>((float(n)-216)/39*255*.7f);
            }
            makeTexture(lut,g::Texture2D,256,1,bytes.data());
            for(unsigned n=0;n<256;++n){const float x=float(n)/255;const float y=x<.5f?2*x*x:std::sqrt((x-.5f)*.5f)+.5f;
                bytes[4*n]=bytes[4*n+1]=bytes[4*n+2]=static_cast<unsigned char>(y*255);bytes[4*n+3]=0;}
            makeTexture(overbrightLut,g::Texture2D,256,1,bytes.data());
            if(k2Ati()) {
                for(unsigned channel=0;channel<3;++channel)for(bool overbright:{false,true}) {
                    std::array<unsigned char,256*4> table{};
                    for(unsigned n=0;n<256;++n) {
                        if(overbright)table[4*n+channel]=bytes[4*n+channel];
                        else if(channel==0)table[4*n]=n<=233?0:static_cast<unsigned char>((float(n)-234)/21*255*.7f);
                        else table[4*n+channel]=n<=215?0:static_cast<unsigned char>((float(n)-216)/39*255*.7f);
                    }
                    auto& id=(overbright?atiOverbrightLut:atiLut)[channel];
                    a.GenTextures(1,&id);a.BindTexture(0x0de0,id);
                    ati.TexImage1D(0x0de0,0,Rgba8,256,0,Rgba,UByte,table.data());
                    a.TexParameteri(0x0de0,WrapS,Clamp);a.TexParameteri(0x0de0,MinFilter,Linear);a.TexParameteri(0x0de0,MagFilter,Linear);
                }
            }
            targetWidth=w;targetHeight=h;targetGame=runtimeGame;targetDesktopArbShaders=settings.desktopArbShaders;historyFront=0;
            error("target initialization");
        }catch(...){releaseTargets();throw;}
    }
    void quad(float alpha=1,bool overscan=false) {
        auto& a=*api;const float w=float(frame->viewport[2]),h=float(frame->viewport[3]);
        const float left=overscan?-25.f:0, right=overscan?w+25:w;
        const float bottom=overscan?(settings.desktopDreamLetterbox?float((*settings.desktopDreamLetterbox)[0])-25:-25.f):0;
        const float top=overscan?(settings.desktopDreamLetterbox?float((*settings.desktopDreamLetterbox)[1])+25:h+25):h;
        const QuadVertex vertices[]={{left,bottom,0,1,1,1,alpha,0,0},{right,bottom,0,1,1,1,alpha,1,0},
            {right,top,0,1,1,1,alpha,1,1},{left,top,0,1,1,1,alpha,0,1}};
        a.BindBuffer(g::ArrayBuffer,vbo);a.BufferData(g::ArrayBuffer,sizeof(vertices),vertices,g::DynamicDraw);
        const auto attr=[&](unsigned slot,int count,std::size_t at){a.EnableVertexAttribArray(slot);a.VertexAttribPointer(slot,count,g::Float,0,sizeof(QuadVertex),reinterpret_cast<const void*>(at));};
        attr(0,3,offsetof(QuadVertex,x));attr(2,4,offsetof(QuadVertex,r));attr(3,2,offsetof(QuadVertex,u));
        constexpr float ones[]={1,1,1,1},uv[]={1,1,0,1};
        for(unsigned slot:{1u,4u,5u,6u}){a.DisableVertexAttribArray(slot);a.VertexAttrib4fv(slot,slot==4?uv:ones);}
        a.BindBuffer(g::ElementArrayBuffer,ibo);
    }
    void flatState(bool blend=false,g::Enum source=g::One,g::Enum destination=g::Zero) {
        auto& a=*api;a.Disable(g::DepthTest);a.DepthMask(0);a.Disable(g::Lighting);a.Disable(g::Fog);
        // ATI helpers inherit alpha, and several only change blend factors.
        // The enclosing first pass disables both once at0x902ca..90308.
        if(!k2Ati())a.Disable(g::AlphaTest);
        a.Disable(g::CullFace);a.Disable(g::StencilTest);a.ColorMask(1,1,1,1);a.PolygonMode(g::FrontAndBack,g::Fill);
        if(blend)a.Enable(g::Blend);else if(!k2Ati())a.Disable(g::Blend);
        a.BlendEquationSeparate(g::FuncAdd,g::FuncAdd);a.BlendFuncSeparate(source,destination,source,destination);
    }
    g::UInt texture(SceneEffectTexture role,unsigned unit,bool clamp=false,bool repeat=false) {
        if(!bindTexture)throw std::runtime_error("Scene effects require a engine texture Bind callback");
        std::string e;const auto id=bindTexture(role,e);
        if(!id||!e.empty())throw std::runtime_error(e.empty()?std::string("Missing ")+sceneEffectTextureLabel(role)+" texture":e);
        api->ActiveTexture(g::Texture0+unit);api->BindTexture(g::Texture2D,id);
        if(clamp||repeat){api->TexParameteri(g::Texture2D,WrapS,repeat?Repeat:Clamp);api->TexParameteri(g::Texture2D,WrapT,repeat?Repeat:Clamp);}
        if(clamp){api->TexParameteri(g::Texture2D,MinFilter,Linear);api->TexParameteri(g::Texture2D,MagFilter,Linear);}
        if(desktopState)desktopState->bind(role==SceneEffectTexture::ForceDistortion?0:
            role==SceneEffectTexture::FilmNoise?1:3,false,id);
        return id;
    }
    void captureHost() {
        bindHost();api->ActiveTexture(g::Texture0);api->BindTexture(captureTarget(),capture);
        api->CopyTexSubImage2D(captureTarget(),0,0,0,frame->viewport[0],frame->viewport[1],
            effectDimensions.captureWidth,effectDimensions.captureHeight);
        error("Desktop scene capture");
    }
    void saveAtiMatrices() {
        auto& a=*api;ati.load(resolver);a.GetIntegerv(0x0ba0,&atiMatrixMode);
        ati.x.GetProgramivARB(desktop_programs::VP,desktop_programs::ProgramBinding,&atiVertexProgram);
        ati.x.GetProgramivARB(desktop_programs::FP,desktop_programs::ProgramBinding,&atiFragmentProgram);
        ati.x.PushAttrib(0x000fffff);
        ati.x.MatrixMode(0x1700);ati.x.PushMatrix();ati.x.MatrixMode(0x1701);ati.x.PushMatrix();
        for(unsigned n=0;n<6;++n){a.ActiveTexture(g::Texture0+n);ati.x.MatrixMode(0x1702);ati.x.PushMatrix();}
        ati.x.MatrixMode(atiMatrixMode);a.ActiveTexture(frame->active);atiMatrices=true;
    }
    void atiBind(unsigned unit,unsigned target,g::UInt texture,bool enable=true) {
        static constexpr g::Enum targets[]={g::Texture2D,g::TextureCube,0x806f,0x0de0};
        auto& a=*api;a.ActiveTexture(g::Texture0+unit);a.BindTexture(targets[target],texture);
        if(enable)a.Enable(targets[target]);else a.Disable(targets[target]);
        auto& state=*desktopState;state.fixedTextureUnits[unit].enabled[target]=enable;
        if(target<2)state.bind(unit,target==1,texture);
        else if(target==2)state.bindVolume(unit,texture);else state.bind1D(unit,texture);
        state.fixedActiveTextureUnit=unit;
    }
    void atiEnable(unsigned unit,unsigned target,bool enable) {
        static constexpr g::Enum targets[]={g::Texture2D,g::TextureCube,0x806f,0x0de0};
        api->ActiveTexture(g::Texture0+unit);
        if(enable)api->Enable(targets[target]);else api->Disable(targets[target]);
        desktopState->fixedTextureUnits[unit].enabled[target]=enable;
        desktopState->fixedActiveTextureUnit=unit;
    }
    void atiCleanup() {
        // FrameBufferModificationPixelShadersATI command2,0x9626f..963b7.
        atiBind(0,0,0,false);
        for(unsigned n=1;n<4;++n){atiBind(n,0,0,false);atiBind(n,3,0,false);atiEnable(n,1,false);}
        atiBind(3,1,0,false);api->ActiveTexture(g::Texture0);desktopState->fixedActiveTextureUnit=0;
    }
    void applyAtiTextureImage() {
        auto& a=*api;applyDesktopFixedTextureState(a,ati.x,*desktopState);
        for(unsigned n=0;n<6;++n) {
            a.ActiveTexture(g::Texture0+n);if(rectangle)a.Disable(Rect);
            ati.x.MatrixMode(0x1702);ati.x.LoadMatrixf(desktopState->textureMatrix(n).values.data());
        }
        a.ActiveTexture(g::Texture0+desktopState->fixedActiveTextureUnit);
    }
    void atiDraw(int op,g::UInt aux,g::UInt outline,g::UInt,
                 const std::array<float,4>& params,float alpha,bool overscan) {
        auto& a=*api;auto& x=ati.x;applyAtiTextureImage();
        a.UseProgram(0);a.Disable(desktop_programs::FP);a.Disable(desktop_programs::VP);
        if(desktopFogEnabled) {
            a.Enable(g::Fog);x.Fogf(0x0b63,sceneFog.start);x.Fogf(0x0b64,sceneFog.end);x.Fogf(0x0b62,sceneFog.density);
            const float color[]={sceneFog.color.x,sceneFog.color.y,sceneFog.color.z,1};x.Fogfv(0x0b66,color);
            x.Fogf(0x0b65,float(sceneFog.mode==model::FogMode::Exponential?0x0800:sceneFog.mode==model::FogMode::ExponentialSquared?0x0801:0x2601));
        }
        const float w=float(frame->viewport[2]),h=float(frame->viewport[3]);
        const float uScale=w/effectDimensions.captureWidth,vScale=h/effectDimensions.captureHeight;
        g::Int viewport[4]{};a.GetIntegerv(Viewport,viewport);
        const float drawWidth=float(viewport[2]),drawHeight=float(viewport[3]);
        const auto identity=model::identityMatrix();
        x.MatrixMode(0x1701);x.LoadMatrixf(identity.values.data());ati.Ortho(0,drawWidth,0,drawHeight,-1,1);
        x.MatrixMode(0x1700);x.LoadMatrixf(identity.values.data());
        const auto captureParameters=[&](unsigned unit) {
            a.ActiveTexture(g::Texture0+unit);
            a.TexParameteri(g::Texture2D,WrapS,Clamp);a.TexParameteri(g::Texture2D,WrapT,Clamp);
            a.TexParameteri(g::Texture2D,MinFilter,Linear);a.TexParameteri(g::Texture2D,MagFilter,Linear);
        };
        if(op==0||op==1||op==2||op==5||op==9){atiBind(0,0,capture);if(op!=0)captureParameters(0);}
        if(op==2)atiBind(1,0,aux);
        if(op==4){atiBind(0,0,aux);atiBind(1,0,capture);captureParameters(1);}
        if(op==5||op==9)for(unsigned n=0;n<3;++n)atiBind(n+1,3,(op==5?atiLut:atiOverbrightLut)[n]);
        if(op==6)for(unsigned n=0;n<4;++n)atiBind(n,0,aux);
        if(op==8)atiBind(0,0,aux);
        if(op==7){a.Enable(g::AlphaTest);atiBind(0,0,aux);atiBind(1,0,0);atiBind(2,0,history[1-historyFront].texture);atiBind(3,0,outline);}
        if(op==3) {
            atiBind(0,0,aux);atiBind(1,0,0);atiBind(2,0,capture);captureParameters(2);
            std::string e;const auto id=ati.program(a,desktop_programs::VP,DesktopAtiForceVertex,e);
            if(!id)throw std::runtime_error(e);
            x.BindProgramARB(desktop_programs::VP,id);const float constant[]={0,1,0,0};
            x.ProgramLocalParameter4fvARB(desktop_programs::VP,15,constant);a.Enable(desktop_programs::VP);overscan=true;
        }
        const float left=overscan?-25.f:0,right=overscan?drawWidth+25:drawWidth;
        const bool letterbox=overscan&&op==7&&bool(settings.desktopDreamLetterbox);
        const float low=overscan?(letterbox?float((*settings.desktopDreamLetterbox)[0])-25:-25.f):0;
        const float high=overscan?(letterbox?float((*settings.desktopDreamLetterbox)[1])+25:drawHeight+25):drawHeight;
        const auto tc=[&](unsigned unit,float u,float v,float z=0,float q=1){ati.MultiTexCoord4f(g::Texture0+unit,u,v,z,q);};
        // Engine immediate order: upper-left, lower-left, lower-right, upper-right.
        ati.Begin(7);
        for(unsigned n=0;n<4;++n) {
            const float u=n>=2?1.f:0.f,v=(n==0||n==3)?1.f:0.f;
            ati.Color4f(1,op==4?0.f:1.f,1,op==6||op==7?alpha:1.f);
            if(op==4){tc(0,u,v);tc(1,u*uScale,v*vScale);}
            else if(op==6)for(unsigned k=0;k<4;++k)tc(k,u+atiTaps[k][0],v+atiTaps[k][1]);
            else if(op==7){tc(0,u,v);tc(1,.15625f,0,u);tc(2,0,.15625f,v);tc(3,u,v);}
            else if(op==3){tc(0,u,1-v);tc(1,uScale/10,vScale/10);tc(2,u*uScale,v*vScale);}
            else if(op==8)tc(0,u,v);
            else {tc(0,u*uScale,v*vScale);if(op==2)tc(1,params[0]*(1-u),7.5f*(1-v));}
            ati.Vertex4f(u?right:left,v?high:low,0,1);
        }
        ati.End();a.Disable(desktop_programs::VP);
        if(op==7) {
            // Dream/Fury retain the force texture on unit0, unlike command2.
            for(unsigned n=1;n<4;++n)atiBind(n,0,0,false);
        }else if(op==4) {
            // Accumulation0x934f7..9357b only touches the two2D inputs.
            atiBind(0,0,0,false);atiBind(1,0,0,false);
        }else if(op==8) {
            // MipmapFilterATI only unbinds unit0. Its enable bit remains set.
            atiBind(0,0,0);
        }else if(op!=6)atiCleanup();
        a.ActiveTexture(g::Texture0);desktopState->fixedActiveTextureUnit=0;
        error("engine ATI fixed draw");
    }
    void desktopDraw(int op,g::UInt aux=0,g::UInt outline=0,g::UInt table=0,
                 std::array<float,4> params={},float alpha=1,bool overscan=false) {
        if(k2Ati()){atiDraw(op,aux,outline,table,params,alpha,overscan);return;}
        desktopShaderRan=true;
        if(desktopState) {
            if(op==1) {
                const auto matrix=desktopSceneSaturationMatrix(settings);
                for(unsigned n=0;n<3;++n)desktopState->fragmentConstants[n]={{matrix[n][0],matrix[n][1],matrix[n][2],0}};
                // Saturation command22 writes its three identity rows into
                // shared env3..5 before the caller writes env0..2.
                desktopState->fragmentConstants[3]={{1,0,0,0}};
                desktopState->fragmentConstants[4]={{0,1,0,0}};
                desktopState->fragmentConstants[5]={{0,0,1,0}};
            }else if(op==4) {
                const auto r=settings.accumulationRatio;
                desktopState->fragmentConstants[0]={{r,r,r,1}};desktopState->fragmentConstants[1]={{1-r,1-r,1-r,0}};
            }else if(op==5||op==9) {
                desktopState->fragmentConstants[0]={{1,0,0,0}};desktopState->fragmentConstants[1]={{0,1,0,0}};
                desktopState->fragmentConstants[2]={{0,0,1,0}};
            }else if(op==7) {
                // Dream/Fury command0x1024, K2Desktop0x9af21..0x9af38.
                desktopState->fragmentConstants[7]={{0,0,0,.5f}};
            }
        }
        auto& a=*api;a.UseProgram(program);a.Uniform1i(mode,op);const auto projection=ortho(float(frame->viewport[2]),float(frame->viewport[3]));
        a.UniformMatrix4fv(mvp,1,0,projection.values.data());const float dimensions[]={float(frame->viewport[2])/(k1Desktop()?effectDimensions.captureWidth:1),
            float(frame->viewport[3])/(k1Desktop()?effectDimensions.captureHeight:1),0};
        a.Uniform3fv(size,1,dimensions);a.Uniform4fv(parameters,1,params.data());
        if(k1Desktop()){
            a.Uniform1i(fogMode,desktopFogEnabled?static_cast<int>(sceneFog.mode):0);
            const float data[]={sceneFog.start,sceneFog.end,sceneFog.density};
            const float color[]={sceneFog.color.x,sceneFog.color.y,sceneFog.color.z};
            a.Uniform3fv(fogData,1,data);a.Uniform3fv(fogColor,1,color);
        }
        const auto matrix=desktopSceneSaturationMatrix(settings);a.Uniform3fv(rows,3,matrix[0].data());
        a.ActiveTexture(g::Texture0);a.BindTexture(captureTarget(),capture);
        for(unsigned unit=1;unit<4;++unit){a.ActiveTexture(g::Texture0+unit);a.BindTexture(g::Texture2D,unit==1?aux:unit==2?outline:table);}
        quad(alpha,overscan);a.DrawElements(g::Triangles,6,g::UnsignedInt,nullptr);error("Desktop effect draw");
    }
    void desktopHistory() {
        captureHost();bindTarget(history[1-historyFront]);flatState();desktopDraw(0);
        pendingHistory=true;bindHost();
    }
    void desktopBloom() {
        captureHost();bindTarget(desktopMips[3]);clearColorTarget();
        if(k2Ati())flatState(true,g::SrcAlpha,g::One);else flatState();
        desktopDraw(5,0,0,lut);
        for(int n=2;n>=0;--n){bindTarget(desktopMips[n]);clearColorTarget();flatState(true,g::SrcAlpha,g::One);desktopDraw(8,desktopMips[n+1].texture);}
        bindTarget(desktopFiltered);clearColorTarget();flatState(true,g::SrcAlpha,g::One);
        auto filter=engineSoftShadowFilter(1); // same stored weights; different shader alpha and source texel size
        for(unsigned pass=0;pass<7;++pass){
            std::array<std::array<float,3>,4> values{};
            for(unsigned t=0;t<4;++t)values[t]={{filter[pass][t].u*8,filter[pass][t].v*8,filter[pass][t].weight}}; // /64, not /512
            if(desktopState&&!k2Ati())for(unsigned n=0;n<4;++n)desktopState->fragmentConstants[n]={{values[n][2],values[n][2],values[n][2],values[n][2]}};
            // Shared filter preserves engine coordinates for all28 taps,
            // including the last three and the unit1..3 axis transposition.
            if(k2Ati())atiTaps=values;
            else {api->UseProgram(program);api->Uniform3fv(taps,4,values[0].data());}
            desktopDraw(6,desktopMips[0].texture,0,0,{},.35f);
        }
        if(k2Ati()){atiBind(0,0,0);for(unsigned n=1;n<4;++n)atiBind(n,0,0,false);}
        bindHost();flatState(true,g::SrcAlpha,g::One);desktopDraw(8,desktopFiltered.texture);
        if(k2Ati())atiCleanup();
    }
    void desktopDream(bool fury) {
        desktopHistory();flatState(true,g::SrcAlpha,g::OneMinusSrcAlpha);
        const auto force=texture(SceneEffectTexture::ForceDistortion,1,true);
        const auto outline=texture(fury?SceneEffectTexture::Fury:SceneEffectTexture::Dream,2,true);
        desktopDraw(7,force,outline,history[1-historyFront].texture,{},.85f,true);
    }
    void cleanupDesktop() {
        if(desktopState&&desktopShaderRan) {
            // K1 ATI-text/K2 ARB effect callers retain command2's target
            // disables and1D unbinds. Host-state restoration is separate.
            desktopState->finishFramebufferFragmentCommand();desktopShaderRan=false;
        }
        auto& a=*api;for(unsigned n=0;n<4;++n){a.ActiveTexture(g::Texture0+n);if(rectangle){a.BindTexture(Rect,0);a.Disable(Rect);}a.BindTexture(g::Texture2D,0);}
        a.ActiveTexture(g::Texture0);a.UseProgram(0);if(!k2Ati())a.Enable(g::Blend);a.BlendFuncSeparate(g::SrcAlpha,g::OneMinusSrcAlpha,g::SrcAlpha,g::OneMinusSrcAlpha);
        a.Enable(g::DepthTest);a.Enable(g::Lighting);a.DepthMask(1);
        if(normalizedCapture()){a.Enable(g::AlphaTest);if(desktopFogEnabled)a.Enable(g::Fog);}
        bindHost();
    }
};
ScenePostprocess::ScenePostprocess():impl_(std::make_unique<Impl>()){}
ScenePostprocess::~ScenePostprocess()=default;
void ScenePostprocess::setResolver(g::Loader resolver){impl_->resolver=std::move(resolver);}
bool ScenePostprocess::begin(g::Api& api,const SceneEffectsSettings& settings,
    std::size_t count,const SceneEffectTextureBinding& bind,EngineMaterialState& desktopState,std::string& error,model::GameVersion game,const model::FogSettings& fog) {
    error.clear();auto& i=*impl_;
    if(i.frame){error="Nested scene effects are not supported";return false;}
    i.runtimeGame=game;i.effectDimensions={};
    if(!validateSceneEffects(settings,error,game))return false;
    if(!settings.enabled){
        // K1's outer wrapper returns before the ATI caller when disabled;
        // its first-frame and previous-speed bytes retain their old values.
        if(game!=model::GameVersion::Kotor1)i.validHistory=i.previousSpeed=false;
        return true;
    }
    if(!bind&&!requiredSceneEffectTextures(settings,count!=0,game).empty()){error="Missing scene-effect texture callback";return false;}
    i.api=&api;i.sceneFog=fog;i.settings=settings;i.producers=count;i.bindTexture=bind;i.desktopState=&desktopState;i.desktopShaderRan=false;
    i.pendingHistory=false;i.pendingSpeed=settings.speedBlur&&!(i.k2Ati()&&settings.forceSight);
    try {
        i.load();i.save();if(i.k2Ati())i.saveAtiMatrices();api.Disable(Scissor);if(i.srgb)api.Disable(Srgb);
        i.ensure();if(i.epoch!=settings.historyEpoch){i.validHistory=i.previousSpeed=false;i.epoch=settings.historyEpoch;}
        i.bindHost();
        i.error("begin");return true;
    }catch(const std::exception& e){error=e.what();cancel();return false;}
}
bool ScenePostprocess::beginDistortion(std::string& error) {
    error.clear();auto& i=*impl_;if(!i.frame)return true;
    try {
        if(i.distorting)throw std::runtime_error("Nested distortion bucket");
        i.distorting=true;
        // DoDistortionBuckets does NOT clear stencil, even with an empty
        // list. Preserve upstream stencil contents, including shadow bits.
        auto& a=*i.api;a.Enable(g::StencilTest);a.ColorMask(0,0,0,0);
        a.StencilMaskSeparate(g::FrontAndBack,1);a.StencilFuncSeparate(g::FrontAndBack,g::Always,1,0xff);
        a.StencilOpSeparate(g::FrontAndBack,g::Keep,g::Keep,g::Replace);
        i.error("distortion entry");return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
void ScenePostprocess::endDistortion() noexcept {
    auto& i=*impl_;if(!i.frame||!i.distorting)return;
    i.api->ColorMask(1,1,1,1);i.api->StencilMaskSeparate(g::FrontAndBack,0xff);i.api->Disable(g::StencilTest);
    i.distorting=false;
}
bool ScenePostprocess::desktopEffects1(std::string& error) {
    error.clear();auto& i=*impl_;if(!i.frame)return true;
    try {
        // K1 FrameBufferModifications 0x311444..0x31144c disables fog
        // before all gates and before dispatching to the active ATI branch.
        if(i.normalizedCapture())i.api->Disable(g::Fog);
        i.desktopFogEnabled=false;
        if(i.k2Ati()){i.api->Disable(g::AlphaTest);i.api->Disable(g::Blend);}
        if(i.settings.speedBlur&&!(i.k2Ati()&&i.settings.forceSight)){
            if(i.validHistory&&i.previousSpeed){i.captureHost();i.flatState();i.desktopDraw(4,i.history[i.historyFront].texture,0,0,{{i.settings.accumulationRatio,0,0,0}});}
            i.desktopHistory();
        }
        if(i.settings.desktopOverbrighten){i.captureHost();i.flatState();i.desktopDraw(9,0,0,i.overbrightLut);}
        if(i.settings.desktopConvolution)i.desktopBloom();
        if(i.producers){i.captureHost();i.flatState(true,g::One,g::Zero);
            const auto force=i.texture(SceneEffectTexture::ForceDistortion,1,true);auto& a=*i.api;
            a.Enable(g::StencilTest);a.StencilFuncSeparate(g::FrontAndBack,0x0201 /* GL_LESS */,0,1);a.StencilOpSeparate(g::FrontAndBack,g::Keep,g::Keep,g::Keep);
            i.desktopDraw(3,force);a.Disable(g::StencilTest);}
        for(bool scan:{true,false})if(scan?i.settings.scanNoise:i.settings.filmNoise){
            i.captureHost();i.flatState();const auto noise=i.texture(SceneEffectTexture::FilmNoise,1,false,true);
            i.desktopDraw(2,noise,0,0,{{scan?0.f:7.5f,0,0,0}});}
        if(!i.k1Desktop()&&i.settings.dream)i.desktopDream(false);
        if(!i.k1Desktop()&&i.settings.furyLevel)i.desktopDream(true);
        i.cleanupDesktop();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool ScenePostprocess::desktopEffects2(std::string& error) {
    error.clear();auto& i=*impl_;if(!i.frame||!i.settings.saturationEnabled)return true;
    try {
        i.desktopFogEnabled=i.normalizedCapture()&&i.api->IsEnabled(g::Fog);
        i.captureHost();i.flatState(true,g::One,g::Zero);
        i.desktopDraw(1);i.cleanupDesktop();return true;
    }
    catch(const std::exception& e){error=e.what();return false;}
}
bool ScenePostprocess::finish(std::string& error) {
    error.clear();auto& i=*impl_;if(!i.frame)return true;
    try {
        if(i.distorting)throw std::runtime_error("Unclosed distortion bucket");
        i.error("finish");
        if(i.pendingHistory){i.historyFront=1-i.historyFront;i.validHistory=true;}
        i.previousSpeed=i.pendingSpeed;i.restore();return true;
    }catch(const std::exception& e){error=e.what();cancel();return false;}
}
void ScenePostprocess::cancel() noexcept {
    auto& i=*impl_;endDistortion();i.pendingHistory=false;
    i.restore();
}
void ScenePostprocess::invalidateHistory() noexcept {impl_->validHistory=impl_->previousSpeed=false;}
void ScenePostprocess::shutdown() noexcept {
    auto& i=*impl_;cancel();i.releaseTargets();
    if(i.api){for(auto* id:{&i.vbo,&i.ibo}){if(*id)i.api->DeleteBuffers(1,id);*id=0;}if(i.program)i.api->DeleteProgram(i.program);}
    i.ati.shutdown();i.program=0;i.api=nullptr;i.loaded=false;i.bindTexture={};i.desktopState=nullptr;
    i.runtimeGame=model::GameVersion::Kotor2;i.effectDimensions={};
}
bool ScenePostprocess::active()const noexcept{return bool(impl_->frame);}
} // namespace neomdl::render
