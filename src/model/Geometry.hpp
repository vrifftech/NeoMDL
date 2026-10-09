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
    // deformed exactly; 0.0 means no authored basis is present.
    float tangentBasisMode{};
    std::array<float, 4> color{{1.0f, 1.0f, 1.0f, 1.0f}};
};
struct RenderGeometry {
    std::vector<RenderVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::size_t authoredTangents{};
};

// Copies source geometry and retained deformation pools without repairing
// attributes, generating tangent frames, or removing authored primitives.
RenderGeometry prepareRenderGeometry(const Mesh& mesh,
                                     const DeformedMesh* deformed = nullptr,
                                     bool engineAnimMeshLocal = false);

// Inverse-transpose of the upper-left 3x3, column-major. Unrepresentable
// inputs raise ModelError.
std::array<float,9> makeNormalMatrix(const Mat4& matrix);
float transformHandedness(const Mat4& matrix) noexcept;

} // namespace neomdl::model
