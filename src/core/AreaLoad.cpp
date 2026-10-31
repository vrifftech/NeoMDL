#include "core/AreaLoad.hpp"
#include "core/AreaLoadTables.hpp"
#include <gff/GFFFile.hpp>
#include <twoda/TwoDAFile.hpp>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace neomdl {
namespace {
using namespace neogff;
void require(bool b,const std::string& s) {if(!b) throw std::runtime_error("Area load: "+s);}
const GffField* field(const GffStruct& s,const char* name) {
    return s.GetFieldByLabel(name);
}
template<class T> const T* typed(const GffStruct& s,const char* n) {
    return dynamic_cast<const T*>(field(s,n));
}
float number(const GffStruct& s,const char* n,float fallback=0) {
    const auto* p=typed<GffFloatField>(s,n);const float v=p?p->value:fallback;
    require(std::isfinite(v),"non-finite "+std::string(n));return v;
}
std::int32_t integer32(const GffStruct& s,const char* n,std::int32_t fallback=0) {
    const auto* p=typed<GffIntField>(s,n);return p?p->value:fallback;
}
model::Vec3 position(const GffStruct& s,const char* n,model::Vec3 fallback={}) {
    const auto* p=typed<GffPositionField>(s,n);if(!p)return fallback;
    const model::Vec3 value{p->value[0],p->value[1],p->value[2]};
    require(std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z),
            "non-finite "+std::string(n));
    return value;
}
model::Quaternion orientation(const GffStruct& s,const char* n,
                              model::Quaternion fallback={}) {
    const auto* p=typed<GffOrientationField>(s,n);if(!p)return fallback;
    // ReadFieldQuaternion stores w,x,y,z; the runtime quaternion is x,y,z,w.
    return {p->value[1],p->value[2],p->value[3],p->value[0]};
}
std::optional<bool> optionalFlag(const GffStruct& s,const char* n) {
    const auto* p=typed<GffByteField>(s,n);if(!p)return {};
    return p->value!=0;
}
std::optional<std::uint8_t> optionalSmallInt(const GffStruct& s,const char* n) {
    const auto* f=field(s,n);if(!f)return {};
    if(const auto* p=dynamic_cast<const GffByteField*>(f))return p->value;
    if(const auto* p=dynamic_cast<const GffIntField*>(f)) {
        require(p->value>=0&&p->value<=255,"out-of-range "+std::string(n));
        return static_cast<std::uint8_t>(p->value);
    }
    throw std::runtime_error("Area load: wrong GFF type for "+std::string(n));
}
std::uint32_t dword(const GffStruct& s,const char* n,std::uint32_t fallback=0) {
    const auto* p=typed<GffUInt32Field>(s,n);return p?p->value:fallback;
}
std::uint16_t word(const GffStruct& s,const char* n,std::uint16_t fallback=0) {
    const auto* p=typed<GffWordField>(s,n);return p?p->value:fallback;
}
std::uint8_t byte(const GffStruct& s,const char* n,std::uint8_t fallback=0) {
    const auto* p=typed<GffByteField>(s,n);return p?p->value:fallback;
}
std::string resref(const GffStruct& s,const char* n) {
    const auto* p=typed<GffResRefField>(s,n);return p&&!p->GetString().empty()?resourceKey(p->GetString()):std::string{};
}
std::string string(const GffStruct& s,const char* n) {
    const auto* p=typed<GffExoStringField>(s,n);return p?p->GetString():std::string{};
}
std::string localized(const GffStruct& s,const char* n) {
    const auto* p=typed<GffLocalizedStringField>(s,n);return p?p->GetString():std::string{};
}
const GffStruct& localizedSource(const GffStruct& placement,const GffStruct& templ,const char* n) {
    return typed<GffLocalizedStringField>(placement,n)?placement:templ;
}
std::optional<model::Vec3> color(const GffStruct& s,const char* n) {
    const auto* p=typed<GffUInt32Field>(s,n);if(!p)return {};
    return model::Vec3{float(p->value&255)/255.f,float((p->value>>8)&255)/255.f,float((p->value>>16)&255)/255.f};
}
void loadGff(GffFile& out,const neoshared::ResourceDocument& r,const char* type) {
    const auto& job=resourceLoadJob();job.update(0,0,"Decoding "+r.fileName);
    out.LoadBytes(r.bytes);job.check();require(out.root()&&out.filetype()==type&&out.version()=="V3.2","expected "+std::string(type)+" V3.2");
}
using Tables=detail::AreaLoadTables;
void creatureVisual(AreaPlacement& placement,const GffStruct& data,bool templates,const ResourceResolver& resolver,model::GameVersion game) {
    // Preserve the original load contract for the required appearance field.
    require(field(data,"Appearance_Type"),"creature has no Appearance_Type; UseTemplates is off or the UTC is incomplete");
    (void)word(data,"Appearance_Type");
    auto state=readCreatureAppearance(data,templates,resolver,placement.diagnostics,resourceLoadJob(),game);
    auto appearance=resolveCreatureAppearance(state,resolver);
    placement.diagnostics.insert(placement.diagnostics.end(),appearance.diagnostics.begin(),appearance.diagnostics.end());
    placement.appearance=appearance.appearance;
    placement.modelResref=appearance.bodyModel;placement.texture=appearance.bodyTexture;
    for(const auto& attachment:appearance.attachments)if(attachment.kind==VisualAttachmentKind::CreatureHead) {
        placement.head=attachment.model;placement.headTexture=attachment.texture;break;
    }
    placement.creatureAuthoredState=state;placement.creatureState=std::move(state);
    placement.creatureAppearance=std::move(appearance);
}
}
model::EngineFrame gitBearingFrame(model::Vec3 p,float bearing) {
    require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(bearing),"non-finite placement");
    return {p,{0,0,std::sin(bearing*.5f),std::cos(bearing*.5f)}};
}
model::EngineFrame gitCreatureFrame(model::Vec3 p,model::Vec3 direction) {
    require(std::isfinite(direction.x)&&std::isfinite(direction.y)&&std::isfinite(direction.z),"non-finite creature orientation");

    const float yaw=direction.x==0&&direction.y==0?0:std::atan2(-direction.x,direction.y);
    return gitBearingFrame(p,yaw);
}
model::Mat4 gitBearingTransform(model::Vec3 p,float bearing) {
    const auto frame=gitBearingFrame(p,bearing);return model::nodeTransform(frame.position,frame.orientation);
}
model::Mat4 gitCreatureTransform(model::Vec3 p,model::Vec3 direction) {
    const auto frame=gitCreatureFrame(p,direction);return model::nodeTransform(frame.position,frame.orientation);
}
AreaLoadPlan readAreaLoad(const std::string& area,const std::string& scope,
    ResourceResolver supplied,const AreaLoadOptions& options,const neoshared::bif::JobControl& job) {
    ResourceLoadScope work(job);job.update(0,0,"Reading module area "+area);
    require(!scope.empty()&&bool(supplied),"an explicit scope identity and resolver are required");
    AreaLoadPlan plan;plan.game=options.game;plan.area=resourceKey(area);plan.scopeIdentity=scope;
    plan.trace=std::make_shared<ResourceTrace>();plan.resolver=traceResources(std::move(supplied),plan.trace,scope+"/"+plan.area);
    const auto required=[&](const std::string& name,std::uint16_t type) {
        auto r=plan.resolver(name,type);require(bool(r),"missing scoped "+name+"."+resourceExtension(type));return std::move(*r);
    };
    struct Template {std::string identity;std::unique_ptr<const GffFile> file;};
    std::map<std::pair<std::string,std::uint16_t>,Template> templateFiles;
    const auto readTemplate=[&](const std::string& name,std::uint16_t type,const char* signature)->const Template& {
        const auto key=std::make_pair(name,type);
        auto found=templateFiles.find(key);
        if(found==templateFiles.end()) {
            auto resource=required(name,type);
            auto file=std::make_unique<GffFile>();loadGff(*file,resource,signature);
            found=templateFiles.emplace(key,Template{std::move(resource.identity),std::move(file)}).first;
        }
        return found->second;
    };
    const auto are=required(plan.area,0x7dc),git=required(plan.area,0x7e7);
    GffFile a,g;loadGff(a,are,"ARE ");loadGff(g,git,"GIT ");
    plan.layout=required(plan.area,0xbb8);
    const auto& ar=*a.root();const auto& gr=*g.root();
    plan.miniGame=readTrackMiniGame(ar,job);
    plan.settings.dirty=readDirtyArea(ar);
    auto& environment=plan.settings.environment;
    // K1 Desktop LoadAreaHeader 0x19d226 / K2 Desktop 0x41983e. These are
    // engine read defaults, including the fog-distance clamp. Scene ambient
    // is DynAmbientColor; Sun/MoonAmbientColor address gidy_sun_amb instead.
    const auto engineColor=[&](const char* name) {
        return color(ar,name).value_or(model::Vec3{});
    };
    const auto engineFogDistance=[&](const char* name) {
        return std::max(0.0f,number(ar,name,10000.0f));
    };
    environment.sceneAmbient=engineColor("DynAmbientColor");
    environment.day.globalAmbient=engineColor("SunAmbientColor");
    environment.day.diffuse=engineColor("SunDiffuseColor");
    environment.day.fogColor=engineColor("SunFogColor");
    environment.day.fogNear=engineFogDistance("SunFogNear");
    environment.day.fogFar=engineFogDistance("SunFogFar");
    environment.day.fogEnabled=byte(ar,"SunFogOn")!=0;
    environment.day.shadowFlag=byte(ar,"SunShadows");
    environment.day.shadowsEnabled=*environment.day.shadowFlag!=0;
    environment.night.globalAmbient=engineColor("MoonAmbientColor");
    environment.night.diffuse=engineColor("MoonDiffuseColor");
    environment.night.fogColor=engineColor("MoonFogColor");
    environment.night.fogNear=engineFogDistance("MoonFogNear");
    environment.night.fogFar=engineFogDistance("MoonFogFar");
    environment.night.fogEnabled=byte(ar,"MoonFogOn")!=0;
    environment.night.shadowFlag=byte(ar,"MoonShadows");
    environment.night.shadowsEnabled=*environment.night.shadowFlag!=0;
    environment.dayNightCycle=byte(ar,"DayNightCycle")!=0;
    environment.startsNight=byte(ar,"IsNight")!=0;
    environment.shadowOpacity=byte(ar,"ShadowOpacity",50u);
    // LoadAreaHeader 0x68c530 reads INT defaults and stores their low bytes.
    // These are not BYTE GFF fields; type mismatches use the engine zero input.
    const auto weatherChance=[&](const char* name) {
        const auto* value=dynamic_cast<const GffIntField*>(field(ar,name));
        return static_cast<std::uint8_t>(value?value->value:0);
    };
    environment.weather.chanceRain=weatherChance("ChanceRain");
    environment.weather.chanceSnow=weatherChance("ChanceSnow");
    environment.weather.chanceLightning=weatherChance("ChanceLightning");
    if(const auto* flags=dynamic_cast<const GffUInt32Field*>(field(ar,"Flags")))environment.weather.areaFlags=flags->value;
    if(environment.weather.areaFlags&1u) {
        environment.weather.chanceRain=0;environment.weather.chanceSnow=0;environment.weather.chanceLightning=0;
    }
    environment.weather.currentWeather=optionalSmallInt(ar,"CurrentWeather");
    environment.weather.disabled=optionalFlag(ar,"DisableWeather");
    // Compatibility aliases preserve the previous API without collapsing the
    // two engine ambient channels in the new runtime.
    plan.settings.ambient=environment.day.globalAmbient;
    plan.settings.diffuse=environment.day.diffuse;
    plan.settings.wind=static_cast<std::uint8_t>(integer32(ar,"WindPower"));
    if(environment.weather.areaFlags&1u)plan.settings.wind=0;
    // IFO is optional for an explicitly selected area; retain provenance when
    // present and diagnose a mismatch rather than switching to another area.
    if(auto ifo=plan.resolver("module",0x7de)) {
        GffFile m;loadGff(m,*ifo,"IFO ");




        const auto timeByte=[&](const char* name,std::uint8_t value=0) {
            const auto* input=dynamic_cast<const GffByteField*>(m.root()->GetFieldByLabel(name));
            return input?input->value:value;
        };
        const auto timeWord=[&](const char* name) {
            const auto* input=dynamic_cast<const GffWordField*>(m.root()->GetFieldByLabel(name));
            return input?input->value:std::uint16_t{};
        };
        const auto timeDword=[&](const char* name,std::uint32_t value=0) {
            const auto* input=dynamic_cast<const GffUInt32Field*>(m.root()->GetFieldByLabel(name));
            return input?input->value:value;
        };
        ModuleTimeSettings time;
        time.minutesPerHour=timeByte("Mod_MinPerHour");
        time.dawnHour=timeByte("Mod_DawnHour");time.duskHour=timeByte("Mod_DuskHour");
        time.startYear=timeDword("Mod_StartYear",1340u);
        time.startMonth=timeByte("Mod_StartMonth",6u);time.startDay=timeByte("Mod_StartDay",1u);
        time.startHour=timeByte("Mod_StartHour",23u);
        time.startMinute=timeWord("Mod_StartMinute");time.startSecond=timeWord("Mod_StartSecond");
        time.startMillisecond=timeWord("Mod_StartMiliSec");
        time.transitionMilliseconds=timeDword("Mod_Transition");
        environment.moduleTime=time;
        if(const auto* list=typed<GffList>(*m.root(),"Mod_Area_list")) {
            bool found=false;for(std::size_t n=0;n<list->count();++n)found|=resref(*list->GetStruct(n),"Area_Name")==plan.area;
            if(!found)plan.diagnostics.push_back("Selected ARE is not listed in module.ifo Mod_Area_list; selection remains explicit.");
        }
    }
    const bool templates=byte(gr,"UseTemplates")!=0;
    Tables tables{plan.resolver,{}};
    struct Family {const char* list;AreaObjectKind kind;std::uint32_t structure;std::uint16_t type;const char* signature;};
    const Family families[]={{"Creature List",AreaObjectKind::Creature,4,0x7eb,"UTC "},
        {"Door List",AreaObjectKind::Door,8,0x7fa,"UTD "},{"Placeable List",AreaObjectKind::Placeable,9,0x7fc,"UTP "}};
    for(const auto& family:families) if(const auto* list=typed<GffList>(gr,family.list)) {
        for(std::size_t n=0;n<list->count();++n) {
            job.update(n,list->count(),"Reading "+std::string(family.list));
            const auto& placement=*list->GetStruct(n);
            if(placement.typeid_!=family.structure)continue;
            AreaPlacement p;p.kind=family.kind;p.sourceIndex=n;p.label=std::string(family.list)+"["+std::to_string(n)+"]";
            try {
                p.templateResref=resref(placement,"TemplateResRef");
                const GffStruct* data=&placement;
                if(templates) {
                    require(!p.templateResref.empty(),p.label+" has UseTemplates but no TemplateResRef");
                    const auto& t=readTemplate(p.templateResref,family.type,family.signature);
                    p.templateIdentity=t.identity;data=t.file->root();
                }
                const auto& tagSource=templates&&family.kind==AreaObjectKind::Door&&options.game==model::GameVersion::Kotor2&&typed<GffExoStringField>(placement,"Tag")?placement:*data;
                const auto tag=string(tagSource,"Tag");if(!tag.empty())p.label+=" "+tag;
                if(family.kind==AreaObjectKind::Creature) {
                    p.frame=gitCreatureFrame({number(placement,"XPosition"),number(placement,"YPosition"),number(placement,"ZPosition")},
                        {number(placement,"XOrientation"),number(placement,"YOrientation"),number(placement,"ZOrientation")});
                    p.world=model::nodeTransform(p.frame.position,p.frame.orientation);
                    creatureVisual(p,*data,templates,plan.resolver,options.game);
                } else {
                    p.frame=gitBearingFrame({number(placement,"X"),number(placement,"Y"),number(placement,"Z")},number(placement,"Bearing"));
                    p.world=model::nodeTransform(p.frame.position,p.frame.orientation);
                    p.staticFlag=byte(*data,"Static")!=0;
                    AreaObjectTrapRecord trap;
                    trap.target={family.kind==AreaObjectKind::Door?TrapObjectKind::Door:TrapObjectKind::Placeable,n};
                    trap.objectId=dword(placement,"ObjectId",0x7f000000u);
                    trap.position=p.frame.position;
                    trap.faction=dword(*data,"Faction");
                    // Door Load retains constructor ones for these three fields;
                    // LoadPlaceable supplies explicit zero defaults instead.
                    const std::uint8_t defaultFlag=family.kind==AreaObjectKind::Door?1u:0u;
                    trap.detectable=byte(*data,"TrapDetectable",defaultFlag);
                    trap.disarmable=byte(*data,"TrapDisarmable",defaultFlag);
                    trap.oneShot=byte(*data,"TrapOneShot",defaultFlag);
                    trap.armed=byte(*data,"TrapFlag");
                    trap.trapType=byte(*data,"TrapType",255u);
                    trap.detectDc=byte(*data,"TrapDetectDC");trap.disarmDc=byte(*data,"DisarmDC");
                    trap.onTrapTriggered=resref(*data,"OnTrapTriggered");
                    trap.onDisarm=resref(*data,"OnDisarm");
                    trap.locked=byte(*data,"Locked");trap.openLockDc=byte(*data,"OpenLockDC");
                    trap.onOpen=resref(*data,"OnOpen");trap.onUnlock=resref(*data,"OnUnlock");
                    if(options.game==model::GameVersion::Kotor2) {
                        trap.openLockDifficulty=byte(*data,"OpenLockDiff",family.kind==AreaObjectKind::Door?1u:0u);
                        if(const auto* modifier=typed<GffCharField>(*data,"OpenLockDiffMod"))
                            trap.openLockDifficultyModifier=static_cast<std::int8_t>(modifier->value);
                        trap.pcLevelAtSpawn=byte(*data,"PCLevelAtSpawn");
                    }
                    if(family.kind==AreaObjectKind::Door)trap.linkedToFlags=byte(placement,"LinkedToFlags");
                    p.trap=std::move(trap);
                    if(family.kind==AreaObjectKind::Placeable) {
                        require(field(*data,"Appearance"),"placeable has no Appearance");
                        p.appearance=dword(*data,"Appearance");p.modelResref=resourceKey(tables.stringValue("placeables",p.appearance,"modelname"));
                        // Both engines load visual state from the UTP or inline
                        // record: Open, saved Animation, then AnimationState.
                        {
                            if(byte(*data,"Open"))
                                p.authoredVisualState.animation=placeableServerAnimation(0x275bu);
                            else if(const auto* animation=typed<GffIntField>(*data,"Animation"))
                                p.authoredVisualState.animation=placeableServerAnimation(static_cast<std::uint16_t>(animation->value));
                            else {
                                p.animationState=byte(*data,"AnimationState");
                                const auto command=placeableAnimationState(*p.animationState);
                                p.authoredVisualState.animation=placeableServerAnimation(command.serverAnimation);
                                p.authoredVisualState.animation->serializedState=*p.animationState;
                            }
                        }
                    } else {
                        // LoadDoor stores Appearance DWORD as AL; nonzero
                        // selects doortypes.Model, zero selects genericdoors.
                        p.appearance=dword(*data,"Appearance")&255u;
                        if(p.appearance) {
                            p.modelResref=resourceKey(tables.stringValue("doortypes",p.appearance,"model"));
                            // LoadDoor initializes one, then consumes the typed
                            // output even on failure (all four engine callers).
                            int visible=1;
                            engine2da::getInt(tables.table("doortypes"),static_cast<int>(p.appearance),"visiblemodel",visible);
                            p.visible=visible!=0;
                        } else p.modelResref=resourceKey(tables.stringValue("genericdoors",byte(*data,"GenericType"),"modelname"));
                        // LoadDoorExternal does not override template OpenState.
                        p.openState=byte(*data,"OpenState");
                        p.authoredVisualState.animation=doorOpenStateAnimation(
                            options.game==model::GameVersion::Kotor1&&!p.visible?3u:*p.openState);
                    }
                    // K1 engine door/placeable loaders have no tweak-color producer.
                    const auto& tweakSource=templates?placement:*data;
                    if(options.game==model::GameVersion::Kotor2&&byte(tweakSource,"UseTweakColor")) {
                        const auto tweak=dword(tweakSource,"TweakColor");
                        ObjectColorShift shift;shift.enabled=true;shift.amount=1.0f;
                        shift.color={float(tweak&255u)/255.0f,float((tweak>>8u)&255u)/255.0f,float((tweak>>16u)&255u)/255.0f};
                        shift.propagateToReferences=true;p.authoredVisualState.colorShift=shift;
                    }
                }
                if(p.openState) p.diagnostics.push_back("OpenState="+std::to_string(*p.openState)+" mapped to engine server animation 0x"+[&]{std::ostringstream x;x<<std::hex<<p.authoredVisualState.animation->serverAnimation;return x.str();}()+"; clip resolution is deferred to the scoped model runtime.");
                if(p.animationState) p.diagnostics.push_back("AnimationState="+std::to_string(*p.animationState)+" mapped to engine server animation 0x"+[&]{std::ostringstream x;x<<std::hex<<p.authoredVisualState.animation->serverAnimation;return x.str();}()+"; clip resolution is deferred to the scoped model runtime.");
            } catch(const neoshared::bif::JobCancelled&) {throw;
            } catch(const std::exception& e) {
                if(!options.allowPartialVisuals)throw;
                p.modelResref.clear();p.diagnostics.push_back(e.what());
            }
            if(p.trap)p.trap->doorVisibleModel=p.visible;
            plan.placements.push_back(std::move(p));
        }
    }
    // CSWSArea::LoadTriggers. The server object/template state and the
    // placement-owned geometry remain distinct. Trigger footprints are admitted
    // to TriggerScene later; gameplay scripts and transitions are retained only
    // as inspection metadata.
    if(const auto* list=typed<GffList>(gr,"TriggerList")) {
        for(std::size_t n=0;n<list->count();++n) {
            job.update(n,list->count(),"Reading area triggers");
            AreaTriggerRecord record;record.sourceIndex=n;
            try {
                const auto& placement=*list->GetStruct(n);
                if(placement.typeid_!=1u)continue;
                record.objectId=dword(placement,"ObjectId",0x7f000000u);
                record.templateResref=resref(placement,"TemplateResRef");
                const GffStruct* data=&placement;
                if(templates) {
                    require(!record.templateResref.empty(),"TriggerList["+std::to_string(n)+"] has UseTemplates but no TemplateResRef");
                    const auto& t=readTemplate(record.templateResref,0x7f0,"UTT ");
                    record.templateIdentity=t.identity;data=t.file->root();
                }
                // K1 0x1a0fbe / K2 Desktop 0x41d164: the UTT owns object
                // fields; only transition links and placement/geometry are
                // restored from GIT after LoadFromTemplate.
                if(const auto* tag=dynamic_cast<const GffExoStringField*>(data->GetFieldByLabel("Tag")))
                    record.tag=tag->GetString();
                record.label="TriggerList["+std::to_string(n)+"]";
                if(!record.tag.empty())record.label+=" "+record.tag;
                else if(const auto name=localized(*data,"LocalizedName");!name.empty())
                    record.label+=" "+name;
                record.position={number(placement,"XPosition"),number(placement,"YPosition"),number(placement,"ZPosition")};
                record.orientation={number(*data,"XOrientation"),number(*data,"YOrientation"),number(*data,"ZOrientation")};
                record.type=integer32(*data,"Type",0);
                record.highlightHeight=number(*data,"HighlightHeight",0.1f);
                if(!(record.highlightHeight>0.0f))record.highlightHeight=0.1f;
                record.cursor=byte(*data,"Cursor");
                if(record.type==1)record.cursor=1;
                else if(record.type==2)record.cursor=0;
                record.linkedToFlags=byte(placement,"LinkedToFlags");
                record.linkedTo=string(placement,"LinkedTo");
                record.linkedToModule=resref(placement,"LinkedToModule");
                record.transitionDestination=localized(localizedSource(placement,*data,"TransitionDestin"),"TransitionDestin");
                record.faction=dword(*data,"Faction");
                record.trapType=byte(*data,"TrapType",255);
                record.trapFlag=byte(*data,"TrapFlag")!=0;
                record.trapOneShot=byte(*data,"TrapOneShot",1)!=0;
                record.trapDisarmable=byte(*data,"TrapDisarmable")!=0;
                record.trapDetectable=byte(*data,"TrapDetectable")!=0;
                record.setByPlayerParty=byte(*data,"SetByPlayerParty")!=0;
                record.keyName=string(*data,"KeyName");
                record.autoRemoveKey=byte(*data,"AutoRemoveKey")!=0;
                // LoadTrigger reads CResRef fields with the constructor's
                // literal "default" retained when a field is absent.
                const auto script=[&](const char* name) {
                    const auto* value=typed<GffResRefField>(*data,name);
                    return value?value->GetString():std::string("default");
                };
                record.onTrapTriggered=script("OnTrapTriggered");
                record.onEnter=script("ScriptOnEnter");
                record.onDisarm=script("OnDisarm");
                if(record.type==2) {
                    // Client Add K1 0x92d4e / K2 Desktop 0x2ca002 uses traps.Model;
                    // the literal substitution is only for a failed 2DA entry.
                    record.modelResref=resourceKey(tables.lookup("traps",record.trapType,"Model").value_or("v_mnfrag"));
                }
                const auto* geometry=typed<GffList>(placement,"Geometry");
                auto geometryPosition=record.position;
                if(!geometry&&templates) {
                    // No GIT Geometry call means the already loaded UTT world
                    // geometry survives. SetPosition does not translate it.
                    geometry=typed<GffList>(*data,"Geometry");
                    geometryPosition={number(*data,"XPosition"),number(*data,"YPosition"),number(*data,"ZPosition")};
                }
                require(geometry,"TriggerList["+std::to_string(n)+"] has no Geometry footprint");
                record.localFootprint.reserve(geometry->count());record.worldFootprint.reserve(geometry->count());
                for(std::size_t pointIndex=0;pointIndex<geometry->count();++pointIndex) {
                    job.check();
                    const auto& point=*geometry->GetStruct(pointIndex);
                    require(point.typeid_==3u,"wrong struct ID in TriggerList Geometry");
                    const model::Vec3 local{number(point,"PointX"),number(point,"PointY"),number(point,"PointZ")};
                    record.localFootprint.push_back(local);
                    record.worldFootprint.push_back({local.x+geometryPosition.x,local.y+geometryPosition.y,local.z+geometryPosition.z});
                }
                if(record.type!=0&&record.type!=1&&record.type!=2)
                    record.diagnostics.push_back("Unclassified engine trigger Type="+std::to_string(record.type)+" retained without inferred gameplay behavior.");
            } catch(const neoshared::bif::JobCancelled&) {throw;
            } catch(const std::exception& e) {
                if(!options.allowPartialVisuals)throw;
                record.diagnostics.push_back(e.what());
                plan.diagnostics.push_back("TriggerList["+std::to_string(n)+"] omitted: "+e.what());
                continue;
            }
            plan.triggers.push_back(std::move(record));
        }
    }
    // CSWSArea::LoadPlaceableCameras. The registry stores Pitch, Height and
    // MicRange alongside the transform; they remain separate authored inputs.
    if(const auto* list=typed<GffList>(gr,"CameraList")) {
        const auto cameraCapacity=options.game==model::GameVersion::Kotor1?50u:70u;
        // K1 Desktop 0x199cbc and K2 Desktop 0x41ef2a have different capacities.
        require(list->count()<=cameraCapacity,"CameraList exceeds engine "+std::to_string(cameraCapacity)+"-camera registry");
        for(std::size_t n=0;n<list->count();++n) {
            job.update(n,list->count(),"Reading area cameras");
            AreaCameraRecord record;record.sourceIndex=n;
            try {
                const auto& entry=*list->GetStruct(n);
                record.cameraId=integer32(entry,"CameraID",-1);
                record.position=position(entry,"Position");
                record.orientation=orientation(entry,"Orientation");
                record.pitch=number(entry,"Pitch");record.height=number(entry,"Height");
                record.fieldOfView=number(entry,"FieldOfView",55.0f);
                record.microphoneRange=number(entry,"MicRange");
            } catch(const neoshared::bif::JobCancelled&) {throw;
            } catch(const std::exception& e) {
                if(!options.allowPartialVisuals)throw;
                record.diagnostics.push_back(e.what());
                plan.diagnostics.push_back("CameraList["+std::to_string(n)+"] omitted: "+e.what());
                continue;
            }
            plan.cameras.push_back(std::move(record));
        }
    }
    if(const auto* list=typed<GffList>(gr,"SoundList")) {
        for(std::size_t n=0;n<list->count();++n) {
            job.update(n,list->count(),"Reading area sounds");
            try {
                const auto& placement=*list->GetStruct(n);
                if(placement.typeid_!=6u)continue; // LoadSounds admits struct type 6
                AreaSoundRecord record;record.sourceIndex=n;
                record.templateResref=resref(placement,"TemplateResRef");
                const GffStruct* data=&placement;
                if(templates) {
                    const auto& t=readTemplate(record.templateResref,0x7f3,"UTS ");
                    record.templateIdentity=t.identity;data=t.file->root();
                }
                // The engine typed GFF readers return their supplied value on
                // an absent field or a different field type.
                const auto b=[&](const char* name,std::uint8_t value=0) {
                    const auto* p=dynamic_cast<const GffByteField*>(data->GetFieldByLabel(name));return p?p->value:value;
                };
                const auto d=[&](const char* name) {
                    const auto* p=dynamic_cast<const GffUInt32Field*>(data->GetFieldByLabel(name));return p?p->value:0u;
                };
                const auto f=[&](const char* name,float value=0) {
                    const auto* p=dynamic_cast<const GffFloatField*>(data->GetFieldByLabel(name));return p?p->value:value;
                };
                record.tag=string(*data,"Tag");
                record.active=b("Active",1)!=0;record.positional=b("Positional",1)!=0;
                record.looping=b("Looping")!=0;record.volume=b("Volume",127);
                record.volumeVariation=b("VolumeVrtn");record.times=b("Times",3);
                record.pitchVariation=f("PitchVariation");record.fixedVariance=f("FixedVariance",1);
                record.hours=d("Hours");record.interval=d("Interval");record.intervalVariation=d("IntervalVrtn");
                record.minimumDistance=f("MinDistance",10);record.maximumDistance=f("MaxDistance",20);
                record.continuous=b("Continuous")!=0;record.random=b("Random")!=0;
                record.randomPosition=b("RandomPosition")!=0;
                record.randomRangeX=f("RandomRangeX");record.randomRangeY=f("RandomRangeY");
                record.generatedType=std::uint8_t(d("GeneratedType"));
                record.position={f("XPosition"),f("YPosition"),f("ZPosition")};
                // Load returns the status of the last typed field read: the
                // final Sound resref, or ZPosition when no Sound is read.
                bool loaded=dynamic_cast<const GffFloatField*>(data->GetFieldByLabel("ZPosition"))!=nullptr;
                if(templates) {
                    const auto p=[&](const char* name) {
                        const auto* input=dynamic_cast<const GffFloatField*>(placement.GetFieldByLabel(name));return input?input->value:0.0f;
                    };
                    record.position={p("XPosition"),p("YPosition"),p("ZPosition")};
                    const auto* generated=dynamic_cast<const GffUInt32Field*>(placement.GetFieldByLabel("GeneratedType"));
                    record.generatedType=generated?std::uint8_t(generated->value):0u;
                }
                if(const auto* entries=dynamic_cast<const GffList*>(data->GetFieldByLabel("Sounds")))
                    for(std::size_t entry=0;entry<entries->count();++entry) {
                        const auto* name=dynamic_cast<const GffResRefField*>(entries->GetStruct(entry)->GetFieldByLabel("Sound"));
                        loaded=name!=nullptr;
                        if(name)record.sounds.push_back(engineSceneAudioResRef(name->GetString()));
                    }
                if(loaded)plan.sounds.push_back(std::move(record));
            } catch(const neoshared::bif::JobCancelled&) {throw;
            } catch(const std::exception& error) {
                if(!options.allowPartialVisuals)throw;
                plan.diagnostics.push_back("SoundList["+std::to_string(n)+"] omitted: "+error.what());
            }
        }
    }
    for(const auto* name:{"WaypointList","Encounter List","StoreList","List"})
        if(const auto* list=typed<GffList>(gr,name);list&&list->count())
            plan.diagnostics.push_back(std::string(name)+": "+std::to_string(list->count())+" records have no established visual consumer; debug-marker geometry is not fabricated.");
    if(options.loadGrass&&number(ar,"Grass_Density")>0) {
        try {const auto surface=required("surfacemat",0x7e1);plan.grass=grassSettingsFromArea(are.bytes,surface.bytes,job,options.game);}
        catch(const neoshared::bif::JobCancelled&){throw;}
        catch(const std::exception& e){if(!options.allowPartialVisuals)throw;plan.diagnostics.push_back(e.what());}
    }
    job.check();return plan;
}
} // namespace neomdl
