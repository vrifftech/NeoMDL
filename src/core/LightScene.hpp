#pragma once

#include "core/ModelDocument.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neomdl {

class ReferenceScene;
class ParticleScene;
class AreaScene;

struct LightFlareElement {
    std::string texture;
    float size{};
    float position{};
    neomdl::model::Vec3 colorShift{};
};

struct RuntimeLight {
    std::uint64_t id{};
    std::size_t instanceIndex{};
    std::size_t nodeIndex{};
    neomdl::model::Vec3 position{};
    neomdl::model::Vec3 color{}; // raw controller RGB; multiplier/fades are applied at submission
    float radius{};
    float shadowRadius{-1.0f};
    float verticalDisplacement{};
    float multiplier{1.0f};
    float flareRadius{};
    float flareVisibility{}; // PartLight +0x88, initialized to zero
    std::string flareDiagnostic;
    std::int32_t priority{};
    // PartLight runtime override; zero defers to the authored node priority.
    std::int32_t runtimePriority{};
    std::int32_t dynamicType{};
    bool ambientOnly{};
    bool affectDynamic{};
    bool castsShadow{};
    bool generateFlare{};
    bool fading{};
    bool hasOwner{true}; // PartLight +0x40; required for the dynamic update list
    bool registeredStatic{};
    bool registeredDynamic{};
    bool registeredRender{};
    bool registeredFlare{};
    std::vector<LightFlareElement> flares;
};

struct ActiveLight {
    std::uint64_t id{};
    neomdl::model::Vec3 position{};
    neomdl::model::Vec3 color{};
    float radius{};
    float shadowRadius{-1.0f};
    float verticalDisplacement{};
    float fade{};
    float normalizedDistance{};
    std::int32_t priority{};
    std::int32_t dynamicType{};
    bool ambientOnly{};
    bool castsShadow{};
};

struct LightSceneStatistics {
    std::size_t all{};
    std::size_t staticLights{};
    std::size_t dynamicLights{};
    std::size_t renderCandidates{};
    std::size_t flareLights{};
    std::size_t owners{};
    std::size_t activeIllumination{};
    bool activeShadow{};
};

using LightRoomId = std::uint64_t;
using LightOwnerId = std::uint64_t;

// Inputs to CollectLightsForGivenRoom, not guessed spatial membership. The
// two lists are the room light table's +0x00 and +0x80 lists respectively.
struct LightRoomRegistration {
    LightRoomId id{};
    std::vector<LightRoomId> influencingRooms; // CAurRoom +0x5c, in engine order
    std::vector<std::uint64_t> primaryLights;
    std::vector<std::uint64_t> secondaryLights;
};

enum class LightQuerySource { Global, Gob, Room };
enum class LightSubmissionMode { Object, Scene, SceneDynamicOnly };
struct LightQuerySphere {
    model::Vec3 position{};
    float radius{};
};
struct LightOwnerQuery {
    LightQuerySource source{LightQuerySource::Global};
    LightSubmissionMode submission{LightSubmissionMode::Object};
    // ControlLightsFade's optional current-Gob prefix restriction. This is a
    // prefix limit, not a filter of the whole list by ownership.
    std::optional<LightOwnerId> sceneOwnerPrefix;
    std::optional<LightQuerySphere> sphere; // explicit host query, not model bounds
    bool includeOutOfRange{}; // PrioritizeLights' engine Boolean argument
    bool hasFadeContainer{true}; // Scene::DoGobBuckets checks Gob +0x1a4
};

// Fixed-function and vertex-program setup use different scales. Keeping them
// explicit avoids multiplying both the PartLight multiplier and container fade.
enum class LightColorSource { FixedFunction, VertexProgram };
enum class ShadowLightQuery { All = 0, Dynamic = 1, Static = 2 };

inline constexpr LightOwnerId mainLightOwner = 0u;

struct OwnerLightSlot {
    std::uint64_t id{};
    float weight{};
    // Snapshot of the engine +0x78 scale at this selection/submission point.
    // It is separate from the object container's weight for immediate draws.
    float vertexScale{1.0f};
};

struct ShadowLightSelection {
    std::vector<OwnerLightSlot> lights;
    std::string diagnostic;
};

// CAurObjectLightContainer owns these weights, not PartLight. Keep the state
// separate even when multiple owners select the same light.
struct OwnerLightSelection {
    model::Vec3 position{};
    float radius{};
    std::vector<OwnerLightSlot> illumination;
    std::vector<OwnerLightSlot> vertexIllumination; // engine +0x44 / +0x74 list snapshot
    std::optional<OwnerLightSlot> shadow;
    LightOwnerQuery query{};
    std::vector<std::uint64_t> renderCandidates;
    std::unordered_map<std::uint64_t, float> candidateDistances;
    std::vector<std::uint64_t> rankedIllumination;
    std::optional<std::uint64_t> bump; // first eligible light in the top three
    std::string diagnostic; // nonempty when this query cannot be reproduced safely
    std::uint64_t lastSelectedFrame{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t lastRetainedFrame{std::numeric_limits<std::uint64_t>::max()};
};




class LightScene final {
public:
    static constexpr std::size_t maximumIlluminationLights = 3u;
    static constexpr std::size_t maximumShadowLights = 1u;
    static constexpr float lightFadeSpeed = 3.0f;
    static constexpr float shadowFadeSpeed = 3.0f;

    void setDocument(const ModelDocument* document);
    void setReferenceScene(const ReferenceScene* references) noexcept { references_ = references; }
    void reset();
    void synchronize(const model::Pose& mainPose, bool includeReferences);
    // ChildAdd/ListSpecialPartsR register a chunk Gob's Parts independently
    // from the emitter which created it. Run before area room collection.
    void synchronizeChunks(const ParticleScene* inspectionParticles,
                           const AreaScene&, bool includeReferences = true);
    const ModelDocument* chunkLightDocument(std::size_t instanceIndex) const noexcept;
    bool chunkOwnerAllocated(LightOwnerId owner) const noexcept { return chunkOwnerIds_.count(owner)!=0; }

    // Application-owned scene inputs. Re-registering an ID updates authored /
    // animated data without resetting owner fades.
    void registerLight(RuntimeLight light, bool refreshCollection = true);
    bool removeLight(std::uint64_t id);
    // SetScene(null) unlists a retained PartLight without destroying its
    // registration or Gob fade-container references.
    void setLightSceneMember(std::uint64_t id, bool member);

    // Engine room lists are provided by an area-aware host, not generated from
    // preview bounding boxes. Null room means the scene's registered room list.
    void registerRoom(LightRoomRegistration room, bool refreshCollection = true);
    bool removeRoom(LightRoomId room);
    void collectLightsForGivenRoom(std::optional<LightRoomId> room);
    void clearRoomCollection();
    const std::vector<std::uint64_t>& collectedRoomLights() const noexcept { return collectedLights_; }
    const std::vector<std::uint64_t>& collectedFlareLights() const noexcept { return collectedFlares_; }
    const std::string& roomCollectionDiagnostic() const noexcept { return roomDiagnostic_; }
    void setOwnerQuery(LightOwnerId owner, LightOwnerQuery query);
    void removeOwner(LightOwnerId owner);
    std::optional<LightOwnerQuery> ownerQuery(LightOwnerId owner) const;

    // GetShadowLights is a scene/projection query, NOT the object's shadow
    // container. Call once per engine projection stage; repeated calls advance
    // its engine transition again. This does not render shadow geometry.
    ShadowLightSelection projectionShadowLights(LightOwnerId owner, ShadowLightQuery mode);
    // GetNearestLights: the alternate RenderOnlyGobs path. No ranking, room
    // collection, container fade, or illumination-scale mutation takes place.
    std::vector<ActiveLight> nearestLights(model::Vec3 position, float radius,
                                          bool dynamicOnly = false) const;
    // RenderOnlyGobs changes fixed-function slots only. VertexProgram::Enable
    // continues reading the retained engine +0x44/+0x74 lists; never replace
    // these with GetNearestLights' registration-order result.
    std::vector<ActiveLight> retainedVertexLights(bool ownerPresent = true) const;
    const RuntimeLight* bumpLight(LightOwnerId owner = mainLightOwner) const noexcept;
    // GetNearestLights updates fixed slots only. GetBumpMapLights continues to
    // read the manager's retained selection, even if the previous owner faded.
    const RuntimeLight* retainedBumpLight() const noexcept;

    // Advance an owner's container at most once per rendered frame; reusing it
    // for several meshes/passes must not speed up fades. endSelectionFrame
    // releases transient containers no longer submitted by the preview. The
    // host retains registered, invisible owners without selecting/updating them.
    void beginSelectionFrame(float elapsedSeconds);
    // Gob light containers outlive visibility. This is host lifetime bookkeeping,
    // not a engine fade update or a room/light query. Call after beginSelectionFrame.
    void retainOwnerForFrame(LightOwnerId owner);
    void selectOwner(LightOwnerId owner, model::Vec3 position, float radius);
    // Scene::DoBlurBuckets reuses Scene +0x8c for every appended owner. Unlike
    // selectOwner's per-Gob guard this is an explicit caller event each time.
    void selectSceneContainer(LightOwnerId container,LightOwnerQuery query);
    void endSelectionFrame();
    const OwnerLightSelection* ownerSelection(LightOwnerId owner = mainLightOwner) const noexcept;

    const std::list<RuntimeLight>& lights() const noexcept { return lights_; }
    std::vector<ActiveLight> illuminationLights(
        LightOwnerId owner = mainLightOwner,
        LightColorSource source = LightColorSource::FixedFunction) const;
    std::optional<ActiveLight> shadowLight(LightOwnerId owner = mainLightOwner) const;
    float illuminationWeight(LightOwnerId owner, std::uint64_t light) const noexcept;
    float shadowWeight(LightOwnerId owner, std::uint64_t light) const noexcept;

    const RuntimeLight* find(std::uint64_t id) const noexcept;
    RuntimeLight* find(std::uint64_t id) noexcept;
    const RuntimeLight* mainNode(std::size_t nodeIndex) const noexcept;
    LightSceneStatistics statistics(LightOwnerId owner = mainLightOwner) const noexcept;
    std::uint64_t revision() const noexcept { return revision_; }

private:
    friend class AreaScene;
    void appendModelLights(const model::Model& model, const model::Pose& pose,
                           const model::Mat4& base, std::size_t instanceIndex,
                           std::vector<RuntimeLight>& pending, float ownerScale=1.0f) const;
    void rankCandidates(OwnerLightSelection& owner);
    bool sortLights(std::vector<std::uint64_t>& ids, std::string& diagnostic) const;
    void refreshRoomCollection();
    void storeLight(RuntimeLight light);
    void enableSceneLights(OwnerLightSelection& owner);
    void snapshotVertexScales(OwnerLightSelection& owner);
    void advanceIlluminationSlots(OwnerLightSelection& owner);
    void advanceShadowSlot(OwnerLightSelection& owner);
    void pruneRemovedLights();

    const ModelDocument* document_{};
    const ReferenceScene* references_{};
    // Retained registrations keep stable references across synchronization.
    std::list<RuntimeLight> lights_;
    std::list<RuntimeLight> pooledLights_;
    std::unordered_set<std::uint64_t> modelLightIds_;
    std::unordered_set<std::uint64_t> chunkLightIds_;
    std::unordered_map<std::size_t,const ModelDocument*> chunkLightDocuments_;
    std::unordered_set<LightOwnerId> chunkOwnerIds_;
    std::vector<LightRoomRegistration> rooms_;
    bool roomCollectionEnabled_{};
    std::optional<LightRoomId> collectedRoom_;
    std::vector<std::uint64_t> collectedLights_;
    std::vector<std::uint64_t> collectedFlares_;
    std::string roomDiagnostic_;
    std::unordered_map<LightOwnerId, LightOwnerQuery> ownerQueries_;
    std::unordered_map<std::uint64_t, float> engineDistances_;
    std::unordered_map<std::uint64_t, float> engineIlluminationScales_;
    std::array<std::vector<OwnerLightSlot>, 2> sceneIllumination_;
    std::vector<std::uint64_t> engineVertexList_;
    std::optional<std::uint64_t> engineBumpLight_;
    std::array<std::vector<std::uint64_t>, 2> previousProjectionLights_;
    std::unordered_map<std::uint64_t, float> projectionWeights_;
    std::unordered_map<LightOwnerId, OwnerLightSelection> owners_;
    float selectionElapsed_{};
    std::uint64_t selectionFrame_{};
    std::uint64_t revision_{};
};

// SetLightGL uses 9/(r*r+r*r), with a 0.1 radius floor.
float k1LightQuadraticAttenuation(float radius) noexcept;
float k2LightQuadraticAttenuation(float radius) noexcept;
// SetUpVertexProgramLightShaderConstants uses 9/(2*r*r), with no 0.1 clamp.
float k2VertexLightQuadraticAttenuation(float radius) noexcept;

} // namespace neomdl
