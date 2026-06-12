#include <model/UvAnimation.hpp>
#include <model/Emitter.hpp>
#include <model/EngineArithmetic.hpp>

namespace neomdl::model {
void initializeProceduralUvPools(Model& model) {
    if (model.asciiBuilt) {






        model.engineMdxPoolBytes=0u;
        // Backported animmesh rows use this KOTOR MDX allocation contract too;
        // their separate packed samples and Part buffers still contain N rows.
        const auto append=[&](std::uint16_t vertexCount,std::uint32_t rowSize) {
            if (rowSize!=0u)
                model.engineMdxPoolBytes+=
                    (static_cast<std::uint32_t>(static_cast<std::int16_t>(vertexCount))+1u)*rowSize+
                    (model.engineMdxPoolBytes&15u);
        };
        for (const auto& allocation:model.asciiMdxAllocations)
            append(allocation.vertexCount,allocation.rowSize);
        std::vector<bool> publishedSkins(model.meshes.size(),false);
        const auto postProcess=[&](const auto& self,std::size_t index)->void {
            const auto& node=model.nodes[index];
            if (node.mesh && model.meshes[*node.mesh].skinned) {
                const auto& mesh=model.meshes[*node.mesh];
                // Duplicate Parts can refer to the same MdlNode. Its first
                // PartSkin::PostProcess fills the vertex/inverse-bind counts; later
                // calls return before allocating (K1 0x5e3ed4/K2 0x711850).
                if(!publishedSkins[*node.mesh]) {
                    publishedSkins[*node.mesh]=true;
                    append(mesh.enginePrelightVertexCount,mesh.mdxRowSize);
                }
                // PartSkin overrides PostProcess without calling the base
                // child traversal, including its already-built return path.
                return;
            }
            for (const auto child:node.children) self(self,child);
        };
        for (std::size_t index=0;index<model.nodes.size();++index)
            if (!model.nodes[index].parent) postProcess(postProcess,index);
    }
    for (auto& mesh : model.meshes) {
        // Effective engine update admission: the primitive has no UV pool
        // for N==0, and ProceduralAnimateHierarchy excludes UV0 offset -1.
        // Invalid reads by the earlier engine center constructor are outside
        // this active-source representation, not replaced with display UVs.
        if (!mesh.uvAnimation.enabledRaw || mesh.mdxTexcoordOffsets[0]==0xffffffffu ||
            mesh.enginePrelightVertexCount==0u) {
            mesh.proceduralUvPool.reset();
            continue;
        }
        // Copies of one loaded resource share Model-owned MDX. ASCII
        // material finalization can rebuild its stride/source after those
        // copies exist; update their shared pool rather than detach it.
        if (!mesh.proceduralUvPool)
            mesh.proceduralUvPool=std::make_shared<ProceduralUvPool>();
        auto& values=mesh.proceduralUvPool->texcoords;
        if (model.asciiBuilt) {
            values.clear();
            values.reserve(mesh.vertices.size());
            for (const auto& vertex : mesh.vertices) values.push_back(vertex.texcoord0);
        } else {
            // Part/primitive construction copies raw MDX words by offset,
            // without the inspection mesh's bitmap or finite-value repair.
            // The shared source retains any submitted sentinel row, while
            // procedural mutation below still visits only signed N rows.
            values=decodeEngineProceduralUvSource(mesh);
        }
        ++mesh.proceduralUvPool->revision;
    }
}

std::vector<ProceduralUvPart> createProceduralUvParts(const Model& model) {
    std::vector<ProceduralUvPart> result(model.meshes.size());


    for (std::size_t i=0;i<model.meshes.size();++i) {
        const auto& mesh=model.meshes[i];
        if (!mesh.proceduralUvPool) continue;
        result[i].centers=mesh.proceduralUvPool->texcoords;
        result[i].current=result[i].centers;
    }
    return result;
}

void animateProceduralUv(const Model& model,
                         std::vector<ProceduralUvPart>& parts, ParticleRandom& random) {


    const auto visit=[&](const auto& self, std::size_t nodeIndex)->void {
        const auto& node=model.nodes[nodeIndex];
        const bool admitted=model.sceneNodeAdmission.empty() || model.sceneNodeAdmission[nodeIndex];
        if (admitted && node.mesh) {
            const auto meshIndex=*node.mesh;
            const auto& mesh=model.meshes[meshIndex];
            if (mesh.proceduralUvPool && mesh.mdxTexcoordOffsets[0]!=0xffffffffu &&
                !mesh.vertices.empty()) {
                auto& part=parts[meshIndex];
                const auto& animation=mesh.uvAnimation;
                const auto count=static_cast<std::int16_t>(mesh.enginePrelightVertexCount);
                const auto amplitude=engineFloatToInt(animation.jitter*1000.0f);
                const float half=animation.jitter*0.5f;
                for (std::int32_t vertex=0;vertex<count;++vertex) {
                    auto& center=part.centers[static_cast<std::size_t>(vertex)];
                    auto& current=part.current[static_cast<std::size_t>(vertex)];
                    center.x+=animation.direction.x;
                    center.y+=animation.direction.y;
                    const auto jitter=[&](float& value, float midpoint) {
                        const auto remainder=engineSignedRemainder(
                            static_cast<std::int32_t>(random.next()),amplitude);
                        const float step=(static_cast<float>(remainder)*0.001f-half)*animation.jitterSpeed;
                        value+=step;
                        const float displacement=value-midpoint;
                        if (displacement>half) value=midpoint+half;
                        else if (displacement<-half) value=midpoint-half;
                    };
                    // With zero quantized amplitude the engine current pool
                    // remains unchanged. Direction advances only the center.
                    if (amplitude) { jitter(current.x,center.x);jitter(current.y,center.y); }
                }
                auto& pool=*mesh.proceduralUvPool;
                for (std::int32_t vertex=0;vertex<count;++vertex)
                    pool.texcoords[static_cast<std::size_t>(vertex)]=part.current[static_cast<std::size_t>(vertex)];
                ++pool.revision;
            }
        }
        for (const auto child:node.children) self(self,child);
    };
    for (std::size_t node=0;node<model.nodes.size();++node)
        if (!model.nodes[node].parent) visit(visit,node);
}
} // namespace neomdl::model
