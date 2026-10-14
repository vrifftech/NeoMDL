#include "wx/BinaryTools.hpp"
#include "core/BinaryEditing.hpp"
#include "NeoWxUi.hpp"
#include <neoshared/texture/FileUtil.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/grid.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace neomdl::ui {

void editBinaryMeshFields(wxWindow* parent, const AuthoringJob& runJob) {
    wxFileDialog open(parent,"Edit Binary Mesh Fields",{},{},"Binary MDL (*.mdl)|*.mdl;*.MDL",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (open.ShowModal() != wxID_OK) return;
    try {
        BinaryModelFile file;
        if (!runJob("Reading binary mesh fields",[&](const auto& job) {
            file = readBinaryModelFile(neosettings::pathFromWx(open.GetPath()),job);
        })) return;
        wxDialog dialog(parent,wxID_ANY,"Binary Mesh Fields — " + open.GetFilename(),
            wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* select = new wxChoice(&dialog,wxID_ANY);
        std::vector<std::size_t> nodes;
        for (std::size_t i = 0; i < file.model.nodes.size(); ++i) if (file.model.nodes[i].mesh) {
            const auto& node = file.model.nodes[i]; nodes.push_back(i);
            select->Append(wxui::toWx(node.name) + wxString::Format("  [0x%zX]",*node.packedOffset));
        }
        layout->Add(select,0,wxEXPAND | wxALL,dialog.FromDIP(10));
        auto* fields = new wxPanel(&dialog);
        auto* grid = new wxFlexGridSizer(2,dialog.FromDIP(8),dialog.FromDIP(12)); grid->AddGrowableCol(1);
        std::array<wxTextCtrl*,2> textures{};
        const char* textureLabels[]{"Base texture","Secondary texture"};
        for (std::size_t i = 0; i < textures.size(); ++i) {
            grid->Add(new wxStaticText(fields,wxID_ANY,textureLabels[i]),0,wxALIGN_CENTER_VERTICAL);
            textures[i] = new wxTextCtrl(fields,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,0,wxTextValidator(wxFILTER_ASCII));
            textures[i]->SetMaxLength(31); grid->Add(textures[i],1,wxEXPAND);
        }
        std::array<wxCheckBox*,5> flags{};
        const char* flagLabels[]{"Rotate texture","Background geometry","Cast shadows","Beaming","Render"};
        for (std::size_t i = 0; i < flags.size(); ++i) {
            flags[i] = new wxCheckBox(fields,wxID_ANY,flagLabels[i]); grid->Add(flags[i]); grid->AddSpacer(1);
        }
        fields->SetSizer(grid); layout->Add(fields,0,wxEXPAND | wxALL,dialog.FromDIP(10));
        auto* note = new wxStaticText(&dialog,wxID_ANY,
            "Edits the selected file on disk. Unsaved ASCII edits are not included.\n"
            "Texture names change references only; texture images, UVs and tangents stay as stored.\n"
            "Save Copy retains all other MDL bytes and copies existing MDX/collision companions unchanged.");
        note->Wrap(dialog.FromDIP(590)); layout->Add(note,0,wxALL,dialog.FromDIP(10));
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* save = new wxButton(&dialog,wxID_ANY,"Save Copy...");
        buttons->AddStretchSpacer(); buttons->Add(save,0,wxRIGHT,dialog.FromDIP(8));
        buttons->Add(new wxButton(&dialog,wxID_CANCEL,"Close"));
        layout->Add(buttons,0,wxEXPAND | wxALL,dialog.FromDIP(10));
        dialog.SetSizerAndFit(layout); dialog.SetMinSize(dialog.GetSize());
        std::map<std::size_t,BinaryMeshEdit> edits;
        const auto load = [&] {
            const auto index = nodes[select->GetSelection()];
            const auto base = *file.model.nodes[index].packedOffset + 80;
            const auto& edit = edits[index];
            for (std::size_t i = 0; i < textures.size(); ++i)
                textures[i]->ChangeValue(wxui::toWx(edit.textures[i].value_or(binaryMeshTexture(file,index,i))));
            for (std::size_t i = 0; i < flags.size(); ++i)
                flags[i]->SetValue(edit.flags[i].value_or(file.mdl[base+309+i] != 0));
        };
        for (std::size_t i = 0; i < textures.size(); ++i) textures[i]->Bind(wxEVT_TEXT,[&,i](wxCommandEvent&) {
            const auto index = nodes[select->GetSelection()]; const auto value = wxui::toStd(textures[i]->GetValue());
            edits[index].textures[i] = value == binaryMeshTexture(file,index,i) ? std::nullopt : std::optional<std::string>{value};
        });
        for (std::size_t i = 0; i < flags.size(); ++i) flags[i]->Bind(wxEVT_CHECKBOX,[&,i](wxCommandEvent&) {
            const auto index = nodes[select->GetSelection()]; const auto base = *file.model.nodes[index].packedOffset + 80;
            const auto value = flags[i]->GetValue();
            edits[index].flags[i] = value == (file.mdl[base+309+i] != 0) ? std::nullopt : std::optional<bool>{value};
        });
        select->Bind(wxEVT_CHOICE,[&](wxCommandEvent&) { load(); });
        save->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
            wxFileDialog output(&dialog,"Save Edited Binary Copy",neosettings::pathToWx(file.path.parent_path()),
                neosettings::pathToWx(file.path.stem())+"-edited.mdl","Binary MDL (*.mdl)|*.mdl",wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (output.ShowModal() != wxID_OK) return;
            try {
                std::vector<CollisionOutput> outputs;
                const auto destination = neosettings::pathFromWx(output.GetPath());
                if (!runJob("Preparing binary copy",[&](const auto& job) {
                    job.check(); outputs = binaryEditOutputs(file,edits,destination);
                })) return;
                auto reserved = file.inputs;
                std::vector<std::filesystem::path> companions;
                for (const auto& item : outputs) {
                    neoshared::checkResourceOutput(item.path,reserved); reserved.push_back(item.path);
                    if (item.path != destination) companions.push_back(item.path);
                }
                if (!confirmCompanionOverwrite(&dialog,companions)) return;
                if (runJob("Saving binary copy",[&](const auto& job) {
                    job.check();
                    for (const auto& item : outputs) neoshared::texture::writeFileBytes(item.path,item.bytes);
                })) note->SetLabel("Saved " + output.GetPath());
            } catch (const std::exception& error) { wxui::showError(&dialog,error); }
        });
        fields->Enable(!nodes.empty()); save->Enable(!nodes.empty());
        if (!nodes.empty()) { select->SetSelection(0); load(); }
        wxui::applyTheme(&dialog,wxui::currentDarkMode()); dialog.ShowModal();
    } catch (const std::exception& error) { wxui::showError(parent,error); }
}

namespace {

struct BinaryViewData {
    std::array<std::vector<std::uint8_t>,2> bytes;
    std::vector<std::size_t> rows;
    std::size_t different{};
    std::size_t size() const { return std::max(bytes[0].size(),bytes[1].size()); }
    bool differs(std::size_t offset) const {
        return offset < size() && (offset >= bytes[0].size() || offset >= bytes[1].size() || bytes[0][offset] != bytes[1][offset]);
    }
};

class BinaryViewTable final : public wxGridTableBase {
public:
    BinaryViewTable(const BinaryViewData& data, bool comparison, bool differences)
        : data_(data), comparison_(comparison), differences_(differences) {}
    int GetNumberRows() override { return static_cast<int>(differences_ ? data_.rows.size() : (data_.size()+15)/16); }
    int GetNumberCols() override { return comparison_ ? 32 : 17; }
    std::size_t file(int column) const { return comparison_ ? column/16 : 0; }
    std::size_t offset(int row,int column = 0) const { return (differences_ ? data_.rows[row] : row)*16+column%16; }
    wxString GetRowLabelValue(int row) override { return wxString::Format("%08zX",offset(row)); }
    wxString GetColLabelValue(int column) override {
        if (comparison_) return wxString::Format("%c:%X",column < 16 ? 'A' : 'B',column%16);
        return column == 16 ? wxString("Characters") : wxString::Format("%X",column);
    }
    wxString GetValue(int row,int column) override {
        const auto at = offset(row,column); const auto& bytes = data_.bytes[file(column)];
        if (!comparison_ && column == 16) {
            wxString characters;
            for (std::size_t i = at; i < at+16; ++i)
                characters += i >= bytes.size() ? ' ' : bytes[i] >= 32 && bytes[i] <= 126 ? static_cast<char>(bytes[i]) : '.';
            return characters;
        }
        return at < bytes.size() ? wxString::Format("%02X",static_cast<unsigned>(bytes[at])) : wxString("—");
    }
    void SetValue(int,int,const wxString&) override {}
    wxGridCellAttr* GetAttr(int row,int column,wxGridCellAttr::wxAttrKind) override {
        if (!comparison_ || !data_.differs(offset(row,column))) return nullptr;
        auto* attr = new wxGridCellAttr;
        const bool dark = wxui::currentDarkMode();
        attr->SetBackgroundColour(dark ? wxColour(100,48,40) : wxColour(255,213,200));
        attr->SetTextColour(dark ? *wxWHITE : *wxBLACK); return attr;
    }
private:
    const BinaryViewData& data_;
    bool comparison_;
    bool differences_;
};

wxString binaryValues(const std::vector<std::uint8_t>& bytes,std::size_t offset) {
    if (offset >= bytes.size()) return "No byte at the selected offset.";
    std::ostringstream text;
    text << "Offset 0x" << std::hex << std::uppercase << offset << std::dec << " (" << offset << ") — little-endian\n";
    for (const unsigned width : {1u,2u,4u,8u}) {
        const auto bits = width*8;
        text << bits << "-bit: ";
        if (width > bytes.size()-offset) {
            text << "unavailable (requires " << width << " bytes)\n";
            continue;
        }
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(bytes[offset+i]) << (i*8);
        text << "unsigned " << value << "   signed ";
        const auto mask = std::numeric_limits<std::uint64_t>::max() >> (64-bits);
        if (value & (std::uint64_t(1) << (bits-1))) text << '-' << ((~value & mask)+1);
        else text << value;
        if (width == 4) {
            const auto raw = static_cast<std::uint32_t>(value);
            float number; std::memcpy(&number,&raw,sizeof(number));
            text << "   float " << std::setprecision(std::numeric_limits<float>::max_digits10) << number;
        } else if (width == 8) {
            double number; std::memcpy(&number,&value,sizeof(number));
            text << "   float " << std::setprecision(std::numeric_limits<double>::max_digits10) << number;
        }
        text << '\n';
    }
    return wxui::toWx(text.str());
}

void viewBinaryFiles(wxWindow* parent, const AuthoringJob& runJob, bool comparison) {
    std::vector<std::filesystem::path> paths(comparison ? 2 : 1);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        wxFileDialog file(parent,comparison ? (i ? "Compare — File B" : "Compare — File A") : "Inspect Binary File",{},{},
            "Model binaries (*.mdl;*.mdx;*.wok;*.pwk;*.dwk)|*.mdl;*.mdx;*.wok;*.pwk;*.dwk|All files (*.*)|*.*",wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (file.ShowModal() != wxID_OK) return;
        paths[i] = neosettings::pathFromWx(file.GetPath());
    }
    try {
        BinaryViewData data;
        if (!runJob(comparison ? "Comparing binary files" : "Reading binary file",[&](const auto& job) {
            ResourceLoadScope scope(job);
            for (std::size_t i = 0; i < paths.size(); ++i) data.bytes[i] = readResourceFile(paths[i]);
            for (std::size_t row = 0; comparison && row*16 < data.size(); ++row) {
                if (row%4096 == 0) job.update(row*16,data.size(),"Comparing stored bytes");
                bool different = false;
                for (std::size_t i = row*16; i < std::min(row*16+16,data.size()); ++i)
                    if (data.differs(i)) { ++data.different; different = true; }
                if (different) data.rows.push_back(row);
            }
        })) return;
        wxDialog dialog(parent,wxID_ANY,comparison ? "Binary Comparison" : "Binary Inspector",
            wxDefaultPosition,parent->FromDIP(wxSize(comparison ? 1230 : 920,740)),
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        for (std::size_t i = 0; i < paths.size(); ++i)
            layout->Add(new wxStaticText(&dialog,wxID_ANY,(comparison ? wxString(i ? "B: " : "A: ") : wxString{})+neosettings::pathToWx(paths[i])+
                wxString::Format("  (%zu bytes)",data.bytes[i].size())),0,wxALL,dialog.FromDIP(6));
        wxCheckBox* only = nullptr;
        if (comparison) {
            layout->Add(new wxStaticText(&dialog,wxID_ANY,wxString::Format("%zu differing byte positions. ",data.different)+
                "Offsets are absolute; inserted bytes shift subsequent comparisons. — marks absent bytes."),0,wxALL,dialog.FromDIP(6));
            only = new wxCheckBox(&dialog,wxID_ANY,"Show only differing rows"); only->SetValue(true);
            layout->Add(only,0,wxALL,dialog.FromDIP(6));
        }
        layout->Add(new wxStaticText(&dialog,wxID_ANY,comparison ?
            "Select a byte in either file to interpret values from that offset." :
            "Select a hex byte to interpret values. Characters selects the row's first byte; dots mark nonprintable ASCII."),
            0,wxALL,dialog.FromDIP(6));
        auto* grid = new wxGrid(&dialog,wxID_ANY);
        auto* values = new wxTextCtrl(&dialog,wxID_ANY,{},wxDefaultPosition,dialog.FromDIP(wxSize(-1,145)),
            wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        const auto font = wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
        values->SetFont(font);
        const auto select = [&](int row,int column) {
            const auto& table = *static_cast<BinaryViewTable*>(grid->GetTable());
            const auto file = table.file(column);
            values->ChangeValue((comparison ? wxString(file ? "B: " : "A: ") : wxString{})+
                neosettings::pathToWx(paths[file].filename())+"\n"+binaryValues(data.bytes[file],table.offset(row,column)));
        };
        const auto display = [&] {
            grid->SetTable(new BinaryViewTable(data,comparison,comparison && only->GetValue()),true);
            grid->EnableEditing(false); grid->SetRowLabelSize(dialog.FromDIP(90));
            grid->SetDefaultCellFont(font);
            grid->SetDefaultCellAlignment(wxALIGN_CENTER,wxALIGN_CENTER);
            for (int column = 0; column < grid->GetNumberCols(); ++column)
                grid->SetColSize(column,dialog.FromDIP(!comparison && column == 16 ? 170 : 33));
            values->ChangeValue("No byte selected.");
            if (grid->GetNumberRows()) { grid->SetGridCursor(0,0); select(0,0); }
            grid->ForceRefresh();
        };
        display();
        if (comparison) only->Bind(wxEVT_CHECKBOX,[&](wxCommandEvent&) { display(); });
        grid->Bind(wxEVT_GRID_SELECT_CELL,[&](wxGridEvent& event) {
            select(event.GetRow(),event.GetCol()); event.Skip();
        });
        layout->Add(grid,1,wxEXPAND | wxALL,dialog.FromDIP(6));
        layout->Add(values,0,wxEXPAND | wxALL,dialog.FromDIP(6));
        layout->Add(dialog.CreateSeparatedButtonSizer(wxCANCEL),0,wxEXPAND | wxALL,dialog.FromDIP(6));
        dialog.FindWindow(wxID_CANCEL)->SetLabel("Close");
        dialog.SetSizer(layout); dialog.SetMinSize(parent->FromDIP(wxSize(850,520)));
        wxui::applyTheme(&dialog,wxui::currentDarkMode()); dialog.ShowModal();
    } catch (const std::exception& error) { wxui::showError(parent,error); }
}

} // namespace

void inspectBinaryFile(wxWindow* parent, const AuthoringJob& runJob) { viewBinaryFiles(parent,runJob,false); }
void compareBinaryFiles(wxWindow* parent, const AuthoringJob& runJob) { viewBinaryFiles(parent,runJob,true); }

} // namespace neomdl::ui
