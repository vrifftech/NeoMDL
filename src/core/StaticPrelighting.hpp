#pragma once
#include "core/SceneQueries.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace neomdl {
struct StaticPrelightLight {
    model::Vec3 position{},color{};
    float radius{};
    bool ambientOnly{};
};
struct StaticPrelightPool {
    struct Identity {};
    StaticPrelightPool()=default;
    StaticPrelightPool(const StaticPrelightPool&)=delete;
    StaticPrelightPool& operator=(const StaticPrelightPool&)=delete;
    StaticPrelightPool(StaticPrelightPool&&)=default;
    StaticPrelightPool& operator=(StaticPrelightPool&&)=default;
    // PartTriMesh's constructor starts without a generated pool. This state
    // belongs to an individual runtime part, never to its shared MDL mesh.
    bool active{};
    std::vector<std::array<std::uint8_t,4>> colors;
    std::uint64_t generation{};
    // Cache ownership outlives a deleted part until its next submission, so
    // allocator address reuse cannot make a new pool look like the old one.
    std::shared_ptr<const Identity> identity=std::make_shared<const Identity>();
};
enum class StaticPrelightUpdate : std::uint8_t { Skipped,Deleted,Updated };

// Exact LightPartTriMesh producer. The caller supplies engine list order and
// the Part::GetWorld frame (scale is not used). Material+0x40 is TXI decal.
// tileMode is SpawnTile's final integer argument, retained in TileData+0x34.
// The helper accepts parts without TileData, but the live LightNodeTile caller
// requires tile data. Ordinary SpawnRoom geometry does not invoke this producer;
// ComputeStaticLighting resolves to the empty Scene::PostProcessLS in all targets.
StaticPrelightUpdate updateStaticPrelightPool(StaticPrelightPool&,
    const model::Mesh&,const HitPartTransform&,model::Vec3 selfIllumination,
    bool decal,const std::vector<StaticPrelightLight>& orderedLights,
    std::int32_t tileMode);
}
