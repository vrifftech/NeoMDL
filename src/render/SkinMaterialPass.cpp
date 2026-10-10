#include "render/SkinMaterialPass.hpp"
#include "render/SkinMaterialPrograms.hpp"
#include "render/DesktopMaterialPrograms.hpp"
#include "render/ShaderMeshRenderer.hpp"
#include <model/EngineCpuSkin.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace neomdl::render {
namespace {
namespace g=gl;
using namespace desktop_programs;
bool skinForceSight(const MeshDrawParameters& p,MaterialProgram type) {
    if(!p.forceSight.enabled || p.material.engineGame!=material::EngineMaterialGame::Kotor2)
        return false;
    // RenderSkinnedVP keeps the old bump path and the no-NV2x fallback.
    if(type==MaterialProgram::SkinBump && !p.material.engineNewSkinBump)return false;
    return !(type==MaterialProgram::SkinFlat && p.material.engineSkinBumpFallback &&
             !p.material.engineHardware.nv2xLevelShaders);
}
material::ForceSightPlan skinForcePlan(const MeshDrawParameters& p,MaterialProgram type) {
    const bool environment=type==MaterialProgram::SkinEnvironment;
    return material::setupForceSight(p.forceSight,p.partHasOwner?&p.forceSightOwner:nullptr,
        environment?52u:50u,environment?58u:56u,false,p.forceSightIllumination);
}
const char* skinForceFragment(unsigned command) {
    switch(command) {
    case 50:return skin_programs::ForceGlow;
    case 52:return skin_programs::ForceEnvironmentGlow;
    case 56:return skin_programs::ForceDesaturate;
    case 58:return skin_programs::ForceEnvironmentDesaturate;
    case 92:return skin_programs::ForceIlluminationDesaturate;
    default:return nullptr; // Desktop dispatcher 94 is a verified no-op.
    }
}
void enabled(g::Api& a,g::Enum cap,bool value){if(value)a.Enable(cap);else a.Disable(cap);}
std::vector<PreviewLight> baseLights(const MeshDrawParameters& p,MaterialProgram program) {
    std::vector<PreviewLight> lights;
    for(std::size_t n=0;n<std::min(p.lightCount,p.lights.size());++n) {
        const auto& light=p.lights[n];
        if(program==MaterialProgram::SkinBump && std::any_of(p.skinBumpLights.begin(),p.skinBumpLights.end(),
            [&](const EngineBumpLight& bump){return bump.id==light.id;}))continue;
        lights.push_back(light);
    }
    return lights;
}
std::string litSkin(const std::vector<PreviewLight>& lights,bool environment=false) {
    return desktop_programs::lightProgram(environment?skin_programs::SkinEnvironment:skin_programs::Skin,lights);
}
model::Vec3 localPoint(const model::Mat4& frame,model::Vec3 p) {
    const auto& m=frame.values;p={p.x-m[12],p.y-m[13],p.z-m[14]};
    return {m[0]*p.x+m[1]*p.y+m[2]*p.z,m[4]*p.x+m[5]*p.y+m[6]*p.z,m[8]*p.x+m[9]*p.y+m[10]*p.z};
}
}

void validateEngineSkinFrame(const model::Mesh& mesh,const model::DeformedMesh& pose,bool requireBasis) {
    if(!pose.valid)
        throw std::invalid_argument("Engine skin needs authored influences and its current palette");
    if(requireBasis && mesh.mdxTangentOffset==0xffffffffu)
        throw std::invalid_argument("Engine skin-bump pass requires the authored three-vector basis");
}

std::vector<EngineSkinVertex> engineSkinVertices(const model::Mesh& mesh,const model::DeformedMesh& pose,bool requireBasis) {
    validateEngineSkinFrame(mesh,pose,requireBasis);
    const auto channels=model::decodeEngineFloatSkinChannels(mesh);
    std::vector<EngineSkinVertex> vertices;vertices.reserve(mesh.vertices.size());
    for(std::size_t n=0;n<mesh.vertices.size();++n){
        const auto& v=mesh.vertices[n];const auto& influence=channels[n];
        EngineSkinVertex out{v.position,mesh.normalAt(n),influence.weights,influence.indices,v.texcoord0,
            v.tangentBasis.bitangent,v.tangentBasis.tangent,v.tangentBasis.normal};
        vertices.push_back(out);
    }
    return vertices;
}
struct SkinMaterialPass::Impl : desktop_programs::ProgramCache {};
SkinMaterialPass::SkinMaterialPass():impl_(std::make_unique<Impl>()){}
SkinMaterialPass::~SkinMaterialPass()=default;
void SkinMaterialPass::setLoader(const g::Loader& loader){impl_->loader=loader;}
bool SkinMaterialPass::prepare(g::Api& a,const MeshDrawParameters& p,MaterialProgram type,std::string& error) {
    auto& i=*impl_;
    if(!i.ready){
        if(!i.loader || !i.x.load(i.loader,error))return false;
        const auto* raw=a.GetString(0x1f03);const std::string extensions=raw?reinterpret_cast<const char*>(raw):"";
        if(extensions.find("GL_ARB_vertex_program")==std::string::npos || extensions.find("GL_ARB_fragment_program")==std::string::npos){error="Engine Desktop skin programs require compatibility ARB vertex/fragment program support";return false;}
        g::Int attributes{},units{},coords{};a.GetIntegerv(g::MaxVertexAttribs,&attributes);a.GetIntegerv(g::MaxTextureUnits,&units);a.GetIntegerv(0x8871,&coords);
        if(attributes<15 || units<6 || coords<5){error="Engine skin material requires 15 attributes, six image units and five texture coordinates";return false;}
        i.ready=true;
    }
    if(p.skinBumpLights.size()>3){error="Engine skin bump list exceeds the manager's three illumination slots";return false;}
    if(baseLights(p,type).size()>4){error="Engine skin base supports three authored lights plus the explicit inspection headlight";return false;}
    // The object position/quaternion conversion is rigid, not a scale-bearing
    // inverse model matrix. No mesh-derived bounding sphere enters this pass.
    if(!p.hasSkinOwnerFrame) {
        error="Engine skin requires its owner frame";return false;
    }
    LegacyGuard restore(a,i.x);
    const auto lights=baseLights(p,type);
    const bool force=skinForceSight(p,type);
    if(force) {
        if(!i.program(a,VP,litSkin(lights,type==MaterialProgram::SkinEnvironment),error))return false;
        // Only the ARB dispatcher selects these fragments. The legacy
        // dispatcher retains and changes the actual fixed texture pipeline.
        if(p.material.engineHardware.arbFragmentPrograms)
            for(const unsigned command:{50u,52u,56u,58u,92u})
                if(!i.program(a,FP,skinForceFragment(command),error))return false;
        if(type==MaterialProgram::SkinBump && p.fog.mode!=model::FogMode::Disabled &&
           (!i.program(a,VP,skin_programs::Fog,error)||!i.program(a,FP,skin_programs::FogFragment,error)))return false;
    } else if(type==MaterialProgram::SkinFlat || type==MaterialProgram::SkinEnvironment) {
        if(!i.program(a,VP,litSkin(lights,type==MaterialProgram::SkinEnvironment),error))return false;
        if(type==MaterialProgram::SkinEnvironment) {
            const auto fragment=p.material.engineAtiValidation
                ? (p.environmentCube?skin_programs::CubeEnvironmentFogFragment:skin_programs::EnvironmentFogFragment)
                : (p.environmentCube?skin_programs::CubeEnvironmentFragment:skin_programs::EnvironmentFragment);
            if(!i.program(a,FP,fragment,error))return false;
        } else if(p.material.engineAtiValidation && p.fog.mode!=model::FogMode::Disabled &&
                  !i.program(a,FP,desktop_programs::standardFogFragment(p.material.engineGame==material::EngineMaterialGame::Kotor1),error))return false;
    } else {
        const char* first=type==MaterialProgram::SkinBumpyShiny?skin_programs::Shiny:
            p.material.engineNewSkinBump?skin_programs::Bump:skin_programs::OldBump;
        if(!i.program(a,VP,first,error) || !i.program(a,FP,type==MaterialProgram::SkinBumpyShiny?
            skin_programs::ShinyFragment:skin_programs::BumpFragment,error) ||
           !i.program(a,VP,litSkin(lights),error))return false;
        if(type==MaterialProgram::SkinBump && p.fog.mode!=model::FogMode::Disabled &&
           (!i.program(a,VP,skin_programs::Fog,error)||!i.program(a,FP,skin_programs::FogFragment,error)))return false;
    }
    return a.GetError()==g::NoError;
}
bool SkinMaterialPass::draw(g::Api& a,const model::Mesh& mesh,const model::DeformedMesh& pose,
    const MeshDrawParameters& p,MaterialProgram type,EngineMaterialState& state,g::UInt normalization,
    g::UInt vbo,g::Size count,const std::vector<model::EngineMeshDraw>& batches,std::uint64_t& calls,std::string& error) {
    // RenderSkinnedVP explicitly removes the part position/orientation from
    // its bone palette. The draw matrix puts that frame back; raw basis and
    // source-position operations still see the original part coordinates.
    auto engine=p;
    engine.modelView=model::multiply(p.modelView,pose.skinPartFrame);
    const auto partWorld=model::multiply(p.skinOwnerFrame,pose.skinPartFrame);
    auto& i=*impl_;auto& x=i.x;LegacyGuard restore(a,x,&engine);
    a.UseProgram(0);a.Disable(0x0b50);a.Disable(0x0b57); // lighting/color material
    a.Enable(VP);a.Disable(FP);
    a.BindBuffer(g::ArrayBuffer,vbo);
    const g::UInt slots[]={0,1,2,4,8,12,13,14};const g::Int sizes[]={3,4,3,4,2,3,3,3};
    const std::size_t offsets[]={offsetof(EngineSkinVertex,position),offsetof(EngineSkinVertex,weights),offsetof(EngineSkinVertex,normal),offsetof(EngineSkinVertex,indices),offsetof(EngineSkinVertex,uv),offsetof(EngineSkinVertex,basis4),offsetof(EngineSkinVertex,basis5),offsetof(EngineSkinVertex,basis6)};
    for(g::UInt slot=0;slot<15;++slot)a.DisableVertexAttribArray(slot);
    for(unsigned n=0;n<8;++n){a.EnableVertexAttribArray(slots[n]);a.VertexAttribPointer(slots[n],sizes[n],g::Float,0,sizeof(EngineSkinVertex),reinterpret_cast<const void*>(offsets[n]));}
    for(unsigned unit=0;unit<6;++unit){a.ActiveTexture(g::Texture0+unit);a.Disable(g::Texture2D);a.Disable(g::TextureCube);}
    a.ActiveTexture(g::Texture0);
    a.Enable(g::DepthTest);a.DepthFunc(g::Lequal);a.DepthMask(p.material.depthWrite);
    enabled(a,g::CullFace,p.cullBackfaces);a.CullFace(g::Back);a.FrontFace(g::CCW);
    a.PolygonMode(g::FrontAndBack,p.wireframe?g::Line:g::Fill);
    a.BlendEquationSeparate(g::FuncAdd,g::FuncAdd);
    x.AlphaFunc(0x0204,p.material.alphaCutoff);
    const float fogColor[]={p.fog.color.x,p.fog.color.y,p.fog.color.z,1};x.Fogfv(0x0b66,fogColor);
    x.Fogf(0x0b63,p.fog.start);x.Fogf(0x0b64,p.fog.end);x.Fogf(0x0b62,p.fog.density);
    x.Fogf(0x0b65,static_cast<float>(p.fog.mode==model::FogMode::Exponential?0x0800:p.fog.mode==model::FogMode::ExponentialSquared?0x0801:0x2601));
    // RenderSkinnedVP 0x109f77..0x10a049: ATI validation remains true on
    // ARB-fragment hardware. The environment shader always consumes secondary
    // red fog; when scene fog is off its vertex producer uses 1000 / 2000.
    if(type==MaterialProgram::SkinEnvironment && p.material.engineAtiValidation &&
       p.fog.mode==model::FogMode::Disabled) {
        x.Fogf(0x0b63,1000.0f);x.Fogf(0x0b64,2000.0f);
    }
    std::array<std::array<float,4>,96> constants{};
    constants[16]={{0,2,3,1}};constants[15]={{1,1,1,1}};constants[89]={{0,0,0,1}};
    for(unsigned row=0;row<3;++row)constants[92-row]={{p.worldFromEye.values[row],p.worldFromEye.values[4+row],p.worldFromEye.values[8+row],p.worldFromEye.values[12+row]}};
    const auto& part=pose.skinPartFrame.values;
    for(unsigned slot=0;slot<pose.skinPalette.size();++slot)if(pose.skinPalette[slot].valid) {
        const auto& source=pose.skinPalette[slot].rows;
        for(unsigned row=0;row<3;++row)for(unsigned c=0;c<4;++c) {
            float value=0;
            for(unsigned k=0;k<3;++k)value+=part[row*4+k]*(source[k*4+c]-(c==3?part[12+k]:0.0f));
            constants[18+slot*3+row][c]=value;
        }
    }
    for(unsigned row=0;row<3;++row)constants[93+row]=p.enginePartMaterial->vertex[row];
    const auto base=baseLights(p,type);
    for(unsigned n=0;n<base.size();++n){const auto& light=base[n];auto& pos=constants[70+4*n];
        for(unsigned row=0;row<3;++row)pos[row]=p.worldFromEye.values[row]*light.position[0]+p.worldFromEye.values[4+row]*light.position[1]+p.worldFromEye.values[8+row]*light.position[2]+p.worldFromEye.values[12+row]*light.position[3];
        pos[3]=light.position[3];
        constants[71+4*n]={{light.attenuation[0],light.attenuation[1],light.attenuation[2],0}};
        constants[72+4*n]={{light.ambient[0],light.ambient[1],light.ambient[2],1}};
        constants[73+4*n]={{light.diffuse[0],light.diffuse[1],light.diffuse[2],1}};}
    const auto vp=[&](const std::string& program){const auto id=i.program(a,VP,program,error);if(!id)return false;x.BindProgramARB(VP,id);for(unsigned k=0;k<96;++k)x.ProgramLocalParameter4fvARB(VP,k,constants[k].data());a.Enable(VP);return true;};
    const auto fp=[&](const char* program){const auto id=i.program(a,FP,program,error);if(!id)return false;x.BindProgramARB(FP,id);for(unsigned k=0;k<state.fragmentConstants.size();++k)x.ProgramLocalParameter4fvARB(FP,k,state.fragmentConstants[k].data());a.Enable(FP);if(p.material.engineGame==material::EngineMaterialGame::Kotor1||p.material.engineHardware.arbFragmentPrograms)state.bindFragmentProgram(program);return true;};
    const bool force=skinForceSight(p,type);
    const bool fixedForce=force && !p.material.engineHardware.arbFragmentPrograms;
    const bool engineFragmentDispatch=p.material.engineGame==material::EngineMaterialGame::Kotor1||p.material.engineHardware.arbFragmentPrograms;
    const auto bind=[&](unsigned unit,bool cube,g::UInt texture,bool repeat){
        if(repeat && texture && p.rebindEngineTexture){texture=p.rebindEngineTexture(texture,error);if(!texture)return false;}
        if(fixedForce && repeat) {
            // Skin's preforce BindTexture0 at109e3a/10c948 resets only
            // ClientActiveTexture and logical stage; BindTID uses server unit.
            state.fixedCurrentStage=0;unit=state.fixedActiveTextureUnit;
            auto& fixed=state.fixedTextureUnits[unit];
            if(p.material.engineHardware.cubeMap)fixed.enabled[1]=cube;
            fixed.enabled[2]=false;
            state.bind(unit,cube&&p.material.engineHardware.cubeMap,texture);
            return true; // forceSetup realizes the complete retained image
        }
        state.bind(unit,cube,texture);
        auto& target=state.fixedTextureUnits[unit];
        if(p.material.engineHardware.cubeMap)target.enabled[1]=cube;
        target.enabled[2]=false;target.enabled[cube?1:0]=true;
        a.ActiveTexture(g::Texture0+unit);a.BindTexture(cube?g::TextureCube:g::Texture2D,texture);a.Enable(cube?g::TextureCube:g::Texture2D);a.ActiveTexture(g::Texture0);return true;};
    const auto blend=[&](g::Enum src,g::Enum dst){a.BlendFuncSeparate(src,dst,src,dst);};
    const auto draw=[&](){
        if(p.material.engineGame==material::EngineMaterialGame::Kotor1){static constexpr g::Enum modes[]={0,1,3,4,5,6,7,8,9,2};for(const auto& batch:batches){a.DrawElements(modes[batch.primitive],static_cast<g::Size>(batch.indexCount),g::UnsignedInt,reinterpret_cast<const void*>(batch.firstIndex*sizeof(std::uint32_t)));++calls;}}
        else{a.DrawElements(g::Triangles,count,g::UnsignedInt,nullptr);++calls;}
    };
    const auto cleanup=[&](){
        // The legacy dispatcher admits only commands29..109 (0xf9222).
        // Its 13/0 calls, including the outer ATI cleanup, change no state.
        if(!engineFragmentDispatch)return;
        state.forceSightFragmentCommand=0;state.finishFragmentCommand(0,p.material.engineGame==material::EngineMaterialGame::Kotor1);a.Disable(FP);x.BindProgramARB(FP,0);for(unsigned unit=0;unit<4;++unit){state.bind(unit,false,0);a.ActiveTexture(g::Texture0+unit);a.BindTexture(g::Texture2D,0);a.Disable(g::Texture2D);a.Disable(g::TextureCube);}a.ActiveTexture(g::Texture0);};
    const auto forceSetup=[&](){
        const auto plan=skinForcePlan(p,type);
        if(plan.replaceBlend)blend(plan.oneSource?g::One:g::SrcAlpha,g::OneMinusSrcAlpha);
        if(fixedForce) {
            a.Disable(FP);
            if(!applyDesktopFixedFragmentCommand(plan.command,engine,state,error))return false;
            applyDesktopFixedTextureState(a,x,state);
            return true;
        }
        const auto* fragment=skinForceFragment(plan.command);
        if(fragment) {
            state.forceSightFragmentCommand=plan.command;
            state.fragmentConstants[1]=plan.illumination
                ?std::array<float,4>{{p.ownerIllumination[0],p.ownerIllumination[1],p.ownerIllumination[2],1}}
                :plan.color;
            if(type==MaterialProgram::SkinEnvironment) {
                state.fixedEnvironmentMap=state.fixedSecondTexture=true;
                // Dispatcher52/58 bind both supplied texture objects. Its ARB
                // fragment still samples unit1's retained 2D target for cubes.
                if(!bind(0,false,p.diffuseTexture,true) ||
                   !bind(1,p.environmentCube,p.environmentTexture,true))return false;
                a.ActiveTexture(g::Texture0+1);
                a.BindTexture(g::Texture2D,state.binding(1,false));
                a.ActiveTexture(g::Texture0);
            }
        }
        fragment=skinForceFragment(state.forceSightFragmentCommand);
        if(fragment) {
            if(!fp(fragment))return false;
            const bool glow=state.forceSightFragmentCommand==50 ||
                            state.forceSightFragmentCommand==52;
            // Desktop dispatcher glow c7 is the literal at0x543ea0; only the
            // desaturation programs read the live desaturation parameter.
            const float weights[]={0.3f,0.59f,0.11f,glow?0.8f:p.forceSight.desaturation};
            std::copy(weights,weights+4,state.fragmentConstants[7].begin());
            x.ProgramLocalParameter4fvARB(FP,7,weights);
        } else a.Disable(FP);
        return true;
    };
    const auto forceCleanup=[&](){
        // Non-ARB command74 is empty and retains targets, texgen and TexEnv.
        if(fixedForce)return;
        // Command74 retains unit0 and cube bindings; 13+0 uses cleanup above.
        state.forceSightFragmentCommand=0;state.finishFragmentCommand(74);a.Disable(FP);
        for(unsigned unit=1;unit<=3;++unit) {
            state.bind(unit,false,0);state.bindVolume(unit,0);
            a.ActiveTexture(g::Texture0+unit);a.BindTexture(g::Texture2D,0);
            a.BindTexture(0x806f,0);a.Disable(g::Texture2D);a.Disable(0x806f);
        }
        a.ActiveTexture(g::Texture0);
    };
    a.Disable(g::AlphaTest);a.Enable(g::Blend);
    if(type==MaterialProgram::SkinFlat || type==MaterialProgram::SkinEnvironment) {
        if(!vp(litSkin(base,type==MaterialProgram::SkinEnvironment)))return false;
        enabled(a,g::Blend,p.material.blending);
        const auto mode=p.material.blendFunction;
        blend(mode==material::BlendFunction::Replace?g::One:g::SrcAlpha,
            mode==material::BlendFunction::Replace?g::Zero:
            mode==material::BlendFunction::Additive?g::One:g::OneMinusSrcAlpha);
        // RenderSkinnedVP's late authored-bump branch 0x10af9f / 0x10afd8
        // replaces only the blend factors; Mat's alpha-test state survives.
        if(type==MaterialProgram::SkinFlat && p.material.engineSkinBumpFallback)
            blend(g::One,g::Zero);
        enabled(a,g::AlphaTest,type==MaterialProgram::SkinFlat && p.material.alphaTest);
        const bool fragmentFog=p.material.engineAtiValidation &&
            (type==MaterialProgram::SkinEnvironment || p.fog.mode!=model::FogMode::Disabled);
        enabled(a,Fog,!fragmentFog && p.fog.mode!=model::FogMode::Disabled);
        if(force && type==MaterialProgram::SkinEnvironment) {
            // Realize the caller's retained binding without a virtual Bind.
            // Commands52/58 perform their own two texture-object calls.
            if(!bind(0,false,state.binding(0,false),false))return false;
        } else if(p.diffuseTexture) {
            if(!bind(0,false,p.diffuseTexture,true))return false;
        }
        x.TexEnvi(0x2300,0x2200,0x2100);
        if(fragmentFog&&engineFragmentDispatch)
            state.fragmentConstants[1]={{p.fog.color.x,p.fog.color.y,p.fog.color.z,1}};
        if(force) {
            if(!forceSetup())return false;
        } else if(type==MaterialProgram::SkinEnvironment) {
            if(engineFragmentDispatch)state.fixedEnvironmentMap=state.fixedSecondTexture=true;
            if(!bind(1,p.environmentCube,p.environmentTexture,true))return false;
            const auto fragment=p.material.engineAtiValidation
                ? (p.environmentCube?skin_programs::CubeEnvironmentFogFragment:skin_programs::EnvironmentFogFragment)
                : (p.environmentCube?skin_programs::CubeEnvironmentFragment:skin_programs::EnvironmentFragment);
            if(!fp(fragment))return false;
        } else if(fragmentFog) {
            // Material::BindTexture0 precedes command 3's second virtual Bind.
            if(!bind(0,false,p.diffuseTexture,true)||!fp(desktop_programs::standardFogFragment(p.material.engineGame==material::EngineMaterialGame::Kotor1)))return false;
        }
        draw(); // engine dispatch-32 only disables targets; no 2D/cube unbind
        if(type==MaterialProgram::SkinEnvironment&&engineFragmentDispatch)state.finishFragmentCommand(32,p.material.engineGame==material::EngineMaterialGame::Kotor1);
        if(force)forceCleanup();
        if(fixedForce && type==MaterialProgram::SkinEnvironment) {
            // RenderEnvironmentMapped0xd015f..d01b5 is caller cleanup,
            // independent of the legacy command74 no-op.
            const auto unit=p.material.engineHardware.multitexture?1u:state.fixedActiveTextureUnit;
            if(p.material.engineHardware.cubeMap)state.fixedTextureUnits[unit].enabled[1]=false;
            state.fixedCurrentStage=0;
            if(p.material.engineHardware.multitexture)state.fixedActiveTextureUnit=0;
        }
        // RenderSkinnedVP 0x10c719 calls command 0 for the ATI family after
        // RenderEnvironmentMapped's command 32 (or after the flat draw).
        if(p.material.engineAtiValidation)cleanup();
    } else if(type==MaterialProgram::SkinBumpyShiny){
        if(!force && p.material.engineSkinShinyPass && p.skinOwnerAlpha>=1.0f){
            constants[15]={{1,0,1,0}};
            if(!vp(skin_programs::Shiny) || !fp(skin_programs::ShinyFragment))return false;
            if(!bind(0,false,p.bumpTexture,true))return false;
            for(unsigned unit=1;unit<=4;++unit)if(!bind(unit,true,normalization,false))return false;
            if(!bind(5,true,p.environmentTexture,true))return false;
            blend(g::One,g::Zero);draw();cleanup();
        }
        constants[15]={{1,1,1,1}};
        if(!vp(litSkin(base)))return false;
        a.Disable(FP);for(unsigned u=1;u<6;++u){a.ActiveTexture(g::Texture0+u);a.Disable(g::TextureCube);a.Disable(g::Texture2D);}a.ActiveTexture(g::Texture0);
        enabled(a,g::Blend,p.material.blending); // engine PopAttrib before pass2
        blend(g::One,g::OneMinusSrcAlpha);a.Disable(g::AlphaTest);
        enabled(a,Fog,p.fog.mode!=model::FogMode::Disabled);
        if(!bind(0,false,p.diffuseTexture,true))return false;
        x.TexEnvi(0x2300,0x2200,0x2100);
        if(force&&!forceSetup())return false;
        draw();if(force)forceCleanup();
    }else{
        a.Disable(Fog);
        if(!force) {
            if(!bind(0,false,p.bumpTexture,true)||!bind(1,true,normalization,false)||!bind(2,true,normalization,false)||!bind(3,false,p.diffuseTexture,true))return false;
            blend(p.material.engineNewSkinBump?g::One:g::SrcAlpha,p.material.engineNewSkinBump?g::Zero:g::OneMinusSrcAlpha);
            const auto camera=localPoint(partWorld,{p.worldFromEye.values[12],p.worldFromEye.values[13],p.worldFromEye.values[14]});
            const std::size_t iterations=std::max<std::size_t>(1,p.skinBumpLights.size());
            for(std::size_t n=0;n<iterations;++n){
                if(!p.skinBumpLights.empty()){
                    const auto& light=p.skinBumpLights[n];const auto position=localPoint(partWorld,light.position);
                    state.bumpVertexConstants[0]={{position.x,position.y,position.z,1}};
                    state.bumpVertexConstants[1]={{camera.x,camera.y,camera.z,1}};
                    state.bumpVertexConstants[2]={{1,0,9.0f/(2.0f*light.radius*light.radius),1}};
                    if(engineFragmentDispatch) {
                    state.fragmentConstants[0]={{p.material.engineSpecularColor[0]*p.material.engineBumpIntensity*light.color[0],p.material.engineSpecularColor[1]*p.material.engineBumpIntensity*light.color[1],p.material.engineSpecularColor[2]*p.material.engineBumpIntensity*light.color[2],1}};
                    state.fragmentConstants[1]={{mesh.ambient.x*light.color[0],mesh.ambient.y*light.color[1],mesh.ambient.z*light.color[2],1}};
                    }
                }else if(engineFragmentDispatch){state.fragmentConstants[0]={{0,0,0,1}};state.fragmentConstants[1]={{0,0,0,1}};}
                for(unsigned k=0;k<3;++k)constants[12+k]=state.bumpVertexConstants[k];
                if(!vp(p.material.engineNewSkinBump?skin_programs::Bump:skin_programs::OldBump)||!fp(skin_programs::BumpFragment))return false;
                draw();
            }
        }
        if(engineFragmentDispatch)state.finishFragmentCommand(13,p.material.engineGame==material::EngineMaterialGame::Kotor1);
        cleanup();blend(g::One,g::One);constants[15]={{1,1,1,1}};
        if(!vp(litSkin(base))||!bind(0,false,p.diffuseTexture,true))return false;
        x.TexEnvi(0x2300,0x2200,0x2100);a.Disable(Fog);a.Disable(g::AlphaTest);
        if(force&&!forceSetup())return false;
        draw();if(force) {
            if(engineFragmentDispatch)state.finishFragmentCommand(13,p.material.engineGame==material::EngineMaterialGame::Kotor1);
            cleanup();
        }
        a.Enable(g::AlphaTest);blend(g::SrcAlpha,g::OneMinusSrcAlpha);
        if(p.fog.mode!=model::FogMode::Disabled){
            if(!vp(skin_programs::Fog)||!fp(skin_programs::FogFragment))return false;
            draw();cleanup();
        }
    }
    if(p.material.engineGame==material::EngineMaterialGame::Kotor1) {
        state.alphaTestEnabled=a.IsEnabled(g::AlphaTest)!=0;
        a.GetFloatv(g::AlphaTestRef,&state.alphaReference);
    }
    const auto code=a.GetError();if(code!=g::NoError){error="Engine skin material submission failed ("+std::to_string(code)+")";return false;}
    return true;
}
void SkinMaterialPass::shutdown(){impl_->shutdown();}
} // namespace neomdl::render
