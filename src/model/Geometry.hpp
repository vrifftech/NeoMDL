#pragma once
#include "model/Model.hpp"

namespace neomdl::model {

// API-neutral interleaved vertex stream. No GL or wxWidgets dependency.
struct RenderVertex {
    Vec3 position{};
    Vec3 normal{};
    Vec3 rawNormal{}; // engine shell input; ordinary skin normals are normalized separately
    Vec2 texcoord0{};
    Vec2 texcoord1{};
    Vec3 tangent{};
    float tangentHandedness{1.0f};
    Vec3 bitangent{};
    Vec3 basisNormal{};
    // 1.0 means the three authored K2 MDX basis vectors were preserved and
    // deformed exactly; 0.0 means NeoMDL generated an orthonormal fallback.
    float tangentBasisMode{};
    std::array<float, 4> color{{1.0f, 1.0f, 1.0f, 1.0f}};
};
struct RenderGeometry {
    std::vector<RenderVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::size_t authoredTangents{};
    std::size_t generatedTangents{};
    std::size_t skippedTriangles{};
};

// Source data remains unchanged. Valid K2 skin bases retain the exact weighted
// bitangent/tangent/normal vectors produced by cNewSkinBumpyVP. Static or
// malformed bases use the existing safe orthonormal viewer fallback; that
// fallback is deliberately identified per vertex rather than presented as the
// K2 tangent generator.
RenderGeometry prepareRenderGeometry(const Mesh& mesh,
                                     const DeformedMesh* deformed = nullptr,
                                     bool engineAnimMeshLocal = false);

// Inverse-transpose of the upper-left 3x3, column-major. Singular/non-finite
// input returns false and identity, avoiding NaN uniforms during zero scales.
bool makeNormalMatrix(const Mat4& matrix, std::array<float, 9>& result) noexcept;
float transformHandedness(const Mat4& matrix) noexcept;

} // namespace neomdl::model
