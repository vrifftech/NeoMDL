#pragma once
#include "model/Model.hpp"
#include "core/ResourceContext.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {
// Sound fields and Play arguments pass through engine sixteen-byte CResRef.
// An empty reference is retained for the engine resource-open failure path.
std::string engineSceneAudioResRef(const std::string&);
struct AreaSoundRecord {
    std::size_t sourceIndex{};
    std::string tag,templateResref,templateIdentity;
    model::Vec3 position{};
    bool active{true},positional{true},looping{};
    std::uint8_t volume{127},volumeVariation{},times{3},generatedType{};
    std::uint32_t hours{},interval{},intervalVariation{};
    float pitchVariation{},fixedVariance{1.0f},minimumDistance{10.0f},maximumDistance{20.0f};
    bool continuous{},random{},randomPosition{};
    float randomRangeX{},randomRangeY{};
    std::vector<std::string> sounds;
};
struct SceneAudioClock {
    std::uint32_t day{},milliseconds{},dayMilliseconds{7200000},hour{};
    std::uint8_t engineTimeOfDay{}; // module byte: 0/1/3 day, 2/4 night
};
class AreaEnvironmentRuntime;
std::optional<SceneAudioClock> sceneAudioClock(const AreaEnvironmentRuntime&) noexcept;
struct SceneAudioRandom {
    std::function<std::uint32_t()> crt; // engine desktop CRT rand domain
    std::function<std::uint32_t()> exo; // CExoRand::Rand, separate from CRT state
};
struct SceneAudioPlay {
    std::uint64_t source{};
    std::string resref;
    std::uint16_t resourceType{4};
    bool positional{},looping{},streaming{},oneShot{};
    bool applyVariations{};
    model::Vec3 position{};
    std::uint8_t volume{127},priorityGroup{};
    float minimumDistance{10.0f},maximumDistance{20.0f};
    float pitchVariation{},fixedVariance{1.0f};
};
struct SceneAudioPriority {
    std::uint8_t priority{},volume{127},maxPlaying{255};
    std::int32_t interrupt{1};
    std::uint16_t fadeMilliseconds{};
    float minimumDistance{10},maximumDistance{20},pitchVariation{};
};
std::vector<SceneAudioPriority> loadSceneAudioPriorities(const ResourceResolver&);
float engineSceneAudioGain(const SceneAudioPlay&,const SceneAudioPriority&,float sliderVolume=1.0f,float twoThreeDBalance=1.0f) noexcept;
float engineSceneAudioPitch(const SceneAudioPlay&,const SceneAudioPriority&,
    unsigned sampleRate,const std::function<std::uint32_t()>& random);
std::uint32_t engineSceneAudioFrequency(const SceneAudioPlay&,const SceneAudioPriority&,
    unsigned sampleRate,const std::function<std::uint32_t()>& random);
SceneAudioPlay mineExplosionAudio(std::uint64_t source,std::string resref,
    model::Vec3 position,const SceneAudioPriority&);
struct SceneAudioBackend {
    // Play returns the engine source duration in milliseconds, or zero for
    // source/resource failure. The host resolves only its prepared PCM map.
    std::function<std::uint32_t(const SceneAudioPlay&)> play;
    std::function<void(std::uint64_t)> stop;
    std::function<bool(std::uint64_t)> playing;
};
struct SceneAudioListener {
    model::Vec3 position{},forward{0,0,-1},up{0,1,0};
};
struct SceneAudioStreamMix {float gain{},pan{};bool writePan{true};};
// Engine world coordinates, before the playback Z conversion. A missing
// result means Update3DStream leaves the provider's retained gain untouched.
std::optional<SceneAudioStreamMix> engineStreamingAudioMix(const SceneAudioPlay&,
    const SceneAudioListener&,float baseGain,int engineSoundMode,model::GameVersion,
    float obstruction=0.0f) noexcept;
// The engine/provider boundary negates world Z for position and both axes.
SceneAudioListener engineAudioProviderListener(SceneAudioListener) noexcept;
model::Vec3 engineAudioProviderPosition(model::Vec3) noexcept;
// Placeable dialogue cameras move the listener along the camera's forward
// axis by MicRange; this is not an attenuation-radius override.
SceneAudioListener placeableCameraAudioListener(model::Vec3 position,
    model::Quaternion orientation,float microphoneRange) noexcept;
bool engineAudioWithinRange(model::Vec3 source,model::Vec3 listener,float maximumDistance) noexcept;

class SceneAudioRuntime final {
public:
    void load(std::vector<AreaSoundRecord>,SceneAudioClock,SceneAudioRandom,
              model::GameVersion game=model::GameVersion::Kotor2);
    void clear(const SceneAudioBackend&);
    void update(SceneAudioClock,const SceneAudioBackend&);
    void play(std::size_t,SceneAudioClock,const SceneAudioBackend&);
    void stop(std::size_t,const SceneAudioBackend&);
    const std::vector<AreaSoundRecord>& records() const noexcept {return records_;}
private:
    struct State {
        bool active{},timeEnabled{true},streaming{},looping{},restartLoop{},first{true};
        std::size_t index{};
        std::uint32_t duration{},interval{};
        SceneAudioClock last;
        std::vector<std::string> sounds;
    };
    void updateInterval(std::size_t);
    void updateTime(std::size_t,SceneAudioClock);
    std::string nextSound(std::size_t);
    std::uint32_t start(std::size_t,const std::string&,bool,const SceneAudioBackend&);
    std::vector<AreaSoundRecord> records_;
    std::vector<State> states_;
    SceneAudioRandom random_;
    std::uint8_t lastTimeOfDay_{};
};
} // namespace neomdl
