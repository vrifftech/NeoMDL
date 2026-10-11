#include "core/AreaObject.hpp"
#include "core/SceneQueries.hpp"
#include <model/AsciiModel.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace neomdl {
namespace {
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c-'A'+'a');
    return value;
}
void require(bool ok, const char* message) {
    if (!ok) throw model::ModelError(message);
}
model::Model view(const model::Model& source, const std::vector<std::uint8_t>& admitted,
                  bool collapsed, std::optional<std::size_t> animatedRoot,
                  std::size_t root) {
    auto result = source;
    // SpawnRoom marks the loaded model as a room before constructing either
    // the surviving Gob or the collapsed Parts (K1 0x35abba / K2 0xc82c9).
    result.classification |= 0x100u;
    result.sceneNodeAdmission = admitted;
    result.collapsedRoomParts = collapsed;
    // Keep detached Parts as inert skeleton anchors: palettes address original
    // node indices even when a referenced bone lies outside the surviving Gob.
    const auto& job=resourceLoadJob();job.check();
    const auto frames = engineCollapsedHitPose(source, model::bindPose(source,job), model::EngineFrame{});
    for (std::size_t i = 0; i < result.nodes.size(); ++i) {
        job.check();
        auto& node = result.nodes[i];
        if (!admitted[i]) {
            node = {};
            node.position = frames[i].position;
            node.orientation = frames[i].orientation;
        } else if (collapsed) {
            node.parent.reset(); node.children.clear();
            node.position = frames[i].position;
            node.orientation = frames[i].orientation;
        } else {
            node.children.erase(std::remove_if(node.children.begin(), node.children.end(),
                [&](auto child) { return child >= admitted.size() || !admitted[child]; }), node.children.end());
            if (i == root) node.children = {*animatedRoot};
            if (i == *animatedRoot) node.parent = root;
        }
    }
    for (auto& mesh : result.meshes) if (mesh.nodeIndex >= admitted.size() || !admitted[mesh.nodeIndex]) {
        const auto node = mesh.nodeIndex;
        mesh = {}; mesh.nodeIndex = node; mesh.render = false;
    }
    if (collapsed) { result.animations.clear(); result.declaredAnimationCount = 0u; }
    const auto pose = model::bindPose(result,job);
    result.worldTransforms = pose.worldTransforms;
    result.bounds = pose.bounds;
    return result;
}
std::string animationKey(const std::optional<ObjectAnimationCommand>& command) {
    if(!command)return {};
    std::ostringstream out;out<<static_cast<unsigned>(command->family)<<':'
        <<static_cast<unsigned>(command->serializedState)<<':'<<command->serverAnimation<<':'
        <<(command->clientAnimation?std::to_string(*command->clientAnimation):std::string("-"))<<':'
        <<lower(command->animationName)<<':'<<command->runFlags<<':'<<command->playbackRate<<':'<<command->startTime;
    return out.str();
}
bool hasOwnerEffects(const OwnerVisualEffects& effects) noexcept {
    return effects.hologram||effects.distortion||effects.colorShiftEnabled||
        effects.illuminationEnabled||effects.perObjectIlluminationEnabled||
        effects.instanceOpacity!=1.0f||!effects.environmentMap.empty()||
        effects.shellAmount!=0.0f||effects.forceSight.glow||effects.forceSight.nonForceBlending;
}
}
RoomModelPartition partitionK2Room(const model::Model& source) {
    const auto& job=resourceLoadJob();job.update(0,0,"Partitioning room "+source.name);
    require(!source.nodes.empty(), "Room has no root Part");
    const auto root = std::find_if(source.nodes.begin(), source.nodes.end(), [](const auto& n) { return !n.parent; });
    require(root != source.nodes.end(), "Room has no root Part");
    const auto rootIndex = static_cast<std::size_t>(root-source.nodes.begin());
    const auto name = lower(source.name + "a");
    std::optional<std::size_t> animated;
    for (std::size_t i = 0; i < source.nodes.size(); ++i)
        if (lower(source.nodes[i].name) == name) { animated = i; break; }
    require(!animated || *animated != rootIndex, "Animated room subtree cannot be its own root");
    std::vector<std::uint8_t> staticMask(source.nodes.size(), 1u), dynamicMask(source.nodes.size());
    if (animated) {
        std::vector<std::size_t> todo{*animated};
        while (!todo.empty()) {
            job.check();
            const auto i=todo.back(); todo.pop_back();
            require(i<source.nodes.size() && !dynamicMask[i], "Cyclic/shared animated room subtree");
            dynamicMask[i]=1u; staticMask[i]=0u;
            todo.insert(todo.end(),source.nodes[i].children.begin(),source.nodes[i].children.end());
        }
        dynamicMask[rootIndex]=1u; staticMask[rootIndex]=0u;
    }
    RoomModelPartition result;
    result.animatedRoot=animated;
    // SpawnRoom constructs the full Gob before detaching its static Parts.
    // A room without the animated subtree uses CreateInstanceTree(NULL),
    // which does not call ProcessSkinSeams (K1 Desktop 0x35acac/0x35ad34).
    if(animated) model::processAsciiSkinSeams(source,job);
    result.staticParts=view(source,staticMask,true,{},rootIndex);
    if(animated) result.animatedParts=view(source,dynamicMask,false,animated,rootIndex);
    return result;
}
bool engineAreaRuntimeSupported(model::GameVersion) noexcept {


    return true;
}
const char* areaObjectKindName(AreaObjectKind kind) noexcept {
    switch(kind) {
    case AreaObjectKind::StaticRoomParts: return "static room Parts";
    case AreaObjectKind::AnimatedRoom: return "animated room Gob";
    case AreaObjectKind::Track: return "track Gob";
    case AreaObjectKind::TrackModel: return "mini-game model Gob";
    case AreaObjectKind::Creature: return "GIT creature";
    case AreaObjectKind::Placeable: return "GIT placeable";
    case AreaObjectKind::Door: return "GIT door";
    case AreaObjectKind::Weather: return "weather Gob";
    case AreaObjectKind::Trigger: return "GIT mine Gob";
    }
    return "unknown area object";
}
AreaObject::AreaObject(std::size_t firstInstance, AreaObjectKind kind, std::string label,
        std::shared_ptr<ModelDocument> document, const model::Mat4& world,
        std::optional<std::size_t> room, AreaRuntimeServices services, bool sceneMember)
    : AreaObject(firstInstance,kind,std::move(label),std::move(document),
        engineRigidFrame(world),room,std::move(services),sceneMember) {}
AreaObject::AreaObject(std::size_t firstInstance, AreaObjectKind kind, std::string label,
        std::shared_ptr<ModelDocument> document, const model::EngineFrame& frame,
        std::optional<std::size_t> room, AreaRuntimeServices services, bool sceneMember)
    : firstInstance_(firstInstance), kind_(kind), label_(std::move(label)), room_(room),
      document_(std::move(document)) {
    resourceLoadJob().check();
    require(document_ && document_->isOpen(), "Area runtime requires an open document");
    require(engineAreaRuntimeSupported(document_->model().game),
            "Unsupported area runtime");
    require(firstInstance_>=0x90000000ull && firstInstance_+document_->references().size()<0xa0000000ull,
            "Area object identity range exceeded");
    // Animated-room seams were processed on the full source before partition.
    // Other Gobs repeat the pass for each NewCAurObject, even on cached models.
    if(!staticPrimary() && kind_!=AreaObjectKind::AnimatedRoom)
        model::processAsciiSkinSeams(document_->model(),resourceLoadJob());
    std::vector<const model::Model*> hierarchy;
    for(std::size_t i=0;i<document_->modelHierarchySize();++i) hierarchy.push_back(&document_->modelAt(i));
    animation_.setModel(&document_->model(),std::move(hierarchy),resourceLoadJob());
    sceneAnimation_=services.animation?services.animation:particles_.sceneAnimation();
    appearanceCombatPaused_=sceneAnimation_->combatPaused();
    particles_.setSceneAnimation(sceneAnimation_);
    references_.setSceneAnimation(sceneAnimation_);
    references_.setParentAnimationOwner(staticPrimary()?nullptr:&animationOwner_,&animation_);
    references_.setOwnerSceneMember(false);
    animationOwner_.configure(document_->model().classification,[this](float seconds,bool evaluate) {
        return advancePrimaryPose(seconds,evaluate);
    });
    if(!staticPrimary())animationOwner_.configureSphere(document_->model(),[this] {
        return engineMinimumHitSphere(document_->model(),
            engineHitPose(document_->model(),&animation_.pose()),this->frame().scale);
    });
    animationOwner_.setProcedural([this](float seconds) {
        animation_.advanceProceduralUv(particles_.runtimeRandom(),(!paused_&&frameSettings_.animate)?seconds:0.0f);
    });
    animationOwner_.setHasAnimationRuns([this]{return !animation_.empty();});
    animationOwner_.setPartAnimation([this](float seconds){advancePrimaryParts(seconds);});
    animationOwner_.setLookAt([this] {
        lookAt_.control(animation_,document_->model(),this->frame());
    });
    const auto& constructionModel=document_->model().animationSynchronizationModel
        ? *document_->model().animationSynchronizationModel : document_->model();
    animationOwner_.setDynamicLights(std::any_of(constructionModel.nodes.begin(),constructionModel.nodes.end(),
        [](const auto& node){return node.light&&(node.light->dynamicType||node.light->affectDynamic);}));
    animation_.setEventCallback([this](const model::AnimationRuntimeEvent& event) {
        // Parts retain every controller write immediately. An event in this
        // traversal must expose those local fields, not the prior frame copy.
        pose_=animation_.pose();
        if(event.hierarchyUpdated)animationOwner_.markHierarchyUpdated();
        if(event.kind==model::AnimationRuntimeEventKind::AnimationEvent) {
            particles_.dispatchEvent(event.name);
            visualState_.note(ObjectVisualStateSource::AnimationEvent,"animation event",event.name,
                lower(event.name)=="detonate");
        }
        const auto callback=animationEvent_;
        if(callback)callback(event);
    },*animationOwner_.lifetime());
    references_.setAnimationEventCallback([this](const ReferenceAnimationEvent& event) {
        if(event.event.kind==model::AnimationRuntimeEventKind::AnimationEvent)
            particles_.dispatchEvent(event.event.name,event.reference);
    });
    references_.setOwnerAnimated([this](std::size_t,float) {references_.takeAnimationEvents();});
    references_.setPartsAnimated([this](std::size_t reference,float seconds) {
        if(frameSettings_.dangly&&seconds>0) {
            model::DanglyEnvironment environment;
            environment.windDisplacement=[this](auto p,float dt){return particles_.windDisplacement(p,dt);};
            references_.simulateDanglyInstance(reference,seconds,environment);
        }
        lightsabers_.advanceReference(pose_,reference,seconds);++revision_;
    });
    references_.setSceneRandom(services.random);
    references_.setOwnerFrame(frame);
    references_.setDocument(document_.get());
    particles_.setSceneRandom(services.random);
    particles_.setSceneWind(services.wind);
    animationOwner_.bindWind(&particles_.windManager(),[this]{return this->world();},
        document_->model().game);
    particles_.setIdentityBase(static_cast<std::uint64_t>(firstInstance_)<<20u, staticPrimary());
    particles_.setReferenceScene(&references_);
    particles_.setSceneQueries(services.queries);
    particles_.deferPrewarm(true);
    particles_.setSceneMember(sceneMember);
    particles_.setDocument(document_.get());
    lightsabers_.setReferenceScene(&references_);
    lightsabers_.setDocument(document_.get());
    lightsabers_.setPrimaryAnimated(!staticPrimary());
    // NewCAurObject finishes with Animate(0) after child construction. Static
    // room Parts come from CreateInstanceTree(NULL) and have no such caller.
    if(!staticPrimary())animation_.advanceProceduralUv(particles_.runtimeRandom(),0.0f);
    if(!staticPrimary()&&sceneMember)animationOwner_.setScene(sceneAnimation_);
    references_.setOwnerSceneMember(sceneMember);
    pose_=animation_.pose();
    model::resetDanglySimulation(document_->model(),pose_,dangly_);
    lightsabers_.sampleConstructedOwners(pose_);
    references_.synchronize(pose_);
    // Construction has completed every owner/reference attachment required by
    // Scene::UpdateEmitters. The first scene render may now run the one-time
    // registration-order prewarm even if no timer tick preceded that paint.
    synchronizeParticleVisualInputs();
    particles_.prepareFrame(pose_,0.0,true);
    particles_.deferPrewarm(false);
    lightsabers_.synchronize(pose_);
    refreshAppearanceEffects();
}
AreaObject::~AreaObject() {
    animationOwner_.setScene(nullptr);references_.setOwnerSceneMember(false);
}
void AreaObject::setSceneAnimation(std::shared_ptr<SceneAnimation> scene) {
    sceneAnimation_=std::move(scene);
    if(animationOwner_.sceneMember())animationOwner_.setScene(sceneAnimation_);
    references_.setSceneAnimation(sceneAnimation_);particles_.setSceneAnimation(sceneAnimation_);
}
void AreaObject::setSceneMember(bool member) {
    if(!staticPrimary())animationOwner_.setScene(member?sceneAnimation_:nullptr);
    references_.setOwnerSceneMember(member);particles_.setSceneMember(member);
}
std::size_t AreaObject::instanceIndex(std::optional<std::size_t> reference) const {
    require(!reference || *reference<document_->references().size(),"Invalid area reference identity");
    return firstInstance_+(reference ? *reference+1u : 0u);
}
std::uint64_t AreaObject::ownerId(std::optional<std::size_t> reference) const {
    return static_cast<std::uint64_t>(instanceIndex(reference));
}
void AreaObject::setWorld(const model::Mat4& world) {
    auto frame=engineRigidFrame(world);frame.scale=references_.ownerScale();setFrame(frame);
}
void AreaObject::setFrame(const model::EngineFrame& frame) {
    require(!staticPrimary(),"Collapsed room Parts cannot be repositioned without rebuilding the room");
    references_.setOwnerFrame(frame,false); ++revision_;
}
const OwnerVisualEffects* AreaObject::ownerVisualEffects(std::optional<std::size_t> reference) const noexcept {
    const auto& effects=engineOwnerVisualInputs(reference);
    return hasOwnerEffects(effects) ? &effects : nullptr;
}
const OwnerVisualEffects& AreaObject::engineOwnerVisualInputs(std::optional<std::size_t> reference) const noexcept {
    return reference&&*reference<referenceAppearanceEffects_.size()
        ? referenceAppearanceEffects_[*reference] : ownerVisualEffects_;
}
void AreaObject::forceAnimateCreatureAppearance() {
    // SetAppearance's ForceAnimate reads GetPausedByCombat captured when
    // body/head geometry was built (K1 Desktop 0x82680, K2 Desktop 0x140146).
    if(!appearanceCombatPaused_)return;
    const auto force=[](SceneAnimationOwner& owner) {
        owner.setForceAnimated(true);owner.animateExternal(1.0f);owner.setForceAnimated(false);
    };
    force(animationOwner_);
    const auto find=[&](auto predicate) {
        for(std::size_t i=0;i<document_->references().size();++i)
            if(predicate(document_->references()[i])) {force(references_.animationOwner(i));break;}
    };
    find([](const auto& reference){return reference.attachmentKind==VisualAttachmentKind::CreatureHead;});
    find([](const auto& reference){return reference.attachmentKind==VisualAttachmentKind::EquippedItem&&
        lower(reference.parentNodeName)=="gogglehook";});
    for(const auto slot:{0x10u,0x20u})find([&](const auto& reference) {
        return reference.attachmentKind==VisualAttachmentKind::EquippedItem&&reference.equipmentSlot==slot;
    });
}
void AreaObject::initializeCreatureForceSight(std::int16_t alignment,std::uint16_t race) {

    if(document_->model().game!=model::GameVersion::Kotor2)return;
    creatureForceSight_=material::creatureForceSightOwner(alignment,race);
    refreshAppearanceEffects();++revision_;
}
void AreaObject::refreshAppearanceEffects() {
    ownerVisualEffects_.forceSight=creatureForceSight_;
    // CreateBTypeBody 0x34c8c0 / CreateHead 0x34d02f set ENVMAP on their
    // selected Gobs. Equipment Gobs do not inherit the creature's map.
    if(effectiveVisual_.environmentMap.empty())ownerVisualEffects_.environmentMap=document_->visualEnvironmentMap();
    referenceAppearanceEffects_.assign(document_->references().size(),referenceOwnerVisualEffects_);
    for(std::size_t i=0;i<referenceAppearanceEffects_.size();++i) {
        const auto& reference=document_->references()[i];
        if(effectiveVisual_.environmentMap.empty())
            referenceAppearanceEffects_[i].environmentMap=reference.appearanceEnvironmentMap;



        if(reference.attachmentKind==VisualAttachmentKind::CreatureHead)
            referenceAppearanceEffects_[i].forceSight=creatureForceSight_;
    }
}
void AreaObject::appearanceBindingsChanged() {refreshAppearanceEffects();++revision_;}
void AreaObject::synchronizeParticleVisualInputs(std::vector<ParticleEmitterTargetInput> targets) {
    ParticleObjectVisualInputs inputs;
    if(effectiveVisual_.colorShift.enabled)inputs.objectTint=effectiveVisual_.colorShift.color;
    inputs.objectAlpha=effectiveVisual_.opacity*
        (effectiveVisual_.colorShift.enabled?effectiveVisual_.colorShift.amount:1.0f);
    inputs.tileAlpha=1.0f;
    inputs.forceUnbounded=effectiveVisual_.forceEmitterRadiusUnbounded;
    inputs.targets=std::move(targets);
    std::string error;
    if(!particles_.setObjectVisualInputs(inputs,error))
        visualState_.note(ObjectVisualStateSource::LiveAdapter,"particle visual inputs",error,false);
}
bool AreaObject::setResolvedParticleTargets(std::vector<ParticleEmitterTargetInput> targets,std::string& error) {
    ParticleObjectVisualInputs inputs=particles_.objectVisualInputs();
    inputs.targets=std::move(targets);
    if(!particles_.setObjectVisualInputs(inputs,error)) {
        visualState_.note(ObjectVisualStateSource::LiveAdapter,"resolve emitter targets",error,false);return false;
    }
    return true;
}
void AreaObject::noteVisualDiagnosticOnce(std::string key,ObjectVisualStateSource source,
                                          std::string operation,std::string detail,bool applied) {
    if(!visualDiagnosticKeys_.insert(std::move(key)).second)return;
    visualState_.note(source,std::move(operation),std::move(detail),applied);
}
bool AreaObject::commitVisualState(const EffectiveObjectVisualState& previous,
                                   ObjectVisualStateSource source,std::string& error) {
    error.clear();
    effectiveVisual_=visualState_.effective();ownerVisualEffects_=effectiveVisual_.ownerEffects();
    referenceOwnerVisualEffects_=ownerVisualEffects_;
    if(!effectiveVisual_.colorShift.propagateToReferences) {
        referenceOwnerVisualEffects_.colorShiftEnabled=false;
        referenceOwnerVisualEffects_.colorShift={{1.0f,1.0f,1.0f,1.0f}};
    }
    refreshAppearanceEffects();
    synchronizeParticleVisualInputs();
    const auto key=animationKey(effectiveVisual_.animation);
    const bool animationChanged=key!=animationKey(previous.animation);
    if(animationChanged && key!=appliedAnimationKey_ && effectiveVisual_.animation) {
        const auto& command=*effectiveVisual_.animation;
        if(!command.resolved()) {
            visualState_.note(source,"apply object animation",
                command.diagnostic.empty()?"numeric animation state has no uniquely resolved model clip":command.diagnostic,false);
        } else if(staticPrimary()) {
            visualState_.note(source,"apply object animation","static room Parts have no Gob animation runtime",false);
        } else {
            const auto result=play(command.animationName,command.playbackRate,command.runFlags,command.startTime);
            if(!result) {
                visualState_.note(source,"apply object animation",result.diagnostic,false);
            } else {
                std::ostringstream out;out<<"server 0x"<<std::hex<<command.serverAnimation
                    <<" / client 0x"<<*command.clientAnimation<<" -> "<<command.animationName;
                if(command.family==ObjectAnimationFamily::Door && command.serializedState==3u)
                    out<<" (engine door selector "<<std::dec<<command.doorAnimationSelector<<')';
                visualState_.note(source,"apply object animation",out.str(),true);
                appliedAnimationKey_=key;
            }
        }
    } else if(animationChanged && !effectiveVisual_.animation && !appliedAnimationKey_.empty() && !staticPrimary()) {
        animation_.clear();pose_=animation_.pose();references_.synchronize(pose_,false);
        appliedAnimationKey_.clear();
        visualState_.note(source,"clear object animation","live animation removed and no authored animation remains",true);
    }
    ++revision_;return true;
}
bool AreaObject::applyAuthoredVisualState(const ObjectVisualStateLayer& layer,std::string& error) {
    const auto previous=effectiveVisual_;
    if(!visualState_.setAuthored(layer,error))return false;
    return commitVisualState(previous,ObjectVisualStateSource::AuthoredGit,error);
}
bool AreaObject::applyLiveVisualState(const ObjectVisualStateLayer& layer,std::string& error) {
    const auto previous=effectiveVisual_;
    if(!visualState_.setLive(layer,error))return false;
    return commitVisualState(previous,ObjectVisualStateSource::LiveAdapter,error);
}
void AreaObject::clearLiveVisualState() {
    const auto previous=effectiveVisual_;visualState_.clearLive();std::string error;
    commitVisualState(previous,ObjectVisualStateSource::LiveAdapter,error);
}
bool AreaObject::dispatchVisualEvent(const ObjectVisualEvent& event,std::string& error) {
    error.clear();
    if(event.kind==ObjectVisualEventKind::Detonate) {
        particles_.dispatchEvent("detonate");
        visualState_.note(ObjectVisualStateSource::AnimationEvent,"detonate","routed to this owner's PartEmitter tree",true);
        return true;
    }
    if(event.kind==ObjectVisualEventKind::ClearLiveOverrides) {
        clearLiveVisualState();return true;
    }
    auto layer=visualState_.live();
    switch(event.kind) {
    case ObjectVisualEventKind::Animation:
        if(!event.animation) {error="Animation event has no resolved numeric command";break;}
        layer.animation=event.animation;break;
    case ObjectVisualEventKind::RotateTexture:
        if(!std::isfinite(event.value)) {error="Texture rotation is non-finite";break;}
        layer.textureRotationDegrees=event.value;break;
    case ObjectVisualEventKind::SetEmitterTarget: {
        if(!event.target) {error="Emitter-target event has no route";break;}
        auto routes=layer.emitterTargets.value_or(effectiveVisual_.emitterTargets);
        const auto key=[&](const ObjectEmitterTargetRoute& route) {
            return lower(route.emitterName)+":"+(route.allSourceOwners?"all":route.sourceReference?std::to_string(*route.sourceReference):"invalid");
        };
        const auto wanted=key(*event.target);
        routes.erase(std::remove_if(routes.begin(),routes.end(),[&](const auto& route){return key(route)==wanted;}),routes.end());
        routes.push_back(*event.target);layer.emitterTargets=std::move(routes);break;
    }
    case ObjectVisualEventKind::ClearEmitterTargets:layer.emitterTargets=std::vector<ObjectEmitterTargetRoute>{};break;
    case ObjectVisualEventKind::SetMotionBlur:
        layer.motionBlur=event.enabled;layer.maximumBlurLength=event.secondaryValue;break;
    case ObjectVisualEventKind::SetDistortion:layer.distortion=event.enabled;break;
    case ObjectVisualEventKind::SetHologram:layer.hologram=event.enabled;break;
    case ObjectVisualEventKind::SetOpacity:layer.opacity=event.value;break;
    case ObjectVisualEventKind::SetEnvironmentMap:layer.environmentMap=event.text;break;
    case ObjectVisualEventKind::SetColorShift:layer.colorShift=event.colorShift;break;
    case ObjectVisualEventKind::SetForceEmitterRadiusUnbounded:layer.forceEmitterRadiusUnbounded=event.enabled;break;
    case ObjectVisualEventKind::Detonate:
    case ObjectVisualEventKind::ClearLiveOverrides:break;
    }
    if(!error.empty()) {
        visualState_.note(ObjectVisualStateSource::LiveAdapter,objectVisualEventName(event.kind),error,false);return false;
    }
    const bool ok=applyLiveVisualState(layer,error);
    if(ok)visualState_.note(ObjectVisualStateSource::LiveAdapter,objectVisualEventName(event.kind),"explicit visual event applied",true);
    return ok;
}
void AreaObject::rememberCreatureAnimation(const model::AnimationPlayResult& result,
    const std::string& name,float rate,std::uint32_t flags) {
    // SetAppearance 0x43eec4 saves the looping identity separately from the
    // fire-and-forget run; SetAnimation 0x45b188 reads the current owner rate.
    if(kind_!=AreaObjectKind::Creature||!result.runId||
       (flags&(model::kAnimationRunOverlay|model::kAnimationRunQueued|
               model::kAnimationRunStop|model::kAnimationRunForceStop)))return;
    if(creatureLoopAnimation_)creatureLoopAnimation_->playbackRate=rate;
    if(!(flags&model::kAnimationRunNonLooping))
        creatureLoopAnimation_=CreatureLoopAnimation{name,rate};
}
model::AnimationPlayResult AreaObject::play(const std::string& name,float rate,std::uint32_t flags,float start) {
    if(staticPrimary()) return {{},false,"Static room Parts have no Gob animation run"};
    const auto lifetime=*animationOwner_.lifetime();
    const bool creature=kind_==AreaObjectKind::Creature;
    if(creature&&!(flags&model::kAnimationRunOverlay)) {



        if(rate==0.0f)rate=1.0f;
        references_.playCreatureEquippedAnimation(name,rate,flags,start);
        if(lifetime.expired())
            throw std::logic_error("Engine Wield animation caller requires its wrapper after equipped-item callbacks");
    }
    auto result=animation_.play(name,rate,flags,start);
    if(lifetime.expired()) {
        if(creature&&result)
            throw std::logic_error("Engine HeadWield animation caller requires its wrapper after the body callback");
        return result;
    }
    rememberCreatureAnimation(result,name,rate,flags);
    if(result) {


        references_.playCreatureHeadAnimation(name,rate,flags,start);
        if(lifetime.expired())return result;
        pose_=animation_.pose(); references_.synchronize(pose_,false); ++revision_;
    }
    return result;
}
model::AnimationPlayResult AreaObject::playOverlay(const std::string& name,float rate,std::uint32_t flags,float start) {


    const auto lifetime=*animationOwner_.lifetime();
    const bool creature=kind_==AreaObjectKind::Creature;
    auto result=animation_.play(name,rate,flags,start);
    if(lifetime.expired()) {
        if(creature&&result)
            throw std::logic_error("Engine HeadWield overlay caller requires its wrapper after the body callback");
        return result;
    }
    if(result) {
        references_.playCreatureHeadAnimation(name,rate,flags,start);
        if(lifetime.expired())return result;
        pose_=animation_.pose();references_.synchronize(pose_,false);++revision_;
    }
    return result;
}
model::AnimationPlayResult AreaObject::playWorldAnimation(const std::string& name,float rate,std::uint32_t flags) {
    const auto lifetime=*animationOwner_.lifetime();
    const auto result=animation_.play(name,rate,flags,0.0f);
    if(lifetime.expired())return result;
    if(result) {pose_=animation_.pose();references_.synchronize(pose_,false);++revision_;}
    return result;
}
void AreaObject::setVisemes(const CreatureVisemes& visemes) {
    // CSWCCreature::LipSync 0x44c060 clears both targets before supplying
    // the new track; HeadWield::SetAnimationVisemes 0x48bdf0 addresses each
    // present Gob independently, including a head whose body has no talk clip.
    stopVisemes();
    playCreatureVisemes(animation_,visemes);
    references_.setCreatureHeadVisemes(visemes);
    pose_=animation_.pose();references_.synchronize(pose_,false);++revision_;
}
void AreaObject::stopVisemes() {
    stopCreatureVisemes(animation_);
    references_.stopCreatureHeadVisemes();
    pose_=animation_.pose();references_.synchronize(pose_,false);++revision_;
}
void AreaObject::beginCreatureLookAt(const std::string& sourceBone,LookAtTargetResolver target,
    float horizontal,float vertical,float transition,float maximumDistance) {
    references_.beginCreatureHeadLookAt(sourceBone,target,horizontal,vertical,transition,maximumDistance);
    lookAt_.begin(animation_,document_->model(),sourceBone,std::move(target),horizontal,vertical,transition,maximumDistance);
}
void AreaObject::endCreatureLookAt() {references_.endCreatureHeadLookAt();lookAt_.end();}

model::AnimationPlayResult AreaObject::playRoomAnimation(std::uint16_t number) {
    if(kind_!=AreaObjectKind::AnimatedRoom) return {{},false,"scriptloop requires an animated room Gob"};
    std::ostringstream name; name<<"scriptloop"<<std::setw(2)<<std::setfill('0')<<number;
    // K1 CSWCRoom::PlayAnimation 0xf49d2 issues only scriptloop; K2's
    // caller first overlays null to replace the preceding room animation.
    if(document_->model().game==model::GameVersion::Kotor2)
        play("null",1.0f,model::kAnimationRunOverlay);
    return play(name.str(),1.0f,model::kAnimationRunOverlay);
}
void AreaObject::animateExternal(float seconds) {
    animationOwner_.animateExternal(seconds);
    particles_.prepareFrame(pose_,0.0,true);
}

void AreaObject::reset() {
    lookAt_.clear();animation_.clear();appliedAnimationKey_.clear();creatureLoopAnimation_.reset();
    references_.reset();
    if(!staticPrimary()&&effectiveVisual_.animation&&effectiveVisual_.animation->resolved()) {
        const auto& command=*effectiveVisual_.animation;
        const auto result=play(command.animationName,command.playbackRate,
            command.runFlags,command.startTime);
        if(result)appliedAnimationKey_=animationKey(effectiveVisual_.animation);
        else visualState_.note(ObjectVisualStateSource::LiveAdapter,"restore object animation",
            result.diagnostic,false);
    }
    pose_=animation_.pose();
    model::resetDanglySimulation(document_->model(),pose_,dangly_);
    references_.synchronize(pose_);
    particles_.deferPrewarm(true);particles_.resetSimulation();
    synchronizeParticleVisualInputs();
    particles_.prepareFrame(pose_,0.0,true);
    particles_.deferPrewarm(false);
    lightsabers_.reset();lightsabers_.synchronize(pose_);
    elapsed_=0; ++revision_;
}
void AreaObject::advancePose(float seconds,const AreaFrameSettings& settings) {
    prepareAnimationFrame(settings);sceneAnimation_->animate(seconds);
}
bool AreaObject::advancePrimaryPose(float seconds,bool evaluate) {
    const auto primarySeconds=(!paused_&&frameSettings_.animate)?seconds:0.0f;
    const auto lifetime=*animationOwner_.lifetime();
    const bool applied=animation_.advance(primarySeconds,evaluate);
    if(lifetime.expired())return applied; // engine Gob::Animate SafePointer
    pose_=animation_.pose();
    animation_.takeEvents(); // observation only; effects ran synchronously
    elapsed_+=seconds;
    references_.synchronize(pose_,false);
    ++revision_;return applied;
}
void AreaObject::advancePrimaryParts(float seconds) {
    if(frameSettings_.dangly) {
        model::DanglyEnvironment environment;
        environment.objectFrame=frame();
        environment.windDisplacement=[this](auto p,float dt){return particles_.windDisplacement(p,dt);};
        model::simulateDanglyMeshes(document_->model(),pose_,dangly_,seconds,environment);
    }
    particles_.setDanglyEnabled(frameSettings_.dangly);
    lightsabers_.advancePrimary(pose_,seconds);lightsabers_.refreshTransforms(pose_);
}

void AreaObject::advanceEffects(float seconds,const AreaFrameSettings& settings) {
    require(std::isfinite(seconds)&&seconds>=0,"Invalid area effects elapsed time");
    seconds=std::min(seconds,0.25f);
    std::vector<bool> referenceAnimating(references_.instances().size());
    if(settings.emitters && seconds>0.0f) {
        for(std::size_t index=0;index<references_.instances().size();++index) {
            const auto& runtime=references_.instances()[index];
            // A referenced CAurObject has its own default-animation run. Its
            // Gob::IsAnimating result is independent of its parent object's run.
            referenceAnimating[index]=runtime.active && runtime.isAnimating();
        }
    }
    const bool primaryAnimating=settings.emitters && seconds>0.0f &&
        !staticPrimary() && animationOwner_.isAnimating();
    particles_.deferPrewarm(false);
    particles_.prepareFrame(pose_,seconds,settings.references,
                            primaryAnimating,referenceAnimating);
}
} // namespace neomdl
