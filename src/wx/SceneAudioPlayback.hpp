#pragma once

#include "core/AreaScene.hpp"
#include "core/SceneAudio.hpp"
#include <neoshared/audio/Audio.hpp>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <tuple>

namespace neomdl::ui {
using SceneAudioResourceKey=std::tuple<std::string,std::uint16_t,bool>;
using SceneAudioBufferKey=SceneAudioResourceKey;
struct SceneAudioPreparedSound {
    std::shared_ptr<const neoshared::audio::PcmData> pcm;
    std::uint32_t durationMilliseconds{},sampleRate{};
};
using SceneAudioPreparedBuffers=std::map<SceneAudioBufferKey,SceneAudioPreparedSound>;
struct PreparedSceneAudioPlayback {
    std::map<SceneAudioResourceKey,std::shared_ptr<const neoshared::audio::PcmData>> sounds;
    std::vector<SceneAudioPriority> priorities;
};

// Run in the same cancellable worker as module loading. Runtime sound events
// only consult this immutable PCM bank; they never perform resource I/O.
PreparedSceneAudioPlayback prepareSceneAudioPlayback(const AreaScene&,
    const ResourceResolver&,model::GameVersion,
    const neoshared::bif::JobControl&);
// Mine-explosion names use the first-byte stream classification shared with
// mineExplosionAudio.
PreparedSceneAudioPlayback prepareSceneAudioPlayback(const std::vector<std::string>& names,
    const ResourceResolver&,model::GameVersion,
    const neoshared::bif::JobControl&);
} // namespace neomdl::ui
