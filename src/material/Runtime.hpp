#pragma once
#include <neoshared/material/Material.hpp>
#include <string_view>

namespace neomdl::material {
// Queue classification remains viewer scene policy. PartTriMesh render-path
// selection and material state are represented separately below.
enum class RenderPass { Opaque, PunchThrough, Translucent, Additive };
enum class BlendFunction : std::uint8_t {
    Alpha,       // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
    Additive,    // GL_SRC_ALPHA, GL_ONE
    Replace,     // GL_ONE, GL_ZERO
};

// Exact numeric K1/K2 PartTriMesh::Draw dispatch values. K1 Desktop selects the
// non-VP paths; K2 normally selects the VP paths. The table makes the
// ABI explicit and keeps diagnostics unambiguous.
enum class TriMeshRenderPath : std::uint8_t {
    Flat = 0,
    FlatVP = 1,
    EMBM = 2,
    LightMappedBumpSpecular = 3,
    LightMappedBumpDiffuse = 4,
    BumpMapped = 5,
    BumpmapFallback = 6,
    EMLM = 7,
    EMLMVP = 8,
    NV1xEMLM = 9,
    EnvironmentMapped = 10,
    EnvironmentMappedVP = 11,
    LightMapped = 12,
    LightmappedVP = 13,
    LightMappedNoShaders = 14,
    NormalMapped = 15,
    NoiseMapped = 16,
};

const char* triMeshRenderPathName(TriMeshRenderPath path) noexcept;
bool triMeshRenderPathUsesLightmap(TriMeshRenderPath path) noexcept;
bool engineMaterialHasBumpMap(const neoshared::material::Material& material);
bool triMeshRenderPathUsesBumpMap(TriMeshRenderPath path) noexcept;
bool triMeshRenderPathUsesEnvironmentMap(TriMeshRenderPath path) noexcept;

struct SurfaceInputs {
    float opacity{1.0f}; // evaluated node alpha
    // PartTriMesh::Mat's owner/model-flag alpha multiplier (owner+0x108).
    // The ordinary Env/EMLM fragment constant does not consume this factor.
    // The standalone host has no gameplay producer and leaves it at one.
    float instanceOpacity{1.0f};
    // Desktop Material::SetMaterialColor applies this only when texture 0 reports
    // useglobalalpha. K1/K2 g_fGlobalAlpha (0x48cce8/0x57bd28) initializes to
    // one and has no engine writer. No automatic alpha producer is missing.
    float globalOpacity{1.0f};
    std::array<float, 3> selfIllumination{};
    bool vertexAlpha{};
    bool skinned{}; // PartSkin dispatch is independent of PartTriMesh::GetRenderPath
};

enum class EngineMaterialGame : std::uint8_t { Kotor1, Kotor2 };

// Engine existingextensions bits, not GL enum values or physical GPU brands.
// GLRender::InitExtensions also sets AtiFragmentShader for ArbFragmentProgram.
namespace k2Extension {
inline constexpr std::uint32_t Bgra=0x00000001u;
inline constexpr std::uint32_t Multitexture=0x00000002u;
inline constexpr std::uint32_t RegisterCombiners=0x00000008u;
inline constexpr std::uint32_t CubeMap=0x00000010u;
inline constexpr std::uint32_t NvVertexProgram=0x00000080u;
inline constexpr std::uint32_t NvTextureShader=0x00000400u;
inline constexpr std::uint32_t RenderToTexture=0x00040000u;
inline constexpr std::uint32_t AtiFragmentShader=0x00100000u;
inline constexpr std::uint32_t ArbVertexProgram=0x00200000u;
inline constexpr std::uint32_t ArbDepthTexture=0x20000000u;
inline constexpr std::uint32_t ArbFragmentProgram=0x40000000u;
}

// Captured engine initialization context. The initial extension mask describes
// NeoMDL's supported Desktop ARB profile; it is not a engine global default (the
// engine bitset starts empty) and is never inferred from the host GPU vendor.
// The integer settings retain the engine low-bit tests. A changed context is a
// new capability snapshot; engine cached predicates do not track live edits.
struct K2HardwareInputs {
    std::uint32_t existingExtensions{
        k2Extension::Bgra|k2Extension::Multitexture|k2Extension::CubeMap|
        k2Extension::RenderToTexture|k2Extension::AtiFragmentShader|
        k2Extension::ArbVertexProgram|k2Extension::ArbDepthTexture|
        k2Extension::ArbFragmentProgram};
    std::int32_t useAtiFragmentShaders{1};
    std::int32_t useArbVertexPrograms{1};
    std::int32_t renderLevel{2};
    std::int32_t unlockAtiSoftShadows{1};
};
struct K2HardwareSelection {
    bool arbFragmentPrograms{};
    bool atiValidation{};
    bool bumpMapCardValidation{};
    bool nonRadeon8500Validation{};
    bool multitexture{};
    bool cubeMap{}; // AurCubeMapAvailable requires cube-map AND multitexture bits
    bool vertexPrograms{};
    bool vertexProgramsNv{};
    bool bumpMapping{};
    bool bumpyShiny{};
    bool nv1xLevelShaders{};
    bool nv2xLevelShaders{};
    bool atiSoftShadows{};
    // Desktop AurFrameBufferModificationsAvailable0x1009d3 initializes its cached
    // result to false; AurCheck0x100c68 has the same sole write.
    bool frameBufferModifications{};
    bool atiFrameBufferModifications{};
};
// Relevant extension producer in desktop GLRender::InitExtensions.
std::uint32_t k2DesktopExtensionBits(std::string_view extensions) noexcept;
K2HardwareSelection selectK2HardwareCapabilities(const K2HardwareInputs&) noexcept;

struct K2PartTriMeshInputs {
    bool lightmapped{};             // authored MdlNodeTriMesh byte
    bool tangentBases{};            // HasTangentBases: MDX offset != 0xffffffff
    bool ownerEnvironmentMap{};     // Gob environment-map override
    bool enableLightmap{true};      // K2 enablelightmap global / viewer override
    bool enableBumpMap{true};       // K2 enablebumpmap global / viewer override
    bool nv2xLevelShaders{true};    // existing viewer enable, AND engine capability
    bool enableBumpyShiny{true};    // K2 enablebumpyshiny; independent of bump map
    K2HardwareInputs engineHardware;
};

struct K2PartTriMeshSelection {
    TriMeshRenderPath path{TriMeshRenderPath::FlatVP};
    float waterAlpha{1.0f};
    bool bumpMap{};
    bool normalMap{};
    bool bumpyShinyMap{};
    bool tangentBases{};
    bool environmentMap{};
    bool lightmap{};
};

// K2 PartTriMesh::GetRenderPath. Texture availability is
// derived from the resolved Material bindings; no graphics API is required.
K2PartTriMeshSelection selectK2PartTriMeshRenderPath(
    const neoshared::material::Material& material, const K2PartTriMeshInputs& inputs);
// Facts, capability arithmetic and the dispatch ABI are shared. Only the
// K1 branch tree differs from K2.
using EnginePartTriMeshInputs = K2PartTriMeshInputs;
using EnginePartTriMeshSelection = K2PartTriMeshSelection;
EnginePartTriMeshSelection selectEnginePartTriMeshRenderPath(
    const neoshared::material::Material&, const EnginePartTriMeshInputs&, EngineMaterialGame);

struct UvState {
    // For unsplit atlases: repeat locally before scale/offset, with half-texel
    // inset applied by the shader. Split NeoTPC layers must NOT be tiled twice.
    std::array<float, 2> scroll{};
    std::array<float, 2> scale{{1,1}};
    std::array<float, 2> offset{};
    std::array<float, 2> texel{};
    std::size_t frame{};
    std::size_t layer{};
    bool atlas{};
    bool clamp{};
};
struct RuntimeMaterial {
    RenderPass pass{RenderPass::Opaque};
    BlendFunction blendFunction{BlendFunction::Alpha};
    // Whether NeoMDL enables GL blending for this submission. The factor pair
    // below is the K2 material state; blend-enable ownership belongs
    // to the scene/bucket layer and remains viewer policy here.
    bool blending{};
    bool depthWrite{true};
    float opacity{1.0f};
    std::array<float, 3> selfIllumination{};
    float alphaCutoff{0.35f}; // K2 Material::SetBlendingMode reference; strict >
    bool alphaTest{};
    bool textureAlphaIsOpacity{true};
    bool k2PartTriMeshState{}; // compatibility name for the common engine state
    EngineMaterialGame engineGame{EngineMaterialGame::Kotor2};
    // Mat state before RenderBumpmapFallback overwrites its ordinary branch.
    BlendFunction engineMaterialBlendFunction{BlendFunction::Alpha};
    // PartTriMesh::Mat uploads env94 before Material::SetMaterialColor applies
    // texture0 useglobalalpha to fixed-function material diffuse alpha only.
    float vertexOpacity{1.0f};
    float environmentOpacity{1.0f}; // raw part alpha, before instance/global factors
    float engineWaterAlpha{1.0f}; // Material+72, independent EMBM opacity
    float instanceOpacity{1.0f};
    float fixedFunctionAlphaScale{1.0f};
    TriMeshRenderPath renderPath{TriMeshRenderPath::FlatVP};
    // Material facts retained for the independent RenderSkinnedVP dispatcher.
    bool skinBumpAvailable{};
    bool skinShinyAvailable{};
    bool skinEnvironmentAvailable{};
    bool engineMaterialEnvironmentAvailable{}; // material slot, excluding owner override
    bool engineSkinBumpTangent{}; // authored HasBumpMap && HasTangentBases
    bool engineNewSkinBump{true}; // usenewbumpyskin, initialized to 1
    bool engineMaterialEnableBumpyShiny{true}; // Mat's env93..95 upload gate
    bool engineSkinShinyPass{true}; // bumpyskinpass, initialized to 1
    bool engineAtiValidation{true}; // AurATIValidate, also true for engine ARB
    bool engineSkinBumpFallback{}; // GetSkinVP -> RenderFlat with ONE / ZERO
    K2HardwareSelection engineHardware{selectK2HardwareCapabilities({})};
    bool useLightmap{};
    bool useBumpMap{};
    bool useEnvironmentMap{};
    neoshared::material::BumpMapKind bumpKind{neoshared::material::BumpMapKind::None};
    float bumpScale{1.0f};
    // RenderSkinnedVP reads these from the bump texture itself, without the
    // preview height scaling, diffuse-texture fallback, or value clamps.
    float engineBumpIntensity{1.0f};
    bool engineDiffuseBump{}, engineSpecularBump{};
    float engineDiffuseBumpIntensity{}, engineSpecularBumpIntensity{};
    std::array<float,3> engineSpecularColor{{1,1,1}};
    bool diffuseBump{true};
    bool specularBump{};
    float diffuseBumpIntensity{1.0f};
    float specularBumpIntensity{1.0f};
    std::array<float,3> specularColor{{1,1,1}};
    float environmentStrength{1.0f};
    UvState diffuseUv, bumpUv;
};
UvState evaluateUv(const neoshared::material::TextureBinding& binding);
RuntimeMaterial evaluateMaterial(const neoshared::material::Material& material, const SurfaceInputs& surface,
                                 double seconds);
RuntimeMaterial evaluateK2PartTriMeshMaterial(
    const neoshared::material::Material& material, const SurfaceInputs& surface,
    const K2PartTriMeshInputs& inputs, double seconds);
RuntimeMaterial evaluateEnginePartTriMeshMaterial(
    const neoshared::material::Material&, const SurfaceInputs&, const EnginePartTriMeshInputs&,
    EngineMaterialGame, double seconds);
struct QueueKey {
    RenderPass pass{RenderPass::Opaque};
    float eyeDepth{};
    // Particle emitters owned by one game object use their signed authored
    // render order before the object's distance. Zero means no authored group.
    std::uint64_t authoredOrderGroup{};
    std::int32_t authoredOrder{};
};
// Scene-wide stable order: solid passes, then far-to-near alpha, then additive.
// Same-owner particle emitters use the native signed authored order.
// Cross-owner and mesh/particle interleaving remains the viewer scene policy.
std::vector<std::size_t> renderQueueOrder(const std::vector<QueueKey>& keys);
} // namespace neomdl::material
