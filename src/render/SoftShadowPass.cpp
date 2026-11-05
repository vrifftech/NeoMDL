#include "render/SoftShadowPass.hpp"
#include "core/SceneEffects.hpp"
#include "render/MaterialPass.hpp"
#include "model/Lighting.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neomdl::render {
unsigned engineAtiShadowCaptureSize(unsigned value) noexcept {
    return engineDesktopAtiCaptureSize(value);
}
SoftShadowFilter engineSoftShadowFilter(float darkening) {
    if (!std::isfinite(darkening)) throw std::invalid_argument("Non-finite soft-shadow darkening factor");
    // Desktop 0x543338; softshadowdarkeningfactor 0x56d20c, texel center 0x56d208.
    constexpr float kernel[28]={
        .12f,.25f,.29f,.25f,.12f,
        .25f,.49f,.58f,.49f,.25f,
        .29f,.58f,.67f,.58f,.29f,
        .25f,.49f,.58f,.49f,.25f,
        .12f,.25f,.29f,.25f,.12f,0,0,0};
    SoftShadowFilter result{};
    for (unsigned pass=0;pass<7;++pass) for (unsigned tap=0;tap<4;++tap) {
        const unsigned index=pass*4u+tap;
        const float column=static_cast<float>(index%5u), row=static_cast<float>(index/5u);
        result[pass][tap]={( (tap==0 ? column : row)-2.0f)/512.0f,
                           ( (tap==0 ? row : column)-2.0f)/512.0f,
                           kernel[index]*darkening};
    }
    return result;
}
namespace {
namespace g=gl;
constexpr g::Enum Rect=0x84F5, RectBinding=0x84F6, Rgba=0x1908, Rgba8=0x8058,
    UnsignedByte=0x1401, MinFilter=0x2801, MagFilter=0x2800, WrapS=0x2802,
    WrapT=0x2803, Linear=0x2601, Clamp=0x2900, Framebuffer=0x8D40,
    DrawFramebuffer=0x8CA9, ReadFramebuffer=0x8CA8, ReadFramebufferBinding=0x8CAA,
    FramebufferBinding=0x8CA6, ColorAttachment0=0x8CE0, FramebufferComplete=0x8CD5,
    Viewport=0x0BA2, ColorClearValue=0x0C22, ColorBufferBit=0x4000,
    ScissorTest=0x0C11, DrawBuffer=0x0C01, ReadBuffer=0x0C02,
    UnpackBuffer=0x88EC, UnpackBufferBinding=0x88EF, MaxTextureSize=0x0D33,
    MaxRectangleTextureSize=0x84F8, Extensions=0x1F03, SampleBuffers=0x80A8,
    FramebufferSrgb=0x8DB9;

bool extension(const char* text,const char* name) {
    if (!text) return false;
    const std::string list(text),token(name);
    std::size_t position=0;
    while ((position=list.find(token,position))!=std::string::npos) {
        const auto end=position+token.size();
        if ((position==0 || list[position-1]==' ') && (end==list.size() || list[end]==' ')) return true;
        position=end;
    }
    return false;
}
const char* vertexSource=R"GLSL(#version 120
attribute vec2 aPosition;
attribute vec2 aUv;
varying vec2 vUv;
void main() { vUv=aUv; gl_Position=vec4(aPosition,0.0,1.0); }
)GLSL";
// Arithmetic translation of the embedded Desktop ARBfp1.0 program at 0x528e17.
// Per-pass SRC_ALPHA/ONE blending, rather than this shader, accumulates results.
const char* fragmentSource=R"GLSL(#version 120
#extension GL_ARB_texture_rectangle : require
uniform sampler2DRect uRectangle;
uniform sampler2D uImage;
uniform vec3 uCaptureSize;
uniform vec3 uTap[4];
uniform int uMode;
varying vec2 vUv;
void main() {
    if (uMode==0) { gl_FragColor=texture2DRect(uRectangle,vUv*uCaptureSize.xy); return; }
    if (uMode==1) { gl_FragColor=texture2D(uImage,vUv); return; }
    vec4 r=texture2D(uImage,vUv+uTap[0].xy)*uTap[0].z;
    r=texture2D(uImage,vUv+uTap[1].xy)*uTap[1].z+r;
    r=texture2D(uImage,vUv+uTap[2].xy)*uTap[2].z+r;
    r=texture2D(uImage,vUv+uTap[3].xy)*uTap[3].z+r;
    gl_FragColor=r;
}
)GLSL";
std::string log(g::Api& a,g::UInt object,bool program) {
    g::Int size=0;
    if (program) a.GetProgramiv(object,g::InfoLogLength,&size); else a.GetShaderiv(object,g::InfoLogLength,&size);
    std::vector<char> text(static_cast<std::size_t>(std::max(size,1)),0);
    g::Size count=0;
    if (program) a.GetProgramInfoLog(object,static_cast<g::Size>(text.size()),&count,text.data());
    else a.GetShaderInfoLog(object,static_cast<g::Size>(text.size()),&count,text.data());
    return std::string(text.data(),static_cast<std::size_t>(std::clamp(count,0,static_cast<int>(text.size()))));
}
g::UInt shader(g::Api& a,g::Enum type,const char* source) {
    const auto id=a.CreateShader(type);
    if (!id) throw std::runtime_error("Cannot allocate engine soft-shadow shader");
    a.ShaderSource(id,1,&source,nullptr);a.CompileShader(id);
    g::Int ok=0;a.GetShaderiv(id,g::CompileStatus,&ok);
    if (!ok) {const auto message=log(a,id,false);a.DeleteShader(id);throw std::runtime_error(message);}
    return id;
}
}
struct SoftShadowPass::Impl {
    g::Api* api{};
    EngineMaterialState* materialState{};
    g::Loader resolver;
    using GenFn=void (NEOMDL_GL_CALL*)(g::Size,g::UInt*);
    using DeleteFn=void (NEOMDL_GL_CALL*)(g::Size,const g::UInt*);
    using BindFn=void (NEOMDL_GL_CALL*)(g::Enum,g::UInt);
    using AttachFn=void (NEOMDL_GL_CALL*)(g::Enum,g::Enum,g::Enum,g::UInt,g::Int);
    using CheckFn=g::Enum (NEOMDL_GL_CALL*)(g::Enum);
    GenFn gen{};DeleteFn remove{};BindFn bind{};AttachFn attach{};CheckFn check{};
    bool separateTargets{},srgbSupported{},arbVertexSupported{},arbFragmentSupported{},rectangleSupported{};
    EngineSoftShadowMode branch{EngineSoftShadowMode::DesktopARB};
    g::UInt program{},vbo{},ebo{},savedScene{},mask{},resampled{},filtered{},fbos[2]{};
    g::UInt atiScene{},atiMask{},atiSource{},atiTarget{};
    g::UInt atiFrontColor{},atiFrontFbo{};
    int atiWidth{},atiHeight{};
    int atiFrontWidth{},atiFrontHeight{};
    std::array<model::Mat4,4> atiMatrices;
    model::FogSettings atiFog;
    // The Desktop non-ARB branch is fixed-function. Do not replace its
    // missing ATI fragment program or incomplete pbuffer textures with ARB's
    // convolution. These entry points are used only for explicit DesktopATI.
    struct Legacy {
        using VoidFn=void (NEOMDL_GL_CALL*)();
        using EnumFn=void (NEOMDL_GL_CALL*)(g::Enum);
        using PairFn=void (NEOMDL_GL_CALL*)(float,float);
        using TripleFn=void (NEOMDL_GL_CALL*)(float,float,float);
        using ColorFn=void (NEOMDL_GL_CALL*)(float,float,float,float);
        using MultiFn=void (NEOMDL_GL_CALL*)(g::Enum,float,float);
        using OrthoFn=void (NEOMDL_GL_CALL*)(double,double,double,double,double,double);
        using EnvFn=void (NEOMDL_GL_CALL*)(g::Enum,g::Enum,g::Int);
        using CopyImageFn=void (NEOMDL_GL_CALL*)(g::Enum,g::Int,g::Enum,g::Int,g::Int,g::Size,g::Size,g::Int);
        using MatrixFn=void (NEOMDL_GL_CALL*)(const float*);
        using FogFloatFn=void (NEOMDL_GL_CALL*)(g::Enum,float);
        using FogIntFn=void (NEOMDL_GL_CALL*)(g::Enum,g::Int);
        using FogVectorFn=void (NEOMDL_GL_CALL*)(g::Enum,const float*);
        EnumFn begin{},matrixMode{},pushAttrib{};
        VoidFn end{},pushMatrix{},popMatrix{},loadIdentity{},popAttrib{},flush{};
        PairFn texCoord{};TripleFn vertex{};ColorFn color{};MultiFn multi{};
        OrthoFn ortho{};EnvFn texEnv{};CopyImageFn copyImage{};
        MatrixFn loadMatrix{};FogFloatFn fogFloat{};FogIntFn fogInt{};FogVectorFn fogVector{};
    } legacy;
    g::Int mode{},captureSize{},taps{};
    int width{},height{};
    SoftShadowFilter filter{};
    struct Frame {
        g::Int viewport[4]{},drawFbo{},readFbo{},drawBuffer{},readBuffer{},drawFboReadBuffer{},rectangle{},unpack{};
        float clear[4]{};
        g::Boolean scissor{},srgb{};
        g::Int matrixMode{};
        bool saved{},legacyState{};
    };
    std::unique_ptr<Frame> frame;

    void load() {
        auto& a=*api;
        int major=0,minor=0;
        const auto* version=reinterpret_cast<const char*>(a.GetString(g::Version));
        if (version) std::sscanf(version,"%d.%d",&major,&minor);
        const auto* extensions=reinterpret_cast<const char*>(a.GetString(Extensions));
        separateTargets=major>=3 || extension(extensions,"GL_ARB_framebuffer_object");
        const bool extFbo=extension(extensions,"GL_EXT_framebuffer_object");
        if (!separateTargets && !extFbo)
            throw std::runtime_error("Desktop ARB soft shadows require framebuffer-object support");
        rectangleSupported=extension(extensions,"GL_ARB_texture_rectangle");
        arbVertexSupported=extension(extensions,"GL_ARB_vertex_program");
        arbFragmentSupported=extension(extensions,"GL_ARB_fragment_program");
        if (branch==EngineSoftShadowMode::DesktopARB && !rectangleSupported)
            throw std::runtime_error("Desktop ARB soft shadows require GL_ARB_texture_rectangle");
        srgbSupported=major>=3 || extension(extensions,"GL_ARB_framebuffer_sRGB") ||
                      extension(extensions,"GL_EXT_framebuffer_sRGB");
        if (!resolver) throw std::runtime_error("Missing soft-shadow OpenGL resolver");
        const auto proc=[&](const char* core,const char* ext) {return resolver(separateTargets ? core : ext);};
        gen=reinterpret_cast<GenFn>(proc("glGenFramebuffers","glGenFramebuffersEXT"));
        remove=reinterpret_cast<DeleteFn>(proc("glDeleteFramebuffers","glDeleteFramebuffersEXT"));
        bind=reinterpret_cast<BindFn>(proc("glBindFramebuffer","glBindFramebufferEXT"));
        attach=reinterpret_cast<AttachFn>(proc("glFramebufferTexture2D","glFramebufferTexture2DEXT"));
        check=reinterpret_cast<CheckFn>(proc("glCheckFramebufferStatus","glCheckFramebufferStatusEXT"));
        if (!gen || !remove || !bind || !attach || !check)
            throw std::runtime_error("Engine soft-shadow framebuffer entry points are unavailable");
        if(branch==EngineSoftShadowMode::DesktopATI) {
#define NEOMDL_LOAD_LEGACY(member,type,name) \
            legacy.member=reinterpret_cast<Legacy::type>(resolver(name)); \
            if(!legacy.member) throw std::runtime_error("Desktop ATI soft shadows require " name)
            NEOMDL_LOAD_LEGACY(begin,EnumFn,"glBegin");
            NEOMDL_LOAD_LEGACY(end,VoidFn,"glEnd");
            NEOMDL_LOAD_LEGACY(matrixMode,EnumFn,"glMatrixMode");
            NEOMDL_LOAD_LEGACY(pushMatrix,VoidFn,"glPushMatrix");
            NEOMDL_LOAD_LEGACY(popMatrix,VoidFn,"glPopMatrix");
            NEOMDL_LOAD_LEGACY(loadIdentity,VoidFn,"glLoadIdentity");
            NEOMDL_LOAD_LEGACY(pushAttrib,EnumFn,"glPushAttrib");
            NEOMDL_LOAD_LEGACY(popAttrib,VoidFn,"glPopAttrib");
            NEOMDL_LOAD_LEGACY(texCoord,PairFn,"glTexCoord2f");
            NEOMDL_LOAD_LEGACY(vertex,TripleFn,"glVertex3f");
            NEOMDL_LOAD_LEGACY(color,ColorFn,"glColor4f");
            NEOMDL_LOAD_LEGACY(multi,MultiFn,"glMultiTexCoord2f");
            NEOMDL_LOAD_LEGACY(ortho,OrthoFn,"glOrtho");
            NEOMDL_LOAD_LEGACY(texEnv,EnvFn,"glTexEnvi");
            NEOMDL_LOAD_LEGACY(flush,VoidFn,"glFlush");
            NEOMDL_LOAD_LEGACY(copyImage,CopyImageFn,"glCopyTexImage2D");
            NEOMDL_LOAD_LEGACY(loadMatrix,MatrixFn,"glLoadMatrixf");
            NEOMDL_LOAD_LEGACY(fogFloat,FogFloatFn,"glFogf");
            NEOMDL_LOAD_LEGACY(fogInt,FogIntFn,"glFogi");
            NEOMDL_LOAD_LEGACY(fogVector,FogVectorFn,"glFogfv");
#undef NEOMDL_LOAD_LEGACY
        }
    }
    void captureState() {
        auto& a=*api;frame=std::make_unique<Frame>();auto& f=*frame;
        a.GetIntegerv(Viewport,f.viewport);a.GetFloatv(ColorClearValue,f.clear);
        a.GetIntegerv(FramebufferBinding,&f.drawFbo);
        if (separateTargets) a.GetIntegerv(ReadFramebufferBinding,&f.readFbo);else f.readFbo=f.drawFbo;
        a.GetIntegerv(DrawBuffer,&f.drawBuffer);a.GetIntegerv(ReadBuffer,&f.readBuffer);
        f.drawFboReadBuffer=f.readBuffer;
        if(separateTargets && f.drawFbo!=f.readFbo) {
            bind(ReadFramebuffer,static_cast<g::UInt>(f.drawFbo));
            a.GetIntegerv(ReadBuffer,&f.drawFboReadBuffer);
            bind(ReadFramebuffer,static_cast<g::UInt>(f.readFbo));
        }
        a.GetIntegerv(UnpackBufferBinding,&f.unpack);
        f.scissor=a.IsEnabled(ScissorTest);if (srgbSupported) f.srgb=a.IsEnabled(FramebufferSrgb);
        a.ActiveTexture(g::Texture0);
        if(branch==EngineSoftShadowMode::DesktopARB) a.GetIntegerv(RectBinding,&f.rectangle);
        else {
            a.GetIntegerv(0x0BA0,&f.matrixMode);
            legacy.pushAttrib(0x000fffff); // host compatibility-state ownership
            legacy.matrixMode(0x1701);legacy.pushMatrix();
            legacy.matrixMode(0x1700);legacy.pushMatrix();
            for(unsigned unit=0;unit<4;++unit) {
                a.ActiveTexture(g::Texture0+unit);legacy.matrixMode(0x1702);legacy.pushMatrix();
            }
            a.ActiveTexture(g::Texture0);legacy.matrixMode(0x1700);
            f.legacyState=true;
        }
    }
    void restoreState() noexcept {
        if (!frame || !api) return;
        auto& a=*api;const auto& f=*frame;
        bind(Framebuffer,static_cast<g::UInt>(f.drawFbo));
        a.DrawBuffer(static_cast<g::Enum>(f.drawBuffer));a.ReadBuffer(static_cast<g::Enum>(f.drawFboReadBuffer));
        if(separateTargets && f.drawFbo!=f.readFbo) {
            bind(ReadFramebuffer,static_cast<g::UInt>(f.readFbo));
            a.ReadBuffer(static_cast<g::Enum>(f.readBuffer));
        }
        if(f.legacyState) {
            for(unsigned unit=0;unit<4;++unit) {
                a.ActiveTexture(g::Texture0+unit);legacy.matrixMode(0x1702);legacy.popMatrix();
            }
            legacy.matrixMode(0x1701);legacy.popMatrix();
            legacy.matrixMode(0x1700);legacy.popMatrix();
            legacy.matrixMode(static_cast<g::Enum>(f.matrixMode));legacy.popAttrib();
        }
        a.Viewport(f.viewport[0],f.viewport[1],f.viewport[2],f.viewport[3]);
        a.ClearColor(f.clear[0],f.clear[1],f.clear[2],f.clear[3]);
        if (f.scissor) a.Enable(ScissorTest);else a.Disable(ScissorTest);
        if (srgbSupported) {if(f.srgb) a.Enable(FramebufferSrgb);else a.Disable(FramebufferSrgb);}
        a.ActiveTexture(g::Texture0);
        if(branch==EngineSoftShadowMode::DesktopARB) a.BindTexture(Rect,static_cast<g::UInt>(f.rectangle));
        a.BindBuffer(UnpackBuffer,static_cast<g::UInt>(f.unpack));
        frame.reset();
    }
    void checkError(const char* stage) {
        const auto value=api->GetError();
        if (value!=g::NoError) throw std::runtime_error(std::string("Engine soft shadows: ")+stage+
                                                      " failed (OpenGL "+std::to_string(value)+")");
    }
    void texture(g::UInt& id,g::Enum target,int w,int h) {
        auto& a=*api;
        if (!id) a.GenTextures(1,&id);
        if (!id) throw std::runtime_error("Cannot allocate engine soft-shadow texture");
        a.BindTexture(target,id);
        a.TexParameteri(target,MinFilter,Linear);a.TexParameteri(target,MagFilter,Linear);
        a.TexParameteri(target,WrapS,Clamp);a.TexParameteri(target,WrapT,Clamp);
        a.TexImage2D(target,0,Rgba8,w,h,0,Rgba,UnsignedByte,nullptr);
    }
    void ensureResources() {
        auto& a=*api;const int w=frame->viewport[2],h=frame->viewport[3];
        g::Int limit=0,rectLimit=0,samples=0;
        a.GetIntegerv(MaxTextureSize,&limit);a.GetIntegerv(MaxRectangleTextureSize,&rectLimit);
        a.GetIntegerv(SampleBuffers,&samples);
        if (w<=0 || h<=0 || w>rectLimit || h>rectLimit || limit<512)
            throw std::runtime_error("Viewport exceeds engine soft-shadow texture limits");
        if (samples!=0) throw std::runtime_error("Engine soft-shadow capture requires a single-sample target");
        a.BindBuffer(UnpackBuffer,0);a.ActiveTexture(g::Texture0);
        if (!program) {
            g::UInt vertex=0,fragment=0,candidate=0;
            try {
                vertex=shader(a,g::VertexShader,vertexSource);fragment=shader(a,g::FragmentShader,fragmentSource);
                candidate=a.CreateProgram();if (!candidate) throw std::runtime_error("Cannot allocate soft-shadow program");
                a.AttachShader(candidate,vertex);a.AttachShader(candidate,fragment);
                a.BindAttribLocation(candidate,0,"aPosition");a.BindAttribLocation(candidate,1,"aUv");
                a.LinkProgram(candidate);g::Int ok=0;a.GetProgramiv(candidate,g::LinkStatus,&ok);
                if (!ok) throw std::runtime_error(log(a,candidate,true));
                a.DeleteShader(vertex);vertex=0;a.DeleteShader(fragment);fragment=0;
                program=candidate;candidate=0;
            } catch (...) {
                if(vertex) a.DeleteShader(vertex);
                if(fragment) a.DeleteShader(fragment);
                if(candidate) a.DeleteProgram(candidate);
                throw;
            }
            mode=a.GetUniformLocation(program,"uMode");captureSize=a.GetUniformLocation(program,"uCaptureSize");
            taps=a.GetUniformLocation(program,"uTap[0]");
            a.UseProgram(program);
            a.Uniform1i(a.GetUniformLocation(program,"uRectangle"),0);
            a.Uniform1i(a.GetUniformLocation(program,"uImage"),1);
        }
        if (!vbo || !ebo) {
            constexpr float vertices[]={-1,1,0,1, -1,-1,0,0, 1,-1,1,0, 1,1,1,1};
            constexpr g::UInt indices[]={0,1,2,0,2,3};
            if (!vbo) a.GenBuffers(1,&vbo);
            if (!ebo) a.GenBuffers(1,&ebo);
            if (!vbo || !ebo) throw std::runtime_error("Cannot allocate engine soft-shadow screen geometry");
            try {
                a.BindBuffer(g::ArrayBuffer,vbo);a.BufferData(g::ArrayBuffer,sizeof(vertices),vertices,g::StaticDraw);
                a.BindBuffer(g::ElementArrayBuffer,ebo);a.BufferData(g::ElementArrayBuffer,sizeof(indices),indices,g::StaticDraw);
                checkError("screen geometry allocation");
            } catch (...) {
                if (vbo) a.DeleteBuffers(1,&vbo);
                if (ebo) a.DeleteBuffers(1,&ebo);
                vbo=ebo=0;
                throw;
            }
        }
        if (!fbos[0] || !fbos[1]) {
            try {
                texture(resampled,g::Texture2D,512,512);texture(filtered,g::Texture2D,512,512);
                gen(2,fbos);
                if (!fbos[0] || !fbos[1]) throw std::runtime_error("Cannot allocate engine soft-shadow framebuffers");
                for (unsigned n=0;n<2;++n) {
                    bind(Framebuffer,fbos[n]);attach(Framebuffer,ColorAttachment0,g::Texture2D,n==0?resampled:filtered,0);
                    a.DrawBuffer(ColorAttachment0);a.ReadBuffer(ColorAttachment0);
                    if (check(Framebuffer)!=FramebufferComplete) throw std::runtime_error("Incomplete engine soft-shadow framebuffer");
                }
                checkError("intermediate framebuffer allocation");
            } catch (...) {
                remove(2,fbos);fbos[0]=fbos[1]=0;
                const g::UInt abandoned[]={resampled,filtered};a.DeleteTextures(2,abandoned);
                resampled=filtered=0;
                throw;
            }
        }
        if (width!=w || height!=h || !savedScene || !mask) {
            texture(savedScene,Rect,w,h);texture(mask,Rect,w,h);
            checkError("texture allocation");width=w;height=h;
        }
        checkError("resource initialization");
    }
    void mainTarget() {
        auto& a=*api;const auto& f=*frame;
        // A host FBO/viewport is an embedding boundary, not engine game state.
        // Read the same color attachment that receives the model's draw calls.
        bind(Framebuffer,static_cast<g::UInt>(f.drawFbo));
        a.DrawBuffer(static_cast<g::Enum>(f.drawBuffer));a.ReadBuffer(static_cast<g::Enum>(f.drawBuffer));
        a.Viewport(f.viewport[0],f.viewport[1],f.viewport[2],f.viewport[3]);
    }
    void atiTextureParameters() {
        auto& a=*api;
        a.TexParameteri(g::Texture2D,MinFilter,Linear);a.TexParameteri(g::Texture2D,MagFilter,Linear);
        a.TexParameteri(g::Texture2D,WrapS,Clamp);a.TexParameteri(g::Texture2D,WrapT,Clamp);
    }
    void atiBackTarget() {
        auto& a=*api;const auto& f=*frame;
        bind(Framebuffer,static_cast<g::UInt>(f.drawFbo));
        const auto buffer=f.drawFbo?static_cast<g::Enum>(f.drawBuffer):g::Back;
        a.DrawBuffer(buffer);a.ReadBuffer(buffer);
        a.Viewport(f.viewport[0],f.viewport[1],f.viewport[2],f.viewport[3]);
    }
    void atiFrontTarget() {
        auto& a=*api;const auto& f=*frame;
        if(f.drawFbo) {
            if(separateTargets) {
                bind(DrawFramebuffer,atiFrontFbo);bind(ReadFramebuffer,static_cast<g::UInt>(f.drawFbo));
                a.DrawBuffer(ColorAttachment0);a.ReadBuffer(static_cast<g::Enum>(f.drawBuffer));
            } else {
                // No reads occur while engine FRONT is selected. EXT-only
                // hosts restore the back read attachment with atiBackTarget.
                bind(Framebuffer,atiFrontFbo);a.DrawBuffer(ColorAttachment0);a.ReadBuffer(ColorAttachment0);
            }
            a.Viewport(0,0,512,512);
        } else {
            bind(Framebuffer,0);a.DrawBuffer(g::Front);a.ReadBuffer(g::Back);
            a.Viewport(f.viewport[0],f.viewport[1],512,512);
        }
    }
    void ensureAtiResources() {
        auto& a=*api;atiBackTarget();a.ActiveTexture(g::Texture0);
        width=frame->viewport[2];height=frame->viewport[3];
        const int w=static_cast<int>(engineAtiShadowCaptureSize(static_cast<unsigned>(width)));
        const int h=static_cast<int>(engineAtiShadowCaptureSize(static_cast<unsigned>(height)));
        if(atiWidth!=w || atiHeight!=h || !atiScene || !atiMask) {
            // Engine CopyTexImage2D requests the complete POT rectangle, even
            // outside the drawable. Do not zero-fill, crop or extrapolate it.
            for(auto* id:{&atiScene,&atiMask}) {
                if(!*id) a.GenTextures(1,id);
                a.BindTexture(g::Texture2D,*id);atiTextureParameters();
                legacy.copyImage(g::Texture2D,0,Rgba8,frame->viewport[0],frame->viewport[1],w,h,0);
            }
            checkError("ATI power-of-two capture allocation");atiWidth=w;atiHeight=h;
        }
        // The Desktop WGL adapter never creates storage or attaches these
        // objects in the non-ARB caller. Retain that incomplete-texture state.
        for(auto* id:{&atiSource,&atiTarget}) if(!*id) {
            a.GenTextures(1,id);a.BindTexture(g::Texture2D,*id);atiTextureParameters();
        }
        checkError("ATI pbuffer texture objects");
        if(frame->drawFbo && (atiFrontWidth!=width || atiFrontHeight!=height || !atiFrontFbo)) {
            // The supplied adapter reuses the window context, whose FRONT and
            // BACK buffers remain distinct. A host FBO has no FRONT attachment:
            // represent that window buffer separately, without attaching either
            // incomplete engine pbuffer texture or feeding it any image data.
            a.BindBuffer(UnpackBuffer,0);
            texture(atiFrontColor,g::Texture2D,width,height);
            if(!atiFrontFbo) gen(1,&atiFrontFbo);
            bind(Framebuffer,atiFrontFbo);
            attach(Framebuffer,ColorAttachment0,g::Texture2D,atiFrontColor,0);
            a.DrawBuffer(ColorAttachment0);a.ReadBuffer(ColorAttachment0);
            if(check(Framebuffer)!=FramebufferComplete)
                throw std::runtime_error("Incomplete ATI engine-front host framebuffer");
            checkError("ATI engine-front host framebuffer allocation");
            atiFrontWidth=width;atiFrontHeight=height;
        }
        atiBackTarget();
    }
    void atiCopy(g::UInt target) {
        auto& a=*api;atiBackTarget();a.ActiveTexture(g::Texture0);a.BindTexture(g::Texture2D,target);
        a.CopyTexSubImage2D(g::Texture2D,0,0,0,frame->viewport[0],frame->viewport[1],atiWidth,atiHeight);
        checkError("ATI scene/mask capture");
    }
    void atiProjection(float w,float h) {
        legacy.matrixMode(0x1701);legacy.loadIdentity();legacy.ortho(0,w,0,h,-1,1);
        legacy.matrixMode(0x1700);legacy.loadIdentity();
    }
    void atiQuad(g::UInt textureId,float u,float v,float w,float h) {
        auto& a=*api;a.ActiveTexture(g::Texture0);a.Enable(g::Texture2D);a.BindTexture(g::Texture2D,textureId);
        legacy.begin(7);
        for(unsigned n=0;n<4;++n) {
            const bool right=n>=2,top=n==0 || n==3;
            legacy.color(1,1,1,1);legacy.texCoord(right?u:0,top?v:0);
            legacy.vertex(right?w:0,top?h:0,0);
        }
        legacy.end();
    }
    void atiFixedState() {
        auto& a=*api;a.UseProgram(0);a.Disable(g::Lighting);a.Disable(g::DepthTest);
        // Engine non-ARB ATI selects fixed fragment processing. The GLSL host
        // may still have unrelated compatibility program enables saved by the
        // attribute guard; they are not engine inputs to this pass.
        if(arbVertexSupported) a.Disable(0x8620);
        if(arbFragmentSupported) a.Disable(0x8804);
        a.DepthMask(0);a.Disable(g::StencilTest);a.Disable(g::CullFace);a.Disable(g::AlphaTest);
        a.Disable(ScissorTest);a.ColorMask(1,1,1,1);a.PolygonMode(g::FrontAndBack,g::Fill);
        if(atiFog.mode==model::FogMode::Disabled) a.Disable(g::Fog);
        else {
            a.Enable(g::Fog);
            const g::Enum mode=atiFog.mode==model::FogMode::Linear?0x2601:
                atiFog.mode==model::FogMode::Exponential?0x0800:0x0801;
            legacy.fogInt(0x0b65,static_cast<g::Int>(mode));
            legacy.fogInt(0x8450,0x8452); // engine context default: fragment depth
            const float color[]={atiFog.color.x,atiFog.color.y,atiFog.color.z,1};
            legacy.fogVector(0x0b66,color);legacy.fogFloat(0x0b63,atiFog.start);
            legacy.fogFloat(0x0b64,atiFog.end);legacy.fogFloat(0x0b62,atiFog.density);
        }
        // Fixed-function scene producers leave MODULATE on unit0/1; unit2/3
        // retain the context default. Restore their engine inputs after GLSL
        // mesh rendering without borrowing unrelated host texture state.
        for(unsigned unit=0;unit<4;++unit) {
            a.ActiveTexture(g::Texture0+unit);a.Disable(g::Texture2D);a.Disable(g::TextureCube);
            a.Disable(0x0de0);a.Disable(0x806f);legacy.texEnv(0x2300,0x2200,0x2100);
            if(rectangleSupported) a.Disable(Rect);
            legacy.matrixMode(0x1702);legacy.loadMatrix(atiMatrices[unit].values.data());
        }
        a.ActiveTexture(g::Texture0);legacy.matrixMode(0x1700);
    }
    void atiRestoreScene() {
        atiBackTarget();atiFixedState();replaceBlend();atiProjection(static_cast<float>(width),static_cast<float>(height));
        // Host interruption recovery must replace the saved pixels directly,
        // independently of the interrupted engine filter's fog/matrix inputs.
        api->Disable(g::Fog);legacy.matrixMode(0x1702);legacy.loadIdentity();legacy.matrixMode(0x1700);
        atiQuad(atiScene,static_cast<float>(width)/atiWidth,static_cast<float>(height)/atiHeight,
                static_cast<float>(width),static_cast<float>(height));
    }
    void atiFinish() {
        auto& a=*api;
        atiCopy(atiMask);atiFixedState();
        // Scene::RenderStencilShadowsForThisLight 0xc37aa leaves the soft
        // mask's ONE/ZERO blend, which the GLSL mesh-state guard restores away.
        replaceBlend();
        a.Clear(ColorBufferBit);
        // wglMakeContextCurrentARB 0x110bf2 calls only SDL's context switch.
        // The ATI caller reuses its main HGLRC, but DrawBuffer(FRONT) at
        // 0x9cf39 still separates resampling from the BACK saved scene.
        atiFrontTarget();
        atiProjection(static_cast<float>(width),static_cast<float>(height));
        atiQuad(atiMask,static_cast<float>(width)/atiWidth,static_cast<float>(height)/atiHeight,
                static_cast<float>(width),static_cast<float>(height));
        atiBackTarget(); // DrawBuffer(BACK), 0x9d17b
        atiQuad(atiScene,static_cast<float>(width)/atiWidth,static_cast<float>(height)/atiHeight,
                static_cast<float>(width),static_cast<float>(height));
        // BloomFilterATI 0x9baee resets bindings, changes the same context's
        // viewport/projection and clears FRONT (0x9d36e), preserving BACK.
        // BindTexImageARB 0x110b3b only
        // unbinds the framebuffer in this branch; it does not provide pixels.
        a.Enable(g::Blend);a.BlendEquationSeparate(g::FuncAdd,g::FuncAdd);
        a.BlendFuncSeparate(g::SrcAlpha,g::One,g::SrcAlpha,g::One);
        atiFrontTarget();atiProjection(512,512);
        a.ClearColor(0,0,0,0);a.Clear(ColorBufferBit);
        for(unsigned unit=0;unit<4;++unit) {
            a.ActiveTexture(g::Texture0+unit);a.Enable(g::Texture2D);
            a.BindTexture(g::Texture2D,atiSource);atiTextureParameters();
        }
        a.ActiveTexture(g::Texture0);
        for(unsigned pass=0;pass<7;++pass) {
            legacy.begin(7);
            for(unsigned n=0;n<4;++n) {
                const bool right=n>=2,top=n==0 || n==3;
                legacy.color(1,1,1,.35f);
                for(unsigned unit=0;unit<4;++unit) {
                    const unsigned index=pass*4u+unit;
                    const float column=static_cast<float>(index%5u),row=static_cast<float>(index/5u);
                    legacy.multi(g::Texture0+unit,
                        (right?1.0f:0.0f)+((unit==0?column:row)-2.0f)/512.0f,
                        (top?1.0f:0.0f)+((unit==0?row:column)-2.0f)/512.0f);
                }
                legacy.vertex(right?512.0f:0.0f,top?512.0f:0.0f,0);
            }
            legacy.end();
        }
        for(unsigned unit=0;unit<4;++unit) {
            a.ActiveTexture(g::Texture0+unit);a.BindTexture(g::Texture2D,0);a.Disable(g::Texture2D);
        }
        legacy.flush();atiBackTarget();a.ActiveTexture(g::Texture0); // 0x9d4fb
        a.BlendFuncSeparate(g::SrcAlpha,g::OneMinusSrcAlpha,g::SrcAlpha,g::OneMinusSrcAlpha);
        atiProjection(static_cast<float>(width),static_cast<float>(height));
        atiQuad(atiTarget,1,1,static_cast<float>(width),static_cast<float>(height));
        a.BindTexture(g::Texture2D,0);a.Disable(g::Texture2D);
        checkError("ATI fixed-function soft-shadow composition");
    }
    void copy(g::UInt target) {
        auto& a=*api;mainTarget();a.ActiveTexture(g::Texture0);a.BindTexture(Rect,target);
        a.CopyTexSubImage2D(Rect,0,0,0,frame->viewport[0],frame->viewport[1],width,height);
        checkError("scene/mask capture");
    }
    void screen(g::UInt textureId,bool rectangle,int operation) {
        auto& a=*api;
        a.UseProgram(program);a.Uniform1i(mode,operation);
        const float dimensions[]={static_cast<float>(width),static_cast<float>(height),0};
        a.Uniform3fv(captureSize,1,dimensions);
        a.ActiveTexture(rectangle ? g::Texture0 : g::Texture1);
        a.BindTexture(rectangle ? Rect : g::Texture2D,textureId);
        a.BindBuffer(g::ArrayBuffer,vbo);a.BindBuffer(g::ElementArrayBuffer,ebo);
        a.VertexAttribPointer(0,2,g::Float,0,4*sizeof(float),nullptr);
        a.VertexAttribPointer(1,2,g::Float,0,4*sizeof(float),reinterpret_cast<const void*>(2*sizeof(float)));
        a.EnableVertexAttribArray(0);a.EnableVertexAttribArray(1);
        a.Disable(g::DepthTest);a.DepthMask(0);a.Disable(g::StencilTest);a.Disable(g::CullFace);
        a.Disable(g::AlphaTest);a.Disable(ScissorTest);a.ColorMask(1,1,1,1);
        a.PolygonMode(g::FrontAndBack,g::Fill);
        a.DrawElements(g::Triangles,6,g::UnsignedInt,nullptr);
    }
    void replaceBlend() {auto& a=*api;a.Enable(g::Blend);a.BlendEquationSeparate(g::FuncAdd,g::FuncAdd);a.BlendFuncSeparate(g::One,g::Zero,g::One,g::Zero);}
    void restoreScene() {
        if(branch==EngineSoftShadowMode::DesktopATI) atiRestoreScene();
        else {mainTarget();replaceBlend();screen(savedScene,true,0);}
    }
};
SoftShadowPass::SoftShadowPass():impl_(std::make_unique<Impl>()) {}
SoftShadowPass::~SoftShadowPass()=default;
void SoftShadowPass::setResolver(g::Loader resolver) {impl_->resolver=std::move(resolver);}
bool SoftShadowPass::begin(g::Api& api,float darkening,std::string& error) {
    impl_->materialState=nullptr;
    return beginPass(api,darkening,EngineSoftShadowMode::DesktopARB,error);
}
bool SoftShadowPass::begin(g::Api& api,float darkening,EngineSoftShadowMode branch,
        EngineMaterialState& materials,const model::FogSettings& fog,std::string& error) {
    impl_->materialState=&materials;
    for(unsigned unit=0;unit<4;++unit) impl_->atiMatrices[unit]=materials.textureMatrix(unit);
    impl_->atiFog=fog;
    return beginPass(api,darkening,branch,error);
}
bool SoftShadowPass::beginPass(g::Api& api,float darkening,EngineSoftShadowMode branch,std::string& error) {
    error.clear();auto& i=*impl_;
    if (i.frame) {error="Nested engine soft-shadow stage";return false;}
    try {
        i.api=&api;
        if(i.branch!=branch) i.width=i.height=0;
        i.branch=branch;i.load();i.captureState();
        if(branch==EngineSoftShadowMode::DesktopATI) {
            i.ensureAtiResources();i.atiCopy(i.atiScene);
            api.BindTexture(g::Texture2D,0);api.Disable(g::Texture2D);i.legacy.flush();
        }
        else {i.filter=engineSoftShadowFilter(darkening);i.ensureResources();i.copy(i.savedScene);}
        i.frame->saved=true;
        // SetUpSoftShadowsARB 0x9cc88: save full RGBA, clear color to transparent
        // black only. Keep the scene's depth/stencil for engine volume drawing.
        api.Disable(ScissorTest);if(i.srgbSupported) api.Disable(FramebufferSrgb);
        api.ColorMask(1,1,1,1);api.ClearColor(0,0,0,0);api.Clear(ColorBufferBit);
        i.checkError("mask setup");return true;
    } catch (const std::exception& e) {error=e.what();cancel();return false;}
}
bool SoftShadowPass::finish(std::string& error) {
    error.clear();auto& i=*impl_;
    if (!i.frame || !i.frame->saved) {error="Engine soft-shadow finish without setup";return false;}
    try {
        auto& a=*i.api;
        if(i.branch==EngineSoftShadowMode::DesktopATI) {
            i.atiFinish();i.frame->saved=false;i.restoreState();return true;
        }
        // FinishSoftShadowsARB 0x9d760: mask capture -> 512 RGBA8 resample ->
        // saved-scene restore -> seven engine filter draws -> alpha composite.
        i.copy(i.mask);i.bind(Framebuffer,i.fbos[0]);a.Viewport(0,0,512,512);
        i.replaceBlend();i.screen(i.mask,true,0);i.checkError("512 mask resample");
        i.restoreScene();i.checkError("saved-scene restore");
        i.bind(Framebuffer,i.fbos[1]);a.Viewport(0,0,512,512);
        a.ColorMask(1,1,1,1);a.ClearColor(0,0,0,0);a.Clear(ColorBufferBit);
        a.Enable(g::Blend);a.BlendEquationSeparate(g::FuncAdd,g::FuncAdd);
        a.BlendFuncSeparate(g::SrcAlpha,g::One,g::SrcAlpha,g::One);
        for (const auto& pass:i.filter) {
            // Avoid depending on aggregate padding at a GPU interface.
            float taps[12]{};
            for (unsigned t=0;t<4;++t) {taps[t*3]=pass[t].u;taps[t*3+1]=pass[t].v;taps[t*3+2]=pass[t].weight;}
            a.UseProgram(i.program);a.Uniform3fv(i.taps,4,taps);i.screen(i.resampled,false,2);
        }
        i.checkError("seven-pass filter");i.mainTarget();
        a.BlendFuncSeparate(g::SrcAlpha,g::OneMinusSrcAlpha,g::SrcAlpha,g::OneMinusSrcAlpha);
        i.screen(i.filtered,false,1);i.checkError("soft-shadow composition");
        if(i.materialState) {
            auto& state=*i.materialState;
            const auto units=state.fixedTextureUnits;
            // FinishSoftShadowsARB0x9dd43 calls framebuffer command2 inside
            // PushAttrib(0x6040): enables are restored, bindings/env are not.
            // Its final0x9dfc8 explicitly disablesFP after PopAttrib.
            state.finishFramebufferFragmentCommand();
            for(unsigned unit=0;unit<units.size();++unit)
                state.fixedTextureUnits[unit].enabled=units[unit].enabled;
            for(unsigned n=0;n<4;++n) {
                const auto weight=i.filter.back()[n].weight;
                state.fragmentConstants[n]={{weight,weight,weight,weight}};
            }
        }
        i.frame->saved=false;i.restoreState();return true;
    } catch (const std::exception& e) {error=e.what();cancel();return false;}
}
void SoftShadowPass::cancel() noexcept {
    auto& i=*impl_;if (!i.frame) return;
    if (i.frame->saved) i.restoreScene();
    i.restoreState();
}
void SoftShadowPass::shutdown() noexcept {
    auto& i=*impl_;cancel();if (!i.api) return;
    auto& a=*i.api;
    if (i.program) a.DeleteProgram(i.program);
    if (i.vbo) a.DeleteBuffers(1,&i.vbo);
    if (i.ebo) a.DeleteBuffers(1,&i.ebo);
    const g::UInt textures[]={i.savedScene,i.mask,i.resampled,i.filtered,i.atiScene,i.atiMask,i.atiSource,i.atiTarget,i.atiFrontColor};
    a.DeleteTextures(9,textures);
    if (i.remove) {i.remove(2,i.fbos);i.remove(1,&i.atiFrontFbo);}
    i.program=i.vbo=i.ebo=i.savedScene=i.mask=i.resampled=i.filtered=0;
    i.fbos[0]=i.fbos[1]=0;i.width=i.height=0;i.api=nullptr;
    i.atiScene=i.atiMask=i.atiSource=i.atiTarget=0;i.atiWidth=i.atiHeight=0;
    i.atiFrontColor=i.atiFrontFbo=0;i.atiFrontWidth=i.atiFrontHeight=0;
}
} // namespace neomdl::render
