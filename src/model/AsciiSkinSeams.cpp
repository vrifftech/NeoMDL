#include <model/AsciiModelInternal.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace neomdl::model {
namespace {

Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }

Vec3 normalizeSeamVector(Vec3 value) {
    // Vector::Normalize: K1 0x66414c. This is the engine tiny-vector
    // branch, not a replacement normal supplied by the viewer.
    const float length=std::sqrt((value.x*value.x+value.y*value.y)+value.z*value.z);
    if(static_cast<double>(length)<1.0e-9) return {1.0f,0.0f,0.0f};
    const float inverse=1.0f/length;
    return {value.x*inverse,value.y*inverse,value.z*inverse};
}

struct SeamMesh {
    std::size_t mesh;
    Vec3 position;
    Quaternion orientation;
    Vec3 sentinel;
};

} // namespace

void initializeAsciiSkinSeamPools(Model& model) {
    for(auto& mesh:model.meshes) {
        // These are the shared interleaved MDX normal lanes, not PartAnimMesh
        // positions/UVs. KOTOR publication zeroes the entire extra MDX row;
        // AsciiNormalPool::sentinel retains its writable zero normal separately
        // from the N logical vertices, including for backported animmeshes.
        auto pool=std::make_shared<AsciiNormalPool>();
        pool->normals.reserve(mesh.enginePrelightVertexCount);
        for(std::size_t index=0;index<mesh.enginePrelightVertexCount;++index)
            pool->normals.push_back(mesh.vertices[index].normal);
        mesh.asciiNormalPool=std::move(pool);
    }
}

void processAsciiSkinSeams(const Model& model,const neoshared::bif::JobControl& job) {
    // This producer belongs to MdlNode, not Part. Its traversal and parent
    // walks use the canonical authored graph even when several Parts share a
    // node. Their meshes retain the same writable normal pools.
    if(model.partSourceModel) {
        processAsciiSkinSeams(*model.partSourceModel,job);
        model.skinSeams=model.partSourceModel->skinSeams;
        return;
    }
    // NewCAurObject calls this after instance-tree PostProcess, including
    // PartSkin vertex generation and tangent generation. Compiled models
    // have the engine skip flag; this entry point is for constructed ASCII.
    if(!model.asciiBuilt) return;
    SkinSeamStatistics result;
    std::vector<SeamMesh> meshes;
    std::function<void(std::size_t)> collect = [&](std::size_t parent) {
        for(const auto index:model.nodes[parent].children) {
            job.check();
            const auto& node=model.nodes[index];
            // CreateSkinNodeList skips a saber's entire subtree, not just
            // that mesh. It does not include the traversal's root itself.
            if((node.flags&kNodeLightsaber)!=0u) continue;
            if(node.mesh && model.meshes[*node.mesh].render) {
                const auto& mesh=model.meshes[*node.mesh];
                if(mesh.skinned) ++result.skinMeshes;
                Vec3 position{};
                Quaternion orientation{};
                auto ancestor=std::optional<std::size_t>{index};
                while(ancestor) {
                    const auto& transform=model.nodes[*ancestor];
                    position=add(ascii::engineModelRotateVector(transform.orientation,position),
                                 transform.position);
                    orientation=ascii::engineModelQuaternionProduct(transform.orientation,orientation);
                    ancestor=transform.parent;
                }
                // The engine interleaved pools allocate N+1 records.
                // Ordinary/dangly/backported animmesh tail xyz=1e7;
                // skin tail xyz=1e6. Seams read this static MDX position,
                // not the N-row CPU or animated Part position arrays.
                const float tail=mesh.skinned?1000000.0f:10000000.0f;
                meshes.push_back({*node.mesh,position,orientation,{tail,tail,tail}});
            }
            collect(index);
        }
    };
    const auto root=static_cast<std::size_t>(std::find_if(model.nodes.begin(),model.nodes.end(),
        [](const Node& node){return !node.parent;})-model.nodes.begin());
    collect(root);
    if(result.skinMeshes==0u) { model.skinSeams=result;return; }
    result.processed=true;

    struct NormalReference { std::size_t mesh,vertex; };
    const auto normal=[&](const NormalReference& ref) -> Vec3& {
        const auto& mesh=model.meshes[meshes[ref.mesh].mesh];
        return ref.vertex==mesh.enginePrelightVertexCount ? mesh.asciiNormalPool->sentinel
                                               : mesh.asciiNormalPool->normals[ref.vertex];
    };
    std::vector<bool> written(meshes.size());
    constexpr float tolerance=0.0010000000474974513f;
    for(std::size_t owner=0;owner+1u<meshes.size();++owner) {
        job.update(owner,meshes.size(),"Processing ASCII model skin seams");
        auto& current=model.meshes[meshes[owner].mesh];
        const auto ownerCount=static_cast<std::int16_t>(current.enginePrelightVertexCount);
        for(std::int32_t vertex=0;vertex<ownerCount;++vertex) {
            job.check();
            const auto point=add(meshes[owner].position,
                ascii::engineModelRotateVector(meshes[owner].orientation,current.vertices[vertex].position));
            auto sum=ascii::engineModelRotateVector(meshes[owner].orientation,current.normalAt(vertex));
            std::vector<NormalReference> matches{{owner,static_cast<std::size_t>(vertex)}};
            // K1 0x5c224c / K2 0x6e00dc visit every OTHER mesh, including
            // earlier ones. Their inclusive candidate loop reads row N.
            for(std::size_t candidate=0;candidate<meshes.size();++candidate) {
                if(candidate==owner) continue;
                const auto& source=model.meshes[meshes[candidate].mesh];
                const auto candidateCount=static_cast<std::int16_t>(source.enginePrelightVertexCount);
                for(std::int32_t sourceVertex=0;sourceVertex<=candidateCount;++sourceVertex) {
                    job.check();
                    // No-face animmeshes never publish a static MDX allocation.
                    // Published animmeshes have KOTOR's defined extra row;
                    // unpublished position offset -1 addresses pool-base-1
                    // (K1 ARM ctor 0x592bd4, seam read 0x5c25b0).
                    if(source.animated && !source.asciiMdxAllocation &&
                       static_cast<std::size_t>(sourceVertex)==source.enginePrelightVertexCount)
                        throw ModelError("ASCII skin seam processing reaches an animmesh without a published MDX pool");
                    ++result.candidateComparisons;
                    const auto local=static_cast<std::size_t>(sourceVertex)==source.enginePrelightVertexCount
                        ? meshes[candidate].sentinel : source.vertices[sourceVertex].position;
                    const auto candidatePoint=add(meshes[candidate].position,
                        ascii::engineModelRotateVector(meshes[candidate].orientation,local));
                    // The comparison is strict; unordered values also fail.
                    if(!(std::abs(candidatePoint.x-point.x)<tolerance) ||
                       !(std::abs(candidatePoint.y-point.y)<tolerance) ||
                       !(std::abs(candidatePoint.z-point.z)<tolerance)) continue;
                    const NormalReference ref{candidate,static_cast<std::size_t>(sourceVertex)};
                    sum=add(sum,ascii::engineModelRotateVector(meshes[candidate].orientation,normal(ref)));
                    matches.push_back(ref);
                }
            }
            if(matches.size()==1u) continue;
            ++result.matchedGroups;
            const float inverse=1.0f/static_cast<float>(matches.size());
            const auto average=normalizeSeamVector({sum.x*inverse,sum.y*inverse,sum.z*inverse});
            for(const auto& ref:matches) {
                auto inverseOrientation=meshes[ref.mesh].orientation;
                inverseOrientation.w=-inverseOrientation.w;
                // Engine writes only the normal channel, without a second
                // normalization or regeneration of the three tangent vectors.
                normal(ref)=ascii::engineModelRotateVector(inverseOrientation,average);
                written[ref.mesh]=true;
                ++result.adjustedVertices;
            }
        }
    }
    // UnlockPool publishes the in-place normal writes to every instance.
    // Advance only the affected pools so retained GPU uploads are refreshed.
    for(std::size_t i=0;i<meshes.size();++i)
        if(written[i]) ++model.meshes[meshes[i].mesh].asciiNormalPool->revision;
    model.skinSeams=result;
}

} // namespace neomdl::model
