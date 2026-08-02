#include "core/MotionBlurScene.hpp"
#include <stdexcept>

namespace neomdl {
namespace {
void require(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(std::string("Owner motion blur: ")+message);
}
}
MotionBlurScene::State& MotionBlurScene::state(LightOwnerId id) {
    const auto i=states_.find(id);require(i!=states_.end(),"owner is not registered");return i->second;
}
const MotionBlurScene::State& MotionBlurScene::state(LightOwnerId id) const {
    const auto i=states_.find(id);require(i!=states_.end(),"owner is not registered");return i->second;
}
void MotionBlurScene::replaceOwners(std::vector<SceneOwnerView> owners) {
    std::map<LightOwnerId,State> next;
    for(const auto& owner:owners) {
        requireSceneOwnerLifetime(owner.ownerLifetime);
        require(owner.asset,"requires a live owner");
        require(!next.count(owner.id),"duplicate owner identity");
        const auto frame=owner.frame;
        State s;const auto old=states_.find(owner.id);
        const bool sameLifetime=old!=states_.end()&&
            old->second.ownerLifetime.has_value()==owner.ownerLifetime.has_value()&&
            (!owner.ownerLifetime||(!owner.ownerLifetime->owner_before(*old->second.ownerLifetime)&&
                                   !old->second.ownerLifetime->owner_before(*owner.ownerLifetime)));
        if(sameLifetime&&old->second.asset==owner.asset&&old->second.pose==owner.pose)s=old->second;
        else {s.asset=owner.asset;s.pose=owner.pose;
            s.previous=owner.resolvePreviousRenderedFrame
                ?owner.resolvePreviousRenderedFrame():model::EngineFrame{};}
        s.current=frame;s.ownerLifetime=owner.ownerLifetime;s.resolveFrame=owner.resolveFrame;s.commitRenderedFrame=owner.commitRenderedFrame;
        next.emplace(owner.id,s);
    }
    // SetScene(null) pools a chunk without resetting its Gob's previous frame
    // or blur settings. Keep that state outside the current admission view;
    // the weak marker neither retains the allocation nor re-admits the owner.
    for(const auto& entry:states_)
        if(!next.count(entry.first) && entry.second.ownerLifetime &&
           !entry.second.ownerLifetime->expired())next.emplace(entry);
    // Replacement is a frame boundary. Validate everything before altering
    // history; fresh lifetimes use the engine constructor identity frame.
    owners_=std::move(owners);states_=std::move(next);beginFrame();
}
void MotionBlurScene::setSettings(LightOwnerId owner,MotionBlurOwnerSettings cfg) {



    state(owner).settings=cfg;
}
MotionBlurOwnerSettings MotionBlurScene::settings(LightOwnerId owner) const {return state(owner).settings;}
void MotionBlurScene::beginFrame() noexcept {pending_.clear();completed_=0;}
bool MotionBlurScene::ordinaryRendered(LightOwnerId owner) {
    auto& s=state(owner);
    requireSceneOwnerLifetime(s.ownerLifetime);
    if(s.resolveFrame)s.current=s.resolveFrame();
    if(s.settings.enabled) {pending_.push_back(owner);return true;}
    s.previous=s.current;
    if(s.commitRenderedFrame)s.commitRenderedFrame(s.current);
    return false;
}
MotionBlurDraw MotionBlurScene::nextDraw() const {
    require(completed_<pending_.size(),"no pending owner");
    const auto id=pending_[completed_];const auto& s=state(id);
    requireSceneOwnerLifetime(s.ownerLifetime);
    const auto current=s.resolveFrame?s.resolveFrame():s.current;
    MotionBlurDraw result;result.owner=id;result.ordinal=completed_;
    result.current=current;result.previous=s.previous;result.maximumLength=s.settings.maximumLength;
    result.worldMotion={current.position.x-s.previous.position.x,current.position.y-s.previous.position.y,
                        current.position.z-s.previous.position.z};
    return result;
}
void MotionBlurScene::completeBlur(const MotionBlurDraw& draw) {
    require(completed_<pending_.size()&&draw.ordinal==completed_&&pending_[completed_]==draw.owner,
            "blur completion is out of append order");
    auto& s=state(draw.owner);requireSceneOwnerLifetime(s.ownerLifetime);
    s.current=draw.current;s.previous=draw.current;
    if(s.commitRenderedFrame)s.commitRenderedFrame(draw.current);
    ++completed_;
}
void MotionBlurScene::resetHistory() {
    for(auto& entry:states_) {
        auto& s=entry.second;requireSceneOwnerLifetime(s.ownerLifetime);
        if(s.resolveFrame)s.current=s.resolveFrame();
        s.previous=s.current;
        if(s.commitRenderedFrame)s.commitRenderedFrame(s.current);
    }
    beginFrame();
}
void MotionBlurScene::clear() noexcept {owners_.clear();states_.clear();beginFrame();}
} // namespace neomdl
