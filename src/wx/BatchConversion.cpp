#include "wx/BatchConversion.hpp"
#include "wx/ConversionSettings.hpp"
#include "NeoWxUi.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filepicker.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace neomdl::ui {

void chooseConversionNaming(wxWindow* parent) {
    wxArrayString choices;
    for (const auto* name : asciiNamingLabels) choices.Add(name);
    wxSingleChoiceDialog dialog(parent, "Default filename for new ASCII saves and conversions", "ASCII Filenames", choices);
    dialog.SetSelection(static_cast<int>(asciiNamingPreference()));
    if (dialog.ShowModal() == wxID_OK) saveAsciiNamingPreference(static_cast<AsciiNaming>(dialog.GetSelection()));
}

void showBatchConversion(wxWindow* parent, ConversionMode mode, const AuthoringJob& runJob,
                         const OutputValidator& validateOutputs) {
    const auto title = mode == ConversionMode::Ascii ? "Batch Decompile" :
        mode == ConversionMode::Binary ? "Batch Compile" : "Quick Convert";
    wxDialog dialog(parent, wxID_ANY, title, wxDefaultPosition, parent->FromDIP(wxSize(900,560)),
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    const auto dip = [&](int size) { return dialog.FromDIP(size); };
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* files = new wxListCtrl(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
    files->InsertColumn(0, "Input model", wxLIST_FORMAT_LEFT, dip(490));
    files->InsertColumn(1, "Result", wxLIST_FORMAT_LEFT, dip(340));
    layout->Add(files, 1, wxEXPAND | wxALL, dip(10));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* add = new wxButton(&dialog, wxID_ANY, "Add Models...");
    auto* remove = new wxButton(&dialog, wxID_ANY, "Remove Selected");
    row->Add(add, 0, wxRIGHT, dip(8)); row->Add(remove);
    layout->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, dip(10));
    layout->Add(new wxStaticText(&dialog, wxID_ANY, "Output folder"), 0, wxLEFT | wxRIGHT, dip(10));
    auto* directory = new wxDirPickerCtrl(&dialog, wxID_ANY, {}, "Choose output folder",
        wxDefaultPosition, wxDefaultSize, wxDIRP_DEFAULT_STYLE | wxDIRP_USE_TEXTCTRL);
    layout->Add(directory, 0, wxEXPAND | wxALL, dip(10));
    auto* settings = new wxBoxSizer(wxHORIZONTAL);
    settings->Add(new wxStaticText(&dialog, wxID_ANY, "Game for ASCII compilation"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dip(6));
    auto* game = new wxChoice(&dialog, wxID_ANY); game->Append("K1"); game->Append("K2"); game->SetSelection(0);
    settings->Add(game, 0, wxRIGHT, dip(18));
    auto* naming = new wxChoice(&dialog, wxID_ANY);
    for (const auto* name : asciiNamingLabels) naming->Append(name);
    naming->SetSelection(static_cast<int>(asciiNamingPreference()));
    settings->Add(new wxStaticText(&dialog, wxID_ANY, "ASCII filename"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dip(6));
    settings->Add(naming, 1);
    layout->Add(settings, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dip(10));
    auto* optionsRow = new wxBoxSizer(wxHORIZONTAL);
    auto* advanced = new wxCheckBox(&dialog, wxID_ANY, "Advanced ASCII");
    auto* geometry = new wxCheckBox(&dialog, wxID_ANY, "Geometry only");
    auto* companions = new wxCheckBox(&dialog, wxID_ANY, "Include collision companions"); companions->SetValue(true);
    optionsRow->Add(advanced, 0, wxRIGHT, dip(16)); optionsRow->Add(geometry, 0, wxRIGHT, dip(16)); optionsRow->Add(companions);
    layout->Add(optionsRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, dip(10));
    auto* note = new wxStaticText(&dialog, wxID_ANY, mode == ConversionMode::Opposite ?
        "Quick Convert reads each file and writes its opposite format. Files on disk are used; open editor changes are not included.\n"
        "Each output gets a .report.txt file. Models run in order; completed files remain if cancelled." :
        "Files on disk are used; open editor changes are not included. Each output gets a .report.txt file.\n"
        "Models run in order. Failed models are reported; completed files remain if cancelled.");
    layout->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, dip(10));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* convert = new wxButton(&dialog, wxID_ANY, "Convert");
    auto* close = new wxButton(&dialog, wxID_CANCEL, "Close");
    buttons->AddStretchSpacer(); buttons->Add(convert, 0, wxRIGHT, dip(8)); buttons->Add(close);
    layout->Add(buttons, 0, wxEXPAND | wxALL, dip(10));
    dialog.SetSizer(layout); dialog.SetMinSize(parent->FromDIP(wxSize(820,510)));
    std::vector<std::filesystem::path> paths;
    const auto controls = [&] {
        remove->Enable(files->GetSelectedItemCount() != 0);
        convert->Enable(!paths.empty() && !directory->GetPath().empty());
        advanced->Enable(mode != ConversionMode::Binary); naming->Enable(mode != ConversionMode::Binary);
    };
    add->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        wxFileDialog file(&dialog, "Add Models", {}, {},
            "Models (*.mdl;*.mdl.ascii)|*.mdl;*.MDL;*.mdl.ascii;*.MDL.ASCII", wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
        if (file.ShowModal() != wxID_OK) return;
        wxArrayString selected; file.GetPaths(selected);
        for (const auto& name : selected) {
            const auto path = neosettings::pathFromWx(name);
            if (std::find(paths.begin(), paths.end(), path) != paths.end()) continue;
            files->InsertItem(paths.size(), name); paths.push_back(path);
        }
        if (directory->GetPath().empty()) directory->SetPath(neosettings::pathToWx(paths.front().parent_path()));
        controls();
    });
    remove->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        for (long i = files->GetItemCount() - 1; i >= 0; --i)
            if (files->GetItemState(i, wxLIST_STATE_SELECTED)) { paths.erase(paths.begin() + i); files->DeleteItem(i); }
        controls();
    });
    files->Bind(wxEVT_LIST_ITEM_SELECTED, [&](wxListEvent&) { controls(); });
    files->Bind(wxEVT_LIST_ITEM_DESELECTED, [&](wxListEvent&) { controls(); });
    directory->Bind(wxEVT_DIRPICKER_CHANGED, [&](wxFileDirPickerEvent&) { controls(); });
    convert->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        try {
            ConversionOptions options; options.game = game->GetSelection() == 0 ? model::GameVersion::Kotor1 : model::GameVersion::Kotor2;
            options.ascii.advanced = advanced->GetValue(); options.ascii.includeAnimations = !geometry->GetValue();
            options.includeCollisions = companions->GetValue();
            const auto style = static_cast<AsciiNaming>(naming->GetSelection());
            const auto destination = neosettings::pathFromWx(directory->GetPath());
            std::vector<ConversionRequest> requests;
            std::vector<std::filesystem::path> inputs, outputs;
            {
                ResourceLoadScope planning(resourceLoadJob());
                for (const auto& path : paths) {
                    requests.push_back(planConversion(path, mode, destination, style));
                    auto source = conversionInputs(requests.back()); inputs.insert(inputs.end(), source.begin(), source.end());
                    auto target = conversionOutputs(requests.back(), options); outputs.insert(outputs.end(), target.begin(), target.end());
                }
                validateOutputs(outputs, inputs);
            }
            if (!confirmCompanionOverwrite(&dialog, outputs)) return;
            saveAsciiNamingPreference(style);
            std::filesystem::create_directories(destination);
            for (long i = 0; i < files->GetItemCount(); ++i) files->SetItem(i, 1, "Queued");
            std::vector<ConversionReport> results;
            wxString interrupted = "Cancelled";
            try {
                runJob(title, [&](const auto& job) {
                    for (std::size_t i = 0; i < requests.size(); ++i) {
                        auto current = job;
                        current.progress = [&, i](std::size_t, std::size_t, const std::string& detail) {
                            job.update(i, requests.size(), std::to_string(i + 1) + "/" + std::to_string(requests.size()) + ": " + detail);
                        };
                        current.update(0, 0, "Converting " + neoshared::pathToUtf8(requests[i].input.filename()));
                        results.push_back(convertFile(requests[i], options, inputs, current));
                    }
                });
            } catch (const std::exception& error) {
                interrupted = wxui::toWx(std::string("Stopped: ") + error.what());
                wxui::showError(&dialog, error);
            }
            for (std::size_t i = 0; i < results.size(); ++i) {
                const auto& result = results[i];
                files->SetItem(i, 1, result.error.empty() ? wxString::Format("Converted (%.2f s)", result.seconds) : wxui::toWx("Failed: " + result.error));
            }
            for (std::size_t i = results.size(); i < paths.size(); ++i)
                files->SetItem(i, 1, i == results.size() ? interrupted : wxString("Not run"));
        } catch (const std::exception& error) { wxui::showError(&dialog, error); }
    });
    controls(); wxui::applyTheme(&dialog, wxui::currentDarkMode()); dialog.ShowModal();
}

} // namespace neomdl::ui
