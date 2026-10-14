#pragma once

#include <model/MaterialAnimation.hpp>
#include <model/EmitterState.hpp>
#include <model/SceneOrdering.hpp>
#include <neoshared/bif/JobControl.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
#include <utility>

namespace neomdl::model {

inline constexpr std::uint16_t kNodeBase = 0x0001;
inline constexpr std::uint16_t kNodeLight = 0x0002;
inline constexpr std::uint16_t kNodeEmitter = 0x0004;
inline constexpr std::uint16_t kNodeCamera = 0x0008;
inline constexpr std::uint16_t kNodeReference = 0x0010;
inline constexpr std::uint16_t kNodeMesh = 0x0020;
inline constexpr std::uint16_t kNodeSkin = 0x0040;
inline constexpr std::uint16_t kNodeAnimation = 0x0080;
inline constexpr std::uint16_t kNodeDangly = 0x0100;
inline constexpr std::uint16_t kNodeAabb = 0x0200;
inline constexpr std::uint16_t kNodeTrigger = 0x0400;
inline constexpr std::uint16_t kNodeLightsaber = 0x0800;

// MdlNodeAABB split masks used by PartAABB::HitCheckGeom. Equality on a
// segment component selects the negative direction, matching the K2 packed.
inline constexpr std::uint32_t kAabbPositiveX = 0x01u;
inline constexpr std::uint32_t kAabbPositiveY = 0x02u;
inline constexpr std::uint32_t kAabbPositiveZ = 0x04u;
inline constexpr std::uint32_t kAabbNegativeX = 0x08u;
inline constexpr std::uint32_t kAabbNegativeY = 0x10u;
inline constexpr std::uint32_t kAabbNegativeZ = 0x20u;
inline constexpr std::uint32_t kAabbDirectionMask = 0x3fu;

// Raw controller field identifiers used by the 32-bit packed MDL records.
// The 64-bit game ports translate these to pointer-sized runtime offsets.
inline constexpr std::uint32_t kControllerPosition = 8u;
inline constexpr std::uint32_t kControllerOrientation = 20u;
inline constexpr std::uint32_t kControllerScale = 36u;

// Flags accepted by the native animation event dispatcher.
inline constexpr std::uint32_t kAnimationEventReverse = 0x01u;
inline constexpr std::uint32_t kAnimationEventFullRange = 0x02u;

enum class GameVersion { Kotor1, Kotor2 };

struct Vec2 {
    float x{};
    float y{};
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Quaternion {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
};

// Runtime position/quaternion channels remain raw. Scale is the owner's
// effective Gob scale when this frame is supplied to enginePartWorld.
struct EngineFrame {
    Vec3 position{};
    Quaternion orientation{};
    float scale{1.0f};
};

// Column-major, consumable by OpenGL matrix uniforms without transposition.
struct Mat4 {
    std::array<float, 16> values{};
};

struct Bounds {
    Vec3 minimum{};
    Vec3 maximum{};
    bool valid{};
};

// On disk: bitangent, tangent, normal; three float3 vectors (36 bytes).
// Source vectors are retained verbatim. valid marks a finite, non-degenerate basis.
struct TangentBasis {
    Vec3 bitangent{};
    Vec3 tangent{};
    Vec3 normal{};
    bool valid{};
};

bool usableTangentBasis(const TangentBasis& basis) noexcept;

struct MeshUvAnimation {
    std::uint32_t enabledRaw{};
    Vec2 direction{};
    float jitter{};
    float jitterSpeed{};
};

struct K2MeshMaterialFields {
    std::uint8_t dirtEnabledRaw{};
    std::int16_t dirtTexture{};
    std::int16_t dirtWorldspace{};
    std::uint8_t hologramDoNotDrawRaw{};
};

struct Vertex {
    Vec3 position{};
    Vec3 normal{};
    Vec2 texcoord0{};
    Vec2 texcoord1{};
    Vec2 texcoord2{};
    Vec2 texcoord3{};
    std::array<std::uint8_t, 4> color{{255u, 255u, 255u, 255u}};
    TangentBasis tangentBasis{};
};

// K2 MaxFace is 32 bytes. The AABB hit path consumes all fields retained here:
// the authored plane, surface-material index, and three vertex indices.
struct MeshFace {
    Vec3 normal{};
    float planeDistance{};
    std::uint32_t surfaceMaterial{};
    std::array<std::int16_t, 3> adjacentFaces{{-1, -1, -1}};
    // PartAABB sign-extends these 16-bit fields before indexing the vertex array.
    std::array<std::int16_t, 3> indices{};
};

struct AabbTreeNode {
    Vec3 minimum{};
    Vec3 maximum{};
    std::int32_t firstChild{-1};
    std::int32_t secondChild{-1};
    std::int32_t faceIndex{-1};
    std::uint32_t splitMask{};
    std::uint32_t sourceOffset{};

    bool leaf() const noexcept { return faceIndex != -1; }
};

struct AabbNodeData {
    std::vector<AabbTreeNode> nodes;
    std::optional<std::size_t> root;
    bool valid{};
};

struct AabbHitOptions {
    // CHitInfo supplies a 32-bit material mask. A face is eligible when the
    // bit corresponding to its authored surface-material index is set.
    std::uint32_t surfaceMask{0xffffffffu};
    float scale{1.0f};
    // K2's aabb_testopposite debug global reverses the split-guided child order.
    bool testOpposite{};
};

struct AabbHit {
    Vec3 position{};
    Vec3 normal{};
    std::uint32_t surfaceMaterial{};
    std::uint32_t surfaceMask{};
    std::size_t faceIndex{};
    std::size_t treeNodeIndex{};
    std::size_t intersectionCount{};
    float segmentFraction{};
};

enum class ControllerValueKind : std::uint8_t {
    Unknown = 0u,
    Float = 1u,
    PackedQuaternion = 2u,
    Vector = 3u,
    Quaternion = 4u,
};

enum class ControllerInterpolation : std::uint8_t {
    Linear = 0u,
    Bezier = 1u,
    Unknown = 0xffu,
};

struct Controller {
    std::uint32_t type{};
    std::int16_t sourceOffset{};
    std::uint16_t rowCount{};
    std::uint16_t timeIndex{};
    std::uint16_t dataIndex{};
    std::uint8_t rawFlags{};
    ControllerValueKind valueKind{ControllerValueKind::Unknown};
    ControllerInterpolation interpolation{ControllerInterpolation::Unknown};
};

// Shared engine packed-key decoder used by playback and ASCII reassembly.
Quaternion unpackEngineControllerQuaternion(std::uint32_t packed, GameVersion game) noexcept;

struct SkinInfluence {
    std::array<float, 4> weights{};
    std::array<std::uint16_t, 4> boneIndices{};
};

// K1/K2 MdlNodeDanglyMesh32 append this authored block to MdlNodeTriMesh32.
// InternalGenVertices copies the source constraint verbatim onto each unique
// generated runtime vertex. PartDanglyMesh::Animate uses those raw values; it
// does not normalize the common 0..255 authoring range.
struct DanglyMeshData {
    // K1 MdlNodeDanglyMesh +0x1b4 / K2 +0x1bc: independent MDL rest-position pool, not
    // the MDX drawing stream. Present-but-empty means a parsed missing pool;
    // nullopt permits explicitly constructed host meshes to use their vertices.
    std::optional<std::vector<Vec3>> restPositions;
    std::vector<float> constraints;
    float displacement{};
    float tightness{};
    float period{};
};

// Defaults follow the K2 PartLight constructors. Authored finite
// values are retained; rendering clamps only its own display inputs.
struct NodeLightState {
    std::array<float,3> color{};
    float radius{};
    float shadowRadius{-1.0f};
    float verticalDisplacement{};
    float multiplier{1.0f};
    std::array<MaterialValueSource,5> sources{};
};

struct LightNodeData {
    float flareRadius{};
    std::vector<std::string> textureNames;
    std::vector<float> flareSizes;
    std::vector<float> flarePositions;
    std::vector<Vec3> flareColorShifts;
    // K2 stores an integer dynamic-light classification rather than a single
    // boolean. dynamic is retained as a convenience for callers.
    std::int32_t dynamicType{};
    // These authored flags are engine signed integer fields. Keep their
    // complete values through ASCII parsing and packed serialization; runtime
    // predicates still interpret nonzero as enabled.
    std::int32_t ambientOnly{};
    std::int32_t castsShadow{};
    bool dynamic{};
    std::int32_t affectDynamic{};
    std::int32_t priority{};
    std::int32_t generateFlare{};
    std::int32_t fading{};
};

struct EmitterNodeData {
    float deadSpace{};
    float blastRadius{};
    float blastLength{};
    std::uint32_t branchCount{};
    std::int32_t controlPointSmoothing{};
    std::uint32_t xGrid{};
    std::uint32_t yGrid{};
    std::uint32_t spawnType{};
    std::string updateMode;
    std::string renderMode;
    std::string blendMode;
    std::string texture;
    std::string chunkName;
    std::int32_t twoSidedTexture{};
    std::int32_t loop{};
    std::uint16_t renderOrder{};
    bool frameBlending{};
    std::string depthTexture;
    std::array<std::uint8_t,32> asciiUpdateMode{},asciiRenderMode{},asciiBlendMode{},
        asciiTexture{},asciiDepthTexture{};
    std::array<std::uint8_t,16> asciiChunkName{};
    // The packed record contains additional engine-specific flags and values.
    // Keeping the complete 224-byte extension as words makes diagnostics and
    // later controller/particle parity work possible without unsafe aliasing.
    std::array<std::uint32_t, 56> rawWords{};
};

struct CameraNodeData {
    // Camera nodes have no additional authored K2 packed payload. Their base
    // node transform defines the preview camera glyph.
};

struct TriggerNodeData {
    // The K1 and K2 implementations have a base-node-only MdlNodeTrigger:
    // InternalParseField is empty and InternalCreateInstance returns null.
    // Runtime PartTrigger volumes are created separately from game-object
    // footprint points through AurPartTriggerCreate.
    bool packedVerified{};
    bool hasPrivatePayload{};
    bool createsRuntimePart{};
};

struct ReferenceNodeData {
    std::string model;
    std::array<std::uint8_t,32> asciiModel{};
    std::int32_t reattachable{};
};

struct LightsaberNodeData {
    static constexpr std::uint16_t frontSegments = 20u;
    static constexpr std::uint16_t backSegments = 20u;
    static constexpr std::uint16_t pieceVertices = 8u;
    static constexpr std::uint16_t segmentVertices = 4u;
    static constexpr std::size_t frontPieceOffset = 0u;
    static constexpr std::size_t frontSegmentOffset = pieceVertices;
    static constexpr std::size_t backPieceOffset =
        frontSegmentOffset + static_cast<std::size_t>(frontSegments) * segmentVertices;
    static constexpr std::size_t backSegmentOffset = backPieceOffset + pieceVertices;
    static constexpr std::size_t generatedVertexCount =
        backSegmentOffset + static_cast<std::size_t>(backSegments) * segmentVertices;

    // K1/K2 MdlNodeLightsaber::InternalPostProcess builds four parallel arrays.
    // positions/normals/texcoords contain the generated 176-vertex stream;
    // originalPositions retains the independent CPU baseline: count+1 rows
    // during ASCII construction, exactly count stored rows after packed load.
    std::vector<Vec3> positions;
    std::vector<Vec3> originalPositions;
    std::vector<Vec3> normals;
    std::vector<Vec2> texcoords;
    bool postprocessed{};
};

// MdlNodeTriMesh's authored query bounds/sphere, independent of MDX draw data.
// HitCheckGeom consumes these bytes verbatim; it does not rebuild animated bounds.
struct EngineMeshBounds {
    Bounds box{};
    float sphereRadius{};
    Vec3 sphereCenter{};
};

// Shared K1/K2 GLRender primitive IDs, not OpenGL enumerants. Animmesh retains
// engine batch boundaries/order independently of the CPU face/query stream.
struct EngineMeshDraw {
    std::uint32_t primitive{3u}; // 3 triangles, 4 strip, 5 fan; table 0x5431f0
    std::size_t firstIndex{};
    std::size_t indexCount{};
};




struct EngineMeshSource {
    std::shared_ptr<const std::vector<std::uint8_t>> mdl,mdx;
    std::size_t nodeOffset{},mdxDataOffset{},mdxVertexOffset{};
    GameVersion game{GameVersion::Kotor2};
};
using EngineCpuSkinInput = EngineMeshSource;
struct EnginePrelightVertex {
    Vec3 position{},normal{};
    std::array<std::uint8_t,4> color{};
};
struct EnginePrelightSource {
    std::vector<EnginePrelightVertex> vertices;
    bool authoredColors{};
};
struct EngineCpuSkinSource {
    std::vector<Vec3> positions, normals;
    std::vector<std::array<float,4>> weights, stripBoneIndices;
    std::vector<std::array<std::uint16_t,4>> triangleBoneIndices;
    std::vector<std::uint32_t> stripVertices, triangleVertices;
    std::vector<std::uint32_t> indices;
    std::vector<EngineMeshDraw> draws;
    // K1 uses unsigned float-to-integer conversion; K2's CPU
    // branch uses signed float-to-integer conversion.
    bool unsignedStripBoneIndices{};
};

struct EngineFloatSkinChannels {
    std::array<float,4> weights,indices;
};

struct AsciiTangentSource {
    std::vector<std::size_t> authoredFaceIndices;
    std::vector<std::uint32_t> faceSmoothing;
    std::vector<Vec3> firstCornerNormals;
    std::vector<std::array<std::array<double,2>,3>> faceTexcoords64;
    // CalculateTangentSpaceBasis reads the full temporary NodeVertex list,
    // after the signed-short count has limited the published MDX pool.
    std::vector<Vec3> generatedPositions;
    std::vector<Vec2> generatedTexcoords;
};

struct AsciiNormalPositionSource {
    std::vector<std::array<double,3>> positions;
    std::vector<std::array<std::uint32_t,3>> faces;
    std::vector<std::uint32_t> faceSmoothing;
    std::vector<std::size_t> authoredFaceIndices;
};

// ParseNode/endnode allocations belong to the Model, not its current root.
// Replacing a root leaves earlier static MDX rows allocated. Keep
// their generated counts and authored temporary-material inputs even when
// the corresponding nodes no longer belong to the loaded tree.
struct AsciiMdxAllocation {
    std::uint16_t vertexCount{};
    std::uint32_t rowSize{};
    std::string primaryTexture;
    bool materialTangents{};
    std::shared_ptr<struct Mesh> source;
};

struct AsciiSkinSource {
    std::vector<float> boneToPalette;
    std::array<std::int16_t,17> paletteToBone{};
    std::vector<Quaternion> inverseOrientations;
    std::vector<Vec3> inverseTranslations;
    std::size_t boneConstantCount{};
};

struct ProceduralUvPool;
struct AsciiNormalPool {
    std::vector<Vec3> normals;
    Vec3 sentinel{};
    std::uint64_t revision{};
};
struct Mesh {
    std::size_t nodeIndex{};
    std::string texture0;
    std::string texture1;
    std::array<std::uint8_t,32> asciiTexture0{},asciiTexture1{};
    Vec3 diffuse{1.0f, 1.0f, 1.0f};
    Vec3 ambient{0.2f, 0.2f, 0.2f};
    std::uint32_t transparencyHint{};
    std::uint32_t mdxBitmap{};
    std::uint32_t mdxRowSize{};
    std::uint32_t mdxColorOffset{0xffffffffu};
    std::array<std::uint32_t, 4> mdxTexcoordOffsets{{
        0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}};
    std::uint32_t mdxTangentOffset{0xffffffffu};
    std::size_t validTangentVertices{};
    std::shared_ptr<const AsciiTangentSource> asciiTangentSource;
    // Unset follows the material; an authored value controls the first basis.
    std::optional<bool> asciiTangents;
    std::shared_ptr<const AsciiNormalPositionSource> asciiNormalPositionSource;
    std::optional<std::size_t> asciiMdxAllocation;
    std::shared_ptr<const AsciiSkinSource> asciiSkinSource;
    // Final authored normals bypass smoothing and seam averaging.
    bool explicitNormals{};
    bool hasVertexColors{};
    MeshUvAnimation uvAnimation{};
    // Model-owned MDX UV channel. Procedural Part updates write this shared
    // pool; copied views retain its identity, as engine instances do.
    std::shared_ptr<ProceduralUvPool> proceduralUvPool;
    std::uint16_t textureCount{};
    bool rotateTexture{};
    bool beaming{};
    float totalArea{};
    std::optional<K2MeshMaterialFields> k2Material;
    // Full common mesh extension, including unknown words and padding. Kept
    // for inspection; it is not a lossless MDL/MDX serialization API.
    std::vector<std::uint8_t> rawMeshHeader;
    std::optional<EngineMeshBounds> engineBounds;
    std::uint32_t mdxWeightOffset{0xffffffffu};
    std::uint32_t mdxBoneIndexOffset{0xffffffffu};
    bool lightmapped{};
    bool castsShadow{};
    bool backgroundGeometry{};
    bool render{true};
    bool skinned{};
    bool animated{}; // engine PartAnimMesh dispatch, not a rigid preview
    std::vector<EngineMeshDraw> engineDraws; // empty means no engine indexed draw
    // BuildVertexIndices' live pool-handle list grows from 16 by doubling;
    // zero-vertex serialization retains it before payload-list fixups.
    std::uint32_t engineIndexPoolCapacity{};
    // K1 engine draw pools, independent of the face stream used by queries.
    std::vector<std::uint32_t> engineDrawIndices;
    std::vector<std::int32_t> engineIndexCounts;
    // Duplicate-index face removal decrements the engine list count without
    // shrinking its allocation. The writer retains this capacity when the
    // resulting face list is empty; nonempty lists are normalized to count.
    std::uint32_t engineFaceCapacity{};
    // Counted authoring pointer arrays survive paths which skip
    // BuildVertexIndices. The writer clears their pointer but retains this
    // count/capacity and includes their table in its zero-filled allocation.
    std::uint32_t engineRawIndexArrayCount{};
    std::vector<std::uint32_t> engineLeftoverFaces;
    std::uint32_t mdxNormalOffset{0xffffffffu};
    std::shared_ptr<const EngineCpuSkinInput> engineCpuSkin;
    std::shared_ptr<const EngineCpuSkinSource> builtCpuSkinSource;
    std::shared_ptr<const std::vector<EngineFloatSkinChannels>> builtFloatSkinChannels;
    // LightPartTriMesh reads the original MDX stream, independently of the
    // inspection mesh's repaired normals, face-derived count and bitmap.
    std::shared_ptr<const EngineMeshSource> enginePrelightInput;
    // ASCII construction supplies the same generated vertex pool directly;
    // packed models retain the serialized stream above.
    std::shared_ptr<const EnginePrelightSource> builtPrelightSource;
    // NewCAurObject smooths the model-owned normal channel for every owner.
    // Views share these writes, including generated row N when an authored
    // index batch references that extra interleaved-pool record.
    std::shared_ptr<AsciiNormalPool> asciiNormalPool;
    std::uint16_t enginePrelightVertexCount{};
    bool hasSkinChannels{};
    bool hasSkinBoneMap{};
    bool hasInverseBindPose{};
    std::optional<DanglyMeshData> dangly;
    std::array<std::int16_t, 17> skinBoneMap{};
    std::size_t skinBoneMapSize{17u};
    std::vector<SkinInfluence> skinInfluences;
    // MdlNodeSkin32 stores model-space inverse-bind transforms as separate
    // quaternion and translation arrays. Quaternions are converted from the
    // engine's w,x,y,z storage order to this API's x,y,z,w order.
    std::vector<Quaternion> inverseBindOrientations;
    std::vector<Vec3> inverseBindTranslations;
    // Parsed for diagnostics and future parity work. The K2 renderer
    // does not consume this array in the normal RenderSkinned path.
    std::vector<std::int32_t> boneConstantIndices;
    std::vector<MeshFace> faces;
    // K2 MdlNodeTriMesh +0x1a0, consumed by CPU shadow projection. Preserve
    // independently of the preferred MDX rendering stream. Parsed malformed
    // pools remain present-but-empty so projection cannot silently substitute.
    std::optional<std::vector<Vec3>> cpuPositions;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    const Vec3& normalAt(std::size_t index) const noexcept {
        return asciiNormalPool ? (index==enginePrelightVertexCount
            ? asciiNormalPool->sentinel : asciiNormalPool->normals[index]) : vertices[index].normal;
    }
    std::uint64_t normalRevision() const noexcept {
        return asciiNormalPool ? asciiNormalPool->revision : 0u;
    }
};

// MdlNodeAnimMesh32 follows the 332-byte K1 / 340-byte K2 mesh extension.
// Final pools are vertex-major, NOT frame-major. Temporary compiler arrays
// and their pointers are retained as bytes, but are not runtime sample pools.
struct AnimMeshData {
    float samplePeriod{};
    std::uint16_t vertexCount{};
    std::int32_t positionSampleCount{};
    std::int32_t texcoordSampleCount{};
    std::vector<Vec3> positionSamples;
    std::vector<Vec2> texcoordSamples;
    // Generated NW authoring tangent xyz/handedness; KOTOR's Flat animmesh
    // draw does not consume this as its unrelated nine-float tangent basis.
    std::vector<std::array<float, 4>> staticTangents;
    std::array<std::uint8_t, 56> rawHeader{};
    // Typed animation nodes own their mesh extension and static geometry even
    // though they are not drawable entries in the model's geometry mesh list.
    std::shared_ptr<const Mesh> authoringMesh;
};

// Per PartAnimMesh instance, never stored on the shared Model. Construction
// allocates the pools without initializing them; CreateInstanceTreeR then
// controls the base node at time 0, weight 1. A channel with no samples stays
// undefined until a full-weight control writes it (there is no MDX fallback).
struct AnimMeshState {
    std::vector<Vec3> localPositions;
    std::vector<Vec2> texcoords;
    bool positionsDefined{};
    bool texcoordsDefined{};
};

struct Node {
    // Absolute offset in the original packed MDL, including its wrapper.
    std::optional<std::size_t> packedOffset;
    std::uint16_t flags{};
    std::uint16_t id{};
    std::uint32_t engineNameIndex{};
    std::string name;
    std::optional<std::size_t> parent;
    Vec3 position{};
    Quaternion orientation{};
    std::vector<std::size_t> children;
    std::optional<std::size_t> mesh;
    std::optional<LightNodeData> light;
    std::optional<EmitterNodeData> emitter;
    std::optional<CameraNodeData> camera;
    std::optional<TriggerNodeData> trigger;
    std::optional<ReferenceNodeData> reference;
    std::optional<LightsaberNodeData> lightsaber;
    std::optional<AabbNodeData> aabb;
    std::optional<AnimMeshData> animmesh;
    std::vector<Controller> controllers;
    // Controller values are retained as raw 32-bit words because compact
    // quaternion keys occupy the same array but are not IEEE-754 floats.
    std::vector<std::uint32_t> controllerDataWords;
};

struct AnimationEvent {
    float time{};
    std::string name;
};

struct Animation {
    std::string name;
    std::uint32_t engineNodeCount{};
    float length{};
    float transitionTime{};
    std::string root;
    std::array<std::uint8_t,32> asciiRoot{};
    std::vector<Node> nodes;
    std::vector<AnimationEvent> events;
};

// A resolved clip in a derived-model -> supermodel hierarchy. modelIndex is an
// index into the hierarchy supplied to findAnimation()/enumerateAnimations().
// positionScale is the product of animationScale values on the descendants
// traversed before the clip owner, matching FindAnimScale in the K2 executables.
struct AnimationReference {
    std::size_t modelIndex{};
    std::size_t animationIndex{};
    std::size_t inheritanceDepth{};
    float positionScale{1.0f};
    bool defaultFallback{};
};

struct AnimationEventOccurrence {
    std::size_t eventIndex{};
    float time{};
    std::string name;
};

// Diagnostics from K2 MdlNode::ProcessSkinSeams. The routine compares every
// vertex of a skin mesh only with vertices in later skin meshes, using an
// inclusive per-axis 0.001 model-unit tolerance. Normals are averaged in
// model space and written back in each mesh's local orientation.
struct SkinSeamStatistics {
    bool processed{};
    std::size_t skinMeshes{};
    std::size_t candidateComparisons{};
    std::size_t matchedGroups{};
    std::size_t adjustedVertices{};
};

struct AsciiLoadSession;

struct Model {
    GameVersion game{GameVersion::Kotor1};
    bool asciiBuilt{};
    bool advancedAscii{};
    bool asciiMaterialsFinalized{};
    std::uint32_t engineNodeCount{};
    // Initial packed header count used by the ASCII load registry's engine
    // superclass lifetime. Canonical export still clears this process state.
    std::uint32_t engineReferenceCount{};
    // ParseNode appends model names even for discarded roots. Animation
    // nodes reuse the last case-insensitive match in this shared table.
    std::vector<std::string> engineNames;
    std::uint32_t engineMdxPoolBytes{};
    std::vector<AsciiMdxAllocation> asciiMdxAllocations;
    std::optional<std::size_t> animationRootNode;
    Bounds engineBounds{};
    float engineRadius{};
    std::string name;
    std::string supermodel;
    std::string engineSupermodelName;
    std::array<std::uint8_t,32> asciiSupermodelName{};
    // ASCII FindModel selects registered objects, which may share a name or
    // exist only in the same source file. Preserve those exact superclass
    // edges for the document handoff instead of resolving the names again.
    // Null means no retained load result (ordinary packed loading); an
    // empty retained chain means the ASCII lookup resolved no superclass.
    // Entries are flat snapshots and do not own nested chain handles.
    std::shared_ptr<const std::vector<Model>> asciiSupermodels;
    // Outward snapshots can retain their live load registry. Registered
    // models themselves never own this handle, so superclass/registry edges
    // cannot form shared_ptr cycles. IDs identify registrations, not names.
    std::shared_ptr<AsciiLoadSession> asciiLoadSession;
    std::uint64_t asciiRegistrationId{};
    std::uint16_t classification{};
    std::optional<bool> compressQuaternions;
    std::vector<std::pair<std::string,bool>> textureTangents;
    std::uint8_t subclassification{};
    bool fog{};
    float animationScale{1.0f};
    std::uint32_t declaredAnimationCount{};
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    // Runtime Part trees contain one node and mesh per child-list occurrence,
    // in depth-first creation order. These maps retain each occurrence's
    // canonical model-node/mesh identity; animation IDs and model-owned
    // normal/procedural-UV pools remain shared. Decoded/ASCII-built source
    // models leave the maps empty and are retained separately for export.
    std::vector<std::size_t> partSourceNodes;
    std::vector<std::size_t> partSourceMeshes;
    // Model-level producers and serializers use the original shared resource,
    // while runtime Part fields use the occurrence vectors above.
    std::shared_ptr<const Model> partSourceModel;
    std::vector<Animation> animations;
    std::vector<Mat4> worldTransforms;
    Bounds bounds{};
    mutable SkinSeamStatistics skinSeams{};
    std::vector<std::string> warnings;
    // Scene::CollapsePartTree runs AFTER model/animation SynchronizeTree.
    // A room's retained Gob therefore keeps the original animation-to-Part
    // binding even when modelname+"a" is reparented directly below its root.
    // Scene views retain original node indices (also used by skin palettes),
    // an immutable binding source, and an admission mask; decoded assets leave
    // these empty. Neither field is packed MDL data.
    std::shared_ptr<const Model> animationSynchronizationModel;
    std::vector<std::uint8_t> sceneNodeAdmission;
    bool collapsedRoomParts{}; // ownerless forest, not a multi-root Gob
};

// One engine vertex-program palette slot. RenderSkinned uploads three consecutive
// vec4 constants beginning at c[18 + slot*3]. Rows are retained explicitly so
// CPU deformation and diagnostics use the same compact-palette representation
// as the vertex programs.
struct EngineSkinPaletteSlot {
    std::int16_t modelNode{-1};
    std::array<float, 12> rows{};
    bool valid{};
};
using K2SkinPaletteSlot = EngineSkinPaletteSlot;

struct DeformedMesh {
    bool valid{};
    // Persistent local sample pools plus the engine part draw frame. positions
    // and normals below remain model-space for inspection/bounds. Engine draws
    // use localPositions and authored normals with this frame, preserving the
    // platform's normal-matrix/normalization behavior rather than baking it.
    std::optional<AnimMeshState> animmesh;
    Mat4 animMeshPartFrame{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    // PartDanglyMesh streams only positions; its authored normal/tangent
    // channels remain static and are transformed with the current part basis.
    Mat4 danglyPartFrame{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    // K2 skin deformation preserves the authored three-vector MDX tangent
    // basis and deforms each vector with the same weighted bone orientations
    // used by cNewSkinBumpyVP. Missing/invalid rows retain the generated viewer
    // fallback on a per-vertex basis.
    bool preserveTangentBasis{};
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    // cSkinBumpedOutVP displaces with the raw weighted ordinary normal before
    // cSkinVP normalizes it for lighting. Preserve both representations.
    std::vector<Vec3> rawSkinNormals;
    std::vector<Vec3> tangents;
    std::vector<Vec3> bitangents;
    std::vector<Vec3> basisNormals;
    std::vector<std::uint8_t> exactTangentBasis;
    // RenderSkinnedVP builds a part-local palette, then uses the part's draw
    // matrix. Keep the scale-free part frame alongside the model-space CPU
    // palette so the engine consumer can recover those coordinates exactly.
    Mat4 skinPartFrame{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    std::array<EngineSkinPaletteSlot, 17> skinPalette{};
    std::size_t skinPaletteSize{};
    std::size_t rejectedInfluences{};
};

// Semantic counterpart of K1/K2 PartDanglyMesh plus VertexPrimitiveDangly.
// The runtime implementations store the previous part
// position/orientation on PartDanglyMesh, generated local positions on the
// part, and one persistent local velocity per vertex on VertexPrimitiveDangly.
struct DanglyMeshState {
    bool initialized{};
    Vec3 previousPartPosition{};
    Quaternion previousPartOrientation{};
    std::vector<Vec3> localPositions;
    std::vector<Vec3> localVelocities;
};

struct DanglySimulationState {
    std::vector<DanglyMeshState> meshes;
};

// World inputs supplied by Scene::GetWindVector and the owning CAurObject.
// windDisplacement returns a displacement/impulse for the clamped frame time,
// not an acceleration. The owning object's frame is kept in engine channels;
// reconstructing its quaternion from a draw matrix loses engine state.
struct DanglyEnvironment {
    EngineFrame objectFrame{};
    std::function<Vec3(Vec3, float)> windDisplacement;
};

struct Pose {
    float time{};
    // When Animation::root names a valid animation node, this is the matching
    // target-model node. Only that node and its descendants receive animation
    // controllers; the engine does not extract its translation into a separate
    // actor-motion channel in the paths observed here.
    std::optional<std::size_t> controlledRootNode;
    std::vector<Vec3> positions;
    std::vector<Quaternion> orientations;
    std::vector<float> scales;
    // Same indexing as Model::nodes, including defaults for non-mesh nodes.
    std::vector<NodeMaterialState> nodeMaterials;
    std::vector<NodeLightState> nodeLights;
    // Copies of an owner's pose retain its live PartEmitter fields. Particle
    // updates and weighted animation controllers write the same engine state;
    // bindPose constructs independent fields for a newly created owner.
    std::vector<std::shared_ptr<NodeEmitterState>> nodeEmitters;
    std::vector<Mat4> worldTransforms;
    // Same indexing as Model::meshes. A valid entry contains model-space CPU
    // skinning output and must not receive the mesh-node transform a second time.
    std::vector<DeformedMesh> deformedMeshes;
    Bounds bounds{};
};

class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A engine executable already fixes the game whose ASCII construction rules
// it uses. A standalone source file has no corresponding packed layout token.
class AsciiModelGameRequired : public ModelError {
public:
    AsciiModelGameRequired() : ModelError("Choose K1 or K2 for ASCII MDL construction") {}
};

bool isAsciiModelInput(const std::vector<std::uint8_t>& mdlBytes) noexcept;

Model decodePackedModelBytes(const std::vector<std::uint8_t>& mdlBytes,
    const std::vector<std::uint8_t>& mdxBytes, const std::string& sourceLabel,
    const neoshared::bif::JobControl& job = {}, bool includeAnimations = true);

using AsciiResourceResolver = std::function<std::optional<std::vector<std::uint8_t>>(
    const std::string& resourceName, const std::string& extension)>;

// Geometry-only reads skip packed animation decoding. ASCII still follows its
// construction commands; its resulting own-animation list is then omitted.
Model decodeModelBytes(const std::vector<std::uint8_t>& mdlBytes,
                       const std::vector<std::uint8_t>& mdxBytes = {},
                       const std::string& sourceLabel = {},
                       const neoshared::bif::JobControl& job = {},
                       std::optional<GameVersion> asciiGame = std::nullopt,
                       const AsciiResourceResolver& asciiResources = {},
                       bool includeAnimations = true);

Model readModelFile(const std::filesystem::path& mdlPath,
                    const std::optional<std::filesystem::path>& mdxPath = std::nullopt,
                    const neoshared::bif::JobControl& job = {},
                    std::optional<GameVersion> asciiGame = std::nullopt,
                    const AsciiResourceResolver& asciiResources = {},
                    bool includeAnimations = true);

// Material-finalized source Model -> engine CreateInstanceTreeR topology.
// Every child link creates a distinct live Part, including repeated links to
// the same source node. The input is the canonical source, not a scene view or
// an already instantiated Part tree. Serialization uses the retained source.
Model instantiateModelParts(std::shared_ptr<const Model> source,
                             const neoshared::bif::JobControl& job = {});

Mat4 identityMatrix() noexcept;
Mat4 multiply(const Mat4& left, const Mat4& right) noexcept;
Mat4 nodeTransform(const Vec3& position, const Quaternion& orientation) noexcept;
Mat4 nodeTransform(const Vec3& position, const Quaternion& orientation, float uniformScale) noexcept;
Vec3 transformPoint(const Mat4& matrix, const Vec3& point) noexcept;
Vec3 normalize(const Vec3& value) noexcept;
Vec3 transformDirection(const Mat4& matrix, const Vec3& direction) noexcept;
inline Vec3 deformedNormalAt(const Mesh& mesh,const DeformedMesh& pose,std::size_t index) noexcept {
    // Dangly SetupArrays binds the shared MDX normal channel at draw time;
    // a later owner's seam writes do not require another simulation tick.
    return mesh.dangly ? transformDirection(pose.danglyPartFrame,mesh.normalAt(index))
                       : pose.normals[index];
}
// Engine quaternion operations do not normalize. Inverse is the engine's
// unary-minus operation (negate w), and Rotate uses the executables' enabled
// quaternionidentityoptimization default for w == +/-1.
Quaternion engineQuaternionProduct(const Quaternion&, const Quaternion&) noexcept;
Quaternion engineQuaternionInverse(Quaternion) noexcept;
Vec3 engineQuaternionRotate(const Quaternion&, const Vec3&) noexcept;
// Part::GetWorld walks from the selected part towards its root, then applies
// the owner's scale, quaternion and position. Its output has no scale channel:
// the returned frame's scale is 1, not an accumulated model or owner scale.
EngineFrame enginePartWorld(const Model&, const Pose&, const EngineFrame& owner,
                           std::size_t nodeIndex);
// CreateInstanceTreeR's initial base-model ControlDispatch(-1,0,1), starting
// from Part's zero position/identity orientation. This precedes ProcessGob's
// constructed sphere; it is not necessarily the serialized node header frame.
void constructedNodeFrame(const Node&,Vec3& position,Quaternion& orientation,
    GameVersion) noexcept;

Pose bindPose(const Model& model, const neoshared::bif::JobControl& job = {});
// Live Part construction evaluates the base controllers from zero/identity,
// including scale. Keep bindPose's authored-header inspection view separate.
Pose constructedBindPose(const Model& model,
                        const neoshared::bif::JobControl& job = {});

// Explicit seam-processing helper retained for hosts constructing geometry.
// Packed loading preserves compiled normals: engine ProcessSkinSeams returns
// immediately for the packed loader's geometry-type 0x80 flag.
SkinSeamStatistics processK2SkinSeams(Model& model,
                                    const neoshared::bif::JobControl& job = {});

// Reproduces PartAABB::HitCheckGeom in node-local coordinates. The segment is
// directed: only front-to-back crossings of authored face planes are accepted.
// Traversal shortens the endpoint after every accepted leaf and returns the
// nearest surviving hit together with the number of accepted intersections.
std::optional<AabbHit> hitTestAabb(const Model& model, std::size_t nodeIndex,
                                   const Vec3& start, const Vec3& end,
                                   const AabbHitOptions& options = {});

// Reproduces the desktop PartDanglyMesh::Animate implementations,
// retaining each mesh's constructed local position and velocity pools.
DanglySimulationState makeDanglySimulationState(const Model& model, const Pose& pose);
void resetDanglySimulation(const Model& model, const Pose& pose,
                           DanglySimulationState& state);
void simulateDanglyMeshes(const Model& model, Pose& pose,
                          DanglySimulationState& state, float elapsedSeconds,
                          const DanglyEnvironment& environment);

// Compatibility/viewer convenience: treats windVelocity as a constant world
// wind and converts it to the displacement returned by Scene::GetWindVector.
// The downward default preserves NeoMDL's existing standalone preview policy;
// callers with a scene wind manager should use DanglyEnvironment instead.
void simulateDanglyMeshes(const Model& model, Pose& pose,
                          DanglySimulationState& state, float elapsedSeconds,
                          Vec3 windVelocity = {0.0f, 0.0f, -9.80665f});

// hierarchy[0] is the most-derived model. Lookup checks local animations first,
// then each supermodel, and finally the terminal model's "default" animation.
std::optional<AnimationReference> findAnimation(
    const std::vector<const Model*>& hierarchy, const std::string& name);

// Returns the visible animation list with case-insensitive descendant overrides
// removed. References retain the inherited position-scale product.
std::vector<AnimationReference> enumerateAnimations(
    const std::vector<const Model*>& hierarchy);

// Reproduces AnimateEvents interval and wrap behavior. Forward intervals are
// (startTime, endTime]; reverse intervals are [endTime, startTime). Events are
// returned in the same order in which the engine dispatches them.
std::vector<AnimationEventOccurrence> animationEventsBetween(
    const Animation& animation, float startTime, float endTime,
    std::uint32_t flags = 0u);

Pose evaluateAnimation(const Model& model, std::size_t animationIndex, float time,
                       bool loop = true, const neoshared::bif::JobControl& job = {});
Pose evaluateAnimation(const Model& targetModel, const Model& animationModel,
                       std::size_t animationIndex, float time,
                       float positionScale, bool loop = true,
                       const neoshared::bif::JobControl& job = {});
std::string gameVersionName(GameVersion game);
std::string nodeFlagsText(std::uint16_t flags);
std::string controllerTypeName(std::uint32_t type);
std::string modelSummary(const Model& model);

} // namespace neomdl::model
