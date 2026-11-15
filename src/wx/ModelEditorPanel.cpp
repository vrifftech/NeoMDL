#include <model/AsciiModel.hpp>
#include <model/AsciiExport.hpp>
#include <model/PackedExport.hpp>
#include "wx/ModelEditorPanel.hpp"

#include "wx/ModelCanvas.hpp"
#include "wx/AsciiEditor.hpp"
#include "wx/BatchConversion.hpp"
#include "wx/BinaryTools.hpp"
#include "wx/ConversionSettings.hpp"
#include "core/GameResourceSession.hpp"
#include "core/ResourceContext.hpp"
#include "core/BinaryEditing.hpp"
#include <model/SceneOrdering.hpp>

#include <neoshared/PathUtf8.hpp>
#include <neoshared/ResourceDocument.hpp>
#include <neoshared/texture/Error.hpp>
#include <neoshared/texture/FileUtil.hpp>
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoGameInstallResolver.hpp"

#include <wx/aui/auibook.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/collpane.h>
#include <wx/checklst.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/clrpicker.h>
#include <wx/spinctrl.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/filedlgcustomize.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
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
    ID_Reload,
    ID_ModelResources,
    ID_SaveAscii,
    ID_SaveAsciiAs,
    ID_AsciiEditor,
    ID_ExportAscii,
    ID_ExportPacked,
    ID_BatchDecompile,
    ID_BatchCompile,
    ID_QuickConvert,
    ID_ConversionNaming,
    ID_NodeProperties,
    ID_NodeTransform,
    ID_VertexProperties,
    ID_ReplaceMesh,
    ID_ConvertMesh,
    ID_RecalculateNormals,
    ID_BinaryMeshFields,
    ID_InspectBinary,
    ID_CompareBinaries,
    ID_AnalyzeBinaries,
    ID_ControllerProperties,
    ID_CollisionCompanions,
    ID_ModelProperties,
    ID_RenameTexture,
    ID_ExportTextures,
    ID_AdvancedSettings,
    ID_DeveloperSettings,
    ID_EngineGraphics,
    ID_EngineReport,
    ID_AreaReplay,
    ID_ResetView,
    ID_ResetDisplay,
    ID_LoadArea,
    ID_ModuleArea,
    ID_ResourceProvenance,
    ID_ClearArea,
    ID_AreaRoom,
    ID_AreaAnimation,
    ID_RoomScriptAnimation,
    ID_DoorState,
    ID_AreaWeather,
    ID_SceneAudio,
    ID_AreaWeatherInputs,
    ID_SceneAudioInputs,
    ID_LoadGrass,
    ID_CloseModel,
    ID_CloseAll,
    ID_FrameModel,
    ID_Textures,
    ID_LoadTextureOverride,
    ID_ClearTextureOverride,
    ID_Lightmaps,
    ID_Wireframe,
    ID_Lighting,
    ID_Headlight,
    ID_Fog,
    ID_LightingSettings,
    ID_OwnerVisualEffects,
    ID_SceneEffects,
    ID_Grid,
    ID_Axes,
    ID_ShowHidden,
    ID_IsolateMeshes,
    ID_HideMeshes,
    ID_ShowAllMeshes,
    ID_CullBackfaces,
    ID_DanglySimulation,
    ID_NodeHelpers,
    ID_PreviewLights,
    ID_EngineShadows,
    ID_PreviewEmitters,
    ID_ShowCameras,
    ID_ViewSelectedCamera,
    ID_ViewCameraHook,
    ID_ViewAreaCamera,
    ID_OrbitCamera,
    ID_FreeFlyCamera,
    ID_CameraControls,
    ID_CameraSettings,
    ID_ShowReferences,
    ID_PreviewLightsabers,
    ID_PreviewAabbs,
    ID_PreviewTriggers,
    ID_About,
    ID_Exit,
    ID_DetonateEmitters,
    ID_Undo,
    ID_Redo,
    ID_Find,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_Workflow,
    ID_ClearRecent,
    ID_RecentFirst,
    ID_RecentLast = ID_RecentFirst + neosettings::kMaxRecentFiles - 1,
};

enum PreviewFeature : unsigned {
    PreviewArea = 1u,
    PreviewParticles = 2u,
    PreviewLightsabers = 4u,
    PreviewDangly = 8u,
    PreviewCameras = 16u,
    PreviewReferences = 32u,
    PreviewLights = 64u,
};

neoshared::texture::TextureData readPreviewTexture(const std::filesystem::path& path) {
    using namespace neoshared::texture;
    const auto kind = kindForExtension(path);
    if (kind != TextureFileKind::Png && kind != TextureFileKind::Jpeg && kind != TextureFileKind::Bmp) {
        auto texture = readTextureFile(path);
        if (texture.cubeMap || !texture.hasPixels())
            throw TextureError("Choose a 2D image for the model's texture override.");
        return texture;
    }
    wxImage image;
    if (!image.LoadFile(neosettings::pathToWx(path)))
        throw TextureError("Could not decode the selected image.");
    TextureData texture;
    texture.hasAlpha = image.HasAlpha() || image.HasMask();
    if (!image.HasAlpha()) image.InitAlpha();
    texture.kind = kind;
    texture.sourcePath = path;
    texture.sourceEncoding = "raw RGBA";
    texture.canvasWidth = static_cast<std::uint32_t>(image.GetWidth());
    texture.canvasHeight = static_cast<std::uint32_t>(image.GetHeight());
    TextureLayer layer;
    layer.width = texture.canvasWidth;
    layer.height = texture.canvasHeight;
    const auto pixels = static_cast<std::size_t>(layer.width) * layer.height;
    layer.rgba.resize(pixels * 4u);
    const auto* rgb = image.GetData();
    const auto* alpha = image.GetAlpha();
    for (std::size_t i = 0; i < pixels; ++i) {
        std::copy_n(rgb + i * 3u, 3u, layer.rgba.data() + i * 4u);
        layer.rgba[i * 4u + 3u] = alpha[i];
    }
    texture.layers.push_back(std::move(layer));
    return texture;
}

class AsciiExportControls final : public wxFileDialogCustomizeHook {
public:
    AsciiExportControls(int mode, bool advancedFormat) : normalMode(mode), advanced(advancedFormat) {}
    void AddCustomControls(wxFileDialogCustomize& customizer) override {
        customizer.AddStaticText("ASCII format");
        const wxString formats[] = {"Standard ASCII", "Advanced ASCII"};
        format_ = customizer.AddChoice(2, formats);
        format_->SetSelection(advanced ? 1 : 0);
        customizer.AddStaticText("Advanced retains stored geometry, skin and controller data.");
        customizer.AddStaticText("ASCII mesh normals");
        const wxString modes[] = {"Automatic", "Smoothing groups",
            "Explicit normals (NeoMDL extension)", "Smoothing groups (adjust position rounding)"};
        normals_ = customizer.AddChoice(4, modes);
        normals_->SetSelection(normalMode);
        customizer.AddStaticText("Automatic: preserve the mesh normal modes in the editor text.");
        customizer.AddStaticText("Explicit normals: NeoMDL extension; recalculate after geometry edits.");
        customizer.AddStaticText("Position rounding may adjust coordinates by one float step.");
        geometryOnly_ = customizer.AddCheckBox("Geometry only (omit this model's animations)");
    }
    void TransferDataFromCustomControls() override {
        normalMode = normals_->GetSelection();
        advanced = format_->GetSelection() == 1;
        includeAnimations = !geometryOnly_->GetValue();
    }
    int normalMode{};
    bool includeAnimations{true};
    bool advanced{};
private:
    wxFileDialogChoice* format_{};
    wxFileDialogChoice* normals_{};
    wxFileDialogCheckBox* geometryOnly_{};
};

class PackedExportControls final : public wxFileDialogCustomizeHook {
public:
    explicit PackedExportControls(model::GameVersion target) : game(target) {}
    void AddCustomControls(wxFileDialogCustomize& customizer) override {
        customizer.AddStaticText("Output game");
        const wxString games[] = {"Knights of the Old Republic (K1)", "The Sith Lords (K2)"};
        game_ = customizer.AddChoice(2, games);
        game_->SetSelection(game == model::GameVersion::Kotor1 ? 0 : 1);
        geometryOnly_ = customizer.AddCheckBox("Geometry only (omit this model's animations)");
    }
    void TransferDataFromCustomControls() override {
        game = game_->GetSelection() == 0 ? model::GameVersion::Kotor1 : model::GameVersion::Kotor2;
        includeAnimations = !geometryOnly_->GetValue();
    }
    model::GameVersion game;
    bool includeAnimations{true};
private:
    wxFileDialogChoice* game_{};
    wxFileDialogCheckBox* geometryOnly_{};
};

class NodeItemData final : public wxTreeItemData {
public:
    explicit NodeItemData(std::size_t value) : index(value) {}
    std::size_t index{};
};

class ModelPage final : public wxPanel {
public:
    ModelPage(wxWindow* parent, std::unique_ptr<ModelDocument> document,
              const RenderOptions& options, bool compact, std::function<void()> changed = [] {})
        : wxPanel(parent), document_(std::move(document)), changed_(std::move(changed)) {
        outputGame_ = document_->sourceModel().game;
        timer_.SetOwner(this);

        auto* preview = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                             wxSP_LIVE_UPDATE | wxSP_3D);
        auto* playback = new wxScrolledWindow(preview);
        advancedAnimation_ = new wxCollapsiblePane(playback, wxID_ANY, "Advanced animation",
            wxDefaultPosition, wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
        auto* advanced = advancedAnimation_->GetPane();
        animationChoice_ = new wxChoice(playback, wxID_ANY, wxDefaultPosition,
                                        wxSize(FromDIP(190), wxDefaultCoord));
        populateAnimations();

        runModeChoice_ = new wxChoice(advanced, wxID_ANY);
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
        immediateCheck_ = new wxCheckBox(advanced, wxID_ANY, "Immediate");
        immediateCheck_->SetValue(false);
        synchronizeCheck_ = new wxCheckBox(advanced, wxID_ANY, "Sync time");
        allowDuplicateCheck_ = new wxCheckBox(advanced, wxID_ANY, "Allow duplicate");
        reportLoopCheck_ = new wxCheckBox(advanced, wxID_ANY, "Report loops");
        startRunButton_ = new wxButton(advanced, wxID_ANY, "Add Animation");
        playButton_ = new wxToggleButton(playback, wxID_ANY, "Play");
        clearRunsButton_ = new wxButton(playback, wxID_ANY, "Stop");

        activeRunChoice_ = new wxChoice(advanced, wxID_ANY, wxDefaultPosition,
                                        wxSize(FromDIP(250), wxDefaultCoord));
        activeRunChoice_->Append("No active runs");
        activeRunChoice_->SetSelection(0);
        fadeRunButton_ = new wxButton(advanced, wxID_ANY, "Fade Out");
        removeRunButton_ = new wxButton(advanced, wxID_ANY, "Remove");
        timeline_ = new wxSlider(playback, wxID_ANY, 0, 0, 1000, wxDefaultPosition,
                                 wxDefaultSize, wxSL_HORIZONTAL);
        timeLabel_ = new wxStaticText(playback, wxID_ANY, "No active run");
        timeLabel_->SetMinSize(wxSize(FromDIP(150), wxDefaultCoord));

        animationChoice_->SetToolTip(
            "Choose an animation to preview. In Advanced animation, Add Animation layers the selected clip.");
        rateSpin_->SetToolTip("Playback multiplier for the selected active run and for newly started animations. Negative values play backwards.");
        loopCheck_->SetToolTip("Loop the selected active run and newly started animations.");
        runModeChoice_->SetToolTip(
            "Engine priority class: base replaces lower base runs, queued starts FIFO, overlay remains above them.");
        immediateCheck_->SetToolTip(
            "Start at full weight instead of fading in over the clip transition time.");
        synchronizeCheck_->SetToolTip(
            "Start at the current time of the next lower-priority animation.");
        allowDuplicateCheck_->SetToolTip(
            "Allow another instance of an animation that is already playing.");
        reportLoopCheck_->SetToolTip(
            "Include animation loop events in detailed diagnostics.");
        playButton_->SetToolTip(
            "Play or pause the preview, including any layered animations.");
        fadeRunButton_->SetToolTip(
            "Fade out the selected animation using its transition time.");
        removeRunButton_->SetToolTip(
            "Remove the selected animation immediately.");
        clearRunsButton_->SetToolTip("Stop all animations and return to the bind pose.");

        auto* clipRow = new wxWrapSizer(wxHORIZONTAL);
        clipRow->Add(new wxStaticText(playback, wxID_ANY, "Animation:"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(5));
        clipRow->Add(animationChoice_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(new wxStaticText(playback, wxID_ANY, "Speed:"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(5));
        clipRow->Add(rateSpin_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(loopCheck_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(playButton_, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        clipRow->Add(clearRunsButton_, 0,
                     wxALIGN_CENTER_VERTICAL | wxBOTTOM, FromDIP(6));

        auto* advancedLayout = new wxBoxSizer(wxVERTICAL);
        auto* flags = new wxWrapSizer(wxHORIZONTAL);
        flags->Add(new wxStaticText(advanced, wxID_ANY, "Priority:"), 0,
                   wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(5));
        for (wxWindow* control : std::initializer_list<wxWindow*>{runModeChoice_, immediateCheck_,
                synchronizeCheck_, allowDuplicateCheck_, reportLoopCheck_, startRunButton_})
            flags->Add(control, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, FromDIP(6));
        advancedLayout->Add(flags, 0, wxEXPAND);
        auto* runRow = new wxWrapSizer(wxHORIZONTAL);
        runRow->Add(new wxStaticText(advanced, wxID_ANY, "Active animation:"), 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        runRow->Add(activeRunChoice_, 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        runRow->Add(fadeRunButton_, 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        runRow->Add(removeRunButton_, 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        advancedLayout->Add(runRow, 0, wxEXPAND);
        advanced->SetSizer(advancedLayout);
        auto* timelineRow = new wxBoxSizer(wxHORIZONTAL);
        timelineRow->Add(timeline_, 1,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        timelineRow->Add(timeLabel_, 0, wxALIGN_CENTER_VERTICAL);

        auto* playbackLayout = new wxBoxSizer(wxVERTICAL);
        playbackLayout->Add(clipRow, 0, wxEXPAND | wxALL, FromDIP(5));
        playbackLayout->Add(timelineRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
        playbackLayout->Add(advancedAnimation_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
        playback->SetSizer(playbackLayout);
        playback->SetScrollRate(FromDIP(10), FromDIP(10));

        splitter_ = new wxSplitterWindow(preview, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                         wxSP_LIVE_UPDATE | wxSP_3D);
        auto* inspector = new wxSplitterWindow(splitter_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                               wxSP_LIVE_UPDATE | wxSP_3D);
        tree_ = new wxTreeCtrl(inspector, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_MULTIPLE |
                                   wxTR_HIDE_ROOT);
        tree_->SetToolTip("Select a node to edit it. Ctrl-click adds or removes nodes; Shift-click selects a range.");
        auto* detailsPanel = new wxPanel(inspector);
        auto* detailsLayout = new wxBoxSizer(wxVERTICAL);
        detailedDiagnostics_ = new wxCheckBox(detailsPanel, wxID_ANY, "Detailed diagnostics");
        editNode_ = new wxButton(detailsPanel, wxID_ANY, "Edit Selected Node...");
        editNode_->SetToolTip("Edit the selected node's properties.");
        editNode_->Enable(false);
        editVertices_ = new wxButton(detailsPanel, wxID_ANY, "Vertices...",wxDefaultPosition,wxDefaultSize,wxBU_EXACTFIT);
        editControllers_ = new wxButton(detailsPanel, wxID_ANY, "Controllers...",wxDefaultPosition,wxDefaultSize,wxBU_EXACTFIT);
        effects_ = new wxButton(detailsPanel,wxID_ANY,"Preview Effects...",wxDefaultPosition,wxDefaultSize,wxBU_EXACTFIT);
        effects_->SetToolTip("Preview owner effects such as holograms. These settings are not saved into the MDL.");
        effects_->Enable(false);
        editVertices_->Enable(false);
        editControllers_->Enable(false);
        auto* dataProperties = new wxWrapSizer(wxHORIZONTAL);
        dataProperties->Add(editVertices_, 0, wxRIGHT|wxBOTTOM, FromDIP(5));
        dataProperties->Add(editControllers_, 0,wxRIGHT|wxBOTTOM,FromDIP(5));
        dataProperties->Add(effects_,0,wxBOTTOM,FromDIP(5));
        details_ = new wxTextCtrl(detailsPanel, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        detailsLayout->Add(detailedDiagnostics_, 0, wxALL, FromDIP(5));
        detailsLayout->Add(editNode_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
        detailsLayout->Add(dataProperties, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
        detailsLayout->Add(details_, 1, wxEXPAND);
        detailsPanel->SetSizer(detailsLayout);
        inspector->SetMinimumPaneSize(FromDIP(80));
        inspector->SplitHorizontally(tree_, detailsPanel);
        inspector->SetSashGravity(0.6);

        sourceSplit_ = new wxSplitterWindow(splitter_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                             wxSP_LIVE_UPDATE | wxSP_3D);
        canvas_ = new ModelCanvas(sourceSplit_);
        canvas_->SetToolTip("Click a mesh to select it; Shift-click adds or removes meshes.\n"
            "View > Inspection contains temporary Hide and Isolate controls.");
        canvas_->setDocument(document_.get());
        canvas_->setOptions(options);
        sourceSplit_->SetMinimumPaneSize(FromDIP(160));
        sourceSplit_->SetSashGravity(0.5);
        if (document_->hasModelOwner()) {
            asciiEditor_ = new AsciiEditor(sourceSplit_, document_,
                [this](std::unique_ptr<ModelDocument> document) { applyAsciiPreview(std::move(document)); },
                [this] { updateEditingControls(); changed_(); });
            sourceSplit_->SplitVertically(canvas_, asciiEditor_);
        } else sourceSplit_->Initialize(canvas_);
        splitter_->SetMinimumPaneSize(FromDIP(160));
        splitter_->SplitVertically(inspector, sourceSplit_);
        splitter_->SetSashGravity(0.0);
        preview->SetMinimumPaneSize(FromDIP(60));
        preview->SplitHorizontally(playback, splitter_);
        preview->SetSashGravity(0.0);
        const auto rememberPane = [this](wxSplitterWindow* pane, const char* key) {
            pane->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this, key](wxSplitterEvent& event) {
                neosettings::AppSettings("NeoMDL").writeDouble(key, ToDIP(event.GetSashPosition()));
                event.StopPropagation();
                event.Skip();
            });
        };
        rememberPane(preview, "Layout/PlaybackHeight");
        rememberPane(splitter_, "Layout/InspectorWidth");
        rememberPane(inspector, "Layout/TreeHeight");
        rememberPane(sourceSplit_, "Layout/ModelPreviewWidth");
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(preview, 1, wxEXPAND);
        SetSizer(layout);
        // Notebook insertion gives the page its real size. Restore splitters
        // afterwards so the placeholder size cannot shift or clamp the panes.
        CallAfter([this, preview, playback, inspector, compact] {
            const neosettings::AppSettings settings("NeoMDL");
            Layout();
            playback->Layout();
            playback->InvalidateBestSize();
            preview->SetSashPosition(FromDIP(static_cast<int>(settings.readDouble(
                "Layout/PlaybackHeight", ToDIP(playback->GetSizer()->GetMinSize().y)))));
            splitter_->SetSashPosition(FromDIP(static_cast<int>(settings.readDouble(
                "Layout/InspectorWidth", compact ? 220 : 300))));
            inspector->SetSashPosition(FromDIP(static_cast<int>(settings.readDouble(
                "Layout/TreeHeight", compact ? 220 : 320))));
            sourceSplit_->SetSashPosition(FromDIP(static_cast<int>(settings.readDouble(
                "Layout/ModelPreviewWidth", compact ? 260 : 420))));
            playback->FitInside();
            playback->Scroll(0, 0);
        });

        updatePreviewFeatures();
        populateTree();
        updateDetails(std::nullopt);
        updateAnimationControls();
        updateEditingControls();
        detailedDiagnostics_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            updateDetails(selectedNode_);
        });
        editNode_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxCommandEvent command(wxEVT_MENU,ID_NodeProperties);
            ProcessWindowEvent(command);
        });
        editVertices_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxCommandEvent command(wxEVT_MENU,ID_VertexProperties);ProcessWindowEvent(command);
        });
        editControllers_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxCommandEvent command(wxEVT_MENU,ID_ControllerProperties);ProcessWindowEvent(command);
        });
        effects_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
            wxCommandEvent command(wxEVT_MENU,ID_OwnerVisualEffects);ProcessWindowEvent(command);
        });
        advancedAnimation_->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
            [this, playback, preview](wxCollapsiblePaneEvent&) {
                playback->FitInside();
                preview->SetSashPosition(playback->GetSizer()->GetMinSize().y);
                updateAnimationControls();
            });
        tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent& event) {
            updateSelection(event.GetItem());
            event.Skip();
        });
        canvas_->Bind(EVT_MODEL_NODE_PICKED, [this](wxCommandEvent& event) {
            if (!event.GetExtraLong()) tree_->UnselectAll();
            if (event.GetInt()>=0) {
                const auto item=nodeItems_[event.GetInt()];
                tree_->SelectItem(item,!event.GetExtraLong() || !tree_->IsSelected(item));
                tree_->EnsureVisible(item);
                updateSelection(item);
            } else if (!event.GetExtraLong()) updateSelection({});
        });
        animationChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            if (advancedAnimation_->IsCollapsed()) previewSelectedAnimation();
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

    ~ModelPage() override {
        timer_.Stop();
        if (asciiEditor_) asciiEditor_->cancelAndWait();
        // The canvas borrows document_, so destroy it before the document.
        DestroyChildren();
    }

    void suspendLoadingPlayback() {
        ++loadingSuspensions_;
        timer_.Stop();
        if (asciiEditor_) asciiEditor_->setPreviewEnabled(false);
        canvas_->setLoading(true);
    }
    void resumeLoadingPlayback() {
        if (--loadingSuspensions_ != 0u) return;
        canvas_->setLoading(false);
        if (asciiEditor_) asciiEditor_->setPreviewEnabled(canvas_->areaScene().empty());
        lastTick_ = std::chrono::steady_clock::now();
        timer_.Start(16);
    }

    const ModelDocument& document() const noexcept { return *document_; }
    const RenderOptions& options() const noexcept { return canvas_->options(); }
    unsigned previewFeatures() const noexcept { return previewFeatures_; }
    ModelDocument& document() noexcept { return *document_; }
    void setTextureOverride(std::shared_ptr<const neoshared::texture::TextureData> texture) {
        document_->setTextureOverride(std::move(texture));
        canvas_->refreshMaterials();
        updateDetails(selectedNode_);
    }
    AsciiEditor* asciiEditor() const noexcept { return asciiEditor_; }
    model::GameVersion outputGame() const noexcept { return outputGame_; }
    void setOutputGame(model::GameVersion game) noexcept { outputGame_ = game; }
    bool selectedNameIsUnique() const {
        const auto& name = document_->model().nodes[*selectedNode_].name;
        return std::count_if(document_->sourceModel().nodes.begin(), document_->sourceModel().nodes.end(),
            [&](const auto& node) { return wxui::toWx(node.name).CmpNoCase(wxui::toWx(name)) == 0; }) == 1;
    }
    bool canEditNode() const {
        if (!asciiEditor_ || !asciiEditor_->previewCurrent() || !selectedNode_ || !canvas_->areaScene().empty()) return false;
        return (asciiEditor_->advanced() && (!document_->sourceModel().asciiBuilt ||
            document_->model().nodes[*selectedNode_].asciiIdentity)) || selectedNameIsUnique();
    }
    bool canEditBinaryNode() const {
        return selectedNode_ && !document_->sourceModel().asciiBuilt && !document_->path().empty() &&
            canvas_->areaScene().empty() && !asciiEditor_->modified();
    }
    bool canEditBinaryMesh() const {
        return canEditBinaryNode() && document_->model().nodes[*selectedNode_].mesh;
    }
    bool canEditBinaryVertices() const {
        return canEditBinaryMesh() && !document_->model().meshes[*document_->model().nodes[*selectedNode_].mesh].vertices.empty() &&
            (document_->model().meshes[*document_->model().nodes[*selectedNode_].mesh].mdxBitmap&
                (document_->model().meshes[*document_->model().nodes[*selectedNode_].mesh].animated ? 0x7eu : 0x7fu));
    }
    bool canEditBinaryControllers() const {
        return canEditBinaryMesh();
    }
    std::size_t selectedPackedOffset() const {
        return *document_->model().nodes[*selectedNode_].packedOffset;
    }
    std::set<std::size_t> selectedPackedOffsets(bool meshesOnly=false) const {
        std::set<std::size_t> offsets;
        for (const auto index : selectedNodes_) {
            const auto& node=document_->model().nodes[index];
            if (!meshesOnly || node.mesh) offsets.insert(*node.packedOffset);
        }
        return offsets;
    }
    void selectPackedNodes(const std::set<std::size_t>& offsets,std::optional<std::size_t> primary) {
        tree_->UnselectAll();
        for (std::size_t i=0;i<document_->model().nodes.size();++i)
            if (offsets.count(*document_->model().nodes[i].packedOffset) && document_->model().nodes[i].packedOffset!=primary)
                tree_->SelectItem(nodeItems_[i]);
        if (primary) selectPackedNode(*primary);
    }
    void selectPackedNode(std::size_t offset) {
        for (std::size_t i=0;i<document_->model().nodes.size();++i)
            if (document_->model().nodes[i].packedOffset==offset) {
                tree_->SelectItem(nodeItems_[i]);
                tree_->EnsureVisible(nodeItems_[i]);
                updateSelection(nodeItems_[i]);
                break;
            }
    }
    bool hasSelectedMeshes() const {
        return std::any_of(selectedNodes_.begin(),selectedNodes_.end(),
            [&](auto index) {return document_->model().nodes[index].mesh || document_->model().nodes[index].reference;});
    }
    void hideSelectedMeshes() {canvas_->hideSelectedMeshes();}
    void isolateSelectedMeshes(bool enabled) {canvas_->isolateSelectedMeshes(enabled);}
    void showAllMeshes() {canvas_->showAllMeshes();}
    bool meshesIsolated() const {return canvas_->meshesIsolated();}
    void copyEditingContext(const ModelPage& source) {
        showAscii(source.asciiVisible());
        detailedDiagnostics_->SetValue(source.detailedDiagnostics_->GetValue());
        outputGame_=source.document_->sourceModel().game==document_->sourceModel().game
            ? source.outputGame_ : document_->sourceModel().game;
        canvas_->copyCameraFrom(*source.canvas_);
        canvas_->setMeshVisibility(source.canvas_->hiddenMeshNodes(),source.canvas_->isolatedMeshNodes());
        updateDetails(selectedNode_);
    }
    void editSelectedNode() {
        const auto sourceIndex = document_->model().partSourceNodes[*selectedNode_];
        asciiEditor_->editNode(document_->sourceModel(), sourceIndex);
    }
    bool canEditVertices() const {
        return canEditNode() && document_->sourceModel().nodes[
            document_->model().partSourceNodes[*selectedNode_]].mesh.has_value();
    }
    bool canReplaceMesh() const { return canEditVertices() && selectedNameIsUnique(); }
    void editSelectedVertices() {
        const auto sourceIndex = document_->model().partSourceNodes[*selectedNode_];
        asciiEditor_->editVertices(document_->sourceModel(), sourceIndex);
    }
    void replaceSelectedMesh(const AuthoringJob& runJob) {
        const auto sourceIndex = document_->model().partSourceNodes[*selectedNode_];
        asciiEditor_->editMeshReplacements(document_->sourceModel(), sourceIndex, runJob);
    }
    bool canConvertMesh() const {
        if (!canEditVertices()) return false;
        const auto& asset = document_->sourceModel();
        const auto& node = asset.nodes[document_->model().partSourceNodes[*selectedNode_]];
        return asset.meshes[*node.mesh].skinned || node.lightsaber.has_value();
    }
    void convertSelectedMesh(const AuthoringJob& runJob) {
        asciiEditor_->convertMesh(document_->sourceModel(),document_->model().partSourceNodes[*selectedNode_],runJob);
    }
    bool canRecalculateNormals() const {
        if (!canEditVertices()) return false;
        const auto& asset = document_->sourceModel();
        const auto& node = asset.nodes[document_->model().partSourceNodes[*selectedNode_]];
        return !node.lightsaber && !asset.meshes[*node.mesh].animated;
    }
    void recalculateSelectedNormals(const AuthoringJob& runJob) {
        asciiEditor_->recalculateNormals(document_->sourceModel(),document_->model().partSourceNodes[*selectedNode_],runJob);
    }
    void editSelectedControllers() {
        const auto sourceIndex = document_->model().partSourceNodes[*selectedNode_];
        asciiEditor_->editControllers(document_->sourceModel(), sourceIndex);
    }
    bool asciiVisible() const noexcept { return asciiVisible_ && canvas_->areaScene().empty(); }
    void showAscii(bool visible) { asciiVisible_ = visible; updateAsciiVisibility(); }
    void setResourceSession(std::shared_ptr<GameResourceSession> session) {
        resourceSession_=std::move(session);
        updateDetails(selectedNode_);
    }
    const char* sourceFormat() const {
        if (!document_->hasModelOwner()) return "Area scene";
        return document_->sourceModel().asciiBuilt ? "ASCII MDL" : "Binary MDL/MDX";
    }
    const std::shared_ptr<GameResourceSession>& resourceSession() const {return resourceSession_;}
    void setFileResources(ResourceResolver resolver) { fileResources_ = std::move(resolver); }
    const ResourceResolver& fileResources() const { return fileResources_; }
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
    bool setSceneEffects(SceneEffectsSettings settings,std::string& error) {
        return canvas_->setSceneEffects(std::move(settings),error);
    }
    void frameModel() { canvas_->frameModel(); canvas_->SetFocus(); }
    AreaScene prepareAreaLoad() { return canvas_->prepareAreaLoad(); }
    void installArea(AreaScene pending,PreparedSceneAudioPlayback sounds={}) {
        canvas_->installArea(std::move(pending),std::move(sounds));
        updatePreviewFeatures();
        updateAsciiVisibility();
    }
    void configureSceneAudio(std::optional<ExoRandom::State> state) {canvas_->configureSceneAudio(state);}

    void stopSceneAudio() {canvas_->stopSceneAudio();}
    void ensureSceneAudioProvider() {canvas_->ensureSceneAudioProvider();}
    std::optional<ExoRandom::State> sceneAudioRandomState() const {return canvas_->sceneAudioRandomState();}
    const std::string& sceneAudioStatus() const {return canvas_->sceneAudioStatus();}
    const ShadowScene& shadowScene() const noexcept {return canvas_->shadowScene();}
    DirtyAreaState dirtyAreaState() const {return canvas_->dirtyAreaState();}
    std::string engineExecutionReport() const {return canvas_->engineExecutionReport();}
    void setSceneShadowInputs(SceneShadowInputs inputs) {canvas_->setSceneShadowInputs(std::move(inputs));}
    void setSceneCasterEnabled(LightOwnerId owner,bool enabled) {canvas_->setSceneCasterEnabled(owner,enabled);}
    void clearArea() { canvas_->clearArea(); updatePreviewFeatures(); updateAsciiVisibility(); }
    const AreaScene& areaScene() const noexcept { return canvas_->areaScene(); }
    void setAreaRoom(std::optional<LightRoomId> id) { canvas_->setAreaRoom(id); }
    void useReplayedAreaRoom() {canvas_->useReplayedAreaRoom();}
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
    void pauseAreaObject(std::size_t instance,bool paused) {canvas_->pauseAreaObject(instance,paused);}
    void resetAreaObject(std::size_t instance) {canvas_->resetAreaObject(instance);}
    bool setDoorOpenState(std::size_t instance,std::uint8_t state,std::int32_t selector,std::string& error) {
        return canvas_->setDoorOpenState(instance,state,selector,error);
    }
    void configureWeather(PreparedWeatherPlayback prepared,WeatherFrameInput input,bool camera,
                          std::optional<std::uint8_t> command,bool viewerPosition) {
        canvas_->configureWeather(std::move(prepared),input,camera,command,viewerPosition);
    }
    const WeatherFrameInput& weatherInputs() const noexcept {return canvas_->weatherInputs();}
    bool weatherUsesViewerCamera() const noexcept {return canvas_->weatherUsesViewerCamera();}
    bool weatherUsesViewerPosition() const noexcept {return canvas_->weatherUsesViewerPosition();}
    float weatherShadowAlpha() const noexcept {return canvas_->weatherShadowAlpha();}
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
    bool freeFlyCameraActive() const noexcept { return canvas_->freeFlyCameraActive(); }
    float freeFlySpeed() const noexcept { return canvas_->freeFlySpeed(); }
    const CameraScene& cameraScene() const noexcept {return canvas_->cameraScene();}
    const TriggerScene& triggerScene() const noexcept {return canvas_->triggerScene();}
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
    void useViewerCamera(bool freeFly) {
        canvas_->useViewerCamera(freeFly);
        canvas_->SetFocus();
        updateDetails(selectedNode_);
    }
    void refreshDetails() { updateDetails(selectedNode_); }
    void setSourceAppearance(bool dark, double fontScale) {
        if (asciiEditor_) asciiEditor_->setAppearance(dark, fontScale);
        canvas_->Refresh(false);
    }

private:
    void updateEditingControls() {
        editNode_->Enable(canEditNode() || canEditBinaryNode());
        editNode_->SetLabel(canEditBinaryNode() ? canEditBinaryMesh() ? "Edit Binary Mesh..." : "Edit Node Transform..." : "Edit Selected Node...");
        editNode_->SetToolTip(canEditBinaryNode() ? "Edit binary model data and save a new copy." : "Edit the selected node in the ASCII source.");
        editVertices_->Enable(document_->sourceModel().asciiBuilt ? canEditVertices() : canEditBinaryVertices());
        editControllers_->Enable(document_->sourceModel().asciiBuilt ? canEditNode() : canEditBinaryControllers());
        effects_->Enable(document_->hasModelOwner() && !model::engineSceneStageOrder(document_->model().game).empty());
        editNode_->GetParent()->Layout();
    }
    void populateAnimations() {
        animationChoice_->Clear();
        animationChoice_->Append("Bind pose");
        for (std::size_t index = 0; index < document_->animations().size(); ++index) {
            auto label = document_->animation(index).name;
            const auto& reference = document_->animationReference(index);
            if (reference.modelIndex != 0u) {
                const auto& owner = document_->animationModel(index);
                label += "  [" + (owner.name.empty() ? "supermodel " + std::to_string(reference.modelIndex) : owner.name) + "]";
            }
            animationChoice_->Append(wxui::toWx(label));
        }
        animationChoice_->SetSelection(0);
    }

    void updateAsciiVisibility() {
        if (!asciiEditor_) return;
        updateEditingControls();
        asciiEditor_->setPreviewEnabled(loadingSuspensions_ == 0u && canvas_->areaScene().empty());
        if (asciiVisible() && !sourceSplit_->IsSplit()) {
            asciiEditor_->Show();
            sourceSplit_->SplitVertically(canvas_, asciiEditor_, FromDIP(static_cast<int>(
                neosettings::AppSettings("NeoMDL").readDouble("Layout/ModelPreviewWidth", 420))));
        } else if (!asciiVisible() && sourceSplit_->IsSplit()) {
            sourceSplit_->Unsplit(asciiEditor_);
        }
    }

    void applyAsciiPreview(std::unique_ptr<ModelDocument> document) {
        if (document_->textureOverride()) document->setTextureOverride(document_->textureOverride());
        const auto selections=selectedNodes_;
        const auto selection=selectedNode_;
        const auto oldHidden=canvas_->hiddenMeshNodes();
        const auto oldIsolated=canvas_->isolatedMeshNodes();
        std::set<std::size_t> hidden;
        std::optional<std::set<std::size_t>> isolated;
        if (oldIsolated) isolated.emplace();
        const bool identified=document->sourceModel().nodes.front().asciiIdentity.has_value();
        const auto animation = selectedAnimation();
        const auto clip = animation ? document_->animation(*animation).name : std::string{};
        const auto* run = selectedRun();
        const float time = run ? run->time : 0.0f;
        const bool resume = playing_;
        auto previous = std::move(document_);
        document_ = std::move(document);
        canvas_->setDocument(document_.get(), true);
        stopPlayback();
        eventLog_.clear();
        selectedNode_.reset();
        selectedNodes_.clear();
        populateTree();
        wxTreeItemId primary;
        for (std::size_t i=0;i<document_->model().nodes.size();++i) {
            const auto& node=document_->model().nodes[i];
            const auto matches=[&](std::size_t old) {
                const auto& original=previous->model().nodes[old];
                return identified ? node.asciiIdentity==original.asciiIdentity.value_or(
                    static_cast<std::uint32_t>(previous->model().partSourceNodes[old])) : node.name==original.name;
            };
            if (std::any_of(selections.begin(),selections.end(),matches)) {
                tree_->SelectItem(nodeItems_[i]);
                if (selection && matches(*selection)) primary=nodeItems_[i];
            }
            if (std::any_of(oldHidden.begin(),oldHidden.end(),matches)) hidden.insert(i);
            if (oldIsolated && std::any_of(oldIsolated->begin(),oldIsolated->end(),matches)) isolated->insert(i);
        }
        canvas_->setMeshVisibility(std::move(hidden),std::move(isolated));
        populateAnimations();
        for (std::size_t index = 0; index < document_->animations().size(); ++index) {
            if (document_->animation(index).name == clip) {
                animationChoice_->SetSelection(static_cast<int>(index + 1u));
                previewSelectedAnimation();
                if (const auto id = selectedRunId()) canvas_->setAnimationRunTime(*id, time, true);
                if (resume) startPlayback();
                break;
            }
        }
        updatePreviewFeatures();
        updateAnimationControls();
        updateSelection(primary);
    }

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

    void updatePreviewFeatures() {
        previewFeatures_ = canvas_->areaScene().empty() ? 0u : PreviewArea;
        const auto scanModel = [this](const model::Model& asset) {
            for (const auto& node : asset.nodes) {
                if (node.emitter) previewFeatures_ |= PreviewParticles;
                if (node.lightsaber) previewFeatures_ |= PreviewLightsabers;
                if (node.camera) previewFeatures_ |= PreviewCameras;
                if (node.light) previewFeatures_ |= PreviewLights;
            }
            for (const auto& mesh : asset.meshes)
                if (mesh.dangly) previewFeatures_ |= PreviewDangly;
        };
        const auto scanDocument = [&](const ModelDocument& document) {
            if (!document.references().empty()) previewFeatures_ |= PreviewReferences;
            const auto scanAsset = [&](const model::Model& asset) {
                scanModel(asset);
                for (const auto& node : asset.nodes)
                    if (node.emitter && !node.emitter->chunkName.empty())
                        if (const auto* chunk = document.chunkModel(node.emitter->chunkName)) {
                            scanModel(chunk->model());
                            for (const auto& reference : chunk->references()) scanModel(reference.model);
                        }
            };
            scanAsset(document.model());
            for (const auto& reference : document.references()) scanAsset(reference.model);
        };
        scanDocument(*document_);
        for (const auto& object : canvas_->areaScene().objects())
            scanDocument(*object->document());
        if (hasCameraHook() || !canvas_->cameraScene().areaCameras().empty()) previewFeatures_ |= PreviewCameras;
    }

    void previewSelectedAnimation() {
        const bool resume = playing_;
        canvas_->clearAnimationRuns();
        stopPlayback();
        eventLog_.clear();
        if (startAnimationRun()) {
            canvas_->frameModel();
            if (resume) startPlayback();
        }
        updateDetails(selectedNode_);
    }

    bool startAnimationRun() {
        const auto animation = selectedAnimation();
        if (!animation) return false;

        const bool advanced = advancedAnimation_->IsExpanded();
        std::uint32_t flags = advanced ? 0u : neomdl::model::kAnimationRunImmediate;
        if (!loopCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunNonLooping;
        if (advanced && immediateCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunImmediate;
        if (advanced && synchronizeCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunSynchronizeTime;
        if (advanced && allowDuplicateCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunAllowDuplicate;
        if (advanced && reportLoopCheck_->GetValue())
            flags |= neomdl::model::kAnimationRunReportLoop;

        const bool engineRuntime = neomdl::model::engineAnimationRuntimeSupported(
            document_->model().game);
        if (engineRuntime) {
            if (advanced && runModeChoice_->GetSelection() == 1)
                flags |= neomdl::model::kAnimationRunQueued;
            else if (advanced && runModeChoice_->GetSelection() == 2)
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
        if (playing_ || eventsChanged) {
            updateAnimationControls();
            if (detailedDiagnostics_->GetValue())
                updateDetailedDiagnostics(selectedNode_);
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
            auto* focus = wxWindow::FindFocus();
            if (focus != rateSpin_ && !rateSpin_->IsDescendant(focus))
                rateSpin_->SetValue(run->playbackRate);
            if (!run->outOfOrder()) loopCheck_->SetValue(run->looping());
            std::ostringstream label;
            label << std::fixed << std::setprecision(3)
                  << time << " / " << length << " s";
            if (advancedAnimation_->IsExpanded())
                label << "  weight " << std::setprecision(2) << run->weight;
            timeLabel_->SetLabel(wxui::toWx(label.str()));
        } else {
            timeline_->SetValue(0);
            timeLabel_->SetLabel("Bind pose");
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
        nodeItems_[index] = item;
        for (const auto child : node.children) appendNode(item, child);
        return item;
    }

    void updateSelection(wxTreeItemId primary) {
        selectedNodes_.clear();selectedNode_.reset();wxArrayTreeItemIds selected;
        tree_->GetSelections(selected);
        for (const auto& item : selected) {
            const auto index=static_cast<NodeItemData*>(tree_->GetItemData(item))->index;
            selectedNodes_.insert(index);
            if (item==primary) selectedNode_=index;
        }
        if (!selectedNode_ && !selectedNodes_.empty()) selectedNode_=*selectedNodes_.begin();
        canvas_->setSelectedNodes(selectedNodes_);
        updateDetails(selectedNode_);updateEditingControls();changed_();
    }

    void populateTree() {
        tree_->DeleteAllItems();
        nodeItems_.resize(document_->model().nodes.size());
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
        if (detailedDiagnostics_->GetValue()) {
            updateDetailedDiagnostics(selected);
        } else {
            std::ostringstream text;
            if (selectedNodes_.size()>1) text<<selectedNodes_.size()<<" nodes selected\n\n";
            if (selected) {
                const auto& node = document_->model().nodes[*selected];
                text << node.name << "\n" << model::nodeFlagsText(node.flags) << "\n\n"
                     << "Position: " << node.position.x << ", " << node.position.y << ", " << node.position.z << "\n";
                if (node.mesh) {
                    const auto& mesh = document_->model().meshes[*node.mesh];
                    text << "Vertices: " << mesh.vertices.size() << "\n"
                         << "Triangles: " << (mesh.animated ? mesh.faces.size() : mesh.indices.size() / 3u) << "\n"
                         << "Texture: " << (mesh.texture0.empty() ? "(none)" : mesh.texture0) << "\n"
                         << "Secondary texture: " << (mesh.texture1.empty() ? "(none)" : mesh.texture1) << "\n"
                         << "Render flag: " << (mesh.render ? "on" : "off") << "\n";
                }
                text << "\n";
            }
            text << model::modelSummary(document_->model()) << "\n"
                 << "Format: " << sourceFormat() << "\n"
                 << "Textures: " << document_->loadedTextureCount() << "\n"
                 << "Animations: " << document_->animations().size() << "\n";
            if (resourceSession_)
                text << "Resources from: " << resourceSession_->installation()->displayName << "\n";
            else if (!document_->path().empty())
                text << "Resources from: model directory\n"
                     << "Choose a game installation in Tools > Model Resource Installation.\n";
            if (document_->hasModelOwner() && !document_->sourceModel().asciiBuilt)
                text << "Generate ASCII in the editor when needed.\n";
            if (const auto animation = selectedAnimation()) {
                const auto& clip = document_->animation(*animation);
                text << "\nSelected animation: " << clip.name << "\nLength: " << clip.length << " s\n"
                     << "Source: " << document_->animationModel(*animation).name << "\n";
            }
            if (!document_->messages().empty())
                text << "\n" << document_->messages().size() << " load note(s); see Detailed diagnostics.\n";
            details_->ChangeValue(wxui::toWx(text.str()));
        }
    }

    void updateDetailedDiagnostics(std::optional<std::size_t> selected) {
        std::ostringstream text;
        text << neomdl::model::modelSummary(document_->model()) << "\n"
             << "Format: " << sourceFormat() << "\n";
        if (resourceSession_)
            text << "Resources from: " << neoshared::pathToUtf8(resourceSession_->installation()->installPath) << "\n";
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
                        document_->material(*node.mesh), surface, inputs);
                } else {
                    rendered=neomdl::material::evaluateMaterial(
                        document_->material(*node.mesh), surface);
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

    std::shared_ptr<ModelDocument> document_;
    std::function<void()> changed_;
    wxSplitterWindow* sourceSplit_{};
    AsciiEditor* asciiEditor_{};
    model::GameVersion outputGame_{};
    bool asciiVisible_{true};
    unsigned previewFeatures_{};
    wxSplitterWindow* splitter_{};
    wxTreeCtrl* tree_{};
    std::vector<wxTreeItemId> nodeItems_;
    wxTextCtrl* details_{};
    wxCheckBox* detailedDiagnostics_{};
    wxButton* editNode_{};
    wxButton* editVertices_{};
    wxButton* editControllers_{};
    wxButton* effects_{};
    ModelCanvas* canvas_{};
    wxCollapsiblePane* advancedAnimation_{};
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
    std::set<std::size_t> selectedNodes_;
    bool playing_{};
    unsigned loadingSuspensions_{};
    std::shared_ptr<GameResourceSession> resourceSession_;
    ResourceResolver fileResources_;
};

class ModelEditorPanelImpl final : public ModelEditorPanel {
public:
    ModelEditorPanelImpl(wxWindow* parent, neomodules::Context context)
        : ModelEditorPanel(parent, std::move(context)) {
        dark_ = settings_.darkMode();
        fontScale_ = settings_.fontScale();
        if(context_.captureModFolder)modFolder_=context_.captureModFolder();
        tabs_ = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB |
                wxAUI_NB_SCROLL_BUTTONS);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(tabs_, 1, wxEXPAND);
        SetSizer(layout);
        createModuleStatusBar(3);
        int widths[] = {-3, -2, FromDIP(220)};
        moduleStatusBar()->SetStatusWidths(3, widths);
        buildMenus();
        bindEvents();
        if (!context_.embedded)
            fontScaleWheelFilter_.attach(wxGetTopLevelParent(this), [this](int steps) { changeFontScale(steps); });
        setAppearance(dark_, fontScale_);
        updateState();
    }

    bool chooseAsciiGame(std::optional<model::GameVersion>& game) {
        wxArrayString choices;
        choices.Add("Knights of the Old Republic");choices.Add("Knights of the Old Republic II");
        wxSingleChoiceDialog choice(this,"Choose the target game for this ASCII model.","Open ASCII MDL",choices);
        if(choice.ShowModal()!=wxID_OK)return false;
        game=choice.GetSelection()==0?model::GameVersion::Kotor1:model::GameVersion::Kotor2;
        return true;
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
            if(!chooseAsciiGame(game))return false;
        }
    }

    bool openFile(const std::filesystem::path& path) override {
        if (jobRunning_) return false;
        try {
            for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index) {
                const auto* page = pageAt(index);
                if (neoshared::sameResourcePath(page->document().path(), path)) {
                    tabs_->SetSelection(index);
                    settings_.addRecentFile(path);
                    return true;
                }
            }
            // Packed headers identify the game. ASCII construction uses the
            // explicitly selected engine rules before resolving dependencies.
            const auto host = captureHostResolver();
            auto document = std::make_unique<ModelDocument>();
            ModelFileSource source;
            std::optional<model::GameVersion> asciiGame;
            if (!runJob("Reading model", [&](const neoshared::bif::JobControl& job) {
                source = readModelFileSource(path, job);source.mod=modFolder_;
                asciiGame = model::modelInputGame(source.resource.bytes, source.resource.sourceDescription, job);
            })) return false;
            if (!asciiGame && !chooseAsciiGame(asciiGame)) return false;

            std::shared_ptr<GameResourceSession> installationSession;
            std::string installationWarning;
            const auto installed = installedResourceResolver(
                *asciiGame, source.path, installationSession,
                installationWarning, true);
            const auto combined = combineResolvers(host, installed);
            if (!runJob("Loading model", [&](const neoshared::bif::JobControl& job) {
                document->openFile(std::move(source), combined, job, asciiGame);
            })) return false;

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

            if (!activateResource(document->identity()))
                addDocument(std::move(document),installationSession,combined);
            settings_.addRecentFile(path);
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
            const auto resolver = captureHostResolver(resource.module.empty()?neoshared::bif::ModFolder::moduleName(resource.sourcePath):resource.module);
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
    void resourceContextChanged() override {
        modFolder_=context_.captureModFolder();installationSessions_.clear();
        setModuleStatusText("Mod folder changed. Reload model resources to update existing previews.",0);
    }
    bool canClose() override {
        if (jobRunning_) { cancelJob_.store(true); return false; }
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index)
            if (!confirmDiscard(pageAt(index))) return false;
        return true;
    }

    void setAppearance(bool dark, double fontScale) override {
        dark_ = dark;
        fontScale_ = fontScale;
        wxui::applyTheme(this, dark_);
        neoview::applyFontScale(this, fontScale_);
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index)
            pageAt(index)->setSourceAppearance(dark_, fontScale_);
        updateMenuChecks();
    }

    void changeFontScale(int steps) {
        fontScale_ = neoview::steppedFontScale(fontScale_, steps);
        settings_.setFontScale(fontScale_);
        setAppearance(dark_, fontScale_);
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
    bool confirmDiscard(ModelPage* page) {
        const auto* editor = page->asciiEditor();
        if (!editor || !editor->modified()) return true;
        tabs_->SetSelection(tabs_->GetPageIndex(page));
        const auto destination = editor->savePath().empty()
            ? "for " + wxui::toWx(page->document().title())
            : "to " + neosettings::pathToWx(editor->savePath());
        wxMessageDialog dialog(this, "Save ASCII edits " + destination + " before continuing?",
            "Unsaved ASCII edits", wxYES_NO | wxCANCEL | wxICON_QUESTION);
        dialog.SetYesNoLabels("Save ASCII", "Discard");
        const auto answer = dialog.ShowModal();
        return answer == wxID_NO || (answer == wxID_YES && page->asciiEditor()->save());
    }

    void editBinaryMesh(BinaryMeshPage initialPage=BinaryMeshPage::Properties) {
        auto* page=activePage();
        const bool selected=page && (initialPage==BinaryMeshPage::Transform ? page->canEditBinaryNode() : page->canEditBinaryMesh());
        const auto offset=selected ? std::optional<std::size_t>{page->selectedPackedOffset()} : std::nullopt;
        const auto offsets=selected ? page->selectedPackedOffsets() : std::set<std::size_t>{};
        const auto saved=editBinaryMeshFields(this,
            [this](const wxString& title,const auto& work) {return runJob(title,work);},
            selected ? page->document().path() : std::filesystem::path{},offset,initialPage,
            selected ? page->selectedPackedOffsets(true) : std::set<std::size_t>{});
        if (!saved) return;
        try {
            auto document=std::make_unique<ModelDocument>();
            const auto resources=selected ? page->document().resourceResolver() : captureHostResolver();
            if (!runJob("Loading edited binary model",[&](const auto& job) {
                auto source=readModelFileSource(saved->path,job);source.mod=modFolder_;
                document->openFile(std::move(source),resources,job);
            })) return;
            const auto session=selected ? page->resourceSession() : std::shared_ptr<GameResourceSession>{};
            addDocument(std::move(document),session,resources,selected ? page : nullptr,selected);
            std::set<std::size_t> selection;
            for (const auto original : offsets) selection.insert(saved->nodeOffsets.at(original));
            activePage()->selectPackedNodes(selection,saved->nodeOffset);
        } catch (const std::exception& error) {
            wxMessageBox("The binary copy was saved to:\n"+neosettings::pathToWx(saved->path)+
                "\n\nIt could not be opened:\n"+wxui::toWx(error.what()),
                "Saved Copy Could Not Be Opened",wxOK|wxICON_ERROR,this);
        }
    }

    void reloadModel(ResourceResolver resources, std::shared_ptr<GameResourceSession> session) {
        auto* page = activePage();
        if (!confirmDiscard(page)) return;
        try {
            auto document = std::make_unique<ModelDocument>();
            std::optional<model::GameVersion> game = page->document().model().game;
            if (!loadModelWithGame("Reloading model", game,
                [&](const neoshared::bif::JobControl& job, std::optional<model::GameVersion> target) {
                    auto source=readModelFileSource(page->document().path(),job);source.mod=modFolder_;
                    document->openFile(std::move(source), resources, job, target);
                })) return;
            addDocument(std::move(document), std::move(session), std::move(resources), page);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    void chooseModelResources() {
        const auto* page = activePage();
        const auto* definition = neogames::findGame(gameIdForModel(page->document().model().game));
        auto installs = neogames::resolver().settings().readSaved(*definition);
        const auto& current = page->resourceSession();
        if (current) neogames::upsertInstall(installs, *definition, *current->installation());
        wxArrayString choices;
        choices.Add("Model directory only");
        int selected = 0;
        for (const auto& install : installs) {
            choices.Add(wxui::toWx(install.displayName + " — " + neoshared::pathToUtf8(install.installPath)));
            if (current && neoshared::sameResourcePath(install.installPath, current->installation()->installPath))
                selected = static_cast<int>(choices.size()) - 1;
        }
        wxSingleChoiceDialog dialog(this,
            "Choose resources for this model. Files beside the model take precedence.\n"
            "Applying reloads the model and resets its preview.\n\n"
            "Manage installations under File > Open Model from Saved Directory.",
            "Model Resource Installation", choices);
        dialog.SetSelection(selected);
        if (dialog.ShowModal() != wxID_OK) return;
        try {
            std::shared_ptr<GameResourceSession> session;
            if (dialog.GetSelection() != 0) {
                session = std::make_shared<GameResourceSession>();
                const auto& install = installs[static_cast<std::size_t>(dialog.GetSelection() - 1)];
                if (!runJob("Indexing model resources", [&](const neoshared::bif::JobControl& job) {
                    std::string error;
                    if (!session->open(install, error, job,modFolder_)) throw std::runtime_error(error);
                })) return;
            }
            reloadModel(session ? session->resolver() : ResourceResolver{}, session);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

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

    ResourceResolver captureHostResolver(const std::string& module={}) const {
        if (context_.captureResourceResolver) {
            auto resolver = !module.empty()&&context_.captureScopedResourceResolver?
                context_.captureScopedResourceResolver(module):context_.captureResourceResolver();
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
        std::string& warning,
        bool containingOnly = false) {
        selectedSession.reset();
        warning.clear();
        const auto gameId = gameIdForModel(modelGame);
        const auto* definition = neogames::findGame(gameId);
        if (definition == nullptr) return {};

        std::optional<neogames::GameInstall> selected;
        if (containingOnly) {
            auto directory = neogames::normalizeConfiguredPath(modelPath).parent_path();
            while (!directory.empty()) {
                selected = neoshared::game::inspectGameInstallation(directory, gameId, "Opened resource");
                if (selected) {
                    for (const auto& saved : neogames::resolver().settings().readSaved(*definition)) {
                        if (neoshared::sameResourcePath(saved.installPath, selected->installPath)) {
                            selected = saved;
                            break;
                        }
                    }
                    break;
                }
                const auto parent = directory.parent_path();
                if (parent == directory) break;
                directory = parent;
            }
            if (!selected) return {};
        } else if (const auto inferred = neogames::resolver().inferFromOpenedPath(modelPath)) {
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
            opened = session->open(*selected, error, job,modFolder_);
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
                    if(!installation->open(selected,error,job,modFolder_))throw std::runtime_error(error);
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
            box->Add(settings.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,10);settings.SetSizerAndFit(box);
            if(settings.ShowModal()!=wxID_OK)return;
            AreaLoadOptions options;options.game=game;options.allowPartialVisuals=partial->GetValue();
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
                wxui::applyTheme(page, dark_);
                neoview::applyFontScale(page, fontScale_);
                page->setSourceAppearance(dark_, fontScale_);
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
            const auto read=[](wxTextCtrl* field) {
                double x=0;
                if(!field->GetValue().ToDouble(&x)||!std::isfinite(x)||!std::isfinite(float(x)))
                    throw std::runtime_error("Shadow inputs must be finite numbers");
                return float(x);
            };
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
            updateState();
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

    void chooseAreaEnvironment() { chooseAreaReplay(false); }

    void chooseAreaReplay(bool developer = true) {
        auto* page=activePage();
        if(!page||page->areaScene().empty())return;
        auto options=page->options();
        const auto& area=page->areaScene();
        const auto& environment=area.environment();
        auto replay=environment.replay();
        wxDialog dialog(this,wxID_ANY,developer ? "Area Camera Replay" : "Area Environment",wxDefaultPosition,
                        wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "Lighting and fog for the current area preview."),
            0,wxEXPAND|wxALL,10);
        auto* useEnvironment=new wxCheckBox(&dialog,wxID_ANY,"Use the area's lighting and fog");
        useEnvironment->SetValue(options.useLoadedAreaEnvironment);root->Add(useEnvironment,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* useCamera=new wxCheckBox(&dialog,wxID_ANY,"Use replay camera when active (authored model camera retains priority)");
        useCamera->SetValue(options.useAreaReplayCamera);root->Add(useCamera,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* followScene=new wxCheckBox(&dialog,wxID_ANY,"Use replay camera position as the shadow fade/scene anchor");
        followScene->SetValue(options.shadowScenePositionFromAreaCamera);root->Add(followScene,0,wxLEFT|wxRIGHT|wxBOTTOM,10);
        auto* followCasters=new wxCheckBox(&dialog,wxID_ANY,"Use replay camera position as the shadow caster-region anchor");
        followCasters->SetValue(options.shadowCasterRegionFromAreaCamera);root->Add(followCasters,0,wxLEFT|wxRIGHT|wxBOTTOM,10);

        root->Show(useCamera,developer);
        root->Show(followScene,developer);
        root->Show(followCasters,developer);
        auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1,1);
        const auto add=[&](const char* label,wxWindow* control) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            grid->Add(control,1,wxEXPAND);
        };
        auto* phase=new wxChoice(&dialog,wxID_ANY);
        for(const auto* label:{"From area","Day","Night","Dawn","Dusk"})phase->Append(label);
        if(!replay.phase)phase->SetSelection(0);
        else phase->SetSelection(*replay.phase==AreaTimeOfDay::Day?1:*replay.phase==AreaTimeOfDay::Night?2:
                                 *replay.phase==AreaTimeOfDay::Dawn?3:4);
        add("Time of day",phase);
        auto* transition=new wxSpinCtrlDouble(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
            wxSP_ARROW_KEYS,0.0,1.0,replay.transition,0.01);transition->SetDigits(3);add("Dawn/dusk progress",transition);
        auto* fogOverride=new wxChoice(&dialog,wxID_ANY);fogOverride->Append("From area");fogOverride->Append("Off");fogOverride->Append("On");
        fogOverride->SetSelection(!replay.fogEnabled?0:*replay.fogEnabled?2:1);add("Fog",fogOverride);
        auto* shadowOverride=new wxChoice(&dialog,wxID_ANY);shadowOverride->Append("From area");shadowOverride->Append("Off");shadowOverride->Append("On");
        shadowOverride->SetSelection(!replay.shadowsEnabled?0:*replay.shadowsEnabled?2:1);add("Shadows",shadowOverride);
        root->Add(grid,0,wxEXPAND|wxLEFT|wxRIGHT,10);
        grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1,1);
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
        auto* clearReplay=new wxCheckBox(&dialog,wxID_ANY,"Clear all explicit environment/camera replay state");add("Reset",clearReplay);
        root->Add(grid,1,wxEXPAND|wxLEFT|wxRIGHT,10);
        root->Show(grid,developer);
        if(developer)root->Add(new wxStaticText(&dialog,wxID_ANY,
            "This dialog edits camera position only. Existing replay orientation is retained; a new camera uses identity orientation.\n"
            "The core API accepts the complete rigid camera transform for captured traces."),0,wxEXPAND|wxALL,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
        dialog.SetSizerAndFit(root);
        while(dialog.ShowModal()==wxID_OK) {
            options.useLoadedAreaEnvironment=useEnvironment->GetValue();
            options.useAreaReplayCamera=useCamera->GetValue();
            options.shadowScenePositionFromAreaCamera=followScene->GetValue();
            options.shadowCasterRegionFromAreaCamera=followCasters->GetValue();
            if(clearReplay->GetValue()) {
                page->clearAreaEnvironmentReplay();
                if(followReplayRoom->GetValue())page->useReplayedAreaRoom();
                applyPreviewOptions(options);break;
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
            state.camera=camera;
            std::string error;
            if(!page->applyAreaEnvironmentReplay(std::move(state),error)) {
                wxMessageBox(wxui::toWx(error),"Area replay not applied",wxOK|wxICON_WARNING,&dialog);continue;
            }
            if(followReplayRoom->GetValue())page->useReplayedAreaRoom();
            applyPreviewOptions(options);break;
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
        if(selected<0)return;
        const auto* object=objects.at(static_cast<std::size_t>(selected));
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
    void showSceneAudio(bool developer = false) {
        auto* page=activePage();if(!page)return;
        wxDialog dialog(this,wxID_ANY,developer ? "Scene Audio Inputs" : "Scene Audio");auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&dialog,wxID_ANY,wxui::toWx(page->sceneAudioStatus())),0,wxALL,10);
        wxArrayString commands;commands.Add("Play area sounds");commands.Add("Enable animation sounds");commands.Add("Stop all scene sounds");
        auto* action=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,commands);action->SetSelection(0);
        box->Add(action,0,wxEXPAND|wxALL,10);
        wxTextCtrl* state = nullptr;
        if (developer) {
            std::string stateText;
            if(const auto captured=page->sceneAudioRandomState())for(auto value:*captured) {
                if(!stateText.empty())stateText+=' ';
                stateText+=std::to_string(value);
            }
            box->Add(new wxStaticText(&dialog,wxID_ANY,
                "CExoRand state: four captured unsigned words, after engine construction.\n"
                "Required for authored interval variation. Leave blank to retain the current state."),0,wxALL,10);
            state=new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(stateText),wxDefaultPosition,wxSize(600,-1));
            box->Add(state,0,wxEXPAND|wxALL,10);
        }
        box->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,10);dialog.SetSizerAndFit(box);
        if(dialog.ShowModal()!=wxID_OK)return;
        try {
            if(action->GetSelection()==2){page->stopSceneAudio();return;}
            std::optional<ExoRandom::State> captured;
            if(developer&&action->GetSelection()==0&&!state->GetValue().empty()) {
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
    void showAreaWeather(bool developer = false) {
        auto* page=activePage();if(!page||page->areaScene().empty())return;
        const bool k1=page->areaScene().game()==model::GameVersion::Kotor1;
        const auto current=page->weatherInputs();
        wxDialog dialog(this,wxID_ANY,developer ? "Area Weather Inputs" : "Area Weather");auto* box=new wxBoxSizer(wxVERTICAL);
        auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1);
        const auto add=[&](const char* label,wxWindow* control) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);grid->Add(control,1,wxEXPAND);
        };
        const auto check=[&](const char* label,bool value) {
            auto* control=new wxCheckBox(&dialog,wxID_ANY,wxEmptyString);control->SetValue(value);add(label,control);return control;
        };
        auto* enabled=check("Preview weather",current.enabled);
        auto* schedule=check("Use area weather schedule",current.serverSchedule);
        const auto number=[&](const char* label,float value) {
            auto* field=new wxTextCtrl(&dialog,wxID_ANY,wxString::Format("%.9g",double(value)));add(label,field);return field;
        };
        wxCheckBox *supplyPlayer = nullptr, *camera = nullptr, *roomDisabled = nullptr;
        wxTextCtrl *x = nullptr, *y = nullptr, *z = nullptr, *shadow = nullptr, *sounds = nullptr;
        wxSpinCtrl* overrideType = nullptr;
        wxChoice* placement = nullptr;
        if (developer) {
            supplyPlayer=check("Supply player position",bool(current.playerPosition));
            const auto position=current.playerPosition.value_or(model::Vec3{});
            x=number("Player world X",position.x);y=number("Player world Y",position.y);z=number("Player world Z",position.z);
            camera=check("Supply viewer camera as module camera",page->weatherUsesViewerCamera());
            roomDisabled=check("Player-room weather-disabled result",current.roomWeatherDisabled);
            roomDisabled->Enable(!k1);
            overrideType=new wxSpinCtrl(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,wxSP_ARROW_KEYS,0,255,current.serverOverride);
            add("Captured server override (255 allows scheduling)",overrideType);
            shadow=number("Engine shadowalpha replay state",page->weatherShadowAlpha());
            sounds=new wxTextCtrl(&dialog,wxID_ANY);add("Thunder resource directory (optional)",sounds);sounds->Enable(!k1);
        } else {
            placement=new wxChoice(&dialog,wxID_ANY);
            placement->Append("Viewport camera");placement->Append("Captured positions (Developer)");
            placement->SetSelection(page->weatherUsesViewerPosition() ? 0 : 1);
            add("Weather placement",placement);
        }
        auto* thunder=check("Play thunder sounds",current.thunderEnabled);thunder->Enable(!k1);
        wxArrayString commands;commands.Add("Keep current transition");commands.Add("Start rain");commands.Add("Start snow");commands.Add("Stop weather");
        auto* command=new wxChoice(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,commands);command->SetSelection(0);add("Weather",command);
        if (!developer) command->Bind(wxEVT_CHOICE,[command,schedule,enabled](wxCommandEvent&) {
            if (command->GetSelection() != 0) schedule->SetValue(false);
            if (command->GetSelection() == 1 || command->GetSelection() == 2) enabled->SetValue(true);
        });
        box->Add(grid,1,wxEXPAND|wxALL,10);
        const auto& state=page->areaScene().weather();
        const wxString weather = state.currentType() == 0 ? "Clear" : state.currentType() == 1 ? "Rain" : "Snow";
        box->Add(new wxStaticText(&dialog,wxID_ANY,wxString::Format("Current weather: %s, density %.3g, transition %.3g s.",
            weather,double(state.density()),double(state.transitionMilliseconds()) / 1000.0)),0,wxALL,10);
        box->Add(new wxStaticText(&dialog,wxID_ANY,developer ?
            "Player and module-camera inputs are independent. Weather placement requires both.\n"
            "The server override gates scheduling; use the engine command separately to change client weather.\n"
            "Applying uses these captured positions. Engine shadowalpha has no renderer consumer." :
            "Viewport placement follows the camera.\n"
            "Choosing rain, snow or stop switches off the area schedule.\n"
            "Captured positions and replay inputs are in Developer > Area Weather Inputs."),0,wxALL,10);
        box->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxALL|wxALIGN_RIGHT,10);dialog.SetSizerAndFit(box);
        if(dialog.ShowModal()!=wxID_OK)return;
        try {
            const auto read=[](wxTextCtrl* field) {double value=0;if(!field->GetValue().ToDouble(&value))throw std::runtime_error("Enter numeric weather inputs.");return static_cast<float>(value);};
            WeatherFrameInput input=current;input.enabled=enabled->GetValue();input.serverSchedule=schedule->GetValue();
            input.thunderEnabled=thunder->GetValue();input.shadowAlpha=page->weatherShadowAlpha();
            if (developer) {
                input.playerPosition.reset();if(supplyPlayer->GetValue())input.playerPosition=model::Vec3{read(x),read(y),read(z)};
                input.roomWeatherDisabled=roomDisabled->GetValue();
                input.serverOverride=static_cast<std::uint8_t>(overrideType->GetValue());input.shadowAlpha=read(shadow);
            }
            const auto soundDirectory=developer ? neosettings::pathFromWx(sounds->GetValue()) : std::filesystem::path{};
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
            const bool viewerPosition = !developer && placement->GetSelection() == 0;
            const bool viewerCamera = developer ? camera->GetValue() : page->weatherUsesViewerCamera();
            page->configureWeather(std::move(prepared),input,viewerCamera,requested,viewerPosition);
            if(!missing.empty()) {
                std::string message="Unavailable thunder resources:\n";for(const auto& name:missing)message+=name+"\n";
                wxMessageBox(wxui::toWx(message),"Weather resource report",wxOK|wxICON_INFORMATION,this);
            }
        } catch(const std::exception& e){wxui::showError(this,e);}
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
        box->Add(dialog.CreateSeparatedButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);dialog.SetSizerAndFit(box);
        if(dialog.ShowModal()!=wxID_OK)return;
        settings.enabled=enabled->GetValue();settings.wind=wind->GetValue();settings.lightmaps=lightmaps->GetValue();
        settings.textured=textured->GetValue();settings.areaWind.reset();
        try {page->configureGrass(settings);}catch(const std::exception& error){wxMessageBox(wxui::toWx(error.what()),"Grass options",wxOK|wxICON_ERROR,this);}
    }

    void chooseAndOpenModel(const std::filesystem::path& initialDirectory = {}) {
        wxFileDialog dialog(
            this, "Open Odyssey models", neosettings::pathToWx(initialDirectory),
            wxEmptyString,
            "Odyssey models (*.mdl;*.mdx;*.mdl.ascii)|*.mdl;*.MDL;*.mdx;*.MDX;*.mdl.ascii;*.MDL.ASCII|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
        if (dialog.ShowModal() != wxID_OK) return;
        wxArrayString paths;
        dialog.GetPaths(paths);
        for (const auto& path : paths) openFile(neosettings::pathFromWx(path));
    }

    void chooseTextureOverride() {
        auto* page = activePage();
        const auto& current = page->document().textureOverride();
        const auto directory = current ? current->sourcePath.parent_path() : page->document().path().parent_path();
        wxFileDialog dialog(this, "Preview Texture Override", neosettings::pathToWx(directory), wxEmptyString,
            "Texture images (*.tga;*.tpc;*.txb;*.dds;*.png;*.jpg;*.jpeg;*.bmp)|"
            "*.tga;*.TGA;*.tpc;*.TPC;*.txb;*.TXB;*.dds;*.DDS;*.png;*.PNG;*.jpg;*.JPG;*.jpeg;*.JPEG;*.bmp;*.BMP",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto path = neosettings::pathFromWx(dialog.GetPath());
        try {
            std::shared_ptr<const neoshared::texture::TextureData> texture;
            if (!runJob("Loading Texture Override", [&](const neoshared::bif::JobControl& job) {
                job.update(0u, 0u, "Reading " + neoshared::pathToUtf8(path.filename()));
                texture = std::make_shared<const neoshared::texture::TextureData>(readPreviewTexture(path));
            })) return;
            page->setTextureOverride(std::move(texture));
            options_.textures = true;
            applyDisplayOptions();
            updateState();
        } catch (const std::exception& error) {
            wxMessageBox(wxui::toWx(error.what()), "Unable to Load Texture Override", wxOK | wxICON_ERROR, this);
        }
    }

    void exportAsciiModel() {
        auto* page=activePage();
        const auto& document=page->document();
        auto protectedInputs=document.openPaths();
        const auto stem=document.path().empty()?document.model().name:conversionFileStem(document.path());
        const auto filename=asciiModelFilename(stem,asciiNamingPreference());
        const auto* editor = page->asciiEditor();
        protectedInputs.insert(protectedInputs.end(), editor->replacementInputs().begin(), editor->replacementInputs().end());
        for(const auto& [kind,asset]:editor->collisions())
            protectedInputs.insert(protectedInputs.end(),asset.inputs.begin(),asset.inputs.end());
        AsciiExportControls controls(asciiNormalMode_, editor->advanced());
        wxFileDialog file(this,"Export ASCII Model and Companions",neosettings::pathToWx(document.path().parent_path()),
            wxui::toWx(filename),"ASCII models (*.mdl;*.mdl.ascii)|*.mdl;*.mdl.ascii|All files (*.*)|*.*",
            wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        file.SetCustomizeHook(controls);
        if(file.ShowModal()!=wxID_OK) return;
        asciiNormalMode_=controls.normalMode;
        model::AsciiExportOptions options;
        options.advanced=controls.advanced;
        if(asciiNormalMode_!=0)options.explicitNormals=asciiNormalMode_==2;
        options.refineTangentRounding=asciiNormalMode_==3;
        options.refineNormalRounding=asciiNormalMode_==3;
        options.includeAnimations=controls.includeAnimations;
        try {
            const auto output=neosettings::pathFromWx(file.GetPath());
            auto reportPath=output;reportPath+=".report.txt";
            const auto collisions=collisionOutputs(editor->collisions(),output,false);
            std::vector<std::filesystem::path> companionPaths{reportPath};
            std::vector<std::filesystem::path> destinations{output,reportPath};
            for(const auto& collision:collisions) {
                destinations.push_back(collision.path);
                companionPaths.push_back(collision.path);
            }
            neoshared::checkResourceOutputs(destinations,protectedInputs);
            model::AsciiExportResult exported;
            ConversionReport report;report.source=document.sourceDescription();report.game=document.model().game;
            std::vector<std::uint8_t> bytes;
            const auto text = editor->text();
            const bool original = !editor->edited();
            if(!runJob("Exporting ASCII model",[&](const neoshared::bif::JobControl& job) {
                // runJob keeps documents alive and suspends scene updates.
                job.check();
                const auto started=std::chrono::steady_clock::now();
                if(original) {
                    exported=model::exportAsciiModel(editor->source()->sourceModel(),job,options);
                    report.summary=model::modelSummary(editor->source()->sourceModel());
                }
                else {
                    const auto edited = editor->source()->rebuildAscii(text, job, std::nullopt, options.includeAnimations);
                    exported=model::exportAsciiModel(edited->sourceModel(),job,options);
                    report.summary=model::modelSummary(edited->sourceModel());
                }
                bytes.assign(exported.text.begin(),exported.text.end());
                report.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
                job.check();
            })) return;
            if(!confirmCompanionOverwrite(this,companionPaths)) return;
            neoshared::texture::writeFileBytes(output,bytes);
            for(const auto& collision:collisions) neoshared::texture::writeFileBytes(collision.path,collision.bytes);
            report.outputs.push_back(output);
            for(const auto& collision:collisions)report.outputs.push_back(collision.path);
            report.warnings=exported.warnings;
            ConversionOptions reportOptions;reportOptions.ascii=options;
            writeConversionReport(reportPath,report,reportOptions);
            setModuleStatusText("ASCII model and companions exported to "+wxui::toWx(neoshared::pathToUtf8(output)),0);
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
        const auto& document=page->document();
        auto protectedInputs=document.openPaths();
        const auto* editor = page->asciiEditor();
        protectedInputs.insert(protectedInputs.end(), editor->replacementInputs().begin(), editor->replacementInputs().end());
        for(const auto& [kind,asset]:editor->collisions())
            protectedInputs.insert(protectedInputs.end(),asset.inputs.begin(),asset.inputs.end());
        const auto filename=document.model().name+"-packed.mdl";
        PackedExportControls controls(page->outputGame());
        wxFileDialog file(this,"Compile Model and Companions",neosettings::pathToWx(document.path().parent_path()),
            wxui::toWx(filename),"Binary models (*.mdl)|*.mdl",
            wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        file.SetCustomizeHook(controls);
        if(file.ShowModal()!=wxID_OK) return;
        page->setOutputGame(controls.game);
        try {
            const auto output=neosettings::pathFromWx(file.GetPath());
            auto companion=output;companion.replace_extension(".mdx");
            auto reportPath=output;reportPath+=".report.txt";
            neoshared::checkResourceOutputs({output,companion,reportPath},protectedInputs);
            protectedInputs.insert(protectedInputs.end(),{output,companion,reportPath});
            model::PackedExportResult exported;
            ConversionReport report;report.source=document.sourceDescription();report.game=controls.game;
            std::vector<CollisionOutput> collisions;
            const auto text = editor->text();
            if(!runJob("Compiling packed model",[&](const neoshared::bif::JobControl& job) {
                job.check();
                const auto started=std::chrono::steady_clock::now();
                const auto edited = editor->source()->rebuildAscii(text, job, controls.game, controls.includeAnimations);
                exported=model::exportPackedModel(edited->sourceModel(),job);
                collisions=collisionOutputs(editor->collisions(),output,true,job);
                report.summary=model::modelSummary(edited->sourceModel());report.warnings=edited->sourceModel().warnings;
                report.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
                job.check();
            })) return;
            std::vector<std::filesystem::path> companionPaths{companion,reportPath};
            std::vector<std::filesystem::path> collisionPaths;
            for(const auto& collision:collisions) {
                collisionPaths.push_back(collision.path);
                companionPaths.push_back(collision.path);
            }
            neoshared::checkResourceOutputs(collisionPaths,protectedInputs);
            if(!confirmCompanionOverwrite(this,companionPaths)) return;
            neoshared::texture::writeFileBytes(companion,exported.mdx);
            neoshared::texture::writeFileBytes(output,exported.mdl);
            for(const auto& collision:collisions) neoshared::texture::writeFileBytes(collision.path,collision.bytes);
            report.outputs={output,companion};
            for(const auto& collision:collisions)report.outputs.push_back(collision.path);
            ConversionOptions reportOptions;reportOptions.ascii.advanced=editor->advanced();reportOptions.ascii.includeAnimations=controls.includeAnimations;
            writeConversionReport(reportPath,report,reportOptions);
            setModuleStatusText("Binary model and companions exported to "+wxui::toWx(neoshared::pathToUtf8(output)),0);
        } catch(const std::exception& error) {
            wxMessageBox(wxui::toWx(error.what()),"Binary model not exported",wxOK|wxICON_ERROR,this);
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

    void addDocument(std::unique_ptr<ModelDocument> document, std::shared_ptr<GameResourceSession> session = {},
                     ResourceResolver fileResources = {}, ModelPage* replacement = nullptr,bool preserveEditingContext=false) {
        const auto title = wxui::toWx(document->title());
        auto* page = new ModelPage(tabs_, std::move(document), replacement ? replacement->options() : options_,
                                   context_.compact, [this] { updateState(); });
        page->setResourceSession(std::move(session));
        page->setFileResources(std::move(fileResources));
        wxui::applyTheme(page, dark_);
        neoview::applyFontScale(page, fontScale_);
        page->setSourceAppearance(dark_, fontScale_);
        const auto index = replacement ? static_cast<std::size_t>(tabs_->GetPageIndex(replacement)) : tabs_->GetPageCount();
        if (!tabs_->InsertPage(index, page, title, true)) {
            page->Destroy();
            throw std::runtime_error("Unable to create a NeoMDL document tab.");
        }
        if (preserveEditingContext) page->copyEditingContext(*replacement);
        if (replacement) tabs_->DeletePage(index + 1u);
        updateState();
    }

    void closePage(int index) {
        if (jobRunning_) { cancelJob_.store(true); return; }
        if (index < 0 || static_cast<std::size_t>(index) >= tabs_->GetPageCount()) return;
        if (!confirmDiscard(pageAt(static_cast<std::size_t>(index)))) return;
        tabs_->DeletePage(static_cast<std::size_t>(index));
        updateState();
    }

    void closeAll() {
        if (!canClose()) return;
        while (tabs_->GetPageCount() != 0u) tabs_->DeletePage(tabs_->GetPageCount() - 1u);
        updateState();
    }

    static void copyDisplayOptions(RenderOptions& target, const RenderOptions& source) {
        for (auto member : {&RenderOptions::textures, &RenderOptions::lightmaps, &RenderOptions::wireframe,
                &RenderOptions::lighting, &RenderOptions::showHidden, &RenderOptions::cullBackfaces,
                &RenderOptions::danglySimulation, &RenderOptions::nodeHelpers, &RenderOptions::previewLights,
                &RenderOptions::fogEnabled, &RenderOptions::previewEmitters, &RenderOptions::showCameras,
                &RenderOptions::showReferences, &RenderOptions::previewLightsabers,
                &RenderOptions::previewAabbs, &RenderOptions::previewTriggers})
            target.*member = source.*member;
        for (auto member : {&RenderOptions::grid, &RenderOptions::axes, &RenderOptions::headlight})
            target.*member = source.*member;
    }

    static void copyHardwareOptions(RenderOptions& target, const RenderOptions& source) {
        target.engineHardware = source.engineHardware;
        target.engineUseVertexPrograms = source.engineUseVertexPrograms;
        target.gobNewSorting = source.gobNewSorting;
        target.procedureMipPath = source.procedureMipPath;
    }

    void applyDisplayOptions() {
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index) {
            auto* page = pageAt(index);
            auto options = page->options();
            copyDisplayOptions(options, options_);
            page->setOptions(options);
            page->refreshDetails();
        }
        updateMenuChecks();
    }

    void applyPreviewOptions(const RenderOptions& options) {
        activePage()->setOptions(options);
        activePage()->refreshDetails();
        updateMenuChecks();
    }

    template<class T>
    void setPreviewOption(T RenderOptions::*member, T value) {
        auto options = activePage()->options();
        options.*member = value;
        applyPreviewOptions(options);
    }

    void applyHardwareOptions() {
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index) {
            auto* page = pageAt(index);
            auto options = page->options();
            copyHardwareOptions(options, options_);
            page->setOptions(options);
            page->refreshDetails();
        }
    }

    void buildMenus() {
        auto* bar = new wxMenuBar;
        auto* file = new wxMenu;
        file->Append(ID_Open, context_.embedded ? "&Open Models...\tCtrl+Shift+O" : "&Open Models...\tCtrl+O");
        recentFilesMenu_ = new wxMenu;
        neosettings::populateRecentFilesMenu(*recentFilesMenu_, settings_, ID_RecentFirst, ID_ClearRecent);
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->Append(ID_Reload, "&Reload Model\tCtrl+R", "Reload the model from disk and reset its preview.");
        file->Append(ID_SaveAscii, "Save &ASCII\tCtrl+S", "Save ASCII source text and collision companions. Use Compile ASCII to Binary to create MDL/MDX.");
        file->Append(ID_SaveAsciiAs, "Save ASCII As...\tCtrl+Shift+S", "Save the editor text to another file.");
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file,
            [this](const neogames::SavedGameDirectory& directory) {
                openSavedGameDirectory(directory);
            },
            neogames::GameDirectoryGameIds{"kotor", "kotor2"},
            "Open Model from Saved &Directory");
        file->Append(ID_LoadArea, "Load Area Layout...");
        file->Append(ID_ModuleArea, "Load Module Area...");
        file->AppendSeparator();
        file->Append(ID_ExportAscii, "Export ASCII MDL...", "Export the loaded model or current ASCII edits with conversion options.");
        file->Append(ID_ExportPacked, "Compile ASCII to &Binary...",
                     "Compile the text in the ASCII editor to MDL/MDX.");
        file->Append(ID_ExportTextures, "Export Texture List...", "Save the model's directly referenced texture names.");
        file->AppendSeparator();
        file->Append(ID_QuickConvert, "Quick Convert Files...", "Read model files and write their opposite format.");
        file->Append(ID_BatchDecompile, "Batch Decompile...", "Convert selected model files to ASCII.");
        file->Append(ID_BatchCompile, "Batch Compile...", "Compile selected model files to binary MDL/MDX.");
        file->Append(ID_BinaryMeshFields, "Binary Mesh Editor...", "Edit the selected binary mesh or open another model, then save a new MDL/MDX copy.");
        file->AppendSeparator();
        file->Append(ID_CloseModel, "&Close Model\tCtrl+W");
        file->Append(ID_CloseAll, "Close &All");
        if (!context_.embedded) {
            file->AppendSeparator();
            file->Append(ID_Exit, "E&xit");
        }
        bar->Append(file, "&File");

        auto* edit = new wxMenu;
        edit->Append(ID_Undo, "&Undo ASCII Edit\tCtrl+Z", "Undo changes to the ASCII source text.");
        edit->Append(ID_Redo, "&Redo ASCII Edit\tCtrl+Y");
        edit->Append(ID_Find, "&Find in ASCII...\tCtrl+F");
        edit->AppendSeparator();
        edit->Append(ID_NodeProperties, "Selected Node Properties...", "Edit the selected node's properties.");
        edit->Append(ID_NodeTransform,"Selected Node Transform...","Edit the selected binary node's base position, rotation and scale.");
        edit->Append(ID_VertexProperties, "Selected Mesh Vertex Properties...", "Edit stored vertex attributes in binary models, or source tables in ASCII models.");
        edit->Append(ID_ReplaceMesh, "Replace Selected Mesh...", "Queue mesh replacements from donor models.");
        edit->Append(ID_ConvertMesh, "Convert Selected Mesh to Trimesh", "Bake a skin at its constructed bind pose, or retain a lightsaber's stationary blade pieces. Undoable ASCII edit.");
        edit->Append(ID_RecalculateNormals, "Recalculate Selected Mesh Normals...", "Choose normal weighting and a crease angle for the selected mesh. Undoable ASCII edit.");
        edit->Append(ID_ControllerProperties, "Selected Node Controller Properties...", "Edit binary mesh opacity/glow, or ASCII node controllers and animation keys.");
        edit->Append(ID_CollisionCompanions, "Collision Companions...", "Edit PWK, door DWK states and room WOK geometry, hooks and links.");
        edit->Append(ID_ModelProperties, "Model Properties...", "Choose the animation root and head link.");
        edit->Append(ID_RenameTexture, "Rename Texture Throughout Model...", "Change matching texture references in this model.");
        bar->Append(edit, "&Edit");

        viewMenu_ = new wxMenu;
        bar->Append(viewMenu_, "&View");
        sceneMenu_ = new wxMenu;
        bar->Append(sceneMenu_, "&Scene");
        buildPreviewMenus(0u);

        auto* tools = new wxMenu;
        tools->Append(ID_ModelResources, "Model Resource Installation...",
                      "Choose the game installation supplying textures and supermodels for this model.");
        tools->Append(ID_ResourceProvenance, "Resource Scope and Load Report...");
        tools->Append(ID_ConversionNaming, "Default ASCII Filename...", "Choose the filename convention for new ASCII saves and conversions.");
        bar->Append(tools, "&Tools");

        developerMenu_ = new wxMenu;
        developerMenu_->Append(ID_EngineGraphics, "Hardware Emulation...");
        developerMenu_->Append(ID_DeveloperSettings, "Rendering Diagnostics...");
        developerMenu_->Append(ID_EngineReport, "Save Rendering Report...");
        developerMenu_->Append(ID_InspectBinary, "Inspect Binary File...", "Inspect bytes, stored data types and model structures.");
        developerMenu_->Append(ID_CompareBinaries, "Compare Binary Files...", "Highlight differences between two files at their stored byte offsets.");
        developerMenu_->Append(ID_AnalyzeBinaries, "Analyze Binary Models...", "Analyze the stored structures of multiple PC MDL/MDX models.");
        bar->Append(developerMenu_, "&Developer");

        helpMenu_ = new wxMenu;
        helpMenu_->Append(ID_Workflow, "Model Editing Workflow...");
        helpMenu_->Append(ID_About, "&About NeoMDL");
        bar->Append(helpMenu_, "&Help");
        setModuleMenus(bar);
        updateMenuChecks();
    }

    void buildPreviewMenus(unsigned features) {
        previewMenuFeatures_ = features;
        const bool area = (features & PreviewArea) != 0u;
        for (auto* menu : {viewMenu_, sceneMenu_})
            while (menu->GetMenuItemCount() != 0u)
                menu->Destroy(menu->FindItemByPosition(0));

        auto* view = viewMenu_;
        if (!context_.embedded) {
            view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
            view->Append(ID_FontIncrease, "Increase UI Font Size\tCtrl++");
            view->Append(ID_FontDecrease, "Decrease UI Font Size\tCtrl+-");
            view->Append(ID_FontReset, "Reset UI Font Size\tCtrl+0");
            view->AppendSeparator();
        }
        view->AppendCheckItem(ID_AsciiEditor, "ASCII Editor");
        view->Append(ID_FrameModel, "&Frame Model", "Press F while the viewport has keyboard focus.");
        auto* camera = new wxMenu;
        camera->AppendCheckItem(ID_OrbitCamera, "&Orbit");
        camera->AppendCheckItem(ID_FreeFlyCamera, "&Free Fly", "Press Shift+F while the viewport has keyboard focus.");
        camera->Append(ID_CameraControls, "Navigation Controls...");
        if (features & PreviewCameras) {
            camera->AppendSeparator();
            camera->Append(ID_ViewSelectedCamera, "View Through Selected Camera");
            camera->Append(ID_ViewCameraHook, "Attach to Camera Hook");
            if (area) camera->Append(ID_ViewAreaCamera, "View Through Area Camera...");
            camera->AppendSeparator();
            camera->Append(ID_CameraSettings, "Projection Settings...");
        }
        view->AppendSubMenu(camera, "&Camera");
        view->AppendSeparator();
        view->AppendCheckItem(ID_Textures, "Show &Textures");
        if (!area) {
            auto* textureOverride = new wxMenu;
            textureOverride->Append(ID_LoadTextureOverride, "Load from File...",
                "Preview an image on all meshes in this model tab using their existing UVs.");
            textureOverride->Append(ID_ClearTextureOverride, "Clear Override",
                "Restore the model's authored textures.");
            view->AppendSubMenu(textureOverride, "Texture Override");
        }
        view->AppendCheckItem(ID_Lightmaps, "Show &Lightmaps");
        view->AppendCheckItem(ID_Wireframe, "&Wireframe");
        view->AppendCheckItem(ID_CullBackfaces, "Cull &Backfaces");
        view->AppendCheckItem(ID_Lighting, "&Lighting");
        view->AppendCheckItem(ID_Headlight, "Preview Key Light");
        if (features & PreviewLights) view->AppendCheckItem(ID_PreviewLights, "Authored Model Lights");
        if (area) view->AppendCheckItem(ID_EngineShadows, "Area Shadows");
        view->AppendCheckItem(ID_Fog, "Show Fog");
        view->Append(ID_LightingSettings, "Lighting and Fog Settings...");
        view->Append(ID_OwnerVisualEffects,"Owner Visual Effects (Preview)...");
        view->Append(ID_SceneEffects,"Scene Effects (Preview)...");
        if (features & (PreviewReferences | PreviewParticles | PreviewLightsabers | PreviewDangly))
            view->AppendSeparator();
        if (features & PreviewReferences) view->AppendCheckItem(ID_ShowReferences, "Referenced Models");
        if (features & PreviewParticles) view->AppendCheckItem(ID_PreviewEmitters, "Particles");
        if (features & PreviewLightsabers) view->AppendCheckItem(ID_PreviewLightsabers, "Lightsabers");
        if (features & PreviewDangly) view->AppendCheckItem(ID_DanglySimulation, "Simulate &Dangly Meshes");
        view->AppendSeparator();
        view->AppendCheckItem(ID_Grid, "Show &Grid");
        view->AppendCheckItem(ID_Axes, "Show &Axes");
        auto* inspection = new wxMenu;
        inspection->AppendCheckItem(ID_IsolateMeshes,"Isolate Selected Meshes","Temporarily show only the selected meshes in this viewport.");
        inspection->Append(ID_HideMeshes,"Hide Selected Meshes","Temporarily hide the selected meshes in this viewport.");
        inspection->Append(ID_ShowAllMeshes,"Show All Meshes","Clear temporary mesh hiding and isolation.");
        inspection->AppendSeparator();
        inspection->AppendCheckItem(ID_ShowHidden, "Show Meshes with Render Flag Off");
        inspection->AppendCheckItem(ID_NodeHelpers, "Node Helpers");
        if (features & PreviewCameras) inspection->AppendCheckItem(ID_ShowCameras, "Camera Markers");
        inspection->AppendCheckItem(ID_PreviewAabbs, "Collision Geometry");
        if (area) inspection->AppendCheckItem(ID_PreviewTriggers, "Trigger Volumes");
        view->AppendSubMenu(inspection, "&Inspection");
        view->AppendSeparator();
        view->Append(ID_ResetView, "Reset Current Preview Rendering");
        view->Append(ID_ResetDisplay, "Reset Shared Display Preferences");

        auto* scene = sceneMenu_;
        scene->Append(ID_AdvancedSettings, "Preview Settings...");
        if (area) {
            scene->AppendSeparator();
            scene->Append(ID_AreaRoom, "Current Area Room...");
            scene->Append(ID_ClearArea, "Clear Area Context");
            scene->Append(ID_LoadGrass, "Load Grass from ARE...");
            scene->AppendSeparator();
            auto* animation = new wxMenu;
            animation->Append(ID_AreaAnimation, "Area Object Animation...");
            animation->Append(ID_RoomScriptAnimation, "Room Animation...");
            animation->Append(ID_DoorState, "Door State...");
            scene->AppendSubMenu(animation, "&Animation");
            scene->AppendSeparator();
            scene->Append(ID_SceneAudio, "Scene Audio...");
            scene->Append(ID_AreaWeather, "Area Weather...");
        }
        if (features & PreviewParticles)
            scene->Append(ID_DetonateEmitters, "Detonate Explosion Emitters");
    }

    void showAdvancedSettings(bool developer = false) {
        const auto* active=activePage();
        const auto& options=active->options();
        const auto features=active->previewFeatures();
        const bool area=!active->areaScene().empty();
        const bool k2=active->document().model().game==model::GameVersion::Kotor2;
        wxDialog dialog(this,wxID_ANY,developer ? "Rendering Diagnostics" : "Preview Settings",wxDefaultPosition,wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "These settings affect only the current tab. Hardware emulation and shared display preferences apply to all tabs."),0,wxALL,10);
        auto* notebook=new wxNotebook(&dialog,wxID_ANY);
        const auto tab=[&](const char* title) {
            auto* panel=new wxPanel(notebook);
            panel->SetSizer(new wxBoxSizer(wxVERTICAL));
            notebook->AddPage(panel,title);
            return panel;
        };
        const auto action=[&](wxPanel* panel,const char* label,
                              void (ModelEditorPanelImpl::*handler)()) {
            auto* button=new wxButton(panel,wxID_ANY,label);
            button->Bind(wxEVT_BUTTON,[this,handler](wxCommandEvent&) {
                (this->*handler)();updateState();
            });
            panel->GetSizer()->Add(button,0,wxEXPAND|wxALL,5);
        };
        const auto toggle=[&](wxPanel* panel,const char* label,bool RenderOptions::*member,bool enabled=true) {
            auto* control=new wxCheckBox(panel,wxID_ANY,label);
            control->SetValue(options.*member);control->Enable(enabled);
            control->Bind(wxEVT_CHECKBOX,[this,member](wxCommandEvent& event) {
                setPreviewOption(member,event.IsChecked());
            });
            panel->GetSizer()->Add(control,0,wxALL,8);
        };
        auto* rendering=tab("Rendering");
        if (developer) {
            toggle(rendering,"K2 nearest-light object-only mode",&RenderOptions::nearestLightsOnlyGobs,k2&&!area);
            if (area) {
                toggle(rendering,"Two-sided shadow stencil",&RenderOptions::engineTwoSidedShadows);
                rendering->GetSizer()->Add(new wxStaticText(rendering,wxID_ANY,"Shadow rendering"),0,wxLEFT|wxTOP,8);
                wxArrayString shadows;
                shadows.Add("Hard shadows");shadows.Add("Soft shadows (ARB)");shadows.Add("Soft shadows (ATI compatibility)");
                auto* shadow=new wxChoice(rendering,wxID_ANY,wxDefaultPosition,wxDefaultSize,shadows);
                shadow->SetSelection(!options.engineSoftShadows?0:
                    options.engineSoftShadowMode==render::EngineSoftShadowMode::DesktopARB?1:2);
                shadow->Bind(wxEVT_CHOICE,[this,shadow](wxCommandEvent&) {
                    auto options=activePage()->options();
                    options.engineSoftShadows=shadow->GetSelection()!=0;
                    options.engineSoftShadowMode=shadow->GetSelection()==2
                        ?render::EngineSoftShadowMode::DesktopATI:render::EngineSoftShadowMode::DesktopARB;
                    applyPreviewOptions(options);
                });
                rendering->GetSizer()->Add(shadow,0,wxEXPAND|wxALL,8);
                toggle(rendering,"Inspect mesh projection volumes",&RenderOptions::inspectMeshProjections,k2);
            }
            rendering->GetSizer()->Add(new wxStaticText(rendering,wxID_ANY,"Object light-query radius"),0,wxLEFT|wxTOP,8);
            auto* radius=new wxSpinCtrlDouble(rendering,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
                wxSP_ARROW_KEYS,0,1000000,options.objectLightRadius,0.1);
            radius->SetDigits(4);
            radius->SetToolTip("External Gob +0x164 state. The engine constructor initializes it to 1; this is not the mesh AABB radius.");
            radius->Bind(wxEVT_SPINCTRLDOUBLE,[this](wxSpinDoubleEvent& event) {
                setPreviewOption(&RenderOptions::objectLightRadius,static_cast<float>(event.GetValue()));
            });
            rendering->GetSizer()->Add(radius,0,wxEXPAND|wxALL,8);
        } else {
            action(rendering,"Lighting and Fog...",&ModelEditorPanelImpl::showLightingSettings);
            if (features & PreviewCameras)
                action(rendering,"Camera Projection...",&ModelEditorPanelImpl::showCameraSettings);
            if (features & PreviewLightsabers)
                action(rendering,"Lightsaber Rendering...",&ModelEditorPanelImpl::showLightsaberSettings);
            if (features & (PreviewParticles | PreviewDangly))
                action(rendering,features & PreviewParticles ? "Particle and Wind Settings..." : "Wind Settings...",
                    &ModelEditorPanelImpl::showParticleSettings);
        }

        if (area) {
            auto* scene=tab("Scene");
            if (developer) {
                action(scene,"Shadow Inputs...",&ModelEditorPanelImpl::chooseSceneShadows);
                action(scene,"Shadow Casters...",&ModelEditorPanelImpl::chooseShadowCasters);
            } else {
                action(scene,"Area Environment...",&ModelEditorPanelImpl::chooseAreaEnvironment);
                action(scene,"Grass Settings...",&ModelEditorPanelImpl::chooseGrassOptions);
            }
        }
        root->Add(notebook,1,wxEXPAND|wxLEFT|wxRIGHT,10);
        root->Add(new wxButton(&dialog,wxID_OK,"Close"),0,wxALIGN_RIGHT|wxALL,10);
        dialog.SetSizerAndFit(root);
        dialog.ShowModal();
    }

    void updateMenuChecks() {
        if (!viewMenu_) return;
        const auto* active=activePage();
        const bool standalone=!active || active->areaScene().empty();
        const auto setCheck = [this](int id, bool checked) {
            if (auto* item = viewMenu_->FindItem(id)) item->Check(checked);
        };
        setCheck(ID_DarkMode, dark_);
        setCheck(ID_OrbitCamera, active && !active->authoredCameraActive() && !active->freeFlyCameraActive());
        setCheck(ID_FreeFlyCamera, active && active->freeFlyCameraActive());
        setCheck(ID_AsciiEditor, active && active->asciiEditor() && active->asciiVisible());
        setCheck(ID_Textures, options_.textures);
        setCheck(ID_Lightmaps, options_.lightmaps);
        setCheck(ID_Wireframe, options_.wireframe);
        setCheck(ID_Lighting, options_.lighting);
        setCheck(ID_Headlight, options_.headlight.value_or(standalone));
        setCheck(ID_Fog, options_.fogEnabled);
        setCheck(ID_Grid, options_.grid.value_or(standalone));
        setCheck(ID_Axes, options_.axes.value_or(standalone));
        setCheck(ID_ShowHidden, options_.showHidden);
        setCheck(ID_IsolateMeshes,active&&active->meshesIsolated());
        setCheck(ID_CullBackfaces, options_.cullBackfaces);
        setCheck(ID_DanglySimulation, options_.danglySimulation);
        setCheck(ID_NodeHelpers, options_.nodeHelpers);
        setCheck(ID_PreviewLights, options_.previewLights);
        setCheck(ID_EngineShadows, active && active->options().engineShadows.value_or(active->areaScene().currentRoom().has_value()));
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

    void showCameraSettings() {
        auto options=activePage()->options();
        wxDialog dialog(this, wxID_ANY, "Authored Camera Projection");
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog, wxID_ANY,
            "Camera projection for this tab. Area cameras retain their authored FOV;\n"
            "the near and far clipping distances apply to all authored-camera views."),
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
            wxSP_ARROW_KEYS, 1.0, 179.0, options.cameraSettings.viewAngle, 1.0);
        fov->SetDigits(2);
        add("Vertical FOV (degrees)", fov);

        auto* nearClip = new wxSpinCtrlDouble(
            &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(160, -1),
            wxSP_ARROW_KEYS, 0.0001, 1000000.0, options.cameraSettings.nearClip, 0.1);
        nearClip->SetDigits(4);
        add("Near clip", nearClip);

        auto* farClip = new wxSpinCtrlDouble(
            &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(160, -1),
            wxSP_ARROW_KEYS, 0.001, 100000000.0, options.cameraSettings.farClip, 100.0);
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
            options.cameraSettings = settings;
            applyPreviewOptions(options);
            break;
        }
    }

    void showParticleSettings() {
        auto options=activePage()->options();
        const bool particles=(activePage()->previewFeatures() & PreviewParticles)!=0u;
        wxDialog dialog(this,wxID_ANY,particles ? "Particle and Wind Settings" : "Wind Settings");
        auto* root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog,wxID_ANY,
            "Preview settings for this tab.\n"
            "Wind affects both particles and dangly meshes. The MDL file is unchanged."),0,wxALL,10);
        auto* grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1,1);
        const auto add=[&](const char* label,wxWindow* c) {
            grid->Add(new wxStaticText(&dialog,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            grid->Add(c,1,wxEXPAND);
        };
        auto settings=options.particleSettings;
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
        root->Add(grid,0,wxEXPAND|wxLEFT|wxRIGHT,10);root->Show(grid,particles);
        grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1,1);
        auto* windPreset=new wxChoice(&dialog,wxID_ANY);
        windPreset->Append("Custom");
        windPreset->Append("K2 wind: none");
        windPreset->Append("K2 wind: light");
        windPreset->Append("K2 wind: strong");
        windPreset->SetSelection(0);add("Wind preset",windPreset);
        const auto wind=vector("Global wind X / Y / Z",{q.x,q.y,q.z});
        const auto windDeviation=vector("Maximum deviation Z / X (degrees)",
            {settings.windMaximumDeviationZDegrees,settings.windMaximumDeviationXDegrees});
        const auto windPeriod=vector("Noise interpolation period (seconds)",
            {settings.windInterpolationPeriod});
        windPeriod[0]->SetRange(std::numeric_limits<float>::denorm_min(),std::numeric_limits<float>::max());
        const auto nearlyEqual=[](float a,float b) {return std::fabs(a-b)<=1.0e-4f;};
        for(unsigned mode=0;mode<=2;++mode) {
            const auto preset=neomdl::model::k2AreaWindPreset(static_cast<std::uint8_t>(mode));
            if(preset && nearlyEqual(q.x,preset->globalWind.x)&&nearlyEqual(q.y,preset->globalWind.y)&&
               nearlyEqual(q.z,preset->globalWind.z)&&
               nearlyEqual(settings.windMaximumDeviationZDegrees,preset->maximumDeviationZDegrees)&&
               nearlyEqual(settings.windMaximumDeviationXDegrees,preset->maximumDeviationXDegrees)) {
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
        root->Add(grid,0,wxEXPAND|wxLEFT|wxRIGHT,10);
        grid=new wxFlexGridSizer(2,6,10);grid->AddGrowableCol(1,1);
        const auto tint=settings.objectTint;
        const auto objectTint=vector("Object tint R / G / B",{tint.x,tint.y,tint.z});
        const auto objectAlpha=vector("Object alpha",{settings.objectAlpha});
        const bool area=!activePage()->areaScene().empty();
        std::vector<wxSpinCtrlDouble*> tileAlpha;
        if(area)tileAlpha=vector("Tile opacity",{settings.tileAlpha});
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
        root->Show(grid,particles);
        root->Add(new wxStaticText(&dialog,wxID_ANY,"Applying changes updates the preview at the current animation time."),0,wxALL,10);
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
            settings.objectAlpha=value(objectAlpha[0]);
            if(area)settings.tileAlpha=value(tileAlpha[0]);
            settings.blastWind=blast->GetValue();
            settings.bounceMode=bounce->GetSelection()==1?neomdl::model::ParticleBounceMode::Reflection:neomdl::model::ParticleBounceMode::Legacy;
            if(!validParticleSceneSettings(settings)) {
                wxMessageBox("Use finite values, a positive wind interpolation period, and a nonzero quaternion.","Invalid particle settings",wxOK|wxICON_WARNING,&dialog);continue;
            }
            options.particleSettings=settings;applyPreviewOptions(options);break;
        }
    }

    void showLightsaberSettings() {
        auto options=activePage()->options();
        wxDialog dialog(this, wxID_ANY, "Lightsaber Render Settings");
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&dialog, wxID_ANY,
            "KOTOR II lightsaber render globals; not saved into the MDL.\n"
            "The engine bucket keeps owner insertion order and renders after particle emitters."),
            0, wxALL, 10);

        auto settings = options.lightsaberSettings;
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
        options.lightsaberSettings = settings;
        applyPreviewOptions(options);
    }

    void showEngineExecution() {
        if(!activePage())return;
        wxDialog dialog(this,wxID_ANY,"Hardware Emulation");
        auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&dialog,wxID_ANY,
            "Hardware emulation applies to all tabs and newly opened models. Changes reload preview textures."),0,wxALL,10);
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
        try {applyHardwareOptions();}
        catch(const std::exception& error) {
            options_=previous;
            std::string diagnostic=error.what();
            try {applyHardwareOptions();}
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

    void showLightingSettings() {
        auto options=activePage()->options();
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
        auto* ambient=new wxColourPickerCtrl(&dialog,wxID_ANY,toColor(options.sceneAmbient[0],options.sceneAmbient[1],options.sceneAmbient[2]));
        add("Scene ambient",ambient);
        auto* mode=new wxChoice(&dialog,wxID_ANY);
        for(const auto* name:{"Disabled","Linear","Exponential","Exponential squared"}) mode->Append(name);
        mode->SetSelection(static_cast<int>(options.fog.mode));add("Fog mode",mode);
        auto* color=new wxColourPickerCtrl(&dialog,wxID_ANY,toColor(options.fog.color.x,options.fog.color.y,options.fog.color.z));
        add("Fog color",color);
        const auto number=[&](double value,double min,double max,double increment) {
            auto* control=new wxSpinCtrlDouble(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,
                wxSP_ARROW_KEYS,min,max,value,increment);control->SetDigits(4);return control;
        };
        auto* start=number(options.fog.start,0,1000000,1);add("Linear fog start",start);
        auto* end=number(options.fog.end,0.001,1000001,1);add("Linear fog end",end);
        auto* density=number(options.fog.density,0,1000,0.005);add("Exponential density",density);
        auto* respect=new wxCheckBox(&dialog,wxID_ANY,"Respect each model's fog flag");
        respect->SetValue(options.fog.respectModelFlag);add("Fog eligibility",respect);
        root->Add(grid,1,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,10);
        root->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
        dialog.SetSizerAndFit(root);
        while(dialog.ShowModal()==wxID_OK) {
            auto fog=options.fog;
            fog.mode=static_cast<neomdl::model::FogMode>(mode->GetSelection());
            fog.start=static_cast<float>(start->GetValue());fog.end=static_cast<float>(end->GetValue());
            fog.density=static_cast<float>(density->GetValue());fog.respectModelFlag=respect->GetValue();
            const auto c=color->GetColour();fog.color={c.Red()/255.0f,c.Green()/255.0f,c.Blue()/255.0f};
            if(!neomdl::model::validFog(fog)) {
                wxMessageBox("Fog values must be finite and nonnegative.","Invalid fog",wxOK|wxICON_WARNING,&dialog);
                continue;
            }
            options.fog=fog;const auto a=ambient->GetColour();
            options.sceneAmbient={{a.Red()/255.0f,a.Green()/255.0f,a.Blue()/255.0f}};
            applyPreviewOptions(options);break;
        }
    }

    void bindEvents() {
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            openFile(settings_.recentFiles().at(static_cast<std::size_t>(event.GetId() - ID_RecentFirst)));
        }, ID_RecentFirst, ID_RecentLast);
        Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
            wxString navigation;
            if (const auto* page = activePage()) {
                if (page->authoredCameraActive()) navigation = "Authored Camera";
                else if (page->freeFlyCameraActive())
                    navigation = wxString::Format("Free Fly | %.3g units/s", double(page->freeFlySpeed()));
                else navigation = "Orbit";
            }
            if (moduleStatusBar()->GetStatusText(2) != navigation)
                setModuleStatusText(navigation, 2);
            event.Skip();
        });
        tabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, [this](wxAuiNotebookEvent& event) {
            updateState();
            event.Skip();
        });
        tabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& event) {
            auto* page = pageAt(event.GetSelection());
            event.Veto();
            if (jobRunning_) { cancelJob_.store(true); return; }
            CallAfter([this, page] { closePage(tabs_->GetPageIndex(page)); });
        });
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            if (jobRunning_) return;
            switch (event.GetId()) {
            case ID_Open: chooseAndOpenModel(); return;
            case ID_Undo: activePage()->asciiEditor()->undo(); return;
            case ID_Redo: activePage()->asciiEditor()->redo(); return;
            case ID_Find:
                activePage()->showAscii(true);
                activePage()->asciiEditor()->focusSearch();
                updateState(); return;
            case ID_DarkMode:
                settings_.setDarkMode(event.IsChecked());
                setAppearance(event.IsChecked(), fontScale_); return;
            case ID_FontIncrease: changeFontScale(1); return;
            case ID_FontDecrease: changeFontScale(-1); return;
            case ID_FontReset:
                fontScaleWheelFilter_.reset();
                settings_.setFontScale(neoview::kDefaultFontScale);
                setAppearance(dark_, neoview::kDefaultFontScale); return;
            case ID_ClearRecent:
                settings_.clearRecentFiles(); return;
            case ID_Reload: reloadModel(activePage()->fileResources(), activePage()->resourceSession()); return;
            case ID_ModelResources: chooseModelResources(); return;
            case ID_SaveAscii: activePage()->asciiEditor()->save(); return;
            case ID_SaveAsciiAs: activePage()->asciiEditor()->save(true); return;
            case ID_AsciiEditor: activePage()->showAscii(event.IsChecked()); updateState(); return;
            case ID_ExportAscii: exportAsciiModel(); return;
            case ID_ExportPacked: exportPackedModel(); return;
            case ID_BatchDecompile: case ID_BatchCompile: case ID_QuickConvert:
                showBatchConversion(this,event.GetId()==ID_BatchDecompile?ConversionMode::Ascii:
                    event.GetId()==ID_BatchCompile?ConversionMode::Binary:ConversionMode::Opposite,
                    [this](const wxString& title,const auto& work){return runJob(title,work);});return;
            case ID_ConversionNaming: chooseConversionNaming(this);return;
            case ID_NodeProperties:
                if (activePage()->canEditBinaryNode()) editBinaryMesh(activePage()->canEditBinaryMesh() ? BinaryMeshPage::Properties : BinaryMeshPage::Transform);
                else activePage()->editSelectedNode();
                return;
            case ID_NodeTransform: editBinaryMesh(BinaryMeshPage::Transform);return;
            case ID_VertexProperties:
                if (!activePage()->document().sourceModel().asciiBuilt) editBinaryMesh(BinaryMeshPage::Vertices);
                else activePage()->editSelectedVertices();
                return;
            case ID_ReplaceMesh:
                activePage()->replaceSelectedMesh([this](const wxString& title, const auto& work) {
                    return runJob(title, work);
                }); return;
            case ID_ControllerProperties:
                if (!activePage()->document().sourceModel().asciiBuilt) editBinaryMesh(BinaryMeshPage::Controllers);
                else activePage()->editSelectedControllers();
                return;
            case ID_OwnerVisualEffects: showOwnerVisualEffects();return;
            case ID_SceneEffects: showSceneEffects();return;
            case ID_ConvertMesh:
                activePage()->convertSelectedMesh([this](const wxString& title,const auto& work) {return runJob(title,work);});return;
            case ID_RecalculateNormals:
                activePage()->recalculateSelectedNormals([this](const wxString& title,const auto& work) {return runJob(title,work);});return;
            case ID_BinaryMeshFields:
                editBinaryMesh();return;
            case ID_InspectBinary:
                inspectBinaryFile(this,[this](const wxString& title,const auto& work) {return runJob(title,work);});return;
            case ID_AnalyzeBinaries:
                analyzeBinaryFiles(this,[this](const wxString& title,const auto& work) {return runJob(title,work);});return;
            case ID_CompareBinaries:
                compareBinaryFiles(this,[this](const wxString& title,const auto& work) {return runJob(title,work);});return;
            case ID_CollisionCompanions: activePage()->asciiEditor()->editCollisions(activePage()->document().sourceModel()); return;
            case ID_ModelProperties: activePage()->asciiEditor()->editModel(activePage()->document().sourceModel()); return;
            case ID_RenameTexture: activePage()->asciiEditor()->renameTexture(); return;
            case ID_ExportTextures: activePage()->asciiEditor()->exportTextureList(); return;
            case ID_AdvancedSettings: showAdvancedSettings(); return;
            case ID_DeveloperSettings: showAdvancedSettings(true); return;
            case ID_EngineGraphics: showEngineExecution(); return;
            case ID_EngineReport: saveEngineExecutionReport(); return;
            case ID_AreaReplay: chooseAreaReplay(); return;
            case ID_ResetView:
                activePage()->clearAreaEnvironmentReplay();
                activePage()->setTextureOverride({});
                activePage()->showAllMeshes();
                applyPreviewOptions(options_);
                updateState();
                return;
            case ID_ResetDisplay: copyDisplayOptions(options_,RenderOptions{});applyDisplayOptions();return;
            case ID_LoadArea: chooseArea(); return;
            case ID_ModuleArea: chooseModuleArea(); return;
            case ID_ResourceProvenance: showResourceProvenance(); return;
            case ID_ClearArea:
                if (auto* page=activePage()) page->clearArea();
                updateState();
                return;
            case ID_AreaRoom: chooseAreaRoom(); return;
            case ID_AreaWeather: showAreaWeather(); return;
            case ID_SceneAudio: showSceneAudio(); return;
            case ID_AreaWeatherInputs: showAreaWeather(true); return;
            case ID_SceneAudioInputs: showSceneAudio(true); return;
            case ID_AreaAnimation: chooseAreaAnimation(); return;
            case ID_RoomScriptAnimation: chooseRoomScriptAnimation(); return;
            case ID_DoorState: chooseDoorState(); return;
            case ID_LoadGrass: chooseGrassArea(); return;
            case ID_CloseModel: closePage(tabs_->GetSelection()); return;
            case ID_CloseAll: closeAll(); return;
            case ID_FrameModel:
                if (auto* page = activePage()) page->frameModel();
                updateState();
                return;
            case ID_Textures: options_.textures = event.IsChecked(); applyDisplayOptions(); return;
            case ID_LoadTextureOverride: chooseTextureOverride(); return;
            case ID_ClearTextureOverride: activePage()->setTextureOverride({}); updateState(); return;
            case ID_Lightmaps: options_.lightmaps = event.IsChecked(); applyDisplayOptions(); return;
            case ID_Wireframe: options_.wireframe = event.IsChecked(); applyDisplayOptions(); return;
            case ID_Lighting: options_.lighting = event.IsChecked(); applyDisplayOptions(); return;
            case ID_Headlight: options_.headlight=event.IsChecked();applyDisplayOptions();return;
            case ID_Fog:
                options_.fogEnabled=event.IsChecked();
                applyDisplayOptions();return;
            case ID_LightingSettings: showLightingSettings();return;
            case ID_Grid: options_.grid = event.IsChecked(); applyDisplayOptions(); return;
            case ID_Axes: options_.axes = event.IsChecked(); applyDisplayOptions(); return;
            case ID_ShowHidden: options_.showHidden = event.IsChecked(); applyDisplayOptions(); return;
            case ID_IsolateMeshes: activePage()->isolateSelectedMeshes(event.IsChecked());updateState();return;
            case ID_HideMeshes: activePage()->hideSelectedMeshes();updateState();return;
            case ID_ShowAllMeshes: activePage()->showAllMeshes();updateState();return;
            case ID_CullBackfaces: options_.cullBackfaces = event.IsChecked(); applyDisplayOptions(); return;
            case ID_DanglySimulation:
                options_.danglySimulation = event.IsChecked(); applyDisplayOptions(); return;
            case ID_NodeHelpers:
                options_.nodeHelpers = event.IsChecked(); applyDisplayOptions(); return;
            case ID_EngineShadows:
                setPreviewOption(&RenderOptions::engineShadows,std::optional<bool>{event.IsChecked()}); return;
            case ID_PreviewLights:
                options_.previewLights = event.IsChecked(); applyDisplayOptions(); return;
            case ID_DetonateEmitters: if(auto* page=activePage()) page->detonateEmitters();return;
            case ID_PreviewEmitters:
                options_.previewEmitters = event.IsChecked(); applyDisplayOptions(); return;
            case ID_ShowCameras:
                options_.showCameras = event.IsChecked(); applyDisplayOptions(); return;
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
                if (auto* page = activePage()) page->useViewerCamera(false);
                updateState();
                return;
            case ID_FreeFlyCamera:
                if (auto* page = activePage()) page->useViewerCamera(!page->freeFlyCameraActive());
                updateState();
                return;
            case ID_CameraControls:
                wxMessageBox(
                    "Left click: select a mesh; Shift-click: add or remove a mesh\n"
                    "Click empty space to clear selection\n\n"
                    "Orbit\n"
                    "  Left drag: rotate around the pivot\n"
                    "  Right or middle drag: pan\n"
                    "  Mouse wheel: move closer or farther, continuing past the pivot\n\n"
                    "Free Fly (Shift+F to toggle)\n"
                    "  Click the viewport to give it keyboard focus\n"
                    "  W/S: forward/backward; A/D: left/right\n"
                    "  Q/E: down/up; hold Shift: move four times faster\n"
                    "  Left or right drag: look around; middle drag: pan\n"
                    "  Mouse wheel up/down: increase/decrease movement speed\n\n"
                    "F: return to Orbit and frame the model or area.",
                    "Camera Navigation", wxOK | wxICON_INFORMATION, this);
                return;
            case ID_CameraSettings: showCameraSettings(); return;
            case ID_ShowReferences:
                options_.showReferences = event.IsChecked(); applyDisplayOptions(); return;
            case ID_PreviewLightsabers:
                options_.previewLightsabers = event.IsChecked(); applyDisplayOptions(); return;
            case ID_PreviewAabbs:
                options_.previewAabbs = event.IsChecked(); applyDisplayOptions(); return;
            case ID_PreviewTriggers:
                options_.previewTriggers = event.IsChecked(); applyDisplayOptions(); return;
            case ID_About:
                wxMessageBox("NeoMDL " NEOMDL_VERSION "\n\nModel viewer, ASCII editor and ASCII / binary MDL converter for Knights of the Old Republic I and II.\n\nEdit an individual model's ASCII beside its 3D preview. Inspect geometry, materials, animations, particles, lightsabers and area layouts. Model dependencies resolve through the model directory, the active resource session or a selected game installation.",
                             "About NeoMDL", wxOK | wxICON_INFORMATION, this);
                return;
            case ID_Workflow:
                wxMessageBox(
                    "Open a model, then click a mesh in the viewport or select a node in the tree.\n\n"
                    "Binary editing: use File > Binary Mesh Editor to edit a model file. Save Copy writes a new MDL/MDX pair and opens it.\n\n"
                    "ASCII editing: generate Standard or Advanced ASCII for a binary model, or open existing ASCII. Edits update the preview after a short pause. Save ASCII writes source text; File > Compile ASCII to Binary creates the game model. Undo/Redo applies to ASCII edits.\n\n"
                    "Textures or supermodels missing? Choose Tools > Model Resource Installation, then inspect Resource Scope and Load Report. Changing the installation reloads the model and resets its preview.\n\n"
                    "View and Scene effects, camera changes and playback are previews. These settings are not written into the model. Area previews do not edit or save module placements.\n\n"
                    "Quick Convert and batch conversion read files from disk. Save ASCII edits before converting those files.",
                    "Model Editing Workflow", wxOK | wxICON_INFORMATION, this);
                return;
            case ID_Exit: requestModuleClose(); return;
            default: event.Skip(); return;
            }
        });
        Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& event) {
            if (event.GetMenu() == recentFilesMenu_)
                neosettings::populateRecentFilesMenu(*recentFilesMenu_, settings_, ID_RecentFirst, ID_ClearRecent);
            updateState();
            event.Skip();
        });
    }

    void updateState() {
        const auto* active = activePage();
        const bool hasDocument = active != nullptr;
        const auto* editor = active ? active->asciiEditor() : nullptr;
        for (std::size_t index = 0; index < tabs_->GetPageCount(); ++index) {
            const auto* page = pageAt(index);
            auto title = wxui::toWx(page->document().title());
            if (page->asciiEditor() && page->asciiEditor()->modified()) title += " *";
            if (tabs_->GetPageText(index) != title) tabs_->SetPageText(index, title);
        }
        const bool area = active && !active->areaScene().empty();
        enableModuleCommand(ID_Undo, editor && editor->ready() && !area && editor->canUndo());
        enableModuleCommand(ID_Redo, editor && editor->ready() && !area && editor->canRedo());
        enableModuleCommand(ID_Find, editor && editor->ready() && !area);
        const unsigned features = active ? active->previewFeatures() : 0u;
        if (features != previewMenuFeatures_) buildPreviewMenus(features);
        const std::pair<int, const char*> areaCommands[] = {
            {ID_AreaReplay, "Area Camera Replay..."},
            {ID_SceneAudioInputs, "Scene Audio Inputs..."},
            {ID_AreaWeatherInputs, "Area Weather Inputs..."},
        };
        for (const auto& command : areaCommands) {
            if (area && !developerMenu_->FindItem(command.first))
                developerMenu_->Append(command.first, command.second);
            else if (!area && developerMenu_->FindItem(command.first))
                developerMenu_->Destroy(command.first);
        }
        enableModuleCommand(ID_SaveAscii,editor&&editor->ready());
        enableModuleCommand(ID_SaveAsciiAs,editor&&editor->ready());
        enableModuleCommand(ID_AsciiEditor,editor&&!area);
        enableModuleCommand(ID_ExportAscii,editor!=nullptr);
        enableModuleCommand(ID_ExportPacked,editor&&editor->ready());
        enableModuleCommand(ID_NodeProperties,active&&(active->canEditNode()||active->canEditBinaryNode()));
        enableModuleCommand(ID_NodeTransform,active&&active->canEditBinaryNode());
        enableModuleCommand(ID_VertexProperties,active&&(active->document().sourceModel().asciiBuilt ? active->canEditVertices() : active->canEditBinaryVertices()));
        enableModuleCommand(ID_ReplaceMesh,active&&active->canReplaceMesh());
        enableModuleCommand(ID_ConvertMesh,active&&active->canConvertMesh());
        enableModuleCommand(ID_RecalculateNormals,active&&active->canRecalculateNormals());
        enableModuleCommand(ID_ControllerProperties,active&&(active->document().sourceModel().asciiBuilt ? active->canEditNode() : active->canEditBinaryControllers()));
        enableModuleCommand(ID_OwnerVisualEffects,active&&active->document().hasModelOwner());
        enableModuleCommand(ID_SceneEffects,active&&active->document().isOpen());
        enableModuleCommand(ID_CollisionCompanions,editor&&editor->previewCurrent()&&!area);
        enableModuleCommand(ID_ModelProperties,editor&&editor->previewCurrent()&&!area);
        enableModuleCommand(ID_RenameTexture,editor&&editor->previewCurrent()&&!area);
        enableModuleCommand(ID_ExportTextures,editor&&editor->previewCurrent()&&!area);
        const bool modelPreview = active && active->document().hasModelOwner() && !area;
        enableModuleCommand(ID_IsolateMeshes,modelPreview&&(active->hasSelectedMeshes()||active->meshesIsolated()));
        enableModuleCommand(ID_HideMeshes,modelPreview&&active->hasSelectedMeshes());
        enableModuleCommand(ID_ShowAllMeshes,modelPreview);
        enableModuleCommand(ID_LoadTextureOverride, modelPreview);
        enableModuleCommand(ID_ClearTextureOverride, modelPreview && active->document().textureOverride());
        enableModuleCommand(ID_ResourceProvenance,hasDocument);
        const bool looseModel = active && !active->document().path().empty() && !area;
        enableModuleCommand(ID_Reload, looseModel);
        enableModuleCommand(ID_ModelResources, looseModel);
        enableModuleCommand(ID_LoadArea,hasDocument&&engineAreaRuntimeSupported(activePage()->document().model().game));
        enableModuleCommand(ID_ClearArea,hasDocument&&!activePage()->areaScene().empty());
        enableModuleCommand(ID_AreaAnimation,hasDocument&&!activePage()->areaScene().objects().empty());
        enableModuleCommand(ID_AreaRoom,activePage()&&!activePage()->areaScene().empty());
        enableModuleCommand(ID_SceneAudio,activePage()&&!activePage()->areaScene().empty());
        enableModuleCommand(ID_SceneAudioInputs,area);
        enableModuleCommand(ID_RoomScriptAnimation,activePage()&&!activePage()->areaScene().rooms().empty());
        enableModuleCommand(ID_AreaWeather,activePage()&&!activePage()->areaScene().empty()&&
            weatherRuntimeSupported(activePage()->areaScene().game()));
        enableModuleCommand(ID_AreaWeatherInputs,area&&weatherRuntimeSupported(active->areaScene().game()));
        enableModuleCommand(ID_CloseModel, hasDocument);
        enableModuleCommand(ID_CloseAll, tabs_->GetPageCount() != 0u);
        enableModuleCommand(ID_FrameModel, hasDocument);
        enableModuleCommand(ID_DetonateEmitters, hasDocument && options_.previewEmitters);
        const bool grassArea=active&&!active->areaScene().empty()&&
            engineAreaRuntimeSupported(active->areaScene().game());
        enableModuleCommand(ID_LoadGrass,grassArea);
        const bool engineScene=active &&
            !model::engineSceneStageOrder(active->document().model().game).empty();
        enableModuleCommand(ID_DoorState,active&&std::any_of(active->areaScene().objects().begin(),
            active->areaScene().objects().end(),[](const auto& object){return object->kind()==AreaObjectKind::Door;}));
        enableModuleCommand(ID_AdvancedSettings,engineScene);
        enableModuleCommand(ID_DeveloperSettings,engineScene);
        enableModuleCommand(ID_EngineGraphics,engineScene);
        enableModuleCommand(ID_EngineReport,engineScene);
        enableModuleCommand(ID_ResetView,hasDocument);
        enableModuleCommand(ID_LightingSettings,hasDocument);
        enableModuleCommand(ID_EngineShadows,active&&active->areaScene().currentRoom().has_value());
        enableModuleCommand(ID_ViewSelectedCamera,
                            active && active->canViewThroughSelectedCamera());
        enableModuleCommand(ID_ViewCameraHook, active && active->hasCameraHook());
        enableModuleCommand(ID_ViewAreaCamera,active&&!active->cameraScene().areaCameras().empty());
        enableModuleCommand(ID_OrbitCamera, hasDocument);
        enableModuleCommand(ID_FreeFlyCamera, hasDocument);
        enableModuleCommand(ID_CameraSettings,active&&active->cameraScene().engineRuntime());
        updateMenuChecks();
        if (const auto* page = active) {
            const auto& document = page->document();
            setModuleStatusText((area && document.hasModelOwner() ? "Area preview | " : "") +
                std::string(page->sourceFormat()) + " | " + neomdl::model::modelSummary(document.model()), 0);
            std::ostringstream right;
            right << document.loadedTextureCount() << " texture(s)";
            if (document.textureOverride())
                right << " | Texture override: " << neoshared::pathToUtf8(document.textureOverride()->sourcePath.filename());
            if (document.supermodelCount() != 0u)
                right << " | " << document.supermodelCount() << " supermodel(s)";
            if (!document.references().empty())
                right << " | " << document.references().size() << " reference(s)";
            if (!document.messages().empty()) right << " | " << document.messages().size() << " diagnostic(s)";
            setModuleStatusText(right.str(), 1);
            setModuleTitle(wxString("NeoMDL - ") + wxui::toWx(document.title()) +
                (editor && editor->modified() ? " *" : ""));
        } else {
            setModuleStatusText("Open an MDL model to begin.", 0);
            setModuleStatusText("ASCII editor and 3D preview", 1);
            setModuleTitle("NeoMDL");
        }
    }

    wxAuiNotebook* tabs_{};
    RenderOptions options_;
    int asciiNormalMode_{};
    neosettings::AppSettings settings_{"NeoMDL"};
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    wxMenu* recentFilesMenu_{};
    double fontScale_{neoview::kDefaultFontScale};
    bool dark_{};
    wxMenu* viewMenu_{};
    wxMenu* sceneMenu_{};
    wxMenu* developerMenu_{};
    unsigned previewMenuFeatures_{};
    wxMenu* helpMenu_{};
    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;
    std::shared_ptr<const neoshared::bif::ModFolder> modFolder_;
    std::map<std::string, std::shared_ptr<GameResourceSession>> installationSessions_;
    bool jobRunning_{};
    std::atomic<bool> cancelJob_{false};
};

} // namespace

ModelEditorPanel* createModelEditorPanel(wxWindow* parent, neomodules::Context context) {
    return new ModelEditorPanelImpl(parent, std::move(context));
}

} // namespace neomdl::ui
