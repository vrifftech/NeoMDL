#pragma once

#include <material/Runtime.hpp>
#include <array>
#include <cstdint>

namespace neomdl::material {

struct ForceSightSettings {
    bool enabled{};
    float desaturation{0.65f}; // g_DesatureFactor
    std::uint32_t elapsedMilliseconds{}; // renderer's retained millisecond clock
};

struct ForceSightOwnerState {
    bool glow{}, nonForceBlending{};
    std::array<float,4> color{};
};



ForceSightOwnerState creatureForceSightOwner(std::int16_t alignment,std::uint16_t race) noexcept;

struct ForceSightPlan {
    unsigned command{};
    std::array<float,4> color{{0,0,0,1}};
    bool replaceBlend{}, oneSource{};
    bool illumination{};
};

// SetupForceSightShader: caller-supplied command pair and owner virtual state.
// No owner means the engine static-part branch, with force-color alpha one.
ForceSightPlan setupForceSight(const ForceSightSettings&,const ForceSightOwnerState*,
    unsigned glowCommand,unsigned desaturateCommand,bool preserveNonForceBlend,
    bool illumination);

} // namespace neomdl::material
