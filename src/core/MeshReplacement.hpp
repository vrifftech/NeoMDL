#pragma once

#include <model/AsciiExport.hpp>

#include <map>

namespace neomdl {

struct MeshDonor {
    model::Model model;
    std::filesystem::path path;
    std::vector<std::filesystem::path> inputs;
};

struct MeshReplacement {
    std::string target, mesh;
    std::shared_ptr<const MeshDonor> donor;
};

MeshDonor readMeshDonor(const std::filesystem::path& path, model::GameVersion asciiGame,
                       const neoshared::bif::JobControl& job = {});

// Case-insensitive, unique node names only; replacements retain the node type.
std::map<std::string, std::size_t> namedMeshes(const model::Model& model,
                                            std::optional<std::uint16_t> flags = {});

// Copies mesh payloads in target-local coordinates. Names, hierarchy and all
// controllers stay with the target. Skin weights bind by name to its skeleton.
// Repeated targets use the last mapping. No files or input models are modified.
model::AsciiExportResult replaceMeshes(const std::string& text, const model::Model& target,
    const std::vector<MeshReplacement>& mappings, bool advanced,
    const neoshared::bif::JobControl& job = {});

} // namespace neomdl
