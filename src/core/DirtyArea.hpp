#pragma once
#include <material/ProceduralTexture.hpp>
#include <array>
#include <cstdint>
#include <string>
namespace neogff { class GffStruct; }
namespace neomdl {
struct DirtyAreaSlot {
    // GFF INT fields, including the signed ARGB bit pattern.
    std::int32_t argb{}, size{16}, formula{1}, function{};
};
struct DirtyAreaState {
    std::array<DirtyAreaSlot,3> slots;
    std::array<material::DirtyTextureInputs,3> controllerInputs() const;
};


DirtyAreaState readDirtyArea(const neogff::GffStruct& root);
std::string describeDirtyArea(const DirtyAreaState& state);
} // namespace neomdl
