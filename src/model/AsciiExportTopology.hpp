#pragma once

#include <model/Model.hpp>

namespace neomdl::model::ascii_export {

// Recover removed authored triangles retained by the compiler's unshifted
// plane/neighbor prefix before reconstructing smoothing connectivity.
void restoreCollapsedAuthoringFaces(Mesh& mesh);

struct ReconstructedTopology {
    std::vector<std::size_t> sourceVertices;
    std::vector<std::array<std::uint32_t,3>> sourceFaces;
    std::size_t changedAdjacency{};
};

// Each compiled row supplies the attributes that ASCII indexes through verts
// (weights, constraints and animation samples). Normals and UVs are independent
// for ordinary meshes; animmesh's explicit source normals belong in this key.
// Candidates retain edges alone, then add point joins sharing a compiled row,
// equal normals, or only source attributes. The caller compares rebuilt fields.
std::vector<ReconstructedTopology> reconstructTopologies(
    const Mesh& mesh,
    const std::vector<Vertex>& compiledVertices,
    const std::vector<std::vector<std::uint32_t>>& sourceAttributes,
    const neoshared::bif::JobControl& job);

} // namespace neomdl::model::ascii_export
