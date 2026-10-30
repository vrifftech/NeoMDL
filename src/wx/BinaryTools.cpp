#include "wx/BinaryTools.hpp"
#include "core/BinaryEditing.hpp"
#include "core/BinaryGameConversion.hpp"
#include "core/BinaryAnalysis.hpp"
#include "core/ModelTransform.hpp"
#include "NeoWxUi.hpp"
#include <neoshared/texture/FileUtil.hpp>
#include <neoshared/PathUtf8.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filedlgcustomize.h>
#include <wx/grid.h>
#include <wx/choicdlg.h>
#include <wx/dc.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/treectrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace neomdl::ui {
namespace {

double binaryNumber(const std::vector<std::uint8_t>& bytes,std::size_t offset,model::BinaryType type) {
    std::uint32_t word{};
    for (std::size_t i=0;i<model::binaryTypeWidth(type);++i) word|=std::uint32_t(bytes[offset+i])<<(8*i);
    if (type==model::BinaryType::Float32) {float value;std::memcpy(&value,&word,4);return value;}
    if (type==model::BinaryType::Int16) return static_cast<std::int16_t>(word);
    return word;
}

void setBinaryNumber(std::vector<std::uint8_t>& bytes,std::size_t offset,model::BinaryType type,double value) {
    std::uint32_t word;
    if (type==model::BinaryType::Float32) {const auto number=static_cast<float>(value);std::memcpy(&word,&number,4);}
    else word=static_cast<std::uint32_t>(static_cast<std::int64_t>(value));
    for (std::size_t i=0;i<model::binaryTypeWidth(type);++i) bytes[offset+i]=static_cast<std::uint8_t>(word>>(8*i));
}

struct BinaryColumn {
    wxString label;
    std::size_t offset{},stride{};
    model::BinaryType type{model::BinaryType::Float32};
    bool readOnly{};
    std::vector<std::pair<double,wxString>> choices{};
    std::function<void(std::size_t)> changed=[](std::size_t) {};
};

class BinaryValueTable final : public wxGridTableBase {
public:
    BinaryValueTable(std::vector<std::uint8_t>& bytes,std::size_t rows,std::vector<BinaryColumn> columns)
        :bytes_(bytes),rows_(rows),columns_(std::move(columns)) {}
    int GetNumberRows() override {return static_cast<int>(rows_);}
    int GetNumberCols() override {return static_cast<int>(columns_.size());}
    wxString GetColLabelValue(int column) override {return columns_[column].label;}
    wxString GetRowLabelValue(int row) override {return wxString::Format("%d",row);}
    wxString GetTypeName(int,int column) override {
        if (!columns_[column].choices.empty()) return wxGRID_VALUE_CHOICE;
        return columns_[column].type==model::BinaryType::Float32 ? wxGRID_VALUE_FLOAT : wxGRID_VALUE_NUMBER;
    }
    bool CanGetValueAs(int row,int column,const wxString& type) override {return type==GetTypeName(row,column);}
    bool CanSetValueAs(int row,int column,const wxString& type) override {return type==GetTypeName(row,column);}
    double GetValueAsDouble(int row,int column) override {
        const auto& field=columns_[column];
        return binaryNumber(bytes_,field.offset+row*field.stride,field.type);
    }
    long GetValueAsLong(int row,int column) override {return static_cast<long>(GetValueAsDouble(row,column));}
    wxString GetValue(int row,int column) override {
        const auto value=GetValueAsDouble(row,column);
        for (const auto& choice : columns_[column].choices)
            if (choice.first==value) return choice.second;
        return wxString::Format("%.9g",value);
    }
    void SetValueAsDouble(int row,int column,double value) override {
        const auto& field=columns_[column];
        setBinaryNumber(bytes_,field.offset+row*field.stride,field.type,value);
        field.changed(row);
    }
    void SetValueAsLong(int row,int column,long value) override {SetValueAsDouble(row,column,value);}
    void SetValue(int row,int column,const wxString& value) override {
        SetValueAsDouble(row,column,std::stod(wxui::toStd(value)));
    }
    const BinaryColumn& column(std::size_t index) const {return columns_[index];}
private:
    std::vector<std::uint8_t>& bytes_;
    std::size_t rows_;
    std::vector<BinaryColumn> columns_;
};

void editBinaryValues(wxWindow* parent,const wxString& title,std::vector<std::uint8_t>& bytes,
                      std::size_t rows,std::vector<BinaryColumn> columns,
                      const wxString& note="Changes stay in this editing session. Save Copy writes the model.") {
    wxDialog dialog(parent,wxID_ANY,title,wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
    auto* layout=new wxBoxSizer(wxVERTICAL);
    auto* grid=new wxGrid(&dialog,wxID_ANY);
    auto* table=new BinaryValueTable(bytes,rows,std::move(columns));
    grid->SetTable(table,true);
    grid->SetDefaultEditor(new wxGridCellFloatEditor(-1,9,wxGRID_FLOAT_FORMAT_COMPACT));
    grid->SetDefaultRenderer(new wxGridCellFloatRenderer(-1,9,wxGRID_FLOAT_FORMAT_COMPACT));
    for (int i=0;i<table->GetNumberCols();++i) {
        auto* attr=new wxGridCellAttr;
        attr->SetReadOnly(table->column(i).readOnly);
        if (table->column(i).type==model::BinaryType::UInt8) {
            attr->SetEditor(new wxGridCellNumberEditor(0,255));
            attr->SetRenderer(new wxGridCellNumberRenderer);
        }
        if (!table->column(i).choices.empty()) {
            wxArrayString labels;
            for (const auto& choice : table->column(i).choices) labels.Add(choice.second);
            attr->SetEditor(new wxGridCellChoiceEditor(labels));
            attr->SetRenderer(new wxGridCellStringRenderer);
        }
        grid->SetColAttr(i,attr);grid->SetColSize(i,dialog.FromDIP(115));
    }
    layout->Add(grid,1,wxEXPAND|wxALL,dialog.FromDIP(8));
    auto* message=new wxStaticText(&dialog,wxID_ANY,note);
    message->Wrap(dialog.FromDIP(780));
    layout->Add(message,0,wxLEFT|wxRIGHT|wxBOTTOM,dialog.FromDIP(8));
    layout->Add(dialog.CreateButtonSizer(wxOK),0,wxEXPAND|wxALL,dialog.FromDIP(8));
    dialog.FindWindow(wxID_OK)->SetLabel("Done");
    dialog.Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        grid->SaveEditControlValue();grid->DisableCellEditControl();dialog.EndModal(wxID_OK);
    },wxID_OK);
    dialog.Bind(wxEVT_CLOSE_WINDOW,[&](wxCloseEvent&) {
        grid->SaveEditControlValue();grid->DisableCellEditControl();dialog.EndModal(wxID_OK);
    });
    dialog.SetSizer(layout);dialog.SetSize(dialog.FromDIP(wxSize(820,500)));
    wxui::applyTheme(&dialog,wxui::currentDarkMode());dialog.ShowModal();
}

void editBinaryVertices(wxWindow* parent,BinaryModelFile& file,std::size_t index,BinaryMeshEdit& edit) {
    const auto& node=file.model.nodes[index];
    const auto& mesh=file.model.meshes[*node.mesh];
    const auto base=*node.packedOffset+80;
    const auto pool=static_cast<std::size_t>(binaryNumber(file.mdl,base+
        (file.model.game==model::GameVersion::Kotor1 ? 324 : 332),model::BinaryType::UInt32));
    std::vector<BinaryColumn> columns;
    const auto add=[&](const char* name,std::size_t offset,const char* const* components,unsigned count,model::BinaryType type,
                      std::function<void(std::size_t)> changed=[](std::size_t) {}) {
        for (unsigned i=0;i<count;++i)
            columns.push_back({wxString::FromUTF8(name)+" "+wxString::FromUTF8(components[i]),
                pool+offset+i*model::binaryTypeWidth(type),mesh.mdxRowSize,type,false,{},changed});
    };
    const char* xyz[]{"X","Y","Z"};const char* uv[]{"U","V"};const char* rgba[]{"R","G","B","A"};
    if ((mesh.mdxBitmap&1u) && !mesh.animated)
        add("Position",mesh.enginePrelightInput->mdxVertexOffset,xyz,3,model::BinaryType::Float32,
            [&](std::size_t row) {edit.positionRows.insert(row);});
    if (mesh.mdxBitmap&0x20u) add("Normal",mesh.mdxNormalOffset,xyz,3,model::BinaryType::Float32);
    for (unsigned channel=0;channel<4;++channel)
        if (mesh.mdxBitmap&(2u<<channel))
            add(("UV "+std::to_string(channel)).c_str(),mesh.mdxTexcoordOffsets[channel],uv,2,model::BinaryType::Float32);
    if (mesh.mdxBitmap&0x40u) add("Colour",mesh.mdxColorOffset,rgba,4,model::BinaryType::UInt8);
    editBinaryValues(parent,"Binary Vertex Attributes — "+wxui::toWx(node.name),*file.mdx,mesh.vertices.size(),std::move(columns),
        "Positions use mesh coordinates; colours use 0–255. Normals stay as entered.\n"
        "Use Regenerate tangent basis in the mesh dialog after geometry, normal or UV edits. Save Copy writes the model.");
}

void editBinarySkin(wxWindow* parent,BinaryModelFile& file,std::size_t index,BinaryMeshEdit& edit) {
    const auto& node=file.model.nodes[index];
    const auto& mesh=file.model.meshes[*node.mesh];
    const auto base=*node.packedOffset+80;
    const auto pool=static_cast<std::size_t>(binaryNumber(file.mdl,base+
        (file.model.game==model::GameVersion::Kotor1 ? 324 : 332),model::BinaryType::UInt32));
    std::vector<std::pair<double,wxString>> bones{{-1,"-1: unassigned"}};
    for (std::size_t i=0;i<mesh.skinBoneMap.size();++i)
        if (mesh.skinBoneMap[i]>=0)
            bones.push_back({static_cast<double>(i),wxString::Format("%zu: ",i)+wxui::toWx(file.model.nodes[mesh.skinBoneMap[i]].name)});
    std::vector<BinaryColumn> columns;
    for (unsigned i=0;i<4;++i) {
        columns.push_back({wxString::Format("Weight %u",i),pool+mesh.mdxWeightOffset+i*4,mesh.mdxRowSize});
        columns.back().changed=[&](std::size_t) {edit.bindingChanged=true;};
        columns.push_back({wxString::Format("Bone %u",i),pool+mesh.mdxBoneIndexOffset+i*4,mesh.mdxRowSize,
            model::BinaryType::Float32,false,bones});
        columns.back().changed=[&](std::size_t) {edit.bindingChanged=true;};
    }
    editBinaryValues(parent,"Binary Skin Weights — "+wxui::toWx(node.name),*file.mdx,mesh.vertices.size(),std::move(columns),
        "Bone choices use the existing palette. Weights are stored as entered; Save Copy writes the model.");
}

void editBinaryDangly(wxWindow* parent,BinaryModelFile& file,std::size_t index) {
    const auto& node=file.model.nodes[index];
    const auto& mesh=file.model.meshes[*node.mesh];
    const auto extension=*node.packedOffset+(file.model.game==model::GameVersion::Kotor1 ? 412u : 420u);
    const wxString labels[]{"Dynamics","Vertex constraints"};
    const int selected=wxGetSingleChoiceIndex("Choose dangly data","Dangly Mesh",2,labels,parent);
    if (selected<0) return;
    if (selected==0)
        editBinaryValues(parent,"Dangly Dynamics — "+wxui::toWx(node.name),file.mdl,1,
            {{"Displacement",extension+12,0},{"Tightness",extension+16,0},{"Period",extension+20,0}});
    else {
        const auto data=12+static_cast<std::size_t>(binaryNumber(file.mdl,extension,model::BinaryType::UInt32));
        editBinaryValues(parent,"Dangly Constraints — "+wxui::toWx(node.name),file.mdl,mesh.dangly->constraints.size(),
            {{"Constraint",data,4}});
    }
}

void editBinaryControllers(wxWindow* parent,BinaryModelFile& file,std::size_t index) {
    auto& original=file.model.nodes[index];
    const auto words=12+static_cast<std::size_t>(binaryNumber(file.mdl,*original.packedOffset+68,model::BinaryType::UInt32));
    for (std::size_t i=0;i<original.controllerDataWords.size();++i)
        original.controllerDataWords[i]=static_cast<std::uint32_t>(binaryNumber(file.mdl,words+i*4,model::BinaryType::UInt32));
    std::vector<std::size_t> controllers;wxArrayString labels;
    for (std::size_t i=0;i<original.controllers.size();++i) {
        const auto& controller=original.controllers[i];
        if (editableBinaryMaterialController(controller)) {
            controllers.push_back(i);labels.Add(controller.type==100u ? "Self-illumination RGB" : "Opacity");
        }
    }
    labels.Add("Create/replace constant opacity");labels.Add("Create/replace constant self-illumination");
    const int chosen=wxGetSingleChoiceIndex("Edit existing values or create a constant base controller",
        "Binary Material Controllers",labels,parent);
    if (chosen<0) return;
    std::size_t controllerIndex;
    if (static_cast<std::size_t>(chosen)<controllers.size()) controllerIndex=controllers[chosen];
    else {
        const auto type=static_cast<std::size_t>(chosen)==controllers.size() ? 132u : 100u;
        const auto state=model::bindNodeMaterial(original);
        setBinaryStaticController(file,index,type,type==132u ? std::vector<float>{state.alpha} :
            std::vector<float>{state.selfIllumination[0],state.selfIllumination[1],state.selfIllumination[2]});
        const auto& current=file.model.nodes[index].controllers;
        controllerIndex=static_cast<std::size_t>(std::find_if(current.begin(),current.end(),
            [&](const auto& c) {return c.type==type;})-current.begin());
    }
    const auto& node=file.model.nodes[index];
    const auto& controller=node.controllers[controllerIndex];
    const auto data=12+static_cast<std::size_t>(binaryNumber(file.mdl,*node.packedOffset+68,model::BinaryType::UInt32));
    const unsigned width=controller.type==100u ? 3u : 1u;
    const unsigned values=width*(controller.interpolation==model::ControllerInterpolation::Bezier ? 3u : 1u);
    std::vector<BinaryColumn> columns{{"Time (seconds)",data+controller.timeIndex*4u,4u,model::BinaryType::Float32,true}};
    const char* rgb[]{"R","G","B"};
    for (unsigned i=0;i<values;++i) {
        const auto label=width==3 ? wxString::FromUTF8(rgb[i%3]) : wxString("Value");
        columns.push_back({values==width ? label : label+wxString::Format(" [%u]",i/width),
            data+(controller.dataIndex+i)*4u,values*4u});
    }
    editBinaryValues(parent,"Binary Controller Values — "+labels[chosen],file.mdl,controller.rowCount,std::move(columns));
}

void editBinaryTransform(wxWindow* parent,BinaryModelFile& file,std::size_t index,BinaryMeshEdit& edit,
                         const wxString& action,const std::function<bool(wxWindow*)>& accept) {
    const auto pose=model::constructedBindPose(file.model);
    const auto p=pose.positions[index];const auto q=pose.orientations[index];
    const std::array<double,8> values{{p.x,p.y,p.z,q.x,q.y,q.z,q.w,pose.scales[index]}};
    const char* labels[]{"Position X","Position Y","Position Z","Quaternion X","Quaternion Y","Quaternion Z","Quaternion W","Uniform scale"};
    wxDialog dialog(parent,wxID_ANY,"Base Node Transform — "+wxui::toWx(file.model.nodes[index].name));
    auto* layout=new wxBoxSizer(wxVERTICAL);auto* form=new wxFlexGridSizer(2,7,10);form->AddGrowableCol(1);
    std::array<wxSpinCtrlDouble*,8> fields{};std::array<double,8> displayed{};
    for (unsigned i=0;i<fields.size();++i) {
        form->Add(new wxStaticText(&dialog,wxID_ANY,labels[i]),0,wxALIGN_CENTER_VERTICAL);
        fields[i]=new wxSpinCtrlDouble(&dialog,wxID_ANY,{},wxDefaultPosition,wxSize(dialog.FromDIP(200),wxDefaultCoord),
            wxSP_ARROW_KEYS,-std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),values[i],0.01);
        fields[i]->SetDigits(9);fields[i]->SetValue(values[i]);displayed[i]=fields[i]->GetValue();
        form->Add(fields[i],1,wxEXPAND);
    }
    layout->Add(form,1,wxEXPAND|wxALL,dialog.FromDIP(10));
    layout->Add(new wxStaticText(&dialog,wxID_ANY,
        "Transforms are relative to the parent node. Rotation uses a unit quaternion X Y Z W.\n"
        "Changes replace the base controllers; animation clips stay intact."),
        0,wxALL,dialog.FromDIP(10));
    layout->Add(dialog.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,dialog.FromDIP(10));
    dialog.FindWindow(wxID_OK)->SetLabel(action);
    dialog.SetSizerAndFit(layout);wxui::applyTheme(&dialog,wxui::currentDarkMode());
    const auto apply=[&](unsigned first,unsigned count,std::uint32_t type) {
        bool changed=false;std::vector<float> output;
        for (unsigned i=first;i<first+count;++i) {changed|=fields[i]->GetValue()!=displayed[i];output.push_back(static_cast<float>(fields[i]->GetValue()));}
        if (changed) {
            setBinaryStaticController(file,index,type,output);edit.transformChanged=true;
            for (unsigned i=first;i<first+count;++i) displayed[i]=fields[i]->GetValue();
        }
    };
    dialog.Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        try {
            apply(0,3,model::kControllerPosition);apply(3,4,model::kControllerOrientation);apply(7,1,model::kControllerScale);
            if (accept(&dialog)) dialog.EndModal(wxID_OK);
        } catch (const std::exception& error) {wxui::showError(&dialog,error);}
    },wxID_OK);
    dialog.ShowModal();
}

class BinarySaveControls final : public wxFileDialogCustomizeHook {
public:
    explicit BinarySaveControls(model::GameVersion original) : game(original) {}
    void AddCustomControls(wxFileDialogCustomize& customizer) override {
        customizer.AddStaticText("Target game");
        const wxString games[]{"Knights of the Old Republic (K1)","The Sith Lords (K2)"};
        choice_=customizer.AddChoice(2,games);
        choice_->SetSelection(game==model::GameVersion::Kotor1 ? 0 : 1);
        customizer.AddStaticText("K1 omits K2 dirt and hologram mesh settings.");
    }
    void TransferDataFromCustomControls() override {
        game=choice_->GetSelection()==0 ? model::GameVersion::Kotor1 : model::GameVersion::Kotor2;
    }
    model::GameVersion game;
private:
    wxFileDialogChoice* choice_{};
};

std::optional<BinaryEditResult> saveBinaryCopy(wxWindow* parent,BinaryModelFile& file,std::size_t node,
    const std::map<std::size_t,BinaryMeshEdit>& edits,const AuthoringJob& runJob,const std::optional<PlanarTransform>& transform={}) {
    BinarySaveControls controls(file.model.game);
    wxFileDialog output(parent,"Save Edited Binary Copy",neosettings::pathToWx(file.path.parent_path()),
        neosettings::pathToWx(file.path.stem())+"-edited.mdl","Binary MDL (*.mdl)|*.mdl",wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    output.SetCustomizeHook(controls);
    if (output.ShowModal()!=wxID_OK) return {};
    std::vector<CollisionOutput> outputs;
    auto protectedInputs=file.inputs;
    std::map<std::size_t,std::size_t> nodeOffsets;
    const auto destination=neosettings::pathFromWx(output.GetPath());
    if (!runJob("Preparing binary copy",[&](const auto& job) {
        const std::vector<std::uint8_t> emptyMdx;
        file.model=model::decodePackedModelBytes(file.mdl,file.mdx ? *file.mdx : emptyMdx,
            neoshared::pathToUtf8(file.path),job,false);
        outputs=binaryEditOutputs(file,edits,destination,job);
        if(transform) {
            const auto inputName=resourceKey(neoshared::pathToUtf8(file.path.stem())),outputName=resourceKey(neoshared::pathToUtf8(destination.stem()));
            std::map<std::uint16_t,neoshared::ResourceDocument> edited;
            auto& source=edited[2002];source.fileName=inputName+".mdl";source.type=2002;source.bytes=outputs.front().bytes;
            if(file.mdx) {auto& stream=edited[3008];stream.fileName=inputName+".mdx";stream.type=3008;stream.bytes=outputs[1].bytes;}
            const auto local=directoryResources(file.path.parent_path());
            const auto resolver=[edited=std::move(edited),local,inputName](const std::string& name,std::uint16_t type)->std::optional<neoshared::ResourceDocument> {
                if(resourceKey(name)==inputName)if(const auto found=edited.find(type);found!=edited.end())return found->second;
                return local(name,type);
            };
            auto models=transformModelResources(inputName,outputName,resolver,*transform);
            std::vector<CollisionOutput> transformed;
            for(auto& model:models) {
                auto path=destination.parent_path()/neoshared::pathFromUtf8(model.fileName);
                if(resourceKey(neoshared::pathToUtf8(path.stem()))==outputName) {path=destination;if(model.type==3008)path.replace_extension(".mdx");}
                transformed.push_back({std::move(path),std::move(model.bytes)});
                protectedInputs.insert(protectedInputs.end(),model.protectedInputs.begin(),model.protectedInputs.end());
            }
            for(std::size_t i=file.mdx?2:1;i<outputs.size();++i) {
                auto mesh=neoshared::bwm::readBwm(outputs[i].bytes,neoshared::pathToUtf8(outputs[i].path));transformWalkmesh(mesh,*transform);
                if(mesh.type==0) {
                    const auto apply=[&](auto& point){const auto p=transform->direction({point[0],point[1],point[2]});point={p.x,p.y,p.z};};
                    apply(mesh.position);for(auto& point:mesh.usePoints)apply(point);for(auto& point:mesh.doorUsePoints)apply(point);
                }
                outputs[i].bytes=neoshared::bwm::writeBwm(mesh);transformed.push_back(std::move(outputs[i]));
            }
            outputs=std::move(transformed);
        }
        if (controls.game!=file.model.game) {
            for(std::size_t i=0;i<outputs.size();++i)if(i==0||resourceKey(neoshared::pathToUtf8(outputs[i].path.extension()))==".mdl") {
                auto offsets=switchBinaryGame(outputs[i].bytes,job);if(i==0)nodeOffsets=std::move(offsets);
            }
        }
        else for (const auto& item : file.model.nodes) nodeOffsets.emplace(*item.packedOffset,*item.packedOffset);
    })) return {};
    std::vector<std::filesystem::path> companions,destinations;
    for (const auto& item : outputs) {
        destinations.push_back(item.path);
        if (item.path!=destination) companions.push_back(item.path);
    }
    neoshared::checkResourceOutputs(destinations,protectedInputs);
    if (!confirmCompanionOverwrite(parent,companions)) return {};
    if (!runJob("Saving binary copy",[&](const auto& job) {
        job.check();
        for (const auto& item : outputs) neoshared::texture::writeFileBytes(item.path,item.bytes);
    })) return {};
    return BinaryEditResult{destination,nodeOffsets.at(*file.model.nodes[node].packedOffset),std::move(nodeOffsets)};
}

} // namespace

std::optional<BinaryEditResult> editBinaryMeshFields(wxWindow* parent, const AuthoringJob& runJob,
    const std::filesystem::path& input, std::optional<std::size_t> selectedOffset, BinaryMeshPage page,
    const std::set<std::size_t>& selectedOffsets) {
    auto path=input;
    if (path.empty()) {
        wxFileDialog open(parent,"Edit Binary Mesh Fields",{},{},"Binary MDL (*.mdl)|*.mdl;*.MDL",
                          wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (open.ShowModal() != wxID_OK) return {};
        path=neosettings::pathFromWx(open.GetPath());
    }
    std::optional<BinaryEditResult> saved;
    try {
        BinaryModelFile file;
        if (!runJob("Reading binary mesh fields",[&](const auto& job) {
            file = readBinaryModelFile(path,job);
        })) return {};
        if (page==BinaryMeshPage::Transform) {
            const auto node=std::find_if(file.model.nodes.begin(),file.model.nodes.end(),
                [&](const auto& value) {return value.packedOffset==selectedOffset;});
            const auto index=static_cast<std::size_t>(node-file.model.nodes.begin());
            std::map<std::size_t,BinaryMeshEdit> edits;
            editBinaryTransform(parent,file,index,edits[index],"Save Copy...",[&](wxWindow* owner) {
                saved=saveBinaryCopy(owner,file,index,edits,runJob);
                return saved.has_value();
            });
            return saved;
        }
        wxDialog dialog(parent,wxID_ANY,"Binary Mesh Fields — " + neosettings::pathToWx(path.filename()),
            wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* select = new wxChoice(&dialog,wxID_ANY);
        std::vector<std::size_t> nodes;
        int selection=0;
        for (std::size_t i = 0; i < file.model.nodes.size(); ++i) if (file.model.nodes[i].mesh) {
            const auto& node = file.model.nodes[i]; nodes.push_back(i);
            if (node.packedOffset==selectedOffset) selection=static_cast<int>(nodes.size()-1);
            select->Append(wxString::Format("%zu. ",nodes.size())+wxui::toWx(node.name));
        }
        layout->Add(select,0,wxEXPAND | wxALL,dialog.FromDIP(10));
        auto* pages = new wxNotebook(&dialog,wxID_ANY);
        auto* fields = new wxPanel(pages);
        auto* grid = new wxFlexGridSizer(2,dialog.FromDIP(8),dialog.FromDIP(12)); grid->AddGrowableCol(1);
        std::array<wxTextCtrl*,2> textures{};
        const char* textureLabels[]{"Base texture","Secondary texture"};
        for (std::size_t i = 0; i < textures.size(); ++i) {
            grid->Add(new wxStaticText(fields,wxID_ANY,textureLabels[i]),0,wxALIGN_CENTER_VERTICAL);
            textures[i] = new wxTextCtrl(fields,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,0,wxTextValidator(wxFILTER_ASCII));
            textures[i]->SetMaxLength(31); grid->Add(textures[i],1,wxEXPAND);
        }
        std::array<wxCheckBox*,6> flags{};
        const char* flagLabels[]{"Lightmapped","Rotate texture","Background geometry","Cast shadows","Beaming","Render"};
        for (std::size_t i = 0; i < flags.size(); ++i) {
            flags[i] = new wxCheckBox(fields,wxID_ANY,flagLabels[i]); grid->Add(flags[i]);
        }
        auto* tangent = new wxCheckBox(fields,wxID_ANY,"Has tangent basis");
        grid->Add(tangent);
        auto* regenerate = new wxCheckBox(fields,wxID_ANY,"Regenerate tangent basis");
        grid->Add(regenerate);
        auto* vertices = new wxButton(fields,wxID_ANY,"Vertex Attributes...");
        vertices->SetToolTip("Edit positions, normals, UVs and colours. Use Regenerate tangent basis after geometry, normal or UV changes when needed.");
        auto* controllers = new wxButton(fields,wxID_ANY,"Opacity / Glow...");
        controllers->SetToolTip("Edit or create base opacity and self-illumination controllers.");
        grid->Add(vertices,0,wxEXPAND);grid->Add(controllers,0,wxEXPAND);
        auto* skin = new wxButton(fields,wxID_ANY,"Skin Weights...");
        auto* dangly = new wxButton(fields,wxID_ANY,"Dangly Settings...");
        grid->Add(skin,0,wxEXPAND);grid->Add(dangly,0,wxEXPAND);
        auto* transform=new wxButton(fields,wxID_ANY,"Base Transform...");grid->Add(transform,0,wxEXPAND);grid->AddSpacer(1);
        fields->SetSizer(grid);pages->AddPage(fields,"Mesh");

        struct NumberField {
            wxSpinCtrlDouble* control;
            std::size_t offset;
            model::BinaryType type;
            double displayed{};
        };
        std::vector<NumberField> numbers;
        const auto number=[&](wxPanel* panel,wxFlexGridSizer* form,const char* label,std::size_t offset,model::BinaryType type) {
            form->Add(new wxStaticText(panel,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
            const bool floating=type==model::BinaryType::Float32;
            const double minimum=floating ? -std::numeric_limits<float>::max() : type==model::BinaryType::Int16 ? -32768.0 : 0.0;
            const double maximum=floating ? std::numeric_limits<float>::max() : type==model::BinaryType::Int16 ? 32767.0 : 4294967295.0;
            auto* control=new wxSpinCtrlDouble(panel,wxID_ANY,{},wxDefaultPosition,wxSize(dialog.FromDIP(180),wxDefaultCoord),
                wxSP_ARROW_KEYS,minimum,maximum,0.0,floating ? 0.01 : 1.0);
            control->SetDigits(floating ? 9 : 0);form->Add(control,1,wxEXPAND);
            numbers.push_back({control,offset,type});
        };
        auto* material=new wxPanel(pages);
        auto* materialForm=new wxFlexGridSizer(2,dialog.FromDIP(7),dialog.FromDIP(10));materialForm->AddGrowableCol(1);
        const char* colours[]{"Diffuse R","Diffuse G","Diffuse B","Ambient R","Ambient G","Ambient B"};
        for (unsigned i=0;i<6;++i) number(material,materialForm,colours[i],60+i*4,model::BinaryType::Float32);
        number(material,materialForm,"Transparency hint (ordering)",84,model::BinaryType::UInt32);
        const bool k2=file.model.game==model::GameVersion::Kotor2;
        wxCheckBox* dirt=nullptr;wxCheckBox* noHologram=nullptr;
        if (k2) {
            dirt=new wxCheckBox(material,wxID_ANY,"Dirt enabled (K2)");materialForm->Add(dirt);materialForm->AddSpacer(1);
            dirt->SetToolTip("Stores the K2 dirt fields. Their visual effect depends on the engine material path.");
            number(material,materialForm,"Dirt texture index",316,model::BinaryType::Int16);
            number(material,materialForm,"Dirt worldspace",318,model::BinaryType::Int16);
            noHologram=new wxCheckBox(material,wxID_ANY,"Exclude from hologram rendering");
            materialForm->Add(noHologram);materialForm->AddSpacer(1);
        }
        material->SetSizer(materialForm);pages->AddPage(material,"Material");
        auto* uv=new wxPanel(pages);
        auto* uvForm=new wxFlexGridSizer(2,dialog.FromDIP(7),dialog.FromDIP(10));uvForm->AddGrowableCol(1);
        auto* animateUv=new wxCheckBox(uv,wxID_ANY,"Animate UVs");uvForm->Add(animateUv);uvForm->AddSpacer(1);
        const char* uvLabels[]{"Scroll X","Scroll Y","Jitter amount","Jitter speed"};
        for (unsigned i=0;i<4;++i) number(uv,uvForm,uvLabels[i],236+i*4,model::BinaryType::Float32);
        uv->SetSizer(uvForm);pages->AddPage(uv,"UV Animation");
        auto* wholeModel=new wxPanel(pages);auto* modelForm=new wxBoxSizer(wxVERTICAL);
        modelForm->Add(new wxStaticText(wholeModel,wxID_ANY,"Rotate whole model around Z (degrees)"),0,wxALL,8);
        auto* modelAngle=new wxSpinCtrlDouble(wholeModel,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,wxSP_ARROW_KEYS,-360,360,0,90);modelAngle->SetDigits(4);modelForm->Add(modelAngle,0,wxEXPAND|wxALL,8);
        auto* modelFlipX=new wxCheckBox(wholeModel,wxID_ANY,"Flip X");auto* modelFlipY=new wxCheckBox(wholeModel,wxID_ANY,"Flip Y");
        modelForm->Add(modelFlipX,0,wxALL,8);modelForm->Add(modelFlipY,0,wxALL,8);
        auto* modelHelp=new wxStaticText(wholeModel,wxID_ANY,"Save Copy applies flips, then rotation around the model origin. Geometry, normals, tangent bases, animation keys and collision companions follow the transform. Referenced models are saved as separate copies.\n\nFor a placed room, use Rooms / VIS in the module designer to coordinate its LYT position, WOK and contained placements.");modelHelp->Wrap(dialog.FromDIP(550));modelForm->Add(modelHelp,0,wxEXPAND|wxALL,8);
        wholeModel->SetSizer(modelForm);pages->AddPage(wholeModel,"Whole model");
        layout->Add(pages,1,wxEXPAND | wxALL,dialog.FromDIP(10));
        auto* batch=new wxCheckBox(&dialog,wxID_ANY,wxString::Format(
            "Apply this mesh's changed properties to %zu selected meshes",selectedOffsets.size()));
        batch->SetToolTip("Copies changed texture references, flags, colours, transparency ordering, K2 material fields and UV animation.\n"
            "Vertex data, skin weights, controllers, transforms and tangent basis remain per mesh.");
        batch->Show(selectedOffsets.size()>1);
        layout->Add(batch,0,wxLEFT|wxRIGHT|wxBOTTOM,dialog.FromDIP(10));
        auto* note = new wxStaticText(&dialog,wxID_ANY,
            "Changes stay in this dialog until Save Copy writes and opens a new MDL/MDX copy.\n"
            "Choose K1 or K2 in Save Copy. Cancel discards this session.\n"
            "Lightmaps use the existing secondary UVs and texture; edit texture/TXI files in NeoTPC.");
        note->Wrap(dialog.FromDIP(590)); layout->Add(note,0,wxALL,dialog.FromDIP(10));
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* save = new wxButton(&dialog,wxID_ANY,"Save Copy...");
        save->SetDefault();
        buttons->AddStretchSpacer(); buttons->Add(save,0,wxRIGHT,dialog.FromDIP(8));
        buttons->Add(new wxButton(&dialog,wxID_CANCEL,"Cancel"));
        layout->Add(buttons,0,wxEXPAND | wxALL,dialog.FromDIP(10));
        dialog.SetSizerAndFit(layout); dialog.SetMinSize(dialog.GetSize());
        std::map<std::size_t,BinaryMeshEdit> edits;
        std::size_t current{};
        const auto headerField=[&](std::size_t offset,model::BinaryType type,double value) {
            const auto base=*file.model.nodes[current].packedOffset+80;
            setBinaryNumber(file.mdl,base+offset,type,value);
            edits[current].headerFields[offset]={file.mdl.begin()+base+offset,
                file.mdl.begin()+base+offset+model::binaryTypeWidth(type)};
        };
        const auto flush=[&] {
            for (auto& field : numbers) {
                const auto value=field.control->GetValue();
                if (value!=field.displayed) {
                    headerField(field.offset,field.type,value);
                    field.displayed=value;
                }
            }
        };
        const auto load = [&] {
            const auto index = nodes[select->GetSelection()];
            current=index;
            const auto base = *file.model.nodes[index].packedOffset + 80;
            const auto& edit = edits[index];
            const auto& node=file.model.nodes[index];
            const auto& mesh=file.model.meshes[*node.mesh];
            for (std::size_t i = 0; i < textures.size(); ++i)
                textures[i]->ChangeValue(wxui::toWx(edit.textures[i].value_or(binaryMeshTexture(file,index,i))));
            for (std::size_t i = 0; i < flags.size(); ++i)
                flags[i]->SetValue(edit.flags[i].value_or(file.mdl[base+308+i] != 0));
            tangent->SetValue(edit.tangentBasis.value_or((mesh.mdxBitmap&0x80u)!=0));
            tangent->Enable(file.mdx.has_value() && !node.lightsaber && !mesh.animated && !mesh.vertices.empty());
            regenerate->SetValue(edit.regenerateTangents);
            regenerate->Enable(tangent->IsEnabled() && tangent->GetValue());
            for (auto& field : numbers) {
                field.control->SetValue(binaryNumber(file.mdl,base+field.offset,field.type));
                field.displayed=field.control->GetValue();
            }
            animateUv->SetValue(binaryNumber(file.mdl,base+232,model::BinaryType::UInt32)!=0);
            if (k2) {dirt->SetValue(file.mdl[base+314]!=0);noHologram->SetValue(file.mdl[base+320]!=0);}
            vertices->Enable(file.mdx.has_value() && !mesh.vertices.empty() && (mesh.mdxBitmap&(mesh.animated ? 0x7eu : 0x7fu)));
            controllers->Enable(true);
            skin->Enable(mesh.skinned && file.mdx.has_value());
            dangly->Enable(mesh.dangly.has_value());
        };
        for (std::size_t i = 0; i < textures.size(); ++i) textures[i]->Bind(wxEVT_TEXT,[&,i](wxCommandEvent&) {
            const auto index = nodes[select->GetSelection()]; const auto value = wxui::toStd(textures[i]->GetValue());
            edits[index].textures[i] = value == binaryMeshTexture(file,index,i) ? std::nullopt : std::optional<std::string>{value};
        });
        for (std::size_t i = 0; i < flags.size(); ++i) flags[i]->Bind(wxEVT_CHECKBOX,[&,i](wxCommandEvent&) {
            const auto index = nodes[select->GetSelection()]; const auto base = *file.model.nodes[index].packedOffset + 80;
            const auto value = flags[i]->GetValue();
            edits[index].flags[i] = value == (file.mdl[base+308+i] != 0) ? std::nullopt : std::optional<bool>{value};
        });
        tangent->Bind(wxEVT_CHECKBOX,[&](wxCommandEvent&) {
            const auto index=nodes[select->GetSelection()];
            const auto& mesh=file.model.meshes[*file.model.nodes[index].mesh];
            const auto value=tangent->GetValue();
            auto& edit=edits[index];
            edit.tangentBasis=value==((mesh.mdxBitmap&0x80u)!=0) ? std::nullopt : std::optional<bool>{value};
            if (!value) {edit.regenerateTangents=false;regenerate->SetValue(false);}
            regenerate->Enable(value);
        });
        regenerate->Bind(wxEVT_CHECKBOX,[&](wxCommandEvent&) {
            edits[nodes[select->GetSelection()]].regenerateTangents=regenerate->GetValue();
        });
        animateUv->Bind(wxEVT_CHECKBOX,[&](wxCommandEvent&) {
            headerField(232,model::BinaryType::UInt32,animateUv->GetValue());
        });
        if (k2) {
            dirt->Bind(wxEVT_CHECKBOX,[&](wxCommandEvent&) {headerField(314,model::BinaryType::UInt8,dirt->GetValue());});
            noHologram->Bind(wxEVT_CHECKBOX,[&](wxCommandEvent&) {headerField(320,model::BinaryType::UInt8,noHologram->GetValue());});
        }
        vertices->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {editBinaryVertices(&dialog,file,current,edits[current]);});
        controllers->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {flush();editBinaryControllers(&dialog,file,current);load();});
        skin->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {editBinarySkin(&dialog,file,current,edits[current]);});
        dangly->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {editBinaryDangly(&dialog,file,current);});
        transform->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
            flush();editBinaryTransform(&dialog,file,current,edits[current],"Apply",[](wxWindow*) {return true;});load();
        });
        select->Bind(wxEVT_CHOICE,[&](wxCommandEvent&) {flush();load();});
        save->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
            flush();
            try {
                auto outputEdits=edits;
                if (batch->GetValue()) {
                    const auto& source=edits[current];
                    for (const auto index : nodes) if (selectedOffsets.count(*file.model.nodes[index].packedOffset)) {
                        auto& target=outputEdits[index];
                        for (std::size_t i=0;i<source.textures.size();++i)
                            if (source.textures[i]) target.textures[i]=source.textures[i];
                        for (std::size_t i=0;i<source.flags.size();++i)
                            if (source.flags[i]) target.flags[i]=source.flags[i];
                        for (const auto& field : source.headerFields) target.headerFields[field.first]=field.second;
                    }
                }
                std::optional<PlanarTransform> transform;
                if(modelAngle->GetValue()!=0||modelFlipX->GetValue()||modelFlipY->GetValue())transform=planarTransform(float(modelAngle->GetValue()),modelFlipX->GetValue(),modelFlipY->GetValue());
                const auto output=saveBinaryCopy(&dialog,file,current,outputEdits,runJob,transform);
                if (output) {saved=*output;dialog.EndModal(wxID_OK);}
            } catch (const std::exception& error) { wxui::showError(&dialog,error); }
        });
        pages->Enable(!nodes.empty());select->Enable(!nodes.empty());save->Enable(!file.model.nodes.empty());
        if (!nodes.empty()) { select->SetSelection(selection); load(); }
        if (page==BinaryMeshPage::Vertices) dialog.CallAfter([&] {editBinaryVertices(&dialog,file,current,edits[current]);});
        if (page==BinaryMeshPage::Controllers) dialog.CallAfter([&] {editBinaryControllers(&dialog,file,current);});
        wxui::applyTheme(&dialog,wxui::currentDarkMode()); dialog.ShowModal();
    } catch (const std::exception& error) { wxui::showError(parent,error); }
    return saved;
}

namespace {

struct BinaryViewData {
    std::array<std::vector<std::uint8_t>,2> bytes;
    std::vector<std::size_t> rows;
    std::size_t different{};
    std::optional<model::PackedLayout> layout;
    std::size_t size() const { return std::max(bytes[0].size(),bytes[1].size()); }
    bool differs(std::size_t offset) const {
        return offset < size() && (offset >= bytes[0].size() || offset >= bytes[1].size() || bytes[0][offset] != bytes[1][offset]);
    }
};

wxColour typeColour(model::BinaryType type) {
    static const wxColour light[]{wxColour(238,238,238),wxColour(228,230,250),wxColour(247,223,232),
        wxColour(214,231,251),wxColour(245,211,220),wxColour(199,222,250),wxColour(211,242,217),
        wxColour(252,235,187),wxColour(234,216,251)};
    static const wxColour dark[]{wxColour(48,48,48),wxColour(58,60,95),wxColour(91,50,68),
        wxColour(42,66,96),wxColour(98,45,62),wxColour(32,59,94),wxColour(35,76,48),
        wxColour(91,72,30),wxColour(70,45,97)};
    return (wxui::currentDarkMode() ? dark : light)[static_cast<std::size_t>(type)];
}

class BinaryViewTable final : public wxGridTableBase {
public:
    BinaryViewTable(const BinaryViewData& data, bool comparison, bool differences, std::size_t file = 0)
        : data_(data), comparison_(comparison), differences_(differences), file_(file) {}
    int GetNumberRows() override {
        const auto size = comparison_ ? data_.size() : data_.bytes[file_].size();
        return static_cast<int>(differences_ ? data_.rows.size() : (size+15)/16);
    }
    int GetNumberCols() override { return comparison_ ? 32 : 17; }
    std::size_t file(int column) const { return comparison_ ? column/16 : file_; }
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
    const model::PackedLayout* layout() const { return data_.layout ? &*data_.layout : nullptr; }
    std::optional<model::BinaryLocation> location(int row,int column) const {
        return data_.layout && column != 16 ? data_.layout->locate(file(column),offset(row,column)) : std::nullopt;
    }
    wxGridCellAttr* GetAttr(int row,int column,wxGridCellAttr::wxAttrKind) override {
        if (!comparison_) {
            const auto found = location(row,column);
            if (!found || !found->field) return nullptr;
            const auto& field = data_.layout->structures[found->structure].fields[*found->field];
            auto* attr = new wxGridCellAttr;
            attr->SetBackgroundColour(typeColour(field.type));
            attr->SetTextColour(wxui::currentDarkMode() ? *wxWHITE : *wxBLACK); return attr;
        }
        if (!data_.differs(offset(row,column))) return nullptr;
        auto* attr = new wxGridCellAttr;
        const bool dark = wxui::currentDarkMode();
        attr->SetBackgroundColour(dark ? wxColour(100,48,40) : wxColour(255,213,200));
        attr->SetTextColour(dark ? *wxWHITE : *wxBLACK); return attr;
    }
private:
    const BinaryViewData& data_;
    bool comparison_;
    bool differences_;
    std::size_t file_;
};

class BinaryCellRenderer final : public wxGridCellStringRenderer {
public:
    wxGridCellRenderer* Clone() const override { return new BinaryCellRenderer; }
    void Draw(wxGrid& grid,wxGridCellAttr& attr,wxDC& dc,const wxRect& rect,int row,int col,bool selected) override {
        wxGridCellStringRenderer::Draw(grid,attr,dc,rect,row,col,selected);
        const auto& table = *static_cast<BinaryViewTable*>(grid.GetTable());
        const auto found = table.location(row,col);
        if (!found) return;
        const auto& region = table.layout()->structures[found->structure];
        const auto at = table.offset(row,col);
        dc.SetPen(wxPen(wxui::currentDarkMode() ? wxColour(160,160,160) : wxColour(110,110,110)));
        if (found->field) {
            const auto& field = region.fields[*found->field];
            const auto first = region.offset+found->record*region.stride+field.offset;
            const auto width = field.type == model::BinaryType::String ? field.size : model::binaryTypeWidth(field.type);
            if ((at-first)%width == 0) dc.DrawLine(rect.GetLeft(),rect.GetTop()+3,rect.GetLeft(),rect.GetBottom()-2);
        }
        dc.SetPen(wxPen(wxui::currentDarkMode() ? *wxWHITE : *wxBLACK,2));
        if (at == region.offset) {
            dc.DrawLine(rect.GetLeft(),rect.GetTop()+1,rect.GetRight(),rect.GetTop()+1);
            dc.DrawLine(rect.GetLeft()+1,rect.GetTop(),rect.GetLeft()+1,rect.GetBottom());
        }
        if (at+1 == region.offset+region.size()) {
            dc.DrawLine(rect.GetLeft(),rect.GetBottom()-1,rect.GetRight(),rect.GetBottom()-1);
            dc.DrawLine(rect.GetRight()-1,rect.GetTop(),rect.GetRight()-1,rect.GetBottom());
        }
    }
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

struct StructureItem final : wxTreeItemData {
    explicit StructureItem(model::BinaryLocation value) : location(value) {}
    model::BinaryLocation location;
};

class BinaryInspector final : public wxDialog {
public:
    BinaryInspector(wxWindow* parent,std::filesystem::path path,std::vector<std::uint8_t> bytes,const AuthoringJob& runJob)
        : wxDialog(parent,wxID_ANY,"Binary Inspector",wxDefaultPosition,parent->FromDIP(wxSize(1170,800)),
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),runJob_(runJob) {
        paths_[0] = std::move(path); data_.bytes[0] = std::move(bytes);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* controls = new wxBoxSizer(wxHORIZONTAL);
        auto* read = new wxButton(this,wxID_ANY,"Read Model Structures");
        const auto extension = resourceKey(neoshared::pathToUtf8(paths_[0].extension()));
        read->Enable(extension == ".mdl" || extension == ".mdx");
        controls->Add(read,0,wxRIGHT,FromDIP(10));
        files_ = new wxChoice(this,wxID_ANY); files_->Append(neosettings::pathToWx(paths_[0].filename())); files_->SetSelection(0);
        controls->Add(files_,1,wxRIGHT,FromDIP(10));
        mode_ = new wxChoice(this,wxID_ANY); mode_->Append("Hierarchy"); mode_->Append("File order"); mode_->SetSelection(0); mode_->Enable(false);
        controls->Add(mode_,0,wxRIGHT,FromDIP(10));
        controls->Add(new wxStaticText(this,wxID_ANY,"Record"),0,wxALIGN_CENTER_VERTICAL | wxRIGHT,FromDIP(4));
        record_ = new wxSpinCtrl(this,wxID_ANY); record_->SetRange(0,0); record_->Enable(false); controls->Add(record_);
        layout->Add(controls,0,wxEXPAND | wxALL,FromDIP(8));
        path_ = new wxStaticText(this,wxID_ANY,neosettings::pathToWx(paths_[0]));
        layout->Add(path_,0,wxLEFT | wxRIGHT | wxBOTTOM,FromDIP(8));
        auto* legend = new wxBoxSizer(wxHORIZONTAL);
        for (std::size_t i = 0; i < 9; ++i) {
            const auto type = static_cast<model::BinaryType>(i);
            auto* label = new wxStaticText(this,wxID_ANY,wxString(" ")+model::binaryTypeName(type)+" ");
            label->SetBackgroundColour(typeColour(type));
            legend->Add(label,0,wxRIGHT,FromDIP(5));
        }
        layout->Add(legend,0,wxLEFT | wxRIGHT | wxBOTTOM,FromDIP(8));
        layout->Add(new wxStaticText(this,wxID_ANY,
            "Type colors and value separators use the model map. Brackets mark structure starts/ends. Record numbers start at 0.\n"
            "Select a structure/field to jump to its bytes; select a byte to locate its structure. Unclassified bytes retain raw numeric interpretation."),
            0,wxLEFT | wxRIGHT | wxBOTTOM,FromDIP(8));
        split_ = new wxSplitterWindow(this,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxSP_LIVE_UPDATE);
        tree_ = new wxTreeCtrl(split_,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_HIDE_ROOT);
        grid_ = new wxGrid(split_,wxID_ANY); tree_->Hide(); split_->Initialize(grid_); split_->SetMinimumPaneSize(FromDIP(150));
        values_ = new wxTextCtrl(this,wxID_ANY,{},wxDefaultPosition,FromDIP(wxSize(-1,190)),wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        values_->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
        layout->Add(split_,1,wxEXPAND | wxLEFT | wxRIGHT,FromDIP(8));
        layout->Add(values_,0,wxEXPAND | wxALL,FromDIP(8));
        layout->Add(CreateSeparatedButtonSizer(wxCANCEL),0,wxEXPAND | wxALL,FromDIP(8));
        FindWindow(wxID_CANCEL)->SetLabel("Close"); SetSizer(layout); SetMinSize(FromDIP(wxSize(980,580)));
        read->Bind(wxEVT_BUTTON,[&,read](wxCommandEvent&) {
            try {
                BinaryAnalysis result;
                if (!runJob_("Reading binary structures",[&](const auto& job) { result = readBinaryAnalysis(paths_[0],data_.bytes[0],job); })) return;
                const auto previous = paths_[file_];
                paths_ = std::move(result.paths); data_.bytes[0] = std::move(result.mdl);
                if (result.mdx) data_.bytes[1] = std::move(*result.mdx);
                data_.layout = std::move(result.layout);
                file_ = previous == paths_[1] ? 1 : 0;
                files_->Clear(); files_->Append("MDL: "+neosettings::pathToWx(paths_[0].filename()));
                if (!paths_[1].empty()) files_->Append("MDX: "+neosettings::pathToWx(paths_[1].filename()));
                files_->SetSelection(static_cast<int>(file_)); mode_->Enable(); read->Enable(false);
                populate(); tree_->Show(); split_->SplitVertically(tree_,grid_,FromDIP(340));
                display();
            } catch (const std::exception& error) { wxui::showError(this,error); }
        });
        files_->Bind(wxEVT_CHOICE,[&](wxCommandEvent&) { file_ = files_->GetSelection(); display(); });
        mode_->Bind(wxEVT_CHOICE,[&](wxCommandEvent&) {
            const auto selected = selected_; populate();
            if (selected) reveal(*selected);
        });
        tree_->Bind(wxEVT_TREE_ITEM_EXPANDING,[&](wxTreeEvent& event) {
            const auto& item = *static_cast<StructureItem*>(tree_->GetItemData(event.GetItem()));
            if (!item.location.field) fields(item.location.structure);
        });
        tree_->Bind(wxEVT_TREE_SEL_CHANGED,[&](wxTreeEvent& event) {
            auto location = static_cast<StructureItem*>(tree_->GetItemData(event.GetItem()))->location;
            if (selected_ && selected_->structure == location.structure) location.record = selected_->record;
            jump(location);
        });
        record_->Bind(wxEVT_SPINCTRL,[&](wxSpinEvent&) {
            auto location = *selected_; location.record = record_->GetValue(); jump(location);
        });
        grid_->Bind(wxEVT_GRID_SELECT_CELL,[&](wxGridEvent& event) { select(event.GetRow(),event.GetCol()); event.Skip(); });
        wxui::applyTheme(this,wxui::currentDarkMode());
        // Theme application owns control defaults; the type legend retains its palette.
        for (std::size_t i = 0; i < 9; ++i) {
            auto* label = legend->GetItem(i)->GetWindow();
            label->SetBackgroundColour(typeColour(static_cast<model::BinaryType>(i)));
            label->SetForegroundColour(wxui::currentDarkMode() ? *wxWHITE : *wxBLACK);
        }
        display();
    }
    ~BinaryInspector() override { DestroyChildren(); }

private:
    void display() {
        grid_->SetTable(new BinaryViewTable(data_,false,false,file_),true);
        grid_->EnableEditing(false); grid_->SetRowLabelSize(FromDIP(90));
        grid_->SetDefaultCellFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
        grid_->SetDefaultCellAlignment(wxALIGN_CENTER,wxALIGN_CENTER); grid_->SetDefaultRenderer(new BinaryCellRenderer);
        for (int column = 0; column < 17; ++column) grid_->SetColSize(column,FromDIP(column == 16 ? 165 : 33));
        path_->SetLabel(neosettings::pathToWx(paths_[file_])+wxString::Format("  (%zu bytes)",data_.bytes[file_].size()));
        values_->ChangeValue("No byte selected."); selected_.reset(); record_->Enable(false);
        if (grid_->GetNumberRows()) { grid_->SetGridCursor(0,0); select(0,0); }
        grid_->ForceRefresh();
    }
    void populate() {
        wxEventBlocker block(tree_); tree_->Freeze(); tree_->DeleteAllItems();
        const auto root = tree_->AddRoot("Structures"); const auto& map = *data_.layout;
        items_.assign(map.structures.size(),{}); fieldItems_.assign(map.structures.size(),{});
        const auto append = [&](std::size_t i,wxTreeItemId parent) {
            const auto& region = map.structures[i];
            const auto label = wxui::toWx(region.name)+wxString::Format("  [%s 0x%zX, %zu x %zu]",
                region.file == 0 ? "MDL" : "MDX",region.offset,region.count,region.stride);
            items_[i] = tree_->AppendItem(parent,label,-1,-1,new StructureItem({i,0,{}}));
            tree_->SetItemHasChildren(items_[i],!region.fields.empty());
        };
        if (mode_->GetSelection() == 0) {
            for (std::size_t i = 0; i < map.structures.size(); ++i)
                append(i,map.structures[i].parent ? items_[*map.structures[i].parent] : root);
        } else {
            for (std::size_t file = 0; file < 2; ++file) for (const auto i : map.ordered[file]) append(i,root);
        }
        tree_->Expand(root); tree_->Thaw();
    }
    void fields(std::size_t index) {
        if (!fieldItems_[index].empty()) return;
        const auto& region = data_.layout->structures[index];
        for (std::size_t i = 0; i < region.fields.size(); ++i) {
            const auto& field = region.fields[i];
            fieldItems_[index].push_back(tree_->AppendItem(items_[index],wxui::toWx(field.name)+
                wxString::Format("  [+%zu, %zu bytes, %s]",field.offset,field.size,model::binaryTypeName(field.type)),
                -1,-1,new StructureItem({index,0,i})));
        }
    }
    void reveal(const model::BinaryLocation& location) {
        wxEventBlocker block(tree_);
        auto item = items_[location.structure];
        if (location.field) { fields(location.structure); item = fieldItems_[location.structure][*location.field]; }
        tree_->SelectItem(item); tree_->EnsureVisible(item);
    }
    void jump(const model::BinaryLocation& location) {
        const auto& region = data_.layout->structures[location.structure];
        if (file_ != region.file) { file_ = region.file; files_->SetSelection(static_cast<int>(file_)); display(); }
        const auto offset = region.offset+location.record*region.stride+(location.field ? region.fields[*location.field].offset : 0);
        const auto row = static_cast<int>(offset/16), column = static_cast<int>(offset%16);
        grid_->SetGridCursor(row,column); grid_->MakeCellVisible(row,column); select(row,column,location);
    }
    void select(int row,int column,std::optional<model::BinaryLocation> requested = {}) {
        const auto& table = *static_cast<BinaryViewTable*>(grid_->GetTable());
        const auto offset = table.offset(row,column);
        const auto location = requested ? requested : data_.layout ? data_.layout->locate(file_,offset) : std::nullopt;
        wxString text;
        if (location) {
            const auto& region = data_.layout->structures[location->structure];
            text = wxui::toWx(region.name)+wxString::Format(" | 0x%zX..0x%zX (end exclusive), %zu bytes\nRecord %zu of %zu; stride %zu",
                region.offset,region.offset+region.size(),region.size(),location->record,region.count,region.stride);
            if (location->field) {
                const auto& field = region.fields[*location->field];
                text += " | "+wxui::toWx(field.name)+" ("+model::binaryTypeName(field.type)+")";
            } else text += requested ? " | Whole record" : " | Unclassified field";
            text += '\n'; selected_ = location;
            record_->SetRange(0,static_cast<int>(region.count-1)); record_->SetValue(static_cast<int>(location->record));
            record_->Enable(region.count > 1); reveal(*location);
        } else {
            selected_.reset(); record_->Enable(false);
            wxEventBlocker block(tree_); tree_->UnselectAll();
            text = data_.layout ? "Outside mapped structures.\n" : "Raw bytes. Read Model Structures to enable typed navigation for PC MDL/MDX.\n";
        }
        values_->ChangeValue(text+binaryValues(data_.bytes[file_],offset));
    }

    const AuthoringJob& runJob_;
    BinaryViewData data_;
    std::array<std::filesystem::path,2> paths_;
    std::size_t file_{};
    wxChoice *files_{}, *mode_{};
    wxSpinCtrl* record_{};
    wxStaticText* path_{};
    wxSplitterWindow* split_{};
    wxTreeCtrl* tree_{};
    wxGrid* grid_{};
    wxTextCtrl* values_{};
    std::vector<wxTreeItemId> items_;
    std::vector<std::vector<wxTreeItemId>> fieldItems_;
    std::optional<model::BinaryLocation> selected_;
};

void viewBinaryComparison(wxWindow* parent, const AuthoringJob& runJob) {
    std::vector<std::filesystem::path> paths(2);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        wxFileDialog file(parent,i ? "Compare — File B" : "Compare — File A",{},{},
            "Model binaries (*.mdl;*.mdx;*.wok;*.pwk;*.dwk)|*.mdl;*.mdx;*.wok;*.pwk;*.dwk|All files (*.*)|*.*",wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (file.ShowModal() != wxID_OK) return;
        paths[i] = neosettings::pathFromWx(file.GetPath());
    }
    try {
        BinaryViewData data;
        if (!runJob("Comparing binary files",[&](const auto& job) {
            ResourceLoadScope scope(job);
            for (std::size_t i = 0; i < paths.size(); ++i) data.bytes[i] = readResourceFile(paths[i]);
            for (std::size_t row = 0; row*16 < data.size(); ++row) {
                if (row%4096 == 0) job.update(row*16,data.size(),"Comparing stored bytes");
                bool different = false;
                for (std::size_t i = row*16; i < std::min(row*16+16,data.size()); ++i)
                    if (data.differs(i)) { ++data.different; different = true; }
                if (different) data.rows.push_back(row);
            }
        })) return;
        wxDialog dialog(parent,wxID_ANY,"Binary Comparison",
            wxDefaultPosition,parent->FromDIP(wxSize(1230,740)),
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        for (std::size_t i = 0; i < paths.size(); ++i)
            layout->Add(new wxStaticText(&dialog,wxID_ANY,wxString(i ? "B: " : "A: ")+neosettings::pathToWx(paths[i])+
                wxString::Format("  (%zu bytes)",data.bytes[i].size())),0,wxALL,dialog.FromDIP(6));
        layout->Add(new wxStaticText(&dialog,wxID_ANY,wxString::Format("%zu differing byte positions. ",data.different)+
                "Offsets are absolute; inserted bytes shift subsequent comparisons. — marks absent bytes."),0,wxALL,dialog.FromDIP(6));
        auto* only = new wxCheckBox(&dialog,wxID_ANY,"Show only differing rows"); only->SetValue(true);
        layout->Add(only,0,wxALL,dialog.FromDIP(6));
        layout->Add(new wxStaticText(&dialog,wxID_ANY,
            "Select a byte in either file to interpret values from that offset."),
            0,wxALL,dialog.FromDIP(6));
        auto* grid = new wxGrid(&dialog,wxID_ANY);
        auto* values = new wxTextCtrl(&dialog,wxID_ANY,{},wxDefaultPosition,dialog.FromDIP(wxSize(-1,145)),
            wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        const auto font = wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
        values->SetFont(font);
        const auto select = [&](int row,int column) {
            const auto& table = *static_cast<BinaryViewTable*>(grid->GetTable());
            const auto file = table.file(column);
            values->ChangeValue(wxString(file ? "B: " : "A: ")+
                neosettings::pathToWx(paths[file].filename())+"\n"+binaryValues(data.bytes[file],table.offset(row,column)));
        };
        const auto display = [&] {
            grid->SetTable(new BinaryViewTable(data,true,only->GetValue()),true);
            grid->EnableEditing(false); grid->SetRowLabelSize(dialog.FromDIP(90));
            grid->SetDefaultCellFont(font);
            grid->SetDefaultCellAlignment(wxALIGN_CENTER,wxALIGN_CENTER);
            for (int column = 0; column < grid->GetNumberCols(); ++column)
                grid->SetColSize(column,dialog.FromDIP(33));
            values->ChangeValue("No byte selected.");
            if (grid->GetNumberRows()) { grid->SetGridCursor(0,0); select(0,0); }
            grid->ForceRefresh();
        };
        display();
        only->Bind(wxEVT_CHECKBOX,[&](wxCommandEvent&) { display(); });
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

void inspectBinaryFile(wxWindow* parent, const AuthoringJob& runJob) {
    wxFileDialog file(parent,"Inspect Binary File",{}, {},
        "Model binaries (*.mdl;*.mdx;*.wok;*.pwk;*.dwk)|*.mdl;*.mdx;*.wok;*.pwk;*.dwk|All files (*.*)|*.*",wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (file.ShowModal() != wxID_OK) return;
    try {
        const auto path = neosettings::pathFromWx(file.GetPath());
        std::vector<std::uint8_t> bytes;
        if (!runJob("Reading binary file",[&](const auto& job) { ResourceLoadScope scope(job); bytes = readResourceFile(path); })) return;
        BinaryInspector dialog(parent,path,std::move(bytes),runJob); dialog.ShowModal();
    } catch (const std::exception& error) { wxui::showError(parent,error); }
}
void analyzeBinaryFiles(wxWindow* parent, const AuthoringJob& runJob) {
    wxFileDialog input(parent,"Analyze Binary Models",{}, {},"Binary MDL (*.mdl)|*.mdl;*.MDL",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (input.ShowModal() != wxID_OK) return;
    wxArrayString paths; input.GetPaths(paths);
    std::vector<BinaryAnalysisResult> results;
    const bool completed = runJob("Analyzing binary structures",[&](const auto& job) {
        for (std::size_t i = 0; i < paths.size(); ++i) {
            job.update(i,paths.size(),"Analyzing "+wxui::toStd(paths[i]));
            results.push_back(analyzeBinaryFile(neosettings::pathFromWx(paths[i]),job));
        }
    });
    auto report = binaryBatchReport(results);
    report += std::to_string(results.size())+" of "+std::to_string(paths.size())+" files processed.";
    if (!completed) report += " Cancelled; remaining files were not analyzed.";
    wxDialog dialog(parent,wxID_ANY,"Binary Structure Analysis",wxDefaultPosition,parent->FromDIP(wxSize(1000,740)),
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* text = new wxTextCtrl(&dialog,wxID_ANY,wxui::toWx(report),wxDefaultPosition,wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    text->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
    layout->Add(text,1,wxEXPAND | wxALL,dialog.FromDIP(8));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* save = new wxButton(&dialog,wxID_ANY,"Save Report..."); buttons->AddStretchSpacer();
    buttons->Add(save,0,wxRIGHT,dialog.FromDIP(8)); buttons->Add(new wxButton(&dialog,wxID_CANCEL,"Close"));
    layout->Add(buttons,0,wxEXPAND | wxALL,dialog.FromDIP(8)); dialog.SetSizer(layout);
    dialog.SetMinSize(parent->FromDIP(wxSize(800,480)));
    save->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        wxFileDialog output(&dialog,"Save Binary Analysis",{},"binary-analysis.txt","Text (*.txt)|*.txt",wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (output.ShowModal() != wxID_OK) return;
        try {
            const auto path = neosettings::pathFromWx(output.GetPath());
            std::vector<std::filesystem::path> inputs;
            for (const auto& name : paths) inputs.push_back(neosettings::pathFromWx(name));
            for (const auto& result : results) for (const auto& source : result.inputs) if (!source.empty()) inputs.push_back(source);
            neoshared::checkResourceOutput(path,inputs);
            neoshared::texture::writeFileBytes(path,{report.begin(),report.end()});
        } catch (const std::exception& error) { wxui::showError(&dialog,error); }
    });
    wxui::applyTheme(&dialog,wxui::currentDarkMode()); dialog.ShowModal();
}

void compareBinaryFiles(wxWindow* parent, const AuthoringJob& runJob) { viewBinaryComparison(parent,runJob); }

} // namespace neomdl::ui
