#include <model/PackedExportInternal.hpp>

namespace neomdl::model::packed_export {

std::size_t nodeRecordSize(const Context& context,const Node& node) {
    // K2 IODispatcher's node writer copies these complete x86 records before
    // relocating their pools (0x7cf3e0). K1's common mesh record is 8 bytes
    // shorter; its specialized extensions have the same sizes.
    switch(node.flags) {
    case kNodeBase:
    case kNodeBase|kNodeCamera:
    case kNodeBase|kNodeTrigger:return 80u;
    case kNodeBase|kNodeLight:return 172u;
    case kNodeBase|kNodeEmitter:return 304u;
    case kNodeBase|kNodeReference:return 116u;
    case kNodeBase|kNodeMesh:return context.meshEnd();
    case kNodeBase|kNodeMesh|kNodeSkin:return context.meshEnd()+100u;
    case kNodeBase|kNodeMesh|kNodeAnimation:return context.meshEnd()+56u;
    case kNodeBase|kNodeMesh|kNodeDangly:return context.meshEnd()+28u;
    case kNodeBase|kNodeMesh|kNodeAabb:return context.meshEnd()+4u;
    case kNodeBase|kNodeMesh|kNodeLightsaber:return context.meshEnd()+20u;
    default:throw ModelError("Packed export has no engine node record for flags "+
                             std::to_string(node.flags));
    }
}

void writeNodeFields(Context& context,const Node& node,std::uint32_t nodeOffset) {
    auto& out=context.mdl;
    const auto at=static_cast<std::size_t>(nodeOffset);
    if(node.flags==(kNodeBase|kNodeLight)) {
        if(!node.light)throw ModelError("Packed export is missing the light extension for "+node.name);
        const auto& light=*node.light;
        out.f32(at+80u,light.flareRadius);
        // The runtime texture-object array at +84 is deliberately left zero:
        // the engine writer clears it at 0x7cfc26..0x7cfc43. Authoring arrays
        // are emitted in engine order: names, sizes, positions, color shifts.
        // These switches are authored %d words, not bool bytes. The engine
        // record copy preserves values such as 2 and -1 (K2 0x7cf8ad).
        out.u32(at+144u,static_cast<std::uint32_t>(light.priority));
        out.u32(at+148u,static_cast<std::uint32_t>(light.ambientOnly));
        out.u32(at+152u,static_cast<std::uint32_t>(light.dynamicType));
        out.u32(at+156u,static_cast<std::uint32_t>(light.affectDynamic));
        out.u32(at+160u,static_cast<std::uint32_t>(light.castsShadow));
        out.u32(at+164u,static_cast<std::uint32_t>(light.generateFlare));
        out.u32(at+168u,static_cast<std::uint32_t>(light.fading));
        if(!light.textureNames.empty()) {
            const auto names=out.array(at+132u,light.textureNames.size(),4u);
            for(std::size_t i=0;i<light.textureNames.size();++i) {
                context.job.check();
                const auto text=out.text(light.textureNames[i]);
                out.u32(names+i*4u,text);
            }
        }
        if(!light.flareSizes.empty()) {
            const auto values=out.array(at+96u,light.flareSizes.size(),4u);
            for(std::size_t i=0;i<light.flareSizes.size();++i)
                out.f32(values+i*4u,light.flareSizes[i]);
        }
        if(!light.flarePositions.empty()) {
            const auto values=out.array(at+108u,light.flarePositions.size(),4u);
            for(std::size_t i=0;i<light.flarePositions.size();++i)
                out.f32(values+i*4u,light.flarePositions[i]);
        }
        if(!light.flareColorShifts.empty()) {
            const auto values=out.array(at+120u,light.flareColorShifts.size(),12u);
            for(std::size_t i=0;i<light.flareColorShifts.size();++i)
                out.vec3(values+i*12u,light.flareColorShifts[i]);
        }
    } else if(node.flags==(kNodeBase|kNodeEmitter)) {
        if(!node.emitter)throw ModelError("Packed export is missing the emitter extension for "+node.name);
        const auto& emitter=*node.emitter;
        // Authoring stores semantic values in the typed fields and OR flags
        // in rawWords.back(). Known fixed-string tails retain earlier writes.
        out.f32(at+80u,emitter.deadSpace);
        out.f32(at+84u,emitter.blastRadius);
        out.f32(at+88u,emitter.blastLength);
        out.u32(at+92u,emitter.branchCount);
        out.u32(at+96u,static_cast<std::uint32_t>(emitter.controlPointSmoothing));
        out.u32(at+100u,emitter.xGrid);
        out.u32(at+104u,emitter.yGrid);
        out.u32(at+108u,emitter.spawnType);
        out.string(at+112u,emitter.updateMode,emitter.asciiUpdateMode);
        out.string(at+144u,emitter.renderMode,emitter.asciiRenderMode);
        out.string(at+176u,emitter.blendMode,emitter.asciiBlendMode);
        out.string(at+208u,emitter.texture,emitter.asciiTexture);
        out.string(at+240u,emitter.chunkName,emitter.asciiChunkName);
        out.u32(at+256u,static_cast<std::uint32_t>(emitter.twoSidedTexture));
        out.u32(at+260u,static_cast<std::uint32_t>(emitter.loop));
        out.u16(at+264u,emitter.renderOrder);
        out.u8(at+266u,emitter.frameBlending?1u:0u);
        out.string(at+267u,emitter.depthTexture,emitter.asciiDepthTexture);
        out.u32(at+300u,emitter.rawWords.back());
    } else if(node.flags==(kNodeBase|kNodeReference)) {
        if(!node.reference)throw ModelError("Packed export is missing the reference extension for "+node.name);
        out.string(at+80u,node.reference->model,node.reference->asciiModel);
        out.u32(at+112u,static_cast<std::uint32_t>(node.reference->reattachable));
    }
    // Base, camera and trigger records have no extension. Mesh and AABB
    // records are emitted by their specialized writers.
}

} // namespace neomdl::model::packed_export
