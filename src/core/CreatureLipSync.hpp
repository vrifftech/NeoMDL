#pragma once

#include "model/AnimationRuntime.hpp"
#include <neoshared/lip/LipFile.hpp>
#include <optional>
#include <vector>

namespace neomdl {

struct CreatureVisemes {
    float duration{};
    std::vector<float> animationTimes;
    std::vector<float> controlTimes;
};

// CSWCCreature::LipSync: K1 Desktop 0x7e1ba; K2 Desktop i386 0x28ac8c.
// LipDelay is K2's Sound Options input (default 0);
// K1 has no delay addition and normalizes the original key time directly.
std::optional<CreatureVisemes> creatureVisemes(const neoshared::lip::LipFile&,
    float lipDelay = 0.0f, model::GameVersion game = model::GameVersion::Kotor2);
// Gob::StopOutOfOrderAnimation: K1 Desktop 0x2dcd88. The talk run
// receives a force-stop request only when an out-of-order run is present.
void stopCreatureVisemes(model::AnimationRuntime&);
model::AnimationPlayResult playCreatureVisemes(model::AnimationRuntime&,
                                               const CreatureVisemes&);

} // namespace neomdl
