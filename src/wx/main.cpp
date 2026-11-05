#include "wx/ModelEditorPanel.hpp"

#include "NeoSettings.hpp"
#include "NeoWxUi.hpp"
#include "neomdl_icon.xpm"

#include <wx/app.h>
#include <wx/frame.h>
#include <wx/icon.h>
#include <wx/image.h>
#include <wx/sizer.h>

namespace {

class NeoMDLFrame final : public wxFrame {
public:
    NeoMDLFrame() : wxFrame(nullptr, wxID_ANY, "NeoMDL") {
        SetIcon(wxIcon(neomdl_icon_xpm));

        neomodules::Context context;
        context.titleChanged = [this](const wxString& title) { SetTitle(title); };
        context.closeRequested = [this] { Close(); };
        panel_ = neomdl::ui::createModelEditorPanel(this, std::move(context));
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
            const auto path = neosettings::pathFromWx(wxString(argv[1]));
            frame->CallAfter([frame, path] { frame->openStartup(path); });
        }
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoMDLApp);
