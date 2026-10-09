#pragma once
#include <model/Model.hpp>
#include <model/LightningControl.hpp>
#include <model/Wind.hpp>
#include <functional>

namespace neomdl::model {
inline constexpr unsigned kParticleRuntimeVersion=13;
NodeEmitterState bindNodeEmitter(const Node& node,bool lightningBranch=false) noexcept;

enum class EmitterBlend { Normal, PunchThrough, Additive };
enum class EmitterUpdate { Fountain, Single, Explosion, Lightning };
// Scene::DoEmitterBucket clamps float32 elapsed. K1 Desktop's MINSS operand
// order selects the cap for NaN; K2 MINSS propagates it.
inline float engineEmitterBucketElapsed(float elapsed,GameVersion game) noexcept {
    return game==GameVersion::Kotor1
        ? (elapsed<0.1f?elapsed:0.1f) : (elapsed>0.1f?0.1f:elapsed);
}
enum class EmitterRender {
    Normal, BillboardWorldZ, BillboardLocalZ, AlignedWorldZ, AlignedParticleDirection,
    MotionBlur, Linked
};
namespace emitter_flags {
inline constexpr std::uint32_t PointToPoint=0x1, PointToPointBezier=0x2,
    AffectedByWind=0x4, Tinted=0x8, Bounce=0x10, RandomFrame=0x20,
    Inherit=0x40, InheritVelocity=0x80, InheritLocal=0x100, Splat=0x200,
    InheritPart=0x400, DepthTexture=0x800, RandomOrder=0x1000;
}
struct EmitterPreviewSupport {
    bool supported{};
    EmitterBlend blend{EmitterBlend::Normal};
    std::string reason;
    EmitterUpdate update{EmitterUpdate::Fountain};
    EmitterRender render{EmitterRender::Normal};
};
// Supported modes are explicit: unsupported world-dependent effects are never
// silently interpreted as a normal Fountain.
EmitterPreviewSupport emitterPreviewSupport(const EmitterNodeData& data);
std::vector<std::string> emitterPreviewWarnings(const EmitterNodeData& data,
                                               const NodeEmitterState& state);







struct ParticleRandom {
    std::uint32_t state{1};
    std::function<std::uint32_t()> source;
    std::uint32_t next();
    // K1 grass calls loaded srand for each source face before consuming
    // the shared rand stream (CreateArrays 0x317c1e). Captured/external streams
    // can preserve that producer without substituting another libc algorithm.
    std::function<void(std::uint32_t)> seedSource;
    void seed(std::uint32_t value) {state=value;if(seedSource)seedSource(value);}
};
// Scene queries are explicit: the MDL does not contain a collision world or a
// target object. Wind returns DISPLACEMENT for this frame, not acceleration.
struct ParticleContact {
    Vec3 normal{0,0,1};
    float fraction{1}; // earliest segment hit, in [0,1]
    std::optional<Vec3> enginePosition; // Scene::HitCheck writes Particle.position
    // Only engine contacts keep raw/zero normals. Inspection proxy behavior is
    // separate and must never be used for flare visibility.
    bool engineNormal{};
};
enum class ParticleBounceMode { Legacy=1, Reflection=2 };
struct ParticleEnvironment {
    GameVersion game{GameVersion::Kotor2};
    std::optional<Mat4> target;
    std::optional<EngineFrame> engineTarget;
    // Actual containing Gob, absent for ownerless Parts. Engine inheritance
    // retains its own owner/part cache; the caller does not supply a delta.
    std::optional<EngineFrame> engineOwnerFrame;
    // InitializeBranch applies the base model controls to its own Part once.
    const NodeEmitterState* baseEmitterState{};
    Vec3 localEmitterForward{0,0,1};
    // Particle::randomDirection evaluates dead-space against the active camera.
    // Hosts without a camera may leave this unset; only dead-space correction is
    // skipped in that case.
    std::optional<Vec3> cameraPosition;
    // SetEmitterAsWeather's standard mover uses the module weather camera,
    // independently of the particle dead-space camera above.
    std::optional<Vec3> weatherCameraPosition;
    // Explicit legacy inspection input. Engine callers supply engineOwnerFrame.
    Vec3 ownerTranslationDelta{};
    std::function<Vec3(Vec3,float)> windDisplacement;
    // Explosion::Update registers blast wind before constructing particles.
    std::function<void(Vec3,float,float,float)> addPointSourceWind;
    std::function<std::optional<ParticleContact>(Vec3,Vec3)> trace;
    ParticleBounceMode bounceMode{ParticleBounceMode::Legacy};
    Vec3 objectTint{1,1,1}; // Tinted input for explicit legacy inspection rendering
    float partScale{1};     // engine Part +0x24, distinct from owning Gob scale
    float objectAlpha{1};   // Gob alpha multiplier
    float tileAlpha{1};     // caller-resolved tile/fade multiplier
    // ChunkyParticle constructor/virtual initialize and chkParticleLife Gob hooks.
    std::function<void(std::uint64_t,bool,float,bool)> chunkBirth;
    std::function<void(std::uint64_t)> chunkExpired;
    std::function<void(std::uint64_t)> chunkDestroyed;
};
// K2 scene-space emitter support from PartEmitter::CalculateRadius,
// AddEmitterToRooms, PartOutside and EmitterOrderCmp. These records are kept
// independent of NeoMDL so archive viewers or future area tools can supply an
// actual room/portal graph without reimplementing the runtime rules.
struct EmitterScenePlane {
    Vec3 normal{};
    float distance{};
};
struct EmitterSceneRoom {
    std::uint64_t identity{};
    float minimumX{}, minimumY{}, maximumX{}, maximumY{};
    std::vector<EmitterScenePlane> visibilityPlanes;
    bool active{true};
};
struct EmitterAttachedObjectSphere {
    Vec3 center{};
    float radius{};
};
struct EmitterRadiusInput {
    const NodeEmitterState* state{};
    Vec3 emitterPosition{};
    Vec3 globalWind{};
    std::optional<Vec3> targetPosition;
    std::vector<EmitterAttachedObjectSphere> attachedObjects;
    // The packed checks an owner-model runtime flag before every other branch.
    // Its setter has not been identified, so hosts must opt into it explicitly.
    bool forceUnbounded{};
    GameVersion game{GameVersion::Kotor2};
};
float calculateK2EmitterRadius(const EmitterRadiusInput& input) noexcept;

// When the owner has not moved, K2 reuses only its first cached room. A moved
// owner is tested against every room's inclusive XY bounds; no match means all
// rooms. Room Z extents are not consulted by AddEmitterToRooms.
std::vector<std::size_t> k2EmitterRoomMembership(
    Vec3 currentOwnerPosition, Vec3 previousOwnerPosition,
    const std::vector<std::size_t>& cachedOwnerRooms,
    const std::vector<EmitterSceneRoom>& rooms);

bool k2EmitterOutside(Vec3 center, float radius,
                      const std::vector<EmitterScenePlane>& planes) noexcept;

struct EmitterBucketEntry {
    std::size_t emitterIndex{};
    std::uint64_t identity{};
    std::uint64_t ownerIdentity{};
    bool hasOwner{};
    std::int16_t renderOrder{};
    std::int32_t category{};
    Vec3 ownerPosition{};
    Vec3 emitterPosition{};
};
std::vector<std::size_t> engineEmitterBucketDrawOrder(
    const std::vector<EmitterBucketEntry>&, Vec3 camera);
void sortK2EmitterBucket(std::vector<EmitterBucketEntry>& entries,
                         Vec3 cameraPosition);

struct ParticleBlast {
    Vec3 center{};
    float radius{}, remaining{}, strength{1};
};
// Compatibility record retained for hosts using the particle API directly.
// The calculation now follows the exact K2 WindManager point-source path.
Vec3 particleBlastDisplacement(const ParticleBlast& blast, Vec3 point) noexcept;
struct EmitterParticle {
    Vec3 position{}, velocity{}; // scene space; no attachment transform on draw
    Quaternion orientation; // engine Particle quaternion, distinct from movement direction
    float age{}, lifetime{}, rotation{}, sourceScale{1.0f};
    float ownerScaleAtBirth{1.0f}; // ChunkyParticle retains its Gob scale until initialize
    std::uint64_t serial{};
    Vec3 tail{}, inheritedDirection{}, direction{0,0,1}, right{1,0,0}, up{0,1,0};
    float inheritedSpeed{};
    std::int32_t frame{};
    float frameOffset{}, lastFrameAge{};
    bool stopped{};
    Vec3 contactNormal{0,0,1};
};
// A K1/K2 lightning branch is a one-level child LightningEmitter. Its source is
// attached to a main-ribbon particle while its target is either the main
// target object or a free endpoint retained until the next branch refresh.
struct LightningBranchState {
    std::size_t attachment{};
    float scale{1}, lightningAge{}, controlAge{}, birthRate{};
    NodeEmitterState properties;
    EngineLightningControl control;
    Vec3 previousSource{},previousDelta{};
    Vec3 source{},target{}, tangentStart{}, tangentEnd{};
    bool targetsMain{};
    std::optional<std::size_t> flattenedStart;
    std::vector<Vec3> offsets;
    std::vector<EmitterParticle> particles, freeParticles;
};
struct EmitterSimulation {
    std::vector<EmitterParticle> particles;
    double accumulator{}; // accumulated simulated time, not a hidden substep timer
    float birthRemainder{}; // engine time-emission accumulator, reset on emission
    std::uint64_t seed{}, nextSerial{}, detonations{};
    ParticleRandom random;
    Mat4 world=identityMatrix(), previousWorld=identityMatrix();
    EngineFrame frame, previousFrame;


    EngineFrame inheritanceFrame;
    Vec3 distanceRemainder{};
    GameVersion game{GameVersion::Kotor2};
    bool initialized{}, previousDetonate{};
    std::size_t freeParticles{};
    std::uint32_t pendingBursts{};
    // Lightning holds a persistent ribbon and two control-point states. Each
    // path starts a new strip; branch boundaries must never generate joins.
    std::vector<std::size_t> ribbonStarts;
    EngineLightningControl lightningControl;
    std::vector<Vec3> lightningOffsets;
    Vec3 lightningPreviousSource{},lightningPreviousDelta{};
    std::vector<LightningBranchState> lightningBranches;
    std::vector<EmitterParticle> lightningFreeParticles;
    float lightningBirthRate{};
    float lightningAge{10000.0f}, controlAge{10000.0f};
    std::size_t lightningMainCount{}, lightningActiveBranches{};
    bool lightningReady{};
    Vec3 renderTint{1,1,1};
    float renderAlpha{1};
    void reset(std::uint64_t newSeed=0) noexcept;
    void detonate() noexcept; // explicit runtime event, not an invented periodic burst
};
// One caller-supplied simulation frame. Update order, birth accounting, motion,
// lifetime curves and frame stepping share the inspected K1/K2 paths. The scene
// caller supplies the engine elapsed-time limit; no population or value budgets
// are inserted here. Pass a shared RNG to reproduce cross-emitter call ordering.
void advanceEmitter(const EmitterNodeData& data, NodeEmitterState& state,
                    const EngineFrame& frame, double elapsedSeconds,
                    EmitterSimulation& simulation,
                    ParticleRandom* sharedRandom=nullptr,
                    const ParticleEnvironment* environment=nullptr,
                    float ownerScale=1.0f);
// Legacy preview policy (retained for unadmitted platforms): infer birth scale
// from the supplied matrix. K1 Desktop uses advanceEmitter with explicit Gob scale.
void advanceLegacyEmitter(const EmitterNodeData& data, NodeEmitterState& state,
                    const Mat4& world, double elapsedSeconds,
                    EmitterSimulation& simulation,
                    ParticleRandom* sharedRandom=nullptr,
                    const ParticleEnvironment* environment=nullptr);
// Rendering advances texture frames in the original engine. Keeping this
// separate prevents shadow passes, bounds queries or inspector reads from
// consuming RNG/advancing animation. Invoke once per presented effect frame.
void updateEmitterParticleFrames(const EmitterNodeData& data,
    const NodeEmitterState& state, EmitterSimulation& simulation,
    ParticleRandom* sharedRandom=nullptr);
// Does not advance age, spawn particles, or consume random numbers. The
// environment callbacks must not mutate the population being traversed.
void moveEmitterParticle(EmitterParticle& particle,const NodeEmitterState& state,
    const Mat4& previous,const Mat4& current,std::uint32_t flags,float dt,
    const ParticleEnvironment* environment=nullptr);
struct ParticleAppearance {
    std::array<float,4> color{};
    Vec2 size{};
    std::uint32_t frame{}, columns{1}, rows{1};
};
ParticleAppearance emitterParticleAppearance(const EmitterNodeData& data,
    const NodeEmitterState& state, const EmitterParticle& particle,
    // Lightning Linked/MotionBlur uses local ordinal * reciprocal population.
    std::optional<float> normalizedPosition={}) noexcept;
struct ParticleFrameSample { std::int32_t frame{-1}; float weight{}; };
// Base, next, previous, next-next, previous-previous. The packed's five-pass
// weights and endpoint behavior are intentionally not normalized to a crossfade.
std::array<ParticleFrameSample,5> emitterParticleFrames(const EmitterNodeData& data,
    const NodeEmitterState& state, const EmitterParticle& particle) noexcept;
// Indexed quads/strips in EYE space. pass=0 is the base image; frame blending
// uses passes 1..4 in order. Standard quads are newest-first; linked and motion-blurred
// strips use their original forward order, not a camera-dependent sort. Texture coordinates use the original grid
// edges (no half-texel inset). width/height retained for source compatibility.
void buildEmitterMesh(const EmitterNodeData& data, const NodeEmitterState& state,
    const EmitterSimulation& simulation, const Mat4& view,
    const Quaternion& cameraOrientation, Mesh& output,
    std::uint32_t textureWidth=0, std::uint32_t textureHeight=0,
    unsigned pass=0, float ownerScale=1.0f);
// Legacy preview policy: retain each particle's captured size scale. K1 Desktop
// uses buildEmitterMesh with the current explicit Gob scale.
void buildLegacyEmitterMesh(const EmitterNodeData& data, const NodeEmitterState& state,
    const EmitterSimulation& simulation, const Mat4& view, Mesh& output,
    std::uint32_t textureWidth=0, std::uint32_t textureHeight=0,
    unsigned pass=0);
} // namespace neomdl::model
