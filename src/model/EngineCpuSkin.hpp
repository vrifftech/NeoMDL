#pragma once
#include "model/Geometry.hpp"

namespace neomdl::model {
EngineCpuSkinSource decodeEngineMeshTopology(const EngineMeshSource&,bool includeCpuTraversal=false,
                                           const neoshared::bif::JobControl& job={});
EngineCpuSkinSource decodeEngineCpuSkinSource(const Mesh&);



std::vector<EngineFloatSkinChannels> decodeEngineFloatSkinChannels(const Mesh&);

// ExecuteSkinTransforms owns two process-wide scratch pools. A renderer owns
// the equivalent retained storage, shared across its CPU skin draw calls.
class EngineCpuSkinPools {
public:
    std::vector<RenderVertex> execute(const Mesh&,const DeformedMesh&,const EngineCpuSkinSource&,bool shell,float shellAmount);
private:
    std::vector<Vec3> positions_,normals_;
};
} // namespace neomdl::model
