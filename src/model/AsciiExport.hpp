#pragma once

#include <model/Model.hpp>

namespace neomdl::model {

struct AsciiExportOptions {
    // Unset retains each mesh's mode; compiled meshes default to smoothing.
    std::optional<bool> explicitNormals;
};

struct AsciiExportResult {
    std::string text;
    // reassembly differences are also recorded as comments in the text.
    std::vector<std::string> warnings;
};

// Exports one model asset, including its own animations. References and the
// supermodel remain resource names; scene instances and live state are not
// flattened into the authoring model. No packed MDL/MDX writer is involved.
AsciiExportResult exportAsciiModel(const Model& model,
                                  const neoshared::bif::JobControl& job = {},
                                  const AsciiExportOptions& options = {});

} // namespace neomdl::model
