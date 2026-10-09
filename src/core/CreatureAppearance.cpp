#include "core/CreatureAppearance.hpp"
#include "core/AreaLoadTables.hpp"
#include <gff/GFFFile.hpp>
#include <twoda/TwoDAFile.hpp>
#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace neomdl {
namespace {
using namespace neogff;
void require(bool value,const std::string& message) {
    if(!value)throw std::runtime_error("Creature appearance: "+message);
}
std::string lower(std::string value) {
    for(auto& c:value)if(c>='A'&&c<='Z')c=char(c+32);
    return value;
}
const GffField* field(const GffStruct& data,const char* name) {
    return data.GetFieldByLabel(name);
}
template<class T> const T* typed(const GffStruct& data,const char* name) {
    // Engine CResGFF::ReadField* returns the supplied default on a missing
    // field or a type mismatch; new appearance fields retain that behavior.
    return dynamic_cast<const T*>(field(data,name));
}
std::uint8_t byte(const GffStruct& data,const char* name,std::uint8_t initial=0) {
    const auto* value=typed<GffByteField>(data,name);return value?value->value:initial;
}
std::uint16_t word(const GffStruct& data,const char* name,std::uint16_t initial=0) {
    const auto* value=typed<GffWordField>(data,name);return value?value->value:initial;
}
std::uint32_t dword(const GffStruct& data,const char* name,std::uint32_t initial=0) {
    const auto* value=typed<GffUInt32Field>(data,name);return value?value->value:initial;
}
std::int32_t integer(const GffStruct& data,const char* name,std::int32_t initial=0) {
    const auto* value=typed<GffIntField>(data,name);return value?value->value:initial;
}
std::string text(const GffStruct& data,const char* name) {
    const auto* value=typed<GffExoStringField>(data,name);return value?value->GetString():std::string{};
}
std::string resref(const GffStruct& data,const char* name) {
    const auto* value=typed<GffResRefField>(data,name);return value?value->GetString():std::string{};
}
using Tables=detail::AreaLoadTables;
int number(Tables& tables,const std::string& file,std::uint32_t row,const std::string& column) {
    // BaseItemArray::Load and CreateHead consume the output unconditionally,
    // including DEFAULT on a missing column (K2 0x464abd/0x34d3c7).
    int value=0;engine2da::getInt(tables.table(file),static_cast<int>(row),column,value);return value;
}
bool textureExists(const ResourceResolver& resolver,const std::string& name) {
    return resolver(name,kTgaResourceType).has_value()||resolver(name,kTpcResourceType).has_value();
}
std::string numbered(const std::string& base,unsigned variation,unsigned width) {
    std::ostringstream result;result<<base<<std::setfill('0')<<std::setw(int(width))<<variation;
    return result.str();
}
CreatureEquipmentItem readItem(const GffStruct& data,std::uint32_t slot,Tables& tables,model::GameVersion game) {
    CreatureEquipmentItem result;result.slot=slot;

    result.baseItem=integer(data,"BaseItem",30);
    if(const auto* variation=typed<GffByteField>(data,"ModelVariation"))result.modelVariation=variation->value;
    else {result.modelVariation=byte(data,"ModelPart1",1);if(!result.modelVariation)result.modelVariation=1;}
    result.modelType=std::uint8_t(number(tables,"baseitems",std::uint32_t(result.baseItem),"ModelType"));

    result.itemType=std::uint8_t(number(tables,"baseitems",std::uint32_t(result.baseItem),"ItemType"));
    result.itemClass=tables.stringValue("baseitems",std::uint32_t(result.baseItem),"ItemClass");
    result.itemClass.resize(std::min<std::size_t>(11,result.itemClass.size()));
    result.defaultModel=tables.stringValue("baseitems",std::uint32_t(result.baseItem),"DefaultModel");
    // Baseitems loader 0x464c4f retains only the low byte of GenderSpecific.
    result.genderSpecific=std::uint8_t(number(tables,"baseitems",std::uint32_t(result.baseItem),"GenderSpecific"))!=0;
    if(result.modelType==1) {
        result.bodyVariation=1;
        const auto value=tables.stringValue("baseitems",std::uint32_t(result.baseItem),"BodyVar");



        const char maximumBody=game==model::GameVersion::Kotor1?'J':'Z';
        if(value.size()==1&&value[0]>='A'&&value[0]<=maximumBody)result.bodyVariation=std::uint8_t(value[0]-'@');
        result.textureVariation=byte(data,"TextureVar",1);
    }


    const auto upgrades=dword(data,"Upgrades");
    if(const auto* properties=typed<GffList>(data,"PropertiesList"))
        for(std::size_t i=0;i<properties->count();++i) {
            resourceLoadJob().check();
            const auto& property=*properties->GetStruct(i);
            const auto upgrade=byte(property,"UpgradeType",255);
            if(word(property,"PropertyName")==59&&
               (upgrade==255||((upgrades>>(upgrade&31u))&1u)))
                result.disguiseAppearance=word(property,"Subtype");
        }
    return result;
}
CreatureEquipmentItem loadItem(const std::string& name,std::uint32_t slot,Tables& tables,model::GameVersion game) {
    const auto& job=resourceLoadJob();job.update(0,0,"Reading equipped item "+name);
    const auto resource=tables.resolver(resourceKey(name),0x7e9);
    require(bool(resource),"missing equipped item "+name+".uti");
    GffFile file;file.LoadBytes(resource->bytes);job.check();
    auto result=readItem(*file.root(),slot,tables,game);result.resref=resourceKey(name);result.identity=resource->identity;
    return result;
}
std::uint8_t animationType(const std::string& value) {


    if(value.find('S')!=std::string::npos)return 11;
    if(value.find('B')!=std::string::npos)return 3;
    if(value.find('F')!=std::string::npos||value.find('L')!=std::string::npos)return 2;
    return 11;
}
char bodyColumnLetter(const CreatureAppearanceState& state) {
    // K1 Desktop 0x81ada..0x81b1f caps at J, K2 Desktop 0x13e600 at N.
    const unsigned maximum=state.game==model::GameVersion::Kotor1?10u:14u;
    return state.armorEquipped&&state.bodyVariation
        ?char(std::min<unsigned>(state.bodyVariation,maximum)+'@'):'A';
}
std::string bodyTexture(const CreatureAppearanceState& state,std::uint16_t row,Tables& tables) {
    const char letter=bodyColumnLetter(state);
    const auto base=tables.cell("appearance",row,std::string("TEX")+letter);
    if(base.empty())return {};


    auto name=numbered(base,state.textureVariation?state.textureVariation:1,2);
    if(!textureExists(tables.resolver,name))name=numbered(base,1,2);
    return resourceKey(name);
}
const CreatureAppearanceAttachment* head(const CreatureAppearance& appearance) {
    const auto it=std::find_if(appearance.attachments.begin(),appearance.attachments.end(),
        [](const auto& a){return a.kind==VisualAttachmentKind::CreatureHead;});
    return it==appearance.attachments.end()?nullptr:&*it;
}
}

CreatureEquipmentItem readCreatureEquipment(const std::string& name,std::uint32_t slot,const ResourceResolver& resolver,
    const neoshared::bif::JobControl& job,model::GameVersion game) {
    ResourceLoadScope work(job);job.check();
    Tables tables{resolver,{}};return loadItem(name,slot,tables,game);
}
void setCreatureEquipment(CreatureAppearanceState& state,std::vector<CreatureEquipmentItem> equipment) {
    // K1 EquipItem 0x1bc274 applies properties to every admitted slot;
    // the alternate-weapon exclusion is added by the K2 caller.
    const auto appliesProperties=[&](const auto& item) {
        return state.game==model::GameVersion::Kotor1||(item.slot!=0x40000&&item.slot!=0x80000);
    };
    const bool previouslyEquippedDisguise=std::any_of(state.equipment.begin(),state.equipment.end(),[&](const auto& item) {
        return appliesProperties(item)&&item.disguiseAppearance.has_value();
    });
    state.equipment=std::move(equipment);state.armorEquipped=false;
    state.bodyVariation=0;state.textureVariation=0;state.disguiseAppearance.reset();


    for(const auto& item:state.equipment) {
        if(item.slot==2) {state.armorEquipped=true;state.bodyVariation=item.bodyVariation;state.textureVariation=item.textureVariation;}
        // EquipItem 0x618c50 skips property application for alternate weapons.
        if(appliesProperties(item)&&item.disguiseAppearance)
            state.disguiseAppearance=item.disguiseAppearance;
    }



    if(previouslyEquippedDisguise&&!state.disguiseAppearance&&state.disguised) {
        state.appearance=state.originalAppearance;state.disguised=false;
    }
}
CreatureAppearanceState readCreatureAppearance(const GffStruct& data,bool useEquipmentTemplates,
    const ResourceResolver& resolver,std::vector<std::string>& diagnostics,const neoshared::bif::JobControl& job,model::GameVersion game) {
    ResourceLoadScope work(job);job.check();
    CreatureAppearanceState state;state.game=game;
    require(field(data,"Appearance_Type"),"creature has no Appearance_Type; UseTemplates is off or the UTC is incomplete");


    state.tag=text(data,"Tag");state.appearance=word(data,"Appearance_Type");
    state.phenotype=std::uint8_t(integer(data,"Phenotype"));state.gender=std::min<std::uint8_t>(byte(data,"Gender"),4);
    state.headVariation=byte(data,"Appearance_Head");if(!state.headVariation)state.headVariation=1;
    state.headOverride=byte(data,"DuplicatingHead",255);state.backupHead=byte(data,"UseBackupHead")!=0;
    state.alignment=std::min<unsigned>(byte(data,"GoodEvil"),100);



    state.race=byte(data,"Race",6u);
    Tables raceTables{resolver,{}};
    const auto raceCount=static_cast<std::uint8_t>(raceTables.table("racialtypes").rowCount());
    require(state.race<raceCount,"Race is outside engine racialtypes entries");
    // LoadCreature 0x625699/0x6256c1: Appearance_Type is already the current
    // disguised row. PM_Appearance is retained exclusively for restoration.
    state.disguised=byte(data,"PM_IsDisguised")!=0;state.originalAppearance=word(data,"PM_Appearance");
    std::vector<CreatureEquipmentItem> equipment;
    if(const auto* list=typed<GffList>(data,"Equip_ItemList"))for(std::size_t i=0;i<list->count();++i) {
        job.update(i,list->count(),"Reading creature equipment");
        const auto& entry=*list->GetStruct(i);
        // ReadItemsFromGff 0x624710 selects UTI vs inline strictly from its
        // caller flag. A failed template item is deleted and the loop continues.
        try {
            Tables tables{resolver,{}};
            equipment.push_back(useEquipmentTemplates?loadItem(resref(entry,"EquippedRes"),entry.typeid_,tables,game)
                :readItem(entry,entry.typeid_,tables,game));
        } catch(const neoshared::bif::JobCancelled&) {throw;
        } catch(const std::exception& error) {
            diagnostics.push_back("Equipped item["+std::to_string(i)+"] was not loaded: "+error.what());
        }
    }
    setCreatureEquipment(state,std::move(equipment));return state;
}

CreatureAppearance resolveCreatureAppearance(const CreatureAppearanceState& state,const ResourceResolver& resolver,
    const CreatureAppearance* previous,const neoshared::bif::JobControl& job) {
    ResourceLoadScope work(job);job.update(0,0,"Resolving creature appearance");
    Tables tables{resolver,{}};CreatureAppearance result;result.game=state.game;
    result.appearance=state.disguiseAppearance.value_or(state.appearance);
    bool skipAlignment=false;


    if(state.game==model::GameVersion::Kotor2&&lower(state.tag)=="handmaiden"&&state.armorEquipped) {
        if(!state.disguised&&!state.disguiseAppearance)
            result.appearance=state.bodyVariation==2?(std::int16_t(state.alignment)<=40?669:454):638;
        skipAlignment=state.bodyVariation==2;
    }
    result.modelType=tables.stringValue("appearance",result.appearance,"modeltype");
    result.animationType=animationType(result.modelType);


    engine2da::getFloat(tables.table("appearance"),result.appearance,"height",result.height);
    result.bodyEnvironmentMap=tables.stringValue("appearance",result.appearance,"envmap");
    if(result.animationType==3) {
        const char letter=bodyColumnLetter(state);
        result.bodyModel=resourceKey(tables.cell("appearance",result.appearance,std::string("model")+letter,true));
        result.bodyTexture=bodyTexture(state,result.appearance,tables);
    } else {


        result.bodyModel=resourceKey(tables.stringValue("appearance",result.appearance,"race"));
        result.bodyTexture=tables.stringValue("appearance",result.appearance,"racetex");
    }
    const auto hasHeadItem=std::any_of(state.equipment.begin(),state.equipment.end(),[](const auto& item){return item.slot==1;});
    std::optional<std::size_t> headIndex;
    if(result.animationType==3&&(state.headVariation||hasHeadItem)) {

        result.headRow=state.headOverride!=255?state.headOverride:
            number(tables,"appearance",result.appearance,state.backupHead?"backuphead":"normalhead");
        {
            // CreateHead reads HEAD even for a negative NORMALHEAD/BACKUPHEAD;
            // C2DA supplies DEFAULT and the caller checks the resulting string.
            const auto model=tables.stringValue("heads",std::uint32_t(result.headRow),"head");
            if(!model.empty()) {
                CreatureAppearanceAttachment attachment;attachment.kind=VisualAttachmentKind::CreatureHead;
                attachment.model=resourceKey(model);attachment.hook="headhook";
                attachment.environmentMap=result.bodyEnvironmentMap;
                // K1 CreateHead 0x82e48 keeps the MDL-authored head texture.
                // K2 CreateHead 0x34d86a additionally applies ALTTEXTURE.
                if(state.game==model::GameVersion::Kotor2) {
                    const auto alternate=tables.stringValue("heads",std::uint32_t(result.headRow),"alttexture");
                    if(!alternate.empty()&&textureExists(resolver,alternate))attachment.texture=resourceKey(alternate);
                }
                if(previous)if(const auto* old=head(*previous))attachment.texture=old->texture;
                headIndex=result.attachments.size();result.attachments.push_back(std::move(attachment));
            }
        }
    }
    if(headIndex&&!skipAlignment) {
        auto& headAttachment=result.attachments[*headIndex];
        const auto alignment=std::int16_t(state.alignment);
        const char* column=alignment<=10?"headtexvvve":alignment<=20?"headtexvve":
            alignment<=30?"headtexve":alignment<=40?"headtexe":nullptr;
        const auto dark=column?tables.cell("heads",std::uint32_t(result.headRow),column):std::string{};



        if(!dark.empty()) {if(textureExists(resolver,dark))headAttachment.texture=resourceKey(dark);}
        else headAttachment.texture=state.game==model::GameVersion::Kotor1?std::string{}:
            tables.stringValue("heads",std::uint32_t(result.headRow),"alttexture");
        const char* bodyColumn=nullptr;
        // K1 0x82ae4..0x82b49 only substitutes unarmored TEXAEVIL at <=30.
        if(state.game==model::GameVersion::Kotor1) {
            if(!state.armorEquipped&&alignment<=30)bodyColumn="texaevil";
        } else if(alignment<=40) {
            if(!state.armorEquipped)bodyColumn="texaevil";
            else switch(state.bodyVariation) {
            case 2:bodyColumn="texbevil";break;
            case 9:bodyColumn="texievil";break;
            case 12:bodyColumn="texlevil";break;
            case 14:bodyColumn="texmevil";break;
            default:break;
            }
        }
        const auto evil=bodyColumn?tables.cell("appearance",result.appearance,bodyColumn):std::string{};
        if(!evil.empty()) {
            const auto name=numbered(evil,1,2);
            if(textureExists(resolver,name))result.bodyTexture=resourceKey(name);
            else if(previous)result.bodyTexture=previous->bodyTexture;
        }
    }


    float weaponScale=0.0f;
    // SwitchWeaponEvent explicitly writes one on a failed read; a literal
    // nonempty packed **** is a successful zero (K2 0x4351ec..0x43520a).
    if(!engine2da::getFloat(tables.table("appearance"),result.appearance,"weaponscale",weaponScale))
        weaponScale=1.0f;
    for(const auto& item:state.equipment) {
        job.check();
        if(item.slot!=1&&item.slot!=0x10&&item.slot!=0x20)continue;
        if(item.slot==1&&!headIndex)continue;
        CreatureAppearanceAttachment attachment;attachment.kind=VisualAttachmentKind::EquippedItem;
        attachment.equipmentSlot=item.slot;
        attachment.equipmentItemType=item.itemType;
        std::string base=item.itemClass;
        if(item.genderSpecific) {base+='_';base+=state.gender?'F':'M';}
        attachment.model=numbered(base+"_",item.modelVariation,3);
        // CreateItem 0x307d1d..0x307d7b substitutes baseitems.DefaultModel
        // only after the computed MDL resource does not exist.
        if(!resolver(attachment.model,kMdlResourceType)) {
            attachment.model=item.defaultModel;
            // CreateItem 0x307e18 diagnoses a missing DefaultModel but retains
            // the item; Wield/CreateHead skip its null visual part. The owning
            // creature and its equipped item properties remain present.
            if(attachment.model.empty()||!resolver(attachment.model,kMdlResourceType)) {
                result.diagnostics.push_back("Equipped item in slot "+std::to_string(item.slot)+
                    " has no computed or DefaultModel MDL; its engine visual part is absent.");
                continue;
            }
        }
        attachment.model=resourceKey(attachment.model);
        if(item.slot==1) {attachment.hook="GoggleHook";attachment.parentAttachment=headIndex;}
        else {attachment.hook=item.slot==0x10?"rhand":"lhand";attachment.scale=weaponScale;}
        result.attachments.push_back(std::move(attachment));
    }
    return result;
}

bool creatureAppearanceChangesGeometry(const CreatureAppearanceState& a,const CreatureAppearanceState& b) {


    if(a.game!=b.game||a.appearance!=b.appearance||a.disguiseAppearance!=b.disguiseAppearance||a.gender!=b.gender||
       a.headVariation!=b.headVariation||a.headOverride!=b.headOverride||a.backupHead!=b.backupHead||
       a.armorEquipped!=b.armorEquipped||a.bodyVariation!=b.bodyVariation||a.textureVariation!=b.textureVariation||
       a.equipment.size()!=b.equipment.size())return true;
    for(std::size_t i=0;i<a.equipment.size();++i) {
        const auto& x=a.equipment[i];const auto& y=b.equipment[i];
        if(x.slot!=y.slot||x.resref!=y.resref||x.identity!=y.identity||x.baseItem!=y.baseItem||
           x.modelVariation!=y.modelVariation||x.textureVariation!=y.textureVariation)return true;
    }
    return false;
}
std::shared_ptr<ModelDocument> assembleCreatureAppearance(const CreatureAppearance& appearance,
    const ResourceResolver& resolver,const std::string& suffix,const neoshared::bif::JobControl& job,
    const model::AsciiResourceResolver& asciiResources) {
    ResourceLoadScope work(job);job.update(0,0,"Assembling creature "+appearance.bodyModel);
    const auto load=[&](const std::string& name) {
        const auto resource=resolver(name,kMdlResourceType);require(bool(resource),"missing scoped object model: "+name);
        auto document=std::make_shared<ModelDocument>();document->openResource(*resource,resolver,job,appearance.game,asciiResources);
        require(document->model().game==appearance.game,"GIT object game differs from the selected area: "+name);return document;
    };
    const auto body=load(appearance.bodyModel);std::vector<VisualModelAttachment> attachments;
    for(const auto& item:appearance.attachments) {
        job.check();
        VisualModelAttachment attachment;attachment.document=load(item.model);attachment.hook=item.hook;
        attachment.texture=item.texture;attachment.environmentMap=item.environmentMap;attachment.kind=item.kind;
        attachment.equipmentSlot=item.equipmentSlot;attachment.parentAttachment=item.parentAttachment;attachment.scale=item.scale;
        attachment.equipmentItemType=item.equipmentItemType;
        attachments.push_back(std::move(attachment));
    }
    return ModelDocument::visualView(body,suffix,appearance.bodyTexture,attachments,appearance.bodyEnvironmentMap);
}
} // namespace neomdl
