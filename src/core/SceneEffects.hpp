#pragma once
#include <model/Model.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {
// Scene/Gob runtime inputs, not MDL/TXI fields. Disabled until a scene host
// explicitly supplies them. The desktop ATI and ARB paths remain distinct.
enum class SceneEffectTexture : unsigned char { FilmNoise, ForceDistortion, Dream, Fury };
struct SceneEffectsSettings {
    bool enabled{};
    bool scanNoise{},filmNoise{},saturationEnabled{},speedBlur{},dream{};
    // EnableDreamDistortion's retained bool (limited/full clairvoyance).
    // Separate from the renderer's global dialog letterbox rectangle below.
    bool dreamLimited{};
    // Present only for the VideoEffects.2da activation path. Engine
    // saturation parameters are copied verbatim; manual settings use the
    // existing host editor validation. -2 is DisableVideoEffect's retained ID.
    std::optional<int> engineVideoEffect;
    unsigned furyLevel{};
    bool forceSight{}; // independent engine global; excludes Desktop ATI speed history
    float forceSightDesaturation{0.65f}; // g_DesatureFactor
    float saturation{1};
    std::array<float,3> saturationMultiplier{{1,1,1}};
    float accumulationRatio{.98f};
    // Captured AuroraUseARBShaders (K2Desktop0x100c9f), independent of host GL.
    bool desktopArbShaders{true};
    bool desktopConvolution{true}; // visualizepass2 defaults to one in the Desktop image
    bool desktopOverbrighten{}; // visualizepass1 defaults to zero
    // Optional engine g_Letterbox_top / g_Letterbox_bottom pixel values for
    // the Desktop dream/fury quad. This is not an invented cinematic bar renderer.
    std::optional<std::array<int,2>> desktopDreamLetterbox;
    // Explicit camera cut/document/scene identity. Advancing this value drops
    // temporal history without restarting procedural textures or their clocks.
    std::uint64_t historyEpoch{};
    // Resources are supplied by the host/resolver. No generated neutral noise
    // or displacement is substituted for a missing engine input.
    std::string filmNoiseTexture,forceDistortionTexture,dreamTexture,furyTexture;
};
struct SceneEffectDimensions {
    int sceneWidth{},sceneHeight{},historyWidth{},historyHeight{};
    int captureWidth{},captureHeight{}; // K1/K2 Desktop ATI strict-next-power-of-two capture
};
unsigned engineDesktopAtiCaptureSize(unsigned drawableDimension) noexcept;
// Desktop ATI capture and ARB history target dimensions.
SceneEffectDimensions sceneEffectDimensions(int width,int height,
    model::GameVersion game=model::GameVersion::Kotor2,bool desktopArbShaders=true);
bool validateSceneEffects(const SceneEffectsSettings&,std::string&,
    model::GameVersion game=model::GameVersion::Kotor2);
std::vector<SceneEffectTexture> requiredSceneEffectTextures(const SceneEffectsSettings&,
    bool distortionProducers,model::GameVersion game=model::GameVersion::Kotor2);
const std::string& sceneEffectTextureName(const SceneEffectsSettings&,SceneEffectTexture);
const char* sceneEffectTextureLabel(SceneEffectTexture) noexcept;
// RenderSaturationAdjustmentARB 0x9febc: individual coefficients are min(1,x),
// except the final blue diagonal.
std::array<std::array<float,3>,3> desktopSceneSaturationMatrix(const SceneEffectsSettings&);
} // namespace neomdl
