#include "wx/AsciiEditor.hpp"
#include "NeoSettings.hpp"
#include "NeoWxUi.hpp"
#include <core/MeshReplacement.hpp>
#include <neoshared/texture/FileUtil.hpp>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace neomdl::ui {

void AsciiEditor::editMeshReplacements(const model::Model& asset, std::size_t nodeIndex,
                                     const AuthoringJob& runJob) {
    wxDialog dialog(this, wxID_ANY, "Replace Meshes", wxDefaultPosition, FromDIP(wxSize(850,540)),
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
    auto* fileName = new wxTextCtrl(&dialog, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    auto* browse = new wxButton(&dialog, wxID_ANY, "Open Donor...");
    fileRow->Add(fileName, 1, wxRIGHT, FromDIP(8)); fileRow->Add(browse);
    layout->Add(fileRow, 0, wxEXPAND | wxALL, FromDIP(10));
    auto* mappingRow = new wxBoxSizer(wxHORIZONTAL);
    auto* target = new wxChoice(&dialog, wxID_ANY);
    auto* donorMesh = new wxChoice(&dialog, wxID_ANY);
    auto* add = new wxButton(&dialog, wxID_ANY, "Queue Replacement");
    mappingRow->Add(new wxStaticText(&dialog, wxID_ANY, "Target mesh"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    mappingRow->Add(target, 1, wxRIGHT, FromDIP(12));
    mappingRow->Add(new wxStaticText(&dialog, wxID_ANY, "Donor mesh"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    mappingRow->Add(donorMesh, 1, wxRIGHT, FromDIP(8)); mappingRow->Add(add);
    layout->Add(mappingRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    auto* queue = new wxListCtrl(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    queue->InsertColumn(0, "Target mesh", wxLIST_FORMAT_LEFT, FromDIP(180));
    queue->InsertColumn(1, "Donor mesh", wxLIST_FORMAT_LEFT, FromDIP(180));
    queue->InsertColumn(2, "Donor file", wxLIST_FORMAT_LEFT, FromDIP(430));
    layout->Add(queue, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    auto* remove = new wxButton(&dialog, wxID_ANY, "Remove Mapping");
    layout->Add(remove, 0, wxALL, FromDIP(10));
    auto* note = new wxStaticText(&dialog, wxID_ANY,
        "Choose meshes of the same type. Geometry, normals, UVs, materials and mesh flags come from the donor.\n"
        "The target keeps its name, hierarchy, transforms and controllers. Donor geometry uses the target's local frame.\n"
        "Skins keep donor bone names and weights and bind to the target skeleton. Texture files are not copied.\n"
        "Queueing the same target updates its mapping. Apply updates the ASCII and preview; save or compile afterward.");
    layout->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(10));
    auto* apply = dialog.FindWindow(wxID_OK);
    apply->SetLabel("Apply Replacements");
    dialog.SetSizer(layout); dialog.SetMinSize(FromDIP(wxSize(820,480)));

    std::vector<std::size_t> targets, donors;
    for (const auto& [name, index] : namedMeshes(asset)) {
        const auto choice = target->Append(wxui::toWx(asset.nodes[index].name));
        targets.push_back(index);
        if (index == nodeIndex) target->SetSelection(choice);
    }
    std::shared_ptr<const MeshDonor> donor;
    std::map<std::string, std::shared_ptr<const MeshDonor>> loaded;
    std::vector<MeshReplacement> mappings;
    const auto selectedRow = [&] { return queue->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); };
    const auto controls = [&] {
        add->Enable(donorMesh->GetSelection() != wxNOT_FOUND);
        remove->Enable(selectedRow() != -1);
        apply->Enable(!mappings.empty());
    };
    const auto showDonor = [&] {
        donorMesh->Clear(); donors.clear();
        if (donor) for (const auto& [name, index] : namedMeshes(donor->model, asset.nodes[targets[target->GetSelection()]].flags)) {
            donorMesh->Append(wxui::toWx(donor->model.nodes[index].name)); donors.push_back(index);
        }
        if (!donors.empty()) donorMesh->SetSelection(0);
        controls();
    };
    const auto showQueue = [&] {
        queue->DeleteAllItems();
        for (std::size_t i = 0; i < mappings.size(); ++i) {
            const auto& mapping = mappings[i];
            queue->InsertItem(i, wxui::toWx(mapping.target));
            queue->SetItem(i, 1, wxui::toWx(mapping.mesh));
            queue->SetItem(i, 2, neosettings::pathToWx(mapping.donor->path));
        }
        controls();
    };
    browse->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        wxFileDialog file(&dialog, "Open Donor Model", {}, {}, "Models (*.mdl)|*.mdl", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (file.ShowModal() != wxID_OK) return;
        try {
            const auto path = neosettings::pathFromWx(file.GetPath());
            const auto key = resourceFileIdentity(path);
            auto found = loaded.find(key);
            if (found == loaded.end()) {
                std::shared_ptr<MeshDonor> next;
                if (!runJob("Loading donor geometry", [&](const auto& job) {
                    next = std::make_shared<MeshDonor>(readMeshDonor(path, asset.game, job));
                })) return;
                found = loaded.emplace(key, std::move(next)).first;
            }
            donor = found->second;
            fileName->ChangeValue(neosettings::pathToWx(donor->path));
            showDonor();
        } catch (const std::exception& error) { wxui::showError(&dialog, error); }
    });
    target->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { showDonor(); });
    donorMesh->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { controls(); });
    queue->Bind(wxEVT_LIST_ITEM_SELECTED, [&](wxListEvent&) { controls(); });
    queue->Bind(wxEVT_LIST_ITEM_DESELECTED, [&](wxListEvent&) { controls(); });
    add->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        MeshReplacement mapping{asset.nodes[targets[target->GetSelection()]].name,
                                donor->model.nodes[donors[donorMesh->GetSelection()]].name, donor};
        const auto found = std::find_if(mappings.begin(), mappings.end(), [&](const auto& value) {
            return value.target == mapping.target;
        });
        if (found == mappings.end()) mappings.push_back(std::move(mapping));
        else *found = std::move(mapping);
        showQueue();
    });
    remove->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        mappings.erase(mappings.begin() + selectedRow()); showQueue();
    });
    dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        try {
            model::AsciiExportResult replacement;
            const auto current = text();
            if (!runJob("Replacing meshes", [&](const auto& job) {
                replacement = replaceMeshes(current, asset, mappings, advanced_, job);
            })) return;
            for (const auto& mapping : mappings)
                replacementInputs_.insert(replacementInputs_.end(), mapping.donor->inputs.begin(), mapping.donor->inputs.end());
            replaceText(replacement.text);
            dialog.EndModal(wxID_OK);
        } catch (const std::exception& error) { wxui::showError(&dialog, error); }
    }, wxID_OK);
    showDonor();
    wxui::applyTheme(&dialog, wxui::currentDarkMode());
    dialog.ShowModal();
}

} // namespace neomdl::ui
