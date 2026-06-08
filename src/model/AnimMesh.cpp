#include <model/AnimMesh.hpp>
#include <model/EngineArithmetic.hpp>
#include <algorithm>
#include <limits>

namespace neomdl::model {
namespace {
void require(bool value, const char* message) {
    if (!value) throw ModelError(std::string("Engine animmesh: ") + message);
}
float blendValue(float old, float value, float weight) {
    return old * (1.0f - weight) + value * weight;
}
Vec3 blendValue(Vec3 a, Vec3 b, float t) {
    return {blendValue(a.x,b.x,t),blendValue(a.y,b.y,t),blendValue(a.z,b.z,t)};
}
Vec2 blendValue(Vec2 a, Vec2 b, float t) {
    return {blendValue(a.x,b.x,t),blendValue(a.y,b.y,t)};
}
template<class V>
void channel(const std::vector<V>& samples, std::int32_t count,
             std::size_t vertices, float time, float period, float weight,
             std::int32_t& frame, std::vector<V>& output, bool& defined) {
    require(count >= 0, "negative sample count");
    require(count == 0 || vertices <= std::numeric_limits<std::size_t>::max() /
            static_cast<std::size_t>(count), "sample dimensions overflow host address space");
    require(samples.size() == vertices * static_cast<std::size_t>(count),
            "sample pool size does not match vertex-major dimensions");
    require(output.size() >= vertices, "source vertex count exceeds the retained pool");
    if (count == 0 || vertices == 0) return;
    require(defined || weight == 1.0f,
            "partial-weight control reads an unwritten engine sample pool");
    const bool interpolate = frame >= 0 && frame < count - 1;
    float fraction = 0.0f;
    if (interpolate) {
        // Engine division and interpolation preserve NaN/Inf, including a
        // zero period with multiple samples. They do not choose a frame rate.
        fraction = time / period - static_cast<float>(frame);
    } else {
        frame = std::clamp(frame, std::int32_t{0}, count - 1);
    }
    for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
        const auto at = vertex * static_cast<std::size_t>(count) + static_cast<std::size_t>(frame);
        V value = samples[at];
        if (interpolate) {
            value = blendValue(value, samples[at+1], fraction);
        }
        // Weight 1 defines a newly allocated engine pool. We do not model
        // allocator garbage or depend on the value of an unread allocation.
        output[vertex] = defined ? blendValue(output[vertex], value, weight) : value;
    }
    // Control loops over the source node's count, which may be smaller than
    // the constructed target pool. Preserve its untouched tail and never mark
    // an unwritten tail as initialized by a prefix-only sample.
    defined = defined || vertices == output.size();
}
}

void controlAnimMesh(const AnimMeshData& data, float secondTime, float blend,
                     AnimMeshState& state) {
    require(data.vertexCount <= 32767u, "vertex count exceeds the signed engine pool consumer");
    std::int32_t frame = 0;
    if (data.samplePeriod != 0.0f) {
        // The desktop K1/K2 frame conversion uses CVTTSS2SI.
        // Unordered periods take this path too (Desktop 0x2df9cd/0x2df9cf).
        // Their invalid/overflow results then enter the engine frame clamp.
        frame = engineFloatToInt(secondTime / data.samplePeriod);
    }
    auto pending = state;
    channel(data.positionSamples, data.positionSampleCount, data.vertexCount,
            secondTime, data.samplePeriod, blend, frame,
            pending.localPositions, pending.positionsDefined);
    // Deliberately reuse frame AFTER position clamping; do not recalculate it.
    channel(data.texcoordSamples, data.texcoordSampleCount, data.vertexCount,
            secondTime, data.samplePeriod, blend, frame,
            pending.texcoords, pending.texcoordsDefined);
    state = std::move(pending);
}

AnimMeshState initializeAnimMesh(const AnimMeshData& data) {
    AnimMeshState state;
    state.localPositions.resize(data.vertexCount);
    state.texcoords.resize(data.vertexCount);


    controlAnimMesh(data, 0.0f, 1.0f, state);
    return state;
}

void controlAnimMeshNode(const Model& model, const Node& source,
                         std::size_t targetNode, float secondTime, float blend,
                         Pose& pose) {
    if (!source.animmesh) return;
    require(targetNode < model.nodes.size(), "synchronized target is outside the model");
    const auto& node = model.nodes[targetNode];
    require(node.animmesh && node.mesh && *node.mesh < pose.deformedMeshes.size(),
            "animation node does not target a constructed PartAnimMesh");
    auto& target = pose.deformedMeshes[*node.mesh];
    require(target.animmesh.has_value(), "missing per-instance sample pools");
    require(node.animmesh->vertexCount >= source.animmesh->vertexCount,
            "animation vertex count exceeds the constructed target pool");
    controlAnimMesh(*source.animmesh, secondTime, blend, *target.animmesh);
}

void rebuildAnimMeshGeometry(const Model& model, Pose& pose) {
    for (std::size_t i=0; i<model.meshes.size(); ++i) {
        const auto& mesh=model.meshes[i];
        if (!mesh.animated) continue;
        require(mesh.nodeIndex < model.nodes.size() && mesh.nodeIndex < pose.worldTransforms.size(),
                "missing part draw frame");
        const auto& node=model.nodes[mesh.nodeIndex];
        require(node.animmesh.has_value(), "missing decoded animmesh extension");
        require(node.animmesh->vertexCount == mesh.vertices.size(), "render vertex count differs from sample pools");
        require(i<pose.deformedMeshes.size(), "missing per-mesh instance state");
        auto& output=pose.deformedMeshes[i];
        if (!output.animmesh) output.animmesh=initializeAnimMesh(*node.animmesh);
        const auto& state=*output.animmesh;
        require(state.localPositions.size()==mesh.vertices.size() && state.texcoords.size()==mesh.vertices.size(),
                "invalid retained sample pool size");
        output.valid=state.positionsDefined;
        output.animMeshPartFrame=pose.worldTransforms[mesh.nodeIndex];
        output.positions.clear();output.normals.clear();
        if (!output.valid) continue; // undefined is explicit, never rigid MDX geometry
        output.positions.reserve(mesh.vertices.size());output.normals.reserve(mesh.vertices.size());
        for (std::size_t j=0;j<mesh.vertices.size();++j) {
            output.positions.push_back(transformPoint(output.animMeshPartFrame,state.localPositions[j]));
            // Inspection normals only. Engine rendering uses the untouched
            // authored MDX normal and the engine part matrix; no face normals
            // are generated from animated positions.
            output.normals.push_back(transformDirection(output.animMeshPartFrame,mesh.normalAt(j)));
        }
    }
}
} // namespace neomdl::model
