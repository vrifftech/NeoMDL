#pragma once

#include <model/Model.hpp>

namespace neomdl::model {

void initializeAsciiSkinSeamPools(Model& model);
void processAsciiSkinSeams(const Model& model,const neoshared::bif::JobControl& job = {});

// ASCII MDL has no packed layout token identifying the game. Its resource
// owner supplies the game explicitly, as the engine loader does.
// Packed authoring records use the x86 ABI for both games. Working K2
// controller and typed-node parsing is shared with K1; K2-only material
// extensions remain gated by the target game. The resource callback supplies
// .mdl/.mdx superclass resources at setsupermodel, as well as .lmt sidecars.
Model decodeAsciiModelBytes(const std::vector<std::uint8_t>& bytes,
                            GameVersion game,
                            const std::string& sourceLabel = {},
                            const neoshared::bif::JobControl& job = {},
                            const AsciiResourceResolver& resources = {});
// Hold the primary model before constructing its reference objects. The
// document calls this once; each successful reference lookup holds its own
// selected registration even when earlier occurrences used the same name.
void retainAsciiModelInstance(const Model& model);
std::optional<Model> resolveAsciiModelInstance(const Model& owner,
                            const std::string& name,
                            const neoshared::bif::JobControl& job = {},
                            const AsciiResourceResolver& resources = {});
// Synchronize the geometry built by the current beginmodelgeom block. The
// caller has already seeded that block's node counter from the superclass.
void synchronizeAsciiModelTree(Model& model, const Model& supermodel);
void rebuildAsciiTangentBasis(Mesh& mesh, const neoshared::bif::JobControl& job = {});

} // namespace neomdl::model
