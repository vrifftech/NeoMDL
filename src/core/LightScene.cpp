#include "core/LightScene.hpp"
#include "core/ReferenceScene.hpp"
#include "core/AreaScene.hpp"
#include "core/ParticleScene.hpp"

#include <model/Lighting.hpp>

#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace neomdl {
namespace {

using neomdl::model::Mat4;
using neomdl::model::Model;
using neomdl::model::Node;
using neomdl::model::Pose;
using neomdl::model::Vec3;

bool finite(float value) noexcept { return std::isfinite(value); }
bool finite(Vec3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool black(Vec3 color) noexcept {
    // The manager compares the three PartLight color floats against
    // exact zero before multiplier/fade are applied.
    return color.x == 0.0f && color.y == 0.0f && color.z == 0.0f;
}

float fadeWeight(float value, bool enabled, float amount) noexcept {
    if (enabled) return std::min(1.0f, value + amount);
    return std::max(0.0f, value - amount);
}

std::uint64_t lightId(std::size_t instanceIndex, std::size_t nodeIndex) noexcept {
    return (static_cast<std::uint64_t>(instanceIndex) << 32u) |
           static_cast<std::uint32_t>(nodeIndex);
}

Vec3 scaledColor(const RuntimeLight& light, float scale) noexcept {
    // Engine submission does not clamp authored RGB to nonnegative values.
    return {light.color.x * scale, light.color.y * scale, light.color.z * scale};
}

std::int32_t effectivePriority(const RuntimeLight& light) noexcept {
    return light.runtimePriority != 0 ? light.runtimePriority : light.priority;
}

float normalizedDistance(const RuntimeLight& light, Vec3 position) noexcept {
    if (!finite(light.position) || !finite(position) || !(light.radius > 0.0f))
        return std::numeric_limits<float>::infinity();
    const float dx = light.position.x - position.x;
    const float dy = light.position.y - position.y;
    return std::sqrt(dx * dx + dy * dy) / light.radius;
}

ActiveLight active(const RuntimeLight& light, float fade, Vec3 ownerPosition, float scale) noexcept {
    ActiveLight result;
    result.id = light.id;
    result.position = light.position;
    result.color = scaledColor(light, scale);
    result.radius = light.radius;
    result.shadowRadius = light.shadowRadius;
    result.verticalDisplacement = light.verticalDisplacement;
    result.fade = fade;
    result.normalizedDistance = normalizedDistance(light, ownerPosition);
    result.priority = effectivePriority(light);
    result.dynamicType = light.dynamicType;
    result.ambientOnly = light.ambientOnly;
    result.castsShadow = light.castsShadow;
    return result;
}

} // namespace

float k1LightQuadraticAttenuation(float radius) noexcept {
    const float squared = radius > 0.1f ? radius * radius : 0.1f * 0.1f;
    return 9.0f / (squared + squared);
}

float k2LightQuadraticAttenuation(float radius) noexcept {
    radius = std::max(radius, 0.1f);
    const float squared = radius * radius;
    return 9.0f / (squared + squared);
}

float k2VertexLightQuadraticAttenuation(float radius) noexcept {
    const float squared = radius * radius;
    return 9.0f / (squared + squared);
}

void LightScene::setDocument(const ModelDocument* document) {
    if (document_ == document) return;
    document_ = document;
    reset();
}

void LightScene::reset() {
    lights_.clear();
    pooledLights_.clear();
    owners_.clear();
    modelLightIds_.clear();
    chunkLightIds_.clear();
    chunkLightDocuments_.clear();
    chunkOwnerIds_.clear();
    rooms_.clear();
    clearRoomCollection();
    ownerQueries_.clear();
    engineDistances_.clear();
    engineIlluminationScales_.clear();
    engineVertexList_.clear();
    engineBumpLight_.reset();
    for (auto& list : sceneIllumination_) list.clear();
    for (auto& list : previousProjectionLights_) list.clear();
    projectionWeights_.clear();
    selectionElapsed_ = 0.0f;
    selectionFrame_ = 0u;
    ++revision_;
}

void LightScene::appendModelLights(const Model& model, const Pose& pose,
                                   const Mat4& base,
                                   std::size_t instanceIndex,
                                   std::vector<RuntimeLight>& pending,float ownerScale) const {
    // K1 Desktop ListSpecialPartsR (0x356d38), AddLight (0x323df8), and
    // UpdateLights (0x322948) retain the same PartLight registration contract.
    for (std::size_t nodeIndex = 0u; nodeIndex < model.nodes.size(); ++nodeIndex) {
        const Node& node = model.nodes[nodeIndex];
        if (!node.light || (node.flags & neomdl::model::kNodeLight) == 0u ||
            nodeIndex >= pose.worldTransforms.size()) {
            continue;
        }
        const auto state = nodeIndex < pose.nodeLights.size()
            ? pose.nodeLights[nodeIndex]
            : neomdl::model::bindNodeLight(node);
        const auto& data = *node.light;

        RuntimeLight runtime;
        runtime.id = lightId(instanceIndex, nodeIndex);
        runtime.instanceIndex = instanceIndex;
        runtime.nodeIndex = nodeIndex;



        const auto& local=pose.worldTransforms[nodeIndex].values;
        runtime.position = neomdl::model::transformPoint(base,
            {local[12]*ownerScale,local[13]*ownerScale,local[14]*ownerScale});
        runtime.color = {state.color[0], state.color[1], state.color[2]};
        runtime.radius = state.radius;
        runtime.shadowRadius = state.shadowRadius;
        runtime.verticalDisplacement = state.verticalDisplacement;
        runtime.multiplier = state.multiplier;
        runtime.flareRadius = data.flareRadius;
        runtime.priority = data.priority;
        runtime.dynamicType = data.dynamicType;
        runtime.ambientOnly = data.ambientOnly;
        runtime.affectDynamic = data.affectDynamic;
        runtime.castsShadow = data.castsShadow;
        runtime.generateFlare = data.generateFlare;
        runtime.fading = data.fading;
        runtime.registeredStatic = data.dynamicType == 0;
        runtime.registeredDynamic = data.dynamicType != 0 || data.affectDynamic;
        runtime.registeredRender = !black(runtime.color);
        // CollectLightsForGivenRoom uses the authored color-shift count, not
        // generateFlare and not the current light RGB, for flare registration.
        runtime.registeredFlare = !data.flareColorShifts.empty();

        const auto elementCount = std::min({data.textureNames.size(),
                                            data.flareSizes.size(),
                                            data.flarePositions.size(),
                                            data.flareColorShifts.size()});
        if (data.textureNames.size()!=data.flareSizes.size() ||
            data.textureNames.size()!=data.flarePositions.size() ||
            data.textureNames.size()!=data.flareColorShifts.size())
            runtime.flareDiagnostic="Inconsistent engine flare arrays; no truncated element substitute";
        runtime.flares.reserve(elementCount);
        for (std::size_t element = 0u; element < elementCount; ++element) {
            LightFlareElement flare;
            flare.texture = data.textureNames[element];
            flare.size = data.flareSizes[element];
            flare.position = data.flarePositions[element];
            flare.colorShift = data.flareColorShifts[element];
            runtime.flares.push_back(std::move(flare));
        }
        pending.push_back(std::move(runtime));
    }
}

void LightScene::synchronize(const Pose& mainPose, bool includeReferences) {
    std::vector<RuntimeLight> pending;
    std::unordered_set<std::uint64_t> detachedIds;
    if (document_ && document_->isOpen()) {
        appendModelLights(document_->model(), mainPose,
                          references_ ? references_->mainWorld()
                                      : neomdl::model::identityMatrix(), 0u, pending,
                          references_ ? references_->ownerScale() : 1.0f);
        if (includeReferences) {
            const auto& definitions = document_->references();
            if (references_) {
                for (std::size_t index = 0u; index < definitions.size(); ++index) {
                    const auto* runtime = references_->instance(index);
                    if (!runtime || !runtime->active) continue;
                    const auto begin=pending.size();
                    appendModelLights(definitions[index].model, runtime->pose,
                                      runtime->world, index + 1u, pending,runtime->scale);
                    if(!runtime->sceneMember)
                        for(auto i=begin;i<pending.size();++i)detachedIds.insert(pending[i].id);
                }
            } else {
                const auto bases = document_->referenceBases(mainPose);
                for (std::size_t index = 0u; index < definitions.size(); ++index) {
                    if (!bases[index]) continue;
                    appendModelLights(definitions[index].model, definitions[index].pose,
                                      *bases[index], index + 1u, pending,definitions[index].visualScale);
                }
            }
        }
    }

    // Validate the complete batch before changing live registrations. A host
    // ID collision must not leave a half-synchronized model on the next frame.
    std::unordered_set<std::uint64_t> synchronizedIds;
    for (auto& runtime : pending) {
        if (!synchronizedIds.insert(runtime.id).second)
            throw std::invalid_argument("Duplicate model light ID in scene synchronization");
        if (const auto* previous = find(runtime.id)) {
            if (!modelLightIds_.count(runtime.id))
                throw std::invalid_argument("Scene light ID collides with a model node light");
            runtime.runtimePriority = previous->runtimePriority;
        }
    }
    // In-place updates preserve retained registrations and unrelated scene
    // lights. Pose synchronization owns only its IDs.
    for (auto& runtime : pending) {
        const auto id=runtime.id;
        storeLight(std::move(runtime));
        setLightSceneMember(id,!detachedIds.count(id));
    }
    const auto destroyed=[this, &synchronizedIds](const RuntimeLight& light) {
        return modelLightIds_.count(light.id) && !synchronizedIds.count(light.id);
    };
    lights_.remove_if(destroyed);
    pooledLights_.remove_if(destroyed);
    modelLightIds_ = std::move(synchronizedIds);

    pruneRemovedLights();
    ++revision_;
}

void LightScene::registerLight(RuntimeLight light, bool refreshCollection) {
    const auto id=light.id;
    storeLight(std::move(light));
    setLightSceneMember(id,true);
    if(refreshCollection)refreshRoomCollection();
}

void LightScene::setLightSceneMember(std::uint64_t id, bool member) {
    auto& from=member?pooledLights_:lights_;
    auto& to=member?lights_:pooledLights_;
    const auto found=std::find_if(from.begin(),from.end(),
        [id](const RuntimeLight& light){return light.id==id;});
    if(found==from.end())return;
    to.splice(to.end(),from,found);
    pruneRemovedLights();
    ++revision_;
}

void LightScene::synchronizeChunks(const ParticleScene* inspectionParticles,
        const AreaScene& area,bool includeReferences) {
    std::vector<RuntimeLight> pending;
    chunkLightDocuments_.clear();
    auto previousChunkOwners=std::move(chunkOwnerIds_);
    chunkOwnerIds_.clear();
    const auto chunkScenes=[&](bool registeredOnly) {
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
        return scenes;
    };
    for(const auto* scene:chunkScenes(false)) {
        const auto& chunk=*scene->chunkOwner();
        const auto remember=[&](LightOwnerId id) {
            chunkOwnerIds_.insert(id);
            chunkLightDocuments_.emplace(static_cast<std::size_t>(id),chunk.asset);
        };
        remember(chunk.ownerIdentity);
        for(std::size_t i=0;i<chunk.references->instances().size();++i)
            remember(chunk.ownerIdentity+i+1u);
    }
    for(const auto* scene:chunkScenes(true)) {
        const auto& chunk=*scene->chunkOwner();
        const auto append=[&](LightOwnerId id,const Model& asset,const Pose& pose,const model::EngineFrame& frame) {
            appendModelLights(asset,pose,model::nodeTransform(frame.position,frame.orientation),
                static_cast<std::size_t>(id),pending,frame.scale);
        };
        append(chunk.ownerIdentity,chunk.asset->model(),chunk.pose,chunk.frame);
        if(includeReferences)for(std::size_t i=0;i<chunk.references->instances().size();++i) {
            const auto& reference=chunk.references->instances()[i];
            if(!reference.active||!reference.sceneMember)continue;
            append(chunk.ownerIdentity+i+1u,chunk.asset->references()[i].model,reference.pose,reference.frame);
        }
    }
    std::unordered_set<std::uint64_t> ids;
    for(auto& runtime:pending) {
        ids.insert(runtime.id);
        const auto pooled=std::find_if(pooledLights_.begin(),pooledLights_.end(),
            [&](const RuntimeLight& light){return light.id==runtime.id;});
        if(pooled!=pooledLights_.end())lights_.splice(lights_.end(),pooledLights_,pooled);
        if(const auto* previous=find(runtime.id))runtime.runtimePriority=previous->runtimePriority;
        storeLight(std::move(runtime));
    }
    // SetScene(null) calls RemoveLight(light,false). It removes registration
    // while the PartLight and both Gob containers
    // remain alive until ChunkyParticle destroys its Gob.
    for(auto it=lights_.begin();it!=lights_.end();) {
        const auto current=it++;
        if(chunkLightIds_.count(current->id)&&!ids.count(current->id))
            pooledLights_.splice(pooledLights_.end(),lights_,current);
    }
    pooledLights_.remove_if([&](const RuntimeLight& light) {
        const auto owner=static_cast<LightOwnerId>(light.instanceIndex);
        return previousChunkOwners.count(owner)&&!chunkOwnerIds_.count(owner);
    });
    chunkLightIds_=std::move(ids);
    pruneRemovedLights();
    ++revision_;
}

const ModelDocument* LightScene::chunkLightDocument(std::size_t instanceIndex) const noexcept {
    const auto found=chunkLightDocuments_.find(instanceIndex);
    return found==chunkLightDocuments_.end()?nullptr:found->second;
}

void LightScene::storeLight(RuntimeLight light) {
    light.registeredStatic = light.dynamicType == 0;
    light.registeredDynamic = light.hasOwner && (light.dynamicType != 0 || light.affectDynamic);
    light.registeredRender = !black(light.color);
    // PartLight constructor: +0x80 starts at zero; +0x78 starts at one.
    engineDistances_.try_emplace(light.id, 0.0f);
    engineIlluminationScales_.try_emplace(light.id, 1.0f);
    if (auto* previous = find(light.id)) {
        light.flareVisibility=previous->flareVisibility;
        *previous = std::move(light);
    } else {
        lights_.push_back(std::move(light));
    }
    ++revision_;
}

bool LightScene::removeLight(std::uint64_t id) {
    const auto it = std::find_if(lights_.begin(), lights_.end(),
        [id](const RuntimeLight& light) { return light.id == id; });
    if (it != lights_.end())lights_.erase(it);
    else {
        const auto pooled=std::find_if(pooledLights_.begin(),pooledLights_.end(),
            [id](const RuntimeLight& light){return light.id==id;});
        if(pooled==pooledLights_.end())return false;
        pooledLights_.erase(pooled);
    }
    modelLightIds_.erase(id);
    chunkLightIds_.erase(id);
    pruneRemovedLights();
    ++revision_;
    return true;
}

void LightScene::pruneRemovedLights() {
    std::unordered_set<std::uint64_t> registered,live;
    registered.reserve(lights_.size());live.reserve(lights_.size()+pooledLights_.size());
    for(const auto& light:lights_) {registered.insert(light.id);live.insert(light.id);}
    for(const auto& light:pooledLights_)live.insert(light.id);
    const auto removed = [&](std::uint64_t id) { return !live.count(id); };
    const auto unregistered = [&](std::uint64_t id) { return !registered.count(id); };
    for (auto& entry : owners_) {
        auto& owner = entry.second;
        owner.illumination.erase(std::remove_if(owner.illumination.begin(), owner.illumination.end(),
            [&](const OwnerLightSlot& slot) { return removed(slot.id); }), owner.illumination.end());
        owner.vertexIllumination.erase(std::remove_if(owner.vertexIllumination.begin(), owner.vertexIllumination.end(),
            [&](const OwnerLightSlot& slot) { return removed(slot.id); }), owner.vertexIllumination.end());
        if (owner.shadow && removed(owner.shadow->id)) owner.shadow.reset();
        auto& ranked = owner.rankedIllumination;
        ranked.erase(std::remove_if(ranked.begin(), ranked.end(),removed), ranked.end());
    }
    if(engineBumpLight_ && removed(*engineBumpLight_)) engineBumpLight_.reset();
    for (auto& room : rooms_) {
        auto& primary = room.primaryLights;
        primary.erase(std::remove_if(primary.begin(), primary.end(), unregistered), primary.end());
        auto& secondary = room.secondaryLights;
        secondary.erase(std::remove_if(secondary.begin(), secondary.end(), unregistered), secondary.end());
    }
    engineVertexList_.erase(std::remove_if(engineVertexList_.begin(), engineVertexList_.end(), removed), engineVertexList_.end());
    for (auto& prior : previousProjectionLights_)
        prior.erase(std::remove_if(prior.begin(), prior.end(), removed), prior.end());
    for (auto it = projectionWeights_.begin(); it != projectionWeights_.end();)
        if (removed(it->first)) it = projectionWeights_.erase(it); else ++it;
    for (auto& entry : owners_) {
        auto& owner = entry.second;
        if (owner.bump && removed(*owner.bump)) owner.bump.reset();
        for (auto it = owner.candidateDistances.begin(); it != owner.candidateDistances.end();)
            if (removed(it->first)) it = owner.candidateDistances.erase(it); else ++it;
        auto& ids = owner.renderCandidates;
        ids.erase(std::remove_if(ids.begin(), ids.end(), removed), ids.end());
    }
    for (auto it = engineDistances_.begin(); it != engineDistances_.end();)
        if (removed(it->first)) it = engineDistances_.erase(it); else ++it;
    for (auto it = engineIlluminationScales_.begin(); it != engineIlluminationScales_.end();)
        if (removed(it->first)) it = engineIlluminationScales_.erase(it); else ++it;
    for (auto& slots : sceneIllumination_)
        slots.erase(std::remove_if(slots.begin(), slots.end(),
            [&](const OwnerLightSlot& slot) { return removed(slot.id); }), slots.end());
    refreshRoomCollection();

}

void LightScene::registerRoom(LightRoomRegistration room, bool refreshCollection) {
    const auto it = std::find_if(rooms_.begin(), rooms_.end(),
        [&](const LightRoomRegistration& entry) { return entry.id == room.id; });
    if (it == rooms_.end()) rooms_.push_back(std::move(room));
    else *it = std::move(room);
    if(refreshCollection)refreshRoomCollection();
    ++revision_;
}

bool LightScene::removeRoom(LightRoomId id) {
    const auto it = std::find_if(rooms_.begin(), rooms_.end(),
        [id](const LightRoomRegistration& room) { return room.id == id; });
    if (it == rooms_.end()) return false;
    rooms_.erase(it);
    for (auto& room : rooms_) {
        auto& ids = room.influencingRooms;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    }
    refreshRoomCollection();
    ++revision_;
    return true;
}

void LightScene::collectLightsForGivenRoom(std::optional<LightRoomId> room) {
    roomCollectionEnabled_ = true;
    collectedRoom_ = room;
    refreshRoomCollection();
    ++revision_;
}

void LightScene::clearRoomCollection() {
    roomCollectionEnabled_ = false;
    collectedRoom_.reset();
    collectedLights_.clear();
    collectedFlares_.clear();
    roomDiagnostic_.clear();
    ++revision_;
}

void LightScene::refreshRoomCollection() {
    collectedLights_.clear();
    collectedFlares_.clear();
    roomDiagnostic_.clear();
    if (!roomCollectionEnabled_) return;
    std::unordered_map<LightRoomId,const LightRoomRegistration*> roomLookup;
    roomLookup.reserve(rooms_.size());
    for(const auto& room:rooms_)roomLookup.emplace(room.id,&room);
    std::unordered_map<std::uint64_t,const RuntimeLight*> lightLookup;
    lightLookup.reserve(lights_.size()+pooledLights_.size());
    for(const auto& light:lights_)lightLookup.emplace(light.id,&light);
    for(const auto& light:pooledLights_)lightLookup.emplace(light.id,&light);
    const auto roomById = [&](LightRoomId id) -> const LightRoomRegistration* {
        const auto it=roomLookup.find(id);
        return it==roomLookup.end()?nullptr:it->second;
    };
    std::vector<LightRoomId> order;
    if (collectedRoom_) {
        const auto* room = roomById(*collectedRoom_);
        if (!room) {
            roomDiagnostic_ = "Requested engine light room is not registered; query not submitted.";
            return;
        }
        order = room->influencingRooms;
        order.push_back(room->id);
    } else {
        for (const auto& room : rooms_) order.push_back(room.id);
    }
    std::unordered_set<std::uint64_t> seenLights,seenFlares;
    for (const auto id : order) {
        const auto* room = roomById(id);
        if (!room) {
            collectedLights_.clear();
            collectedFlares_.clear();
            roomDiagnostic_ = "Influencing engine light room is not registered; query not submitted.";
            return;
        }
        const auto collect = [&](const std::vector<std::uint64_t>& ids, bool deduplicate) {
            for (const auto lightId : ids) {
                const auto found=lightLookup.find(lightId);
                if(found==lightLookup.end())continue;
                const auto* light=found->second;
                if (light->registeredFlare&&seenFlares.insert(lightId).second) collectedFlares_.push_back(lightId);
                if (!black(light->color)) {
                    const bool first=seenLights.insert(lightId).second;
                    if (!deduplicate||first) collectedLights_.push_back(lightId);
                }
            }
        };
        collect(room->primaryLights, false);
        collect(room->secondaryLights, true);
    }
}

void LightScene::setOwnerQuery(LightOwnerId owner, LightOwnerQuery query) {
    ownerQueries_[owner] = query;
    ++revision_;
}

void LightScene::removeOwner(LightOwnerId owner) {
    owners_.erase(owner);
    ownerQueries_.erase(owner);
    ++revision_;
}

void LightScene::snapshotVertexScales(OwnerLightSelection& owner) {
    owner.vertexIllumination.clear();
    if (!owner.diagnostic.empty()) return;
    // Object-container submission writes PartLight +0x78 then replaces the
    // manager's +0x44 list with its admitted container (Desktop 0xdf343..0xdf3eb).
    if (owner.query.submission == LightSubmissionMode::Object && owner.query.hasFadeContainer)
        for (const auto& slot : owner.illumination)
            engineIlluminationScales_.at(slot.id) = slot.weight;
    if (owner.query.submission == LightSubmissionMode::Scene) {



        for (const auto id : owner.rankedIllumination) {
            if (owner.vertexIllumination.size() == maximumIlluminationLights) break;
            const auto scale = engineIlluminationScales_.at(id);
            owner.vertexIllumination.push_back({id, scale, scale});
        }
    } else {
        owner.vertexIllumination = owner.illumination;
        for (auto& slot : owner.vertexIllumination)
            slot.vertexScale = owner.query.submission == LightSubmissionMode::SceneDynamicOnly
                ? 1.0f // VP mode 1 reads +0x74 and ignores the separate +0x7c fade
                : engineIlluminationScales_.at(slot.id);
    }
}

std::vector<ActiveLight> LightScene::retainedVertexLights(bool ownerPresent) const {
    std::vector<ActiveLight> result;
    if (ownerPresent) {
        for (const auto id : engineVertexList_) {
            if (result.size() == maximumIlluminationLights) break;
            const auto* light = find(id);
            if (!light) continue;
            const auto scale = engineIlluminationScales_.at(id);
            result.push_back(active(*light,scale,{},scale));
        }
    } else {
        for (const auto& slot : sceneIllumination_[1]) {
            const auto* light = find(slot.id);
            if (light) result.push_back(active(*light,1.0f,{},1.0f));
        }
    }
    return result;
}

std::optional<LightOwnerQuery> LightScene::ownerQuery(LightOwnerId id) const {
    const auto found = ownerQueries_.find(id);
    return found == ownerQueries_.end() ? std::nullopt : std::optional<LightOwnerQuery>{found->second};
}

void LightScene::enableSceneLights(OwnerLightSelection& owner) {
    const bool dynamicOnly = owner.query.submission == LightSubmissionMode::SceneDynamicOnly;
    auto& current = sceneIllumination_[dynamicOnly ? 1u : 0u];
    // Ordinary scene fading and object submission share engine PartLight +0x78.
    // The dynamic-only channel uses its distinct +0x7c field instead.
    if (!dynamicOnly)
        for (auto& slot : current) slot.weight = engineIlluminationScales_.at(slot.id);
    std::vector<std::uint64_t> desired;
    for (const auto id : owner.rankedIllumination) {
        // EnableLightsDynamicOnly: exactly type 1, not every nonzero type.
        if (!dynamicOnly || find(id)->dynamicType == 1) desired.push_back(id);
    }
    auto limit = maximumIlluminationLights;
    if (owner.query.sceneOwnerPrefix) {
        std::size_t prefix = 0;
        for (const auto id : desired) {
            if (!find(id)->hasOwner || find(id)->instanceIndex != *owner.query.sceneOwnerPrefix) break;
            ++prefix;
        }
        limit = std::min(limit, prefix);
    }
    const auto rank = [&](std::uint64_t id) {
        return static_cast<std::size_t>(std::find(desired.begin(), desired.end(), id) - desired.begin());
    };


    for (std::size_t i = current.size(); i-- > 0;) {
        auto& slot = current[i];
        const auto index = rank(slot.id);
        if (!find(slot.id)) {
            current.erase(current.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const bool wanted = index < desired.size() && index < limit;
        if (wanted) slot.weight = std::min(1.0f, slot.weight + selectionElapsed_);
        else slot.weight -= selectionElapsed_;
        const bool expired = slot.weight < 0.0f;
        if (!dynamicOnly) engineIlluminationScales_.at(slot.id) = expired ? 0.0f : slot.weight;
        if (expired) current.erase(current.begin() + static_cast<std::ptrdiff_t>(i));
    }
    std::size_t newNonFading = 0;
    const auto contains = [&](std::uint64_t id) {
        return std::any_of(current.begin(), current.end(),
            [id](const OwnerLightSlot& slot) { return slot.id == id; });
    };
    for (std::size_t i = 0; i < std::min(limit, desired.size()); ++i)
        if (!find(desired[i])->fading && !contains(desired[i])) ++newNonFading;
    auto free = static_cast<std::ptrdiff_t>(limit) - static_cast<std::ptrdiff_t>(current.size());
    for (std::size_t i = current.size(); i-- > 0 && static_cast<std::ptrdiff_t>(newNonFading) > free;) {
        const auto index = rank(current[i].id);
        // Preserve the engine strict > comparison at Desktop 0xdf9bd.
        if (index == desired.size() || index > limit) {
            current.erase(current.begin() + static_cast<std::ptrdiff_t>(i));
            ++free;
        }
    }
    for (std::size_t i = 0; i < limit && i < desired.size() && current.size() < limit; ++i) {
        const auto id = desired[i];
        auto it = std::find_if(current.begin(), current.end(),
            [id](const OwnerLightSlot& slot) { return slot.id == id; });
        if (it == current.end()) {
            current.push_back({id, find(id)->fading ? 0.0f : 1.0f});
            it = current.end() - 1;
        }
        // This is a second increment for retained entries while space remains.
        // Do not normalize it to one increment: Desktop 0xdfa0b..0xdfab2 does both.
        it->weight = std::min(1.0f, it->weight + selectionElapsed_);
        if (!dynamicOnly) engineIlluminationScales_.at(id) = it->weight;
    }
    std::vector<std::uint64_t> sorted;
    for (const auto& slot : current) sorted.push_back(slot.id);
    if (!sortLights(sorted, owner.diagnostic)) {
        owner.illumination.clear();
        owner.shadow.reset();
        return;
    }
    std::vector<OwnerLightSlot> ordered;
    for (const auto id : sorted) {
        const auto it = std::find_if(current.begin(), current.end(),
            [id](const OwnerLightSlot& slot) { return slot.id == id; });
        ordered.push_back(*it);
    }
    current = ordered;
    owner.illumination = std::move(ordered);
    owner.shadow.reset();
}

const RuntimeLight* LightScene::retainedBumpLight() const noexcept {
    return engineBumpLight_ ? find(*engineBumpLight_) : nullptr;
}

const RuntimeLight* LightScene::bumpLight(LightOwnerId id) const noexcept {
    const auto* owner = ownerSelection(id);
    if (!owner || !owner->bump || !owner->diagnostic.empty()) return nullptr;
    // Engine getter returns a PartLight pointer, not a pre-shaded contribution.
    // Each specialized material consumer is responsible for its own constants.
    return find(*owner->bump);
}

ShadowLightSelection LightScene::projectionShadowLights(LightOwnerId id, ShadowLightQuery mode) {
    ShadowLightSelection result;
    if (mode == ShadowLightQuery::Static) {
        // GetShadowLights(2): authored priority strictly below initial 6;
        // static type OR radius >=1000. Runtime priority does not participate.
        const RuntimeLight* best = nullptr;
        std::int32_t priority = 6;
        for (const auto& light : lights_) {
            if (light.castsShadow && !light.ambientOnly &&
                (light.radius >= 1000.0f || light.dynamicType == 0) && light.priority < priority) {
                best = &light;
                priority = light.priority;
            }
        }
        if (best) result.lights.push_back({best->id, 1.0f});
        return result;
    }
    if (mode != ShadowLightQuery::All && mode != ShadowLightQuery::Dynamic) {
        result.diagnostic = "Unsupported K2 shadow-light query mode.";
        return result;
    }
    const auto* owner = ownerSelection(id);
    if (!owner || !owner->diagnostic.empty()) {
        result.diagnostic = owner ? owner->diagnostic : "No engine position/radius light query has been selected.";
        return result;
    }
    // Restore this queued query's engine distances. Another owner may have
    // used the same PartLights since this position/radius query was selected.
    for (const auto& entry : owner->candidateDistances) engineDistances_[entry.first] = entry.second;
    std::vector<std::uint64_t> ranked;
    for (const auto lightId : owner->renderCandidates) {
        const auto* light = find(lightId);
        // PrioritizeShadow: threshold is >0.1 in any raw channel, before fades.
        if (light && light->castsShadow && !light->ambientOnly &&
            (light->color.x > 0.1f || light->color.y > 0.1f || light->color.z > 0.1f))
            ranked.push_back(lightId);
    }
    if (!sortLights(ranked, result.diagnostic)) return result;
    std::vector<std::uint64_t> chosen;
    for (const auto lightId : ranked) {
        const auto* light = find(lightId);
        if (mode == ShadowLightQuery::Dynamic && light->dynamicType == 0) continue;
        if (engineDistances_.at(lightId) > 1.0f) continue;
        chosen.push_back(lightId);
        if (chosen.size() == maximumShadowLights) break;
    }
    auto& previous = previousProjectionLights_[mode == ShadowLightQuery::Dynamic ? 1u : 0u];
    for (const auto lightId : chosen)
        if (std::find(previous.begin(), previous.end(), lightId) == previous.end())
            projectionWeights_[lightId] = 0.0f;
    const float step = selectionElapsed_ / 0.3f;
    for (std::size_t i = 0; i < std::min(maximumShadowLights, previous.size()); ++i) {
        const auto lightId = previous[i];
        if (!find(lightId) || std::find(chosen.begin(), chosen.end(), lightId) != chosen.end()) continue;
        auto& weight = projectionWeights_[lightId];
        weight = std::max(0.0f, weight - step);
        if (weight > 0.0f) chosen.insert(chosen.begin(), lightId);
    }
    if (chosen.size() > maximumShadowLights) chosen.resize(maximumShadowLights);
    for (const auto lightId : chosen) {
        auto& weight = projectionWeights_[lightId];
        // Engine adds dt/0.3 to ALL survivors, including an outgoing entry
        // just prepended above. Preserve that behavior, not a guessed crossfade.
        weight = std::min(1.0f, weight + step);
        result.lights.push_back({lightId, weight});
    }
    previous = std::move(chosen);
    ++revision_;
    return result;
}

std::vector<ActiveLight> LightScene::nearestLights(Vec3 position, float radius,
                                                   bool dynamicOnly) const {
    if (!finite(position) || !finite(radius) || radius < 0.0f)
        throw std::invalid_argument("Invalid K2 nearest-light query sphere");
    std::vector<ActiveLight> result;
    result.reserve(maximumIlluminationLights);


    for (const auto& light : lights_) {
        if (!(light.radius > 0.0f) ||
            (dynamicOnly ? light.dynamicType == 0
                         : light.dynamicType == 0 && !light.affectDynamic)) continue;
        const float dx = light.position.x - position.x;
        const float dy = light.position.y - position.y;
        const float dz = light.position.z - position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(distance <= light.radius + radius)) continue;
        // Black and ambient-only lights still occupy a slot in this routine.
        result.push_back(active(light, 1.0f, position, light.multiplier));
        if (result.size() == maximumIlluminationLights) break;
    }
    return result;
}

void LightScene::beginSelectionFrame(float elapsedSeconds) {
    selectionElapsed_ = elapsedSeconds;
    refreshRoomCollection();
    if (++selectionFrame_ == 0u) { owners_.clear(); ++selectionFrame_; }
}

void LightScene::retainOwnerForFrame(LightOwnerId id) {
    owners_[id].lastRetainedFrame = selectionFrame_;
}

void LightScene::selectOwner(LightOwnerId id, Vec3 position, float radius) {
    auto& owner = owners_[id];
    if (owner.lastSelectedFrame == selectionFrame_) return;
    owner.lastSelectedFrame = selectionFrame_;
    owner.position = position;
    owner.radius = radius;
    const auto query = ownerQueries_.find(id);
    owner.query = query == ownerQueries_.end() ? LightOwnerQuery{} : query->second;
    if (owner.query.sphere) {
        owner.position = owner.query.sphere->position;
        owner.radius = owner.query.sphere->radius;
    }
    rankCandidates(owner);
    if (!owner.diagnostic.empty()) {
        // Fail closed: do not continue submitting a stale selection while the
        // current query is unsupported or lacks its engine room inputs.
        owner.illumination.clear();
        owner.shadow.reset();
    } else if (owner.query.submission != LightSubmissionMode::Object) {
        enableSceneLights(owner);
    } else if (owner.query.hasFadeContainer) {
        advanceIlluminationSlots(owner);
        advanceShadowSlot(owner);
    } else {
        owner.illumination.clear();
        owner.shadow.reset();
        for (const auto light : owner.rankedIllumination) {
            if (owner.illumination.size() == maximumIlluminationLights) break;
            owner.illumination.push_back({light, 1.0f});
        }
    }
    snapshotVertexScales(owner);
    engineBumpLight_=owner.diagnostic.empty() ? owner.bump : std::nullopt;
    engineVertexList_.clear();
    if (owner.diagnostic.empty()) {
        if (owner.query.submission == LightSubmissionMode::SceneDynamicOnly)
            engineVertexList_ = owner.rankedIllumination;
        else for (const auto& slot : owner.vertexIllumination) engineVertexList_.push_back(slot.id);
    }
    ++revision_;
}

void LightScene::selectSceneContainer(LightOwnerId container,LightOwnerQuery query) {
    if(query.submission!=LightSubmissionMode::Scene||!query.sphere)
        throw std::runtime_error("Scene light-container selection requires a scene query and sphere");
    const auto sphere=*query.sphere;
    if(!std::isfinite(sphere.radius)||sphere.radius<0||!std::isfinite(sphere.position.x)||
       !std::isfinite(sphere.position.y)||!std::isfinite(sphere.position.z))
        throw std::runtime_error("Invalid scene light-container sphere");
    setOwnerQuery(container,std::move(query));
    owners_[container].lastSelectedFrame=selectionFrame_-1u;
    selectOwner(container,sphere.position,sphere.radius);
}

void LightScene::endSelectionFrame() {
    const auto previousSize = owners_.size();
    for (auto it = owners_.begin(); it != owners_.end();) {
        if (it->second.lastSelectedFrame != selectionFrame_ &&
            it->second.lastRetainedFrame != selectionFrame_ &&
            !chunkOwnerIds_.count(it->first)) it = owners_.erase(it);
        else ++it;
    }
    if (owners_.size() != previousSize) ++revision_;
}

const OwnerLightSelection* LightScene::ownerSelection(LightOwnerId id) const noexcept {
    const auto it = owners_.find(id);
    return it == owners_.end() ? nullptr : &it->second;
}

void LightScene::rankCandidates(OwnerLightSelection& owner) {
    owner.renderCandidates.clear();
    owner.candidateDistances.clear();
    owner.rankedIllumination.clear();
    owner.bump.reset();
    owner.diagnostic.clear();
    if (!finite(owner.position) || !finite(owner.radius) || owner.radius < 0.0f) {
        owner.diagnostic = "Invalid K2 light query sphere; query not submitted.";
        return;
    }
    const bool supplied = owner.query.source != LightQuerySource::Global;
    if (supplied && (!roomCollectionEnabled_ || !roomDiagnostic_.empty())) {
        owner.diagnostic = roomDiagnostic_.empty()
            ? "Engine room collection has not been supplied; query not submitted."
            : roomDiagnostic_;
        return;
    }
    const float broadRadius = supplied ? owner.radius + owner.radius : owner.radius;
    if (!finite(broadRadius)) {
        owner.diagnostic = "K2 light query radius overflow; query not submitted.";
        return;
    }
    const auto admit = [&](const RuntimeLight& light) {



        if ((!light.affectDynamic && light.dynamicType == 0) ||
            !finite(light.position) || !(light.radius > 0.0f) ||
            (supplied && black(light.color))) return;
        if (!(std::fabs(light.position.x - owner.position.x) - light.radius < broadRadius) ||
            !(std::fabs(light.position.y - owner.position.y) - light.radius < broadRadius)) return;
        if (std::find(owner.renderCandidates.begin(), owner.renderCandidates.end(), light.id) ==
            owner.renderCandidates.end()) owner.renderCandidates.push_back(light.id);
    };
    if (supplied) {
        for (const auto id : collectedLights_) if (const auto* light = find(id)) admit(*light);
    } else {
        for (const auto& light : lights_) admit(light);
    }
    // PrioritizeIllumination filters candidates before ranking them.
    for (const auto id : owner.renderCandidates) {
        const auto* light = find(id);
        engineDistances_[id] = black(light->color) ? 100000.0f :
            normalizedDistance(*light, owner.position);
        owner.candidateDistances[id] = engineDistances_[id];
        if (!black(light->color) && (owner.query.includeOutOfRange ||
            normalizedDistance(*light, owner.position) <= 1.0f))
            owner.rankedIllumination.push_back(id);
    }
    if (!sortLights(owner.rankedIllumination, owner.diagnostic)) return;
    const auto count = std::min(maximumIlluminationLights, owner.rankedIllumination.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto* light = find(owner.rankedIllumination[i]);
        if (!light->ambientOnly && light->dynamicType != 0) {
            owner.bump = light->id;
            break;
        }
    }
}

bool LightScene::sortLights(std::vector<std::uint64_t>& ids, std::string& diagnostic) const {
    diagnostic.clear();
    struct SortKey {
        std::uint64_t id;
        std::int32_t priority, dynamicType;
        float distance;
        bool black;
    };
    std::vector<SortKey> keys;
    keys.reserve(ids.size());
    for (const auto id : ids) {
        const auto* light = find(id);
        const auto distance = engineDistances_.find(id);
        if (!light || distance == engineDistances_.end()) {
            diagnostic = "Missing light or distance for light selection.";
            ids.clear();
            return false;
        }
        keys.push_back({id, effectivePriority(*light), light->dynamicType,
                        distance->second, black(light->color)});
    }
    const auto typeRank = [](std::int32_t type) {
        switch (type) {
        case 1: return 0;
        case 2: return 1;
        case 0: return 2;
        default: return 3;
        }
    };
    std::stable_sort(keys.begin(), keys.end(), [&](const SortKey& left, const SortKey& right) {
        if (left.black != right.black) return !left.black;
        if (left.priority != right.priority) return left.priority < right.priority;
        const auto leftType = typeRank(left.dynamicType), rightType = typeRank(right.dynamicType);
        if (leftType != rightType) return leftType < rightType;
        if (left.dynamicType != right.dynamicType) return left.dynamicType < right.dynamicType;
        // NaNs follow every numeric distance; infinities keep numeric order.
        const bool leftNaN = std::isnan(left.distance), rightNaN = std::isnan(right.distance);
        if (leftNaN != rightNaN) return !leftNaN;
        if (left.distance < right.distance) return true;
        if (right.distance < left.distance) return false;
        return left.id < right.id;
    });
    ids.clear();
    for (const auto& key : keys) ids.push_back(key.id);
    return true;
}

void LightScene::advanceIlluminationSlots(OwnerLightSelection& owner) {
    const auto count = std::min(maximumIlluminationLights, owner.rankedIllumination.size());
    const auto desiredEnd = owner.rankedIllumination.begin() + static_cast<std::ptrdiff_t>(count);
    const float step = selectionElapsed_ * lightFadeSpeed;
    for (auto& slot : owner.illumination) {
        const auto* light = find(slot.id);
        const bool wanted = light && std::find(owner.rankedIllumination.begin(), desiredEnd, slot.id) != desiredEnd;
        slot.weight = light && light->fading
            ? fadeWeight(slot.weight, wanted, step)
            : (wanted ? 1.0f : 0.0f);
    }
    owner.illumination.erase(std::remove_if(owner.illumination.begin(), owner.illumination.end(),
        [&](const OwnerLightSlot& slot) {
            return !find(slot.id) || (slot.weight <= 0.0f &&
                std::find(owner.rankedIllumination.begin(), desiredEnd, slot.id) == desiredEnd);
        }), owner.illumination.end());
    for (auto it = owner.rankedIllumination.begin(); it != desiredEnd; ++it) {
        if (owner.illumination.size() >= maximumIlluminationLights ||
            owner.illumination.size() >= owner.rankedIllumination.size()) break;
        if (std::any_of(owner.illumination.begin(), owner.illumination.end(),
                [it](const OwnerLightSlot& slot) { return slot.id == *it; })) continue;


        owner.illumination.push_back({*it, 0.0f});
    }
}

void LightScene::advanceShadowSlot(OwnerLightSelection& owner) {
    std::optional<std::uint64_t> desired;
    for (const auto id : owner.rankedIllumination) {
        const auto* light = find(id);
        if (light && light->castsShadow) { desired = id; break; }
    }
    const float step = selectionElapsed_ * shadowFadeSpeed;
    // Object shadow fading is unconditional, unlike illumination's authored
    // fading flag (Desktop 0xdf1c9..0xdf2a3). It has its own weight per owner.
    if (owner.shadow && (!desired || owner.shadow->id != *desired)) {
        owner.shadow->weight = fadeWeight(owner.shadow->weight, false, step);
        if (owner.shadow->weight <= 0.0f) owner.shadow.reset();
    }
    if (!owner.shadow && desired) owner.shadow = OwnerLightSlot{*desired, 0.0f};
    if (owner.shadow && desired && owner.shadow->id == *desired)
        owner.shadow->weight = fadeWeight(owner.shadow->weight, true, step);
}

std::vector<ActiveLight> LightScene::illuminationLights(LightOwnerId id, LightColorSource source) const {
    std::vector<ActiveLight> result;
    const auto* owner = ownerSelection(id);
    if (!owner) return result;
    const auto& slots = source == LightColorSource::VertexProgram
        ? owner->vertexIllumination : owner->illumination;
    result.reserve(slots.size());
    for (const auto& slot : slots) {
        const auto* light = find(slot.id);
        if (light) {
            float scale = slot.vertexScale;
            if (source == LightColorSource::FixedFunction) {
                if (owner->query.submission != LightSubmissionMode::Object)
                    scale = slot.weight; // ControlLightsFade multiplies RGB by fade first
                else
                    scale = owner->query.hasFadeContainer ? slot.weight : light->multiplier;
            }
            auto contribution = active(*light, slot.weight, owner->position, scale);
            if (source == LightColorSource::FixedFunction &&
                owner->query.submission != LightSubmissionMode::Object) {
                // Preserve (RGB * fade) * multiplier, not RGB * (fade * multiplier).
                contribution.color.x *= light->multiplier;
                contribution.color.y *= light->multiplier;
                contribution.color.z *= light->multiplier;
            }
            result.push_back(contribution);
        }
    }
    return result;
}

std::optional<ActiveLight> LightScene::shadowLight(LightOwnerId id) const {
    const auto* owner = ownerSelection(id);
    if (!owner || !owner->shadow || owner->shadow->weight <= 0.0f) return std::nullopt;
    const auto* light = find(owner->shadow->id);
    return light ? std::optional<ActiveLight>{active(*light, owner->shadow->weight, owner->position, owner->shadow->weight)}
                 : std::nullopt;
}

float LightScene::illuminationWeight(LightOwnerId owner, std::uint64_t light) const noexcept {
    const auto* selection = ownerSelection(owner);
    if (selection) for (const auto& slot : selection->illumination)
        if (slot.id == light) return slot.weight;
    return 0.0f;
}

float LightScene::shadowWeight(LightOwnerId owner, std::uint64_t light) const noexcept {
    const auto* selection = ownerSelection(owner);
    return selection && selection->shadow && selection->shadow->id == light
        ? selection->shadow->weight : 0.0f;
}

const RuntimeLight* LightScene::find(std::uint64_t id) const noexcept {
    const auto found = std::find_if(lights_.begin(), lights_.end(),
        [id](const RuntimeLight& light) { return light.id == id; });
    if(found!=lights_.end())return &*found;
    const auto pooled=std::find_if(pooledLights_.begin(),pooledLights_.end(),
        [id](const RuntimeLight& light){return light.id==id;});
    return pooled==pooledLights_.end()?nullptr:&*pooled;
}

RuntimeLight* LightScene::find(std::uint64_t id) noexcept {
    return const_cast<RuntimeLight*>(static_cast<const LightScene&>(*this).find(id));
}

const RuntimeLight* LightScene::mainNode(std::size_t nodeIndex) const noexcept {
    return find(lightId(0u, nodeIndex));
}

LightSceneStatistics LightScene::statistics(LightOwnerId owner) const noexcept {
    LightSceneStatistics result;
    result.all = lights_.size();
    for (const auto& light : lights_) {
        if (light.registeredStatic) ++result.staticLights;
        if (light.registeredDynamic) ++result.dynamicLights;
        if (light.registeredRender) ++result.renderCandidates;
        if (light.registeredFlare) ++result.flareLights;
    }
    result.owners = owners_.size();
    if (const auto* selection = ownerSelection(owner)) {
        for (const auto& slot : selection->illumination)
            if (slot.weight > 0.0f && find(slot.id)) ++result.activeIllumination;
        result.activeShadow = selection->shadow && selection->shadow->weight > 0.0f &&
                              find(selection->shadow->id);
    }
    return result;
}

} // namespace neomdl
