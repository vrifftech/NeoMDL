#pragma once
#include "core/ModelDocument.hpp"
#include "core/ReferenceScene.hpp"
#include "core/LightsaberScene.hpp"
#include <model/Emitter.hpp>
#include <model/SceneOrdering.hpp>
#include <model/Wind.hpp>
#include <memory>
#include <array>
#include <map>
#include <tuple>

namespace neomdl {
class SceneQueries;
class ParticleScene;
// Restore a decoded frame grid without mutating NeoTPC's shared texture object.
// Returns null with a diagnostic for mismatched grids, cubes or invalid storage.
std::shared_ptr<const neoshared::texture::TextureData> assembleParticleAtlas(
    const neoshared::texture::TextureData& texture, std::uint32_t columns,
    std::uint32_t rows, std::string& diagnostic);
// Explicit preview-world inputs. Empty targetNode uses the supplied transform;
// a named target follows a node of the main model, including its animation.
// This is not automatic discovery of a game object's target or collision world.
struct ParticleSceneSettings {
    bool targetEnabled{};
    std::string targetNode;
    neomdl::model::Vec3 targetPosition{};
    neomdl::model::Quaternion targetOrientation{};
    neomdl::model::Vec3 windVelocity{}; // K2 global vector, scene units / second
    float windMaximumDeviationZDegrees{};
    float windMaximumDeviationXDegrees{};
    float windInterpolationPeriod{2.0f};
    neomdl::model::Vec3 objectTint{1,1,1};
    float objectAlpha{1}, tileAlpha{1};
    bool blastWind{true};
    neomdl::model::ParticleBounceMode bounceMode{neomdl::model::ParticleBounceMode::Legacy};
    bool operator==(const ParticleSceneSettings& other) const noexcept;
};
bool validParticleSceneSettings(const ParticleSceneSettings& settings) noexcept;
struct ChunkParticleInstance {
    ~ChunkParticleInstance();
    // Non-owning late submissions observe destruction, never extend a Gob's
    // engine lifetime. Pooling/recycle retain this same allocation identity.
    std::shared_ptr<const void> lifetime=std::make_shared<const unsigned char>(0);
    SceneAnimationOwner animationOwner;
    std::uint64_t serial{};
    const ModelDocument* asset{};
    neomdl::model::Pose pose;
    neomdl::model::EngineFrame frame{};
    neomdl::model::Mat4 world=neomdl::model::identityMatrix();
    std::vector<std::optional<neomdl::model::Mat4>> referenceBases;
    std::shared_ptr<ReferenceScene> references;
    LightsaberScene lightsabers;
    std::unique_ptr<ParticleScene> particles;
    std::uint64_t ownerIdentity{},sceneRegistration{};
    neomdl::model::DanglySimulationState danglyState;
    bool active{}, everAssigned{}, playImpact{}, danglyEnabled{true};
    neomdl::model::Mat4 orientation=neomdl::model::identityMatrix();
    float ownerScale{1.0f};
    float time{};
    model::AnimationRuntime animationRuntime;
};
struct ChunkMeshDraw {
    const neomdl::model::Mesh* mesh{};
    const neomdl::model::DeformedMesh* deformed{};
    const neoshared::material::Material* material{};
    const ModelDocument* resources{};
    neomdl::model::NodeMaterialState materialState;
    neomdl::model::GameVersion game{neomdl::model::GameVersion::Kotor2};
    neomdl::model::Mat4 world=neomdl::model::identityMatrix();
    bool fog{};
    // Owner frame for engine light/camera conversion; distinct from a rigid
    // mesh's owner*node draw matrix. Chunk scale is retained from initialize.
    neomdl::model::Mat4 ownerFrame=neomdl::model::identityMatrix();
    std::uint64_t ownerIdentity{};
};

struct ParticleEmitterTargetInput {
    std::string emitterName;
    bool allSourceOwners{true};
    std::optional<std::size_t> sourceReference;
    neomdl::model::EngineFrame frame{};
    neomdl::model::Mat4 target=neomdl::model::identityMatrix();
};
struct ParticleObjectVisualInputs {
    neomdl::model::Vec3 objectTint{1,1,1};
    float objectAlpha{1},tileAlpha{1};
    bool forceUnbounded{};
    std::vector<ParticleEmitterTargetInput> targets;
};
bool validParticleObjectVisualInputs(const ParticleObjectVisualInputs&,std::string& error) noexcept;
// Shared by engine canvas and offscreen validation. Skin outputs are already
// in model space. Reference attachments and node transforms apply exactly once.
// A previously admitted owner can be pooled/recycled before its late Draw;
// this snapshot deliberately does not repeat scene-membership admission.
ChunkMeshDraw chunkParticleMesh(const ChunkParticleInstance& instance,
    std::optional<std::size_t> reference, std::size_t meshIndex);
std::vector<ChunkMeshDraw> chunkParticleMeshes(const ChunkParticleInstance& instance);
struct ParticleEmitterInstance {
    std::optional<std::size_t> reference;
    std::size_t nodeIndex{};
    neomdl::model::GameVersion game{neomdl::model::GameVersion::Kotor2};
    const neomdl::model::EmitterNodeData* data{};
    neomdl::model::NodeEmitterState baseProperties;
    std::shared_ptr<neomdl::model::NodeEmitterState> properties;
    neomdl::model::EmitterPreviewSupport support;
    neomdl::model::EmitterSimulation simulation;
    neomdl::model::Mesh mesh;
    std::array<neomdl::model::Mesh,4> frameBlendMeshes;
    std::shared_ptr<const neoshared::texture::TextureData> atlas;
    neoshared::material::TextureBinding texture;
    neomdl::model::EngineFrame frame{};
    neomdl::model::Mat4 world=neomdl::model::identityMatrix();
    std::uint32_t textureWidth{},textureHeight{};
    bool active{}, visible{true}, modelFog{}, textureSupported{true};
    bool cullingBypass{}, radiusInvalidated{true};
    std::uint64_t sceneIdentity{}, ownerIdentity{};
    std::int32_t sceneCategory{};
    neomdl::model::Vec3 center{}, ownerPosition{}, ownerTranslationDelta{};
    // Explicit legacy inspection history. Engine AnimateParticles owns its
    // distinct, conditionally committed inheritance cache in EmitterSimulation.
    neomdl::model::Vec3 lastAnimatedOwnerPosition{};
    bool lastAnimatedOwnerPositionValid{};
    float calculatedRadius{1.0f};
    float ownerScale{1.0f};
    std::vector<std::size_t> roomMembership;
    const ModelDocument* chunkAsset{};
    std::optional<neomdl::model::AnimationReference> chunkClip;
    std::vector<std::unique_ptr<ChunkParticleInstance>> chunks;
    std::vector<ChunkParticleInstance*> freeChunks;
    std::uint64_t registrationOrder{};
    neomdl::model::ParticleEnvironment preparedEnvironment;
    std::optional<neomdl::model::Mat4> preparedTarget;
    std::optional<neomdl::model::EngineFrame> preparedTargetFrame;
    bool preparedForceUnbounded{};
    std::string diagnostic, resourceDiagnostic;
};
model::EmitterBucketEntry emitterSortEntry(const ParticleEmitterInstance&,std::size_t sourceIndex);
struct ParticleReplayContext {
    model::EngineFrame cameraFrame{};
    neomdl::model::Mat4 view=neomdl::model::identityMatrix();
    std::vector<neomdl::model::EmitterScenePlane> planes;
    // The host owns the inspection Gob's ReferenceScene and its clock.
    std::function<void(const neomdl::model::Pose&,float)> prepareReferences;
    // Commit generated PartEmitter values to the existing inspection
    // owner without replacing that owner's live field identities.
    std::function<void(const neomdl::model::Pose&)> commitEmitterState;
};
struct ParticleSceneStatistics {
    std::size_t emitters{}, unsupported{}, particles{};
    std::size_t activeBlasts{}, missingTargets{}, chunkInstances{}, registeredWindyEmitters{};
    std::size_t visibleEmitters{}, roomRegistrations{};
    bool prewarmed{};
};
struct ParticlePrewarmStep {
    std::size_t emitterIndex{};
    std::size_t particlesBefore{}, particlesAfter{};
    std::uint32_t randomBefore{}, randomAfter{};
    bool completed{};
};
// Stable Mesh addresses for the renderer cache. Reset the GPU cache when
// replacing the document; resetSimulation() keeps these addresses intact.
class ParticleScene final {
public:
    ParticleScene();
    void setSceneAnimation(std::shared_ptr<SceneAnimation>);
    const std::shared_ptr<SceneAnimation>& sceneAnimation() const noexcept {return sceneAnimation_;}
    ~ParticleScene();
    // The scene owns the manager and must outlive this component. A non-null
    // manager is sampled/registered here but advanced only by the scene host.
    void setSceneWind(neomdl::model::WindManager* wind);
    void setSceneRandom(neomdl::model::ParticleRandom* random) noexcept;
    neomdl::model::ParticleRandom& runtimeRandom() noexcept { return sceneRandom_ ? *sceneRandom_ : random_; }
    // Namespace identities before document construction. Zero preserves the
    // isolated viewer's historical IDs. Static room emitters have no Gob.
    void setIdentityBase(std::uint64_t base, bool staticPrimary = false) noexcept {
        identityBase_ = base; staticPrimary_ = staticPrimary;
    }
    neomdl::model::WindManager& windManager() noexcept {
        return sceneWind_ ? *sceneWind_ : wind_;
    }
    ParticleScene(const ParticleScene&)=delete;
    ParticleScene& operator=(const ParticleScene&)=delete;
    ParticleScene(ParticleScene&&)=delete;
    ParticleScene& operator=(ParticleScene&&)=delete;
    void setDocument(const ModelDocument* document);
    const ModelDocument* document() const noexcept { return document_; }
    ParticleScene* rootScene() noexcept { return rootScene_ ? rootScene_ : this; }
    const ParticleScene* rootScene() const noexcept { return rootScene_ ? rootScene_ : this; }
    const ChunkParticleInstance* chunkOwner() const noexcept { return chunkOwner_; }
    SceneAnimationOwner* chunkAnimationOwner() const noexcept {return chunkOwner_?&chunkOwner_->animationOwner:nullptr;}
    void appendActiveChunkScenes(std::vector<ParticleScene*>&);
    void appendActiveChunkScenes(std::vector<const ParticleScene*>&) const;
    void appendAllocatedChunkScenes(std::vector<const ParticleScene*>&) const;
    void setChunkMembershipChanged(std::function<void()> callback) { chunkMembershipChanged_=std::move(callback); }
    void setResourcesDestroyed(std::function<void(const ParticleScene&)> callback) { resourcesDestroyed_=std::move(callback); }
    void setChunkLightsaberSettings(LightsaberSceneSettings settings);
    void setEmitterRemovalSink(std::function<void(ParticleEmitterInstance*)> sink) { emitterRemovalSink_=std::move(sink); }
    bool sceneMember() const noexcept { return sceneMember_; }
    void setSceneMember(bool);
    void prepareChunkFrame();
    void setWeather(bool value) noexcept { weather_=value; }
    void setWeatherCamera(std::optional<model::Vec3> position) noexcept { weatherCameraPosition_=position; }
    void setReferenceScene(const ReferenceScene* references) noexcept { references_ = references; }
    // Non-owning: the host clears this before releasing/replacing its scene.
    void setSceneQueries(const SceneQueries* queries) noexcept { sceneQueries_=queries; }
    std::optional<model::ParticleContact> traceEngineScene(model::Vec3 from,model::Vec3 to) const;
    // Invalid settings leave the previous state untouched. Changed settings
    // reset simulations and blast history but preserve stable GPU mesh keys.
    bool setSettings(const ParticleSceneSettings& settings);

    bool engineRuntime(neomdl::model::GameVersion game) const noexcept {
        return neomdl::model::engineAnimationRuntimeSupported(game);
    }
    void setDanglyEnabled(bool enabled);
    const ParticleSceneSettings& settings() const noexcept {return settings_;}
    bool setObjectVisualInputs(const ParticleObjectVisualInputs&,std::string& error);
    const ParticleObjectVisualInputs& objectVisualInputs() const noexcept {return objectVisualInputs_;}
    const std::vector<neomdl::model::PointSourceWind>& pointSourceWinds() const noexcept {
        return windManager().pointSources();
    }
    const std::vector<neomdl::model::PointSourceWind>& blasts() const noexcept {
        return pointSourceWinds();
    }
    void addPointSourceWind(neomdl::model::Vec3 position, float radius,
                            float lifetime, float strength=1.0f) {
        windManager().addPointSourceWind(position,radius,lifetime,strength);
    }
    void clearPointSourceWinds() noexcept { windManager().clearPointSourceWinds(); }
    const neomdl::model::WindManager& windManager() const noexcept {
        return sceneWind_ ? *sceneWind_ : wind_;
    }
    // Scene::GetWindVector-compatible displacement sample shared by particles
    // and K2 PartDanglyMesh. The seconds argument is the caller's clamped step.
    neomdl::model::Vec3 windDisplacement(
        neomdl::model::Vec3 position, float seconds) const noexcept;
    // rand_wincompatible is shared by particles, wind and texture controllers.
    std::uint32_t nextRuntimeRandom() { return runtimeRandom().next(); }
    void resetSimulation();
    void deferPrewarm(bool defer) noexcept { prewarmDeferred_ = defer; }
    // Pose/spatial preparation runs no Gob animation or PartEmitter simulation. The
    // two animation arguments reproduce Scene::IsAnimating and Gob::IsAnimating
    // inputs later consumed by the merged engine emitter bucket.
    void prepareFrame(const neomdl::model::Pose& pose, double elapsedSeconds,
                      bool includeReferences=true,
                      bool primaryOwnerAnimating=false,
                      const std::vector<bool>& referenceOwnerAnimating={});
    void advance(const neomdl::model::Pose& pose, double elapsedSeconds,
                 bool includeReferences=true, bool finishWind=true);
    // Scene::Render advances the K2 wind timer after its one-time
    // UpdateEmitters prewarm and before RenderSinglePass/DoEmitterBucket.
    void finishWindFrame(float elapsedSeconds);
    // Optional room definitions let an area-aware host reproduce the K2 room
    // registration path. NeoMDL's isolated viewer leaves this empty and uses
    // the supplied scene/frustum planes as the no-room BSP path.
    void setEmitterRooms(std::vector<neomdl::model::EmitterSceneRoom> rooms);
    const std::vector<neomdl::model::EmitterSceneRoom>& emitterRooms() const noexcept {
        return rooms_;
    }
    void buildMeshes(const neomdl::model::Mat4& view,
        const std::vector<neomdl::model::EmitterScenePlane>& scenePlanes = {},
        bool sortLocalBucket = true);
    // Scene-coordinator operations. Hosts should call prepareFrame for every
    // owner first, prewarm scenes in registration order, then prepareBucket,
    // sort the merged admissions, and invoke animate/render in that order.
    void prepareBucket(const neomdl::model::Mat4& view,
        const std::vector<neomdl::model::EmitterScenePlane>& scenePlanes,
        const model::EngineFrame& camera);
    bool prewarmPending() const noexcept {
        return !prewarmed_ && !deterministicReplay_ && !prewarmDeferred_;
    }
    void prewarmPrepared(std::vector<ParticlePrewarmStep>* trace=nullptr);
    void prewarmRegisteredEmitter(std::size_t,ParticlePrewarmStep&);
    void finishPrewarm() noexcept;
    std::size_t particleCount() const noexcept;
    bool preparedOwnerAnimating(std::size_t emitterIndex) const noexcept;
    bool animatePreparedEmitter(std::size_t emitterIndex, double elapsedSeconds);
    void renderPreparedEmitter(std::size_t emitterIndex,
                               const neomdl::model::Mat4& view,const model::EngineFrame& camera);
    void finishBucketFrame() noexcept;
    // Scene hosts merge admissions, not already sorted local buckets.
    const std::vector<std::size_t>& emitterAdmissions() const noexcept {return emitterAdmissions_;}
    const std::vector<std::size_t>& emitterBucket() const noexcept { return emitterBucket_; }
    // Per-object event: no reference means only this model's emitters.
    void dispatchEvent(const std::string& name, std::optional<std::size_t> reference={});
    void detonateAll(bool includeReferences=true);
    // Replays at a declared 60 Hz cadence, including event and frame updates.
    bool replay(std::optional<std::size_t> clip, double seconds, bool loop,
                bool includeReferences,const ParticleReplayContext&);
    const std::string& replayDiagnostic() const noexcept {return replayDiagnostic_;}
    const std::vector<std::unique_ptr<ParticleEmitterInstance>>& emitters() const noexcept {return emitters_;}
    const std::vector<std::string>& diagnostics() const noexcept {return diagnostics_;}
    ParticleSceneStatistics statistics() const noexcept;
    neomdl::model::Bounds bounds() const noexcept;
private:
    bool advanceChunkOwner(float,bool);
    void advanceChunkParts(float);
    void chunkBirth(ParticleEmitterInstance&,std::uint64_t,bool,float,bool);
    void chunkExpired(ParticleEmitterInstance&,std::uint64_t);
    void synchronizeReferenceRegistrations(bool force=false);
    void prepareChunkCallbacks(ParticleEmitterInstance&,model::ParticleEnvironment&);
    struct OwnerSpatialState {
        std::uint64_t identity{};
        neomdl::model::Vec3 current{}, previous{};
        bool initialized{};
        bool pendingAnimating{};
        std::vector<std::size_t> cachedRooms;
    };
    void updateEmitterChunks(ParticleEmitterInstance&,float elapsedSeconds);
    void buildEmitterMeshes(ParticleEmitterInstance&,
                            const neomdl::model::Mat4& view,const model::EngineFrame& camera);
    void clearEmitterMeshes(ParticleEmitterInstance&) noexcept;
    void configureWind();
    void deregisterWindyEmitters() noexcept;
    void registerWindyEmitters();
    void synchronizeEmitterSpatialState(
        ParticleEmitterInstance& emitter,
        const std::optional<neomdl::model::Mat4>& target);
    void rebuildEmitterBucket();
    void refreshEngineEmitterFrame(ParticleEmitterInstance&,model::ParticleEnvironment&);
    void prewarmEmitter(ParticleEmitterInstance&);
    OwnerSpatialState& ownerState(std::uint64_t identity);
    ParticleSceneSettings settings_;
    ParticleObjectVisualInputs objectVisualInputs_;
    bool danglyEnabled_{true};
    std::size_t missingTargets_{};
    std::optional<neomdl::model::Vec3> cameraPosition_;
    std::vector<neomdl::model::EmitterScenePlane> scenePlanes_;
    std::vector<neomdl::model::EmitterSceneRoom> rooms_;
    std::vector<OwnerSpatialState> owners_;
    std::vector<std::uint64_t> referenceRegistrations_;
    std::vector<std::size_t> emitterBucket_, emitterAdmissions_;

    bool visibilityInitialized_{};
    bool prewarmed_{};
    bool prewarmDeferred_{};
    bool deterministicReplay_{};
    using AtlasKey=std::tuple<const neoshared::texture::TextureData*,std::uint32_t,std::uint32_t>;
    std::map<AtlasKey,std::shared_ptr<const neoshared::texture::TextureData>> atlases_;
    const ModelDocument* document_{};
    std::shared_ptr<SceneAnimation> sceneAnimation_=std::make_shared<SceneAnimation>();
    const ReferenceScene* references_{};
    const SceneQueries* sceneQueries_{};
    neomdl::model::ParticleRandom random_;
    neomdl::model::ParticleRandom* sceneRandom_{};
    std::uint64_t identityBase_{};
    bool staticPrimary_{};
    ParticleScene* rootScene_{};
    ChunkParticleInstance* chunkOwner_{};
    bool sceneMember_{true},weather_{};
    std::optional<model::Vec3> weatherCameraPosition_;
    std::function<void()> chunkMembershipChanged_;
    std::function<void(const ParticleScene&)> resourcesDestroyed_;
    LightsaberSceneSettings chunkLightsaberSettings_;
    std::function<void(ParticleEmitterInstance*)> emitterRemovalSink_;
    neomdl::model::WindManager wind_;
    neomdl::model::WindManager* sceneWind_{};
    neomdl::model::ParticleEnvironment preparedEnvironment_;
    std::optional<neomdl::model::Mat4> preparedTarget_;
    bool prepared_{};
    std::string replayDiagnostic_;
    std::vector<std::unique_ptr<ParticleEmitterInstance>> emitters_;
    std::vector<std::string> diagnostics_;
};
} // namespace neomdl
