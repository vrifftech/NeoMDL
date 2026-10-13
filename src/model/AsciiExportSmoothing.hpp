#pragma once

#include <model/Model.hpp>

namespace neomdl::model::ascii_export {

struct ReconstructedSmoothing {
    std::vector<std::uint32_t> masks;
    double squaredNormalError{};
    std::size_t changedCornerNormals{};
    float maxNormalComponentDifference{};
};

ReconstructedSmoothing reconstructSmoothing(
    const Mesh&, const std::vector<Vertex>& compiledVertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const neoshared::bif::JobControl&,
    const std::vector<Vec3>* cornerTargets = nullptr,
    bool refine = true,
    const std::vector<bool>* ignoredCorners = nullptr,
    const std::vector<std::uint32_t>* seedMasks = nullptr);

} // namespace neomdl::model::ascii_export
