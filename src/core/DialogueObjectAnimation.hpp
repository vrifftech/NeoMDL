#pragma once

#include <cstdint>
#include <string>

namespace neomdl {
namespace detail {

struct DialogueObjectAnimationProjection {
    std::uint16_t clientAnimation{};
    bool synthesizeCuts{},defaultOutsideCuts{};
    // SetAnimation's default branch issues PlayAnimation("",1,0,start).
    // This is a engine command, not a missing-name substitution.
    bool playEmpty{};
    std::uint32_t flags{};
};
DialogueObjectAnimationProjection dialogueObjectAnimation(
    std::uint16_t serverAnimation,bool door,std::int32_t doorSelector);

std::string dialogueObjectAnimationName(std::uint16_t serverAnimation,
    const DialogueObjectAnimationProjection&, const std::string& tableName);

} // namespace detail
} // namespace neomdl
