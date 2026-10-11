#pragma once

#include <model/Model.hpp>

namespace neomdl::model::ascii_export {

struct ReconstructedTopology {
    std::vector<std::size_t> sourceVertices;
    std::vector<std::array<std::uint32_t,3>> sourceFaces;
    std::size_t changedAdjacency{};
};

// Each compiled row supplies the attributes that ASCII indexes through verts
// (weights, constraints and animation samples). Normals and UVs are independent
// for ordinary meshes; animmesh's explicit source normals belong in this key.
// Candidates retain edges alone, then also point joins supported by a compiled
// row or equal retained normals. The caller compares reconstructed field errors.
std::vector<ReconstructedTopology> reconstructTopologies(
    const Mesh& mesh,
    const std::vector<Vertex>& compiledVertices,
    const std::vector<std::vector<std::uint32_t>>& sourceAttributes,
    const neoshared::bif::JobControl& job);

} // namespace neomdl::model::ascii_export
