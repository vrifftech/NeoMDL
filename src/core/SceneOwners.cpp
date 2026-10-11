#include "core/SceneOwners.hpp"
#include "core/ModelDocument.hpp"
#include "core/ReferenceScene.hpp"
#include "core/AreaScene.hpp"
#include "core/ParticleScene.hpp"
#include "core/SceneQueries.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
namespace neomdl {
namespace {
void bindOwnerHistory(SceneOwnerView& view,const SceneAnimationOwner& source) {
    // State identity is stable across handle moves; callbacks are borrowed and
    // guarded by the separately invalidated engine owner lifetime token.
    const auto* owner=&source;
    view.animationOwner=owner;
    view.resolvePreviousRenderedFrame=[owner]{return owner->previousRenderedFrame();};
    view.commitRenderedFrame=[owner](const model::EngineFrame& frame){owner->commitRenderedFrame(frame);};
    view.resolveRangePosition=[owner,asset=view.asset,pose=view.pose,
            frame=view.resolveFrame]() -> std::optional<model::Vec3> {
        if(!owner->inCutscene())return {};
        // FindPart is case-insensitive, parent-first, then authored children
        // (K2 Desktop 0xa7f2f). The caller keeps Gob origin when no Part is found.
        const auto find=[&](const auto& self,std::size_t index)->std::optional<std::size_t> {
            const auto& node=asset->nodes[index];
            constexpr char name[]="cutscenedummy";
            bool equal=node.name.size()==sizeof(name)-1u;
            for(std::size_t i=0;equal&&i<sizeof(name)-1u;++i) {
                const char c=node.name[i];
                equal=(c>='A'&&c<='Z'?char(c-'A'+'a'):c)==name[i];
            }
            if(equal)return index;
            for(const auto child:node.children)if(const auto found=self(self,child))return found;
            return {};
        };
        for(std::size_t index=0;index<asset->nodes.size();++index)if(!asset->nodes[index].parent) {
            if(const auto node=find(find,index))
                return model::enginePartWorld(*asset,*pose,frame(),*node).position;
            break; // ordinary Gobs have one root Part
        }
        return {};
    };
}
void require(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(std::string("Scene owners: ")+message);
} }
std::vector<SceneOwnerView> collectSceneOwners(
        const ModelDocument& document,const model::Pose& pose,
        const ReferenceScene& references,const AreaScene& area,
        bool includeReferences,float objectLightRadius,const model::DanglySimulationState* mainDangly,
        const ParticleScene* inspectionParticles,SceneAnimationOwner* mainAnimationOwner) {
    require(document.isOpen(),"registration requires a live document or module scene");
    require(std::isfinite(objectLightRadius)&&objectLightRadius>=0,"invalid object light radius");
    std::vector<SceneOwnerView> casters;
    const auto add=[&](LightOwnerId id,const std::string& label,const model::Model& asset,
        const model::Pose* current,const model::DanglySimulationState* dangly,const model::EngineFrame& frame) {
        const auto world=model::nodeTransform(frame.position,frame.orientation);
        const auto ownerScale=frame.scale;
        SceneOwnerView caster;caster.id=id;caster.label=label;caster.asset=&asset;caster.pose=current;
        caster.dangly=dangly;caster.frame=frame;caster.world=world;caster.scale=ownerScale;
        caster.lightSphere={model::transformPoint(world,{}),objectLightRadius};
        // Non-casters can still supply the selected light container. Their
        // registration must not require geometry bounds that they do not own;
        // buildFrame rejects their classification before any projection work.
        caster.visible=area.empty()||!(asset.classification&0x24u)||
            area.ownerVisible(asset.classification,asset.bounds,world);
        caster.renderVisible=area.empty()||asset.meshes.empty()||
            ((asset.bounds.valid||(asset.classification&model::kK2ModelCharacter))&&
             area.ownerVisible(asset.classification,asset.bounds,world));
        casters.push_back(std::move(caster));
    };
    if(document.hasModelOwner()) {
        add(mainLightOwner,"Inspection owner",document.model(),&pose,mainDangly,references.mainFrame());
        casters.back().sourceDocument=&document;
        casters.back().resolveFrame=[&references]{return references.mainFrame();};
        casters.back().resolveWorld=[&references]{return references.mainWorld();};
        casters.back().resolveScale=[&references]{return references.ownerScale();};
        if(mainAnimationOwner) {casters.back().ownerLifetime=mainAnimationOwner->lifetime();
            bindOwnerHistory(casters.back(),*mainAnimationOwner);}
    }
    if(includeReferences)for(std::size_t i=0;i<document.references().size();++i)
        if(const auto* ref=references.instance(i);ref&&ref->active&&ref->sceneMember) {
            add(i+1,"Inspection reference "+std::to_string(i),document.references()[i].model,&ref->pose,&ref->danglyState,ref->frame);
            casters.back().sourceDocument=&document;
            casters.back().ownerLifetime=ref->animationOwner.lifetime();
            casters.back().resolveFrame=[&references,i]{return references.instance(i)->frame;};
            casters.back().resolveWorld=[&references,i]{return references.instance(i)->world;};
            casters.back().resolveScale=[&references,i]{return references.instance(i)->scale;};
            bindOwnerHistory(casters.back(),ref->animationOwner);
        }
    for(const auto& object:area.objects()) {
        if(!object->staticPrimary()) {
            add(object->ownerId(),object->label(),object->document()->model(),
                &object->pose(),&object->danglyState(),object->frame());
            casters.back().sourceDocument=object->document().get();
            casters.back().ownerLifetime=object->animationOwner().lifetime();
            casters.back().resolveFrame=[owner=object.get()]{return owner->frame();};
            casters.back().resolveScale=[owner=object.get()]{return owner->frame().scale;};
            casters.back().resolveWorld=[owner=object.get()]{return owner->world();};
            bindOwnerHistory(casters.back(),object->animationOwner());
        }
        if(includeReferences)for(std::size_t i=0;i<object->references().instances().size();++i) {
            const auto& ref=object->references().instances()[i];if(ref.active&&ref.sceneMember) {
                add(object->ownerId(i),object->label()+" / reference "+std::to_string(i),
                    object->document()->references()[i].model,&ref.pose,&ref.danglyState,ref.frame);
                casters.back().sourceDocument=object->document().get();
                casters.back().ownerLifetime=ref.animationOwner.lifetime();
                casters.back().resolveFrame=[owner=object.get(),i]{return owner->references().instance(i)->frame;};
                casters.back().resolveWorld=[owner=object.get(),i]{return owner->references().instance(i)->world;};
                casters.back().resolveScale=[owner=object.get(),i]{return owner->references().instance(i)->scale;};
                bindOwnerHistory(casters.back(),ref.animationOwner);
            }
        }
    }
    auto chunks=collectParticleChunkOwners(inspectionParticles,area,includeReferences);
    casters.insert(casters.end(),std::make_move_iterator(chunks.begin()),std::make_move_iterator(chunks.end()));
    return casters;
}
std::vector<SceneOwnerView> collectParticleChunkOwners(
        const ParticleScene* inspectionParticles,const AreaScene& area,bool includeReferences,bool registeredOnly) {
    std::vector<const ParticleScene*> scenes;
    const auto collect=[&](const ParticleScene& source) {
        if(registeredOnly)source.appendActiveChunkScenes(scenes);
        else source.appendAllocatedChunkScenes(scenes);
    };
    if(inspectionParticles)collect(*inspectionParticles);
    for(const auto* root:area.particleSceneRoots())collect(*root);
    if(registeredOnly)std::stable_sort(scenes.begin(),scenes.end(),[](const auto* left,const auto* right) {
        return left->chunkOwner()->sceneRegistration<right->chunkOwner()->sceneRegistration;
    });
    std::vector<SceneOwnerView> owners;
    const auto add=[&](LightOwnerId id,const std::string& label,const ModelDocument& document,
            const model::Model& asset,const model::Pose& pose,
            const model::DanglySimulationState& dangly,const model::EngineFrame& frame,float radius) {
        const auto world=model::nodeTransform(frame.position,frame.orientation);
        const auto scale=frame.scale;
        SceneOwnerView owner;
        owner.id=id;owner.label=label;owner.asset=&asset;owner.sourceDocument=&document;
        owner.pose=&pose;owner.dangly=&dangly;owner.frame=frame;owner.world=world;owner.scale=scale;
        owner.lightSphere={model::transformPoint(world,{}),radius};
        owner.visible=area.empty()||!(asset.classification&0x24u)||
            area.ownerVisible(asset.classification,asset.bounds,world);
        owner.renderVisible=area.empty()||asset.meshes.empty()||
            ((asset.bounds.valid||(asset.classification&model::kK2ModelCharacter))&&
             area.ownerVisible(asset.classification,asset.bounds,world));
        owners.push_back(std::move(owner));
    };
    for(const auto* scene:scenes) {
        const auto& chunk=*scene->chunkOwner();
        const auto label="Particle chunk "+std::to_string(chunk.ownerIdentity);
        add(chunk.ownerIdentity,label,*chunk.asset,chunk.asset->model(),chunk.pose,
            chunk.danglyState,chunk.frame,chunk.engineOwnerRadius);
        owners.back().ownerLifetime=chunk.lifetime;
        owners.back().resolveFrame=[source=&chunk] {return source->frame;};
        owners.back().resolveWorld=[source=&chunk] {return source->world;};
        owners.back().resolveScale=[source=&chunk] {return source->ownerScale;};
        bindOwnerHistory(owners.back(),chunk.animationOwner);
        if(includeReferences)for(std::size_t i=0;i<chunk.references->instances().size();++i) {
            const auto& reference=chunk.references->instances()[i];
            if(registeredOnly&&(!reference.active||!reference.sceneMember))continue;
            const auto& asset=chunk.asset->references()[i].model;
            add(chunk.ownerIdentity+i+1u,label+" / reference "+std::to_string(i),
                *chunk.asset,asset,reference.pose,reference.danglyState,reference.frame,
                engineConstructedOwnerSphere(asset).radius);
            owners.back().ownerLifetime=reference.animationOwner.lifetime();
            owners.back().resolveFrame=[source=&reference] {return source->frame;};
            owners.back().resolveWorld=[source=&reference] {return source->world;};
            owners.back().resolveScale=[source=&reference] {return source->scale;};
            bindOwnerHistory(owners.back(),reference.animationOwner);
        }
    }
    return owners;
}
} // namespace neomdl
