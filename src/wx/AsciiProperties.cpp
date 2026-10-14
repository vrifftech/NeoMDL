#include "wx/AsciiEditor.hpp"
#include "wx/AsciiSource.hpp"
#include "core/MeshConversion.hpp"
#include "core/MeshNormals.hpp"
#include <model/AsciiModelInternal.hpp>
#include <model/AsciiControllerFields.hpp>
#include <neoshared/texture/FileUtil.hpp>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>

namespace neomdl::ui {
namespace {

std::string vectorText(std::initializer_list<float> values) {
    std::ostringstream output;
    output << std::setprecision(9);
    for (auto value : values) output << value << ' ';
    auto text = output.str();
    text.pop_back();
    return text;
}

wxTextCtrl* textField(wxDialog& dialog, wxFlexGridSizer& grid,
                      const char* label, const std::string& value) {
    grid.Add(new wxStaticText(&dialog, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto* input = new wxTextCtrl(&dialog, wxID_ANY, wxui::toWx(value));
    grid.Add(input, 1, wxEXPAND);
    return input;
}

} // namespace

void AsciiEditor::editNode(const model::Model& asset, std::size_t nodeIndex) {
    const auto& node = asset.nodes[nodeIndex];
    SourceText source(text());
    const auto blocks = source.nodes();
    const auto& block = *std::find_if(blocks.begin(), blocks.end(), [&](const auto& item) {
        return item.geometry && source.matches(item, node, nodeIndex);
    });
    wxDialog dialog(this, wxID_ANY, "Node Properties — " + wxui::toWx(node.name),
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, FromDIP(7), FromDIP(10));
    grid->AddGrowableCol(1);
    Edits edits;
    const auto scalar = [&](const char* label, const std::string& name, const std::string& initial) {
        auto* control = textField(dialog, *grid, label, initial);
        control->Bind(wxEVT_TEXT, [&, control, name](wxCommandEvent&) {
            edits[name] = wxui::toStd(control->GetValue());
        });
        return control;
    };
    model::Vec3 position;
    model::Quaternion rotation;
    model::constructedNodeFrame(node, position, rotation, asset.game);
    auto* move = scalar("Position (X Y Z)", "position", vectorText({position.x, position.y, position.z}));
    const auto orientation = source.field(block, "orientation");
    auto* rotate = advanced_ ? scalar("Rotation (quaternion X Y Z W)", "nodeorientation",
        vectorText({rotation.x, rotation.y, rotation.z, rotation.w})) :
        scalar("Rotation (axis X Y Z, angle in radians)", "orientation", orientation.empty() ? "1 0 0 0" : orientation);
    const auto scaleValue = source.field(block, "scale");
    auto* scale = scalar("Uniform scale", "scale", scaleValue.empty() ? "1" : scaleValue);
    const bool rawControllers = !source.field(block, "controllerdata").empty();
    const auto staticController = [&](std::uint32_t type) {
        return !rawControllers && std::all_of(node.controllers.begin(), node.controllers.end(), [&](const auto& c) {
            return c.type != type || (c.rowCount == 1 && c.sourceOffset == -1 &&
                node.controllerDataWords[c.timeIndex] == 0u && !(c.rawFlags & 0x10u));
        });
    };
    move->Enable(staticController(model::kControllerPosition));
    rotate->Enable(staticController(model::kControllerOrientation));
    scale->Enable(staticController(model::kControllerScale));
    move->SetToolTip("Edits the base position and stored node position. Use Controller Properties for animated controllers.");
    rotate->SetToolTip("Edits the base rotation and stored node orientation. Use Controller Properties for animated controllers.");
    bool regenerate = false;
    bool tangentChanged = false;
    wxCheckBox* tangent = nullptr;
    if (node.mesh) {
        const auto& mesh = asset.meshes[*node.mesh];
        scalar("Base texture", "texture0", mesh.texture0);
        scalar("Lightmap / secondary texture", "texture1", mesh.texture1);
        const auto flag = [&](const char* label, const std::string& name, bool value) {
            auto* check = new wxCheckBox(&dialog, wxID_ANY, label);
            check->SetValue(value);
            grid->Add(check, 0, wxALIGN_CENTER_VERTICAL); grid->AddSpacer(1);
            check->Bind(wxEVT_CHECKBOX, [&, check, name](wxCommandEvent&) {
                edits[name] = check->GetValue() ? "1" : "0";
            });
            return check;
        };
        flag("Render", "render", mesh.render);
        flag("Cast shadows", "shadow", mesh.castsShadow);
        flag("Lightmapped", "lightmapped", mesh.lightmapped);
        flag("Rotate texture", "rotatetexture", mesh.rotateTexture);
        flag("Beaming", "beaming", mesh.beaming);
        flag("Background geometry", "m_bisbackgroundgeometry", mesh.backgroundGeometry);
        if (node.aabb) {
            auto* rebuild = new wxCheckBox(&dialog, wxID_ANY, "Rebuild AABB tree on Apply");
            grid->Add(rebuild, 0, wxALIGN_CENTER_VERTICAL); grid->AddSpacer(1);
            rebuild->Bind(wxEVT_CHECKBOX, [&, rebuild](wxCommandEvent&) {
                if (rebuild->GetValue()) edits["aabb"] = std::nullopt;
                else edits.erase("aabb");
            });
        }
        if (advanced_ && !node.lightsaber && !mesh.animated) {
            tangent = flag("Has tangent basis", "hastangentbasis", (mesh.mdxBitmap & 0x80u) != 0);
            auto* rebuild = new wxCheckBox(&dialog, wxID_ANY, "Regenerate tangent basis on Apply");
            grid->Add(rebuild, 0, wxALIGN_CENTER_VERTICAL); grid->AddSpacer(1);
            tangent->Bind(wxEVT_CHECKBOX, [&, rebuild](wxCommandEvent& event) {
                tangentChanged = true;
                if (!tangent->GetValue()) { regenerate = false; rebuild->SetValue(false); }
                event.Skip();
            });
            rebuild->Bind(wxEVT_CHECKBOX, [&, rebuild](wxCommandEvent&) {
                regenerate = rebuild->GetValue();
                if (regenerate) { tangent->SetValue(true); edits["hastangentbasis"] = "1"; tangentChanged = true; }
            });
        }
        if (advanced_ && node.lightsaber) {
            auto* rebuild = new wxCheckBox(&dialog, wxID_ANY, "Regenerate saber arrays from geometry on Apply");
            rebuild->SetToolTip("Replaces retained saber arrays using verts, normals, tverts, faces and selector colors. The whole change can be undone.");
            grid->Add(rebuild, 0, wxALIGN_CENTER_VERTICAL); grid->AddSpacer(1);
            rebuild->Bind(wxEVT_CHECKBOX, [&, rebuild](wxCommandEvent&) {
                for (const auto* field : {"saberverts", "sabernormals", "saberuvs", "saberoriginalverts"})
                    if (rebuild->GetValue()) edits[field] = std::nullopt;
                    else edits.erase(field);
            });
        }
    }
    layout->Add(grid, 1, wxEXPAND | wxALL, FromDIP(12));
    if (!advanced_ && node.mesh && !node.lightsaber && !asset.meshes[*node.mesh].animated)
        layout->Add(new wxStaticText(&dialog, wxID_ANY,
            "Tangent controls are available in Advanced ASCII."), 0, wxALL, FromDIP(12));
    if (!move->IsEnabled() || !rotate->IsEnabled() || !scale->IsEnabled())
        layout->Add(new wxStaticText(&dialog, wxID_ANY,
            "Keyed or raw transforms are edited in Controller Properties."), 0, wxALL, FromDIP(12));
    layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(12));
    dialog.FindWindow(wxID_OK)->SetLabel("Apply");
    dialog.SetSizerAndFit(layout);
    dialog.SetMinSize(wxSize(FromDIP(520), dialog.GetSize().y));
    dialog.SetSize(dialog.GetMinSize());
    dialog.SetAffirmativeId(wxID_OK);
    if (dialog.ShowModal() != wxID_OK || edits.empty()) return;
    if (advanced_ && edits.count("position")) edits["nodeposition"] = edits.at("position");
    if (edits.count("nodeorientation")) {
        edits["orientation"] = std::nullopt;
        edits["orientationpackedkey"] = std::nullopt;
        edits["orientationquatkey"] = "1 -1\n    0 " + *edits.at("nodeorientation");
    }
    if (edits.count("texture0")) edits["bitmap"] = std::nullopt;
    if (tangentChanged) source.materializeTangentPolicies();
    if (regenerate) {
        edits["hastangentbasis"] = "1";
        edits["tangentbasis"] = std::nullopt;
        edits["tangentindices"] = std::nullopt;
    }
    source.editNode(node, nodeIndex, edits);
    replaceText(source.text());
}

void AsciiEditor::convertMesh(const model::Model& asset, std::size_t nodeIndex, const AuthoringJob& runJob) {
    const auto current = text();
    std::string converted;
    if (runJob("Converting mesh to trimesh",[&](const auto& job) {
        converted = convertMeshToTrimesh(current,asset,nodeIndex,advanced_,job);
    })) replaceText(converted);
}

void AsciiEditor::recalculateNormals(const model::Model& asset, std::size_t nodeIndex, const AuthoringJob& runJob) {
    wxDialog dialog(this, wxID_ANY, "Recalculate Normals — " + wxui::toWx(asset.nodes[nodeIndex].name));
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(12));
    grid->AddGrowableCol(1);
    grid->Add(new wxStaticText(&dialog, wxID_ANY, "Weighting"), 0, wxALIGN_CENTER_VERTICAL);
    auto* weighting = new wxChoice(&dialog, wxID_ANY);
    for (const auto* label : {"Face area (default)", "Corner angle", "Face area × corner angle", "Equal faces"})
        weighting->Append(wxString::FromUTF8(label));
    weighting->SetSelection(0);
    grid->Add(weighting, 1, wxEXPAND);
    grid->Add(new wxStaticText(&dialog, wxID_ANY, "Crease angle (degrees)"), 0, wxALIGN_CENTER_VERTICAL);
    auto* crease = new wxSpinCtrlDouble(&dialog, wxID_ANY, "180", wxDefaultPosition, wxDefaultSize,
        wxSP_ARROW_KEYS, 0, 180, 180, 1);
    crease->SetDigits(1);
    grid->Add(crease, 1, wxEXPAND);
    layout->Add(grid, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* groups = new wxCheckBox(&dialog, wxID_ANY, "Respect existing smoothing groups");
    groups->SetValue(true);
    layout->Add(groups, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* explanation = new wxStaticText(&dialog, wxID_ANY,
        "Faces sharing a source vertex blend when their angle is at most the crease angle. "
        "180° adds no angle limit. Clear the checkbox to smooth across group boundaries.\n\n"
        "Apply replaces this mesh's normals and rebuilds its enabled tangent basis. "
        "Undo restores the previous data.");
    explanation->Wrap(FromDIP(430));
    layout->Add(explanation, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(12));
    dialog.FindWindow(wxID_OK)->SetLabel("Apply");
    dialog.SetSizerAndFit(layout);
    if (dialog.ShowModal() != wxID_OK) return;
    const MeshNormalOptions options{static_cast<NormalWeighting>(weighting->GetSelection()),
        crease->GetValue(), groups->GetValue()};
    const auto current = text();
    std::string recalculated;
    if (runJob("Recalculating mesh normals", [&](const auto& job) {
        recalculated = recalculateMeshNormals(current, asset, nodeIndex, options, job);
    })) replaceText(recalculated);
}

void AsciiEditor::editModel(const model::Model& asset) {
    SourceText source(text());
    const bool identities = advanced_ && (!asset.asciiBuilt || asset.nodes.front().asciiIdentity);
    wxDialog dialog(this, wxID_ANY, "Model Properties");
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* root = new wxChoice(&dialog, wxID_ANY);
    std::vector<std::size_t> nodeIndices;
    int geometryRoot = 0, neck = wxNOT_FOUND;
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const auto& node = asset.nodes[i];
        const auto selection = static_cast<int>(nodeIndices.size());
        nodeIndices.push_back(i);
        root->Append(wxui::toWx(node.name + (identities ? " [" + std::to_string(node.asciiIdentity.value_or(i)) + "]" : "") +
            (!node.parent ? " (geometry root)" : "")));
        if (!node.parent) geometryRoot = selection;
        if (model::ascii::equal(node.name, "neck_g")) neck = selection;
        if (asset.animationRootNode == i) root->SetSelection(selection);
    }
    if (!asset.animationRootNode) root->SetSelection(geometryRoot);
    const int initial = root->GetSelection();
    layout->Add(new wxStaticText(&dialog, wxID_ANY, "Animation root"), 0, wxALL, FromDIP(10));
    layout->Add(root, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    auto* link = new wxCheckBox(&dialog, wxID_ANY, "Link head at neck_g");
    link->Enable(neck != wxNOT_FOUND);
    link->SetValue(neck != wxNOT_FOUND && initial == neck);
    layout->Add(link, 0, wxALL, FromDIP(10));
    layout->Add(new wxStaticText(&dialog, wxID_ANY,
        "Selects where animation controller application begins.\n"
        "Head link selects neck_g; clearing it selects the geometry root."), 0, wxALL, FromDIP(10));
    link->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent&) { root->SetSelection(link->GetValue() ? neck : geometryRoot); });
    root->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { link->SetValue(root->GetSelection() == neck); });
    layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(10));
    dialog.SetSizerAndFit(layout);
    if (dialog.ShowModal() != wxID_OK || root->GetSelection() == initial) return;
    std::size_t begin = 0;
    while (SourceText::key(source.lines[begin]) != "beginmodelgeom") ++begin;
    auto end = begin + 1;
    while (SourceText::key(source.lines[end]) != "endmodelgeom") end = source.nextRecord(end);
    const auto selected = nodeIndices[root->GetSelection()];
    const auto& node = asset.nodes[selected];
    source.edit({begin, end, {}, true, {}}, identities ?
        Edits{{"smodelanimationrootname", std::nullopt},
              {"smodelanimationrootidentity", std::to_string(node.asciiIdentity.value_or(selected))}} :
        Edits{{"smodelanimationrootname", node.name}});
    replaceText(source.text());
}

void AsciiEditor::renameTexture() {
    SourceText source(text());
    wxDialog dialog(this, wxID_ANY, "Rename Texture Throughout Model");
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* from = new wxChoice(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, textureNames(source));
    if (from->GetCount()) from->SetSelection(0);
    layout->Add(new wxStaticText(&dialog, wxID_ANY, "Texture"), 0, wxALL, FromDIP(10));
    layout->Add(from, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    layout->Add(new wxStaticText(&dialog, wxID_ANY, "Replacement texture"), 0, wxALL, FromDIP(10));
    auto* to = new wxTextCtrl(&dialog, wxID_ANY);
    layout->Add(to, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(10));
    dialog.FindWindow(wxID_OK)->Enable(false);
    to->Bind(wxEVT_TEXT, [&](wxCommandEvent&) {
        dialog.FindWindow(wxID_OK)->Enable(from->GetSelection() != wxNOT_FOUND && !to->GetValue().empty());
    });
    dialog.SetSizerAndFit(layout);
    if (dialog.ShowModal() != wxID_OK) return;
    const auto name = wxui::toStd(from->GetStringSelection());
    const auto replacement = wxui::toStd(to->GetValue());
    for (const auto& texture : source.textures())
        if (model::ascii::equal(texture.name, name))
            source.lines[texture.line].replace(texture.column, texture.length, replacement);
    replaceText(source.text());
}

void AsciiEditor::exportTextureList() {
    wxFileDialog file(this, "Export Texture List", neosettings::pathToWx(source_->path().parent_path()),
        wxui::toWx(source_->sourceModel().name + "-textures.txt"), "Text files (*.txt)|*.txt",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (file.ShowModal() != wxID_OK) return;
    try {
        const auto path = neosettings::pathFromWx(file.GetPath());
        neoshared::checkResourceOutput(path, source_->openPaths());
        std::string list;
        for (const auto& name : textureNames(SourceText(text()))) list += wxui::toStd(name) + '\n';
        neoshared::texture::writeFileBytes(path, {list.begin(), list.end()});
    } catch (const std::exception& error) { wxui::showError(this, error); }
}

} // namespace neomdl::ui
