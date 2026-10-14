#pragma once

#include "core/CollisionAssets.hpp"

namespace neomdl {

struct BinaryMeshEdit {
    std::array<std::optional<std::string>,2> textures;
    // Common mesh header bytes +309..+313, in stored order.
    std::array<std::optional<bool>,5> flags;
};

struct BinaryModelFile {
    std::filesystem::path path;
    model::Model model;
    std::vector<std::uint8_t> mdl;
    std::optional<std::vector<std::uint8_t>> mdx;
    std::map<CollisionKind,std::vector<std::uint8_t>> collisions;
    std::vector<std::filesystem::path> inputs;
};

BinaryModelFile readBinaryModelFile(const std::filesystem::path& path,
    const neoshared::bif::JobControl& job = {});
std::string binaryMeshTexture(const BinaryModelFile& file, std::size_t node, std::size_t channel);
std::vector<CollisionOutput> binaryEditOutputs(const BinaryModelFile& file,
    const std::map<std::size_t,BinaryMeshEdit>& edits, const std::filesystem::path& output);

} // namespace neomdl
