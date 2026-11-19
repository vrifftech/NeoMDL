#pragma once

#include "core/ModelDocument.hpp"
#include "core/CameraScene.hpp"
#include "core/ReferenceScene.hpp"
#include "core/LightsaberScene.hpp"
#include "core/LightScene.hpp"
#include "core/ShadowScene.hpp"
#include "render/MotionBlurPass.hpp"
#include "core/AreaScene.hpp"
#include "core/ActivePauseClock.hpp"
#include "core/ParticleScene.hpp"
#include "core/EmitterBucket.hpp"
#include "core/TriggerScene.hpp"
#include "core/DialogueRuntime.hpp"
#include "core/DialogueEffects.hpp"
#include "core/DialogueParticipants.hpp"
#include "core/VideoEffects.hpp"
#include "core/DialogueSpeech.hpp"
#include "core/DialogueAnimation.hpp"
#include "core/CreatureLipSync.hpp"
#include "core/DoorVisualRuntime.hpp"
#include "wx/SceneAudioPlayback.hpp"
#include "wx/SceneAudioDevice.hpp"
#include "core/SceneAudioVoices.hpp"
#include "core/ExoRandom.hpp"
#include <neoshared/audio/Playback.hpp>
#include "render/ShaderMeshRenderer.hpp"
#include "render/OwnerEffects.hpp"
#include "render/TextureCache.hpp"

#include <model/AnimationRuntime.hpp>

#include <wx/glcanvas.h>

#include <cstdint>
#include <chrono>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace neomdl::ui {

wxDECLARE_EVENT(EVT_MODEL_NODE_PICKED, wxCommandEvent);
wxDECLARE_EVENT(EVT_AREA_OWNER_PICKED, wxCommandEvent);

enum class AreaGizmoMode {None, Move, Rotate};
enum class AreaGizmoPhase {Begin, Preview, Commit, Cancel};
class AreaGizmoEvent : public wxCommandEvent {
public:
    using wxCommandEvent::wxCommandEvent;
    wxEvent* Clone() const override {return new AreaGizmoEvent(*this);}
    AreaGizmoPhase phase{AreaGizmoPhase::Preview};
    model::Vec3 position;
    float heading{}; // Degrees around world Z.
};
wxDECLARE_EVENT(EVT_AREA_GIZMO, AreaGizmoEvent);

struct AreaSnapSettings {float grid{},rotation{};bool walkmesh{};};
inline constexpr std::size_t kAreaTriggerVertexLimit=255;
enum class AreaMarkerKind {Waypoint, Sound, Camera, Entry, Item, Encounter, Store};
struct AreaEditorMarker {std::string path;AreaMarkerKind kind;model::Vec3 position;};
wxDECLARE_EVENT(EVT_AREA_MARKER_PICKED, wxCommandEvent);
class AreaPolygonEvent : public wxCommandEvent {
public:
    using wxCommandEvent::wxCommandEvent;
    wxEvent* Clone() const override {return new AreaPolygonEvent(*this);}
    AreaGizmoPhase phase{AreaGizmoPhase::Preview};
    std::vector<model::Vec3> points;
    int vertex{-1};
    bool drawing{};
};
wxDECLARE_EVENT(EVT_AREA_POLYGON, AreaPolygonEvent);

enum class WalkmeshTool {View,Select,Paint,Vertices};
using WalkmeshFace=std::pair<std::string,std::uint32_t>;
using WalkmeshVertex=std::pair<std::string,std::uint32_t>;
struct WalkmeshLayer {std::string room;std::shared_ptr<neoshared::bwm::BwmFile> mesh;};
class WalkmeshEvent : public wxCommandEvent {
public:
    using wxCommandEvent::wxCommandEvent;
    wxEvent* Clone() const override {return new WalkmeshEvent(*this);}
    AreaGizmoPhase phase{AreaGizmoPhase::Preview};
    bool painting{};
    std::optional<WalkmeshFace> face;
};
wxDECLARE_EVENT(EVT_WALKMESH_EDIT, WalkmeshEvent);
wxDECLARE_EVENT(EVT_ROOM_EDGE_PICKED, wxCommandEvent);
model::Vec3 walkmeshMaterialColor(std::uint32_t material);

std::string triggerDetectionReport(const TriggerDetectionResult&);

struct DialogueActorAnimation {
    std::optional<std::size_t> owner;
    DialogueAnimationCommand command;
};

struct DialogueVoiceAsset {
    std::string resref;
    std::shared_ptr<const neoshared::audio::PcmData> audio;
    std::optional<CreatureVisemes> visemes;
    bool resourceAvailable{};
    DialogueNodeTiming timing;
};
struct DialogueLineAsset {
    std::vector<DialogueVoiceAsset> voices;
    std::size_t selectedVoice{};
    std::optional<std::size_t> speaker;
    bool speakerBound{};
    bool speakerCreature{true};
    float speakerHeight{0.5f};
    std::optional<std::size_t> listener;
    bool listenerBound{};
    float listenerHeight{0.5f};
    std::optional<float> cameraPullback;
    bool speakerAppearance565{};
    std::vector<DialogueActorAnimation> animations;
};
struct PreparedDialogueActor {
    std::optional<std::size_t> owner;
    bool canPlay{}, injured{}; // explicitly captured engine creature predicates
    bool creature{};
    DialogueParticipantInputs participant;
    DialogueHeadTracking headTracking;
    DialogueActorAnimation listReset, pause, listen, talk;
};
struct PreparedDialoguePlayback {
    neoshared::dlg::Dialogue dialogue;
    DialogueSpeechState speech;
    std::vector<DialogueNodeTiming> timing;
    std::vector<DialogueLineAsset> lines;
    std::vector<PreparedDialogueActor> actors;
    std::unique_ptr<ModelDocument> cameraModel;
    std::shared_ptr<const neo2da::TwoDAFile> videoEffects;
};
struct PreparedWeatherPlayback {
    WeatherResources resources;
    PreparedSceneAudioPlayback thunder;
};

struct WorldPlaybackSettings {
    bool engineRoomSelection{};
    bool paused{};
    std::optional<HitOwnerId> playerOwner;
    std::uint8_t cameraMode{};
    // The inspection host explicitly supplies its active camera as the
    // module camera. This is independent of whether a player is bound.
    bool moduleCameraPresent{true};
    DoorFadeSettings doorFade;
    bool cameraSceneFocus{true};
    model::Vec3 sceneFocus{};
    // UpdateWind's active-pause state comes from the retained engine clock.
    std::uint8_t creatureWindMode{1};
    float creatureWindIntensity{2.5f};
    std::map<HitOwnerId,std::uint8_t> creatureFadeStates;
};

struct LightsaberPreviewSettings {
    // Engine debug/render globals rather than authored MDL fields.
    bool additiveBlur{};
    bool lighting{};
    bool wireframe{};
    bool renderPieces{true};
    bool renderFrontHalf{true};

    friend bool operator==(const LightsaberPreviewSettings& left,
                           const LightsaberPreviewSettings& right) noexcept {
        return left.additiveBlur == right.additiveBlur &&
               left.lighting == right.lighting &&
               left.wireframe == right.wireframe &&
               left.renderPieces == right.renderPieces &&
               left.renderFrontHalf == right.renderFrontHalf;
    }
    friend bool operator!=(const LightsaberPreviewSettings& left,
                           const LightsaberPreviewSettings& right) noexcept {
        return !(left == right);
    }
};


struct CameraPreviewSettings {
    // K2 Camera defaults and the animated-camera clip distances from
    // CSWCModule::SetAnimatedCamera. FOV is external Camera state, not MDL data.
    float viewAngle{kK2CameraDefaultViewAngle};
    float nearClip{kK2AnimatedCameraNearClip};
    float farClip{kK2AnimatedCameraFarClip};

    friend bool operator==(const CameraPreviewSettings& left,
                           const CameraPreviewSettings& right) noexcept {
        return left.viewAngle == right.viewAngle &&
               left.nearClip == right.nearClip &&
               left.farClip == right.farClip;
    }
    friend bool operator!=(const CameraPreviewSettings& left,
                           const CameraPreviewSettings& right) noexcept {
        return !(left == right);
    }
};

struct K2SkinPreviewSettings {
    // These are owning-Gob/scene values in K2, not MDL fields. They default to
    // disabled so loading a model never invents runtime effects.
    bool motionBlur{};
    neomdl::model::Vec3 objectMotion{}; // current minus previous Gob position
    float maximumBlurLength{1.5f};
    bool sideExtrusion{true};
    float sideScale{1.0f};

    friend bool operator==(const K2SkinPreviewSettings& left,
                           const K2SkinPreviewSettings& right) noexcept {
        return left.motionBlur == right.motionBlur &&
               left.objectMotion.x == right.objectMotion.x &&
               left.objectMotion.y == right.objectMotion.y &&
               left.objectMotion.z == right.objectMotion.z &&
               left.maximumBlurLength == right.maximumBlurLength &&
               left.sideExtrusion == right.sideExtrusion &&
               left.sideScale == right.sideScale;
    }
    friend bool operator!=(const K2SkinPreviewSettings& left,
                           const K2SkinPreviewSettings& right) noexcept {
        return !(left == right);
    }
};

struct RenderOptions {
    bool textures{true};
    bool lightmaps{true};
    bool wireframe{};
    bool lighting{true};
    // Unset uses standalone-model or area-view defaults.
    std::optional<bool> grid;
    std::optional<bool> axes;
    bool showHidden{};
    bool cullBackfaces{true};
    bool danglySimulation{true};
    bool nodeHelpers{};
    bool previewLights{true};
    // Scene::RenderOnlyGobs with enablegetnearestlights enabled. This is an
    // alternate scene mode, not a different ranking for the normal buckets.
    bool nearestLightsOnlyGobs{};
    // Area shadows follow the current room; explicit inputs remain diagnostic.
    std::optional<bool> engineShadows;
    bool engineTwoSidedShadows{};
    std::optional<bool> engineLensFlares; // enabled for area views
    bool engineSoftShadows{}; // explicit Desktop branch, not hardware auto-detection
    neomdl::render::EngineSoftShadowMode engineSoftShadowMode{neomdl::render::EngineSoftShadowMode::DesktopARB};
    neomdl::material::K2HardwareInputs engineHardware; // captured engine capability context
    bool engineUseVertexPrograms{true}; // PartSkin::Draw's separate usevertexprograms global
    bool inspectMeshProjections{};
    // Gob's runtime radius is externally supplied, not the rendered mesh AABB.
    // Gob::Gob initializes +0x164 to 1; GetMinimumSphere is a separate operation.
    float objectLightRadius{1.0f};
    std::optional<bool> headlight;
    bool gobNewSorting{true}; // global switch, independent of Force Sight
    neomdl::render::ProcedureMipPath procedureMipPath{neomdl::render::ProcedureMipPath::SgiAutomatic};
    bool fogEnabled{true};
    neomdl::model::FogSettings fog{};
    std::array<float,3> sceneAmbient{{0.25f,0.25f,0.25f}};
    bool useLoadedAreaEnvironment{true};
    bool useAreaReplayCamera{true};
    // Camera overrides apply only in explicit shadow-input inspection mode.
    bool shadowScenePositionFromAreaCamera{};
    bool shadowCasterRegionFromAreaCamera{};
    bool previewEmitters{true};
    neomdl::ParticleSceneSettings particleSettings;
    bool showCameras{};
    CameraPreviewSettings cameraSettings;
    bool showReferences{true};
    bool previewLightsabers{true};
    LightsaberPreviewSettings lightsaberSettings;
    K2SkinPreviewSettings skinSettings;
    bool previewAabbs{};
    bool previewTriggers{};
    int tangentBasisDebug{}; // 0 shaded; 1 tangent; 2 normal; 3 bitangent
};

struct AabbDebugSubmission {
    const neomdl::model::Mesh* mesh{};
    const neomdl::model::AabbNodeData* tree{};
    neomdl::model::Mat4 world{neomdl::model::identityMatrix()};
    bool selected{};
};

struct PreparedTrapDoorAnimation {
    std::size_t sourceIndex{};
    std::vector<ObjectAnimationCommand> animations;
};
using PreparedTrapDoorAnimations=std::vector<PreparedTrapDoorAnimation>;

struct TriggerMinePreviewInputs {
    std::optional<TriggerObserver> observer;
    std::int32_t reputation{};
};

class ModelCanvas final : public wxGLCanvas {
public:
    explicit ModelCanvas(wxWindow* parent);
    ~ModelCanvas() override;

    void setDocument(const ModelDocument* document, bool preserveCamera = false);
    void refreshMaterials();
    void setLoading(bool loading);
    // Index 0 is the main Gob; reference index n is owner n+1. Runtime-only.
    const OwnerVisualEffects& ownerVisualEffects(std::size_t owner) const;
    bool setOwnerVisualEffects(std::size_t owner, const OwnerVisualEffects&, std::string& error);
    const SceneEffectsSettings& sceneEffects() const noexcept { return sceneEffects_; }
    bool setSceneEffects(SceneEffectsSettings,std::string& error,bool resetHistory=true);
    void setScriptVideoEffect(bool enabled) noexcept {scriptVideoEffect_=enabled;}
    float textureAngle(std::size_t owner, std::size_t node) const noexcept;
    bool setTextureAngle(std::size_t owner, std::size_t node, float degrees, std::string& error);
    // Snapshot on the UI thread, populate the returned CPU scene on a worker,
    // then install on the UI thread after the worker has joined.
    AreaScene prepareAreaLoad();
    void installArea(AreaScene pending,PreparedSceneAudioPlayback sounds={},bool preserveCamera=false);
    void clearArea();
    void configureSceneAudio(std::optional<ExoRandom::State> capturedState);
    void ensureSceneAudioProvider();

    void mergeSceneAudioAssets(PreparedSceneAudioPlayback);
    std::size_t addTriggerMine(PreparedTriggerMine,PreparedSceneAudioPlayback);
    void stopSceneAudio();
    std::optional<ExoRandom::State> sceneAudioRandomState() const {
        return sceneAudioExo_?std::optional<ExoRandom::State>{sceneAudioExo_->state()}:std::nullopt;
    }
    const std::string& sceneAudioStatus() const noexcept {return sceneAudioStatus_;}
    const ShadowScene& shadowScene() const noexcept {return shadowScene_;}
    const MotionBlurScene& motionBlurScene() const noexcept {return motionBlurScene_;}
    void setMotionBlurOwner(LightOwnerId owner,MotionBlurOwnerSettings settings);
    void resetMotionBlurHistory();
    // Explicit external Gob movement; references follow their attachment. No
    // history reset: the next completed Render/RenderBlur owns that transition.
    void setSceneOwnerWorld(LightOwnerId owner,const model::Mat4& world);
    const material::DirtyMaterialSettings& dirtySettings() const noexcept {return dirtySettings_;}
    void setDirtyMaterial(LightOwnerId owner,std::size_t mesh,material::DirtyMaterialSettings settings);
    void disableDirtyMaterial();
    void loadDirtyArea(const std::filesystem::path& are);
    void useLoadedDirtyArea();
    DirtyAreaState dirtyAreaState() const;
    std::string engineExecutionReport() const;
    void setSceneShadowInputs(SceneShadowInputs inputs);
    void setSceneCasterEnabled(LightOwnerId owner,bool enabled);
    void setAreaRoom(std::optional<LightRoomId> id);
    void useReplayedAreaRoom();
    const WorldPlaybackSettings& worldPlaybackSettings() const noexcept {return worldPlaybackSettings_;}
    void setWorldPlaybackSettings(WorldPlaybackSettings settings);
    void setWorldPauseState(std::uint8_t mask,int enable,const std::vector<HitOwnerId>& exclusions);
    std::uint8_t worldPauseMask() const noexcept {return worldClock_.pauseMask();}
    std::vector<HitOwnerId> worldPauseExclusions() const;
    void resetWorldTime();
    bool applyAreaEnvironmentReplay(AreaEnvironmentReplayState state,std::string& error);
    void clearAreaEnvironmentReplay();
    const AreaScene& areaScene() const noexcept { return areaScene_; }
    TriggerMinePreviewInputs triggerMineInputs(std::size_t source) const;
    bool setTriggerMineInputs(std::size_t source,bool flagged,TriggerMinePreviewInputs);
    bool deleteTriggerMine(std::size_t source,std::uint8_t reason);
    TriggerDetectionResult checkTriggerDetection(const TriggerDetectionInput&);
    void captureTriggerDetection(TriggerDetectionInput);
    void clearTriggerDetectionCaptures();
    void setTrapInventory(TrapInventoryState state) {areaScene_.setTrapInventory(std::move(state));}
    bool enterTriggerMine(std::size_t source,const TriggerMineEnterInput&);
    bool resumeTriggerMineEntry();
    TrapActionResult completeTrapAction(TrapObjectTarget,TrapAction,const TrapActionInput&);
    TrapSetMineResult completeSetMine(const TrapSetMinePlan&,std::optional<PreparedTriggerMine>,PreparedSceneAudioPlayback,PreparedTrapDoorAnimations);
    bool receiveTrapRequest(const TriggerMineRequest&);
    bool setTriggerCreator(std::size_t source,std::uint64_t id) {return areaScene_.setTriggerCreator(source,id);}
    bool setObjectTrapFlag(TrapObjectTarget target,std::uint8_t flag) {return areaScene_.flagObjectTrap(target,flag);}
    bool setObjectTrapId(TrapObjectTarget target,std::uint64_t id) {return areaScene_.setObjectTrapId(target,id);}
    bool captureObjectLock(TrapObjectTarget target,const AreaObjectTrapRecord& state) {return areaScene_.captureObjectLock(target,state);}
    bool setTriggerObjectId(std::size_t source,std::uint64_t id) {return areaScene_.setTriggerObjectId(source,id);}
    void loadGrassArea(const std::filesystem::path& path);
    void configureGrass(const GrassSettings& settings);
    bool playAreaAnimation(std::size_t instance,const std::string& clip,float rate,std::string& error);
    void playRoomAnimation(const std::string& room,std::int32_t number);
    void captureTrackAnimation(std::size_t track,TrackAnimationState state);
    void startTrackAnimation(std::size_t track);
    void stopTrackAnimation(std::size_t track);
    void startMiniGameTracks() {areaScene_.startMiniGameTracks();Refresh(false);}
    void stopMiniGameTracks() {areaScene_.stopMiniGameTracks();Refresh(false);}
    void setTrackSpeed(std::size_t track,float speed);
    void playTrackFollowerAnimation(std::size_t track,const std::string& name,int looping,int queued,int overlay);
    void removeTrackFollowerAnimation(std::size_t track,const std::string& name);
    void pauseAreaObject(std::size_t instance,bool paused);
    void resetAreaObject(std::size_t instance);
    bool setDoorOpenState(std::size_t instance,std::uint8_t state,std::int32_t selector,std::string& error);
    void startDialogue(PreparedDialoguePlayback prepared,std::size_t entry);
    void stopDialogue();
    void chooseDialogueReply(std::size_t reply);
    bool dialogueFading() const noexcept {return dialogueFade_.fading();}
    const DialogueRuntime& dialogue() const noexcept {return dialogueRuntime_;}
    std::vector<std::string> takeDialogueMessages();
    void configureWeather(PreparedWeatherPlayback prepared,WeatherFrameInput input,bool viewerCamera,
                          std::optional<std::uint8_t> command,bool viewerPosition=false);
    const WeatherFrameInput& weatherInputs() const noexcept {return weatherInput_;}
    bool weatherUsesViewerCamera() const noexcept {return weatherUseViewerCamera_;}
    bool weatherUsesViewerPosition() const noexcept {return weatherUseViewerPosition_;}
    float weatherShadowAlpha() const noexcept {return weatherShadowAlpha_;}
    std::optional<std::size_t> applyCreatureAppearance(std::size_t instance,
        const CreatureAppearanceState&,std::string& error);
    bool loadLipSync(std::optional<std::size_t> instance,const std::filesystem::path& path,
                     float lipDelay,std::string& error);
    void stopLipSync(std::optional<std::size_t> instance);
    void setOptions(RenderOptions options);
    const RenderOptions& options() const noexcept { return options_; }
    void setSelectedNodes(std::set<std::size_t> nodes);
    void setSelectedAreaOwners(std::set<LightOwnerId> owners);
    void framePosition(model::Vec3 position, float extent);
    std::optional<model::Vec3> areaPlacementPoint(wxPoint position,std::optional<float> planeZ={}) const;
    void setAreaGizmo(AreaGizmoMode mode,model::Vec3 position={},float heading=0.0f);
    bool areaGizmoDragging() const noexcept {return gizmoDrag_.has_value();}
    void cancelAreaGizmo();
    void previewAreaTrigger(std::size_t sourceIndex,model::Vec3 position,float headingDelta);
    void previewAreaCamera(std::size_t sourceIndex,model::Vec3 position);
    void previewAreaCameras(const std::vector<AreaCameraRecord>& records,std::optional<std::size_t> active={});
    void setViewerOrientation(float yaw,float pitch);
    void setAreaSnapping(AreaSnapSettings settings) {areaSnap_=settings;}
    std::optional<model::Vec3> snappedAreaPoint(model::Vec3 position,bool snapZ=true) const;
    float snappedAreaHeading(float heading) const;
    void setAreaMarkers(std::vector<AreaEditorMarker> markers,std::string selected);
    void setWalkmeshLayers(std::vector<WalkmeshLayer> layers);
    void configureWalkmesh(bool visible,bool xray,WalkmeshTool tool,std::string room);
    const std::set<WalkmeshFace>& selectedWalkmeshFaces() const noexcept {return walkmeshSelection_;}
    const std::set<WalkmeshVertex>& selectedWalkmeshVertices() const noexcept {return walkmeshVertices_;}
    void setWalkmeshVertices(std::set<WalkmeshVertex> vertices);
    void setWalkmeshSelection(std::set<WalkmeshFace> faces);
    bool walkmeshDragging() const noexcept {return walkmeshDrag_.has_value();}
    void cancelWalkmeshStroke();
    void updateRoomWalkmesh(const std::string& room,const neoshared::bwm::BwmFile& mesh) {areaScene_.replaceRoomWalkmesh(room,mesh);Refresh(false);}
    void setRoomConnections(std::vector<RoomConnection> connections);
    void configureRoomConnections(bool visible,bool xray,std::optional<std::size_t> selected);
    void setRoomVisibility(LightRoomId from,LightRoomId to,bool visible) {areaScene_.setRoomVisibility(from,to,visible);Refresh(false);}
    void replaceAreaVisibility(const std::vector<std::uint8_t>& bytes) {areaScene_.replaceRoomVisibility(bytes);Refresh(false);}
    void previewRoomMove(std::optional<RoomMovePreview> preview) {roomMovePreview_=std::move(preview);Refresh(false);}
    void moveAreaMarker(const std::string& path,model::Vec3 position);
    void setAreaPolygon(std::vector<model::Vec3> points,bool vertices);
    void beginAreaPolygon(std::optional<float> planeZ={},float zOffset=0.0f,std::optional<std::size_t> vertexLimit=kAreaTriggerVertexLimit);
    bool areaPolygonDrawing() const noexcept {return polygonDrawing_;}
    bool areaPolygonDragging() const noexcept {return polygonDrag_.has_value();}
    void finishAreaPolygon();
    void cancelAreaPolygon();
    void previewAreaTriggerGeometry(std::size_t sourceIndex,const std::vector<model::Vec3>& points);
    void hideSelectedMeshes();
    void isolateSelectedMeshes(bool enabled);
    void showAllMeshes();
    bool meshesIsolated() const noexcept {return isolatedNodes_.has_value();}
    const std::set<std::size_t>& hiddenMeshNodes() const noexcept {return hiddenNodes_;}
    const std::optional<std::set<std::size_t>>& isolatedMeshNodes() const noexcept {return isolatedNodes_;}
    void setMeshVisibility(std::set<std::size_t> hidden,std::optional<std::set<std::size_t>> isolated);
    void copyCameraFrom(const ModelCanvas& source);
    // Compatibility single-run entry point used by hosts that only need the
    // historical animation picker. The K2 run API below exposes the recovered
    // Gob/AnimRun priority list directly.
    void setAnimation(std::optional<std::size_t> index, float time,
                      bool loop = false, bool continuous = false);
    neomdl::model::AnimationPlayResult playAnimationRun(
        std::size_t clipIndex, float playbackRate = 1.0f,
        std::uint32_t flags = 0u, float startTime = 0.0f);
    neomdl::model::AnimationPlayResult playOutOfOrderAnimationRun(
        std::size_t clipIndex,
        const std::vector<float>& normalizedAnimationTimes,
        const std::vector<float>& controlTimes, float duration);
    bool stopAnimationRun(std::uint64_t id, bool graceful = true);
    bool removeAnimationRun(std::uint64_t id);
    bool setAnimationRunTime(std::uint64_t id, float time,
                             bool rebuildFromBind = true);
    bool setAnimationRunPlaybackRate(std::uint64_t id, float playbackRate);
    bool setAnimationRunLooping(std::uint64_t id, bool looping);
    void clearAnimationRuns();
    const std::vector<neomdl::model::AnimationRun>& animationRuns() const noexcept {
        return animationRuntime_.runs();
    }
    const neomdl::model::AnimationRun* animationRun(
        std::uint64_t id) const noexcept { return animationRuntime_.run(id); }
    std::optional<std::uint64_t> primaryAnimationRunId() const noexcept {
        return animationRuntime_.primaryRunId();
    }
    std::vector<neomdl::model::AnimationRuntimeEvent>
        takeAnimationRuntimeEvents();
    void resetDynamics();
    void dispatchParticleEvent(const std::string& name);
    void detonateEmitters();
    void advance(float elapsedSeconds, bool advanceParticles = true,
                 bool advanceAnimations = true);
    const ParticleScene& particleScene() const noexcept { return particleScene_; }
    const ReferenceScene& referenceScene() const noexcept { return referenceScene_; }
    // Area-aware hosts may register real scene lights/room lists and set owner
    // query inputs. Call Refresh after changing host state; never infer rooms
    // from the isolated model's bounding box.
    LightScene& lightScene() noexcept { return lightScene_; }
    const LightScene& lightScene() const noexcept { return lightScene_; }
    const CameraScene& cameraScene() const noexcept { return cameraScene_; }
    const TriggerScene& triggerScene() const noexcept { return triggerScene_; }
    TriggerScene& triggerScene() noexcept { return triggerScene_; }
    std::size_t createTriggerVolume(
        std::vector<neomdl::model::Vec3> footprint,
        std::array<float, 4> color = {{1.0f, 1.0f, 1.0f, 1.0f}},
        std::uintptr_t owner = 0u);
    bool removeTriggerVolume(std::size_t index);
    void clearTriggerVolumes();
    bool setTriggerHighlighted(std::size_t index, bool highlighted,
                               float height, bool group = false);
    bool setTriggerHighlightParameters(
        std::size_t index,
        const neomdl::model::TriggerHighlightParameters& parameters);
    bool setTriggerClientVisualState(std::size_t index,
                                     const TriggerClientVisualState& state);
    void setTriggerGlobalAlpha(float alpha);
    std::optional<RuntimeTriggerHit> hitTestTriggers(
        const neomdl::model::Vec3& start,
        const neomdl::model::Vec3& end,
        const neomdl::model::TriggerHitOptions& options = {}) const;
    bool activateCameraNode(std::size_t nodeIndex);
    bool activateCameraHook();
    bool activateAreaCamera(std::size_t index);
    bool activateAreaCameraId(std::int32_t cameraId);
    void useViewerCamera(bool freeFly);
    bool freeFlyCameraActive() const noexcept { return freeFly_ && !authoredCameraActive(); }
    float freeFlySpeed() const noexcept { return flySpeed_; }
    bool freeFlyCollision() const noexcept { return flyCollision_; }
    void setFreeFlyCollision(bool enabled) noexcept { flyCollision_ = enabled; }
    bool authoredCameraActive() const noexcept { return cameraScene_.anyActive()||dialogueCameraActive_||dialogueActorCameraActive_||activeAreaReplayCamera(); }
    ReferenceReattachResult reattachReferences(
        std::optional<std::size_t> ownerReference,
        std::optional<ReferenceObjectTarget> target);
    ReferenceReattachResult restoreReferenceAttachments(
        std::optional<std::size_t> ownerReference);
    const neomdl::model::Pose* pose() const noexcept { return pose_ ? &*pose_ : nullptr; }
    void frameModel();
    neomdl::render::RendererStatistics rendererStatistics() const noexcept { return meshRenderer_.statistics(); }

private:
    void advanceDialogue();
    void restoreCompletedDialogueAnimations(std::uint32_t elapsedMilliseconds);
    void finishDialogueActors();
    const PreparedDialogueActor& dialogueActor(std::optional<std::size_t> owner) const;
    void setDialogueInCutscene(std::optional<std::size_t> owner, bool enabled);
    void setDialogueAlwaysRendering(std::optional<std::size_t> owner, bool enabled);
    void playDialogueAnimation(const DialogueActorAnimation&, bool saveLoop, bool talkReset = false);
    void resetDialogueActor(std::optional<std::size_t> owner, bool talkReset);
    void setDialogueAnimations(const DialogueLineAsset&);
    void restoreDialogueCamera();
    const DialogueVoiceAsset& selectedDialogueVoice(std::size_t lineIndex) const;
    void dispatchDialogueEvents();
    void applyDialogueVideoEffect(DialogueVideoCaller,int row);
    void prepareDialogueParticipants(const DialogueLineAsset&,std::uint32_t cameraAngle);
    void clearDialogueParticipants();
    bool lookAtDialogueParticipant(std::optional<std::size_t> source,
        std::optional<std::optional<std::size_t>> target,float maximumDistance);
    void orientDialogueParticipant(std::optional<std::size_t> source,std::optional<std::size_t> target);
    model::EngineFrame dialogueLogicalFrame(std::optional<std::size_t> owner) const;
    void retainParticleTextures(const ParticleScene&);
    void releaseParticleResources(const ParticleScene&);
    void refreshChunkMembership();
    void releaseChunkCallbacks();
    void beforeTriggerDestroyed(AreaObject&);
    void refreshTriggerObservers();
    void consumeTriggerEvents();
    void consumeTriggerDetectionResults();
    void advanceWeather();
    model::EngineFrame sceneCameraFrame() const;
    SceneAudioBackend sceneAudioBackend();
    SceneAudioVoiceBackend sceneAudioVoiceBackend();
    void updateSceneAudioStream(std::uint64_t provider,const SceneAudioPlay&);
    std::uint32_t playSceneAudio(const SceneAudioPlay&);
    void admitSceneAudio(std::uint64_t provider,const SceneAudioPlay&,std::uint32_t frequency);
    void updateSceneAudioListener();
    void advanceSceneAudio();
    void clearSceneAudio();
    void initializeAreaObject(AreaObject& object);
    DialogueCameraActor dialogueCameraActor(std::optional<std::size_t> owner,float height) const;
    void updateDialogueActorCamera();
    void onPaint(wxPaintEvent& event);
    void onSize(wxSizeEvent& event);
    void onErase(wxEraseEvent& event);
    void onMouse(wxMouseEvent& event);
    bool handleWalkmesh(wxMouseEvent& event);
    std::optional<WalkmeshFace> pickWalkmesh(wxPoint point) const;
    std::optional<WalkmeshVertex> pickWalkmeshVertex(wxPoint point) const;
    void emitWalkmesh(AreaGizmoPhase phase,bool painting,std::optional<WalkmeshFace> face={});
    void drawWalkmeshOverlay();
    void drawRoomConnections();
    std::optional<std::size_t> pickRoomConnection(wxPoint point) const;
    void onKey(wxKeyEvent& event);
    void onFocusLost(wxFocusEvent& event);
    void onCaptureLost(wxMouseCaptureLostEvent& event);
    void considerMeshPick(const neomdl::render::MeshSubmission& submission);
    void resetCameraInput();
    bool advanceCamera();
    void moveFlyCamera(model::Vec3 position);
    model::Vec3 cameraOffset(model::Vec3 local) const;
    model::Vec3 viewerCameraPosition() const;
    void initializeGl();
    void clearGpuTextures();
    neomdl::render::GpuTexture textureFor(const neoshared::material::TextureBinding& binding,
                                         std::size_t layer = 0, const ModelDocument* resources = nullptr);
    neomdl::render::GpuTexture textureForResref(const std::string& resref,
                                                const ModelDocument* resources = nullptr);
    void applyProjection(int width, int height);
    void synchronizeCameraScene();
    void bindExternalCameraControl();
    neomdl::model::DanglyEnvironment danglyEnvironment(
        neomdl::model::EngineFrame objectFrame = {}) const;
    void rebuildPose(bool resetDynamics);
    void collectAnimationRuntimeEvents();
    bool advanceMainOwner(float seconds,bool evaluatePose);
    SceneAnimationOwner* animationOwnerForId(LightOwnerId owner);
    void updateAnimationVisibility();
    bool engineOwnerAdmitted(LightOwnerId owner,std::uint16_t classification);
    void animateRenderOwner(LightOwnerId owner);
    void refreshOwnerSubmission(render::MeshSubmission& submission,LightOwnerId owner,
        const AreaObject* retainedObject,std::optional<std::size_t> retainedReference);
    void refreshAnimatedSubmission(render::MeshSubmission& submission,
        const model::Model& asset,const model::Pose& pose,const model::Mat4& world,
        const model::EngineFrame& frame,std::uint64_t revision,const neoshared::material::Material& material,
        const ModelDocument* document,const OwnerVisualEffects* effects,
        std::optional<float> textureAngle = {});
    void resetParticleHistoryForAnimationSeek();
    void updateSceneBounds();
    void drawScene();
    void drawEngineShadows();
    bool synchronizeEngineQueries();
    bool drawEngineLensFlares();
    void drawMeshProjectionInspection();
    void drawGridAndAxes();
    void drawAreaGizmo();
    void drawAreaEditorOverlay();
    bool handleAreaPolygon(wxMouseEvent& event);
    void emitAreaPolygon(AreaGizmoPhase phase,bool drawing,int vertex=-1);
    std::optional<std::string> pickAreaMarker(wxPoint position) const;
    bool handleAreaGizmo(wxMouseEvent& event);
    model::Vec3 gizmoScreenPoint(model::Vec3 position) const;
    std::optional<model::Vec3> gizmoConstraint(wxPoint position,int handle,model::Vec3 origin) const;
    void emitAreaGizmo(AreaGizmoPhase phase);
    void queueAreaRooms();
    bool prepareSubmission(const render::MeshSubmission& submission,
                           render::MeshDrawParameters& parameters, std::string& error);
    void selectSceneDynamicLights();
    void synchronizeShadowCasters();
    const AreaReplayCamera* activeAreaReplayCamera() const noexcept;
    neomdl::model::FogSettings effectiveFog() const noexcept;
    std::array<float,3> effectiveSceneAmbient() const noexcept;
    bool effectiveAreaShadows() const noexcept;
    void configurePreviewLights(const std::vector<neomdl::model::Mat4>& transforms);

    void drawReferenceModels();
    std::vector<ParticleScene*> particleScenes();
    bool queueParticles(float elapsedSeconds, bool sceneAnimating, std::string& error);
    void queueChunkModels();
    void queueLightsabers();
    void queueAabbDebug(const neomdl::model::Mesh& mesh,
                        const neomdl::model::Node& node,
                        const neomdl::model::Mat4& world, bool selected,
                        bool kotor2);
    void drawAabbDebugQueue();
    void drawTriggerBucket();
    bool drawTriggerHighlights(bool group);
    void drawLightsaberBucket();
    void drawNodeHelpers(const std::vector<neomdl::model::Mat4>& transforms);
    void drawLightGlyph(const neomdl::model::LightNodeData& light, float scale);
    void drawEmitterGlyph(const neomdl::model::EmitterNodeData& emitter,
                          std::size_t nodeIndex, float scale);
    void drawCameraGlyph(float scale);
    void drawAreaCameraGlyphs();
    void drawReferenceGlyph(bool loaded, float scale);
    void drawTriggerGlyph(bool packedVerified, float scale);
    void drawMesh(const neomdl::model::Mesh& mesh,
                  const neomdl::model::DeformedMesh* deformed,
                  bool selected, const neoshared::material::Material& material,
                  const neomdl::model::NodeMaterialState& nodeMaterial,
                  neomdl::model::GameVersion game, bool modelFog, LightOwnerId lightOwner,
                  const ModelDocument* resources = nullptr,
                  std::vector<neomdl::render::MeshSubmission>* targetQueue = nullptr,
                  std::uint64_t sceneOwner = 0u, float ownerAlpha = 1.0f,
                  std::uint16_t ownerModelClassification = 0u,
                  bool ownerHasTransparency = false,
                  float ownerCameraDistance = 0.0f,
                  bool allowK2SkinEffects = false,
                  const OwnerVisualEffects* ownerEffects = nullptr,
                  const model::Mat4* engineOwnerFrame = nullptr,
                  std::optional<float> ownerTextureAngleDegrees = std::nullopt,
                  const HitPartTransform* enginePartFrame = nullptr,std::string* failure = nullptr,
                  std::optional<float> retainedEyeDepth = std::nullopt);
    const neomdl::model::Bounds& activeBounds() const noexcept;

    std::unique_ptr<wxGLContext> context_;
    bool loading_{};
    PreparedDialoguePlayback dialogueAssets_;
    DialogueRuntime dialogueRuntime_;
    neoshared::audio::AudioPlayback dialogueAudio_;
    ReferenceScene dialogueCameraReferences_;
    model::AnimationRuntime dialogueCameraAnimation_;
    SceneAnimationOwner dialogueCameraAnimationOwner_;
    SceneAnimationOwner externalCameraAnimationOwner_;
    CameraScene dialogueCameraScene_;
    DialogueCameraScene dialogueActorCamera_;
    DialogueCameraSequence dialogueCameraSequence_;
    bool dialogueActorCameraActive_{};
    bool dialogueSavedCamera_{};
    std::optional<CameraAttachment> dialoguePreviousCamera_;
    std::optional<std::size_t> dialoguePreviousAreaCamera_;
    const DialogueLineAsset* dialogueShotActors_{};
    DialogueFade dialogueFade_;
    float dialogueDrawElapsed_{};
    bool dialogueCameraActive_{},dialogueAudioFailed_{},resumeDialogueAudio_{};
    std::string dialogueCameraClip_;
    float dialogueCameraFov_{kK2CameraDefaultViewAngle};
    std::optional<std::size_t> dialogueLipSpeaker_;
    bool dialogueLipSpeakerBound_{};
    bool dialogueMainLip_{};
    std::vector<std::string> dialogueMessages_;
    struct ActiveDialogueAnimation {
        const DialogueActorAnimation* source{};
        const DialogueActorAnimation* loopingSource{};
        std::optional<std::uint64_t> run;
        std::optional<model::EngineFrame> logicalFrame;
        bool currentDialog{};
        std::uint32_t remainingMilliseconds{};
    };
    std::vector<ActiveDialogueAnimation> dialogueActorAnimations_;
    std::map<std::optional<std::size_t>,DialogueParticipantState> dialogueParticipantStates_;
    const DialogueLineAsset* dialogueShotParticipants_{};
    LookAtAnimation dialogueMainLookAt_;
    const DialogueLineAsset* dialoguePreviousActors_{};
    const model::Pose* inspectionReplayPose_{};
    std::vector<model::TriggerPlane> lastScenePlanes_;
    WeatherFrameInput weatherInput_;
    bool weatherUseViewerCamera_{};
    bool weatherUseViewerPosition_{true};
    float weatherShadowAlpha_{0.5f};
    PreparedSceneAudioPlayback sceneAudioAssets_;
    std::unique_ptr<SceneAudioDevice> sceneAudioProvider_;
    SceneAudioPreparedBuffers sceneAudioBuffers_;
    SceneAudioVoices sceneAudioVoices_;
    SceneAudioRuntime sceneAudioRuntime_;
    std::optional<ExoRandom> sceneAudioExo_;
    SceneAudioListener sceneAudioListener_;
    std::uint64_t sceneAudioOneShot_{std::uint64_t{1}<<63};
    bool sceneAudioScheduled_{};
    std::string sceneAudioStatus_;
    const ModelDocument* document_{};
    RenderOptions options_;
    std::vector<OwnerVisualEffects> ownerEffects_;
    SceneEffectsSettings sceneEffects_;
    bool scriptVideoEffect_{}; // K2 ExecuteCommandEnable/DisableVideoEffect GUI latch
    material::ForceSightSettings effectiveForceSight() const noexcept;
    std::uint32_t forceSightMilliseconds_{};
    std::size_t distortionProducerCount_{};
    std::map<std::pair<std::size_t,std::size_t>,float> textureAngles_;
    std::set<std::size_t> selectedNodes_,hiddenNodes_;
    std::optional<std::set<std::size_t>> isolatedNodes_;
    bool meshVisible(std::size_t node) const;
    std::size_t referenceSelectionNode(std::size_t reference) const;
    neomdl::model::AnimationRuntime animationRuntime_;
    SceneAnimationOwner mainAnimationOwner_;
    bool inspectionAnimationsAdvancing_{true};
    bool mainAnimationAdvanced_{};
    std::vector<neomdl::model::AnimationRuntimeEvent>
        pendingAnimationRuntimeEvents_;
    // Retained only for the deterministic single-clip particle replay path.
    // A true run stack has no single source timeline, so explicit seeks reset
    // particle history and resume from the newly composed emitter state.
    std::optional<std::size_t> replayAnimationIndex_;
    float replayAnimationTime_{};
    bool replayAnimationLoop_{};
    std::optional<std::uint64_t> compatibilityRunId_;
    std::optional<neomdl::model::Pose> pose_;
    neomdl::model::DanglySimulationState danglyState_;
    neomdl::model::Bounds sceneBounds_;
    neomdl::render::TextureCache textureCache_;
    // Both games use static PartTriMesh buckets and dynamic Gob buckets;
    // their Gob comparators differ. Remaining inspection draws are separate.
    std::vector<neomdl::render::MeshSubmission> renderQueue_;
    std::vector<bool> ownerOrderingTransparency_;
    std::vector<neomdl::render::MeshSubmission> hologramQueue_;
    std::vector<neomdl::render::MeshSubmission> distortionQueue_;
    std::vector<neomdl::render::MeshSubmission> staticMeshQueue_;
    std::vector<neomdl::render::MeshSubmission> dynamicGobQueue_;
    std::map<LightOwnerId,SceneAnimationOwner*> renderAnimationOwners_;
    SceneEmitterPrewarmTrace emitterPrewarmTrace_;
    SceneEmitterFrameTrace emitterFrameTrace_;
    std::string particleError_;
    std::vector<neomdl::render::MeshSubmission> lightsaberQueue_;
    // Explicit legacy manual skin diagnostic, separate from motionBlurQueue_.
    std::vector<neomdl::render::MeshSubmission> diagnosticSkinBlurQueue_;
    std::vector<AabbDebugSubmission> aabbDebugQueue_;
    std::array<float,9> eyeToWorld_{{1,0,0,0,1,0,0,0,1}};
    bool k2Lights_{};
    bool sceneLightsSelectedThisFrame_{};
    neomdl::model::Mat4 cameraView_=neomdl::model::identityMatrix();
    float pendingProcedureElapsed_{};
    std::string proceduralTextureError_;
    neomdl::render::ShaderMeshRenderer meshRenderer_;
    std::uint64_t poseRevision_{};
    bool pendingGpuReset_{true};
    bool shaderErrorReported_{};
    bool glInitialized_{};
    bool draggingRotate_{};
    bool draggingPan_{};
    std::optional<wxPoint> selectionStart_;
    struct MeshPick {
        wxPoint position;
        float depth{};
        std::optional<std::size_t> node;
        bool additive{};
        std::optional<LightOwnerId> areaOwner;
    };
    std::optional<MeshPick> meshPick_;
    std::set<LightOwnerId> selectedAreaOwners_;
    std::vector<WalkmeshLayer> walkmeshLayers_;
    std::set<WalkmeshFace> walkmeshSelection_,walkmeshSelectionBefore_;
    std::set<WalkmeshVertex> walkmeshVertices_;
    WalkmeshTool walkmeshTool_{WalkmeshTool::View};
    bool walkmeshVisible_{},walkmeshXray_{};
    std::string walkmeshRoom_;
    struct WalkmeshDrag {wxPoint start,current;bool additive{},painting{},vertices{};};
    std::optional<WalkmeshDrag> walkmeshDrag_;
    std::vector<RoomConnection> roomConnections_;
    std::optional<std::size_t> selectedRoomConnection_;
    bool roomConnectionsVisible_{},roomConnectionsXray_{};
    std::optional<RoomMovePreview> roomMovePreview_;
    AreaGizmoMode gizmoMode_{AreaGizmoMode::None};
    model::Vec3 gizmoPosition_{};
    float gizmoHeading_{},gizmoSize_{};
    struct GizmoDrag {
        int handle{}; // X, Y, Z, XY plane, heading ring.
        model::Vec3 position,anchor;
        float heading{},angle{},rotation{};
    };
    std::optional<GizmoDrag> gizmoDrag_;
    AreaSnapSettings areaSnap_;
    std::vector<AreaEditorMarker> areaMarkers_;
    std::string selectedAreaMarker_;
    std::vector<model::Vec3> polygonPoints_;
    bool polygonVertices_{},polygonDrawing_{};
    std::optional<float> polygonPlaneZ_;
    std::optional<std::size_t> polygonVertexLimit_;
    float polygonZOffset_{};
    struct PolygonDrag {std::size_t vertex;model::Vec3 position;};
    std::optional<PolygonDrag> polygonDrag_;
    bool freeFly_{};
    bool flyCollision_{};
    std::array<bool, 6> flyMovement_{};
    std::chrono::steady_clock::time_point cameraTick_{std::chrono::steady_clock::now()};
    model::Vec3 flyPosition_{};
    float flySpeed_{2.0f};
    wxPoint lastMouse_;
    float yaw_{35.0f};
    float pitch_{25.0f};
    float distance_{10.0f};
    ReferenceScene referenceScene_;
    CameraScene cameraScene_;
    ParticleScene particleScene_;
    LightsaberScene lightsaberScene_;
    LightScene lightScene_;
    ShadowScene shadowScene_;
    material::DirtyMaterialSettings dirtySettings_;
    LightOwnerId dirtyOwner_{};
    const model::Mesh* dirtyMesh_{}; // borrowed until document/area replacement
    std::optional<DirtyAreaState> explicitDirtyArea_;
    std::string explicitDirtyIdentity_;
    std::uint64_t dirtyAreaEpoch_{};
    MotionBlurScene motionBlurScene_;
    render::MotionBlurQueue motionBlurQueue_;
    bool pendingMotionHistoryReset_{};
    AreaScene areaScene_;
    float pendingSceneElapsed_{};
    float triggerFrameElapsed_{};
    bool pendingEmitterSceneAnimating_{};
    std::string grassError_;
    WorldPlaybackSettings worldPlaybackSettings_;
    void updateWorldRoomSelection();
    EngineActivePauseClock worldClock_;
    // Visual appearance replacement changes the Gob ID, not the creature's
    // engine object identity or its last-AI clock stamp.
    std::map<HitOwnerId,HitOwnerId> worldClockOwnerKeys_;
    void synchronizeWorldClockCreatures();
    void clearWorldClock();
    void resetWorldClock(bool preservePause);
    void synchronizeWorldPause();
    double worldMicrosecondsRemainder_{};
    float pendingDoorElapsed_{};
    float doorFrameElapsed_{};
    SceneQueries sceneQueries_;
    std::string sceneQueryError_;
    float pendingFlareElapsed_{};
    std::unordered_map<LightOwnerId,LightQuerySphere> ownerLightSpheres_;
    std::vector<LightOwnerId> areaOwnerQueries_;
    float sceneFarClip_{1000.0f};
    std::optional<CameraHitState> pickingCamera_;
    TriggerScene triggerScene_;
    std::map<std::size_t,TriggerMinePreviewInputs> triggerMineInputs_;
    std::map<std::size_t,std::vector<ObjectAnimationCommand>> trapDoorAnimations_;
    std::size_t triggerCullPlane_{};
    std::uint64_t particleRevision_{};
    float pendingLightElapsed_{};
    neomdl::model::Vec3 center_{};
};

} // namespace neomdl::ui
