#pragma once

#include <model/Model.hpp>

namespace neomdl {

enum class NormalWeighting { Area, CornerAngle, AreaAngle, Equal };

struct MeshNormalOptions {
    NormalWeighting weighting{NormalWeighting::Area};
    double creaseDegrees{180.0};
    bool respectSmoothingGroups{true};
};

std::string recalculateMeshNormals(const std::string& text, const model::Model& model,
    std::size_t nodeIndex, const MeshNormalOptions& options,
    const neoshared::bif::JobControl& job = {});

} // namespace neomdl
