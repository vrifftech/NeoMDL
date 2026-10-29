#pragma once

#include "core/CollisionAssets.hpp"
#include <set>

namespace neomdl {

inline bool editableBinaryMaterialController(const model::Controller& controller) {
    return controller.rowCount &&
        (controller.interpolation==model::ControllerInterpolation::Linear ||
         controller.interpolation==model::ControllerInterpolation::Bezier) &&
        ((controller.type==100u && controller.valueKind==model::ControllerValueKind::Vector) ||
         (controller.type==132u && controller.valueKind==model::ControllerValueKind::Float));
}

struct BinaryMeshEdit {
    std::array<std::optional<std::string>,2> textures;
    // Common mesh header bytes +308..+313, in stored order.
    std::array<std::optional<bool>,6> flags;
    std::map<std::size_t,std::vector<std::uint8_t>> headerFields;
    std::optional<bool> tangentBasis;
    bool regenerateTangents{};
    std::set<std::size_t> positionRows;
    bool transformChanged{},bindingChanged{};
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
void setBinaryStaticController(BinaryModelFile& file,std::size_t node,std::uint32_t type,
                               const std::vector<float>& values);
std::vector<CollisionOutput> binaryEditOutputs(const BinaryModelFile& file,
    const std::map<std::size_t,BinaryMeshEdit>& edits, const std::filesystem::path& output,
    const neoshared::bif::JobControl& job = {});

} // namespace neomdl
