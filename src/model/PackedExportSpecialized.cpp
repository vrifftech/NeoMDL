#include <model/PackedExportInternal.hpp>

namespace neomdl::model::packed_export {
namespace {

void writeSkin(Context& context,const Mesh& mesh,std::size_t extension) {
    auto& output=context.mdl;
    // K2 IODispatcher 0x7cf9cc, K1 Desktop ResetSkin 0x321698. These
    // offsets follow the common triangle header, whose size differs by 8.
    output.u32(extension+12u,mesh.mdxWeightOffset);
    output.u32(extension+16u,mesh.mdxBoneIndexOffset);
    const auto* source=mesh.asciiSkinSource.get();
    const auto mapCount=source?source->boneToPalette.size():0u;
    const auto map=output.append(mapCount*4u);
    output.u32(extension+20u,map);
    output.u32(extension+24u,static_cast<std::uint32_t>(mapCount));
    for(std::size_t i=0;i<mapCount;++i)output.f32(map+i*4u,source->boneToPalette[i]);

    // The runtime arrays use stable node indices; the retained authoring
    // pools preserve engine DFS order, including an authored qbone prefix.
    const auto& orientations=source?source->inverseOrientations:mesh.inverseBindOrientations;
    const auto& translations=source?source->inverseTranslations:mesh.inverseBindTranslations;
    const auto quaternions=output.array(extension+28u,orientations.size(),16u);
    for(std::size_t i=0;i<orientations.size();++i)
        output.quaternion(quaternions+i*16u,orientations[i]);
    const auto positions=output.array(extension+40u,translations.size(),12u);
    for(std::size_t i=0;i<translations.size();++i)
        output.vec3(positions+i*12u,translations[i]);

    // PartSkin resizes the integer cache, retaining any authored prefix.
    // Newly allocated engine integer slots and unused short palette slots
    // have no initialized producer; canonical authoring output zeros them.
    const auto constantCount=source?source->boneConstantCount:mesh.boneConstantIndices.size();
    // K2 0x712d59 allocates DFS-count integers, then 0x712d8a copies the
    // prior count. A longer authored prefix exceeds that engine allocation.
    if(mesh.boneConstantIndices.size()>constantCount)
        throw ModelError("Skin boneconstantindices exceeds the engine DFS-sized allocation");
    const auto constants=output.array(extension+52u,constantCount,4u);
    for(std::size_t i=0;i<mesh.boneConstantIndices.size();++i)
        output.u32(constants+i*4u,static_cast<std::uint32_t>(mesh.boneConstantIndices[i]));
    if(source)
        for(std::size_t i=0;i<source->paletteToBone.size();++i)
            output.u16(extension+64u+i*2u,static_cast<std::uint16_t>(source->paletteToBone[i]));
}

void writeDangly(Context& context,const Mesh& mesh,std::size_t extension) {
    auto& output=context.mdl;
    const auto& data=*mesh.dangly;
    const auto constraints=output.array(extension,data.constraints.size(),4u);
    for(std::size_t i=0;i<data.constraints.size();++i)
        output.f32(constraints+i*4u,data.constraints[i]);
    output.f32(extension+12u,data.displacement);
    output.f32(extension+16u,data.tightness);
    output.f32(extension+20u,data.period);
    // Writer 0x7cf81b copies the independent rest-position pool, exactly N.
    const auto positions=output.append(static_cast<std::size_t>(mesh.enginePrelightVertexCount)*12u);
    output.u32(extension+24u,positions);
    for(std::size_t i=0;i<mesh.enginePrelightVertexCount;++i)
        output.vec3(positions+i*12u,(*data.restPositions)[i]);
}

void writeAnimMesh(Context& context,const Node& node,std::size_t extension) {
    auto& output=context.mdl;
    const auto& data=*node.animmesh;
    output.f32(extension,data.samplePeriod);
    // Temporary source arrays at +4/+16/+28 have been removed by engine
    // cleanup. Final KOTOR32 sample pools are vertex-major, at +40/+44.
    // NW's accepted producer allocates when the frame count is nonzero,
    // including new[0] for a mesh with no generated vertices (0x457cd9).
    // The KOTOR writer tests pointer presence, publishing the current cursor
    // even when the following copy has zero bytes (0x7cf691/0x7cf6e5).
    if(data.positionSampleCount!=0) {
        const auto positions=output.append(data.positionSamples.size()*12u);
        output.u32(extension+40u,positions);
        for(std::size_t i=0;i<data.positionSamples.size();++i)
            output.vec3(positions+i*12u,data.positionSamples[i]);
    }
    if(data.texcoordSampleCount!=0) {
        const auto texcoords=output.append(data.texcoordSamples.size()*8u);
        output.u32(extension+44u,texcoords);
        for(std::size_t i=0;i<data.texcoordSamples.size();++i)
            output.vec2(texcoords+i*8u,data.texcoordSamples[i]);
    }
    output.u32(extension+48u,static_cast<std::uint32_t>(data.positionSampleCount));
    output.u32(extension+52u,static_cast<std::uint32_t>(data.texcoordSampleCount));
}

void writeLightsaber(Context& context,const Node& node,std::size_t extension) {
    auto& output=context.mdl;
    const auto& data=*node.lightsaber;
    // IODispatcher 0x7cfb38: position, normal, UV payload order. Pool
    // handles at +12/+16 are process values, zero in authoring output.
    const auto positions=output.append(data.positions.size()*12u);
    output.u32(extension,positions);
    for(std::size_t i=0;i<data.positions.size();++i)
        output.vec3(positions+i*12u,data.positions[i]);
    const auto normals=output.append(data.normals.size()*12u);
    output.u32(extension+8u,normals);
    for(std::size_t i=0;i<data.normals.size();++i)
        output.vec3(normals+i*12u,data.normals[i]);
    const auto texcoords=output.append(data.texcoords.size()*8u);
    output.u32(extension+4u,texcoords);
    for(std::size_t i=0;i<data.texcoords.size();++i)
        output.vec2(texcoords+i*8u,data.texcoords[i]);
}

std::uint32_t writeAabb(Context& context,const AabbNodeData& tree,std::size_t index) {
    context.job.check();
    auto& output=context.mdl;
    const auto& node=tree.nodes[index];
    const auto offset=output.append(40u);
    output.vec3(offset,node.minimum);output.vec3(offset+12u,node.maximum);
    output.u32(offset+32u,static_cast<std::uint32_t>(node.faceIndex));
    output.u32(offset+36u,node.splitMask);
    // Engine local writer 0x7cfd70 relocates children in left-first order.
    if(node.firstChild>=0)
        output.u32(offset+24u,writeAabb(context,tree,static_cast<std::size_t>(node.firstChild)));
    if(node.secondChild>=0)
        output.u32(offset+28u,writeAabb(context,tree,static_cast<std::size_t>(node.secondChild)));
    return offset;
}

} // namespace

void writeMeshSpecialized(Context& context,const Node& node,const Mesh& mesh,
                          std::uint32_t nodeOffset) {
    const auto extension=static_cast<std::size_t>(nodeOffset)+context.meshEnd();
    switch(node.flags) {
    case 0x61u: writeSkin(context,mesh,extension);break;
    case 0x121u: writeDangly(context,mesh,extension);break;
    case 0xa1u: writeAnimMesh(context,node,extension);break;
    case 0x821u: writeLightsaber(context,node,extension);break;
    case 0x221u:
        if(node.aabb->root)
            context.mdl.u32(extension,writeAabb(context,*node.aabb,*node.aabb->root));
        break;
    default: break;
    }
}

} // namespace neomdl::model::packed_export
