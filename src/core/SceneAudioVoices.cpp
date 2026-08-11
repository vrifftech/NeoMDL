#include "core/SceneAudioVoices.hpp"
#include <algorithm>

namespace neomdl {
SceneAudioVoiceLimits engineSceneAudioVoiceLimits(std::uint8_t twoD,std::uint8_t threeD) noexcept {
    const auto ordinary=std::uint8_t(twoD/2u);
    return {ordinary,threeD,std::uint8_t(twoD-ordinary)};
}
SceneAudioVoiceLimits engineSceneAudioDefaultVoiceLimits(model::GameVersion game) noexcept {
    // InitializeSoundOptions: K1 Desktop 20aff/20bde; K2 Desktop 198069/1980fc.
    return game==model::GameVersion::Kotor1?engineSceneAudioVoiceLimits(24,16):
        engineSceneAudioVoiceLimits(64,64);
}
void SceneAudioVoices::reset(std::vector<SceneAudioPriority> priorities,SceneAudioVoiceLimits limits,
        std::function<std::uint32_t()> random,model::GameVersion game) {
    priorities_=std::move(priorities);groupCounts_.assign(priorities_.size(),0);
    slots2D_.assign(limits.ordinary2D,{});slots3D_.assign(limits.ordinary3D,{});
    streamLimit_=limits.streams;random_=std::move(random);game_=game;
    voices_.clear();playing_.clear();waiting_.clear();streams_.clear();waitingStreams_.clear();disposal_.clear();
    nextStreamProvider_=std::uint64_t{1}<<32;
}
bool SceneAudioVoices::inRange(const SceneAudioPlay& p,model::Vec3 listener) const noexcept {
    return !p.positional||engineAudioWithinRange(p.position,listener,p.maximumDistance);
}
bool SceneAudioVoices::playing(std::uint64_t id,const SceneAudioVoiceBackend& b) const {
    const auto it=voices_.find(id);
    return it!=voices_.end()&&it->second.provider&&b.playing(it->second.provider);
}
void SceneAudioVoices::setReserved(bool positional,std::size_t slot,bool value) {
    (positional?slots3D_:slots2D_).at(slot).reserved=value;
}
void SceneAudioVoices::removePlaying(std::uint64_t id) {
    auto& v=voices_.at(id);if(!v.playingList)return;
    (v.request.streaming?streams_:playing_).remove(id);
    --groupCounts_.at(v.request.priorityGroup);v.playingList=false;
}
void SceneAudioVoices::removeWaiting(std::uint64_t id) {
    auto& v=voices_.at(id);if(!v.waitingList)return;
    (v.request.streaming?waitingStreams_:waiting_).remove(id);v.waitingList=false;
}
void SceneAudioVoices::wait(std::uint64_t id) {
    auto& v=voices_.at(id);if(v.waitingList)return;
    (v.request.streaming?waitingStreams_:waiting_).push_front(id);v.waitingList=true;
}
void SceneAudioVoices::shutdown(std::uint64_t id,const SceneAudioVoiceBackend& b) {
    auto& v=voices_.at(id);removePlaying(id);removeWaiting(id);
    if(v.request.streaming) {
        if(v.provider)b.pauseStream(v.provider);
        return;
    }
    if(v.provider)b.stop(v.provider);
    if(v.slot) {
        // A disposed one-shot retains the engine slot pointer after a steal.
        // Its shutdown therefore clears that same slot, even after reuse.
        (v.request.positional?slots3D_:slots2D_).at(*v.slot).owner.reset();
        v.slot.reset();
    }
    v.provider=0;
}
void SceneAudioVoices::closeStream(std::uint64_t id,const SceneAudioVoiceBackend& b) {
    shutdown(id,b);auto& v=voices_.at(id);
    if(v.provider)b.stop(v.provider);
    v.provider=0;
}
void SceneAudioVoices::destroy(std::uint64_t id,const SceneAudioVoiceBackend& b) {
    const auto it=voices_.find(id);if(it==voices_.end())return;
    if(it->second.request.streaming)closeStream(id,b);else shutdown(id,b);
    disposal_.remove(id);voices_.erase(id);
}
void SceneAudioVoices::stop(std::uint64_t id,const SceneAudioVoiceBackend& b) {
    const auto it=voices_.find(id);if(it==voices_.end())return;
    if(it->second.request.streaming)shutdown(id,b);
    else destroy(id,b);
}
void SceneAudioVoices::clear(const SceneAudioVoiceBackend& b) {
    while(!voices_.empty())destroy(voices_.begin()->first,b);
}
void SceneAudioVoices::dispose(std::uint64_t id,const SceneAudioVoiceBackend& b) {
    auto& v=voices_.at(id);
    if(playing(id,b)) {
        if(!v.disposal){v.disposal=true;disposal_.push_front(id);}
    } else if(!v.disposal)stop(id,b);
}
void SceneAudioVoices::interrupt(std::uint64_t id,const SceneAudioVoiceBackend& b) {
    const auto request=voices_.at(id).request;
    if(!request.streaming&&request.oneShot) {dispose(id,b);return;}
    shutdown(id,b);
    if(request.looping)wait(id);
}
std::optional<std::size_t> SceneAudioVoices::allocate(const SceneAudioPlay& p,const SceneAudioVoiceBackend& b) {
    auto& slots=p.positional?slots3D_:slots2D_;
    const auto priority=priorities_.at(p.priorityGroup).priority;
    std::optional<std::size_t> candidate;std::uint8_t worst{};
    for(std::size_t i=0;i<slots.size();++i) {
        const auto& slot=slots[i];if(slot.reserved)continue;
        if(!slot.owner)return i;
        const auto group=voices_.at(*slot.owner).request.priorityGroup;
        const auto other=priorities_.at(group).priority;
        if(other>priority&&other>worst){candidate=i;worst=other;}
    }
    if(candidate)interrupt(*slots[*candidate].owner,b);
    return candidate;
}
bool SceneAudioVoices::admitOrdinary(std::uint64_t id,bool retry,const SceneAudioVoiceBackend& b,model::Vec3 listener) {
    auto& v=voices_.at(id);const auto p=v.request;
    // CalculatePitchVarianceFrequency precedes range and every failed retry.
    v.frequency=engineSceneAudioFrequency(p,priorities_.at(p.priorityGroup),v.resource.sampleRate,random_);
    if(inRange(p,listener)) {
        const auto& group=priorities_.at(p.priorityGroup);
        bool allowed=true;
        if(groupCounts_.at(p.priorityGroup)>=group.maxPlaying) {
            if(retry||!group.interrupt)allowed=false;
            else for(const auto other:playing_)
                if(voices_.at(other).request.priorityGroup==p.priorityGroup){interrupt(other,b);break;}
        }
        if(allowed)if(const auto slot=allocate(p,b)) {
            auto& target=voices_.at(id);target.slot=slot;
            target.provider=1u+*slot+(p.positional?std::uint64_t{65536}:0u);
            (p.positional?slots3D_:slots2D_)[*slot].owner=id;
            b.start(target.provider,p,target.frequency);
            removeWaiting(id);playing_.push_front(id);target.playingList=true;
            ++groupCounts_.at(p.priorityGroup);return true;
        }
    }
    if(p.looping)wait(id);
    return false;
}
bool SceneAudioVoices::admitStream(std::uint64_t id,bool retry,const SceneAudioVoiceBackend& b,model::Vec3 listener) {
    auto& v=voices_.at(id);const auto p=v.request;const auto& group=priorities_.at(p.priorityGroup);
    // Play on an initialized source pauses it and removes prior membership.
    // InitializeSource and mix/loop setters run before admission, even when
    // the request will wait or return a zero duration.
    const bool retained=v.provider!=0;
    if(retained)shutdown(id,b);
    if(!v.provider)v.provider=nextStreamProvider_++;
    b.prepareStream(v.provider,p,retained);
    if(groupCounts_.at(p.priorityGroup)>=group.maxPlaying) {
        if(retry||!group.interrupt){if(p.looping)wait(id);return false;}
        for(const auto other:streams_)
            if(voices_.at(other).request.priorityGroup==p.priorityGroup){interrupt(other,b);break;}
    }
    if(p.looping&&!inRange(p,listener)) {
        if(game_==model::GameVersion::Kotor2)closeStream(id,b);
        wait(id);return false;
    }
    if(streams_.size()>=streamLimit_) {
        // AddToStreamingList considers looping victims only, in AddHead order.
        std::optional<std::uint64_t> candidate;std::uint8_t worst{};
        for(const auto other:streams_) {
            const auto& request=voices_.at(other).request;
            const auto priority=priorities_.at(request.priorityGroup).priority;
            if(request.looping&&priority>group.priority&&priority>worst){candidate=other;worst=priority;}
        }
        if(!candidate)return false;
        interrupt(*candidate,b);
    }
    removeWaiting(id);streams_.push_front(id);v.playingList=true;
    ++groupCounts_.at(p.priorityGroup);b.resumeStream(v.provider);return true;
}
std::uint32_t SceneAudioVoices::play(SceneAudioPlay p,const SceneAudioVoiceBackend& b,model::Vec3 listener) {
    p.resref=engineSceneAudioResRef(p.resref);
    const auto previous=voices_.find(p.source);
    if(previous!=voices_.end()&&!p.streaming&&previous->second.slot&&
       previous->second.request.resref==p.resref) {
        // Engine Play's already-bound hardware branch restarts the same voice
        // at zero without a new frequency draw, allocation or list insertion.
        auto& v=previous->second;v.request=std::move(p);
        b.restart(v.provider,v.request.looping);return v.resource.durationMilliseconds;
    }
    if(previous!=voices_.end()&&p.streaming&&previous->second.request.streaming&&
       previous->second.request.resref==p.resref) {
        const auto id=p.source;previous->second.request=std::move(p);
        const bool admitted=admitStream(id,false,b,listener);
        return admitted||voices_.at(id).waitingList?previous->second.resource.durationMilliseconds:0u;
    }
    destroy(p.source,b);
    const auto resource=b.resource(p);if(!resource)return 0;
    const auto id=p.source;Voice v;v.request=std::move(p);v.resource=*resource;
    voices_.emplace(id,std::move(v));
    const bool streamed=voices_.at(id).request.streaming;
    const bool admitted=streamed?admitStream(id,false,b,listener):admitOrdinary(id,false,b,listener);
    const auto duration=resource->durationMilliseconds;
    // Stream Play returns zero on a full physical list (and a rejected
    // nonlooping group); waiting loops retain their engine stream duration.
    const bool waiting=voices_.at(id).waitingList;
    if(!admitted&&!waiting) {
        if(!streamed)destroy(id,b);
        return streamed?0u:duration;
    }
    return duration;
}
void SceneAudioVoices::update(const SceneAudioVoiceBackend& b,model::Vec3 listener) {
    // Engine order: completed streams, active streams, waiting streams,
    // ordinary playing list, ordinary waiting list, disposable one-shots.
    for(auto it=streams_.begin();it!=streams_.end();) {
        const auto id=*it++;
        if(!playing(id,b)&&(game_==model::GameVersion::Kotor1||!voices_.at(id).request.looping))
            removePlaying(id);
    }
    for(auto it=streams_.begin();it!=streams_.end();) {
        const auto id=*it++;const auto p=voices_.at(id).request;
        if(p.looping&&!inRange(p,listener)) {
            if(game_==model::GameVersion::Kotor2)closeStream(id,b);else shutdown(id,b);
            wait(id);
        }
        else b.updateStream(voices_.at(id).provider,p);
    }
    for(auto it=waitingStreams_.begin();it!=waitingStreams_.end();) {
        const auto id=*it++;const auto& v=voices_.at(id);
        if(streams_.size()<streamLimit_&&groupCounts_.at(v.request.priorityGroup)<priorities_.at(v.request.priorityGroup).maxPlaying&&
           inRange(v.request,listener)) {
            if(v.provider)b.updateStream(v.provider,v.request);
            admitStream(id,true,b,listener);
        }
    }
    for(auto it=playing_.begin();it!=playing_.end();) {
        const auto id=*it++;const auto p=voices_.at(id).request;
        if(!playing(id,b)) {
            removePlaying(id);
            if(p.oneShot)dispose(id,b);else shutdown(id,b);
        } else if(!inRange(p,listener)) {
            removePlaying(id);
            if(p.oneShot)dispose(id,b);
            else {shutdown(id,b);if(p.looping)wait(id);}
        }
    }
    for(auto it=waiting_.begin();it!=waiting_.end();) {
        const auto id=*it++;admitOrdinary(id,true,b,listener);
    }
    for(auto it=disposal_.begin();it!=disposal_.end();) {
        const auto id=*it++;if(!playing(id,b))stop(id,b);
    }
}
} // namespace neomdl
