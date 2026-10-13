#include "wx/AsciiEditor.hpp"
#include "wx/AsciiSource.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/grid.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <deque>

namespace neomdl::ui {
namespace {

struct PropertyRow {
    std::vector<std::string> cells;
    std::string original, comment;
    bool changed{};
};

PropertyRow readRow(const std::string& text) {
    PropertyRow row;
    row.original = text;
    for (auto start = text.find_first_not_of(" \t\r"); start != std::string::npos;) {
        if (text[start] == '#') { row.comment = text.substr(start); break; }
        const auto end = text.find_first_of(" \t\r", start);
        row.cells.push_back(text.substr(start, end - start));
        start = text.find_first_not_of(" \t\r", end);
    }
    return row;
}

std::string rowText(const PropertyRow& row) {
    std::string text;
    for (const auto& cell : row.cells) {
        if (!text.empty()) text += ' ';
        text += cell;
    }
    if (!row.comment.empty()) text += " " + row.comment;
    return text;
}

struct PropertySheet {
    std::size_t begin{}, end{}, context{};
    std::string keyword, label, header, terminator, offset;
    std::vector<std::string> columns;
    std::vector<PropertyRow> rows;
    bool array{}, changed{}, removed{};

    std::vector<std::string> replacement() const {
        if (removed) return {};
        if (!array) return {"  " + keyword + " " + rowText(rows.front())};
        std::vector<std::string> text;
        if (terminator.empty()) {
            auto line = "  " + keyword + " " + std::to_string(rows.size());
            if (!offset.empty()) line += " " + offset;
            const auto comment = readRow(header).comment;
            if (!comment.empty()) line += " " + comment;
            text.push_back(std::move(line));
        } else text.push_back(header);
        for (const auto& row : rows)
            text.push_back(row.changed ? "    " + rowText(row) : row.original);
        if (!terminator.empty()) text.push_back(terminator);
        return text;
    }
};

PropertySheet readSheet(const SourceText& source, std::size_t line, std::size_t context,
                        const std::string& label, std::vector<std::string> columns, bool array) {
    PropertySheet sheet;
    sheet.begin = line;
    sheet.end = source.nextRecord(line);
    sheet.context = context;
    sheet.keyword = SourceText::key(source.lines[line]);
    sheet.header = source.lines[line];
    sheet.label = label;
    sheet.columns = std::move(columns);
    sheet.array = array;
    if (array) {
        auto end = sheet.end;
        if (SourceText::key(source.lines[end - 1]).compare(0, 7, "endlist") == 0)
            sheet.terminator = source.lines[--end];
        for (auto row = line + 1; row < end; ++row) sheet.rows.push_back(readRow(source.lines[row]));
        if (sheet.keyword == "orientationpackedkey" || sheet.keyword == "orientationquatkey") {
            std::size_t count;
            std::istringstream(SourceText::value(sheet.header)) >> count >> sheet.offset;
        }
    } else sheet.rows.push_back(readRow(SourceText::value(sheet.header)));
    return sheet;
}

// The grid reads the selected source table on demand, so a large mesh does not
// create a widget or a second wxString table for every vertex component.
class PropertyTable final : public wxGridTableBase {
public:
    explicit PropertyTable(PropertySheet& sheet) : sheet_(sheet) {}
    int GetNumberRows() override { return static_cast<int>(sheet_.rows.size()); }
    int GetNumberCols() override { return static_cast<int>(sheet_.columns.size()); }
    wxString GetRowLabelValue(int row) override { return std::to_string(row); }
    wxString GetColLabelValue(int column) override { return wxui::toWx(sheet_.columns[column]); }
    wxString GetValue(int row, int column) override {
        const auto& cells = sheet_.rows[row].cells;
        return static_cast<std::size_t>(column) < cells.size() ? wxui::toWx(cells[column]) : wxString{};
    }
    void SetValue(int row, int column, const wxString& value) override {
        auto& record = sheet_.rows[row];
        record.cells.resize(std::max(record.cells.size(), static_cast<std::size_t>(column + 1)));
        const auto text = wxui::toStd(value);
        if (record.cells[column] == text) return;
        record.cells[column] = text;
        record.changed = sheet_.changed = true;
    }
private:
    PropertySheet& sheet_;
};

const std::map<std::string, std::pair<std::string, std::vector<std::string>>> vertexFields{
    {"verts", {"Positions", {"X", "Y", "Z"}}},
    {"normals", {"Normals", {"X", "Y", "Z"}}},
    {"tverts", {"Texture UV 0", {"U", "V", "W"}}},
    {"tverts1", {"Texture UV 1", {"U", "V", "W"}}},
    {"tverts2", {"Texture UV 2", {"U", "V", "W"}}},
    {"tverts3", {"Texture UV 3", {"U", "V", "W"}}},
    {"colors", {"Colors (RGB 0–1)", {"R", "G", "B"}}},
    {"colorsrgba", {"Colors (RGBA 0–255)", {"R", "G", "B", "A"}}},
    {"constraints", {"Dangly constraints", {"Constraint"}}},
    {"weights", {"Skin weights", {"Bone 1", "Weight 1", "Bone 2", "Weight 2", "Bone 3", "Weight 3", "Bone 4", "Weight 4"}}},
    {"skinweights", {"Skin weights and palette slots", {"Weight 1", "Weight 2", "Weight 3", "Weight 4", "Slot 1", "Slot 2", "Slot 3", "Slot 4"}}},
    {"cpuverts", {"CPU positions", {"X", "Y", "Z"}}},
    {"tangentbasis", {"Tangent basis", {"Bitangent X", "Bitangent Y", "Bitangent Z", "Tangent X", "Tangent Y", "Tangent Z", "Normal X", "Normal Y", "Normal Z"}}},
    {"normalindices", {"Normal corner indices", {"Corner 0", "Corner 1", "Corner 2"}}},
    {"colorindices", {"Color corner indices", {"Corner 0", "Corner 1", "Corner 2"}}},
    {"tangentindices", {"Tangent corner indices", {"Corner 0", "Corner 1", "Corner 2"}}},
    {"cpuindices", {"CPU position corner indices", {"Corner 0", "Corner 1", "Corner 2"}}},
    {"texindices1", {"UV 1 corner indices", {"Corner 0", "Corner 1", "Corner 2"}}},
    {"texindices2", {"UV 2 corner indices", {"Corner 0", "Corner 1", "Corner 2"}}},
    {"texindices3", {"UV 3 corner indices", {"Corner 0", "Corner 1", "Corner 2"}}},
    {"faces", {"Face assignments", {"Vertex 0", "Vertex 1", "Vertex 2", "Smoothing", "UV 0", "UV 1", "UV 2", "Material"}}},
    {"faceplanes", {"Face planes", {"Normal X", "Normal Y", "Normal Z", "Distance"}}},
    {"animverts", {"Animated positions", {"X", "Y", "Z"}}},
    {"animtverts", {"Animated UVs", {"U", "V", "W"}}},
    {"tangents", {"Animated tangents", {"X", "Y", "Z", "W"}}}
};

std::vector<std::string> controllerComponents(const std::string& field) {
    if (field == "position") return {"X", "Y", "Z"};
    if (field == "orientation") return {"Axis X", "Axis Y", "Axis Z", "Angle (radians)"};
    if (field == "color" || field == "selfillumcolor" || field == "colorstart" ||
        field == "colormid" || field == "colorend") return {"R", "G", "B"};
    return {"Value"};
}

std::vector<std::string> controllerColumns(const std::string& keyword) {
    if (keyword == "controllerdata") return {"Word (uint32 decimal)"};
    if (keyword == "controllerrecord") return {"Type", "Source offset", "Rows", "Time index", "Data index", "Flags"};
    if (keyword == "orientationpackedkey") return {"Time (seconds)", "Packed quaternion (uint32)"};
    if (keyword == "orientationquatkey") return {"Time (seconds)", "X", "Y", "Z", "W"};
    const bool bezier = keyword.size() >= 9 && keyword.compare(keyword.size() - 9, 9, "bezierkey") == 0;
    const bool keyed = bezier || (keyword.size() >= 3 && keyword.compare(keyword.size() - 3, 3, "key") == 0);
    const auto field = keyword.substr(0, keyword.size() - (bezier ? 9u : keyed ? 3u : 0u));
    auto columns = controllerComponents(field);
    if (bezier) {
        if (columns.size() == 1) columns = {"Stored value 0", "Stored value 1", "Stored value 2"};
        else {
            const auto components = columns;
            for (const auto* direction : {"Incoming ", "Outgoing "})
                for (const auto& component : components) columns.push_back(direction + component);
        }
    }
    if (keyed) columns.insert(columns.begin(), "Time (seconds)");
    return columns;
}

bool controllerField(const std::string& key, std::uint16_t flags) {
    if (key == "controllerdata" || key == "controllerrecord" ||
        key == "orientationpackedkey" || key == "orientationquatkey") return true;
    return std::any_of(std::begin(model::ascii::controllerFields), std::end(model::ascii::controllerFields),
        [&](const auto& field) {
            const auto name = std::string(field.name);
            return (flags & field.nodeFlag) && (key == name || key == name + "key" || key == name + "bezierkey");
        });
}

PropertyRow newControllerRow(const PropertySheet& sheet) {
    PropertyRow row;
    row.changed = true;
    row.cells.resize(sheet.columns.size(), "0");
    if (sheet.keyword == "orientation" || sheet.keyword == "orientationkey")
        row.cells[sheet.array ? 3 : 2] = "1";
    else if (sheet.keyword == "orientationquatkey") row.cells[4] = "1";
    else if (sheet.keyword == "orientationpackedkey") row.cells[1] = std::to_string(1023u | (1023u << 11u) | (511u << 22u));
    else if (sheet.keyword == "scale" || sheet.keyword == "scalekey" || sheet.keyword == "scalebezierkey" ||
             sheet.keyword == "alpha" || sheet.keyword == "alphakey" || sheet.keyword == "alphabezierkey")
        row.cells[sheet.array ? 1 : 0] = "1";
    else if (sheet.keyword == "controllerrecord") row.cells = {"8", "16", "1", "0", "1", "3"};
    return row;
}

class DataPropertiesDialog final : public wxDialog {
public:
    DataPropertiesDialog(wxWindow* parent, const SourceText& source, const model::Node& node,
                         bool controllers, bool advanced)
        : wxDialog(parent, wxID_ANY, wxui::toWx(std::string(controllers ? "Controller Properties — " : "Vertex Properties — ") + node.name),
                   wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          controllers_(controllers), advanced_(advanced), flags_(node.flags) {
        for (const auto& block : source.nodes())
            if (model::ascii::equal(block.name, node.name) && (controllers || block.geometry)) contexts_.push_back(block);
        for (std::size_t context = 0; context < contexts_.size(); ++context) {
            const auto& block = contexts_[context];
            insertions_.push_back(block.end);
            for (auto line = block.begin + 1; line < block.end; line = source.nextRecord(line)) {
                const auto key = SourceText::key(source.lines[line]);
                if (key == "nodeposition" || key == "nodeorientation")
                    insertions_.back() = std::min(insertions_.back(), line);
                if (controllers && controllerField(key, flags_)) {
                    const bool array = key == "controllerdata" ||
                        (key.size() >= 3 && key.compare(key.size() - 3, 3, "key") == 0);
                    sheets_.push_back(readSheet(source, line, context, key, controllerColumns(key), array));
                } else if (!controllers) {
                    const auto found = vertexFields.find(key);
                    if (found != vertexFields.end())
                        sheets_.push_back(readSheet(source, line, context, found->second.first, found->second.second, true));
                }
            }
        }
        auto* layout = new wxBoxSizer(wxVERTICAL);
        context_ = new wxChoice(this, wxID_ANY);
        for (const auto& block : contexts_)
            context_->Append(wxui::toWx(block.geometry ? "Base geometry" : "Animation: " + block.animation));
        context_->SetSelection(0);
        if (controllers) layout->Add(context_, 0, wxEXPAND | wxALL, FromDIP(10));
        else context_->Hide();
        auto* selection = new wxBoxSizer(wxHORIZONTAL);
        sheet_ = new wxChoice(this, wxID_ANY);
        selection->Add(sheet_, 1, wxEXPAND);
        if (controllers) {
            auto* add = new wxButton(this, wxID_ANY, "Add Controller...");
            remove_ = new wxButton(this, wxID_ANY, "Remove Controller");
            selection->Add(add, 0, wxLEFT, FromDIP(6));
            selection->Add(remove_, 0, wxLEFT, FromDIP(6));
            add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { addController(); });
            remove_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                flush(); current_->removed = current_->changed = true; refresh();
            });
        }
        layout->Add(selection, 0, wxEXPAND | wxALL, FromDIP(10));
        grid_ = new wxGrid(this, wxID_ANY);
        empty_.columns = {"Value"};
        grid_->SetTable(new PropertyTable(empty_), true);
        grid_->SetRowLabelSize(FromDIP(65));
        grid_->SetDefaultColSize(FromDIP(110));
        layout->Add(grid_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
        if (controllers) {
            auto* rows = new wxBoxSizer(wxHORIZONTAL);
            addRow_ = new wxButton(this, wxID_ANY, "Add Row");
            deleteRow_ = new wxButton(this, wxID_ANY, "Delete Row");
            up_ = new wxButton(this, wxID_ANY, "Move Up");
            down_ = new wxButton(this, wxID_ANY, "Move Down");
            for (auto* button : {addRow_, deleteRow_, up_, down_}) rows->Add(button, 0, wxRIGHT, FromDIP(6));
            layout->Add(rows, 0, wxALL, FromDIP(10));
            addRow_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                flush();
                current_->rows.push_back(newControllerRow(*current_));
                current_->changed = true;
                showSheet(static_cast<int>(current_->rows.size()) - 1);
            });
            deleteRow_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                flush();
                const auto row = grid_->GetGridCursorRow();
                current_->rows.erase(current_->rows.begin() + row);
                current_->changed = true;
                showSheet(std::min(row, static_cast<int>(current_->rows.size()) - 1));
            });
            up_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { moveRow(-1); });
            down_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { moveRow(1); });
            grid_->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& event) { rowButtons(event.GetRow()); event.Skip(); });
            auto* sourceOffset = new wxBoxSizer(wxHORIZONTAL);
            offsetLabel_ = new wxStaticText(this, wxID_ANY, "Source offset");
            offset_ = new wxTextCtrl(this, wxID_ANY);
            sourceOffset->Add(offsetLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            sourceOffset->Add(offset_, 0);
            layout->Add(sourceOffset, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
            offset_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
                current_->offset = wxui::toStd(offset_->GetValue()); current_->changed = true;
            });
        }
        note_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
        layout->Add(note_, 0, wxEXPAND | wxALL, FromDIP(10));
        layout->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(10));
        FindWindow(wxID_OK)->SetLabel("Apply");
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { flush(); EndModal(wxID_OK); }, wxID_OK);
        context_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { flush(); refresh(); });
        sheet_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            flush(); current_ = visible_[sheet_->GetSelection()]; showSheet();
        });
        SetSizer(layout);
        SetMinSize(FromDIP(wxSize(720, 500)));
        SetSize(FromDIP(wxSize(940, 640)));
        refresh();
    }

    bool apply(SourceText& source) const {
        std::vector<const PropertySheet*> changes;
        for (const auto& sheet : sheets_) if (sheet.changed) changes.push_back(&sheet);
        std::stable_sort(changes.begin(), changes.end(), [](const auto* a, const auto* b) { return a->begin < b->begin; });
        for (auto change = changes.rbegin(); change != changes.rend(); ++change) {
            const auto& sheet = **change;
            auto replacement = sheet.replacement();
            source.lines.erase(source.lines.begin() + static_cast<std::ptrdiff_t>(sheet.begin),
                               source.lines.begin() + static_cast<std::ptrdiff_t>(sheet.end));
            source.lines.insert(source.lines.begin() + static_cast<std::ptrdiff_t>(sheet.begin),
                                replacement.begin(), replacement.end());
        }
        return !changes.empty();
    }

private:
    void flush() { grid_->SaveEditControlValue(); grid_->DisableCellEditControl(); }
    void refresh(PropertySheet* selected = nullptr) {
        visible_.clear(); sheet_->Clear();
        int selection = 0;
        for (auto& sheet : sheets_)
            if (!sheet.removed && sheet.context == static_cast<std::size_t>(context_->GetSelection())) {
                if (&sheet == selected) selection = static_cast<int>(visible_.size());
                visible_.push_back(&sheet);
                sheet_->Append(wxui::toWx(std::to_string(visible_.size()) + ". " + sheet.label));
            }
        sheet_->Enable(!visible_.empty());
        current_ = visible_.empty() ? &empty_ : visible_[selection];
        sheet_->SetSelection(visible_.empty() ? wxNOT_FOUND : selection);
        showSheet();
    }
    void rowButtons(int row) {
        const bool array = current_ != &empty_ && current_->array;
        addRow_->Enable(array);
        deleteRow_->Enable(array && row >= 0);
        up_->Enable(array && row > 0);
        down_->Enable(array && row >= 0 && row + 1 < static_cast<int>(current_->rows.size()));
    }
    void showSheet(int row = 0) {
        grid_->SetTable(new PropertyTable(*current_), true);
        if (!current_->rows.empty()) grid_->SetGridCursor(std::max(0, row), 0);
        if (controllers_) {
            remove_->Enable(current_ != &empty_);
            rowButtons(current_->rows.empty() ? -1 : std::max(0, row));
            const bool offset = current_->keyword == "orientationpackedkey" || current_->keyword == "orientationquatkey";
            offset_->Show(offset); offsetLabel_->Show(offset);
            offset_->ChangeValue(wxui::toWx(current_->offset));
            if (current_->keyword == "controllerdata" || current_->keyword == "controllerrecord")
                note_->SetLabel("Raw indices refer to the stored word pool. Keep records and shared data indices consistent.\nApply rebuilds the preview; the whole edit can be undone in the ASCII pane.");
            else if (current_->keyword.find("bezierkey") != std::string::npos && current_->columns.size() == 4)
                note_->SetLabel("Scalar Bezier rows retain the engine's three stored values; they are not vector-style tangent handles.\nTimes are in seconds. Apply rebuilds the preview; the whole edit can be undone in the ASCII pane.");
            else note_->SetLabel("Edit constants or key times and values. Row order is retained.\nApply rebuilds the preview; the whole edit can be undone in the ASCII pane.");
        } else note_->SetLabel("Row numbers are source indices, starting at zero. Positions, UVs and normals can use separate pools.\nApply rebuilds the preview. Supplied normals, tangents, CPU positions, face planes and bounds remain authored.");
        Layout();
    }
    void moveRow(int direction) {
        flush();
        const auto row = grid_->GetGridCursorRow();
        std::swap(current_->rows[row], current_->rows[row + direction]);
        current_->changed = true;
        showSheet(row + direction);
    }
    void addController() {
        flush();
        wxDialog dialog(this, wxID_ANY, "Add Controller");
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* field = new wxChoice(&dialog, wxID_ANY);
        for (const auto& item : model::ascii::controllerFields)
            if (flags_ & item.nodeFlag) field->Append(wxui::toWx(std::string(item.name)));
        if (advanced_) { field->Append("controllerrecord"); field->Append("controllerdata"); }
        field->SetSelection(0);
        auto* form = new wxChoice(&dialog, wxID_ANY);
        const auto forms = [&] {
            const auto name = wxui::toStd(field->GetStringSelection());
            form->Clear();
            if (name == "controllerrecord" || name == "controllerdata") form->Append("Raw");
            else {
                form->Append("Constant"); form->Append("Linear keys");
                if (name != "orientation") form->Append("Bezier keys");
            }
            form->SetSelection(0);
        };
        forms();
        field->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { forms(); });
        layout->Add(new wxStaticText(&dialog, wxID_ANY, "Controller"), 0, wxALL, FromDIP(10));
        layout->Add(field, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
        layout->Add(new wxStaticText(&dialog, wxID_ANY, "Form"), 0, wxALL, FromDIP(10));
        layout->Add(form, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
        layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(10));
        dialog.SetSizerAndFit(layout);
        if (dialog.ShowModal() != wxID_OK) return;
        PropertySheet added;
        added.context = static_cast<std::size_t>(context_->GetSelection());
        added.begin = added.end = insertions_[added.context];
        added.keyword = wxui::toStd(field->GetStringSelection());
        added.array = added.keyword == "controllerdata" || form->GetSelection() != 0;
        if (advanced_ && added.keyword == "orientation") {
            added.keyword = "orientationquatkey";
            added.array = true;
            added.offset = form->GetSelection() == 0 ? "-1" : "28";
        } else if (form->GetSelection() == 1) added.keyword += "key";
        else if (form->GetSelection() == 2) added.keyword += "bezierkey";
        added.label = added.keyword;
        added.columns = controllerColumns(added.keyword);
        added.rows.push_back(newControllerRow(added));
        added.changed = true;
        sheets_.push_back(std::move(added));
        refresh(&sheets_.back());
    }

    bool controllers_, advanced_;
    std::uint16_t flags_;
    std::vector<SourceText::Block> contexts_;
    std::vector<std::size_t> insertions_;
    std::deque<PropertySheet> sheets_;
    std::vector<PropertySheet*> visible_;
    PropertySheet empty_, *current_{&empty_};
    wxChoice *context_{}, *sheet_{};
    wxGrid* grid_{};
    wxButton *remove_{}, *addRow_{}, *deleteRow_{}, *up_{}, *down_{};
    wxTextCtrl* offset_{};
    wxStaticText *offsetLabel_{}, *note_{};
};

} // namespace

void AsciiEditor::editVertices(const model::Model& asset, std::size_t nodeIndex) {
    SourceText source(text());
    DataPropertiesDialog dialog(this, source, asset.nodes[nodeIndex], false, advanced_);
    if (dialog.ShowModal() == wxID_OK && dialog.apply(source)) replaceText(source.text());
}

void AsciiEditor::editControllers(const model::Model& asset, std::size_t nodeIndex) {
    SourceText source(text());
    DataPropertiesDialog dialog(this, source, asset.nodes[nodeIndex], true, advanced_);
    if (dialog.ShowModal() == wxID_OK && dialog.apply(source)) replaceText(source.text());
}

} // namespace neomdl::ui
