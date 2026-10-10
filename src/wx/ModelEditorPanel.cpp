#include <model/AsciiModel.hpp>
#include <model/AsciiExport.hpp>
#include <model/PackedExport.hpp>
#include "wx/ModelEditorPanel.hpp"

#include "wx/ModelCanvas.hpp"
#include "wx/TrapRuntimeCapture.hpp"
#include "core/GameResourceSession.hpp"
#include "core/CreatureAppearance.hpp"
#include "core/DialogueAudioSource.hpp"
#include "core/VideoEffects.hpp"
#include "core/ResourceContext.hpp"
#include <model/SceneOrdering.hpp>
#include <neotlk/TlkLookup.hpp>
#include <twoda/TwoDAFile.hpp>

#include <neoshared/PathUtf8.hpp>
#include <neoshared/ResourceDocument.hpp>
#include <neoshared/texture/FileUtil.hpp>
#include "NeoSettings.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoGameInstallResolver.hpp"

#include <wx/aui/auibook.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/clrpicker.h>
#include <wx/spinctrl.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/slider.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/tglbtn.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/utils.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <iomanip>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace neomdl::ui {

static_assert(neomodules::kPanelApiVersion >= 4,
              "NeoMDL requires the NeoShared captured resource-resolver panel API.");

namespace {

enum CommandId {
    ID_Open = wxID_HIGHEST + 23600,
    ID_ExportAscii,
    ID_ExportPacked,
    ID_LoadArea,
    ID_ModuleArea,
    ID_ResourceProvenance,
    ID_SceneShadows,
    ID_ShadowCasters,
    ID_ClearArea,
    ID_AreaRoom,
    ID_AreaEnvironment,
    ID_WorldPlayback,
    ID_AreaAnimation,
    ID_RoomScriptAnimation,
    ID_TrackAnimation,
    ID_DoorState,
    ID_DialoguePlayback,
    ID_AreaWeather,
    ID_SceneAudio,
    ID_LoadGrass,
    ID_GrassOptions,
    ID_CloseModel,
    ID_CloseAll,
    ID_FrameModel,
    ID_Textures,
    ID_Lightmaps,
    ID_Wireframe,
    ID_Lighting,
    ID_Headlight,
    ID_Fog,
    ID_LightingSettings,
    ID_Grid,
    ID_Axes,
    ID_ShowHidden,
    ID_CullBackfaces,
    ID_DanglySimulation,
    ID_NodeHelpers,
    ID_PreviewLights,
    ID_NearestLightsOnlyGobs,
    ID_EngineShadows,
    ID_EngineTwoSidedShadows,
    ID_EngineSoftShadows,
    ID_EngineSoftShadowsATI,
    ID_EngineLensFlares,
    ID_ForceSight,
    ID_InspectMeshProjections,
    ID_PreviewEmitters,
    ID_ShowCameras,
    ID_ViewSelectedCamera,
    ID_ViewCameraHook,
    ID_ViewAreaCamera,
    ID_OrbitCamera,
    ID_CameraSettings,
    ID_ShowReferences,
    ID_PreviewLightsabers,
    ID_LightsaberSettings,
    ID_K2SkinSettings,
    ID_MotionBlurOwners,
    ID_DirtyMaterial,
    ID_DirtyArea,
    ID_EngineExecution,
    ID_EngineExecutionReport,
    ID_OwnerVisualEffects,
    ID_SceneEffects,
    ID_VideoEffectRow,
    ID_RuntimeTextureRotation,
    ID_PreviewAabbs,
    ID_PreviewTriggers,
    ID_TriggerVisualState,
    ID_TriggerMineRuntime,
    ID_CreatureAppearance,
    ID_CreatureLipSync,
    ID_About,
    ID_Exit,
    ID_DetonateEmitters,
    ID_ParticleSettings,
};

class NodeItemData final : public wxTreeItemData {
public:
    explicit NodeItemData(std::size_t value) : index(value) {}
    std::size_t index{};
};

class ModelPage final : public wxPanel {
public:
    ModelPage(wxWindow* parent, std::unique_ptr<ModelDocument> document,
              const RenderOptions& options, bool compact)
        : wxPanel(parent), document_(std::move(document)) {
        timer_.SetOwner(this);

        auto* playback = new wxPanel(this);
        animationChoice_ = new wxChoice(playback, wxID_ANY, wxDefaultPosition,
                                        wxSize(FromDIP(190), wxDefaultCoord));
        animationChoice_->Append("Bind pose");
        for (std::size_t index = 0u; index < document_->animations().size(); ++index) {
            const auto& animation = document_->animation(index);
            const auto& reference = document_->animationReference(index);
            std::string label = animation.name;
            if (reference.modelIndex != 0u) {
                const auto& owner = document_->animationModel(index);
                label += "  [" + (owner.name.empty()
                    ? "supermodel " + std::to_string(reference.modelIndex)
                    : owner.name) + "]";
            }
            animationChoice_->Append(wxui::toWx(label));
        }
        animationChoice_->SetSelection(0);

        runModeChoice_ = new wxChoice(playback, wxID_ANY);
        runModeChoice_->Append("Base");
        runModeChoice_->Append("Queued");
        runModeChoice_->Append("Overlay");
        runModeChoice_->SetSelection(0);
        rateSpin_ = new wxSpinCtrlDouble(playback, wxID_ANY);
        rateSpin_->SetRange(-8.0, 8.0);
        rateSpin_->SetIncrement(0.05);
        rateSpin_->SetDigits(2);
        rateSpin_->SetValue(1.0);
        rateSpin_->SetMinSize(wxSize(FromDIP(82), wxDefaultCoord));
        loopCheck_ = new wxCheckBox(playback, wxID_ANY, "Loop");
        loopCheck_->SetValue(true);
        immediateCheck_ = new wxCheckBox(playback, wxID_ANY, "Immediate");
        immediateCheck_->SetValue(false);
        synchronizeCheck_ = new wxCheckBox(playback, wxID_ANY, "Sync time");
        allowDuplicateCheck_ = new wxCheckBox(playback, wxID_ANY, "Allow duplicate");
        reportLoopCheck_ = new wxCheckBox(playback, wxID_ANY, "Report loops");
        startRunButton_ = new wxButton(playback, wxID_ANY, "Start Run");
        playButton_ = new wxToggleButton(playback, wxID_ANY, "Play");
        clearRunsButton_ = new wxButton(playback, wxID_ANY, "Clear All");

        activeRunChoice_ = new wxChoice(playback, wxID_ANY, wxDefaultPosition,
                                        wxSize(FromDIP(250), wxDefaultCoord));
        activeRunChoice_->Append("No active runs");
        activeRunChoice_->SetSelection(0);
        fadeRunButton_ = new wxButton(playback, wxID_ANY, "Fade Out");
        removeRunButton_ = new wxButton(playback, wxID_ANY, "Remove");
        timeline_ = new wxSlider(playback, wxID_ANY, 0, 0, 1000, wxDefaultPosition,
                                 wxDefaultSize, wxSL_HORIZONTAL);
        timeLabel_ = new wxStaticText(playback, wxID_ANY, "No active run");
        timeLabel_->SetMinSize(wxSize(FromDIP(150), wxDefaultCoord));

        animationChoice_->SetToolTip(
            "Choose the clip used by Start Run. Selecting a clip does not replace existing runs.");
        runModeChoice_->SetToolTip(
            "Engine priority class: base replaces lower base runs, queued starts FIFO, overlay remains above them.");
        immediateCheck_->SetToolTip(
            "Set engine AnimRun flag 0x2: start at full weight instead of using the clip transition time.");
        synchronizeCheck_->SetToolTip(
            "Set engine AnimRun flag 0x10: copy the next lower-priority run's current time when inserted.");
        allowDuplicateCheck_->SetToolTip(
            "Set engine AnimRun flag 0x40: create another run even when the same animation is already active.");
        reportLoopCheck_->SetToolTip(
            "Set engine AnimRun flag 0x4 and record each loop callback in the runtime event log.");
        playButton_->SetToolTip(
            "Advance or pause the complete animation-run list.");
        fadeRunButton_->SetToolTip(
            "Set engine force-stop flag 0x400. Runs with a transition fade out; immediate runs complete on the next animation update.");
        removeRunButton_->SetToolTip(
            "Remove the selected run immediately (engine stop flag 0x80 semantics).");

        auto* clipRow = new wxWrapSizer(wxHORIZONTAL);
        clipRow->Add(new wxStaticText(playback, wxID_ANY, "Animation:"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(5));
        clipRow->Add(animationChoice_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(new wxStaticText(playback, wxID_ANY, "Priority:"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(5));
        clipRow->Add(runModeChoice_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(new wxStaticText(playback, wxID_ANY, "Rate:"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(5));
        clipRow->Add(rateSpin_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(loopCheck_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(immediateCheck_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(synchronizeCheck_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(allowDuplicateCheck_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(reportLoopCheck_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(startRunButton_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(playButton_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(clearRunsButton_, 0,
                     wxALIGN_CENTER_VERTICAL | wxBOTTOM, FromDIP(6));

        auto* runRow = new wxBoxSizer(wxHORIZONTAL);
        runRow->Add(new wxStaticText(playback, wxID_ANY, "Active run:"), 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        runRow->Add(activeRunChoice_, 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        runRow->Add(fadeRunButton_, 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        runRow->Add(removeRunButton_, 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        runRow->Add(timeline_, 1,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        runRow->Add(timeLabel_, 0, wxALIGN_CENTER_VERTICAL);

        auto* playbackLayout = new wxBoxSizer(wxVERTICAL);
        playbackLayout->Add(clipRow, 0, wxEXPAND);
        playbackLayout->Add(runRow, 0, wxEXPAND | wxTOP, FromDIP(2));
        playback->SetSizer(playbackLayout);

        splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                         wxSP_LIVE_UPDATE | wxSP_3D);
        auto* inspector = new wxPanel(splitter_);
        tree_ = new wxTreeCtrl(inspector, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE |
                                   wxTR_HIDE_ROOT);
        details_ = new wxTextCtrl(inspector, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        auto* inspectorLayout = new wxBoxSizer(wxVERTICAL);
        inspectorLayout->Add(tree_, 3, wxEXPAND);
        inspectorLayout->Add(details_, 2, wxEXPAND | wxTOP, FromDIP(5));
        inspector->SetSizer(inspectorLayout);

        canvas_ = new ModelCanvas(splitter_);
        canvas_->setDocument(document_.get());
        canvas_->setOptions(options);
        splitter_->SetMinimumPaneSize(FromDIP(160));
        splitter_->SplitVertically(inspector, canvas_, FromDIP(compact ? 220 : 300));
        splitter_->SetSashGravity(0.0);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(playback, 0, wxEXPAND | wxALL, FromDIP(5));
        layout->Add(splitter_, 1, wxEXPAND);
        SetSizer(layout);

        populateTree();
        updateDetails(std::nullopt);
        updateAnimationControls();
        tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent& event) {
            auto* data = dynamic_cast<NodeItemData*>(tree_->GetItemData(event.GetItem()));
            selectedNode_ = data ? std::optional<std::size_t>(data->index) : std::nullopt;
            canvas_->setSelectedNode(selectedNode_);
            updateDetails(selectedNode_);
            event.Skip();
        });
        animationChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            updateAnimationControls();
            updateDetails(selectedNode_);
        });
        startRunButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (startAnimationRun()) {
                startPlayback();
                updateAnimationControls();
                updateDetails(selectedNode_);
            }
        });
        playButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
            if (event.IsChecked()) startPlayback();
            else stopPlayback();
            updateAnimationControls();
        });
        activeRunChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            updateAnimationControls();
            updateDetails(selectedNode_);
        });
        rateSpin_->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
            if (const auto id = selectedRunId())
                canvas_->setAnimationRunPlaybackRate(*id,
                    static_cast<float>(rateSpin_->GetValue()));
            updateAnimationControls();
            updateDetails(selectedNode_);
        });
        loopCheck_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
            if (const auto id = selectedRunId())
                canvas_->setAnimationRunLooping(*id, event.IsChecked());
            updateAnimationControls();
            updateDetails(selectedNode_);
        });
        timeline_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
            const auto id = selectedRunId();
            const auto* run = selectedRun();
            const auto length = run ? animationRunLength(*run) : 0.0f;
            if (id && length > 0.0f) {
                const float time = length *
                    static_cast<float>(timeline_->GetValue()) / 1000.0f;
                canvas_->setAnimationRunTime(*id, time, true);
            }
            lastTick_ = std::chrono::steady_clock::now();
            drainAnimationEvents();
            updateAnimationControls();
            updateDetails(selectedNode_);
        });
        fadeRunButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (const auto id = selectedRunId()) {
                canvas_->stopAnimationRun(*id, true);
                drainAnimationEvents();
                startPlayback();
                updateAnimationControls();
                updateDetails(selectedNode_);
            }
        });
        removeRunButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const auto id = selectedRunId();
            if (id) canvas_->removeAnimationRun(*id);
            drainAnimationEvents();
            rebuildRunChoice();
            if (canvas_->animationRuns().empty()) stopPlayback();
            updateAnimationControls();
            updateDetails(selectedNode_);
        });
        clearRunsButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            canvas_->clearAnimationRuns();
            stopPlayback();
            eventLog_.clear();
            rebuildRunChoice();
            updateAnimationControls();
            updateDetails(selectedNode_);
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { advancePlayback(); });
        lastTick_ = std::chrono::steady_clock::now();
        timer_.Start(16);
    }

    ~ModelPage() override { timer_.Stop(); }

    void suspendLoadingPlayback() {
        ++loadingSuspensions_;
        timer_.Stop();
        canvas_->setLoading(true);
    }
    void resumeLoadingPlayback() {
        if (--loadingSuspensions_ != 0u) return;
        canvas_->setLoading(false);
        lastTick_ = std::chrono::steady_clock::now();
        timer_.Start(16);
    }

    const ModelDocument& document() const noexcept { return *document_; }
    ModelDocument& document() noexcept { return *document_; }
    void setResourceSession(std::shared_ptr<GameResourceSession> session) {resourceSession_=std::move(session);}
    ResourceResolver voiceResolver() const {
        return resourceSession_?resourceSession_->voiceResolver():ResourceResolver{};
    }
    const std::shared_ptr<GameResourceSession>& resourceSession() const {return resourceSession_;}
    void setOptions(const RenderOptions& options) {
        canvas_->setOptions(options);
        updateAnimationControls();
    }
    const OwnerVisualEffects& ownerVisualEffects(std::size_t owner) const { return canvas_->ownerVisualEffects(owner); }
    bool setOwnerVisualEffects(std::size_t owner,const OwnerVisualEffects& effects,std::string& error) {
        const auto ok=canvas_->setOwnerVisualEffects(owner,effects,error);
        if(ok) updateDetails(selectedNode_);
        return ok;
    }
    const SceneEffectsSettings& sceneEffects() const noexcept { return canvas_->sceneEffects(); }
    bool setSceneEffects(SceneEffectsSettings settings,std::string& error,bool resetHistory=true) {
        return canvas_->setSceneEffects(std::move(settings),error,resetHistory);
    }
    void setScriptVideoEffect(bool enabled) {canvas_->setScriptVideoEffect(enabled);}
    float textureAngle(std::size_t owner,std::size_t node) const {return canvas_->textureAngle(owner,node);}
    bool setTextureAngle(std::size_t owner,std::size_t node,float degrees,std::string& error) {
        return canvas_->setTextureAngle(owner,node,degrees,error);
    }
    void frameModel() { canvas_->frameModel(); }
    AreaScene prepareAreaLoad() { return canvas_->prepareAreaLoad(); }
    void installArea(AreaScene pending,PreparedSceneAudioPlayback sounds={}) {
        canvas_->installArea(std::move(pending),std::move(sounds));
    }
    void configureSceneAudio(std::optional<ExoRandom::State> state) {canvas_->configureSceneAudio(state);}

    void stopSceneAudio() {canvas_->stopSceneAudio();}
    void ensureSceneAudioProvider() {canvas_->ensureSceneAudioProvider();}
    std::optional<ExoRandom::State> sceneAudioRandomState() const {return canvas_->sceneAudioRandomState();}
    const std::string& sceneAudioStatus() const {return canvas_->sceneAudioStatus();}
    const ShadowScene& shadowScene() const noexcept {return canvas_->shadowScene();}
    const MotionBlurScene& motionBlurScene() const noexcept {return canvas_->motionBlurScene();}
    void setMotionBlurOwner(LightOwnerId owner,MotionBlurOwnerSettings settings) {canvas_->setMotionBlurOwner(owner,settings);}
    void resetMotionBlurHistory() {canvas_->resetMotionBlurHistory();}
    void setSceneOwnerWorld(LightOwnerId owner,const model::Mat4& world) {canvas_->setSceneOwnerWorld(owner,world);}
    const material::DirtyMaterialSettings& dirtySettings() const noexcept {return canvas_->dirtySettings();}
    void setDirtyMaterial(LightOwnerId owner,std::size_t mesh,material::DirtyMaterialSettings settings) {canvas_->setDirtyMaterial(owner,mesh,settings);}
    void disableDirtyMaterial() {canvas_->disableDirtyMaterial();canvas_->Refresh(false);}
    void loadDirtyArea(const std::filesystem::path& path) {canvas_->loadDirtyArea(path);}
    void useLoadedDirtyArea() {canvas_->useLoadedDirtyArea();}
    DirtyAreaState dirtyAreaState() const {return canvas_->dirtyAreaState();}
    std::string engineExecutionReport() const {return canvas_->engineExecutionReport();}
    void setSceneShadowInputs(SceneShadowInputs inputs) {canvas_->setSceneShadowInputs(std::move(inputs));}
    void setSceneCasterEnabled(LightOwnerId owner,bool enabled) {canvas_->setSceneCasterEnabled(owner,enabled);}
    void clearArea() { canvas_->clearArea(); }
    const AreaScene& areaScene() const noexcept { return canvas_->areaScene(); }
    void setAreaRoom(std::optional<LightRoomId> id) { canvas_->setAreaRoom(id); }
    void useReplayedAreaRoom() {canvas_->useReplayedAreaRoom();}
    const WorldPlaybackSettings& worldPlaybackSettings() const noexcept {return canvas_->worldPlaybackSettings();}
    void setWorldPlaybackSettings(WorldPlaybackSettings settings) {canvas_->setWorldPlaybackSettings(std::move(settings));}
    std::uint8_t worldPauseMask() const noexcept {return canvas_->worldPauseMask();}
    std::vector<HitOwnerId> worldPauseExclusions() const {return canvas_->worldPauseExclusions();}
    void setWorldPauseState(std::uint8_t mask,int enable,const std::vector<HitOwnerId>& exclusions) {
        canvas_->setWorldPauseState(mask,enable,exclusions);
    }
    void resetWorldTime() {canvas_->resetWorldTime();}
    bool applyAreaEnvironmentReplay(AreaEnvironmentReplayState state,std::string& error) {
        return canvas_->applyAreaEnvironmentReplay(std::move(state),error);
    }
    void clearAreaEnvironmentReplay() {canvas_->clearAreaEnvironmentReplay();}
    void loadGrassArea(const std::filesystem::path& path) {canvas_->loadGrassArea(path);}
    void configureGrass(const GrassSettings& settings) {canvas_->configureGrass(settings);}
    bool playAreaAnimation(std::size_t instance,const std::string& clip,float rate,std::string& error) {
        return canvas_->playAreaAnimation(instance,clip,rate,error);
    }
    void playRoomAnimation(const std::string& room,std::int32_t number) {canvas_->playRoomAnimation(room,number);}
    void captureTrackAnimation(std::size_t track,TrackAnimationState state) {canvas_->captureTrackAnimation(track,std::move(state));}
    void startTrackAnimation(std::size_t track) {canvas_->startTrackAnimation(track);}
    void stopTrackAnimation(std::size_t track) {canvas_->stopTrackAnimation(track);}
    void startMiniGameTracks() {canvas_->startMiniGameTracks();}
    void stopMiniGameTracks() {canvas_->stopMiniGameTracks();}
    void setTrackSpeed(std::size_t track,float speed) {canvas_->setTrackSpeed(track,speed);}
    void playTrackFollowerAnimation(std::size_t track,const std::string& name,int loop,int queued,int overlay) {
        canvas_->playTrackFollowerAnimation(track,name,loop,queued,overlay);
    }
    void removeTrackFollowerAnimation(std::size_t track,const std::string& name) {canvas_->removeTrackFollowerAnimation(track,name);}
    void pauseAreaObject(std::size_t instance,bool paused) {canvas_->pauseAreaObject(instance,paused);}
    void resetAreaObject(std::size_t instance) {canvas_->resetAreaObject(instance);}
    bool setDoorOpenState(std::size_t instance,std::uint8_t state,std::int32_t selector,std::string& error) {
        return canvas_->setDoorOpenState(instance,state,selector,error);
    }
    void startDialogue(PreparedDialoguePlayback playback,std::size_t entry) {canvas_->startDialogue(std::move(playback),entry);}
    void stopDialogue() {canvas_->stopDialogue();}
    bool dialogueFading() const {return canvas_->dialogueFading();}
    void chooseDialogueReply(std::size_t reply) {canvas_->chooseDialogueReply(reply);}
    const DialogueRuntime& dialogue() const noexcept {return canvas_->dialogue();}
    void configureWeather(PreparedWeatherPlayback prepared,WeatherFrameInput input,bool camera,
                          std::optional<std::uint8_t> command) {canvas_->configureWeather(std::move(prepared),input,camera,command);}
    const WeatherFrameInput& weatherInputs() const noexcept {return canvas_->weatherInputs();}
    bool weatherUsesViewerCamera() const noexcept {return canvas_->weatherUsesViewerCamera();}
    float weatherShadowAlpha() const noexcept {return canvas_->weatherShadowAlpha();}
    std::optional<std::size_t> applyCreatureAppearance(std::size_t instance,
        const CreatureAppearanceState& state,std::string& error) {
        const auto result=canvas_->applyCreatureAppearance(instance,state,error);
        if(result)updateDetails(selectedNode_);
        return result;
    }
    bool loadLipSync(std::optional<std::size_t> instance,const std::filesystem::path& path,
                     float delay,std::string& error) {
        if(!canvas_->loadLipSync(instance,path,delay,error))return false;
        if(!instance) {
            lastTick_=std::chrono::steady_clock::now();
            playing_=!canvas_->animationRuns().empty();
            playButton_->SetValue(playing_);updateAnimationControls();
        }
        updateDetails(selectedNode_);return true;
    }
    void stopLipSync(std::optional<std::size_t> instance) {
        canvas_->stopLipSync(instance);
        if(!instance)updateAnimationControls();
        updateDetails(selectedNode_);
    }
    void detonateEmitters() { canvas_->detonateEmitters();updateDetails(selectedNode_); }
    bool canViewThroughSelectedCamera() const noexcept {
        return canvas_->cameraScene().engineRuntime() &&
               selectedNode_ && *selectedNode_ < document_->model().nodes.size() &&
               document_->model().nodes[*selectedNode_].camera.has_value();
    }
    bool hasCameraHook() const noexcept {
        return canvas_->cameraScene().engineHook() != nullptr;
    }
    bool authoredCameraActive() const noexcept {
        return canvas_->authoredCameraActive();
    }
    const CameraScene& cameraScene() const noexcept {return canvas_->cameraScene();}
    const TriggerScene& triggerScene() const noexcept {return canvas_->triggerScene();}
    TriggerMinePreviewInputs triggerMineInputs(std::size_t source) const {return canvas_->triggerMineInputs(source);}
    bool setTriggerMineInputs(std::size_t source,bool flagged,TriggerMinePreviewInputs input) {
        return canvas_->setTriggerMineInputs(source,flagged,std::move(input));
    }
    bool deleteTriggerMine(std::size_t source,std::uint8_t reason) {return canvas_->deleteTriggerMine(source,reason);}
    void setTrapInventory(TrapInventoryState state) {canvas_->setTrapInventory(std::move(state));}
    std::size_t addTriggerMine(PreparedTriggerMine prepared,PreparedSceneAudioPlayback sounds) {
        return canvas_->addTriggerMine(std::move(prepared),std::move(sounds));
    }
    bool enterTriggerMine(std::size_t source,const TriggerMineEnterInput& input) {return canvas_->enterTriggerMine(source,input);}
    bool resumeTriggerMineEntry() {return canvas_->resumeTriggerMineEntry();}
    TrapActionResult completeTrapAction(TrapObjectTarget target,TrapAction action,const TrapActionInput& input) {
        return canvas_->completeTrapAction(target,action,input);
    }
    TrapSetMineResult completeSetMine(const TrapSetMinePlan& plan,std::optional<PreparedTriggerMine> mine,PreparedSceneAudioPlayback sounds,PreparedTrapDoorAnimations doors) {
        return canvas_->completeSetMine(plan,std::move(mine),std::move(sounds),std::move(doors));
    }
    bool setTriggerCreator(std::size_t source,std::uint64_t id) {return canvas_->setTriggerCreator(source,id);}
    bool setObjectTrapFlag(TrapObjectTarget target,std::uint8_t flag) {return canvas_->setObjectTrapFlag(target,flag);}
    bool setObjectTrapId(TrapObjectTarget target,std::uint64_t id) {return canvas_->setObjectTrapId(target,id);}
    bool captureObjectLock(TrapObjectTarget target,const AreaObjectTrapRecord& state) {return canvas_->captureObjectLock(target,state);}
    bool setTriggerObjectId(std::size_t source,std::uint64_t id) {return canvas_->setTriggerObjectId(source,id);}
    TriggerDetectionResult checkTriggerDetection(const TriggerDetectionInput& input) {return canvas_->checkTriggerDetection(input);}
    void captureTriggerDetection(TriggerDetectionInput input) {canvas_->captureTriggerDetection(std::move(input));}
    void clearTriggerDetectionCaptures() {canvas_->clearTriggerDetectionCaptures();}
    bool setTriggerClientVisualState(std::size_t index,const TriggerClientVisualState& state) {
        return canvas_->setTriggerClientVisualState(index,state);
    }
    bool viewThroughSelectedCamera() {
        if (!canViewThroughSelectedCamera()) return false;
        const bool activated = canvas_->activateCameraNode(*selectedNode_);
        updateDetails(selectedNode_);
        return activated;
    }
    bool viewThroughCameraHook() {
        const bool activated = canvas_->activateCameraHook();
        updateDetails(selectedNode_);
        return activated;
    }
    bool viewThroughAreaCamera(std::size_t index) {
        const bool activated=canvas_->activateAreaCamera(index);
        updateDetails(selectedNode_);return activated;
    }
    void useOrbitCamera() {
        canvas_->useOrbitCamera();
        updateDetails(selectedNode_);
    }
    void refreshDetails() { updateDetails(selectedNode_); }
    void setAppearance(bool dark) {
        wxui::applyTheme(this, dark);
        canvas_->Refresh(false);
    }

private:
    std::optional<std::size_t> selectedAnimation() const {
        const auto selection = animationChoice_
            ? animationChoice_->GetSelection() : wxNOT_FOUND;
        if (selection <= 0) return std::nullopt;
        const auto index = static_cast<std::size_t>(selection - 1);
        return index < document_->animations().size()
            ? std::optional<std::size_t>(index) : std::nullopt;
    }

    float animationRunLength(
        const neomdl::model::AnimationRun& run) const noexcept {
        if (run.outOfOrder()) return run.outOfOrderDuration;
        if (run.animation.modelIndex >= document_->modelHierarchySize())
            return 0.0f;
        const auto& owner = document_->modelAt(run.animation.modelIndex);
        return run.animation.animationIndex < owner.animations.size()
            ? owner.animations[run.animation.animationIndex].length : 0.0f;
    }

    std::optional<std::uint64_t> selectedRunId() const noexcept {
        if (!activeRunChoice_) return std::nullopt;
        const int selection = activeRunChoice_->GetSelection();
        if (selection == wxNOT_FOUND || selection < 0 ||
            static_cast<std::size_t>(selection) >= runChoiceIds_.size()) {
            return std::nullopt;
        }
        return runChoiceIds_[static_cast<std::size_t>(selection)];
    }

    const neomdl::model::AnimationRun* selectedRun() const noexcept {
        const auto id = selectedRunId();
        return id ? canvas_->animationRun(*id) : nullptr;
    }

    void rebuildRunChoice(
        std::optional<std::uint64_t> preferred = std::nullopt) {
        if (!activeRunChoice_) return;
        const auto previous = preferred ? preferred : selectedRunId();
        std::vector<std::uint64_t> ids;
        std::vector<std::string> labels;
        ids.reserve(canvas_->animationRuns().size());
        labels.reserve(canvas_->animationRuns().size());
        for (const auto& run : canvas_->animationRuns()) {
            ids.push_back(run.id);
            std::ostringstream label;
            label << '#' << run.id << "  ["
                  << neomdl::model::animationRunPriorityName(run)
                  << "]  " << run.name;
            if (run.queued()) label << "  (waiting)";
            if (run.transitionComplete) label << "  (finishing)";
            labels.push_back(label.str());
        }

        const bool rebuild = ids != runChoiceIds_ ||
                             labels != runChoiceLabels_;
        if (rebuild) {
            activeRunChoice_->Freeze();
            activeRunChoice_->Clear();
            runChoiceIds_ = ids;
            runChoiceLabels_ = labels;
            if (runChoiceIds_.empty()) {
                activeRunChoice_->Append("No active runs");
            } else {
                for (const auto& label : runChoiceLabels_)
                    activeRunChoice_->Append(wxui::toWx(label));
            }
            activeRunChoice_->Thaw();
        }

        int selection = wxNOT_FOUND;
        if (previous) {
            const auto found = std::find(runChoiceIds_.begin(),
                                         runChoiceIds_.end(), *previous);
            if (found != runChoiceIds_.end()) {
                selection = static_cast<int>(
                    std::distance(runChoiceIds_.begin(), found));
            }
        }
        if (selection == wxNOT_FOUND) {
            if (const auto primary = canvas_->primaryAnimationRunId()) {
                const auto found = std::find(runChoiceIds_.begin(),
                                             runChoiceIds_.end(), *primary);
                if (found != runChoiceIds_.end()) {
                    selection = static_cast<int>(
                        std::distance(runChoiceIds_.begin(), found));
                }
            }
        }
        if (selection == wxNOT_FOUND) selection = 0;
        if (activeRunChoice_->GetCount() != 0u)
            activeRunChoice_->SetSelection(selection);
        activeRunChoice_->Enable(!runChoiceIds_.empty());
    }

    bool startAnimationRun() {
        const auto animation = selectedAnimation();
        if (!animation) return false;

        std::uint32_t flags = 0u;
        if (!loopCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunNonLooping;
        if (immediateCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunImmediate;
        if (synchronizeCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunSynchronizeTime;
        if (allowDuplicateCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunAllowDuplicate;
        if (reportLoopCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunReportLoop;

        const bool engineRuntime = neomdl::model::engineAnimationRuntimeSupported(
            document_->model().game);
        if (engineRuntime) {
            if (runModeChoice_->GetSelection() == 1)
                flags |= neomdl::model::kAnimationRunQueued;
            else if (runModeChoice_->GetSelection() == 2)
                flags |= neomdl::model::kAnimationRunOverlay;
        } else {
            // Keep an immediate-run fallback for unsupported model types.
            canvas_->clearAnimationRuns();
            flags |= neomdl::model::kAnimationRunImmediate;
        }

        const auto result = canvas_->playAnimationRun(
            *animation, static_cast<float>(rateSpin_->GetValue()), flags);
        drainAnimationEvents();
        if (!result) {
            if (!result.diagnostic.empty()) {
                wxMessageBox(wxui::toWx(result.diagnostic),
                             "Animation Run Was Not Started",
                             wxOK | wxICON_WARNING, this);
            }
            return false;
        }
        rebuildRunChoice(result.runId);
        return result.runId.has_value();
    }

    void startPlayback() {
        if (canvas_->animationRuns().empty() && !startAnimationRun()) {
            if (playButton_) playButton_->SetValue(false);
            playing_ = false;
            return;
        }
        lastTick_ = std::chrono::steady_clock::now();
        playing_ = !canvas_->animationRuns().empty();
        if (playButton_) playButton_->SetValue(playing_);
    }

    void stopPlayback() {
        playing_ = false;
        if (playButton_) playButton_->SetValue(false);
    }

    void recordEvents(
        const std::vector<neomdl::model::AnimationRuntimeEvent>& events) {
        for (const auto& event : events) {
            std::ostringstream text;
            text << std::fixed << std::setprecision(3) << event.time
                 << " s  ["
                 << neomdl::model::animationRuntimeEventName(event.kind)
                 << "]  #" << event.runId << ' ' << event.animationName;
            if (!event.name.empty()) text << "  " << event.name;
            eventLog_.push_back(text.str());
        }
    }

    bool drainAnimationEvents() {
        auto events = canvas_->takeAnimationRuntimeEvents();
        if (events.empty()) return false;
        recordEvents(events);
        return true;
    }

    void advancePlayback() {
        if (loadingSuspensions_ != 0u) return;
        const auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - lastTick_).count();
        lastTick_ = now;
        if (!std::isfinite(elapsed) || elapsed < 0.0f) elapsed = 0.0f;

        const bool noRuns = canvas_->animationRuns().empty();
        canvas_->advance(elapsed, playing_ || noRuns, playing_);
        bool eventsChanged = drainAnimationEvents();
        for(auto& message:canvas_->takeDialogueMessages()) {
            eventLog_.push_back(std::move(message));eventsChanged=true;
        }
        if(eventsChanged)recordEvents({});
        if (playing_ && canvas_->animationRuns().empty()) stopPlayback();
        if (playing_ || eventsChanged) {
            rebuildRunChoice();
            updateAnimationControls();
            updateDetails(selectedNode_);
        }
    }

    void updateAnimationControls() {
        rebuildRunChoice();
        const bool engineRuntime = neomdl::model::engineAnimationRuntimeSupported(
            document_->model().game);
        const auto animation = selectedAnimation();
        const auto* run = selectedRun();
        const bool hasRuns = !canvas_->animationRuns().empty();

        startRunButton_->Enable(animation.has_value());
        playButton_->Enable(hasRuns || animation.has_value());
        clearRunsButton_->Enable(hasRuns);
        runModeChoice_->Enable(engineRuntime && animation.has_value());
        immediateCheck_->Enable(engineRuntime && animation.has_value());
        synchronizeCheck_->Enable(engineRuntime && animation.has_value());
        allowDuplicateCheck_->Enable(engineRuntime && animation.has_value());
        reportLoopCheck_->Enable(engineRuntime && animation.has_value());
        if (!engineRuntime) {
            runModeChoice_->SetSelection(0);
            immediateCheck_->SetValue(true);
            synchronizeCheck_->SetValue(false);
            allowDuplicateCheck_->SetValue(false);
            reportLoopCheck_->SetValue(false);
        }

        fadeRunButton_->Enable(run != nullptr);
        removeRunButton_->Enable(run != nullptr);
        timeline_->Enable(run != nullptr && animationRunLength(*run) > 0.0f);
        loopCheck_->Enable(run ? !run->outOfOrder() : animation.has_value());
        rateSpin_->Enable(run != nullptr || animation.has_value());

        if (run) {
            const float length = animationRunLength(*run);
            const float time = length > 0.0f
                ? std::clamp(run->time, 0.0f, length) : 0.0f;
            const auto slider = length > 0.0f
                ? static_cast<int>(std::lround(time * 1000.0f / length)) : 0;
            timeline_->SetValue(std::clamp(slider, 0, 1000));
            if (wxWindow::FindFocus() != rateSpin_)
                rateSpin_->SetValue(run->playbackRate);
            if (!run->outOfOrder()) loopCheck_->SetValue(run->looping());
            std::ostringstream label;
            label << std::fixed << std::setprecision(3)
                  << time << " / " << length << " s  w "
                  << std::setprecision(2) << run->weight;
            timeLabel_->SetLabel(wxui::toWx(label.str()));
        } else {
            timeline_->SetValue(0);
            timeLabel_->SetLabel("No active run");
        }

        if (playing_ && !hasRuns) playing_ = false;
        playButton_->SetValue(playing_);
        playButton_->SetLabel(playing_ ? "Pause" : "Play");
        if (auto* parent = timeLabel_->GetParent()) parent->Layout();
    }

    wxTreeItemId appendNode(const wxTreeItemId& parent, std::size_t index) {
        if (index >= document_->model().nodes.size()) return {};
        const auto& node = document_->model().nodes[index];
        wxString label = wxui::toWx(node.name);
        if ((node.flags & neomdl::model::kNodeMesh) != 0u) label += "  [mesh]";
        if (node.mesh && *node.mesh < document_->model().meshes.size() &&
            document_->model().meshes[*node.mesh].dangly) label += " [dangly]";
        if (node.light) label += " [light]";
        if (node.emitter) label += " [emitter]";
        if (node.camera) label += " [camera]";
        if (node.reference) label += " [reference]";
        if (node.lightsaber) label += " [lightsaber]";
        if (node.trigger) label += " [trigger]";
        const auto item = tree_->AppendItem(parent, label, -1, -1, new NodeItemData(index));
        for (const auto child : node.children) appendNode(item, child);
        return item;
    }

    void populateTree() {
        tree_->DeleteAllItems();
        const auto root = tree_->AddRoot("model");
        bool appended = false;
        for (std::size_t index = 0; index < document_->model().nodes.size(); ++index) {
            if (!document_->model().nodes[index].parent) {
                const auto item = appendNode(root, index);
                if (item.IsOk()) {
                    tree_->Expand(item);
                    appended = true;
                }
            }
        }
        if (!appended) {
            for (std::size_t index = 0; index < document_->model().nodes.size(); ++index)
                appendNode(root, index);
        }
        tree_->Expand(root);
    }

    void updateDetails(std::optional<std::size_t> selected) {
        std::ostringstream text;
        text << neomdl::model::modelSummary(document_->model()) << "\n";
        if (document_->model().game == neomdl::model::GameVersion::Kotor2) {
            const auto& seams = document_->model().skinSeams;
            text << "K2 ProcessSkinSeams: "
                 << (seams.processed ? "processed" : "skipped for compiled model")
                 << " (" << seams.skinMeshes << " skin meshes, "
                 << seams.matchedGroups << " matched groups, "
                 << seams.adjustedVertices << " normal writes, "
                 << seams.candidateComparisons << " comparisons)\n";
        } else {
            text << "K1 rendering: engine desktop materials, skinning and scene effects; engine Gob ordering\n";
        }
        if (!document_->sourceDescription().empty())
            text << "Source: " << document_->sourceDescription() << "\n";
        text << "Decoded textures: " << document_->loadedTextureCount() << "\n";
        text << "Resolved reference instances: " << document_->references().size() << "\n";
        text << "Trigger markers / runtime volumes: "
             << canvas_->triggerScene().modelMarkerCount() << " / "
             << canvas_->triggerScene().volumes().size() << "\n";
        const auto inheritedCount = static_cast<std::size_t>(std::count_if(
            document_->animations().begin(), document_->animations().end(),
            [](const auto& reference) { return reference.modelIndex != 0u; }));
        text << "Model hierarchy: " << document_->modelHierarchySize() << " model(s), "
             << document_->supermodelCount() << " supermodel(s)\n"
             << "Resolved animations: " << document_->animations().size() << " ("
             << document_->animations().size() - inheritedCount << " local, "
             << inheritedCount << " inherited)\n";
        if (document_->supermodelCount() != 0u) {
            text << "Supermodel chain:\n";
            for (std::size_t index = 1u; index < document_->modelHierarchySize(); ++index) {
                const auto& model = document_->modelAt(index);
                text << "  " << index << ": "
                     << (model.name.empty() ? "(unnamed)" : model.name)
                     << "  [animationScale " << model.animationScale << "]\n";
            }
        }
        const auto& animationRuns = canvas_->animationRuns();
        text << "Animation runtime: "
             << (neomdl::model::engineAnimationRuntimeSupported(
                     document_->model().game)
                     ? "engine AnimRun list" : "single-run compatibility")
             << "\n"
             << "Active runs: " << animationRuns.size() << "\n";
        if (!animationRuns.empty()) {
            text << "Run priority (highest to lowest; evaluated lowest to highest):\n";
            const auto selectedRunValue = selectedRunId();
            for (const auto& run : animationRuns) {
                const float length = animationRunLength(run);
                text << (selectedRunValue && *selectedRunValue == run.id ? "  > " : "    ")
                     << '#' << run.id << " ["
                     << neomdl::model::animationRunPriorityName(run) << "] "
                     << run.name << "  time " << run.time << " / " << length
                     << "  weight " << run.weight
                     << "  transition " << run.transitionTime
                     << "  rate " << run.playbackRate
                     << "  flags 0x" << std::hex << run.flags << std::dec;
                if (run.queued()) text << "  waiting";
                if ((run.flags & neomdl::model::kAnimationRunForceStop) != 0u)
                    text << "  fading-out";
                if (run.transitionComplete) text << "  complete";
                text << "\n";
            }
        }
        if (const auto* pose = canvas_->pose(); pose && pose->controlledRootNode &&
            *pose->controlledRootNode < document_->model().nodes.size()) {
            text << "Last controlled target subtree: "
                 << document_->model().nodes[*pose->controlledRootNode].name << "\n";
        }
        if (const auto animationIndex = selectedAnimation()) {
            const auto& animation = document_->animation(*animationIndex);
            const auto& reference = document_->animationReference(*animationIndex);
            const auto& owner = document_->animationModel(*animationIndex);
            std::size_t controllerCount = 0u;
            for (const auto& node : animation.nodes) controllerCount += node.controllers.size();
            text << "Selected animation clip: " << animation.name << "\n"
                 << "Source model: "
                 << (owner.name.empty() ? "(unnamed)" : owner.name)
                 << (reference.modelIndex == 0u ? " (local)" : " (supermodel)") << "\n"
                 << "Inheritance depth: " << reference.inheritanceDepth << "\n"
                 << "Inherited position scale: " << reference.positionScale << "\n"
                 << "Length / transition: " << animation.length << " / "
                 << animation.transitionTime << " s\n"
                 << "Root selector: "
                 << (animation.root.empty() ? "(none)" : animation.root) << "\n"
                 << "Animation nodes/controllers: " << animation.nodes.size() << " / "
                 << controllerCount << "\n"
                 << "Events: " << animation.events.size() << "\n";
        }
        if (!eventLog_.empty()) {
            text << "Recent animation runtime events:\n";
            for (const auto& event : eventLog_) text << "  " << event << "\n";
        }
        if (selected && *selected < document_->model().nodes.size()) {
            const auto& node = document_->model().nodes[*selected];
            text << "\nNode: " << node.name << "\n"
                 << "ID: " << node.id << "\n"
                 << "Types: " << neomdl::model::nodeFlagsText(node.flags) << "\n"
                 << "Position: " << node.position.x << ", " << node.position.y << ", "
                 << node.position.z << "\n"
                 << "Children: " << node.children.size() << "\n"
                 << "Base controllers: " << node.controllers.size() << "\n";
            if (node.mesh && *node.mesh < document_->model().meshes.size()) {
                const auto& mesh = document_->model().meshes[*node.mesh];
                text << "Vertices: " << mesh.vertices.size() << "\n"
                     << "Triangle faces: " << (mesh.animated ? mesh.faces.size() : mesh.indices.size() / 3u) << "\n"
                     << "Diffuse texture: " << (mesh.texture0.empty() ? "(none)" : mesh.texture0) << "\n"
                     << "Secondary texture: " << (mesh.texture1.empty() ? "(none)" : mesh.texture1) << "\n"
                     << "Render flag: " << (mesh.render ? "on" : "off") << "\n"
                     << "Skin node: " << (mesh.skinned ? "yes" : "no") << "\n";
                const auto* activePose = canvas_->pose();
                const auto state = activePose && *selected < activePose->nodeMaterials.size()
                    ? activePose->nodeMaterials[*selected]
                    : neomdl::model::bindNodeMaterial(node);
                neomdl::material::SurfaceInputs surface;
                surface.opacity = state.alpha;
                surface.skinned = mesh.skinned;
                surface.selfIllumination = state.selfIllumination;
                surface.vertexAlpha = mesh.hasVertexColors && std::any_of(
                    mesh.vertices.begin(), mesh.vertices.end(),
                    [](const auto& vertex) { return vertex.color[3] != 255u; });
                neomdl::material::RuntimeMaterial rendered;
                std::optional<neomdl::material::K2PartTriMeshSelection>
                    k2Selection;
                if(document_->model().game==neomdl::model::GameVersion::Kotor2) {
                    neomdl::material::K2PartTriMeshInputs inputs;
                    inputs.engineHardware=canvas_->options().engineHardware;
                    inputs.lightmapped=mesh.lightmapped;
                    inputs.enableLightmap=canvas_->options().lightmaps;
                    inputs.tangentBases=mesh.mdxTangentOffset!=0xffffffffu;
                    k2Selection=neomdl::material::selectK2PartTriMeshRenderPath(
                        document_->material(*node.mesh),inputs);
                    rendered=neomdl::material::evaluateK2PartTriMeshMaterial(
                        document_->material(*node.mesh), surface, inputs,
                        canvas_->materialTime());
                } else {
                    rendered=neomdl::material::evaluateMaterial(
                        document_->material(*node.mesh), surface, canvas_->materialTime());
                }
                const char* passName = "opaque";
                using neomdl::material::RenderPass;
                switch (rendered.pass) {
                case RenderPass::PunchThrough: passName = "punch-through"; break;
                case RenderPass::Translucent: passName = "translucent"; break;
                case RenderPass::Additive: passName = "additive"; break;
                default: break;
                }
                text << "Sampled alpha: " << state.alpha << " ("
                     << neomdl::model::materialValueSourceName(state.alphaSource) << ")\n"
                     << "Effective opacity: " << rendered.opacity << "\n"
                     << "Self-illumination RGB: " << state.selfIllumination[0] << ", "
                     << state.selfIllumination[1] << ", " << state.selfIllumination[2] << " ("
                     << neomdl::model::materialValueSourceName(state.selfIlluminationSource)
                     << ")\n"
                     << "Material pass: " << passName << "\n";
                if (mesh.animated && node.animmesh) {
                    const auto& anim = *node.animmesh;
                    text << "Animmesh draw: PartAnimMesh::Draw -> RenderFlat\n"
                         << "Draw profile: engine fixed function\n"
                         << "Sample period: " << anim.samplePeriod << " s\n"
                         << "Position / UV frames per vertex: " << anim.positionSampleCount
                         << " / " << anim.texcoordSampleCount << "\n"
                         << "Position / UV sample values: " << anim.positionSamples.size()
                         << " / " << anim.texcoordSamples.size() << "\n"
                         << "Engine draw batches / indexes: " << mesh.engineDraws.size()
                         << " / " << mesh.indices.size() << "\n"
                         << "Queries: current morph positions; shadows: authored MDL pool\n";
                    if (activePose && *node.mesh < activePose->deformedMeshes.size()) {
                        const auto& current = activePose->deformedMeshes[*node.mesh].animmesh;
                        text << "Current position / UV pools defined: "
                             << (current && current->positionsDefined ? "yes" : "no") << " / "
                             << (current && current->texcoordsDefined ? "yes" : "no") << "\n";
                    }
                }
                if(k2Selection && !mesh.animated) {
                    const char* blend="SRC_ALPHA, ONE_MINUS_SRC_ALPHA";
                    using neomdl::material::BlendFunction;
                    if(rendered.blendFunction==BlendFunction::Additive)
                        blend="SRC_ALPHA, ONE";
                    else if(rendered.blendFunction==BlendFunction::Replace)
                        blend="ONE, ZERO";
                    text << "K2 PartTriMesh render path: "
                         << neomdl::material::triMeshRenderPathName(
                                rendered.renderPath)
                         << " (" << static_cast<unsigned>(rendered.renderPath)
                         << ")\n"
                         << "K2 blend factors: " << blend << "\n"
                         << "K2 depth write / alpha test: "
                         << (rendered.depthWrite ? "on" : "off") << " / ";
                    if(rendered.alphaTest) text << "> " << rendered.alphaCutoff;
                    else text << "off";
                    text << "\n"
                         << "K2 path inputs - lightmap / environment / bump / tangents: "
                         << k2Selection->lightmap << " / "
                         << k2Selection->environmentMap << " / "
                         << k2Selection->bumpMap << " / "
                         << k2Selection->tangentBases << "\n"
                         << "K2 wateralpha: " << k2Selection->waterAlpha << "\n";
                }
                text << "Vertex colors: " << (mesh.hasVertexColors ? "RGBA decoded" : "absent") << "\n"
                     << "Valid authored tangent bases: " << mesh.validTangentVertices
                     << " / " << mesh.vertices.size() << "\n"
                     << "MDX tangent offset: " << mesh.mdxTangentOffset << "\n"
                     << "Texture count: " << mesh.textureCount << "\n"
                     << "Authored ambient RGB: " << mesh.ambient.x << ", " << mesh.ambient.y << ", " << mesh.ambient.z << "\n"
                     << "Casts shadow: " << mesh.castsShadow << "\n"
                     << "Model fog eligible: " << document_->model().fog << "\n"
                     << "Transparency hint (raw): " << mesh.transparencyHint << "\n"
                     << "UV animation enabled (raw): " << mesh.uvAnimation.enabledRaw << "\n"
                     << "UV direction: " << mesh.uvAnimation.direction.x << ", "
                     << mesh.uvAnimation.direction.y << "\n"
                     << "UV jitter / speed: " << mesh.uvAnimation.jitter << " / "
                     << mesh.uvAnimation.jitterSpeed << "\n"
                     << "Rotate texture / beaming: " << mesh.rotateTexture << " / " << mesh.beaming << "\n"
                     << "Authored total area: " << mesh.totalArea << "\n";
                if (mesh.k2Material) {
                    const auto& material = *mesh.k2Material;
                    text << "K2 dirt flag / texture / worldspace: "
                         << static_cast<unsigned>(material.dirtEnabledRaw) << " / "
                         << material.dirtTexture << " / " << material.dirtWorldspace << "\n"
                         << "K2 hologram do-not-draw (raw): "
                         << static_cast<unsigned>(material.hologramDoNotDrawRaw) << "\n";
                }
                if (mesh.skinned) {
                    const neomdl::model::DeformedMesh* deformation = nullptr;
                    if (const auto* pose = canvas_->pose(); pose &&
                        *node.mesh < pose->deformedMeshes.size() &&
                        pose->deformedMeshes[*node.mesh].valid) {
                        deformation = &pose->deformedMeshes[*node.mesh];
                    }
                    const auto validPaletteSlots = deformation
                        ? static_cast<std::size_t>(std::count_if(
                              deformation->skinPalette.begin(),
                              deformation->skinPalette.end(),
                              [](const auto& slot) { return slot.valid; }))
                        : 0u;
                    const auto exactBasisVertices = deformation
                        ? static_cast<std::size_t>(std::count(
                              deformation->exactTangentBasis.begin(),
                              deformation->exactTangentBasis.end(),
                              static_cast<std::uint8_t>(1u)))
                        : 0u;
                    text << "Skin influence rows: " << mesh.skinInfluences.size() << "\n"
                         << "Skin bone map: " << (mesh.hasSkinBoneMap ? "decoded" : "unavailable") << "\n"
                         << "Inverse-bind bones: " << mesh.inverseBindOrientations.size() << "\n"
                         << "K2 compact-palette CPU deformation: "
                         << (deformation ? "active" : "fallback") << "\n"
                         << "K2 valid palette slots / span: " << validPaletteSlots
                         << " / " << (deformation ? deformation->skinPaletteSize : 0u) << "\n"
                         << "Rejected skin influences: "
                         << (deformation ? deformation->rejectedInfluences : 0u) << "\n"
                         << "Raw weighted ordinary normals: "
                         << (deformation ? deformation->rawSkinNormals.size() : 0u) << "\n"
                         << "Exact weighted tangent-basis vertices: "
                         << exactBasisVertices << " / " << mesh.vertices.size() << "\n";
                    if (document_->model().game ==
                        neomdl::model::GameVersion::Kotor2) {
                        const auto& skin = canvas_->options().skinSettings;
                        text << "Owner shell texture / amount: "
                             << canvas_->ownerVisualEffects(0).shellTexture << " / "
                             << canvas_->ownerVisualEffects(0).shellAmount << "\n"
                             << "Manual skin-blur diagnostic / supplied model-space motion: "
                             << (skin.motionBlur ? "on" : "off") << " / "
                             << skin.objectMotion.x << ", " << skin.objectMotion.y
                             << ", " << skin.objectMotion.z << "\n"
                             << "Maximum blur / side extrusion / scalar: "
                             << skin.maximumBlurLength << " / "
                             << (skin.sideExtrusion ? "on" : "off") << " / "
                             << skin.sideScale << "\n";
                    }
                }
                if (mesh.dangly) {
                    const auto& dangly = *mesh.dangly;
                    bool deformationActive = false;
                    if (const auto* pose = canvas_->pose(); pose &&
                        *node.mesh < pose->deformedMeshes.size()) {
                        deformationActive = pose->deformedMeshes[*node.mesh].valid;
                    }
                    text << "Dangly constraints: " << dangly.constraints.size() << "\n"
                         << "Dangly displacement/tightness/period: "
                         << dangly.displacement << " / " << dangly.tightness << " / "
                         << dangly.period << "\n"
                         << "K2 PartDanglyMesh runtime: "
                         << (document_->model().game == neomdl::model::GameVersion::Kotor2
                                 ? (deformationActive ? "active" : "inactive")
                                 : "not inferred from K2 executables") << "\n"
                         << "Constraint representation: raw generated-vertex values\n"
                         << "Runtime normal update: static authored channels\n";
                }
                if ((node.flags & neomdl::model::kNodeAabb) != 0u)
                    text << "Authored AABB faces: " << mesh.faces.size() << "\n";
            }
            if (node.aabb) {
                const auto& tree = *node.aabb;
                std::size_t internalNodes = 0u;
                std::size_t leafNodes = 0u;
                std::size_t maximumDepth = 0u;
                std::vector<bool> visited(tree.nodes.size(), false);
                std::vector<std::pair<std::size_t, std::size_t>> pending;
                if (tree.root && *tree.root < tree.nodes.size())
                    pending.emplace_back(*tree.root, 0u);
                while (!pending.empty()) {
                    const auto [index, depth] = pending.back();
                    pending.pop_back();
                    if (index >= tree.nodes.size() || visited[index]) continue;
                    visited[index] = true;
                    maximumDepth = std::max(maximumDepth, depth);
                    const auto& treeNode = tree.nodes[index];
                    if (treeNode.leaf()) {
                        ++leafNodes;
                    } else {
                        ++internalNodes;
                        if (treeNode.secondChild >= 0)
                            pending.emplace_back(
                                static_cast<std::size_t>(treeNode.secondChild),
                                depth + 1u);
                        if (treeNode.firstChild >= 0)
                            pending.emplace_back(
                                static_cast<std::size_t>(treeNode.firstChild),
                                depth + 1u);
                    }
                }
                text << "AABB tree records: " << tree.nodes.size() << "\n"
                     << "AABB reachable internal/leaves: " << internalNodes << " / "
                     << leafNodes << "\n"
                     << "AABB maximum depth: " << maximumDepth << "\n"
                     << "AABB tree validation: "
                     << (tree.valid ? "valid" : "diagnostics present") << "\n";
                if (tree.root && *tree.root < tree.nodes.size()) {
                    const auto& root = tree.nodes[*tree.root];
                    text << "AABB root source offset: 0x" << std::hex
                         << root.sourceOffset << std::dec << "\n"
                         << "AABB root split mask: 0x" << std::hex
                         << root.splitMask << std::dec << "\n";
                } else {
                    text << "AABB root: unavailable\n";
                }
            }
            if (node.light) {
                const auto& light = *node.light;
                const auto* active=canvas_->pose();
                const auto sampled=active && *selected<active->nodeLights.size() ? active->nodeLights[*selected] : neomdl::model::bindNodeLight(node);
                text << "Sampled light RGB: " << sampled.color[0] << ", " << sampled.color[1] << ", " << sampled.color[2] << "\n"
                     << "Light radius / multiplier: " << sampled.radius << " / " << sampled.multiplier << "\n"
                     << "Shadow radius: " << sampled.shadowRadius << "\n"
                     << "Shadow-source world-Z displacement: " << sampled.verticalDisplacement << "\n"
                     << "Color / radius source: " << neomdl::model::materialValueSourceName(sampled.sources[0]) << " / "
                     << neomdl::model::materialValueSourceName(sampled.sources[1]) << "\n";
                text << "Shadow / displacement / multiplier source: "
                     << neomdl::model::materialValueSourceName(sampled.sources[2]) << " / "
                     << neomdl::model::materialValueSourceName(sampled.sources[3]) << " / "
                     << neomdl::model::materialValueSourceName(sampled.sources[4]) << "\n"
                     << "Light priority: " << light.priority << "\n"
                     << "Dynamic type: " << light.dynamicType << "\n"
                     << "Light flags: "
                     << (light.ambientOnly ? "ambient-only " : "")
                     << (light.dynamic ? "dynamic " : "")
                     << (light.affectDynamic ? "affect-dynamic " : "")
                     << (light.castsShadow ? "shadow " : "")
                     << (light.generateFlare ? "generate-flare " : "")
                     << (light.fading ? "fading " : "") << "\n"
                     << "Flare radius/textures: " << light.flareRadius << " / "
                     << light.textureNames.size() << "\n";
                if (const auto* owner = canvas_->lightScene().ownerSelection();
                    owner && !owner->diagnostic.empty()) {
                    text << "K2 light query: " << owner->diagnostic << "\n";
                }
                if (const auto* runtime = canvas_->lightScene().mainNode(*selected)) {
                    text << "K2 registration static/dynamic/render/flare: "
                         << (runtime->registeredStatic ? "yes" : "no") << " / "
                         << (runtime->registeredDynamic ? "yes" : "no") << " / "
                         << (runtime->registeredRender ? "yes" : "no") << " / "
                         << (runtime->registeredFlare ? "yes" : "no") << "\n"
                         << "K2 main-owner illumination/shadow fades: "
                         << canvas_->lightScene().illuminationWeight(mainLightOwner, runtime->id) << " / "
                         << canvas_->lightScene().shadowWeight(mainLightOwner, runtime->id) << "\n"
                         << "Lens-flare rendering: enable Engine Lens Flares; requires K2 area context\n"
                         << "K2 quadratic attenuation: "
                         << neomdl::k2LightQuadraticAttenuation(runtime->radius)
                         << "\n"
                         << "K2 resolved flare elements: "
                         << runtime->flares.size() << "\n";
                }
            }
            if (node.emitter) {
                const auto& emitter = *node.emitter;
                text << "Emitter update/render/blend: "
                     << (emitter.updateMode.empty() ? "(none)" : emitter.updateMode) << " / "
                     << (emitter.renderMode.empty() ? "(none)" : emitter.renderMode) << " / "
                     << (emitter.blendMode.empty() ? "(none)" : emitter.blendMode) << "\n"
                     << "Emitter texture/chunk: "
                     << (emitter.texture.empty() ? "(none)" : emitter.texture) << " / "
                     << (emitter.chunkName.empty() ? "(none)" : emitter.chunkName) << "\n"
                     << "Emitter dead space/radius/length: " << emitter.deadSpace << " / "
                     << emitter.blastRadius << " / " << emitter.blastLength << "\n"
                     << "Emitter grid/spawn/order: " << emitter.xGrid << "x" << emitter.yGrid
                     << " / " << emitter.spawnType << " / " << emitter.renderOrder << "\n"
                     << "Emitter frame blending: "
                     << (emitter.frameBlending ? "yes" : "no") << "\n"
                     << "Emitter depth texture: "
                     << (emitter.depthTexture.empty() ? "(none)" : emitter.depthTexture)
                     << "\n";
            }
            if (node.emitter) {
                const auto sampled=canvas_->pose() && *selected<canvas_->pose()->nodeEmitters.size()
                    ? *canvas_->pose()->nodeEmitters[*selected] : neomdl::model::bindNodeEmitter(node);
                text << "Sampled birth rate / lifetime: " << sampled.birthRate << " / " << sampled.lifeExpectancy << "\n"
                     << "Sampled mass / random birth rate: " << sampled.mass << " / " << sampled.randomBirthRate << "\n"
                     << "Lifetime key fractions: " << sampled.percentStart << " / " << sampled.percentMid << " / " << sampled.percentEnd << "\n"
                     << "Sampled alpha start/mid/end: " << sampled.alphaStart << " / " << sampled.alphaMid << " / " << sampled.alphaEnd << "\n";
                text << "Target combine / Bezier handles / threshold: " << sampled.combineTime << " / "
                     << sampled.bezier2 << " / " << sampled.bezier3 << " / " << sampled.targetThreshold << "\n"
                     << "Target gravity / drag / bounce: " << sampled.gravity << " / " << sampled.drag << " / " << sampled.bounceCoefficient << "\n";
                text << "Lightning subdivisions / radius / scale / delay: " << sampled.lightningSubdivisions << " / "
                     << sampled.lightningRadius << " / " << sampled.lightningScale << " / " << sampled.lightningDelay << "\n"
                     << "Lightning control density / radius / delay: " << sampled.controlPointCount << " / "
                     << sampled.controlPointRadius << " / " << sampled.controlPointDelay << "\n"
                     << "Lightning branches / smoothing: " << node.emitter->branchCount << " / "
                     << (node.emitter->controlPointSmoothing?"yes":"no") << "\n";
                const auto contextStats=canvas_->particleScene().statistics();
                text << "Loaded chunk assets / live model instances: " << document_->loadedChunkModelCount()
                     << " / " << contextStats.chunkInstances << "\n";
                text << "Scene blast winds / missing targets: " << contextStats.activeBlasts << " / " << contextStats.missingTargets << "\n"
                     << "Registered windy emitters: " << contextStats.registeredWindyEmitters << "\n";
                if(!canvas_->particleScene().replayDiagnostic().empty())
                    text << canvas_->particleScene().replayDiagnostic() << "\n";
                for(const auto& emitter:canvas_->particleScene().emitters())
                    if(!emitter->reference && emitter->nodeIndex==*selected)
                        text << "Particle preview: " << emitter->diagnostic << "\n"
                             << "Live particles: " << emitter->simulation.particles.size() << "\n";
            }
            if (node.camera) {
                text << "Camera node payload: no camera-specific fields\n"
                     << "Camera controllers: generic position (8), orientation (20), scale (36)\n"
                     << "Runtime owner: external CAurCamera attached to a model part\n"
                     << "View convention: local -Z forward, local +Y up\n";
                if (canvas_->cameraScene().engineRuntime()) {
                    const auto& settings = canvas_->options().cameraSettings;
                    text << "External vertical FOV / clip: "
                         << settings.viewAngle << " degrees / "
                         << settings.nearClip << " / " << settings.farClip << "\n";
                    if (const auto* runtime = canvas_->cameraScene().mainNode(*selected)) {
                        const auto* active = canvas_->cameraScene().active();
                        const bool isActive = active &&
                            active->attachment == runtime->attachment;
                        text << "World position: " << runtime->position.x << ", "
                             << runtime->position.y << ", "
                             << runtime->position.z << "\n"
                             << "World forward: " << runtime->forward.x << ", "
                             << runtime->forward.y << ", "
                             << runtime->forward.z << "\n"
                             << "World up: " << runtime->up.x << ", "
                             << runtime->up.y << ", " << runtime->up.z << "\n"
                             << "Engine camerahook target: "
                             << (runtime->engineHook ? "yes" : "no") << "\n"
                             << "Current preview camera: "
                             << (isActive ? "active" : "inactive") << "\n";
                    }
                    text << "FOV source: external cutscene/dialog call, not MDL data\n";
                } else {
                    text << "Camera runtime unavailable for this model\n";
                }
            }
            if (node.trigger) {
                const auto& trigger = *node.trigger;
                text << "Trigger-node private payload: "
                     << (trigger.hasPrivatePayload ? "present" : "none") << "\n"
                     << "Trigger-node runtime Part: "
                     << (trigger.createsRuntimePart ? "created" : "not created")
                     << "\n";
                if (trigger.packedVerified) {
                    text << "Engine packed behavior: InternalParseField is a no-op; "
                            "InternalCreateInstance returns null\n"
                         << "Runtime volume source: game-object footprint passed to "
                            "AurPartTriggerCreate, outside MDL data\n"
                         << "Runtime volume count in this preview scene: "
                         << canvas_->triggerScene().volumes().size() << "\n"
                         << "Global runtime alpha: "
                         << canvas_->triggerScene().globalAlpha() << "\n";
                } else {
                    text << "Trigger-node runtime unavailable for this model\n";
                }
            }
            if (node.reference) {
                const auto& reference = *node.reference;
                const auto loaded = static_cast<std::size_t>(std::count_if(
                    document_->references().begin(), document_->references().end(),
                    [selected](const ReferenceInstance& instance) {
                        return !instance.parentInstance &&
                               instance.parentNodeIndex == *selected;
                    }));
                text << "Reference model: "
                     << (reference.model.empty() ? "(none)" : reference.model) << "\n"
                     << "Reattachable: " << (reference.reattachable ? "yes" : "no") << "\n"
                     << "Resolved instances at this node: " << loaded << "\n";
                for (std::size_t instanceIndex = 0u;
                     instanceIndex < document_->references().size(); ++instanceIndex) {
                    const auto& definition = document_->references()[instanceIndex];
                    if (definition.parentInstance ||
                        definition.parentNodeIndex != *selected) continue;
                    const auto* runtime = canvas_->referenceScene().instance(instanceIndex);
                    if (!runtime) continue;
                    const auto position = neomdl::model::transformPoint(runtime->world, {});
                    text << "Runtime reference " << instanceIndex << ": "
                         << (runtime->attached ? "attached" : "detached")
                         << (runtime->reattached ? ", reattached" : ", authored target")
                         << "\n"
                         << "  Target part: "
                         << (runtime->targetNodeName.empty() ? "(none)" : runtime->targetNodeName)
                         << "\n"
                         << "  Default animation speed/start/current: "
                         << runtime->playbackRate << " / "
                         << runtime->initialAnimationTime << " / "
                         << runtime->animationTime << "\n"
                         << "  Object position: " << position.x << ", "
                         << position.y << ", " << position.z << "\n";
                }
            }
            if (node.lightsaber) {
                text << "Lightsaber topology constants: "
                     << neomdl::model::LightsaberNodeData::frontSegments
                     << " front / "
                     << neomdl::model::LightsaberNodeData::backSegments
                     << " back segments, "
                     << neomdl::model::LightsaberNodeData::pieceVertices
                     << " vertices per piece\n";
            }
        }
        if (!document_->messages().empty()) {
            text << "\nDiagnostics:\n";
            for (const auto& message : document_->messages()) text << "• " << message << "\n";
        }
        details_->ChangeValue(wxui::toWx(text.str()));
    }

    std::unique_ptr<ModelDocument> document_;
    wxSplitterWindow* splitter_{};
    wxTreeCtrl* tree_{};
    wxTextCtrl* details_{};
    ModelCanvas* canvas_{};
    wxChoice* animationChoice_{};
    wxChoice* runModeChoice_{};
    wxSpinCtrlDouble* rateSpin_{};
    wxCheckBox* loopCheck_{};
    wxCheckBox* immediateCheck_{};
    wxCheckBox* synchronizeCheck_{};
    wxCheckBox* allowDuplicateCheck_{};
    wxCheckBox* reportLoopCheck_{};
    wxButton* startRunButton_{};
    wxToggleButton* playButton_{};
    wxButton* clearRunsButton_{};
    wxChoice* activeRunChoice_{};
    wxButton* fadeRunButton_{};
    wxButton* removeRunButton_{};
    wxSlider* timeline_{};
    wxStaticText* timeLabel_{};
    std::vector<std::uint64_t> runChoiceIds_;
    std::vector<std::string> runChoiceLabels_;
    wxTimer timer_;
    std::chrono::steady_clock::time_point lastTick_{};
    std::vector<std::string> eventLog_;
    std::optional<std::size_t> selectedNode_;
    bool playing_{};
    unsigned loadingSuspensions_{};
    std::shared_ptr<GameResourceSession> resourceSession_;
};

class ModelEditorPanelImpl final : public ModelEditorPanel {
public:
    ModelEditorPanelImpl(wxWindow* parent, neomodules::Context context)
        : ModelEditorPanel(parent, std::move(context)) {
        buildMenus();
        tabs_ = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB |
                wxAUI_NB_SCROLL_BUTTONS);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(tabs_, 1, wxEXPAND);
        SetSizer(layout);
        createModuleStatusBar(2);
        int widths[] = {-3, -2};
        moduleStatusBar()->SetStatusWidths(2, widths);
        bindEvents();
        updateState();
    }

    bool loadModelWithGame(const wxString& title,
        std::optional<model::GameVersion>& game,
        const std::function<void(const neoshared::bif::JobControl&,std::optional<model::GameVersion>)>& load) {
        for(;;) {
            bool gameRequired=false;
            if(!runJob(title,[&](const neoshared::bif::JobControl& job) {
                try {load(job,game);}
                catch(const model::AsciiModelGameRequired&) {gameRequired=true;}
            }))return false;
            if(!gameRequired)return true;
            wxArrayString choices;
            choices.Add("Knights of the Old Republic");choices.Add("Knights of the Old Republic II");
            wxSingleChoiceDialog choice(this,"Choose the target game for this ASCII model.","Open ASCII MDL",choices);
            if(choice.ShowModal()!=wxID_OK)return false;
            game=choice.GetSelection()==0?model::GameVersion::Kotor1:model::GameVersion::Kotor2;
        }
    }

    bool openFile(const std::filesystem::path& path) override {
        if (jobRunning_) return false;
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index) {
            const auto* page = pageAt(index);
            for (const auto& openPath : page->document().openPaths()) {
                if (neoshared::sameResourcePath(openPath, path)) {
                    tabs_->SetSelection(index);
                    return true;
                }
            }
        }
        try {
            // Packed headers identify the game. ASCII construction uses the
            // explicitly selected engine rules before resolving dependencies.
            auto document = std::make_unique<ModelDocument>();
            std::optional<model::GameVersion> asciiGame;
            if (!loadModelWithGame("Loading model",asciiGame,
                [&](const neoshared::bif::JobControl& job,std::optional<model::GameVersion> game) {
                document->openFile(path, {}, job,game);
            })) return false;

            std::shared_ptr<GameResourceSession> installationSession;
            std::string installationWarning;
            const auto installed = installedResourceResolver(
                document->model().game, path, installationSession,
                installationWarning);
            const auto combined = combineResolvers(captureHostResolver(), installed);
            if (combined) {
                auto enriched = std::make_unique<ModelDocument>();
                asciiGame=document->model().game;
                if (!loadModelWithGame("Loading model resources",asciiGame,
                    [&](const neoshared::bif::JobControl& job,std::optional<model::GameVersion> game) {
                    try {
                        enriched->openFile(path, combined, job,game);
                        document = std::move(enriched);
                    } catch (const neoshared::bif::JobCancelled&) {
                        throw;
                    } catch (const std::exception& error) {
                        document->appendDiagnostic(
                            "Installed-game resource lookup was unavailable; the model was opened "
                            "with same-directory resources only: " + std::string(error.what()));
                    }
                })) return false;
            }

            if (installationSession && installationSession->installation()) {
                const auto& install = *installationSession->installation();
                document->appendDiagnostic(
                    "Installed-game resources: " + install.displayName + " (" +
                    neoshared::pathToUtf8(install.installPath) + ")");
                for (const auto& diagnostic : installationSession->diagnostics())
                    document->appendDiagnostic(diagnostic);
            } else if (!installationWarning.empty()) {
                document->appendDiagnostic(std::move(installationWarning));
            }

            if (activateResource(document->identity())) return true;
            addDocument(std::move(document),installationSession);
            return true;
        } catch (const neoshared::bif::JobCancelled&) {
            return false;
        } catch (const std::exception& error) {
            wxui::showError(this, error);
            return false;
        }
    }

    bool openResource(neoshared::ResourceDocument resource) override {
        if (jobRunning_) return false;
        if (!resource.identity.empty() && activateResource(resource.identity)) return true;
        try {
            auto document = std::make_unique<ModelDocument>();
            const auto resolver = captureHostResolver();
            std::optional<model::GameVersion> asciiGame;
            if (!loadModelWithGame("Loading model resource",asciiGame,
                [&](const neoshared::bif::JobControl& job,std::optional<model::GameVersion> game) {
                document->openResource(resource, resolver, job,game);
            })) return false;
            // An MDX resource canonicalizes to its same-name MDL document. Check
            // the resolved identity as well so opening both halves reuses one tab.
            if (activateResource(document->identity())) return true;
            addDocument(std::move(document));
            return true;
        } catch (const std::exception& error) {
            wxui::showError(this, error);
            return false;
        }
    }

    bool activateResource(const std::string& identity) override {
        if (identity.empty()) return false;
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index) {
            if (pageAt(index)->document().identity() == identity) {
                tabs_->SetSelection(index);
                return true;
            }
        }
        return false;
    }

    std::size_t documentCount() const override { return tabs_->GetPageCount(); }
    bool canClose() override {
        if (jobRunning_) { cancelJob_.store(true); return false; }
        return true;
    }

    void setAppearance(bool dark, double) override {
        dark_ = dark;
        wxui::applyTheme(this, dark_);
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index)
            pageAt(index)->setAppearance(dark_);
    }

    std::vector<std::filesystem::path> openPaths() const override {
        std::vector<std::filesystem::path> paths;
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index) {
            const auto documentPaths = pageAt(index)->document().openPaths();
            paths.insert(paths.end(), documentPaths.begin(), documentPaths.end());
            const auto areaPaths=pageAt(index)->areaScene().openPaths();
            paths.insert(paths.end(),areaPaths.begin(),areaPaths.end());
        }
        return paths;
    }

private:
    template<class Work>
    bool runJob(const wxString& title, Work work) {
        if (jobRunning_) return false;
        jobRunning_ = true;
        cancelJob_.store(false);
        for (std::size_t i = 0; i < tabs_->GetPageCount(); ++i)
            pageAt(i)->suspendLoadingPlayback();
        struct RestoreUi {
            ModelEditorPanelImpl& panel;
            ~RestoreUi() {
                panel.jobRunning_ = false;
                for (std::size_t i = 0; i < panel.tabs_->GetPageCount(); ++i)
                    panel.pageAt(i)->resumeLoadingPlayback();
            }
        } restore{*this};
        std::atomic<bool> finished{false};
        std::mutex progressMutex;
        std::string detail = "Preparing...";
        std::size_t done = 0, total = 0;
        std::exception_ptr failure;
        neoshared::bif::JobControl control;
        control.cancelled = [this] { return cancelJob_.load(); };
        control.progress = [&](std::size_t current, std::size_t count, const std::string& text) {
            std::lock_guard<std::mutex> lock(progressMutex);
            done = current; total = count; detail = text;
        };
        wxProgressDialog dialog(title, "Preparing...", 1000, this,
            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_AUTO_HIDE);
        std::thread worker([&] {
            try {
                ResourceLoadScope scope(control);
                work(control);
                control.check();
            } catch (...) { failure = std::current_exception(); }
            finished.store(true);
        });
        struct JoinWorker {
            std::thread& worker;
            std::atomic<bool>& cancel;
            ~JoinWorker() {
                if (worker.joinable()) { cancel.store(true); worker.join(); }
            }
        } join{worker, cancelJob_};
        while (!finished.load()) {
            std::string text;
            std::size_t current, count;
            { std::lock_guard<std::mutex> lock(progressMutex);
              current = done; count = total; text = detail; }
            const bool proceed = count
                ? dialog.Update(static_cast<int>(std::min(999.0,
                    1000.0 * static_cast<double>(current) / static_cast<double>(count))), wxui::toWx(text))
                : dialog.Pulse(wxui::toWx(text));
            if (!proceed) cancelJob_.store(true);
            wxMilliSleep(10);
        }
        worker.join();
        if (cancelJob_.load()) {
            setModuleStatusText("Operation cancelled; current documents and scenes retained.", 0);
            return false;
        }
        if (failure) {
            try { std::rethrow_exception(failure); }
            catch (const neoshared::bif::JobCancelled&) {
                setModuleStatusText("Operation cancelled; current documents and scenes retained.", 0);
                return false;
            }
        }
        if (!dialog.Update(1000) || cancelJob_.load()) {
            setModuleStatusText("Operation cancelled; current documents and scenes retained.", 0);
            return false;
        }
        return true;
    }

    ResourceResolver captureHostResolver() const {
        if (context_.captureResourceResolver) {
            auto resolver = context_.captureResourceResolver();
            if (!resolver) return {};
            return [resolver = std::move(resolver)](const std::string& resref, std::uint16_t type) {
                return resolver(resref, type, resourceLoadJob());
            };
        }
        if (context_.resolveResource)
            throw std::runtime_error("The host must provide a captured resource resolver for background model loading.");
        return {};
    }

    static neogames::GameId gameIdForModel(neomdl::model::GameVersion game) {
        return game == neomdl::model::GameVersion::Kotor2
            ? neogames::GameId::Kotor2 : neogames::GameId::Kotor1;
    }

    static ResourceResolver combineResolvers(ResourceResolver first,
                                             ResourceResolver second) {
        if (!first) return second;
        if (!second) return first;
        return [first = std::move(first), second = std::move(second)](
                   const std::string& resref, std::uint16_t type)
                -> std::optional<neoshared::ResourceDocument> {
            if (auto resource = first(resref, type)) return resource;
            return second(resref, type);
        };
    }

    ResourceResolver installedResourceResolver(
        neomdl::model::GameVersion modelGame,
        const std::filesystem::path& modelPath,
        std::shared_ptr<GameResourceSession>& selectedSession,
        std::string& warning) {
        selectedSession.reset();
        warning.clear();
        const auto gameId = gameIdForModel(modelGame);
        const auto* definition = neogames::findGame(gameId);
        if (definition == nullptr) return {};

        std::optional<neogames::GameInstall> selected;
        if (const auto inferred = neogames::resolver().inferFromOpenedPath(modelPath)) {
            if (inferred->id == definition->id &&
                neogames::isValidGameInstallation(*definition, inferred->installPath)) {
                selected = *inferred;
            }
        }
        if (!selected) {
            auto installs = neogames::resolver().resolveInstalls(
                *definition, modelPath.empty()
                    ? std::optional<std::filesystem::path>{}
                    : std::optional<std::filesystem::path>{modelPath});
            const auto usable = std::find_if(installs.begin(), installs.end(),
                [&](const neogames::GameInstall& install) {
                    return neogames::isValidGameInstallation(
                        *definition, install.installPath);
                });
            if (usable != installs.end()) selected = *usable;
        }
        if (!selected) {
            if (!context_.resolveResource && !context_.captureResourceResolver) {
                warning = "No validated " + definition->displayName +
                    " installation is configured. Same-directory model resources were used.";
            }
            return {};
        }

        const std::string key = definition->id + ":" +
            (selected->installId.empty()
                ? neoshared::genericPathToUtf8(selected->installPath)
                : selected->installId) + ":" +
            neoshared::genericPathToUtf8(selected->installPath);
        const auto cached = installationSessions_.find(key);
        if (cached != installationSessions_.end()) {
            selectedSession = cached->second;
            return selectedSession->resolver();
        }

        auto session = std::make_shared<GameResourceSession>();
        std::string error;
        setModuleStatusText("Indexing " + definition->displayName + " resources...", 0);
        bool opened = false;
        if (!runJob("Indexing installed game", [&](const neoshared::bif::JobControl& job) {
            opened = session->open(*selected, error, job);
        })) throw neoshared::bif::JobCancelled();
        if (!opened) {
            if (!context_.resolveResource && !context_.captureResourceResolver) {
                warning = "The selected " + definition->displayName +
                    " installation could not be indexed: " + error;
            }
            return {};
        }
        installationSessions_.emplace(key, session);
        selectedSession = std::move(session);
        return selectedSession->resolver();
    }

    void showResourceProvenance() {
        const auto* page=activePage();if(!page)return;
        std::ostringstream report;
        report<<"DOCUMENT\n"<<page->document().identity()<<"\n"<<page->document().sourceDescription()<<"\n\n";
        const auto& area=page->areaScene();
        if(!area.empty()) {
            report<<"AREA SCOPE\n"<<area.identity()<<"\n\nVISUAL OBJECTS\n";
            for(const auto& p:area.placements())report<<p.label<<" | "<<p.modelResref<<" | "
                <<(p.runtimeInstance?"owner "+std::to_string(*p.runtimeInstance):p.visible?"UNRESOLVED":"engine invisible model")<<"\n";
            report<<"\nGIT TRIGGERS\n";
            const auto& runtimeTriggers=page->triggerScene().records();
            for(std::size_t i=0;i<area.triggers().size();++i) {
                const auto& t=area.triggers()[i];
                report<<"["<<t.sourceIndex<<"] "<<t.label<<" object=0x"<<std::hex<<t.objectId<<std::dec
                    <<" type="<<t.type<<" points="<<t.worldFootprint.size()
                    <<" height="<<t.highlightHeight;
                const auto runtime=std::find_if(runtimeTriggers.begin(),runtimeTriggers.end(),[&](const auto& r) {
                    return r.source==RuntimeTriggerSource::AreaGit&&r.sourceIndex==t.sourceIndex;
                });
                if(runtime!=runtimeTriggers.end())report<<" live(suppress="<<runtime->clientVisualState.updateSuppressed
                    <<", transition="<<runtime->clientVisualState.areaTransitionMode
                    <<", display="<<runtime->clientVisualState.displayEnabled<<")";
                report<<"\n";
            }
            report<<"\nGIT PLACEABLE CAMERAS\n";
            for(const auto& c:area.cameras())report<<"["<<c.sourceIndex<<"] id="<<c.cameraId
                <<" position="<<c.position.x<<','<<c.position.y<<','<<c.position.z
                <<" orientation="<<c.orientation.x<<','<<c.orientation.y<<','<<c.orientation.z<<','<<c.orientation.w
                <<" fov="<<c.fieldOfView<<" pitch="<<c.pitch<<" height="<<c.height
                <<" mic-range="<<c.microphoneRange<<"\n";
            report<<"\nLOAD DIAGNOSTICS\n";
            for(const auto& d:area.diagnostics())report<<d<<"\n";
            const auto& environment=area.environment().sample();
            const auto vector=[&](const char* label,const std::optional<model::Vec3>& value) {
                if(value)report<<label<<value->x<<','<<value->y<<','<<value->z<<"\n";
            };
            report<<"\nAREA ENVIRONMENT / REPLAY\nphase="<<areaTimeOfDayName(environment.phase)
                <<" transition="<<environment.transition
                <<" fog="<<(environment.hasFogState?(environment.fogEnabled?"on":"off"):"not authored")
                <<" shadows="<<(environment.hasShadowState?(environment.shadowsEnabled?"on":"off"):"not authored")
                <<" shadow-opacity="<<environment.shadowOpacity<<"\n";
            vector("scene ambient=",environment.sceneAmbient);
            vector("gidy_sun ambient=",environment.globalAmbient);
            vector("gidy_sun diffuse=",environment.diffuse);
            vector("fog color=",environment.fogColor);
            if(environment.fogNear&&environment.fogFar)
                report<<"fog range="<<*environment.fogNear<<" .. "<<*environment.fogFar<<"\n";
            report<<"current room="<<(area.currentRoom()?std::to_string(*area.currentRoom()):std::string("all rooms"))
                <<(area.currentRoomOverridden()?" (manual override)\n":" (replay or camera WOK query)\n");
            if(const auto* camera=area.activeReplayCamera())
                report<<"replay camera="<<camera->world.values[12]<<','<<camera->world.values[13]<<','
                    <<camera->world.values[14]<<" fov="<<camera->viewAngle<<" clips="
                    <<camera->nearClip<<" .. "<<camera->farClip<<"\n";
        }
        report<<"\nDIRTY AREA INPUTS (automatic material routing is absent in both K2 executables)\n"<<describeDirtyArea(page->dirtyAreaState());
        report<<"\nDEPENDENCIES (requester -> selected identity; missing and failed probes retained)\n";
        auto dependencies=page->document().dependencies();
        const auto areaDependencies=area.dependencies();dependencies.insert(dependencies.end(),areaDependencies.begin(),areaDependencies.end());
        for(const auto& d:dependencies) {
            report<<(d.status==DependencyStatus::Found?"FOUND":d.status==DependencyStatus::Missing?"MISSING":"FAILED")
                <<" "<<d.resref<<"."<<resourceExtension(d.type)<<"\n  requester: "<<d.requester
                <<"\n  identity: "<<d.identity<<"\n  source: "<<d.source;
            if(!d.error.empty())report<<"\n  error: "<<d.error;
            report<<"\n";
        }
        wxDialog dialog(this,wxID_ANY,"Module resources and visual load report",wxDefaultPosition,wxSize(940,680),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(report.str()),wxDefaultPosition,wxDefaultSize,
            wxTE_MULTILINE|wxTE_READONLY|wxTE_DONTWRAP),1,wxEXPAND|wxALL,8);
        box->Add(dialog.CreateButtonSizer(wxOK),0,wxALIGN_RIGHT|wxALL,8);dialog.SetSizer(box);dialog.ShowModal();
    }
    void chooseModuleArea() {
        try {
            std::shared_ptr<GameResourceSession> installation;std::string warning;
            const auto* active=activePage();
            wxArrayString games;games.Add("Knights of the Old Republic");games.Add("Knights of the Old Republic II");
            wxSingleChoiceDialog gameChoice(this,"Choose the game installation for this module.","Load module area",games);
            gameChoice.SetSelection(active&&active->document().model().game==model::GameVersion::Kotor2?1:0);
            if(gameChoice.ShowModal()!=wxID_OK)return;
            const auto game=gameChoice.GetSelection()==0?model::GameVersion::Kotor1:model::GameVersion::Kotor2;
            const auto gameId=game==model::GameVersion::Kotor1?neogames::GameId::Kotor1:neogames::GameId::Kotor2;
            const auto* gameDefinition=neogames::findGame(gameId);
            installedResourceResolver(game,active?active->document().path():std::filesystem::path{},installation,warning);
            if(!installation) {
                wxDirDialog directory(this,wxui::toWx("Select the "+gameDefinition->displayName+" installation (chitin.key)"),wxEmptyString,wxDD_DEFAULT_STYLE|wxDD_DIR_MUST_EXIST);
                if(directory.ShowModal()!=wxID_OK)return;
                neogames::GameInstall selected;selected.id=gameDefinition->id;selected.displayName=gameDefinition->displayName;
                selected.installPath=neosettings::pathFromWx(directory.GetPath());
                installation=std::make_shared<GameResourceSession>();std::string error;
                if(!runJob("Indexing game installation",[&](const neoshared::bif::JobControl& job) {
                    if(!installation->open(selected,error,job))throw std::runtime_error(error);
                }))return;
            }
            const auto modules=installation->modules();
            if(modules.empty())throw std::runtime_error("No module MOD/RIM sets were found under modules/rims");
            wxArrayString names;for(const auto& module:modules)names.Add(wxui::toWx(module.name));
            const int selected=wxGetSingleChoiceIndex("Only this module's archives will be mounted; Override and base resources remain available.","Select module",names,this);
            if(selected<0)return;
            auto scope=std::make_shared<GameResourceSession>();std::string error;
            const auto module=modules.at(std::size_t(selected));
            if(!runJob("Indexing module",[&](const neoshared::bif::JobControl& job) {
                if(!scope->openModule(*installation,module,error,job))throw std::runtime_error(error);
            }))return;
            const auto areas=scope->areaResrefs();if(areas.empty())throw std::runtime_error("The selected module contains no ARE resources");
            wxArrayString areaNames;for(const auto& area:areas)areaNames.Add(wxui::toWx(area));
            const int chosen=wxGetSingleChoiceIndex("Load archive-backed ARE/GIT/LYT/VIS into a new scene tab.","Select visual area",areaNames,this);
            if(chosen<0)return;
            wxDialog settings(this,wxID_ANY,"Module visual load");auto* box=new wxBoxSizer(wxVERTICAL);
            box->Add(new wxStaticText(&settings,wxID_ANY,
                "Loads rooms, tracks, doors, placeables and authored creature appearances.\n"
                "This is not gameplay replay: scripts and actor animation states are not inferred."),0,wxALL,10);
            auto* partial=new wxCheckBox(&settings,wxID_ANY,"Allow unresolved visual objects (report every omission)");
            box->Add(partial,0,wxALL,10);
            auto* grass=new wxCheckBox(&settings,wxID_ANY,"Load ARE grass");
            const bool grassSupported=engineAreaRuntimeSupported(game);
            grass->SetValue(grassSupported);grass->Enable(grassSupported);box->Add(grass,0,wxALL,10);
            box->Add(settings.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,10);settings.SetSizerAndFit(box);
            if(settings.ShowModal()!=wxID_OK)return;
            AreaLoadOptions options;options.game=game;options.allowPartialVisuals=partial->GetValue();options.loadGrass=grass->GetValue();
            const auto area=areas.at(std::size_t(chosen));
            const auto identity=scope->scopeIdentity()+"/area/"+area;
            // Create wx/GL ownership on the UI thread. The worker receives
            // only a private CPU scene, immutable resolver and engine snapshot.
            auto document=ModelDocument::emptySceneDocument(identity,module.name+" / "+area,game);
            auto* page=new ModelPage(tabs_,std::move(document),options_,context_.compact);
            page->setResourceSession(scope);
            page->suspendLoadingPlayback();
            try {
                auto pending=page->prepareAreaLoad();
                PreparedSceneAudioPlayback sounds;
                const auto resolver=scope->resolver();
                const auto scopeIdentity=scope->scopeIdentity();
                if(!runJob("Loading module area",[&](const neoshared::bif::JobControl& job) {
                    AreaRuntimeServices services;
                    pending.loadModuleArea(area,scopeIdentity,resolver,options,services,job);
                    sounds=prepareSceneAudioPlayback(pending,resolver,game,job);
                })) {page->Destroy();return;}
                page->installArea(std::move(pending),std::move(sounds));
                page->setAppearance(dark_);
                if(!tabs_->AddPage(page,wxui::toWx(page->document().title()),true))throw std::runtime_error("Could not add module scene tab");
                page->resumeLoadingPlayback();
            } catch(...) {page->Destroy();throw;}
            updateState();setModuleStatusText("Module loaded; camera WOK queries supply the current room. Resource report lists loads and omissions.",0);
            if(!page->areaScene().diagnostics().empty())showResourceProvenance();
        } catch(const neoshared::bif::JobCancelled&) {
            // The joined job already reported cancellation; nothing is published.
        } catch(const std::exception& e) {wxui::showError(this,e);}
    }
    void chooseSceneShadows() {
        auto* page=activePage();if(!page)return;
        auto inputs=page->shadowScene().inputs();
        const auto casters=page->shadowScene().casters();
        wxDialog dialog(this,wxID_ANY,"Scene shadow inputs");auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "Automatic mode follows scene focus and its retained shadow-light selection.\n"
            "Explicit mode uses the world-space inputs below for inspection."),0,wxALL,10);
        auto* automatic=new wxCheckBox(&dialog,wxID_ANY,"Use engine scene-focus shadow inputs");
        automatic->SetValue(inputs.mode==SceneShadowInputMode::SceneFocus);
        root->Add(automatic,0,wxALL,10);
        wxArrayString owners;owners.Add("Independent scene container");
        for(const auto& c:casters)owners.Add(wxui::toWx(c.label+" ["+std::to_string(c.id)+"]"));
        auto* owner=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,owners);owner->SetSelection(0);
        for(std::size_t i=0;i<casters.size();++i)
            if(inputs.containerOwner==casters[i].id)owner->SetSelection(int(i+1));
        root->Add(owner,0,wxEXPAND|wxALL,10);
        auto* collected=new wxCheckBox(&dialog,wxID_ANY,"Independent container uses collected room lights (otherwise global)");
        collected->SetValue(inputs.containerSource==LightQuerySource::Room);root->Add(collected,0,wxALL,10);
        auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1);
        const auto input=[&](const char* label,float value) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            auto* field=new wxTextCtrl(&dialog,wxID_ANY,wxString::Format("%.9g",double(value)));grid->Add(field,1,wxEXPAND);return field;
        };
        wxTextCtrl* values[]={input("Container X",inputs.containerSphere.position.x),input("Container Y",inputs.containerSphere.position.y),
            input("Container Z",inputs.containerSphere.position.z),input("Container radius",inputs.containerSphere.radius),
            input("Fade center X",inputs.scenePosition.x),input("Fade center Y",inputs.scenePosition.y),input("Fade center Z",inputs.scenePosition.z),
            input("Caster region X",inputs.casterRegion.x),input("Caster region Y",inputs.casterRegion.y),input("Caster region Z",inputs.casterRegion.z)};
        root->Add(grid,1,wxEXPAND|wxALL,10);
        const auto enableExplicit=[&] {
            const bool enabled=!automatic->GetValue();
            owner->Enable(enabled);collected->Enable(enabled);
            for(auto* field:values)field->Enable(enabled);
        };
        enableExplicit();
        automatic->Bind(wxEVT_CHECKBOX,[&](wxCommandEvent&){enableExplicit();});
        auto* limitEnabled=new wxCheckBox(&dialog,wxID_ANY,"Enable engine shadowdistance limit");limitEnabled->SetValue(bool(inputs.maximumCasterDistance));
        root->Add(limitEnabled,0,wxALL,10);
        auto* distance=new wxTextCtrl(&dialog,wxID_ANY,wxString::Format("%.9g",double(inputs.maximumCasterDistance.value_or(100.f))));
        root->Add(distance,0,wxEXPAND|wxLEFT|wxRIGHT,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALIGN_RIGHT|wxALL,10);dialog.SetSizerAndFit(root);
        if(dialog.ShowModal()!=wxID_OK)return;
        try {
            const auto read=[](wxTextCtrl* field) {double x=0;if(!field->GetValue().ToDouble(&x)||!std::isfinite(x)||!std::isfinite(float(x)))
                throw std::runtime_error("Shadow inputs must be finite numbers");return float(x);};
            inputs.mode=automatic->GetValue()?SceneShadowInputMode::SceneFocus:SceneShadowInputMode::Explicit;
            inputs.containerOwner=owner->GetSelection()==0?std::nullopt:
                std::optional<LightOwnerId>{casters.at(std::size_t(owner->GetSelection()-1)).id};
            inputs.containerSphere={{read(values[0]),read(values[1]),read(values[2])},read(values[3])};
            inputs.scenePosition={read(values[4]),read(values[5]),read(values[6])};
            inputs.casterRegion={read(values[7]),read(values[8]),read(values[9])};
            inputs.containerSource=collected->GetValue()?LightQuerySource::Room:LightQuerySource::Global;
            inputs.maximumCasterDistance=limitEnabled->GetValue()?std::optional<float>{read(distance)}:std::nullopt;
            page->setSceneShadowInputs(std::move(inputs));
        } catch(const std::exception& e) {wxui::showError(this,e);}
    }
    void chooseShadowCasters() {
        auto* page=activePage();if(!page)return;
        const auto casters=page->shadowScene().casters();wxArrayString names;wxArrayInt selected;
        for(std::size_t i=0;i<casters.size();++i) {
            names.Add(wxui::toWx(casters[i].label+" ["+std::to_string(casters[i].id)+"]"));
            if(page->shadowScene().casterEnabled(casters[i].id))selected.Add(int(i));
        }
        wxMultiChoiceDialog dialog(this,"Eligibility still follows visibility, classification and engine distance guards.","Scene shadow casters",names);
        dialog.SetSelections(selected);if(dialog.ShowModal()!=wxID_OK)return;
        const auto chosen=dialog.GetSelections();
        for(std::size_t i=0;i<casters.size();++i)page->setSceneCasterEnabled(casters[i].id,chosen.Index(int(i))!=wxNOT_FOUND);
    }

    void chooseArea() {
        auto* page = activePage();
        if (!page || !engineAreaRuntimeSupported(page->document().model().game)) {
            wxMessageBox("Open a model using a supported area runtime before loading area context.","Area context",wxOK|wxICON_INFORMATION,this);
            return;
        }
        wxFileDialog dialog(this,"Load area layout (packed room models)",wxEmptyString,wxEmptyString,
            "Area layouts (*.lyt)|*.lyt;*.LYT",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal()!=wxID_OK) return;
        try {
            std::shared_ptr<GameResourceSession> session;
            std::string warning;
            const auto installed=installedResourceResolver(page->document().model().game,page->document().path(),session,warning);
            const auto resolver=combineResolvers(captureHostResolver(),installed);
            const auto path=neosettings::pathFromWx(dialog.GetPath());
            page->suspendLoadingPlayback();
            struct ResumePage { ModelPage& page; ~ResumePage(){page.resumeLoadingPlayback();} } resume{*page};
            auto pending=page->prepareAreaLoad();
            if(!runJob("Loading area layout",[&](const neoshared::bif::JobControl& job) {
                pending.load(path,resolver,{},job);
            }))return;
            page->installArea(std::move(pending));
            setModuleStatusText("Area context loaded; room selection follows the camera when WOK data is available.",0);
        } catch (const neoshared::bif::JobCancelled&) {
            // The joined job retained the current area.
        } catch (const std::exception& error) {
            wxMessageBox(wxui::toWx(error.what()),"Area context not loaded",wxOK|wxICON_ERROR,this);
        }
    }
    void chooseAreaRoom() {
        auto* page=activePage();
        if (!page || page->areaScene().empty()) return;
        wxArrayString choices;
        choices.Add("Use replay room or camera WOK query");
        choices.Add("All rooms (manual override)");
        for (const auto& room:page->areaScene().rooms()) choices.Add(wxui::toWx(room.name));
        const int selected=wxGetSingleChoiceIndex("Scene current-room state", "Area visibility",choices,this);
        if (selected<0) return;
        if(selected==0)page->useReplayedAreaRoom();
        else page->setAreaRoom(selected==1 ? std::nullopt :
            std::optional<LightRoomId>{page->areaScene().rooms()[static_cast<std::size_t>(selected-2)].id});
    }

    void chooseWorldPlayback() {
        auto* page=activePage();if(!page)return;
        auto settings=page->worldPlaybackSettings();
        wxDialog dialog(this,wxID_ANY,"World Playback",wxDefaultPosition,wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);
        auto* engineRoom=new wxCheckBox(&dialog,wxID_ANY,"Derive the current room and scene focus from engine player/camera state");
        engineRoom->SetValue(settings.engineRoomSelection);root->Add(engineRoom,0,wxALL,10);
        wxArrayString names;std::vector<std::optional<HitOwnerId>> owners;
        names.Add("No player");owners.push_back({});
        if(page->document().hasModelOwner()) {names.Add("Main model");owners.push_back(0u);}
        for(const auto& object:page->areaScene().objects())if(object->kind()==AreaObjectKind::Creature) {
            names.Add(wxui::toWx(object->label()+" ["+std::to_string(object->instanceIndex())+"]"));
            owners.push_back(object->ownerId());
        }
        auto* player=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,names);
        player->SetSelection(0);
        for(std::size_t i=0;i<owners.size();++i)if(owners[i]==settings.playerOwner)player->SetSelection(int(i));
        root->Add(new wxStaticText(&dialog,wxID_ANY,"Player owner"),0,wxLEFT|wxRIGHT,10);
        root->Add(player,0,wxEXPAND|wxALL,10);
        auto* mode=new wxSpinCtrl(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
            wxSP_ARROW_KEYS,0,255,settings.cameraMode);
        root->Add(new wxStaticText(&dialog,wxID_ANY,"Camera mode (7 selects the module camera)"),0,wxLEFT|wxRIGHT,10);
        root->Add(mode,0,wxEXPAND|wxALL,10);
        auto* camera=new wxCheckBox(&dialog,wxID_ANY,"Supply the active view as the module camera");
        camera->SetValue(settings.moduleCameraPresent);root->Add(camera,0,wxALL,10);
        auto* paused=new wxCheckBox(&dialog,wxID_ANY,"Pause the module clock");
        paused->SetValue(settings.paused);root->Add(paused,0,wxALL,10);
        wxArrayString pauseCommands;
        for(const auto* text:{"Keep current engine pause state","Enable pause level 1","Disable pause level 1",
                             "Enable pause level 2","Disable pause level 2"})pauseCommands.Add(text);
        auto* pauseCommand=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,pauseCommands);
        pauseCommand->SetSelection(0);
        root->Add(new wxStaticText(&dialog,wxID_ANY,wxString::Format(
            "Engine pause command (current flags: %u)",unsigned(page->worldPauseMask()))),0,wxLEFT|wxRIGHT,10);
        root->Add(pauseCommand,0,wxEXPAND|wxALL,10);
        wxArrayString exclusionNames;std::vector<HitOwnerId> exclusionOwners;
        for(std::size_t i=0;i<owners.size();++i)if(owners[i]) {
            exclusionNames.Add(names[unsigned(i)]);exclusionOwners.push_back(*owners[i]);
        }
        const auto currentExclusions=page->worldPauseExclusions();
        auto* exclusions=new wxCheckListBox(&dialog,wxID_ANY,wxDefaultPosition,wxSize(-1,120),exclusionNames);
        for(std::size_t i=0;i<exclusionOwners.size();++i)
            exclusions->Check(unsigned(i),std::find(currentExclusions.begin(),currentExclusions.end(),
                exclusionOwners[i])!=currentExclusions.end());
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "Creatures excluded from the selected engine pause command"),0,wxLEFT|wxRIGHT,10);
        root->Add(exclusions,0,wxEXPAND|wxALL,10);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "Exclusions follow the selected command; they are never inferred from the player.\n"
            "Enabling an already active level 1 preserves its existing exclusion list."),0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        if(const auto& time=page->areaScene().environment().worldTime()) {
            root->Add(new wxStaticText(&dialog,wxID_ANY,wxui::toWx(
                "Module time: "+std::to_string(time->year)+"/"+std::to_string(time->month)+"/"+
                std::to_string(time->day)+", hour "+std::to_string(time->hour)+" ("+
                areaTimeOfDayName(time->phase)+")")),0,wxALL,10);
        } else root->Add(new wxStaticText(&dialog,wxID_ANY,"No module clock is loaded for this scene."),0,wxALL,10);
        auto* resetTime=new wxCheckBox(&dialog,wxID_ANY,"Reset the module clock to its loaded state");
        root->Add(resetTime,0,wxALL,10);
        auto* fadeDoors=new wxCheckBox(&dialog,wxID_ANY,"Enable engine door fading");
        fadeDoors->SetValue(settings.doorFade.enabled);root->Add(fadeDoors,0,wxALL,10);
        auto* fadeGrid=new wxFlexGridSizer(2,6,10);fadeGrid->AddGrowableCol(1);
        const auto fadeNumber=[&](const char* label,float value) {
            fadeGrid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            auto* field=new wxTextCtrl(&dialog,wxID_ANY,wxString::Format("%.9g",double(value)));
            fadeGrid->Add(field,1,wxEXPAND);return field;
        };
        wxTextCtrl* fadeValues[]={fadeNumber("Minimum door alpha",settings.doorFade.minimumAlpha),
            fadeNumber("Fade-to-zero time (seconds)",settings.doorFade.fadeToZeroTime),
            fadeNumber("Scene focus X",settings.sceneFocus.x),fadeNumber("Scene focus Y",settings.sceneFocus.y),
            fadeNumber("Scene focus Z",settings.sceneFocus.z)};
        root->Add(fadeGrid,0,wxEXPAND|wxALL,10);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "The bound player's position follows its live owner transform. Dialogue playback supplies its camera state.\n"
            "A missing player or room hit retains the current room. Manual and replay room overrides retain priority."),
            0,wxEXPAND|wxALL,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
        dialog.SetSizerAndFit(root);
        if(dialog.ShowModal()!=wxID_OK)return;
        settings.engineRoomSelection=engineRoom->GetValue();
        settings.paused=paused->GetValue();
        settings.playerOwner=owners[std::size_t(player->GetSelection())];
        settings.cameraMode=static_cast<std::uint8_t>(mode->GetValue());
        settings.moduleCameraPresent=camera->GetValue();
        float* destinations[]={&settings.doorFade.minimumAlpha,&settings.doorFade.fadeToZeroTime,
            &settings.sceneFocus.x,&settings.sceneFocus.y,&settings.sceneFocus.z};
        for(std::size_t i=0;i<5;++i) {
            double value=0;if(!fadeValues[i]->GetValue().ToDouble(&value)) {
                wxMessageBox("Enter numeric door fade and scene focus values.","World Playback",wxOK|wxICON_ERROR,this);return;
            }
            *destinations[i]=static_cast<float>(value);
        }
        settings.doorFade.enabled=fadeDoors->GetValue();
        page->setWorldPlaybackSettings(std::move(settings));
        const auto command=pauseCommand->GetSelection();
        if(command!=0) {
            std::vector<HitOwnerId> selected;
            for(std::size_t i=0;i<exclusionOwners.size();++i)
                if(exclusions->IsChecked(unsigned(i)))selected.push_back(exclusionOwners[i]);
            page->setWorldPauseState(command<=2?1u:2u,(command==1||command==3)?1:0,selected);
        }
        if(resetTime->GetValue())page->resetWorldTime();
    }

    void chooseAreaEnvironment() {
        auto* page=activePage();
        if(!page||page->areaScene().empty())return;
        const auto& area=page->areaScene();
        const auto& environment=area.environment();
        auto replay=environment.replay();
        wxDialog dialog(this,wxID_ANY,"Area Environment / Camera Replay",wxDefaultPosition,
                        wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "ARE day/night tuples color the scoped gidy_sun light model and fog state.\n"
            "Room selection follows the camera's WOK query unless a manual or replay room is selected."),
            0,wxEXPAND|wxALL,10);
        auto* useEnvironment=new wxCheckBox(&dialog,wxID_ANY,"Use loaded area environment instead of manual fog/ambient overrides");
        useEnvironment->SetValue(options_.useLoadedAreaEnvironment);root->Add(useEnvironment,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* useCamera=new wxCheckBox(&dialog,wxID_ANY,"Use replay camera when active (authored model camera retains priority)");
        useCamera->SetValue(options_.useAreaReplayCamera);root->Add(useCamera,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* followScene=new wxCheckBox(&dialog,wxID_ANY,"Use replay camera position as the shadow fade/scene anchor");
        followScene->SetValue(options_.shadowScenePositionFromAreaCamera);root->Add(followScene,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* followCasters=new wxCheckBox(&dialog,wxID_ANY,"Use replay camera position as the shadow caster-region anchor");
        followCasters->SetValue(options_.shadowCasterRegionFromAreaCamera);root->Add(followCasters,0,wxLEFT|wxRIGHT|wxBOTTOM,10);

        auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1,1);
        const auto add=[&](const char* label,wxWindow* control) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            grid->Add(control,1,wxEXPAND);
        };
        auto* phase=new wxChoice(&dialog,wxID_ANY);
        for(const auto* label:{"Module clock / authored endpoint","Day","Night","Dawn (night to day)","Dusk (day to night)"})phase->Append(label);
        if(!replay.phase)phase->SetSelection(0);
        else phase->SetSelection(*replay.phase==AreaTimeOfDay::Day?1:*replay.phase==AreaTimeOfDay::Night?2:
                                 *replay.phase==AreaTimeOfDay::Dawn?3:4);
        add("Time-of-day state",phase);
        auto* transition=new wxSpinCtrlDouble(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
            wxSP_ARROW_KEYS,0.0,1.0,replay.transition,0.01);transition->SetDigits(3);add("Dawn/dusk transition",transition);
        auto* fogOverride=new wxChoice(&dialog,wxID_ANY);fogOverride->Append("Authored endpoint");fogOverride->Append("Force disabled");fogOverride->Append("Force enabled");
        fogOverride->SetSelection(!replay.fogEnabled?0:*replay.fogEnabled?2:1);add("Fog enable state",fogOverride);
        auto* shadowOverride=new wxChoice(&dialog,wxID_ANY);shadowOverride->Append("Authored endpoint");shadowOverride->Append("Force disabled");shadowOverride->Append("Force enabled");
        shadowOverride->SetSelection(!replay.shadowsEnabled?0:*replay.shadowsEnabled?2:1);add("Shadow enable state",shadowOverride);
        auto* forceBright=new wxCheckBox(&dialog,wxID_ANY,"Set diffuse to black and global ambient to white");
        forceBright->SetValue(replay.forceBrightSunAndMoon);add("Engine ForceBrightSunAndMoon",forceBright);

        wxArrayString rooms;rooms.Add("No explicit replay room");
        for(const auto& room:area.rooms())rooms.Add(wxui::toWx(room.name+" ["+std::to_string(room.id)+"]"));
        const auto roomSelection=[&](std::optional<std::uint64_t> id) {
            if(!id)return 0;
            for(std::size_t i=0;i<area.rooms().size();++i)if(area.rooms()[i].id==*id)return int(i+1);
            return 0;
        };
        auto* replayRoom=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,rooms);
        replayRoom->SetSelection(roomSelection(replay.currentRoom));add("Replay current room",replayRoom);
        auto* followReplayRoom=new wxCheckBox(&dialog,wxID_ANY,"Release the manual room override after applying");
        followReplayRoom->SetValue(!area.currentRoomOverridden());add("Room ownership",followReplayRoom);

        AreaReplayCamera camera=replay.camera.value_or(AreaReplayCamera{});
        auto* cameraActive=new wxCheckBox(&dialog,wxID_ANY,"Active");cameraActive->SetValue(camera.active);add("Replay camera",cameraActive);
        const auto number=[&](double value,double minimum,double maximum,double increment,int digits=3) {
            auto* control=new wxSpinCtrlDouble(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
                wxSP_ARROW_KEYS,minimum,maximum,value,increment);control->SetDigits(digits);return control;
        };
        auto* cameraX=number(camera.world.values[12],-1000000,1000000,.1);add("Camera world X",cameraX);
        auto* cameraY=number(camera.world.values[13],-1000000,1000000,.1);add("Camera world Y",cameraY);
        auto* cameraZ=number(camera.world.values[14],-1000000,1000000,.1);add("Camera world Z",cameraZ);
        auto* cameraFov=number(camera.viewAngle,1,179,.25);add("Vertical FOV (degrees)",cameraFov);
        auto* cameraNear=number(camera.nearClip,.001,1000000,.01,4);add("Near clip",cameraNear);
        auto* cameraFar=number(camera.farClip,.002,10000000,1);add("Far clip",cameraFar);
        auto* cameraRoom=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,rooms);
        cameraRoom->SetSelection(roomSelection(camera.currentRoom));add("Camera-supplied room",cameraRoom);
        auto* cameraCut=new wxCheckBox(&dialog,wxID_ANY,"Invalidate temporal postprocess history on apply");add("Camera cut",cameraCut);
        auto* clearReplay=new wxCheckBox(&dialog,wxID_ANY,"Clear all explicit environment/camera replay state");add("Reset",clearReplay);
        root->Add(grid,1,wxEXPAND|wxLEFT|wxRIGHT,10);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "This dialog edits camera position only. Existing replay orientation is retained; a new camera uses identity orientation.\n"
            "The core API accepts the complete rigid camera transform for captured traces."),0,wxEXPAND|wxALL,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
        dialog.SetSizerAndFit(root);
        while(dialog.ShowModal()==wxID_OK) {
            options_.useLoadedAreaEnvironment=useEnvironment->GetValue();
            options_.useAreaReplayCamera=useCamera->GetValue();
            options_.shadowScenePositionFromAreaCamera=followScene->GetValue();
            options_.shadowCasterRegionFromAreaCamera=followCasters->GetValue();
            if(clearReplay->GetValue()) {
                page->clearAreaEnvironmentReplay();
                if(followReplayRoom->GetValue())page->useReplayedAreaRoom();
                applyOptions();break;
            }
            AreaEnvironmentReplayState state;
            switch(phase->GetSelection()) {
            case 1:state.phase=AreaTimeOfDay::Day;break;
            case 2:state.phase=AreaTimeOfDay::Night;break;
            case 3:state.phase=AreaTimeOfDay::Dawn;break;
            case 4:state.phase=AreaTimeOfDay::Dusk;break;
            default:break;
            }
            state.transition=static_cast<float>(transition->GetValue());
            if(fogOverride->GetSelection())state.fogEnabled=fogOverride->GetSelection()==2;
            if(shadowOverride->GetSelection())state.shadowsEnabled=shadowOverride->GetSelection()==2;
            state.forceBrightSunAndMoon=forceBright->GetValue();
            if(replayRoom->GetSelection()>0)state.currentRoom=area.rooms()[std::size_t(replayRoom->GetSelection()-1)].id;
            camera.active=cameraActive->GetValue();
            camera.world.values[12]=static_cast<float>(cameraX->GetValue());
            camera.world.values[13]=static_cast<float>(cameraY->GetValue());
            camera.world.values[14]=static_cast<float>(cameraZ->GetValue());
            camera.viewAngle=static_cast<float>(cameraFov->GetValue());
            camera.nearClip=static_cast<float>(cameraNear->GetValue());
            camera.farClip=static_cast<float>(cameraFar->GetValue());
            camera.currentRoom.reset();
            if(cameraRoom->GetSelection()>0)camera.currentRoom=area.rooms()[std::size_t(cameraRoom->GetSelection()-1)].id;
            camera.cut=cameraCut->GetValue();state.camera=camera;
            std::string error;
            if(!page->applyAreaEnvironmentReplay(std::move(state),error)) {
                wxMessageBox(wxui::toWx(error),"Area replay not applied",wxOK|wxICON_WARNING,&dialog);continue;
            }
            if(followReplayRoom->GetValue())page->useReplayedAreaRoom();
            applyOptions();break;
        }
    }

    void chooseAreaAnimation() {
        auto* page=activePage();if(!page||page->areaScene().empty())return;
        wxArrayString names;std::vector<const AreaObject*> objects;
        for(const auto& object:page->areaScene().objects())if(!object->staticPrimary()) {
            names.Add(wxui::toWx(object->label()+" — "+areaObjectKindName(object->kind())+
                " ["+std::to_string(object->instanceIndex())+"]"));objects.push_back(object.get());
        }
        if(objects.empty()){wxMessageBox("This layout has no animated room or track Gobs.","Area animation",wxOK|wxICON_INFORMATION,this);return;}
        const int selected=wxGetSingleChoiceIndex("Select an independent area owner", "Area animation",names,this);
        if(selected<0)return;const auto* object=objects.at(static_cast<std::size_t>(selected));
        wxArrayString actions;actions.Add("Pause primary run");actions.Add("Resume primary run");actions.Add("Reset this owner");
        const auto& doc=*object->document();
        for(std::size_t i=0;i<doc.animations().size();++i)actions.Add(wxui::toWx(doc.animation(i).name));
        const int action=wxGetSingleChoiceIndex("Reference clocks remain independent. Room scriptloop at rate 1 uses the engine null/overlay sequence.",
            wxui::toWx(object->label()),actions,this);
        if(action<0)return;
        if(action<2){page->pauseAreaObject(object->instanceIndex(),action==0);return;}
        if(action==2){page->resetAreaObject(object->instanceIndex());return;}
        const auto text=wxGetTextFromUser("Playback rate", "Area animation", "1",this);if(text.empty())return;
        double rate=0;if(!text.ToDouble(&rate)||!std::isfinite(rate)||rate<=0) {
            wxMessageBox("Enter a finite rate greater than 0.","Area animation",wxOK|wxICON_ERROR,this);return;
        }
        std::string error;
        if(!page->playAreaAnimation(object->instanceIndex(),doc.animation(static_cast<std::size_t>(action-3)).name,static_cast<float>(rate),error))
            wxMessageBox(wxui::toWx(error),"Area animation",wxOK|wxICON_ERROR,this);
    }
    void chooseRoomScriptAnimation() {
        auto* page=activePage();if(!page)return;
        wxArrayString names;for(const auto& room:page->areaScene().rooms())names.Add(wxui::toWx(room.name));
        const int selected=wxGetSingleChoiceIndex("Room receiving PlayRoomAnimation", "Room script animation",names,this);
        if(selected<0)return;
        wxDialog dialog(this,wxID_ANY,"Room script animation");auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,"Animation number (engine unsigned 16-bit conversion)"),0,wxALL,10);
        auto* number=new wxSpinCtrl(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
            wxSP_ARROW_KEYS,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),0);
        root->Add(number,0,wxEXPAND|wxALL,10);root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL,10);
        dialog.SetSizerAndFit(root);if(dialog.ShowModal()!=wxID_OK)return;
        page->playRoomAnimation(page->areaScene().rooms()[std::size_t(selected)].name,number->GetValue());
    }
    void chooseTrackAnimation() {
        auto* page=activePage();if(!page)return;
        if(page->areaScene().miniGame()) {
            wxArrayString commands;commands.Add("Activate loaded minigame tracks");
            commands.Add("Stop loaded minigame tracks");commands.Add("Inspect a track and its bindings");
            const auto command=wxGetSingleChoiceIndex(
                "Models and modelhook attachments come from the area's MiniGame records.",
                "Track playback",commands,this);
            if(command<0)return;
            if(command==0){page->startMiniGameTracks();return;}
            if(command==1){page->stopMiniGameTracks();return;}
        }
        wxArrayString names;std::vector<std::size_t> tracks;
        for(const auto& object:page->areaScene().objects())if(object->kind()==AreaObjectKind::Track) {
            names.Add(wxui::toWx(object->label()));tracks.push_back(object->instanceIndex());
        }
        const int selected=wxGetSingleChoiceIndex("Select the track Gob", "Track follower animation",names,this);
        if(selected<0)return;const auto track=tracks[std::size_t(selected)];
        TrackAnimationState state;
        if(const auto* captured=page->areaScene().trackAnimation(track))state=*captured;
        wxDialog dialog(this,wxID_ANY,"Track follower animation",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1);
        const auto add=[&](const char* label,wxWindow* control) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);grid->Add(control,1,wxEXPAND);
        };
        auto* kind=new wxChoice(&dialog,wxID_ANY);for(const auto* name:{"Base","Enemy","Player"})kind->Append(name);
        kind->SetSelection(static_cast<int>(state.kind));add("Follower kind",kind);
        auto* mode=new wxSpinCtrl(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
            wxSP_ARROW_KEYS,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),state.mode);add("Captured loop mode",mode);
        auto* speed=new wxTextCtrl(&dialog,wxID_ANY,wxString::Format("%.9g",double(state.speed)));add("Speed (100 = rate 1)",speed);
        std::string modelIds;for(const auto owner:state.models){if(!modelIds.empty())modelIds+=' ';modelIds+=std::to_string(owner);}
        auto* models=new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(modelIds));add("Ordered model instance IDs",models);
        auto* playerModel=new wxTextCtrl(&dialog,wxID_ANY,state.playerModel?wxui::toWx(std::to_string(*state.playerModel)):wxString{});
        add("Separate player model ID (optional)",playerModel);
        auto* action=new wxChoice(&dialog,wxID_ANY);
        for(const auto* name:{"Capture state only","Go","Stop","Set speed","Play follower animation","Remove follower animation"})action->Append(name);
        action->SetSelection(0);add("Command",action);
        auto* clip=new wxTextCtrl(&dialog,wxID_ANY);add("Follower animation",clip);
        auto* looping=new wxCheckBox(&dialog,wxID_ANY,"Looping");looping->SetValue(true);add("Animation",looping);
        auto* queued=new wxCheckBox(&dialog,wxID_ANY,"Queued");add("Queue",queued);
        auto* overlay=new wxCheckBox(&dialog,wxID_ANY,"Overlay");add("Layer",overlay);
        root->Add(grid,1,wxEXPAND|wxALL,10);
        std::string available="Available model IDs:";
        for(const auto& object:page->areaScene().objects())if(!object->staticPrimary())
            available+='\n'+std::to_string(object->instanceIndex())+" — "+object->label();
        root->Add(new wxStaticText(&dialog,wxID_ANY,wxui::toWx(available)),0,wxEXPAND|wxALL,10);
        root->Add(new wxStaticText(&dialog,wxID_ANY,"Bindings retain their entered order. These commands use the existing scene animation clocks."),0,wxALL,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);dialog.SetSizerAndFit(root);
        while(dialog.ShowModal()==wxID_OK) {
            try {
                double value=0;if(!speed->GetValue().ToDouble(&value))throw std::runtime_error("Enter a numeric track speed.");
                const auto requestedSpeed=static_cast<float>(value);
                state.kind=static_cast<TrackFollowerKind>(kind->GetSelection());state.mode=mode->GetValue();
                state.models.clear();std::istringstream list(models->GetValue().ToStdString());std::size_t owner=0;
                while(list>>owner)state.models.push_back(owner);
                if(!list.eof())throw std::runtime_error("Enter model instance IDs separated by spaces.");
                state.playerModel.reset();const auto playerText=playerModel->GetValue();
                if(!playerText.empty()) {
                    std::istringstream one(playerText.ToStdString());if(!(one>>owner))throw std::runtime_error("Enter one player model instance ID.");
                    one>>std::ws;if(!one.eof())throw std::runtime_error("Enter one player model instance ID.");state.playerModel=owner;
                }
                const auto validateBinding=[&](std::size_t id) {
                    const auto* object=page->areaScene().object(id);
                    if(!object||object->instanceIndex()!=id||object->staticPrimary())
                        throw std::runtime_error("Model instance ID does not identify an available primary Gob: "+std::to_string(id));
                };
                for(const auto id:state.models)validateBinding(id);
                if(state.playerModel)validateBinding(*state.playerModel);
                // Capture preserves running/event state. The SetSpeed caller,
                // rather than a plain snapshot write, updates an existing run.
                if(action->GetSelection()!=3)state.speed=requestedSpeed;
                page->captureTrackAnimation(track,state);
                switch(action->GetSelection()) {
                case 1:page->startTrackAnimation(track);break;
                case 2:page->stopTrackAnimation(track);break;
                case 3:page->setTrackSpeed(track,requestedSpeed);break;
                case 4:page->playTrackFollowerAnimation(track,clip->GetValue().ToStdString(),looping->GetValue(),queued->GetValue(),overlay->GetValue());break;
                case 5:page->removeTrackFollowerAnimation(track,clip->GetValue().ToStdString());break;
                default:break;
                }
                break;
            } catch(const std::exception& error) {wxMessageBox(wxui::toWx(error.what()),"Track animation",wxOK|wxICON_ERROR,&dialog);}
        }
    }

    void chooseDoorState() {
        auto* page=activePage();if(!page)return;
        wxArrayString names;std::vector<std::size_t> instances;
        for(const auto& object:page->areaScene().objects())if(object->kind()==AreaObjectKind::Door) {
            names.Add(wxui::toWx(object->label()));instances.push_back(object->instanceIndex());
        }
        const int selected=wxGetSingleChoiceIndex("Choose an loaded door", "Door OpenState",names,this);
        if(selected<0)return;
        wxArrayString states;
        for(unsigned state=0;state<4;++state)states.Add(wxString::Format("OpenState %u",state));
        const int state=wxGetSingleChoiceIndex("Apply the engine serialized door state", "Door OpenState",states,this);
        if(state<0)return;
        const auto* object=page->areaScene().object(instances.at(static_cast<std::size_t>(selected)));
        const auto& command=object->effectiveVisualState().animation;
        const auto initial=command?command->doorAnimationSelector:1;
        wxDialog selectorDialog(this,wxID_ANY,"Door animation selector");
        auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&selectorDialog,wxID_ANY,
            "Engine OpenState 3 uses client animation 0x147 when this value is zero, and 0x14A otherwise.\n"
            "The engine door constructor initializes it to 1."),0,wxALL,10);
        auto* selector=new wxSpinCtrl(&selectorDialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
            wxSP_ARROW_KEYS,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),initial);
        box->Add(selector,0,wxEXPAND|wxALL,10);
        box->Add(selectorDialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL,10);selectorDialog.SetSizerAndFit(box);
        if(selectorDialog.ShowModal()!=wxID_OK)return;
        std::string error;
        if(!page->setDoorOpenState(instances.at(static_cast<std::size_t>(selected)),static_cast<std::uint8_t>(state),selector->GetValue(),error))
            wxMessageBox(wxui::toWx(error),"Door state not applied",wxOK|wxICON_ERROR,this);
    }
    void showSceneAudio() {
        auto* page=activePage();if(!page)return;
        wxDialog dialog(this,wxID_ANY,"Scene Audio");auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&dialog,wxID_ANY,wxui::toWx(page->sceneAudioStatus())),0,wxALL,10);
        wxArrayString commands;commands.Add("Run loaded sound objects");commands.Add("Prepare sound events");commands.Add("Stop scene sounds");
        auto* action=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,commands);action->SetSelection(0);
        box->Add(action,0,wxEXPAND|wxALL,10);
        std::string stateText;
        if(const auto state=page->sceneAudioRandomState())for(auto value:*state) {
            if(!stateText.empty())stateText+=' ';stateText+=std::to_string(value);
        }
        box->Add(new wxStaticText(&dialog,wxID_ANY,
            "CExoRand state: four captured unsigned words, after engine construction.\n"
            "Required for authored interval variation. Leave blank to retain the current state."),0,wxALL,10);
        auto* state=new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(stateText),wxDefaultPosition,wxSize(600,-1));
        box->Add(state,0,wxEXPAND|wxALL,10);
        box->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,10);dialog.SetSizerAndFit(box);
        if(dialog.ShowModal()!=wxID_OK)return;
        try {
            if(action->GetSelection()==2){page->stopSceneAudio();return;}
            std::optional<ExoRandom::State> captured;
            if(action->GetSelection()==0&&!state->GetValue().empty()) {
                ExoRandom::State words{};std::istringstream text(state->GetValue().ToStdString());
                for(auto& word:words) {
                    std::uint64_t value{};
                    if(!(text>>value)||value>0xffffffffu)throw std::runtime_error("Enter four unsigned 32-bit state words.");
                    word=static_cast<std::uint32_t>(value);
                }
                text>>std::ws;if(!text.eof())throw std::runtime_error("Enter exactly four state words.");captured=words;
            }
            if(action->GetSelection()==0)page->configureSceneAudio(captured);
            else page->ensureSceneAudioProvider();
        }catch(const std::exception& error){wxui::showError(this,error);}
    }
    void showAreaWeather() {
        auto* page=activePage();if(!page||page->areaScene().empty())return;
        const bool k1=page->areaScene().game()==model::GameVersion::Kotor1;
        const auto current=page->weatherInputs();
        wxDialog dialog(this,wxID_ANY,"Engine area weather");auto* box=new wxBoxSizer(wxVERTICAL);
        auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1);
        const auto add=[&](const char* label,wxWindow* control) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);grid->Add(control,1,wxEXPAND);
        };
        const auto check=[&](const char* label,bool value) {
            auto* control=new wxCheckBox(&dialog,wxID_ANY,wxEmptyString);control->SetValue(value);add(label,control);return control;
        };
        auto* enabled=check("Run engine weather",current.enabled);
        auto* schedule=check("Run the engine area weather schedule",current.serverSchedule);
        auto* supplyPlayer=check("Supply player position",bool(current.playerPosition));
        const auto position=current.playerPosition.value_or(model::Vec3{});
        const auto number=[&](const char* label,float value) {
            auto* field=new wxTextCtrl(&dialog,wxID_ANY,wxString::Format("%.9g",double(value)));add(label,field);return field;
        };
        auto* x=number("Player world X",position.x);auto* y=number("Player world Y",position.y);auto* z=number("Player world Z",position.z);
        auto* camera=check("Supply viewer camera as module camera",page->weatherUsesViewerCamera());
        auto* roomDisabled=check("Player-room weather-disabled result",current.roomWeatherDisabled);
        auto* thunder=check("Ambient weather sound enabled",current.thunderEnabled);
        roomDisabled->Enable(!k1);thunder->Enable(!k1);
        auto* overrideType=new wxSpinCtrl(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,wxSP_ARROW_KEYS,0,255,current.serverOverride);
        add("Captured server override (255 allows scheduling)",overrideType);
        auto* shadow=number("Engine shadowalpha replay state",page->weatherShadowAlpha());
        wxArrayString commands;commands.Add("Keep current transition");commands.Add("Start rain");commands.Add("Start snow");commands.Add("Stop weather");
        auto* command=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,commands);command->SetSelection(0);add("Engine command",command);
        auto* sounds=new wxTextCtrl(&dialog,wxID_ANY);add("Explicit thunder resource directory (optional)",sounds);sounds->Enable(!k1);
        box->Add(grid,1,wxEXPAND|wxALL,10);
        const auto& state=page->areaScene().weather();
        box->Add(new wxStaticText(&dialog,wxID_ANY,wxString::Format("Current type %u, density %.6g, transition %u ms.",
            unsigned(state.currentType()),double(state.density()),unsigned(state.transitionMilliseconds()))),0,wxALL,10);
        box->Add(new wxStaticText(&dialog,wxID_ANY,
            "Player and module-camera inputs are independent. Weather placement requires both.\n"
            "The server override gates scheduling; use the engine command separately to change client weather.\n"
            "Disabling playback destroys weather Gobs. The engine shadowalpha has no renderer consumer in the engine."),0,wxALL,10);
        box->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,10);dialog.SetSizerAndFit(box);
        if(dialog.ShowModal()!=wxID_OK)return;
        try {
            const auto read=[](wxTextCtrl* field) {double value=0;if(!field->GetValue().ToDouble(&value))throw std::runtime_error("Enter numeric weather inputs.");return static_cast<float>(value);};
            WeatherFrameInput input=current;input.enabled=enabled->GetValue();input.serverSchedule=schedule->GetValue();
            input.playerPosition.reset();if(supplyPlayer->GetValue())input.playerPosition=model::Vec3{read(x),read(y),read(z)};
            input.roomWeatherDisabled=roomDisabled->GetValue();input.thunderEnabled=thunder->GetValue();
            input.serverOverride=static_cast<std::uint8_t>(overrideType->GetValue());input.shadowAlpha=read(shadow);
            const auto soundDirectory=neosettings::pathFromWx(sounds->GetValue());
            const auto resolver=page->areaScene().resourceResolver();
            const auto asciiResources=page->areaScene().asciiResourceResolver();
            PreparedWeatherPlayback prepared;
            std::vector<std::string> missing;
            if(input.enabled&&!runJob("Loading engine weather models and sound",[&](const neoshared::bif::JobControl& job) {
                prepared.resources=prepareWeatherResources(resolver,job,page->areaScene().game(),asciiResources);
                const auto soundResolver=soundDirectory.empty()?resolver:directoryResources(soundDirectory,resolver);
                if(!k1) {
                    const std::vector<std::string> names{"as_wt_thundercl1","as_wt_thundercl2","as_wt_thunderds1","as_wt_thunderds2"};
                    prepared.thunder=prepareSceneAudioPlayback(names,soundResolver,
                        page->areaScene().game(),job);
                    for(const auto& name:names)
                        if(!prepared.thunder.sounds.count(SceneAudioResourceKey{name,4u,false}))missing.push_back(name);
                }
            }))return;
            std::optional<std::uint8_t> requested;
            if(command->GetSelection()>0)requested=command->GetSelection()==3?0u:static_cast<std::uint8_t>(command->GetSelection());
            page->configureWeather(std::move(prepared),input,camera->GetValue(),requested);
            if(!missing.empty()) {
                std::string message="Unavailable thunder resources:\n";for(const auto& name:missing)message+=name+"\n";
                wxMessageBox(wxui::toWx(message),"Weather resource report",wxOK|wxICON_INFORMATION,this);
            }
        } catch(const std::exception& e){wxui::showError(this,e);}
    }
    void showDialoguePlayback() {
        auto* page=activePage();if(!page)return;
        wxArrayString actions;actions.Add("Load and play a dialogue");actions.Add("Choose a reply");actions.Add("Stop playback");
        const int action=wxGetSingleChoiceIndex("Dialogue playback", "Dialogue Playback",actions,this);
        if(action<0)return;
        if(action==2){page->stopDialogue();return;}
        if(action==1) {
            const auto& runtime=page->dialogue();
            if(!runtime.waitingForReply()) {
                wxMessageBox("The current dialogue is not waiting for a reply.","Dialogue Playback",wxOK|wxICON_INFORMATION,this);return;
            }
            wxArrayString replies;
            for(const auto index:runtime.replies()) {
                const auto& line=runtime.dialogue().lines.at(index);
                replies.Add(wxui::toWx(line.label()+": "+line.text));
            }
            const int selected=wxGetSingleChoiceIndex("Choose the player reply", "Dialogue Playback",replies,this);
            if(selected>=0)page->chooseDialogueReply(runtime.dialogue().lines.at(runtime.replies().at(std::size_t(selected))).index);
            return;
        }
        wxFileDialog file(this,"Load dialogue",wxEmptyString,wxEmptyString,
            "Dialogue (*.dlg)|*.dlg;*.DLG",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if(file.ShowModal()!=wxID_OK)return;
        const auto path=neosettings::pathFromWx(file.GetPath());
        try {
            const auto game=page->document().model().game;
            auto voice=page->voiceResolver();
            std::shared_ptr<GameResourceSession> installation=page->resourceSession();
            std::string warning;
            if(!voice) {
                installedResourceResolver(game,page->document().path(),installation,warning);
                if(installation)voice=installation->voiceResolver();
            }
            wxDialog settings(this,wxID_ANY,"Dialogue resources");auto* box=new wxBoxSizer(wxVERTICAL);
            auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1);
            const auto add=[&](const char* name,wxWindow* control) {
                grid->Add(new wxStaticText(&settings,wxID_ANY,name),0,wxALIGN_CENTER_VERTICAL);grid->Add(control,1,wxEXPAND);
            };
            auto* directory=new wxTextCtrl(&settings,wxID_ANY,neosettings::pathToWx(path.parent_path()),wxDefaultPosition,wxSize(500,-1));
            add("Explicit resource directory",directory);
            std::filesystem::path tlkPath;
            if(installation&&installation->installation())
                if(const auto table=uniqueResourceFile(installation->installation()->installPath,"dialog.tlk"))tlkPath=*table;
            auto* table=new wxTextCtrl(&settings,wxID_ANY,neosettings::pathToWx(tlkPath));add("Talk table (blank for embedded text)",table);
            auto* delay=new wxTextCtrl(&settings,wxID_ANY,"0");add("LIP delay (K2, seconds)",delay);
            delay->Enable(game==model::GameVersion::Kotor2);
            auto* ownerTag=new wxTextCtrl(&settings,wxID_ANY);add("Conversation owner Tag (explicit runtime input)",ownerTag);
            wxArrayString genders;genders.Add("Male");genders.Add("Female");
            auto* gender=new wxChoice(&settings,wxID_ANY,wxDefaultPosition,wxDefaultSize,genders);gender->SetSelection(0);add("Localized text variant",gender);
            box->Add(grid,1,wxEXPAND|wxALL,10);
            box->Add(new wxStaticText(&settings,wxID_ANY,
                "Explicit directory resources precede the captured game scope.\n"
                "Script conditions and actions require a game host; playback reports those boundaries.\n"
                "Choose replies through this menu while the dialogue is waiting."),0,wxALL,10);
            box->Add(settings.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,10);settings.SetSizerAndFit(box);
            if(settings.ShowModal()!=wxID_OK)return;
            double lipDelay=0;if(!delay->GetValue().ToDouble(&lipDelay))throw std::runtime_error("Enter a numeric LIP delay.");
            const auto resourcesPath=neosettings::pathFromWx(directory->GetValue());
            tlkPath=neosettings::pathFromWx(table->GetValue());
            const auto textGender=static_cast<std::uint32_t>(gender->GetSelection());
            const auto engineOwnerTag=wxui::toStd(ownerTag->GetValue());
            const auto archive=page->areaScene().empty()?page->document().resourceResolver():page->areaScene().resourceResolver();
            const auto sceneEffects=page->sceneEffects();
            const auto hardware=material::selectK2HardwareCapabilities(options_.engineHardware);
            PreparedDialoguePlayback prepared;
            std::vector<std::string> diagnostics;
            if(!runJob("Loading dialogue, voice and camera resources",[&](const neoshared::bif::JobControl& job) {
                const auto resources=resourcesPath.empty()?archive:directoryResources(resourcesPath,archive);
                ResourceResolver sounds=voice;
                if(!resourcesPath.empty()) {
                    const auto explicitFiles=directoryResources(resourcesPath,{});
                    sounds=[explicitFiles,voice,game](const std::string& name,std::uint16_t type)
                        ->std::optional<neoshared::ResourceDocument> {
                        auto resource=game==model::GameVersion::Kotor1
                            ?resolveK1DialogueVoice(explicitFiles,name,type)
                            :explicitFiles(name,type);
                        if(resource)return resource;
                        return voice?voice(name,type):std::nullopt;
                    };
                }
                neotlk::TlkLookup tlk;
                neoshared::dlg::ReadOptions options;options.runtimeFields=true;options.preserveBlankText=true;options.gender=textGender;
                options.checkpoint=[&]{job.check();};
                if(!tlkPath.empty()) {job.update(0,0,"Loading dialogue talk table");tlk.load(tlkPath);options.talkTable=&tlk;options.language=tlk.languageId();}
                prepared.dialogue=neoshared::dlg::read(path,options);
                if(resources)if(auto resource=resources("VideoEffects",0x7e1)) {
                    auto table=std::make_shared<neo2da::TwoDAFile>();
                    table->loadBytes(resource->bytes);job.check();prepared.videoEffects=std::move(table);
                }
                std::set<std::string> effectTextures;
                for(const auto& line:prepared.dialogue.lines)if(line.camera.videoEffect&&*line.camera.videoEffect>=0) {
                    const auto effect=activateVideoEffect(prepared.videoEffects.get(),*line.camera.videoEffect,
                        sceneEffects,game,
                        hardware.frameBufferModifications||hardware.atiFrameBufferModifications);
                    for(const auto role:requiredSceneEffectTextures(effect.settings,false,game))
                        effectTextures.insert(sceneEffectTextureName(effect.settings,role));
                    if(effect.settings.furyLevel)effectTextures.insert(effect.settings.furyTexture);
                }
                for(const auto& name:effectTextures)if(!name.empty()) {
                    job.update(0,0,"Loading dialogue effect texture "+name);std::string error;
                    if(!page->document().loadRuntimeTexture(name,resources,error))throw std::runtime_error(error);
                }
                auto loadedVoices=loadDialogueVoices(prepared.dialogue,resources,game,job);
                prepared.lines.resize(prepared.dialogue.lines.size());
                for(std::size_t i=0;i<prepared.lines.size();++i) {
                    auto& line=prepared.dialogue.lines[i];auto& asset=prepared.lines[i];
                    if(game==model::GameVersion::Kotor2) {
                        line.voiceResRef=loadedVoices[i].resref;
                        line.soundFlags=loadedVoices[i].flags;
                    }
                    job.update(i,prepared.lines.size(),"Loading "+line.label()+" "+loadedVoices[i].resref);
                    if(!line.textProblem.empty())diagnostics.push_back(line.label()+": "+line.textProblem);
                    for(const auto& voiceResRef:dialogueVoiceCandidates(loadedVoices[i],line.kind,game)) {
                        job.check();
                        DialogueVoiceAsset voiceAsset;voiceAsset.resref=voiceResRef;
                        // HandleDialogSound tries engine source type 8, then 4
                        // only when Play fails. Alien candidates share this path.
                        if(sounds&&!voiceResRef.empty())for(const auto type:{std::uint16_t{8},std::uint16_t{4}}) {
                            const auto resource=sounds(voiceResRef,type);
                            if(!resource)continue;
                            voiceAsset.resourceAvailable=true;
                            try {
                                const auto source=dialogueAudioSourceSpan(resource->bytes,type,game);
                                if(!source)continue;
                                if(source->offset==0&&source->size==resource->bytes.size())
                                    voiceAsset.audio=neoshared::audio::decodeBytes(resource->bytes,[&]{job.check();});
                                else {
                                    const auto begin=resource->bytes.begin()+static_cast<std::ptrdiff_t>(source->offset);
                                    const std::vector<std::uint8_t> bytes(begin,begin+static_cast<std::ptrdiff_t>(source->size));
                                    voiceAsset.audio=neoshared::audio::decodeBytes(bytes,[&]{job.check();});
                                }
                            }
                            catch(const neoshared::bif::JobCancelled&){throw;}
                            catch(const std::exception& e){diagnostics.push_back(resource->identity+": "+e.what());}
                            if(voiceAsset.audio)break;
                        }
                        if(resources&&!voiceResRef.empty())if(const auto lip=resources(voiceResRef,0xbbc))
                            voiceAsset.visemes=creatureVisemes(neoshared::lip::readLip(lip->bytes,lip->identity),static_cast<float>(lipDelay),game);
                        voiceAsset.timing=dialogueNodeTiming(prepared.dialogue,line,
                            {true,voiceAsset.resourceAvailable,engineOwnerTag},game);
                        asset.voices.push_back(std::move(voiceAsset));
                    }
                    prepared.timing.push_back(asset.voices.front().timing);
                }
                prepared.speech.initialize(resourceKey(neoshared::pathToUtf8(prepared.dialogue.source.stem())),
                    std::move(loadedVoices),game);
                if(resources&&!prepared.dialogue.cameraModel.empty()) {
                    if(auto camera=resources(prepared.dialogue.cameraModel,0x7d2)) {
                        prepared.cameraModel=std::make_unique<ModelDocument>();
                        prepared.cameraModel->openResource(std::move(*camera),resources,job,game);
                    } else diagnostics.push_back("CameraModel is unavailable: "+prepared.dialogue.cameraModel);
                }
            }))return;
            wxArrayString entries;std::vector<std::size_t> entryIndices;
            for(const auto& line:prepared.dialogue.lines)if(line.kind==neoshared::dlg::NodeKind::Entry) {
                entries.Add(wxui::toWx(line.label()+": "+line.text));entryIndices.push_back(line.index);
            }
            const int entry=wxGetSingleChoiceIndex("Choose the explicit starting entry", "Dialogue Playback",entries,this);
            if(entry<0)return;
            struct Actor {std::optional<std::size_t> instance;bool bound;float height{0.5f};bool appearance565{};bool creature{true};bool captured{},canPlay{},injured{};std::uint16_t appearance{};DialogueParticipantInputs participant;};
            std::vector<Actor> actors{{{},false}};wxArrayString actorNames;actorNames.Add("Unbound actor");
            if(page->document().hasModelOwner()) {actors.push_back({{},true});actorNames.Add("Main model (creature actor)");}
            for(const auto& object:page->areaScene().objects())if(object->kind()==AreaObjectKind::Creature||
                object->kind()==AreaObjectKind::Placeable||object->kind()==AreaObjectKind::Door) {
                Actor actor{object->instanceIndex(),true};actor.creature=object->kind()==AreaObjectKind::Creature;
                for(const auto& placement:page->areaScene().placements())if(placement.runtimeInstance==actor.instance&&placement.creatureAppearance)
                    {actor.height=placement.creatureAppearance->height;actor.appearance565=placement.creatureAppearance->appearance==565u;actor.appearance=placement.creatureAppearance->appearance;}
                actors.push_back(actor);actorNames.Add(wxui::toWx(object->label()));
            }
            std::map<std::string,Actor> bindings;
            const auto bindActor=[&](const std::string& tag,const std::string& role)->std::optional<Actor> {
                const auto found=bindings.find(tag);if(found!=bindings.end())return found->second;
                const int chosen=wxGetSingleChoiceIndex(wxui::toWx("Bind "+role+": "+tag),"Dialogue actors",actorNames,this);
                if(chosen<0)return {};
                auto& actor=actors.at(static_cast<std::size_t>(chosen));
                if(actor.bound&&actor.creature&&!actor.captured) {
                    wxDialog state(this,wxID_ANY,"Captured creature dialogue state");
                    auto* content=new wxBoxSizer(wxVERTICAL);
                    content->Add(new wxStaticText(&state,wxID_ANY,actorNames[chosen]),0,wxALL,10);
                    auto* canPlay=new wxCheckBox(&state,wxID_ANY,"Can play dialogue animations");
                    auto* injured=new wxCheckBox(&state,wxID_ANY,"Injured");
                    canPlay->SetValue(true);
                    content->Add(canPlay,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
                    content->Add(injured,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
                    content->Add(new wxStaticText(&state,wxID_ANY,"Appearance row for head tracking"),0,wxLEFT|wxRIGHT,10);
                    auto* appearance=new wxSpinCtrl(&state,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
                        wxSP_ARROW_KEYS,0,65535,actor.appearance);
                    // Loaded actors already carry the displayed (including disguise) appearance.
                    appearance->Enable(!actor.instance);content->Add(appearance,0,wxALL|wxEXPAND,10);
                    content->Add(new wxStaticText(&state,wxID_ANY,"Live client walk rate"),0,wxLEFT|wxRIGHT,10);
                    auto* walkRate=new wxTextCtrl(&state,wxID_ANY,"0");content->Add(walkRate,0,wxALL|wxEXPAND,10);
                    auto* orientLock=new wxCheckBox(&state,wxID_ANY,"Excluded from dialogue body orientation");
                    auto* headLock=new wxCheckBox(&state,wxID_ANY,"Excluded from dialogue head following");
                    auto* lookSuppressed=new wxCheckBox(&state,wxID_ANY,"Creature LookAt suppression field is nonzero");
                    auto* objectLookFlag=new wxCheckBox(&state,wxID_ANY,"Object LookAt suppression field is nonzero");
                    for(auto* control:{orientLock,headLock,lookSuppressed,objectLookFlag})
                        content->Add(control,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
                    content->Add(new wxStaticText(&state,wxID_ANY,
                        "Supply the captured game state for this creature.\n"
                        "These values apply throughout this playback."),0,wxALL,10);
                    content->Add(state.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,10);
                    state.SetSizerAndFit(content);if(state.ShowModal()!=wxID_OK)return {};
                    double capturedWalkRate;
                    if(!walkRate->GetValue().ToDouble(&capturedWalkRate))
                        throw std::runtime_error("The captured walk rate must be a number.");
                    actor.participant.walkRate=static_cast<float>(capturedWalkRate);
                    actor.participant.preventOrientation=orientLock->GetValue();
                    actor.participant.preventHeadFollow=headLock->GetValue();
                    actor.participant.lookAtSuppressed=lookSuppressed->GetValue();
                    actor.participant.objectLookAtFlag=objectLookFlag->GetValue();
                    actor.appearance=static_cast<std::uint16_t>(appearance->GetValue());
                    actor.canPlay=canPlay->GetValue();actor.injured=injured->GetValue();actor.captured=true;
                }
                bindings.emplace(tag,actor);return actor;
            };
            for(std::size_t i=0;i<prepared.lines.size();++i) {
                const auto& line=prepared.dialogue.lines[i];
                const auto speaker=!line.speaker.empty()?line.speaker:
                    line.kind==neoshared::dlg::NodeKind::Reply?std::string("Player"):std::string("Conversation owner");
                const auto spoken=bindActor(speaker,"speaker");if(!spoken)return;
                auto& asset=prepared.lines[i];asset.speaker=spoken->instance;asset.speakerBound=spoken->bound;asset.speakerHeight=spoken->height;asset.speakerAppearance565=spoken->appearance565;asset.speakerCreature=spoken->creature;
                const auto listener=line.listener.empty()?std::string("Listener (empty engine tag)"):line.listener;
                const auto heard=bindActor(listener,"listener");if(!heard)return;
                asset.listener=heard->instance;asset.listenerBound=heard->bound;asset.listenerHeight=heard->height;
            }
            struct ActorRequest {
                std::size_t line{};std::uint16_t authored{};Actor binding;
                const ModelDocument* document{};
                std::optional<CreatureAppearance> appearance;
                std::optional<CreatureAppearanceState> state;
                DialogueAnimationContext context;
                std::optional<ObjectAnimationCommand> engineState;
            };
            std::vector<ActorRequest> requests;
            for(std::size_t i=0;i<prepared.dialogue.lines.size();++i)for(const auto& animation:prepared.dialogue.lines[i].animations) {
                const auto participant=animation.participant.empty()?std::string("Animation participant (empty tag)"):animation.participant;
                const auto binding=bindActor(participant,"animation participant");if(!binding)return;
                if(!binding->bound)continue;
                ActorRequest request;request.line=i;request.authored=animation.animation.value_or(0);request.binding=*binding;
                if(request.binding.creature)request.context.family=DialogueAnimationFamily::Creature;
                request.document=&page->document();
                if(request.binding.instance) {
                    const auto* object=page->areaScene().object(*request.binding.instance);
                    request.document=object->document().get();request.engineState=object->effectiveVisualState().animation;
                    if(object->kind()==AreaObjectKind::Door) {
                        request.context.family=DialogueAnimationFamily::Door;
                        if(object->effectiveVisualState().animation)
                            request.context.doorSelector=object->effectiveVisualState().animation->doorAnimationSelector;
                    } else if(object->kind()==AreaObjectKind::Placeable)request.context.family=DialogueAnimationFamily::Placeable;
                    for(const auto& placement:page->areaScene().placements())if(placement.runtimeInstance==request.binding.instance) {
                        request.appearance=placement.creatureAppearance;request.state=placement.creatureState;break;
                    }
                }
                requests.push_back(std::move(request));
            }
            std::map<std::optional<std::size_t>,ActorRequest> resetRequests;
            for(const auto& binding:bindings)if(binding.second.bound) {
                ActorRequest request;request.binding=binding.second;request.document=&page->document();
                if(request.binding.creature)request.context.family=DialogueAnimationFamily::Creature;
                if(request.binding.instance) {
                    const auto* object=page->areaScene().object(*request.binding.instance);
                    request.document=object->document().get();request.engineState=object->effectiveVisualState().animation;
                    if(object->kind()==AreaObjectKind::Door) {
                        request.context.family=DialogueAnimationFamily::Door;
                        if(request.engineState)request.context.doorSelector=request.engineState->doorAnimationSelector;
                    } else if(object->kind()==AreaObjectKind::Placeable)request.context.family=DialogueAnimationFamily::Placeable;
                    for(const auto& placement:page->areaScene().placements())if(placement.runtimeInstance==request.binding.instance) {
                        request.appearance=placement.creatureAppearance;request.state=placement.creatureState;break;
                    }
                }
                resetRequests.emplace(request.binding.instance,std::move(request));
            }
            if(!runJob("Resolving engine dialogue animations",[&](const neoshared::bif::JobControl& job) {
                for(std::size_t i=0;i<requests.size();++i) {
                    job.update(i,requests.size(),"Resolving dialogue participant animation");
                    const auto& request=requests[i];const auto resources=request.document->resourceResolver();
                    auto context=request.context;
                    if(request.binding.canPlay&&request.appearance&&request.state)
                        context=dialogueAnimationContext(*request.appearance,*request.state,resources);
                    const bool dispatchable=context.family==DialogueAnimationFamily::Placeable ||
                        (context.family==DialogueAnimationFamily::Creature&&request.binding.canPlay);
                    DialogueAnimationCommand command;command.family=context.family;command.serverAnimation=request.authored;
                    if(dispatchable) {
                        command=resolveDialogueAnimation(request.authored,resources,*request.document,context);
                        if(command.dialog&&!command.resolved())throw std::runtime_error(command.diagnostic);
                    }
                    prepared.lines[request.line].animations.push_back({request.binding.instance,std::move(command)});
                }
                for(std::size_t i=0;i<prepared.lines.size();++i) {
                    auto& asset=prepared.lines[i];const auto& line=prepared.dialogue.lines[i];
                    if(!asset.speakerBound||prepared.dialogue.conversationType.value_or(0)==1||line.camera.angle.value_or(0)>1u)continue;
                    std::uint16_t serverAnimation=10038u;
                    for(const auto& request:requests)if(request.line==i&&request.binding.instance==asset.speaker) {
                        serverAnimation=request.authored;break;
                    }
                    const auto& speaker=resetRequests.at(asset.speaker);
                    asset.cameraPullback=dialogueCameraPullback(serverAnimation,speaker.document->resourceResolver());
                }
                for(const auto& pair:resetRequests) {
                    job.check();const auto& request=pair.second;const auto resources=request.document->resourceResolver();
                    auto context=request.context;
                    if(request.binding.canPlay&&request.appearance&&request.state)
                        context=dialogueAnimationContext(*request.appearance,*request.state,resources);
                    PreparedDialogueActor actor;actor.owner=request.binding.instance;
                    actor.canPlay=request.binding.canPlay;actor.injured=request.binding.injured;
                    actor.creature=request.binding.creature;actor.participant=request.binding.participant;
                    if(actor.creature) {
                        if(!resources)throw std::runtime_error("Dialogue head tracking requires appearance.2da.");
                        const auto appearance=resources("appearance",0x7e1);
                        if(!appearance)throw std::runtime_error("Dialogue head tracking requires appearance.2da.");
                        neo2da::TwoDAFile table;table.loadBytes(appearance->bytes);job.check();
                        actor.headTracking=dialogueHeadTracking(table,request.binding.appearance);
                    }
                    const auto resolve=[&](std::uint16_t id) {
                        auto command=resolveDialogueAnimation(id,resources,*request.document,context);
                        if(!command.resolved())throw std::runtime_error(command.diagnostic);
                        return DialogueActorAnimation{actor.owner,std::move(command)};
                    };
                    actor.pause.owner=actor.listReset.owner=actor.listen.owner=actor.talk.owner=actor.owner;
                    actor.pause.command.family=actor.listReset.command.family=actor.listen.command.family=actor.talk.command.family=context.family;
                    // CanPlay gates creature Set/Reset callers. Door talk reset
                    // is admitted by K1 only; neither game dispatches door rows.
                    if((context.family==DialogueAnimationFamily::Creature&&actor.canPlay)||
                       context.family==DialogueAnimationFamily::Placeable||
                       (context.family==DialogueAnimationFamily::Door&&game==model::GameVersion::Kotor1))
                        actor.pause=resolve(10000u);
                    if(context.family==DialogueAnimationFamily::Creature) {
                        actor.listReset=actor.pause;
                        if(actor.canPlay) {
                            bool listen=false,talk=false;
                            for(const auto& line:prepared.lines) {
                                const bool explicitRow=std::any_of(line.animations.begin(),line.animations.end(),
                                    [&](const auto& row){return row.owner==actor.owner;});
                                if(explicitRow)continue;
                                listen|=line.listenerBound&&line.listener==actor.owner;
                                talk|=line.speakerBound&&line.speaker==actor.owner;
                            }
                            if(listen)actor.listen=resolve(actor.injured?10155u:10030u);
                            if(talk)actor.talk=resolve(actor.injured?10154u:10038u);
                        }
                    } else if(context.family==DialogueAnimationFamily::Placeable) {
                        if(!request.engineState)throw std::runtime_error("The dialogue placeable has no captured engine animation state.");
                        const auto& state=*request.engineState;auto& command=actor.listReset.command;
                        command.serverAnimation=state.serverAnimation;command.name=state.animationName;
                        command.playEmpty=command.name.empty();command.playbackRate=state.playbackRate;command.runFlags=state.runFlags;
                    }
                    prepared.actors.push_back(std::move(actor));
                }
            }))return;
            page->startDialogue(std::move(prepared),entryIndices.at(static_cast<std::size_t>(entry)));
            if(!diagnostics.empty()) {
                std::string message;for(const auto& text:diagnostics)message+=text+"\n";
                wxMessageBox(wxui::toWx(message),"Dialogue resource report",wxOK|wxICON_INFORMATION,this);
            }
        } catch(const neoshared::bif::JobCancelled&) {
        } catch(const std::exception& e){wxui::showError(this,e);}
    }
    void showCreatureAppearance() {
        auto* page=activePage();if(!page)return;
        wxArrayString names;std::vector<const AreaPlacement*> creatures;
        for(const auto& placement:page->areaScene().placements()) {
            if(!placement.creatureState||!placement.runtimeInstance)continue;
            names.Add(wxui::toWx(placement.label+" ["+std::to_string(*placement.runtimeInstance)+"]"));
            creatures.push_back(&placement);
        }
        if(creatures.empty())return;
        const int selected=wxGetSingleChoiceIndex("Choose an loaded creature", "Creature Appearance",names,this);
        if(selected<0)return;
        const auto& placement=*creatures[static_cast<std::size_t>(selected)];
        const auto instance=*placement.runtimeInstance;
        const auto* object=page->areaScene().object(instance);
        if(!object)return;
        const auto resolver=object->document()->resourceResolver();
        auto state=*placement.creatureState;
        wxDialog dialog(this,wxID_ANY,"Creature Appearance",wxDefaultPosition,wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);
        auto* content=new wxScrolledWindow(&dialog,wxID_ANY,wxDefaultPosition,wxSize(FromDIP(570),FromDIP(450)));
        content->SetScrollRate(0,FromDIP(12));
        auto* body=new wxBoxSizer(wxVERTICAL);
        auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1,1);
        const auto add=[&](const char* label,wxWindow* control) {
            grid->Add(new wxStaticText(content,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            grid->Add(control,1,wxEXPAND);
        };
        const auto number=[&](const char* label,int value,int minimum,int maximum) {
            auto* control=new wxSpinCtrl(content,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
                                        wxSP_ARROW_KEYS,minimum,maximum,value);
            add(label,control);return control;
        };
        const auto toggle=[&](const char* label,bool value) {
            auto* control=new wxCheckBox(content,wxID_ANY,wxEmptyString);control->SetValue(value);
            add(label,control);return control;
        };
        auto* appearance=number("Appearance row",state.appearance,0,65535);
        auto* phenotype=number("Phenotype",state.phenotype,0,255);
        auto* gender=number("Gender",state.gender,0,255);
        auto* headVariation=number("Head variation",state.headVariation,0,255);
        auto* headOverride=number("Head override",state.headOverride,0,255);
        auto* backupHead=toggle("Use backup head",state.backupHead);
        auto* alignment=number("Alignment",state.alignment,std::numeric_limits<std::int16_t>::min(),
                               std::numeric_limits<std::int16_t>::max());
        auto* armor=toggle("Armor equipped",state.armorEquipped);
        auto* bodyVariation=number("Body variation",state.bodyVariation,0,255);
        auto* textureVariation=number("Texture variation",state.textureVariation,0,255);
        auto* disguise=toggle("Disguise appearance override",state.disguiseAppearance.has_value());
        auto* disguiseRow=number("Disguise appearance row",state.disguiseAppearance.value_or(0),0,65535);
        disguiseRow->Enable(disguise->GetValue());
        disguise->Bind(wxEVT_CHECKBOX,[=](wxCommandEvent&){disguiseRow->Enable(disguise->GetValue());});
        body->Add(grid,0,wxEXPAND|wxALL,10);
        if(state.disguised)body->Add(new wxStaticText(content,wxID_ANY,
            wxui::toWx("Loaded disguise; original appearance row: "+std::to_string(state.originalAppearance))),
            0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        body->Add(new wxStaticText(content,wxID_ANY,"Equipped items"),0,wxLEFT|wxRIGHT|wxTOP,10);
        auto* equipment=new wxChoice(content,wxID_ANY);
        body->Add(equipment,0,wxEXPAND|wxALL,10);
        auto* equipmentButtons=new wxBoxSizer(wxHORIZONTAL);
        auto* addItem=new wxButton(content,wxID_ANY,"Add / Replace UTI...");
        auto* removeItem=new wxButton(content,wxID_ANY,"Remove Selected");
        equipmentButtons->Add(addItem,0,wxRIGHT,6);equipmentButtons->Add(removeItem);
        body->Add(equipmentButtons,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        const auto refreshEquipment=[&] {
            equipment->Clear();
            for(const auto& item:state.equipment) {
                std::ostringstream label;label<<"0x"<<std::hex<<item.slot<<std::dec<<": "
                    <<(item.resref.empty()?"Inline item":item.resref)<<" (base item "<<item.baseItem<<")";
                equipment->Append(wxui::toWx(label.str()));
            }
            if(!state.equipment.empty())equipment->SetSelection(0);
            removeItem->Enable(!state.equipment.empty());
        };
        const auto equipmentChanged=[&] {
            setCreatureEquipment(state,state.equipment);
            armor->SetValue(state.armorEquipped);bodyVariation->SetValue(state.bodyVariation);
            textureVariation->SetValue(state.textureVariation);
            disguise->SetValue(state.disguiseAppearance.has_value());
            disguiseRow->SetValue(state.disguiseAppearance.value_or(0));
            disguiseRow->Enable(disguise->GetValue());refreshEquipment();
        };
        addItem->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
            wxArrayString slots;slots.Add("Head");slots.Add("Armor");slots.Add("Right hand");
            slots.Add("Left hand");slots.Add("Other slot mask...");
            const int chosen=wxGetSingleChoiceIndex("Equipment slot", "Add / Replace UTI",slots,&dialog);
            if(chosen<0)return;
            const std::uint32_t displayedSlots[]{0x1u,0x2u,0x10u,0x20u};
            std::uint32_t slot{};
            if(chosen<4)slot=displayedSlots[chosen];
            else {
                wxTextEntryDialog input(&dialog,"Slot mask (decimal or 0x hexadecimal)","Equipment slot");
                if(input.ShowModal()!=wxID_OK)return;
                std::istringstream parsed(wxui::toStd(input.GetValue()));
                std::int64_t value{};std::string extra;
                if(!(parsed>>std::setbase(0)>>value)||(parsed>>extra)||value<0||
                   value>std::numeric_limits<std::uint32_t>::max()) {
                    wxMessageBox("Enter a 32-bit unsigned slot mask.","Equipment slot",wxOK|wxICON_ERROR,&dialog);return;
                }
                slot=static_cast<std::uint32_t>(value);
            }
            wxTextEntryDialog input(&dialog,"UTI resource name in this creature's resource scope", "Add / Replace UTI");
            if(input.ShowModal()!=wxID_OK)return;
            try {
                auto item=readCreatureEquipment(wxui::toStd(input.GetValue()),slot,resolver,resourceLoadJob(),state.game);
                state.equipment.erase(std::remove_if(state.equipment.begin(),state.equipment.end(),
                    [=](const auto& existing){return existing.slot==slot;}),state.equipment.end());
                state.equipment.push_back(std::move(item));equipmentChanged();
            }catch(const std::exception& error) {
                wxMessageBox(wxui::toWx(error.what()),"Equipment not loaded",wxOK|wxICON_ERROR,&dialog);
            }
        });
        removeItem->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
            const int chosen=equipment->GetSelection();if(chosen==wxNOT_FOUND)return;
            state.equipment.erase(state.equipment.begin()+chosen);equipmentChanged();
        });
        refreshEquipment();content->SetSizer(body);
        root->Add(content,1,wxEXPAND|wxALL,5);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
        dialog.SetSizerAndFit(root);
        while(dialog.ShowModal()==wxID_OK) {
            state.appearance=static_cast<std::uint16_t>(appearance->GetValue());
            state.phenotype=static_cast<std::uint8_t>(phenotype->GetValue());
            state.gender=static_cast<std::uint8_t>(gender->GetValue());
            state.headVariation=static_cast<std::uint8_t>(headVariation->GetValue());
            state.headOverride=static_cast<std::uint8_t>(headOverride->GetValue());
            state.backupHead=backupHead->GetValue();state.alignment=static_cast<std::int16_t>(alignment->GetValue());
            state.armorEquipped=armor->GetValue();
            state.bodyVariation=static_cast<std::uint8_t>(bodyVariation->GetValue());
            state.textureVariation=static_cast<std::uint8_t>(textureVariation->GetValue());
            state.disguiseAppearance=disguise->GetValue()
                ?std::optional<std::uint16_t>{static_cast<std::uint16_t>(disguiseRow->GetValue())}:std::nullopt;
            std::string error;
            if(!page->applyCreatureAppearance(instance,state,error)) {
                wxMessageBox(wxui::toWx(error),"Appearance not applied",wxOK|wxICON_ERROR,&dialog);continue;
            }
            setModuleStatusText("Creature appearance applied.",0);updateState();return;
        }
    }
    void showCreatureLipSync() {
        auto* page=activePage();if(!page)return;
        wxArrayString names;
        std::vector<std::optional<std::size_t>> instances;
        if(page->document().hasModelOwner()) {
            names.Add("Main model");instances.push_back(std::nullopt);
        }
        for(const auto& placement:page->areaScene().placements()) {
            if(!placement.creatureState||!placement.runtimeInstance)continue;
            names.Add(wxui::toWx(placement.label+" ["+std::to_string(*placement.runtimeInstance)+"]"));
            instances.push_back(placement.runtimeInstance);
        }
        wxDialog dialog(this,wxID_ANY,"Creature Lip Sync");
        auto* root=new wxBoxSizer(wxVERTICAL);
        auto* target=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,names);target->SetSelection(0);
        root->Add(new wxStaticText(&dialog,wxID_ANY,"Creature or model"),0,wxLEFT|wxRIGHT|wxTOP,10);
        root->Add(target,0,wxEXPAND|wxALL,10);
        auto* delay=new wxTextCtrl(&dialog,wxID_ANY,"0");
        delay->Enable(page->document().model().game==model::GameVersion::Kotor2);
        root->Add(new wxStaticText(&dialog,wxID_ANY,"Lip delay (K2, seconds)"),0,wxLEFT|wxRIGHT,10);
        root->Add(delay,0,wxEXPAND|wxALL,10);
        auto* buttons=new wxBoxSizer(wxHORIZONTAL);
        auto* load=new wxButton(&dialog,wxID_ANY,"Load LIP...");
        auto* stop=new wxButton(&dialog,wxID_ANY,"Stop");
        buttons->Add(load,0,wxRIGHT,6);buttons->Add(stop,0,wxRIGHT,6);
        buttons->Add(new wxButton(&dialog,wxID_CANCEL,"Cancel"));
        root->Add(buttons,0,wxALL,10);dialog.SetSizerAndFit(root);
        load->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
            float seconds{};std::string extra;std::istringstream input(wxui::toStd(delay->GetValue()));
            if(!(input>>seconds)||(input>>extra)) {
                wxMessageBox("Enter a 32-bit floating-point delay.","Lip delay",wxOK|wxICON_ERROR,&dialog);return;
            }
            wxFileDialog file(&dialog,"Load creature lip sync",wxEmptyString,wxEmptyString,
                              "Lip sync (*.lip)|*.lip;*.LIP",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(file.ShowModal()!=wxID_OK)return;
            std::string error;
            if(!page->loadLipSync(instances[static_cast<std::size_t>(target->GetSelection())],
                                 neosettings::pathFromWx(file.GetPath()),seconds,error)) {
                wxMessageBox(wxui::toWx(error),"Lip sync not loaded",wxOK|wxICON_ERROR,&dialog);return;
            }
            dialog.EndModal(wxID_OK);
        });
        stop->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
            page->stopLipSync(instances[static_cast<std::size_t>(target->GetSelection())]);
            dialog.EndModal(wxID_OK);
        });
        dialog.ShowModal();
    }
    void chooseGrassArea() {
        auto* page=activePage();if(!page||page->areaScene().empty()||!engineAreaRuntimeSupported(page->areaScene().game())) {
            wxMessageBox("Load an area with a supported engine runtime first.","Area grass",wxOK|wxICON_INFORMATION,this);return;
        }
        wxFileDialog dialog(this,"Load grass and wind fields from ARE",wxEmptyString,wxEmptyString,
            "Areas (*.are)|*.are;*.ARE",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if(dialog.ShowModal()!=wxID_OK)return;
        try {page->loadGrassArea(neosettings::pathFromWx(dialog.GetPath()));
            const auto stats=page->areaScene().grass().statistics();
            setModuleStatusText(wxui::toWx("Grass: "+std::to_string(stats.faces)+" admitted faces, "+std::to_string(stats.blades)+" potential blades; geometry is allocated by camera range."),0);
        }catch(const std::exception& error){wxMessageBox(wxui::toWx(error.what()),"Area grass not loaded",wxOK|wxICON_ERROR,this);}
    }
    void chooseGrassOptions() {
        auto* page=activePage();if(!page||page->areaScene().empty()||!engineAreaRuntimeSupported(page->areaScene().game()))return;
        auto settings=page->areaScene().grass().settings();
        wxDialog dialog(this,wxID_ANY,"Engine triangle-bin grass",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&dialog,wxID_ANY,"Load an ARE and surfacemat.2da before enabling grass.\nThese controls are per-document preview inputs, not gameplay state."),0,wxALL,10);
        const auto check=[&](const char* label,bool value){auto* c=new wxCheckBox(&dialog,wxID_ANY,label);c->SetValue(value);box->Add(c,0,wxLEFT|wxRIGHT|wxBOTTOM,10);return c;};
        auto* enabled=check("Render engine grass bins",settings.enabled);
        auto* wind=check("Update near-bin wind",settings.wind);
        auto* lightmaps=check("Admit face lightmap tails",settings.lightmaps);
        auto* textured=check("Engine textured-grass branch",settings.textured);
        box->Add(new wxStaticText(&dialog,wxID_ANY,"Force Sight follows the scene setting."),0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        box->Add(dialog.CreateSeparatedButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);dialog.SetSizerAndFit(box);
        if(dialog.ShowModal()!=wxID_OK)return;
        settings.enabled=enabled->GetValue();settings.wind=wind->GetValue();settings.lightmaps=lightmaps->GetValue();
        settings.textured=textured->GetValue();settings.areaWind.reset();
        try {page->configureGrass(settings);}catch(const std::exception& error){wxMessageBox(wxui::toWx(error.what()),"Grass options",wxOK|wxICON_ERROR,this);}
    }

    void chooseAndOpenModel(const std::filesystem::path& initialDirectory = {}) {
        wxFileDialog dialog(
            this, "Open Odyssey model", neosettings::pathToWx(initialDirectory),
            wxEmptyString,
            "Odyssey models (*.mdl;*.mdx)|*.mdl;*.MDL;*.mdx;*.MDX|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK)
            openFile(neosettings::pathFromWx(dialog.GetPath()));
    }

    void exportAsciiModel() {
        auto* page=activePage();
        if(!page || !page->document().hasModelOwner()) return;
        const auto& document=page->document();
        const auto protectedInputs=document.openPaths();
        auto stem=document.path().stem();
        if(stem.empty()) stem=std::filesystem::u8path(document.model().name);
        const auto filename=neoshared::pathToUtf8(stem)+"-ascii.mdl";
        wxFileDialog file(this,"Export ASCII MDL",neosettings::pathToWx(document.path().parent_path()),
            wxui::toWx(filename),"ASCII models (*.mdl)|*.mdl|All files (*.*)|*.*",
            wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(file.ShowModal()!=wxID_OK) return;
        try {
            const auto output=neosettings::pathFromWx(file.GetPath());
            neoshared::checkResourceOutput(output,protectedInputs);
            model::AsciiExportResult exported;
            std::vector<std::uint8_t> bytes;
            if(!runJob("Exporting ASCII model",[&](const neoshared::bif::JobControl& job) {
                // Documents stay open while runJob owns the modal operation.
                // Copy the decoded asset on the worker, not the live pose or
                // any area/creature assembly maintained by the canvas.
                job.check();
                const auto snapshot=document.sourceModel();
                exported=model::exportAsciiModel(snapshot,job);
                bytes.assign(exported.text.begin(),exported.text.end());
                job.check();
            })) return;
            neoshared::texture::writeFileBytes(output,bytes);
            setModuleStatusText("ASCII model exported to "+wxui::toWx(neoshared::pathToUtf8(output)),0);
            if(!exported.warnings.empty()) {
                std::ostringstream message;
                message<<"The ASCII model was exported with the following notes:\n";
                for(const auto& warning:exported.warnings) message<<"\n"<<warning;
                wxMessageBox(wxui::toWx(message.str()),"ASCII export notes",wxOK|wxICON_INFORMATION,this);
            }
        } catch(const std::exception& error) {
            wxMessageBox(wxui::toWx(error.what()),"ASCII model not exported",wxOK|wxICON_ERROR,this);
        }
    }

    void exportPackedModel() {
        auto* page=activePage();
        if(!page || !page->document().hasModelOwner()) return;
        const auto& document=page->document();
        auto protectedInputs=document.openPaths();
        const auto filename=neoshared::pathToUtf8(document.path().stem())+"-packed.mdl";
        wxFileDialog file(this,"Export packed MDL/MDX",neosettings::pathToWx(document.path().parent_path()),
            wxui::toWx(filename),"Packed models (*.mdl)|*.mdl",
            wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(file.ShowModal()!=wxID_OK) return;
        try {
            const auto output=neosettings::pathFromWx(file.GetPath());
            auto companion=output;companion.replace_extension(".mdx");
            neoshared::checkResourceOutput(output,protectedInputs);
            protectedInputs.push_back(output);
            neoshared::checkResourceOutput(companion,protectedInputs);
            if(std::filesystem::exists(companion) && wxMessageBox(
                    "Replace the companion file "+wxui::toWx(neoshared::pathToUtf8(companion))+"?",
                    "Export packed MDL/MDX",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES) return;
            model::PackedExportResult exported;
            if(!runJob("Compiling packed model",[&](const neoshared::bif::JobControl& job) {
                job.check();
                const auto snapshot=document.sourceModel();
                exported=model::exportPackedModel(snapshot,job);
                job.check();
            })) return;
            neoshared::texture::writeFileBytes(companion,exported.mdx);
            neoshared::texture::writeFileBytes(output,exported.mdl);
            setModuleStatusText("Packed MDL/MDX exported to "+wxui::toWx(neoshared::pathToUtf8(output)),0);
        } catch(const std::exception& error) {
            wxMessageBox(wxui::toWx(error.what()),"Packed model not exported",wxOK|wxICON_ERROR,this);
        }
    }

    void openSavedGameDirectory(const neogames::SavedGameDirectory& directory) {
        if (directory.customDirectory) {
            chooseAndOpenModel(directory.path);
            return;
        }
        neogames::GamePathSettings settings;
        if (!settings.setActiveInstall(directory.gameId, directory.installId)) {
            wxMessageBox(
                "The selected saved game directory could not be made active.",
                "Game Directory Unavailable", wxOK | wxICON_WARNING, this);
            return;
        }
        std::filesystem::path initial = directory.path;
        if (const auto* definition = neogames::findGame(directory.gameId)) {
            const auto override = neoshared::game::firstExisting(
                directory.path, definition->overrideRelativePaths);
            if (neoshared::game::isDirectoryPath(override)) initial = override;
        }
        chooseAndOpenModel(initial);
    }

    ModelPage* pageAt(std::size_t index) const {
        return static_cast<ModelPage*>(tabs_->GetPage(index));
    }

    ModelPage* activePage() const {
        const auto selected = tabs_->GetSelection();
        return selected == wxNOT_FOUND ? nullptr : pageAt(static_cast<std::size_t>(selected));
    }

    void addDocument(std::unique_ptr<ModelDocument> document,std::shared_ptr<GameResourceSession> session={}) {
        const auto title = wxui::toWx(document->title());
        auto* page = new ModelPage(tabs_, std::move(document), options_, context_.compact);
        page->setResourceSession(std::move(session));
        page->setAppearance(dark_);
        if (!tabs_->AddPage(page, title, true)) {
            page->Destroy();
            throw std::runtime_error("Unable to create a NeoMDL document tab.");
        }
        updateState();
    }

    void closePage(int index) {
        if (jobRunning_) { cancelJob_.store(true); return; }
        if (index < 0 || static_cast<std::size_t>(index) >= tabs_->GetPageCount()) return;
        tabs_->DeletePage(static_cast<std::size_t>(index));
        updateState();
    }

    void closeAll() {
        if (jobRunning_) { cancelJob_.store(true); return; }
        while (tabs_->GetPageCount() != 0u) tabs_->DeletePage(tabs_->GetPageCount() - 1u);
        updateState();
    }

    void applyOptions() {
        if(options_.fog.mode!=neomdl::model::FogMode::Disabled) lastFogMode_=options_.fog.mode;
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index) {
            pageAt(index)->setOptions(options_);
            pageAt(index)->refreshDetails();
        }
        updateMenuChecks();
    }

    void buildMenus() {
        auto* bar = new wxMenuBar;
        fileMenu_ = new wxMenu;
        auto* file = fileMenu_;
        file->Append(ID_Open, "&Open Model...\tCtrl+O");
        file->Append(ID_ExportAscii, "Export &ASCII MDL...");
        file->Append(ID_ExportPacked, "Export &packed MDL/MDX...");
        file->Append(ID_LoadArea, "Load Area Layout...");
        file->Append(ID_ModuleArea, "Load Module Area...");
        file->Append(ID_ResourceProvenance, "Resource Scope and Load Report...");
        file->Append(ID_ClearArea, "Clear Area Context");
        file->Append(ID_AreaRoom, "Select Current Area Room...");
        file->Append(ID_AreaEnvironment, "Area Environment / Camera Replay...");
        file->Append(ID_WorldPlayback, "World Playback...");
        file->Append(ID_AreaWeather, "Engine Area Weather...");
        file->Append(ID_SceneAudio, "Scene Audio...");
        file->Append(ID_LoadGrass, "Load Area Grass from ARE...");
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file,
            [this](const neogames::SavedGameDirectory& directory) {
                openSavedGameDirectory(directory);
            },
            neogames::GameDirectoryGameIds{"kotor", "kotor2"},
            "Open Model from Saved &Directory");
        file->AppendSeparator();
        file->Append(ID_CloseModel, "&Close Model\tCtrl+W");
        file->Append(ID_CloseAll, "Close &All");
        file->AppendSeparator();
        file->Append(ID_Exit, "E&xit");
        bar->Append(file, "&File");

        viewMenu_ = new wxMenu;
        auto* view = viewMenu_;
        view->Append(ID_FrameModel, "&Frame Model\tF");
        view->AppendSeparator();
        view->AppendCheckItem(ID_Textures, "Show &Textures");
        view->AppendCheckItem(ID_Lightmaps, "Show &Lightmaps");
        view->AppendCheckItem(ID_Wireframe, "&Wireframe");
        view->AppendCheckItem(ID_Lighting, "&Lighting");
        view->AppendCheckItem(ID_Headlight, "Preview Key Light");
        view->AppendCheckItem(ID_PreviewLights, "Authored Model Lights");
        view->AppendCheckItem(ID_NearestLightsOnlyGobs, "K2 Nearest-Light Gob-Only Mode",
            "Engine roomless alternate mode: registration-order 3D light queries; ignored while area context is loaded");
        view->AppendCheckItem(ID_EngineShadows, "Engine CPU Shadows (Scene Owners)",
            "K2 CPU rigid/dangly shadow stage; requires an area current room; optional Desktop soft composition; skin/direct branches draw nothing");
        view->AppendCheckItem(ID_EngineTwoSidedShadows, "Desktop Two-Sided Shadow Stencil",
            "Use the recovered Desktop depth-pass/wrapping branch, not the default depth-fail two-draw branch");
        view->AppendCheckItem(ID_EngineSoftShadows, "Desktop Engine Soft Shadows (ARB)",
                              "Use the recovered 512-pixel ARB image-space shadow path; requires framebuffer objects and rectangle textures.");
        view->AppendCheckItem(ID_EngineSoftShadowsATI, "Desktop Engine Soft Shadows (ATI Compatibility)",
                              "Replay the supplied non-ARB path, including its incomplete pbuffer textures; this is not the ARB filter.");
        view->AppendCheckItem(ID_EngineLensFlares, "Engine Lens Flares",
            "Use collected room lights and engine scene hit queries");
        view->AppendCheckItem(ID_ForceSight, "KOTOR II Force Sight",
            "Engine Force Sight materials, owner traversal, grass and flares");
        view->AppendCheckItem(ID_InspectMeshProjections, "Inspect Engine Mesh Projection Volumes",
            "Inspector wireframe: explicit visible-room sources through DoDynamicMeshProjections; not a engine scene-stage insertion");
        view->AppendCheckItem(ID_Fog, "Scene Fog");
        view->Append(ID_LightingSettings, "Lighting and Fog Settings...");
        auto* camera = new wxMenu;
        camera->Append(ID_ViewSelectedCamera, "View Through Selected Camera");
        camera->Append(ID_ViewCameraHook, "Attach to Engine \"camerahook\"");
        camera->Append(ID_ViewAreaCamera, "View Through GIT Placeable Camera...");
        camera->Append(ID_OrbitCamera, "Return to Orbit Camera");
        camera->AppendSeparator();
        camera->Append(ID_CameraSettings, "Projection Settings...");
        view->AppendSubMenu(camera, "Authored &Camera");
        view->AppendCheckItem(ID_Grid, "Show &Grid");
        view->AppendCheckItem(ID_Axes, "Show &Axes");
        view->AppendCheckItem(ID_ShowHidden, "Show Meshes with Render Flag Off");
        view->AppendCheckItem(ID_CullBackfaces, "Cull &Backfaces");
        view->AppendSeparator();
        view->AppendCheckItem(ID_DanglySimulation, "Simulate &Dangly Meshes");
        view->AppendCheckItem(ID_NodeHelpers, "Show Node &Helpers");
        auto* previews = new wxMenu;
        previews->AppendCheckItem(ID_PreviewEmitters, "Textured particles");
        previews->Append(ID_DetonateEmitters, "Detonate explosion emitters");
        previews->Append(ID_ParticleSettings, "Particle Scene Settings...");
        previews->AppendCheckItem(ID_ShowCameras, "Cameras");
        previews->AppendCheckItem(ID_ShowReferences, "Referenced Models");
        previews->AppendCheckItem(ID_PreviewLightsabers, "Lightsabers");
        previews->Append(ID_LightsaberSettings, "Lightsaber Render Settings...");
        previews->Append(ID_MotionBlurOwners, "Engine Motion-blur Owners...");
        previews->Append(ID_K2SkinSettings, "Manual Skin Blur Diagnostic...");
        previews->Append(ID_DirtyMaterial, "Legacy Desktop Dirty Caller (Diagnostic)...");
        previews->Append(ID_DirtyArea, "Dirty Area Inputs...");
        previews->Append(ID_EngineExecution, "Procedural Upload Settings...");
        previews->Append(ID_EngineExecutionReport, "Save Engine Execution Report...");
        previews->Append(ID_OwnerVisualEffects, "Owner Visual Effects...");
        previews->Append(ID_SceneEffects, "Engine Scene Effects...");
        previews->Append(ID_VideoEffectRow, "Engine Video Effect Row...");
        previews->Append(ID_AreaAnimation, "Area Object Animation...");
        previews->Append(ID_RoomScriptAnimation, "Room Script Animation...");
        previews->Append(ID_TrackAnimation, "Track Follower Animation...");
        previews->Append(ID_DoorState, "Door OpenState...");
        previews->Append(ID_CreatureAppearance, "Creature Appearance...");
        previews->Append(ID_CreatureLipSync, "Creature Lip Sync...");
        previews->Append(ID_DialoguePlayback, "Dialogue Playback...");
        previews->Append(ID_SceneShadows, "Scene Shadow Inputs...");
        previews->Append(ID_ShadowCasters, "Scene Shadow Casters...");
        previews->Append(ID_GrassOptions, "Engine Triangle-bin Grass...");
        previews->Append(ID_RuntimeTextureRotation, "Runtime Texture Rotation...");
        previews->AppendCheckItem(ID_PreviewAabbs, "Engine AABB Collision Geometry");
        previews->AppendCheckItem(ID_PreviewTriggers, "Engine Runtime Trigger Volumes");
        previews->Append(ID_TriggerVisualState, "GIT Trigger Visual State...");
        previews->Append(ID_TriggerMineRuntime, "Trap Runtime...");
        view->AppendSubMenu(previews, "Node &Previews");
        bar->Append(view, "&View");

        helpMenu_ = new wxMenu;
        auto* help = helpMenu_;
        help->Append(ID_About, "&About NeoMDL");
        bar->Append(help, "&Help");
        setModuleMenus(bar);
        updateMenuChecks();
    }

    void updateMenuChecks() {
        if (!viewMenu_) return;
        const auto setCheck = [this](int id, bool checked) {
            if (auto* item = viewMenu_->FindItem(id)) item->Check(checked);
        };
        setCheck(ID_Textures, options_.textures);
        setCheck(ID_Lightmaps, options_.lightmaps);
        setCheck(ID_Wireframe, options_.wireframe);
        setCheck(ID_Lighting, options_.lighting);
        setCheck(ID_Headlight, options_.headlight);
        setCheck(ID_Fog, options_.fog.mode!=neomdl::model::FogMode::Disabled);
        setCheck(ID_Grid, options_.grid);
        setCheck(ID_Axes, options_.axes);
        setCheck(ID_ShowHidden, options_.showHidden);
        setCheck(ID_CullBackfaces, options_.cullBackfaces);
        setCheck(ID_DanglySimulation, options_.danglySimulation);
        setCheck(ID_NodeHelpers, options_.nodeHelpers);
        setCheck(ID_PreviewLights, options_.previewLights);
        setCheck(ID_NearestLightsOnlyGobs, options_.nearestLightsOnlyGobs);
        setCheck(ID_EngineShadows, options_.engineShadows);
        setCheck(ID_EngineTwoSidedShadows, options_.engineTwoSidedShadows);
        setCheck(ID_EngineSoftShadows, options_.engineSoftShadows &&
            options_.engineSoftShadowMode==render::EngineSoftShadowMode::DesktopARB);
        setCheck(ID_EngineSoftShadowsATI, options_.engineSoftShadows &&
            options_.engineSoftShadowMode==render::EngineSoftShadowMode::DesktopATI);
        setCheck(ID_EngineLensFlares, options_.engineLensFlares);
        setCheck(ID_ForceSight, activePage()&&activePage()->sceneEffects().forceSight);
        setCheck(ID_InspectMeshProjections, options_.inspectMeshProjections);
        setCheck(ID_PreviewEmitters, options_.previewEmitters);
        setCheck(ID_ShowCameras, options_.showCameras);
        setCheck(ID_ShowReferences, options_.showReferences);
        setCheck(ID_PreviewLightsabers, options_.previewLightsabers);
        setCheck(ID_PreviewAabbs, options_.previewAabbs);
        setCheck(ID_PreviewTriggers, options_.previewTriggers);
    }

    void showAreaCameraChooser() {
        auto* page=activePage();if(!page)return;
        const auto& cameras=page->cameraScene().areaCameras();
        if(cameras.empty()) {
            wxMessageBox("The active area contains no GIT CameraList records.",
                         "GIT Placeable Camera",wxOK|wxICON_INFORMATION,this);return;
        }
        wxArrayString choices;
        for(std::size_t i=0;i<cameras.size();++i) {
            const auto& c=cameras[i].authored;
            std::ostringstream label;label<<"["<<i<<"] ID "<<c.cameraId<<" | "
                <<c.position.x<<", "<<c.position.y<<", "<<c.position.z
                <<" | FOV "<<c.fieldOfView;
            choices.Add(wxui::toWx(label.str()));
        }
        const int selected=wxGetSingleChoiceIndex(
            "Authored position, orientation, pitch, height and FOV drive this camera. Microphone range is retained as metadata.",
            "View Through GIT Placeable Camera",choices,this);
        if(selected>=0&&!page->viewThroughAreaCamera(static_cast<std::size_t>(selected)))
            wxMessageBox("The selected camera is no longer available.","GIT Placeable Camera",wxOK|wxICON_ERROR,this);
        updateState();
    }

    void showTriggerVisualState() {
        auto* page=activePage();if(!page)return;
        const auto& records=page->triggerScene().records();
        std::vector<std::size_t> indices;wxArrayString choices;
        for(std::size_t i=0;i<records.size();++i) if(records[i].source==RuntimeTriggerSource::AreaGit) {
            indices.push_back(i);std::ostringstream label;label<<"["<<i<<"] "<<records[i].label
                <<" | object 0x"<<std::hex<<records[i].objectId<<std::dec
                <<" | type "<<records[i].type;choices.Add(wxui::toWx(label.str()));
        }
        if(indices.empty()) {
            wxMessageBox("The active area contains no loaded GIT trigger footprints.",
                         "GIT Trigger Visual State",wxOK|wxICON_INFORMATION,this);return;
        }
        const int selected=wxGetSingleChoiceIndex(
            "Select an loaded trigger. These controls override the engine client state initialized from the loaded trigger.",
            "GIT Trigger Visual State",choices,this);
        if(selected<0)return;
        const auto index=indices[static_cast<std::size_t>(selected)];
        const auto current=records[index].clientVisualState;
        wxDialog dialog(this,wxID_ANY,"GIT Trigger Visual State");
        auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "UpdateTriggerColor selects cyan or blue and alpha 0 or 0.25, then builds the authored-height highlight volume. The suppression byte leaves prior visual state unchanged."),
            0,wxEXPAND|wxALL,8);
        auto* suppressed=new wxCheckBox(&dialog,wxID_ANY,"Suppress highlight update");
        auto* transition=new wxCheckBox(&dialog,wxID_ANY,"Area-transition color selector (blue instead of cyan)");
        auto* display=new wxCheckBox(&dialog,wxID_ANY,"Display highlight (alpha 0.25 instead of 0)");
        suppressed->SetValue(current.updateSuppressed);transition->SetValue(current.areaTransitionMode);display->SetValue(current.displayEnabled);
        root->Add(suppressed,0,wxLEFT|wxRIGHT|wxTOP,8);root->Add(transition,0,wxLEFT|wxRIGHT|wxTOP,8);
        root->Add(display,0,wxLEFT|wxRIGHT|wxTOP|wxBOTTOM,8);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALIGN_RIGHT|wxALL,8);
        dialog.SetSizerAndFit(root);if(dialog.ShowModal()!=wxID_OK)return;
        TriggerClientVisualState state;state.updateSuppressed=suppressed->GetValue();
        state.areaTransitionMode=transition->GetValue();state.displayEnabled=display->GetValue();
        if(!page->setTriggerClientVisualState(index,state)) {
            wxMessageBox("The selected trigger is no longer available.","GIT Trigger Visual State",wxOK|wxICON_ERROR,this);return;
        }
        if(state.displayEnabled&&!options_.previewTriggers) {options_.previewTriggers=true;applyOptions();}
        updateState();
    }

    std::optional<std::string> editTrapCapture(const wxString& title,const wxString& description,
                                              const std::string& initial) {
        wxDialog dialog(this,wxID_ANY,title,wxDefaultPosition,wxSize(850,650),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);
        auto* help=new wxStaticText(&dialog,wxID_ANY,description);help->Wrap(810);root->Add(help,0,wxALL,10);
        auto* text=new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(initial),wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_DONTWRAP);
        root->Add(text,1,wxEXPAND|wxLEFT|wxRIGHT,10);
        auto* load=new wxButton(&dialog,wxID_ANY,"Load capture JSON...");root->Add(load,0,wxALL,10);
        load->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
            wxFileDialog file(&dialog,"Load trap capture",{}, {},"JSON files (*.json)|*.json|All files|*",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(file.ShowModal()!=wxID_OK)return;
            std::string bytes;
            if(runJob("Reading trap capture",[&](const neoshared::bif::JobControl& job) {
                job.check();std::ifstream input(neosettings::pathFromWx(file.GetPath()),std::ios::binary);
                if(!input)throw std::runtime_error("Cannot open the selected capture");
                bytes.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());job.check();
                if(input.bad())throw std::runtime_error("Cannot read the selected capture");
            }))text->SetValue(wxui::toWx(bytes));
        });
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALIGN_RIGHT|wxALL,10);dialog.SetSizer(root);
        if(dialog.ShowModal()!=wxID_OK)return {};
        return wxui::toStd(text->GetValue());
    }
    void showTrapDetectionCapture() {
        auto* page=activePage();if(!page)return;
        wxArrayString modes;modes.Add("Execute one captured check");modes.Add("Retain actor and run engine cadence");modes.Add("Stop retained detection");
        const int mode=wxGetSingleChoiceIndex("Choose how to replay captured trap detection.","Trap detection",modes,this);
        if(mode<0)return;
        if(mode==2){page->clearTriggerDetectionCaptures();return;}
        const auto text=editTrapCapture("Replay trap detection",
            wxui::toWx(std::string("Supply captured actor fields, complete engine area-object order (including non-traps), reputation pairs and libc rand outputs. ")+
            "areaObjects entries are {exists, trap: null or {kind, sourceIndex}}; reputations are {from, toward, value}. "+
            (mode==0?"This executes one engine detection check for mines, doors and placeables.":
                "Retained playback advances the captured actor timer on the world clock at the engine cadence. Actor state stays fixed until recaptured; results go to the animation/event log. Playback stops when captured random or reputation input is unavailable.")),
            R"({
  "objectId": null, "faction": null, "elapsedSinceCheckMilliseconds": null,
  "position": [null, null, null], "serverAnimation": null, "detectionMode": null,
  "difficulty": null, "awarenessRank": null, "fivePointPenalty": null,
  "partyMember": null, "areaPresent": null, "partyObjectIds": [],
  "areaObjects": [], "reputations": [], "nativeRand": []
})");
        if(!text)return;
        TriggerDetectionInput input;
        if(!runJob("Reading detection capture",[&](const neoshared::bif::JobControl& job) {
            job.check();input=trapcapture::detection(*text);job.check();
        }))return;
        if(mode==1){page->captureTriggerDetection(std::move(input));return;}
        const auto result=page->checkTriggerDetection(input);
        wxMessageBox(wxui::toWx(triggerDetectionReport(result)),"Trap detection",wxOK|wxICON_INFORMATION,this);
    }
    void showObjectTrapState() {
        auto* page=activePage();if(!page)return;
        const auto& traps=page->areaScene().triggerRuntime().objectTraps();wxArrayString names;
        for(const auto& trap:traps)names.Add(wxui::toWx(std::string(trap.target.kind==TrapObjectKind::Door?"DoorList[":"Placeable List[")+std::to_string(trap.target.sourceIndex)+"]"));
        if(names.empty()){wxMessageBox("The area contains no door or placeable trap records.","Trap Runtime",wxOK,this);return;}
        const int choice=wxGetSingleChoiceIndex("Choose an loaded object trap.","Object trap state",names,this);
        if(choice<0)return;const auto& trap=traps[static_cast<std::size_t>(choice)];
        const auto text=editTrapCapture("Captured object trap state","Capture the current server identity, trap flag and lock inputs. PCLevelAtSpawn may have been assigned from G_PC_LEVEL by the engine heartbeat; no player level is inferred.",
            neojson::stringify(neojson::Value::object({{"objectId",trapcapture::number(trap.objectId)},
                {"flagged",trapcapture::number(trap.flagged)},
                {"locked",trapcapture::number(trap.locked)},{"openLockDc",trapcapture::number(trap.openLockDc)},
                {"openLockDifficulty",trapcapture::number(trap.openLockDifficulty)},
                {"openLockDifficultyModifier",trapcapture::number(trap.openLockDifficultyModifier)},
                {"pcLevelAtSpawn",trapcapture::number(trap.pcLevelAtSpawn)}})));
        if(!text)return;const auto capture=neojson::parse(*text);
        const auto id=trapcapture::integer<std::uint64_t>(capture.at("objectId"));
        const auto flag=trapcapture::integer<std::uint8_t>(capture.at("flagged"));
        AreaObjectTrapRecord lock=trap;
        lock.locked=trapcapture::integer<std::uint8_t>(capture.at("locked"));
        lock.openLockDc=trapcapture::integer<std::uint8_t>(capture.at("openLockDc"));
        lock.openLockDifficulty=trapcapture::integer<std::uint8_t>(capture.at("openLockDifficulty"));
        lock.openLockDifficultyModifier=trapcapture::integer<std::int8_t>(capture.at("openLockDifficultyModifier"));
        lock.pcLevelAtSpawn=trapcapture::integer<std::uint8_t>(capture.at("pcLevelAtSpawn"));
        page->setObjectTrapId(trap.target,id);page->setObjectTrapFlag(trap.target,flag);
        page->captureObjectLock(trap.target,lock);updateState();
    }
    void showTrapActionCapture() {
        auto* page=activePage();if(!page)return;
        std::vector<TrapObjectTarget> targets;wxArrayString names;
        for(const auto& mine:page->areaScene().triggerRuntime().mines())if(!mine.serverRemoved) {
            targets.push_back({TrapObjectKind::Mine,mine.sourceIndex});names.Add(wxui::toWx("Mine["+std::to_string(mine.sourceIndex)+"]"));
        }
        for(const auto& trap:page->areaScene().triggerRuntime().objectTraps()) {
            targets.push_back(trap.target);names.Add(wxui::toWx(std::string(trap.target.kind==TrapObjectKind::Door?"Door[":"Placeable[")+std::to_string(trap.target.sourceIndex)+"]"));
        }
        const int selected=wxGetSingleChoiceIndex("Choose the engine action target.","Trap action completion",names,this);
        if(selected<0)return;
        wxArrayString actions;actions.Add("Flag");actions.Add("Disarm");actions.Add("Recover");
        const int operation=wxGetSingleChoiceIndex("Replay the captured in-range completion pass.","Trap action completion",actions,this);
        if(operation<0)return;
        const auto text=editTrapCapture("Trap action completion capture",
            "Supply the actor's captured action-completion inputs. This does not simulate movement or the action queue. "
            "K1 combat uses libc rand; K2 combat uses rand_wincompatible. Recovered item allocation/admission and scripts pause for captured external results.",
            R"({
  "actor": null, "demolitionsRank": null, "inCombat": null,
  "actorPartyMember": null, "actorPlayerFlag": null, "creatorIsCreature": null,
  "creatorPartyMember": null, "creatorPlayerFlag": null, "difficulty": null,
  "nativeRand": [],
  "firing": {
    "targetIsCreature": null, "creatorIsCreature": null, "creatorHasFaction": null,
    "force": null, "mineImmune": null, "targetFaction": null,
    "reputationToCreator": null, "reputationToTrigger": null,
    "feat238": null, "partyMember": null, "partyLeaderHasServerCreature": null,
    "partyLeaderFeat239": null
  }
})");
        if(!text)return;
        TrapActionInput input;
        if(!runJob("Reading trap action capture",[&](const neoshared::bif::JobControl& job) {
            job.check();input=trapcapture::action(*text);job.check();
        }))return;
        const auto result=page->completeTrapAction(targets.at(static_cast<std::size_t>(selected)),
            static_cast<TrapAction>(operation),input);
        const std::string report=!result.accepted?"Engine action was not accepted.":
            std::string(result.success?"Engine action succeeded.":"Engine action failed.")+
            "\nRoll: "+std::to_string(result.roll)+"; skill: "+std::to_string(result.skill)+"; DC: "+std::to_string(result.difficultyClass)+
            (result.creatorBypass?"\nEngine creator/party bypass applied.":"")+
            (result.completed?"\nTransition completed.":"\nPaused for a engine external request. See the event log.");
        wxMessageBox(wxui::toWx(report),"Trap action result",wxOK|wxICON_INFORMATION,this);updateState();
    }
    void showSetMineCapture() {
        auto* page=activePage();if(!page)return;
        const auto text=editTrapCapture("Set mine completion capture",
            "Supply the engine in-range SetMine inputs and loaded inventory item. target is null for ground, or {kind, sourceIndex} for an existing trap. "
            "newObjectId is the captured server allocation identity. For a linked door, linkedDoorObjectId is the actual linked-door result; "
            "2130706432 means no linked door, null means uncaptured. Model, animation and explosion audio load on a worker before admission and item consumption.",
            R"({
  "actor": null, "item": null, "newObjectId": null, "target": null, "linkedDoorObjectId": null,
  "actorPosition": [null, null, null], "actorFaction": null,
  "demolitionsRank": null, "baseDemolitionsRank": null, "inCombat": null,
  "areaPresent": null, "areaCanSetMines": null, "difficulty": null, "nativeRand": [],
  "firing": {
    "targetIsCreature": null, "creatorIsCreature": null, "creatorHasFaction": null,
    "force": null, "mineImmune": null, "targetFaction": null,
    "reputationToCreator": null, "reputationToTrigger": null,
    "feat238": null, "partyMember": null, "partyLeaderHasServerCreature": null,
    "partyLeaderFeat239": null
  }
})");
        if(!text)return;
        const auto inventory=page->areaScene().triggerRuntime().inventory();
        const auto objectTraps=page->areaScene().triggerRuntime().objectTraps();
        const auto resources=page->areaScene().resourceResolver();const auto game=page->areaScene().game();
        const auto asciiResources=page->areaScene().asciiResourceResolver();
        TrapSetMinePlan plan;std::optional<PreparedTriggerMine> prepared;PreparedSceneAudioPlayback sounds;
        PreparedTrapDoorAnimations doors;
        if(!runJob("Preparing placed mine",[&](const neoshared::bif::JobControl& job) {
            ResourceLoadScope scope(job);job.check();
            const auto input=trapcapture::setMine(*text);
            plan=TriggerRuntime::prepareSetMine(input,inventory,resources,game,objectTraps);job.check();
            if(plan.objectVisual&&input.target&&input.target->kind==TrapObjectKind::Door) {
                const auto& target=*std::find_if(objectTraps.begin(),objectTraps.end(),[&](const auto& value) {
                    return value.target.kind==TrapObjectKind::Door&&value.target.sourceIndex==input.target->sourceIndex;
                });
                const auto prepareDoor=[&](const ObjectTrapState& door) {
                    for(const auto& placement:page->areaScene().placements())
                        if(placement.kind==AreaObjectKind::Door&&placement.sourceIndex==door.target.sourceIndex&&placement.runtimeInstance) {
                            const auto* owner=page->areaScene().object(*placement.runtimeInstance);
                            PreparedTrapDoorAnimation preparedDoor;preparedDoor.sourceIndex=door.target.sourceIndex;
                            for(int selector=0;selector<(door.doorVisibleModel?1:2);++selector) {
                                auto command=doorOpenStateAnimation(door.doorVisibleModel?2u:3u,selector);
                                std::string diagnostic;
                                if(!resolveObjectAnimation(command,resources,*owner->document(),diagnostic))
                                    throw std::runtime_error(diagnostic);
                                preparedDoor.animations.push_back(std::move(command));
                            }
                            doors.push_back(std::move(preparedDoor));
                        }
                };
                prepareDoor(target);
                if((target.linkedToFlags==1u||target.linkedToFlags==2u)&&input.linkedDoorObjectId&&
                   *input.linkedDoorObjectId!=0x7f000000u) {
                    const auto linked=std::find_if(objectTraps.begin(),objectTraps.end(),[&](const auto& value) {
                        return value.target.kind==TrapObjectKind::Door&&value.objectId==*input.linkedDoorObjectId;
                    });
                    if(linked==objectTraps.end())throw std::runtime_error("Captured linked door is outside the loaded area");
                    prepareDoor(*linked);
                }
            }
            if(plan.record) {
                // Room WOKs and surface masks are immutable during the loading job.
                plan.record->worldFootprint=page->areaScene().newMineFootprint(plan.record->position);
                prepared=AreaScene::prepareTriggerMine(*plan.record,plan.constructorState,resources,game,job,asciiResources);
                std::vector<std::string> names;
                if(!prepared->runtime.explosionSound.empty())names.push_back(prepared->runtime.explosionSound);
                sounds=prepareSceneAudioPlayback(names,resources,game,job);
            }
            job.check();
        }))return;
        const auto result=page->completeSetMine(plan,std::move(prepared),std::move(sounds),std::move(doors));
        std::string message=!result.accepted?"Engine SetMine was not accepted.":
            std::string(result.success?"Engine SetMine succeeded.":result.catastrophic?"Engine SetMine failed catastrophically.":"Engine SetMine failed.")+
            "\nRoll: "+std::to_string(result.roll)+"; skill: "+std::to_string(result.skill)+"; DC: "+std::to_string(result.difficultyClass);
        if(result.createdSourceIndex)message+="\nCreated mine source index: "+std::to_string(*result.createdSourceIndex);
        if(result.accepted&&!result.completed)message+="\nPaused for an external engine request; use Trap Runtime to resume after applying its captured effects.";
        wxMessageBox(wxui::toWx(message),"SetMine result",wxOK|wxICON_INFORMATION,this);updateState();
    }
    void showTriggerMineRuntime() {
        try {
        auto* page=activePage();if(!page)return;
        wxArrayString operations;
        operations.Add("Mine visual state and captured client delete");
        operations.Add("Inventory snapshot / load");
        operations.Add("Replay captured detection (mine / door / placeable)");
        operations.Add("Door / placeable trap state");
        operations.Add("Enter mine using captured actor / inventory");
        operations.Add("Resume after captured external completion");
        operations.Add("Flag / disarm / recover action completion");
        operations.Add("Set mine action completion");
        const int operation=wxGetSingleChoiceIndex("Choose a trap runtime operation.","Trap Runtime",operations,this);
        if(operation<0)return;
        if(operation==1) {
            const auto text=editTrapCapture("Engine trap inventory capture",
                "Load or edit the captured item registry and ordered repositories. IDs are exact 64-bit values. "
                "Items contain objectId, tag, templateResref, containedItems, firstActivePropertySubtype, stackSize and possessor. "
                "The last three are nullable captures; SetMine diagnoses missing required values. Actors contain actor, inParty, availablePartyMember, personalItems and equippedItems. "
                "Each actor has 20 equipment slots (null means empty). K1 uses the party access booleans; K2 uses topMenuOverrideCharacter. "
                "No item identities, ownership or script effects are inferred.",
                trapcapture::inventoryText(page->areaScene().triggerRuntime().inventory()));
            if(text) {
                TrapInventoryState inventory;
                if(runJob("Reading inventory capture",[&](const neoshared::bif::JobControl& job) {
                    job.check();inventory=trapcapture::inventory(*text);job.check();
                }))page->setTrapInventory(std::move(inventory));
            }
            return;
        }
        if(operation==2){showTrapDetectionCapture();return;}
        if(operation==3){showObjectTrapState();return;}
        if(operation==6){showTrapActionCapture();return;}
        if(operation==7){showSetMineCapture();return;}
        if(operation==5) {
            const auto& pending=page->areaScene().triggerRuntime().pendingMineScript();
            if(!pending){wxMessageBox("No trap request is waiting for completion.","Trap Runtime",wxOK,this);return;}
            wxDialog confirmation(this,wxID_ANY,"Captured external completion");auto* box=new wxBoxSizer(wxVERTICAL);
            box->Add(new wxStaticText(&confirmation,wxID_ANY,wxui::toWx((pending->kind==TriggerMineRequest::Kind::RecoverInventoryItem?"Pending recovered item: "+pending->resource:
                pending->kind==TriggerMineRequest::Kind::UnequipInventoryItem?"Pending engine unequip: "+std::to_string(pending->item):"Pending engine script: "+pending->script)+
                "\nActor: "+std::to_string(pending->actor)+"\nLoad the engine result before resuming. The viewer does not execute scripts or allocate recovered item identities.")),0,wxALL,12);
            auto* completed=new wxCheckBox(&confirmation,wxID_ANY,"The engine request completed; its relevant captured results have been applied.");
            box->Add(completed,0,wxALL,12);box->Add(confirmation.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,12);
            confirmation.SetSizerAndFit(box);if(confirmation.ShowModal()!=wxID_OK||!completed->GetValue())return;
            const bool done=page->resumeTriggerMineEntry();
            wxMessageBox(done?"Trap continuation completed.":"Paused at the next engine external request.","Trap Runtime",wxOK,this);return;
        }
        const auto& mines=page->areaScene().triggerRuntime().mines();
        std::vector<std::size_t> indices;wxArrayString choices;
        for(std::size_t i=0;i<mines.size();++i)if(!mines[i].serverRemoved) {
            const auto& mine=mines[i];indices.push_back(i);
            choices.Add(wxui::toWx("TriggerList["+std::to_string(mine.sourceIndex)+"] | faction "+
                std::to_string(mine.faction)+" | animation "+std::to_string(mine.serverAnimation)));
        }
        if(indices.empty())return;
        const int selected=wxGetSingleChoiceIndex("Choose an loaded mine.","Trigger Mine Runtime",choices,this);
        if(selected<0)return;
        const auto mine=mines[indices[static_cast<std::size_t>(selected)]];
        if(operation==4) {
            const auto text=editTrapCapture("Captured mine entry",
                "Supply the engine firing inputs. The key is resolved from the loaded inventory, including containers and equipment. "
                "Execution pauses at each RunScript request; no script effects are supplied by the viewer.",
                R"({
  "actor": null, "targetIsCreature": null, "creatorIsCreature": null,
  "creatorHasFaction": null, "force": null, "mineImmune": null,
  "targetFaction": null, "reputationToCreator": null, "reputationToTrigger": null,
  "feat238": null, "partyMember": null, "partyLeaderHasServerCreature": null,
  "partyLeaderFeat239": null
})");
            if(!text)return;
            TriggerMineEnterInput input;
            if(!runJob("Reading mine entry capture",[&](const neoshared::bif::JobControl& job) {
                job.check();input=trapcapture::enter(*text);job.check();
            }))return;
            const bool done=page->enterTriggerMine(mine.sourceIndex,input);
            wxMessageBox(done?"Engine mine entry completed.":"Mine entry is paused for a engine script. See the event log and resume only after applying captured effects.","Trap Runtime",wxOK,this);return;
        }
        auto inputs=page->triggerMineInputs(mine.sourceIndex);
        wxDialog dialog(this,wxID_ANY,"Trigger Mine Runtime");
        auto* root=new wxBoxSizer(wxVERTICAL);
        auto* flag=new wxCheckBox(&dialog,wxID_ANY,"Flagged mine");flag->SetValue(mine.flagged);
        root->Add(flag,0,wxALL,8);
        auto* observer=new wxCheckBox(&dialog,wxID_ANY,"Supply a captured player observer");
        observer->SetValue(inputs.observer.has_value());root->Add(observer,0,wxLEFT|wxRIGHT|wxBOTTOM,8);
        auto* grid=new wxFlexGridSizer(2,6,8);grid->AddGrowableCol(1);
        const auto entry=[&](const wxString& label,const std::string& value) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            auto* text=new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(value));grid->Add(text,1,wxEXPAND);return text;
        };
        auto* mineIdentity=entry("Mine server object ID (decimal)",std::to_string(mine.objectId));
        auto* creatorIdentity=entry("Mine creator ID (decimal)",std::to_string(mine.creator));
        auto* identity=entry("Observer object ID (decimal)",inputs.observer?std::to_string(inputs.observer->objectId):"");
        auto* faction=entry("Observer faction",inputs.observer?std::to_string(inputs.observer->faction):"");
        auto* reputation=entry("Mine reputation toward observer",inputs.observer?std::to_string(inputs.reputation):"");
        root->Add(grid,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,8);
        const auto enableObserver=[=] {const bool enabled=observer->GetValue();identity->Enable(enabled);faction->Enable(enabled);reputation->Enable(enabled);};
        enableObserver();observer->Bind(wxEVT_CHECKBOX,[=](wxCommandEvent&){enableObserver();});
        auto* action=new wxChoice(&dialog,wxID_ANY);
        action->Append("Update state");action->Append("Replay explosion");action->Append("Replay disarm");action->SetSelection(0);
        root->Add(action,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,8);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "Flag and observer state drive the engine detected/undetected animation.\n"
            "Explosion and disarm replay the client animation and destruction sequence.\n"
            "Use Trap Runtime to load inventory or replay a captured detection check.\n"
            "Explosion playback uses its authored sound and engine channel."),0,wxALL,8);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALIGN_RIGHT|wxALL,8);
        dialog.SetSizerAndFit(root);if(dialog.ShowModal()!=wxID_OK)return;
        unsigned long long mineId{},creatorId{};
        if(!mineIdentity->GetValue().ToULongLong(&mineId)||!creatorIdentity->GetValue().ToULongLong(&creatorId))
            throw std::runtime_error("Enter the captured mine and creator object IDs");
        inputs.observer.reset();
        if(observer->GetValue()) {
            unsigned long long id{},group{};long relation{};
            if(!identity->GetValue().ToULongLong(&id)||
               !faction->GetValue().ToULongLong(&group)||group>0xffffffffull||
               !reputation->GetValue().ToLong(&relation)||relation<(-2147483647L-1L)||relation>2147483647L) {
                wxMessageBox("Enter the captured observer ID, faction and reputation.","Trigger Mine Runtime",wxOK|wxICON_ERROR,this);return;
            }
            inputs.observer=TriggerObserver{static_cast<std::uint64_t>(id),static_cast<std::uint32_t>(group)};
            inputs.reputation=static_cast<std::int32_t>(relation);
        }
        page->setTriggerObjectId(mine.sourceIndex,static_cast<std::uint64_t>(mineId));
        page->setTriggerCreator(mine.sourceIndex,static_cast<std::uint64_t>(creatorId));
        if(!page->setTriggerMineInputs(mine.sourceIndex,flag->GetValue(),inputs))return;
        if(action->GetSelection()!=0)page->deleteTriggerMine(mine.sourceIndex,static_cast<std::uint8_t>(action->GetSelection()));
        updateState();
        } catch(const std::exception& error){wxui::showError(this,error);}
    }

    void showCameraSettings() {
        wxDialog dialog(this, wxID_ANY, "Authored Camera Projection");
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog, wxID_ANY,
            "The camera node has no private FOV field or controller.\n"
            "These values belong to the external CAurCamera attached to the model part.\n"
            "SetAnimatedCamera uses 0.1 / 10000 clipping; the Camera default FOV is 45 degrees.\n"
            "Loaded GIT CameraList views use their authored FOV and these explicit near/far clips."),
            0, wxALL, 10);

        auto* grid = new wxFlexGridSizer(2, 6, 10);
        grid->AddGrowableCol(1, 1);
        const auto add = [&](const char* label, wxWindow* control) {
            grid->Add(new wxStaticText(&dialog, wxID_ANY, label),
                      0, wxALIGN_CENTER_VERTICAL);
            grid->Add(control, 1, wxEXPAND);
        };

        auto* fov = new wxSpinCtrlDouble(
            &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(160, -1),
            wxSP_ARROW_KEYS, 1.0, 179.0, options_.cameraSettings.viewAngle, 1.0);
        fov->SetDigits(2);
        add("Vertical FOV (degrees)", fov);

        auto* nearClip = new wxSpinCtrlDouble(
            &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(160, -1),
            wxSP_ARROW_KEYS, 0.0001, 1000000.0, options_.cameraSettings.nearClip, 0.1);
        nearClip->SetDigits(4);
        add("Near clip", nearClip);

        auto* farClip = new wxSpinCtrlDouble(
            &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(160, -1),
            wxSP_ARROW_KEYS, 0.001, 100000000.0, options_.cameraSettings.farClip, 100.0);
        farClip->SetDigits(3);
        add("Far clip", farClip);

        root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        root->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL),
                  0, wxEXPAND | wxALL, 10);
        dialog.SetSizerAndFit(root);

        while (dialog.ShowModal() == wxID_OK) {
            CameraPreviewSettings settings;
            settings.viewAngle = static_cast<float>(fov->GetValue());
            settings.nearClip = static_cast<float>(nearClip->GetValue());
            settings.farClip = static_cast<float>(farClip->GetValue());
            if (!std::isfinite(settings.viewAngle) ||
                !std::isfinite(settings.nearClip) ||
                !std::isfinite(settings.farClip) ||
                settings.viewAngle <= 0.0f || settings.viewAngle >= 180.0f ||
                settings.nearClip <= 0.0f ||
                settings.farClip <= settings.nearClip) {
                wxMessageBox(
                    "Use a vertical FOV between 0 and 180 degrees, a positive near clip, "
                    "and a far clip greater than the near clip.",
                    "Invalid camera projection", wxOK | wxICON_WARNING, &dialog);
                continue;
            }
            options_.cameraSettings = settings;
            applyOptions();
            break;
        }
    }

    void showParticleSettings() {
        wxDialog dialog(this,wxID_ANY,"Particle Scene Settings");
        auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "Preview-world inputs, not changes to the MDL. All targeted emitters use this target.\n"
            "Wind uses the KOTOR II scene WindManager; particles and DanglyMesh share each sample."),0,wxALL,10);
        auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1,1);
        const auto add=[&](const char* label,wxWindow* c) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            grid->Add(c,1,wxEXPAND);
        };
        auto settings=options_.particleSettings;
        auto* target=new wxCheckBox(&dialog,wxID_ANY,"Enable point-to-point target");
        target->SetValue(settings.targetEnabled);add("Target",target);
        auto* node=new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(settings.targetNode));add("Target node (optional)",node);
        const auto vector=[&](const char* label,std::initializer_list<float> values) {
            auto* panel=new wxPanel(&dialog);auto* row=new wxBoxSizer(wxHORIZONTAL);
            std::vector<wxSpinCtrlDouble*> controls;
            for(const float v:values) {
                auto* c=new wxSpinCtrlDouble(panel,wxID_ANY,wxEmptyString,wxDefaultPosition,wxSize(105,-1),
                    wxSP_ARROW_KEYS,std::numeric_limits<float>::lowest(),std::numeric_limits<float>::max(),v,0.1);c->SetDigits(3);
                row->Add(c,1,wxRIGHT,4);controls.push_back(c);
            }
            panel->SetSizer(row);add(label,panel);return controls;
        };
        const auto p=settings.targetPosition,q=settings.windVelocity;
        const auto position=vector("World target X / Y / Z",{p.x,p.y,p.z});
        const auto rotation=settings.targetOrientation;
        const auto orientation=vector("Target quaternion X / Y / Z / W",{rotation.x,rotation.y,rotation.z,rotation.w});
        auto* windPreset=new wxChoice(&dialog,wxID_ANY);
        windPreset->Append("Custom");
        windPreset->Append("K2 area wind 0 (none)");
        windPreset->Append("K2 area wind 1 (light)");
        windPreset->Append("K2 area wind 2 (strong)");
        windPreset->SetSelection(0);add("Area wind preset",windPreset);
        const auto wind=vector("Global wind X / Y / Z",{q.x,q.y,q.z});
        const auto windDeviation=vector("Maximum deviation Z / X (degrees)",
            {settings.windMaximumDeviationZDegrees,settings.windMaximumDeviationXDegrees});
        const auto windPeriod=vector("Noise interpolation period (seconds)",
            {settings.windInterpolationPeriod});
        windPeriod[0]->SetRange(std::numeric_limits<float>::denorm_min(),std::numeric_limits<float>::max());
        const auto near=[](float a,float b) {return std::fabs(a-b)<=1.0e-4f;};
        for(unsigned mode=0;mode<=2;++mode) {
            const auto preset=neomdl::model::k2AreaWindPreset(static_cast<std::uint8_t>(mode));
            if(preset && near(q.x,preset->globalWind.x)&&near(q.y,preset->globalWind.y)&&
               near(q.z,preset->globalWind.z)&&
               near(settings.windMaximumDeviationZDegrees,preset->maximumDeviationZDegrees)&&
               near(settings.windMaximumDeviationXDegrees,preset->maximumDeviationXDegrees)) {
                windPreset->SetSelection(static_cast<int>(mode)+1);break;
            }
        }
        windPreset->Bind(wxEVT_CHOICE,[=](wxCommandEvent&) {
            const int selection=windPreset->GetSelection();
            if(selection<=0) return;
            const auto preset=neomdl::model::k2AreaWindPreset(
                static_cast<std::uint8_t>(selection-1));
            if(!preset) return;
            wind[0]->SetValue(preset->globalWind.x);
            wind[1]->SetValue(preset->globalWind.y);
            wind[2]->SetValue(preset->globalWind.z);
            windDeviation[0]->SetValue(preset->maximumDeviationZDegrees);
            windDeviation[1]->SetValue(preset->maximumDeviationXDegrees);
        });
        const auto tint=settings.objectTint;
        const auto objectTint=vector("Object tint R / G / B",{tint.x,tint.y,tint.z});
        const auto objectAlpha=vector("Object alpha",{settings.objectAlpha});
        const auto tileAlpha=vector("Resolved tile alpha",{settings.tileAlpha});
        auto* blast=new wxCheckBox(&dialog,wxID_ANY,"Enable explosion point-source winds");
        blast->SetValue(settings.blastWind);add("Blast wind",blast);
        auto* bounce=new wxChoice(&dialog,wxID_ANY);
        bounce->Append("Legacy (type 1)");bounce->Append("Reflection (type 2)");
        bounce->SetSelection(settings.bounceMode==neomdl::model::ParticleBounceMode::Reflection?1:0);add("Standard bounce response",bounce);
        const auto refresh=[=]() {
            const bool manual=target->GetValue()&&node->GetValue().IsEmpty();
            node->Enable(target->GetValue());
            for(auto* c:position)c->Enable(manual);
            for(auto* c:orientation)c->Enable(manual);
        };
        target->Bind(wxEVT_CHECKBOX,[refresh](wxCommandEvent&){refresh();});
        node->Bind(wxEVT_TEXT,[refresh](wxCommandEvent&){refresh();});
        refresh();
        root->Add(grid,0,wxEXPAND|wxLEFT|wxRIGHT,10);
        root->Add(new wxStaticText(&dialog,wxID_ANY,"Applying changed settings resets particles and replays the current animation time."),0,wxALL,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);dialog.SetSizerAndFit(root);
        while(dialog.ShowModal()==wxID_OK) {
            const auto value=[](wxSpinCtrlDouble* c){return static_cast<float>(c->GetValue());};
            settings.targetEnabled=target->GetValue();settings.targetNode=wxui::toStd(node->GetValue());
            settings.targetPosition={value(position[0]),value(position[1]),value(position[2])};
            settings.targetOrientation={value(orientation[0]),value(orientation[1]),value(orientation[2]),value(orientation[3])};
            settings.windVelocity={value(wind[0]),value(wind[1]),value(wind[2])};
            settings.windMaximumDeviationZDegrees=value(windDeviation[0]);
            settings.windMaximumDeviationXDegrees=value(windDeviation[1]);
            settings.windInterpolationPeriod=value(windPeriod[0]);
            settings.objectTint={value(objectTint[0]),value(objectTint[1]),value(objectTint[2])};
            settings.objectAlpha=value(objectAlpha[0]);settings.tileAlpha=value(tileAlpha[0]);
            settings.blastWind=blast->GetValue();
            settings.bounceMode=bounce->GetSelection()==1?neomdl::model::ParticleBounceMode::Reflection:neomdl::model::ParticleBounceMode::Legacy;
            if(!validParticleSceneSettings(settings)) {
                wxMessageBox("Use finite values, a positive wind interpolation period, and a nonzero quaternion.","Invalid particle settings",wxOK|wxICON_WARNING,&dialog);continue;
            }
            options_.particleSettings=settings;applyOptions();break;
        }
    }

    void showLightsaberSettings() {
        wxDialog dialog(this, wxID_ANY, "Lightsaber Render Settings");
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog, wxID_ANY,
            "KOTOR II lightsaber render globals; not saved into the MDL.\n"
            "The engine bucket keeps owner insertion order and renders after particle emitters."),
            0, wxALL, 10);

        auto settings = options_.lightsaberSettings;
        auto* additive = new wxCheckBox(&dialog, wxID_ANY,
            "Additive blur (GL_ONE, GL_ONE; no depth writes)");
        additive->SetValue(settings.additiveBlur);
        auto* lighting = new wxCheckBox(&dialog, wxID_ANY,
            "Use enabled scene lighting");
        lighting->SetValue(settings.lighting);
        auto* wireframe = new wxCheckBox(&dialog, wxID_ANY,
            "Wireframe lightsaber geometry");
        wireframe->SetValue(settings.wireframe);
        auto* pieces = new wxCheckBox(&dialog, wxID_ANY,
            "Render stationary piece geometry");
        pieces->SetValue(settings.renderPieces);
        auto* front = new wxCheckBox(&dialog, wxID_ANY,
            "Render predictive front half while moving");
        front->SetValue(settings.renderFrontHalf);

        root->Add(additive, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(lighting, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(wireframe, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(pieces, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(front, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(new wxStaticText(&dialog, wxID_ANY,
            "Preview defaults: standard alpha blending, lighting off, "
            "wireframe off, stationary pieces on, front half on."),
            0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL),
                  0, wxEXPAND | wxALL, 10);
        dialog.SetSizerAndFit(root);
        if (dialog.ShowModal() != wxID_OK) return;

        settings.additiveBlur = additive->GetValue();
        settings.lighting = lighting->GetValue();
        settings.wireframe = wireframe->GetValue();
        settings.renderPieces = pieces->GetValue();
        settings.renderFrontHalf = front->GetValue();
        options_.lightsaberSettings = settings;
        applyOptions();
    }

    void showRuntimeTextureRotation() {
        auto* page=activePage();if(!page) return;
        wxArrayString choices;
        std::vector<std::pair<std::size_t,std::size_t>> parts;
        const auto append=[&](const model::Model& asset,std::size_t owner,const std::string& name) {
            if(model::engineSceneStageOrder(asset.game).empty()) return;
            for(const auto& mesh:asset.meshes) if(mesh.nodeIndex<asset.nodes.size()) {
                choices.Add(wxui::toWx(name+": "+asset.nodes[mesh.nodeIndex].name));
                parts.emplace_back(owner,mesh.nodeIndex);
            }
        };
        append(page->document().model(),0,"Main owner");
        for(std::size_t i=0;i<page->document().references().size();++i)
            append(page->document().references()[i].model,i+1,"Reference "+std::to_string(i+1));
        if(parts.empty()) return;
        const int selected=wxGetSingleChoiceIndex("Choose the runtime mesh part",
            "Runtime Texture Rotation",choices,this);
        if(selected<0) return;
        const auto part=parts[static_cast<std::size_t>(selected)];
        const auto text=wxGetTextFromUser(
            "Engine runtime angle in degrees; not saved into the MDL.\n"
            "Fixed-function consumers rotate around (0,0); engine vertex programs keep raw UVs.\n"
            "Zero restores the retained texture matrix. No animation speed is inferred.",
            "Runtime Texture Rotation",wxString::Format("%.9g",double(page->textureAngle(part.first,part.second))),this);
        if(text.empty()) return;
        double degrees=0;std::string error;
        if(!text.ToDouble(&degrees) || !std::isfinite(degrees) ||
           !page->setTextureAngle(part.first,part.second,static_cast<float>(degrees),error))
            wxMessageBox(error.empty()?"Enter a finite angle in degrees.":wxui::toWx(error),
                "Invalid texture rotation",wxOK|wxICON_WARNING,this);
    }

    void showMotionBlurOwners() {
        auto* page=activePage();if(!page)return;
        const auto owners=page->motionBlurScene().owners();
        wxArrayString names;
        for(const auto& owner:owners)names.Add(wxui::toWx(owner.label+" ["+std::to_string(owner.id)+"]"));
        if(names.empty()){wxMessageBox("No rendered engine scene owners are registered.","Motion-blur owners",wxOK|wxICON_INFORMATION,this);return;}
        const int selected=wxGetSingleChoiceIndex("Owner flags and render-committed history; not particles, lightsabers or temporal postprocessing.",
            "Engine motion-blur owners",names,this);if(selected<0)return;
        const auto& owner=owners.at(static_cast<std::size_t>(selected));
        auto settings=page->motionBlurScene().settings(owner.id);
        wxDialog dialog(this,wxID_ANY,"Engine Gob motion blur");auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&dialog,wxID_ANY,
            "Engine blur repeats the owner traversal and extrudes skin geometry.\n"
            "History advances after completed rendering, not animation ticks. References follow their parent.\n"
            "Position editing moves a primary Gob once. Rotation-only motion does not trigger skin extrusion."),0,wxALL,10);
        auto* enabled=new wxCheckBox(&dialog,wxID_ANY,"Append this owner to the engine blur list");enabled->SetValue(settings.enabled);box->Add(enabled,0,wxALL,10);
        box->Add(new wxStaticText(&dialog,wxID_ANY,"Maximum blur length (>0)"),0,wxLEFT,10);
        auto* maximum=new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(std::to_string(settings.maximumLength)));box->Add(maximum,0,wxEXPAND|wxALL,10);
        auto* move=new wxCheckBox(&dialog,wxID_ANY,"Set primary owner world position (not an attached reference)");box->Add(move,0,wxALL,10);
        const auto& w=owner.world.values;
        auto* position=new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(std::to_string(w[12])+" "+std::to_string(w[13])+" "+std::to_string(w[14])));box->Add(position,0,wxEXPAND|wxALL,10);
        auto* cut=new wxCheckBox(&dialog,wxID_ANY,"Reset all owner histories at the next frame (explicit cut)");box->Add(cut,0,wxALL,10);
        box->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);dialog.SetSizerAndFit(box);
        if(dialog.ShowModal()!=wxID_OK)return;
        try {
            std::string extra;std::istringstream limit(wxui::toStd(maximum->GetValue()));
            if(!(limit>>settings.maximumLength)||(limit>>extra)||!std::isfinite(settings.maximumLength)||settings.maximumLength<=0)
                throw std::runtime_error("Maximum length must be finite and positive");
            auto world=owner.world;
            if(move->GetValue()) {
                std::istringstream input(wxui::toStd(position->GetValue()));
                if(!(input>>world.values[12]>>world.values[13]>>world.values[14])||(input>>extra))throw std::runtime_error("Enter three finite position components");
                (void)engineRigidFrame(world);
                // Validate transform and owner before altering its flag/history.
                page->setSceneOwnerWorld(owner.id,world);
            }
            settings.enabled=enabled->GetValue();page->setMotionBlurOwner(owner.id,settings);
            if(cut->GetValue())page->resetMotionBlurHistory();
        }catch(const std::exception& e){wxMessageBox(wxui::toWx(e.what()),"Motion-blur inputs",wxOK|wxICON_ERROR,this);}
    }
    void showEngineExecution() {
        if(!activePage())return;
        wxDialog dialog(this,wxID_ANY,"Engine graphics");
        auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&dialog,wxID_ANY,
            "Changing upload capabilities resets context-owned textures and controller history."),0,wxALL,10);
        auto* ati=new wxCheckBox(&dialog,wxID_ANY,"Use ATI fragment shaders (engine capability setting)");
        ati->SetValue((options_.engineHardware.useAtiFragmentShaders&1)!=0);box->Add(ati,0,wxEXPAND|wxALL,10);
        auto* skinVp=new wxCheckBox(&dialog,wxID_ANY,"Use engine skin vertex programs when available");
        skinVp->SetValue(options_.engineUseVertexPrograms);box->Add(skinVp,0,wxEXPAND|wxALL,10);
        constexpr auto nvBits=material::k2Extension::RegisterCombiners|material::k2Extension::NvTextureShader;
        auto* nv=new wxCheckBox(&dialog,wxID_ANY,"Captured NV register-combiner and texture-shader capabilities");
        nv->SetValue((options_.engineHardware.existingExtensions&nvBits)==nvBits);box->Add(nv,0,wxEXPAND|wxALL,10);
        box->Add(new wxStaticText(&dialog,wxID_ANY,
            "ATI selection also affects ARB hardware.\n"
            "Changing these inputs starts a new graphics context state."),0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* newSorting=new wxCheckBox(&dialog,wxID_ANY,"Gob newSorting: apply transparency ordering before classification");
        newSorting->SetValue(options_.gobNewSorting);box->Add(newSorting,0,wxEXPAND|wxALL,10);
        wxArrayString mips;mips.Add("2D uploads: SGI automatic mip capability");mips.Add("2D uploads: GLU/Aur scaling and byte mip kernels");
        auto* mip=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,mips);
        mip->SetSelection(static_cast<int>(options_.procedureMipPath));box->Add(mip,0,wxEXPAND|wxALL,10);
        box->Add(new wxStaticText(&dialog,wxID_ANY,
            "Portable mipmaps and projection use the bundled implementation."),0,wxALL,10);
        box->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);dialog.SetSizerAndFit(box);
        if(dialog.ShowModal()!=wxID_OK)return;
        const auto previous=options_;
        options_.engineHardware.useAtiFragmentShaders=ati->GetValue();
        options_.engineUseVertexPrograms=skinVp->GetValue();
        options_.engineHardware.existingExtensions&=~nvBits;
        if(nv->GetValue())options_.engineHardware.existingExtensions|=nvBits;
        options_.gobNewSorting=newSorting->GetValue();
        options_.procedureMipPath=static_cast<render::ProcedureMipPath>(mip->GetSelection());
        try {applyOptions();}
        catch(const std::exception& error) {
            options_=previous;
            std::string diagnostic=error.what();
            try {applyOptions();}
            catch(const std::exception& rollback) {diagnostic+="\nCould not restore every tab: ";diagnostic+=rollback.what();}
            wxMessageBox(wxui::toWx(diagnostic),"Engine graphics",wxOK|wxICON_ERROR,this);
        }
    }
    void saveEngineExecutionReport() {
        auto* page=activePage();if(!page)return;
        wxFileDialog file(this,"Save engine execution report",wxEmptyString,"neomdl-engine-execution.txt",
            "Text report (*.txt)|*.txt",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(file.ShowModal()!=wxID_OK)return;
        try {
            std::ofstream out(neosettings::pathFromWx(file.GetPath()),std::ios::binary);
            if(!out)throw std::runtime_error("Cannot open execution report");
            out<<page->engineExecutionReport();out.close();
            if(!out)throw std::runtime_error("Cannot complete execution report");
        }catch(const std::exception& e){wxMessageBox(wxui::toWx(e.what()),"Execution report",wxOK|wxICON_ERROR,this);}
    }
    void showDirtyArea() {
        auto* page=activePage();if(!page)return;wxArrayString actions;
        actions.Add("Show effective tuples");actions.Add("Load tuples from an explicit ARE");actions.Add("Use loaded module tuples / no-area defaults");
        const int action=wxGetSingleChoiceIndex("Reconfiguration replaces only Dirty controllers; it does not rewrite a live controller.","Dirty area inputs",actions,this);
        if(action<0)return;
        try {
            if(action==1) {
                wxFileDialog file(this,"Read engine Dirty tuples",wxEmptyString,wxEmptyString,"K2 area (*.are)|*.are;*.ARE",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
                if(file.ShowModal()!=wxID_OK)return;page->loadDirtyArea(neosettings::pathFromWx(file.GetPath()));
            }else if(action==2)page->useLoadedDirtyArea();
            wxMessageBox(wxui::toWx(describeDirtyArea(page->dirtyAreaState())+
                "Engine automatic Dirty routing is absent. Only the explicit Desktop diagnostic samples 3D."),"Dirty area inputs",wxOK|wxICON_INFORMATION,this);
        }catch(const std::exception& e){wxMessageBox(wxui::toWx(e.what()),"Dirty ARE not loaded",wxOK|wxICON_ERROR,this);}
    }
    void showDirtyMaterial() {
        auto* page=activePage();if(!page)return;
        wxArrayString actions;actions.Add("Disable diagnostic");actions.Add("Select a rigid owner mesh for the legacy Desktop caller");
        const int action=wxGetSingleChoiceIndex("Neither engine GetRenderPath selects NoiseMapped. This is an explicit dormant-caller diagnostic, not gameplay activation.","Legacy Dirty caller",actions,this);
        if(action<0)return;if(action==0){page->disableDirtyMaterial();return;}
        struct Target{LightOwnerId owner;std::size_t mesh;};std::vector<Target> targets;wxArrayString names;
        for(const auto& owner:page->motionBlurScene().owners())for(std::size_t i=0;i<owner.asset->meshes.size();++i) {
            const auto& mesh=owner.asset->meshes[i];if(mesh.skinned||mesh.animated||mesh.indices.empty())continue;
            targets.push_back({owner.id,i});names.Add(wxui::toWx(owner.label+" / mesh "+std::to_string(i)));
        }
        if(targets.empty()){wxMessageBox("No rigid K2 meshes are registered.","Dirty material",wxOK|wxICON_INFORMATION,this);return;}
        const int chosen=wxGetSingleChoiceIndex("Only this target is rerouted. Other owners retain their ordinary materials.","Dirty target",names,this);if(chosen<0)return;
        auto value=page->dirtySettings();wxDialog dialog(this,wxID_ANY,"Legacy Desktop Dirty caller");auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&dialog,wxID_ANY,
            "Uses real dirtytex / dirtytex2 / dirtytex3 resources with mipmap 0.\n"
            "Formula/material combinations with engine no-op dispatch are rejected.\n"
            "The lightmap-only Force Sight command samples retained 2D state, not the volume.\n"
            "Subtract lightmap variants inherit 3D filtering, including an incomplete default sampler."),0,wxALL,10);
        wxArrayString slots;slots.Add("dirtytex");slots.Add("dirtytex2");slots.Add("dirtytex3");
        auto* slot=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,slots);slot->SetSelection(static_cast<int>(value.slot));box->Add(slot,0,wxEXPAND|wxALL,10);
        auto* world=new wxCheckBox(&dialog,wxID_ANY,"Engine world-space noise coordinates");world->SetValue(value.worldSpace==1);box->Add(world,0,wxALL,10);
        auto* force=new wxCheckBox(&dialog,wxID_ANY,"Force Sight branch (caller only)");force->SetValue(value.forceSight);box->Add(force,0,wxALL,10);
        auto* shift=new wxCheckBox(&dialog,wxID_ANY,"Color-shift command (engine no-op; diagnostic error)");shift->SetValue(value.colorShift);box->Add(shift,0,wxALL,10);
        box->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);dialog.SetSizerAndFit(box);if(dialog.ShowModal()!=wxID_OK)return;
        try {
            value.enabled=true;value.slot=static_cast<unsigned>(slot->GetSelection());value.worldSpace=world->GetValue()?1:0;
            value.forceSight=force->GetValue();value.colorShift=shift->GetValue();
            std::shared_ptr<GameResourceSession> session;std::string warning,error;
            const auto installed=installedResourceResolver(page->document().model().game,page->document().path(),session,warning);
            const auto resolver=combineResolvers(context_.resolveResource,installed);
            if(value.slot>=3||!page->document().loadRuntimeTexture(material::dirtyTextureNames[value.slot],resolver,error))throw std::runtime_error(error.empty()?"Invalid Dirty slot":error);
            const auto target=targets.at(static_cast<std::size_t>(chosen));page->setDirtyMaterial(target.owner,target.mesh,value);
        }catch(const std::exception& e){wxMessageBox(wxui::toWx(e.what()),"Dirty diagnostic not enabled",wxOK|wxICON_WARNING,this);}
    }

    void showVideoEffectRow() {
        auto* page=activePage();if(!page)return;
        const auto game=page->document().model().game;
        const auto current=page->sceneEffects();
        wxTextEntryDialog dialog(this,
            "VideoEffects.2da row ordinal. A negative value disables the engine video effects.\n"
            "Framebuffer capture and its texture inputs remain in Engine Scene Effects.",
            "Engine Video Effect Row",wxString::Format("%d",current.engineVideoEffect.value_or(-2)));
        if(dialog.ShowModal()!=wxID_OK)return;
        long long row{};
        if(!dialog.GetValue().ToLongLong(&row)||row<std::numeric_limits<int>::min()||row>std::numeric_limits<int>::max()) {
            wxMessageBox("Enter a signed 32-bit row ordinal.","Video effect row",wxOK|wxICON_WARNING,this);return;
        }
        try {
            std::shared_ptr<GameResourceSession> session;std::string warning,error;
            ResourceResolver resolver;
            if(row>=0) {
                const auto installed=installedResourceResolver(page->document().model().game,page->document().path(),session,warning);
                resolver=combineResolvers(page->document().resourceResolver(),combineResolvers(captureHostResolver(),installed));
            }
            const auto hardware=material::selectK2HardwareCapabilities(options_.engineHardware);
            VideoEffectActivation activation;
            if(!runJob("Loading engine video effect",[&](const neoshared::bif::JobControl& job) {
                activation=loadVideoEffect(resolver,static_cast<int>(row),current,game,
                    hardware.frameBufferModifications||hardware.atiFrameBufferModifications);
                if(row<0)return; // engine disable does not read a table or reload retained textures
                auto textures=requiredSceneEffectTextures(activation.settings,false,game);
                if(activation.settings.furyLevel&&std::find(textures.begin(),textures.end(),SceneEffectTexture::Fury)==textures.end())
                    textures.push_back(SceneEffectTexture::Fury);
                for(const auto role:textures) {
                    const auto& name=sceneEffectTextureName(activation.settings,role);
                    job.update(0,0,"Loading video-effect texture "+name);
                    if(!name.empty()&&!page->document().loadRuntimeTexture(name,resolver,error))throw std::runtime_error(error);
                }
            }))return;
            if(!page->setSceneEffects(std::move(activation.settings),error,false))throw std::runtime_error(error);
            if(game==model::GameVersion::Kotor2)page->setScriptVideoEffect(row>=0);
            updateState();setModuleStatusText("Engine video effect: "+std::to_string(activation.effectId),0);
        }catch(const std::exception& error){wxui::showError(this,error);}
    }

    void showSceneEffects() {
        auto* page=activePage();
        if(!page||model::engineSceneStageOrder(page->document().model().game).empty())return;
        const auto game=page->document().model().game;
        const bool k1=game==model::GameVersion::Kotor1;
        auto value=page->sceneEffects();
        wxDialog dialog(this,wxID_ANY,"Engine Scene Effects — current document",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            k1 ? "KOTOR I engine scene effects. Applying resets temporal history.\n"
                 "Uses the engine ATI effect order."
               : "Full-scene runtime inputs, not MDL/TXI fields. Applying resets temporal history.\n"
                 "Uses stencil distortion and ARB overlays.\n"
                 "Use actual texture ResRefs; no neutral replacement images are generated."),0,wxALL,10);
        auto* form=new wxScrolledWindow(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxVSCROLL);
        auto* grid=new wxFlexGridSizer(2,5,10);grid->AddGrowableCol(1,1);
        const auto toggle=[&](const char* label,bool state) {
            auto* c=new wxCheckBox(form,wxID_ANY,label);c->SetValue(state);grid->Add(c);grid->AddSpacer(1);return c;
        };
        const auto field=[&](const char* label,const std::string& text) {
            grid->Add(new wxStaticText(form,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            auto* c=new wxTextCtrl(form,wxID_ANY,wxui::toWx(text));c->SetMinSize(wxSize(FromDIP(260),wxDefaultCoord));grid->Add(c,1,wxEXPAND);return c;
        };
        auto* enabled=toggle("Enable engine scene-effect graph",value.enabled);
        auto* film=toggle("Film noise",value.filmNoise);auto* scan=toggle("Scan noise",value.scanNoise);
        auto* saturation=toggle("Saturation adjustment",value.saturationEnabled);
        auto* speed=toggle("Temporal speed blur",value.speedBlur);
        auto* dream=k1?nullptr:toggle("Dream overlay",value.dream);
        auto* forceSight=k1?nullptr:toggle("Force Sight (independent of framebuffer effects)",value.forceSight);
        auto* forceDesaturation=k1?nullptr:field("Force Sight desaturation",std::to_string(value.forceSightDesaturation));
        auto* convolution=toggle("Desktop convolution/bloom",value.desktopConvolution);
        auto* overbright=toggle("Desktop overbright lookup",value.desktopOverbrighten);
        auto* fury=k1?nullptr:field("Fury level (0 disables)",std::to_string(value.furyLevel));
        auto* amount=field("Saturation [0,1]",std::to_string(value.saturation));
        auto* multiplier=field("Saturation RGB multipliers",std::to_string(value.saturationMultiplier[0])+" "+std::to_string(value.saturationMultiplier[1])+" "+std::to_string(value.saturationMultiplier[2]));
        auto* accumulation=field("History ratio [0,1]",std::to_string(value.accumulationRatio));
        auto* letterbox=k1?nullptr:field("Desktop dream letterbox top/bottom (blank: off)",value.desktopDreamLetterbox?
            std::to_string((*value.desktopDreamLetterbox)[0])+" "+std::to_string((*value.desktopDreamLetterbox)[1]):"");
        auto* noiseTex=field("Film / scan noise ResRef",value.filmNoiseTexture);
        auto* forceTex=field("Force distortion ResRef",value.forceDistortionTexture);
        auto* dreamTex=k1?nullptr:field("Dream outline ResRef",value.dreamTexture);
        auto* furyTex=k1?nullptr:field("Fury outline ResRef",value.furyTexture);
        form->SetSizer(grid);form->SetScrollRate(0,FromDIP(16));form->SetMinSize(FromDIP(wxSize(600,420)));form->FitInside();
        root->Add(form,1,wxEXPAND|wxLEFT|wxRIGHT,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);dialog.SetSizerAndFit(root);
        const auto parse=[](wxTextCtrl* c,auto& v){std::istringstream input(wxui::toStd(c->GetValue()));std::string extra;return bool(input>>v)&&!(input>>extra);};
        while(dialog.ShowModal()==wxID_OK) {
            value.engineVideoEffect.reset();
            value.enabled=enabled->GetValue();value.filmNoise=film->GetValue();value.scanNoise=scan->GetValue();
            value.saturationEnabled=saturation->GetValue();value.speedBlur=speed->GetValue();
            if(!k1) {value.dream=dream->GetValue();value.forceSight=forceSight->GetValue();}
            value.desktopConvolution=convolution->GetValue();value.desktopOverbrighten=overbright->GetValue();
            std::istringstream rgb(wxui::toStd(multiplier->GetValue()));std::string extra;long long level=value.furyLevel;
            if((!k1&&(!parse(fury,level)||level<0||level>2147483647||!parse(forceDesaturation,value.forceSightDesaturation)))||
               !parse(amount,value.saturation)||!parse(accumulation,value.accumulationRatio)||
               !(rgb>>value.saturationMultiplier[0]>>value.saturationMultiplier[1]>>value.saturationMultiplier[2])||(rgb>>extra)) {
                wxMessageBox("Enter finite numeric inputs with the indicated component counts.","Invalid scene settings",wxOK|wxICON_WARNING,&dialog);continue;
            }
            if(!k1) {
              value.desktopDreamLetterbox.reset();
              const auto bounds=wxui::toStd(letterbox->GetValue());
              if(!bounds.empty()) {
                std::istringstream input(bounds);std::array<int,2> range{};
                if(!(input>>range[0]>>range[1])||(input>>extra)) {
                    wxMessageBox("Use two integer pixel bounds, or leave the field empty.","Invalid letterbox bounds",wxOK|wxICON_WARNING,&dialog);continue;
                }
                value.desktopDreamLetterbox=range;
              }
            }
            value.furyLevel=static_cast<unsigned>(level);
            value.filmNoiseTexture=wxui::toStd(noiseTex->GetValue());value.forceDistortionTexture=wxui::toStd(forceTex->GetValue());
            if(!k1) {value.dreamTexture=wxui::toStd(dreamTex->GetValue());value.furyTexture=wxui::toStd(furyTex->GetValue());}
            std::string error;
            if(!validateSceneEffects(value,error,game)) {
                wxMessageBox(wxui::toWx(error),"Invalid scene settings",wxOK|wxICON_WARNING,&dialog);continue;
            }
            std::shared_ptr<GameResourceSession> session;std::string warning;
            const auto installed=installedResourceResolver(page->document().model().game,page->document().path(),session,warning);
            const auto resolver=combineResolvers(context_.resolveResource,installed);
            bool okay=true;
            // Load all explicitly named inputs, including a force field selected
            // before enabling an owner. Required-but-blank inputs fail in canvas.
            if(value.enabled)for(auto role:{SceneEffectTexture::FilmNoise,SceneEffectTexture::ForceDistortion,SceneEffectTexture::Dream,SceneEffectTexture::Fury}) {
                if(k1&&(role==SceneEffectTexture::Dream||role==SceneEffectTexture::Fury))continue;
                const auto& name=sceneEffectTextureName(value,role);
                if(!name.empty()&&!page->document().loadRuntimeTexture(name,resolver,error)){okay=false;break;}
            }
            if(!okay||!page->setSceneEffects(value,error)) {
                wxMessageBox(wxui::toWx(error),"Scene effects not applied",wxOK|wxICON_WARNING,&dialog);continue;
            }
            updateState();break;
        }
    }

    void showOwnerVisualEffects() {
        auto* page=activePage();
        if(!page||model::engineSceneStageOrder(page->document().model().game).empty())return;
        wxArrayString owners;
        owners.Add("Main owner");
        for(std::size_t i=0;i<page->document().references().size();++i)
            owners.Add(wxui::toWx("Reference "+std::to_string(i+1)+": "+page->document().references()[i].resref));
        const int selected=owners.size()==1 ? 0 :
            wxGetSingleChoiceIndex("Choose the independent runtime owner", "Owner Visual Effects", owners,this);
        if(selected<0) return;
        const auto owner=static_cast<std::size_t>(selected);
        const auto ownerGame=owner ? page->document().references()[owner-1].model.game
                                  : page->document().model().game;
        const bool k1=ownerGame==model::GameVersion::Kotor1;
        const bool hologramAvailable=!k1 &&
            page->document().model().game==model::GameVersion::Kotor2;
        auto value=page->ownerVisualEffects(owner);
        wxDialog dialog(this,wxID_ANY,wxString("Owner Visual Effects — ")+owners[owner]);
        auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            !hologramAvailable ? "Runtime state for this owner and its parts; not saved into the MDL.\n"
                 "Reference owners, area rooms and particle chunks are independent."
               : "Runtime state for this owner and its parts; not saved into the MDL.\n"
                 "Other reference owners, area rooms and particle chunks are independent.\n"
                 "Holograms use the actual Holotex resource. No replacement texture is generated."),0,wxALL,10);
        wxCheckBox* hologram=nullptr;
        if(hologramAvailable) {
            hologram=new wxCheckBox(&dialog,wxID_ANY,"Enable engine hologram rendering");
            hologram->SetValue(value.hologram);root->Add(hologram,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        }
        auto* distortion=new wxCheckBox(&dialog,wxID_ANY,hologramAvailable ? "Enable engine distortion producer (hologram takes precedence)" : "Enable engine distortion producer");
        distortion->SetValue(value.distortion);root->Add(distortion,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* grid=new wxFlexGridSizer(2,7,10);grid->AddGrowableCol(1,1);
        const auto format=[](const auto& values) {
            std::ostringstream text;text<<std::setprecision(9);
            for(std::size_t i=0;i<values.size();++i) {if(i) text<<", ";text<<values[i];}
            return wxui::toWx(text.str());
        };
        const auto field=[&](const char* label,const wxString& text) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            auto* control=new wxTextCtrl(&dialog,wxID_ANY,text);
            control->SetMinSize(wxSize(FromDIP(270),wxDefaultCoord));
            grid->Add(control,1,wxEXPAND);return control;
        };
        const auto toggle=[&](const char* label,bool on) {
            auto* control=new wxCheckBox(&dialog,wxID_ANY,label);control->SetValue(on);
            grid->Add(control,0,wxALIGN_CENTER_VERTICAL);grid->AddSpacer(1);return control;
        };
        auto* shift=toggle("Multiply material diffuse RGBA",value.colorShiftEnabled);
        auto* shiftValue=field("Diffuse shift R, G, B, A",format(value.colorShift));
        auto* emission=toggle("Add material emission",value.illuminationEnabled);
        auto* emissionValue=field("Emission addition R, G, B",format(value.illumination));
        auto* illumination=k1?nullptr:toggle("Enable per-object illumination shader",value.perObjectIlluminationEnabled);
        auto* illuminationValue=k1?nullptr:field("Shader illumination R, G, B",format(value.perObjectIllumination));
        auto* forceGlow=k1?nullptr:toggle("Force Sight glow",value.forceSight.glow);
        auto* forceBlend=k1?nullptr:toggle("Force Sight non-force blending",value.forceSight.nonForceBlending);
        auto* forceColor=k1?nullptr:field("Force Sight glow R, G, B, A",format(value.forceSight.color));
        auto* shell=toggle("Enable bumped-out shell",value.shellAmount!=0.0f);
        auto* shellTexture=field("Shell texture ResRef",wxui::toWx(value.shellTexture));
        auto* shellAmount=field("Shell displacement",format(std::array<float,1>{{value.shellAmount}}));
        root->Add(grid,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
        if(!k1)root->Add(new wxStaticText(&dialog,wxID_ANY,
            "The illumination shader only activates in the engine consuming material paths."),0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "Shell blend factors, alpha testing and depth writes come from the effect texture's TXI.\n"
            "Signed finite inputs are retained; zero displacement disables the shell."),0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
        dialog.SetSizerAndFit(root);
        const auto parse=[](wxTextCtrl* control,auto& values) {
            auto text=wxui::toStd(control->GetValue());
            std::replace(text.begin(),text.end(),',',' ');
            std::istringstream input(text);
            for(auto& component:values) if(!(input>>component)||!std::isfinite(component)) return false;
            std::string remainder;return !(input>>remainder);
        };
        while(dialog.ShowModal()==wxID_OK) {
            std::array<float,1> amount{};
            if(!parse(shiftValue,value.colorShift)||!parse(emissionValue,value.illumination)||
               (!k1&&(!parse(illuminationValue,value.perObjectIllumination)||!parse(forceColor,value.forceSight.color)))||!parse(shellAmount,amount)) {
                wxMessageBox("Enter the indicated number of finite components, separated by spaces or commas.",
                    "Invalid effect values",wxOK|wxICON_WARNING,&dialog);continue;
            }
            if(hologramAvailable)value.hologram=hologram->GetValue();
            value.distortion=distortion->GetValue();
            value.colorShiftEnabled=shift->GetValue();
            value.illuminationEnabled=emission->GetValue();
            if(!k1) {
                value.perObjectIlluminationEnabled=illumination->GetValue();
                value.forceSight.glow=forceGlow->GetValue();
                value.forceSight.nonForceBlending=forceBlend->GetValue();
            }
            value.shellTexture=wxui::toStd(shellTexture->GetValue());
            value.shellAmount=shell->GetValue() ? amount[0] : 0.0f;
            std::string error;
            if(!render::validOwnerVisualEffects(value,error)) {
                wxMessageBox(wxui::toWx(error),"Invalid owner state",wxOK|wxICON_WARNING,&dialog);continue;
            }
            std::shared_ptr<GameResourceSession> session;
            std::string warning;
            const auto installed=installedResourceResolver(page->document().model().game,
                page->document().path(),session,warning);
            const auto resolver=combineResolvers(context_.resolveResource,installed);
            if((hologramAvailable && value.hologram && !page->document().loadRuntimeTexture("Holotex",resolver,error)) ||
               (value.shellAmount!=0.0f && !page->document().loadRuntimeTexture(value.shellTexture,resolver,error)) ||
               !page->setOwnerVisualEffects(owner,value,error)) {
                wxMessageBox(wxui::toWx(error),"Owner effects not applied",wxOK|wxICON_WARNING,&dialog);continue;
            }
            updateState();break;
        }
    }

    void showK2SkinSettings() {
        wxDialog dialog(this, wxID_ANY, "Manual Skin Blur Diagnostic");
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog, wxID_ANY,
            "Recovered PartSkin owner/scene state; these values are not MDL fields.\n"
            "Manual-vector diagnostic, separate from the engine blur-owner list.\n"
            "Bumped-out shells are configured per owner in Owner Visual Effects."),
            0, wxALL, 10);

        auto settings = options_.skinSettings;
        auto* blur = new wxCheckBox(&dialog, wxID_ANY,
            "Preview translation motion blur");
        blur->SetValue(settings.motionBlur);
        auto* side = new wxCheckBox(&dialog, wxID_ANY,
            "Enable recovered camera-side extrusion");
        side->SetValue(settings.sideExtrusion);

        const auto number = [&](double value, double minimum, double maximum,
                                double increment) {
            auto* control = new wxSpinCtrlDouble(
                &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition,
                wxDefaultSize, wxSP_ARROW_KEYS, minimum, maximum, value,
                increment);
            control->SetDigits(4);
            return control;
        };
        auto* motionX = number(settings.objectMotion.x, -100000.0, 100000.0, 0.01);
        auto* motionY = number(settings.objectMotion.y, -100000.0, 100000.0, 0.01);
        auto* motionZ = number(settings.objectMotion.z, -100000.0, 100000.0, 0.01);
        auto* maximum = number(settings.maximumBlurLength, 0.0001, 100000.0, 0.01);
        auto* sideScale = number(settings.sideScale, 0.0, 100000.0, 0.01);

        root->Add(blur, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        auto* blurGrid = new wxFlexGridSizer(2, 7, 10);
        blurGrid->AddGrowableCol(1, 1);
        const auto addBlur = [&](const char* label, wxWindow* control) {
            blurGrid->Add(new wxStaticText(&dialog, wxID_ANY, label),
                          0, wxALIGN_CENTER_VERTICAL);
            blurGrid->Add(control, 1, wxEXPAND);
        };
        addBlur("Gob motion X", motionX);
        addBlur("Gob motion Y", motionY);
        addBlur("Gob motion Z", motionZ);
        addBlur("Maximum blur length", maximum);
        root->Add(blurGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(side, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        auto* sideGrid = new wxFlexGridSizer(2, 7, 10);
        sideGrid->AddGrowableCol(1, 1);
        sideGrid->Add(new wxStaticText(&dialog, wxID_ANY,
                                      "External side scalar"),
                      0, wxALIGN_CENTER_VERTICAL);
        sideGrid->Add(sideScale, 1, wxEXPAND);
        root->Add(sideGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(new wxStaticText(&dialog, wxID_ANY,
            "Gob motion is current minus previous object position in model space.\n"
            "The recovered shader clamps its length, extrudes only back-facing vertices,\n"
            "and uses a fixed 0.15 multiplier after the external side scalar."),
            0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        const auto updateEnabled = [&] {
            const bool blurEnabled = blur->GetValue();
            motionX->Enable(blurEnabled);
            motionY->Enable(blurEnabled);
            motionZ->Enable(blurEnabled);
            maximum->Enable(blurEnabled);
            side->Enable(blurEnabled);
            sideScale->Enable(blurEnabled && side->GetValue());
        };
        blur->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent&) { updateEnabled(); });
        side->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent&) { updateEnabled(); });
        updateEnabled();

        root->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL),
                  0, wxEXPAND | wxALL, 10);
        dialog.SetSizerAndFit(root);
        while (dialog.ShowModal() == wxID_OK) {
            settings.motionBlur = blur->GetValue();
            settings.objectMotion = {
                static_cast<float>(motionX->GetValue()),
                static_cast<float>(motionY->GetValue()),
                static_cast<float>(motionZ->GetValue())};
            settings.maximumBlurLength =
                static_cast<float>(maximum->GetValue());
            settings.sideExtrusion = side->GetValue();
            settings.sideScale = static_cast<float>(sideScale->GetValue());
            const bool valid = std::isfinite(settings.objectMotion.x) &&
                std::isfinite(settings.objectMotion.y) &&
                std::isfinite(settings.objectMotion.z) &&
                std::isfinite(settings.maximumBlurLength) &&
                settings.maximumBlurLength > 0.0f &&
                std::isfinite(settings.sideScale) && settings.sideScale >= 0.0f;
            if (!valid) {
                wxMessageBox(
                    "Use finite values, a positive maximum blur length, and a "
                    "nonnegative side scalar.",
                    "Invalid Skin Settings", wxOK | wxICON_WARNING, &dialog);
                continue;
            }
            options_.skinSettings = settings;
            applyOptions();
            break;
        }
    }

    void showLightingSettings() {
        wxDialog dialog(this,wxID_ANY,"Scene Lighting and Fog");
        auto* root=new wxBoxSizer(wxVERTICAL);
        auto* note=new wxStaticText(&dialog,wxID_ANY,
            "Viewer scene settings; not saved into the model.\n"
            "Up to eight lights per mesh, including the optional preview key.\n"
            "Fog distances are scene units. MDL files do not supply scene fog color/range.");
        root->Add(note,0,wxALL,10);
        auto* grid=new wxFlexGridSizer(2,8,12);grid->AddGrowableCol(1,1);
        const auto add=[&](const char* label,wxWindow* control) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            grid->Add(control,1,wxEXPAND);
        };
        const auto toColor=[](float r,float g,float b) {
            return wxColour(static_cast<unsigned char>(std::clamp(r,0.0f,1.0f)*255),
                static_cast<unsigned char>(std::clamp(g,0.0f,1.0f)*255),
                static_cast<unsigned char>(std::clamp(b,0.0f,1.0f)*255));
        };
        auto* ambient=new wxColourPickerCtrl(&dialog,wxID_ANY,toColor(options_.sceneAmbient[0],options_.sceneAmbient[1],options_.sceneAmbient[2]));
        add("Scene ambient",ambient);
        auto* mode=new wxChoice(&dialog,wxID_ANY);
        for(const auto* name:{"Disabled","Linear","Exponential","Exponential squared"}) mode->Append(name);
        mode->SetSelection(static_cast<int>(options_.fog.mode));add("Fog mode",mode);
        auto* color=new wxColourPickerCtrl(&dialog,wxID_ANY,toColor(options_.fog.color.x,options_.fog.color.y,options_.fog.color.z));
        add("Fog color",color);
        const auto number=[&](double value,double min,double max,double increment) {
            auto* control=new wxSpinCtrlDouble(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
                wxSP_ARROW_KEYS,min,max,value,increment);control->SetDigits(4);return control;
        };
        auto* ownerRadius=number(options_.objectLightRadius,0,1000000,0.1);
        add("Gob light-query radius",ownerRadius);
        ownerRadius->SetToolTip("External Gob +0x164 state. The engine constructor initializes it to 1; this is not the mesh AABB radius.");
        auto* start=number(options_.fog.start,0,1000000,1);add("Linear fog start",start);
        auto* end=number(options_.fog.end,0.001,1000001,1);add("Linear fog end",end);
        auto* density=number(options_.fog.density,0,1000,0.005);add("Exponential density",density);
        auto* respect=new wxCheckBox(&dialog,wxID_ANY,"Respect each model's fog flag");
        respect->SetValue(options_.fog.respectModelFlag);add("Fog eligibility",respect);
        root->Add(grid,1,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
        dialog.SetSizerAndFit(root);
        while(dialog.ShowModal()==wxID_OK) {
            auto fog=options_.fog;
            fog.mode=static_cast<neomdl::model::FogMode>(mode->GetSelection());
            fog.start=static_cast<float>(start->GetValue());fog.end=static_cast<float>(end->GetValue());
            fog.density=static_cast<float>(density->GetValue());fog.respectModelFlag=respect->GetValue();
            const auto c=color->GetColour();fog.color={c.Red()/255.0f,c.Green()/255.0f,c.Blue()/255.0f};
            if(!neomdl::model::validFog(fog)) {
                wxMessageBox("Fog values must be finite and nonnegative.","Invalid fog",wxOK|wxICON_WARNING,&dialog);
                continue;
            }
            options_.objectLightRadius=static_cast<float>(ownerRadius->GetValue());
            options_.fog=fog;const auto a=ambient->GetColour();
            options_.sceneAmbient={{a.Red()/255.0f,a.Green()/255.0f,a.Blue()/255.0f}};
            applyOptions();break;
        }
    }

    void bindEvents() {
        tabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, [this](wxAuiNotebookEvent& event) {
            updateState();
            event.Skip();
        });
        tabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& event) {
            const int page = event.GetSelection();
            event.Veto();
            if (jobRunning_) { cancelJob_.store(true); return; }
            CallAfter([this, page] { closePage(page); });
        });
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            if (jobRunning_) return;
            switch (event.GetId()) {
            case ID_Open: chooseAndOpenModel(); return;
            case ID_ExportAscii: exportAsciiModel(); return;
            case ID_ExportPacked: exportPackedModel(); return;
            case ID_LoadArea: chooseArea(); return;
            case ID_ModuleArea: chooseModuleArea(); return;
            case ID_ResourceProvenance: showResourceProvenance(); return;
            case ID_SceneShadows: chooseSceneShadows(); return;
            case ID_ShadowCasters: chooseShadowCasters(); return;
            case ID_ClearArea: if (auto* page=activePage()) page->clearArea(); return;
            case ID_AreaRoom: chooseAreaRoom(); return;
            case ID_AreaEnvironment: chooseAreaEnvironment(); return;
            case ID_WorldPlayback: chooseWorldPlayback(); return;
            case ID_AreaWeather: showAreaWeather(); return;
            case ID_SceneAudio: showSceneAudio(); return;
            case ID_AreaAnimation: chooseAreaAnimation(); return;
            case ID_RoomScriptAnimation: chooseRoomScriptAnimation(); return;
            case ID_TrackAnimation: chooseTrackAnimation(); return;
            case ID_DoorState: chooseDoorState(); return;
            case ID_LoadGrass: chooseGrassArea(); return;
            case ID_GrassOptions: chooseGrassOptions(); return;
            case ID_CloseModel: closePage(tabs_->GetSelection()); return;
            case ID_CloseAll: closeAll(); return;
            case ID_FrameModel: if (auto* page = activePage()) page->frameModel(); return;
            case ID_Textures: options_.textures = event.IsChecked(); applyOptions(); return;
            case ID_Lightmaps: options_.lightmaps = event.IsChecked(); applyOptions(); return;
            case ID_Wireframe: options_.wireframe = event.IsChecked(); applyOptions(); return;
            case ID_Lighting: options_.lighting = event.IsChecked(); applyOptions(); return;
            case ID_Headlight: options_.headlight=event.IsChecked();applyOptions();return;
            case ID_Fog:
                options_.fog.mode=event.IsChecked() ? lastFogMode_ : neomdl::model::FogMode::Disabled;
                applyOptions();return;
            case ID_LightingSettings: showLightingSettings();return;
            case ID_Grid: options_.grid = event.IsChecked(); applyOptions(); return;
            case ID_Axes: options_.axes = event.IsChecked(); applyOptions(); return;
            case ID_ShowHidden: options_.showHidden = event.IsChecked(); applyOptions(); return;
            case ID_CullBackfaces: options_.cullBackfaces = event.IsChecked(); applyOptions(); return;
            case ID_DanglySimulation:
                options_.danglySimulation = event.IsChecked(); applyOptions(); return;
            case ID_NodeHelpers:
                options_.nodeHelpers = event.IsChecked(); applyOptions(); return;
            case ID_EngineShadows:
                options_.engineShadows = event.IsChecked(); applyOptions(); return;
            case ID_EngineTwoSidedShadows:
                options_.engineTwoSidedShadows = event.IsChecked(); applyOptions(); return;
            case ID_EngineLensFlares:
                options_.engineLensFlares = event.IsChecked(); applyOptions(); return;
            case ID_ForceSight: {
                if(auto* page=activePage()) {
                    auto state=page->sceneEffects();state.forceSight=event.IsChecked();std::string error;
                    if(!page->setSceneEffects(state,error,false))
                        wxMessageBox(wxui::toWx(error),"Force Sight",wxOK|wxICON_WARNING,this);
                }
                updateState();return;
            }
            case ID_EngineSoftShadows:
                options_.engineSoftShadows = event.IsChecked();
                options_.engineSoftShadowMode=render::EngineSoftShadowMode::DesktopARB;applyOptions();return;
            case ID_EngineSoftShadowsATI:
                options_.engineSoftShadows = event.IsChecked();
                options_.engineSoftShadowMode=render::EngineSoftShadowMode::DesktopATI;applyOptions();return;
            case ID_InspectMeshProjections:
                options_.inspectMeshProjections = event.IsChecked(); applyOptions(); return;
            case ID_NearestLightsOnlyGobs:
                options_.nearestLightsOnlyGobs = event.IsChecked(); applyOptions(); return;
            case ID_PreviewLights:
                options_.previewLights = event.IsChecked(); applyOptions(); return;
            case ID_ParticleSettings: showParticleSettings();return;
            case ID_DetonateEmitters: if(auto* page=activePage()) page->detonateEmitters();return;
            case ID_PreviewEmitters:
                options_.previewEmitters = event.IsChecked(); applyOptions(); return;
            case ID_ShowCameras:
                options_.showCameras = event.IsChecked(); applyOptions(); return;
            case ID_ViewSelectedCamera:
                if (auto* page = activePage();
                    !page || !page->viewThroughSelectedCamera()) {
                    wxMessageBox(
                        "Select a KOTOR II camera node first.",
                        "Authored Camera", wxOK | wxICON_INFORMATION, this);
                }
                updateState();
                return;
            case ID_ViewCameraHook:
                if (auto* page = activePage();
                    !page || !page->viewThroughCameraHook()) {
                    wxMessageBox(
                        "This KOTOR II model has no part named \"camerahook\".",
                        "Authored Camera", wxOK | wxICON_INFORMATION, this);
                }
                updateState();
                return;
            case ID_ViewAreaCamera: showAreaCameraChooser(); return;
            case ID_OrbitCamera:
                if (auto* page = activePage()) page->useOrbitCamera();
                updateState();
                return;
            case ID_CameraSettings: showCameraSettings(); return;
            case ID_ShowReferences:
                options_.showReferences = event.IsChecked(); applyOptions(); return;
            case ID_PreviewLightsabers:
                options_.previewLightsabers = event.IsChecked(); applyOptions(); return;
            case ID_LightsaberSettings: showLightsaberSettings(); return;
            case ID_MotionBlurOwners: showMotionBlurOwners(); return;
            case ID_DirtyMaterial: showDirtyMaterial(); return;
            case ID_DirtyArea: showDirtyArea(); return;
            case ID_EngineExecution: showEngineExecution(); return;
            case ID_EngineExecutionReport: saveEngineExecutionReport(); return;
            case ID_K2SkinSettings: showK2SkinSettings(); return;
            case ID_OwnerVisualEffects: showOwnerVisualEffects(); return;
            case ID_SceneEffects: showSceneEffects(); return;
            case ID_VideoEffectRow: showVideoEffectRow(); return;
            case ID_RuntimeTextureRotation: showRuntimeTextureRotation(); return;
            case ID_PreviewAabbs:
                options_.previewAabbs = event.IsChecked(); applyOptions(); return;
            case ID_PreviewTriggers:
                options_.previewTriggers = event.IsChecked(); applyOptions(); return;
            case ID_TriggerVisualState: showTriggerVisualState(); return;
            case ID_TriggerMineRuntime: showTriggerMineRuntime(); return;
            case ID_CreatureAppearance: showCreatureAppearance(); return;
            case ID_CreatureLipSync: showCreatureLipSync(); return;
            case ID_DialoguePlayback: showDialoguePlayback(); return;
            case ID_About:
                wxMessageBox("NeoMDL renders packed Odyssey MDL/MDX geometry and resolves model dependencies through same-directory overrides, the active host resource session, or a validated discovered KotOR installation.\n\nCurrent renderer: engine K1/K2 materials and compact-palette skinning; K1/K2 desktop animation scheduling, independent reference clocks, particle simulation and nested chunk emitters; engine dangly, lighting, shadows and lightsaber paths. K1/K2 desktop area queries, triggers, weather, facial playback, dialogue and cameras use verified engine behavior. The renderer uses the desktop desktop-derived behavior on all supported host platforms.",
                             "About NeoMDL", wxOK | wxICON_INFORMATION, this);
                return;
            case ID_Exit: requestModuleClose(); return;
            default: event.Skip(); return;
            }
        });
        Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& event) {
            updateState();
            event.Skip();
        });
    }

    void updateState() {
        const bool hasDocument = activePage() != nullptr;
        enableModuleCommand(ID_ExportAscii,activePage()&&activePage()->document().hasModelOwner());
        enableModuleCommand(ID_ExportPacked,activePage()&&activePage()->document().hasModelOwner()&&
            activePage()->document().model().asciiBuilt);
        enableModuleCommand(ID_ResourceProvenance,hasDocument);
        enableModuleCommand(ID_SceneShadows,hasDocument);
        enableModuleCommand(ID_ShadowCasters,hasDocument);
        enableModuleCommand(ID_AreaRoom,activePage()&&!activePage()->areaScene().empty());
        enableModuleCommand(ID_AreaEnvironment,activePage()&&!activePage()->areaScene().empty());
        enableModuleCommand(ID_WorldPlayback,hasDocument);
        enableModuleCommand(ID_SceneAudio,activePage()&&!activePage()->areaScene().empty());
        enableModuleCommand(ID_RoomScriptAnimation,activePage()&&!activePage()->areaScene().rooms().empty());
        enableModuleCommand(ID_TrackAnimation,activePage()&&std::any_of(activePage()->areaScene().objects().begin(),
            activePage()->areaScene().objects().end(),[](const auto& object){return object->kind()==AreaObjectKind::Track;}));
        enableModuleCommand(ID_AreaWeather,activePage()&&!activePage()->areaScene().empty()&&
            weatherRuntimeSupported(activePage()->areaScene().game()));
        enableModuleCommand(ID_CloseModel, hasDocument);
        enableModuleCommand(ID_CloseAll, tabs_->GetPageCount() != 0u);
        enableModuleCommand(ID_FrameModel, hasDocument);
        enableModuleCommand(ID_DetonateEmitters, hasDocument && options_.previewEmitters);
        enableModuleCommand(ID_LightsaberSettings, hasDocument);
        const auto* active = activePage();
        const bool grassArea=active&&!active->areaScene().empty()&&
            engineAreaRuntimeSupported(active->areaScene().game());
        enableModuleCommand(ID_LoadGrass,grassArea);
        enableModuleCommand(ID_GrassOptions,grassArea);
        const bool engineScene=active &&
            !model::engineSceneStageOrder(active->document().model().game).empty();
        enableModuleCommand(ID_DoorState,active&&std::any_of(active->areaScene().objects().begin(),
            active->areaScene().objects().end(),[](const auto& object){return object->kind()==AreaObjectKind::Door;}));
        const bool hasLoadedCreature=active&&
            std::any_of(active->areaScene().placements().begin(),active->areaScene().placements().end(),
                [](const auto& placement){return placement.creatureState&&placement.runtimeInstance;});
        enableModuleCommand(ID_CreatureAppearance,hasLoadedCreature);
        enableModuleCommand(ID_DialoguePlayback,engineScene);
        enableModuleCommand(ID_CreatureLipSync,engineScene&&
            (active->document().hasModelOwner()||hasLoadedCreature));
        enableModuleCommand(ID_RuntimeTextureRotation,engineScene);
        enableModuleCommand(ID_SceneEffects,engineScene);
        enableModuleCommand(ID_ForceSight,active&&active->document().model().game==model::GameVersion::Kotor2);
        enableModuleCommand(ID_VideoEffectRow,active!=nullptr);
        enableModuleCommand(ID_MotionBlurOwners,engineScene);
        enableModuleCommand(ID_DirtyMaterial,active&&active->document().model().game==model::GameVersion::Kotor2);
        enableModuleCommand(ID_DirtyArea,active&&active->document().model().game==model::GameVersion::Kotor2);
        enableModuleCommand(ID_EngineExecution,engineScene);
        enableModuleCommand(ID_EngineExecutionReport,active!=nullptr);
        enableModuleCommand(ID_OwnerVisualEffects,engineScene);
        enableModuleCommand(ID_K2SkinSettings,engineScene);
        enableModuleCommand(ID_ViewSelectedCamera,
                            active && active->canViewThroughSelectedCamera());
        enableModuleCommand(ID_ViewCameraHook, active && active->hasCameraHook());
        enableModuleCommand(ID_ViewAreaCamera,active&&!active->cameraScene().areaCameras().empty());
        enableModuleCommand(ID_TriggerVisualState,active&&std::any_of(active->triggerScene().records().begin(),active->triggerScene().records().end(),[](const auto& record){return record.source==RuntimeTriggerSource::AreaGit;}));
        enableModuleCommand(ID_TriggerMineRuntime,active&&!active->areaScene().empty());
        enableModuleCommand(ID_OrbitCamera, active && active->authoredCameraActive());
        enableModuleCommand(ID_CameraSettings,active&&active->cameraScene().engineRuntime());
        updateMenuChecks();
        if (const auto* page = active) {
            const auto& document = page->document();
            setModuleStatusText(neomdl::model::modelSummary(document.model()), 0);
            std::ostringstream right;
            right << document.loadedTextureCount() << " texture(s)";
            if (document.supermodelCount() != 0u)
                right << " | " << document.supermodelCount() << " supermodel(s)";
            if (!document.references().empty())
                right << " | " << document.references().size() << " reference(s)";
            if (!document.messages().empty()) right << " | " << document.messages().size() << " diagnostic(s)";
            setModuleStatusText(right.str(), 1);
            setModuleTitle(wxString("NeoMDL - ") + wxui::toWx(document.title()));
        } else {
            setModuleStatusText("Open an MDL model to begin.", 0);
            setModuleStatusText("Read-only renderer", 1);
            setModuleTitle("NeoMDL");
        }
    }

    wxAuiNotebook* tabs_{};
    RenderOptions options_;
    neomdl::model::FogMode lastFogMode_{neomdl::model::FogMode::Linear};
    bool dark_{};
    wxMenu* fileMenu_{};
    wxMenu* viewMenu_{};
    wxMenu* helpMenu_{};
    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;
    std::map<std::string, std::shared_ptr<GameResourceSession>> installationSessions_;
    bool jobRunning_{};
    std::atomic<bool> cancelJob_{false};
};

} // namespace

ModelEditorPanel* createModelEditorPanel(wxWindow* parent, neomodules::Context context) {
    return new ModelEditorPanelImpl(parent, std::move(context));
}

} // namespace neomdl::ui
