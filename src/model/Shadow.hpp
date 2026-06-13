#pragma once
#include "model/Model.hpp"

namespace neomdl::model {

struct ShadowPlane {
    Vec3 normal{};
    float distance{}; // dot(normal, point) + distance; positive is clipped
};

// Resolved engine branch inputs, not inferred from mesh bounds or render flags.
struct ShadowProjectionSettings {
    float nearCapOffset{};
    bool disableVertexClipping{};
    bool clampReversedExtrusion{true};



    bool directPrimitivePath{};
};
struct ShadowProjectionInput {
    Vec3 localLight{};
    float projectionDistance{};
    ShadowPlane plane{};
    ShadowProjectionSettings settings{};
};








Mesh buildShadowVolume(const Mesh& mesh, const ShadowProjectionInput& input,
                       const DanglyMeshState* dangly = nullptr);

struct ShadowVolume {
    Mesh geometry;
    Mat4 world{identityMatrix()};
    std::size_t nodeIndex{};
    std::size_t sourceIndex{};
    std::size_t lightIndex{};
    bool beaming{}; // geometry-specific clipping flag
    // Gob::PartVolumeProject calls SetBeaming BEFORE virtual Project. Empty
    // geometry can therefore carry a state transition; dynamic mesh queries
    // do not call SetBeaming and leave this false. Preserve list order.
    bool beginsBeaming{};
};
struct GobShadowInput {
    EngineFrame ownerFrame{}; // engine Gob fields, including separate draw scale
    Vec3 worldLight{};
    float projectionDistance{};
    ShadowPlane worldPlane{};
    ShadowProjectionSettings settings{};
    // Part +0x2c is runtime enable state, NOT the authored mesh.render flag.
    // Null means the host has not disabled any part. A disabled part prunes
    // its whole subtree, matching the engine early return.
    const std::vector<std::uint8_t>* enabledParts{};
};




std::vector<ShadowVolume> buildGobShadowVolumes(
    const Model& model, const Pose* pose, const DanglySimulationState* dangly,
    const GobShadowInput& input);

struct MeshProjectionSource {
    const Node* node{};
    const Mesh* mesh{};
    Mat4 world{identityMatrix()}; // engine registered Part position/orientation
    const DanglyMeshState* dangly{};
};
struct MeshProjectionLight {
    Vec3 position{};
    float radius{};
    float shadowRadius{-1.0f};
};
struct MeshProjectionInput {
    ShadowPlane cameraFarPlane{}; // engine method adds 0.05 exactly once
    float minimumShadowLength{5.0f}; // engine shadowlength initial value
    ShadowProjectionSettings settings{};
};












std::vector<ShadowVolume> buildDynamicMeshProjections(
    const std::vector<MeshProjectionSource>& sources,
    const std::vector<MeshProjectionLight>& lights,
    const MeshProjectionInput& input);

// RenderStencilShadowsForThisLight: center + ten perimeter vertices,
// radius cap 50, byte-quantized half-weight alpha. No floor-receiver substitute.
Mesh buildShadowFadeFan(Vec3 scenePosition, Vec3 lightPosition,
                       float radius, float weight);

// Inverse rigid frame only. Scale is intentionally handled by the caller's
// separate render stack, never divided into the engine light or clip plane.
bool rigidShadowFrame(const Mat4& world, Vec3 worldLight, ShadowPlane worldPlane,
                      Vec3& localLight, ShadowPlane& localPlane) noexcept;

} // namespace neomdl::model
