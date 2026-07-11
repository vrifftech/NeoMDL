#include "core/ActivePauseClock.hpp"

#include <algorithm>
#include <utility>

namespace neomdl {

EngineActivePauseClock::EngineActivePauseClock() {
    // CWorldTimer's 1340/6/1 23:59:56 constructor epoch and five minutes/hour.
    reset(450380u,10496000u,7200000u);
}

void EngineActivePauseClock::reset(std::uint32_t day,std::uint32_t timeOfDay,
                                  std::uint32_t dayMilliseconds,
                                  std::uint64_t snapshotMicroseconds) {
    clearExclusions();
    dayMilliseconds_=dayMilliseconds;
    hostSnapshot_=snapshotMicroseconds;
    pauseMask_=0;worldDelta_=0;
    creatures_.clear();exclusions_.clear();
    for(auto& timer:timers_) {
        timer={};
        timer.snapshot=timer.previousSnapshot=timer.lastSample=hostSnapshot_;
        setTime(timer,{day,timeOfDay});
    }
}

void EngineActivePauseClock::snapshot(Timer& timer) noexcept {
    // SetSnapshotTime at the client-owned default snapshot scale (100).
    timer.previousSnapshot=timer.snapshot;
    timer.snapshot+=(hostSnapshot_-timer.lastSample)*100u/100u;
    timer.lastSample=hostSnapshot_;
}

EngineWorldTime EngineActivePauseClock::time(const Timer& timer) const noexcept {
    EngineWorldTime result;
    if(timer.paused) result=timer.frozen;
    else {
        const auto milliseconds=timer.snapshot/1000u;
        result.calendarDay=static_cast<std::uint32_t>(milliseconds/dayMilliseconds_)+
                           timer.offset.calendarDay;
        result.timeOfDayMilliseconds=static_cast<std::uint32_t>(milliseconds%dayMilliseconds_)+
                                    timer.offset.timeOfDayMilliseconds;
    }
    // GetWorldTime preserves unsigned offset arithmetic, then normalizes its
    // negative-offset and day-overflow cases in this order.
    while(result.timeOfDayMilliseconds>~dayMilliseconds_) {
        --result.calendarDay;
        result.timeOfDayMilliseconds+=dayMilliseconds_;
    }
    while(result.timeOfDayMilliseconds>=dayMilliseconds_) {
        ++result.calendarDay;
        result.timeOfDayMilliseconds-=dayMilliseconds_;
    }
    return result;
}

void EngineActivePauseClock::setTime(Timer& timer,EngineWorldTime value) noexcept {
    const auto milliseconds=timer.snapshot/1000u;
    timer.offset.calendarDay=value.calendarDay-
        static_cast<std::uint32_t>(milliseconds/dayMilliseconds_);
    timer.offset.timeOfDayMilliseconds=value.timeOfDayMilliseconds-
        static_cast<std::uint32_t>(milliseconds%dayMilliseconds_);
}

void EngineActivePauseClock::pause(Timer& timer) noexcept {
    if(!timer.paused) {timer.frozen=time(timer);timer.paused=true;}
}

void EngineActivePauseClock::unpause(Timer& timer) noexcept {
    if(timer.paused) {setTime(timer,timer.frozen);timer.paused=false;}
}

void EngineActivePauseClock::resetTimer(Timer& timer,const Timer& source) noexcept {
    // ResetTimer copies both snapshots and the world epoch/frozen state, but
    // retains the destination's last high-resolution sampling timestamp.
    const auto lastSample=timer.lastSample;
    timer=source;
    timer.lastSample=lastSample;
}

std::uint8_t EngineActivePauseClock::activePauseState() const noexcept {
    return (pauseMask_&2u)?2u:pauseMask_&1u;
}

const EngineActivePauseClock::Timer&
EngineActivePauseClock::activeTimer(std::uint64_t owner) const noexcept {
    const auto state=activePauseState();
    if(state&&std::find(exclusions_.begin(),exclusions_.end(),owner)!=exclusions_.end())
        return timers_[state];
    return timers_[0];
}

EngineWorldTime EngineActivePauseClock::worldTime() const noexcept {return time(timers_[0]);}

void EngineActivePauseClock::registerCreature(std::uint64_t owner,
                                            std::function<void(bool)> forceAnimations) {
    creatures_.try_emplace(owner,Creature{time(activeTimer(owner)),0u,std::move(forceAnimations)});
}

void EngineActivePauseClock::unregisterCreature(std::uint64_t owner) {
    creatures_.erase(owner);
}

bool EngineActivePauseClock::containsCreature(std::uint64_t owner) const noexcept {
    return creatures_.find(owner)!=creatures_.end();
}

void EngineActivePauseClock::clearExclusions() {
    // ClearPauseExclusionList removes each head before selecting that owner's
    // replacement timer and rebasing its last-AI stamp. Duplicates retain the
    // engine ordered-list semantics.
    while(!exclusions_.empty()) {
        const auto owner=exclusions_.front();
        exclusions_.erase(exclusions_.begin());
        const auto creature=creatures_.find(owner);
        if(creature!=creatures_.end()) {
            creature->second.previous=time(activeTimer(owner));
            creature->second.forceAnimations(false);
        }
    }
}

void EngineActivePauseClock::addExclusion(std::uint64_t owner) {
    // The engine object-array lookup must succeed before insertion/rebasing.
    const auto creature=creatures_.find(owner);
    if(creature==creatures_.end())return;
    creature->second.forceAnimations(true);
    exclusions_.push_back(owner);
    creature->second.previous=time(activeTimer(owner));
}

bool EngineActivePauseClock::setPauseState(std::uint8_t mask,int enable,
                                         const std::vector<std::uint64_t>& exclusions) {
    if((pauseMask_&mask)==enable)return false;
    auto& world=timers_[0];auto& first=timers_[1];
    auto& second=timers_[2];auto& third=timers_[3];
    if(enable==1) {
        pauseMask_|=mask;
        pause(world);snapshot(world);snapshot(world);
        if(activePauseState()==2u) {pause(first);snapshot(first);snapshot(first);}
    } else {
        pauseMask_&=static_cast<std::uint8_t>(~mask);
        if(mask==2u) {
            pause(second);pause(third);
            resetTimer(second,first);resetTimer(third,first);
            snapshot(second);snapshot(third);snapshot(first);
            second.previousSnapshot=second.snapshot;
            third.previousSnapshot=third.snapshot;
            first.previousSnapshot=first.snapshot;
            if(activePauseState()!=1u) {
                snapshot(world);world.previousSnapshot=world.snapshot;unpause(world);
            }
            unpause(first);unpause(second);unpause(third);
        } else if(mask==1u) {
            pause(first);pause(second);pause(third);
            resetTimer(first,world);resetTimer(second,world);pause(third);
            snapshot(world);snapshot(first);snapshot(second);snapshot(third);
            second.previousSnapshot=second.snapshot;
            third.previousSnapshot=third.snapshot;
            first.previousSnapshot=first.snapshot;
            world.previousSnapshot=world.snapshot;
            unpause(world);unpause(first);unpause(second);unpause(third);
        }
    }
    // The input is a captured list, distinct from the currently owned list.
    // Callers may pass exclusions() when replaying another engine transition.
    const auto replacement=exclusions;
    clearExclusions();
    for(const auto owner:replacement)addExclusion(owner);
    return true;
}

std::uint32_t engineWorldTimeRemainderMilliseconds(EngineWorldTime current,
        EngineWorldTime previous,std::uint32_t dayMilliseconds) noexcept {
    auto milliseconds=current.timeOfDayMilliseconds-previous.timeOfDayMilliseconds;
    if(milliseconds>=dayMilliseconds)milliseconds+=dayMilliseconds;
    return milliseconds;
}

std::uint32_t EngineActivePauseClock::difference(EngineWorldTime current,
                                                EngineWorldTime previous) const noexcept {
    // AIUpdate consumes only SubtractWorldTimes' DWORD millisecond output, not
    // the returned whole-day difference. Its callers rebase on timer switches.
    return engineWorldTimeRemainderMilliseconds(current,previous,dayMilliseconds_);
}

void EngineActivePauseClock::advance(std::uint64_t elapsedMicroseconds) {
    const auto previousWorld=worldTime();
    hostSnapshot_+=elapsedMicroseconds;
    // Client MainLoop snapshot admission is separate from PauseWorldTimer's
    // frozen flag. In particular, do not collapse this into worldPaused().
    if(!(pauseMask_&3u))snapshot(timers_[0]);
    if(!(pauseMask_&2u))snapshot(timers_[1]);
    snapshot(timers_[2]);
    worldDelta_=difference(worldTime(),previousWorld);
    for(auto& [owner,creature]:creatures_) {
        const auto current=time(activeTimer(owner));
        creature.delta=difference(current,creature.previous);
        creature.previous=current;
    }
}

std::uint32_t EngineActivePauseClock::creatureDeltaMilliseconds(std::uint64_t owner) const {
    return creatures_.at(owner).delta;
}

} // namespace neomdl
