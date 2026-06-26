#include <model/PackedExportInternal.hpp>

#include <algorithm>

namespace neomdl::model::packed_export {
namespace {

void fixedName(Buffer& buffer,std::size_t at,const std::string& value,
               const char* description) {
    // MaxTree and animation/event names occupy 32-byte C strings. The
    // active x86 authoring constructor uses __strcpy_chk(...,32), not a
    // truncating copy (K2 0x6ec222). Do not silently lose authored bytes.
    if(value.size()>=32u)
        throw ModelError(std::string(description)+" exceeds the engine 32-byte name field");
    buffer.string(at,value,32u);
}

void writeControllers(Context& context,const Node& node,std::uint32_t offset) {
    auto& out=context.mdl;
    // IODispatcher 0x7d040d..0x7d048b appends descriptors and words only
    // when their counts are nonzero; empty lists retain constructor zeros.
    if(!node.controllers.empty()) {
        const auto records=out.array(offset+56u,node.controllers.size(),16u);
        for(std::size_t i=0;i<node.controllers.size();++i) {
            const auto at=records+i*16u;
            const auto& key=node.controllers[i];
            out.u32(at,key.type);
            out.u16(at+4u,static_cast<std::uint16_t>(key.sourceOffset));
            out.u16(at+6u,key.rowCount);
            out.u16(at+8u,key.timeIndex);
            out.u16(at+10u,key.dataIndex);
            out.u8(at+12u,key.rawFlags);
            // Engine padding13..15 has no semantic consumer. Buffer's
            // canonical zero padding does not fabricate controller values.
        }
    }
    if(!node.controllerDataWords.empty()) {
        const auto values=out.array(offset+68u,node.controllerDataWords.size(),4u);
        for(std::size_t i=0;i<node.controllerDataWords.size();++i)
            out.u32(values+i*4u,node.controllerDataWords[i]);
    }
}

} // namespace

std::uint32_t writeNode(Context& context,const std::vector<Node>& nodes,
                        std::size_t index,std::uint32_t treeOffset,
                        std::uint32_t /*parentOffset*/) {
    context.job.check();
    const auto& node=nodes.at(index);
    auto& out=context.mdl;
    const auto offset=out.append(nodeRecordSize(context,node));
    // Engine records every occurrence but its relocation lookup selects the
    // first matching source pointer (0x7d02e0/0x7d0330). Retain that identity
    // when repeated parent directives leave a node in multiple child lists.
    context.nodeOffsets.emplace(&node,offset);
    out.u16(offset,node.flags);
    out.u16(offset+2u,node.id);
    out.u32(offset+4u,node.engineNameIndex);
    out.u32(offset+8u,treeOffset);
    if(node.parent) {
        const auto parent=context.nodeOffsets.find(&nodes.at(*node.parent));
        // A source parent not yet in the engine relocation list resolves to
        // zero (0x7d0318..0x7d034b), even when this traversal came from another
        // node's child list. Do not substitute the traversal's parent.
        if(parent!=context.nodeOffsets.end()) out.u32(offset+12u,parent->second);
    }
    out.vec3(offset+16u,node.position);
    out.quaternion(offset+28u,node.orientation);

    if(node.flags&kNodeMesh) {
        const auto& mesh=node.mesh?context.model.meshes.at(*node.mesh):
            *node.animmesh->authoringMesh;
        // Specialized pools precede common mesh pools; writeMesh owns that
        // dispatch. Base-node arrays follow both (0x7cf74b/0x7d0290).
        writeMesh(context,node,mesh,offset);
    } else {
        writeNodeFields(context,node,offset);
    }

    // The engine writer publishes a child-table offset even for an empty
    // table, then emits descendants before this node's controller arrays.
    const auto children=out.array(offset+44u,node.children.size(),4u);
    for(std::size_t i=0;i<node.children.size();++i) {
        const auto child=writeNode(context,nodes,node.children[i],treeOffset,offset);
        out.u32(children+i*4u,child);
    }
    writeControllers(context,node,offset);
    return offset;
}

std::uint32_t writeAnimation(Context& context,const Animation& animation) {
    context.job.check();
    auto& out=context.mdl;
    const auto offset=out.append(136u);
    // The packed loaders replace both dispatcher words. Runtime free lists,
    // refcount and loaded-data pointer retain their constructor/reset zeros.
    // K1 ResetAnimation 0x322179/0x322204; K2 writer0x7ce485.
    fixedName(out,offset+8u,animation.name,"Animation name");
    out.u32(offset+44u,animation.engineNodeCount);
    out.u8(offset+76u,5u);
    out.f32(offset+80u,animation.length);
    out.f32(offset+84u,animation.transitionTime);
    fixedName(out,offset+88u,animation.root,"Animation root name");
    if(!animation.events.empty()) {
        const auto events=out.array(offset+120u,animation.events.size(),36u);
        for(std::size_t i=0;i<animation.events.size();++i) {
            const auto at=events+i*36u;
            out.f32(at,animation.events[i].time);
            fixedName(out,at+4u,animation.events[i].name,"Animation event name");
        }
    }
    const auto root=std::find_if(animation.nodes.begin(),animation.nodes.end(),
        [](const Node& node){return !node.parent;});
    if(root!=animation.nodes.end())
        out.u32(offset+40u,writeNode(context,animation.nodes,
            static_cast<std::size_t>(root-animation.nodes.begin()),offset,0u));
    return offset;
}

} // namespace neomdl::model::packed_export
