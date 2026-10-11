// DoBinBucket/RenderGrassPolys, checked Desktop 0xc13c4/0x127de6.
// This is separate from PartTriMesh material dispatch.
#include "render/GrassPass.hpp"
#include "render/EngineDesktopFragmentPrograms.hpp"
#include "render/LightSubmission.hpp"
#include "render/DesktopFixedFunction.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>

namespace neomdl::render {
namespace {
void require(bool b,const std::string& e){if(!b)throw std::runtime_error("Grass renderer: "+e);}
std::array<float,4> vector4(model::Vec3 v,float w){return {{v.x,v.y,v.z,w}};}
constexpr const char* desktopVertex=R"GLSL(#version 120
attribute vec3 a_position;attribute vec3 a_normal;
attribute vec2 a_texCoord0;attribute vec2 a_texCoord1;
uniform mat4 mv,mvp,textureMatrix;uniform bool fixedPath,lighting,k1,normalizeNormals;
uniform vec4 diffuse,ambient,emission;
uniform int lightCount;uniform vec4 lightPosition[8];
uniform vec3 lightDiffuse[8],lightAmbient[8],lightAttenuation[8];
#ifdef ENGINE_GRASS_FIXED_FRAGMENT
uniform bool fixedUv1Array;
#endif
varying vec4 primary;varying vec2 uv0,uv1;varying float fogCoordinate;
void main(){vec4 eye=mv*vec4(a_position,1);gl_Position=mvp*vec4(a_position,1);
 uv0=fixedPath?(textureMatrix*vec4(a_texCoord0,0,1)).xy:a_texCoord0;uv1=a_texCoord1;
 fogCoordinate=fixedPath?abs(eye.z):length(eye.xyz);
 if(fixedPath&&!lighting){primary=vec4(0,0,0,1);}
 else if(fixedPath&&!k1){primary=vec4(emission.rgb,diffuse.a);}
 else{vec3 n=mat3(mv)*a_normal;if(fixedPath&&normalizeNormals)n=normalize(n);vec3 color=emission.rgb;
  for(int i=0;i<8;++i){if(i>=lightCount)break;
   vec3 d=lightPosition[i].xyz-eye.xyz*lightPosition[i].w;
   float dd=dot(d,d);vec3 direction=d*inversesqrt(dd);
   float attenuation=1.;if(lightPosition[i].w!=0.){vec3 a=lightAttenuation[i];attenuation=1./(a.x+a.y*sqrt(dd)+a.z*dd);}
   color+=attenuation*(ambient.rgb*lightAmbient[i]+diffuse.rgb*lightDiffuse[i]*max(dot(n,direction),0.));}
  primary=clamp(vec4(color,diffuse.a),0.,1.);}
#ifdef ENGINE_GRASS_FIXED_FRAGMENT
 gl_FrontColor=primary;gl_FogFragCoord=fogCoordinate;
 if(fixedPath){
  vec3 n=mat3(mv)*a_normal;if(normalizeNormals)n=normalize(n);
  engineFixedTexcoords(eye,n,vec4(a_texCoord0,0,1),vec4(a_texCoord1,0,1),fixedUv1Array);
 }else{
  // StandardVP copies attributes8..11 only; higher outputs are unwritten.
  gl_TexCoord[0]=vec4(a_texCoord0,0,1);gl_TexCoord[1]=vec4(a_texCoord1,0,1);
  gl_TexCoord[2]=uFixedCoordinates[2];gl_TexCoord[3]=uFixedCoordinates[3];}
#endif
}
)GLSL";
constexpr const char* desktopFragment=R"GLSL(#version 120
uniform sampler2D texture0,texture1;uniform bool lightmapped,forceSight,alphaTest,k1,nvCombiners;
uniform float alphaCutoff,desaturation;uniform int fogMode;uniform vec4 fogData;uniform vec3 fogColor;
varying vec4 primary;varying vec2 uv0,uv1;varying float fogCoordinate;
void main(){vec4 base=texture2D(texture0,uv0);vec3 rgb=base.rgb*primary.rgb;
 float alpha=base.a*primary.a;
 if(lightmapped){vec3 lm=texture2D(texture1,uv1).rgb;
  if(k1){vec3 sum=primary.rgb+lm;if(nvCombiners)sum=clamp(sum,0.,1.);
   rgb=base.rgb*sum;if(nvCombiners)rgb=clamp(rgb,0.,1.);}
  else rgb+=base.rgb*lm;}
 if(forceSight){rgb=vec3(dot(rgb,vec3(.30,.59,.11))*desaturation);
  // Engine desaturating Lightmap FP calculates r2.a, but outputs r0.a!
  if(lightmapped)alpha=base.a;
 }else if((k1||!lightmapped)&&fogMode!=0){float t=1.;
  if(fogMode==1)t=(fogData.y-fogCoordinate)/(fogData.y-fogData.x);
  else if(fogMode==2)t=exp(-fogData.z*fogCoordinate);
  else t=exp(-pow(fogData.z*fogCoordinate,2.));rgb=mix(fogColor,rgb,clamp(t,0.,1.));}
 if(alphaTest&&!(alpha>alphaCutoff))discard;gl_FragColor=vec4(rgb,alpha);}
)GLSL";
struct GrassVertex {
    model::Vec3 position,normal;
    model::Vec2 texcoord0,texcoord1;
};
std::vector<GrassVertex> grassVertices(const model::Mesh& mesh) {
    std::vector<GrassVertex> vertices;vertices.reserve(mesh.vertices.size());
    const auto finite=[](auto v){return std::isfinite(v.x)&&std::isfinite(v.y);};
    for(std::size_t n=0;n<mesh.vertices.size();++n) {
        const auto& source=mesh.vertices[n];const auto normal=mesh.normalAt(n);
        require(finite(source.position)&&std::isfinite(source.position.z)&&
            finite(normal)&&std::isfinite(normal.z)&&finite(source.texcoord0)&&finite(source.texcoord1),
            "non-finite source vertex");
        vertices.push_back({source.position,normal,source.texcoord0,source.texcoord1});
    }
    return vertices;
}
void bindGrassVertexBuffer(gl::Api& a,gl::UInt buffer,bool lightmapped) {
    const gl::Int sizes[]={3,3,2,2};
    const gl::UInt slots[]={0,1,3,4};
    const std::size_t offsets[]={offsetof(GrassVertex,position),offsetof(GrassVertex,normal),
        offsetof(GrassVertex,texcoord0),offsetof(GrassVertex,texcoord1)};
    a.BindBuffer(gl::ArrayBuffer,buffer);
    for(unsigned n=0;n<4;++n) {
        if(n==3&&!lightmapped) {
            const float uv[]={1,1,0,1};a.VertexAttrib4fv(slots[n],uv);a.DisableVertexAttribArray(slots[n]);
        } else {
            a.EnableVertexAttribArray(slots[n]);
            a.VertexAttribPointer(slots[n],sizes[n],gl::Float,0,sizeof(GrassVertex),reinterpret_cast<const void*>(offsets[n]));
        }
    }
    const float white[]={1,1,1,1};
    for(const gl::UInt slot:{2u,5u,6u}) {a.VertexAttrib4fv(slot,white);a.DisableVertexAttribArray(slot);}
}
gl::UInt compile(gl::Api& a,gl::Enum kind,const char* source) {
    auto s=a.CreateShader(kind);require(s!=0,"cannot allocate shader");a.ShaderSource(s,1,&source,nullptr);a.CompileShader(s);
    gl::Int ok=0;a.GetShaderiv(s,gl::CompileStatus,&ok);
    if(!ok){char log[4096]{};a.GetShaderInfoLog(s,4095,nullptr,log);a.DeleteShader(s);throw std::runtime_error(std::string("Grass shader: ")+log);}return s;
}
}
void retainDesktopStandardVertexState(const model::Mesh& mesh,const MeshDrawParameters& p,
        MaterialProgram program,DesktopStandardVertexState& state) noexcept {
    const bool k1=p.material.engineGame==material::EngineMaterialGame::Kotor1;
    if(program==MaterialProgram::Preview||(!k1&&mesh.animated)||
       p.inspectMaterial||p.outlineOnly||p.particlePass||p.lightsaberPass||
       p.engineFlareProgram!=EngineFlareProgram::None||p.engineShadowPass!=EngineShadowPass::None)return;
    state.fixedEmission=p.enginePartMaterial->emission;
    state.emission=p.enginePartMaterial->vertex[0];
    state.diffuse=p.enginePartMaterial->vertex[1];state.ambient=p.enginePartMaterial->vertex[2];
    if(program!=MaterialProgram::FlatFixed&&p.skinEffect==SkinRenderEffect::None&&
       !mesh.animated&&(!k1||k2LightColorSource(p)==LightColorSource::VertexProgram)) {
        state.lightCount=std::min(p.lightCount,p.lights.size());state.lights=p.lights;
    }
}
std::vector<GrassDrawPlan> prepareGrassBin(const GrassBin& bin,const GrassSettings& settings,
        const EngineGrassRenderSettings& cfg,const DesktopStandardVertexState& desktop) {
    validateGrassSettings(settings);require(bin.surface<32&&settings.surfaces[bin.surface].has_value(),"bin has no material");
    for(float f:cfg.view.values)require(std::isfinite(f),"non-finite view");
    for(float f:cfg.projection.values)require(std::isfinite(f),"non-finite projection");
    require(model::validFog(cfg.fog),"invalid scene fog");
    const auto& material=*settings.surfaces[bin.surface];
    const bool lm=bin.lightmapped&&settings.textured;
    const bool k1=cfg.game==model::GameVersion::Kotor1;
    std::vector<GrassDrawPlan> out;
    require(settings.game==cfg.game,"grass area and renderer game differ");
    // Desktop K1 0x316d6e / K2 0x127dfd.
    if(!cfg.hardware.nv1xLevelShaders&&!cfg.hardware.multitexture)return out;
    if(k1) {
        // K1 0x3172ad / 0x3173b3 pass &bin+0x40, not allocation bin+0x38.
        // DrawLightmappedGrass binds ARRAY_BUFFER=0 and reads this as client
        // vertex memory; even one quad puts its normals beyond the 112-byte
        // object. Do not silently replace the engine pointer with K2's pool.
        require(lm&&cfg.hardware.nv1xLevelShaders,
            "K1 Desktop ordinary/non-NV lightmapped grass uses engine object memory as vertex arrays (0x3172ad/0x3173b3); no corrected-pool substitute is used");
    } else if(lm&&!cfg.hardware.nv1xLevelShaders) {
        // K2 retained the same object-address operand in its low-end LM
        // passes: Desktop 0x12847b / 0x12853c.
        // Both helpers read that address directly; ordinary grass uses the pool.
        require(false,"K2 Desktop non-NV lightmapped grass uses engine bin+0x40 as vertex arrays (0x12847b/0x12853c); no corrected-pool substitute is used");
    }
    const auto append=[&](const GrassGroup& group,const GrassGroup* borrowed) {
        GrassDrawPlan d;d.mesh=GrassScene::drawMesh(bin,group,lm);
        d.lightmapped=lm;
        d.fixed=!lm||cfg.hardware.nonRadeon8500Validation;d.group=borrowed;
        const bool forceSight=!k1&&cfg.forceSight;
        d.programId=lm?(forceSight?57u:26u):(forceSight?56u:0u);
        d.emission=d.fixed?(k1?desktop.fixedEmission:vector4(material.emissive,1)):desktop.emission;
        out.push_back(std::move(d));
    };
    if(lm)for(const auto& group:bin.groups)append(group,&group);
    else append({0,bin.bladeCount,0,{}},nullptr);
    return out;
}
bool GrassPass::prepareDesktop(gl::Api& a,bool fixedFragment,std::string& error) {
    auto& program=fixedFragment?desktopFixedProgram_:desktopProgram_;
    if(program)return true;
    gl::UInt v=0,f=0,p=0;
    try{std::string vertex(desktopVertex);if(fixedFragment)vertex.insert(vertex.find('\n')+1,std::string("#define ENGINE_GRASS_FIXED_FRAGMENT\n")+desktopFixedCoordinatesGlsl);
        v=compile(a,gl::VertexShader,vertex.c_str());if(!fixedFragment)f=compile(a,gl::FragmentShader,desktopFragment);p=a.CreateProgram();require(p!=0,"cannot allocate program");a.AttachShader(p,v);if(f)a.AttachShader(p,f);
        const char* names[]={"a_position","a_normal","a_color","a_texCoord0","a_texCoord1"};for(unsigned i=0;i<5;++i)a.BindAttribLocation(p,i,names[i]);a.LinkProgram(p);
        gl::Int ok=0;a.GetProgramiv(p,gl::LinkStatus,&ok);if(!ok){char log[4096]{};a.GetProgramInfoLog(p,4095,nullptr,log);throw std::runtime_error(std::string("Grass link: ")+log);}
        a.DeleteShader(v);if(f)a.DeleteShader(f);program=p;return true;
    }catch(const std::exception& e){if(v)a.DeleteShader(v);if(f)a.DeleteShader(f);if(p)a.DeleteProgram(p);error=e.what();return false;}
}
bool GrassPass::draw(gl::Api& a,const GrassScene& scene,const EngineGrassRenderSettings& cfg,
        const GrassTextureBinding& binding,EngineSceneRasterState& raster,
        EngineMaterialState& desktopTextures,DesktopStandardVertexState& desktop,
        std::uint64_t& drawCalls,std::string& error) {
    error.clear();const auto& s=scene.settings();if(!s.enabled)return true;
    try {
        require(bool(binding),"no texture Bind callback");require(std::isfinite(cfg.restoredAlphaCutoff)&&std::isfinite(cfg.desaturation),"non-finite caller constants");
        const bool k1=cfg.game==model::GameVersion::Kotor1,forceSight=!k1&&cfg.forceSight;
        const bool fixedForce=forceSight&&!cfg.hardware.arbFragmentPrograms;
        const bool engineFragmentDispatch=k1?cfg.hardware.atiValidation:cfg.hardware.arbFragmentPrograms;
        std::unique_ptr<desktop_programs::LegacyGuard> fixedRestore;
        if(fixedForce) {
            if(!legacyReady_){if(!legacy_.load(loader_,error))return false;legacyReady_=true;}
            fixedRestore=std::make_unique<desktop_programs::LegacyGuard>(a,legacy_);
        }
        if(k1&&!k1StandardCreated_&&!cfg.hardware.nonRadeon8500Validation&&cfg.hardware.nv1xLevelShaders&&cfg.hardware.vertexPrograms) {
            const auto order=scene.drawOrder();
            if(std::any_of(order.begin(),order.end(),[&](auto index){return scene.bins()[index].lightmapped&&s.textured;})) {
                // The first Enable(nullptr,true) compiles the grass VP.
                // SetupVertexProgramLights initializes all light colors to
                // zero (0x322f35..0x322fe6); the null Part then skips BOTH
                // upload sites 0x375d39 / 0x375f04. Later enables retain them.
                desktop.lightCount=0;
                k1StandardCreated_=true;
            }
        }
        struct BinPlan {const GrassBin* bin;std::vector<GrassDrawPlan> draws;};std::vector<BinPlan> plans;
        for(auto index:scene.drawOrder()) {
            const auto& bin=scene.bins()[index];auto draws=prepareGrassBin(bin,s,cfg,desktop);
            if(!draws.empty())plans.push_back({&bin,std::move(draws)});
        }
        if(!plans.empty()&&!prepareDesktop(a,fixedForce,error))return false;
        if(!plans.empty()) {if(!vbo_)a.GenBuffers(1,&vbo_);if(!ibo_)a.GenBuffers(1,&ibo_);require(vbo_&&ibo_,"cannot allocate stream buffers");}
        if(!k1)raster.alphaTest=true;
        raster.alphaFunction=gl::Greater;raster.alphaReference=s.alphaCutoff;
        raster.blend=true;raster.source=gl::SrcAlpha;raster.destination=gl::OneMinusSrcAlpha;
        if(!k1)raster.depthWrite=true;
        raster.cullFaces=false;raster.clientArrays={{true,true,false,true}};
        struct Finish {gl::Api& a;EngineSceneRasterState& r;float alpha;
            ~Finish(){r.cullFaces=true;r.alphaFunction=gl::Greater;r.alphaReference=alpha;a.AlphaFunc(gl::Greater,alpha);a.Enable(gl::CullFace);}
        } finish{a,raster,cfg.restoredAlphaCutoff};
        a.Enable(gl::Blend);a.Disable(gl::CullFace);if(!k1)a.DepthMask(1);
        a.BlendEquationSeparate(gl::FuncAdd,gl::FuncAdd);a.BlendFuncSeparate(gl::SrcAlpha,gl::OneMinusSrcAlpha,gl::SrcAlpha,gl::OneMinusSrcAlpha);
        a.Disable(gl::AlphaTest); // explicit shader alpha test, independent of desktop host state
        const auto bind=[&](const GrassBin& bin,const std::string& name,unsigned unit,bool selectStage=true) {
            unsigned active=unit;
            if(fixedForce) {
                if(selectStage) {
                    desktopTextures.fixedCurrentStage=unit;
                    if(cfg.hardware.multitexture)desktopTextures.fixedActiveTextureUnit=unit;
                }
                active=desktopTextures.fixedActiveTextureUnit;
            }
            a.ActiveTexture(gl::Texture0+active);const auto texture=binding(bin,name,unit,error);
            require(texture!=0&&error.empty(),error.empty()?"missing texture "+name:error);
            desktopTextures.bind(active,false,texture);
            auto& fixed=desktopTextures.fixedTextureUnits[active];
            if(cfg.hardware.cubeMap)fixed.enabled[1]=false;
            fixed.enabled[2]=false;
            a.BindTexture(gl::Texture2D,texture);return texture;
        };
        for(auto& plan:plans) {
            const auto& bin=*plan.bin;const auto& material=*s.surfaces[bin.surface];
            if(cfg.hardware.vertexPrograms&&!cfg.hardware.nonRadeon8500Validation) {
                desktop.diffuse=vector4(material.diffuse,bin.opacity);desktop.ambient=vector4(material.ambient,1);
            }
            raster.materialAmbient=k1?vector4(material.ambient,1):std::array<float,4>{{0,0,0,1}};
            raster.materialDiffuse=k1?vector4(material.diffuse,bin.opacity):std::array<float,4>{{0,0,0,bin.opacity}};
            a.Materialfv(gl::Front,gl::MaterialAmbient,raster.materialAmbient.data());a.Materialfv(gl::Front,gl::MaterialDiffuse,raster.materialDiffuse.data());
            if(!k1){raster.materialEmission=vector4(material.emissive,bin.opacity);a.Materialfv(gl::Front,gl::MaterialEmission,raster.materialEmission.data());}
            // BindTID does not activate the logical stage. The preceding
            // lightmapped bin can leave server unit1 active after resetting
            // only ClientActiveTexture and the logical stage (0x128695).
            if(s.textured)bind(bin,material.texture,fixedForce?desktopTextures.fixedCurrentStage:0,!fixedForce);
            // Engine cleanup after all groups, including callback/GL failure.
            struct Cleanup {EngineMaterialState& desktop;bool lm,force,k1,fixedForce,engineFragmentDispatch;
                ~Cleanup(){
                    if(engineFragmentDispatch && (lm||force))desktop.finishFragmentCommand(force?74u:32u,k1);
                    if(fixedForce&&lm)desktop.fixedCurrentStage=0;
                }
            } cleanup{desktopTextures,bin.lightmapped&&s.textured,forceSight,k1,fixedForce,engineFragmentDispatch};
            for(auto& d:plan.draws) {
                gl::UInt diffuseTexture{},lightmapTexture{};
                if(d.lightmapped) {diffuseTexture=bind(bin,material.texture,0);require(d.group&&!d.group->lightmap.empty(),"missing group lightmap");lightmapTexture=bind(bin,d.group->lightmap,1);}
                const auto vertices=grassVertices(d.mesh);
                a.BindBuffer(gl::ArrayBuffer,vbo_);a.BufferData(gl::ArrayBuffer,gl::Bytes(vertices.size()*sizeof(GrassVertex)),vertices.data(),gl::DynamicDraw);
                a.BindBuffer(gl::ElementArrayBuffer,ibo_);a.BufferData(gl::ElementArrayBuffer,gl::Bytes(d.mesh.indices.size()*sizeof(std::uint32_t)),d.mesh.indices.data(),gl::DynamicDraw);
                bindGrassVertexBuffer(a,vbo_,d.lightmapped);
                if(forceSight&&engineFragmentDispatch) {
                    desktopTextures.fragmentConstants[1]={{0,0,0,d.lightmapped?.75f:1.f}};
                    if(engineFragmentDispatch)desktopTextures.fragmentConstants[7]={{.3f,.59f,.11f,cfg.desaturation}};
                }
                if(engineFragmentDispatch) {
                    if(d.lightmapped) {
                        desktopTextures.fixedSecondTexture=true;
                        desktopTextures.fixedTextureUnits[1].enabled[0]=true;
                    }
                    if(const auto* fragment=desktop_programs::k2FragmentProgram(d.programId))desktopTextures.bindFragmentProgram(fragment);
                }
                const auto desktopProgram=fixedForce?desktopFixedProgram_:desktopProgram_;
                a.UseProgram(desktopProgram);
                const auto location=[&](const char* n){return a.GetUniformLocation(desktopProgram,n);};
                const auto integer=[&](const char* n,int x){a.Uniform1i(location(n),x);};const auto scalar=[&](const char* n,float x){a.Uniform1f(location(n),x);};
                auto mvp=model::multiply(cfg.projection,cfg.view);a.UniformMatrix4fv(location("mv"),1,0,cfg.view.values.data());a.UniformMatrix4fv(location("mvp"),1,0,mvp.values.data());
                a.UniformMatrix4fv(location("textureMatrix"),1,0,desktopTextures.textureMatrix(0).values.data());
                integer("fixedPath",d.fixed);integer("lighting",raster.lighting);integer("normalizeNormals",desktopTextures.normalizeNormals());integer("k1",k1);integer("nvCombiners",k1&&!cfg.hardware.atiValidation);integer("alphaTest",raster.alphaTest);integer("lightmapped",d.lightmapped);integer("forceSight",forceSight);integer("texture0",0);integer("texture1",1);
                auto diffuse=vector4(material.diffuse,bin.opacity),ambient=vector4(material.ambient,1);
                a.Uniform4fv(location("diffuse"),1,diffuse.data());a.Uniform4fv(location("ambient"),1,ambient.data());a.Uniform4fv(location("emission"),1,d.emission.data());
                scalar("alphaCutoff",s.alphaCutoff);scalar("desaturation",cfg.desaturation);
                const auto lightCount=d.fixed&&k1?std::min<std::size_t>(8,cfg.fixedLights.size()):desktop.lightCount;
                integer("lightCount",int(lightCount));
                for(std::size_t l=0;l<lightCount;++l){const auto suffix="["+std::to_string(l)+"]";const auto& light=d.fixed&&k1?cfg.fixedLights[l]:desktop.lights[l];a.Uniform4fv(location(("lightPosition"+suffix).c_str()),1,light.position.data());a.Uniform3fv(location(("lightDiffuse"+suffix).c_str()),1,light.diffuse.data());a.Uniform3fv(location(("lightAmbient"+suffix).c_str()),1,light.ambient.data());a.Uniform3fv(location(("lightAttenuation"+suffix).c_str()),1,light.attenuation.data());}
                integer("fogMode",raster.fog?static_cast<int>(cfg.fog.mode):0);float fog[]={cfg.fog.start,cfg.fog.end,cfg.fog.density,0};a.Uniform4fv(location("fogData"),1,fog);float color[]={cfg.fog.color.x,cfg.fog.color.y,cfg.fog.color.z};a.Uniform3fv(location("fogColor"),1,color);
                for(unsigned u=0;u<2;++u){a.ActiveTexture(gl::Texture0+u);a.BindTexture(gl::Texture2D,desktopTextures.binding(u,false));}
                if(fixedForce) {
                    MeshDrawParameters parameters;parameters.material.engineHardware=cfg.hardware;
                    parameters.diffuseTexture=diffuseTexture;
                    parameters.lightmapTexture=lightmapTexture;
                    // The two virtual binds for command57 occurred above;
                    // command56 only enables unit0 and performs no Bind.
                    require(applyDesktopFixedFragmentCommand(d.programId,parameters,desktopTextures,error),error);
                    applyDesktopFixedTextureState(a,legacy_,desktopTextures);
                    applyDesktopFixedVertexInputs(a,desktopProgram,desktopTextures);
                    integer("fixedUv1Array",d.lightmapped);
                    a.Disable(desktop_programs::VP);a.Disable(desktop_programs::FP);
                    if(raster.alphaTest)a.Enable(gl::AlphaTest);else a.Disable(gl::AlphaTest);
                    a.AlphaFunc(gl::Greater,s.alphaCutoff);
                    if(raster.fog)a.Enable(desktop_programs::Fog);else a.Disable(desktop_programs::Fog);
                    legacy_.Fogf(0x0b63,cfg.fog.start);legacy_.Fogf(0x0b64,cfg.fog.end);legacy_.Fogf(0x0b62,cfg.fog.density);
                    legacy_.Fogf(0x0b65,float(cfg.fog.mode==model::FogMode::Exponential?0x0800:cfg.fog.mode==model::FogMode::ExponentialSquared?0x0801:0x2601));
                    const float fogColor[]={cfg.fog.color.x,cfg.fog.color.y,cfg.fog.color.z,1};legacy_.Fogfv(0x0b66,fogColor);
                }
                require(d.mesh.indices.size()<=std::size_t(std::numeric_limits<gl::Size>::max()),"grass index draw exceeds GL limit");
                a.DrawElements(gl::Triangles,gl::Size(d.mesh.indices.size()),gl::UnsignedInt,nullptr);++drawCalls;
            }
        }
        return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
void GrassPass::shutdown(gl::Api& a) noexcept {if(vbo_)a.DeleteBuffers(1,&vbo_);if(ibo_)a.DeleteBuffers(1,&ibo_);if(desktopProgram_)a.DeleteProgram(desktopProgram_);if(desktopFixedProgram_)a.DeleteProgram(desktopFixedProgram_);vbo_=ibo_=desktopProgram_=desktopFixedProgram_=0;legacyReady_=false;k1StandardCreated_={};}
} // namespace neomdl::render
