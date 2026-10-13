#pragma once

#include <model/Model.hpp>

namespace neomdl::model::ascii_export {

struct ReconstructedSmoothing {
    std::vector<std::uint32_t> masks;
    double squaredNormalError{};
    std::size_t changedCornerNormals{};
    float maxNormalComponentDifference{};
};

struct TangentSmoothingSource {
    const std::vector<Vec3>& firstCornerNormals;
    const std::vector<std::array<std::int16_t,3>>& cornerRows;
    bool useGeneratedWriters{true};
};

ReconstructedSmoothing reconstructSmoothing(
    const Mesh&, const std::vector<Vertex>& compiledVertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const neoshared::bif::JobControl&,
    const std::vector<Vec3>* cornerTargets = nullptr,
    bool refine = true,
    const std::vector<bool>* ignoredCorners = nullptr,
    const std::vector<std::uint32_t>* seedMasks = nullptr,
    bool useStoredRows = false,
    const TangentSmoothingSource* tangents = nullptr);

} // namespace neomdl::model::ascii_export
