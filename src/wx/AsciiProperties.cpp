#include "wx/AsciiEditor.hpp"
#include "NeoWxUi.hpp"
#include <model/AsciiModelInternal.hpp>
#include <model/AsciiControllerFields.hpp>
#include <neoshared/texture/FileUtil.hpp>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>

namespace neomdl::ui {
namespace {

using Edits = std::map<std::string, std::optional<std::string>>;

// Focused edits retain every untouched line and go through the normal compiler.
// These are source ranges, not a second model representation.
struct SourceText {
    struct Block { std::size_t begin, end; std::string name; bool geometry; };
    struct Texture { std::size_t line, column, length; std::string name; bool policy; };
    std::vector<std::string> lines;

    explicit SourceText(const std::string& text) {
        std::istringstream input(text);
        for (std::string line; std::getline(input, line);) lines.push_back(std::move(line));
    }
    static std::string key(const std::string& line) {
        std::istringstream input(line);
        std::string result;
        input >> result;
        return model::ascii::lower(result);
    }
    static std::string value(const std::string& line) {
        const auto begin = line.find_first_not_of(" \t");
        const auto wordEnd = line.find_first_of(" \t\r", begin);
        const auto start = line.find_first_not_of(" \t\r", wordEnd);
        return start == std::string::npos ? std::string{} : line.substr(start);
    }
    std::size_t nextRecord(std::size_t line) const {
        const auto name = key(lines[line]);
        if (name == "node") {
            for (++line; key(lines[line]).compare(0, 7, "endnode") != 0; line = nextRecord(line)) {}
            return line + 1;
        }
        if (name == "vertexindices") {
            std::size_t count;
            std::istringstream(value(lines[line])) >> count;
            for (std::size_t i = 0; i < count; ++i) {
                std::size_t rows;
                std::istringstream(lines[++line]) >> rows;
                line += rows;
            }
            return line + 1;
        }
        static const std::vector<std::string> arrays{
            "verts", "normals", "normalindices", "tverts", "tverts1", "tverts2", "tverts3",
            "texindices1", "texindices2", "texindices3", "colors", "colorsrgba", "colorindices",
            "tangentbasis", "tangentindices", "cpuverts", "cpuindices", "faceplanes", "faces",
            "vertexindicescount", "leftoverfaces", "constraints", "animverts", "animtverts",
            "tangents", "mirrorlist", "texturenames", "flaresizes", "flarepositions", "flarecolorshifts",
            "weights", "qbone_ref_inv", "tbone_ref_inv", "boneconstantindices", "skinbonemap",
            "skinpalette", "skinbindrotations", "skinbindtranslations", "skinweights", "controllerdata"};
        const auto keyword = std::string_view(name);
        const auto suffix = name.size() >= 9 && name.compare(name.size() - 9, 9, "bezierkey") == 0 ? 9u :
            name.size() >= 3 && name.compare(name.size() - 3, 3, "key") == 0 ? 3u : 0u;
        const bool controllerKeys = suffix && std::any_of(std::begin(model::ascii::controllerFields),
            std::end(model::ascii::controllerFields), [&](const auto& field) {
                return keyword.substr(0, keyword.size() - suffix) == field.name;
            });
        if (controllerKeys || name == "orientationpackedkey" || name == "orientationquatkey" ||
            std::find(arrays.begin(), arrays.end(), name) != arrays.end()) {
            std::size_t count;
            if (std::istringstream(value(lines[line])) >> count) return line + count + 1;
            while (key(lines[++line]).compare(0, 7, "endlist") != 0) {}
        }
        return line + 1;
    }
    std::vector<Block> nodes() const {
        std::vector<Block> result;
        bool geometry = false;
        for (std::size_t i = 0; i < lines.size(); i = nextRecord(i)) {
            const auto field = key(lines[i]);
            if (field == "beginmodelgeom") geometry = true;
            else if (field == "endmodelgeom") geometry = false;
            else if (field == "node") {
                std::string type, name;
                std::istringstream(value(lines[i])) >> type >> name;
                result.push_back({i, nextRecord(i) - 1, name, geometry});
            }
        }
        return result;
    }
    std::string field(const Block& block, const std::string& name) const {
        std::string result;
        for (auto i = block.begin + 1; i < block.end; i = nextRecord(i))
            if (key(lines[i]) == name) result = value(lines[i]);
        return result;
    }
    void edit(const Block& block, Edits edits) {
        std::vector<std::string> replacement;
        for (auto i = block.begin + 1; i < block.end; i = nextRecord(i)) {
            const auto name = key(lines[i]);
            if (!edits.count(name)) {
                replacement.insert(replacement.end(), lines.begin() + static_cast<std::ptrdiff_t>(i),
                    lines.begin() + static_cast<std::ptrdiff_t>(nextRecord(i)));
            }
        }
        for (const auto& [name, contents] : edits)
            if (contents) replacement.push_back("  " + name + " " + *contents);
        const auto start = lines.begin() + static_cast<std::ptrdiff_t>(block.begin + 1);
        lines.erase(start, lines.begin() + static_cast<std::ptrdiff_t>(block.end));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(block.begin + 1),
                     replacement.begin(), replacement.end());
    }
    void editNode(const std::string& name, const Edits& edits) {
        const auto blocks = nodes();
        const auto found = std::find_if(blocks.begin(), blocks.end(), [&](const auto& block) {
            return block.geometry && model::ascii::equal(block.name, name);
        });
        edit(*found, edits);
    }
    void materializeTangentPolicies() {
        std::map<std::string, std::string> policies;
        std::vector<std::pair<Block, std::string>> targets;
        std::size_t line = 0;
        for (const auto& block : nodes()) {
            for (; line < block.begin; line = nextRecord(line)) {
                if (key(lines[line]) == "newmodel") policies.clear();
                if (key(lines[line]) == "texturetangents") {
                    std::string name, enabled;
                    std::istringstream(value(lines[line])) >> name >> enabled;
                    policies[model::ascii::lower(name)] = enabled;
                }
            }
            std::string texture;
            for (auto i = block.begin + 1; i < block.end; i = nextRecord(i))
                if (key(lines[i]) == "texture0" || key(lines[i]) == "bitmap")
                    std::istringstream(value(lines[i])) >> texture;
            const auto policy = policies.find(model::ascii::lower(texture));
            if (policy != policies.end()) targets.emplace_back(block, policy->second);
            line = block.end + 1;
        }
        for (auto target = targets.rbegin(); target != targets.rend(); ++target)
            edit(target->first, {{"hastangentbasis", target->second}});
        for (std::size_t i = 0; i < lines.size();)
            if (key(lines[i]) == "texturetangents") lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
            else i = nextRecord(i);
    }
    std::vector<Texture> textures() const {
        std::vector<Texture> result;
        const auto append = [&](std::size_t line, bool wholeLine, bool policy) {
            auto column = lines[line].find_first_not_of(" \t\r");
            if (!wholeLine) {
                column = lines[line].find_first_of(" \t", column);
                column = lines[line].find_first_not_of(" \t\r", column);
            }
            if (column == std::string::npos) return;
            const auto end = lines[line].find_first_of(" \t\r", column);
            const auto name = lines[line].substr(column, end - column);
            if (name.front() == '#' || model::ascii::equal(name, "null") || model::ascii::equal(name, "none")) return;
            result.push_back({line, column, name.size(), name, policy});
        };
        for (std::size_t i = 0; i < lines.size();) {
            const auto name = key(lines[i]);
            if (name == "texture0" || name == "bitmap" || name == "texture1" ||
                name == "texture" || name == "m_sdepthtexturename" || name == "texturetangents")
                append(i, false, name == "texturetangents");
            else if (name == "texturenames") {
                const auto end = nextRecord(i);
                for (auto row = i + 1; row < end; ++row)
                    if (key(lines[row]).compare(0, 7, "endlist") != 0) append(row, true, false);
            }
            i = name == "node" ? i + 1 : nextRecord(i);
        }
        return result;
    }
    wxArrayString textureNames() const {
        std::map<std::string, std::string> names;
        for (const auto& texture : textures())
            if (!texture.policy) names.emplace(model::ascii::lower(texture.name), texture.name);
        wxArrayString result;
        for (const auto& [key, name] : names) result.Add(wxui::toWx(name));
        return result;
    }
    std::string text() const {
        std::string result;
        for (const auto& line : lines) result += line + '\n';
        return result;
    }
};

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
        return item.geometry && model::ascii::equal(item.name, node.name);
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
    move->SetToolTip("Edits the base position and stored node position. Animated controllers remain in the ASCII editor.");
    rotate->SetToolTip("Edits the base rotation and stored node orientation. Animated controllers remain in the ASCII editor.");
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
    }
    layout->Add(grid, 1, wxEXPAND | wxALL, FromDIP(12));
    if (!advanced_ && node.mesh && !node.lightsaber && !asset.meshes[*node.mesh].animated)
        layout->Add(new wxStaticText(&dialog, wxID_ANY,
            "Tangent controls are available in Advanced ASCII."), 0, wxALL, FromDIP(12));
    if (!move->IsEnabled() || !rotate->IsEnabled() || !scale->IsEnabled())
        layout->Add(new wxStaticText(&dialog, wxID_ANY,
            "Keyed or raw transform controllers are edited in the ASCII pane."), 0, wxALL, FromDIP(12));
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
    source.editNode(node.name, edits);
    replaceText(source.text());
}

void AsciiEditor::editModel(const model::Model& asset) {
    SourceText source(text());
    wxDialog dialog(this, wxID_ANY, "Model Properties");
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* root = new wxChoice(&dialog, wxID_ANY);
    std::vector<std::size_t> nodeIndices;
    int geometryRoot = 0, neck = wxNOT_FOUND;
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const auto& node = asset.nodes[i];
        const auto selection = static_cast<int>(nodeIndices.size());
        nodeIndices.push_back(i);
        root->Append(wxui::toWx(node.name + (!node.parent ? " (geometry root)" : "")));
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
    source.edit({begin, end, {}, true}, {{"smodelanimationrootname",
        asset.nodes[nodeIndices[root->GetSelection()]].name}});
    replaceText(source.text());
}

void AsciiEditor::renameTexture() {
    SourceText source(text());
    wxDialog dialog(this, wxID_ANY, "Rename Texture Throughout Model");
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* from = new wxChoice(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, source.textureNames());
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
        for (const auto& name : SourceText(text()).textureNames()) list += wxui::toStd(name) + '\n';
        neoshared::texture::writeFileBytes(path, {list.begin(), list.end()});
    } catch (const std::exception& error) { wxui::showError(this, error); }
}

} // namespace neomdl::ui
