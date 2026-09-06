#include "render/LightSubmission.hpp"
#include <algorithm>

namespace neomdl::render {
namespace {
bool k1RigidMaterial(const MeshDrawParameters& p) noexcept {
    return p.material.k2PartTriMeshState &&
        p.material.engineGame==material::EngineMaterialGame::Kotor1 &&
        !p.exactK2SkinVertexProgram && !p.engineCpuSkin && !p.engineAnimMesh;
}
}
void setEngineBumpLight(const RuntimeLight* light,MeshDrawParameters& p) {
    using Path=material::TriMeshRenderPath;
    const bool consumes=(p.exactK2SkinVertexProgram && p.material.skinBumpAvailable) ||
        (k1RigidMaterial(p) && (p.material.renderPath==Path::BumpMapped ||
         p.material.renderPath==Path::LightMappedBumpDiffuse ||
         p.material.renderPath==Path::LightMappedBumpSpecular));
    p.skinBumpLights.clear();
    if(consumes && light)
        p.skinBumpLights.push_back({light->id,light->position,
            {{light->color.x,light->color.y,light->color.z}},light->radius});
}
void appendK2Lights(const std::vector<ActiveLight>& sources,
                    const model::Mat4& view, MeshDrawParameters& parameters, LightColorSource sourceFamily) {
    for (const auto& source : sources) {
        if (parameters.lightCount >= parameters.lights.size()) break;
        PreviewLight light;
        light.id = source.id;
        light.priority = source.priority;
        const auto position = model::transformPoint(view, source.position);
        light.position = {{position.x, position.y, position.z, 1.0f}};
        light.attenuation = {{1.0f, 0.0f,
            sourceFamily == LightColorSource::VertexProgram
                ? k2VertexLightQuadraticAttenuation(source.radius)
                : k2LightQuadraticAttenuation(source.radius)}};
        const std::array<float, 3> color{{source.color.x, source.color.y, source.color.z}};
        light.diffuse = source.ambientOnly ? std::array<float, 3>{} : color;
        light.ambient = source.ambientOnly ? color : std::array<float, 3>{};
        parameters.lights[parameters.lightCount++] = light;
    }
}
LightColorSource k2LightColorSource(const MeshDrawParameters& parameters) noexcept {
    if(parameters.engineCpuSkin) {
        // ExecuteSkinTransforms selects the material after CPU deformation.
        // K1 selects fixed RenderEnvironmentMapped or RenderFlat. K2
        // environment paths select vertex programs.
        if(parameters.material.engineGame==material::EngineMaterialGame::Kotor1)
            return LightColorSource::FixedFunction;
        if(parameters.skinEffect==SkinRenderEffect::BumpedOutShell)
            return LightColorSource::FixedFunction;
        return parameters.material.skinEnvironmentAvailable
            ? LightColorSource::VertexProgram : LightColorSource::FixedFunction;
    }
    if(k1RigidMaterial(parameters)) {
        using Path=material::TriMeshRenderPath;
        // K1 RenderEMBM's base pass uses the lit Standard VP. RenderBumpMapped
        // uses the lit Bump VP only for a nonempty bump list; otherwise its
        // engine caller delegates to fixed-function RenderFlat.
        if(parameters.material.renderPath==Path::EMBM ||
           (parameters.material.renderPath==Path::BumpMapped && !parameters.skinBumpLights.empty()))
            return LightColorSource::VertexProgram;
    }
    bool vertexProgram = parameters.exactK2SkinVertexProgram;
    if (parameters.material.k2PartTriMeshState) {
        using Path = material::TriMeshRenderPath;
        switch (parameters.material.renderPath) {
        case Path::FlatVP:
        case Path::EMLMVP:
        case Path::EnvironmentMappedVP:
        case Path::LightmappedVP:
            vertexProgram = true;
            break;
        default: break; // BumpmapFallback calls RenderFlat, not a bump renderer.
        }
    }
    return vertexProgram ? LightColorSource::VertexProgram : LightColorSource::FixedFunction;
}
void appendK2OwnerLights(const LightScene& scene, LightOwnerId owner,
                        const model::Mat4& view, MeshDrawParameters& parameters) {
    setEngineBumpLight(scene.bumpLight(owner),parameters);
    const auto family = k2LightColorSource(parameters);
    if(parameters.material.k2PartTriMeshState && parameters.engineCpuSkin) {
        // Read both snapshots after the SAME selectOwner event; neither read
        // advances fades. Preserve any explicit inspection prefix in each.
        auto fixed=parameters;
        appendK2Lights(scene.illuminationLights(owner,LightColorSource::FixedFunction),view,fixed,LightColorSource::FixedFunction);
        parameters.fixedFunctionLights=std::vector<PreviewLight>(fixed.lights.begin(),
            fixed.lights.begin()+std::min(fixed.lightCount,fixed.lights.size()));
    }
    appendK2Lights(scene.illuminationLights(owner, family), view, parameters, family);
}
} // namespace neomdl::render
