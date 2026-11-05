#include "wx/ModelEditorPanel.hpp"

#include "NeoSettings.hpp"
#include "NeoWxUi.hpp"
#include "neomdl_icon.xpm"

#include <wx/app.h>
#include <wx/dnd.h>
#include <wx/frame.h>
#include <wx/icon.h>
#include <wx/image.h>
#include <wx/sizer.h>

#include <vector>

namespace {

class ModelDropTarget final : public wxFileDropTarget {
public:
    explicit ModelDropTarget(neomdl::ui::ModelEditorPanel* panel) : panel_(panel) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) override {
        bool opened = false;
        for (const auto& file : files)
            opened = panel_->openFile(neosettings::pathFromWx(file)) || opened;
        return opened;
    }
private:
    neomdl::ui::ModelEditorPanel* panel_;
};

class NeoMDLFrame final : public wxFrame {
public:
    NeoMDLFrame() : wxFrame(nullptr, wxID_ANY, "NeoMDL") {
        SetIcon(wxIcon(neomdl_icon_xpm));

        neomodules::Context context;
        context.titleChanged = [this](const wxString& title) { SetTitle(title); };
        context.closeRequested = [this] { Close(); };
        panel_ = neomdl::ui::createModelEditorPanel(this, std::move(context));
        SetDropTarget(new ModelDropTarget(panel_));
        SetMenuBar(panel_->takeMenus().release());
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(panel_, 1, wxEXPAND);
        SetSizer(layout);
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            if (!neomodules::routeCommand({panel_}, event)) event.Skip();
        });
        Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& event) {
            neomodules::routeMenuOpen({panel_}, event);
            event.Skip();
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
            if (!panel_->canClose()) {
                // A loading worker must finish joining before its owning
                // panel is destroyed, including a forced programmatic close.
                if(event.CanVeto())event.Veto();
                return;
            }
            settings_.saveWindowPlacement(*this);
            event.Skip();
        });
        wxui::configureResponsiveWindow(*this, wxSize(1180, 780), wxSize(700, 460));
        settings_.restoreWindowPlacement(*this);
    }

    ~NeoMDLFrame() override { DestroyChildren(); }
    void openStartup(const std::filesystem::path& path) { panel_->openFile(path); }

private:
    neomdl::ui::ModelEditorPanel* panel_{};
    neosettings::AppSettings settings_{"NeoMDL"};
};

class NeoMDLApp final : public wxApp {
public:
    bool OnInit() override {
        SetAppName("NeoMDL");
        SetVendorName("Neo Tools");
        wxInitAllImageHandlers();
        auto* frame = new NeoMDLFrame;
        frame->Show();
        if (argc > 1) {
            std::vector<std::filesystem::path> paths;
            for (int index = 1; index < argc; ++index)
                paths.push_back(neosettings::pathFromWx(wxString(argv[index])));
            frame->CallAfter([frame, paths] {
                for (const auto& path : paths) frame->openStartup(path);
            });
        }
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoMDLApp);
