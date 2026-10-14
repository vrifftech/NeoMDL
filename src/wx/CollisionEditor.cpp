#include "wx/AsciiEditor.hpp"
#include "wx/AsciiSource.hpp"
#include "NeoSettings.hpp"
#include <neoshared/texture/FileUtil.hpp>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>

#include <iomanip>

namespace neomdl::ui {

bool confirmCompanionOverwrite(wxWindow* parent,const std::vector<std::filesystem::path>& paths) {
    wxString existing;
    for(const auto& path:paths) if(std::filesystem::exists(path))
        existing+="\n"+wxui::toWx(neoshared::texture::pathToUtf8(path));
    return existing.empty() || wxMessageBox("Replace these output files?\n"+existing,
        "Save Model Outputs",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,parent)==wxYES;
}

void AsciiEditor::editCollisions(const model::Model& asset) {
    auto pending=collisions_;
    wxDialog dialog(this,wxID_ANY,"Collision Companions",wxDefaultPosition,FromDIP(wxSize(1000,720)),
                    wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
    auto* layout=new wxBoxSizer(wxVERTICAL);
    auto* row=new wxBoxSizer(wxHORIZONTAL);
    auto* kind=new wxChoice(&dialog,wxID_ANY);
    for(int i=0;i<5;++i) kind->Append(collisionLabel(static_cast<CollisionKind>(i)));
    auto selected=pending.empty()?CollisionKind::Room:pending.begin()->first;
    kind->SetSelection(static_cast<int>(selected));
    row->Add(kind,1,wxRIGHT,FromDIP(8));
    auto* importFile=new wxButton(&dialog,wxID_ANY,"Import...");
    auto* remove=new wxButton(&dialog,wxID_ANY,"Remove from Export");
    row->Add(importFile,0,wxRIGHT,FromDIP(8));row->Add(remove);
    layout->Add(row,0,wxEXPAND|wxALL,FromDIP(10));
    auto* geometry=new wxBoxSizer(wxHORIZONTAL);
    auto* node=new wxChoice(&dialog,wxID_ANY);
    std::vector<std::size_t> nodes;
    for(std::size_t i=0;i<asset.nodes.size();++i) if(asset.nodes[i].mesh) {
        node->Append(wxui::toWx(asset.nodes[i].name));nodes.push_back(i);
    }
    node->SetSelection(nodes.empty()?wxNOT_FOUND:0);
    geometry->Add(node,1,wxRIGHT,FromDIP(8));
    auto* fromModel=new wxButton(&dialog,wxID_ANY,"Use MDL Mesh");
    auto* toModel=new wxButton(&dialog,wxID_ANY,"Use WOK in MDL AABB");
    geometry->Add(fromModel,0,wxRIGHT,FromDIP(8));geometry->Add(toModel);
    layout->Add(geometry,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(10));
    auto* location=new wxBoxSizer(wxHORIZONTAL);
    location->Add(new wxStaticText(&dialog,wxID_ANY,"Room layout placement (X Y Z)"),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
    auto* placement=new wxTextCtrl(&dialog,wxID_ANY,"0 0 0");
    location->Add(placement,1,wxRIGHT,FromDIP(8));
    auto* layoutFile=new wxButton(&dialog,wxID_ANY,"Read LYT...");location->Add(layoutFile);
    layout->Add(location,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(10));
    auto* text=new wxStyledTextCtrl(&dialog);
    text->SetCodePage(wxSTC_CP_UTF8);text->SetWrapMode(wxSTC_WRAP_NONE);
    text->SetUseTabs(false);text->SetTabWidth(2);text->SetMarginType(0,wxSTC_MARGIN_NUMBER);
    text->SetMarginWidth(0,FromDIP(60));text->SetMarginWidth(1,0);
    const auto colors=wxui::themePalette(wxui::currentDarkMode());
    text->StyleSetFont(wxSTC_STYLE_DEFAULT,wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
    text->StyleSetForeground(wxSTC_STYLE_DEFAULT,colors.text);
    text->StyleSetBackground(wxSTC_STYLE_DEFAULT,colors.field);text->StyleClearAll();
    text->StyleSetForeground(wxSTC_STYLE_LINENUMBER,colors.mutedText);
    text->StyleSetBackground(wxSTC_STYLE_LINENUMBER,colors.panel);
    text->SetCaretForeground(colors.text);text->SetSelBackground(true,colors.selection);
    text->SetSelForeground(true,colors.selectionText);
    layout->Add(text,1,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(10));
    auto* note=new wxStaticText(&dialog,wxID_ANY,
        "Edit vertices, face materials, hooks and roomlinks (face * 3 + edge). Compile rebuilds navigation and AABB data.\n"
        "Use MDL Mesh replaces this companion's geometry and links. Use WOK in MDL AABB replaces the selected model mesh.\n"
        "Room placement is applied when copying between local MDL and world WOK coordinates. Loaded WOK vertices keep their placement.\n"
        "Apply retains companion edits. Save/Export Model includes them; individual companion export is also available below.");
    layout->Add(note,0,wxALL,FromDIP(10));
    auto* exports=new wxBoxSizer(wxHORIZONTAL);
    auto* ascii=new wxButton(&dialog,wxID_ANY,"Export ASCII...");
    auto* binary=new wxButton(&dialog,wxID_ANY,"Compile Companion...");
    exports->Add(ascii,0,wxRIGHT,FromDIP(8));exports->Add(binary);
    layout->Add(exports,0,wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(10));
    layout->Add(dialog.CreateSeparatedButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,FromDIP(10));
    dialog.FindWindow(wxID_OK)->SetLabel("Apply");dialog.SetSizer(layout);dialog.SetMinSize(FromDIP(wxSize(860,580)));
    bool changed=false;
    const auto save=[&] {
        if(pending.count(selected) && text->GetModify()) {
            pending.at(selected).text=wxui::toStd(text->GetText());text->SetSavePoint();changed=true;
        }
    };
    const auto coordinates=[&] {
        model::Vec3 value;std::istringstream input(wxui::toStd(placement->GetValue()));
        input.exceptions(std::ios::failbit|std::ios::badbit);input>>value.x>>value.y>>value.z;return value;
    };
    const auto controls=[&] {
        const bool present=pending.count(selected)!=0;
        fromModel->Enable(!nodes.empty());
        bool unique=false,aabb=false;
        if(node->GetSelection()!=wxNOT_FOUND) {
            const auto& target=asset.nodes[nodes[node->GetSelection()]];
            aabb=target.aabb.has_value();
            unique=std::count_if(asset.nodes.begin(),asset.nodes.end(),[&](const auto& candidate) {
                return model::ascii::equal(candidate.name,target.name);
            })==1;
        }
        toModel->Enable(present && selected==CollisionKind::Room && aabb && unique);
        placement->Enable(selected==CollisionKind::Room);layoutFile->Enable(selected==CollisionKind::Room);
        remove->Enable(present);ascii->Enable(present);binary->Enable(present);
    };
    const auto show=[&] {
        text->SetReadOnly(false);
        text->SetText(pending.count(selected)?wxui::toWx(pending.at(selected).text):
            "# No companion loaded. Import one or choose a model mesh and Use MDL Mesh.\n");
        text->EmptyUndoBuffer();text->SetSavePoint();text->SetReadOnly(!pending.count(selected));controls();
    };
    show();
    kind->Bind(wxEVT_CHOICE,[&](wxCommandEvent&) {save();selected=static_cast<CollisionKind>(kind->GetSelection());show();});
    node->Bind(wxEVT_CHOICE,[&](wxCommandEvent&) {controls();});
    remove->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {pending.erase(selected);changed=true;show();});
    importFile->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        wxFileDialog file(&dialog,"Import Collision Companion",{}, {},
            "Collision files (*.wok;*.pwk;*.dwk)|*.wok;*.pwk;*.dwk|All files (*.*)|*.*",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if(file.ShowModal()!=wxID_OK) return;
        try {
            const auto path=neosettings::pathFromWx(file.GetPath());neoshared::ResourceDocument resource;
            resource.fileName=neoshared::texture::pathToUtf8(path.filename());resource.bytes=readResourceFile(path);
            resource.identity=resourceFileIdentity(path);
            resource.protectedInputs={path};pending[selected]=readCollision(resource,selected);changed=true;show();
        } catch(const std::exception& error) {wxui::showError(&dialog,error);}
    });
    layoutFile->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        wxFileDialog file(&dialog,"Read Room Placement",{}, {},"Layouts (*.lyt)|*.lyt",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
        if(file.ShowModal()!=wxID_OK) return;
        try {
            const auto bytes=readResourceFile(neosettings::pathFromWx(file.GetPath()));
            const auto rooms=collisionLayout(std::string(bytes.begin(),bytes.end()));
            const auto p=rooms.at(model::ascii::lower(asset.name));
            std::ostringstream value;value<<std::setprecision(9)<<p.x<<' '<<p.y<<' '<<p.z;
            placement->ChangeValue(wxui::toWx(value.str()));
        } catch(const std::exception& error) {wxui::showError(&dialog,error);}
    });
    fromModel->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        try {
            save();
            auto mesh=collisionFromModel(asset,nodes[node->GetSelection()],selected,
                selected==CollisionKind::Room?coordinates():model::Vec3{});
            if(pending.count(selected)) {
                const auto previous=readCollisionAscii(pending.at(selected).text,selected);
                mesh.mesh.usePoints=previous.mesh.usePoints;mesh.mesh.doorUsePoints=previous.mesh.doorUsePoints;
                mesh.walkableMaterials=previous.walkableMaterials;
            }
            pending[selected].text=writeCollisionAscii(mesh,selected);changed=true;show();
        } catch(const std::exception& error) {wxui::showError(&dialog,error);}
    });
    toModel->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        try {
            save();useRoomCollision(asset,nodes[node->GetSelection()],
                readCollisionAscii(pending.at(CollisionKind::Room).text,CollisionKind::Room),coordinates());
            dialog.EndModal(wxID_OK);
        } catch(const std::exception& error) {wxui::showError(&dialog,error);}
    });
    const auto exportOne=[&](bool packed) {
        save();
        const auto name=collisionPath(std::filesystem::u8path(asset.name+(packed?"-packed.mdl":"-ascii.mdl")),selected);
        wxFileDialog file(&dialog,packed?"Compile Collision Companion":"Export Collision ASCII",{},
            wxui::toWx(neoshared::texture::pathToUtf8(name)),"Collision files (*.wok;*.pwk;*.dwk)|*.wok;*.pwk;*.dwk",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(file.ShowModal()!=wxID_OK) return;
        try {
            const auto path=neosettings::pathFromWx(file.GetPath());auto inputs=source_->openPaths();
            for(const auto& [kind,collision]:pending)
                inputs.insert(inputs.end(),collision.inputs.begin(),collision.inputs.end());
            neoshared::checkResourceOutput(path,inputs);
            const auto output=collisionOutputs({{selected,pending.at(selected)}},path,packed);
            neoshared::texture::writeFileBytes(path,output.front().bytes);
        } catch(const std::exception& error) {wxui::showError(&dialog,error);}
    };
    ascii->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {exportOne(false);});
    binary->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {exportOne(true);});
    if(dialog.ShowModal()!=wxID_OK) return;
    save();
    if(changed) {collisions_=std::move(pending);collisionsModified_=true;changed_();}
}

void AsciiEditor::useRoomCollision(const model::Model& asset,std::size_t index,
                                 const CollisionMesh& collision,model::Vec3 placement) {
    const auto pose=model::bindPose(asset);
    std::ostringstream vertices,faces;vertices<<std::setprecision(9)<<collision.mesh.vertices.size();
    for(const auto& v:collision.mesh.vertices) {
        const auto p=collisionLocalPoint(pose.worldTransforms[index],{v[0],v[1],v[2]},placement);
        vertices<<"\n    "<<p.x<<' '<<p.y<<' '<<p.z;
    }
    faces<<collision.mesh.faces.size();
    for(const auto& face:collision.mesh.faces)
        faces<<"\n    "<<face.vertices[0]<<' '<<face.vertices[1]<<' '<<face.vertices[2]<<" 1 0 0 0 "<<face.material;
    Edits edits{{"verts",vertices.str()},{"faces",faces.str()},{"aabb",std::nullopt}};
    for(const auto* field:{"normals","normalindices","tverts","tverts1","tverts2","tverts3",
        "texindices1","texindices2","texindices3","tangentbasis","tangentindices","cpuverts","cpuindices",
        "colors","colorsrgba","colorindices","faceplanes","spherecenter","sphereradius",
        "vertexindices","vertexindicescount","leftoverfaces"}) edits[field]=std::nullopt;
    SourceText source(text());source.editNode(asset.nodes[index],index,edits);replaceText(source.text());
}

} // namespace neomdl::ui
