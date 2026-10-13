#include <model/AsciiExportInternal.hpp>
#include <model/AsciiModelInternal.hpp>
#include <model/AsciiExportSmoothing.hpp>
#include <model/AsciiExportTopology.hpp>
#include <model/EngineCpuSkin.hpp>
#include <model/UvAnimation.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_set>

namespace neomdl::model::ascii_export {
namespace {

bool same(Vec3 a,Vec3 b) {
    return a.x==b.x && a.y==b.y && a.z==b.z;
}
double sphereError(const Mesh& mesh,const std::vector<Vertex>& vertices,
                   const ReconstructedTopology& topology) {
    if(!mesh.engineBounds) return 0.0;
    Vec3 center{};
    for(const auto index:topology.sourceVertices) {
        const auto p=vertices[index].position;
        center={center.x+p.x,center.y+p.y,center.z+p.z};
    }
    if(!topology.sourceVertices.empty()) {
        const float scale=1.0f/static_cast<float>(topology.sourceVertices.size());
        center={center.x*scale,center.y*scale,center.z*scale};
    }
    float radius{};
    for(const auto index:topology.sourceVertices) {
        const auto p=vertices[index].position;
        const Vec3 delta{p.x-center.x,p.y-center.y,p.z-center.z};
        const float distance=std::sqrt((delta.x*delta.x+delta.y*delta.y)+delta.z*delta.z);
        if(distance>radius) radius=distance;
    }
    const auto& original=*mesh.engineBounds;
    const double x=static_cast<double>(center.x)-original.sphereCenter.x;
    const double y=static_cast<double>(center.y)-original.sphereCenter.y;
    const double z=static_cast<double>(center.z)-original.sphereCenter.z;
    const double r=static_cast<double>(radius)-original.sphereRadius;
    return ((x*x+y*y)+z*z)+r*r;
}
std::uint32_t rawBits(float value) {
    std::uint32_t result;
    std::memcpy(&result,&value,sizeof(result));
    return result;
}
std::uint32_t bits(float value) {
    // Engine equality treats positive and negative zero as the same value.
    return value==0.0f?0u:rawBits(value);
}
std::uint32_t word(const std::vector<std::uint8_t>& bytes,std::size_t at) {
    return std::uint32_t(bytes.at(at)) | (std::uint32_t(bytes.at(at+1u))<<8u) |
        (std::uint32_t(bytes.at(at+2u))<<16u) | (std::uint32_t(bytes.at(at+3u))<<24u);
}
float scalar(const std::vector<std::uint8_t>& bytes,std::size_t at) {
    const auto encoded=word(bytes,at);
    float result;
    std::memcpy(&result,&encoded,sizeof(result));
    return result;
}
Vec3 vector(const std::vector<std::uint8_t>& bytes,std::size_t at) {
    return {scalar(bytes,at),scalar(bytes,at+4u),scalar(bytes,at+8u)};
}
std::string textureName(const Mesh& mesh,std::size_t channel) {
    if(!mesh.enginePrelightInput)return channel==0u?mesh.texture0:mesh.texture1;
    // Inspection trims resource names and treats NULL/NONE as absent. Export
    // the stored spelling; engine text tokens cannot encode its whitespace.
    std::string result;
    for(std::size_t i=0;i<32u;++i) {
        const auto value=static_cast<char>(mesh.rawMeshHeader.at(88u+32u*channel+i));
        if(!value)break;
        result+=value;
    }
    return result;
}
Vec2 uv(const Vertex& vertex,std::size_t channel) {
    switch(channel) {
    case 0u:return vertex.texcoord0;
    case 1u:return vertex.texcoord1;
    case 2u:return vertex.texcoord2;
    default:return vertex.texcoord3;
    }
}
void setUv(Vertex& vertex,std::size_t channel,Vec2 value) {
    switch(channel) {
    case 0u:vertex.texcoord0=value;break;
    case 1u:vertex.texcoord1=value;break;
    case 2u:vertex.texcoord2=value;break;
    default:vertex.texcoord3=value;break;
    }
}

std::vector<Vertex> sourceVertices(const Mesh& mesh,const Node& node,Writer& out) {
    auto result=mesh.vertices;
    // ASCII instances share the engine MDX normal pool. A later instance's
    // seam processing changes the source read by every existing instance.
    for(std::size_t i=0;i<result.size();++i) result[i].normal=mesh.normalAt(i);
    if(mesh.enginePrelightInput) {
        const auto& source=*mesh.enginePrelightInput;
        const auto& bytes=*source.mdx;
        for(std::size_t i=0;i<result.size();++i) {
            out.job().check();
            auto& vertex=result[i];
            const auto row=source.mdxDataOffset+i*mesh.mdxRowSize;
            // Use enabled compiled channels directly.
            if(mesh.mdxBitmap&1u) vertex.position=vector(bytes,row+source.mdxVertexOffset);
            else if(mesh.cpuPositions) vertex.position=mesh.cpuPositions->at(i);
            else throw ModelError("Cannot export mesh "+node.name+": no compiled vertex-position pool");
            if(mesh.mdxBitmap&0x20u) vertex.normal=vector(bytes,row+mesh.mdxNormalOffset);
            if(mesh.mdxBitmap&0x40u)
                for(std::size_t channel=0;channel<4u;++channel)
                    vertex.color[channel]=bytes.at(row+mesh.mdxColorOffset+channel);
            for(std::size_t channel=0;channel<4u;++channel)
                if(mesh.mdxBitmap&(2u<<channel)) {
                    const auto offset=row+mesh.mdxTexcoordOffsets[channel];
                    setUv(vertex,channel,{scalar(bytes,offset),scalar(bytes,offset+4u)});
                }
        }
    }




    if(mesh.proceduralUvPool)
        for(std::size_t i=0;i<result.size();++i)
            result[i].texcoord0=mesh.proceduralUvPool->texcoords.at(i);
    return result;
}

using Triangle=std::array<std::uint32_t,3>;
Triangle oriented(Triangle triangle) {
    const Triangle next{{triangle[1],triangle[2],triangle[0]}};
    const Triangle last{{triangle[2],triangle[0],triangle[1]}};
    return std::min(triangle,std::min(next,last));
}
bool degenerate(Triangle triangle) {
    return triangle[0]==triangle[1] || triangle[1]==triangle[2] || triangle[2]==triangle[0];
}
void diagnoseTopology(Writer& out,const Node& node,const Mesh& mesh) {
    // The authored faces supply surface materials, adjacency and query planes.
    // Engine index batches are an independent rendering input. Export faces
    // from the former and report any difference instead of substituting the
    // inspection renderer's triangulation for those authored records.
    std::vector<EngineMeshDraw> draws;
    std::vector<std::uint32_t> indices;
    if(mesh.enginePrelightInput && !mesh.animated && !node.lightsaber) {
        const auto engine=decodeEngineMeshTopology(*mesh.enginePrelightInput,false,out.job());
        draws=engine.draws;indices=engine.indices;
    } else {
        draws=mesh.engineDraws;indices=mesh.engineDrawIndices;
    }
    if(node.lightsaber) return; // Its piece/segment topology has a separate exporter.
    std::vector<Triangle> authored,rendered;
    for(const auto& face:mesh.faces) {
        Triangle triangle{};
        for(std::size_t corner=0;corner<3u;++corner)
            triangle[corner]=static_cast<std::uint32_t>(face.indices[corner]);
        if(!degenerate(triangle)) authored.push_back(oriented(triangle));
    }
    for(const auto& draw:draws) {
        out.job().check();
        const auto at=[&](std::size_t i){return indices.at(draw.firstIndex+i);};
        const auto append=[&](Triangle triangle) {
            if(!degenerate(triangle)) rendered.push_back(oriented(triangle));
        };
        if(draw.primitive==3u) {
            for(std::size_t i=0;i+2u<draw.indexCount;i+=3u) append({{at(i),at(i+1u),at(i+2u)}});
        } else if(draw.primitive==4u) {
            for(std::size_t i=2u;i<draw.indexCount;++i)
                append(i&1u?Triangle{{at(i-1u),at(i-2u),at(i)}}:Triangle{{at(i-2u),at(i-1u),at(i)}});
        } else if(draw.primitive==5u) {
            for(std::size_t i=2u;i<draw.indexCount;++i) append({{at(0u),at(i-1u),at(i)}});
        } else if(draw.indexCount) {
            out.warning("Mesh "+node.name+": engine drawing primitive "+std::to_string(draw.primitive)+
                " cannot be represented by ASCII triangular faces; the MDL face records are exported");
        }
    }
    if(authored==rendered) return;
    auto sortedAuthored=authored,sortedRendered=rendered;
    std::sort(sortedAuthored.begin(),sortedAuthored.end());
    std::sort(sortedRendered.begin(),sortedRendered.end());
    if(sortedAuthored==sortedRendered)
        out.warning("Mesh "+node.name+": engine triangle draw order differs from MDL face order; ASCII retains the MDL triangles");
    else
        out.warning("Mesh "+node.name+": engine draw geometry differs from the MDL face/query stream; ASCII exports the MDL faces and does not replace them with draw triangles");
}

void materialFields(Writer& out,const Model& model,const Node& node,const Mesh& mesh) {
    const auto texture0=textureName(mesh,0u),texture1=textureName(mesh,1u);
    if(!texture0.empty()) out.line("texture0 "+out.token(texture0,"mesh "+node.name+" texture0"));
    if(!texture1.empty()) out.line("texture1 "+out.token(texture1,"mesh "+node.name+" texture1"));
    if(node.flags==(kNodeBase|kNodeMesh) || node.flags==(kNodeBase|kNodeMesh|kNodeAabb)) {
        // Ordinary/AABB postprocessing rewrites these names even with no
        // faces. ASCII has no later field that bypasses that producer.
        if(texture0.empty())
            out.warning("Mesh "+node.name+": engine ASCII load replaces an empty texture0 with a default null texture name");
        if(texture1.size()>16u)
            out.warning("Mesh "+node.name+": engine ASCII load truncates texture1 to 16 bytes");
    }
    // Export the authored IEEE color words from compiled records.
    const auto diffuse=mesh.enginePrelightInput?vector(mesh.rawMeshHeader,60u):mesh.diffuse;
    const auto ambient=mesh.enginePrelightInput?vector(mesh.rawMeshHeader,72u):mesh.ambient;
    out.line("diffuse "+out.vector(diffuse));
    out.line("ambient "+out.vector(ambient));
    const auto flag=[&](const char* name,bool value){out.line(std::string(name)+(value?" 1":" 0"));};
    flag("shadow",mesh.castsShadow);
    flag("render",mesh.render);
    flag("beaming",mesh.beaming);
    flag("rotatetexture",mesh.rotateTexture);
    flag("lightmapped",mesh.lightmapped);
    flag("m_bIsBackgroundGeometry",mesh.backgroundGeometry);
    out.line("transparencyhint "+std::to_string(static_cast<std::int32_t>(mesh.transparencyHint)));
    out.line("animateuv "+std::to_string(static_cast<std::int32_t>(mesh.uvAnimation.enabledRaw)));
    out.line("uvdirectionx "+out.scalar(mesh.uvAnimation.direction.x));
    out.line("uvdirectiony "+out.scalar(mesh.uvAnimation.direction.y));
    out.line("uvjitter "+out.scalar(mesh.uvAnimation.jitter));
    out.line("uvjitterspeed "+out.scalar(mesh.uvAnimation.jitterSpeed));
    if(mesh.engineBounds) {
        out.line("bmin "+out.vector(mesh.engineBounds->box.minimum));
        out.line("bmax "+out.vector(mesh.engineBounds->box.maximum));
    }
    if(model.game==GameVersion::Kotor2 && mesh.k2Material) {
        const auto& material=*mesh.k2Material;
        flag("dirt_enabled",material.dirtEnabledRaw!=0u);
        out.line("dirt_texture "+std::to_string(material.dirtTexture));
        out.line("dirt_worldspace "+std::to_string(material.dirtWorldspace));
        flag("hologram_donotdraw",material.hologramDoNotDrawRaw!=0u);
    }
}

MeshReconstruction reconstructMesh(Writer& out,const Node& node,const Mesh& mesh,
                                   bool refine=true,bool finalNormals=true) {
    auto vertices=sourceVertices(mesh,node,out);
    const auto* animation=node.animmesh?&*node.animmesh:nullptr;
    if(animation && (mesh.mdxBitmap&2u)==0u)
        throw ModelError("Cannot export animmesh "+node.name+": its static UV source pool is unavailable; engine authoring requires tverts even when animtverts is empty");
    if(mesh.skinned && !mesh.engineCpuSkin && !mesh.builtFloatSkinChannels)
        throw ModelError("Cannot export skin mesh "+node.name+": engine float weight/index channels are unavailable");
    const auto skin=mesh.skinned?decodeEngineFloatSkinChannels(mesh):std::vector<EngineFloatSkinChannels>{};
    const bool hasNormals=!mesh.enginePrelightInput || (mesh.mdxBitmap&0x20u)!=0u;
    const bool explicitNormals=!animation && hasNormals && out.explicitNormals(mesh);
    const bool hasColors=mesh.hasVertexColors || (mesh.enginePrelightInput && (mesh.mdxBitmap&0x40u)!=0u);
    std::vector<std::vector<std::uint32_t>> sourceAttributes(vertices.size());
    for(std::size_t index=0;index<vertices.size();++index) {
        out.job().check();
        const auto& vertex=vertices.at(index);
        auto& key=sourceAttributes[index];
        const auto append=[&](Vec3 v){key.push_back(bits(v.x));key.push_back(bits(v.y));key.push_back(bits(v.z));};
        if(animation && hasNormals) append(vertex.normal);
        if(animation) {
            // InternalGenVertices reads frame * rawVertexCount + sourceIndex.
            // Final pools transpose this to vertex * sampleCount + frame.
            // Distinct position timelines must retain separate source verts.
            const auto count=static_cast<std::size_t>(animation->positionSampleCount);
            for(std::size_t frame=0;frame<count;++frame)
                append(animation->positionSamples.at(index*count+frame));
            if(!animation->staticTangents.empty())
                for(const auto value:animation->staticTangents.at(index)) key.push_back(bits(value));
        }
        if(hasColors) {
            std::uint32_t color{};
            for(std::size_t channel=0;channel<4u;++channel)
                color|=std::uint32_t(vertex.color[channel])<<(channel*8u);
            key.push_back(color);
        }
        if(mesh.dangly) {
            // UV splits can retain distinct engine constraint zero signs.
            // Keep their source rows separate; the engine generator still
            // uses numeric equality when every generated attribute agrees.
            key.push_back(rawBits(mesh.dangly->constraints.at(index)));
            if(mesh.dangly->restPositions) append(mesh.dangly->restPositions->at(index));
        }
        if(mesh.skinned) {
            for(const auto value:skin.at(index).weights) key.push_back(bits(value));
            for(const auto value:skin.at(index).indices) key.push_back(bits(value));
        }
    }
    for(const auto& face:mesh.faces)
        for(const auto index:face.indices)
            if(index<0)
                throw ModelError("Cannot export mesh "+node.name+": an engine face has a negative vertex index");
    if(mesh.asciiNormalPositionSource) {
        const auto& retained=*mesh.asciiNormalPositionSource;
        using Key=std::pair<std::uint32_t,std::vector<std::uint32_t>>;
        std::map<Key,std::size_t> sources;
        std::vector<std::array<Key,3>> cornerKeys(mesh.faces.size());
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto row=static_cast<std::size_t>(mesh.faces[face].indices[corner]);
                auto attributes=sourceAttributes[row];
                const auto p=vertices[row].position;
                for(const auto value:{p.x,p.y,p.z}) attributes.push_back(rawBits(value));
                Key key{retained.faces[retained.authoredFaceIndices[face]][corner],std::move(attributes)};
                sources.emplace(key,row);cornerKeys[face][corner]=std::move(key);
            }
        MeshReconstruction result;
        result.vertices=std::move(vertices);result.hasNormals=hasNormals;result.explicitNormals=explicitNormals;
        for(auto& source:sources) {
            const auto row=source.second;
            source.second=result.topology.sourceVertices.size();
            result.topology.sourceVertices.push_back(row);
            const auto p=result.vertices[row].position;
            result.sourcePositions.push_back(p);
            auto high=retained.positions[source.first.first];
            const std::array<float,3> low{{p.x,p.y,p.z}};
            for(std::size_t axis=0;axis<3u;++axis)
                if(rawBits(static_cast<float>(high[axis]))!=rawBits(low[axis])) high[axis]=low[axis];
            result.preciseSourcePositions.push_back(high);
        }
        for(std::size_t face=0;face<mesh.faces.size();++face) {
            std::array<std::uint32_t,3> indices;
            for(std::size_t corner=0;corner<3u;++corner)
                indices[corner]=static_cast<std::uint32_t>(sources.at(cornerKeys[face][corner]));
            result.topology.sourceFaces.push_back(indices);
            result.smoothing.masks.push_back(retained.faceSmoothing[retained.authoredFaceIndices[face]]);
            result.faceOrder.push_back(face);result.cornerRotations.push_back(0u);
        }
        if(mesh.asciiTangentSource) result.preciseFaceTexcoords=mesh.asciiTangentSource->faceTexcoords64;
        return result;
    }
    auto topologies=reconstructTopologies(mesh,vertices,sourceAttributes,out.job());
    std::size_t selected{};
    ReconstructedSmoothing reconstructed;
    double selectedSphereError{};
    for(std::size_t i=0;i<topologies.size();++i) {
        const auto& topology=topologies[i];
        ReconstructedSmoothing candidate;
        if(!animation && hasNormals && !explicitNormals)
            candidate=reconstructSmoothing(mesh,vertices,topology.sourceVertices,topology.sourceFaces,out.job(),nullptr,refine);
        else candidate.masks.assign(mesh.faces.size(),animation?1u:0u);
        const auto candidateSphereError=sphereError(mesh,vertices,topology);
        // Prefer final normals over the source-duplication-dependent sphere.
        // Skin-seam precursors still need both scores preserved: a closer
        // local normal fit need not improve the final seam output.
        if(i==0u || (!explicitNormals && candidate.squaredNormalError<reconstructed.squaredNormalError &&
                    (finalNormals || candidateSphereError<=selectedSphereError)) ||
           ((explicitNormals || candidate.squaredNormalError==reconstructed.squaredNormalError) &&
            candidateSphereError<selectedSphereError)) {
            selected=i;
            selectedSphereError=candidateSphereError;
            reconstructed=std::move(candidate);
        }
    }
    auto& topology=topologies[selected];
    std::vector<std::size_t> faceOrder(mesh.faces.size());
    std::iota(faceOrder.begin(),faceOrder.end(),std::size_t{});
    std::vector<Vec3> positions;
    for(const auto vertex:topology.sourceVertices) positions.push_back(vertices[vertex].position);
    return {std::move(vertices),{std::move(topology.sourceVertices),std::move(topology.sourceFaces)},
        std::move(reconstructed),topology.changedAdjacency,hasNormals,explicitNormals,
        std::vector<std::uint8_t>(mesh.faces.size(),0u),std::move(faceOrder),std::move(positions),
        mesh.asciiTangentSource?mesh.asciiTangentSource->faceTexcoords64:
            std::vector<std::array<std::array<double,2>,3>>{}, {}};
}

using CornerRows=std::vector<std::vector<std::array<std::int16_t,3>>>;

struct PreciseUvPool {
    std::vector<std::array<double,2>> values;
    std::vector<std::array<std::uint32_t,3>> indices;
};

PreciseUvPool preciseUvPool(const Mesh& mesh,const MeshReconstruction& plan) {
    PreciseUvPool result;
    if(plan.preciseFaceTexcoords.empty()) return result;
    for(const auto& vertex:plan.vertices)
        result.values.push_back({vertex.texcoord0.x,vertex.texcoord0.y});
    const auto words=[](const std::array<double,2>& value) {
        std::array<std::uint64_t,2> result;
        std::memcpy(result.data(),value.data(),sizeof(result));
        return result;
    };
    std::map<std::array<std::uint64_t,2>,std::uint32_t> additions;
    for(std::size_t face=0;face<mesh.faces.size();++face) {
        std::array<std::uint32_t,3> indices;
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto original=static_cast<std::uint32_t>(mesh.faces[face].indices[corner]);
            const auto& value=plan.preciseFaceTexcoords[face][corner];
            if(words(value)==words(result.values[original])) indices[corner]=original;
            else {
                const auto inserted=additions.emplace(words(value),static_cast<std::uint32_t>(result.values.size()));
                if(inserted.second) result.values.push_back(value);
                indices[corner]=inserted.first->second;
            }
        }
        result.indices.push_back(indices);
    }
    return result;
}

CornerRows generateGeometry(Model& rebuilt,const Model& source,
                           const std::map<const Mesh*,MeshReconstruction>& plans,
                           bool smooth,const neoshared::bif::JobControl& job) {
    rebuilt.partSourceModel.reset();
    rebuilt.asciiBuilt=true;
    CornerRows rows(source.meshes.size());
    for(std::size_t index=0;index<source.meshes.size();++index) {
        const auto& mesh=source.meshes[index];
        const auto found=plans.find(&mesh);
        if(found==plans.end()) continue; // Saber geometry has its own producer.
        const auto& plan=found->second;
        const auto precise=preciseUvPool(mesh,plan);
        ascii::RawNode raw;
        rebuilt.meshes[index].explicitNormals=plan.explicitNormals;
        raw.node=source.nodes[mesh.nodeIndex];
        if(!plan.preciseSourcePositions.empty()) {
            raw.normalPositionPrecision=64u;
            raw.normalPositions64=plan.preciseSourcePositions;
        }
        const auto* animation=raw.node.animmesh?&*raw.node.animmesh:nullptr;
        const bool colors=mesh.hasVertexColors || (mesh.enginePrelightInput && (mesh.mdxBitmap&0x40u));
        const auto skin=mesh.skinned?decodeEngineFloatSkinChannels(mesh):std::vector<EngineFloatSkinChannels>{};
        if(mesh.skinned) raw.skinData.emplace();
        if(mesh.cpuPositions) raw.cpuPositions=*mesh.cpuPositions;
        if(mesh.mdxBitmap&0x80u)
            for(const auto& vertex:plan.vertices) raw.tangentBases.push_back(vertex.tangentBasis);
        for(std::size_t source=0;source<plan.topology.sourceVertices.size();++source) {
            const auto vertex=plan.topology.sourceVertices[source];
            const auto& value=plan.vertices[vertex];
            raw.vertices.push_back(plan.sourcePositions[source]);
            if(animation && plan.hasNormals) raw.normals.push_back(value.normal);
            if(colors) raw.packedColors.push_back(value.color);
            if(mesh.skinned) raw.skinChannels.push_back(skin.at(vertex));
            if(mesh.dangly) raw.constraints.push_back(mesh.dangly->constraints.at(vertex));
            if(animation && !animation->staticTangents.empty())
                raw.animTangents.push_back(animation->staticTangents.at(vertex));
        }
        if(plan.explicitNormals)
            for(const auto& vertex:plan.vertices) raw.normals.push_back(vertex.normal);
        for(std::size_t channel=0;channel<4u;++channel)
            if(mesh.mdxBitmap&(2u<<channel))
                for(const auto& vertex:plan.vertices) {
                    const auto value=uv(vertex,channel);
                    raw.texcoords[channel].push_back({value.x,value.y,0.0f});
                }
        if(!precise.values.empty()) {
            raw.tangentUvPrecision=64u;
            raw.tangentTexcoords64=precise.values;
            raw.texcoords[0].clear();
            for(const auto& value:precise.values)
                raw.texcoords[0].push_back({static_cast<float>(value[0]),static_cast<float>(value[1]),0.0f});
        }
        for(const auto face:plan.faceOrder) {
            ascii::RawFace value;
            for(std::size_t corner=0;corner<3u;++corner)
                value.vertices[corner]=plan.topology.sourceFaces[face][plan.sourceCorner(face,corner)];
            value.smoothing=smooth && !animation && plan.hasNormals && !mesh.asciiNormalPositionSource?1u:plan.smoothing.masks[face];
            value.material=mesh.faces[face].surfaceMaterial;
            for(auto& channel:value.texcoords)
                for(std::size_t corner=0;corner<3u;++corner)
                    channel[corner]=static_cast<std::uint32_t>(
                        mesh.faces[face].indices[plan.sourceCorner(face,corner)]);
            if(plan.explicitNormals) raw.normalIndices.push_back(value.texcoords[0]);
            if(mesh.cpuPositions) raw.cpuIndices.push_back(value.texcoords[0]);
            if(mesh.mdxBitmap&0x80u) raw.tangentIndices.push_back(value.texcoords[0]);
            if(!precise.values.empty())
                for(std::size_t corner=0;corner<3u;++corner)
                    value.texcoords[0][corner]=precise.indices[face][plan.sourceCorner(face,corner)];
            raw.faces.push_back(value);
        }
        if(animation) {
            raw.samplePeriod=animation->samplePeriod;
            for(std::int32_t frame=0;frame<animation->positionSampleCount;++frame)
                for(const auto vertex:plan.topology.sourceVertices)
                    raw.animatedVertices.push_back(animation->positionSamples.at(
                        vertex*animation->positionSampleCount+frame));
            for(std::int32_t frame=0;frame<animation->texcoordSampleCount;++frame)
                for(std::size_t vertex=0;vertex<plan.vertices.size();++vertex) {
                    const auto value=animation->texcoordSamples.at(vertex*animation->texcoordSampleCount+frame);
                    raw.animatedTexcoords.push_back({value.x,value.y,0.0f});
                }
        }
        const auto generated=ascii::generateAsciiMeshGeometry(rebuilt,raw,rebuilt.meshes[index],job);
        rows[index].resize(generated.size());
        for(std::size_t output=0;output<generated.size();++output) {
            const auto face=plan.faceOrder[output];
            for(std::size_t corner=0;corner<3u;++corner)
                rows[index][face][plan.sourceCorner(face,corner)]=generated[output][corner];
        }
    }
    initializeAsciiSkinSeamPools(rebuilt);
    return rows;
}

bool preservesSkinChannels(const Model& model,const CornerRows& rows) {
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        if(!mesh.skinned) continue;
        const auto channels=decodeEngineFloatSkinChannels(mesh);
        std::vector<std::size_t> first(mesh.faces.size()*3u,channels.size());
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto source=static_cast<std::size_t>(mesh.faces[face].indices[corner]);
                auto& retained=first[static_cast<std::uint16_t>(rows[index][face][corner])];
                if(retained==channels.size()) retained=source;
                const auto& previous=channels[retained];
                const auto& current=channels[source];
                if(previous.weights!=current.weights || previous.indices!=current.indices) return false;
            }
    }
    return true;
}

double measureSeamNormals(const Model& rebuilt,const Model& source,const CornerRows& rows,
                         std::map<const Mesh*,MeshReconstruction>& plans,
                         const Mesh* changed=nullptr,bool record=true) {
    double error{};
    for(std::size_t index=0;index<source.meshes.size();++index) {
        const auto& mesh=source.meshes[index];
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !found->second.hasNormals) continue;
        auto& plan=found->second;
        auto& result=plan.smoothing;
        if(changed && &mesh!=changed && !(changed->render && mesh.render)) {
            error+=result.squaredNormalError;
            continue;
        }
        double squared{};
        std::size_t changedCorners{};
        float maxDifference{};
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto target=plan.vertices[mesh.faces[face].indices[corner]].normal;
                const auto actual=rebuilt.meshes[index].normalAt(rows[index][face][corner]);
                const double x=double(actual.x)-target.x,y=double(actual.y)-target.y,z=double(actual.z)-target.z;
                squared+=(x*x+y*y)+z*z;
                if(record) {
                    const float delta=std::max({std::abs(actual.x-target.x),std::abs(actual.y-target.y),
                        std::abs(actual.z-target.z)});
                    changedCorners+=delta!=0.0f;
                    maxDifference=std::max(maxDifference,delta);
                }
            }
        if(record) {
            result.squaredNormalError=squared;
            result.changedCornerNormals=changedCorners;
            result.maxNormalComponentDifference=maxDifference;
        }
        error+=squared;
    }
    return error;
}

double measureTangentBasis(Model& rebuilt,const Model& source,const CornerRows& rows,
                          const std::map<const Mesh*,MeshReconstruction>& plans,
                          const neoshared::bif::JobControl& job,
                          const std::map<const Mesh*,MeshReconstruction>* changed=nullptr) {
    double error{};
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        return (x*x+y*y)+z*z;
    };
    for(std::size_t index=0;index<source.meshes.size();++index) {
        const auto& mesh=source.meshes[index];
        const auto found=plans.find(&mesh);
        if(!(mesh.mdxBitmap&0x80u) || mesh.animated || mesh.dangly || found==plans.end()) continue;
        if(!changed || changed->find(&mesh)!=changed->end())
            rebuildAsciiTangentBasis(rebuilt.meshes[index],job);
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto& target=found->second.vertices[mesh.faces[face].indices[corner]].tangentBasis;
                const auto& actual=rebuilt.meshes[index].vertices[rows[index][face][corner]].tangentBasis;
                error+=difference(actual.bitangent,target.bitangent)+difference(actual.tangent,target.tangent)+
                    difference(actual.normal,target.normal);
            }
    }
    return error;
}

enum class CornerSearch { Cancellation,TangentAxes,TangentOrder };

void reconstructCyclicCornerPass(Writer& out,const Model& model,const Model& frames,
                                std::map<const Mesh*,MeshReconstruction>& plans,
                                const Mesh* changedMesh,CornerSearch search,bool neighbors) {
    const bool axisTangents=search!=CornerSearch::Cancellation;
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.job());
    auto best=measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    best+=measureSeamNormals(current,model,rows,plans);
    std::vector<std::pair<const Mesh*,std::size_t>> candidates;
    std::vector<std::array<std::size_t,2>> pairs;
    std::map<const Mesh*,std::vector<std::array<std::size_t,2>>> compensatingPairs;
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        if(changedMesh && (changedMesh->render?!mesh.render:&mesh!=changedMesh)) continue;
        const auto found=plans.find(&mesh);
        if(found==plans.end() || mesh.animated || !found->second.hasNormals ||
           found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
        const auto& plan=found->second;
        std::vector<bool> cancelled(plan.topology.sourceVertices.size(),false);
        std::vector<bool> singular(mesh.faces.size(),false);
        std::vector<Vec3> cancelledTangents;
        const auto cancellationOutput=[](Vec3 normal) {
            return same(normal,{}) || same(normal,{1.0f,0.0f,0.0f});
        };
        const auto axisAligned=[](Vec3 value) {
            return (value.x==0.0f && value.y==0.0f) ||
                (value.x==0.0f && value.z==0.0f) || (value.y==0.0f && value.z==0.0f);
        };
        const bool tangents=(mesh.mdxBitmap&0x80u) && !mesh.dangly;
        for(std::size_t face=0;face<mesh.faces.size();++face) {
            bool changedBasis=false;
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto actual=current.meshes[index].normalAt(rows[index][face][corner]);
                const auto target=plan.vertices[mesh.faces[face].indices[corner]].normal;
                const auto source=plan.topology.sourceFaces[face][corner];
                if(!axisTangents && !same(actual,target) &&
                   (cancellationOutput(actual) || cancellationOutput(target)))
                    cancelled[source]=true;
                if(tangents) {
                    const auto& generated=current.meshes[index].vertices[rows[index][face][corner]];
                    const auto& retained=plan.vertices[mesh.faces[face].indices[corner]].tangentBasis;
                    const auto inspect=[&](Vec3 value,Vec3 wanted) {
                        if(same(value,wanted)) return;
                        changedBasis=true;
                        const bool original=cancellationOutput(value) || cancellationOutput(wanted);
                        if(search==CornerSearch::TangentOrder ? axisAligned(wanted) :
                           axisTangents ? !original && axisAligned(value) && axisAligned(wanted) : original)
                            cancelledTangents.push_back(generated.position);
                    };
                    inspect(generated.tangentBasis.bitangent,retained.bitangent);
                    inspect(generated.tangentBasis.tangent,retained.tangent);
                    inspect(generated.tangentBasis.normal,retained.normal);
                }
            }
            if(changedBasis && !axisTangents) {
                const auto texcoord=[&](std::size_t corner) {
                    return uv(plan.vertices[mesh.faces[face].indices[plan.sourceCorner(face,corner)]],0u);
                };
                const auto a=texcoord(0u),b=texcoord(1u),c=texcoord(2u);
                singular[face]=(b.x-a.x)*(c.y-a.y)-(c.x-a.x)*(b.y-a.y)==0.0f;
            }
        }
        // Tangent contributions join equal positions even when the authoring
        // topology uses distinct source vertices at that point.
        for(std::size_t source=0;source<cancelled.size();++source) {
            const auto point=plan.vertices[plan.topology.sourceVertices[source]].position;
            if(std::any_of(cancelledTangents.begin(),cancelledTangents.end(),
                [&](Vec3 other){return same(point,other);})) cancelled[source]=true;
        }
        const auto firstCandidate=candidates.size();
        std::vector<std::vector<std::size_t>> incident(cancelled.size()),allIncident(cancelled.size());
        for(std::size_t face=0;face<mesh.faces.size();++face) {
            const auto& vertices=plan.topology.sourceFaces[face];
            if(neighbors) for(const auto vertex:vertices) allIncident[vertex].push_back(face);
            if(singular[face] || std::any_of(vertices.begin(),vertices.end(),
                [&](auto vertex){return cancelled[vertex];})) {
                for(const auto vertex:vertices)
                    if(singular[face] || cancelled[vertex]) incident[vertex].push_back(candidates.size());
                candidates.emplace_back(&mesh,face);
            }
        }
        const auto firstPair=pairs.size();
        for(const auto& faces:incident)
            for(std::size_t first=0;first<faces.size();++first)
                for(std::size_t second=first+1u;second<faces.size();++second)
                    if(faces[first]!=faces[second]) pairs.push_back({faces[first],faces[second]});
        if(!neighbors || firstCandidate==candidates.size()) continue;
        // A changed cyclic start also affects the face's other source
        // normals. An incident face can compensate for those changes even
        // when none of its own corners has a cancelled output.
        auto& neighborPairs=compensatingPairs[&mesh];
        for(std::size_t index=firstCandidate;index<candidates.size();++index) {
            const auto face=candidates[index].second;
            for(const auto vertex:plan.topology.sourceFaces[face])
                for(const auto other:allIncident[vertex])
                    if(face!=other) neighborPairs.push_back({std::min(face,other),std::max(face,other)});
        }
        std::sort(neighborPairs.begin(),neighborPairs.end());
        neighborPairs.erase(std::unique(neighborPairs.begin(),neighborPairs.end()),neighborPairs.end());
        std::vector<std::array<std::size_t,2>> existing;
        for(std::size_t index=firstPair;index<pairs.size();++index)
            existing.push_back({candidates[pairs[index][0]].second,candidates[pairs[index][1]].second});
        std::sort(existing.begin(),existing.end());
        neighborPairs.erase(std::remove_if(neighborPairs.begin(),neighborPairs.end(),[&](const auto& pair) {
            return std::binary_search(existing.begin(),existing.end(),pair);
        }),neighborPairs.end());
    }
    std::sort(pairs.begin(),pairs.end());
    pairs.erase(std::unique(pairs.begin(),pairs.end()),pairs.end());
    if(candidates.empty()) return;
    // Cyclic corners retain triangle winding and connectivity but change
    // float cross-product rounding. Score cancelled vectors and singular UV
    // faces using the unchanged normal/basis producers.
    // Rendered meshes interact through seams. Each excluded mesh is an
    // independent component, so its cyclic search need not revisit the others.
    std::map<const Mesh*,std::vector<std::size_t>> groups;
    for(std::size_t i=0;i<candidates.size();++i) {
        const auto* mesh=candidates[i].first;
        groups[mesh->render?nullptr:mesh].push_back(i);
    }
    for(const auto& group:groups) {
        bool improved;
        do {
            improved=false;
            auto selected=plans;
            auto selectedGeometry=current;
            auto selectedRows=rows;
            const auto consider=[&](const Mesh* mesh,MeshReconstruction trial) {
                const auto& retained=plans.at(mesh);
                if(trial.faceOrder!=retained.faceOrder &&
                   !preservesSourceFaceNeighbors(*mesh,trial.topology.sourceFaces,
                       retained.faceOrder,trial.faceOrder)) return;
                std::map<const Mesh*,MeshReconstruction> changed;
                changed.emplace(mesh,std::move(trial));
                auto rebuilt=current;
                auto generated=rows;
                const auto index=static_cast<std::size_t>(mesh-model.meshes.data());
                rebuilt.meshes[index]=frames.meshes[index];
                auto changedRows=generateGeometry(rebuilt,model,changed,false,out.job());
                generated[index]=std::move(changedRows[index]);
                if(!preservesSkinChannels(model,generated)) return;
                auto error=measureTangentBasis(rebuilt,model,generated,plans,out.job(),&changed);
                if(mesh->render)
                    processAsciiSkinSeams(rebuilt,out.job());
                else {
                    // CreateSkinNodeList excludes non-rendered meshes. Changing
                    // only those leaves every ordered seam input unchanged.
                    for(std::size_t index=0;index<model.meshes.size();++index)
                        if(model.meshes[index].render)
                            rebuilt.meshes[index].asciiNormalPool=current.meshes[index].asciiNormalPool;
                }
                error+=measureSeamNormals(rebuilt,model,generated,plans,mesh,false);
                if(error<best) {
                    best=error;selected=plans;selected.at(mesh)=std::move(changed.at(mesh));improved=true;
                    selectedGeometry=std::move(rebuilt);selectedRows=std::move(generated);
                }
            };
            // Mirrored tangent sums can retain only rounding residue. Move
            // their contributors as well as their cyclic starts; swapping
            // alone cannot recover every accumulation order.
            if(search==CornerSearch::TangentOrder) for(const auto& pair:pairs) {
                const auto& first=candidates[pair[0]];
                const auto& second=candidates[pair[1]];
                const auto* mesh=first.first;
                if((mesh->render?nullptr:mesh)!=group.first || model.nodes[mesh->nodeIndex].aabb) continue;
                const auto& order=plans.at(mesh).faceOrder;
                const auto a=std::find(order.begin(),order.end(),first.second)-order.begin();
                const auto b=std::find(order.begin(),order.end(),second.second)-order.begin();
                auto trial=plans.at(mesh);
                std::swap(trial.faceOrder[a],trial.faceOrder[b]);
                consider(mesh,std::move(trial));
                for(const auto& move:{std::make_pair(a,b),std::make_pair(b,a)}) {
                    trial=plans.at(mesh);
                    trial.faceOrder.erase(trial.faceOrder.begin()+move.first);
                    trial.faceOrder.insert(trial.faceOrder.begin()+move.second,order[move.first]);
                    consider(mesh,std::move(trial));
                }
            }
            if(search!=CornerSearch::TangentOrder) for(const auto index:group.second) {
                const auto& candidate=candidates[index];
                for(std::uint8_t rotation=0u;rotation<3u;++rotation) {
                    if(plans.at(candidate.first).cornerRotations[candidate.second]==rotation) continue;
                    auto trial=plans.at(candidate.first);
                    trial.cornerRotations[candidate.second]=rotation;
                    consider(candidate.first,std::move(trial));
                }
            }
            // Opposing contributions can require two cyclic starts to move
            // together. Only pair affected faces incident to the same source.
            if(!improved && search!=CornerSearch::TangentOrder) for(const auto& pair:pairs) {
                const auto& first=candidates[pair[0]];
                const auto& second=candidates[pair[1]];
                if((first.first->render?nullptr:first.first)!=group.first) continue;
                for(std::uint8_t a=0u;a<3u;++a)
                    for(std::uint8_t b=0u;b<3u;++b) {
                        if(plans.at(first.first).cornerRotations[first.second]==a ||
                           plans.at(second.first).cornerRotations[second.second]==b) continue;
                        auto trial=plans.at(first.first);
                        trial.cornerRotations[first.second]=a;
                        trial.cornerRotations[second.second]=b;
                        consider(first.first,std::move(trial));
                    }
            }
            // Keep the established single/source-pair search first. Only
            // at its fixed point try compensating neighbors, then resume it
            // after accepting an improvement.
            if(!improved && search!=CornerSearch::TangentOrder) for(const auto& entry:compensatingPairs) {
                const auto* mesh=entry.first;
                if((mesh->render?nullptr:mesh)!=group.first) continue;
                for(const auto& pair:entry.second)
                    for(std::uint8_t a=0u;a<3u;++a)
                        for(std::uint8_t b=0u;b<3u;++b) {
                            if(plans.at(mesh).cornerRotations[pair[0]]==a ||
                               plans.at(mesh).cornerRotations[pair[1]]==b) continue;
                            auto trial=plans.at(mesh);
                            trial.cornerRotations[pair[0]]=a;
                            trial.cornerRotations[pair[1]]=b;
                            consider(mesh,std::move(trial));
                        }
            }
            if(improved) {
                plans=std::move(selected);current=std::move(selectedGeometry);rows=std::move(selectedRows);
                measureSeamNormals(current,model,rows,plans);
            }
        } while(improved);
    }
}

void reconstructCyclicCorners(Writer& out,const Model& model,const Model& frames,
                              std::map<const Mesh*,MeshReconstruction>& plans,
                              const Mesh* changedMesh=nullptr) {
    reconstructCyclicCornerPass(out,model,frames,plans,changedMesh,CornerSearch::Cancellation,false);
}

bool improvesReconstructedShading(const Model& model,
        const std::map<const Mesh*,MeshReconstruction>& plans,
        const Model& current,const CornerRows& rows,
        const Model& rebuilt,const CornerRows& generated,bool retainAngles=false) {
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        return (x*x+y*y)+z*z;
    };
    const auto angle=[](Vec3 a,Vec3 b) {
        if(same(a,b)) return 0.0;
        if(same(a,Vec3{}) || same(b,Vec3{})) return std::acos(-1.0);
        const double x=double(a.y)*b.z-double(a.z)*b.y;
        const double y=double(a.z)*b.x-double(a.x)*b.z;
        const double z=double(a.x)*b.y-double(a.y)*b.x;
        return std::atan2(std::sqrt((x*x+y*y)+z*z),
            (double(a.x)*b.x+double(a.y)*b.y)+double(a.z)*b.z);
    };
    bool retained=true,improved=false;
    const auto compare=[&](Vec3 before,Vec3 after,Vec3 target) {
        const auto oldError=difference(before,target),newError=difference(after,target);
        retained=retained && newError<=oldError;
        if(retainAngles) retained=retained && angle(after,target)<=angle(before,target);
        improved=improved || newError<oldError;
    };
    for(std::size_t index=0;index<model.meshes.size() && retained;++index) {
        const auto& source=model.meshes[index];
        const auto existing=plans.find(&source);
        if(existing==plans.end()) continue;
        const auto& plan=existing->second;
        for(std::size_t face=0;face<source.faces.size() && retained;++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto beforeRow=rows[index][face][corner],afterRow=generated[index][face][corner];
                const auto& target=plan.vertices[source.faces[face].indices[corner]];
                if(plan.hasNormals)
                    compare(current.meshes[index].normalAt(beforeRow),
                        rebuilt.meshes[index].normalAt(afterRow),target.normal);
                if((source.mdxBitmap&0x80u) && !source.animated && !source.dangly) {
                    const auto& before=current.meshes[index].vertices[beforeRow].tangentBasis;
                    const auto& after=rebuilt.meshes[index].vertices[afterRow].tangentBasis;
                    compare(before.bitangent,after.bitangent,target.tangentBasis.bitangent);
                    compare(before.tangent,after.tangent,target.tangentBasis.tangent);
                    compare(before.normal,after.normal,target.tangentBasis.normal);
                }
            }
    }
    return retained && improved;
}

void reconstructFaceOrder(Writer& out,const Model& model,const Model& frames,
                          std::map<const Mesh*,MeshReconstruction>& plans) {
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    for(const auto& mesh:model.meshes) {
        const auto found=plans.find(&mesh);
        if(found==plans.end() || mesh.animated || !found->second.hasNormals ||
           found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
        // AABB leaves address the retained face indices directly.
        if(model.nodes[mesh.nodeIndex].aabb) continue;
        const auto& plan=found->second;
        auto order=reconstructNormalFaceOrder(mesh,plan.vertices,plan.topology.sourceVertices,
            plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,plan.sourcePositions,out.job());
        auto stars=reconstructNormalCornerStars(mesh,plan.vertices,plan.topology.sourceVertices,
            plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,order,plan.sourcePositions,out.job());
        if(stars.faceOrder==plan.faceOrder && stars.rotations==plan.cornerRotations) continue;
        auto trial=plans;
        trial.at(&mesh).faceOrder=std::move(stars.faceOrder);
        trial.at(&mesh).cornerRotations=std::move(stars.rotations);
        auto rebuilt=frames;
        const auto generated=generateGeometry(rebuilt,model,trial,false,out.job());
        if(!preservesSkinChannels(model,generated)) continue;
        measureTangentBasis(rebuilt,model,generated,trial,out.job());
        processAsciiSkinSeams(rebuilt,out.job());
        if(improvesReconstructedShading(model,plans,current,rows,rebuilt,generated)) {
            plans=std::move(trial);current=std::move(rebuilt);rows=generated;
        }
    }
    measureSeamNormals(current,model,rows,plans);
}

void reconstructTangentRounding(Writer& out,const Model& model,const Model& frames,
                               std::map<const Mesh*,MeshReconstruction>& plans) {
    if(!out.refineTangentRounding()) return;
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    const auto opposite=[](Vec3 value,Vec3 target) {
        return !same(value,target) && (value.x*target.x+value.y*target.y)+value.z*target.z<=0.0f;
    };
    struct Score {
        double total{};
        std::size_t directions{};
        std::vector<double> maxima;
    };
    const auto score=[&](const Model& rebuilt,const CornerRows& generated) {
        Score result;result.maxima.resize(model.meshes.size());
        for(std::size_t index=0;index<model.meshes.size();++index) {
            const auto& mesh=model.meshes[index];
            const auto found=plans.find(&mesh);
            if(found==plans.end()) continue;
            const auto difference=[](Vec3 actual,Vec3 target) {
                const double x=double(actual.x)-target.x,y=double(actual.y)-target.y,z=double(actual.z)-target.z;
                return (x*x+y*y)+z*z;
            };
            for(std::size_t face=0;face<mesh.faces.size();++face)
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto& target=found->second.vertices[mesh.faces[face].indices[corner]];
                    const auto row=generated[index][face][corner];
                    double error{};
                    if(found->second.hasNormals) error+=difference(rebuilt.meshes[index].normalAt(row),target.normal);
                    if((mesh.mdxBitmap&0x80u) && !mesh.animated && !mesh.dangly) {
                        const auto& actual=rebuilt.meshes[index].vertices[row].tangentBasis;
                        error+=difference(actual.bitangent,target.tangentBasis.bitangent)+
                            difference(actual.tangent,target.tangentBasis.tangent)+
                            difference(actual.normal,target.tangentBasis.normal);
                        if(mesh.render) result.directions+=opposite(actual.bitangent,target.tangentBasis.bitangent) ||
                            opposite(actual.tangent,target.tangentBasis.tangent) ||
                            opposite(actual.normal,target.tangentBasis.normal);
                    }
                    result.total+=error;
                    result.maxima[index]=std::max(result.maxima[index],error);
                }
        }
        return result;
    };
    auto best=score(current,rows);
    const auto limits=best.maxima;
    const auto component=[](Vec3& p,std::size_t axis)->float& {
        return axis==0u?p.x:axis==1u?p.y:p.z;
    };
    for(const auto& mesh:model.meshes) {
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !mesh.render || !(mesh.mdxBitmap&0x80u) || mesh.animated || mesh.dangly ||
           found->second.explicitNormals || mesh.asciiNormalPositionSource || model.nodes[mesh.nodeIndex].aabb) continue;
        const auto index=static_cast<std::size_t>(&mesh-model.meshes.data());
        const auto original=found->second.sourcePositions;
        const auto originalRotations=found->second.cornerRotations;
        std::map<std::array<float,3>,std::vector<std::size_t>> positions;
        for(std::size_t source=0;source<original.size();++source) {
            const auto p=original[source];
            positions[{{p.x,p.y,p.z}}].push_back(source);
        }
        std::vector<std::vector<std::size_t>> incident(original.size());
        for(std::size_t face=0;face<mesh.faces.size();++face) {
            const auto& topology=found->second.topology.sourceFaces[face];
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto source=topology[corner];
                auto& faces=incident[source];
                if(faces.empty() || faces.back()!=face) faces.push_back(face);
            }
        }
        for(;;) {
            const auto& plan=plans.at(&mesh);
            std::vector<std::size_t> writers(current.meshes[index].vertices.size());
            for(const auto face:plan.faceOrder)
                for(const auto row:rows[index][face]) writers[row]=face;
            std::vector<bool> selected(original.size()),contributing(mesh.faces.size());
            for(std::size_t face=0;face<mesh.faces.size();++face)
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto row=rows[index][face][corner];
                    const auto& actual=current.meshes[index].vertices[row];
                    const auto& target=plan.vertices[mesh.faces[face].indices[corner]].tangentBasis;
                    if(!opposite(actual.tangentBasis.bitangent,target.bitangent) &&
                       !opposite(actual.tangentBasis.tangent,target.tangent) &&
                       !opposite(actual.tangentBasis.normal,target.normal)) continue;
                    const auto writer=writers[row];
                    // Replay the tangent writer's exact position/mask gather;
                    // unrelated incident faces cannot repair this direction.
                    for(std::size_t other=0;other<mesh.faces.size();++other) {
                        if(other!=writer && !(plan.smoothing.masks[writer]&plan.smoothing.masks[other])) continue;
                        const auto& indices=plan.topology.sourceFaces[other];
                        if(other==writer || std::any_of(indices.begin(),indices.end(),[&](std::uint32_t source) {
                                return same(plan.sourcePositions[source],actual.position);
                            })) {
                            contributing[other]=true;
                            for(const auto source:indices) selected[source]=true;
                        }
                    }
                }
            std::optional<MeshReconstruction> chosen;
            Model chosenModel;
            CornerRows chosenRows;
            auto chosenScore=best;
            std::tuple<std::size_t,std::size_t,std::size_t,double,double> chosenRank;
            for(const auto& group:positions) {
                if(std::none_of(group.second.begin(),group.second.end(),
                    [&](std::size_t source){return selected[source];})) continue;
                std::vector<std::size_t> faces;
                for(const auto source:group.second)
                    for(const auto face:incident[source]) if(contributing[face]) faces.push_back(face);
                std::sort(faces.begin(),faces.end());
                faces.erase(std::unique(faces.begin(),faces.end()),faces.end());
                for(std::size_t axis=0;axis<3u;++axis) {
                    auto initial=original[group.second.front()];
                    const float value=component(initial,axis);
                    // A fixed neighborhood of the retained value cannot
                    // drift through repeated nextafter steps. Coincident
                    // source positions move together, retaining degeneracy.
                    const std::array<float,3> values{{value,
                        std::nextafter(value,-std::numeric_limits<float>::infinity()),
                        std::nextafter(value,std::numeric_limits<float>::infinity())}};
                    for(const auto replacement:values)
                        for(std::size_t choice=0;choice<=faces.size()*2u;++choice) {
                            out.job().check();
                            auto candidate=plans.at(&mesh);
                            bool changed=false;
                            for(const auto source:group.second) {
                                auto& coordinate=component(candidate.sourcePositions[source],axis);
                                changed=changed || coordinate!=replacement;
                                coordinate=replacement;
                            }
                            if(choice) {
                                if(!changed && axis!=0u) continue;
                                auto& rotation=candidate.cornerRotations[faces[(choice-1u)/2u]];
                                rotation=static_cast<std::uint8_t>((rotation+1u+(choice-1u)%2u)%3u);
                                changed=true;
                            }
                            if(!changed) continue;
                            std::map<const Mesh*,MeshReconstruction> changedPlans{{&mesh,candidate}};
                            auto rebuilt=current;
                            rebuilt.meshes[index]=frames.meshes[index];
                            auto generated=rows;
                            const auto changedRows=generateGeometry(rebuilt,model,changedPlans,false,out.job());
                            generated[index]=changedRows[index];
                            if(!preservesSkinChannels(model,generated)) continue;
                            measureTangentBasis(rebuilt,model,generated,plans,out.job(),&changedPlans);
                            processAsciiSkinSeams(rebuilt,out.job());
                            const auto proposed=score(rebuilt,generated);
                            if(!(proposed.total<best.total) || proposed.directions>=best.directions) continue;
                            bool retained=true;
                            for(std::size_t m=0;m<limits.size();++m)
                                retained=retained && proposed.maxima[m]<=limits[m];
                            if(!retained) continue;
                            std::size_t coordinates{},rotations{};double displacement{};
                            for(std::size_t source=0;source<original.size();++source) {
                                auto before=original[source],after=candidate.sourcePositions[source];
                                for(std::size_t c=0;c<3u;++c) {
                                    const double delta=double(component(after,c))-component(before,c);
                                    coordinates+=delta!=0.0;displacement+=delta*delta;
                                }
                            }
                            for(std::size_t f=0;f<mesh.faces.size();++f)
                                rotations+=candidate.cornerRotations[f]!=originalRotations[f];
                            const auto rank=std::make_tuple(proposed.directions,coordinates,rotations,
                                displacement,proposed.total);
                            if(chosen && !(rank<chosenRank)) continue;
                            chosenRank=rank;chosenScore=proposed;chosen=std::move(candidate);
                            chosenModel=std::move(rebuilt);chosenRows=std::move(generated);
                        }
                }
            }
            if(!chosen) break;
            plans.at(&mesh)=std::move(*chosen);current=std::move(chosenModel);
            rows=std::move(chosenRows);best=std::move(chosenScore);
        }
    }
    measureSeamNormals(current,model,rows,plans);
}

void reconstructNormalRoundingPass(Writer& out,const Model& model,const Model& frames,
                                   std::map<const Mesh*,MeshReconstruction>& plans) {
    if(!out.refineNormalRounding()) return;
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    for(const auto& mesh:model.meshes) {
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !mesh.render || mesh.animated || mesh.dangly ||
           !found->second.hasNormals || found->second.explicitNormals || mesh.asciiNormalPositionSource ||
           model.nodes[mesh.nodeIndex].aabb) continue;
        const auto& plan=found->second;
        auto refined=reconstructNormalRounding(mesh,plan.vertices,plan.topology.sourceVertices,
            plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,
            plan.sourcePositions,out.job());
        if(refined.rotations==plan.cornerRotations && refined.faceOrder==plan.faceOrder &&
           std::equal(refined.positions.begin(),refined.positions.end(),plan.sourcePositions.begin(),same)) continue;
        auto trial=plans;
        trial.at(&mesh).sourcePositions=std::move(refined.positions);
        trial.at(&mesh).cornerRotations=std::move(refined.rotations);
        trial.at(&mesh).faceOrder=std::move(refined.faceOrder);
        auto rebuilt=frames;
        const auto generated=generateGeometry(rebuilt,model,trial,false,out.job());
        if(!preservesSkinChannels(model,generated)) continue;
        measureTangentBasis(rebuilt,model,generated,trial,out.job());
        processAsciiSkinSeams(rebuilt,out.job());
        if(improvesReconstructedShading(model,plans,current,rows,rebuilt,generated,true)) {
            plans=std::move(trial);current=std::move(rebuilt);rows=generated;
        }
    }
    measureSeamNormals(current,model,rows,plans);
}

struct RoundedExpression {
    std::string key;
    double constant{};
    bool literal{},negative{};
};

RoundedExpression expressionConstant(double value) {
    const double magnitude=std::abs(value);
    std::uint64_t word;
    std::memcpy(&word,&magnitude,sizeof(word));
    return {"c"+std::to_string(word),magnitude,true,value<0.0};
}
RoundedExpression expressionNegate(RoundedExpression value) {
    if(!value.literal || value.constant!=0.0) value.negative=!value.negative;
    return value;
}
RoundedExpression expressionAdd(RoundedExpression a,RoundedExpression b,bool narrowed) {
    if(a.literal && a.constant==0.0) return b;
    if(b.literal && b.constant==0.0) return a;
    if(a.key==b.key && a.negative!=b.negative) return expressionConstant(0.0);
    if(a.literal && b.literal) {
        const double result=(a.negative?-a.constant:a.constant)+(b.negative?-b.constant:b.constant);
        return expressionConstant(narrowed?double(static_cast<float>(result)):result);
    }
    if(b.key<a.key) std::swap(a,b);
    const bool negative=a.negative;
    if(negative) {a=expressionNegate(a);b=expressionNegate(b);}
    return {std::string(narrowed?"f":"d")+"("+a.key+","+(b.negative?"-":"+")+b.key+")",0.0,false,negative};
}
RoundedExpression expressionProduct(RoundedExpression a,RoundedExpression b) {
    if((a.literal && a.constant==0.0) || (b.literal && b.constant==0.0)) return expressionConstant(0.0);
    if(a.literal && a.constant==1.0) return a.negative?expressionNegate(b):b;
    if(b.literal && b.constant==1.0) return b.negative?expressionNegate(a):a;
    if(a.literal && b.literal)
        return expressionConstant((a.negative?-a.constant:a.constant)*(b.negative?-b.constant:b.constant));
    const bool negative=a.negative!=b.negative;
    if(b.key<a.key) std::swap(a,b);
    return {"m("+a.key+","+b.key+")",0.0,false,negative};
}
RoundedExpression expressionNarrow(RoundedExpression value) {
    if(value.literal) return expressionConstant(static_cast<float>(value.negative?-value.constant:value.constant));
    value.key="n("+value.key+")";return value;
}

bool normalPrecisionMesh(const Mesh& mesh) {
    if(!mesh.render || mesh.animated) return false;
    std::map<std::array<std::uint32_t,3>,std::uint32_t> positions;
    std::vector<std::uint32_t> sources;
    for(const auto& vertex:mesh.vertices) {
        const auto p=vertex.position;
        const auto inserted=positions.emplace(std::array<std::uint32_t,3>{{bits(p.x),bits(p.y),bits(p.z)}},
            static_cast<std::uint32_t>(positions.size()));
        sources.push_back(inserted.first->second);
    }
    std::set<Triangle> winding;
    for(const auto& face:mesh.faces) {
        Triangle triangle{{sources[face.indices[0]],sources[face.indices[1]],sources[face.indices[2]]}};
        if(degenerate(triangle)) continue;
        auto reversed=triangle;std::swap(reversed[1],reversed[2]);
        if(winding.find(oriented(reversed))!=winding.end()) return true;
        winding.insert(oriented(triangle));
    }
    return false;
}

void reconstructNormalPositionPrecision(Writer& out,const Model& model,const Model& frames,
                                       std::map<const Mesh*,MeshReconstruction>& plans) {
    const auto eligible=[](const auto& entry) {
        return normalPrecisionMesh(*entry.first) && entry.second.hasNormals && !entry.second.explicitNormals;
    };
    if(std::none_of(plans.begin(),plans.end(),eligible)) return;
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    const auto component=[](Vec3 value,std::size_t axis) {
        return axis==0u?value.x:axis==1u?value.y:value.z;
    };
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        return (x*x+y*y)+z*z;
    };
    const auto angle=[](Vec3 a,Vec3 b) {
        if(same(a,b)) return 0.0;
        if(same(a,Vec3{}) || same(b,Vec3{})) return std::acos(-1.0);
        const double x=double(a.y)*b.z-double(a.z)*b.y;
        const double y=double(a.z)*b.x-double(a.x)*b.z;
        const double z=double(a.x)*b.y-double(a.y)*b.x;
        return std::atan2(std::sqrt((x*x+y*y)+z*z),
            (double(a.x)*b.x+double(a.y)*b.y)+double(a.z)*b.z);
    };
    const auto normal=ascii::normalizeVertexNormal;
    struct Transform {Vec3 position;Quaternion orientation;};
    std::vector<Transform> transforms(model.meshes.size());
    for(std::size_t index=0;index<model.meshes.size();++index) {
        auto& transform=transforms[index];
        auto ancestor=std::optional<std::size_t>{model.meshes[index].nodeIndex};
        while(ancestor) {
            const auto& node=frames.nodes[*ancestor];
            const auto rotated=ascii::engineModelRotateVector(node.orientation,transform.position);
            transform.position={rotated.x+node.position.x,rotated.y+node.position.y,rotated.z+node.position.z};
            transform.orientation=ascii::engineModelQuaternionProduct(node.orientation,transform.orientation);
            ancestor=node.parent;
        }
    }
    const auto world=[&](std::size_t index,Vec3 point) {
        const auto rotated=ascii::engineModelRotateVector(transforms[index].orientation,point);
        const auto p=transforms[index].position;
        return Vec3{rotated.x+p.x,rotated.y+p.y,rotated.z+p.z};
    };
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !eligible(*found)) continue;
        auto& plan=found->second;
        auto high=plan.preciseSourcePositions;
        if(high.empty())
            for(const auto p:plan.sourcePositions) high.push_back({p.x,p.y,p.z});
        for(std::size_t source=0;source<high.size();++source)
            for(std::size_t axis=0;axis<3u;++axis) {
                const auto low=component(plan.sourcePositions[source],axis);
                if(rawBits(static_cast<float>(high[source][axis]))!=rawBits(low)) high[source][axis]=low;
            }
        struct Group {
            std::size_t source;
            std::vector<std::size_t> faces,corners;
            bool cancelled{},seamFree{true};
        };
        std::vector<Group> groups;
        std::map<std::tuple<std::uint32_t,std::uint32_t,std::size_t>,std::size_t> groupIds;
        std::vector<std::vector<std::size_t>> incident(high.size()),faceGroups(mesh.faces.size());
        const auto rebuildGroups=[&] {
            groups.clear();groupIds.clear();
            incident.assign(high.size(),{});faceGroups.assign(mesh.faces.size(),{});
            for(const auto face:plan.faceOrder) {
                for(const auto source:plan.topology.sourceFaces[face])
                    if(incident[source].empty() || incident[source].back()!=face) incident[source].push_back(face);
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto source=plan.topology.sourceFaces[face][corner],mask=plan.smoothing.masks[face];
                    const auto key=std::make_tuple(source,mask,mask?0u:face);
                    const auto inserted=groupIds.emplace(key,groups.size());
                    if(inserted.second) groups.push_back({source,{},{},false,true});
                    groups[inserted.first->second].corners.push_back(face*3u+corner);
                }
            }
            for(auto& group:groups) {
                const auto first=group.corners.front()/3u;
                const auto mask=plan.smoothing.masks[first];
                std::map<Triangle,std::size_t> winding;
                for(const auto face:incident[group.source])
                    if(face==first || (mask&plan.smoothing.masks[face])) {
                        group.faces.push_back(face);
                        ++winding[oriented(plan.topology.sourceFaces[face])];
                        faceGroups[face].push_back(static_cast<std::size_t>(&group-groups.data()));
                    }
                group.cancelled=!winding.empty() && std::all_of(winding.begin(),winding.end(),[&](const auto& entry) {
                    auto reversed=entry.first;std::swap(reversed[1],reversed[2]);
                    const auto opposite=winding.find(oriented(reversed));
                    return opposite!=winding.end() && opposite->second==entry.second;
                });
            }
            if(std::none_of(groups.begin(),groups.end(),[](const Group& group){return group.cancelled;})) return false;
            if(current.skinSeams.processed) {
                for(auto& group:groups) {
                    const auto point=world(index,plan.sourcePositions[group.source]);
                    for(std::size_t other=0;other<current.meshes.size() && group.seamFree;++other) {
                        const auto& candidate=current.meshes[other];
                        if(other==index || !candidate.render || candidate.explicitNormals) continue;
                        for(std::size_t row=0;row<=candidate.enginePrelightVertexCount;++row) {
                            const float tail=candidate.skinned?1000000.0f:10000000.0f;
                            const auto p=world(other,row==candidate.enginePrelightVertexCount?
                                Vec3{tail,tail,tail}:candidate.vertices[row].position);
                            constexpr float tolerance=0.0010000000474974513f;
                            if(std::abs(p.x-point.x)<tolerance && std::abs(p.y-point.y)<tolerance &&
                               std::abs(p.z-point.z)<tolerance) {group.seamFree=false;break;}
                        }
                    }
                }
            }
            return true;
        };
        if(!rebuildGroups()) continue;
        const auto products=[&](std::size_t face,std::size_t source,std::size_t axis,double value,
                                std::size_t paired=std::numeric_limits<std::size_t>::max(),
                                std::size_t rotation=3u) {
            std::array<Vec3,3> low;
            std::array<std::array<double,3>,3> precise;
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto vertex=plan.topology.sourceFaces[face][rotation<3u?
                    (rotation+corner)%3u:plan.sourceCorner(face,corner)];
                low[corner]=plan.sourcePositions[vertex];precise[corner]=high[vertex];
                if(vertex==source) precise[corner][axis]=value;
                if(vertex==paired) precise[corner][axis]=high[paired][axis]-(value-high[source][axis]);
            }
            return ascii::normalPositionFaceProducts(low,precise);
        };
        const auto crossFrom=[](const std::array<Vec3,2>& product) {
            return Vec3{product[0].x-product[1].x,product[0].y-product[1].y,product[0].z-product[1].z};
        };
        const auto enumerateAxis=[&](std::size_t source,std::size_t axis,std::size_t paired,
                                     const std::set<std::size_t>& affected,
                                     const std::vector<Vec3>& localCrosses,const auto& consider,
                                     bool cyclicStates=false) {
            out.job().check();
            const double initial=high[source][axis];
            auto pathFaces=incident[source];
            if(paired<high.size()) {
                pathFaces.insert(pathFaces.end(),incident[paired].begin(),incident[paired].end());
                std::sort(pathFaces.begin(),pathFaces.end());
                pathFaces.erase(std::unique(pathFaces.begin(),pathFaces.end()),pathFaces.end());
            }
            std::vector<double> ends;
            const float stored=component(plan.sourcePositions[source],axis);
            for(const float direction:{-std::numeric_limits<float>::infinity(),
                                        std::numeric_limits<float>::infinity()}) {
                if(stored==0.0f && std::signbit(stored)!=std::signbit(direction)) continue;
                const float neighbor=std::nextafter(stored,direction);
                const double edge=std::isinf(neighbor)?double(stored)+
                    (double(stored)-std::nextafter(stored,0.0f))*0.5:(double(stored)+neighbor)*0.5;
                double end=std::nextafter(edge,double(stored));
                if(paired<high.size()) {
                    const auto retained=[&](double value) {
                        return rawBits(static_cast<float>(high[paired][axis]-(value-initial)))==
                            rawBits(component(plan.sourcePositions[paired],axis));
                    };
                    if(!retained(end)) {
                        double inside=initial,outside=end;
                        while(std::nextafter(inside,outside)!=outside) {
                            const double middle=inside+(outside-inside)*0.5;
                            if(retained(middle)) inside=middle;else outside=middle;
                        }
                        end=inside;
                    }
                }
                ends.push_back(end);
            }
            // Keep the double expression tree and its rounded products intact.
            const auto symbolicCross=[&](std::size_t face) {
                std::array<std::array<RoundedExpression,3>,3> residual;
                std::array<Vec3,3> low;
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto vertex=plan.topology.sourceFaces[face][plan.sourceCorner(face,corner)];
                    low[corner]=plan.sourcePositions[vertex];
                    for(std::size_t c=0;c<3u;++c) {
                        auto authored=vertex==source && c==axis?RoundedExpression{"v",0.0,false,false}:
                            expressionConstant(high[vertex][c]);
                        if(vertex==paired && c==axis)
                            authored=expressionAdd(expressionConstant(high[paired][axis]),
                                expressionNegate(expressionAdd(RoundedExpression{"v",0.0,false,false},
                                    expressionConstant(-initial),false)),false);
                        residual[corner][c]=expressionAdd(authored,
                            expressionConstant(-double(component(low[corner],c))),false);
                    }
                }
                std::array<std::array<RoundedExpression,3>,2> edge;
                for(std::size_t e=0;e<2u;++e)
                    for(std::size_t c=0;c<3u;++c) {
                        const float base=component(low[e+1u],c)-component(low[e],c);
                        edge[e][c]=expressionAdd(expressionConstant(base),
                            expressionAdd(residual[e+1u][c],expressionNegate(residual[e][c]),false),false);
                    }
                std::array<RoundedExpression,3> result;
                for(std::size_t c=0;c<3u;++c) {
                    const auto a=(c+1u)%3u,b=(c+2u)%3u;
                    result[c]=expressionAdd(expressionNarrow(expressionProduct(edge[0][a],edge[1][b])),
                        expressionNegate(expressionNarrow(expressionProduct(edge[0][b],edge[1][a]))),true);
                }
                return result;
            };
            const auto invariant=[&] {
                for(const auto id:affected) {
                    std::array<RoundedExpression,3> sum{{expressionConstant(0.0),expressionConstant(0.0),expressionConstant(0.0)}};
                    for(const auto face:groups[id].faces) {
                        const auto value=symbolicCross(face);
                        for(std::size_t c=0;c<3u;++c) sum[c]=expressionAdd(sum[c],value[c],true);
                    }
                    if(!std::all_of(sum.begin(),sum.end(),[](const auto& value) {
                        return value.literal && std::isfinite(value.constant);
                    })) return false;
                }
                for(const auto end:ends)
                    for(const auto id:affected) {
                        std::array<float,3> magnitude{};
                        for(const auto face:groups[id].faces) {
                            const auto a=products(face,source,axis,initial,paired),
                                b=products(face,source,axis,end,paired);
                            for(std::size_t c=0;c<3u;++c) {
                                const float first=std::max(std::abs(component(a[0],c)),std::abs(component(b[0],c))),
                                    second=std::max(std::abs(component(a[1],c)),std::abs(component(b[1],c)));
                                magnitude[c]+=first+second;
                                if(!std::isfinite(component(a[0],c)) || !std::isfinite(component(a[1],c)) ||
                                   !std::isfinite(component(b[0],c)) || !std::isfinite(component(b[1],c)) ||
                                   !std::isfinite(magnitude[c])) return false;
                            }
                        }
                    }
                return true;
            };
            if(!cyclicStates && invariant()) return;
            struct StateHash {
                std::size_t operator()(const std::vector<std::uint32_t>& words) const {
                    std::size_t hash{};
                    for(const auto word:words) hash=(hash^word)*16777619u;
                    return hash;
                }
            };
            std::unordered_set<std::vector<std::uint32_t>,StateHash> visited;
            struct AxisState {
                double value;
                std::vector<std::array<Vec3,2>> products;
            };
            const auto stride=cyclicStates?4u:1u;
            const bool groupBounds=paired>=high.size() && !cyclicStates;
            const auto state=[&](double value) {
                AxisState result{value,{}};
                result.products.reserve(pathFaces.size()*stride);
                for(const auto face:pathFaces) {
                    result.products.push_back(products(face,source,axis,value,paired));
                    if(cyclicStates) for(std::size_t rotation=0;rotation<3u;++rotation)
                        result.products.push_back(products(face,source,axis,value,paired,rotation));
                }
                return result;
            };
            auto proposed=localCrosses;
            const auto emit=[&](const AxisState& value) {
                std::vector<std::uint32_t> key;
                key.reserve(pathFaces.size()*stride*3u);
                for(std::size_t index=0;index<pathFaces.size();++index) {
                    const auto face=pathFaces[index];
                    const auto result=crossFrom(value.products[index*stride]);
                    proposed[face]=result;
                    key.push_back(rawBits(result.x));key.push_back(rawBits(result.y));key.push_back(rawBits(result.z));
                    if(cyclicStates) for(std::size_t rotation=0;rotation<3u;++rotation) {
                        const auto alternate=crossFrom(value.products[index*stride+rotation+1u]);
                        key.push_back(rawBits(alternate.x));key.push_back(rawBits(alternate.y));
                        key.push_back(rawBits(alternate.z));
                    }
                }
                if(visited.insert(std::move(key)).second) consider(value.value,proposed);
            };
            const auto initialState=state(initial);
            emit(initialState);
            for(const auto end:ends) {
                auto lowerState=initialState;
                std::vector<AxisState> upperStates;
                upperStates.push_back(state(end));
                emit(upperStates.back());
                auto lowerCross=localCrosses,upperCross=localCrosses;
                while(!upperStates.empty()) {
                    out.job().check();
                    const auto& upperState=upperStates.back();
                    // Same-axis paths leave only one varying factor in each product.
                    bool constant=true;
                    for(std::size_t index=0;index<pathFaces.size();++index) {
                        const auto face=pathFaces[index];
                        if(cyclicStates) for(std::size_t rotation=0;rotation<3u;++rotation) {
                            const auto& a=lowerState.products[index*stride+rotation+1u];
                            const auto& b=upperState.products[index*stride+rotation+1u];
                            for(std::size_t c=0;c<3u;++c) {
                                const float lower=std::min(component(a[0],c),component(b[0],c))-
                                    std::max(component(a[1],c),component(b[1],c));
                                const float upper=std::max(component(a[0],c),component(b[0],c))-
                                    std::min(component(a[1],c),component(b[1],c));
                                constant=constant && rawBits(lower)==rawBits(upper);
                            }
                        }
                        const auto& a=lowerState.products[index*stride];
                        const auto& b=upperState.products[index*stride];
                        std::array<float,3> lows{},highs{};
                        for(std::size_t c=0;c<3u;++c) {
                            const float a0=component(a[0],c),a1=component(a[1],c),
                                b0=component(b[0],c),b1=component(b[1],c);
                            const float lower=std::min(a0,b0)-std::max(a1,b1),
                                upper=std::max(a0,b0)-std::min(a1,b1);
                            lows[c]=lower;highs[c]=upper;
                            constant=constant && rawBits(lower)==rawBits(upper);
                        }
                        lowerCross[face]={lows[0],lows[1],lows[2]};
                        upperCross[face]={highs[0],highs[1],highs[2]};
                        if(!constant && !groupBounds) break;
                    }
                    if(!constant && groupBounds) {
                        constant=true;
                        for(const auto id:affected) {
                            Vec3 lower{},upper{};
                            for(const auto face:groups[id].faces) {
                                const auto a=lowerCross[face],b=upperCross[face];
                                lower={lower.x+a.x,lower.y+a.y,a.z+lower.z};
                                upper={upper.x+b.x,upper.y+b.y,b.z+upper.z};
                            }
                            constant=constant && rawBits(lower.x)==rawBits(upper.x) &&
                                rawBits(lower.y)==rawBits(upper.y) && rawBits(lower.z)==rawBits(upper.z);
                            if(!constant) break;
                        }
                    }
                    if(constant || std::nextafter(lowerState.value,upperState.value)==upperState.value) {
                        emit(upperState);
                        lowerState=std::move(upperStates.back());upperStates.pop_back();continue;
                    }
                    const double middle=lowerState.value+(upperState.value-lowerState.value)*0.5;
                    upperStates.push_back(state(middle));
                }
            }
        };
        const auto unresolvedSources=[&] {
            std::vector<bool> selected(high.size());
            for(const auto& group:groups) {
                if(!group.cancelled || !group.seamFree) continue;
                bool differs=false;
                for(const auto corner:group.corners) {
                    const auto face=corner/3u,c=corner%3u;
                    differs=differs || !same(current.meshes[index].normalAt(rows[index][face][c]),
                        plan.vertices[mesh.faces[face].indices[c]].normal);
                }
                if(differs) for(const auto face:group.faces)
                    for(const auto source:plan.topology.sourceFaces[face]) selected[source]=true;
            }
            return selected;
        };
        std::vector<std::vector<std::array<double,3>>> coupledOrigins;
        for(auto selected=unresolvedSources();
            std::any_of(selected.begin(),selected.end(),[](bool value){return value;});
            selected=unresolvedSources()) {
            out.job().check();
            std::vector<Vec3> crosses;
            for(std::size_t face=0;face<mesh.faces.size();++face)
                crosses.push_back(crossFrom(products(face,high.size(),0u,0.0)));
            struct Candidate {std::size_t source;std::array<double,3> position;double error;};
            const auto collectCandidates=[&](const std::vector<bool>& allowed,
                                             const std::vector<Vec3>& inputCrosses,
                                             const std::vector<Vec3>* inputNormals,
                                             std::optional<std::size_t> targetGroup=std::nullopt) {
                std::vector<Candidate> candidates;
                for(std::size_t source=0;source<high.size();++source) {
                    if(!allowed[source]) continue;
                    std::set<std::size_t> affected;
                    for(const auto face:incident[source])
                        affected.insert(faceGroups[face].begin(),faceGroups[face].end());
                    if(std::any_of(affected.begin(),affected.end(),[&](std::size_t group){return !groups[group].seamFree;})) continue;
                    const auto original=high[source];
                    auto localCrosses=inputCrosses;
                    std::vector<Vec3> localNormals(groups.size());
                    std::vector<std::array<double,2>> localErrors(mesh.faces.size()*3u);
                    const auto updateNormals=[&] {
                        for(const auto id:affected) {
                            Vec3 sum{};
                            for(const auto face:groups[id].faces) {
                                const auto value=localCrosses[face];
                                sum={sum.x+value.x,sum.y+value.y,value.z+sum.z};
                            }
                            localNormals[id]=normal(sum);
                            for(const auto corner:groups[id].corners) {
                                const auto target=plan.vertices[mesh.faces[corner/3u].indices[corner%3u]].normal;
                                localErrors[corner]={{difference(localNormals[id],target),angle(localNormals[id],target)}};
                            }
                        }
                    };
                    updateNormals();
                    // Converge one source's editable XYZ before choosing among
                    // sources; an immediately better neighbor need not have the
                    // best completed coordinate reconstruction.
                    for(;;) {
                        struct Move {std::size_t axis;double value,error;};
                        std::vector<Move> moves;
                        for(std::size_t axis=0;axis<3u;++axis) {
                            enumerateAxis(source,axis,high.size(),affected,localCrosses,
                                [&](double value,const std::vector<Vec3>& proposed) {
                                double error{};bool improved=false;
                                for(const auto id:affected) {
                                    if(targetGroup && id!=*targetGroup) continue;
                                    const auto& group=groups[id];Vec3 sum{};
                                    for(const auto face:group.faces) {
                                        const auto cross=proposed[face];
                                        sum={sum.x+cross.x,sum.y+cross.y,cross.z+sum.z};
                                    }
                                    const auto after=normal(sum);
                                    for(const auto corner:group.corners) {
                                        const auto face=corner/3u,c=corner%3u;
                                        const auto target=plan.vertices[mesh.faces[face].indices[c]].normal;
                                        const auto& previous=localErrors[corner];
                                        const double next=difference(after,target);
                                        if(next>previous[0] || angle(after,target)>previous[1]) return;
                                        error+=next-previous[0];improved=improved || next<previous[0];
                                    }
                                }
                                if(improved) moves.push_back({axis,value,error});
                            },targetGroup.has_value());
                        }
                        if(moves.empty()) break;
                        const auto best=std::min_element(moves.begin(),moves.end(),[&](const Move& a,const Move& b) {
                            if(targetGroup) return std::tie(a.error,a.axis,a.value)<std::tie(b.error,b.axis,b.value);
                            return std::make_tuple(a.error,std::abs(a.value-high[source][a.axis]),a.axis,a.value)<
                                std::make_tuple(b.error,std::abs(b.value-high[source][b.axis]),b.axis,b.value);
                        });
                        if(targetGroup) {
                            double preceding{};
                            for(const auto corner:groups[*targetGroup].corners) {
                                const auto face=corner/3u,c=corner%3u;
                                const auto target=plan.vertices[mesh.faces[face].indices[c]].normal;
                                const auto before=inputNormals?inputNormals->at(*targetGroup):
                                    current.meshes[index].normalAt(rows[index][face][c]);
                                preceding+=difference(localNormals[*targetGroup],target)-difference(before,target);
                            }
                            for(const auto& move:moves) if(move.error==best->error) {
                                auto position=high[source];position[move.axis]=move.value;
                                candidates.push_back({source,position,preceding+move.error});
                            }
                        }
                        high[source][best->axis]=best->value;
                        for(const auto face:incident[source])
                            localCrosses[face]=crossFrom(products(face,high.size(),0u,0.0));
                        updateNormals();
                        double error{};
                        for(const auto id:affected) if(!targetGroup || id==*targetGroup)
                            for(const auto corner:groups[id].corners) {
                                const auto face=corner/3u,c=corner%3u;
                                const auto target=plan.vertices[mesh.faces[face].indices[c]].normal;
                                const auto before=inputNormals?inputNormals->at(id):
                                    current.meshes[index].normalAt(rows[index][face][c]);
                                error+=difference(localNormals[id],target)-difference(before,target);
                            }
                        candidates.push_back({source,high[source],error});
                    }
                    high[source]=original;
                }
                return candidates;
            };
            const auto sortCandidates=[&](std::vector<Candidate>& candidates) {
                std::size_t authoredComponents{};
                for(std::size_t source=0;source<high.size();++source)
                    for(std::size_t axis=0;axis<3u;++axis)
                        authoredComponents+=high[source][axis]!=double(component(plan.sourcePositions[source],axis));
                const auto componentCount=[&](const Candidate& candidate) {
                    auto count=authoredComponents;
                    for(std::size_t axis=0;axis<3u;++axis) {
                        const double packed=component(plan.sourcePositions[candidate.source],axis);
                        count-=high[candidate.source][axis]!=packed;
                        count+=candidate.position[axis]!=packed;
                    }
                    return count;
                };
                std::sort(candidates.begin(),candidates.end(),[&](const Candidate& a,const Candidate& b) {
                    const auto displacement=[&](const Candidate& value) {
                        double sum{};
                        for(std::size_t axis=0;axis<3u;++axis)
                            sum+=std::abs(value.position[axis]-high[value.source][axis]);
                        return sum;
                    };
                    return std::make_tuple(a.error,componentCount(a),displacement(a),a.source,a.position)<
                        std::make_tuple(b.error,componentCount(b),displacement(b),b.source,b.position);
                });
            };
            auto candidates=collectCandidates(selected,crosses,nullptr);
            sortCandidates(candidates);
            const auto accept=[&](MeshReconstruction candidate) {
                std::map<const Mesh*,MeshReconstruction> changed;
                changed.emplace(&mesh,std::move(candidate));
                auto rebuilt=current;
                rebuilt.meshes[index]=frames.meshes[index];
                auto generated=rows;
                auto changedRows=generateGeometry(rebuilt,model,changed,false,out.job());
                generated[index]=std::move(changedRows[index]);
                if(!preservesSkinChannels(model,generated)) return false;
                measureTangentBasis(rebuilt,model,generated,plans,out.job(),&changed);
                processAsciiSkinSeams(rebuilt,out.job());
                if(!improvesReconstructedShading(model,plans,current,rows,rebuilt,generated,true)) return false;
                plan=std::move(changed.at(&mesh));
                if(!plan.preciseSourcePositions.empty()) high=plan.preciseSourcePositions;
                current=std::move(rebuilt);rows=std::move(generated);return true;
            };
            bool accepted=false;
            for(const auto& candidate:candidates) {
                auto trial=plan;
                trial.preciseSourcePositions=high;trial.preciseSourcePositions[candidate.source]=candidate.position;
                if(accept(std::move(trial))) {accepted=true;break;}
            }
            if(accepted) continue;

            // Reconsider missing smoothing links with the new face crosses.
            // A connected set of missing links may need one shared bit before
            // any of its individual links becomes an improvement.
            std::vector<bool> eligibleCorners(mesh.faces.size()*3u);
            for(const auto& group:groups) if(group.seamFree && selected[group.source])
                for(const auto corner:group.corners) {
                    const auto face=corner/3u,c=corner%3u;
                    eligibleCorners[corner]=!same(current.meshes[index].normalAt(rows[index][face][c]),
                        plan.vertices[mesh.faces[face].indices[c]].normal);
                }
            const auto masks=sharedSeamSmoothingCandidates(mesh,plan.vertices,plan.topology.sourceFaces,
                plan.smoothing.masks,eligibleCorners,out.job(),false,true);
            for(const auto& candidate:masks) {
                auto trial=plan;trial.smoothing.masks=candidate.masks;
                if(accept(std::move(trial))) {accepted=true;break;}
            }
            if(accepted) {
                if(!rebuildGroups()) break;
                continue;
            }

            // Keep the published edge neighbors while refitting the ordered
            // gathers against the actual authored-position precision.
            if(!model.nodes[mesh.nodeIndex].aabb) {
                auto order=reconstructNormalFaceOrder(mesh,plan.vertices,plan.topology.sourceVertices,
                    plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,
                    plan.sourcePositions,out.job(),&high);
                if(order!=plan.faceOrder) {
                    auto trial=plan;trial.faceOrder=std::move(order);
                    accepted=accept(std::move(trial));
                }
                if(!accepted) {
                    auto stars=reconstructNormalCornerStars(mesh,plan.vertices,plan.topology.sourceVertices,
                        plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,
                        plan.sourcePositions,out.job(),&high);
                    if(stars.faceOrder!=plan.faceOrder || stars.rotations!=plan.cornerRotations) {
                        auto trial=plan;trial.faceOrder=std::move(stars.faceOrder);
                        trial.cornerRotations=std::move(stars.rotations);accepted=accept(std::move(trial));
                    }
                }
            }
            if(!accepted) {
                struct Seed {
                    std::size_t first,second,axis;
                    double value,error;
                    std::vector<std::size_t> repairs;
                    bool direct;
                };
                std::vector<std::size_t> repairSources;
                for(std::size_t source=0;source<selected.size();++source)
                    if(selected[source]) repairSources.push_back(source);
                std::vector<std::vector<std::size_t>> reaches(groups.size());
                std::vector<std::array<double,2>> previousErrors(mesh.faces.size()*3u);
                for(std::size_t id=0;id<groups.size();++id) {
                    for(const auto face:groups[id].faces)
                        for(const auto source:plan.topology.sourceFaces[face]) reaches[id].push_back(source);
                    std::sort(reaches[id].begin(),reaches[id].end());
                    for(const auto corner:groups[id].corners) {
                        const auto face=corner/3u,c=corner%3u;
                        const auto target=plan.vertices[mesh.faces[face].indices[c]].normal;
                        const auto before=current.meshes[index].normalAt(rows[index][face][c]);
                        previousErrors[corner]={{difference(before,target),angle(before,target)}};
                    }
                }
                std::set<std::pair<std::size_t,std::size_t>> pairs;
                for(const auto& group:groups) {
                    if(!group.cancelled || !group.seamFree || !selected[group.source]) continue;
                    for(const auto face:group.faces) {
                        const auto& triangle=plan.topology.sourceFaces[face];
                        for(std::size_t edge=0;edge<3u;++edge) {
                            const auto a=triangle[edge],b=triangle[(edge+1u)%3u];
                            if(a!=b) pairs.emplace(std::min(a,b),std::max(a,b));
                        }
                    }
                }
                std::vector<Seed> seeds;
                for(const auto& pair:pairs) {
                    std::set<std::size_t> affected;
                    for(const auto source:{pair.first,pair.second})
                        for(const auto face:incident[source])
                            affected.insert(faceGroups[face].begin(),faceGroups[face].end());
                    if(std::any_of(affected.begin(),affected.end(),[&](std::size_t id){return !groups[id].seamFree;})) continue;
                    for(std::size_t axis=0;axis<3u;++axis)
                        enumerateAxis(pair.first,axis,pair.second,affected,crosses,
                            [&](double value,const std::vector<Vec3>& proposed) {
                                bool improved=false,direct=true;double error{};
                                auto repairs=repairSources;
                                for(const auto id:affected) {
                                    const auto& group=groups[id];Vec3 sum{};
                                    for(const auto face:group.faces) {
                                        const auto cross=proposed[face];
                                        sum={sum.x+cross.x,sum.y+cross.y,cross.z+sum.z};
                                    }
                                    const auto after=normal(sum);bool worse=false;
                                    for(const auto corner:group.corners) {
                                        const auto face=corner/3u,c=corner%3u;
                                        const auto target=plan.vertices[mesh.faces[face].indices[c]].normal;
                                        const auto& previous=previousErrors[corner];
                                        const double next=difference(after,target);
                                        const bool angleWorse=angle(after,target)>previous[1];
                                        error+=next-previous[0];worse=worse || next>previous[0] || angleWorse;
                                        improved=improved || (group.cancelled && next<previous[0] && !angleWorse);
                                    }
                                    if(worse) {
                                        direct=false;
                                        // One repair source must influence every worsened group.
                                        repairs.erase(std::remove_if(repairs.begin(),repairs.end(),
                                            [&](std::size_t source) {
                                                return !std::binary_search(reaches[id].begin(),reaches[id].end(),source);
                                            }),repairs.end());
                                        if(repairs.empty()) return;
                                    }
                                }
                                if(improved && (direct || !repairs.empty()))
                                    seeds.push_back({pair.first,pair.second,axis,value,error,std::move(repairs),direct});
                            });
                }
                std::sort(seeds.begin(),seeds.end(),[](const Seed& a,const Seed& b) {
                    return std::tie(a.error,a.first,a.second,a.axis,a.value)<
                        std::tie(b.error,b.first,b.second,b.axis,b.value);
                });
                for(const auto& seed:seeds) {
                    const auto original=high;
                    high[seed.second][seed.axis]-=seed.value-high[seed.first][seed.axis];
                    high[seed.first][seed.axis]=seed.value;
                    auto trial=plan;trial.preciseSourcePositions=high;
                    if(seed.direct && accept(trial)) {
                        coupledOrigins.push_back(original);accepted=true;break;
                    }
                    auto seedCrosses=crosses;
                    for(const auto source:{seed.first,seed.second})
                        for(const auto face:incident[source])
                            seedCrosses[face]=crossFrom(products(face,high.size(),0u,0.0));
                    std::vector<Vec3> seedNormals;
                    for(const auto& group:groups) {
                        Vec3 sum{};
                        for(const auto face:group.faces) {
                            const auto cross=seedCrosses[face];
                            sum={sum.x+cross.x,sum.y+cross.y,cross.z+sum.z};
                        }
                        seedNormals.push_back(normal(sum));
                    }
                    std::vector<bool> allowed(high.size());
                    for(const auto source:seed.repairs) allowed[source]=true;
                    auto repairs=collectCandidates(allowed,seedCrosses,&seedNormals);
                    sortCandidates(repairs);
                    for(const auto& repair:repairs) {
                        trial.preciseSourcePositions=high;
                        trial.preciseSourcePositions[repair.source]=repair.position;
                        if(accept(trial)) {accepted=true;break;}
                    }
                    if(accepted) {coupledOrigins.push_back(original);break;}
                    high=original;
                }
            }
            if(!accepted && !coupledOrigins.empty() && !model.nodes[mesh.nodeIndex].aabb) {
                const auto retained=high;
                std::vector<Vec3> baselineCorners(mesh.faces.size()*3u);
                for(std::size_t face=0;face<mesh.faces.size();++face)
                    for(std::size_t corner=0;corner<3u;++corner)
                        baselineCorners[face*3u+corner]=current.meshes[index].normalAt(rows[index][face][corner]);
                const auto groupNormal=[&](std::size_t id,const std::vector<Vec3>& values) {
                    Vec3 sum{};
                    for(const auto face:groups[id].faces) {
                        const auto value=values[face];sum={sum.x+value.x,sum.y+value.y,value.z+sum.z};
                    }
                    return normal(sum);
                };
                const auto localCrosses=[&] {
                    std::vector<Vec3> result;
                    for(std::size_t face=0;face<mesh.faces.size();++face)
                        result.push_back(crossFrom(products(face,high.size(),0u,0.0)));
                    return result;
                };
                const auto worsens=[&](std::size_t id,Vec3 proposed) {
                    for(const auto corner:groups[id].corners) {
                        const auto target=plan.vertices[mesh.faces[corner/3u].indices[corner%3u]].normal;
                        const auto before=baselineCorners[corner];
                        if(difference(proposed,target)>difference(before,target) ||
                           angle(proposed,target)>angle(before,target)) return true;
                    }
                    return false;
                };
                std::vector<std::pair<double,std::size_t>> targets;
                for(std::size_t id=0;id<groups.size();++id) {
                    const auto& group=groups[id];
                    if(!group.cancelled || !group.seamFree) continue;
                    double error{};
                    for(const auto corner:group.corners)
                        error=std::max(error,angle(baselineCorners[corner],
                            plan.vertices[mesh.faces[corner/3u].indices[corner%3u]].normal));
                    if(error!=0.0) targets.emplace_back(-error,id);
                }
                std::sort(targets.begin(),targets.end());
                auto origins=std::move(coupledOrigins);coupledOrigins.clear();
                for(const auto& origin:origins) {
                    std::set<std::size_t> originGroups;
                    for(std::size_t vertex=0;vertex<high.size();++vertex) if(origin[vertex]!=retained[vertex])
                        for(const auto face:incident[vertex])
                            originGroups.insert(faceGroups[face].begin(),faceGroups[face].end());
                    for(const auto& targetEntry:targets) {
                        const auto targetId=targetEntry.second,source=groups[targetId].source;
                        high=origin;
                        const auto seedCrosses=localCrosses();
                        std::set<std::size_t> affected;
                        for(const auto face:incident[source])
                            affected.insert(faceGroups[face].begin(),faceGroups[face].end());
                        if(std::any_of(affected.begin(),affected.end(),[&](std::size_t id){return !groups[id].seamFree;})) continue;
                        auto scored=affected;scored.insert(originGroups.begin(),originGroups.end());
                        std::array<std::vector<double>,3> axisValues;
                        for(std::size_t axis=0;axis<3u;++axis)
                            enumerateAxis(source,axis,high.size(),affected,seedCrosses,
                                [&](double value,const std::vector<Vec3>&) {axisValues[axis].push_back(value);},true);
                        std::vector<std::pair<std::size_t,std::size_t>> axes{{0,1},{0,2},{1,2}};
                        std::sort(axes.begin(),axes.end(),[&](const auto& a,const auto& b) {
                            return std::make_tuple(axisValues[a.first].size()*axisValues[a.second].size(),a)<
                                std::make_tuple(axisValues[b.first].size()*axisValues[b.second].size(),b);
                        });
                        for(const auto& pair:axes) {
                            std::optional<std::size_t> leastWorsened;
                            std::vector<std::array<double,3>> seeds;
                            std::set<std::vector<std::uint32_t>> states;
                            for(const auto first:axisValues[pair.first]) {
                                high=origin;high[source][pair.first]=first;
                                const auto intermediate=localCrosses();
                                enumerateAxis(source,pair.second,high.size(),affected,intermediate,
                                    [&](double second,const std::vector<Vec3>& proposed) {
                                        std::size_t worse{};bool improves=false;
                                        for(const auto id:scored) {
                                            const auto value=groupNormal(id,proposed);
                                            for(const auto corner:groups[id].corners) {
                                                const auto target=plan.vertices[mesh.faces[corner/3u].indices[corner%3u]].normal;
                                                const auto before=baselineCorners[corner];
                                                const double previous=difference(before,target),next=difference(value,target);
                                                const bool angleWorse=angle(value,target)>angle(before,target);
                                                worse+=next>previous || angleWorse;
                                                if(id==targetId) {
                                                    if(next>previous || angleWorse) return;
                                                    improves=improves || next<previous;
                                                }
                                            }
                                        }
                                        if(!improves) return;
                                        if(leastWorsened && worse>*leastWorsened) return;
                                        if(!leastWorsened || worse<*leastWorsened) {
                                            leastWorsened=worse;seeds.clear();states.clear();
                                        }
                                        std::vector<std::uint32_t> key;
                                        for(const auto face:incident[source]) for(std::size_t rotation=0;rotation<3u;++rotation) {
                                            const auto cross=crossFrom(products(face,source,pair.second,second,high.size(),rotation));
                                            key.push_back(rawBits(cross.x));key.push_back(rawBits(cross.y));key.push_back(rawBits(cross.z));
                                        }
                                        if(states.insert(std::move(key)).second) {
                                            auto position=high[source];position[pair.second]=second;seeds.push_back(position);
                                        }
                                    },true);
                            }
                            std::sort(seeds.begin(),seeds.end());
                            struct TopologyTrial {
                                std::vector<std::pair<std::size_t,std::array<double,3>>> positions;
                                std::vector<std::size_t> domain;
                                std::size_t worsened;
                                double error,targetError;
                            };
                            std::vector<TopologyTrial> trials;
                            const auto addTopologyTrials=[&](const std::vector<Vec3>& proposed) {
                                std::set<std::size_t> core(groups[targetId].faces.begin(),groups[targetId].faces.end());
                                std::set<std::size_t> changedGroups;
                                std::vector<std::pair<std::size_t,std::array<double,3>>> positions;
                                for(std::size_t vertex=0;vertex<high.size();++vertex) {
                                    if(high[vertex]!=origin[vertex]) positions.emplace_back(vertex,high[vertex]);
                                    if(high[vertex]!=retained[vertex])
                                        for(const auto face:incident[vertex])
                                            changedGroups.insert(faceGroups[face].begin(),faceGroups[face].end());
                                }
                                std::size_t worse{};double error{},targetError{};
                                for(const auto id:changedGroups) {
                                    const auto value=groupNormal(id,proposed);
                                    if(worsens(id,value)) {
                                        if(!groups[id].seamFree) return;
                                        core.insert(groups[id].faces.begin(),groups[id].faces.end());
                                    }
                                    for(const auto corner:groups[id].corners) {
                                        const auto target=plan.vertices[mesh.faces[corner/3u].indices[corner%3u]].normal;
                                        const auto before=baselineCorners[corner];
                                        const double previous=difference(before,target),next=difference(value,target);
                                        worse+=next>previous || angle(value,target)>angle(before,target);
                                        error+=next-previous;
                                        if(id==targetId) targetError+=next;
                                    }
                                }
                                std::set<std::vector<std::size_t>> domains;
                                domains.emplace(core.begin(),core.end());
                                for(const auto face:core) for(const auto id:faceGroups[face]) {
                                    if(!groups[id].cancelled || !groups[id].seamFree) continue;
                                    auto expanded=core;expanded.insert(groups[id].faces.begin(),groups[id].faces.end());
                                    domains.emplace(expanded.begin(),expanded.end());
                                }
                                for(const auto& domain:domains)
                                    trials.push_back({positions,domain,worse,error,targetError});
                            };
                            for(const auto& seed:seeds) {
                                high=origin;high[source]=seed;
                                const auto proposed=localCrosses();
                                addTopologyTrials(proposed);
                                std::vector<Vec3> seedNormals;
                                for(std::size_t id=0;id<groups.size();++id) seedNormals.push_back(groupNormal(id,proposed));
                                std::vector<bool> neighbors(high.size());
                                for(const auto face:groups[targetId].faces)
                                    for(const auto vertex:plan.topology.sourceFaces[face]) neighbors[vertex]=true;
                                const auto repairs=collectCandidates(neighbors,proposed,&seedNormals,targetId);
                                const auto seeded=high;
                                for(const auto& repair:repairs) {
                                    high=seeded;high[repair.source]=repair.position;
                                    addTopologyTrials(localCrosses());
                                }
                            }
                            std::sort(trials.begin(),trials.end(),[](const auto& a,const auto& b) {
                                const auto aSize=a.domain.size(),bSize=b.domain.size();
                                return std::tie(aSize,a.worsened,a.error,a.targetError,a.positions,a.domain)<
                                    std::tie(bSize,b.worsened,b.error,b.targetError,b.positions,b.domain);
                            });
                            trials.erase(std::unique(trials.begin(),trials.end(),[](const auto& a,const auto& b) {
                                return a.positions==b.positions && a.domain==b.domain;
                            }),trials.end());
                            for(const auto& candidate:trials) {
                                high=origin;
                                for(const auto& position:candidate.positions) high[position.first]=position.second;
                                auto stars=reconstructNormalCornerStars(mesh,plan.vertices,plan.topology.sourceVertices,
                                    plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,
                                    plan.sourcePositions,out.job(),&high,&candidate.domain,&baselineCorners);
                                auto trial=plan;trial.preciseSourcePositions=high;
                                trial.cornerRotations=std::move(stars.rotations);trial.faceOrder=std::move(stars.faceOrder);
                                if(accept(std::move(trial))) {accepted=true;break;}
                            }
                            if(accepted) break;
                        }
                        if(accepted) break;
                    }
                    if(accepted) break;
                }
                if(!accepted) high=retained;
            }
            if(!accepted || !rebuildGroups()) break;
        }
    }
    measureSeamNormals(current,model,rows,plans);
}

bool tangentPrecisionMesh(const Mesh& mesh) {
    return mesh.render && (mesh.mdxBitmap&0x80u) && !mesh.animated && !mesh.dangly;
}

void reconstructTangentUvPrecision(Writer& out,const Model& model,const Model& frames,
                                  std::map<const Mesh*,MeshReconstruction>& plans) {
    if(std::none_of(model.meshes.begin(),model.meshes.end(),tangentPrecisionMesh)) return;
    auto current=frames;
    const auto rows=generateGeometry(current,model,plans,false,out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    const auto opposite=[](Vec3 a,Vec3 b) {
        return !same(a,b) && (a.x*b.x+a.y*b.y)+a.z*b.z<=0.0f;
    };
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        return (x*x+y*y)+z*z;
    };
    const auto angle=[](Vec3 a,Vec3 b) {
        const double x=double(a.y)*b.z-double(a.z)*b.y;
        const double y=double(a.z)*b.x-double(a.x)*b.z;
        const double z=double(a.x)*b.y-double(a.y)*b.x;
        return std::atan2(std::sqrt((x*x+y*y)+z*z),
            (double(a.x)*b.x+double(a.y)*b.y)+double(a.z)*b.z);
    };
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !tangentPrecisionMesh(mesh)) continue;
        auto& plan=found->second;
        auto& existing=current.meshes[index];
        const auto rowSize=existing.mdxRowSize,weightOffset=existing.mdxWeightOffset,
            boneOffset=existing.mdxBoneIndexOffset,tangentOffset=existing.mdxTangentOffset;
        for(;;) {
            const auto& retained=*existing.asciiTangentSource;
            std::vector<unsigned> selected(existing.faces.size());
            std::vector<std::optional<std::size_t>> writers(existing.vertices.size());
            for(std::size_t face=0;face<existing.faces.size();++face)
                for(const auto row:existing.faces[face].indices) writers[row]=face;
            std::size_t oldOpposed{};
            for(std::size_t face=0;face<mesh.faces.size();++face)
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto row=rows[index][face][corner];
                    const auto& actual=existing.vertices[row];
                    const auto& target=plan.vertices[mesh.faces[face].indices[corner]].tangentBasis;
                    const bool b=opposite(actual.tangentBasis.bitangent,target.bitangent);
                    const bool t=opposite(actual.tangentBasis.tangent,target.tangent);
                    const bool n=opposite(actual.tangentBasis.normal,target.normal);
                    oldOpposed+=b || t || n;
                    if((!b && !t && !n) || !writers[row]) continue;
                    const auto writer=*writers[row];
                    for(std::size_t other=0;other<existing.faces.size();++other) {
                        if(other!=writer && !(retained.faceSmoothing[writer]&retained.faceSmoothing[other])) continue;
                        const auto& indices=existing.faces[other].indices;
                        if(other==writer || std::any_of(indices.begin(),indices.end(),[&](std::int16_t source) {
                                return same(retained.generatedPositions[source],actual.position);
                            })) selected[other]|=(b || n?1u:0u)|(t || n?2u:0u);
                    }
                }
            if(!oldOpposed) break;
            auto source=std::make_shared<AsciiTangentSource>(*existing.asciiTangentSource);
            if(source->faceTexcoords64.empty())
                for(const auto& face:existing.faces) {
                    std::array<std::array<double,2>,3> values;
                    for(std::size_t corner=0;corner<3u;++corner) {
                        const auto value=source->generatedTexcoords[face.indices[corner]];
                        values[corner]={value.x,value.y};
                    }
                    source->faceTexcoords64.push_back(values);
                }
            struct Search {
                std::size_t face,corner,component;
                double cursor,end;
                Vec3 value;
            };
            std::vector<Search> searches;
            const auto derivative=[&](const Search& search,double value) {
                const auto& face=existing.faces[search.face];
                const auto a=source->generatedPositions[face.indices[0]],
                    b=source->generatedPositions[face.indices[1]],c=source->generatedPositions[face.indices[2]];
                const Vec3 p{b.x-a.x,b.y-a.y,b.z-a.z},q{c.x-a.x,c.y-a.y,c.z-a.z};
                std::array<Vec2,3> uv;
                for(std::size_t corner=0;corner<3u;++corner)
                    uv[corner]=source->generatedTexcoords[face.indices[corner]];
                auto precise=source->faceTexcoords64[search.face];
                precise[search.corner][search.component]=value;
                return ascii::tangentUvDerivative(p,q,uv,precise,search.component);
            };
            bool accepted=false;
            const auto consider=[&](const Search& search,double value) {
                out.job().check();
                auto candidate=existing;
                auto proposed=std::make_shared<AsciiTangentSource>(*source);
                proposed->faceTexcoords64[search.face][search.corner][search.component]=value;
                candidate.asciiTangentSource=proposed;
                rebuildAsciiTangentBasis(candidate,out.job());
                std::size_t opposed{};
                bool retained=true,improved=false;
                const auto compare=[&](Vec3 before,Vec3 after,Vec3 target) {
                    if(same(before,after)) return;
                    const double oldError=difference(before,target),newError=difference(after,target);
                    retained=retained && newError<=oldError && angle(after,target)<=angle(before,target);
                    improved=improved || newError<oldError;
                };
                for(std::size_t face=0;face<mesh.faces.size() && retained;++face)
                    for(std::size_t corner=0;corner<3u;++corner) {
                        const auto row=rows[index][face][corner];
                        const auto& before=existing.vertices[row].tangentBasis;
                        const auto& after=candidate.vertices[row].tangentBasis;
                        const auto& target=plan.vertices[mesh.faces[face].indices[corner]].tangentBasis;
                        compare(before.bitangent,after.bitangent,target.bitangent);
                        compare(before.tangent,after.tangent,target.tangent);
                        compare(before.normal,after.normal,target.normal);
                        opposed+=opposite(after.bitangent,target.bitangent) ||
                            opposite(after.tangent,target.tangent) || opposite(after.normal,target.normal);
                    }
                if(!retained || !improved || opposed>=oldOpposed) return false;
                if(plan.preciseFaceTexcoords.empty()) {
                    plan.preciseFaceTexcoords.resize(mesh.faces.size());
                    for(std::size_t face=0;face<mesh.faces.size();++face)
                        for(std::size_t corner=0;corner<3u;++corner) {
                            const auto value=plan.vertices[mesh.faces[face].indices[corner]].texcoord0;
                            plan.preciseFaceTexcoords[face][corner]={value.x,value.y};
                        }
                }
                const auto original=plan.faceOrder[source->authoredFaceIndices[search.face]];
                plan.preciseFaceTexcoords[original][plan.sourceCorner(original,search.corner)][search.component]=value;
                candidate.mdxRowSize=rowSize;candidate.mdxWeightOffset=weightOffset;
                candidate.mdxBoneIndexOffset=boneOffset;candidate.mdxTangentOffset=tangentOffset;
                existing=std::move(candidate);
                return true;
            };
            for(std::size_t face=0;face<existing.faces.size() && !accepted;++face) {
                if(!selected[face]) continue;
                for(std::size_t corner=1u;corner<3u && !accepted;++corner)
                    for(std::size_t component=0;component<2u && !accepted;++component) {
                        if(!(selected[face]&(1u<<component))) continue;
                        const double value=source->faceTexcoords64[face][corner][component];
                        const float rounded=static_cast<float>(value);
                        for(const float direction:{-std::numeric_limits<float>::infinity(),
                                                    std::numeric_limits<float>::infinity()}) {
                            if(rounded==0.0f && std::signbit(rounded)!=std::signbit(direction)) continue;
                            const float neighbor=std::nextafter(rounded,direction);
                            const double edge=std::isinf(neighbor)?double(rounded)+
                                (double(rounded)-std::nextafter(rounded,0.0f))*0.5:
                                (double(rounded)+neighbor)*0.5;
                            const double end=std::nextafter(edge,double(rounded));
                            Search search{face,corner,component,value,end,{}};
                            search.value=derivative(search,value);
                            if(same(search.value,derivative(search,end))) continue;
                            if(consider(search,end)) {accepted=true;break;}
                            searches.push_back(search);
                        }
                    }
            }
            // Search the derivative's component and normalization boundaries.
            // Near a singular UV, enumerating every intervening float can
            // take billions of steps.
            for(const auto& search:searches) {
                const auto boundary=[&](const auto& state) {
                    const auto initial=state(search.value);
                    if(initial==state(derivative(search,search.end))) return false;
                    double low=search.cursor,high=search.end;
                    while(std::nextafter(low,high)!=high) {
                        const double middle=low+(high-low)*0.5;
                        if(state(derivative(search,middle))==initial) low=middle;
                        else high=middle;
                    }
                    return consider(search,low) || consider(search,high);
                };
                for(std::size_t axis=0;axis<3u && !accepted;++axis) {
                    const auto component=[&](Vec3 value) {return axis==0u?value.x:axis==1u?value.y:value.z;};
                    accepted=boundary(component) || boundary([&](Vec3 value) {
                        const auto v=component(value);return v<0.0f?-1:v>0.0f?1:0;
                    });
                }
                if(!accepted) accepted=boundary([](Vec3 value) {
                    return double(std::sqrt((value.x*value.x+value.y*value.y)+value.z*value.z))<1e-9;
                });
                if(accepted) break;
            }
            if(!accepted) break;
        }
    }
}

} // namespace

void reconstructSkinSeamSmoothing(Writer& out,const Model& model) {
    if(std::all_of(model.meshes.begin(),model.meshes.end(),
        [&](const Mesh& mesh) {return out.explicitNormals(mesh);})) return;
    if(std::none_of(model.meshes.begin(),model.meshes.end(),[](const Mesh& mesh){return mesh.skinned;})) {
        if(!out.refineTangentRounding() && !out.refineNormalRounding() &&
           std::none_of(model.meshes.begin(),model.meshes.end(),tangentPrecisionMesh) &&
           std::none_of(model.meshes.begin(),model.meshes.end(),normalPrecisionMesh)) return;
        for(const auto& mesh:model.meshes) {
            const auto& node=model.nodes[mesh.nodeIndex];
            if(!node.lightsaber) out.meshReconstructions.emplace(&mesh,reconstructMesh(out,node,mesh));
        }
        auto frames=model;
        frames.animations.clear();
        reconstructTangentRounding(out,model,frames,out.meshReconstructions);
        reconstructNormalRoundingPass(out,model,frames,out.meshReconstructions);
        reconstructTangentUvPrecision(out,model,frames,out.meshReconstructions);
        reconstructNormalPositionPrecision(out,model,frames,out.meshReconstructions);
        return;
    }
    using Plans=std::map<const Mesh*,MeshReconstruction>;
    const auto reconstruct=[&](bool refine) {
        Plans result;
        for(const auto& mesh:model.meshes) {
            const auto& node=model.nodes[mesh.nodeIndex];
            if(!node.lightsaber) result.emplace(&mesh,reconstructMesh(out,node,mesh,refine,false));
        }
        return result;
    };
    auto& plans=out.meshReconstructions;
    plans=reconstruct(false);
    auto frames=model;
    frames.animations.clear();
    if(std::none_of(plans.begin(),plans.end(),[](const auto& entry) {
        return entry.second.hasNormals && !entry.second.explicitNormals && !entry.first->animated;
    })) {
        return;
    }
    enum class PrecursorFit { Smooth,SplitOpposing,Unscored,RefineExisting,RetainedVertices };
    const auto precursor=[&](const Plans& source,bool refine,PrecursorFit fit=PrecursorFit::Smooth) {
        // Saved normals can be outputs of ProcessSkinSeams. Reconstruct
        // geometric precursors only where that producer changes a corner.
        auto smooth=frames;
        auto rows=generateGeometry(smooth,model,source,true,out.job());
        processAsciiSkinSeams(smooth,out.job());
        if(fit==PrecursorFit::SplitOpposing) {
            auto flatPlans=source,hybrid=source;
            for(auto& entry:flatPlans) {
                const auto& mesh=*entry.first;
                auto& plan=entry.second;
                if(mesh.animated || !plan.hasNormals || plan.explicitNormals || mesh.asciiNormalPositionSource) continue;
                plan.smoothing.masks.assign(mesh.faces.size(),0u);
                hybrid.at(&mesh).smoothing.masks.assign(mesh.faces.size(),1u);
            }
            auto flat=frames;
            const auto flatRows=generateGeometry(flat,model,flatPlans,false,out.job());
            bool split=false;
            for(std::size_t index=0;index<model.meshes.size();++index) {
                const auto& mesh=model.meshes[index];
                const auto found=hybrid.find(&mesh);
                if(found==hybrid.end() || mesh.animated || !found->second.hasNormals || found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
                for(std::size_t face=0;face<mesh.faces.size();++face)
                    for(std::size_t corner=0;corner<3u;++corner) {
                        out.job().check();
                        const auto row=rows[index][face][corner];
                        const auto before=smooth.meshes[index].vertices[row].normal;
                        if(same(before,smooth.meshes[index].normalAt(row))) continue;
                        const auto normal=flat.meshes[index].vertices[flatRows[index][face][corner]].normal;
                        if((normal.x*before.x+normal.y*before.y)+normal.z*before.z<0.0f) {
                            found->second.smoothing.masks[face]=0u;
                            split=true;
                        }
                    }
            }
            if(!split) return source;
            // Opposing faces can require separate normals before a seam.
            // This supplies targets only; nonseam corners still constrain
            // the final masks below, including other joins on these faces.
            smooth=frames;
            rows=generateGeometry(smooth,model,hybrid,false,out.job());
            processAsciiSkinSeams(smooth,out.job());
        }
        auto candidate=source;
        for(std::size_t index=0;index<model.meshes.size();++index) {
            const auto& mesh=model.meshes[index];
            const auto found=candidate.find(&mesh);
            if(found==candidate.end() || mesh.animated || !found->second.hasNormals || found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
            auto& plan=found->second;
            std::vector<Vec3> targets;
            std::vector<bool> ignored;
            bool adjusted=false;
            for(std::size_t face=0;face<mesh.faces.size();++face)
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto row=rows[index][face][corner];
                    const auto before=smooth.meshes[index].vertices[row].normal;
                    const auto after=smooth.meshes[index].normalAt(row);
                    const bool seam=!same(before,after);
                    targets.push_back(seam?before:plan.vertices[mesh.faces[face].indices[corner]].normal);
                    ignored.push_back(seam);
                    adjusted|=seam;
                }
            if(adjusted) {
                const auto& topology=plan.topology;
                const bool ignoreSeams=fit==PrecursorFit::Unscored || fit==PrecursorFit::RefineExisting ||
                    fit==PrecursorFit::RetainedVertices;
                plan.smoothing=reconstructSmoothing(mesh,plan.vertices,topology.sourceVertices,
                    topology.sourceFaces,out.job(),&targets,refine,ignoreSeams?&ignored:nullptr,
                    fit==PrecursorFit::RefineExisting || fit==PrecursorFit::RetainedVertices?
                        &plan.smoothing.masks:nullptr,
                    fit==PrecursorFit::RetainedVertices,nullptr,&plan.cornerRotations);
            }
        }
        return candidate;
    };
    // A closer per-mesh fit can be worse after seams, including through
    // changed vertex sharing. Retain both searches until after that pass.
    std::vector<Plans> alternatives;
    alternatives.push_back(plans);
    alternatives.push_back(precursor(plans,false));
    alternatives.push_back(reconstruct(true));
    alternatives.push_back(precursor(alternatives.back(),true));
    alternatives.push_back(precursor(alternatives[2],true,PrecursorFit::SplitOpposing));
    // Seam outputs do not uniquely determine their geometric inputs.
    // Also fit the unaffected corners without imposing guessed inputs;
    // the complete seam-output score below still judges every corner.
    alternatives.push_back(precursor(alternatives[2],true,PrecursorFit::Unscored));
    alternatives.push_back(precursor(alternatives[2],true,PrecursorFit::RefineExisting));
    alternatives.push_back(precursor(alternatives[2],true,PrecursorFit::RetainedVertices));
    auto baseline=frames;
    const auto baselineRows=generateGeometry(baseline,model,plans,false,out.job());
    processAsciiSkinSeams(baseline,out.job());
    auto best=measureSeamNormals(baseline,model,baselineRows,plans);
    const auto consider=[&](Plans trial) {
        auto rebuilt=frames;
        const auto rows=generateGeometry(rebuilt,model,trial,false,out.job());
        processAsciiSkinSeams(rebuilt,out.job());
        // NodeVertex equality excludes skin influences. Changed smoothing
        // may create new welds, which must still retain every corner's weights.
        if(!preservesSkinChannels(model,rows)) return;
        const auto error=measureSeamNormals(rebuilt,model,rows,trial);
        if(error<best) {best=error;plans=std::move(trial);}
    };
    for(std::size_t i=1u;i<alternatives.size();++i) consider(alternatives[i]);
    // Precursor fits can help one mesh and hurt another. Select them
    // individually, scoring the complete ordered seam pass each time.
    double previous;
    do {
        previous=best;
        consider(precursor(plans,true,PrecursorFit::RefineExisting));
        for(const auto& mesh:model.meshes)
            for(const auto& alternative:alternatives) {
                const auto found=alternative.find(&mesh);
                if(found==alternative.end()) continue;
                const auto& current=plans.at(&mesh);
                const auto& candidate=found->second;
                if(current.smoothing.masks==candidate.smoothing.masks &&
                   current.topology.sourceVertices==candidate.topology.sourceVertices &&
                   current.topology.sourceFaces==candidate.topology.sourceFaces) continue;
                auto trial=plans;
                trial.at(&mesh)=candidate;
                consider(std::move(trial));
            }
    } while(best<previous);

    // Retained tangent channels precede ProcessSkinSeams and disambiguate
    // smoothing inputs whose final ordinary normals became equal. The local
    // fit proposes intersections; the actual producers judge their effects.
    double bestTangent{};
    const auto refineTangents=[&] {
        bool refined=false;
        bool tangentChanged=std::any_of(plans.begin(),plans.end(),[](const auto& entry) {
            const auto& mesh=*entry.first;
            return (mesh.mdxBitmap&0x80u) && !mesh.animated && !mesh.dangly && !entry.second.explicitNormals;
        });
        while(tangentChanged) {
            tangentChanged=false;
            // A corner-local proposal can split a row before its last writer
            // becomes independently selectable in the next generated mapping.
            for(const bool useGeneratedWriters:{false,true}) {
                auto current=frames;
                const auto currentRows=generateGeometry(current,model,plans,false,out.job());
                bestTangent=measureTangentBasis(current,model,currentRows,plans,out.job());
                auto candidate=plans;
                bool proposed=false;
                for(std::size_t index=0;index<model.meshes.size();++index) {
                    const auto& mesh=model.meshes[index];
                    const auto found=candidate.find(&mesh);
                    if(!(mesh.mdxBitmap&0x80u) || mesh.animated || mesh.dangly || found==candidate.end() ||
                       found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
                    auto& plan=found->second;
                    const TangentSmoothingSource tangents{
                        current.meshes[index].asciiTangentSource->firstCornerNormals,currentRows[index],useGeneratedWriters};
                    const auto smoothing=reconstructSmoothing(mesh,plan.vertices,plan.topology.sourceVertices,
                        plan.topology.sourceFaces,out.job(),nullptr,true,nullptr,&plan.smoothing.masks,false,
                        &tangents,&plan.cornerRotations);
                    proposed|=smoothing.masks!=plan.smoothing.masks;
                    plan.smoothing=smoothing;
                }
                if(!proposed) continue;
                const auto considerTangents=[&](Plans trial) {
                    auto rebuilt=frames;
                    const auto rows=generateGeometry(rebuilt,model,trial,false,out.job());
                    const auto tangentError=measureTangentBasis(rebuilt,model,rows,trial,out.job());
                    processAsciiSkinSeams(rebuilt,out.job());
                    if(!preservesSkinChannels(model,rows)) return;
                    const auto normalError=measureSeamNormals(rebuilt,model,rows,trial);
                    if(normalError<=best && tangentError<=bestTangent &&
                       (normalError<best || tangentError<bestTangent)) {
                        best=normalError;bestTangent=tangentError;plans=std::move(trial);tangentChanged=true;refined=true;
                    }
                };
                considerTangents(candidate);
                for(const auto& entry:candidate)
                    if(entry.second.smoothing.masks!=plans.at(entry.first).smoothing.masks) {
                        auto trial=plans;
                        trial.at(entry.first)=entry.second;
                        considerTangents(std::move(trial));
                    }
            }
        }
        return refined;
    };
    refineTangents();
    for(;;) {
        reconstructCyclicCorners(out,model,frames,plans);
        auto current=frames;
        auto rows=generateGeometry(current,model,plans,false,out.job());
        bestTangent=measureTangentBasis(current,model,rows,plans,out.job());
        processAsciiSkinSeams(current,out.job());
        best=measureSeamNormals(current,model,rows,plans);
        bool refitted=false;
        const auto considerRefit=[&](Plans trial) {
            Plans changed;
            for(const auto& entry:trial)
                if(entry.second.smoothing.masks!=plans.at(entry.first).smoothing.masks ||
                   entry.second.cornerRotations!=plans.at(entry.first).cornerRotations)
                    changed.emplace(entry);
            auto rebuilt=current;
            auto generated=rows;
            for(const auto& entry:changed) {
                const auto index=static_cast<std::size_t>(entry.first-model.meshes.data());
                rebuilt.meshes[index]=frames.meshes[index];
            }
            const auto changedRows=generateGeometry(rebuilt,model,changed,false,out.job());
            for(const auto& entry:changed) {
                const auto index=static_cast<std::size_t>(entry.first-model.meshes.data());
                generated[index]=changedRows[index];
            }
            if(!preservesSkinChannels(model,generated)) return;
            const auto tangentError=measureTangentBasis(rebuilt,model,generated,trial,out.job(),&changed);
            if(std::any_of(changed.begin(),changed.end(),[](const auto& entry){return entry.first->render;}))
                processAsciiSkinSeams(rebuilt,out.job());
            else
                for(std::size_t index=0;index<model.meshes.size();++index)
                    if(model.meshes[index].render)
                        rebuilt.meshes[index].asciiNormalPool=current.meshes[index].asciiNormalPool;
            const auto normalError=measureSeamNormals(rebuilt,model,generated,trial);
            if(normalError<=best && tangentError<=bestTangent &&
               (normalError<best || tangentError<bestTangent)) {
                best=normalError;bestTangent=tangentError;
                plans=std::move(trial);refitted=true;
                current=std::move(rebuilt);rows=std::move(generated);
            }
        };
        // Cyclic starts change the rounded contributions used to fit masks.
        // Refit those relationships in the emitted corner order, then let
        // the complete normal and tangent producers judge each proposal.
        for(const auto fit:{PrecursorFit::RefineExisting,PrecursorFit::RetainedVertices}) {
            const auto candidate=precursor(plans,true,fit);
            considerRefit(candidate);
            for(const auto& entry:candidate)
                if(entry.second.smoothing.masks!=plans.at(entry.first).smoothing.masks) {
                    auto trial=plans;
                    trial.at(entry.first)=entry.second;
                    considerRefit(std::move(trial));
                }
        }
        const auto refineSeamJoins=[&](bool differentNormals) {
            auto seams=frames;
            const auto seamRows=generateGeometry(seams,model,plans,false,out.job());
            processAsciiSkinSeams(seams,out.job());
            struct Join {const Mesh* mesh;Vec3 point;std::vector<std::uint32_t> masks;};
            std::vector<Join> joins;
            const auto apply=[](Plans& trial,const Join& join) {
                auto& masks=trial.at(join.mesh).smoothing.masks;
                for(std::size_t face=0;face<masks.size();++face) masks[face]|=join.masks[face];
            };
            for(std::size_t index=0;index<model.meshes.size();++index) {
                const auto& mesh=model.meshes[index];
                const auto found=plans.find(&mesh);
                if(found==plans.end() || mesh.animated || !found->second.hasNormals ||
                   found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
                std::vector<bool> seamCorners;
                for(const auto& face:seamRows[index])
                    for(const auto row:face)
                        seamCorners.push_back(!same(seams.meshes[index].vertices[row].normal,
                            seams.meshes[index].normalAt(row)));
                // A local fit cannot judge a precursor overwritten by seams.
                // Propose its shared-normal joins directly to the ordered pass.
                const auto candidates=sharedSeamSmoothingCandidates(mesh,found->second.vertices,
                    found->second.topology.sourceFaces,found->second.smoothing.masks,seamCorners,out.job(),
                    differentNormals);
                if(differentNormals) {
                    // Coupled joins recover missing vertex sharing across meshes.
                    std::vector<std::set<std::size_t>> retained(found->second.sourcePositions.size()),
                        generated(retained.size());
                    for(std::size_t face=0;face<mesh.faces.size();++face)
                        for(std::size_t corner=0;corner<3u;++corner) {
                            const auto source=found->second.topology.sourceFaces[face][corner];
                            retained[source].insert(mesh.faces[face].indices[corner]);
                            generated[source].insert(seamRows[index][face][corner]);
                        }
                    Vec3 position{};
                    Quaternion orientation{};
                    auto ancestor=std::optional<std::size_t>{mesh.nodeIndex};
                    while(ancestor) {
                        const auto& node=frames.nodes[*ancestor];
                        const auto rotated=ascii::engineModelRotateVector(node.orientation,position);
                        position={rotated.x+node.position.x,rotated.y+node.position.y,rotated.z+node.position.z};
                        orientation=ascii::engineModelQuaternionProduct(node.orientation,orientation);
                        ancestor=node.parent;
                    }
                    auto coupled=sharedSeamSmoothingCandidates(mesh,found->second.vertices,
                        found->second.topology.sourceFaces,found->second.smoothing.masks,seamCorners,out.job());
                    coupled.insert(coupled.end(),candidates.begin(),candidates.end());
                    for(const auto& candidate:coupled)
                        if(generated[candidate.source].size()>retained[candidate.source].size()) {
                            const auto point=ascii::engineModelRotateVector(orientation,
                                found->second.sourcePositions[candidate.source]);
                            joins.push_back({&mesh,{point.x+position.x,point.y+position.y,point.z+position.z},candidate.masks});
                        }
                }
                std::set<std::vector<std::uint32_t>> masks;
                for(const auto& candidate:candidates) masks.insert(candidate.masks);
                for(const auto& candidate:masks) {
                    auto trial=plans;
                    trial.at(&mesh).smoothing.masks=candidate;
                    considerRefit(std::move(trial));
                }
            }
            // Both sides of an ordered skin seam can need a join together;
            // either change alone may increase the final normal error.
            std::stable_sort(joins.begin(),joins.end(),[](const Join& a,const Join& b){return a.point.x<b.point.x;});
            constexpr float tolerance=0.0010000000474974513f;
            for(std::size_t a=0;a<joins.size();++a)
                for(std::size_t b=a+1u;b<joins.size() && joins[b].point.x-joins[a].point.x<tolerance;++b) {
                    const auto& first=joins[a];const auto& second=joins[b];
                    if(first.mesh==second.mesh || !(std::abs(first.point.y-second.point.y)<tolerance) ||
                       !(std::abs(first.point.z-second.point.z)<tolerance)) continue;
                    auto trial=plans;
                    apply(trial,first);apply(trial,second);
                    considerRefit(std::move(trial));
                }
        };
        refineSeamJoins(false);
        const bool tangentRefitted=refineTangents();
        if(!refitted && !tangentRefitted) {
            refineSeamJoins(true);
            if(refitted) continue;
            // A shared contributor set can require different cyclic starts.
            // Judge that combined reconstruction before rejecting the join.
            for(const auto& mesh:model.meshes) {
                const auto found=plans.find(&mesh);
                if(found==plans.end() || mesh.animated || !found->second.hasNormals ||
                   found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
                const auto& plan=found->second;
                std::vector<std::vector<std::uint32_t>> candidates;
                reconstructSmoothing(mesh,plan.vertices,plan.topology.sourceVertices,
                    plan.topology.sourceFaces,out.job(),nullptr,false,nullptr,&plan.smoothing.masks,
                    false,nullptr,&plan.cornerRotations,&candidates);
                // A shared-row move can need neighboring non-seam joins refitted
                // before its complete seam output improves.
                const auto index=static_cast<std::size_t>(&mesh-model.meshes.data());
                std::vector<bool> seamCorners;
                for(const auto& face:rows[index])
                    for(const auto row:face)
                        seamCorners.push_back(!same(current.meshes[index].vertices[row].normal,
                            current.meshes[index].normalAt(row)));
                const auto direct=candidates.size();
                for(std::size_t i=0;i<direct;++i) {
                    auto refined=reconstructSmoothing(mesh,plan.vertices,plan.topology.sourceVertices,
                        plan.topology.sourceFaces,out.job(),nullptr,true,&seamCorners,&candidates[i],
                        false,nullptr,&plan.cornerRotations);
                    if(std::find(candidates.begin(),candidates.end(),refined.masks)==candidates.end())
                        candidates.push_back(std::move(refined.masks));
                }
                for(const auto& masks:candidates) {
                    auto trial=plans;
                    trial.at(&mesh).smoothing.masks=masks;
                    reconstructCyclicCorners(out,model,frames,trial,&mesh);
                    considerRefit(std::move(trial));
                }
            }
            if(!refitted) break;
        }
    }
    // Keep the additional cyclic candidates out of private smoothing-join
    // trials; refine the accepted reconstruction once those reach a fixed point.
    reconstructCyclicCornerPass(out,model,frames,plans,nullptr,CornerSearch::Cancellation,true);
    // Recover differing tangent axes after the original cyclic search.
    if(std::any_of(plans.begin(),plans.end(),[](const auto& entry) {
        const auto& mesh=*entry.first;
        return (mesh.mdxBitmap&0x80u) && !mesh.animated && !mesh.dangly &&
            entry.second.hasNormals && !entry.second.explicitNormals;
    })) reconstructCyclicCornerPass(out,model,frames,plans,nullptr,CornerSearch::TangentAxes,true);
    // Refine normal accumulation order while retaining neighbors, basis
    // corners and skin records.
    reconstructFaceOrder(out,model,frames,plans);
    reconstructTangentRounding(out,model,frames,plans);
    reconstructNormalRoundingPass(out,model,frames,plans);
    reconstructTangentUvPrecision(out,model,frames,plans);
    // Resolve UV precision before ordering the remaining cancelled tangents.
    reconstructCyclicCornerPass(out,model,frames,plans,nullptr,CornerSearch::TangentOrder,false);
    reconstructNormalPositionPrecision(out,model,frames,plans);
}

ReconstructedMesh writeMesh(Writer& out,const Model& model,const Node& node,const Mesh& mesh) {
    materialFields(out,model,node,mesh);
    if(mesh.engineBounds) {
        out.line("spherecenter "+out.vector(mesh.engineBounds->sphereCenter));
        out.line("sphereradius "+out.scalar(mesh.engineBounds->sphereRadius));
    }
    if(node.lightsaber) {
        writeLightsaber(out,node,out.explicitNormals(mesh));
        return {};
    }
    const auto cached=out.meshReconstructions.find(&mesh);
    auto reconstruction=cached==out.meshReconstructions.end()?reconstructMesh(out,node,mesh):
        std::move(cached->second);
    const auto& vertices=reconstruction.vertices;
    auto& result=reconstruction.topology;
    const auto& reconstructed=reconstruction.smoothing;
    const auto* animation=node.animmesh?&*node.animmesh:nullptr;
    const bool hasNormals=reconstruction.hasNormals;
    const bool hasColors=mesh.hasVertexColors || (mesh.enginePrelightInput && (mesh.mdxBitmap&0x40u)!=0u);
    if(reconstruction.changedAdjacency)
        out.warning("Mesh "+node.name+": source edge connectivity cannot reproduce "+
            std::to_string(reconstruction.changedAdjacency)+" stored face-neighbour entries");
    const auto& smoothing=reconstructed.masks;
    const auto precise=preciseUvPool(mesh,reconstruction);
    if(reconstructed.changedCornerNormals && !reconstruction.explicitNormals)
        out.warning("Mesh "+node.name+": reconstructed smoothing changes "+
            std::to_string(reconstructed.changedCornerNormals)+" corner normals; maximum component difference "+
            out.scalar(reconstructed.maxNormalComponentDifference));
    if(!std::is_sorted(reconstruction.faceOrder.begin(),reconstruction.faceOrder.end()))
        out.warning("Mesh "+node.name+": source face order changes during smoothing reconstruction");
    std::size_t adjusted{};
    float displacement{};
    for(std::size_t source=0;source<result.sourceVertices.size();++source) {
        const auto original=vertices[result.sourceVertices[source]].position;
        const auto position=reconstruction.sourcePositions[source];
        adjusted+=!same(original,position);
        displacement=std::max({displacement,std::abs(original.x-position.x),
            std::abs(original.y-position.y),std::abs(original.z-position.z)});
    }
    if(adjusted) out.warning("Mesh "+node.name+": rounding refinement adjusts "+
        std::to_string(adjusted)+" source positions to neighboring float values; maximum coordinate change "+
        out.scalar(displacement));
    if(!hasNormals && !vertices.empty())
        out.warning("Mesh "+node.name+": no compiled normal channel exists; source connectivity is retained and face normals are generated");
    diagnoseTopology(out,node,mesh);
    if(!reconstruction.preciseSourcePositions.empty()) out.line("normalpositionprecision 64");
    out.line("verts "+std::to_string(result.sourceVertices.size()));
    for(std::size_t source=0;source<reconstruction.sourcePositions.size();++source) {
        if(reconstruction.preciseSourcePositions.empty()) out.line(out.vector(reconstruction.sourcePositions[source]));
        else {
            const auto& value=reconstruction.preciseSourcePositions[source];
            out.line(out.scalarDouble(value[0])+" "+out.scalarDouble(value[1])+" "+out.scalarDouble(value[2]));
        }
    }
    if(reconstruction.explicitNormals) {
        out.explicitNormalNote();
        out.line("normals "+std::to_string(vertices.size()));
        for(const auto& vertex:vertices) out.line(out.vector(vertex.normal));
        out.line("normalindices "+std::to_string(mesh.faces.size()));
        for(const auto i:reconstruction.faceOrder) {
            const auto& face=mesh.faces[i];
            out.line(std::to_string(static_cast<std::uint32_t>(face.indices[reconstruction.sourceCorner(i,0u)]))+" "+
                std::to_string(static_cast<std::uint32_t>(face.indices[reconstruction.sourceCorner(i,1u)]))+" "+
                std::to_string(static_cast<std::uint32_t>(face.indices[reconstruction.sourceCorner(i,2u)])));
        }
    }
    if(animation && hasNormals) {
        // NW ProcessVerts uses raw vertex indices for a supplied normal list.
        out.line("normals "+std::to_string(result.sourceVertices.size()));
        for(const auto index:result.sourceVertices) out.line(out.vector(vertices[index].normal));
    }
    if(animation && !animation->staticTangents.empty()) {
        out.line("tangents "+std::to_string(result.sourceVertices.size()));
        for(const auto index:result.sourceVertices) {
            const auto& value=animation->staticTangents.at(index);
            out.line(out.scalar(value[0])+" "+out.scalar(value[1])+" "+
                     out.scalar(value[2])+" "+out.scalar(value[3]));
        }
    }
    for(std::size_t channel=0;channel<4u;++channel) {
        if((mesh.mdxBitmap&(2u<<channel))==0u) continue;
        const auto count=channel==0u && !precise.values.empty()?precise.values.size():vertices.size();
        if(channel==0u && !precise.values.empty()) out.line("tangentuvprecision 64");
        out.line(std::string("tverts")+(channel?std::to_string(channel):std::string{})+" "+std::to_string(count));
        for(std::size_t i=0;i<count;++i) {
            if(channel==0u && !precise.values.empty()) {
                const auto& value=precise.values[i];
                out.line(out.scalarDouble(value[0])+" "+out.scalarDouble(value[1])+" 0");
            } else {
                const auto value=uv(vertices[i],channel);
                out.line(out.scalar(value.x)+" "+out.scalar(value.y)+" 0");
            }
        }
        if(channel) {
            out.line("texindices"+std::to_string(channel)+" "+std::to_string(mesh.faces.size()));
            for(const auto i:reconstruction.faceOrder) {
                const auto& face=mesh.faces[i];
                out.line(std::to_string(static_cast<std::uint32_t>(face.indices[reconstruction.sourceCorner(i,0u)]))+" "+
                    std::to_string(static_cast<std::uint32_t>(face.indices[reconstruction.sourceCorner(i,1u)]))+" "+
                    std::to_string(static_cast<std::uint32_t>(face.indices[reconstruction.sourceCorner(i,2u)])));
            }
        }
    }
    if(animation) {
        out.line("sampleperiod "+out.scalar(animation->samplePeriod));
        const auto positions=static_cast<std::size_t>(animation->positionSampleCount);
        out.line("animverts "+std::to_string(result.sourceVertices.size()*positions));
        for(std::size_t frame=0;frame<positions;++frame)
            for(const auto index:result.sourceVertices)
                out.line(out.vector(animation->positionSamples.at(index*positions+frame)));
        const auto texcoords=static_cast<std::size_t>(animation->texcoordSampleCount);
        out.line("animtverts "+std::to_string(vertices.size()*texcoords));
        for(std::size_t frame=0;frame<texcoords;++frame)
            for(std::size_t index=0;index<vertices.size();++index) {
                const auto value=animation->texcoordSamples.at(index*texcoords+frame);
                out.line(out.scalar(value.x)+" "+out.scalar(value.y)+" 0");
            }
        out.warning("Animmesh "+node.name+": frame-major authoring arrays are generated from the final vertex-major sample pools; original third UV components, face mirror values and smoothing groups are unavailable, so regenerated animated normals and vertex merging can differ");
    }
    if(!textureName(mesh,1u).empty() && (mesh.mdxBitmap&4u))
        out.warning("Mesh "+node.name+": secondary UVs contain any compiled lightmap transform; original .lmt data is unavailable and an active .lmt sidecar would be applied again during ASCII load");
    const auto cornerIndices=[&](const std::string& field) {
        out.line(field+" "+std::to_string(mesh.faces.size()));
        for(const auto i:reconstruction.faceOrder) {
            const auto& face=mesh.faces[i];
            out.line(std::to_string(face.indices[reconstruction.sourceCorner(i,0u)])+" "+
                std::to_string(face.indices[reconstruction.sourceCorner(i,1u)])+" "+
                std::to_string(face.indices[reconstruction.sourceCorner(i,2u)]));
        }
    };
    if(hasColors) {
        out.line("colorsrgba "+std::to_string(vertices.size()));
        for(const auto& vertex:vertices) {
            const auto& c=vertex.color;
            out.line(std::to_string(c[0])+" "+std::to_string(c[1])+" "+
                std::to_string(c[2])+" "+std::to_string(c[3]));
        }
        cornerIndices("colorindices");
    }
    out.line("hastangentbasis "+std::to_string((mesh.mdxBitmap&0x80u)!=0u));
    if(mesh.mdxBitmap&0x80u) {
        out.line("tangentbasis "+std::to_string(vertices.size()));
        for(const auto& vertex:vertices) {
            const auto& basis=vertex.tangentBasis;
            out.line(out.vector(basis.bitangent)+" "+out.vector(basis.tangent)+" "+out.vector(basis.normal));
        }
        cornerIndices("tangentindices");
    }
    if(mesh.cpuPositions && (adjusted || !std::equal(mesh.cpuPositions->begin(),mesh.cpuPositions->end(),vertices.begin(),
        [](Vec3 position,const Vertex& vertex) {return same(position,vertex.position);}))) {
        out.line("cpuverts "+std::to_string(mesh.cpuPositions->size()));
        for(const auto value:*mesh.cpuPositions) out.line(out.vector(value));
        cornerIndices("cpuindices");
    }
    out.line("faceplanes "+std::to_string(mesh.faces.size()));
    for(const auto i:reconstruction.faceOrder) {
        const auto& face=mesh.faces[i];
        out.line(out.vector(face.normal)+" "+out.scalar(face.planeDistance));
    }
    out.line("faces "+std::to_string(mesh.faces.size()));
    for(const auto i:reconstruction.faceOrder) {
        const auto& sourceFace=result.sourceFaces[i];
        const auto& compiled=mesh.faces[i];
        const auto a=reconstruction.sourceCorner(i,0u),b=reconstruction.sourceCorner(i,1u),
                   c=reconstruction.sourceCorner(i,2u);
        const auto texcoord=[&](std::size_t corner) {
            return precise.values.empty()?static_cast<std::uint32_t>(compiled.indices[corner]):precise.indices[i][corner];
        };
        out.line(std::to_string(sourceFace[a])+" "+std::to_string(sourceFace[b])+" "+std::to_string(sourceFace[c])+" "+
            std::to_string(static_cast<std::int32_t>(smoothing[i]))+" "+
            std::to_string(texcoord(a))+" "+std::to_string(texcoord(b))+" "+std::to_string(texcoord(c))+" "+
            std::to_string(static_cast<std::int32_t>(compiled.surfaceMaterial)));
    }
    if(mesh.dangly) {
        const auto& dangly=*mesh.dangly;
        out.line("displacement "+out.scalar(dangly.displacement));
        out.line("tightness "+out.scalar(dangly.tightness));
        out.line("period "+out.scalar(dangly.period));
        bool different=false;
        if(vertices.empty() && mesh.faces.empty()) {
            // The engine no-face producer retains the authored constraint
            // pool independently of its empty generated vertex/rest pools.
            out.line("constraints "+std::to_string(dangly.constraints.size()));
            for(const auto constraint:dangly.constraints)
                out.line(out.scalar(constraint));
        } else {
            out.line("constraints "+std::to_string(result.sourceVertices.size()));
            for(const auto index:result.sourceVertices) {
                out.line(out.scalar(dangly.constraints.at(index)));
                if(dangly.restPositions) different|=!same(dangly.restPositions->at(index),vertices[index].position);
            }
        }
        if(different)
            out.warning("Mesh "+node.name+": compiled dangly rest positions differ from its drawing positions; the ASCII builder derives its rest pool from verts");
    }
    return std::move(result);
}

} // namespace neomdl::model::ascii_export
