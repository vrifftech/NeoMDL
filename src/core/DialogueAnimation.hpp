#pragma once

#include "core/ResourceContext.hpp"
#include "model/AnimationRuntime.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace neomdl {
class ModelDocument;
struct CreatureAppearance;
struct CreatureAppearanceState;

enum class DialogueAnimationFamily : std::uint8_t { Base, Creature, Placeable, Door };
// CSWCAnimBase constructor values. The creature projection supplies the
// appearance/equipment producers; live movement and feats remain explicit.
struct DialogueAnimationContext {
    DialogueAnimationFamily family{DialogueAnimationFamily::Base};
    std::int32_t doorSelector{1};
    std::uint8_t creatureType{}, weaponClass{};
    float objectAnimationRate{1.0f}; // CSWCObject +0x4c constructor / live rate
    bool runLooksAwful{}, lightsaber{};
    // Constructors initialize K1 +0xdc / K2 +0xe8 to 0.
    // No independent producer is inferred.
    bool engineFieldE8{};
    std::optional<bool> feat207{false};
};
DialogueAnimationContext dialogueAnimationContext(const CreatureAppearance&,
    const CreatureAppearanceState&, const ResourceResolver&);

struct DialogueAnimationCommand {
    DialogueAnimationFamily family{DialogueAnimationFamily::Base};
    std::uint16_t serverAnimation{}, clientAnimation{};
    std::string name;
    bool fireForget{}, looping{}, dialog{}, dialogOverlay{}, playOutOfPlace{}, playEmpty{}, suppressed{}, engineOverlay{};
    float playbackRate{1.0f};
    std::uint32_t runFlags{};
    std::uint32_t fireForgetMilliseconds{};
    std::uint32_t overlayStopFlags{};
    std::string diagnostic;
    bool resolved() const noexcept { return (playEmpty || suppressed || !name.empty()) && diagnostic.empty(); }
};
// Uses engine numeric row indexing and the actor's ServerToClient projection,
// including engine cut-name synthesis and the engine default-name retry.
DialogueAnimationCommand resolveDialogueAnimation(std::uint16_t authored,
    const ResourceResolver&, const ModelDocument&,
    const DialogueAnimationContext& = {});
// UpdateShotCamera admits the engine dialog-animation predicates before
// reading dialoganimations[serverAnimation-10000].CU_PB_Range. Rejected
// animations leave the camera's retained pullback untouched (nullopt).
std::optional<float> dialogueCameraPullback(std::uint16_t serverAnimation,
    const ResourceResolver&);
// IsAnimationPlayingInDialog asks whether the current animation is a dialog
// animation on creatures only. Supply the retained current numeric animation
// predicate; model runs and head/viseme overlays do not determine that identity.
bool dialogueAnimationIsPlaying(const DialogueAnimationCommand&,
    bool currentDialog) noexcept;
// Engine object counter is independent of the model run. Expiration is a
// signed-negative result after wrapping DWORD subtraction, not <= 0.
bool advanceDialogueAnimationCounter(std::uint32_t& remainingMilliseconds,
    std::uint32_t elapsedMilliseconds) noexcept;
} // namespace neomdl
