#pragma once
#include "render/ShaderMeshRenderer.hpp"
#include "render/DesktopMaterialPrograms.hpp"
#include "core/GrassScene.hpp"
namespace neomdl::render {
// VertexProgram::Enable(nullptr,true) keeps env93 and light registers. Grass
// writes env94/95.
struct DesktopStandardVertexState {
    std::array<float,4> emission{};
    std::array<float,4> diffuse{},ambient{}; // retained env94/95, also written by grass
    std::array<PreviewLight,8> lights{};
    std::size_t lightCount{};
    // GL material emission excludes the scene-ambient term added to env93.
    std::array<float,4> fixedEmission{{0,0,0,1}};
};
void retainDesktopStandardVertexState(const model::Mesh&,const MeshDrawParameters&,
                                 MaterialProgram,DesktopStandardVertexState&) noexcept;
struct GrassDrawPlan {
    model::Mesh mesh;
    bool lightmapped{}, fixed{};
    unsigned programId{};
    std::array<float,4> emission{};
    const GrassGroup* group{};
};
std::vector<GrassDrawPlan> prepareGrassBin(const GrassBin&,const GrassSettings&,
    const EngineGrassRenderSettings&,const DesktopStandardVertexState&);
class GrassPass final {
public:
    void setLoader(const gl::Loader& loader) { loader_=loader; }
    bool draw(gl::Api&,const GrassScene&,const EngineGrassRenderSettings&,
        const GrassTextureBinding&,EngineSceneRasterState&,
        EngineMaterialState&,DesktopStandardVertexState&,
        std::uint64_t& drawCalls,std::string& error);
    void shutdown(gl::Api&) noexcept;
private:
    struct Uniforms {
        gl::Int mv{},mvp{},textureMatrix{},fixedPath{},lighting{},normalizeNormals{};
        gl::Int k1{},nvCombiners{},alphaTest{},lightmapped{},forceSight{},texture0{},texture1{};
        gl::Int diffuse{},ambient{},emission{},alphaCutoff{},desaturation{},lightCount{};
        gl::Int fogMode{},fogData{},fogColor{},fixedUv1Array{};
        std::array<gl::Int,8> lightPosition{},lightDiffuse{},lightAmbient{},lightAttenuation{};
        DesktopFixedVertexUniforms fixedInputs;
    } desktopUniforms_,desktopFixedUniforms_;
    bool prepareDesktop(gl::Api&,bool fixedFragment,std::string&);
    gl::Loader loader_;
    desktop_programs::LegacyApi legacy_;
    bool legacyReady_{};
    gl::UInt desktopProgram_{},desktopFixedProgram_{},vbo_{},ibo_{};
    bool k1StandardCreated_{};
};
} // namespace neomdl::render
