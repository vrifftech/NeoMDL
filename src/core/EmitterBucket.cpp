#include "core/EmitterBucket.hpp"
#include <algorithm>
#include <stdexcept>
namespace neomdl {
namespace {
void validateScenes(const std::vector<ParticleScene*>& scenes) {
    for(std::size_t i=0;i<scenes.size();++i) {
        if(!scenes[i])throw std::invalid_argument("Null particle scene in emitter bucket");
        if(std::find(scenes.begin(),scenes.begin()+static_cast<std::ptrdiff_t>(i),scenes[i])!=
           scenes.begin()+static_cast<std::ptrdiff_t>(i))
            throw std::invalid_argument("Duplicate particle scene in emitter bucket");
    }
}
struct SharedEngineServices {
    model::ParticleRandom* random{};
    model::WindManager* wind{};
};
SharedEngineServices validateSharedEngineServices(const std::vector<ParticleScene*>& scenes) {
    model::ParticleRandom* random=nullptr;
    model::WindManager* wind=nullptr;
    for(auto* scene:scenes) {
        const bool engine=std::any_of(scene->emitters().begin(),scene->emitters().end(),
            [scene](const auto& emitter){return scene->engineRuntime(emitter->game);});
        if(!engine)continue;
        auto* candidateRandom=&scene->runtimeRandom();
        auto* candidateWind=&scene->windManager();
        if(!random){random=candidateRandom;wind=candidateWind;continue;}
        if(random!=candidateRandom)
            throw std::invalid_argument("Engine emitter scenes must share one random stream");
        if(wind!=candidateWind)
            throw std::invalid_argument("Engine emitter scenes must share one WindManager");
    }
    return {random,wind};
}
std::vector<ParticleScene*> registeredScenes(const std::vector<ParticleScene*>& roots) {
    std::vector<ParticleScene*> result;
    for(auto* root:roots) {if(root->sceneMember())result.push_back(root);root->appendActiveChunkScenes(result);}
    return result;
}
std::size_t retainedParticleCount(const std::vector<ParticleScene*>& roots) {
    std::size_t total=0;
    for(const auto* root:roots) {
        total+=root->particleCount();
        std::vector<const ParticleScene*> chunks;root->appendAllocatedChunkScenes(chunks);
        for(const auto* chunk:chunks)total+=chunk->particleCount();
    }
    return total;
}
std::size_t rootIndex(const std::vector<ParticleScene*>& roots,const ParticleScene* scene) {
    return static_cast<std::size_t>(std::find(roots.begin(),roots.end(),scene->rootScene())-roots.begin());
}
std::vector<SceneEmitterDraw> sortedDraws(const std::vector<ParticleScene*>& scenes,
    const model::Mat4& view,const std::vector<model::EmitterScenePlane>& planes,
    const model::EngineFrame& cameraFrame) {
    validateScenes(scenes);
    std::vector<SceneEmitterDraw> sources;
    std::vector<model::EmitterBucketEntry> entries;
    bool engineOrdering=true;
    for(auto* scene:registeredScenes(scenes)) {
        scene->prepareChunkFrame();scene->prepareBucket(view,planes,cameraFrame);
        const auto s=rootIndex(scenes,scene);
        for(const auto index:scene->emitterAdmissions()) {
            auto* e=scene->emitters().at(index).get();
            sources.push_back({scene,e,s,index});
            engineOrdering=engineOrdering && scene->engineRuntime(e->game);
        }
    }
    std::stable_sort(sources.begin(),sources.end(),[](const auto& left,const auto& right) {
        return left.emitter->registrationOrder<right.emitter->registrationOrder;
    });
    for(const auto& source:sources) entries.push_back(emitterSortEntry(*source.emitter,entries.size()));
    if(!engineOrdering)return sources;
    const auto camera=cameraFrame.position;
    const auto order=model::engineEmitterBucketDrawOrder(entries,camera);
    std::vector<SceneEmitterDraw> draw;draw.reserve(order.size());
    for(auto i:order)draw.push_back(sources.at(i));
    return draw;
}
struct BucketRemovalScope {
    const std::vector<ParticleScene*>& roots;
    BucketRemovalScope(const std::vector<ParticleScene*>& owners,std::vector<SceneEmitterDraw>& storage,
                       std::size_t& activeCount):roots(owners) {
        for(auto* root:roots) root->setEmitterRemovalSink([&storage,&activeCount](ParticleEmitterInstance* emitter) {
            // RemoveFromSceneManager shifts its live list and decrements count,
            // but leaves the old last backing slot intact. DoEmitterBucket
            // reloads the same index after AnimateParticles, even when removing
            // a preceding child shifted the parent into an earlier live slot.
            for(std::size_t index=activeCount;index>0;--index) if(storage[index-1].emitter==emitter) {
                std::move(storage.begin()+static_cast<std::ptrdiff_t>(index),
                    storage.begin()+static_cast<std::ptrdiff_t>(activeCount),
                    storage.begin()+static_cast<std::ptrdiff_t>(index-1));
                --activeCount;break;
            }
        });
    }
    ~BucketRemovalScope() {for(auto* root:roots)root->setEmitterRemovalSink({});}
};
void finishAll(const std::vector<ParticleScene*>& scenes) noexcept {
    for(auto* scene:scenes)if(scene) {
        scene->finishBucketFrame();
        std::vector<ParticleScene*> chunks;scene->appendActiveChunkScenes(chunks);
        for(auto* chunk:chunks) chunk->finishBucketFrame();
    }
}
}
void prewarmSceneEmitterBucket(const std::vector<ParticleScene*>& scenes,
                               SceneEmitterPrewarmTrace* output) {
    validateScenes(scenes);const auto services=validateSharedEngineServices(scenes);
    SceneEmitterPrewarmTrace trace;
    trace.particlesBefore=retainedParticleCount(scenes);
    trace.randomBefore=services.random?services.random->state:0u;
    std::size_t total=trace.particlesBefore;ParticleChunkBudget chunks;
    try {
        // ListSpecialPartsR appends immediately at ChunkyParticle birth.
        // UpdateEmitters compares the live list length after each hundred-step
        // emitter call, so additions/removals participate in this same traversal.
        for(std::size_t cursor=0;;++cursor) {
            std::vector<SceneEmitterDraw> registered;
            for(auto* scene:registeredScenes(scenes)) if(scene->prewarmPending()) {
                scene->prepareChunkFrame();
                for(std::size_t index=0;index<scene->emitters().size();++index) {
                    auto* emitter=scene->emitters()[index].get();
                    if(emitter->active && scene->engineRuntime(emitter->game))
                        registered.push_back({scene,emitter,rootIndex(scenes,scene),index});
                }
            }
            std::stable_sort(registered.begin(),registered.end(),[](const auto& left,const auto& right) {
                // K1 Desktop 0x353cbe / K2 Desktop 0xc5d10 traverse static model
                // emitter lists before the changing scene Gob emitter list.
                if((left.emitter->ownerIdentity==0)!=(right.emitter->ownerIdentity==0))
                    return left.emitter->ownerIdentity==0;
                return left.emitter->registrationOrder<right.emitter->registrationOrder;
            });
            if(cursor>=registered.size())break;
            const auto current=registered[cursor];ParticlePrewarmStep step;
            total=retainedParticleCount(scenes);
            try {
                current.scene->prewarmRegisteredEmitter(current.emitterIndex,total,chunks,step);
            } catch(...) {
                trace.order.push_back({current.sceneIndex,step.emitterIndex,step.particlesBefore,
                    step.particlesAfter,step.randomBefore,step.randomAfter,step.completed});
                throw;
            }
            trace.order.push_back({current.sceneIndex,step.emitterIndex,step.particlesBefore,
                step.particlesAfter,step.randomBefore,step.randomAfter,step.completed});
        }
        for(auto* scene:scenes) if(scene->prewarmPending()) scene->finishPrewarm();
        trace.particlesAfter=retainedParticleCount(scenes);
        trace.randomAfter=services.random?services.random->state:0u;
        trace.completed=true;
    } catch(...) {
        trace.particlesAfter=retainedParticleCount(scenes);
        trace.randomAfter=services.random?services.random->state:0u;
        if(output)*output=std::move(trace);
        throw;
    }
    if(output)*output=std::move(trace);
}
std::vector<SceneEmitterDraw> executeSceneEmitterBucket(const std::vector<ParticleScene*>& scenes,
    const model::Mat4& view,const std::vector<model::EmitterScenePlane>& planes,
    const SceneEmitterFrameSettings& settingsFrame,SceneEmitterFrameTrace* output,
    const SceneEmitterRenderCallback& render) {
    validateScenes(scenes);validateSharedEngineServices(scenes);
    SceneEmitterFrameTrace trace;trace.requestedElapsed=settingsFrame.elapsedSeconds;
    trace.sceneAnimating=settingsFrame.sceneAnimating;
    try {
        auto draw=sortedDraws(scenes,view,planes,settingsFrame.cameraFrame);
        std::size_t activeCount=draw.size();
        BucketRemovalScope removals(scenes,draw,activeCount);
        std::size_t total=retainedParticleCount(scenes);
        ParticleChunkBudget chunks;
        for(std::size_t cursor=0;cursor<activeCount;++cursor) {
            auto item=draw[cursor];
            total=retainedParticleCount(scenes);
            item.scene->prepareChunkFrame();
            SceneEmitterFrameStep step;step.sceneIndex=item.sceneIndex;step.emitterIndex=item.emitterIndex;
            step.sceneGuard=settingsFrame.sceneAnimating;
            step.ownerGuardQueried=!step.sceneGuard && item.emitter->ownerIdentity!=0;
            step.ownerGuard=step.ownerGuardQueried && item.scene->preparedOwnerAnimating(item.emitterIndex);
            step.animateCalled=step.sceneGuard||step.ownerGuard;
            if(step.animateCalled)step.appliedElapsed=model::engineEmitterBucketElapsed(
                static_cast<float>(settingsFrame.elapsedSeconds),item.emitter->game);
            step.particlesBefore=item.emitter->simulation.particles.size();
            step.randomBefore=item.scene->runtimeRandom().state;
            try {
                if(step.animateCalled)
                    step.animateCompleted=item.scene->animatePreparedEmitter(
                        item.emitterIndex,settingsFrame.elapsedSeconds,total,chunks);
                step.randomAfterAnimate=item.scene->runtimeRandom().state;
                // The engine caller reloads its live bucket index after
                // AnimateParticles, whose Single update may destroy children.
                item=draw[cursor];
                step.renderCalled=true;
                item.scene->renderPreparedEmitter(item.emitterIndex,view,settingsFrame.cameraFrame);
                step.renderFuncCompleted=true;
                step.randomAfterRenderFunc=item.scene->runtimeRandom().state;
                if(render) {
                    step.drawCallbackCalled=true;
                    render(item);
                    step.drawCallbackCompleted=true;
                }
                step.renderCompleted=true;
            } catch(...) {
                step.randomAfterAnimate=item.scene->runtimeRandom().state;
                if(step.renderCalled&&!step.renderFuncCompleted)
                    step.randomAfterRenderFunc=item.scene->runtimeRandom().state;
                step.randomAfterRender=item.scene->runtimeRandom().state;
                step.particlesAfter=item.emitter->simulation.particles.size();
                trace.steps.push_back(step);
                throw;
            }
            step.randomAfterRender=item.scene->runtimeRandom().state;
            step.particlesAfter=item.emitter->simulation.particles.size();
            trace.steps.push_back(step);
        }
        trace.completed=true;
        finishAll(scenes);
        if(output)*output=trace;
        draw.resize(activeCount);
        return draw;
    } catch(...) {
        finishAll(scenes);
        if(output)*output=std::move(trace);
        throw;
    }
}
void discardSceneEmitterFrame(const std::vector<ParticleScene*>& scenes) noexcept {
    finishAll(scenes);
}
} // namespace neomdl
