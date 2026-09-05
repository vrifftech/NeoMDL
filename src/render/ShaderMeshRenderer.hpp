#pragma once
#include "render/GlApi.hpp"
#include "render/SoftShadowPass.hpp"
#include "render/MaterialPass.hpp"
#include <material/DirtyMaterial.hpp>
#include <material/ForceSight.hpp>
#include "render/SceneStageState.hpp"
#include "core/LensFlares.hpp"
#include "core/GrassScene.hpp"
#include "core/SceneOwnerLifetime.hpp"
#include "core/StaticPrelighting.hpp"
#include <model/Geometry.hpp>
#include <model/Emitter.hpp>
#include <model/Lighting.hpp>
#include <material/Runtime.hpp>

#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <functional>
#include <optional>

namespace neomdl::render {
struct PreviewLight {
    std::uint64_t id{};
    std::int32_t priority{};
    std::array<float,4> position{{0,0,1,0}}; // eye space; w=0 directional
    std::array<float,3> diffuse{{0.92f,0.92f,0.92f}};
    std::array<float,3> ambient{{0.22f,0.22f,0.25f}};
    std::array<float,3> attenuation{{1,0,0}};
};
enum class PrimitiveTopology : std::uint8_t {
    Triangles,
    QuadStrip,
    TriangleStrip,
    TriangleFan,
};

enum class EngineShadowPass : std::uint8_t {
    None, Increment, Decrement, TwoSidedDepthPass, Fade
};
// These are different engine algorithms, not interchangeable optimizations.
// TwoSidedDepthPass translates the Desktop EXT branch through separate GL state.
enum class EngineShadowStencilMode : std::uint8_t { TwoPassDepthFail, TwoSidedDepthPass };

// cSkinMotionBlurVP effect, applied to already-skinned positions.
enum class SkinRenderEffect : std::uint8_t {
    None,
    MotionBlur,
    BumpedOutShell, // raw (unnormalized) source/weighted skin normal displacement
};
enum class EngineFlareProgram : std::uint8_t { None, DesktopArb, FixedFunction };

enum class OwnerFragmentEffect : std::uint8_t {
    None, Hologram, Illumination, IlluminationEnvironment
};

// Raw GetBumpMapLights result. This is NOT a faded PreviewLight; the skin
// pass consumes authored RGB/radius and temporarily excludes this light's id
// from its additive ordinary-light pass (Desktop 0x10b6c3..0x10ca2b).
struct EngineBumpLight {
    std::uint64_t id{};
    model::Vec3 position{}; // world coordinates
    std::array<float,3> color{};
    float radius{};
};
struct MeshDrawParameters {
    // Separate generated RGBA pool owned by the runtime PartTriMesh. An
    // ordinary loaded room has none; authored MDX color is not this pool.
    const StaticPrelightPool* enginePrelightPool{};
    bool engineOnlyGob{}; // LightManager::m_pOnlyGob suppresses its color array
    std::int32_t engineTileMode{}; // explicit SpawnTile argument; ordinary rooms have no TileData
    bool engineIgnoreMaterial{},engineAlwaysMaterial{}; // engine ignoremat / alwaysmat globals
    std::optional<EnginePartMaterialImage> enginePartMaterial;
    material::ForceSightSettings forceSight;
    material::ForceSightOwnerState forceSightOwner;
    bool forceSightIllumination{};
    material::DirtyMaterialInputs dirty;
    neomdl::model::Mat4 modelView=neomdl::model::identityMatrix();
    neomdl::model::Mat4 projection=neomdl::model::identityMatrix();
    gl::UInt diffuseTexture{};
    // Stable per decoded texture rather than the context-assigned GL name.
    // K2 DoMeshBuckets follows CAurTextureBasic's ordered TID list.
    std::uint64_t diffuseTextureOrder{};
    gl::UInt lightmapTexture{};
    bool lightmapCube{};
    gl::UInt bumpTexture{};
    gl::UInt environmentTexture{};
    bool environmentCube{};
    // Rotation from eye coordinates back into the document scene, not the
    // mesh-local transform. Keeps environment reflections stable during orbit.
    std::array<float,9> eyeToWorld{{1,0,0,0,1,0,0,0,1}};
    neomdl::material::RuntimeMaterial material{};
    // Retained Gob door fade, applied after ordinary owner material state.
    // Carries the same value to a CPU shell without another Render update.
    float engineDoorAlpha{1.0f};
    // Set from Mesh::animated by the renderer, not a selectable material mode.
    // PartAnimMesh::Draw bypasses PartTriMesh::Draw/GetRenderPath/effect dispatch.
    std::array<float,4> engineFlatCurrentTexcoord{{0,0,0,1}}; // Desktop fixed current UV when array is disabled
    bool engineAnimMesh{};
    // PartSkin::Draw's outer usevertexprograms/NV2x/scene/NonRadeon8500 gate.
    // Distinct from AurVertexProgramsAvailable's extension query.
    bool engineSkinVertexPrograms{true};
    bool engineSkinShellVertexPrograms{true}; // DrawBumpedOut omits usevertexprograms
    bool engineCpuSkin{}; // selected internally from the engine outer gate
    std::uint32_t engineCpuSkinRestoreTexture{}; // CPU shell's original diffuse Bind

    OwnerFragmentEffect ownerFragment{OwnerFragmentEffect::None};
    gl::UInt hologramFilterTexture{};
    neomdl::model::Mat4 worldFromEye=neomdl::model::identityMatrix();
    // Part::GetPosAndOri input for rigid bump lights, independent of scale in
    // the drawing matrix. Defaults match the engine Part constructor.
    model::Vec3 enginePartPosition{};
    model::Quaternion enginePartOrientation{};
    std::array<float,3> ownerIllumination{{0.5f,0.5f,0.5f}};
    float shellAmount{};
    // Streamed CPU-generated geometry refreshes vertices AND indices when its
    // revision changes, including same-size color/UV/topology edits.
    bool streamedGeometry{};
    // Particle color passes share one queue position and never write depth.
    bool particlePass{};
    // K2 PartLightsaber::Draw always runs with blending enabled. Its default
    // path keeps depth writes and the existing SRC_ALPHA blend; the optional
    // blur-add branch switches to GL_ONE/GL_ONE and disables depth writes.
    bool lightsaberPass{};
    // K1 Desktop Gob::Render/PartDraw invokes Mat before PartLightsaber::Draw.
    // The prepared material supplies its blend/depth/cutoff inputs, while
    // alpha-test enable remains the enclosing scene's live value.
    bool engineLightsaberMaterial{};
    // Gob::RenderBlur re-traversal. Ordinary parts inherit the enclosing blur
    // stage's depth-write mask instead of their opaque queue classification.
    bool engineBlurTraversal{};
    bool engineDesktopFixedFragment{}; // verified non-ARB caller with no fragment program
    EngineFlareProgram engineFlareProgram{EngineFlareProgram::None};
    float flareDesaturation{0.65f}; // engine g_DesatureFactor initialization
    PrimitiveTopology primitive{PrimitiveTopology::Triangles};
    bool oneOneBlend{};
    EngineShadowPass engineShadowPass{EngineShadowPass::None};
    std::uint32_t shadowStencilThreshold{};
    // Explicit engine stage state; per-draw guards must not erase SetBeaming's
    // effect on later casters. These fields are internal to drawEngineShadows.
    bool shadowBeaming{};
    bool shadowBlendEnabled{};
    bool shadowStencilOff{};
    bool shadowSoftMask{};
    std::array<float,4> shadowBeamColor{{1,1,1,0.2f}};
    bool lighting{true};
    // Explicit viewer switches (e.g. textures disabled) are not engine material
    // execution. Do not silently use the preview shader for a engine failure.
    bool inspectMaterial{};
    // PartTriMesh::Mat applies Scene ambient and owner alpha factors only for
    // owner-backed parts; static room parts use the ownerless branch.
    bool partHasOwner{true};
    // Optional explicit engine NORMALIZE command. Unset uses the persistent
    // state established by Scene's normalization-cube initialization.
    std::optional<bool> fixedFunctionNormalizeNormals;
    // PartTriMesh::RotateTexture runtime degrees; never inferred from the
    // authored rotateTexture enable byte or from animation elapsed time.
    float textureAngleDegrees{};
    model::Mat4 fixedTextureMatrix=model::identityMatrix(); // resolved at draw
    std::array<float,3> sceneAmbient{{1.0f,1.0f,1.0f}};
    neomdl::model::FogSettings fog{};
    bool cullBackfaces{true};
    bool wireframe{};
    bool selected{};
    bool outlineOnly{}; // internal queue selection pass; no depth write
    int basisDebug{}; // 0: shaded; 1: tangent; 2: normal; 3: bitangent
    SkinRenderEffect skinEffect{SkinRenderEffect::None};
    // Selects the skin vertex-program fog-coordinate path even for
    // the ordinary (non-blur) skinned draw.
    bool exactK2SkinVertexProgram{};
    std::vector<EngineBumpLight> skinBumpLights;
    // Position/orientation only. The engine local-light conversion excludes
    // Gob scale. Canvas provides this from the owner, not a mesh-bound proxy.
    model::Mat4 skinOwnerFrame=model::identityMatrix();
    bool hasSkinOwnerFrame{};
    float skinOwnerAlpha{1.0f};
    // Texture Bind is an event, not just a GL name. Internal multipasses can
    // select the next Cycle TID after the preceding bind updated its controller.
    std::function<gl::UInt(gl::UInt,std::string&)> rebindEngineTexture;
    // AurTextureGetTID selects a controller frame without a Bind/Control event.
    std::function<gl::UInt(gl::UInt,std::string&)> queryEngineTexture;
    // Owning-Gob state, not authored model data: current minus previous position.
    neomdl::model::Vec3 skinMotion{};
    float skinMaximumBlurLength{1.5f};
    // Engine RenderSkinnedVPBlur constants are produced from the live Gob,
    // camera and DeltaT. The manual inspection pass supplies its own inputs.
    bool engineSkinBlur{},skinBlurMotionAdmitted{true},skinBlurClockwise{};
    neomdl::model::Vec3 skinBlurDirection{1.0f,0.0f,0.0f};
    float skinBlurSpeed{};
    float skinBlurSideScale{1.0f};
    neomdl::model::Vec3 skinCameraPosition{};
    neomdl::model::Vec3 skinBlurSideDirection{1.0f,0.0f,0.0f};
    // Motion blur needs a material color independent of authored mesh diffuse
    // without invoking uOverride,
    // which intentionally bypasses textures and all ordinary shading.
    bool hasColorOverride{};
    std::array<float,4> colorOverride{{1.0f,1.0f,1.0f,1.0f}};
    std::array<PreviewLight,8> lights{};
    std::size_t lightCount{1};
    // CPU skin shells consume fixed-function light slots independently of
    // the vertex-program light family. An unset value uses lights directly.
    std::optional<std::vector<PreviewLight>> fixedFunctionLights;
};
// Configure the native standard particle blend/depth/fog state.
// Call after assigning scene fog; framePass 1..4 are neighboring-frame draws.
void configureParticlePass(neomdl::model::EmitterBlend blend, unsigned framePass,
                           MeshDrawParameters& parameters);
struct MeshSubmission {
    // A real Gob traversal can have no drawable meshes. This explicit command
    // retains its bucket position, light selection and Render/history event.
    bool ownerOnly{};
    const neomdl::model::Mesh* mesh{};
    const neomdl::model::DeformedMesh* deformed{};
    std::uint64_t poseRevision{};
    MeshDrawParameters parameters;
    bool drawPrimary{true};
    // Part::Draw's second pass, not another sorted part or a shell bucket.
    std::optional<MeshDrawParameters> trailingShell;
    std::function<bool(MeshDrawParameters&,std::string&)> prepareTextures;
    float eyeDepth{};
    // Resolve engine light state AFTER bucket sorting, at the consuming draw.
    std::optional<std::uint64_t> lightOwner;
    std::size_t inspectionLightCount{};
    // K2 DoGobBuckets sorts complete owning Gobs, then Gob::Render retains the
    // submitted part order inside each owner. Zero means no owner.
    std::uint64_t sceneOwner{};
    float ownerAlpha{1.0f};
    std::uint16_t ownerModelClassification{};
    bool ownerHasTransparency{};
    float ownerCameraDistance{};
    // Nonzero only for particle emitters belonging to the same rendered game
    // object. The engine compares signed 16-bit authored renderOrder values.
    std::uint64_t authoredOrderGroup{};
    std::int32_t authoredOrder{};
    SceneOwnerLifetime ownerLifetime;
    // Preserve the bucket's admitted owner/order while reading its live pose
    // and transform at consumption, including a pooled or recycled chunk.
    std::function<void(MeshSubmission&)> resolveOwner;
    bool materialOnly{}; // PartDraw calls Mat before rigid Draw's alpha rejection
};
// Checks destruction before a callback can touch borrowed geometry/pose.
void resolveSceneSubmission(MeshSubmission& submission);
struct EngineShadowVolumeDraw {
    neomdl::model::Mesh geometry;
    neomdl::model::Mat4 modelView{neomdl::model::identityMatrix()};
    bool beginsBeaming{}; // processed even when geometry is empty
};
struct EngineShadowEffects {
    EngineSoftShadowMode softMode{EngineSoftShadowMode::Disabled};
    // SetBeaming: engine projectioncolor, projectionalpha and stenciloff.
    std::array<float,4> beamColor{{1,1,1,0.2f}};
    bool beamingStencilOff{true};
    float softDarkening{0.4f};
    neomdl::model::FogSettings fog; // Scene::DoFog retained by the non-ARB shadow caller
};
struct EngineFlareRenderSettings {
    model::GameVersion game{model::GameVersion::Kotor2};
    EngineFlareProgram program{EngineFlareProgram::DesktopArb};
    // Inherited caller state. K2 RenderSinglePass owns alpha function GREATER,0;
    // RenderLensFlares retains alpha enable.
    bool alphaTest{true};
    bool cullBackfaces{true};
    float desaturation{0.65f};
    material::K2HardwareSelection hardware{material::selectK2HardwareCapabilities({})};
};
using FlareTextureBinding = std::function<gl::UInt(const LensFlareQuad&, std::string&)>;
struct EngineGrassRenderSettings {
    model::Mat4 view{model::identityMatrix()}, projection{model::identityMatrix()};
    model::FogSettings fog;
    bool forceSight{}; // live K2 global; not grass-generation settings
    float restoredAlphaCutoff{}; // scene alphatest global, not incoming host GL
    float desaturation{.65f}; // Desktop env7.a
    model::GameVersion game{model::GameVersion::Kotor2};
    material::K2HardwareSelection hardware{material::selectK2HardwareCapabilities({})};
    // EnableLightsDynamicOnly runs before static meshes and grass. These are
    // that selection's fixed-function colors/attenuation in eye space; the
    // Standard VP instead consumes its separately retained register list.
    std::vector<PreviewLight> fixedLights;
};
using GrassTextureBinding=std::function<gl::UInt(const GrassBin&,const std::string&,unsigned,std::string&)>;
struct RendererStatistics {
    std::uint64_t staticUploads{};
    std::uint64_t dynamicUploads{};
    std::uint64_t engineSkinUploads{};
    std::uint64_t drawCalls{};
    std::size_t cachedMeshes{};
};
enum class QueueOrdering : std::uint8_t {
    Material,
    Submission,
    K2StaticMeshes,
    K2DynamicGobs,
    K1DynamicGobs,
    K2HologramGobs,
    StaticMeshes = K2StaticMeshes,
};
using EngineSceneStageCallback = std::function<bool(model::K2SceneStage,
    EngineSceneStageResult&,std::string&)>;
using SubmissionPreparation = std::function<bool(
    const MeshSubmission&, MeshDrawParameters&, std::string&)>;

// Called after an entire contiguous Gob group, including shells, before any
// viewer selection outlines. Indices follow actual engine bucket draw order.
using OwnerDrawCompletion = std::function<bool(const std::vector<MeshSubmission>&,
    const std::vector<std::size_t>&,std::string&)>;
// Called once for an admitted Gob before its first Part (including an
// owner-only marker), after ordering/animation/visibility and before material.
using OwnerDrawBegin = std::function<bool(const MeshSubmission&,std::string&)>;

class ShaderMeshRenderer final {
public:
    ShaderMeshRenderer();
    ~ShaderMeshRenderer();
    ShaderMeshRenderer(const ShaderMeshRenderer&)=delete;
    ShaderMeshRenderer& operator=(const ShaderMeshRenderer&)=delete;
    // All methods except construction/statistics/destruction require this
    // renderer's context to be current on the calling thread.
    bool initialize(const gl::Loader& resolver, std::string& error);
    bool draw(const neomdl::model::Mesh& mesh,
              const neomdl::model::DeformedMesh* deformed,
              std::uint64_t poseRevision, const MeshDrawParameters& parameters,
              std::string& error);
    bool consumeEnginePartMaterial(const model::Mesh&,const MeshDrawParameters&,std::string& error);
    // Non-streamed meshes are immutable while cached. Call this with the context current
    // before editing vertex/index values in-place or replacing a document.
    // Draw one scene bucket set. Selection outlines are deferred
    // until after that set's color passes and never write depth.
    // Controls transparency priority independently of Force Sight.
    void setGobNewSorting(bool) noexcept;
    void setForceSight(const material::ForceSightSettings&) noexcept;
    bool gobNewSorting() const noexcept;
    bool drawQueue(const std::vector<MeshSubmission>& submissions, std::string& error,
                   QueueOrdering ordering = QueueOrdering::Material,
                   const SubmissionPreparation& prepare = {},
                   const OwnerDrawCompletion& completedOwner = {},
                   const OwnerDrawBegin& beginOwner = {},
                   const OwnerDrawBegin& animateOwner = {});
    // Execute scene-stage boundaries independently of bucket contents.
    // The callback submits scene buckets, not UI overlays. Its results record
    // nonempty engine subcallers whose exit state differs from an empty call.
    // Engine state persists; the incoming application GL state is restored on
    // success, failure, and exceptions. Nested execution is rejected.
    bool drawEngineScene(const EngineSceneSettings&,const EngineSceneStageCallback&,std::string& error);
    bool drawEngineGrass(const GrassScene&,const EngineGrassRenderSettings&,
                         const GrassTextureBinding&,std::string& error);
    void invalidateSceneHistory() noexcept;
    // K1 PartEmitter::RenderFunc brackets its entire sprite/chunk traversal,
    // including an empty draw. These writes belong to the engine scene.
    void beginK1DesktopEmitter(bool distortion);
    void finishK1DesktopEmitter();
    // Engine stencil path with optional Desktop ARB image-space finish.
    // The host supplies ordered geometry/state events and the radial fade fan.
    // Captures and
    // restores front/back stencil, color masks and all ordinary mesh state.
    bool drawEngineShadows(const std::vector<EngineShadowVolumeDraw>& volumes,
                               const neomdl::model::Mesh& fadeFan,
                               const neomdl::model::Mat4& view,
                               const neomdl::model::Mat4& projection,
                               std::string& error,
                               EngineShadowStencilMode mode = EngineShadowStencilMode::TwoPassDepthFail,
                               const EngineShadowEffects& effects = {});
    // Inspector-only wireframe of a translated engine geometry entry point.
    // This neither changes stencil nor fabricates a engine scene-stage caller.
    bool drawShadowVolumeInspection(const std::vector<EngineShadowVolumeDraw>& volumes,
                                    const neomdl::model::Mat4& projection,
                                    std::string& error);
    // Scene::RenderLensFlares: original Desktop ARB/fixed consumers, including
    // program history and one texture Bind per element.
    bool drawEngineLensFlares(const std::vector<LensFlareQuad>& quads,
                             const model::Mat4& projection,
                             const FlareTextureBinding& bindTexture,
                             std::string& error,
                             const EngineFlareRenderSettings& settings = {});
    // Middle dialogue panel's AurGUIStartLayer/SetupViewport quad. The caller
    // supplies engine packed RGBA and the full canvas viewport after the scene.
    bool drawDialogueFade(std::uint32_t rgba,std::string& error);
    // Detach an allocation while its Mesh identity is still alive. This does
    // no GL work; physical buffer deletion waits for the current context.
    void retireGeometry(const neomdl::model::Mesh*);
    void flushRetiredGeometry();
    void clearGeometry();
    // Call on texture deletion before a GL name can be reused. Engine history
    // is independent of incidental host/context texture bindings.
    void forgetTexture(gl::UInt texture) noexcept;
    EngineMaterialState& materialState() noexcept;
    const EngineMaterialState& materialState() const noexcept;
    void shutdown();
    bool ready() const noexcept;
    RendererStatistics statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace neomdl::render
