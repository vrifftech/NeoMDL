#pragma once
#include <model/Model.hpp>

namespace neomdl::model {
struct ParticleRandom;

// The source UVs belong to the Model's MDX pool, while every PartTriMesh
// retains separate moving centers and a current jitter pool. Ordinary engine
// draw arrays read the shared source after the procedural traversal writes it.
struct ProceduralUvPool {
    std::vector<Vec2> texcoords;
    std::uint64_t revision{};
};
struct ProceduralUvPart {
    std::vector<Vec2> centers, current;
};
std::vector<Vec2> decodeEngineProceduralUvSource(const Mesh&);
void initializeProceduralUvPools(Model&);
std::vector<ProceduralUvPart> createProceduralUvParts(const Model&);
void animateProceduralUv(const Model&,
                         std::vector<ProceduralUvPart>&, ParticleRandom&);
} // namespace neomdl::model
