#pragma once

#include "model/AnimationRuntime.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

enum class TrackFollowerKind : std::uint8_t { Base, Enemy, Player };

struct WorldAnimationFrameInput {
    model::Vec3 camera;
    bool disableGobDistance{}; // engine global constructor0
    bool checkEvents{true}; // engine global constructor1
};
float worldAnimationCallbackDistance(model::Vec3 owner,const WorldAnimationFrameInput&,
    model::GameVersion) noexcept;

// Captured CSWTrackFollower state. A LYT track alone does not produce a
// follower type, model list, script binding, or a Go request.
struct TrackAnimationState {
    TrackFollowerKind kind{TrackFollowerKind::Base};
    std::int32_t mode{1};
    float speed{100.0f};
    bool running{}, eventsRegistered{};
    std::vector<std::size_t> models;
    std::optional<std::size_t> playerModel;
    std::optional<std::size_t> lastCustomSource;
    std::string lastCustomEvent;
};

struct WorldAnimationEvent {
    std::size_t track{}, source{};
    // Engine OnAnimKeyEvent requests script slot3 for custom_ and slot9
    // for an enemy loop. These outputs do not execute a server VM.
    std::uint8_t scriptSlot{};
    std::string argument;
};

using WorldAnimationDispatch = std::function<void(std::size_t,const std::string&,float,std::uint32_t)>;

// The caller supplies an existing track Gob. Missing-track selection belongs
// to its engine object lookup, outside these non-null producer operations.
void startTrackAnimation(std::size_t track,TrackAnimationState&,const WorldAnimationDispatch&,
    const std::function<void(std::size_t)>& registerEvent);
void stopTrackAnimation(std::size_t track,TrackAnimationState&,const WorldAnimationDispatch&);
void setTrackAnimationSpeed(std::size_t track,TrackAnimationState&,float,const WorldAnimationDispatch&);
void playTrackFollowerAnimation(const TrackAnimationState&,const std::string&,float,
    std::uint32_t,const WorldAnimationDispatch&);
void playTrackScriptAnimation(const TrackAnimationState&,model::GameVersion,const std::string&,
    int looping,int queued,int overlay,const WorldAnimationDispatch&);
void removeTrackScriptAnimation(const TrackAnimationState&,const std::string&,const WorldAnimationDispatch&);
std::optional<WorldAnimationEvent> trackAnimationEvent(std::size_t track,TrackAnimationState&,
    std::size_t source,const std::string& event,const WorldAnimationDispatch&);

} // namespace neomdl
