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
    const TangentSmoothingSource* tangents = nullptr,
    const std::vector<std::uint8_t>* cornerRotations = nullptr,
    std::vector<std::vector<std::uint32_t>>* sharedCandidates = nullptr);

std::vector<std::vector<std::uint32_t>> sharedSeamSmoothingCandidates(
    const Mesh&, const std::vector<Vertex>& compiledVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const std::vector<bool>& seamCorners,
    const neoshared::bif::JobControl&,
    bool differentNormals = false,
    bool missingComponents = false);

std::vector<std::size_t> reconstructNormalFaceOrder(
    const Mesh&, const std::vector<Vertex>& compiledVertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const std::vector<std::uint8_t>& rotations,
    const std::vector<std::size_t>& currentOrder,
    const std::vector<Vec3>& positions,
    const neoshared::bif::JobControl&,
    const std::vector<std::array<double,3>>* precisePositions = nullptr);

struct ReconstructedNormalStars {
    std::vector<std::uint8_t> rotations;
    std::vector<std::size_t> faceOrder;
};

ReconstructedNormalStars reconstructNormalCornerStars(
    const Mesh&, const std::vector<Vertex>& compiledVertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const std::vector<std::uint8_t>& rotations,
    const std::vector<std::size_t>& faceOrder,
    const std::vector<Vec3>& positions,
    const neoshared::bif::JobControl&,
    const std::vector<std::array<double,3>>* precisePositions = nullptr,
    const std::vector<std::size_t>* movingFaces = nullptr,
    const std::vector<Vec3>* baselineCornerNormals = nullptr);

struct ReconstructedNormalRounding {
    std::vector<Vec3> positions;
    std::vector<std::uint8_t> rotations;
    std::vector<std::size_t> faceOrder;
};

ReconstructedNormalRounding reconstructNormalRounding(
    const Mesh&, const std::vector<Vertex>& compiledVertices,
    const std::vector<std::size_t>& sourceVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const std::vector<std::uint8_t>& rotations,
    const std::vector<std::size_t>& faceOrder,
    const std::vector<Vec3>& currentPositions,
    const neoshared::bif::JobControl&);

} // namespace neomdl::model::ascii_export
