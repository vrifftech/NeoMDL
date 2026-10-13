#include <model/AsciiModelInternal.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace neomdl::model {
namespace {

Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }

std::uint32_t positionKey(float value) {
    std::uint32_t bits;
    std::memcpy(&bits,&value,sizeof(bits));
    return bits&0x80000000u?~bits:bits^0x80000000u;
}

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
                if(!mesh.explicitNormals) {
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
            }
            collect(index);
        }
    };
    const auto root=static_cast<std::size_t>(std::find_if(model.nodes.begin(),model.nodes.end(),
        [](const Node& node){return !node.parent;})-model.nodes.begin());
    collect(root);
    if(result.skinMeshes==0u) { model.skinSeams=result;return; }
    result.processed=true;

    // Positions stay fixed while the ordered seam pass updates normal pools.
    std::vector<std::vector<Vec3>> positions(meshes.size());
    using PositionEntry=std::pair<std::uint32_t,std::size_t>;
    std::vector<std::vector<PositionEntry>> byX(meshes.size());
    for(std::size_t index=0;index<meshes.size();++index) {
        const auto& entry=meshes[index];
        const auto& mesh=model.meshes[entry.mesh];
        const auto count=static_cast<std::int16_t>(mesh.enginePrelightVertexCount);
        for(std::int32_t vertex=0;vertex<=count;++vertex) {
            const auto local=static_cast<std::size_t>(vertex)==mesh.enginePrelightVertexCount
                ? entry.sentinel : mesh.vertices[vertex].position;
            positions[index].push_back(add(entry.position,
                ascii::engineModelRotateVector(entry.orientation,local)));
            byX[index].emplace_back(positionKey(positions[index].back().x),vertex);
        }
        std::sort(byX[index].begin(),byX[index].end());
    }

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
            const auto point=positions[owner][vertex];
            const auto lower=positionKey(std::nextafter(point.x-tolerance,-std::numeric_limits<float>::infinity()));
            const auto upper=positionKey(std::nextafter(point.x+tolerance,std::numeric_limits<float>::infinity()));
            auto sum=ascii::engineModelRotateVector(meshes[owner].orientation,current.normalAt(vertex));
            std::vector<NormalReference> matches{{owner,static_cast<std::size_t>(vertex)}};
            // K1 0x5c224c / K2 0x6e00dc visit every OTHER mesh, including
            // earlier ones. Their inclusive candidate loop reads row N.
            for(std::size_t candidate=0;candidate<meshes.size();++candidate) {
                if(candidate==owner) continue;
                job.check();
                const auto& source=model.meshes[meshes[candidate].mesh];
                // The inclusive engine traversal also reads the unpublished
                // tail of a no-face animmesh (K1 0x5c25b0).
                if(source.animated && !source.asciiMdxAllocation &&
                   positions[candidate].size()==source.enginePrelightVertexCount+1u)
                    throw ModelError("ASCII skin seam processing reaches an animmesh without a published MDX pool");
                result.candidateComparisons+=positions[candidate].size();
                const auto first=matches.size();
                const auto begin=std::lower_bound(byX[candidate].begin(),byX[candidate].end(),PositionEntry{lower,0u});
                const auto end=std::upper_bound(begin,byX[candidate].end(),
                    PositionEntry{upper,std::numeric_limits<std::size_t>::max()});
                for(auto entry=begin;entry!=end;++entry) {
                    const auto sourceVertex=entry->second;
                    const auto candidatePoint=positions[candidate][sourceVertex];
                    // The comparison is strict; unordered values also fail.
                    if(!(std::abs(candidatePoint.x-point.x)<tolerance) ||
                       !(std::abs(candidatePoint.y-point.y)<tolerance) ||
                       !(std::abs(candidatePoint.z-point.z)<tolerance)) continue;
                    matches.push_back({candidate,sourceVertex});
                }
                // Spatial lookup must not change the engine's accumulation order.
                std::sort(matches.begin()+static_cast<std::ptrdiff_t>(first),matches.end(),
                    [](const auto& a,const auto& b){return a.vertex<b.vertex;});
            }
            if(matches.size()==1u) continue;
            for(std::size_t match=1u;match<matches.size();++match) {
                const auto& ref=matches[match];
                sum=add(sum,ascii::engineModelRotateVector(meshes[ref.mesh].orientation,normal(ref)));
            }
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
