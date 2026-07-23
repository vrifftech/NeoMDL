#include "core/DialogueAnimation.hpp"
#include "core/DialogueObjectAnimation.hpp"
#include "core/CreatureAppearance.hpp"
#include "core/ModelDocument.hpp"
#include "core/Engine2DA.hpp"
#include <twoda/TwoDAFile.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace neomdl {
namespace {
// ServerToClientAnimation Desktop 0x221c9c. The
// sparse constant cases below are transcribed from the engine jump tables.
bool feat207(const DialogueAnimationContext& context) {
    if (!context.feat207) throw std::runtime_error("Dialogue animation requires the actor's engine feat 207 state");
    return *context.feat207;
}
std::uint16_t weaponReady(std::uint16_t id,const DialogueAnimationContext& c) {
    constexpr std::uint16_t values[]={92,133,174,215,223,237,245,0,245};
    if(c.weaponClass<1u || c.weaponClass>9u)return id;
    return c.weaponClass==8u?(feat207(c)?535u:249u):values[c.weaponClass-1u];
}
std::uint16_t weaponWalk(const DialogueAnimationContext& c) {
    switch(c.weaponClass) {
    case 2:return c.engineFieldE8?338u:0u;
    case 3:return c.engineFieldE8?341u:0u;
    case 4:return c.engineFieldE8?339u:0u;
    case 7:case 9:return 340u;
    default:return 0u;
    }
}
std::uint16_t weaponRun(const DialogueAnimationContext& c) {
    switch(c.weaponClass) {
    case 2:return c.engineFieldE8?345u:2u;
    case 3:return c.engineFieldE8?343u:2u;
    case 4:return c.engineFieldE8?3u:2u;
    case 7:case 9:return 342u;
    default:return 2u;
    }
}
std::uint16_t weaponAction(const DialogueAnimationContext& c,bool second) {
    constexpr std::uint16_t first[]={377,378,376,473,474,475,10157,476};
    constexpr std::uint16_t next[]={132,173,214,222,236,244,10158,244};
    if(c.weaponClass<2u || c.weaponClass>9u)return second?10158u:10157u;
    return second?next[c.weaponClass-2u]:first[c.weaponClass-2u];
}
std::uint16_t weaponIdle(const DialogueAnimationContext& c) {
    constexpr std::uint16_t values[]={91,132,173,214,222,236,244,0,244};
    if(c.weaponClass<1u || c.weaponClass>9u)return 10246u;
    return c.weaponClass==8u?(feat207(c)?534u:373u):values[c.weaponClass-1u];
}
std::uint16_t weaponIdleNonHumanoid(const DialogueAnimationContext& c) {
    constexpr std::uint16_t values[]={91,132,173,214,222,236,244,10246,244};
    return c.weaponClass>=1u && c.weaponClass<=9u?values[c.weaponClass-1u]:10246u;
}
std::uint16_t weaponGesture(const DialogueAnimationContext& c,bool second) {
    switch(c.weaponClass) {
    case 2:return c.lightsaber?std::uint16_t(second?110:109):455u;
    case 3:return c.lightsaber?std::uint16_t(second?151:150):455u;
    case 4:return c.lightsaber?std::uint16_t(second?192:191):455u;
    case 8:return feat207(c)?std::uint16_t(second?493:492):455u;
    default:return 455u;
    }
}
std::uint16_t weaponDeactivate(const DialogueAnimationContext& c) {
    constexpr std::uint16_t values[]={124,206,165,220,234,242,0,242};
    if(c.weaponClass<2u || c.weaponClass>9u)return 10302u;
    return c.weaponClass==8u?(feat207(c)?526u:280u):values[c.weaponClass-2u];
}
std::uint16_t humanoid(std::uint16_t id,const DialogueAnimationContext& context) {
    switch(id) {
    case 10000u: case 10149u: return 6u;
    case 10001u: case 10148u: return weaponReady(id,context);
    case 10002u: case 10003u: return weaponWalk(context);
    case 10004u: return context.runLooksAwful ? 0u : weaponRun(context);
    case 10005u: case 10007u: case 10219u: case 10220u: return 85u;
    case 10006u: return 81u;
    case 10008u: return 83u;
    case 10009u: case 10109u: return 300u;
    case 10011u: return 302u;
    case 10012u: return 301u;
    case 10014u: case 10077u: return 303u;
    case 10015u: return 62u;
    case 10016u: return 64u;
    case 10017u: return 63u;
    case 10018u: return 65u;
    case 10019u: return 67u;
    case 10021u: return 310u;
    case 10022u: return 333u;
    case 10023u: return 77u;
    case 10028u: return 33u;
    case 10029u: return 31u;
    case 10030u: return 18u;
    case 10032u: case 10033u: return 24u;
    case 10034u: return 16u;
    case 10035u: return 19u;
    case 10038u: return 25u;
    case 10039u: return 27u;
    case 10040u: return 26u;
    case 10041u: return 29u;
    case 10042u: return 28u;
    case 10044u: return 17u;
    case 10052u: return 7u;
    case 10053u: return 11u;
    case 10054u: return 10u;
    case 10055u: return 12u;
    case 10056u: return 13u;
    case 10057u: return 14u;
    case 10058u: return 15u;
    case 10059u: return 40u;
    case 10060u: return 41u;
    case 10061u: return 70u;
    case 10070u: return 37u;
    case 10092u: return 8u;
    case 10093u: return 1u;
    case 10094u: return context.runLooksAwful ? 1u : 4u;
    case 10117u: return 75u;
    case 10118u: return 270u;
    case 10120u: return 32u;
    case 10121u: return 44u;
    case 10122u: return 53u;
    case 10123u: return 54u;
    case 10124u: return 74u;
    case 10125u: return 43u;
    case 10126u: return 68u;
    case 10127u: return 38u;
    case 10128u: case 10134u: return 47u;
    case 10129u: return 57u;
    case 10130u: return 58u;
    case 10131u: case 10135u: return 48u;
    case 10132u: case 10141u: case 10165u: return 51u;
    case 10133u: return 5u;
    case 10136u: return 38u;
    case 10137u: return 76u;
    case 10138u: return 78u;
    case 10139u: return 79u;
    case 10140u: return 52u;
    case 10142u: return 346u;
    case 10145u: return 355u;
    case 10147u: return 357u;
    case 10150u: return 72u;
    case 10151u: return 359u;
    case 10152u: return 360u;
    case 10154u: return 370u;
    case 10155u: return 371u;
    case 10156u: return 375u;
    case 10157u: return weaponAction(context, false);
    case 10158u: return weaponAction(context, true);
    case 10159u: return 34u;
    case 10160u: return 35u;
    case 10161u: return 71u;
    case 10162u: return 69u;
    case 10163u: return 384u;
    case 10164u: return 385u;
    case 10221u: return 80u;
    case 10222u: return 82u;
    case 10223u: return 381u;
    case 10224u: return 382u;
    case 10237u: case 10238u: return 23u;
    case 10246u: return weaponIdle(context);
    case 10271u: return 383u;
    case 10300u: return weaponGesture(context, false);
    case 10301u: return weaponGesture(context, true);
    case 10302u: return weaponDeactivate(context);
    case 10400u: case 10402u: return 84u;
    case 10401u: return 115u;
    case 10403u: return 462u;
    case 10404u: return 463u;
    case 10405u: return 464u;
    case 10406u: return 465u;
    case 10407u: return 466u;
    case 10408u: return 467u;
    case 10409u: return 468u;
    case 10410u: return 469u;
    case 10411u: return 470u;
    case 10412u: return 471u;
    case 10413u: return 472u;
    case 10414u: return 461u;
    case 10415u: return 459u;
    case 10416u: return 460u;
    case 10417u: return 536u;
    case 10418u: return 457u;
    case 10419u: return 458u;
    case 10424u: return 316u;
    case 10425u: return 317u;
    case 10426u: return 318u;
    case 10427u: return 319u;
    case 10428u: return 320u;
    case 10429u: return context.weaponClass>=2u && context.weaponClass<=4u ? std::uint16_t(context.weaponClass+387u) : 10429u;
    case 10499u: return 557u;
    case 10500u: return 558u;
    case 10501u: return 560u;
    case 10503u: return 561u;
    case 10504u: return 562u;
    case 10505u: return 563u;
    case 10506u: return 564u;
    case 10507u: return 565u;
    case 10508u: return 566u;
    case 10509u: return 567u;
    case 10510u: return 568u;
    case 10511u: return 569u;
    case 11000u: return 66u;
    case 11001u: return 537u;
    case 11002u: return 538u;
    default:return id;
    }
}

std::uint16_t nonHumanoid(std::uint16_t id,const DialogueAnimationContext& context) {
    switch(id) {
    case 10000u: case 10055u: case 10056u: case 10057u: case 10058u: case 10092u: case 10149u: return 256u;
    case 10001u: case 10148u: return 278u;
    case 10002u: case 10003u: case 10084u: case 10133u: return 253u;
    case 10004u: case 10094u: return context.runLooksAwful ? 253u : 255u;
    case 10006u: case 10008u: case 10156u: return 275u;
    case 10009u: case 10109u: return 300u;
    case 10011u: return 302u;
    case 10012u: return 301u;
    case 10014u: case 10077u: case 10302u: return 280u;
    case 10015u: case 10016u: return 379u;
    case 10021u: return 310u;
    case 10022u: return 333u;
    case 10023u: return 268u;
    case 10028u: return 263u;
    case 10030u: case 10155u: return 18u;
    case 10038u: case 10039u: case 10040u: case 10041u: case 10042u: return 261u;
    case 10044u: return 260u;
    case 10052u: return 257u;
    case 10053u: return 258u;
    case 10054u: return 259u;
    case 10074u: return 299u;
    case 10093u: return 254u;
    case 10117u: return 266u;
    case 10118u: return 270u;
    case 10124u: return 74u;
    case 10125u: return 43u;
    case 10137u: return 267u;
    case 10138u: return 269u;
    case 10142u: return 346u;
    case 10147u: return 358u;
    case 10150u: return 264u;
    case 10152u: return 369u;
    case 10154u: return 372u;
    case 10219u: case 10220u: return 272u;
    case 10221u: case 10222u: return 274u;
    case 10223u: case 10224u: return 273u;
    case 10246u: return weaponIdleNonHumanoid(context);
    case 10400u: case 10402u: return 271u;
    case 10401u: return 115u;
    case 10416u: return 460u;
    case 11000u:return 380u;
    case 11001u:return 537u;
    case 11002u:return 538u;
    default:return id;
    }
}
std::uint16_t clientAnimation(std::uint16_t id,const DialogueAnimationContext& c,
                              model::GameVersion game) {
    if(game==model::GameVersion::Kotor1) {
        // K1 Desktop 0x59d9e, humanoid jump table 0x476500 and weapon tables
        // 0x4774ac/0x4774d4/0x4774fc. Reuse the common K2 projection below;
        // these are the engine differences, including K2-only numeric rows.
        if(id==11001u)return 392u;
        if(id==11002u)return 393u;
        if(id==10021u || id==10022u || (id>=10403u && id<11000u))return id;
        if(c.creatureType==1u) {
            switch(id) {
            case 10005u:case 10007u:case 10165u:return id;
            case 10001u:case 10148u:
                if(c.weaponClass==8u)return 249u;
                break;
            case 10246u:
                if(c.weaponClass==8u)return 373u;
                break;
            case 10157u:case 10158u:
                if(c.weaponClass<2u || c.weaponClass>4u)return id;
                break;
            case 10300u:case 10301u:
                if(c.weaponClass==2u)return id==10300u?109u:110u;
                if(c.weaponClass==3u)return 151u;
                if(c.weaponClass==4u)return 192u;
                return id;
            case 10302u:
                if(c.weaponClass==8u)return 280u;
                break;
            default:break;
            }
        }
    }
    return c.creatureType==1u?humanoid(id,c):nonHumanoid(id,c);
}
struct Tables {
    const ResourceResolver& resolver;
    std::map<std::string,neo2da::TwoDAFile> files;
    const neo2da::TwoDAFile& table(const std::string& name) {
        resourceLoadJob().check();
        auto it=files.find(name);
        if(it==files.end()) {
            const auto resource=resolver?resolver(name,0x7e1u):std::nullopt;
            if(!resource)throw std::runtime_error("Dialogue animation requires scoped "+name+".2da");
            neo2da::TwoDAFile file;file.loadBytes(resource->bytes);
            resourceLoadJob().check();it=files.emplace(name,std::move(file)).first;
        }
        return it->second;
    }
    std::string cell(const std::string& name,std::uint32_t row,const std::string& column) {
        std::string value;
        // GetAnimationName clears failed reads before its engine cut/pause
        // naming branches; a nonempty packed **** remains an actual name.
        if(!engine2da::getString(table(name),static_cast<int>(row),column,value))value.clear();
        return value;
    }
    std::optional<int> number(const std::string& name,std::uint32_t row,const std::string& column) {
        int value=0;
        if(!engine2da::getInt(table(name),static_cast<int>(row),column,value))return {};
        return value;
    }
    int numberValue(const std::string& name,std::uint32_t row,const std::string& column) {
        int value=0;engine2da::getInt(table(name),static_cast<int>(row),column,value);return value;
    }

    bool flag(const std::string& name,std::uint32_t row,const std::string& column) {
        return number(name,row,column).value_or(0)!=0;
    }
    float floating(const std::string& name,std::uint32_t row,const std::string& column) {
        float value=0.0f;
        // UpdateShotCamera replaces failed CU_PB_Range reads with zero.
        if(!engine2da::getFloat(table(name),static_cast<int>(row),column,value))value=0.0f;
        return value;
    }
};
struct DialogueAnimationTypes {bool fireForget{},looping{},overlay{};};
DialogueAnimationTypes dialogueAnimationTypes(std::uint16_t authored,Tables& tables) {
    const bool dialogTable=authored>=10000u &&
        std::size_t(authored-10000u)<tables.table("dialoganimations").rowCount();
    const auto table=dialogTable?"dialoganimations":"animations";
    const auto row=dialogTable?std::uint32_t(authored-10000u):std::uint32_t(authored);
    const bool dialog=tables.flag(table,row,"dialog");
    DialogueAnimationTypes result{dialog && tables.flag(table,row,"fireforget"),
        dialog && tables.flag(table,row,"looping"),
        dialogTable && dialog && tables.flag(table,row,"overlay")};
    // The dialog-table branch has no numeric cut retry. Animations does,
    // including engine gaps1128..1199 and1528..1599 in these predicates.
    if(!dialogTable) {
        result.fireForget=result.fireForget || static_cast<std::uint16_t>(authored-1000u)<328u;
        result.looping=result.looping || static_cast<std::uint16_t>(authored-1400u)<328u;
    }
    return result;
}
std::string animationName(std::uint16_t id,const DialogueAnimationContext& c,Tables& tables,
                          model::GameVersion game) {
    auto name=tables.cell("animations",clientAnimation(id,c,game),"name");
    if(!name.empty())return name;
    const auto offset=static_cast<std::uint16_t>(id-1000u);
    if(offset<=727u && offset%200u<128u) {
        constexpr const char* suffix[]={"","w","l","wl"};
        char buffer[16];std::snprintf(buffer,sizeof(buffer),"cut%03u%s",offset%200u+1u,suffix[offset/200u]);
        return buffer;
    }
    // GetAnimationName retries server pause10000 after an empty NAME except
    // for10098. It does not choose any clip merely because it exists.
    if(id!=10098u)return tables.cell("animations",clientAnimation(10000u,c,game),"name");
    return {};
}
bool internalOverlay(std::uint16_t id,model::GameVersion game) {
    // K1 SetAnimationInternal 0x59bb0 predates the K2 10417 case.
    if(id==10417u)return game==model::GameVersion::Kotor2;
    switch(id) {
    case 10029:case 10030:case 10034:case 10038:case 10039:case 10040:
    case 10041:case 10042:case 10070:case 10071:case 10129:case 10130:
    case 10136:case 10154:case 10155:case 10302:return true;
    default:return false;
    }
}
std::uint32_t fireForgetMilliseconds(std::uint16_t id,float seconds,const DialogueAnimationContext& context,
    model::GameVersion game) {
    // GetAnimationLength: K1 0x5859e / K2 0x21f9a2, then the null-combat-data
    // AddFireForgetAnimation producer K1 0x80bd7 / K2 0x285428.
    if(context.family==DialogueAnimationFamily::Base)return 500u;
    if(context.family==DialogueAnimationFamily::Creature && id==6u)return 50u;
    const float milliseconds=seconds>0.0f?seconds*1000.0f:0.0f;
    const float rate=context.objectAnimationRate;
    const bool placeable=context.family==DialogueAnimationFamily::Placeable;
    const double value=placeable?double(milliseconds/rate):rate==0.0f?double(milliseconds):
        double(milliseconds)/double(std::fabs(rate));
    if(game==model::GameVersion::Kotor1) {
        // K1 SSE DWORD conversions saturate. Its nonzero-rate Desktop double
        // path preserves the unordered integer-indefinite result.
        if(std::isnan(value))return placeable || rate==0.0f?0u:0x80000000u;
        if(value>=4294967295.0)return 0xffffffffu;
        if(value<=0.0)return 0u;
        return static_cast<std::uint32_t>(value);
    }
    // K2 i386 fisttp to signed qword then low DWORD. Out-of-range or unordered
    // hardware conversion yields INT64_MIN, whose low DWORD is zero.
    if(!(value>=-9223372036854775808.0 && value<9223372036854775808.0))return 0u;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(value));
}
std::uint32_t overlayStopFlags(std::uint16_t id) {
    // SetOverlayAnimation flag table, K1 0x59a49.
    const bool force=id==10029u||id==10030u||id==10034u||
        (id>=10038u&&id<=10042u)||id==10070u||id==10071u||
        id==10136u||id==10145u||id==10154u||id==10155u;
    const bool loops=id==10030u||(id>=10038u&&id<=10042u)||id==10154u||id==10155u;
    return (force?model::kAnimationRunForceStop:model::kAnimationRunStop)|
        (loops?0u:model::kAnimationRunNonLooping);
}
bool reverseAnimation(std::uint16_t id) {
    switch(id) {
    case 10003:case 10081:case 10083:case 10212:case 10238:case 10256:
    case 10258:case 10271:return true;
    default:return false;
    }
}
}
DialogueAnimationContext dialogueAnimationContext(const CreatureAppearance& appearance,
    const CreatureAppearanceState& state,const ResourceResolver& resolver) {
    DialogueAnimationContext result;result.family=DialogueAnimationFamily::Creature;
    // CreateBodyAppearance/CreateSimple test GetAnimType2/3 and ModelType L.
    result.creatureType=(appearance.animationType==2u || appearance.animationType==3u) &&
        appearance.modelType.find('L')==std::string::npos?1u:2u;
    result.feat207.reset(); // visual state does not contain server creature feats
    const CreatureEquipmentItem* right=nullptr;const CreatureEquipmentItem* left=nullptr;
    for(const auto& item:state.equipment) {
        if(item.slot==0x10u)right=&item;
        else if(item.slot==0x20u)left=&item;
    }
    if(!right && !left) {result.weaponClass=8u;return result;}
    if(!right)return result;
    result.lightsaber=right->baseItem>=8 && right->baseItem<=10;
    Tables tables{resolver,{}};
    if(left) {
        const auto type=std::uint8_t(tables.numberValue("baseitems",std::uint32_t(left->baseItem),"weaponwield"));
        result.weaponClass=type==2u?4u:type==4u?6u:0u;
        result.lightsaber=result.lightsaber && left->baseItem>=8 && left->baseItem<=10;
    } else {
        const auto type=std::uint8_t(tables.numberValue("baseitems",std::uint32_t(right->baseItem),"weaponwield"));
        constexpr std::uint8_t classes[]={1,2,3,5,7,9};
        result.weaponClass=type>=1u && type<=6u?classes[type-1u]:0u;
    }
    return result;
}
DialogueAnimationCommand resolveDialogueAnimation(std::uint16_t authored,
    const ResourceResolver& resolver,const ModelDocument& document,const DialogueAnimationContext& context) {
    DialogueAnimationCommand command;command.serverAnimation=authored;command.family=context.family;
    const auto game=document.model().game;
    try {
        Tables tables{resolver,{}};
        const auto types=dialogueAnimationTypes(authored,tables);
        command.fireForget=types.fireForget;command.looping=types.looping;
        command.dialogOverlay=types.overlay;
        command.dialog=command.fireForget || command.looping;
        command.playOutOfPlace=static_cast<std::uint16_t>(authored-1200u)<128u ||
            static_cast<std::uint16_t>(authored-1600u)<128u;
        if(context.family==DialogueAnimationFamily::Placeable || context.family==DialogueAnimationFamily::Door) {
            const auto projection=detail::dialogueObjectAnimation(authored,
                context.family==DialogueAnimationFamily::Door,context.doorSelector);
            command.clientAnimation=projection.clientAnimation;
            command.runFlags=projection.flags;command.playEmpty=projection.playEmpty;
            if(!command.playEmpty) {
                command.name=detail::dialogueObjectAnimationName(authored,projection,
                    tables.cell("animations",command.clientAnimation,"name"));
                command.playEmpty=command.name.empty();
            }
            const auto animation=document.findAnimation(command.name);
            if(command.fireForget) {
                const float seconds=animation?document.modelAt(animation->modelIndex).animations[animation->animationIndex].length:0.0f;
                command.fireForgetMilliseconds=fireForgetMilliseconds(authored,seconds,context,game);
            }
            if(!command.playEmpty && !animation)
                command.diagnostic="Dialogue animation '"+command.name+"' is absent from the actor model hierarchy";
            return command;
        }
        command.clientAnimation=clientAnimation(authored,context,game);
        command.engineOverlay=tables.flag("animations",command.clientAnimation,"overlay");
        command.overlayStopFlags=overlayStopFlags(authored);
        command.name=animationName(authored,context,tables,game);
        if(command.fireForget) {
            const auto animation=document.findAnimation(command.name);
            const float seconds=animation?document.modelAt(animation->modelIndex).animations[animation->animationIndex].length:0.0f;
            command.fireForgetMilliseconds=fireForgetMilliseconds(authored,seconds,context,game);
        }
        command.runFlags=command.fireForget?model::kAnimationRunNonLooping:0u;
        if(command.playOutOfPlace)
            command.runFlags|=model::kAnimationRunImmediate;
        if(internalOverlay(authored,game) || (command.fireForget && command.engineOverlay)) {
            command.playbackRate=1.0f;command.runFlags=model::kAnimationRunOverlay;
            const bool loops=authored==10030u || (authored>=10038u && authored<=10042u) ||
                authored==10154u || authored==10155u;
            if(!loops)command.runFlags|=model::kAnimationRunNonLooping;
            if(authored==373u)command.playbackRate=-1.0f;
            // SetOverlayAnimation suppresses pause1 explicitly.
            if(command.name=="pause1")command.suppressed=true;
        } else if(reverseAnimation(authored))command.playbackRate=-1.0f;
        command.playEmpty=command.name.empty();
        if(!command.playEmpty && !command.suppressed && !document.findAnimation(command.name))
            command.diagnostic="Dialogue animation '"+command.name+"' is absent from the actor model hierarchy";
    } catch(const neoshared::bif::JobCancelled&) {throw;}
    catch(const std::exception& e) {command.diagnostic=e.what();}
    return command;
}
std::optional<float> dialogueCameraPullback(std::uint16_t serverAnimation,
    const ResourceResolver& resolver) {
    Tables tables{resolver,{}};
    const auto types=dialogueAnimationTypes(serverAnimation,tables);
    if(!types.fireForget && !types.looping)return std::nullopt;
    return tables.floating("dialoganimations",std::uint32_t(std::int32_t(serverAnimation)-10000),"CU_PB_Range");
}
bool dialogueAnimationIsPlaying(const DialogueAnimationCommand& command,
    bool currentDialog) noexcept {
    // K1 0x4c4f8 and K2 0x26531a cast each participant to CSWCCreature,
    // then ask AnimationDialog about the retained server animation ID.
    return command.family==DialogueAnimationFamily::Creature && currentDialog;
}
bool advanceDialogueAnimationCounter(std::uint32_t& remaining,std::uint32_t elapsed) noexcept {
    remaining-=elapsed;
    if((remaining&0x80000000u)==0u)return false;
    remaining=0u;
    return true;
}
} // namespace neomdl
