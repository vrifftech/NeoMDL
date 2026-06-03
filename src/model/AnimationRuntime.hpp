#pragma once

#include <model/Model.hpp>
#include <model/UvAnimation.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neomdl::model {




inline constexpr std::uint32_t kAnimationRunNonLooping = 0x001u;
inline constexpr std::uint32_t kAnimationRunImmediate = 0x002u;
inline constexpr std::uint32_t kAnimationRunReportLoop = 0x004u;
inline constexpr std::uint32_t kAnimationRunOverlay = 0x008u;
inline constexpr std::uint32_t kAnimationRunSynchronizeTime = 0x010u;
inline constexpr std::uint32_t kAnimationRunQueued = 0x020u;
inline constexpr std::uint32_t kAnimationRunAllowDuplicate = 0x040u;
inline constexpr std::uint32_t kAnimationRunStop = 0x080u;
inline constexpr std::uint32_t kAnimationRunTraceControllers = 0x100u;
inline constexpr std::uint32_t kAnimationRunOutOfOrder = 0x200u;
inline constexpr std::uint32_t kAnimationRunForceStop = 0x400u;

struct AnimationRun {
    std::uint64_t id{};
    AnimationReference animation;
    std::string name;
    float playbackRate{1.0f};
    std::uint32_t flags{};
    float previousTime{};
    float time{};
    float weight{1.0f};
    float transitionTime{};
    float positionScale{1.0f};
    bool transitionComplete{};

    // OOAnimRun stores animation sample points separately from the normalized
    // control timeline. The first PlayOutOfOrderAnimation array is multiplied
    // by Animation::length by the constructor; these vectors retain the exact
    // runtime representation after that conversion.
    float outOfOrderDuration{};
    std::vector<float> outOfOrderAnimationTimes;
    std::vector<float> outOfOrderControlTimes;

    bool queued() const noexcept { return (flags & kAnimationRunQueued) != 0u; }
    bool overlay() const noexcept { return (flags & kAnimationRunOverlay) != 0u; }
    bool outOfOrder() const noexcept { return (flags & kAnimationRunOutOfOrder) != 0u; }
    bool looping() const noexcept { return (flags & kAnimationRunNonLooping) == 0u; }
};

enum class AnimationRuntimeEventKind : std::uint8_t {
    Started,
    Loop,
    Completed,
    AnimationEvent,
};

struct AnimationRuntimeEvent {
    AnimationRuntimeEventKind kind{AnimationRuntimeEventKind::AnimationEvent};
    std::uint64_t runId{};
    std::string animationName;
    float time{};
    std::string name;
    // A controller in the current Animate traversal has already set Gob's
    // sticky hierarchy-updated bit. This includes an earlier run with no
    // authored events. Clock-only admission leaves the owner bit unchanged.
    bool hierarchyUpdated{};
};

struct AnimationPlayResult {
    std::optional<std::uint64_t> runId;
    bool cleared{};
    std::string diagnostic;

    explicit operator bool() const noexcept {
        return runId.has_value() || cleared;
    }
};

// The desktop K1 and K2 schedulers share priority and run lifetime behavior.
inline constexpr bool engineAnimationRuntimeSupported(GameVersion game) noexcept {
    return game == GameVersion::Kotor1 || game == GameVersion::Kotor2;
}

// Stateful counterpart of Gob's AnimRun list. The list is stored in the
// same priority direction as the engine: index zero is highest priority, while
// Animate traverses it from the last (lowest-priority) run back to the first.
// Pose state persists between frames because NewController blends each sampled
// key against the current Part value rather than rebuilding from bind pose.
class AnimationRuntime final {
public:
    using EventCallback = std::function<void(const AnimationRuntimeEvent&)>;
    // Gob::DoEvent invokes the registered producer synchronously. The weak
    // owner identity implements the SafePointer checks in Gob::Animate; it
    // must expire when the owning Gob is destroyed, not when it leaves Scene.
    void setEventCallback(EventCallback callback,
                          std::weak_ptr<const void> ownerLifetime);
    void setModel(const Model* targetModel,
                  std::vector<const Model*> hierarchy,
                  const neoshared::bif::JobControl& job = {});
    void clear(bool resetToBindPose = true);

    bool valid() const noexcept { return targetModel_ != nullptr; }
    const Pose& pose() const noexcept { return pose_; }
    const std::vector<AnimationRun>& runs() const noexcept { return runs_; }
    bool empty() const noexcept { return runs_.empty(); }

    const AnimationRun* run(std::uint64_t id) const noexcept;
    AnimationRun* run(std::uint64_t id) noexcept;

    AnimationPlayResult play(const std::string& name,
                             float playbackRate = 1.0f,
                             std::uint32_t flags = 0u,
                             float startTime = 0.0f);
    AnimationPlayResult play(const AnimationReference& animation,
                             float playbackRate = 1.0f,
                             std::uint32_t flags = 0u,
                             float startTime = 0.0f);
    AnimationPlayResult playOutOfOrder(
        const std::string& name,
        const std::vector<float>& normalizedAnimationTimes,
        const std::vector<float>& controlTimes,
        float duration);
    AnimationPlayResult playOutOfOrder(
        const AnimationReference& animation,
        const std::vector<float>& normalizedAnimationTimes,
        const std::vector<float>& controlTimes,
        float duration);

    // graceful=true reproduces the 0x400 force-stop request: the run is marked
    // non-looping and fades with its transition. graceful=false reproduces the
    // 0x80 stop command and removes the run immediately.
    bool stop(std::uint64_t id, bool graceful = true);
    bool remove(std::uint64_t id);
    bool setRunTime(std::uint64_t id, float time, bool rebuildFromBind = true);
    bool setRunPlaybackRate(std::uint64_t id, float playbackRate) noexcept;
    bool setRunLooping(std::uint64_t id, bool looping) noexcept;

    // Advances all active runs by one Gob::Animate frame and rebuilds the pose
    // over the persistent current Part state. elapsedSeconds is clamped only by
    // callers; the engine math itself is preserved here.
    // Engine visibility/pause admission can retain clocks/events while omitting
    // AnimateHierarchy writes. Returns whether a controller was dispatched.
    bool advance(float elapsedSeconds, bool evaluatePose = true);
    // Gob::Animate's procedural traversal runs before animation controls and
    // consumes the scene RNG once per admitted vertex axis, without dt scaling.
    void advanceProceduralUv(ParticleRandom&, float elapsedSeconds);
    // Gob's generated LookAtAnimation is an owned, highest-priority overlay.
    // Its two-key controller still uses the ordinary engine run sampler.
    void beginLookAtAnimation(std::size_t sourceNode,float transitionTime);
    bool lookAtAnimationActive();
    void controlLookAtAnimation(Quaternion current,Quaternion desired,bool ending);

    // Recompose the current run samples. resetToBindPose is useful for an
    // explicit viewer seek; normal continuous playback must leave it false.
    void rebuildPose(bool resetToBindPose = false);

    // Observations only: represented engine effects have already been called.
    std::vector<AnimationRuntimeEvent> takeEvents();
    std::optional<std::uint64_t> primaryRunId() const noexcept;
    float primaryTime() const noexcept;

private:
    AnimationPlayResult playResolved(
        AnimationReference animation, std::string name,
        float playbackRate, std::uint32_t flags, float startTime,
        bool allowLookupSemantics,
        std::optional<std::uint64_t> exactReuseRun = std::nullopt);
    AnimationPlayResult playOutOfOrderResolved(
        const AnimationReference& animation,
        std::string name,
        const std::vector<float>& normalizedAnimationTimes,
        const std::vector<float>& controlTimes,
        float duration);
    const Animation* animationFor(const AnimationReference& reference) const noexcept;
    std::optional<std::size_t> runIndex(std::uint64_t id) const noexcept;
    bool emit(AnimationRuntimeEventKind kind, const AnimationRun& run,
              float time = 0.0f, std::string name = {},
              bool hierarchyUpdated = false);
    bool emitAnimationEvents(const AnimationRun& run, bool hierarchyUpdated);
    AnimationRun& borrowedRun(std::uint64_t id);
    void composeRuns();

    const Model* targetModel_{};
    std::vector<const Model*> hierarchy_;
    std::vector<AnimationRun> runs_;
    Pose pose_;
    std::vector<ProceduralUvPart> proceduralUvParts_;
    std::vector<AnimationRuntimeEvent> events_;
    EventCallback eventCallback_;
    std::weak_ptr<const void> eventOwnerLifetime_;
    std::uint64_t nextRunId_{1u};
    std::optional<Animation> lookAtAnimation_;
    std::optional<std::uint64_t> lookAtRun_;
};

const char* animationRunPriorityName(const AnimationRun& run) noexcept;
const char* animationRuntimeEventName(AnimationRuntimeEventKind kind) noexcept;

} // namespace neomdl::model
