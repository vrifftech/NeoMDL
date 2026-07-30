#include "core/LightsaberScene.hpp"
#include "core/ReferenceScene.hpp"
#include "core/SceneAnimation.hpp"
#include "core/SceneQueries.hpp"

#include <algorithm>
#include <cmath>

namespace neomdl {
namespace m = neomdl::model;
namespace {

constexpr std::size_t kHistoryCapacity = 20u;
constexpr float kSegmentLife = 0.012f;
constexpr float kSegmentInterval = 0.001f;
constexpr float kMinimumAgeStep = 0.010f;
constexpr float kFudgeFactor = 0.5f;
constexpr float kStationaryRefreshAge = 60.0f;
constexpr float kTeleportDistance = 1.0f;

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool finite(const m::Vec3& value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool equal(const m::Vec3& left, const m::Vec3& right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

float squaredDistance(const m::Vec3& left, const m::Vec3& right) noexcept {
    const auto x = left.x - right.x;
    const auto y = left.y - right.y;
    const auto z = left.z - right.z;
    return x * x + y * y + z * z;
}

m::Vec3 add(const m::Vec3& left, const m::Vec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

m::Vec3 subtract(const m::Vec3& left, const m::Vec3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

m::Vec3 multiply(const m::Vec3& value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

m::Vec3 interpolate(const m::Vec3& left, const m::Vec3& right,
                    float fraction) noexcept {
    return {left.x + (right.x - left.x) * fraction,
            left.y + (right.y - left.y) * fraction,
            left.z + (right.z - left.z) * fraction};
}

float dot(const m::Vec3& left, const m::Vec3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

m::Quaternion multiply(const m::Quaternion& left,
                       const m::Quaternion& right) noexcept {
    return m::engineQuaternionProduct(left,right);
}

m::Vec3 rotate(const m::Quaternion& orientation, const m::Vec3& value) noexcept {
    return m::engineQuaternionRotate(orientation,value);
}

m::Quaternion packedSlerp(m::Quaternion left, const m::Quaternion& right,
                          float fraction) noexcept {
    // The engine Quaternion::slerp path negates
    // the first argument for a negative dot product, has no linear shortcut,
    // and leaves the interpolated result unnormalized.
    float cosine = left.x * right.x + left.y * right.y +
                   left.z * right.z + left.w * right.w;
    if (cosine < 0.0f) {
        cosine = -cosine;
        left.x = -left.x;
        left.y = -left.y;
        left.z = -left.z;
        left.w = -left.w;
    }
    if (cosine >= 1.0f) return left;
    const auto angle = std::acos(cosine);
    if (angle == 0.0f) return left;
    const auto sine = std::sin(angle);
    const auto rightAngle = fraction * angle;
    const auto leftWeight = std::sin(angle - rightAngle) / sine;
    const auto rightWeight = std::sin(rightAngle) / sine;
    return {left.x * leftWeight + right.x * rightWeight,
            left.y * leftWeight + right.y * rightWeight,
            left.z * leftWeight + right.z * rightWeight,
            left.w * leftWeight + right.w * rightWeight};
}

m::Quaternion rotationArc(const m::Vec3& from, const m::Vec3& to) noexcept {
    return engineRotationArc(from,to);
}

m::Mesh makeMeshBase(std::size_t nodeIndex) {
    m::Mesh mesh;
    mesh.nodeIndex = nodeIndex;
    mesh.diffuse = {1.0f, 1.0f, 1.0f};
    mesh.ambient = {};
    mesh.render = true;
    mesh.castsShadow = false;
    mesh.hasVertexColors = false;
    return mesh;
}

m::Mesh makeStaticMesh(const m::LightsaberNodeData& data, std::size_t nodeIndex,
                       m::GameVersion game) {
    const bool separate=game==m::GameVersion::Kotor1;
    auto mesh = makeMeshBase(nodeIndex);

    // K2 emits one GL_QUAD_STRIP, including a repeated front endpoint
    // and a final odd vertex. K1 has two eight-vertex strips.
    constexpr std::array<std::size_t, 17> sourceOrder{{
        m::LightsaberNodeData::frontPieceOffset + 0u,
        m::LightsaberNodeData::frontPieceOffset + 4u,
        m::LightsaberNodeData::frontPieceOffset + 1u,
        m::LightsaberNodeData::frontPieceOffset + 5u,
        m::LightsaberNodeData::frontPieceOffset + 2u,
        m::LightsaberNodeData::frontPieceOffset + 6u,
        m::LightsaberNodeData::frontPieceOffset + 3u,
        m::LightsaberNodeData::frontPieceOffset + 7u,
        m::LightsaberNodeData::frontPieceOffset + 7u,
        m::LightsaberNodeData::backPieceOffset + 4u,
        m::LightsaberNodeData::backPieceOffset + 0u,
        m::LightsaberNodeData::backPieceOffset + 5u,
        m::LightsaberNodeData::backPieceOffset + 1u,
        m::LightsaberNodeData::backPieceOffset + 6u,
        m::LightsaberNodeData::backPieceOffset + 2u,
        m::LightsaberNodeData::backPieceOffset + 7u,
        m::LightsaberNodeData::backPieceOffset + 3u,
    }};
    mesh.vertices.reserve(sourceOrder.size());
    for (std::size_t index = 0u; index < sourceOrder.size(); ++index) {
        // K1 Desktop ends its first strip at 0x342da8 and starts the back strip
        // at 0x342db4. It has neither the duplicate nor the odd tail.
        if (separate && index == 8u) continue;
        const auto source = sourceOrder[index];
        m::Vertex vertex;
        vertex.position = data.positions[source];
        vertex.normal = data.normals[source];
        vertex.texcoord0 = data.texcoords[source];
        mesh.vertices.push_back(vertex);
    }

    // K2's seventeenth index is retained; OpenGL ignores that incomplete pair.
    mesh.indices.reserve(mesh.vertices.size());
    for (std::uint32_t index = 0u; index < mesh.vertices.size(); ++index)
        mesh.indices.push_back(index);
    if (separate) {
        mesh.engineDraws.push_back({7u,0u,8u});
        mesh.engineDraws.push_back({7u,8u,8u});
    }
    return mesh;
}

using Edge = std::array<m::Vec3, 4>;
using SegmentSchedule = std::array<std::vector<float>, kHistoryCapacity - 1u>;

std::vector<LightsaberHistorySample> logicalHistory(
    const LightsaberInstance& instance) {
    std::vector<LightsaberHistorySample> result;
    result.reserve(instance.historyCount);
    if (instance.historyCount == 0u) return result;

    auto index = static_cast<std::size_t>(
        (instance.writeIndex + kHistoryCapacity - 1u) % kHistoryCapacity);
    for (std::uint16_t count = 0u; count < instance.historyCount; ++count) {
        result.push_back(instance.history[index]);
        index = (index + kHistoryCapacity - 1u) % kHistoryCapacity;
    }
    return result;
}

SegmentSchedule segmentSchedule(
    const std::vector<LightsaberHistorySample>& samples) {
    SegmentSchedule result;
    float elapsed = kSegmentInterval;
    for (std::size_t pair = 0u; pair + 1u < samples.size() && pair < result.size();
         ++pair) {
        const auto span = samples[pair + 1u].age - samples[pair].age;
        if (!(span > 0.0f) || !finite(span)) continue;
        const auto step = kSegmentInterval / span;
        if (!(step > 0.0f) || !finite(step)) continue;
        for (auto fraction = step;
             fraction < 1.0f && elapsed < kSegmentLife;
             fraction += step, elapsed += kSegmentInterval) {
            result[pair].push_back(fraction);
        }
    }
    return result;
}

m::Vec3 toCurrentLocal(const m::Vec3& source,
                       const LightsaberHistorySample& sample,
                       const m::Vec3& currentPosition,
                       const m::Quaternion& currentInverse) noexcept {
    const auto worldPosition = add(rotate(sample.orientation, source), sample.position);
    return rotate(currentInverse, subtract(worldPosition, currentPosition));
}

Edge makeEdge(const m::LightsaberNodeData& data, std::size_t sourceOffset,
              const LightsaberHistorySample& sample,
              const m::Vec3& currentPosition,
              const m::Quaternion& currentInverse) noexcept {
    Edge result;
    for (std::size_t index = 0u; index < result.size(); ++index) {
        result[index] = toCurrentLocal(data.positions[sourceOffset + index], sample,
                                       currentPosition, currentInverse);
    }
    return result;
}

void appendVertex(m::Mesh& mesh, const m::LightsaberNodeData& data,
                  const m::Vec3& position, std::size_t sourceIndex) {
    m::Vertex vertex;
    vertex.position = position;
    // The moving path does not submit normals and disables lighting by default.
    // Retain the corresponding generated normal so the streamed Mesh remains a
    // well-formed renderer input without changing the default visual path.
    vertex.normal = data.normals[sourceIndex];
    vertex.texcoord0 = data.texcoords[sourceIndex];
    mesh.vertices.push_back(vertex);
}

void appendBackSection(m::Mesh& mesh, const m::LightsaberNodeData& data,
                       const Edge& next, const Edge& previous, bool separate) {
    const auto first = mesh.vertices.size();
    const auto source = m::LightsaberNodeData::backPieceOffset;
    if (!separate) appendVertex(mesh, data, next[0], source);
    appendVertex(mesh, data, next[0], source);
    appendVertex(mesh, data, previous[0], source);
    for (std::size_t index = 1u; index < next.size(); ++index) {
        appendVertex(mesh, data, next[index], source + index);
        appendVertex(mesh, data, previous[index], source + index);
    }
    if (!separate) appendVertex(mesh, data, previous[3], source + 3u);
    else mesh.engineDraws.push_back({7u,first,8u});
}

void appendFrontSection(m::Mesh& mesh, const m::LightsaberNodeData& data,
                        const Edge& previous, const Edge& next, bool separate) {
    const auto first = mesh.vertices.size();
    const auto source = m::LightsaberNodeData::frontPieceOffset;
    if (!separate) appendVertex(mesh, data, previous[0], source);
    appendVertex(mesh, data, previous[0], source);
    appendVertex(mesh, data, next[0], source);
    for (std::size_t index = 1u; index < next.size(); ++index) {
        appendVertex(mesh, data, previous[index], source + index);
        appendVertex(mesh, data, next[index], source + index);
    }
    if (!separate) appendVertex(mesh, data, next[3], source + 3u);
    else mesh.engineDraws.push_back({7u,first,8u});
}

void appendBackCap(m::Mesh& mesh, const m::LightsaberNodeData& data,
                   const Edge& base, const Edge& tips, bool separate) {
    const auto first = mesh.vertices.size();
    const auto baseSource = m::LightsaberNodeData::backPieceOffset;
    const auto tipSource = baseSource + 4u;
    if (!separate) appendVertex(mesh, data, tips[0], tipSource);
    appendVertex(mesh, data, tips[0], tipSource);
    appendVertex(mesh, data, base[0], baseSource);
    for (std::size_t index = 1u; index < base.size(); ++index) {
        appendVertex(mesh, data, tips[index], tipSource + index);
        appendVertex(mesh, data, base[index], baseSource + index);
    }
    if (!separate) appendVertex(mesh, data, base[3], baseSource + 3u);
    else mesh.engineDraws.push_back({7u,first,8u});
}

void appendFrontCap(m::Mesh& mesh, const m::LightsaberNodeData& data,
                    const Edge& base, const Edge& tips, bool separate) {
    const auto first = mesh.vertices.size();
    const auto baseSource = m::LightsaberNodeData::frontPieceOffset;
    const auto tipSource = baseSource + 4u;
    if (!separate) appendVertex(mesh, data, base[0], baseSource);
    appendVertex(mesh, data, base[0], baseSource);
    appendVertex(mesh, data, tips[0], tipSource);
    for (std::size_t index = 1u; index < base.size(); ++index) {
        appendVertex(mesh, data, base[index], baseSource + index);
        appendVertex(mesh, data, tips[index], tipSource + index);
    }
    if (!separate) appendVertex(mesh, data, tips[3], tipSource + 3u);
    else mesh.engineDraws.push_back({7u,first,8u});
}

Edge capTips(const Edge& base, const Edge& previous,
             const m::Vec3& bladeVector, float bladeLength,
             float threshold, float fallbackSign) noexcept {
    Edge result;
    for (std::size_t index = 0u; index < result.size(); ++index) {
        const auto displacement = subtract(base[index], previous[index]);
        const auto displacementLength = std::sqrt(dot(displacement, displacement));
        const auto extension = displacementLength != 0.0f &&
                               threshold <= displacementLength
            ? multiply(displacement, bladeLength / displacementLength)
            : multiply(bladeVector, fallbackSign);
        result[index] = add(base[index], extension);
    }
    return result;
}

void buildQuadStripIndices(m::Mesh& mesh) {
    mesh.indices.reserve(mesh.vertices.size());
    for (std::uint32_t index = 0u; index < mesh.vertices.size(); ++index)
        mesh.indices.push_back(index);
}

void rebuildMovingMesh(LightsaberInstance& instance,
                       const LightsaberSceneSettings& settings) {
    auto& mesh = instance.movingMesh;
    mesh.vertices.clear();
    mesh.indices.clear();
    mesh.engineDraws.clear();
    const bool separate = instance.game == m::GameVersion::Kotor1;
    instance.usePieceMesh = settings.renderPieces && instance.historyCount <= 1u;
    if (!instance.data || instance.usePieceMesh) return;

    auto samples = logicalHistory(instance);
    const auto& data = *instance.data;
    const auto currentPosition = instance.frame.position;
    const auto currentOrientation = instance.frame.orientation;
    const auto currentInverse = m::engineQuaternionInverse(currentOrientation);
    if (samples.empty()) {
        // PartLightsaber starts with zeroed history storage. When renderpieces
        // is disabled, Draw still takes the generated-segment path before the
        // first movement sample exists, using the current Part transform.
        if (instance.game==m::GameVersion::Kotor1) samples.push_back(instance.history[
            (instance.writeIndex + kHistoryCapacity - 1u) % kHistoryCapacity]);
        else samples.push_back({0.0f, currentPosition, currentOrientation});
    }
    const auto schedule = segmentSchedule(samples);
    const auto bladeVector = subtract(data.positions[4u], data.positions[0u]);
    const auto bladeLength = std::sqrt(dot(bladeVector, bladeVector));
    const auto threshold = kFudgeFactor * bladeLength;

    // Historical/back half. K2 joins ten-vertex sections with repeated ends;
    // K1 opens/closes an eight-vertex strip for each section and cap.
    auto currentEdge = makeEdge(data, m::LightsaberNodeData::backPieceOffset,
                                samples[0], currentPosition, currentInverse);
    auto previousEdge = currentEdge;
    for (std::size_t pair = 0u; pair + 1u < samples.size() && pair < schedule.size();
         ++pair) {
        const auto& newer = samples[pair];
        const auto& older = samples[pair + 1u];
        for (const auto fraction : schedule[pair]) {
            LightsaberHistorySample interpolated;
            interpolated.position = interpolate(newer.position, older.position, fraction);
            interpolated.orientation = packedSlerp(
                newer.orientation, older.orientation, fraction);
            const auto nextEdge = makeEdge(
                data, m::LightsaberNodeData::backPieceOffset, interpolated,
                currentPosition, currentInverse);
            previousEdge = currentEdge;
            appendBackSection(mesh, data, nextEdge, previousEdge, separate);
            currentEdge = nextEdge;
        }
    }
    appendBackCap(mesh, data, currentEdge,
                  capTips(currentEdge, previousEdge, bladeVector, bladeLength,
                          threshold, 1.0f), separate);

    if (!settings.renderFrontHalf) {
        buildQuadStripIndices(mesh);
        return;
    }

    // The packed's enabled-by-default front half predicts future transforms
    // from the newest displacement and newest-two orientation delta.
    const auto velocity = samples.size() > 1u
        ? subtract(samples[0].position, samples[1].position) : m::Vec3{};
    const auto reference = subtract(data.positions[3u], data.positions[0u]);
    auto previousVector = rotate(multiply(currentInverse,
        samples.size() > 1u ? samples[1].orientation : samples[0].orientation),
        reference);
    auto predictedPosition = samples[0].position;
    auto predictedOrientation = samples[0].orientation;
    auto currentVector = rotate(
        multiply(currentInverse, predictedOrientation), reference);
    currentEdge = makeEdge(data, m::LightsaberNodeData::frontPieceOffset,
                           samples[0], currentPosition, currentInverse);
    previousEdge = currentEdge;
    for (std::size_t pair = 0u; pair + 1u < samples.size() && pair < schedule.size();
         ++pair) {
        const auto arc = rotationArc(previousVector, currentVector);
        const auto nextOrientation = multiply(predictedOrientation, arc);
        const auto nextPosition = add(predictedPosition, velocity);
        for (const auto fraction : schedule[pair]) {
            LightsaberHistorySample interpolated;
            interpolated.position = interpolate(
                predictedPosition, nextPosition, fraction);
            interpolated.orientation = packedSlerp(
                predictedOrientation, nextOrientation, fraction);
            const auto nextEdge = makeEdge(
                data, m::LightsaberNodeData::frontPieceOffset, interpolated,
                currentPosition, currentInverse);
            previousEdge = currentEdge;
            appendFrontSection(mesh, data, previousEdge, nextEdge, separate);
            currentEdge = nextEdge;
        }
        previousVector = currentVector;
        predictedPosition = nextPosition;
        predictedOrientation = nextOrientation;
        currentVector = rotate(
            multiply(currentInverse, predictedOrientation), reference);
    }
    appendFrontCap(mesh, data, currentEdge,
                   capTips(currentEdge, previousEdge, bladeVector, bladeLength,
                           threshold, -1.0f), separate);
    buildQuadStripIndices(mesh);
}

void clearRuntimeState(LightsaberInstance& instance) noexcept {
    instance.frame = {};
    instance.world = m::identityMatrix();
    instance.movingMesh.vertices.clear();
    instance.movingMesh.indices.clear();
    instance.movingMesh.engineDraws.clear();
    instance.writeIndex = 0u;
    instance.oldestIndex = 0u;
    instance.historyCount = 0u;
    instance.lastPosition = {};
    instance.active = false;
    instance.usePieceMesh = true;
    for (auto& sample : instance.history) sample = {};
}

void sample(LightsaberInstance& instance, const m::EngineFrame& frame,
            const m::Mat4& world, float elapsedSeconds, bool ageHistory) noexcept {
    instance.frame = frame;
    instance.world = world;
    instance.active = true;

    // Animate still samples GetWorld when paused or without a current scene;
    // only history aging requires IsAnimating (K1 Desktop 0x2dd019, K2 0xfd5e0).
    if (ageHistory && SceneAnimation::currentSceneAnimating() && instance.historyCount != 0u) {
        const auto ageDelta = std::max(kMinimumAgeStep, elapsedSeconds);
        std::uint16_t expired = 0u;
        auto cursor = instance.oldestIndex;
        for (std::uint16_t visited = 0u; visited < instance.historyCount; ++visited) {
            auto& entry = instance.history[cursor];
            entry.age += ageDelta;
            if (entry.age > kSegmentLife &&
                static_cast<int>(instance.historyCount) - static_cast<int>(expired) >= 2) {
                ++expired;
                instance.oldestIndex = static_cast<std::uint16_t>(
                    (instance.oldestIndex + 1u) % kHistoryCapacity);
            }
            cursor = static_cast<std::uint16_t>((cursor + 1u) % kHistoryCapacity);
        }
        instance.historyCount = static_cast<std::uint16_t>(instance.historyCount - expired);
    }

    // Animate copies GetWorld's quaternion directly into the history ring:
    // K2 Desktop 0xfd730 -> 0xfd81e, K1 Desktop 0x2dd119 -> 0x2dd1f0.
    const auto position = frame.position;
    const auto orientation = frame.orientation;
    // K1 Desktop 0x2dd12e returns on equal position without the K2 age refresh.
    if (equal(position, instance.lastPosition) &&
        (instance.game == m::GameVersion::Kotor1 ||
         instance.history[instance.oldestIndex].age <= kStationaryRefreshAge)) {
        return;
    }

    if (squaredDistance(position, instance.lastPosition) >
        kTeleportDistance * kTeleportDistance) {
        instance.writeIndex = 0u;
        instance.oldestIndex = 0u;
        instance.historyCount = 0u;
    }
    instance.lastPosition = position;
    auto& entry = instance.history[instance.writeIndex];
    entry.age = 0.0f;
    entry.position = position;
    entry.orientation = orientation;

    if (instance.historyCount != kHistoryCapacity) {
        instance.writeIndex = static_cast<std::uint16_t>(
            (instance.writeIndex + 1u) % kHistoryCapacity);
        ++instance.historyCount;
    }
}

void extend(m::Bounds& bounds, const m::Vec3& point) noexcept {
    if (!finite(point)) return;
    if (!bounds.valid) {
        bounds.minimum = bounds.maximum = point;
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

} // namespace

bool LightsaberScene::setSettings(LightsaberSceneSettings settings) {
    if (settings_ == settings) return false;
    settings_ = settings;
    for (auto& instance : instances_) {
        if (instance->active) rebuildMovingMesh(*instance, settings_);
        else instance->usePieceMesh = settings_.renderPieces;
    }
    ++revision_;
    return true;
}

void LightsaberScene::setDocument(const ModelDocument* document) {
    instances_.clear();
    document_ = document;
    ++revision_;
    if (!document_ || !document_->isOpen()) return;

    const auto addModel = [&](const m::Model& model,
                              std::optional<std::size_t> reference) {
        for (std::size_t nodeIndex = 0u; nodeIndex < model.nodes.size(); ++nodeIndex) {
            const auto& node = model.nodes[nodeIndex];
            if (!node.lightsaber || !node.lightsaber->postprocessed) continue;
            const auto& data = *node.lightsaber;
            if (data.positions.size() != m::LightsaberNodeData::generatedVertexCount ||
                data.normals.size() != data.positions.size() ||
                data.texcoords.size() != data.positions.size()) {
                continue;
            }
            auto instance = std::make_unique<LightsaberInstance>();
            instance->game = model.game;
            instance->reference = reference;
            instance->nodeIndex = nodeIndex;
            instance->data = &data;
            instance->modelFog = model.fog;
            instance->staticMesh = makeStaticMesh(data, nodeIndex, model.game);
            instance->movingMesh = makeMeshBase(nodeIndex);
            if(model.game==m::GameVersion::Kotor1) {
                // PartTriMesh::Mat supplies the authored ambient material to
                // the fixed-function saber draw, independently of its stream.
                const auto& materialMesh=model.meshes.at(*node.mesh);
                instance->staticMesh.ambient=materialMesh.ambient;
                instance->movingMesh.ambient=materialMesh.ambient;
            }
            clearRuntimeState(*instance);
            instances_.push_back(std::move(instance));
        }
    };

    addModel(document_->model(), std::nullopt);
    for (std::size_t index = 0u; index < document_->references().size(); ++index)
        addModel(document_->references()[index].model, index);
}

void LightsaberScene::reset() {
    for (auto& instance : instances_) clearRuntimeState(*instance);
    ++revision_;
}

void LightsaberScene::synchronize(const m::Pose& pose, bool includeReferences) {
    update(pose, 0.0f, includeReferences, false);
}

void LightsaberScene::advance(const m::Pose& pose, float elapsedSeconds,
                              bool includeReferences) {
    if (!finite(elapsedSeconds) || elapsedSeconds < 0.0f) elapsedSeconds = 0.0f;
    update(pose, elapsedSeconds, includeReferences, true);
}

void LightsaberScene::advancePrimary(const m::Pose& pose, float elapsedSeconds) {
    update(pose,elapsedSeconds,true,true,true,OwnerSelection::Primary);
}

void LightsaberScene::advanceReference(const m::Pose& pose, std::size_t reference,
                                       float elapsedSeconds) {
    update(pose,elapsedSeconds,true,true,true,OwnerSelection::References,reference);
}

void LightsaberScene::sampleConstructedOwners(const m::Pose& primaryPose) {
    for(auto& instance:instances_) {
        const auto& owner=instance->reference
            ? document_->references().at(*instance->reference).model : document_->model();
        // NewCAurObject Animate(0) reaches Part::Animate only for class0x10.
        // Collapsed room Parts never have a constructor Gob callback.
        if(!(owner.classification&0x10u)||(!instance->reference&&!primaryAnimated_))continue;
        const auto& pose=instance->reference
            ? document_->references().at(*instance->reference).pose : primaryPose;
        sample(*instance,m::enginePartWorld(owner,pose,{},instance->nodeIndex),
               pose.worldTransforms.at(instance->nodeIndex),0.0f,true);
    }
    refreshTransforms(primaryPose);
}

void LightsaberScene::refreshTransforms(const m::Pose& pose, bool includeReferences) {
    update(pose,0.0f,includeReferences,false,false);
}

void LightsaberScene::update(const m::Pose& pose, float elapsedSeconds,
                             bool includeReferences, bool ageHistory,
                             bool sampleHistory, OwnerSelection selection,
                             std::optional<std::size_t> selectedReference) {
    if (!document_ || !document_->isOpen()) return;
    const auto bases = includeReferences && !references_
        ? document_->referenceBases(pose)
        : std::vector<std::optional<m::Mat4>>{};
    const auto frames = includeReferences && !references_
        ? document_->referenceFrames(pose)
        : std::vector<std::optional<m::EngineFrame>>{};
    bool changed = false;
    for (auto& instance : instances_) {
        if(selection==OwnerSelection::Primary && instance->reference)continue;
        if(selection==OwnerSelection::References && !instance->reference)continue;
        if(selectedReference && instance->reference!=selectedReference)continue;
        const auto wasActive = instance->active;
        instance->active = false;
        std::optional<m::Mat4> world;
        m::EngineFrame frame;
        if (!instance->reference) {
            if (instance->nodeIndex < pose.worldTransforms.size()) {
                const auto base = references_ ? references_->mainWorld()
                                              : m::identityMatrix();
                const auto owner=m::multiply(base,m::nodeTransform({}, {},
                    references_ ? references_->ownerScale() : 1.0f));
                world = m::multiply(owner, pose.worldTransforms[instance->nodeIndex]);
                frame = m::enginePartWorld(document_->model(),pose,
                    references_ ? references_->mainFrame() : m::EngineFrame{},
                    instance->nodeIndex);
            }
        } else if (includeReferences && references_) {
            const auto* runtime = references_->instance(*instance->reference);
            if (runtime && runtime->active &&
                instance->nodeIndex < runtime->pose.worldTransforms.size()) {



                const auto scale=runtime->scale;
                const auto owner=m::multiply(runtime->world,m::nodeTransform({}, {},scale));
                world = m::multiply(owner,runtime->pose.worldTransforms[instance->nodeIndex]);
                frame = m::enginePartWorld(
                    document_->references()[*instance->reference].model,
                    runtime->pose,runtime->frame,instance->nodeIndex);
            }
        } else if (includeReferences && *instance->reference < frames.size() &&
                   frames[*instance->reference]) {
            const auto& reference = document_->references()[*instance->reference];
            if (instance->nodeIndex < reference.pose.worldTransforms.size()) {
                world = m::multiply(*bases[*instance->reference],
                    reference.pose.worldTransforms[instance->nodeIndex]);
                frame = m::enginePartWorld(reference.model,reference.pose,
                    *frames[*instance->reference],instance->nodeIndex);
            }
        }
        if (world) {
            if (!instance->reference && !primaryAnimated_) {
                instance->frame = frame;
                instance->world = *world;
                instance->active = true;
                instance->usePieceMesh = true;
            } else if(sampleHistory)sample(*instance,frame,*world,elapsedSeconds,ageHistory);
            else {instance->frame=frame;instance->world=*world;instance->active=true;}
            if (instance->active) rebuildMovingMesh(*instance, settings_);
            changed = true;
        } else if (wasActive) {
            changed = true;
        }
    }
    if (changed) ++revision_;
}

m::Bounds LightsaberScene::bounds() const noexcept {
    m::Bounds result;
    for (const auto& instance : instances_) {
        if (!instance->active) continue;
        for (const auto& vertex : instance->renderMesh().vertices)
            extend(result, m::transformPoint(instance->world, vertex.position));
    }
    return result;
}

} // namespace neomdl
