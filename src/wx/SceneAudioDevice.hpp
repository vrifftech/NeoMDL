#pragma once
#include "wx/SceneAudioPlayback.hpp"
#include <memory>

namespace neomdl::ui {
struct SceneAudioSourceParameters {
    float gain{1},pitch{1},minimumDistance{10},maximumDistance{20};
    std::array<float,3> position{};
    bool positional{},looping{};
};
SceneAudioBufferKey sceneAudioBufferKey(const SceneAudioPlay&);
// The bundled miniaudio engine serves both games. Game scheduling,
// gain, pitch and stream mixing remain in core; this class owns device state.
class SceneAudioDevice final {
public:
    SceneAudioDevice();
    ~SceneAudioDevice();
    void play(std::uint64_t,const SceneAudioPreparedSound&,const SceneAudioSourceParameters&);
    void preparePausedStream(std::uint64_t,const SceneAudioPreparedSound&);
    void setLooping(std::uint64_t,bool);
    void pause(std::uint64_t);
    void resume(std::uint64_t);
    void restart(std::uint64_t,bool looping);
    bool playing(std::uint64_t) const;
    void setGain(std::uint64_t,float);
    void setPan(std::uint64_t,float);
    void erase(std::uint64_t);
    void setSuspended(bool);
    void setListener(std::array<float,3>,std::array<float,3>,std::array<float,3>);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
SceneAudioPreparedBuffers prepareSceneAudioBuffers(
    const PreparedSceneAudioPlayback&,const SceneAudioPreparedBuffers& existing,
    const neoshared::bif::JobControl&);
}
