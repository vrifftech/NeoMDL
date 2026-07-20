#pragma once
#include "core/SceneQueries.hpp"
#include "model/AnimationRuntime.hpp"
#include <functional>

namespace neomdl {
struct LookAtTargetFrame {
    HitPartTransform part;
    model::Vec3 ownerPosition;
};
// Resolve stable owner/node identities at Control time. A destroyed target
// returns null, reproducing the engine SafePointer deactivation.
using LookAtTargetResolver=std::function<std::optional<LookAtTargetFrame>()>;
class LookAtAnimation final {
public:
    bool begin(model::AnimationRuntime&,const model::Model&,const std::string& sourceBone,
        LookAtTargetResolver,float horizontalArc,float verticalArc,float transition,float maximumDistance);
    void end() noexcept {deactivated_=true;}
    void clear() {target_={};}
    // Engine Control obtains the source Part frame directly, before rotating
    // its forward vector; a draw-matrix quaternion reassembly changes it.
    void control(model::AnimationRuntime&,const model::Model&,const model::EngineFrame& owner);
private:
    LookAtTargetResolver target_;
    std::size_t sourceNode_{};
    float horizontal_{},vertical_{},distance_{};
    bool deactivated_{},ending_{};
};
} // namespace neomdl
