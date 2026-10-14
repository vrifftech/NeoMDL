#pragma once

#include "core/ResourceContext.hpp"
#include <model/Model.hpp>

namespace neomdl {

// Shared material/TXI-dependent ASCII construction, without image decoding.
void finalizeAsciiMaterials(model::Model& model, const ResourceResolver& resolver,
                            const neoshared::bif::JobControl& job = {});

} // namespace neomdl
