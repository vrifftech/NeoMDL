#pragma once

#include <model/AsciiExport.hpp>

namespace neomdl {

// Freeze a skin at its constructed bind pose, or retain a saber's stationary
// blade pieces, as an ordinary mesh. Node transforms and controllers stay.
std::string convertMeshToTrimesh(const std::string& text, const model::Model& model,
    std::size_t nodeIndex, bool advanced, const neoshared::bif::JobControl& job = {});

} // namespace neomdl
