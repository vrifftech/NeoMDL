#include "render/MaterialPass.hpp"
#include "render/ShaderMeshRenderer.hpp"
#include "render/DesktopMaterialPrograms.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace neomdl::render {
void prepareEnginePartMaterial(const model::Mesh& mesh,MeshDrawParameters& p,EnginePartMaterialCache& cache) {
    cache.image.uploaded=false;
    if(p.engineIgnoreMaterial) {p.enginePartMaterial=cache.image;return;}
    const std::array<float,3> white{{1,1,1}},zero{};
    const std::array<float,3> authoredDiffuse{{mesh.diffuse.x,mesh.diffuse.y,mesh.diffuse.z}};
    // StaticPartNeedsMat: authored ambient is deliberately absent from this
    // predicate. TileData only forces it when generated colors are inactive.
    // RuntimeMaterial already replaces decal self illumination with (1,1,1),
    // so the final nonzero test also covers the engine separate decal test.
    // Owner illumination can modify that value, but owners always need Mat.
    const bool needs=p.partHasOwner ||
        (!(p.enginePrelightPool&&p.enginePrelightPool->active)&&p.engineTileMode!=0) ||
        authoredDiffuse!=white || p.material.environmentOpacity!=1 || p.material.selfIllumination!=zero;
    if(!needs&&!cache.lastNeedsMaterial&&!p.engineAlwaysMaterial) {p.enginePartMaterial=cache.image;return;}
    auto diffuse=needs?(p.hasColorOverride?std::array<float,3>{{p.colorOverride[0],p.colorOverride[1],p.colorOverride[2]}}:authoredDiffuse):white;
    const std::array<float,3> ambient=needs?std::array<float,3>{{mesh.ambient.x,mesh.ambient.y,mesh.ambient.z}}:white;
    const auto emission=needs?p.material.selfIllumination:zero;
    const float alpha=p.partHasOwner?p.material.vertexOpacity:p.material.environmentOpacity;
    const bool differs=diffuse!=cache.lastDiffuse||ambient!=cache.lastAmbient||emission!=cache.lastEmission||alpha!=cache.lastAlpha;
    if(!(cache.lastNeedsMaterial||needs) ? !p.engineAlwaysMaterial :
       (!differs&&!cache.materialHasChanged&&!p.engineAlwaysMaterial)) {p.enginePartMaterial=cache.image;return;}
    for(unsigned c=0;c<3;++c) {
        cache.image.ambient[c]=ambient[c];cache.image.diffuse[c]=diffuse[c];cache.image.emission[c]=emission[c];
    }
    cache.image.ambient[3]=cache.image.emission[3]=1;
    cache.image.diffuse[3]=alpha*(p.partHasOwner?p.material.fixedFunctionAlphaScale:1.0f);
    if((p.engineSkinVertexPrograms||p.material.engineMaterialEnableBumpyShiny)&&p.material.engineHardware.vertexPrograms) {
        for(unsigned c=0;c<3;++c) {
            cache.image.vertex[0][c]=emission[c]+(p.partHasOwner?p.sceneAmbient[c]*ambient[c]:0.0f);
            cache.image.vertex[1][c]=diffuse[c];cache.image.vertex[2][c]=ambient[c];
        }
        cache.image.vertex[0][3]=cache.image.vertex[2][3]=1;cache.image.vertex[1][3]=alpha;
    }
    cache.lastDiffuse=diffuse;cache.lastAmbient=ambient;cache.lastEmission=emission;
    cache.lastAlpha=alpha;cache.lastNeedsMaterial=needs;cache.materialHasChanged=false;
    cache.image.uploaded=true;p.enginePartMaterial=cache.image;
}
bool resolveMaterialProgram(const MeshDrawParameters& p, MaterialProgram& program,
                            std::string& error) {
    program=MaterialProgram::Preview;
    error.clear();
    if(p.engineLightsaberMaterial) {
        program=MaterialProgram::FlatFixed;return true;
    }
    if(!p.material.k2PartTriMeshState || p.inspectMaterial || p.outlineOnly ||
       p.particlePass || p.lightsaberPass || p.engineFlareProgram!=EngineFlareProgram::None || p.engineShadowPass!=EngineShadowPass::None)
        return true;
    if(p.dirty.settings.enabled) {
        material::DirtyMaterialPlan plan;
        if(p.exactK2SkinVertexProgram||p.engineAnimMesh||p.skinEffect!=SkinRenderEffect::None||p.ownerFragment!=OwnerFragmentEffect::None) {
            error="Dirty diagnostic requires the rigid PartTriMesh caller, not skin/animmesh/owner-effect substitution";return false;
        }
        if(!material::makeDirtyMaterialPlan(p.dirty,p.material.useLightmap,p.material.useEnvironmentMap,plan,error,!p.material.engineHardware.arbFragmentPrograms))return false;
        if(!p.diffuseTexture||!p.dirty.texture||(plan.lightmap&&(!p.lightmapTexture||p.lightmapCube))||(plan.environment&&!p.environmentTexture)) {
            error="Dirty diagnostic is missing a required engine diffuse, volume, lightmap or environment resource";return false;
        }
        program=MaterialProgram::Dirty;return true;
    }
    if(p.engineAnimMesh && p.skinEffect==SkinRenderEffect::None) {
        program=MaterialProgram::FlatFixed;return true; // PartAnimMesh::Draw -> RenderFlat
    }
    if(p.engineCpuSkin && p.skinEffect==SkinRenderEffect::BumpedOutShell) {
        program=MaterialProgram::FlatFixed;return true; // CPU shell RenderFlat
    }
    if(p.ownerFragment!=OwnerFragmentEffect::None || p.skinEffect!=SkinRenderEffect::None) return true;
    if(p.engineCpuSkin) {
        if(p.material.engineGame==material::EngineMaterialGame::Kotor1) {
            program=MaterialProgram::K1Rigid;return true;
        }
        // ExecuteSkinTransforms 0x107340 selects the ordinary rigid
        // environment VP after producing its CPU pools; other skins use flat.
        program=p.material.skinEnvironmentAvailable
            ? MaterialProgram::Environment : MaterialProgram::FlatFixed;
    } else if(p.exactK2SkinVertexProgram) {
        if(p.material.skinNormalAvailable) {
            program=MaterialProgram::SkinNormal;return true;
        }
        // RenderSkinnedVP 0x1097e0 tests bumpy-shiny before the separate
        // BumpMapCardValidate / NV2x gates used by ordinary skin bump.
        if(p.material.skinShinyAvailable || p.material.skinBumpAvailable) {
            if(!p.bumpTexture || !p.diffuseTexture) {
                error="Engine skin-bump path requires its resolved bump and diffuse textures";return false;
            }
            program=p.material.skinShinyAvailable
                ? MaterialProgram::SkinBumpyShiny : MaterialProgram::SkinBump;
        } else if((p.material.skinEnvironmentAvailable || p.material.useEnvironmentMap) && p.environmentTexture) {
            program=MaterialProgram::SkinEnvironment;
        } else program=MaterialProgram::SkinFlat;
        return true;
    }
    if(!p.engineCpuSkin && p.material.renderPath==material::TriMeshRenderPath::NormalMapped) {
        program=MaterialProgram::NormalMapped;return true;
    }
    if(!p.engineCpuSkin && p.material.engineGame==material::EngineMaterialGame::Kotor1) {
        // K1 Desktop Draw dispatches the non-VP paths selected by GetRenderPath.
        // Their engine passes own the material commands and binding history.
        program=MaterialProgram::K1Rigid;
        return true;
    }
    using Path=material::TriMeshRenderPath;
    if(!p.engineCpuSkin)switch(p.material.renderPath) {
    case Path::FlatVP: program=MaterialProgram::FlatVertex;break;
    case Path::LightmappedVP: program=MaterialProgram::Lightmap;break;
    case Path::EnvironmentMappedVP: program=MaterialProgram::Environment;break;
    case Path::EMLMVP: program=MaterialProgram::EnvironmentLightmap;break;
    case Path::BumpmapFallback: program=MaterialProgram::FlatFixed;break;
    default:
        error=std::string("No translated ordinary material program for ")+
            material::triMeshRenderPathName(p.material.renderPath);
        return false;
    }
    const bool lm=program==MaterialProgram::Lightmap || program==MaterialProgram::EnvironmentLightmap;
    const bool env=program==MaterialProgram::Environment || program==MaterialProgram::EnvironmentLightmap;
    if((lm||env) && !p.diffuseTexture) {
        error="Engine ARB material requires its diffuse texture; no white-texture substitute";return false;
    }
    if(lm && (!p.lightmapTexture || p.lightmapCube)) {
        error="Engine ARB lightmap sampler requires a resolved 2D lightmap";return false;
    }
    if(env && !p.environmentTexture) {
        error="Engine ARB environment material has no resolved environment texture";return false;
    }
    return true;
}

void EngineMaterialState::reset() noexcept {
    fixedTextureUnits={};fixedActiveTextureUnit=fixedCurrentStage=0;
    fixedTextureMatrixError=0;for(auto& stack:matrixStacks_)stack.clear();
    fixedEnvironmentMap=fixedSecondTexture=false;
    fixedTextureUnits[0].enabled[0]=true; // SetupOpenGL0x4a0c9e enables2D0
    bindings_={};normalizeNormals_=false;fragmentConstants={};bumpVertexConstants={};
    partMaterials={};fragmentProgram={};fragmentProgramEnabled=false;
    alphaTestEnabled=true;alphaReference=0;forceSightFragmentCommand=0;
    for(auto& matrix:matrices_) matrix=model::identityMatrix();
}
void EngineMaterialState::initializeNormalization(std::uint32_t texture) noexcept {
    // Scene::Scene 0xbff71 -> AurTextureGetNormalizationCubeMap ->
    // AsNormalizeVectorCubeMap 0xbd628: enables NORMALIZE, binds cube on unit1.
    bindings_[1][1]=texture;normalizeNormals_=true;
}
void EngineMaterialState::forgetTexture(std::uint32_t texture) noexcept {
    if(!texture)return;
    for(auto& unit:bindings_)for(auto& bound:unit)if(bound==texture)bound=0;
}
void EngineMaterialState::bind(unsigned unit,bool cube,std::uint32_t texture) {
    bindings_.at(unit)[cube ? 1 : 0]=texture;
}
std::uint32_t EngineMaterialState::binding(unsigned unit,bool cube) const {
    return bindings_.at(unit)[cube ? 1 : 0];
}
void EngineMaterialState::bindVolume(unsigned unit,std::uint32_t texture) {bindings_.at(unit)[2]=texture;}
std::uint32_t EngineMaterialState::volumeBinding(unsigned unit) const {return bindings_.at(unit)[2];}
void EngineMaterialState::bind1D(unsigned unit,std::uint32_t texture) {bindings_.at(unit)[3]=texture;}
std::uint32_t EngineMaterialState::oneDBinding(unsigned unit) const {return bindings_.at(unit)[3];}
void EngineMaterialState::finishFragmentCommand(unsigned command,bool kotor1) {
    // GLRender::EnableFragmentShadersARB/ATI: program binding, enable,
    // per-target bindings and texture enables are independently retained.
    switch(command) {
    case 0:
        disableFragmentProgram(true);
        for(unsigned unit=0;unit<4;++unit)bind(unit,false,0);
        break;
    case 13:
        disableFragmentProgram();
        for(unsigned unit=1;unit<=3;++unit) {
            fixedTextureUnits[unit].enabled[0]=false;
            if(!kotor1||unit!=3)fixedTextureUnits[unit].enabled[1]=false;
        }
        break;
    case 32:
        disableFragmentProgram();
        if(kotor1) {
            if(fixedSecondTexture) {
                fixedTextureUnits[1].enabled[0]=false;
                if(fixedEnvironmentMap) {
                    fixedTextureUnits[1].enabled[1]=false;
                    fixedTextureUnits[1].texgenEnabled={};fixedEnvironmentMap=false;
                }
                fixedSecondTexture=false;
            }
        } else {
            for(unsigned unit=1;unit<=3;++unit) {
                fixedTextureUnits[unit].enabled[0]=fixedTextureUnits[unit].enabled[2]=false;
                bindVolume(unit,0);
            }
            if(fixedEnvironmentMap) {
                fixedTextureUnits[1].enabled[1]=fixedTextureUnits[2].enabled[1]=false;
                fixedTextureUnits[1].texgenEnabled={};
            }
            fixedEnvironmentMap=false;fixedSecondTexture=false;
        }
        break;
    case 37:
        disableFragmentProgram();
        for(unsigned unit=1;unit<=2;++unit)fixedTextureUnits[unit].enabled[0]=fixedTextureUnits[unit].enabled[1]=false;
        fixedTextureUnits[2].texgenEnabled={};
        break;
    case 74:
        disableFragmentProgram();
        if(fixedEnvironmentMap) {
            for(unsigned unit=1;unit<=2;++unit) {
                fixedTextureUnits[unit].enabled[1]=false;
                fixedTextureUnits[unit].texgenEnabled={};
            }
            fixedEnvironmentMap=false;
        }
        fixedSecondTexture=false;
        for(unsigned unit=1;unit<=3;++unit) {
            fixedTextureUnits[unit].enabled[0]=fixedTextureUnits[unit].enabled[2]=false;
            bind(unit,false,0);bindVolume(unit,0);
        }
        break;
    case 77:
        disableFragmentProgram();bind(1,false,0);fixedTextureUnits[1].enabled[0]=false;
        break;
    default:return;
    }
    forceSightFragmentCommand=0;fixedActiveTextureUnit=0;
}
void EngineMaterialState::finishDirty(unsigned cleanupCommand) {
    finishFragmentCommand(cleanupCommand);
}
void EngineMaterialState::finishFramebufferFragmentCommand() {
    // FrameBufferModificationPixelShaders command2: K1 ATI-text0x301c7f,
    // K2 ARB0x9b335. Unlike material command0, this disables texture targets
    // and clears1D1..3 as well. Cube1/2 and every3D binding are retained.
    disableFragmentProgram();
    for(unsigned unit=0;unit<4;++unit) {
        bind(unit,false,0);fixedTextureUnits[unit].enabled[0]=false;
        if(unit) {
            bind1D(unit,0);
            fixedTextureUnits[unit].enabled[1]=false;
            fixedTextureUnits[unit].enabled[3]=false;
        }
    }
    bind(3,true,0);fixedActiveTextureUnit=0;
}
void EngineMaterialState::setTextureMatrix(unsigned unit,const model::Mat4& matrix) {
    for(float x:matrix.values)if(!std::isfinite(x))throw std::invalid_argument("Non-finite engine texture matrix");
    matrices_.at(unit)=matrix;
}
const model::Mat4& EngineMaterialState::textureMatrix(unsigned unit) const {
    return matrices_.at(unit);
}
void EngineMaterialState::pushTextureMatrix(unsigned unit,const model::Mat4& matrix) {
    // Engine glPushMatrix preserves the current stack when its context limit
    // is reached; the following glLoadIdentity/glRotate still replace its top.
    auto& stack=matrixStacks_.at(unit);
    if(stack.size()+1u<textureStackLimit_)stack.push_back(matrices_.at(unit));
    else if(!fixedTextureMatrixError)fixedTextureMatrixError=0x0503;
    matrices_.at(unit)=matrix;
}
void EngineMaterialState::popTextureMatrix(unsigned unit) {
    auto& stack=matrixStacks_.at(unit);
    if(stack.empty()) {if(!fixedTextureMatrixError)fixedTextureMatrixError=0x0504;return;}
    matrices_.at(unit)=stack.back();stack.pop_back();
}
bool applyDesktopFixedFragmentCommand(unsigned command,MeshDrawParameters& p,EngineMaterialState& state,std::string& error) {
    const auto stage=[&](unsigned unit) {
        state.fixedCurrentStage=unit;
        if(p.material.engineHardware.multitexture)state.fixedActiveTextureUnit=unit;
    };
    const auto bind=[&](unsigned unit,bool cube,gl::UInt& texture) {
        stage(unit);
        if(p.rebindEngineTexture) {
            texture=p.rebindEngineTexture(texture,error);
            if(!texture)return false;
        }
        // CAurTexture::Bind0xbb67e -> AurTextureBindTID0xba979. Target
        // enables affect the active GL unit, even when multitexture is absent.
        auto& fixed=state.fixedTextureUnits[state.fixedActiveTextureUnit];
        if(p.material.engineHardware.cubeMap)fixed.enabled[1]=cube;
        fixed.enabled[2]=false;
        state.bind(state.fixedActiveTextureUnit,cube&&p.material.engineHardware.cubeMap,texture);
        return true;
    };
    const auto environment=[&](unsigned unit) {
        if(!bind(unit,p.environmentCube,p.environmentTexture))return false;
        auto& fixed=state.fixedTextureUnits[state.fixedActiveTextureUnit];
        if(!p.environmentCube)fixed.enabled[0]=true;
        for(unsigned n=0;n<(p.environmentCube?3u:2u);++n) {
            fixed.texgenEnabled[n]=true;fixed.texgenMode[n]=p.environmentCube?0x8512:0x2402;
        }
        return true;
    };
    const auto volume=[&](unsigned unit) {
        stage(unit);
        auto& fixed=state.fixedTextureUnits[state.fixedActiveTextureUnit];
        fixed.enabled[2]=true;
        if(p.rebindEngineTexture) {
            p.dirty.texture=p.rebindEngineTexture(p.dirty.texture,error);
            if(!p.dirty.texture)return false;
        }
        if(p.material.engineHardware.cubeMap)fixed.enabled[1]=false;
        state.bindVolume(state.fixedActiveTextureUnit,p.dirty.texture);return true;
    };
    switch(command) {
    case 50:case 56:case 92:case 93: // 0xf977a: unconditional glActiveTexture0
        state.fixedActiveTextureUnit=0;state.fixedTextureUnits[0].enabled[0]=true;break;
    case 51:case 57: // 0xf9cdf
        stage(0);state.fixedTextureUnits[state.fixedActiveTextureUnit].enabled[0]=true;
        if(!bind(0,false,p.diffuseTexture))return false;
        stage(1);state.fixedTextureUnits[state.fixedActiveTextureUnit].enabled[0]=true;
        if(!bind(1,p.lightmapCube,p.lightmapTexture))return false;
        state.fixedSecondTexture=true;break;
    case 52:case 58:case 94:case 95: // 0xf94f7
        state.fixedEnvironmentMap=state.fixedSecondTexture=true;
        if(!bind(0,false,p.diffuseTexture)||!environment(1))return false;
        break;
    case 53:case 59: // 0xf9f95/0xfa092: diffuse, environment2, lightmap1
        state.fixedEnvironmentMap=state.fixedSecondTexture=true;
        if(!bind(0,false,p.diffuseTexture)||!environment(2))return false;
        stage(1);state.fixedTextureUnits[state.fixedActiveTextureUnit].enabled[0]=true;
        if(!bind(1,p.lightmapCube,p.lightmapTexture))return false;
        break;
    case 62:case 63:case 64: // 0xf9244 ->0xf9392
        if(p.dirty.texture) {
            stage(0);state.fixedTextureUnits[state.fixedActiveTextureUnit].enabled[0]=true;
            if(!bind(0,false,p.diffuseTexture)||!volume(1))return false;
        }
        break;
    case 65:case 66:case 67: // 0xf9251
        state.fixedEnvironmentMap=state.fixedSecondTexture=true;
        if(!bind(0,false,p.diffuseTexture)||!environment(1)||!volume(2))return false;
        break;
    case 68:case 69:case 70: // 0xf92db
        stage(0);state.fixedTextureUnits[state.fixedActiveTextureUnit].enabled[0]=true;
        if(!bind(0,false,p.diffuseTexture))return false;
        stage(1);state.fixedTextureUnits[state.fixedActiveTextureUnit].enabled[0]=true;
        if(!bind(1,p.lightmapCube,p.lightmapTexture))return false;
        state.fixedSecondTexture=true;
        if(!volume(2))return false;
        break;
    case 71:case 72:case 73: // 0xf9b0d
        state.fixedEnvironmentMap=state.fixedSecondTexture=true;
        if(!bind(0,false,p.diffuseTexture)||!environment(2))return false;
        stage(1);state.fixedTextureUnits[state.fixedActiveTextureUnit].enabled[0]=true;
        if(!bind(1,p.lightmapCube,p.lightmapTexture)||!volume(3))return false;
        break;
    case 75:case 76: // 0xf9d5f: global hologram filter, no fragment program
        stage(1);state.fixedTextureUnits[state.fixedActiveTextureUnit].enabled[0]=true;
        if(!bind(1,false,p.hologramFilterTexture))return false;
        break;
    default:break; // 0/13 out of range; 32/74/77 table entries are no-ops.
    }
    return true;
}
void applyDesktopFixedTextureState(gl::Api& a,desktop_programs::LegacyApi& x,const EngineMaterialState& state) {
    constexpr gl::Enum targets[]={0x0de1,0x8513,0x806f,0x0de0};
    for(unsigned unit=0;unit<state.fixedTextureUnits.size();++unit) {
        a.ActiveTexture(gl::Texture0+unit);
        const auto& fixed=state.fixedTextureUnits[unit];
        const gl::UInt textures[]={state.binding(unit,false),state.binding(unit,true),state.volumeBinding(unit),state.oneDBinding(unit)};
        for(unsigned target=0;target<4;++target) {
            a.BindTexture(targets[target],textures[target]);
            if(fixed.enabled[target])a.Enable(targets[target]);else a.Disable(targets[target]);
        }
        const auto& env=fixed.environment;
        x.TexEnvi(0x2300,0x2200,env.mode);x.TexEnvfv(0x2300,0x2201,env.color.data());
        x.TexEnvi(0x2300,0x8571,env.combineRgb);x.TexEnvi(0x2300,0x8572,env.combineAlpha);
        x.TexEnvi(0x2300,0x8573,env.rgbScale);x.TexEnvi(0x2300,0x0d1c,env.alphaScale);
        for(unsigned n=0;n<3;++n) {
            x.TexEnvi(0x2300,0x8580+n,env.rgbSources[n]);x.TexEnvi(0x2300,0x8588+n,env.alphaSources[n]);
            x.TexEnvi(0x2300,0x8590+n,env.rgbOperands[n]);x.TexEnvi(0x2300,0x8598+n,env.alphaOperands[n]);
            x.TexGeni(0x2000+n,0x2500,fixed.texgenMode[n]);
            if(fixed.texgenEnabled[n])a.Enable(0x0c60+n);else a.Disable(0x0c60+n);
        }
    }
    a.ActiveTexture(gl::Texture0+state.fixedActiveTextureUnit);
}
DesktopFixedVertexUniforms desktopFixedVertexUniformLocations(gl::Api& a,std::uint32_t program) {
    return {a.GetUniformLocation(program,"uFixedCoordinates[0]"),
        a.GetUniformLocation(program,"uFixedTexgenEnabled[0]"),
        a.GetUniformLocation(program,"uFixedTexgenModes[0]"),
        a.GetUniformLocation(program,"uFixedMatrices[0]")};
}
void applyDesktopFixedVertexInputs(gl::Api& a,const DesktopFixedVertexUniforms& u,const EngineMaterialState& state,const model::Mat4* textureMatrix0) {
    std::array<float,24> coordinates{},texgenEnabled{},texgenModes{};
    std::array<float,96> matrices{};
    for(unsigned unit=0;unit<6;++unit) {
        const auto& fixed=state.fixedTextureUnits[unit];
        std::copy(fixed.currentCoordinate.begin(),fixed.currentCoordinate.end(),coordinates.begin()+unit*4);
        for(unsigned axis=0;axis<3;++axis) {
            texgenEnabled[unit*4+axis]=fixed.texgenEnabled[axis]?1.0f:0.0f;
            texgenModes[unit*4+axis]=static_cast<float>(fixed.texgenMode[axis]);
        }
        const auto& matrix=unit==0&&textureMatrix0?*textureMatrix0:state.textureMatrix(unit);
        std::copy(matrix.values.begin(),matrix.values.end(),matrices.begin()+unit*16);
    }
    a.Uniform4fv(u.coordinates,6,coordinates.data());
    a.Uniform4fv(u.texgenEnabled,6,texgenEnabled.data());
    a.Uniform4fv(u.texgenModes,6,texgenModes.data());
    a.UniformMatrix4fv(u.matrices,6,0,matrices.data());
}
model::Mat4 engineTextureRotation(float degrees) {
    if(!std::isfinite(degrees))throw std::invalid_argument("Non-finite engine texture angle");
    auto matrix=model::identityMatrix();
    const auto radians=static_cast<double>(degrees)*0.017453292519943295769;
    const float c=static_cast<float>(std::cos(radians)),s=static_cast<float>(std::sin(radians));
    matrix.values[0]=c;matrix.values[1]=s;matrix.values[4]=-s;matrix.values[5]=c;
    return matrix;
}
void EngineMaterialState::prepare(MeshDrawParameters& p,MaterialProgram program) {
    const bool legacyForce=p.material.engineGame==material::EngineMaterialGame::Kotor2 &&
        !p.material.engineHardware.arbFragmentPrograms && p.forceSight.enabled &&
        p.material.k2PartTriMeshState && !p.inspectMaterial && !p.outlineOnly;
    if(legacyForce && p.diffuseTexture) {
        // PartTriMesh::Mat0xdb216 -> Material::BindTexture0f32ae does not
        // activateunit0. A preceding legacy lightmap can retain serverunit1.
        bind(fixedActiveTextureUnit,false,p.diffuseTexture);
        auto& fixed=fixedTextureUnits[fixedActiveTextureUnit];
        if(p.material.engineHardware.cubeMap)fixed.enabled[1]=false;
        fixed.enabled[2]=false;
    }
    if(program==MaterialProgram::Dirty ||
       program==MaterialProgram::K1Rigid)return;
    if (p.engineFlareProgram!=EngineFlareProgram::None) {
        bind(0,false,p.diffuseTexture);
        if (p.engineFlareProgram==EngineFlareProgram::DesktopArb) fragmentConstants[1]={{0,0,0,0.75f}};
        return;
    }
    if(!p.material.k2PartTriMeshState || p.inspectMaterial || p.outlineOnly ||
       p.engineShadowPass!=EngineShadowPass::None)return;
    if(p.diffuseTexture && !legacyForce) {
        bind(0,false,p.diffuseTexture);
        if(p.material.engineHardware.cubeMap)fixedTextureUnits[0].enabled[1]=false;
        fixedTextureUnits[0].enabled[2]=false;
    }
    if(p.ownerFragment==OwnerFragmentEffect::Hologram) {
        bind(1,false,p.hologramFilterTexture);
        fixedTextureUnits[1].enabled[0]=true;
        if(p.material.engineHardware.cubeMap)fixedTextureUnits[1].enabled[1]=false;
        fixedTextureUnits[1].enabled[2]=false;
    }
    const bool force=p.forceSight.enabled && p.material.engineGame==material::EngineMaterialGame::Kotor2 && !p.engineAnimMesh;
    const bool engineFragmentDispatch=p.material.engineGame==material::EngineMaterialGame::Kotor1||p.material.engineHardware.arbFragmentPrograms;
    if(!force && engineFragmentDispatch && (program==MaterialProgram::Lightmap || program==MaterialProgram::EnvironmentLightmap)) {
        bind(1,p.lightmapCube,p.lightmapTexture);fixedSecondTexture=true;
        if(program==MaterialProgram::Lightmap)fixedTextureUnits[0].enabled[0]=true;
        fixedTextureUnits[1].enabled[0]=true;
        if(p.material.engineHardware.cubeMap)fixedTextureUnits[1].enabled[1]=p.lightmapCube;
        fixedTextureUnits[1].enabled[2]=false;
    }
    if(!force && engineFragmentDispatch && (program==MaterialProgram::Environment || program==MaterialProgram::SkinEnvironment || program==MaterialProgram::EnvironmentLightmap ||
       p.ownerFragment==OwnerFragmentEffect::IlluminationEnvironment)) {
        const unsigned unit=program==MaterialProgram::EnvironmentLightmap ? 2u : 1u;
        bind(unit,p.environmentCube,p.environmentTexture);fixedEnvironmentMap=fixedSecondTexture=true;
        if(p.material.engineHardware.cubeMap)fixedTextureUnits[unit].enabled[1]=p.environmentCube;
        fixedTextureUnits[unit].enabled[2]=false;
        if(!p.environmentCube)fixedTextureUnits[unit].enabled[0]=true;
        for(unsigned axis=0;axis<(p.environmentCube?3u:2u);++axis) {
            fixedTextureUnits[unit].texgenEnabled[axis]=true;
            fixedTextureUnits[unit].texgenMode[axis]=p.environmentCube?0x8512u:0x2402u;
        }
    }
    if(engineFragmentDispatch && (program==MaterialProgram::Environment || program==MaterialProgram::SkinEnvironment || program==MaterialProgram::EnvironmentLightmap))
        fragmentConstants[0]={{1,1,1,p.material.environmentOpacity}};
    normalizeNormals_=p.fixedFunctionNormalizeNormals.value_or(normalizeNormals_);
    p.fixedFunctionNormalizeNormals=normalizeNormals_;
    // K1 PartAnimMesh::Draw 0x34b46c / K2 0xd5df4 go straight to
    // RenderFlat; neither calls the TriMesh texture-matrix push/pop wrapper.
    p.fixedTextureMatrix=!p.engineAnimMesh && p.textureAngleDegrees!=0.0f
        ? engineTextureRotation(p.textureAngleDegrees) : matrices_[0];
    // TransformTextureMatrix pushes/replaces; RestoreTextureMatrix pops. The
    // retained matrix is not overwritten by a part's temporary rotation.
}
std::vector<std::uint8_t> normalizationCubeFace(unsigned face,unsigned size) {
    if(face>=6 || size==0 || size>256)throw std::invalid_argument("Invalid normalization cube face");
    std::vector<std::uint8_t> image(static_cast<std::size_t>(size)*size*3);
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
        const float u=2.0f*((static_cast<float>(x)+0.5f)/size)-1.0f;
        const float v=2.0f*((static_cast<float>(y)+0.5f)/size)-1.0f;
        std::array<float,3> n;
        switch(face) {
        case 0:n={1,-v,-u};break;case 1:n={-1,-v,u};break;
        case 2:n={u,1,v};break;case 3:n={u,-1,-v};break;
        case 4:n={u,-v,1};break;default:n={-u,-v,-1};break;
        }
        const float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        for(unsigned c=0;c<3;++c)image[(static_cast<std::size_t>(y)*size+x)*3+c]=
            static_cast<std::uint8_t>(128.0f+127.0f*(n[c]/length));
    }
    return image;
}
} // namespace neomdl::render
