#include "render/DirtyMaterialPass.hpp"
#include "render/ShaderMeshRenderer.hpp"
#include "render/EngineDesktopFragmentPrograms.hpp"
#include <stdexcept>
namespace neomdl::render {
bool prepareDirtyBindings(gl::Api& a,MeshDrawParameters& p,const material::DirtyMaterialPlan& plan,
                          EngineMaterialState& state,std::string& error) {
    try {
        if(plan.fixedDispatch) {
            if(!applyDesktopFixedFragmentCommand(plan.command,p,state,error))return false;
            const auto unit=state.fixedActiveTextureUnit;
            a.ActiveTexture(gl::Texture0+unit);a.BindTexture(0x806f,state.volumeBinding(unit));
            // Shared legacy dispatcher tail0xf9f01: repeat S/T/R then linear
            // minification/magnification for every admitted Dirty formula.
            for(const auto parameter:{0x2802u,0x2803u,0x8072u})a.TexParameteri(0x806f,parameter,0x2901);
            a.TexParameteri(0x806f,0x2801,0x2601);a.TexParameteri(0x806f,0x2800,0x2601);
            return true;
        }
        if(!plan.environment)state.fixedTextureUnits[0].enabled[0]=true;
        if(plan.lightmap) {
            state.fixedSecondTexture=true;state.fixedTextureUnits[1].enabled[0]=true;
        }
        if(plan.environment)state.fixedEnvironmentMap=state.fixedSecondTexture=true;
        for(const auto& event:plan.binds) {
            const auto role=event.role;
            gl::UInt* id=role==material::DirtyTextureRole::Diffuse?&p.diffuseTexture:
                role==material::DirtyTextureRole::Environment?&p.environmentTexture:
                role==material::DirtyTextureRole::Lightmap?&p.lightmapTexture:&p.dirty.texture;
            const auto value=p.rebindEngineTexture?p.rebindEngineTexture(*id,error):*id;
            if(!error.empty()||!value) {if(error.empty())error="Dirty caller texture Bind returned no engine texture";return false;}
            *id=value;
            if(role==material::DirtyTextureRole::Volume) {
                state.bindVolume(event.unit,value);state.fixedTextureUnits[event.unit].enabled[2]=true;
                if(p.material.engineHardware.cubeMap)state.fixedTextureUnits[event.unit].enabled[1]=false;
                a.ActiveTexture(gl::Texture0+event.unit);a.BindTexture(0x806f,value);
                a.TexParameteri(0x806f,0x2802,0x2901);a.TexParameteri(0x806f,0x2803,0x2901);a.TexParameteri(0x806f,0x8072,0x2901);
                if(plan.forceLinear) {a.TexParameteri(0x806f,0x2801,0x2601);a.TexParameteri(0x806f,0x2800,0x2601);}
            } else {
                const bool cube=role==material::DirtyTextureRole::Environment&&p.environmentCube;
                state.bind(event.unit,cube,value);
                auto& target=state.fixedTextureUnits[event.unit];
                if(p.material.engineHardware.cubeMap)target.enabled[1]=cube;
                target.enabled[2]=false;
                if(role==material::DirtyTextureRole::Environment) {
                    if(!cube)target.enabled[0]=true;
                    for(unsigned axis=0;axis<(cube?3u:2u);++axis) {
                        target.texgenEnabled[axis]=true;
                        target.texgenMode[axis]=cube?0x8512u:0x2402u;
                    }
                }
            }
        }
        // The EMLM FP is hard-coded to a 2D environment sampler, even when
        // the dispatcher just virtually bound an environment CUBEMAP.
        if(plan.environment) {p.environmentTexture=state.binding(2,false);p.environmentCube=false;}
        // Command68 binds a 3D image, but its original string says 2D. It must
        // consume the retained OTHER target, never a fabricated volume slice.
        if(!plan.samplesVolume)p.bumpTexture=state.binding(2,false);
        if(plan.desaturate&&p.material.engineHardware.arbFragmentPrograms) {
            state.fragmentConstants[1]={{0,0,0,p.dirty.settings.forceAlpha}};
            state.fragmentConstants[7]={{.3f,.59f,.11f,p.dirty.settings.desaturation}};
        }
        if(p.material.engineHardware.arbFragmentPrograms)
            if(const auto* fragment=desktop_programs::k2FragmentProgram(plan.command))state.bindFragmentProgram(fragment);
        a.ActiveTexture(gl::Texture0);
        return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
} // namespace neomdl::render
