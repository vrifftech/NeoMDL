#pragma once
#include "core/SceneAudio.hpp"
#include <list>
#include <map>

namespace neomdl {
struct SceneAudioVoiceLimits {
    std::uint8_t ordinary2D{},ordinary3D{},streams{};
};
SceneAudioVoiceLimits engineSceneAudioDefaultVoiceLimits(model::GameVersion) noexcept;
// Counts captured after engine provider allocation; no configuration clamp is
// applied to these already effective counts.
SceneAudioVoiceLimits engineSceneAudioVoiceLimits(std::uint8_t allocated2D,
    std::uint8_t allocated3D) noexcept;
struct SceneAudioVoiceResource {
    std::uint32_t durationMilliseconds{},sampleRate{};
};
struct SceneAudioVoiceBackend {
    std::function<std::optional<SceneAudioVoiceResource>(const SceneAudioPlay&)> resource;
    // Provider IDs represent physical voices. The request's source is the
    // independent logical owner; a engine stolen one-shot can alias a voice.
    std::function<void(std::uint64_t,const SceneAudioPlay&,std::uint32_t)> start;
    // Already-bound ordinary Play preserves the physical sample and all its
    // settings, changing only looping and its playback offset before resume.
    std::function<void(std::uint64_t,bool)> restart;
    // Streaming InitializeSource precedes engine group/range admission.
    // Replays retain the channel unless a engine close transition ran.
    std::function<void(std::uint64_t,const SceneAudioPlay&,bool retained)> prepareStream;
    std::function<void(std::uint64_t)> resumeStream;
    std::function<void(std::uint64_t)> pauseStream;
    std::function<void(std::uint64_t)> stop;
    std::function<bool(std::uint64_t)> playing;
    std::function<void(std::uint64_t,const SceneAudioPlay&)> updateStream;
};
class SceneAudioVoices final {
public:
    // Clear the previous provider session before resetting this ledger.
    void reset(std::vector<SceneAudioPriority>,SceneAudioVoiceLimits,
               std::function<std::uint32_t()> crt,
               model::GameVersion game=model::GameVersion::Kotor2);
    std::uint32_t play(SceneAudioPlay,const SceneAudioVoiceBackend&,model::Vec3 listener);
    void update(const SceneAudioVoiceBackend&,model::Vec3 listener);
    void stop(std::uint64_t logicalSource,const SceneAudioVoiceBackend&);
    void clear(const SceneAudioVoiceBackend&);
    bool playing(std::uint64_t logicalSource,const SceneAudioVoiceBackend&) const;
    // Explicit capture of the engine slot's reserved flag, not a new policy.
    void setReserved(bool positional,std::size_t slot,bool reserved);
private:
    struct Slot {std::optional<std::uint64_t> owner;bool reserved{};};
    struct Voice {
        SceneAudioPlay request;SceneAudioVoiceResource resource;
        std::optional<std::size_t> slot;
        std::uint64_t provider{};
        std::uint32_t frequency{};
        bool playingList{},waitingList{},disposal{};
    };
    using List=std::list<std::uint64_t>;
    void removePlaying(std::uint64_t);
    void removeWaiting(std::uint64_t);
    void wait(std::uint64_t);
    void dispose(std::uint64_t,const SceneAudioVoiceBackend&);
    void shutdown(std::uint64_t,const SceneAudioVoiceBackend&);
    void closeStream(std::uint64_t,const SceneAudioVoiceBackend&);
    void destroy(std::uint64_t,const SceneAudioVoiceBackend&);
    void interrupt(std::uint64_t,const SceneAudioVoiceBackend&);
    std::optional<std::size_t> allocate(const SceneAudioPlay&,const SceneAudioVoiceBackend&);
    bool admitOrdinary(std::uint64_t,bool retry,const SceneAudioVoiceBackend&,model::Vec3);
    bool admitStream(std::uint64_t,bool retry,const SceneAudioVoiceBackend&,model::Vec3);
    bool inRange(const SceneAudioPlay&,model::Vec3) const noexcept;
    std::vector<SceneAudioPriority> priorities_;
    std::vector<std::uint8_t> groupCounts_;
    std::vector<Slot> slots2D_,slots3D_;
    std::uint8_t streamLimit_{};
    std::function<std::uint32_t()> random_;
    model::GameVersion game_{model::GameVersion::Kotor2};
    std::map<std::uint64_t,Voice> voices_;
    List playing_,waiting_,streams_,waitingStreams_,disposal_;
    std::uint64_t nextStreamProvider_{std::uint64_t{1}<<32};
};
} // namespace neomdl
