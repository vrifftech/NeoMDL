#include "core/CreatureLipSync.hpp"
#include <algorithm>

namespace neomdl {

std::optional<CreatureVisemes> creatureVisemes(const neoshared::lip::LipFile& lip,
                                              float lipDelay,model::GameVersion game) {
    // Endpoint comparisons and float32 constants occur in K2 Desktop
    // 0x28ac8c and K1 Desktop 0x7e315. Reject the empty table before
    // accessing the endpoints of this invalid engine memory domain.
    if(lip.frames.empty()) return std::nullopt;
    const float first=lip.frames.front().timeSeconds/lip.durationSeconds;
    const float last=lip.frames.back().timeSeconds/lip.durationSeconds;
    // UCOMISS/JAE rejects each ordered endpoint violation; an unordered
    // final key passes those comparisons and is overwritten with 1 below.
    if(first!=0.0f || last>=1.0000100135803223f || last<=0.9999899864196777f)
        return std::nullopt;
    CreatureVisemes result;
    result.duration=lip.durationSeconds;
    result.animationTimes.reserve(lip.frames.size());
    result.controlTimes.reserve(lip.frames.size());
    for(const auto& key:lip.frames) {
        result.animationTimes.push_back((static_cast<float>(key.shape)+1.0f)*0.0625f);
        result.controlTimes.push_back(game==model::GameVersion::Kotor1
            ? key.timeSeconds/lip.durationSeconds
            : (key.timeSeconds+lipDelay)/lip.durationSeconds);
    }
    result.controlTimes.back()=1.0f;
    return result;
}

void stopCreatureVisemes(model::AnimationRuntime& animation) {
    if(std::any_of(animation.runs().begin(),animation.runs().end(),
        [](const auto& run){return run.outOfOrder();}))
        animation.play("talk",1.0f,model::kAnimationRunForceStop,0.0f);
}

model::AnimationPlayResult playCreatureVisemes(model::AnimationRuntime& animation,
                                               const CreatureVisemes& visemes) {
    return animation.playOutOfOrder("talk",visemes.animationTimes,
                                     visemes.controlTimes,visemes.duration);
}

} // namespace neomdl
