#pragma once
#include <cstdint>
#include <array>
#include <vector>
#include <model/Model.hpp>
#include <string>
#include <string_view>

namespace neomdl::render {
struct MeshDrawParameters;
namespace gl { struct Api; }
namespace desktop_programs { struct LegacyApi; }
struct EnginePartMaterialImage {
    std::array<float,4> ambient{{.2f,.2f,.2f,1}},diffuse{{.8f,.8f,.8f,1}},emission{{0,0,0,1}};
    std::array<std::array<float,4>,3> vertex{}; // env93,94,95, independently retained
    bool uploaded{};
};
// PartTriMesh::Mat's static cache. Scene::Render and DoBinBucket invalidate
// it; a changed scene ambient alone is not a material-cache comparison input.
struct EnginePartMaterialCache {
    std::array<float,3> lastDiffuse{{-1,-1,-1}},lastAmbient{{-1,-1,-1}},lastEmission{{-1,-1,-1}};
    float lastAlpha{-1};
    bool lastNeedsMaterial{true},materialHasChanged{true};
    EnginePartMaterialImage image;
};
void prepareEnginePartMaterial(const model::Mesh&,MeshDrawParameters&,EnginePartMaterialCache&);
// GL texture environment belongs to the engine context, including when an
// EnableFragmentShaders command deliberately leaves the fragment program off.
struct EngineFixedTextureEnvironment {
    unsigned mode{0x2100}, combineRgb{0x2100}, combineAlpha{0x2100};
    std::array<unsigned,3> rgbSources{{0x1702,0x8578,0x8576}},alphaSources{{0x1702,0x8578,0x8576}};
    std::array<unsigned,3> rgbOperands{{0x0300,0x0300,0x0302}},alphaOperands{{0x0302,0x0302,0x0302}};
    int rgbScale{1},alphaScale{1};
    std::array<float,4> color{};
};
struct EngineFixedTextureUnit {
    std::array<bool,4> enabled{}; // 2D, cube, 3D, 1D
    EngineFixedTextureEnvironment environment;
    std::array<bool,3> texgenEnabled{};
    std::array<unsigned,3> texgenMode{{0x2400,0x2400,0x2400}};
    std::array<float,4> currentCoordinate{{0,0,0,1}};
};
// Numeric values are local GLSL selectors, not engine dispatch indices.
enum class MaterialProgram : std::uint8_t {
    Preview, FlatVertex, Lightmap, Environment, EnvironmentLightmap, FlatFixed, SkinBump, SkinBumpyShiny, SkinFlat, SkinEnvironment, Dirty, K1Rigid
};
// Engine texture objects have one retained binding for EACH target at EACH
// engine texture unit. The host GL bindings are deliberately not this state:
// helpers and per-draw GL guards must not change the engine's material history.
class EngineMaterialState {
public:
    void reset() noexcept;
    void initializeNormalization(std::uint32_t cube) noexcept;
    void forgetTexture(std::uint32_t texture) noexcept;
    void bind(unsigned unit, bool cube, std::uint32_t texture);
    void bindVolume(unsigned unit,std::uint32_t texture);
    void bind1D(unsigned unit,std::uint32_t texture);
    std::uint32_t oneDBinding(unsigned unit) const;
    std::uint32_t volumeBinding(unsigned unit) const;
    void finishDirty(unsigned cleanupCommand);
    void finishFragmentCommand(unsigned command,bool kotor1=false);
    void finishFramebufferFragmentCommand();
    std::uint32_t binding(unsigned unit, bool cube) const;
    void setTextureMatrix(unsigned unit, const model::Mat4& matrix);
    const model::Mat4& textureMatrix(unsigned unit) const;
    void setTextureMatrixStackLimit(unsigned depth) noexcept { textureStackLimit_=depth; }
    void pushTextureMatrix(unsigned unit,const model::Mat4&);
    void popTextureMatrix(unsigned unit);
    unsigned fixedTextureMatrixError{}; // retained engine GL_STACK_OVER/UNDERFLOW

    bool normalizeNormals() const noexcept { return normalizeNormals_; }
    void setNormalizeNormals(bool enabled) noexcept { normalizeNormals_=enabled; }
    // Advances engine binding history and resolves the ACTUAL sampled target.
    // Part rotation is a scoped load-identity/rotate, not an accumulated matrix.
    void prepare(MeshDrawParameters& parameters, MaterialProgram program);
    // Selector retained by the existing Force Sight GLSL translation. The
    // complete engine program identity/enable image below is authoritative
    // for callers (including blur) that do not select a new fragment.
    unsigned forceSightFragmentCommand{};
    std::array<EngineFixedTextureUnit,6> fixedTextureUnits;
    unsigned fixedActiveTextureUnit{},fixedCurrentStage{};
    bool fixedEnvironmentMap{},fixedSecondTexture{};
    // Engine FP enable and bound identity are independent. Commands13/32/37/
    // 74/77 disable without unbinding; command0 also binds program zero.
    // Static shader source is the relocated executable identity, never host GL state.
    std::string_view fragmentProgram;
    bool fragmentProgramEnabled{};
    void bindFragmentProgram(std::string_view source) { fragmentProgram=source;fragmentProgramEnabled=true; }
    void disableFragmentProgram(bool unbind=false) { fragmentProgramEnabled=false;if(unbind)fragmentProgram={}; }
    // Engine SetFragmentShaderConstant and dispatcher writes use shared env
    // registers; executable ARB translations relocate those to private locals.
    std::array<std::array<float,4>,8> fragmentConstants{};
    std::array<std::array<float,4>,3> bumpVertexConstants{}; // env12..14 survive no-light draws
    std::array<EnginePartMaterialCache,2> partMaterials; // independent K1/K2 executable statics
    // Successful Desktop material exits captured before their host-state guard.
    // K1 has no late-frame alpha-reference reset before postprocess/flares.
    bool alphaTestEnabled{true};
    float alphaReference{};
    // Programs with unwritten primary outputs (e.g. SkinHolo) keep them
    // unwritten; rigid Holo and bumped shells have their own engine writes.

private:
    std::array<std::array<std::uint32_t,4>,6> bindings_{};
    std::array<model::Mat4,6> matrices_{{model::identityMatrix(),model::identityMatrix(),
        model::identityMatrix(),model::identityMatrix(),model::identityMatrix(),model::identityMatrix()}};
    std::array<std::vector<model::Mat4>,6> matrixStacks_;
    unsigned textureStackLimit_{};
    bool normalizeNormals_{};
};
// K2 Desktop non-ARB GLRender::EnableFragmentShaders0xf9204. The binding
// callback remains the engine virtual texture Bind producer (TXI lifetime).
bool applyDesktopFixedFragmentCommand(unsigned command,MeshDrawParameters&,EngineMaterialState&,std::string& error);
void applyDesktopFixedTextureState(gl::Api&,desktop_programs::LegacyApi&,const EngineMaterialState&);
struct DesktopFixedVertexUniforms {
    std::int32_t coordinates{},texgenEnabled{},texgenModes{},matrices{};
};
DesktopFixedVertexUniforms desktopFixedVertexUniformLocations(gl::Api&,std::uint32_t program);
void applyDesktopFixedVertexInputs(gl::Api&,const DesktopFixedVertexUniforms&,const EngineMaterialState&,
                               const model::Mat4* textureMatrix0=nullptr);
// GLRender::MakeNormalizationCubeMap: six 256-square RGB faces, encoded with
// trunc(128 + 127*unitComponent), not the preview normal encoding.
std::vector<std::uint8_t> normalizationCubeFace(unsigned face, unsigned size=256);
model::Mat4 engineTextureRotation(float degrees);

// Resolve ordinary PartTriMesh and PartSkin paths before touching GL state.
// Owner effects and specialized skin programs have separate consumers.
bool resolveMaterialProgram(const MeshDrawParameters& parameters,
                            MaterialProgram& program, std::string& error);
} // namespace neomdl::render
