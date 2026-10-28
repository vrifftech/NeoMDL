#pragma once

#include <model/Model.hpp>
#include <map>

namespace neomdl::model::ascii_export {


struct SourceFaceNeighbors {
    using Neighbor=std::pair<bool,std::size_t>;
    struct EdgeNeighbors {
        std::vector<std::size_t> faces;
        std::vector<Neighbor> neighbors;
    };
    bool animated{};
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::vector<std::size_t>> edges;
    std::vector<EdgeNeighbors> expected;
    static std::vector<std::size_t> ranks(const std::vector<std::size_t>& order);
    Neighbor neighbor(const std::vector<std::size_t>& incident,std::size_t face,
        const std::vector<std::size_t>& order,const std::vector<std::size_t>& rank) const;
    SourceFaceNeighbors(const Mesh& mesh,const std::vector<std::array<std::uint32_t,3>>& faces,
        const std::vector<std::size_t>& order);
    bool preserves(const std::vector<std::size_t>& order,const std::vector<std::size_t>& rank) const;
};

struct ReconstructedSmoothing {
    std::vector<std::uint32_t> masks;
    double squaredNormalError{};
    std::size_t changedCornerNormals{};
    float maxNormalComponentDifference{};
};

struct TangentSmoothingSource {
    const std::vector<TangentBasis>& faceBases;
    const std::vector<std::size_t>& faceOrder;
    const std::vector<std::array<std::uint16_t,3>>& cornerRows;
    bool useGeneratedWriters{true};
    bool roundNormalScores{};
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
    std::vector<std::vector<std::uint32_t>>* sharedCandidates = nullptr,
    const std::vector<std::array<double,3>>* precisePositions = nullptr);

struct SeamSmoothingCandidate {
    std::size_t source;
    std::vector<std::uint32_t> masks;
};

std::vector<SeamSmoothingCandidate> sharedSeamSmoothingCandidates(
    const Mesh&, const std::vector<Vertex>& compiledVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const std::vector<bool>& seamCorners,
    const neoshared::bif::JobControl&,
    bool differentNormals = false,
    bool missingComponents = false);

std::vector<SeamSmoothingCandidate> retainedRowSmoothingCandidates(
    const Mesh&, const std::vector<Vertex>& compiledVertices,
    const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::uint32_t>& masks,
    const neoshared::bif::JobControl&);

bool preservesSourceFaceNeighbors(
    const Mesh&, const std::vector<std::array<std::uint32_t,3>>& sourceFaces,
    const std::vector<std::size_t>& currentOrder,
    const std::vector<std::size_t>& candidateOrder);

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
