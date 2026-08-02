#pragma once
#include "core/SceneOwners.hpp"
#include "core/SceneQueries.hpp"
#include <map>

namespace neomdl {
struct MotionBlurOwnerSettings {
    bool enabled{}; // Gob constructor: false, not inferred from motion or class
    float maximumLength{1.5f};
};
struct MotionBlurDraw {
    LightOwnerId owner{};
    std::size_t ordinal{};
    model::EngineFrame current, previous;
    model::Vec3 worldMotion{};
    float maximumLength{1.5f};
};




class MotionBlurScene final {
public:
    void replaceOwners(std::vector<SceneOwnerView> owners);
    const std::vector<SceneOwnerView>& owners() const noexcept { return owners_; }
    void setSettings(LightOwnerId owner,MotionBlurOwnerSettings settings);
    MotionBlurOwnerSettings settings(LightOwnerId owner) const;
    void beginFrame() noexcept;
    // Called only AFTER an ordinary Gob traversal (including its shells).
    // False means no blur append; disabled owners still commit their history.
    bool ordinaryRendered(LightOwnerId owner);
    const std::vector<LightOwnerId>& pending() const noexcept { return pending_; }
    std::size_t completedCount() const noexcept { return completed_; }
    MotionBlurDraw nextDraw() const;
    // A failed draw must not call this. Earlier completed owners stay committed.
    void completeBlur(const MotionBlurDraw& draw);
    void resetHistory(); // explicit cut; seeds current, does not change flags
    void clear() noexcept;
private:
    struct State {
        const model::Model* asset{};
        const model::Pose* pose{};
        model::EngineFrame current,previous;
        MotionBlurOwnerSettings settings;
        SceneOwnerLifetime ownerLifetime;
        std::function<model::EngineFrame()> resolveFrame;
        std::function<void(const model::EngineFrame&)> commitRenderedFrame;
    };
    State& state(LightOwnerId owner);
    const State& state(LightOwnerId owner) const;
    std::vector<SceneOwnerView> owners_;
    std::map<LightOwnerId,State> states_;
    std::vector<LightOwnerId> pending_;
    std::size_t completed_{};
};
} // namespace neomdl
