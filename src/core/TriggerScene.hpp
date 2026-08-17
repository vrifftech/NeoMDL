#pragma once

#include "core/ModelDocument.hpp"
#include "core/AreaVisualRecords.hpp"

#include <model/Trigger.hpp>
#include <model/SceneOrdering.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

struct RuntimeTriggerHit {
    std::size_t volumeIndex{};
    neomdl::model::TriggerHit hit;
};

enum class RuntimeTriggerSource { Manual, AreaGit };

// Explicit replay inputs for CSWCTrigger::UpdateTriggerColor. Loaded Type
// initializes the engine client selectors; later live calls may replace them.
struct TriggerClientVisualState {
    bool updateSuppressed{};       // engine +0x108 early-return gate
    bool areaTransitionMode{};     // engine +0x10c color selector
    bool displayEnabled{};         // engine +0x110 alpha selector
};

// Parallel provenance/state for TriggerScene::volumes(). The GIT ObjectId is
// retained as a serialized identity; it is not represented as a engine pointer.
struct RuntimeTriggerRecord {
    RuntimeTriggerSource source{RuntimeTriggerSource::Manual};
    std::optional<std::size_t> sourceIndex;
    std::uint64_t objectId{0x7f000000u};
    std::string label;
    std::string tag;
    std::string templateResref;
    std::string templateIdentity;
    std::int32_t type{};
    float authoredHighlightHeight{0.1f};
    TriggerClientVisualState clientVisualState{};
};

// PartTrigger is scene/game-object runtime state, not an MdlNodeTrigger
// instance. TriggerScene owns those externally supplied footprint volumes and
// separately reports the inert trigger markers present in the opened model.
class TriggerScene final {
public:
    void setDocument(const ModelDocument* document);

    void reset();
    void advance(float elapsedSeconds) noexcept;

    std::size_t createVolume(
        std::vector<neomdl::model::Vec3> footprint,
        std::array<float, 4> color = {{1.0f, 1.0f, 1.0f, 1.0f}},
        std::uintptr_t owner = 0u);
    bool removeVolume(std::size_t index);
    void clearVolumes();
    // Replaces only GIT-owned volumes and preserves explicit/manual volumes.
    bool replaceAreaTriggers(const std::vector<AreaTriggerRecord>& records,
                             std::string& error);
    void clearAreaTriggers();
    bool applyClientVisualState(std::size_t index,
                                const TriggerClientVisualState& state);
    bool applySpecialDisplay(std::size_t index, bool displayAll);

    bool setHighlighted(std::size_t index, bool highlighted, float height,
                        bool group = false);
    bool setHighlightParameters(
        std::size_t index,
        const neomdl::model::TriggerHighlightParameters& parameters);
    bool setColor(std::size_t index, std::array<float, 4> color);

    std::optional<RuntimeTriggerHit> hitTest(
        const neomdl::model::Vec3& start,
        const neomdl::model::Vec3& end,
        const neomdl::model::TriggerHitOptions& options = {}) const;

    void setGlobalAlpha(float alpha) noexcept;
    float globalAlpha() const noexcept { return globalAlpha_; }
    float scenePhase() const noexcept { return scenePhase_; }
    std::size_t modelMarkerCount() const noexcept { return modelMarkerCount_; }
    bool kotor2Runtime() const noexcept { return kotor2Runtime_; }
    bool runtimeSupported() const noexcept {
        return document_&&document_->isOpen();
    }
    model::GameVersion game() const noexcept {
        return document_?document_->model().game:model::GameVersion::Kotor2;
    }
    const std::vector<neomdl::model::TriggerVolume>& volumes() const noexcept {
        return volumes_;
    }
    const std::vector<RuntimeTriggerRecord>& records() const noexcept {
        return records_;
    }
    std::vector<neomdl::model::TriggerVolume>& volumes() noexcept {
        return volumes_;
    }
    neomdl::model::Bounds bounds() const noexcept;
    std::uint64_t revision() const noexcept { return revision_; }

private:
    const ModelDocument* document_{};
    bool kotor2Runtime_{};

    std::size_t modelMarkerCount_{};
    std::vector<neomdl::model::TriggerVolume> volumes_;
    std::vector<RuntimeTriggerRecord> records_;

    float globalAlpha_{};
    float scenePhase_{};
    std::uint64_t revision_{};
};

} // namespace neomdl
