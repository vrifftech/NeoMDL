#include "core/MeshNormals.hpp"
#include <model/AsciiExportInternal.hpp>
#include <model/AsciiSource.hpp>

namespace neomdl {
namespace {

using Vector = std::array<double,3>;

Vector subtract(Vector a, Vector b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
double angle(Vector a, Vector b) {
    const double x=a[1]*b[2]-a[2]*b[1], y=a[2]*b[0]-a[0]*b[2], z=a[0]*b[1]-a[1]*b[0];
    return std::atan2(std::sqrt(x*x+y*y+z*z),a[0]*b[0]+a[1]*b[1]+a[2]*b[2]);
}

} // namespace

std::string recalculateMeshNormals(const std::string& text, const model::Model& asset,
    std::size_t nodeIndex, const MeshNormalOptions& options, const neoshared::bif::JobControl& job) {
    using namespace model::ascii;
    const auto& node=asset.nodes[nodeIndex];
    SourceText source(text);
    const auto blocks=source.nodes();
    const auto& block=*std::find_if(blocks.begin(),blocks.end(),[&](const auto& item) {
        return item.geometry && source.matches(item,node,nodeIndex);
    });
    // Read authored topology with the existing parser, before normal/UV splits
    // and compiled-vertex welding. Equal positions are not shared source vertices.
    std::string geometry;
    for(auto line=block.begin+1;line<block.end;) {
        const auto end=source.nextRecord(line);
        const auto key=SourceText::key(source.lines[line]);
        if(key=="verts" || key=="faces" || key=="normalpositionprecision") {
            geometry+=key+" "+SourceText::value(source.lines[line])+"\n";
            for(auto row=line+1;row<end;++row) geometry+=source.lines[row]+"\n";
        }
        line=end;
    }
    RawNode raw;
    raw.node.flags=node.flags;
    initializeNode(raw,asset.game);
    const std::vector<std::uint8_t> bytes(geometry.begin(),geometry.end());
    Reader reader(bytes,asset.game,node.name,job);
    Line line;
    while(reader.next(line)) parseMeshField(line,reader,raw,asset.game);

    const auto position=[&](std::size_t index) -> Vector {
        if(raw.normalPositionPrecision==64u) return raw.normalPositions64.at(index);
        const auto p=raw.vertices.at(index);
        return {p.x,p.y,p.z};
    };
    struct Face {
        NormalFaceCross cross;
        model::Vec3 unit;
        std::array<double,3> angles{};
    };
    std::vector<Face> faces;
    std::vector<std::vector<std::pair<std::size_t,std::size_t>>> incident(raw.vertices.size());
    const bool weightedAngle=options.weighting==NormalWeighting::CornerAngle || options.weighting==NormalWeighting::AreaAngle;
    for(std::size_t i=0;i<raw.faces.size();++i) {
        job.check();
        const auto& indices=raw.faces[i].vertices;
        const std::array<model::Vec3,3> vertices{{raw.vertices.at(indices[0]),raw.vertices.at(indices[1]),raw.vertices.at(indices[2])}};
        auto cross=normalFaceCross(vertices[0],vertices[1],vertices[2]);
        if(raw.normalPositionPrecision==64u) {
            const auto value=normalPositionFaceCross(vertices,{position(indices[0]),position(indices[1]),position(indices[2])});
            cross={value.x,value.y,value.z};
        }
        Face face{cross,normalizeVertexNormal(cross.accumulate({})),{}};
        for(std::size_t corner=0;corner<3;++corner) {
            if(weightedAngle) {
                const auto p=position(indices[corner]);
                face.angles[corner]=angle(subtract(position(indices[(corner+1)%3]),p),
                    subtract(position(indices[(corner+2)%3]),p));
            }
            auto& adjacent=incident.at(indices[corner]);
            // Match the compiler's last-corner ownership for repeated indices.
            if(!adjacent.empty() && adjacent.back().first==i) adjacent.back().second=corner;
            else adjacent.emplace_back(i,corner);
        }
        faces.push_back(face);
    }

    const double crease=options.creaseDegrees*std::acos(-1.0)/180.0;
    std::vector<model::Vec3> normals;
    std::map<std::array<float,3>,std::uint32_t> normalPool;
    std::vector<std::array<std::uint32_t,3>> normalIndices(raw.faces.size());
    for(std::size_t i=0;i<raw.faces.size();++i) {
        job.check();
        if((i&255u)==0u) job.update(i,raw.faces.size(),"Recalculating normals: "+node.name);
        const auto& current=raw.faces[i];
        for(std::size_t corner=0;corner<3;++corner) {
            model::Vec3 sum{};
            for(const auto& [j,sharedCorner]:incident[current.vertices[corner]]) {
                if(i!=j) {
                    if(options.respectSmoothingGroups && !(current.smoothing&raw.faces[j].smoothing)) continue;
                    const auto a=faces[i].cross, b=faces[j].cross;
                    // Compare with this face, not a transitive smoothing fan.
                    if(options.creaseDegrees<180.0 && angle({a.x,a.y,a.z},{b.x,b.y,b.z})>crease) continue;
                }
                const auto& face=faces[j];
                auto contribution=face.cross;
                if(options.weighting==NormalWeighting::CornerAngle || options.weighting==NormalWeighting::Equal)
                    contribution={face.unit.x,face.unit.y,face.unit.z};
                if(weightedAngle) {
                    const auto weight=face.angles[sharedCorner];
                    contribution={static_cast<float>(contribution.x*weight),
                        static_cast<float>(contribution.y*weight),contribution.z*weight};
                }
                sum=contribution.accumulate(sum);
            }
            const auto normal=normalizeVertexNormal(sum);
            const auto entry=normalPool.emplace(std::array<float,3>{{normal.x,normal.y,normal.z}},
                static_cast<std::uint32_t>(normals.size()));
            if(entry.second) normals.push_back(normal);
            normalIndices[i][corner]=entry.first->second;
        }
    }
    model::ascii_export::Writer writer(asset.game,job);
    std::string normalText=std::to_string(normals.size());
    for(const auto normal:normals) { job.check(); normalText+="\n    "+writer.vector(normal); }
    std::string indexText=std::to_string(normalIndices.size());
    for(const auto& indices:normalIndices)
        indexText+="\n    "+std::to_string(indices[0])+" "+std::to_string(indices[1])+" "+std::to_string(indices[2]);
    Edits edits{{"normals",std::move(normalText)},{"normalindices",std::move(indexText)},
        {"tangentbasis",std::nullopt},{"tangentindices",std::nullopt},
        {"vertexindicescount",std::nullopt},{"vertexindices",std::nullopt},{"leftoverfaces",std::nullopt}};
    // A supplied tangent pool enables tangents even without an explicit policy.
    // Keep that choice when replacing the pool with generated data.
    if(!source.field(block,"tangentbasis").empty() && asset.meshes[*node.mesh].asciiTangents.value_or(false))
        edits["hastangentbasis"]="1";
    source.edit(block,std::move(edits));
    return source.text();
}

} // namespace neomdl
