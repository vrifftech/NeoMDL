#include <model/PackedExportInternal.hpp>

#include <algorithm>
#include <utility>

namespace neomdl::model {

PackedExportResult exportPackedModel(const Model& model,
                                    const neoshared::bif::JobControl& job) {
    if(!model.asciiBuilt)
        throw ModelError("Packed compilation requires an ASCII-built model");
    if(!model.asciiMaterialsFinalized)
        throw ModelError("Packed compilation requires finalized ASCII model materials");

    packed_export::Context context(model,job);
    job.update(0u,4u,"Compiling model vertex data: "+model.name);
    packed_export::buildMdx(context);
    auto& output=context.mdl;
    output.append(0xc4u);
    // Canonical PC format identifiers already used by the packed decoder.
    // Engine input replaces both callback slots before invoking them.
    output.u32(0u,context.k2()?4285200u:4273776u);
    output.u32(4u,context.k2()?4216320u:4216096u);
    output.string(8u,model.name,32u);
    output.u32(0x2cu,model.engineNodeCount);
    output.u8(0x4cu,2u);
    output.u16(0x50u,model.classification);
    output.u8(0x52u,model.subclassification);
    output.u8(0x53u,model.fog?1u:0u);
    output.vec3(0x68u,model.engineBounds.minimum);
    output.vec3(0x74u,model.engineBounds.maximum);
    output.f32(0x80u,model.engineRadius);
    output.f32(0x84u,model.animationScale);
    output.string(0x88u,model.engineSupermodelName,32u);
    output.u32(0xb0u,static_cast<std::uint32_t>(context.mdx.bytes.size()));

    // K2 IODispatcher::Write 0x7ce11b: names precede animations, including
    // names from discarded roots. ParseNode retains their original indices.
    job.update(1u,4u,"Compiling model names and animations: "+model.name);
    const auto names=output.array(0xb8u,model.engineNames.size(),4u);
    for(std::size_t i=0;i<model.engineNames.size();++i) {
        job.check();
        const auto offset=output.text(model.engineNames[i]);
        output.u32(names+i*4u,offset);
    }
    const auto animations=output.array(0x58u,model.animations.size(),4u);
    for(std::size_t i=0;i<model.animations.size();++i) {
        const auto offset=packed_export::writeAnimation(context,model.animations[i]);
        output.u32(animations+i*4u,offset);
    }

    // The full engine writer emits animation trees before the base tree.
    // Child tables and every descendant precede a node's controller arrays.
    job.update(2u,4u,"Compiling model node tree: "+model.name);
    const auto root=std::find_if(model.nodes.begin(),model.nodes.end(),
        [](const Node& node) { return !node.parent; });
    if(root!=model.nodes.end()) {
        const auto offset=packed_export::writeNode(context,model.nodes,
            static_cast<std::size_t>(root-model.nodes.begin()),0u,0u);
        output.u32(0x28u,offset);
    }
    if(model.animationRootNode) {
        const auto found=context.nodeOffsets.find(&model.nodes[*model.animationRootNode]);
        // The engine pointer-map lookup also publishes zero for a discarded
        // animation-root pointer (K2 0x7ce4f0..0x7ce51c).
        if(found!=context.nodeOffsets.end()) output.u32(0xa8u,found->second);
    }

    job.update(3u,4u,"Writing packed model wrapper: "+model.name);
    // IODispatcher writes its full zeroed sizing-pass allocation, not just
    // the final publication cursor (K2 0x7cdf4e..0x7cdf6c, 0x7ce522).
    output.append(context.mdlUnwrittenBytes);
    packed_export::Buffer wrapped;
    wrapped.append(12u);
    wrapped.u32(4u,static_cast<std::uint32_t>(output.bytes.size()));
    wrapped.u32(8u,static_cast<std::uint32_t>(context.mdx.bytes.size()));
    wrapped.bytes.insert(wrapped.bytes.end(),output.bytes.begin(),output.bytes.end());
    job.update(4u,4u,"Compiled packed model: "+model.name);
    return {std::move(wrapped.bytes),std::move(context.mdx.bytes)};
}

} // namespace neomdl::model
