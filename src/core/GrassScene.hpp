#pragma once
#include "core/ModelDocument.hpp"
#include <model/Emitter.hpp>
#include <model/Wind.hpp>
#include <model/Trigger.hpp>
#include <array>
#include <optional>

namespace neomdl {
// CAurGrassInfo and Scene globals, not MDL material flags or a billboard mode.
struct GrassMaterial {
    std::string texture;
    float density{}, quadSize{1};
    model::Vec3 diffuse{1,1,1}, ambient{}, emissive{};
    std::array<float,4> probabilities{{.25f,.25f,.25f,.25f}};
};
struct GrassSettings {
    bool enabled{}, lightmaps{true}, wind{true}, textured{true}, forceSight{};
    float binSize{2.6f}, nearSquared{225}, farSquared{900}, stiffness{1}, alphaCutoff{.4f};
    std::array<std::optional<GrassMaterial>,32> surfaces;
    std::optional<std::uint8_t> areaWind;
    model::GameVersion game{model::GameVersion::Kotor2};
};
void validateGrassSettings(const GrassSettings&);
// Only decodes the visual grass/wind fields. No module mounting or gameplay
// state is guessed. Byte parsing is delegated to neoshared GFF/2DA.
GrassSettings grassSettingsFromArea(const std::vector<std::uint8_t>& are,
                                   const std::vector<std::uint8_t>& surfaceTable,
                                   const neoshared::bif::JobControl& job = resourceLoadJob(),
                                   model::GameVersion game = model::GameVersion::Kotor2);
struct GrassSourceFace {std::size_t part{},face{};};
struct GrassRoomSource {
    std::uint64_t room{};
    std::shared_ptr<ModelDocument> document;
    const model::Pose* pose{}; // read only while rebuild() executes
    model::Mat4 world{model::identityMatrix()};
    model::Bounds bounds;
    // Optional original face-admission sequence, before bin creation. Must be
    // an exact permutation of enabled surface faces; not an output sort order.
    std::optional<std::vector<GrassSourceFace>> faceAdmissionOrder{};
};
struct GrassTriangle {
    std::array<model::Vec3,3> positions;
    std::array<model::Vec2,3> lightmapUvs;
    std::array<std::array<std::uint8_t,4>,3> colors;
    model::Vec3 normal;
    std::size_t part{}, bladeCount{};
    std::size_t face{}; // authored mesh face index; vector order is admission order
    bool lightmapped{}, colored{};
    std::string lightmap;
};
struct GrassGroup {
    std::size_t firstVertex{}, bladeCount{}, part{};
    std::string lightmap;
    // SortTriangles takes this Part from the UNSORTED list at a sorted-group
    // boundary. It is not necessarily the Part used to sum this group's count.
    std::size_t lightmapPart{};
};
struct GrassFaceBuild {
    std::size_t inputOrdinal{},part{},face{},firstBlade{},bladeCount{};
    std::uint32_t seed{},randomStateAfter{};
};
struct GrassBin {
    std::uint64_t room{}, identity{};
    std::shared_ptr<ModelDocument> document;
    std::size_t x{},y{},surface{};
    std::vector<GrassTriangle> triangles;
    model::Bounds bounds;
    model::Vec3 center{};
    float radius{}, minimumDistanceSquared{}, opacity{};
    std::size_t bladeCount{}, tailFloats{5};
    bool lightmapped{}, colored{}, built{}, active{}, visible{};
    // Engine allocation: tightly packed position Vec3s, then interleaved tails.
    // Keeping that layout matters: DrawLightmappedGrass derives the tail base
    // from EACH group's blade count, not the entire bin's count (both executables).
    std::vector<float> pool;
    std::vector<GrassGroup> groups;
    std::vector<GrassFaceBuild> faceBuilds; // actual original-order RNG/geometry
    std::uint32_t randomStateBefore{},randomStateAfter{};
    std::uint64_t revision{};
};
// Stable part ordering determines group counts without changing geometry order.
std::vector<GrassGroup> engineGrassGroups(const GrassBin&);
struct GrassStatistics { std::size_t bins{}, faces{}, blades{}, residentBins{}, residentBlades{}; };
class GrassScene final {
public:
    void rebuild(const std::vector<GrassRoomSource>&, const GrassSettings&,
                 const neoshared::bif::JobControl& job = resourceLoadJob());
    void clear() noexcept;
    const GrassSettings& settings() const noexcept {return settings_;}
    const std::vector<GrassBin>& bins() const noexcept {return bins_;}
    GrassStatistics statistics() const noexcept;
    // ManageSceneBSP/CollectBins/AurProcessActiveBins. Builds in range even if
    // outside the draw frustum, and releases geometry leaving the active set.
    void collect(model::Vec3 camera,const std::vector<std::uint64_t>& activeRooms,
                 const std::vector<model::TriggerPlane>& planes,model::ParticleRandom&);
    // RenderGrassPolys updates wind once per visible bin, after Scene's wind
    // timer update, not once per lightmap group and not for far/faded bins.
    void advanceWind(float seconds,const model::WindManager&);
    // Material-major, then engine bin-registration order.
    std::vector<std::size_t> drawOrder() const;
    // Translates the actual split-array consumer, including its group-relative
    // tail-base contract. Throws rather than reading beyond malformed pools.
    static model::Mesh drawMesh(const GrassBin&,const GrassGroup&,bool lightmapped);
private:
    void build(GrassBin&,model::ParticleRandom&);
    GrassSettings settings_;
    std::vector<GrassBin> bins_;
    std::vector<std::size_t> activeBins_;
};
} // namespace neomdl
