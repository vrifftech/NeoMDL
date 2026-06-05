#pragma once

#include <model/Model.hpp>

namespace neomdl::model {

// Contextual light controller IDs. Never dispatch these on mesh/emitter nodes.
inline constexpr std::uint32_t kControllerLightColor = 76u;
inline constexpr std::uint32_t kControllerLightRadius = 88u;
inline constexpr std::uint32_t kControllerLightShadowRadius = 96u;
inline constexpr std::uint32_t kControllerLightVerticalDisplacement = 100u;
inline constexpr std::uint32_t kControllerLightMultiplier = 140u;

NodeLightState bindNodeLight(const Node& node) noexcept;

enum class FogMode : std::uint8_t { Disabled, Linear, Exponential, ExponentialSquared };
struct FogSettings {
    FogMode mode{FogMode::Disabled};
    Vec3 color{0.15f,0.16f,0.18f};
    float start{10.0f};
    float end{100.0f};
    float density{0.025f};
    bool respectModelFlag{true};
};
bool validFog(const FogSettings& fog) noexcept;
FogSettings fogForModel(const FogSettings& scene, bool modelFog) noexcept;

} // namespace neomdl::model
