#pragma once

#include "core/LightScene.hpp"
#include "core/AreaObject.hpp"
#include "core/SceneQueries.hpp"
#include "core/GrassScene.hpp"
#include "core/AreaLoad.hpp"
#include "core/WeatherScene.hpp"
#include "core/WorldAnimation.hpp"
#include "core/WorldRoomState.hpp"
#include "core/TriggerRuntime.hpp"
#include "model/Emitter.hpp"
#include "model/Trigger.hpp"
#include <neoshared/bwm/BwmFile.hpp>

#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

struct AreaRoom {
    LightRoomId id{};
    std::string name;
    model::Vec3 position{};
    model::Mat4 world{model::identityMatrix()};
    std::shared_ptr<ModelDocument> document;
    AreaObject* runtime{}; // owned by AreaScene; stable heap address across moves
    const model::Pose& pose() const { return runtime->pose(); }
    // Engine room endpoints; a room without a static AABB Part retains the
    // inverted +100000/-100000 sentinel, not an invented geometry bound.
    model::Bounds bounds;
    std::vector<model::Bounds> meshBounds;
    std::shared_ptr<const HitRoomGeometry> hitGeometry;
    bool background{};
    std::vector<LightRoomId> influencingRooms;
    std::optional<neoshared::bwm::BwmFile> walkmesh;
    std::optional<neoshared::ResourceDocument> walkmeshSource;
};

// NeoMDL's visual area host. LYT/VIS are scene metadata, not ASCII MDL.
// Static Parts, retained animated room Gobs, references and track Gobs have
// distinct ownership. A failed resource/partition load leaves this host intact.
// A borrowed draw view, never a baked/static copy. Deformed coordinates are
// already model-space; world contains exactly one outer owner transform.
struct AreaMeshInstance {
    const ModelDocument* document{};
    const model::Model* asset{};
    const model::Pose* pose{};
    const SceneAnimationOwner* animationOwner{};
    const model::Mesh* mesh{};
    const model::DeformedMesh* deformed{};
    const neoshared::material::Material* material{};
    model::NodeMaterialState state;
    model::Mat4 world{model::identityMatrix()}, ownerWorld{model::identityMatrix()};
    model::EngineFrame ownerFrame{};
    std::uint64_t owner{}, revision{};
    bool staticPart{}, hasTransparency{};
    const OwnerVisualEffects* ownerEffects{};
    float textureAngleDegrees{};
    float ownerScale{1.0f};
};
// Resource-only worker result. Live owner construction and registration happen
// when the SetMine action commits on the scene thread.
struct PreparedTriggerMine {
    AreaTriggerRecord record;
    TriggerMineState runtime;
    std::shared_ptr<ModelDocument> document;
    ObjectVisualStateLayer authored;
    model::GameVersion game{model::GameVersion::Kotor2};
};
struct AreaWorldState;
class AreaScene;
struct AreaAnimationHost {AreaScene* scene{};};
class AreaScene final {
public:
    AreaScene():animationHost_(std::make_shared<AreaAnimationHost>()) {animationHost_->scene=this;}
    AreaScene(const AreaScene&) = delete;
    AreaScene& operator=(const AreaScene&) = delete;
    AreaScene(AreaScene&& other) noexcept : AreaScene() {swap(other);}
    AreaScene& operator=(AreaScene&& other) noexcept;
    // UI-side snapshot/commit for background preparation. The worker owns its
    // random stream and wind callbacks; no live scene service crosses threads.
    // Both calls receive the canvas's wind/random services. Prepare is called
    // on a fresh host; scene advancement stays paused through commit.
    void prepareRuntime(AreaRuntimeServices services);
    void commitRuntime(AreaRuntimeServices services);




    void load(const std::filesystem::path& layout, const ResourceResolver& resolver = {},
              AreaRuntimeServices services = {},
              const neoshared::bif::JobControl& job = resourceLoadJob());
    void loadResource(neoshared::ResourceDocument layout, const ResourceResolver& resolver,
                      AreaRuntimeServices services = {},
                      const neoshared::bif::JobControl& job = resourceLoadJob());
    void loadModuleArea(const std::string& area, const std::string& scopeIdentity,
                        const ResourceResolver& resolver, AreaLoadOptions options = {},
                        AreaRuntimeServices services = {},
                        const neoshared::bif::JobControl& job = resourceLoadJob());
    const std::string& identity() const noexcept { return identity_; }
    model::GameVersion game() const noexcept { return game_; }
    const std::vector<AreaPlacement>& placements() const noexcept { return placements_; }
    const std::vector<AreaTriggerRecord>& triggers() const noexcept { return triggers_; }
    static PreparedTriggerMine prepareTriggerMine(AreaTriggerRecord,TriggerMineState,
        const ResourceResolver&,model::GameVersion,
        const neoshared::bif::JobControl& job=resourceLoadJob(),
        const model::AsciiResourceResolver& asciiResources = {});
    std::size_t addTriggerMine(PreparedTriggerMine);
    // CreateNewGeometry(2, target, area) ignores target and uses the trigger's
    // actor-derived position; ComputeHeight(1) consumes the first walkable face.
    std::vector<model::Vec3> newMineFootprint(model::Vec3 actorPosition) const;

    const TriggerRuntime& triggerRuntime() const noexcept {return triggerRuntime_;}
    // The callback runs while the owner and its component callbacks still
    // exist, before their engine delayed destruction releases the model Gob.
    using TriggerPreDestroy = std::function<void(AreaObject&)>;
    void updateTriggerObserver(const std::optional<TriggerObserver>&,const TriggerReputation&);
    bool updateTriggerMineObserver(std::size_t,const std::optional<TriggerObserver>&,std::int32_t reputation);
    TriggerDetectionResult checkTriggerDetection(const TriggerDetectionInput&);
    void captureTriggerDetection(TriggerDetectionInput input) {triggerRuntime_.captureDetection(std::move(input));}
    void clearTriggerDetectionCaptures() noexcept {triggerRuntime_.clearDetectionCaptures();}
    std::vector<TriggerDetectionResult> takeTriggerDetectionResults() {return triggerRuntime_.takeDetectionResults();}
    std::vector<TriggerMineRequest> enterTriggerMine(std::size_t,const TriggerMineEnterInput&) const;
    void setTrapInventory(TrapInventoryState state) {triggerRuntime_.setInventory(std::move(state));}
    bool enterTriggerMineFromInventory(std::size_t,const TriggerMineEnterInput&,const TriggerRuntime::RequestSink&);
    bool resumeTriggerMineEntry(const TriggerRuntime::RequestSink& sink) {return triggerRuntime_.resumeMineEntry(sink);}
    TrapActionResult completeTrapAction(TrapObjectTarget target,TrapAction action,
        const TrapActionInput& input,const TriggerRuntime::RequestSink& sink) {
        return triggerRuntime_.completeTrapAction(target,action,input,sink);
    }
    bool resumeTrapAction(const TriggerRuntime::RequestSink& sink) {return triggerRuntime_.resumeTrapAction(sink);}
    TrapSetMineResult completeSetMine(const TrapSetMinePlan& plan,const TriggerRuntime::CreateMine& create,
        const TriggerRuntime::RequestSink& sink) {return triggerRuntime_.completeSetMine(plan,create,sink);}
    bool flagObjectTrap(TrapObjectTarget,std::uint8_t value);
    bool setObjectTrapId(TrapObjectTarget,std::uint64_t objectId);
    bool captureObjectLock(TrapObjectTarget target,const AreaObjectTrapRecord& state) {return triggerRuntime_.captureObjectLock(target,state);}
    bool flagTriggerMine(std::size_t sourceIndex,bool value);
    bool setTriggerObjectId(std::size_t sourceIndex,std::uint64_t objectId);
    bool setTriggerCreator(std::size_t sourceIndex,std::uint64_t creator);
    bool deleteTrigger(std::size_t sourceIndex,std::uint8_t reason,const TriggerPreDestroy&);
    void advanceTriggers(std::uint32_t elapsedMilliseconds,const TriggerPreDestroy&);
    std::vector<TriggerRuntimeEvent> takeTriggerEvents();
    std::vector<TriggerMineRequest> takeTrapServerRequests() {return triggerRuntime_.takeServerRequests();}
    const std::vector<AreaCameraRecord>& cameras() const noexcept { return cameras_; }
    const std::vector<AreaSoundRecord>& sounds() const noexcept { return sounds_; }
    const std::optional<MiniGameDefinition>& miniGame() const noexcept {return miniGame_;}
    const std::vector<TrackFollowerInstance>& trackFollowers() const noexcept {return trackFollowers_;}
    // SetInputClass(1) activates the mini-game and calls Go. Loading the ARE
    // constructs owners and attachments but does not manufacture that request.
    void startMiniGameTracks();
    void stopMiniGameTracks();
    // Replay the already-produced CSWMiniPlayer::Control offset. Translate's
    // world/local flag is captured memory; Rotate ignores that flag.
    void setMiniGamePlayerOffset(model::Vec3 offset,bool worldSpace);
    // Prepared resources only: ForceSetTrack deletes the previous owned Gob,
    // keeps model owners/clocks, creates fresh behaviors and clears running.
    void replaceMiniGameTrack(std::size_t follower,std::shared_ptr<ModelDocument> track,
                              model::Vec3 prototypePosition);
    void forceMiniGameTrackAttachment(std::size_t follower);
    const std::vector<std::string>& diagnostics() const noexcept { return diagnostics_; }
    std::vector<ResourceDependency> dependencies() const;
    std::vector<std::filesystem::path> openPaths() const;
    const AreaVisualSettings& visualSettings() const noexcept { return visualSettings_; }
    const AreaEnvironmentRuntime& environment() const noexcept { return environment_; }
    ResourceResolver resourceResolver() const {return resolver_;}
    model::AsciiResourceResolver asciiResourceResolver() const {return services_.asciiResources;}
    void installWeatherResources(WeatherResources resources) {weatherResources_=std::move(resources);}
    void startWeather(std::uint8_t type) noexcept {weather_.start(type);}
    void stopWeather() noexcept {weather_.stop();}
    bool setWeatherImmediate(std::uint8_t type,float density,
        const std::function<void(AreaObject&)>& initialize={});
    WeatherFrameOutput advanceWeather(const WeatherFrameInput&,
        const std::function<void(AreaObject&)>& initialize={});
    const WeatherScene& weather() const noexcept {return weather_;}
    bool applyEnvironmentReplay(AreaEnvironmentReplayState state,std::string& error);
    void clearEnvironmentReplay();
    void advanceWorldTime(EngineWorldTime current,float clientSeconds,bool paused=false);
    void resetWeatherWorldTime(EngineWorldTime current) noexcept {weather_.resetWorldTime(current);}
    void resetWorldTime();
    const AreaReplayCamera* activeReplayCamera() const noexcept {return environment_.activeCamera();}
    const std::vector<std::unique_ptr<AreaObject>>& objects() const noexcept { return objects_; }
    // Detached weather Gobs retain independently registered particle-chunk
    // descendants. Their own emitters remain excluded by sceneMember().
    std::vector<ParticleScene*> particleSceneRoots();
    std::vector<const ParticleScene*> particleSceneRoots() const;
    AreaObject* object(std::size_t instance) noexcept;
    const AreaObject* object(std::size_t instance) const noexcept;
    // Script producer: first case-insensitive current-area room match, then
    // truncate the VM integer to uint16. Missing rooms/Gobs are engine no-ops.
    void playRoomAnimation(const std::string& roomName,std::int32_t animation);
    // Capture the follower separately from LYT construction. Replacing this
    // snapshot does not create attachments or manufacture a Go request.
    void captureTrackAnimation(std::size_t track,TrackAnimationState state);
    const TrackAnimationState* trackAnimation(std::size_t track) const noexcept;
    void startTrackAnimation(std::size_t track);
    void stopTrackAnimation(std::size_t track);
    void setTrackSpeed(std::size_t track,float speed);
    void playTrackFollowerAnimation(std::size_t track,const std::string& name,int looping,int queued,int overlay);
    void removeTrackFollowerAnimation(std::size_t track,const std::string& name);
    // Engine OnAnimKeyEvent invokes the represented script request before
    // Gob::Animate resumes. The retained queue is observation only.
    void setWorldAnimationEventCallback(std::function<void(const WorldAnimationEvent&)> callback) {
        worldAnimationEvent_=std::move(callback);
    }
    std::vector<WorldAnimationEvent> takeWorldAnimationEvents();
    void setWorldAnimationFrameInput(WorldAnimationFrameInput input) noexcept {worldAnimationFrameInput_=input;}
    bool applyObjectVisualState(std::size_t instance, ObjectVisualStateLayer state,
                                std::string& error);
    // Commit a layer whose resources and animation commands are already prepared.
    bool applyPreparedObjectVisualState(AreaObject&,const ObjectVisualStateLayer&,std::string& error);
    bool dispatchObjectVisualEvent(std::size_t instance, ObjectVisualEvent event,
                                   std::string& error);
    bool clearObjectVisualState(std::size_t instance, std::string& error);
    // Reuses the current owner for alignment/material changes. Engine model
    // replacement receives a fresh host identity range and restarts its loop.
    std::optional<std::size_t> applyCreatureAppearance(std::size_t instance,
        const CreatureAppearanceState&, std::string& error);
    const ModelDocument* lightDocument(std::size_t instance) const noexcept;
    void setQueries(const SceneQueries* queries) noexcept;
    void prepareAnimationFrame(const AreaFrameSettings&);
    const std::shared_ptr<SceneAnimation>& sceneAnimation() const noexcept {return services_.animation;}
    void advancePose(float seconds, const AreaFrameSettings& settings);
    void advanceEffects(float seconds, const AreaFrameSettings& settings);
    void finishWindFrame(float seconds);
    model::WindManager& wind();
    // Explicit ARE grass/wind load; never guesses a module from a LYT name.
    void loadGrassArea(const std::filesystem::path& are,
                       const neoshared::bif::JobControl& job = resourceLoadJob());
    void configureGrass(const GrassSettings& settings, bool applyWind = true,
                        const neoshared::bif::JobControl& job = resourceLoadJob());
    const GrassScene& grass() const noexcept { return grass_; }
    void collectGrass(model::Vec3 camera,const std::vector<model::TriggerPlane>& planes);
    void advanceGrassWind(float seconds);

    void clear(LightScene& lights);
    bool empty() const noexcept { return identity_.empty(); }
    const std::vector<AreaRoom>& rooms() const noexcept { return rooms_; }
    const std::optional<neoshared::ResourceDocument>& surfaceTable() const noexcept {return surfaceTable_;}
    void replaceRoomWalkmesh(const std::string& name,const neoshared::bwm::BwmFile& mesh);
    const std::filesystem::path& path() const noexcept { return path_; }
    model::Bounds bounds() const noexcept;
    std::vector<AreaMeshInstance> meshInstances(bool includeReferences = true, bool dangly = true,
        const std::vector<model::TriggerPlane>& planes = {}) const;

    // CSWSArea::GetRoomIndex queries scoped WOK triangles in LYT order.
    // The position comes from the engine caller/host, never from an MDL box.
    // Nonzero allSurfaces selects CheckAABBAll; otherwise WalkCheck materials.
    bool roomPositionQueriesAvailable() const noexcept {
        return walkableMaterials_.has_value();
    }
    std::optional<LightRoomId> findRoomAt(model::Vec3 position, bool allSurfaces = false) const;
    std::optional<model::Vec3> walkmeshHit(model::Vec3 start,model::Vec3 end) const;
    std::optional<model::Vec3> nearestWalkmeshPoint(model::Vec3 position) const;
    void updateCurrentRoomFromPosition(model::Vec3 position, bool allSurfaces = false);
    // Engine game caller: ignore-visibility suppresses updates; no player or
    // WOK miss retains the previous room (K1 0x2890a, K2 0x194392).
    void updateCurrentRoomFromWorld(const WorldRoomInputs&);
    // Explicit Scene.currentRoom overrides the position producer. Null
    // reproduces the engine all-room branch.
    void setCurrentRoom(std::optional<LightRoomId> room);
    // Release a manual override and resume an explicit replay room, or the
    // scoped WOK position producer when replay supplies no room. A no-hit
    // result uses the engine all-room branch.
    void useReplayedCurrentRoom();
    bool currentRoomOverridden() const noexcept {return currentRoomOverride_;}
    std::optional<LightRoomId> currentRoom() const noexcept { return currentRoom_; }
    void setIgnoreVisibility(bool ignore) noexcept { ignoreVisibility_ = ignore; }
    bool ignoreVisibility() const noexcept { return ignoreVisibility_; }

    // ManageSceneBSP collects the scene room once, not separately for each Gob.
    // Rebuild primary/secondary light membership before selecting any owner.
    void synchronize(LightScene& lights, const std::vector<model::TriggerPlane>& planes);
    const std::vector<LightRoomId>& activeRooms() const noexcept { return activeRooms_; }
    bool roomActive(LightRoomId id) const noexcept;
    bool ownerVisible(std::uint16_t classification, const model::Bounds& localBounds,
                      const model::Mat4& world) const;
    std::vector<LightRoomId> ownerRooms(std::uint16_t classification,
                                      const model::Bounds& localBounds,
                                      const model::Mat4& world) const;
    std::vector<model::EmitterSceneRoom> emitterRooms(
        const std::vector<model::TriggerPlane>& planes) const;

    // AddLightToRooms/AddObjectToRooms use inclusive XY bounds, ignore Z and
    // radius, and fall back to all rooms when no room contains the point.
    std::vector<LightRoomId> roomsAt(model::Vec3 point) const;

    // Reserved host identities cannot overlap the main/ref model IDs (0..N).
    static constexpr LightOwnerId sceneLightOwner = 0xfffffff000000000ull;
    static constexpr LightOwnerId sceneDynamicLightOwner = sceneLightOwner + 1u;
    static constexpr std::size_t environmentLightInstance = 0x8ffffffeull;

private:
    void swap(AreaScene& other) noexcept;
    void bindAnimationOwners();
    void synchronizeCurrentRoom();
    const AreaRoom* room(LightRoomId id) const noexcept;
    bool prepareVisualState(AreaObject& object, ObjectVisualStateLayer& state,
                            std::string& diagnostic);
    bool prepareVisualEvent(AreaObject& object, ObjectVisualEvent& event,
                            std::string& diagnostic);
    void synchronizeObjectTargets();
    void dispatchTriggerEvents(const TriggerPreDestroy&);
    AreaObject* worldAnimationObject(std::size_t instance) noexcept;
    void releaseWorldAnimationOwner(std::size_t instance);
    WorldAnimationDispatch worldAnimationDispatch();
    void dispatchWorldAnimationEvent(AreaObject&,const model::AnimationRuntimeEvent&);
    void synchronizeTrackFollower(TrackFollowerInstance&);
    WeatherVisualCallbacks weatherCallbacks(const std::function<void(AreaObject&)>& initialize);
    void setWeatherAttached(std::size_t slot,bool attached);
    // Services precede objects so their lifetime encloses component callbacks.
    std::shared_ptr<AreaAnimationHost> animationHost_;
    std::shared_ptr<AreaWorldState> worldState_;
    AreaRuntimeServices services_;
    ResourceResolver resolver_;
    std::shared_ptr<ResourceTrace> trace_;
    std::string identity_;
    model::GameVersion game_{model::GameVersion::Kotor2};
    std::vector<AreaPlacement> placements_;
    std::vector<AreaTriggerRecord> triggers_;
    TriggerRuntime triggerRuntime_;
    std::vector<TriggerRuntimeEvent> triggerEvents_;
    std::vector<AreaCameraRecord> cameras_;
    std::vector<AreaSoundRecord> sounds_;
    std::vector<std::string> diagnostics_;
    AreaVisualSettings visualSettings_;
    AreaEnvironmentRuntime environment_;
    // Engine LoadArea spawns scoped model gidy_sun at a fixed transform and
    // recolors its named PartLights. It remains separate from GIT owners.
    std::shared_ptr<ModelDocument> environmentLightDocument_;
    std::optional<model::Pose> environmentLightPose_;
    model::Mat4 environmentLightWorld_{model::nodeTransform({4000.0f,4500.0f,7000.0f},{})};
    GrassScene grass_;
    bool ownsWind_{};
    WeatherScene weather_;
    WeatherResources weatherResources_;
    // Detached Gobs retain their own animation state but leave every scene
    // registration. Attached ownership lives in the ordinary objects_ list.
    std::array<AreaObject*,7> weatherObjects_{};
    std::array<std::unique_ptr<AreaObject>,7> detachedWeather_;
    std::size_t nextWeatherInstance_{0x98000000ull};
    std::optional<model::Vec3> weatherCamera_;
    std::vector<std::unique_ptr<AreaObject>> objects_;
    std::map<std::size_t,TrackAnimationState> trackAnimations_;
    std::optional<MiniGameDefinition> miniGame_;
    std::vector<TrackFollowerInstance> trackFollowers_;
    struct TrackAnimationCallback {std::size_t track{};bool captureInitialHook{};};
    // Each Gob owns a live callback list: ForceSetTrack adds starttrack,
    // every Go appends wildcard callbacks, and DoEvent visits backwards.
    std::map<std::size_t,std::vector<TrackAnimationCallback>> trackAnimationCallbacks_;
    std::vector<WorldAnimationEvent> worldAnimationEvents_;
    std::function<void(const WorldAnimationEvent&)> worldAnimationEvent_;
    WorldAnimationFrameInput worldAnimationFrameInput_{};
    std::vector<AreaRoom> rooms_;
    std::optional<std::uint32_t> walkableMaterials_;
    std::optional<neoshared::ResourceDocument> surfaceTable_;
    std::filesystem::path path_;
    std::optional<LightRoomId> currentRoom_;
    std::optional<LightRoomId> automaticCurrentRoom_;
    bool currentRoomOverride_{};
    bool ignoreVisibility_{};
    std::vector<LightRoomId> activeRooms_;
    std::vector<std::uint64_t> ownedLightIds_;
};

} // namespace neomdl
