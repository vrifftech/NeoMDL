#pragma once

#include <model/Model.hpp>
#include <material/ForceSight.hpp>
#include <array>
#include <string>

namespace neomdl {
// Runtime Gob state, never serialized as MDL fields. Each main/reference owner
// has its own instance. Enabling an effect applies to that owner's part tree,
// not to a separately owned reference, particle chunk, or area room.
struct OwnerVisualEffects {
    material::ForceSightOwnerState forceSight;
    bool hologram{};
    bool distortion{}; // Gob::EnableDistortion; hologram admission takes precedence
    bool colorShiftEnabled{};
    std::array<float,4> colorShift{{1,1,1,1}};
    // Gob/model-instance material alpha and runtime environment-map override.
    float instanceOpacity{1.0f};
    std::string environmentMap;
    bool illuminationEnabled{};
    std::array<float,3> illumination{}; // additive material emission
    bool perObjectIlluminationEnabled{};
    std::array<float,3> perObjectIllumination{{0.5f,0.5f,0.5f}};
    std::string shellTexture;
    float shellAmount{}; // zero disables; signed engine displacement
};
enum class EngineOwnerBucket { Ordinary, Hologram, Distortion, Lightsaber };


inline EngineOwnerBucket engineOwnerBucket(const OwnerVisualEffects& owner,
                                           bool runtimeModelDistortion=false) noexcept {
    if(owner.hologram)return EngineOwnerBucket::Hologram;
    return owner.distortion||runtimeModelDistortion?EngineOwnerBucket::Distortion:EngineOwnerBucket::Ordinary;
}
// K1 Desktop ManageSceneBSP 0x3612fe has only the explicit distortion flag and
// Model+0x50 bit 1. For K1, InputPacked::Reset 0x32223e and NewCAurObject
// 0x32f5b4 prove that this is the loaded MDL classification unsigned short;
// callers supply classification&0x02. K2 adds the earlier hologram admission.
inline EngineOwnerBucket engineOwnerBucket(model::GameVersion game,
                                           const OwnerVisualEffects& owner,
                                           bool runtimeModelDistortion=false) noexcept {
    if(game==model::GameVersion::Kotor2)return engineOwnerBucket(owner,runtimeModelDistortion);
    return owner.distortion||runtimeModelDistortion?EngineOwnerBucket::Distortion:EngineOwnerBucket::Ordinary;
}
// The loaded classification producer is verified for K1. Preserve the
// existing explicit runtime-flag input for K2 callers.
inline EngineOwnerBucket engineOwnerBucket(const model::Model& asset,
                                           const OwnerVisualEffects& owner) noexcept {
    const auto bucket=engineOwnerBucket(asset.game,owner,asset.game==model::GameVersion::Kotor1 &&
        (asset.classification&0x02u)!=0u);
    if(bucket==EngineOwnerBucket::Ordinary && asset.game==model::GameVersion::Kotor1 &&
       (asset.classification&0x10u)!=0u)return EngineOwnerBucket::Lightsaber;
    return bucket;
}
} // namespace neomdl
