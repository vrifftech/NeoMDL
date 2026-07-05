#pragma once
#include <material/Runtime.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace neomdl::material {
struct DirtyMaterialSettings {
    // Explicit dormant-caller inspection, NEVER automatic PartTriMesh routing.
    // Neither K2 GetRenderPath returns NoiseMapped (16).
    bool enabled{};
    unsigned slot{};
    int worldSpace{1}; // exactly one selects world; every other engine value selects local
    bool forceSight{},colorShift{};
    float desaturation{0.6f},forceAlpha{0.75f};
};
struct DirtyMaterialInputs {
    DirtyMaterialSettings settings;
    std::uint32_t texture{};
    int initializedFormula{}; // signed byte captured by Dirty Init, not live ARE state
};
enum class DirtyTextureRole { Diffuse, Environment, Lightmap, Volume };
struct DirtyBindingEvent { DirtyTextureRole role; unsigned unit; };
struct DirtyMaterialPlan {
    unsigned command{},cleanupCommand{32},volumeUnit{};
    bool lightmap{},environment{},add{},desaturate{},forceLinear{},samplesVolume{true};
    std::vector<DirtyBindingEvent> binds;
    bool fixedDispatch{};
};
// RenderNoiseMapped (Desktop 0xce7b8) -> EnableFragmentShadersARB (0xf64b4).
// Explicitly refuses engine no-op dispatches instead of inventing shaders.
bool makeDirtyMaterialPlan(const DirtyMaterialInputs& input,
                          bool lightmap,bool environment,DirtyMaterialPlan& plan,std::string& error,
                          bool legacyFragmentDispatcher=false);
std::array<float,3> dirtyWorldCoordinates(std::array<float,3> world) noexcept;
inline constexpr const char* dirtyTextureNames[3]={"dirtytex","dirtytex2","dirtytex3"};
} // namespace neomdl::material
