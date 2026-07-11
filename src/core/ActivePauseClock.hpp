#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace neomdl {

struct EngineWorldTime {
    std::uint32_t calendarDay{},timeOfDayMilliseconds{};
};

// The DWORD millisecond output of SubtractWorldTimes for forward world epochs.
// Module/area and creature callers intentionally discard its whole-day output.
std::uint32_t engineWorldTimeRemainderMilliseconds(EngineWorldTime current,
    EngineWorldTime previous,std::uint32_t dayMilliseconds) noexcept;

// Client CWorldTimer/GetActiveTimer and CSWCCreature::AIUpdate clocks shared by
// the engine clients. Owner IDs identify registered scene creatures;
// exclusion membership is captured from SetPauseState, never inferred from the
// player, an animation flag, or the current dialogue participants.
class EngineActivePauseClock {
public:
    EngineActivePauseClock();
    void reset(std::uint32_t day,std::uint32_t timeOfDay,
               std::uint32_t dayMilliseconds,std::uint64_t snapshotMicroseconds=0);
    void registerCreature(std::uint64_t owner,std::function<void(bool)> forceAnimations);
    void unregisterCreature(std::uint64_t owner);
    bool containsCreature(std::uint64_t owner) const noexcept;

    // The engine operation takes a mask and an int, not a desired final state.
    // Its entry comparison is (oldMask & mask)==enable, including mask 2.
    // Returns whether that comparison admitted the operation and its list.
    bool setPauseState(std::uint8_t mask,int enable,
                       const std::vector<std::uint64_t>& exclusions);
    // The host supplies the elapsed high-resolution snapshot interval. Explicit
    // inspector pause supplies zero; engine pause still advances this clock.
    // Each registered creature samples its active timer once per frame, even
    // when it has no fire-and-forget animation or dialogue running.
    void advance(std::uint64_t elapsedMicroseconds);

    std::uint32_t creatureDeltaMilliseconds(std::uint64_t owner) const;
    std::uint32_t worldDeltaMilliseconds() const noexcept { return worldDelta_; }
    EngineWorldTime worldTime() const noexcept;
    std::uint32_t dayMilliseconds() const noexcept {return dayMilliseconds_;}
    bool worldPaused() const noexcept { return timers_[0].paused; }
    std::uint8_t pauseMask() const noexcept { return pauseMask_; }
    std::uint8_t activePauseState() const noexcept;
    const std::vector<std::uint64_t>& exclusions() const noexcept { return exclusions_; }

private:
    struct Timer {
        std::uint64_t snapshot{},previousSnapshot{},lastSample{};
        EngineWorldTime frozen{},offset{};
        bool paused{};
    };
    struct Creature {
        EngineWorldTime previous{};
        std::uint32_t delta{};
        std::function<void(bool)> forceAnimations;
    };
    void snapshot(Timer& timer) noexcept;
    EngineWorldTime time(const Timer& timer) const noexcept;
    void setTime(Timer& timer,EngineWorldTime value) noexcept;
    void pause(Timer& timer) noexcept;
    void unpause(Timer& timer) noexcept;
    static void resetTimer(Timer& timer,const Timer& source) noexcept;
    const Timer& activeTimer(std::uint64_t owner) const noexcept;
    void clearExclusions();
    void addExclusion(std::uint64_t owner);
    std::uint32_t difference(EngineWorldTime current,EngineWorldTime previous) const noexcept;

    // The fourth timer participates in SetPauseState's reset/unpause sequence;
    // GetActiveTimer selects only the first three.
    std::array<Timer,4> timers_{};
    std::map<std::uint64_t,Creature> creatures_;
    std::vector<std::uint64_t> exclusions_;
    std::uint64_t hostSnapshot_{};
    std::uint32_t dayMilliseconds_{7200000u},worldDelta_{};
    std::uint8_t pauseMask_{};
};

} // namespace neomdl
