#include "core/ObjectVisualState.hpp"
#include "core/ModelDocument.hpp"
#include "core/Engine2DA.hpp"
#include "core/DialogueObjectAnimation.hpp"
#include "model/AnimationRuntime.hpp"

#include <twoda/TwoDAFile.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <set>
#include <sstream>
#include <tuple>

namespace neomdl {
namespace {
std::string lower(std::string value) {
    for(auto& c:value) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
bool finite(model::Vec3 v) noexcept {
    return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
}
bool finite(const model::Mat4& m) noexcept {
    return std::all_of(m.values.begin(),m.values.end(),[](float value){return std::isfinite(value);});
}
std::string resourceKeyLocal(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n\v\f");
    const auto last=value.find_last_not_of(" \t\r\n\v\f");
    if(first==std::string::npos)return {};
    value=value.substr(first,last-first+1u);
    for(auto& c:value)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
template<class T> T selected(const std::optional<T>& live,const std::optional<T>& authored,T fallback) {
    if (live) return *live;
    if (authored) return *authored;
    return fallback;
}
template<class T> std::optional<T> selectedOptional(const std::optional<T>& live,const std::optional<T>& authored) {
    return live ? live : authored;
}
std::string animationCell(const neo2da::TwoDAFile& table,std::uint16_t row) {
    std::string value;
    if(!engine2da::getString(table,row,"name",value))value.clear();
    return resourceKeyLocal(value);
}
bool sameTargetKey(const ObjectEmitterTargetRoute& a,const ObjectEmitterTargetRoute& b) {
    return lower(a.emitterName)==lower(b.emitterName) &&
        a.allSourceOwners==b.allSourceOwners && a.sourceReference==b.sourceReference;
}
}

const char* objectVisualStateSourceName(ObjectVisualStateSource source) noexcept {
    switch(source) {
    case ObjectVisualStateSource::AuthoredGit:return "authored GIT/template";
    case ObjectVisualStateSource::LiveAdapter:return "live adapter";
    case ObjectVisualStateSource::AnimationEvent:return "animation event";
    case ObjectVisualStateSource::CapturedTrace:return "captured trace";
    }
    return "unknown";
}

ObjectAnimationCommand doorOpenStateAnimation(std::uint8_t state,std::int32_t engineSelector) {
    ObjectAnimationCommand command;command.family=ObjectAnimationFamily::Door;
    command.doorAnimationSelector=engineSelector;
    command.serializedState=state;command.runFlags=model::kAnimationRunImmediate;
    switch(state) {
    case 0: command.serverAnimation=0x2726u;command.clientCandidates={0x014du};break;
    case 1: command.serverAnimation=0x2742u;command.clientCandidates={0x014bu};break;
    case 2: command.serverAnimation=0x2743u;command.clientCandidates={0x014cu};break;
    case 3:
        command.serverAnimation=0x2758u;



        command.clientCandidates={engineSelector==0?std::uint16_t{0x0147u}:std::uint16_t{0x014au}};
        break;
    default:
        command.diagnostic="Unsupported authored Door OpenState "+std::to_string(state);
        break;
    }
    return command;
}

ObjectAnimationCommand placeableAnimationState(std::uint8_t state) {
    ObjectAnimationCommand command;command.family=ObjectAnimationFamily::Placeable;
    command.serializedState=state;command.runFlags=0u;
    switch(state) {
    case 0: command.serverAnimation=0x2710u;command.clientCandidates={0x0130u};break;
    case 1: command.serverAnimation=0x275bu;command.clientCandidates={0x0136u};break;
    case 2: command.serverAnimation=0x275cu;command.clientCandidates={0x0137u};break;
    case 3: command.serverAnimation=0x2758u;command.clientCandidates={0x0133u};break;
    case 4: command.serverAnimation=0x2759u;command.clientCandidates={0x0134u};break;
    case 5: command.serverAnimation=0x275au;command.clientCandidates={0x0135u};break;
    default:
        // LoadPlaceable falls back to 0x2710 for values outside 1..5.
        command.serverAnimation=0x2710u;command.clientCandidates={0x0130u};
        command.diagnostic="LoadPlaceable maps out-of-range AnimationState "+
            std::to_string(state)+" to the default server animation 0x2710.";
        break;
    }
    return command;
}

ObjectAnimationCommand placeableServerAnimation(std::uint16_t serverAnimation) {
    // K1 Desktop LoadPlaceable 0x26f666 reads the saved INT Animation before
    // AnimationState. Its client map/flags (0x5e2ec/0x5e0d2) match K2.
    const auto projection=detail::dialogueObjectAnimation(serverAnimation,false,1);
    ObjectAnimationCommand command;command.family=ObjectAnimationFamily::Placeable;
    command.serverAnimation=serverAnimation;command.runFlags=projection.flags;
    command.enginePlaceablePolicy=true;command.playEmpty=projection.playEmpty;
    command.clientCandidates={projection.clientAnimation};
    return command;
}

ObjectAnimationCommand triggerServerAnimation(std::uint16_t serverAnimation) {
    // CSWCAnimBaseTrigger::ServerToClientAnimation K1 0x5e9ee /
    // K2 Desktop 0x141d7c. The ordinary SetAnimation caller supplies flags 0.
    ObjectAnimationCommand command;command.family=ObjectAnimationFamily::Trigger;
    command.serverAnimation=serverAnimation;
    command.clientCandidates={serverAnimation==10143u?std::uint16_t{347u}:
        serverAnimation==10144u?std::uint16_t{350u}:serverAnimation};
    return command;
}

bool resolveObjectAnimation(ObjectAnimationCommand& command,const ResourceResolver& resolver,
                            const ModelDocument& document,std::string& diagnostic) {
    diagnostic.clear();command.animationName.clear();command.clientAnimation.reset();
    command.candidateNames.clear();
    if(command.family==ObjectAnimationFamily::Trigger)command.playEmpty=false;
    if(command.enginePlaceablePolicy) {
        const auto projection=detail::dialogueObjectAnimation(command.serverAnimation,false,1);
        command.playEmpty=projection.playEmpty;
        if(command.playEmpty) {command.clientAnimation=projection.clientAnimation;return true;}
    }
    if(command.clientCandidates.empty()) {
        diagnostic=command.diagnostic.empty()?"Object animation has no engine client candidate":command.diagnostic;
        return false;
    }
    if(!resolver) {diagnostic="Object animation resolution requires the retained scoped resource resolver";return false;}
    const auto resource=resolver("animations",0x7e1u);
    if(!resource) {diagnostic="Scoped animations.2da is unavailable; numeric object state was retained";return false;}
    if(resource->bytes.size()>16u*1024u*1024u) {diagnostic="animations.2da exceeds the 16 MiB host limit";return false;}
    neo2da::TwoDAFile table;
    try {table.loadBytes(resource->bytes);} catch(const std::exception& e) {
        diagnostic="Cannot decode scoped animations.2da: "+std::string(e.what());return false;
    }
    std::vector<std::pair<std::uint16_t,std::string>> available;
    for(const auto client:command.clientCandidates) {
        auto name=animationCell(table,client);
        if(command.family==ObjectAnimationFamily::Trigger&&name.empty()) {
            command.playEmpty=true;command.clientAnimation=client;return true;
        }
        if(command.enginePlaceablePolicy) {
            const auto projection=detail::dialogueObjectAnimation(command.serverAnimation,false,1);
            name=detail::dialogueObjectAnimationName(command.serverAnimation,projection,name);
            if(name.empty()) {
                command.playEmpty=true;command.clientAnimation=client;return true;
            }
        }
        command.candidateNames.push_back(name);
        if(!name.empty()&&document.findAnimation(name))available.emplace_back(client,name);
    }
    if(available.size()!=1u) {
        std::ostringstream out;
        out<<(available.empty()?"No":"Multiple")<<" engine animation candidates resolve in model '"
           <<document.model().name<<"' for server animation 0x"<<std::hex<<command.serverAnimation;
        if(!command.diagnostic.empty())out<<"; "<<command.diagnostic;
        diagnostic=out.str();return false;
    }
    command.clientAnimation=available.front().first;command.animationName=available.front().second;
    if(!command.diagnostic.empty())diagnostic=command.diagnostic;
    return true;
}

OwnerVisualEffects EffectiveObjectVisualState::ownerEffects() const {
    OwnerVisualEffects result;
    result.distortion=distortion;result.hologram=hologram;
    result.colorShiftEnabled=colorShift.enabled;
    result.colorShift={{colorShift.color.x,colorShift.color.y,colorShift.color.z,colorShift.amount}};
    result.instanceOpacity=opacity;
    result.environmentMap=environmentMap;
    return result;
}

bool validObjectVisualStateLayer(const ObjectVisualStateLayer& layer,std::string& error) noexcept {
    error.clear();
    const auto fail=[&](const char* message){error=message;return false;};
    if(layer.colorShift) {
        const auto& shift=*layer.colorShift;
        if(!finite(shift.color)||!std::isfinite(shift.amount))return fail("Color-shift values must be finite");
        if(shift.color.x<0.0f||shift.color.x>1.0f||shift.color.y<0.0f||shift.color.y>1.0f||
           shift.color.z<0.0f||shift.color.z>1.0f||shift.amount<0.0f||shift.amount>1.0f)
            return fail("Color-shift values must be within [0,1]");
    }
    if(layer.opacity&&(!std::isfinite(*layer.opacity)||*layer.opacity<0.0f||*layer.opacity>1.0f))
        return fail("Object opacity must be finite and within [0,1]");
    if(layer.textureRotationDegrees&&!std::isfinite(*layer.textureRotationDegrees))
        return fail("Texture rotation must be finite");
    if(layer.environmentMap) {
        const auto key=resourceKeyLocal(*layer.environmentMap);
        if(!layer.environmentMap->empty()&&key.empty())return fail("Environment-map ResRef is invalid");
        if(key.size()>16u)return fail("Environment-map ResRef exceeds 16 characters");
    }
    if(layer.animation) {
        if(!std::isfinite(layer.animation->playbackRate)||layer.animation->playbackRate==0.0f||
           !std::isfinite(layer.animation->startTime))return fail("Object animation timing is invalid");
    }
    if(layer.emitterTargets) {
        std::vector<ObjectEmitterTargetRoute> seen;
        for(const auto& route:*layer.emitterTargets) {
            const bool owner=route.targetOwner.has_value(),world=route.explicitWorld.has_value();
            if(owner==world)return fail("Emitter target route must set exactly one target owner or explicit transform");
            if(world&&!finite(*route.explicitWorld))return fail("Emitter target transform contains non-finite values");
            if(route.emitterName.size()>64u||route.targetNode.size()>64u)
                return fail("Emitter target selector exceeds the retained name limit");
            if(!owner&&!route.targetNode.empty())return fail("A target node requires a target owner");
            if(route.allSourceOwners&&route.sourceReference)
                return fail("An all-owner target route cannot also select one source reference");
            if(!route.allSourceOwners&&!route.sourceReference)return fail("A reference-selective target route requires a source reference index");
            if(std::any_of(seen.begin(),seen.end(),[&](const auto& x){return sameTargetKey(x,route);}))
                return fail("Duplicate emitter target route selector");
            seen.push_back(route);
        }
    }
    return true;
}

void ObjectVisualStateAdapter::note(ObjectVisualStateSource source,std::string operation,
                                    std::string detail,bool applied) {
    trace_.push_back({nextSequence_++,source,std::move(operation),std::move(detail),applied});
}
bool ObjectVisualStateAdapter::setAuthored(const ObjectVisualStateLayer& layer,std::string& error) {
    if(!validObjectVisualStateLayer(layer,error)) {note(ObjectVisualStateSource::AuthoredGit,"set authored state",error,false);return false;}
    authored_=layer;note(ObjectVisualStateSource::AuthoredGit,"set authored state","validated authored object visual inputs",true);return true;
}
bool ObjectVisualStateAdapter::setLive(const ObjectVisualStateLayer& layer,std::string& error) {
    if(!validObjectVisualStateLayer(layer,error)) {note(ObjectVisualStateSource::LiveAdapter,"set live state",error,false);return false;}
    live_=layer;note(ObjectVisualStateSource::LiveAdapter,"set live state","validated live object visual inputs",true);return true;
}
void ObjectVisualStateAdapter::clearLive() {
    live_={};note(ObjectVisualStateSource::LiveAdapter,"clear live state","authored values are visible again",true);
}
EffectiveObjectVisualState ObjectVisualStateAdapter::effective() const {
    EffectiveObjectVisualState result;
    result.animation=selectedOptional(live_.animation,authored_.animation);
    result.colorShift=selected(live_.colorShift,authored_.colorShift,ObjectColorShift{});
    result.opacity=selected(live_.opacity,authored_.opacity,1.0f);
    result.environmentMap=selected(live_.environmentMap,authored_.environmentMap,std::string{});
    result.environmentMap=resourceKeyLocal(result.environmentMap);
    result.textureRotationDegrees=selected(live_.textureRotationDegrees,authored_.textureRotationDegrees,0.0f);
    result.motionBlur.enabled=selected(live_.motionBlur,authored_.motionBlur,false);
    result.motionBlur.maximumLength=selected(live_.maximumBlurLength,authored_.maximumBlurLength,1.5f);
    result.distortion=selected(live_.distortion,authored_.distortion,false);
    result.hologram=selected(live_.hologram,authored_.hologram,false);
    result.forceEmitterRadiusUnbounded=selected(live_.forceEmitterRadiusUnbounded,
        authored_.forceEmitterRadiusUnbounded,false);
    if(live_.emitterTargets)result.emitterTargets=*live_.emitterTargets;
    else if(authored_.emitterTargets)result.emitterTargets=*authored_.emitterTargets;
    return result;
}

const char* objectVisualEventName(ObjectVisualEventKind kind) noexcept {
    switch(kind) {
    case ObjectVisualEventKind::Animation:return "animation";
    case ObjectVisualEventKind::Detonate:return "detonate";
    case ObjectVisualEventKind::RotateTexture:return "rotate texture";
    case ObjectVisualEventKind::SetEmitterTarget:return "set emitter target";
    case ObjectVisualEventKind::ClearEmitterTargets:return "clear emitter targets";
    case ObjectVisualEventKind::SetMotionBlur:return "set motion blur";
    case ObjectVisualEventKind::SetDistortion:return "set distortion";
    case ObjectVisualEventKind::SetHologram:return "set hologram";
    case ObjectVisualEventKind::SetOpacity:return "set opacity";
    case ObjectVisualEventKind::SetEnvironmentMap:return "set environment map";
    case ObjectVisualEventKind::SetColorShift:return "set color shift";
    case ObjectVisualEventKind::SetForceEmitterRadiusUnbounded:return "set emitter-radius override";
    case ObjectVisualEventKind::ClearLiveOverrides:return "clear live overrides";
    }
    return "unknown";
}

} // namespace neomdl
