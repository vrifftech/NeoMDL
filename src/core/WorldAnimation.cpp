#include "core/WorldAnimation.hpp"

#include <cctype>
#include <cmath>
#include <cstring>

namespace neomdl {
namespace {
std::uint32_t synchronizedTrackFlags(const TrackAnimationState& state) {
    return state.mode==1?0x11u:0x14u;
}
bool startsNoCase(const std::string& value,const char* prefix) {
    const auto length=std::strlen(prefix);
    if(value.size()<length)return false;
    for(std::size_t i=0;i<length;++i)
        if(std::tolower(static_cast<unsigned char>(value[i]))!=prefix[i])return false;
    return true;
}
}

float worldAnimationCallbackDistance(model::Vec3 owner,const WorldAnimationFrameInput& input,
    model::GameVersion game) noexcept {
    if(input.disableGobDistance)return 0.0f;
    float x=owner.x-input.camera.x,y=owner.y-input.camera.y;
    if(0.0f>x)x=-x;
    if(0.0f>y)y=-y;
    // Gob::Animate uses XY Chebyshev distance. The Desktop compiler operand
    // order differs: preserve MAXSS's second-operand result on unordered.
    return game==model::GameVersion::Kotor1?(x>y?x:y):(y>x?y:x);
}

void startTrackAnimation(std::size_t track,TrackAnimationState& state,const WorldAnimationDispatch& play,
    const std::function<void(std::size_t)>& registerEvent) {
    // K1 0x17cf1e / K2 Desktop 0x256eac. The running bit is set even when
    // a Gob rejects a missing clip; it is not derived from render-run success.
    if(state.running)return;
    state.eventsRegistered=true;
    const bool looping=state.mode==1;
    const auto modelCount=state.models.size();
    for(std::size_t index=0;index<modelCount;++index) {
        const auto owner=state.models.at(index);
        registerEvent(owner);play(owner,"Ready_01",1.0f,0u);
    }
    const auto flags=(looping?5u:4u)|(state.kind==TrackFollowerKind::Player?0x100u:0u);
    registerEvent(track);
    play(track,"track",state.speed/100.0f,flags);
    state.running=true;
}
void stopTrackAnimation(std::size_t track,TrackAnimationState& state,const WorldAnimationDispatch& play) {
    // Engine Stop changes track rate to zero with time synchronization. It
    // neither destroys the run nor stops the independent follower-model runs.
    if(!state.running)return;
    play(track,"track",0.0f,synchronizedTrackFlags(state));
    state.running=false;
}
void setTrackAnimationSpeed(std::size_t track,TrackAnimationState& state,float speed,
    const WorldAnimationDispatch& play) {
    state.speed=speed;
    if(state.running)play(track,"track",speed/100.0f,synchronizedTrackFlags(state));
}
void playTrackFollowerAnimation(const TrackAnimationState& state,const std::string& name,float rate,
    std::uint32_t flags,const WorldAnimationDispatch& play) {
    // This caller animates the follower's ordered models, not the track Gob.
    for(const auto owner:state.models)play(owner,name,rate,flags);
    if(state.kind==TrackFollowerKind::Player&&state.playerModel)play(*state.playerModel,name,rate,flags);
}
void playTrackScriptAnimation(const TrackAnimationState& state,model::GameVersion game,
    const std::string& name,int looping,int queued,int overlay,const WorldAnimationDispatch& play) {
    // K1 0x2d598a / K2 Desktop 0x3585ee. K2 adds an empty-string clear
    // request; K1 forwards its empty C string with the ordinary flags.
    if(game==model::GameVersion::Kotor2&&name.empty()) {
        playTrackFollowerAnimation(state,{},0.0f,model::kAnimationRunImmediate,play);
        return;
    }
    const auto flags=(looping==0?model::kAnimationRunNonLooping:0u)|
        (queued!=0?model::kAnimationRunQueued:0u)|(overlay!=0?model::kAnimationRunOverlay:0u);
    playTrackFollowerAnimation(state,name,1.0f,flags,play);
}
void removeTrackScriptAnimation(const TrackAnimationState& state,const std::string& name,
    const WorldAnimationDispatch& play) {
    playTrackFollowerAnimation(state,name,1.0f,model::kAnimationRunStop,play);
}
std::optional<WorldAnimationEvent> trackAnimationEvent(std::size_t track,TrackAnimationState& state,
    std::size_t source,const std::string& event,const WorldAnimationDispatch& play) {
    // K1 0x17d19c / K2 Desktop 0x2559ec: case-insensitive prefix tests,
    // including custom_ with an empty suffix and loop-prefixed authored keys.
    if(startsNoCase(event,"custom_")) {
        state.lastCustomSource=source;state.lastCustomEvent=event.substr(7);
        return WorldAnimationEvent{track,source,3u,state.lastCustomEvent};
    }
    if(startsNoCase(event,"loop")&&state.kind==TrackFollowerKind::Enemy) {
        if(state.mode!=-1) {
            // CSWMiniEnemy::SetLooping accepts only +1 and -1. Preserve
            // its rejection of other decremented values, including zero.
            const auto next=static_cast<std::uint32_t>(state.mode)-1u;
            if(next==1u)state.mode=1;
            else if(next==0xffffffffu)state.mode=-1;
            setTrackAnimationSpeed(track,state,state.speed,play);
        }
        return WorldAnimationEvent{track,source,9u,{}};
    }
    return std::nullopt;
}

} // namespace neomdl
