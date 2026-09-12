#include "wx/SceneAudioPlayback.hpp"
#include "core/SceneAudioSource.hpp"
#include <set>

namespace neomdl::ui {
namespace {
void addMine(std::set<SceneAudioResourceKey>& requested,const std::string& name) {
    const auto request=mineExplosionAudio(0,name,{},{});
    requested.emplace(engineSceneAudioResRef(name),request.resourceType,request.streaming);
}
PreparedSceneAudioPlayback prepare(const std::set<SceneAudioResourceKey>& requested,
    const ResourceResolver& resolver,model::GameVersion game,
    const neoshared::bif::JobControl& job) {
    ResourceLoadScope scope(job);
    PreparedSceneAudioPlayback result;
    // Engine sound initialization loads the group table before any object
    // requests a voice, including areas whose first sound is a placed mine.
    result.priorities=loadSceneAudioPriorities(resolver);
    std::uint64_t completed=0;
    for(const auto& key:requested) {
        const auto& [name,type,streaming]=key;
        job.update(completed,requested.size(),"Preparing scene sound "+name);
        if(!name.empty())if(auto resource=resolver(name,type)) {
            if(const auto span=engineSceneAudioSourceSpan(resource->bytes,streaming,game)) {
                PreparedSceneAudioResource sound;
                const std::vector<std::uint8_t> encoded(resource->bytes.begin()+span->offset,
                    resource->bytes.begin()+span->offset+span->size);
                sound.pcm=neoshared::audio::decodeBytes(encoded,[&]{job.check();});
                result.sounds.emplace(key,std::move(sound));
            }
        }
        ++completed;
    }
    job.update(completed,requested.size(),"Scene sounds ready");
    return result;
}
}
PreparedSceneAudioPlayback prepareSceneAudioPlayback(const AreaScene& area,
    const ResourceResolver& resolver,model::GameVersion game,
    const neoshared::bif::JobControl& job) {
    std::set<SceneAudioResourceKey> requested;
    for(const auto& sound:area.sounds())
        for(const auto& name:sound.sounds)requested.emplace(engineSceneAudioResRef(name),4u,sound.looping);
    for(const auto& mine:area.triggerRuntime().mines())
        if(!mine.explosionSound.empty())addMine(requested,mine.explosionSound);
    return prepare(requested,resolver,game,job);
}
PreparedSceneAudioPlayback prepareSceneAudioPlayback(const std::vector<std::string>& names,
    const ResourceResolver& resolver,model::GameVersion game,
    const neoshared::bif::JobControl& job) {
    std::set<SceneAudioResourceKey> requested;
    for(const auto& name:names)addMine(requested,name);
    return prepare(requested,resolver,game,job);
}
} // namespace neomdl::ui
