#include "wx/SceneAudioDevice.hpp"
#include "wx/OpenALProvider.hpp"
namespace neomdl::ui {
SceneAudioBufferKey sceneAudioBufferKey(const SceneAudioPlay& p) {
    return {engineSceneAudioResRef(p.resref),p.resourceType,p.streaming};
}
SceneAudioPreparedBuffers prepareSceneAudioDeviceSounds(SceneAudioDevice& device,
    const PreparedSceneAudioPlayback& assets,const SceneAudioPreparedBuffers& existing,
    const neoshared::bif::JobControl& job) {
    SceneAudioPreparedBuffers result;
    for(const auto& [key,sound]:assets.sounds) {
        job.check();
        if(existing.count(key))continue;
        job.update(result.size(),assets.sounds.size(),"Preparing scene sound "+std::get<0>(key));
        result.emplace(key,device.preparePcm(*sound.pcm));
    }
    return result;
}
std::unique_ptr<SceneAudioDevice> SceneAudioDevice::create() {
    auto device=std::make_unique<SceneAudioDevice>();device->playback_=makeOpenALPlayback();return device;
}
SceneAudioPreparedSound SceneAudioDevice::preparePcm(const neoshared::audio::PcmData& data) {
    return {playback_->prepare(data),static_cast<std::uint32_t>(data.durationSeconds()*1000.0),data.sampleRate};
}
void SceneAudioDevice::play(std::uint64_t id,const SceneAudioPreparedSound& sound,const SceneAudioSourceParameters& p) {
    playback_->play(id,static_cast<neoshared::audio::OpenALPlayback::Buffer>(sound.buffer),
        {p.gain,p.pitch,p.position,p.minimumDistance,p.maximumDistance,p.positional,p.looping});
}
void SceneAudioDevice::preparePausedStream(std::uint64_t id,const SceneAudioPreparedSound& sound) {
    playback_->preparePausedStream(id,static_cast<neoshared::audio::OpenALPlayback::Buffer>(sound.buffer));
}
void SceneAudioDevice::setLooping(std::uint64_t id,bool value) {playback_->setLooping(id,value);}
void SceneAudioDevice::pause(std::uint64_t id) {playback_->pause(id);}
void SceneAudioDevice::resume(std::uint64_t id) {playback_->resume(id);}
void SceneAudioDevice::restart(std::uint64_t id,bool looping) {playback_->restart(id,looping);}
void SceneAudioDevice::stop(std::uint64_t id) {playback_->stop(id);}
bool SceneAudioDevice::playing(std::uint64_t id) const {return playback_->playing(id);}
void SceneAudioDevice::setGain(std::uint64_t id,float gain) {playback_->setGain(id,gain);}
void SceneAudioDevice::setPan(std::uint64_t id,float pan) {playback_->setPan(id,pan);}
void SceneAudioDevice::erase(std::uint64_t id) {playback_->erase(id);}
void SceneAudioDevice::clear() {playback_->clear();}
void SceneAudioDevice::setSuspended(bool value) {playback_->setSuspended(value);}
void SceneAudioDevice::setListener(std::array<float,3> position,std::array<float,3> forward,std::array<float,3> up) {
    playback_->setListener(position,forward,up);
}
}
