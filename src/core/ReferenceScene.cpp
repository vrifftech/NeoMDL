#include "core/ReferenceScene.hpp"
#include "core/SceneQueries.hpp"
#include <model/AsciiModel.hpp>

#include <algorithm>
#include <cmath>

namespace neomdl {
namespace m = neomdl::model;
namespace {

bool finite(float value) noexcept { return std::isfinite(value); }

m::Vec3 subtract(m::Vec3 left, m::Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}
m::Vec3 scale(m::Vec3 value, float amount) noexcept {
    return {value.x * amount, value.y * amount, value.z * amount};
}
float dot(m::Vec3 left, m::Vec3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}
m::Vec3 cross(m::Vec3 left, m::Vec3 right) noexcept {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}
float lengthSquared(m::Vec3 value) noexcept { return dot(value, value); }
m::Vec3 normalized(m::Vec3 value, m::Vec3 fallback = {1.0f, 0.0f, 0.0f}) noexcept {
    const auto squared = static_cast<double>(lengthSquared(value));
    if (!(squared > 1.0e-20) || !std::isfinite(squared)) return fallback;
    return scale(value, static_cast<float>(1.0 / std::sqrt(squared)));
}

std::string lower(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

m::Quaternion normalized(m::Quaternion value) noexcept {
    const auto squared = static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y +
        static_cast<double>(value.z) * value.z +
        static_cast<double>(value.w) * value.w;
    if (!(squared > 1.0e-20) || !std::isfinite(squared)) return {};
    const auto reciprocal = static_cast<float>(1.0 / std::sqrt(squared));
    return {value.x * reciprocal, value.y * reciprocal,
            value.z * reciprocal, value.w * reciprocal};
}

m::Quaternion quaternionFromRigid(const m::Mat4& matrix) noexcept {
    const float m00 = matrix.values[0];
    const float m11 = matrix.values[5];
    const float m22 = matrix.values[10];
    const float trace = m00 + m11 + m22;
    m::Quaternion result;
    if (trace > 0.0f) {
        const float s = std::sqrt(std::max(0.0f, trace + 1.0f)) * 2.0f;
        if (s <= 1.0e-8f) return {};
        result.w = 0.25f * s;
        result.x = (matrix.values[6] - matrix.values[9]) / s;
        result.y = (matrix.values[8] - matrix.values[2]) / s;
        result.z = (matrix.values[1] - matrix.values[4]) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(std::max(0.0f, 1.0f + m00 - m11 - m22)) * 2.0f;
        if (s <= 1.0e-8f) return {};
        result.w = (matrix.values[6] - matrix.values[9]) / s;
        result.x = 0.25f * s;
        result.y = (matrix.values[4] + matrix.values[1]) / s;
        result.z = (matrix.values[8] + matrix.values[2]) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(std::max(0.0f, 1.0f + m11 - m00 - m22)) * 2.0f;
        if (s <= 1.0e-8f) return {};
        result.w = (matrix.values[8] - matrix.values[2]) / s;
        result.x = (matrix.values[4] + matrix.values[1]) / s;
        result.y = 0.25f * s;
        result.z = (matrix.values[9] + matrix.values[6]) / s;
    } else {
        const float s = std::sqrt(std::max(0.0f, 1.0f + m22 - m00 - m11)) * 2.0f;
        if (s <= 1.0e-8f) return {};
        result.w = (matrix.values[1] - matrix.values[4]) / s;
        result.x = (matrix.values[8] + matrix.values[2]) / s;
        result.y = (matrix.values[9] + matrix.values[6]) / s;
        result.z = 0.25f * s;
    }
    return normalized(result);
}

m::Mat4 rigidTransform(const m::Mat4& matrix) noexcept {
    const m::Vec3 translation{matrix.values[12], matrix.values[13], matrix.values[14]};
    m::Vec3 x{matrix.values[0], matrix.values[1], matrix.values[2]};
    m::Vec3 y{matrix.values[4], matrix.values[5], matrix.values[6]};
    const m::Vec3 sourceZ{matrix.values[8], matrix.values[9], matrix.values[10]};
    x = normalized(x);
    y = subtract(y, scale(x, dot(x, y)));
    if (lengthSquared(y) <= 1.0e-12f) {
        y = cross(sourceZ, x);
        if (lengthSquared(y) <= 1.0e-12f)
            y = cross(std::fabs(x.z) < 0.9f ? m::Vec3{0.0f, 0.0f, 1.0f}
                                             : m::Vec3{0.0f, 1.0f, 0.0f}, x);
    }
    y = normalized(y, {0.0f, 1.0f, 0.0f});
    m::Vec3 z = normalized(cross(x, y), {0.0f, 0.0f, 1.0f});
    if (dot(z, sourceZ) < 0.0f) {
        y = scale(y, -1.0f);
        z = scale(z, -1.0f);
    }
    m::Mat4 result = m::identityMatrix();
    result.values[0] = x.x; result.values[1] = x.y; result.values[2] = x.z;
    result.values[4] = y.x; result.values[5] = y.y; result.values[6] = y.z;
    result.values[8] = z.x; result.values[9] = z.y; result.values[10] = z.z;
    result.values[12] = translation.x;
    result.values[13] = translation.y;
    result.values[14] = translation.z;
    return result;
}

void extend(m::Bounds& bounds, m::Vec3 point) noexcept {
    if (!finite(point.x) || !finite(point.y) || !finite(point.z)) return;
    if (!bounds.valid) {
        bounds.minimum = point;
        bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

void extendTransformed(m::Bounds& destination, const m::Bounds& source,
                       const m::Mat4& transform) noexcept {
    if (!source.valid) return;
    for (unsigned bits = 0u; bits < 8u; ++bits) {
        const m::Vec3 corner{
            (bits & 1u) != 0u ? source.maximum.x : source.minimum.x,
            (bits & 2u) != 0u ? source.maximum.y : source.minimum.y,
            (bits & 4u) != 0u ? source.maximum.z : source.minimum.z};
        extend(destination, m::transformPoint(transform, corner));
    }
}

} // namespace

void ReferenceScene::setSceneAnimation(std::shared_ptr<SceneAnimation> scene) {
    sceneAnimation_=std::move(scene);
    for(const auto index:sceneRegistrationOrder_)
        instances_[index].animationOwner.setScene(sceneAnimation_);
}
void ReferenceScene::setOwnerSceneMember(bool member) {
    if(ownerSceneMember_==member)return;
    ownerSceneMember_=member;
    std::vector<std::size_t> roots;
    for(std::size_t index=0;index<instances_.size();++index)
        if(instances_[index].attached&&!instances_[index].targetReference)roots.push_back(index);
    std::sort(roots.begin(),roots.end(),[&](auto a,auto b) {
        return instances_[a].attachmentOrder>instances_[b].attachmentOrder;
    });
    for(const auto index:roots)setInstanceSceneMember(index,member);
    // UpdateScenesAttach only forwards scene membership. Position/orientation
    // remain retained until attachment Control or explicit reattachment.
    if(!engineRuntime_)resolveWorlds();
}

void ReferenceScene::setDocument(const ModelDocument* document) {
    document_ = document;
    resetInstances(1u,true);
}

const RuntimeReferenceInstance* ReferenceScene::instance(std::size_t index) const noexcept {
    return index < instances_.size() ? &instances_[index] : nullptr;
}

void ReferenceScene::reset(std::uint32_t randomSeed) {
    resetInstances(randomSeed,false);
}

void ReferenceScene::resetInstances(std::uint32_t randomSeed, bool constructOwners) {
    resourceLoadJob().check();
    instances_.clear();
    children_.clear();
    mainPose_ = {};
    mainFrame_ = ownerFrame_;
    mainWorld_ = m::identityMatrix();
    diagnostics_.clear();
    animationEvents_.clear();
    sceneRegistrationOrder_.clear();
    nextAttachmentOrder_=1u;
    nextSceneRegistration_=1u;
    engineRuntime_ = document_ && document_->isOpen() &&
        m::engineAnimationRuntimeSupported(document_->model().game);
    ++revision_;
    if (!document_ || !document_->isOpen()) return;

    const auto& definitions = document_->references();
    instances_.resize(definitions.size());
    children_.resize(definitions.size());
    for (std::size_t index = 0u; index < definitions.size(); ++index) {
        resourceLoadJob().check();
        instances_[index].definitionIndex = index;
        instances_[index].pose = m::bindPose(definitions[index].model, resourceLoadJob());
        m::resetDanglySimulation(definitions[index].model,instances_[index].pose,
                                 instances_[index].danglyState);
        instances_[index].world = m::identityMatrix();
        instances_[index].scale = ownerScale_*definitions[index].visualScale;
        instances_[index].frame.scale = instances_[index].scale;
        setAuthoredAttachment(index);
        if (definitions[index].parentInstance &&
            *definitions[index].parentInstance < children_.size()) {
            children_[*definitions[index].parentInstance].push_back(index);
        }
    }

    if (engineRuntime_) {
        ownedRandom_.state = randomSeed;
        auto& random = sceneRandom_ ? *sceneRandom_ : ownedRandom_;
        std::vector<std::uint8_t> state(definitions.size(), 0u);
        for (std::size_t index = 0u; index < definitions.size(); ++index) {
            if (!definitions[index].parentInstance)
                initializeInstance(index, random, state, constructOwners);
        }
        for (std::size_t index = 0u; index < definitions.size(); ++index)
            initializeInstance(index, random, state, constructOwners);
    } else {
        // Unsupported model games retain static reference inspection.
        for (auto& runtime : instances_) runtime.active = true;
    }

    if (engineRuntime_) {
        // SetScene adds the owner, then broadcasts UpdateScenes to attachment
        // listeners in reverse registration order; each child repeats that
        // operation for its own dependents. Scene::Animate walks the resulting
        // Gob list backwards. This is independent of resource/definition order.
        for (auto& runtime:instances_) runtime.sceneMember=false;
        std::vector<std::size_t> roots;
        for (std::size_t index=0;index<instances_.size();++index)
            if (instances_[index].attached && !instances_[index].targetReference)
                roots.push_back(index);
        std::sort(roots.begin(),roots.end(),[&](auto left,auto right) {
            return instances_[left].attachmentOrder>instances_[right].attachmentOrder;
        });
        for (const auto root:roots) setInstanceSceneMember(root,ownerSceneMember_);
    }

    // NewCAurObject attaches references as part of object construction. Keep a
    // valid bind-pose attachment chain even before the first viewer animation
    // evaluation so specialized child parts cannot observe identity placeholders.
    captureMainPose(parentAnimationRuntime_ ? parentAnimationRuntime_->pose()
        : m::bindPose(document_->model(), resourceLoadJob()));
    resolveWorlds();
}

void ReferenceScene::initializeInstance(std::size_t index,
                                        m::ParticleRandom& random,
                                        std::vector<std::uint8_t>& state,
                                        bool constructOwners) {
    resourceLoadJob().check();
    if (index >= instances_.size() || state[index] == 2u) return;
    if (state[index] == 1u) {
        diagnostics_.push_back("Reference ownership cycle prevented runtime construction at instance " +
                               std::to_string(index) + ".");
        return;
    }
    state[index] = 1u;
    auto& runtime = instances_[index];
    const auto& definition=document_->references()[index];
    // CreateReferenceObjects calls NewCAurObject for every child, including
    // repeated cached assets. Inspection rewind retains those engine owners.
    if(constructOwners) m::processAsciiSkinSeams(definition.model,resourceLoadJob());
    std::vector<const m::Model*> hierarchy{&definition.model};
    for(const auto& supermodel:definition.supermodels) hierarchy.push_back(&supermodel);
    runtime.animationRuntime.emplace();
    runtime.animationRuntime->setModel(&definition.model,std::move(hierarchy),resourceLoadJob());
    runtime.animationRuntime->setEventCallback([this,index](const m::AnimationRuntimeEvent& event) {
        auto& owner=instances_[index];
        owner.pose=owner.animationRuntime->pose();
        owner.animationTime=owner.animationRuntime->primaryTime();
        if(event.hierarchyUpdated)owner.animationOwner.markHierarchyUpdated();
        ReferenceAnimationEvent observed{index,event};
        animationEvents_.push_back(observed);
        const auto callback=animationEvent_;
        if(callback)callback(observed);
    },*runtime.animationOwner.lifetime());
    runtime.animationOwner.configure(definition.model.classification,[this,index](float seconds,bool evaluate) {
        auto& random=sceneRandom_?*sceneRandom_:ownedRandom_;
        return advanceInstance(index,seconds,evaluate,random);
    });
    runtime.animationOwner.configureSphere(definition.model,[this,index] {
        const auto& runtime=instances_[index];
        const auto& asset=document_->references()[index].model;
        return engineMinimumHitSphere(asset,engineHitPose(asset,&runtime.animationRuntime->pose()),runtime.scale);
    });
    runtime.animationOwner.setProcedural([this,index](float seconds) {
        instances_[index].animationRuntime->advanceProceduralUv(sceneRandom_?*sceneRandom_:ownedRandom_,seconds);
    });
    runtime.animationOwner.setHasAnimationRuns([this,index]{return !instances_[index].animationRuntime->empty();});
    runtime.animationOwner.setPartAnimation([this,index](float seconds) {
        if(partsAnimated_)partsAnimated_(index,seconds);
    });
    runtime.animationOwner.setLookAt([this,index] {
        auto& runtime=instances_[index];
        runtime.lookAt.control(*runtime.animationRuntime,document_->references()[index].model,runtime.frame);
    });
    bindAttachmentControl(index);
    runtime.animationOwner.setDynamicLights(std::any_of(definition.model.nodes.begin(),definition.model.nodes.end(),
        [](const auto& node){return node.light&&(node.light->dynamicType||node.light->affectDynamic);}));
    for (const auto child : children_[index])
        initializeInstance(child, random, state, constructOwners);
    runtime.attachmentOrder=nextAttachmentOrder_++;
    runtime.scaleAttachmentOrder=runtime.attachmentOrder;



    runtime.animationRuntime->advanceProceduralUv(random,0.0f);
    if(definition.visualAttachment) {


        if(definition.attachmentKind==VisualAttachmentKind::EquippedItem &&
           lower(definition.parentNodeName)=="gogglehook")
            runtime.animationRuntime->play("default");
        runtime.pose=runtime.animationRuntime->pose();
        runtime.animationTime=runtime.animationRuntime->primaryTime();
        m::resetDanglySimulation(document_->references()[index].model,runtime.pose,runtime.danglyState);
        runtime.active=true;state[index]=2u;return;
    }
    runtime.defaultAnimation = document_->findReferenceAnimation(index, "default");
    const auto speedRandom = static_cast<int>(random.next() % 100u);
    const auto timeRandom = random.next() % 100u;
    runtime.playbackRate = 1.0f + static_cast<float>(speedRandom - 50) / 1000.0f;
    runtime.initialAnimationTime = static_cast<float>(timeRandom) / 100.0f;
    runtime.animationTime = runtime.initialAnimationTime;
    // K1 Desktop CreateReferenceObjects 0x32f7a7..0x32f844 and K2 Desktop
    // 0xaf606..0xaf697 issue the same command after child construction.
    runtime.animationRuntime->play("default", runtime.playbackRate, 0u,
                                   runtime.initialAnimationTime);
    runtime.pose = runtime.animationRuntime->pose();
    runtime.animationTime = runtime.animationRuntime->primaryTime();
    m::resetDanglySimulation(document_->references()[index].model,runtime.pose,
                             runtime.danglyState);
    runtime.active = true;
    state[index] = 2u;
}

void ReferenceScene::bindAttachmentControl(std::size_t index) {
    auto& owner=instances_[index].animationOwner;
    if(!instances_[index].attached) {owner.setControl({});return;}
    const auto lifetime=*owner.lifetime();
    const auto revision=owner.controlRevision()+1;
    owner.setControl([this,index,lifetime,revision](float seconds) {
        const auto target=instances_[index].targetReference;
        auto* dependency=target?&instances_[*target].animationOwner:parentAnimationOwner_;
        const auto targetLifetime=dependency?dependency->lifetime():SceneOwnerLifetime{};
        if(SceneAnimation::dependenciesAnimating()&&dependency)dependency->animate(seconds);
        // CAurBehavior::Control returns false when its Gob was destroyed or
        // changed behavior during a dependency's synchronous event callback.
        if(lifetime.expired())return;
        auto& current=instances_[index];
        if(current.animationOwner.controlRevision()!=revision)return;
        if(targetLifetime&&targetLifetime->expired()) {
            current.attached=false;current.animationOwner.setControl({});return;
        }
        const auto reference=current.targetReference;
        const auto& model=reference?document_->references()[*reference].model:document_->model();
        const auto& pose=reference?instances_[*reference].animationRuntime->pose():parentAnimationRuntime_->pose();
        const auto& owner=reference?instances_[*reference].frame:mainFrame_;
        const auto scale=reference?instances_[*reference].scale:ownerScale_;
        const auto& definition=document_->references()[index];
        current.scale=definition.visualAttachment?ownerScale_*definition.visualScale:scale;
        // Part::GetWorld reads current local fields; an event may invoke this
        // Control before the parent completes its frame's pose matrices.
        // Only this behavior's Gob receives the resulting frame.
        current.frame=m::enginePartWorld(model,pose,owner,current.targetNodeIndex);
        current.frame.scale=current.scale;
        current.world=m::nodeTransform(current.frame.position,current.frame.orientation);
    });
}

void ReferenceScene::setAuthoredAttachment(std::size_t index) {
    if (!document_ || index >= instances_.size() ||
        index >= document_->references().size()) return;
    const auto& definition = document_->references()[index];
    auto& runtime = instances_[index];
    runtime.targetReference = definition.parentInstance;
    runtime.targetNodeIndex = definition.parentNodeIndex;
    runtime.targetNodeName = definition.parentNodeName;
    // LoadHead ignores AttachToObject's missing-hook return. The detached
    // Gob retains its constructor frame; no alternate attachment is selected.
    runtime.attached = !definition.visualAttachmentDetached;
    if(definition.visualAttachmentDetached)runtime.sceneMember=false;
    runtime.reattached = false;
}

void ReferenceScene::setInstanceSceneMember(std::size_t index, bool member) {
    auto& runtime=instances_[index];
    if (runtime.sceneMember==member) return;
    if (runtime.sceneMember) {
        sceneRegistrationOrder_.erase(std::remove(sceneRegistrationOrder_.begin(),
            sceneRegistrationOrder_.end(),index),sceneRegistrationOrder_.end());
    }
    runtime.sceneMember=member;
    runtime.active=member;
    runtime.sceneRegistration=member ? nextSceneRegistration_++ : 0u;
    runtime.animationOwner.setScene(member?sceneAnimation_:nullptr);
    if (member) sceneRegistrationOrder_.push_back(index);
    std::vector<std::size_t> dependents;
    for (std::size_t child=0;child<instances_.size();++child)
        if (instances_[child].attached && instances_[child].targetReference==index)
            dependents.push_back(child);
    std::sort(dependents.begin(),dependents.end(),[&](auto left,auto right) {
        return instances_[left].attachmentOrder>instances_[right].attachmentOrder;
    });
    for (const auto child:dependents) setInstanceSceneMember(child,member);
}

void ReferenceScene::captureMainPose(const m::Pose& pose) {
    mainPose_.positions=pose.positions;mainPose_.orientations=pose.orientations;
    mainPose_.scales=pose.scales;mainPose_.worldTransforms=pose.worldTransforms;
}

void ReferenceScene::synchronize(const m::Pose& mainPose,bool resolveAttachments) {
    captureMainPose(mainPose);
    if(resolveAttachments)resolveWorlds();
    ++revision_;
}

bool ReferenceScene::advanceInstance(std::size_t index,float seconds,bool evaluate,m::ParticleRandom&) {
    auto& runtime=instances_[index];
    const auto lifetime=*runtime.animationOwner.lifetime();
    const bool applied=runtime.animationRuntime->advance(seconds,evaluate);
    if(lifetime.expired())return applied;
    runtime.pose=runtime.animationRuntime->pose();
    runtime.animationTime=runtime.animationRuntime->primaryTime();
    runtime.animationRuntime->takeEvents(); // diagnostic queue only
    ++revision_;
    if(ownerAnimated_)ownerAnimated_(index,seconds);
    return applied;
}
void ReferenceScene::advance(const m::Pose& mainPose,float elapsedSeconds,m::ParticleRandom& random,
                            const std::function<void(std::size_t)>& ownerAnimated) {
    // Explicit scoped inspection/replay. Ordinary scenes dispatch these same
    // registered owners through their shared SceneAnimation coordinator.
    for(auto registered=sceneRegistrationOrder_.rbegin();registered!=sceneRegistrationOrder_.rend();++registered) {
        const auto index=*registered;
        instances_[index].animationRuntime->advanceProceduralUv(random,elapsedSeconds);
        const auto lifetime=*instances_[index].animationOwner.lifetime();
        advanceInstance(index,elapsedSeconds,true,random);
        if(lifetime.expired())return;
        if(ownerAnimated)ownerAnimated(index);
        if(lifetime.expired())return;
    }
    captureMainPose(mainPose);resolveWorlds();++revision_;
}

std::vector<ReferenceAnimationEvent> ReferenceScene::takeAnimationEvents() {
    auto result=std::move(animationEvents_);
    animationEvents_.clear();
    return result;
}

void ReferenceScene::playCreatureEquippedAnimation(const std::string& name,float rate,
                                                    std::uint32_t flags,float start) {




    if(rate==0.0f)rate=1.0f;
    // Engine visits left then right, excluding only left ItemType 29
    // (0x38a4d3). GoggleHook headwear is outside this forwarding path.
    constexpr std::uint32_t slots[]{0x20u,0x10u};
    for(const auto slot:slots)
        for(std::size_t index=0;index<instances_.size();++index) {
            const auto& definition=document_->references()[index];
            if(definition.attachmentKind!=VisualAttachmentKind::EquippedItem ||
               definition.equipmentSlot!=slot ||
               (slot==0x20u&&definition.equipmentItemType==29u))continue;
            auto& runtime=instances_[index];
            const auto lifetime=*runtime.animationOwner.lifetime();
            runtime.animationRuntime->play(name,rate,flags,start);
            if(lifetime.expired())return;
            runtime.pose=runtime.animationRuntime->pose();
            runtime.animationTime=runtime.animationRuntime->primaryTime();
        }
    ++revision_;
}

void ReferenceScene::playCreatureHeadAnimation(const std::string& name,float rate,
                                                std::uint32_t flags,float start) {


    for(std::size_t index=0;index<instances_.size();++index)
        if(document_->references()[index].attachmentKind==VisualAttachmentKind::CreatureHead) {
            auto& runtime=instances_[index];
            const auto lifetime=*runtime.animationOwner.lifetime();
            runtime.animationRuntime->play(name,rate,flags,start);
            if(lifetime.expired())return;
            runtime.pose=runtime.animationRuntime->pose();
            runtime.animationTime=runtime.animationRuntime->primaryTime();
        }
    ++revision_;
}

void ReferenceScene::setCreatureHeadVisemes(const CreatureVisemes& visemes) {


    for(std::size_t index=0;index<instances_.size();++index)
        if(document_->references()[index].attachmentKind==VisualAttachmentKind::CreatureHead) {
            auto& runtime=instances_[index];
            const auto lifetime=*runtime.animationOwner.lifetime();
            playCreatureVisemes(*runtime.animationRuntime,visemes);
            if(lifetime.expired())return;
            runtime.pose=runtime.animationRuntime->pose();
            runtime.animationTime=runtime.animationRuntime->primaryTime();
        }
    ++revision_;
}

void ReferenceScene::stopCreatureHeadVisemes() {
    for(std::size_t index=0;index<instances_.size();++index)
        if(document_->references()[index].attachmentKind==VisualAttachmentKind::CreatureHead) {
            auto& runtime=instances_[index];
            const auto lifetime=*runtime.animationOwner.lifetime();
            stopCreatureVisemes(*runtime.animationRuntime);
            if(lifetime.expired())return;
            runtime.pose=runtime.animationRuntime->pose();
            runtime.animationTime=runtime.animationRuntime->primaryTime();
        }
    ++revision_;
}

void ReferenceScene::beginCreatureHeadLookAt(const std::string& sourceBone,LookAtTargetResolver target,
    float horizontal,float vertical,float transition,float maximumDistance) {
    for(std::size_t index=0;index<instances_.size();++index)
        if(document_->references()[index].attachmentKind==VisualAttachmentKind::CreatureHead) {
            auto& runtime=instances_[index];
            runtime.lookAt.begin(*runtime.animationRuntime,document_->references()[index].model,
                sourceBone,target,horizontal,vertical,transition,maximumDistance);
        }
}
void ReferenceScene::endCreatureHeadLookAt() {
    for(std::size_t index=0;index<instances_.size();++index)
        if(document_->references()[index].attachmentKind==VisualAttachmentKind::CreatureHead)
            instances_[index].lookAt.end();
}

void ReferenceScene::resetDangly() {
    if (!document_) return;
    for (std::size_t index = 0u; index < instances_.size() &&
         index < document_->references().size(); ++index) {
        auto& runtime = instances_[index];
        if(runtime.animationRuntime) {
            runtime.pose=runtime.animationRuntime->pose();
            runtime.animationTime=runtime.animationRuntime->primaryTime();
        } else if (runtime.defaultAnimation) {
            runtime.pose = document_->evaluateReferenceAnimation(
                index, *runtime.defaultAnimation, runtime.animationTime, true);
        } else {
            runtime.pose = m::bindPose(document_->references()[index].model);
        }
        m::resetDanglySimulation(document_->references()[index].model, runtime.pose,
                                 runtime.danglyState);
    }
    resolveWorlds();
    ++revision_;
}

void ReferenceScene::simulateDangly(
    float elapsedSeconds, const m::DanglyEnvironment& environment) {
    // The K1/K2 PartDanglyMesh lifecycle is independent of the
    // reference default-animation initialization retained above. Both own a
    // separate position/velocity pool driven by the desktop model kernel.
    if (!document_) return;
    for(std::size_t index=0u;index<instances_.size() &&
        index<document_->references().size();++index) {
        auto& runtime=instances_[index];
        if(!runtime.active) continue;
        auto instanceEnvironment=environment;
        instanceEnvironment.objectFrame=runtime.frame;
        m::simulateDanglyMeshes(document_->references()[index].model,runtime.pose,
                                runtime.danglyState,elapsedSeconds,instanceEnvironment);
    }
    ++revision_;
}

void ReferenceScene::simulateDanglyInstance(std::size_t index,float seconds,const m::DanglyEnvironment& environment) {
    auto& runtime=instances_[index];
    auto instanceEnvironment=environment;
    instanceEnvironment.objectFrame=runtime.frame;
    m::simulateDanglyMeshes(document_->references()[index].model,runtime.pose,runtime.danglyState,seconds,instanceEnvironment);
    ++revision_;
}

void ReferenceScene::setOwnerFrame(const m::EngineFrame& frame,bool resolveAttachments) {
    // Gob::SetOrientation copies all four components verbatim in every packed;
    // matrices are derived views and are never the engine orientation source.
    ownerFrame_=frame;ownerScale_=frame.scale;
    ownerWorld_=m::nodeTransform(frame.position,frame.orientation);
    mainFrame_=ownerFrame_;
    mainWorld_=m::nodeTransform(mainFrame_.position,mainFrame_.orientation);
    if(resolveAttachments)resolveWorlds();
    ++revision_;
}

void ReferenceScene::setOwnerWorld(const m::Mat4& world,bool resolveAttachments) {
    // Preserve the explicit matrix inspection adapter. Engine owner callers
    // use setOwnerFrame and never pass through this decomposition.
    if(!std::all_of(world.values.begin(),world.values.end(),
        [](float value){return std::isfinite(value);}))
        throw m::ModelError("Non-finite reference owner transform");
    const auto rigid=rigidTransform(world);
    setOwnerFrame({m::transformPoint(rigid,{}),quaternionFromRigid(rigid),ownerScale_},resolveAttachments);
}

void ReferenceScene::setOwnerScale(float scale) {
    ownerScale_=scale;ownerFrame_.scale=scale;mainFrame_.scale=scale;
    if(engineRuntime_)propagateAttachedScales(std::nullopt,scale);
    else resolveWorlds();
    ++revision_;
}

model::EngineFrame ReferenceScene::enginePartFrame(std::optional<std::size_t> reference,
                                                  std::size_t node) const {
    if(reference) {
        const auto& runtime=instances_[*reference];
        return m::enginePartWorld(document_->references()[*reference].model,
            runtime.animationRuntime->pose(),runtime.frame,node);
    }
    return m::enginePartWorld(document_->model(),
        parentAnimationRuntime_?parentAnimationRuntime_->pose():mainPose_,mainFrame_,node);
}

void ReferenceScene::propagateAttachedScales(std::optional<std::size_t> parent,float scale) {



    std::vector<std::size_t> attached;
    for(std::size_t index=0;index<instances_.size();++index)
        if(instances_[index].attached && instances_[index].targetReference==parent)
            attached.push_back(index);
    std::sort(attached.begin(),attached.end(),[&](auto left,auto right) {
        return instances_[left].scaleAttachmentOrder<instances_[right].scaleAttachmentOrder;
    });
    for(const auto index:attached) {
        auto& runtime=instances_[index];
        const auto& definition=document_->references()[index];
        // Appearance assembly records cumulative visual scale; ordinary
        // PartReference objects retain constructor local scale one.
        runtime.scale=definition.visualAttachment?ownerScale_*definition.visualScale:scale;
        runtime.frame.scale=runtime.scale;
        propagateAttachedScales(index,runtime.scale);
    }
}

void ReferenceScene::resolveWorlds() {
    mainFrame_=ownerFrame_;
    mainWorld_=m::nodeTransform(mainFrame_.position,mainFrame_.orientation);
    std::vector<std::uint8_t> state(instances_.size(), 0u);
    if (!engineRuntime_)
        for(std::size_t index=0;index<instances_.size();++index)
            instances_[index].scale=ownerScale_*document_->references()[index].visualScale;
    for (std::size_t index = 0u; index < instances_.size(); ++index)
        resolveWorld(index, state);
}

bool ReferenceScene::resolveWorld(std::size_t index,
                                  std::vector<std::uint8_t>& state,bool liveTargets) {
    if (index >= instances_.size()) return false;
    if (state[index] == 2u) return instances_[index].active || !instances_[index].sceneMember;
    if (state[index] == 1u) {
        instances_[index].attached = false;
        diagnostics_.push_back("Reference reattachment cycle detached instance " +
                               std::to_string(index) + ".");
        state[index] = 2u;
        return instances_[index].active;
    }
    state[index] = 1u;
    auto& runtime = instances_[index];
    if (!runtime.attached) {



        runtime.active = runtime.sceneMember;
        state[index] = 2u;
        return true;
    }

    const m::Pose* targetPose{};
    m::Mat4 targetWorld = mainWorld_;
    m::EngineFrame targetFrame=mainFrame_;
    const m::Model* targetModel{};
    bool targetSceneMember=ownerSceneMember_;
    if (runtime.targetReference) {
        const auto target = *runtime.targetReference;
        if (target >= instances_.size() || target == index || !resolveWorld(target, state)) {
            runtime.attached = false;
            runtime.active = true;
            diagnostics_.push_back("Reference instance " + std::to_string(index) +
                                   " lost an invalid object attachment.");
            state[index] = 2u;
            return true;
        }
        targetPose = liveTargets ? &instances_[target].animationRuntime->pose()
                                 : &instances_[target].pose;
        targetWorld = instances_[target].world;
        targetFrame = instances_[target].frame;
        targetModel = &document_->references()[target].model;
        targetSceneMember=instances_[target].sceneMember;
    } else {
        targetModel = &document_->model();
        if(liveTargets && parentAnimationRuntime_)targetPose=&parentAnimationRuntime_->pose();
    }

    const auto& transforms = targetPose ? targetPose->worldTransforms : mainPose_.worldTransforms;
    if (!targetModel || runtime.targetNodeIndex >= targetModel->nodes.size() ||
        runtime.targetNodeIndex >= transforms.size()) {
        runtime.attached = false;
        runtime.active = true;
        diagnostics_.push_back("Reference instance " + std::to_string(index) +
                               " lost an invalid part attachment.");
        state[index] = 2u;
        return true;
    }




    const auto targetScale=runtime.targetReference
        ? instances_[*runtime.targetReference].scale : ownerScale_;
    if (engineRuntime_) {
        // Ordinary PartReference Gobs retain constructor local scale one.
        // Reattachment inherits the actual target's effective scale, while a
        // missing-hook detach keeps the previous scale above. Appearance
        // assembly already supplies cumulative visual scale for its root Gobs.
        runtime.scale=document_->references()[index].visualAttachment
            ? ownerScale_*document_->references()[index].visualScale : targetScale;
    }
    if(engineRuntime_) {
        runtime.frame=m::enginePartWorld(*targetModel,targetPose?*targetPose:mainPose_,
            targetFrame,runtime.targetNodeIndex);
        runtime.frame.scale=runtime.scale;
        runtime.world=m::nodeTransform(runtime.frame.position,runtime.frame.orientation);
    } else {
        auto hook=transforms[runtime.targetNodeIndex];
        hook.values[12]*=targetScale;hook.values[13]*=targetScale;hook.values[14]*=targetScale;
        runtime.world=m::multiply(targetWorld,hook);
        runtime.frame={m::transformPoint(runtime.world,{}),quaternionFromRigid(rigidTransform(runtime.world)),runtime.scale};
    }
    runtime.sceneMember=targetSceneMember;
    runtime.active = runtime.sceneMember;
    state[index] = 2u;
    return true;
}

std::optional<std::size_t> ReferenceScene::findNode(
    const ReferenceObjectTarget& target) const noexcept {
    if (!document_) return std::nullopt;
    const m::Model* model{};
    if (target.reference) {
        if (*target.reference >= document_->references().size()) return std::nullopt;
        model = &document_->references()[*target.reference].model;
    } else {
        model = &document_->model();
    }
    const auto requested = lower(target.nodeName);
    if (requested.empty()) return std::nullopt;
    for (std::size_t index = 0u; index < model->nodes.size(); ++index) {
        if (lower(model->nodes[index].name) == requested) return index;
    }
    return std::nullopt;
}

ReferenceReattachResult ReferenceScene::reattachOwnedReferences(
    std::optional<std::size_t> ownerReference,
    std::optional<ReferenceObjectTarget> target) {
    ReferenceReattachResult result;
    if (!document_ || !engineRuntime_) return result;
    if (ownerReference && *ownerReference >= instances_.size()) return result;

    std::optional<std::size_t> targetNode;
    if (target) targetNode = findNode(*target);
    const auto& ownerModel=ownerReference
        ? document_->references()[*ownerReference].model : document_->model();
    // ReattachReference walks the owner's Part tree breadth-first, unlike
    // construction's depth-first walk. K1 0x32fcb5..0x32ff20 and K2 Desktop
    // 0xaff16..0xb0246 attach and turn each reference before the next Part.
    std::vector<std::size_t> parts;
    for (std::size_t node=0;node<ownerModel.nodes.size();++node)
        if (!ownerModel.nodes[node].parent) parts.push_back(node);
    for (std::size_t cursor=0;cursor<parts.size();++cursor) {
        const auto node=parts[cursor];
        for (std::size_t index=0;index<instances_.size();++index) {
            const auto& definition=document_->references()[index];
            if (definition.parentInstance!=ownerReference ||
                definition.parentNodeIndex!=node || !definition.reattachable) continue;
            ++result.eligible;
            auto& runtime=instances_[index];
            const bool wasAttached=runtime.attached;
            const auto previousTarget=runtime.targetReference;
            if (!target) {
                setInstanceSceneMember(index,false);
                setAuthoredAttachment(index);
                runtime.attachmentOrder=nextAttachmentOrder_++;
                setInstanceSceneMember(index,runtime.targetReference
                    ? instances_[*runtime.targetReference].sceneMember : ownerSceneMember_);
                ++result.attached;
            } else if (targetNode) {
                setInstanceSceneMember(index,false);
                runtime.targetReference=target->reference;
                runtime.targetNodeIndex=*targetNode;
                runtime.targetNodeName=target->nodeName;
                runtime.attached=true;
                runtime.reattached=true;
                runtime.attachmentOrder=nextAttachmentOrder_++;
                setInstanceSceneMember(index,runtime.targetReference
                    ? instances_[*runtime.targetReference].sceneMember : ownerSceneMember_);
                ++result.attached;
            } else {
                // A failed FindPart clears the attachment behavior. The Gob
                // retains its last world frame, scene membership and clock.
                runtime.targetReference=target->reference;
                runtime.targetNodeName=target->nodeName;
                runtime.attached=false;
                runtime.reattached=true;
                ++result.detached;
            }
            // AddAttachment deduplicates before appending. Replacing a
            // behavior on the same target refreshes event registration, but
            // does not move this Gob in that target's scale-propagation list.
            if(runtime.attached && (!wasAttached || previousTarget!=runtime.targetReference))
                runtime.scaleAttachmentOrder=runtime.attachmentOrder;
            bindAttachmentControl(index);
            // AttachToObject reads the target's retained Gob frame/current
            // Part fields directly; it does not run the target's attachment
            // controller (K2 Desktop 0xaf0be..0xaf0d7). In particular, retain a
            // facing change made for an earlier sibling in this traversal.
            std::vector<std::uint8_t> state(instances_.size(),2u);
            state[index]=0u;
            resolveWorld(index,state,true);
            turnOwnerToward(ownerReference,index);
        }
        for (const auto child:ownerModel.nodes[node].children) parts.push_back(child);
    }
    if (result.eligible != 0u) ++revision_;
    return result;
}

void ReferenceScene::turnOwnerToward(std::optional<std::size_t> ownerReference,
                                     std::size_t childReference) {
    if (childReference >= instances_.size()) return;
    if(ownerReference && *ownerReference>=instances_.size())return;
    auto& ownerFrame=ownerReference?instances_[*ownerReference].frame:mainFrame_;
    const auto direction=subtract(instances_[childReference].frame.position,ownerFrame.position);
    // ReattachReference reads Quaternion::zdir, without the Quaternion*Vector
    // w==+/-1 shortcut (K2 Desktop 0xaffe5..0xb0054).
    const auto forward=m::engineQuaternionAxis(ownerFrame.orientation,2u);
    const auto arc=engineRotationArc(forward,direction);
    const auto corrected=m::engineQuaternionProduct(arc,ownerFrame.orientation);
    ownerFrame.orientation=corrected;
    if(ownerReference) {
        instances_[*ownerReference].world=m::nodeTransform(ownerFrame.position,corrected);
    } else {
        ownerFrame_.orientation=corrected;
        ownerWorld_=m::nodeTransform(ownerFrame_.position,corrected);
        mainWorld_=ownerWorld_;
    }
}

m::Bounds ReferenceScene::bounds() const noexcept {
    m::Bounds result;
    if (!document_) return result;
    for (std::size_t index = 0u; index < instances_.size(); ++index) {
        if (!instances_[index].active ||
            index >= document_->references().size()) continue;
        auto drawnWorld=instances_[index].world;
        const float scale=instances_[index].scale;
        for(std::size_t column=0;column<3;++column)
            for(std::size_t row=0;row<3;++row)drawnWorld.values[column*4+row]*=scale;
        extendTransformed(result, instances_[index].pose.bounds,drawnWorld);
    }
    return result;
}

} // namespace neomdl
