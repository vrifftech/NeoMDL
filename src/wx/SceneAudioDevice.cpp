#include "wx/SceneAudioDevice.hpp"
#include "miniaudio.h"
#include <stdexcept>

namespace neomdl::ui {
namespace {
void checkAudio(ma_result result,const char* operation) {
    if(result!=MA_SUCCESS)
        throw std::runtime_error(std::string("Scene audio ")+operation+": "+ma_result_description(result));
}

struct Voice {
    std::shared_ptr<const neoshared::audio::PcmData> pcm;
    ma_audio_buffer buffer{};
    ma_sound sound{};
    bool resumeAfterSuspend{};

    Voice(ma_engine& engine,std::shared_ptr<const neoshared::audio::PcmData> data):pcm(std::move(data)) {
        auto config=ma_audio_buffer_config_init(ma_format_f32,pcm->channels,
            pcm->frameCount(),pcm->samples.data(),nullptr);
        config.sampleRate=pcm->sampleRate;
        checkAudio(ma_audio_buffer_init(&config,&buffer),"buffer initialization failed");
        const auto result=ma_sound_init_from_data_source(&engine,&buffer,
            MA_SOUND_FLAG_NO_SPATIALIZATION,nullptr,&sound);
        if(result!=MA_SUCCESS) {
            ma_audio_buffer_uninit(&buffer);
            checkAudio(result,"voice initialization failed");
        }
        ma_sound_set_positioning(&sound,ma_positioning_relative);
    }
    ~Voice() {
        ma_sound_uninit(&sound); // Detaches from the mixer before releasing PCM.
        ma_audio_buffer_uninit(&buffer);
    }
};
}

struct SceneAudioDevice::Impl {
    ma_engine engine{};
    std::map<std::uint64_t,std::unique_ptr<Voice>> voices;
    bool suspended{};

    Impl() {
        auto config=ma_engine_config_init();
        config.channels=2;
        config.periodSizeInMilliseconds=10;
        checkAudio(ma_engine_init(&config,&engine),"device initialization failed");
    }
    ~Impl() {
        voices.clear();
        ma_engine_uninit(&engine);
    }
};

SceneAudioBufferKey sceneAudioBufferKey(const SceneAudioPlay& p) {
    return {engineSceneAudioResRef(p.resref),p.resourceType,p.streaming};
}
SceneAudioPreparedBuffers prepareSceneAudioBuffers(const PreparedSceneAudioPlayback& assets,
    const SceneAudioPreparedBuffers& existing,const neoshared::bif::JobControl& job) {
    SceneAudioPreparedBuffers result;
    for(const auto& [key,sound]:assets.sounds) {
        job.check();
        if(existing.count(key))continue;
        job.update(result.size(),assets.sounds.size(),"Preparing scene sound "+std::get<0>(key));
        result.emplace(key,SceneAudioPreparedSound{sound.pcm,
            static_cast<std::uint32_t>(sound.pcm->durationSeconds()*1000.0),sound.pcm->sampleRate});
    }
    return result;
}

SceneAudioDevice::SceneAudioDevice():impl_(std::make_unique<Impl>()) {}
SceneAudioDevice::~SceneAudioDevice()=default;

void SceneAudioDevice::preparePausedStream(std::uint64_t id,const SceneAudioPreparedSound& sound) {
    impl_->voices.erase(id);
    impl_->voices.emplace(id,std::make_unique<Voice>(impl_->engine,sound.pcm));
}
void SceneAudioDevice::play(std::uint64_t id,const SceneAudioPreparedSound& sound,
    const SceneAudioSourceParameters& p) {
    preparePausedStream(id,sound);
    auto& voice=*impl_->voices.at(id);
    ma_sound_set_volume(&voice.sound,p.gain);
    ma_sound_set_pitch(&voice.sound,p.pitch);
    ma_sound_set_looping(&voice.sound,p.looping);
    // Stereo resources retain their authored channels; only mono is spatialized.
    if(p.positional&&voice.pcm->channels==1) {
        ma_sound_set_spatialization_enabled(&voice.sound,MA_TRUE);
        ma_sound_set_attenuation_model(&voice.sound,ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&voice.sound,p.minimumDistance);
        ma_sound_set_max_distance(&voice.sound,p.maximumDistance);
        ma_sound_set_position(&voice.sound,p.position[0],p.position[1],p.position[2]);
        // Preserve the engine bridge's listener-relative zero-origin behavior.
        if(p.position[0]!=0||p.position[1]!=0||p.position[2]!=0)
            ma_sound_set_positioning(&voice.sound,ma_positioning_absolute);
    }
    resume(id);
}
void SceneAudioDevice::setLooping(std::uint64_t id,bool value) {
    ma_sound_set_looping(&impl_->voices.at(id)->sound,value);
}
void SceneAudioDevice::pause(std::uint64_t id) {
    auto& voice=*impl_->voices.at(id);
    checkAudio(ma_sound_stop(&voice.sound),"pause failed");
    voice.resumeAfterSuspend=false;
}
void SceneAudioDevice::resume(std::uint64_t id) {
    auto& voice=*impl_->voices.at(id);
    voice.resumeAfterSuspend=impl_->suspended;
    if(!impl_->suspended)checkAudio(ma_sound_start(&voice.sound),"start failed");
}
void SceneAudioDevice::restart(std::uint64_t id,bool looping) {
    auto& voice=*impl_->voices.at(id);
    const bool ended=ma_sound_at_end(&voice.sound);
    pause(id);
    ma_sound_set_looping(&voice.sound,looping);
    checkAudio(ma_sound_seek_to_pcm_frame(&voice.sound,0),"restart seek failed");
    // An already completed ordinary sample remains stopped, as in the engine.
    if(!ended)resume(id);
}
bool SceneAudioDevice::playing(std::uint64_t id) const {
    const auto found=impl_->voices.find(id);
    return found!=impl_->voices.end()&&(found->second->resumeAfterSuspend||
        ma_sound_is_playing(&found->second->sound));
}
void SceneAudioDevice::setGain(std::uint64_t id,float gain) {
    ma_sound_set_volume(&impl_->voices.at(id)->sound,gain);
}
void SceneAudioDevice::setPan(std::uint64_t id,float pan) {
    auto& voice=*impl_->voices.at(id);
    if(voice.pcm->channels==1)ma_sound_set_pan(&voice.sound,pan);
}
void SceneAudioDevice::erase(std::uint64_t id) {impl_->voices.erase(id);}
void SceneAudioDevice::setSuspended(bool value) {
    if(impl_->suspended==value)return;
    impl_->suspended=value;
    for(auto& [id,voice]:impl_->voices) {
        if(value) {
            voice->resumeAfterSuspend=ma_sound_is_playing(&voice->sound);
            checkAudio(ma_sound_stop(&voice->sound),"suspension failed");
        } else if(voice->resumeAfterSuspend) {
            checkAudio(ma_sound_start(&voice->sound),"resume failed");
            voice->resumeAfterSuspend=false;
        }
    }
}
void SceneAudioDevice::setListener(std::array<float,3> position,std::array<float,3> forward,
    std::array<float,3> up) {
    ma_engine_listener_set_position(&impl_->engine,0,position[0],position[1],position[2]);
    ma_engine_listener_set_direction(&impl_->engine,0,forward[0],forward[1],forward[2]);
    ma_engine_listener_set_world_up(&impl_->engine,0,up[0],up[1],up[2]);
}
}
