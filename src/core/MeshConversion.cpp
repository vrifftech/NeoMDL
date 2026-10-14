#include "core/MeshConversion.hpp"
#include <model/AsciiExportInternal.hpp>
#include <model/AsciiSource.hpp>

#include <set>

namespace neomdl {
namespace {

model::Vec3 cross(model::Vec3 a, model::Vec3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
float dot(model::Vec3 a, model::Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }

} // namespace

std::string convertMeshToTrimesh(const std::string& text, const model::Model& asset,
    std::size_t nodeIndex, bool advanced, const neoshared::bif::JobControl& job) {
    using model::ascii::SourceText;
    const auto& node = asset.nodes[nodeIndex];
    const auto& mesh = asset.meshes[*node.mesh];
    auto vertices = mesh.vertices;
    auto faces = mesh.faces;
    auto channels = mesh.mdxBitmap;
    if (mesh.skinned) {
        job.update(0, 0, "Baking constructed skin pose: " + node.name);
        const auto pose = model::constructedBindPose(asset, job);
        const auto& deformed = pose.deformedMeshes[*node.mesh];
        const auto& m = pose.worldTransforms[nodeIndex].values;
        const model::Vec3 x{m[0],m[1],m[2]}, y{m[4],m[5],m[6]}, z{m[8],m[9],m[10]};
        const auto yz = cross(y,z), zx = cross(z,x), xy = cross(x,y);
        const auto determinant = dot(x,yz);
        const auto localDirection = [&](model::Vec3 v) -> model::Vec3 {
            return {dot(yz,v)/determinant, dot(zx,v)/determinant, dot(xy,v)/determinant};
        };
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            job.check();
            auto& v = vertices[i];
            const auto p = deformed.positions.at(i);
            v.position = localDirection({p.x-m[12],p.y-m[13],p.z-m[14]});
            const auto n = deformed.normals.at(i);
            v.normal = model::normalize({dot(x,n),dot(y,n),dot(z,n)});
            v.tangentBasis = {localDirection(deformed.bitangents.at(i)),
                localDirection(deformed.tangents.at(i)), localDirection(deformed.basisNormals.at(i)),
                v.tangentBasis.valid};
        }
    } else {
        job.update(0, 0, "Converting stationary blade pieces: " + node.name);
        const auto& saber = *node.lightsaber;
        vertices.clear(); faces.clear(); channels = 0x23u;
        for (const auto base : {model::LightsaberNodeData::frontPieceOffset, model::LightsaberNodeData::backPieceOffset})
            for (std::size_t i = 0; i < model::LightsaberNodeData::pieceVertices; ++i) {
                model::Vertex v;
                v.position = saber.positions.at(base+i); v.normal = saber.normals.at(base+i);
                v.texcoord0 = saber.texcoords.at(base+i); vertices.push_back(v);
            }
        constexpr std::array<std::array<std::int16_t,3>,12> triangles{{
            {{0,4,5}},{{0,5,1}},{{1,5,6}},{{1,6,2}},{{2,6,7}},{{2,7,3}},
            {{13,12,8}},{{9,13,8}},{{14,13,9}},{{10,14,9}},{{15,14,10}},{{11,15,10}}}};
        for (const auto& triangle : triangles) { model::MeshFace face; face.indices = triangle; faces.push_back(face); }
    }
    model::AsciiExportOptions options; options.advanced = advanced; options.explicitNormals = true;
    model::ascii_export::Writer writer(asset.game, job, options); writer.indent(2);
    const auto array = [&](const char* name, const auto& value) {
        writer.line(std::string(name) + " " + std::to_string(vertices.size()));
        for (const auto& vertex : vertices) writer.line(value(vertex));
    };
    array("verts", [&](const auto& v) { return writer.vector(v.position); });
    array("normals", [&](const auto& v) { return writer.vector(v.normal); });
    const auto indices = [&](const model::MeshFace& face) {
        return std::to_string(static_cast<std::uint16_t>(face.indices[0])) + " " +
            std::to_string(static_cast<std::uint16_t>(face.indices[1])) + " " +
            std::to_string(static_cast<std::uint16_t>(face.indices[2]));
    };
    const auto corners = [&](const std::string& name) {
        writer.line(name + " " + std::to_string(faces.size()));
        for (const auto& face : faces) writer.line(indices(face));
    };
    corners("normalindices");
    for (unsigned channel = 0; channel < 4; ++channel) if (channels & (2u << channel)) {
        const auto name = "tverts" + (channel ? std::to_string(channel) : std::string{});
        array(name.c_str(), [&](const auto& v) {
            const model::Vec2 uvs[]{v.texcoord0,v.texcoord1,v.texcoord2,v.texcoord3};
            return writer.scalar(uvs[channel].x) + " " + writer.scalar(uvs[channel].y) + " 0";
        });
        if (channel) corners("texindices" + std::to_string(channel));
    }
    if (channels & 0x40u) {
        array(advanced ? "colorsrgba" : "colors", [&](const auto& v) {
            if (!advanced) return writer.vector({v.color[0]/255.0f,v.color[1]/255.0f,v.color[2]/255.0f});
            return std::to_string(v.color[0])+" "+std::to_string(v.color[1])+" "+std::to_string(v.color[2])+" "+std::to_string(v.color[3]);
        });
        if (advanced) corners("colorindices");
    }
    if (advanced) {
        writer.line(std::string("hastangentbasis ") + ((channels & 0x80u) ? "1" : "0"));
        if (channels & 0x80u) {
            array("tangentbasis", [&](const auto& v) {
                return writer.vector(v.tangentBasis.bitangent)+" "+writer.vector(v.tangentBasis.tangent)+" "+writer.vector(v.tangentBasis.normal);
            });
            corners("tangentindices");
        }
    }
    writer.line("faces " + std::to_string(faces.size()));
    for (const auto& face : faces)
        writer.line(indices(face)+" 0 "+((channels & 2u) ? indices(face) : "0 0 0")+" "+std::to_string(face.surfaceMaterial));
    const SourceText geometry(writer.finish().text);
    static const std::set<std::string> removed{
        "verts","normals","normalindices","normalpositionprecision","tangentuvprecision",
        "tverts","tverts1","tverts2","tverts3","texindices1","texindices2","texindices3",
        "colors","colorsrgba","colorindices","hastangentbasis","tangentbasis","tangentindices",
        "cpuverts","cpuindices","faceplanes","faces","bmin","bmax","spherecenter","sphereradius","totalarea",
        "vertexindicescount","leftoverfaces","vertexindices","weights","qbone_ref_inv","tbone_ref_inv",
        "boneconstantindices","skinbonemap","skinpalette","skinbindrotations","skinbindtranslations","skinweights",
        "saberverts","sabernormals","saberuvs","saberoriginalverts"};
    SourceText source(text);
    source.materializeTangentPolicies();
    const auto blocks = source.nodes();
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        if (!source.matches(*block,node,nodeIndex)) continue;
        std::string type; std::istringstream(SourceText::value(source.lines[block->begin])) >> type;
        if (!block->geometry && !model::ascii::equal(type,"skin") && !model::ascii::equal(type,"lightsaber")) continue;
        std::vector<std::string> body;
        for (auto line = block->begin+1; line < block->end; line = source.nextRecord(line))
            if (!removed.count(SourceText::key(source.lines[line])))
                body.insert(body.end(),source.lines.begin()+line,source.lines.begin()+source.nextRecord(line));
        if (block->geometry) body.insert(body.end(),geometry.lines.begin(),geometry.lines.end());
        source.lines[block->begin] = "node trimesh " + block->name;
        source.lines.erase(source.lines.begin()+block->begin+1,source.lines.begin()+block->end);
        source.lines.insert(source.lines.begin()+block->begin+1,body.begin(),body.end());
    }
    return source.text();
}

} // namespace neomdl
