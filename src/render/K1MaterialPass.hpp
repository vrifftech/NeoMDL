#pragma once
#include "render/GlApi.hpp"
#include "render/MaterialPass.hpp"
#include <model/Model.hpp>
#include <memory>
namespace neomdl::render {
struct MeshDrawParameters;
struct EngineRigidVertex {
    model::Vec3 position{},normal{};
    std::array<float,4> color{{1,1,1,1}};
    model::Vec2 uv0{},uv1{};
    model::Vec3 basis4{},basis5{},basis6{};
};
std::vector<EngineRigidVertex> engineRigidVertices(const model::Mesh&,const model::DeformedMesh*);
// K1's ordinary caller uses fixed transforms/texgen and its ATI fragment
// dispatcher. Program allocation and host-state restoration are shared with K2.
class K1MaterialPass final {
public:
    K1MaterialPass(); ~K1MaterialPass();
    void setLoader(const gl::Loader&);
    bool prepare(gl::Api&,const MeshDrawParameters&,std::string&);
    bool draw(gl::Api&,const model::Mesh&,const MeshDrawParameters&,
        EngineMaterialState&,gl::UInt normalizationCube,gl::UInt vertexBuffer,
        gl::Size count,const std::vector<model::EngineMeshDraw>& batches,std::uint64_t& drawCalls,std::string&);
    void shutdown();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace neomdl::render
