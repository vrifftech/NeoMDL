#pragma once

#include <model/Model.hpp>

namespace neomdl::model {

struct PackedExportResult {
    std::vector<std::uint8_t> mdl;
    std::vector<std::uint8_t> mdx;
};

// Compile an ASCII-built asset into the selected game's 32-bit MDL/MDX
// format. ASCII material processing must be finalized (ModelDocument does
// this with its resource resolver). This consumes retained authoring data,
// not the viewer's draw pose.
// Material-dependent vertex channels must be finalized before compiling.
// It is not a lossless rewriter for previously decoded packed models.
PackedExportResult exportPackedModel(const Model& model,
                                    const neoshared::bif::JobControl& job = {});

} // namespace neomdl::model
