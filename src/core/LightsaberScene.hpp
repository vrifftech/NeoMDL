#pragma once

#include "core/ModelDocument.hpp"
#include <model/SceneOrdering.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace neomdl {

class ReferenceScene;
struct HitOwner;

struct LightsaberHistorySample {
    float age{};
    neomdl::model::Vec3 position{};
    neomdl::model::Quaternion orientation{};
};

struct LightsaberSceneSettings {
    // Defaults from the engine game packed globals:
    // renderpieces=1 and lightsaberrenderfronthalf=1.
    bool renderPieces{true};
    bool renderFrontHalf{true};

    friend bool operator==(const LightsaberSceneSettings& left,
                           const LightsaberSceneSettings& right) noexcept {
        return left.renderPieces == right.renderPieces &&
               left.renderFrontHalf == right.renderFrontHalf;
    }
    friend bool operator!=(const LightsaberSceneSettings& left,
                           const LightsaberSceneSettings& right) noexcept {
        return !(left == right);
    }
};

struct LightsaberInstance {
    neomdl::model::GameVersion game{neomdl::model::GameVersion::Kotor2};

    std::optional<std::size_t> reference;
    std::size_t nodeIndex{};
    const neomdl::model::LightsaberNodeData* data{};
    neomdl::model::Mesh staticMesh;
    neomdl::model::Mesh movingMesh;
    // Part::GetWorld's raw channels feed history and Draw's inverse transform.
    // The rendering matrix is a separate application cache.
    neomdl::model::EngineFrame frame{};
    neomdl::model::Mat4 world = neomdl::model::identityMatrix();
    std::array<LightsaberHistorySample, 20> history{};
    std::uint16_t writeIndex{};
    std::uint16_t oldestIndex{};
    std::uint16_t historyCount{};
    neomdl::model::Vec3 lastPosition{};
    bool modelFog{};
    bool active{};
    bool usePieceMesh{true};

    const neomdl::model::Mesh& renderMesh() const noexcept {
        return !usePieceMesh && !movingMesh.indices.empty()
            ? movingMesh : staticMesh;
    }
    bool streamsMovingGeometry() const noexcept {
        return !usePieceMesh && !movingMesh.indices.empty();
    }
};

// Runtime state from K1/K2 PartLightsaber. The
// idle path submits the postprocessed piece stream used when the history has
// zero or one sample. Moving instances rebuild the native historical
// and predictive quad-strip halves from the 20-entry transform history ring.
class LightsaberScene final {
public:
    bool setSettings(LightsaberSceneSettings settings);
    const LightsaberSceneSettings& settings() const noexcept { return settings_; }
    void setDocument(const ModelDocument* document);

    void setReferenceScene(const ReferenceScene* references) noexcept { references_ = references; }
    void reset();
    void setPrimaryAnimated(bool value) noexcept { primaryAnimated_ = value; }
    void synchronize(const neomdl::model::Pose& pose,
                     bool includeReferences = true);
    void advance(const neomdl::model::Pose& pose, float elapsedSeconds,
                 bool includeReferences = true);
    // Independently scheduled Gobs: references animate before their containing
    // Gob. A later SetPosition/RenderNothing changes Draw's frame, not history.
    void advancePrimary(const neomdl::model::Pose& pose, float elapsedSeconds);
    void advanceReference(const neomdl::model::Pose& pose, std::size_t reference,
                          float elapsedSeconds);
    // NewCAurObject Animate(0) occurs independently in each owner, before the
    // reference's default animation and attachment to its containing Gob.
    void sampleConstructedOwners(const neomdl::model::Pose& primaryPose);
    void refreshTransforms(const neomdl::model::Pose& pose,
                           bool includeReferences = true);

    const std::vector<std::unique_ptr<LightsaberInstance>>& instances() const noexcept {
        return instances_;
    }
    neomdl::model::Bounds bounds() const noexcept;
    std::uint64_t revision() const noexcept { return revision_; }

private:
    enum class OwnerSelection { All, Primary, References };
    void update(const neomdl::model::Pose& pose, float elapsedSeconds,
                bool includeReferences, bool ageHistory, bool sampleHistory = true,
                OwnerSelection selection = OwnerSelection::All,
                std::optional<std::size_t> selectedReference = std::nullopt);

    const ModelDocument* document_{};
    const ReferenceScene* references_{};
    std::vector<std::unique_ptr<LightsaberInstance>> instances_;
    LightsaberSceneSettings settings_;

    std::uint64_t revision_{};
    bool primaryAnimated_{true};
};

} // namespace neomdl
