#pragma once
#include "render/GlApi.hpp"
#include "render/MaterialPass.hpp"
#include <model/Model.hpp>
#include <memory>
#include <vector>

namespace neomdl::render {
struct MeshDrawParameters;
// Matches engine attribute semantics. No generated/orthogonalized basis and no
// CPU-normalized skin weights enter the engine multipass programs.
struct EngineSkinVertex {
    model::Vec3 position, normal;
    std::array<float,4> weights, indices;
    model::Vec2 uv;
    model::Vec3 basis4, basis5, basis6;
};
// Validate current pose on EVERY draw, including cached source-vertex draws.
void validateEngineSkinFrame(const model::Mesh&, const model::DeformedMesh&, bool requireBasis=false);
std::vector<EngineSkinVertex> engineSkinVertices(const model::Mesh&,
                                                const model::DeformedMesh&, bool requireBasis=true);
// A per-context program bundle, owned by ShaderMeshRenderer. Geometry stays in
// that renderer's existing cache. This is not a second scene/material manager.
class SkinMaterialPass final {
public:
    SkinMaterialPass();
    ~SkinMaterialPass();
    SkinMaterialPass(const SkinMaterialPass&)=delete;
    SkinMaterialPass& operator=(const SkinMaterialPass&)=delete;
    void setLoader(const gl::Loader&);
    bool prepare(gl::Api&, const MeshDrawParameters&, MaterialProgram,
                 std::string& error);
    // The caller owns the element buffer and guards generic arrays, bindings,
    // and host GLSL. This method additionally guards ARB/matrix/fixed state.
    bool draw(gl::Api&, const model::Mesh&, const model::DeformedMesh&,
              const MeshDrawParameters&, MaterialProgram, EngineMaterialState&,
              gl::UInt normalizationCube, gl::UInt vertexBuffer, gl::Size count,
              const std::vector<model::EngineMeshDraw>& batches,
              std::uint64_t& drawCalls, std::string& error);
    bool draw(gl::Api& api,const model::Mesh& mesh,const model::DeformedMesh& pose,
              const MeshDrawParameters& parameters,MaterialProgram program,EngineMaterialState& state,
              gl::UInt normalizationCube,gl::UInt vertexBuffer,gl::Size count,
              std::uint64_t& drawCalls,std::string& error) {
        return draw(api,mesh,pose,parameters,program,state,normalizationCube,vertexBuffer,
                    count,mesh.engineDraws,drawCalls,error);
    }
    void shutdown(); // current context required
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace neomdl::render
