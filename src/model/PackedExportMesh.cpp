#include <model/PackedExportInternal.hpp>
#include <model/UvAnimation.hpp>

namespace neomdl::model::packed_export {
namespace {

std::size_t signedVertexCount(const Mesh& mesh) {
    const auto count=static_cast<std::int16_t>(mesh.enginePrelightVertexCount);
    if(count<0)
        throw ModelError("Packed MDL construction requires the engine nonnegative signed mesh vertex count");
    return static_cast<std::size_t>(count);
}

std::uint32_t appendMdxPool(Context& context,const Mesh& mesh) {
    auto& out=context.mdx;
    // InternalPostProcess adds the current low four bits, rather than the
    // complement to a 16-byte boundary (K2 0x6e4b7c..0x6e4c16).
    out.append(out.bytes.size()&15u);
    const auto count=signedVertexCount(mesh);
    const auto offset=out.append((count+1u)*mesh.mdxRowSize);
    for(std::size_t index=0;index<count;++index) {
        context.job.check();
        const auto row=offset+index*mesh.mdxRowSize;
        const auto& vertex=mesh.vertices.at(index);
        out.vec3(row,vertex.position);
        out.vec3(row+mesh.mdxNormalOffset,mesh.normalAt(index));
        const std::array<Vec2,4> uv{{vertex.texcoord0,vertex.texcoord1,
                                    vertex.texcoord2,vertex.texcoord3}};
        for(std::size_t channel=0;channel<4u;++channel)
            if(mesh.mdxTexcoordOffsets[channel]!=0xffffffffu)
                out.vec2(row+mesh.mdxTexcoordOffsets[channel],
                    channel==0u && mesh.proceduralUvPool
                        ?mesh.proceduralUvPool->texcoords.at(index):uv[channel]);
        if(mesh.mdxColorOffset!=0xffffffffu)
            out.copy(row+mesh.mdxColorOffset,vertex.color.data(),vertex.color.size());
        if(mesh.mdxTangentOffset!=0xffffffffu) {
            const auto basis=row+mesh.mdxTangentOffset;
            out.vec3(basis,vertex.tangentBasis.bitangent);
            out.vec3(basis+12u,vertex.tangentBasis.tangent);
            out.vec3(basis+24u,vertex.tangentBasis.normal);
        }
        if(mesh.skinned) {
            const auto& channels=mesh.builtFloatSkinChannels->at(index);
            for(std::size_t channel=0;channel<4u;++channel) {
                out.f32(row+mesh.mdxWeightOffset+channel*4u,channels.weights[channel]);
                out.f32(row+mesh.mdxBoneIndexOffset+channel*4u,channels.indices[channel]);
            }
        }
    }
    // The producer memset covers the entire extra row. Only xyz and the
    // skin default weight are nonzero; owner seam writes share its normal.
    const auto tail=offset+count*mesh.mdxRowSize;
    const float distant=mesh.skinned?1000000.0f:10000000.0f;
    out.vec3(tail,{distant,distant,distant});
    if(mesh.asciiNormalPool)out.vec3(tail+mesh.mdxNormalOffset,mesh.asciiNormalPool->sentinel);
    if(mesh.skinned)out.f32(tail+mesh.mdxWeightOffset,1.0f);
    context.mdxOffsets.emplace(&mesh,offset);
    return offset;
}

void meshHeader(Context& context,const Node& node,const Mesh& mesh,std::uint32_t nodeOffset,
                std::uint16_t vertexCount) {
    auto& out=context.mdl;
    const auto base=nodeOffset+80u;
    if(mesh.engineBounds) {
        out.vec3(base+20u,mesh.engineBounds->box.minimum);
        out.vec3(base+32u,mesh.engineBounds->box.maximum);
        out.f32(base+44u,mesh.engineBounds->sphereRadius);
        out.vec3(base+48u,mesh.engineBounds->sphereCenter);
    }
    out.vec3(base+60u,mesh.diffuse);out.vec3(base+72u,mesh.ambient);
    out.u32(base+84u,mesh.transparencyHint);
    out.string(base+88u,mesh.texture0,32u);out.string(base+120u,mesh.texture1,32u);
    // Process-local callbacks and render-pool handles have no portable
    // identity. ResetTriMesh replaces them on loading.
    // The writer clears the raw index-pointer table but retains its count
    // and capacity. Counted ASCII Parse assigns both to the requested size;
    // only BuildVertexIndices clears them (K2 0x7cff1f, 0x6e3ef6).
    out.u32(nodeOffset+0xecu,mesh.engineRawIndexArrayCount);
    out.u32(nodeOffset+0xf0u,mesh.engineRawIndexArrayCount);
    // The copied live-pool header survives the writer's zero-vertex exit.
    // Positive-N payload emission replaces its capacity with the count.
    out.u32(nodeOffset+0x11cu,static_cast<std::uint32_t>(mesh.engineDraws.size()));
    out.u32(nodeOffset+0x120u,mesh.engineIndexPoolCapacity);
    out.u32(nodeOffset+0x128u,0xffffffffu);
    if(!mesh.engineDraws.empty())out.u32(nodeOffset+0x130u,3u);
    out.u32(base+232u,mesh.uvAnimation.enabledRaw);
    out.vec2(base+236u,mesh.uvAnimation.direction);
    out.f32(base+244u,mesh.uvAnimation.jitter);
    out.f32(base+248u,mesh.uvAnimation.jitterSpeed);
    out.u32(base+252u,mesh.mdxRowSize==0u?0xffffffffu:mesh.mdxRowSize);
    out.u32(base+256u,mesh.mdxBitmap);
    out.u32(base+260u,mesh.mdxRowSize==0u?0xffffffffu:0u);
    out.u32(base+264u,mesh.mdxNormalOffset);
    out.u32(base+268u,mesh.mdxColorOffset);
    for(std::size_t channel=0;channel<4u;++channel)
        out.u32(base+272u+channel*4u,mesh.mdxTexcoordOffsets[channel]);
    out.u32(base+288u,mesh.mdxTangentOffset);
    for(std::size_t unused=292u;unused<=300u;unused+=4u)out.u32(base+unused,0xffffffffu);
    out.u16(base+304u,vertexCount);
    // Saber PostProcess owns one UV stream and increments this count
    // independently of the absent interleaved MDX layout (0x6e5c9b).
    out.u16(base+306u,node.lightsaber?1u:mesh.textureCount);
    out.u8(base+308u,mesh.lightmapped);out.u8(base+309u,mesh.rotateTexture);
    out.u8(base+310u,mesh.backgroundGeometry);out.u8(base+311u,mesh.castsShadow);
    out.u8(base+312u,mesh.beaming);out.u8(base+313u,mesh.render);
    if(context.k2() && mesh.k2Material) {
        out.u8(base+314u,mesh.k2Material->dirtEnabledRaw);
        out.u16(base+316u,static_cast<std::uint16_t>(mesh.k2Material->dirtTexture));
        out.u16(base+318u,static_cast<std::uint16_t>(mesh.k2Material->dirtWorldspace));
        out.u8(base+320u,mesh.k2Material->hologramDoNotDrawRaw);
    }
    out.f32(base+(context.k2()?324u:316u),mesh.totalArea);
    const auto mdx=context.mdxOffsets.find(&mesh);
    if(mdx!=context.mdxOffsets.end())
        out.u32(nodeOffset+context.meshEnd()-8u,mdx->second);
}

void meshPayload(Context& context,const Mesh& mesh,std::uint32_t nodeOffset,std::uint16_t vertexCount) {
    auto& out=context.mdl;
    // The sizer counts this discarded pointer list even when the mesh has
    // vertices; its writer clears +0xe8 without advancing the cursor.
    context.mdlUnwrittenBytes+=static_cast<std::size_t>(mesh.engineRawIndexArrayCount)*4u;
    // IODispatcher's common writer 0x7cfe20 emits these arrays in this order.
    if(!mesh.faces.empty()) {
        auto faceOffset=out.array(nodeOffset+0x58u,mesh.faces.size(),32u);
        for(const auto& face:mesh.faces) {
            out.vec3(faceOffset,face.normal);out.f32(faceOffset+12u,face.planeDistance);
            out.u32(faceOffset+16u,face.surfaceMaterial);
            for(std::size_t corner=0;corner<3u;++corner) {
                out.u16(faceOffset+20u+corner*2u,static_cast<std::uint16_t>(face.adjacentFaces[corner]));
                out.u16(faceOffset+26u+corner*2u,static_cast<std::uint16_t>(face.indices[corner]));
            }
            faceOffset+=32u;
        }
    } else {
        // The engine writer skips the whole list fixup for count zero,
        // preserving its copied capacity after all faces were pruned
        // (K2 0x6e1998, 0x7cfe41..0x7cfe53). Keep its process pointer zero.
        out.u32(nodeOffset+0x60u,mesh.engineFaceCapacity);
    }
    if(!mesh.engineLeftoverFaces.empty()) {
        auto at=out.array(nodeOffset+0xf4u,mesh.engineLeftoverFaces.size(),4u);
        for(const auto value:mesh.engineLeftoverFaces) {out.u32(at,value);at+=4u;}
    }
    auto counts=out.array(nodeOffset+0x100u,mesh.engineIndexCounts.size(),4u);
    for(const auto count:mesh.engineIndexCounts) {out.u32(counts,static_cast<std::uint32_t>(count));counts+=4u;}
    if(vertexCount==0u) {
        // The engine sizing pass includes both index tables and every pool
        // before the writer's zero-vertex exit (K2 0x7cf31f..0x7cf36a,
        // 0x7cff29). The accepted animmesh backport can retain an empty batch.
        // Those reserved bytes remain zero at the end of the allocation;
        // advancing this node's cursor would change subsequent offsets.
        context.mdlUnwrittenBytes+=mesh.engineDraws.size()*8u+
            mesh.engineDrawIndices.size()*2u;
        return;
    }
    const auto count=static_cast<std::size_t>(vertexCount);
    auto positions=out.append(count*12u);
    out.u32(nodeOffset+context.meshEnd()-4u,positions);
    for(std::size_t vertex=0;vertex<count;++vertex) {
        out.vec3(positions,mesh.cpuPositions->at(vertex));positions+=12u;
    }
    // Engine allocation size is exactly requestedCount*2. The output's
    // separate process-token list is overwritten by ResetTriMesh; keep its
    // count and canonical zero tokens, while relocating each source list.
    const auto batches=mesh.engineDraws.size();
    const auto offsets=out.array(nodeOffset+0x10cu,batches,4u);
    out.array(nodeOffset+0x118u,batches,4u);
    std::size_t firstIndex{};
    for(std::size_t batch=0;batch<batches;++batch) {
        const auto indices=static_cast<std::size_t>(mesh.engineIndexCounts.at(batch));
        auto at=out.append(indices*2u);out.u32(offsets+batch*4u,at);
        for(std::size_t index=0;index<indices;++index) {
            out.u16(at,static_cast<std::uint16_t>(mesh.engineDrawIndices.at(firstIndex+index)));at+=2u;
        }
        firstIndex+=indices;
    }
    // BuildVertexIndices' enabled-tristrips==0 authoring path has no tail.
    out.u32(nodeOffset+0x124u,0xffffffffu);
    out.u32(nodeOffset+0x128u,0xffffffffu);
}

} // namespace

void buildMdx(Context& context) {
    const auto& model=context.model;
    // Engine writes the complete model pool, including completed allocations
    // whose source node was discarded by a subsequent root replacement.
    std::vector<const Mesh*> sources;
    sources.reserve(model.asciiMdxAllocations.size());
    for(const auto& allocation:model.asciiMdxAllocations)sources.push_back(allocation.source.get());
    for(const auto& mesh:model.meshes)
        if(mesh.asciiMdxAllocation)sources.at(*mesh.asciiMdxAllocation)=&mesh;
    for(const auto& animation:model.animations)
        for(const auto& node:animation.nodes)
            if(node.animmesh && node.animmesh->authoringMesh && node.animmesh->authoringMesh->asciiMdxAllocation)
                sources.at(*node.animmesh->authoringMesh->asciiMdxAllocation)=node.animmesh->authoringMesh.get();
    for(const auto* mesh:sources)appendMdxPool(context,*mesh);
    // PartSkin allocates after all ParseNode publications, in engine sibling
    // order, and its PostProcess override does not traverse child parts.
    const auto appendSkin=[&](const auto& self,std::size_t index)->void {
        const auto& node=model.nodes[index];
        if(node.mesh && model.meshes[*node.mesh].skinned) {
            const auto& mesh=model.meshes[*node.mesh];
            // Two Parts may share one MdlNode. The first PostProcess writes
            // its vertex/inverse-bind counts; subsequent calls return before a new
            // allocation (PartSkin::PostProcess, K1 0x5e3ed4/K2 0x711850).
            if(mesh.mdxRowSize!=0u && context.mdxOffsets.count(&mesh)==0u)
                appendMdxPool(context,mesh);
            return;
        }
        for(const auto child:node.children)self(self,child);
    };
    for(std::size_t index=0;index<model.nodes.size();++index)
        if(!model.nodes[index].parent)appendSkin(appendSkin,index);
}

void writeMesh(Context& context,const Node& node,const Mesh& mesh,std::uint32_t nodeOffset) {
    // Saber owns separate 176-row streams; it has no interleaved/prelight
    // source even though its engine mesh header carries that vertex count.
    const auto count=node.lightsaber?static_cast<std::uint16_t>(node.lightsaber->positions.size()):
        static_cast<std::uint16_t>(signedVertexCount(mesh));
    meshHeader(context,node,mesh,nodeOffset,count);
    writeMeshSpecialized(context,node,mesh,nodeOffset);
    meshPayload(context,mesh,nodeOffset,count);
}

} // namespace neomdl::model::packed_export
