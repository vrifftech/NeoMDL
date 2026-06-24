#include <model/AsciiExportInternal.hpp>
#include <model/AsciiModelInternal.hpp>
#include <model/EngineCpuSkin.hpp>
#include <model/EnginePrelighting.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace neomdl::model::ascii_export {
namespace {

std::vector<std::size_t> depthFirstNodes(const Model& model,std::size_t node) {
    while(model.nodes.at(node).parent) node=*model.nodes[node].parent;
    std::vector<std::size_t> order;
    const auto visit=[&](const auto& self,std::size_t index) -> void {
        order.push_back(index);
        for(const auto child:model.nodes[index].children) self(self,child);
    };
    visit(visit,node);
    return order;
}

bool same(Vec3 left,Vec3 right) noexcept {
    return left.x==right.x && left.y==right.y && left.z==right.z;
}

} // namespace

void writeSkin(Writer& writer,const Model& model,const Mesh& mesh,
               const ReconstructedMesh& generated) {
    if(!mesh.engineCpuSkin && !mesh.builtFloatSkinChannels)
        throw ModelError("ASCII skin export requires the engine float weight/index channels for node "+
                         model.nodes.at(mesh.nodeIndex).name);
    const auto channels=decodeEngineFloatSkinChannels(mesh);
    const auto nodes=depthFirstNodes(model,mesh.nodeIndex);
    const auto boneName=[&](float engineIndex) {
        // A named ASCII influence rebuilds an integral compact-palette slot.
        // Fractional/invalid shader indices have no equivalent bone-name
        // record; do not replace them with repaired inspection influences.
        if(!std::isfinite(engineIndex) || engineIndex<0.0f ||
           engineIndex>=static_cast<float>(mesh.skinBoneMapSize) ||
           std::trunc(engineIndex)!=engineIndex)
            throw ModelError("ASCII skin export cannot name the engine palette index "+
                             writer.scalar(engineIndex)+" in node "+model.nodes.at(mesh.nodeIndex).name);
        const auto mapped=mesh.skinBoneMap.at(static_cast<std::size_t>(engineIndex));
        if(mapped<0 || static_cast<std::size_t>(mapped)>=model.nodes.size())
            throw ModelError("ASCII skin export has a engine palette entry without a model bone in node "+
                             model.nodes.at(mesh.nodeIndex).name);
        const auto bone=static_cast<std::size_t>(mapped);
        if(bone==nodes.front()) return std::string{"ROOT"};
        const auto& name=model.nodes[bone].name;
        // FindPartIndexDFS treats ROOT specially and otherwise selects the
        // first case-insensitive name in sibling-ordered depth-first order.
        const auto first=std::find_if(nodes.begin(),nodes.end(),[&](std::size_t index) {
            return ascii::equal(model.nodes[index].name,name);
        });
        if(ascii::equal(name,"ROOT") || first==nodes.end() || *first!=bone)
            throw ModelError("ASCII skin export cannot uniquely address bone "+name+
                             " through engine FindPartIndexDFS");
        return writer.token(name,"skin bone name");
    };

    writer.line("weights "+std::to_string(generated.sourceVertices.size()));
    for(const auto source:generated.sourceVertices) {
        writer.job().check();
        const auto& influence=channels.at(source);
        std::string row;
        for(std::size_t channel=0;channel<4u;++channel) {
            if(channel) row+=' ';
            // Engine omitted/zero influences use ROOT with zero weight.
            // Preserve authored weights, including their sign and total;
            // there is no sorting or normalization in the ASCII producer.
            row+=influence.weights[channel]==0.0f
                ? "ROOT" : boneName(influence.indices[channel]);
            row+=' ';row+=writer.scalar(influence.weights[channel]);
        }
        writer.line(row);
    }
    // Supplying tbone_ref_inv would make PartSkin::PostProcess skip the
    // complete first-instance builder, including its generated draw pool.
    // Functional authoring output therefore lets the engine producer derive
    // both bind arrays from the model's initial node/controller transforms.
    writer.warning("Skin "+model.nodes.at(mesh.nodeIndex).name+
                   ": engine ASCII load regenerates inverse bind transforms from the initial node transforms; compiled inverse bind arrays are not emitted.");
}

void writeLightsaber(Writer& writer,const Model& model,const Node& node,
                    const Mesh& mesh,const ReconstructedMesh& generated) {
    if(!mesh.enginePrelightInput || mesh.vertices.empty() || generated.sourceFaces.empty())
        throw ModelError("ASCII lightsaber export requires the authored source mesh and faces for node "+
                         node.name+"; the generated 176-vertex saber pool does not retain them");
    const auto source=decodeEnginePrelightSource(mesh);
    if(!source.authoredColors)
        throw ModelError("ASCII lightsaber export requires engine red/blue selector colors for node "+node.name);

    // ProcessVerts appends a normal at the first face containing a source
    // vertex. A same-face repeated corner appends another entry; saber's
    // distinct producer then indexes that pool directly by source vertex.
    // Establish which selected vertices the generated authoring faces
    // can address without inventing geometry or an explicit-normal field.
    const auto missing=std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> normalSlots(generated.sourceVertices.size(),missing);
    std::vector<bool> encountered(generated.sourceVertices.size());
    std::size_t normalCount{};
    for(const auto& face:generated.sourceFaces) {
        for(const auto index:face) {
            if(!encountered.at(index)) {
                if(normalSlots[index]==missing) normalSlots[index]=normalCount;
                ++normalCount;
            }
        }
        for(const auto index:face) encountered[index]=true;
    }
    for(const auto colorChannel:{0u,2u}) {
        for(std::uint8_t selector=0;selector<LightsaberNodeData::pieceVertices;++selector) {
            writer.job().check();
            const auto engine=std::find_if(source.vertices.begin(),source.vertices.end(),
                [&](const EnginePrelightVertex& value) { return value.color[colorChannel]==selector; });
            if(engine==source.vertices.end())
                throw ModelError("ASCII lightsaber export is missing engine selector "+
                                 std::to_string(selector)+" in node "+node.name);
            const auto output=std::find_if(generated.sourceVertices.begin(),generated.sourceVertices.end(),
                [&](std::size_t index) { return source.vertices.at(index).color[colorChannel]==selector; });
            if(output==generated.sourceVertices.end())
                throw ModelError("ASCII lightsaber export has no generated source for selector "+
                                 std::to_string(selector)+" in node "+node.name);
            const auto canonical=static_cast<std::size_t>(output-generated.sourceVertices.begin());
            const auto engineIndex=static_cast<std::size_t>(engine-source.vertices.begin());
            const auto& selected=source.vertices.at(*output);
            const auto engineUv=mesh.vertices.at(engineIndex).texcoord0;
            const auto outputUv=mesh.vertices.at(*output).texcoord0;
            if(!same(engine->position,selected.position) || !same(engine->normal,selected.normal) ||
               engineUv.x!=outputUv.x || engineUv.y!=outputUv.y)
                throw ModelError("ASCII lightsaber face ordering changes the first engine selector match in node "+node.name);
            if(normalSlots.at(canonical)!=canonical)
                throw ModelError("ASCII lightsaber source faces cannot reproduce the engine normal-pool index for node "+node.name);
        }
    }

    writer.line("colors "+std::to_string(generated.sourceVertices.size()));
    for(const auto index:generated.sourceVertices) {
        writer.job().check();
        const auto& color=source.vertices.at(index).color;
        // The parser's int(color*255+0.5) recovers the original byte code.
        // Keep all original selector channels; no synthetic unused color is
        // assigned to hide duplicate codes or change first-match semantics.
        writer.line(writer.vector({static_cast<float>(color[0])/255.0f,
                                   static_cast<float>(color[1])/255.0f,
                                   static_cast<float>(color[2])/255.0f}));
    }
    (void)model;
}

} // namespace neomdl::model::ascii_export
