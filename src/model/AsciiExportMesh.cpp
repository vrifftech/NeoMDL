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
    if(mesh.uvAnimation.enabledRaw) {
        out.line("uvdirectionx "+out.scalar(mesh.uvAnimation.direction.x));
        out.line("uvdirectiony "+out.scalar(mesh.uvAnimation.direction.y));
        out.line("uvjitter "+out.scalar(mesh.uvAnimation.jitter));
        out.line("uvjitterspeed "+out.scalar(mesh.uvAnimation.jitterSpeed));
    }
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
                                   bool refine=true,bool finalNormals=true,bool reconstructNormals=false) {
    auto vertices=sourceVertices(mesh,node,out);
    const auto* animation=node.animmesh?&*node.animmesh:nullptr;
    if(animation && (mesh.mdxBitmap&2u)==0u)
        throw ModelError("Cannot export animmesh "+node.name+": its static UV source pool is unavailable; engine authoring requires tverts even when animtverts is empty");
    if(mesh.skinned && !mesh.engineCpuSkin && !mesh.builtFloatSkinChannels)
        throw ModelError("Cannot export skin mesh "+node.name+": engine float weight/index channels are unavailable");
    auto skin=mesh.skinned?decodeEngineFloatSkinChannels(mesh):std::vector<EngineFloatSkinChannels>{};
    const bool hasNormals=!mesh.enginePrelightInput || (mesh.mdxBitmap&0x20u)!=0u;
    const bool explicitNormals=!animation && hasNormals && out.explicitNormals(mesh) && !reconstructNormals;
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
        result.vertexData=std::make_shared<const std::vector<Vertex>>(std::move(vertices));
        result.skinChannels=std::make_shared<const std::vector<EngineFloatSkinChannels>>(std::move(skin));
        result.hasNormals=hasNormals;result.explicitNormals=explicitNormals;
        for(auto& source:sources) {
            const auto row=source.second;
            source.second=result.topology.sourceVertices.size();
            result.topology.sourceVertices.push_back(row);
            const auto p=result.vertices()[row].position;
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
            candidate=out.smoothing(mesh,vertices,topology.sourceVertices,topology.sourceFaces,out.job(),nullptr,refine);
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
    return {std::make_shared<const std::vector<Vertex>>(std::move(vertices)),{std::move(topology.sourceVertices),std::move(topology.sourceFaces)},
        std::move(reconstructed),topology.changedAdjacency,hasNormals,explicitNormals,
        std::vector<std::uint8_t>(mesh.faces.size(),0u),std::move(faceOrder),std::move(positions),
        mesh.asciiTangentSource?mesh.asciiTangentSource->faceTexcoords64:
            std::vector<std::array<std::array<double,2>,3>>{}, {},
        std::make_shared<const std::vector<EngineFloatSkinChannels>>(std::move(skin))};
}

using CornerRows=std::vector<std::vector<std::array<std::int16_t,3>>>;

struct PreciseUvPool {
    std::vector<std::array<double,2>> values;
    std::vector<std::array<std::uint32_t,3>> indices;
};

PreciseUvPool preciseUvPool(const Mesh& mesh,const MeshReconstruction& plan) {
    PreciseUvPool result;
    if(plan.preciseFaceTexcoords.empty()) return result;
    for(const auto& vertex:plan.vertices())
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
                           bool smooth,bool advanced,const neoshared::bif::JobControl& job,
                           bool resetSeams=true) {
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
        raw.vertices.reserve(plan.sourcePositions.size());
        raw.faces.reserve(plan.faceOrder.size());
        raw.normals.reserve(plan.vertices().size());
        raw.skinChannels.reserve(plan.topology.sourceVertices.size());
        for(std::size_t channel=0;channel<4u;++channel)
            if(mesh.mdxBitmap&(2u<<channel)) raw.texcoords[channel].reserve(plan.vertices().size());
        rebuilt.meshes[index].explicitNormals=plan.explicitNormals;
        raw.node=source.nodes[mesh.nodeIndex];
        if(!plan.preciseSourcePositions.empty()) {
            raw.normalPositionPrecision=64u;
            raw.normalPositions64=plan.preciseSourcePositions;
        }
        const auto* animation=raw.node.animmesh?&*raw.node.animmesh:nullptr;
        const bool colors=mesh.hasVertexColors || (mesh.enginePrelightInput && (mesh.mdxBitmap&0x40u));
        const auto& skin=*plan.skinChannels;
        if(mesh.skinned) raw.skinData.emplace();
        if(advanced && mesh.cpuPositions) raw.cpuPositions=*mesh.cpuPositions;
        if(advanced && (mesh.mdxBitmap&0x80u))
            for(const auto& vertex:plan.vertices()) raw.tangentBases.push_back(vertex.tangentBasis);
        for(std::size_t source=0;source<plan.topology.sourceVertices.size();++source) {
            const auto vertex=plan.topology.sourceVertices[source];
            const auto& value=plan.vertices()[vertex];
            raw.vertices.push_back(plan.sourcePositions[source]);
            if(animation && plan.hasNormals) raw.normals.push_back(value.normal);
            if(colors) {
                if(advanced) raw.packedColors.push_back(value.color);
                else raw.colors.push_back({float(value.color[0])/255.0f,
                    float(value.color[1])/255.0f,float(value.color[2])/255.0f});
            }
            if(mesh.skinned) raw.skinChannels.push_back(skin.at(vertex));
            if(mesh.dangly) raw.constraints.push_back(mesh.dangly->constraints.at(vertex));
            if(animation && !animation->staticTangents.empty())
                raw.animTangents.push_back(animation->staticTangents.at(vertex));
        }
        if(plan.explicitNormals)
            for(const auto& vertex:plan.vertices()) raw.normals.push_back(vertex.normal);
        for(std::size_t channel=0;channel<4u;++channel)
            if(mesh.mdxBitmap&(2u<<channel))
                for(const auto& vertex:plan.vertices()) {
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
            if(advanced && mesh.cpuPositions) raw.cpuIndices.push_back(value.texcoords[0]);
            if(advanced && (mesh.mdxBitmap&0x80u)) raw.tangentIndices.push_back(value.texcoords[0]);
            // Match the emitted texindices arrays: lightmap UVs keep their
            // original rows when primary-UV precision adds separate entries.
            for(std::size_t channel=1u;channel<4u;++channel)
                if(mesh.mdxBitmap&(2u<<channel)) {
                    const auto& indices=value.texcoords[channel];
                    raw.textureIndices[channel-1u].push_back({static_cast<float>(indices[0]),
                        static_cast<float>(indices[1]),static_cast<float>(indices[2])});
                }
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
                for(std::size_t vertex=0;vertex<plan.vertices().size();++vertex) {
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
    if(resetSeams) initializeAsciiSkinSeamPools(rebuilt);
    return rows;
}

bool preservesSkinChannels(const Model& model,const CornerRows& rows,
                           const std::map<const Mesh*,MeshReconstruction>& plans,const Mesh* changed=nullptr) {
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        if(!mesh.skinned || (changed && &mesh!=changed)) continue;
        const auto& channels=*plans.at(&mesh).skinChannels;
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
                const auto target=plan.vertices()[mesh.faces[face].indices[corner]].normal;
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

double measureMeshTangents(const Mesh& rebuilt,const Mesh& source,
                          const std::vector<std::array<std::int16_t,3>>& rows,
                          const MeshReconstruction& plan) {
    double error{};
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        return (x*x+y*y)+z*z;
    };
    for(std::size_t face=0;face<source.faces.size();++face)
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto& target=plan.vertices()[source.faces[face].indices[corner]].tangentBasis;
            const auto& actual=rebuilt.vertices[rows[face][corner]].tangentBasis;
            error+=difference(actual.bitangent,target.bitangent)+difference(actual.tangent,target.tangent)+
                difference(actual.normal,target.normal);
        }
    return error;
}

double measureTangentBasis(Model& rebuilt,const Model& source,const CornerRows& rows,
                          const std::map<const Mesh*,MeshReconstruction>& plans,
                          const neoshared::bif::JobControl& job,
                          const std::map<const Mesh*,MeshReconstruction>* changed=nullptr) {
    double error{};
    for(std::size_t index=0;index<source.meshes.size();++index) {
        const auto& mesh=source.meshes[index];
        const auto found=plans.find(&mesh);
        if(!(mesh.mdxBitmap&0x80u) || mesh.animated || mesh.dangly || found==plans.end()) continue;
        if(!changed || changed->find(&mesh)!=changed->end())
            rebuildAsciiTangentBasis(rebuilt.meshes[index],job);
        error+=measureMeshTangents(rebuilt.meshes[index],mesh,rows[index],found->second);
    }
    return error;
}

enum class CornerSearch { Cancellation,TangentAxes,TangentOrder };

void reconstructCyclicCornerPass(Writer& out,const Model& model,const Model& frames,
                                std::map<const Mesh*,MeshReconstruction>& plans,
                                const Mesh* changedMesh,CornerSearch search,bool neighbors) {
    const bool axisTangents=search!=CornerSearch::Cancellation;
    AsciiSkinSeamCache seams;
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
    auto best=measureTangentBasis(current,model,rows,plans,out.job());
    std::vector<double> tangentErrors(model.meshes.size());
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        const auto found=plans.find(&mesh);
        if((mesh.mdxBitmap&0x80u) && !mesh.animated && !mesh.dangly && found!=plans.end())
            tangentErrors[index]=measureMeshTangents(current.meshes[index],mesh,rows[index],found->second);
    }
    processAsciiSkinSeams(current,out.job(),seams);
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
                const auto target=plan.vertices()[mesh.faces[face].indices[corner]].normal;
                const auto source=plan.topology.sourceFaces[face][corner];
                if(!axisTangents && !same(actual,target) &&
                   (cancellationOutput(actual) || cancellationOutput(target)))
                    cancelled[source]=true;
                if(tangents) {
                    const auto& generated=current.meshes[index].vertices[rows[index][face][corner]];
                    const auto& retained=plan.vertices()[mesh.faces[face].indices[corner]].tangentBasis;
                    const auto inspect=[&](Vec3 value,Vec3 wanted) {
                        const double x=double(value.x)-wanted.x,y=double(value.y)-wanted.y,z=double(value.z)-wanted.z;
                        constexpr double rounding=8.0*std::numeric_limits<float>::epsilon();
                        if((x*x+y*y)+z*z<=rounding*rounding) return;
                        changedBasis=true;
                        const bool original=cancellationOutput(value) || cancellationOutput(wanted);
                        if(search==CornerSearch::TangentOrder ? axisAligned(wanted) :
                           axisTangents ? !original && axisAligned(value) && axisAligned(wanted) : original)
                            cancelledTangents.push_back(generated.position);
                    };
                    inspect(generated.tangentBasis.bitangent,retained.bitangent);
                    inspect(generated.tangentBasis.tangent,retained.tangent);
                    // Recover the basis-normal cancellation through UV
                    // precision after choosing the geometric contributors.
                }
            }
            if(changedBasis && !axisTangents) {
                const auto texcoord=[&](std::size_t corner) {
                    return uv(plan.vertices()[mesh.faces[face].indices[plan.sourceCorner(face,corner)]],0u);
                };
                const auto a=texcoord(0u),b=texcoord(1u),c=texcoord(2u);
                singular[face]=(b.x-a.x)*(c.y-a.y)-(c.x-a.x)*(b.y-a.y)==0.0f;
            }
        }
        // Tangent contributions join equal positions even when the authoring
        // topology uses distinct source vertices at that point.
        for(std::size_t source=0;source<cancelled.size();++source) {
            const auto point=plan.vertices()[plan.topology.sourceVertices[source]].position;
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
    // Only skinned models have cross-mesh seam dependencies. Search each
    // static or excluded mesh independently instead of revisiting unrelated
    // meshes after every accepted ordering change.
    const bool skinned=std::any_of(model.meshes.begin(),model.meshes.end(),
        [](const Mesh& mesh){return mesh.skinned;});
    const auto groupOf=[&](const Mesh* mesh){return skinned && mesh->render?nullptr:mesh;};
    std::map<const Mesh*,std::vector<std::size_t>> groups;
    for(std::size_t i=0;i<candidates.size();++i) {
        const auto* mesh=candidates[i].first;
        groups[groupOf(mesh)].push_back(i);
    }
    std::map<const Mesh*,SourceFaceNeighbors> faceNeighbors;
    for(const auto& entry:plans)
        faceNeighbors.emplace(entry.first,SourceFaceNeighbors(*entry.first,
            entry.second.topology.sourceFaces,entry.second.faceOrder));
    for(const auto& group:groups) {
        bool improved;
        do {
            improved=false;
            auto selected=plans;
            auto selectedGeometry=current;
            auto selectedRows=rows;
            auto selectedTangentErrors=tangentErrors;
            auto rebuilt=current;
            auto generated=rows;
            const Mesh* previousMesh=nullptr;
            const auto consider=[&](const Mesh* mesh,MeshReconstruction trial) {
                const auto& retained=plans.at(mesh);
                if(trial.faceOrder!=retained.faceOrder &&
                   !faceNeighbors.at(mesh).preserves(trial.faceOrder,SourceFaceNeighbors::ranks(trial.faceOrder))) return;
                std::map<const Mesh*,MeshReconstruction> changed;
                changed.emplace(mesh,std::move(trial));
                if(previousMesh && previousMesh!=mesh) {
                    const auto previous=static_cast<std::size_t>(previousMesh-model.meshes.data());
                    rebuilt.meshes[previous]=current.meshes[previous];
                    generated[previous]=rows[previous];
                }
                previousMesh=mesh;
                const auto index=static_cast<std::size_t>(mesh-model.meshes.data());
                rebuilt.meshes[index]=frames.meshes[index];
                auto changedRows=generateGeometry(rebuilt,model,changed,false,out.advanced(),out.job(),mesh->render);
                generated[index]=std::move(changedRows[index]);
                if(!preservesSkinChannels(model,generated,plans,mesh)) return;
                auto errors=tangentErrors;
                if((mesh->mdxBitmap&0x80u) && !mesh->dangly) {
                    rebuildAsciiTangentBasis(rebuilt.meshes[index],out.job());
                    errors[index]=measureMeshTangents(rebuilt.meshes[index],*mesh,generated[index],retained);
                }
                double error{};
                for(const auto value:errors) error+=value;
                // Normal error is nonnegative, so this candidate cannot win.
                if(error>=best) return;
                if(mesh->render)
                    processAsciiSkinSeams(rebuilt,out.job(),seams);
                else {
                    // CreateSkinNodeList excludes non-rendered meshes. Changing
                    // only those leaves every ordered seam input unchanged.
                    for(std::size_t index=0;index<model.meshes.size();++index)
                        if(model.meshes[index].render)
                            rebuilt.meshes[index].asciiNormalPool=current.meshes[index].asciiNormalPool;
                }
                error+=measureSeamNormals(rebuilt,model,generated,plans,mesh,false);
                if(error<best) {
                    best=error;improved=true;
                    if(search==CornerSearch::TangentOrder) {
                        selected=plans;selected.at(mesh)=std::move(changed.at(mesh));
                        selectedGeometry=rebuilt;selectedRows=generated;selectedTangentErrors=std::move(errors);
                    } else {
                        // Commit cyclic improvements together in one traversal.
                        // Later proposals start from the latest accepted geometry.
                        plans.at(mesh)=std::move(changed.at(mesh));
                        current=rebuilt;rows=generated;tangentErrors=std::move(errors);
                    }
                }
            };
            // Mirrored tangent sums can retain only rounding residue. Move
            // their contributors as well as their cyclic starts; swapping
            // alone cannot recover every accumulation order.
            if(search==CornerSearch::TangentOrder) for(const auto& pair:pairs) {
                const auto& first=candidates[pair[0]];
                const auto& second=candidates[pair[1]];
                const auto* mesh=first.first;
                if(groupOf(mesh)!=group.first || model.nodes[mesh->nodeIndex].aabb) continue;
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
                if(groupOf(first.first)!=group.first) continue;
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
                if(groupOf(mesh)!=group.first) continue;
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
                if(search==CornerSearch::TangentOrder) {
                    plans=std::move(selected);current=std::move(selectedGeometry);rows=std::move(selectedRows);
                    tangentErrors=std::move(selectedTangentErrors);
                }
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
        const Model& rebuilt,const CornerRows& generated,bool retainAngles=false,
        double rounding=0.0) {
    const auto difference=[&](Vec3 a,Vec3 b) {
        if(rounding && (same(a,Vec3{}) || same(b,Vec3{}))) return same(a,b)?0.0:1.0;
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
        retained=retained && newError<=std::max(oldError,rounding*rounding);
        if(retainAngles) retained=retained && angle(after,target)<=std::max(angle(before,target),rounding);
        improved=improved || (newError<oldError && oldError>rounding*rounding);
    };
    for(std::size_t index=0;index<model.meshes.size() && retained;++index) {
        const auto& source=model.meshes[index];
        const auto existing=plans.find(&source);
        if(existing==plans.end()) continue;
        const auto& plan=existing->second;
        for(std::size_t face=0;face<source.faces.size() && retained;++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto beforeRow=rows[index][face][corner],afterRow=generated[index][face][corner];
                const auto& target=plan.vertices()[source.faces[face].indices[corner]];
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
    auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    for(const auto& mesh:model.meshes) {
        const auto found=plans.find(&mesh);
        if(found==plans.end() || mesh.animated || !found->second.hasNormals ||
           found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
        // AABB leaves address the retained face indices directly.
        if(model.nodes[mesh.nodeIndex].aabb) continue;
        const auto& plan=found->second;
        auto order=reconstructNormalFaceOrder(mesh,plan.vertices(),plan.topology.sourceVertices,
            plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,plan.sourcePositions,out.job());
        auto stars=reconstructNormalCornerStars(mesh,plan.vertices(),plan.topology.sourceVertices,
            plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,order,plan.sourcePositions,out.job());
        if(stars.faceOrder==plan.faceOrder && stars.rotations==plan.cornerRotations) continue;
        auto trial=plans;
        trial.at(&mesh).faceOrder=std::move(stars.faceOrder);
        trial.at(&mesh).cornerRotations=std::move(stars.rotations);
        auto rebuilt=frames;
        const auto generated=generateGeometry(rebuilt,model,trial,false,out.advanced(),out.job());
        if(!preservesSkinChannels(model,generated,plans)) continue;
        measureTangentBasis(rebuilt,model,generated,trial,out.job());
        processAsciiSkinSeams(rebuilt,out.job());
        if(improvesReconstructedShading(model,plans,current,rows,rebuilt,generated)) {
            plans=std::move(trial);current=std::move(rebuilt);rows=generated;
        }
    }
    measureSeamNormals(current,model,rows,plans);
}

// Retained tangent channels disambiguate smoothing even on coplanar faces.
bool reconstructTangentSmoothing(Writer& out,const Model& model,const Model& frames,
                                 std::map<const Mesh*,MeshReconstruction>& plans) {
    using Plans=std::map<const Mesh*,MeshReconstruction>;
    AsciiSkinSeamCache seamCache;
    auto baseline=frames;
    const auto baselineRows=generateGeometry(baseline,model,plans,false,out.advanced(),out.job());
    processAsciiSkinSeams(baseline,out.job(),seamCache);
    double best=measureSeamNormals(baseline,model,baselineRows,plans),bestTangent{};
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
            const auto currentRows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
            bestTangent=measureTangentBasis(current,model,currentRows,plans,out.job());
            auto candidate=plans;
            bool proposed=false;
            for(std::size_t index=0;index<model.meshes.size();++index) {
                const auto& mesh=model.meshes[index];
                const auto found=candidate.find(&mesh);
                if(!(mesh.mdxBitmap&0x80u) || mesh.animated || mesh.dangly || found==candidate.end() ||
                   found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
                auto& plan=found->second;
                const auto& generated=current.meshes[index];
                const auto& source=*generated.asciiTangentSource;
                auto bases=ascii::tangentFaceBases(generated,out.job());
                const auto topology=ascii::prepareTangentBasis(generated,out.job());
                ascii::orientSingularTangentComponents(topology.singular,bases,out.job());
                std::vector<TangentBasis> authoredBases(mesh.faces.size());
                for(std::size_t face=0;face<bases.size();++face)
                    authoredBases[plan.faceOrder[source.authoredFaceIndices[face]]]=bases[face];
                const TangentSmoothingSource tangents{
                    authoredBases,plan.faceOrder,currentRows[index],useGeneratedWriters};
                const auto smoothing=out.smoothing(mesh,plan.vertices(),plan.topology.sourceVertices,
                    plan.topology.sourceFaces,out.job(),nullptr,true,nullptr,&plan.smoothing.masks,false,
                    &tangents,&plan.cornerRotations,nullptr,
                    plan.preciseSourcePositions.empty()?nullptr:&plan.preciseSourcePositions);
                proposed|=smoothing.masks!=plan.smoothing.masks;
                plan.smoothing=smoothing;
            }
            if(!proposed) continue;
            const auto considerTangents=[&](Plans trial) {
                auto rebuilt=frames;
                const auto rows=generateGeometry(rebuilt,model,trial,false,out.advanced(),out.job());
                const auto tangentError=measureTangentBasis(rebuilt,model,rows,trial,out.job());
                if(tangentError>bestTangent) return;
                processAsciiSkinSeams(rebuilt,out.job(),seamCache);
                if(!preservesSkinChannels(model,rows,plans)) return;
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
}

void reconstructTangentRounding(Writer& out,const Model& model,const Model& frames,
                               std::map<const Mesh*,MeshReconstruction>& plans) {
    if(!out.refineTangentRounding()) return;
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
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
                    const auto& target=found->second.vertices()[mesh.faces[face].indices[corner]];
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
                    const auto& target=plan.vertices()[mesh.faces[face].indices[corner]].tangentBasis;
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
                            const auto changedRows=generateGeometry(rebuilt,model,changedPlans,false,out.advanced(),out.job());
                            generated[index]=changedRows[index];
                            if(!preservesSkinChannels(model,generated,plans)) continue;
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
    auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    for(const auto& mesh:model.meshes) {
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !mesh.render || mesh.animated || mesh.dangly ||
           !found->second.hasNormals || found->second.explicitNormals || mesh.asciiNormalPositionSource ||
           model.nodes[mesh.nodeIndex].aabb) continue;
        const auto& plan=found->second;
        auto refined=reconstructNormalRounding(mesh,plan.vertices(),plan.topology.sourceVertices,
            plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,
            plan.sourcePositions,out.job());
        if(refined.rotations==plan.cornerRotations && refined.faceOrder==plan.faceOrder &&
           std::equal(refined.positions.begin(),refined.positions.end(),plan.sourcePositions.begin(),same)) continue;
        auto trial=plans;
        trial.at(&mesh).sourcePositions=std::move(refined.positions);
        trial.at(&mesh).cornerRotations=std::move(refined.rotations);
        trial.at(&mesh).faceOrder=std::move(refined.faceOrder);
        auto rebuilt=frames;
        const auto generated=generateGeometry(rebuilt,model,trial,false,out.advanced(),out.job());
        if(!preservesSkinChannels(model,generated,plans)) continue;
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
    if(mesh.animated) return false;
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
    auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    const auto component=[](Vec3 value,std::size_t axis) {
        return axis==0u?value.x:axis==1u?value.y:value.z;
    };
    // Precision reconstruction may change the last few bits of an otherwise
    // unchanged unit normal. Score those float-rounding differences equally.
    constexpr double rounding=32.0*std::numeric_limits<float>::epsilon();
    const auto difference=[](Vec3 a,Vec3 b) {
        if(same(a,Vec3{}) || same(b,Vec3{})) return same(a,b)?0.0:1.0;
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        const double squared=(x*x+y*y)+z*z;
        return squared<=rounding*rounding?0.0:squared;
    };
    const auto angle=[](Vec3 a,Vec3 b) {
        if(same(a,b)) return 0.0;
        if(same(a,Vec3{}) || same(b,Vec3{})) return std::acos(-1.0);
        const double x=double(a.y)*b.z-double(a.z)*b.y;
        const double y=double(a.z)*b.x-double(a.x)*b.z;
        const double z=double(a.x)*b.y-double(a.y)*b.x;
        const double value=std::atan2(std::sqrt((x*x+y*y)+z*z),
            (double(a.x)*b.x+double(a.y)*b.y)+double(a.z)*b.z);
        return value<=rounding?0.0:value;
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
        const auto accept=[&](MeshReconstruction candidate) {
            std::map<const Mesh*,MeshReconstruction> changed;
            changed.emplace(&mesh,std::move(candidate));
            auto rebuilt=current;
            rebuilt.meshes[index]=frames.meshes[index];
            auto generated=rows;
            auto changedRows=generateGeometry(rebuilt,model,changed,false,out.advanced(),out.job());
            generated[index]=std::move(changedRows[index]);
            if(!preservesSkinChannels(model,generated,plans)) return false;
            measureTangentBasis(rebuilt,model,generated,plans,out.job(),&changed);
            processAsciiSkinSeams(rebuilt,out.job());
            if(!improvesReconstructedShading(model,plans,current,rows,rebuilt,generated,true,rounding)) return false;
            plan=std::move(changed.at(&mesh));
            if(!plan.preciseSourcePositions.empty()) high=plan.preciseSourcePositions;
            current=std::move(rebuilt);rows=std::move(generated);return true;
        };
        // Retain the original links as a precision seed as well as refitting
        // them: a refit can discard a link needed by an exact zero normal.
        using PrecisionFit=std::tuple<std::vector<std::uint32_t>,std::vector<std::uint8_t>,
            std::vector<std::size_t>,std::vector<std::array<double,3>>>;
        std::set<PrecisionFit> precisionFits;
        if(plan.preciseSourcePositions.empty()) for(const bool refit:{false,true}) for(const bool sourceOrder:{false,true}) {
            auto trial=plan;
            trial.preciseSourcePositions=high;
            if(sourceOrder) {
                std::fill(trial.cornerRotations.begin(),trial.cornerRotations.end(),0u);
                std::iota(trial.faceOrder.begin(),trial.faceOrder.end(),std::size_t{});
            }
            if(refit) trial.smoothing=out.smoothing(mesh,plan.vertices(),plan.topology.sourceVertices,
                plan.topology.sourceFaces,out.job(),nullptr,true,nullptr,&plan.smoothing.masks,
                false,nullptr,&trial.cornerRotations,nullptr,&high);
            // Reconstructed and source order can produce the same seed.
            // Fit each complete precision state once.
            if(precisionFits.emplace(trial.smoothing.masks,trial.cornerRotations,
                    trial.faceOrder,high).second) {
                if(!model.nodes[mesh.nodeIndex].aabb) {
                    const auto stars=reconstructNormalCornerStars(mesh,plan.vertices(),plan.topology.sourceVertices,
                        plan.topology.sourceFaces,trial.smoothing.masks,trial.cornerRotations,trial.faceOrder,
                        plan.sourcePositions,out.job(),&high);
                    trial.cornerRotations=stars.rotations;trial.faceOrder=stars.faceOrder;
                }
                accept(std::move(trial));
            }
        }
        // Opposite triangles at a leaf tip share one authored position but
        // round their products independently. Recover that small XYZ residual
        // jointly with both cyclic starts, then refit the remaining faces.
        struct Tip {
            std::size_t source;
            std::array<std::size_t,2> faces,corners;
        };
        std::vector<std::vector<std::size_t>> tipIncident(high.size());
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(const auto source:plan.topology.sourceFaces[face])
                if(tipIncident[source].empty() || tipIncident[source].back()!=face)
                    tipIncident[source].push_back(face);
        std::vector<Tip> tips;
        std::vector<bool> tipFaces(mesh.faces.size()),tipVertices(high.size());
        for(std::size_t source=0;source<tipIncident.size();++source) {
            const auto& faces=tipIncident[source];
            if(faces.size()!=2u) continue;
            auto reverse=plan.topology.sourceFaces[faces[0]];
            std::swap(reverse[1],reverse[2]);
            if(oriented(reverse)!=oriented(plan.topology.sourceFaces[faces[1]])) continue;
            Tip tip{source,{faces[0],faces[1]},{}};
            for(std::size_t side=0;side<2u;++side) {
                const auto& triangle=plan.topology.sourceFaces[tip.faces[side]];
                tip.corners[side]=std::find(triangle.begin(),triangle.end(),source)-triangle.begin();
                tipFaces[tip.faces[side]]=true;
            }
            tipVertices[source]=true;tips.push_back(tip);
        }
        const auto unresolvedTips=[&] {
            return std::any_of(tips.begin(),tips.end(),[&](const Tip& tip) {
                for(std::size_t side=0;side<2u;++side) {
                    const auto face=tip.faces[side],corner=tip.corners[side];
                    if(difference(current.meshes[index].normalAt(rows[index][face][corner]),
                        plan.vertices()[mesh.faces[face].indices[corner]].normal)>0.0) return true;
                }
                return false;
            });
        };
        if(!model.nodes[mesh.nodeIndex].aabb && unresolvedTips()) {
            std::vector<std::array<std::array<double,2>,3>> intervals(high.size());
            for(std::size_t source=0;source<high.size();++source) for(std::size_t axis=0;axis<3u;++axis) {
                const float value=component(plan.sourcePositions[source],axis);
                for(std::size_t side=0;side<2u;++side) {
                    const auto direction=side?std::numeric_limits<float>::infinity():-std::numeric_limits<float>::infinity();
                    const double edge=(double(value)+std::nextafter(value,direction))*0.5;
                    intervals[source][axis][side]=std::nextafter(edge,double(value));
                    if(value==0.0f && std::signbit(value)!=std::signbit(direction)) intervals[source][axis][side]=value;
                }
            }
            std::vector<std::size_t> coreFaces;
            for(std::size_t face=0;face<tipFaces.size();++face) if(!tipFaces[face]) coreFaces.push_back(face);
            const auto fitTips=[&](MeshReconstruction& trial) {
                auto& positions=trial.preciseSourcePositions;
                for(const auto& tip:tips) {
                    const auto initialPosition=positions[tip.source];
                    const auto products=[&](std::size_t face,std::size_t rotation) {
                        std::array<Vec3,3> low;
                        std::array<std::array<double,3>,3> precise;
                        for(std::size_t c=0;c<3u;++c) {
                            const auto source=trial.topology.sourceFaces[face][(rotation+c)%3u];
                            low[c]=trial.sourcePositions[source];precise[c]=positions[source];
                        }
                        return ascii::normalPositionFaceProducts(low,precise);
                    };
                    const auto crosses=[&](std::size_t face,std::size_t rotation) {
                        const auto p=products(face,rotation);
                        return Vec3{p[0].x-p[1].x,p[0].y-p[1].y,p[0].z-p[1].z};
                    };
                    const bool joined=(trial.smoothing.masks[tip.faces[0]]&trial.smoothing.masks[tip.faces[1]])!=0u;
                    const auto score=[&](std::size_t first,std::size_t second) {
                        const auto a=crosses(tip.faces[0],first),b=crosses(tip.faces[1],second);
                        const auto sum=normal(Vec3{a.x+b.x,a.y+b.y,a.z+b.z});
                        double squared{},angular{};
                        for(std::size_t side=0;side<2u;++side) {
                            const auto target=trial.vertices()[mesh.faces[tip.faces[side]].indices[tip.corners[side]]].normal;
                            const auto value=joined?sum:normal(side?b:a);
                            squared+=difference(value,target);angular+=angle(value,target);
                        }
                        return std::make_pair(squared,angular);
                    };
                    auto best=score(trial.cornerRotations[tip.faces[0]],trial.cornerRotations[tip.faces[1]]);
                    auto bestPosition=positions[tip.source];
                    std::array<std::uint8_t,2> bestRotations{trial.cornerRotations[tip.faces[0]],trial.cornerRotations[tip.faces[1]]};
                    const auto consider=[&] {
                        for(std::uint8_t a=0;a<3u;++a) for(std::uint8_t b=0;b<3u;++b) {
                            const auto value=score(a,b);
                            if(value<best) {best=value;bestPosition=positions[tip.source];bestRotations={a,b};}
                        }
                    };
                    std::array<std::vector<double>,3> seeds;
                    for(std::size_t axis=0;axis<3u;++axis) {
                        const auto ends=intervals[tip.source][axis];
                        seeds[axis]={initialPosition[axis],double(component(trial.sourcePositions[tip.source],axis)),ends[0],ends[1]};
                        std::sort(seeds[axis].begin(),seeds[axis].end());
                        seeds[axis].erase(std::unique(seeds[axis].begin(),seeds[axis].end()),seeds[axis].end());
                    }
                    for(const auto x:seeds[0]) for(const auto y:seeds[1]) for(const auto z:seeds[2]) {
                        positions[tip.source]={x,y,z};consider();
                    }
                    positions[tip.source]=bestPosition;
                    for(;;) {
                        const auto before=best;
                        for(std::size_t axis=0;axis<3u;++axis) {
                            const auto origin=positions[tip.source];
                            const auto ends=intervals[tip.source][axis];
                            std::set<double> values{origin[axis],ends[0],ends[1]};
                            // Every product is monotone in one coordinate.
                            // Visit both sides of its float rounding boundaries.
                            for(const auto face:tip.faces) for(std::size_t rotation=0;rotation<3u;++rotation)
                                for(std::size_t term=0;term<2u;++term) for(std::size_t c=0;c<3u;++c) {
                                    const auto product=[&](double value) {
                                        positions[tip.source][axis]=value;
                                        return component(products(face,rotation)[term],c);
                                    };
                                    double cursor=ends[0];const auto final=product(ends[1]);
                                    while(product(cursor)!=final) {
                                        out.job().check();const auto initial=product(cursor);
                                        double inside=cursor,outside=ends[1];
                                        while(std::nextafter(inside,outside)!=outside) {
                                            const double middle=inside+(outside-inside)*0.5;
                                            if(product(middle)==initial) inside=middle;else outside=middle;
                                        }
                                        values.insert(inside);values.insert(outside);cursor=outside;
                                    }
                                }
                            for(const auto value:values) {positions[tip.source]=origin;positions[tip.source][axis]=value;consider();}
                            positions[tip.source]=bestPosition;
                        }
                        if(!(best<before)) break;
                    }
                    positions[tip.source]=bestPosition;
                    trial.cornerRotations[tip.faces[0]]=bestRotations[0];
                    trial.cornerRotations[tip.faces[1]]=bestRotations[1];
                }
            };
            std::vector<std::vector<std::uint32_t>> maskSeeds{plan.smoothing.masks};
            auto linked=plan.smoothing.masks;
            std::uint32_t used{};for(const auto mask:linked) used|=mask;
            for(const auto& tip:tips) if(!(linked[tip.faces[0]]&linked[tip.faces[1]])) {
                std::uint32_t bit=1u;while(bit && (used&bit)) bit<<=1u;
                if(bit) {linked[tip.faces[0]]|=bit;linked[tip.faces[1]]|=bit;used|=bit;}
            }
            if(linked!=plan.smoothing.masks) maskSeeds.push_back(std::move(linked));
            maskSeeds.emplace_back(mesh.faces.size(),1u);
            for(const auto& masks:maskSeeds) {
                const auto origin=high;
                std::vector<std::vector<std::array<double,3>>> seeds{origin};
                for(std::size_t source=0;source<high.size();++source) if(!tipVertices[source])
                    for(std::size_t axis=0;axis<3u;++axis) for(const auto value:intervals[source][axis]) {
                        auto seed=origin;seed[source][axis]=value;seeds.push_back(std::move(seed));
                    }
                for(const auto& seed:seeds) {
                    if(!unresolvedTips()) break;
                    auto trial=plan;trial.smoothing.masks=masks;trial.preciseSourcePositions=seed;
                    fitTips(trial);
                    std::vector<Vec3> baseline(mesh.faces.size()*3u);
                    for(std::size_t face=0;face<mesh.faces.size();++face) for(std::size_t c=0;c<3u;++c)
                        baseline[face*3u+c]=current.meshes[index].normalAt(rows[index][face][c]);
                    const auto stars=reconstructNormalCornerStars(mesh,trial.vertices(),trial.topology.sourceVertices,
                        trial.topology.sourceFaces,trial.smoothing.masks,trial.cornerRotations,trial.faceOrder,
                        trial.sourcePositions,out.job(),&trial.preciseSourcePositions,&coreFaces,&baseline);
                    trial.cornerRotations=stars.rotations;trial.faceOrder=stars.faceOrder;
                    accept(std::move(trial));
                }
            }
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
            if(current.skinSeams.processed && mesh.render) {
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
                    differs=differs || difference(current.meshes[index].normalAt(rows[index][face][c]),
                        plan.vertices()[mesh.faces[face].indices[c]].normal)>0.0;
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
                                const auto target=plan.vertices()[mesh.faces[corner/3u].indices[corner%3u]].normal;
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
                                        const auto target=plan.vertices()[mesh.faces[face].indices[c]].normal;
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
                                const auto target=plan.vertices()[mesh.faces[face].indices[c]].normal;
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
                                const auto target=plan.vertices()[mesh.faces[face].indices[c]].normal;
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
                    eligibleCorners[corner]=difference(current.meshes[index].normalAt(rows[index][face][c]),
                        plan.vertices()[mesh.faces[face].indices[c]].normal)>0.0;
                }
            auto masks=sharedSeamSmoothingCandidates(mesh,plan.vertices(),plan.topology.sourceFaces,
                plan.smoothing.masks,eligibleCorners,out.job(),false,true);
            // Precision changes the producer for every face in the mesh.
            // Join disconnected missing-link groups together so each can be
            // refitted before that arithmetic change is measured globally.
            std::uint32_t used{};
            for(const auto mask:plan.smoothing.masks) used|=mask;
            auto combined=plan.smoothing.masks;
            for(const auto& candidate:masks)
                for(std::size_t face=0;face<combined.size();++face)
                    combined[face]|=candidate.masks[face]&~used;
            if(combined!=plan.smoothing.masks) masks.insert(masks.begin(),{0u,std::move(combined)});
            std::vector<Vec3> baselineCorners(mesh.faces.size()*3u);
            for(std::size_t face=0;face<mesh.faces.size();++face)
                for(std::size_t corner=0;corner<3u;++corner)
                    baselineCorners[face*3u+corner]=current.meshes[index].normalAt(rows[index][face][corner]);
            for(const auto& candidate:masks) {
                auto trial=plan;trial.smoothing.masks=candidate.masks;
                if(accept(trial)) {accepted=true;break;}
                trial.preciseSourcePositions=high;
                if(!model.nodes[mesh.nodeIndex].aabb) {
                    const auto stars=reconstructNormalCornerStars(mesh,plan.vertices(),plan.topology.sourceVertices,
                        plan.topology.sourceFaces,trial.smoothing.masks,trial.cornerRotations,trial.faceOrder,
                        plan.sourcePositions,out.job(),&high,nullptr,&baselineCorners);
                    trial.cornerRotations=stars.rotations;trial.faceOrder=stars.faceOrder;
                }
                if(accept(std::move(trial))) {accepted=true;break;}
            }
            if(accepted) {
                if(!rebuildGroups()) break;
                continue;
            }

            // Keep the published edge neighbors while refitting the ordered
            // gathers against the actual authored-position precision.
            if(!model.nodes[mesh.nodeIndex].aabb) {
                auto order=reconstructNormalFaceOrder(mesh,plan.vertices(),plan.topology.sourceVertices,
                    plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,
                    plan.sourcePositions,out.job(),&high);
                if(order!=plan.faceOrder) {
                    auto trial=plan;trial.faceOrder=std::move(order);trial.preciseSourcePositions=high;
                    accepted=accept(std::move(trial));
                }
                if(!accepted) {
                    auto stars=reconstructNormalCornerStars(mesh,plan.vertices(),plan.topology.sourceVertices,
                        plan.topology.sourceFaces,plan.smoothing.masks,plan.cornerRotations,plan.faceOrder,
                        plan.sourcePositions,out.job(),&high);
                    if(stars.faceOrder!=plan.faceOrder || stars.rotations!=plan.cornerRotations) {
                        auto trial=plan;trial.faceOrder=std::move(stars.faceOrder);
                        trial.preciseSourcePositions=high;
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
                        const auto target=plan.vertices()[mesh.faces[face].indices[c]].normal;
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
                                // Enabling position precision also changes unchanged faces.
                                // Include their corner limits when finding a common repair source.
                                for(std::size_t id=0;id<groups.size();++id) {
                                    const auto& group=groups[id];
                                    if(!group.seamFree) continue;
                                    Vec3 sum{};
                                    for(const auto face:group.faces) {
                                        const auto cross=proposed[face];
                                        sum={sum.x+cross.x,sum.y+cross.y,cross.z+sum.z};
                                    }
                                    const auto after=normal(sum);bool worse=false;
                                    for(const auto corner:group.corners) {
                                        const auto face=corner/3u,c=corner%3u;
                                        const auto target=plan.vertices()[mesh.faces[face].indices[c]].normal;
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
                        const auto target=plan.vertices()[mesh.faces[corner/3u].indices[corner%3u]].normal;
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
                            plan.vertices()[mesh.faces[corner/3u].indices[corner%3u]].normal));
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
                                                const auto target=plan.vertices()[mesh.faces[corner/3u].indices[corner%3u]].normal;
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
                                        const auto target=plan.vertices()[mesh.faces[corner/3u].indices[corner%3u]].normal;
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
                                auto stars=reconstructNormalCornerStars(mesh,plan.vertices(),plan.topology.sourceVertices,
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

// A seam can hide the geometric normals that produced its stored rows.
// Refit local contributors against the complete ordered seam output, including
// removing links that a per-mesh normal fit would otherwise retain.
void reconstructSeamSmoothing(Writer& out,const Model& model,const Model& frames,
                               std::map<const Mesh*,MeshReconstruction>& plans) {
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
    auto bestTangent=measureTangentBasis(current,model,rows,plans,out.job());
    AsciiSkinSeamCache cache;
    processAsciiSkinSeams(current,out.job(),cache);
    auto best=measureSeamNormals(current,model,rows,plans);
    constexpr double rounding=8.0*std::numeric_limits<float>::epsilon();
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !mesh.render || mesh.animated || !found->second.hasNormals ||
           found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
        auto& plan=found->second;
        // The engine observes overlap and equal-mask reuse among incident
        // faces. Distinct bit labels with those same relationships generate
        // the same ordered normal and tangent contributions.
        std::map<std::array<std::uint32_t,3>,std::vector<std::size_t>> incident;
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(const auto source:plan.topology.sourceFaces[face]) {
                const auto p=plan.sourcePositions[source];
                auto& faces=incident[{bits(p.x),bits(p.y),bits(p.z)}];
                if(faces.empty() || faces.back()!=face) faces.push_back(face);
            }
        std::set<std::pair<std::size_t,std::size_t>> relationSet;
        for(const auto& entry:incident)
            for(std::size_t a=0;a<entry.second.size();++a)
                for(std::size_t b=a+1u;b<entry.second.size();++b)
                    relationSet.emplace(entry.second[a],entry.second[b]);
        const std::vector<std::pair<std::size_t,std::size_t>> relations(relationSet.begin(),relationSet.end());
        const auto relationship=[](std::uint32_t a,std::uint32_t b) -> std::uint8_t {
            return !(a&b)?0u:a==b?2u:1u;
        };
        std::vector<std::uint8_t> initial;
        initial.reserve(relations.size());
        std::vector<std::vector<std::size_t>> faceRelations(mesh.faces.size());
        for(std::size_t index=0;index<relations.size();++index) {
            const auto& relation=relations[index];
            initial.push_back(relationship(plan.smoothing.masks[relation.first],plan.smoothing.masks[relation.second]));
            faceRelations[relation.first].push_back(index);
            faceRelations[relation.second].push_back(index);
        }
        using Signature=std::vector<std::pair<std::size_t,std::uint8_t>>;
        std::set<Signature> visited;
        std::set<std::pair<Signature,std::size_t>> roundedVisited;
        auto retained=visited.emplace().first;
        std::vector<std::size_t> affected;
        auto rebuilt=current;
        auto generatedRows=rows;
        const auto consider=[&](const auto& edits,std::optional<std::size_t> roundedFace={},bool retainCorners=false) {
            // Only relations incident to the edited faces can change. Keep
            // their differences from the fixed initial state as the same
            // equivalence key, and copy a plan only for a new relationship.
            auto key=*retained;
            affected.clear();
            for(const auto& edit:edits) {
                const auto& list=faceRelations[edit.first];
                affected.insert(affected.end(),list.begin(),list.end());
            }
            std::sort(affected.begin(),affected.end());
            affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
            const auto mask=[&](std::size_t face) {
                for(const auto& edit:edits) if(edit.first==face) return edit.second;
                return plan.smoothing.masks[face];
            };
            for(const auto index:affected) {
                const auto& relation=relations[index];
                const auto value=relationship(mask(relation.first),mask(relation.second));
                const auto at=std::lower_bound(key.begin(),key.end(),index,
                    [](const auto& entry,std::size_t index){return entry.first<index;});
                if(value==initial[index]) {
                    if(at!=key.end() && at->first==index) key.erase(at);
                } else if(at!=key.end() && at->first==index) at->second=value;
                else key.insert(at,{index,value});
            }
            const auto evaluated=visited.insert(std::move(key));
            const bool tryRounded=roundedFace && roundedVisited.emplace(*evaluated.first,*roundedFace).second;
            if(!evaluated.second && !tryRounded) return false;
            auto candidate=plan;
            for(const auto& edit:edits) candidate.smoothing.masks[edit.first]=edit.second;
            const auto evaluate=[&](MeshReconstruction proposal,bool rounded) {
                std::map<const Mesh*,MeshReconstruction> changed;
                changed.emplace(&mesh,std::move(proposal));
                rebuilt.meshes[index]=frames.meshes[index];
                const auto changedRows=generateGeometry(rebuilt,model,changed,false,out.advanced(),out.job());
                generatedRows[index]=changedRows[index];
                if(!preservesSkinChannels(model,generatedRows,plans)) return false;
                const auto tangent=measureTangentBasis(rebuilt,model,generatedRows,plans,out.job(),&changed);
                if(tangent>bestTangent) return false;
                processAsciiSkinSeams(rebuilt,out.job(),cache);
                const auto error=measureSeamNormals(rebuilt,model,generatedRows,plans,nullptr,false);
                if(error>best || !(error<best || tangent<bestTangent)) return false;
                if(retainCorners && !improvesReconstructedShading(model,plans,current,rows,
                        rebuilt,generatedRows,true,rounding)) return false;
                plan=std::move(changed.at(&mesh));current=rebuilt;rows=generatedRows;
                if(rounded) {
                    auto key=*evaluated.first;
                    visited.clear();roundedVisited.clear();
                    retained=visited.insert(std::move(key)).first;
                } else retained=evaluated.first;
                best=error;bestTangent=tangent;return true;
            };
            if(!tryRounded || candidate.preciseFaceTexcoords.empty())
                return evaluated.second && evaluate(std::move(candidate),false);
            bool accepted=evaluated.second && evaluate(candidate,false);
            if(tryRounded && !candidate.preciseFaceTexcoords.empty()) {
                auto& precise=candidate.preciseFaceTexcoords[*roundedFace];
                const auto previous=precise;
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto uv=plan.vertices()[mesh.faces[*roundedFace].indices[corner]].texcoord0;
                    precise[corner]={uv.x,uv.y};
                }
                // A precision residual chosen for an old contributor set
                // can reverse a collapsed tangent after that set is split.
                if(precise!=previous) accepted=evaluate(std::move(candidate),true) || accepted;
            }
            return accepted;
        };
        for(;;) {
            std::vector<bool> selected(plan.sourcePositions.size());
            for(std::size_t face=0;face<mesh.faces.size();++face)
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto row=rows[index][face][corner];
                    const auto before=current.meshes[index].vertices[row].normal;
                    const auto actual=current.meshes[index].normalAt(row);
                    const auto target=plan.vertices()[mesh.faces[face].indices[corner]].normal;
                    const double x=double(actual.x)-target.x,y=double(actual.y)-target.y,z=double(actual.z)-target.z;
                    if(!same(before,actual) && (x*x+y*y)+z*z>rounding*rounding)
                        selected[plan.topology.sourceFaces[face][corner]]=true;
                }
            // Seam-averaged targets can split one smooth surface into several
            // local masks. Rejoin the smooth faces together while retaining
            // flat faces, then score the complete ordered seam result.
            if(std::any_of(selected.begin(),selected.end(),[](bool value){return value;})) {
                std::vector<std::pair<std::size_t,std::uint32_t>> joined;
                for(std::size_t face=0;face<mesh.faces.size();++face)
                    if(plan.smoothing.masks[face]>1u) joined.emplace_back(face,1u);
                if(consider(joined,std::nullopt,true)) continue;
            }
            bool accepted=false;
            for(std::size_t face=0;face<mesh.faces.size();++face) {
                const auto& sources=plan.topology.sourceFaces[face];
                if(std::none_of(sources.begin(),sources.end(),[&](auto source){return selected[source];})) continue;
                std::uint32_t relevant{};
                std::set<std::uint32_t> masks{0u};
                for(std::size_t other=0;other<mesh.faces.size();++other) {
                    const auto& neighbors=plan.topology.sourceFaces[other];
                    if(std::none_of(sources.begin(),sources.end(),[&](auto source) {
                        return std::find(neighbors.begin(),neighbors.end(),source)!=neighbors.end();
                    })) continue;
                    relevant|=plan.smoothing.masks[other];
                    masks.insert(plan.smoothing.masks[other]);
                }
                const auto original=plan.smoothing.masks[face];
                masks.insert(relevant);masks.insert(relevant^original);
                for(std::uint32_t a=1u;a;a<<=1u) if(relevant&a) {
                    masks.insert(original^a);
                    for(std::uint32_t b=a<<1u;b;b<<=1u)
                        if(relevant&b) masks.insert(original^a^b);
                }
                for(const auto mask:masks) {
                    accepted=consider(std::array{std::pair{face,mask}}) || accepted;
                }
            }
            if(!accepted) {
                std::map<std::pair<std::size_t,std::size_t>,std::set<std::int16_t>> generated;
                for(std::size_t face=0;face<mesh.faces.size();++face)
                    for(std::size_t corner=0;corner<3u;++corner) {
                        const auto source=plan.topology.sourceFaces[face][corner];
                        generated[{source,mesh.faces[face].indices[corner]}].insert(rows[index][face][corner]);
                    }
                std::vector<bool> split(selected.size());
                for(const auto& entry:generated)
                    if(entry.second.size()>1u && selected[entry.first.first]) split[entry.first.first]=true;
                // Couple one contributor-bit change on each face. The
                // single-face search already covers its local combinations;
                // pairing these moves can cross a local minimum without
                // changing source-index edges or generated bounds.
                const auto candidates=[&](std::size_t face) {
                    std::uint32_t relevant{};
                    const auto& sources=plan.topology.sourceFaces[face];
                    for(std::size_t other=0;other<mesh.faces.size();++other) {
                        const auto& neighbors=plan.topology.sourceFaces[other];
                        if(std::none_of(sources.begin(),sources.end(),[&](auto source) {
                            return std::find(neighbors.begin(),neighbors.end(),source)!=neighbors.end();
                        })) continue;
                        relevant|=plan.smoothing.masks[other];
                    }
                    const auto original=plan.smoothing.masks[face];
                    std::set<std::uint32_t> masks{original};
                    for(std::uint32_t bit=1u;bit;bit<<=1u)
                        if(relevant&bit) masks.insert(original^bit);
                    return masks;
                };
                for(const auto& relation:relations) {
                    const auto face=relation.first,other=relation.second;
                    const auto& a=plan.topology.sourceFaces[face];const auto& b=plan.topology.sourceFaces[other];
                    if(std::none_of(a.begin(),a.end(),[&](auto source) {
                        return split[source] && std::find(b.begin(),b.end(),source)!=b.end();
                    })) continue;
                    const auto left=candidates(face),right=candidates(other);
                    for(const auto first:left) for(const auto second:right) {
                        accepted=consider(std::array{std::pair{face,first},std::pair{other,second}}) || accepted;
                    }
                }
            }
            if(!accepted && (mesh.mdxBitmap&0x80u) && !mesh.dangly) {
                std::set<std::array<std::uint32_t,3>> selectedPositions;
                const auto difference=[](Vec3 a,Vec3 b) {
                    const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
                    return (x*x+y*y)+z*z;
                };
                for(std::size_t face=0;face<mesh.faces.size();++face)
                    for(std::size_t corner=0;corner<3u;++corner) {
                        const auto& actual=current.meshes[index].vertices[rows[index][face][corner]].tangentBasis;
                        const auto& target=plan.vertices()[mesh.faces[face].indices[corner]].tangentBasis;
                        if(difference(actual.bitangent,target.bitangent)+difference(actual.tangent,target.tangent)+
                           difference(actual.normal,target.normal)>rounding*rounding) {
                            const auto p=plan.sourcePositions[plan.topology.sourceFaces[face][corner]];
                            selectedPositions.insert({bits(p.x),bits(p.y),bits(p.z)});
                        }
                    }
                // One group bit can join unrelated corners at a coincident
                // position. Give its required joins at the other corners a
                // separate bit before removing the unwanted contribution.
                for(const auto& position:selectedPositions) {
                    const auto& faces=incident.at(position);
                    for(const auto face:faces) {
                        std::set<std::uint32_t> removals;
                        const auto original=plan.smoothing.masks[face];
                        if(original) removals.insert(original);
                        for(std::uint32_t bit=1u;bit;bit<<=1u)
                            if(original&bit) removals.insert(bit);
                        for(const auto other:faces) if(other!=face) {
                            const auto shared=plan.smoothing.masks[face]&plan.smoothing.masks[other];
                            if(shared) removals.insert(shared);
                        }
                        for(const auto removed:removals) {
                            const auto original=plan.smoothing.masks[face];
                            const auto remaining=original&~removed;
                            std::uint32_t used{};
                            for(const auto mask:plan.smoothing.masks) used|=mask;
                            for(std::uint32_t fresh=1u;fresh;fresh<<=1u) if(!(used&fresh)) {
                                std::map<std::size_t,std::uint32_t> changed{{face,remaining|fresh}};
                                for(const auto source:plan.topology.sourceFaces[face]) {
                                    const auto p=plan.sourcePositions[source];
                                    const std::array<std::uint32_t,3> key{bits(p.x),bits(p.y),bits(p.z)};
                                    if(key==position) continue;
                                    for(const auto other:incident.at(key)) if(other!=face) {
                                        const auto mask=plan.smoothing.masks[other];
                                        if((original&mask) && !(remaining&mask)) changed[other]=mask|fresh;
                                    }
                                }
                                const std::vector<std::pair<std::size_t,std::uint32_t>> edits(changed.begin(),changed.end());
                                accepted=consider(std::array{std::pair{face,remaining|fresh}},face) || accepted;
                                accepted=consider(edits,face) || accepted;
                                break;
                            }
                        }
                    }
                }
            }
            if(!accepted) break;
        }
    }
    measureSeamNormals(current,model,rows,plans);
}

bool tangentPrecisionMesh(const Mesh& mesh) {
    return (mesh.mdxBitmap&0x80u) && !mesh.animated && !mesh.dangly;
}

void reconstructTangentUvPrecision(Writer& out,const Model& model,const Model& frames,
                                  std::map<const Mesh*,MeshReconstruction>& plans,bool singular=false) {
    if(std::none_of(model.meshes.begin(),model.meshes.end(),tangentPrecisionMesh)) return;
    auto current=frames;
    const auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    const auto opposite=[](Vec3 a,Vec3 b) {
        return !same(a,b) && (a.x*b.x+a.y*b.y)+a.z*b.z<=0.0f;
    };
    const auto selectedVector=[&](Vec3 value,Vec3 target) {
        return singular?!same(value,target):opposite(value,target);
    };
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        return (x*x+y*y)+z*z;
    };
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !tangentPrecisionMesh(mesh)) continue;
        auto& plan=found->second;
        auto& existing=current.meshes[index];
        const auto rowSize=existing.mdxRowSize,weightOffset=existing.mdxWeightOffset,
            boneOffset=existing.mdxBoneIndexOffset,tangentOffset=existing.mdxTangentOffset;
        const auto topology=ascii::prepareTangentBasis(existing,out.job());
        const auto& initial=*existing.asciiTangentSource;
        std::map<std::array<std::uint32_t,3>,std::vector<std::size_t>> incident;
        std::vector<std::optional<std::size_t>> writers(existing.vertices.size());
        for(std::size_t face=0;face<existing.faces.size();++face)
            for(const auto row:existing.faces[face].indices) {
                writers[row]=face;
                const auto p=initial.generatedPositions[row];
                auto& faces=incident[{bits(p.x),bits(p.y),bits(p.z)}];
                if(faces.empty() || faces.back()!=face) faces.push_back(face);
            }
        std::vector<std::vector<std::size_t>> influences(existing.vertices.size());
        for(std::size_t row=0;row<writers.size();++row) if(writers[row]) {
            const auto writer=*writers[row];
            const auto p=existing.vertices[row].position;
            auto& faces=influences[row];
            faces.push_back(writer);
            for(const auto other:incident[{bits(p.x),bits(p.y),bits(p.z)}]) {
                if(other==writer || !(initial.faceSmoothing[writer]&initial.faceSmoothing[other])) continue;
                const auto& indices=existing.faces[other].indices;
                if(std::any_of(indices.begin(),indices.end(),[&](std::int16_t source) {
                    return same(initial.generatedPositions[source],p);
                })) faces.push_back(other);
            }
        }
        std::vector<bool> completed(existing.faces.size());
        for(;;) {
            const auto& retained=*existing.asciiTangentSource;
            std::vector<unsigned> selected(existing.faces.size());
            std::size_t oldOpposed{};
            for(std::size_t face=0;face<mesh.faces.size();++face)
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto row=rows[index][face][corner];
                    const auto& actual=existing.vertices[row];
                    const auto& target=plan.vertices()[mesh.faces[face].indices[corner]].tangentBasis;
                    const bool opposed=opposite(actual.tangentBasis.bitangent,target.bitangent) ||
                        opposite(actual.tangentBasis.tangent,target.tangent) ||
                        opposite(actual.tangentBasis.normal,target.normal);
                    oldOpposed+=opposed;
                    const bool b=selectedVector(actual.tangentBasis.bitangent,target.bitangent);
                    const bool t=selectedVector(actual.tangentBasis.tangent,target.tangent);
                    const bool n=selectedVector(actual.tangentBasis.normal,target.normal);
                    if(!b && !t && !n) continue;
                    for(const auto other:influences[row])
                        selected[other]|=(b || n?1u:0u)|(t || n?2u:0u);
                }
            if(singular) {
                bool chosen=false;
                for(std::size_t face=0;face<selected.size();++face) {
                    const auto& indices=existing.faces[face].indices;
                    const auto a=retained.generatedTexcoords[indices[0]],
                        b=retained.generatedTexcoords[indices[1]],c=retained.generatedTexcoords[indices[2]];
                    if(chosen || completed[face] ||
                       (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)!=0.0f) selected[face]=0u;
                    if(selected[face]) {chosen=true;completed[face]=true;}
                }
                if(!chosen) break;
            } else if(!oldOpposed) break;
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
            const auto faceDerivative=[&](std::size_t index,const std::array<std::array<double,2>,3>& precise,
                                          std::size_t component) {
                const auto& face=existing.faces[index];
                const auto a=source->generatedPositions[face.indices[0]],
                    b=source->generatedPositions[face.indices[1]],c=source->generatedPositions[face.indices[2]];
                const Vec3 p{b.x-a.x,b.y-a.y,b.z-a.z},q{c.x-a.x,c.y-a.y,c.z-a.z};
                std::array<Vec2,3> uv;
                for(std::size_t corner=0;corner<3u;++corner)
                    uv[corner]=source->generatedTexcoords[face.indices[corner]];
                return ascii::tangentUvDerivative(p,q,uv,precise,component);
            };
            const auto derivative=[&](const Search& search,double value) {
                auto precise=source->faceTexcoords64[search.face];
                precise[search.corner][search.component]=value;
                return faceDerivative(search.face,precise,search.component);
            };
            std::set<std::pair<std::size_t,std::array<std::uint32_t,6>>> evaluated;
            const auto normalized=[](Vec3 value) {
                const float length=std::sqrt((value.x*value.x+value.y*value.y)+value.z*value.z);
                if(static_cast<double>(length)<1e-9) return Vec3{1.0f,0.0f,0.0f};
                const float scale=1.0f/length;
                return Vec3{value.x*scale,value.y*scale,value.z*scale};
            };
            const auto remember=[&](std::size_t face,const std::array<std::array<double,2>,3>& precise) {
                const auto b=normalized(faceDerivative(face,precise,0u)),
                    t=normalized(faceDerivative(face,precise,1u));
                return evaluated.emplace(face,std::array<std::uint32_t,6>{
                    rawBits(b.x),rawBits(b.y),rawBits(b.z),rawBits(t.x),rawBits(t.y),rawBits(t.z)}).second;
            };
            for(std::size_t face=0;face<selected.size();++face)
                if(selected[face]) remember(face,source->faceTexcoords64[face]);
            bool accepted=false;
            auto candidate=existing;
            std::vector<std::vector<std::size_t>> contributors(existing.vertices.size()),affectedByFace(existing.faces.size());
            for(std::size_t row=0;row<writers.size();++row) if(writers[row]) {
                const auto writer=topology.writers[row],group=topology.vertexPositions[row];
                contributors[row].push_back(writer);
                for(auto at=topology.offsets[group];at<topology.offsets[group+1u];++at) {
                    const auto other=topology.incident[at].first;
                    if(other!=writer && (source->faceSmoothing[writer]&source->faceSmoothing[other]))
                        contributors[row].push_back(other);
                }
                for(const auto face:contributors[row]) affectedByFace[face].push_back(row);
            }
            const auto faceBasis=[&](std::size_t face,const std::array<std::array<double,2>,3>& precise) {
                TangentBasis basis;
                basis.bitangent=normalized(faceDerivative(face,precise,0u));
                basis.tangent=normalized(faceDerivative(face,precise,1u));
                basis.normal=ascii::tangentBasisNormal(basis.bitangent,basis.tangent,{});
                const auto n=source->firstCornerNormals[face];
                if((basis.normal.x*n.x+basis.normal.y*n.y)+basis.normal.z*n.z<0.0f) {
                    basis.tangent={-basis.tangent.x,-basis.tangent.y,-basis.tangent.z};
                    basis.normal={-basis.normal.x,-basis.normal.y,-basis.normal.z};
                }
                return basis;
            };
            std::vector<TangentBasis> rawBases(existing.faces.size());
            const auto updateRaw=[&] {
                for(std::size_t face=0;face<rawBases.size();++face)
                    rawBases[face]=faceBasis(face,source->faceTexcoords64[face]);
            };
            updateRaw();
            auto cachedSource=source;
            auto candidateBases=rawBases;
            ascii::orientSingularTangentComponents(topology.singular,candidateBases,out.job());
            const auto considerPrecise=[&](std::size_t changedFace,
                                           const std::array<std::array<double,2>,3>& precise) {
                out.job().check();
                if(!remember(changedFace,precise)) return false;
                auto proposed=std::make_shared<AsciiTangentSource>(*source);
                proposed->faceTexcoords64[changedFace]=precise;
                candidate.asciiTangentSource=proposed;
                if(cachedSource!=source) {updateRaw();cachedSource=source;}
                auto proposedBases=rawBases;
                proposedBases[changedFace]=faceBasis(changedFace,precise);
                ascii::orientSingularTangentComponents(topology.singular,proposedBases,out.job());
                std::vector<std::size_t> affected;
                for(std::size_t face=0;face<proposedBases.size();++face)
                    if(!same(candidateBases[face].bitangent,proposedBases[face].bitangent) ||
                       !same(candidateBases[face].tangent,proposedBases[face].tangent) ||
                       !same(candidateBases[face].normal,proposedBases[face].normal))
                        affected.insert(affected.end(),affectedByFace[face].begin(),affectedByFace[face].end());
                std::sort(affected.begin(),affected.end());
                affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
                const auto add=[](Vec3 a,Vec3 b){return Vec3{a.x+b.x,a.y+b.y,a.z+b.z};};
                for(const auto row:affected) {
                    TangentBasis basis;
                    for(const auto face:contributors[row]) {
                        basis.bitangent=add(basis.bitangent,proposedBases[face].bitangent);
                        basis.tangent=add(basis.tangent,proposedBases[face].tangent);
                        basis.normal=add(basis.normal,proposedBases[face].normal);
                    }
                    basis.bitangent=normalized(basis.bitangent);basis.tangent=normalized(basis.tangent);
                    basis.normal=normalized(basis.normal);basis.valid=true;
                    candidate.vertices[row].tangentBasis=basis;
                }
                candidateBases=std::move(proposedBases);
                candidate.mdxRowSize=rowSize;candidate.mdxWeightOffset=weightOffset;
                candidate.mdxBoneIndexOffset=boneOffset;candidate.mdxTangentOffset=tangentOffset;
                std::size_t opposed{};
                // One UV residual changes several basis vectors. Compare their
                // total loss while retaining the count of opposed directions.
                double oldError{},newError{};
                const auto compare=[&](Vec3 before,Vec3 after,Vec3 target) {
                    if(same(before,after)) return;
                    oldError+=difference(before,target);
                    newError+=difference(after,target);
                };
                for(std::size_t face=0;face<mesh.faces.size();++face)
                    for(std::size_t corner=0;corner<3u;++corner) {
                        const auto row=rows[index][face][corner];
                        const auto& before=existing.vertices[row].tangentBasis;
                        const auto& after=candidate.vertices[row].tangentBasis;
                        const auto& target=plan.vertices()[mesh.faces[face].indices[corner]].tangentBasis;
                        compare(before.bitangent,after.bitangent,target.bitangent);
                        compare(before.tangent,after.tangent,target.tangent);
                        compare(before.normal,after.normal,target.normal);
                        opposed+=opposite(after.bitangent,target.bitangent) ||
                            opposite(after.tangent,target.tangent) || opposite(after.normal,target.normal);
                    }
                if(!(newError<oldError) || opposed>oldOpposed) return false;
                if(plan.preciseFaceTexcoords.empty()) {
                    plan.preciseFaceTexcoords.resize(mesh.faces.size());
                    for(std::size_t face=0;face<mesh.faces.size();++face)
                        for(std::size_t corner=0;corner<3u;++corner) {
                            const auto value=plan.vertices()[mesh.faces[face].indices[corner]].texcoord0;
                            plan.preciseFaceTexcoords[face][corner]={value.x,value.y};
                        }
                }
                const auto original=plan.faceOrder[source->authoredFaceIndices[changedFace]];
                for(std::size_t corner=0;corner<3u;++corner)
                    plan.preciseFaceTexcoords[original][plan.sourceCorner(original,corner)]=
                        proposed->faceTexcoords64[changedFace][corner];
                existing=candidate;
                oldOpposed=opposed;
                return true;
            };
            const auto consider=[&](const Search& search,double value,const Search* paired=nullptr) {
                auto precise=source->faceTexcoords64[search.face];
                precise[search.corner][search.component]=value;
                if(paired) precise[paired->corner][paired->component]=paired->end;
                return considerPrecise(search.face,precise);
            };
            if(singular) {
                // Reconstruct the whole face-corner UV domain. Collapsed UVs
                // can need all three U or V residuals, or both derivatives,
                // to change together before the final basis improves.
                for(std::size_t face=0;face<selected.size();++face) {
                    if(!selected[face]) continue;
                    std::array<std::vector<std::array<double,3>>,2> derivatives;
                    for(std::size_t component=0;component<2u;++component) {
                        std::array<std::vector<double>,3> values;
                        for(std::size_t corner=0;corner<3u;++corner) {
                            const double value=source->faceTexcoords64[face][corner][component];
                            values[corner].push_back(value);
                            if(!(selected[face]&(1u<<component))) continue;
                            const float rounded=static_cast<float>(value);
                            values[corner].push_back(double(rounded));
                            for(const float direction:{-std::numeric_limits<float>::infinity(),
                                                        std::numeric_limits<float>::infinity()}) {
                                if(rounded==0.0f && std::signbit(rounded)!=std::signbit(direction)) continue;
                                const float neighbor=std::nextafter(rounded,direction);
                                const double edge=std::isinf(neighbor)?double(rounded)+
                                    (double(rounded)-std::nextafter(rounded,0.0f))*0.5:
                                    (double(rounded)+neighbor)*0.5;
                                values[corner].push_back(std::nextafter(edge,double(rounded)));
                            }
                        }
                        std::set<std::array<std::uint32_t,3>> seen;
                        for(const auto a:values[0]) for(const auto b:values[1]) for(const auto c:values[2]) {
                            const std::array<double,3> value{a,b,c};
                            auto precise=source->faceTexcoords64[face];
                            for(std::size_t corner=0;corner<3u;++corner) precise[corner][component]=value[corner];
                            const auto derivative=normalized(faceDerivative(face,precise,component));
                            if(seen.insert({rawBits(derivative.x),rawBits(derivative.y),rawBits(derivative.z)}).second)
                                derivatives[component].push_back(value);
                        }
                    }
                    for(const auto& u:derivatives[0]) for(const auto& v:derivatives[1]) {
                        std::array<std::array<double,2>,3> precise;
                        for(std::size_t corner=0;corner<3u;++corner) precise[corner]={u[corner],v[corner]};
                        accepted=considerPrecise(face,precise) || accepted;
                    }
                }
                // Rounded endpoints do not cover cancellation boundaries
                // inside a corner's float interval. Search those boundaries
                // from the best complete face domain retained above.
                if(accepted) source=std::make_shared<AsciiTangentSource>(*existing.asciiTangentSource);
                evaluated.clear();
                for(std::size_t face=0;face<selected.size();++face)
                    if(selected[face]) remember(face,source->faceTexcoords64[face]);
                accepted=false;
            }
            for(std::size_t face=0;face<existing.faces.size() && !accepted;++face) {
                if(!selected[face]) continue;
                for(std::size_t corner=0u;corner<3u && !accepted;++corner)
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
            // UV residuals can require two coordinates to move together.
            // Judge their combined basis when neither move improves alone.
            for(std::size_t a=0;a<searches.size() && !accepted;++a)
                for(std::size_t b=a+1u;b<searches.size() && !accepted;++b) {
                    const auto& first=searches[a];const auto& second=searches[b];
                    if(first.face==second.face && (first.corner!=second.corner || first.component!=second.component))
                        accepted=consider(first,first.end,&second) || accepted;
                }
            if(!accepted && !singular) break;
        }
    }
}

// Position and UV indices describe separate corner domains. Recover shared
// UV precision atomically across coincident corners so adjacent faces can
// improve together even when either residual alone worsens the final basis.
void reconstructTangentUvDomains(Writer& out,const Model& model,const Model& frames,
                                std::map<const Mesh*,MeshReconstruction>& plans) {
    if(std::none_of(model.meshes.begin(),model.meshes.end(),tangentPrecisionMesh)) return;
    auto current=frames;
    const auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
    measureTangentBasis(current,model,rows,plans,out.job());
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        return (x*x+y*y)+z*z;
    };
    const auto opposite=[](Vec3 a,Vec3 b) {
        return !same(a,b) && (a.x*b.x+a.y*b.y)+a.z*b.z<=0.0f;
    };
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];const auto found=plans.find(&mesh);
        if(found==plans.end() || !tangentPrecisionMesh(mesh)) continue;
        auto& plan=found->second;auto& existing=current.meshes[index];
        const auto topology=ascii::prepareTangentBasis(existing,out.job());
        auto source=std::make_shared<AsciiTangentSource>(*existing.asciiTangentSource);
        if(source->faceTexcoords64.empty())
            for(const auto& face:existing.faces) {
                std::array<std::array<double,2>,3> precise;
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto uv=source->generatedTexcoords[face.indices[corner]];
                    precise[corner]={uv.x,uv.y};
                }
                source->faceTexcoords64.push_back(precise);
            }
        const auto normalize=[](Vec3 value) {
            const float length=std::sqrt((value.x*value.x+value.y*value.y)+value.z*value.z);
            if(double(length)<1e-9) return Vec3{1.0f,0.0f,0.0f};
            const float scale=1.0f/length;return Vec3{value.x*scale,value.y*scale,value.z*scale};
        };
        const auto add=[](Vec3 a,Vec3 b){return Vec3{a.x+b.x,a.y+b.y,a.z+b.z};};
        std::vector<TangentBasis> targets;
        std::vector<std::vector<std::size_t>> cornersByRow(existing.vertices.size());
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                cornersByRow[rows[index][face][corner]].push_back(targets.size());
                targets.push_back(plan.vertices()[mesh.faces[face].indices[corner]].tangentBasis);
            }
        std::vector<double> errors(targets.size());
        std::vector<std::size_t> opposed(targets.size());
        const auto scoreRow=[&](std::size_t row,const TangentBasis& value,
                                auto& squared,auto& reversed) {
            for(const auto corner:cornersByRow[row]) {
                const auto& target=targets[corner];
                squared[corner]=difference(value.bitangent,target.bitangent)+difference(value.tangent,target.tangent)+
                    difference(value.normal,target.normal);
                reversed[corner]=opposite(value.bitangent,target.bitangent) || opposite(value.tangent,target.tangent) ||
                    opposite(value.normal,target.normal);
            }
        };
        std::vector<std::vector<std::size_t>> contributors(existing.vertices.size()),affectedByFace(existing.faces.size());
        for(std::size_t row=0;row<cornersByRow.size();++row) if(!cornersByRow[row].empty()) {
            scoreRow(row,existing.vertices[row].tangentBasis,errors,opposed);
            const auto writer=topology.writers[row],group=topology.vertexPositions[row];
            contributors[row].push_back(writer);
            for(auto at=topology.offsets[group];at<topology.offsets[group+1u];++at) {
                const auto other=topology.incident[at].first;
                if(other!=writer && (source->faceSmoothing[writer]&source->faceSmoothing[other]))
                    contributors[row].push_back(other);
            }
            for(const auto face:contributors[row]) affectedByFace[face].push_back(row);
        }
        const auto score=[&](const auto& squared,const auto& reversed) {
            // Keep the original source-corner addition order, including
            // repeated readers, so candidate comparisons remain identical.
            return std::make_pair(std::accumulate(squared.begin(),squared.end(),0.0),
                std::accumulate(reversed.begin(),reversed.end(),std::size_t{}));
        };
        auto best=score(errors,opposed);
        auto trialErrors=errors;auto trialOpposed=opposed;
        const auto faceBasis=[&](std::size_t face) {
            const auto& indices=existing.faces[face].indices;
            const auto a=source->generatedPositions[indices[0]],b=source->generatedPositions[indices[1]],
                c=source->generatedPositions[indices[2]];
            const Vec3 p{b.x-a.x,b.y-a.y,b.z-a.z},q{c.x-a.x,c.y-a.y,c.z-a.z};
            const std::array<Vec2,3> uv{source->generatedTexcoords[indices[0]],
                source->generatedTexcoords[indices[1]],source->generatedTexcoords[indices[2]]};
            TangentBasis basis;
            basis.bitangent=normalize(ascii::tangentUvDerivative(p,q,uv,source->faceTexcoords64[face],0u));
            basis.tangent=normalize(ascii::tangentUvDerivative(p,q,uv,source->faceTexcoords64[face],1u));
            basis.normal=ascii::tangentBasisNormal(basis.bitangent,basis.tangent,{});
            const auto n=source->firstCornerNormals[face];
            if((basis.normal.x*n.x+basis.normal.y*n.y)+basis.normal.z*n.z<0.0f) {
                basis.tangent={-basis.tangent.x,-basis.tangent.y,-basis.tangent.z};
                basis.normal={-basis.normal.x,-basis.normal.y,-basis.normal.z};
            }
            return basis;
        };
        std::vector<TangentBasis> rawBases;
        for(std::size_t face=0;face<existing.faces.size();++face) rawBases.push_back(faceBasis(face));
        auto bases=rawBases;
        ascii::orientSingularTangentComponents(topology.singular,bases,out.job());
        std::vector<std::size_t> writers(existing.vertices.size());
        for(std::size_t face=0;face<existing.faces.size();++face)
            for(const auto row:existing.faces[face].indices) writers[row]=face;
        std::vector<bool> selected(existing.faces.size()),normalSelected(existing.faces.size());
        constexpr double rounding=8.0*std::numeric_limits<float>::epsilon();
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto row=rows[index][face][corner];
                const auto& actual=existing.vertices[row];
                const auto& target=plan.vertices()[mesh.faces[face].indices[corner]].tangentBasis;
                const auto& value=actual.tangentBasis;
                if(difference(value.bitangent,target.bitangent)+difference(value.tangent,target.tangent)+
                   difference(value.normal,target.normal)<=rounding*rounding) continue;
                const bool normalError=difference(value.normal,target.normal)>rounding*rounding;
                const auto writer=writers[row];selected[writer]=true;
                normalSelected[writer]=normalSelected[writer] || normalError;
                for(std::size_t other=0;other<existing.faces.size();++other)
                    if(source->faceSmoothing[writer]&source->faceSmoothing[other])
                        for(const auto vertex:existing.faces[other].indices)
                            if(same(source->generatedPositions[vertex],actual.position)) {
                                selected[other]=true;
                                normalSelected[other]=normalSelected[other] || normalError;
                            }
            }
        using Corner=std::pair<std::size_t,std::size_t>;
        std::map<std::array<std::uint32_t,5>,std::vector<Corner>> domains;
        for(std::size_t face=0;face<existing.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto row=existing.faces[face].indices[corner];
                const auto p=source->generatedPositions[row];const auto uv=source->generatedTexcoords[row];
                domains[{bits(p.x),bits(p.y),bits(p.z),rawBits(uv.x),rawBits(uv.y)}].emplace_back(face,corner);
            }
        struct UvSearch {
            std::vector<Corner> corners;
            std::optional<std::size_t> component;
        };
        std::vector<UvSearch> searches;
        for(const auto& entry:domains)
            if(entry.second.size()>1u && std::any_of(entry.second.begin(),entry.second.end(),
               [&](const auto& corner){return selected[corner.first];})) searches.push_back({entry.second,{}});
        // A shared UV domain and a single face corner have different effects
        // on cancellation. Refine both without changing their packed UVs.
        for(std::size_t face=0;face<selected.size();++face) if(selected[face])
            for(std::size_t corner=0;corner<3u;++corner) searches.push_back({{{face,corner}}, {}});
        // Equal UV coordinates can belong to different geometric vertices.
        // Move those corners together to retain a collapsed UV domain while
        // recovering its derivative's rounding boundaries.
        for(std::size_t face=0;face<normalSelected.size();++face) if(normalSelected[face])
            for(std::size_t component=0;component<2u;++component) {
                std::map<float,std::vector<Corner>> groups;
                for(std::size_t corner=0;corner<3u;++corner) {
                    const auto uv=source->generatedTexcoords[existing.faces[face].indices[corner]];
                    groups[component?uv.y:uv.x].emplace_back(face,corner);
                }
                for(auto& entry:groups) searches.push_back({std::move(entry.second),component});
            }
        for(const auto& search:searches) {
            const auto& corners=search.corners;
            std::set<std::size_t> affected;
            for(const auto& corner:corners) affected.insert(corner.first);
            const auto signature=[&](const AsciiTangentSource& proposed) {
                std::vector<std::uint32_t> key;
                for(const auto face:affected) {
                    const auto& indices=existing.faces[face].indices;
                    const auto a=proposed.generatedPositions[indices[0]],b=proposed.generatedPositions[indices[1]],
                        c=proposed.generatedPositions[indices[2]];
                    const Vec3 p{b.x-a.x,b.y-a.y,b.z-a.z},q{c.x-a.x,c.y-a.y,c.z-a.z};
                    const std::array<Vec2,3> uv{proposed.generatedTexcoords[indices[0]],
                        proposed.generatedTexcoords[indices[1]],proposed.generatedTexcoords[indices[2]]};
                    for(std::size_t component=0;component<2u;++component) {
                        auto value=ascii::tangentUvDerivative(p,q,uv,proposed.faceTexcoords64[face],component);
                        const float length=std::sqrt((value.x*value.x+value.y*value.y)+value.z*value.z);
                        if(double(length)<1e-9) value={1.0f,0.0f,0.0f};
                        else {const auto scale=1.0f/length;value={value.x*scale,value.y*scale,value.z*scale};}
                        key.insert(key.end(),{rawBits(value.x),rawBits(value.y),rawBits(value.z)});
                    }
                }
                return key;
            };
            std::set<std::vector<std::uint32_t>> evaluated{signature(*source)};
            for(std::size_t component=0;component<2u;++component) {
                if(search.component && component!=*search.component) continue;
                std::set<double> values;
                for(const auto& corner:corners) values.insert(source->faceTexcoords64[corner.first][corner.second][component]);
                const float rounded=static_cast<float>(*values.begin());values.insert(double(rounded));
                for(const float direction:{-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity()}) {
                    if(rounded==0.0f && std::signbit(rounded)!=std::signbit(direction)) continue;
                    const float neighbor=std::nextafter(rounded,direction);
                    const double edge=std::isinf(neighbor)?double(rounded)+
                        (double(rounded)-std::nextafter(rounded,0.0f))*0.5:(double(rounded)+neighbor)*0.5;
                    values.insert(std::nextafter(edge,double(rounded)));
                }
                if(search.component) {
                    const auto face=corners.front().first;
                    const auto& indices=existing.faces[face].indices;
                    const auto a=source->generatedPositions[indices[0]],
                        b=source->generatedPositions[indices[1]],c=source->generatedPositions[indices[2]];
                    const std::array<float,3> p{b.x-a.x,b.y-a.y,b.z-a.z},q{c.x-a.x,c.y-a.y,c.z-a.z};
                    std::array<float,3> uv;
                    for(std::size_t corner=0;corner<3u;++corner) {
                        const auto value=source->generatedTexcoords[indices[corner]];
                        uv[corner]=component?value.y:value.x;
                    }
                    const auto product=[&](std::size_t axis,double value) {
                        std::array<double,3> precise;
                        for(std::size_t corner=0;corner<3u;++corner)
                            precise[corner]=source->faceTexcoords64[face][corner][component];
                        for(const auto& corner:corners) precise[corner.second]=value;
                        const auto other=axis<3u?1u:2u;
                        const float base=uv[other]-uv[0];
                        const double residual=(precise[other]-uv[other])-(precise[0]-uv[0]);
                        const double coefficient=residual==0.0?double(base):double(base)+residual;
                        return static_cast<float>((axis<3u?q[axis]:p[axis-3u])*coefficient);
                    };
                    const double begin=*values.begin(),end=*values.rbegin();
                    // Each rounded scalar product is monotone across this
                    // interval. Visit its transitions, including both sides,
                    // instead of sampling arbitrary decimal subdivisions.
                    for(std::size_t axis=0;axis<6u;++axis) {
                        double cursor=begin;
                        const auto last=product(axis,end);
                        while(product(axis,cursor)!=last) {
                            const auto initial=product(axis,cursor);
                            double low=cursor,high=end;
                            while(std::nextafter(low,high)!=high) {
                                const double middle=low+(high-low)*0.5;
                                if(product(axis,middle)==initial) low=middle;
                                else high=middle;
                            }
                            values.insert(low);values.insert(high);cursor=high;
                        }
                    }
                }
                for(const auto value:values) {
                    out.job().check();
                    std::vector<double> previous;
                    for(const auto& corner:corners) {
                        auto& coordinate=source->faceTexcoords64[corner.first][corner.second][component];
                        previous.push_back(coordinate);coordinate=value;
                    }
                    const auto restore=[&] {
                        for(std::size_t i=0;i<corners.size();++i)
                            source->faceTexcoords64[corners[i].first][corners[i].second][component]=previous[i];
                    };
                    if(!evaluated.insert(signature(*source)).second) {restore();continue;}
                    // UV edits leave topology and all unrelated face bases
                    // fixed. Propagate only changed contributions, including
                    // orientation changes across a singular component.
                    auto proposedBases=rawBases;
                    std::vector<TangentBasis> changedRaw;
                    for(const auto face:affected) {
                        const auto basis=faceBasis(face);changedRaw.push_back(basis);proposedBases[face]=basis;
                    }
                    ascii::orientSingularTangentComponents(topology.singular,proposedBases,out.job());
                    std::vector<std::size_t> affectedRows;
                    for(std::size_t face=0;face<bases.size();++face)
                        if(!same(bases[face].bitangent,proposedBases[face].bitangent) ||
                           !same(bases[face].tangent,proposedBases[face].tangent) ||
                           !same(bases[face].normal,proposedBases[face].normal))
                            affectedRows.insert(affectedRows.end(),affectedByFace[face].begin(),affectedByFace[face].end());
                    std::sort(affectedRows.begin(),affectedRows.end());
                    affectedRows.erase(std::unique(affectedRows.begin(),affectedRows.end()),affectedRows.end());
                    std::vector<TangentBasis> changedRows;
                    for(const auto row:affectedRows) {
                        TangentBasis basis;
                        for(const auto face:contributors[row]) {
                            basis.bitangent=add(basis.bitangent,proposedBases[face].bitangent);
                            basis.tangent=add(basis.tangent,proposedBases[face].tangent);
                            basis.normal=add(basis.normal,proposedBases[face].normal);
                        }
                        basis.bitangent=normalize(basis.bitangent);basis.tangent=normalize(basis.tangent);
                        basis.normal=normalize(basis.normal);basis.valid=true;
                        scoreRow(row,basis,trialErrors,trialOpposed);changedRows.push_back(basis);
                    }
                    const auto result=score(trialErrors,trialOpposed);
                    if(!(result.first<best.first) || result.second>best.second) {
                        restore();
                        for(const auto row:affectedRows) for(const auto corner:cornersByRow[row]) {
                            trialErrors[corner]=errors[corner];trialOpposed[corner]=opposed[corner];
                        }
                        continue;
                    }
                    if(plan.preciseFaceTexcoords.empty()) {
                        plan.preciseFaceTexcoords.resize(mesh.faces.size());
                        for(std::size_t face=0;face<mesh.faces.size();++face)
                            for(std::size_t corner=0;corner<3u;++corner) {
                                const auto uv=plan.vertices()[mesh.faces[face].indices[corner]].texcoord0;
                                plan.preciseFaceTexcoords[face][corner]={uv.x,uv.y};
                            }
                    }
                    for(const auto& corner:corners) {
                        const auto original=plan.faceOrder[source->authoredFaceIndices[corner.first]];
                        plan.preciseFaceTexcoords[original][plan.sourceCorner(original,corner.second)]=
                            source->faceTexcoords64[corner.first][corner.second];
                    }
                    for(std::size_t i=0;i<affectedRows.size();++i) {
                        const auto row=affectedRows[i];existing.vertices[row].tangentBasis=changedRows[i];
                        for(const auto corner:cornersByRow[row]) {
                            errors[corner]=trialErrors[corner];opposed[corner]=trialOpposed[corner];
                        }
                    }
                    std::size_t updated{};
                    for(const auto face:affected) rawBases[face]=changedRaw[updated++];
                    bases=std::move(proposedBases);existing.asciiTangentSource=source;best=result;
                }
            }
        }
    }
}

using TangentFit=std::array<std::array<double,2>,3>;
using TangentFitCache=std::map<std::vector<std::uint32_t>,std::optional<TangentFit>>;

// Recover UV residuals from the requested derivative directions, including
// directions hidden by cancellation of several incident face contributions.
void reconstructTangentDirections(Writer& out,const Model& model,const Model& frames,
                                   std::map<const Mesh*,MeshReconstruction>& plans,const Mesh* selectedMesh,std::size_t selectedFace,
                                   TangentFitCache& cache,bool independent=false,const TangentBasis* faceTarget=nullptr,bool interior=false,
                                   const Vec3* normalTarget=nullptr) {
    auto current=frames;
    const std::map<const Mesh*,MeshReconstruction> selectedPlans{{selectedMesh,plans.at(selectedMesh)}};
    const auto rows=generateGeometry(current,model,selectedPlans,false,out.advanced(),out.job(),false);
    measureTangentBasis(current,model,rows,selectedPlans,out.job());
    const auto normalize=[](Vec3 v) {
        const float length=std::sqrt((v.x*v.x+v.y*v.y)+v.z*v.z);
        if(double(length)<1e-9) return Vec3{1.0f,0.0f,0.0f};
        const float scale=1.0f/length;return Vec3{v.x*scale,v.y*scale,v.z*scale};
    };
    const auto add=[](Vec3 a,Vec3 b){return Vec3{a.x+b.x,a.y+b.y,a.z+b.z};};
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;
        constexpr double quantum=8.0*std::numeric_limits<float>::epsilon();
        // Score at float reconstruction resolution so equivalent errors
        // do not walk millions of authored UV residuals.
        return std::floor(((x*x+y*y)+z*z)/(quantum*quantum)+0.5);
    };
    const auto component=[](Vec3 v,std::size_t axis){return axis==0u?v.x:axis==1u?v.y:v.z;};
    const auto normalizedBounds=[&](Vec3 low,Vec3 high) {
        std::array<float,3> nearest{},farthest{};
        for(std::size_t i=0;i<3u;++i) {
            const auto l=component(low,i),h=component(high,i);
            nearest[i]=l<=0.0f && h>=0.0f?0.0f:std::min(std::abs(l),std::abs(h));
            farthest[i]=std::max(std::abs(l),std::abs(h));
        }
        const float minimum=std::sqrt((nearest[0]*nearest[0]+nearest[1]*nearest[1])+nearest[2]*nearest[2]);
        const float maximum=std::sqrt((farthest[0]*farthest[0]+farthest[1]*farthest[1])+farthest[2]*farthest[2]);
        if(double(maximum)<1e-9) return std::make_pair(Vec3{1.0f,0.0f,0.0f},Vec3{1.0f,0.0f,0.0f});
        const float lower=1.0f/maximum,upper=1.0f/std::max(minimum,1e-9f);
        std::array<float,3> lo{},hi{};
        for(std::size_t i=0;i<3u;++i) {
            const auto l=component(low,i),h=component(high,i);
            const std::array<float,4> products{l*lower,l*upper,h*lower,h*upper};
            lo[i]=*std::min_element(products.begin(),products.end());
            hi[i]=*std::max_element(products.begin(),products.end());
            if(double(minimum)<1e-9) {
                const float fallback=i==0u?1.0f:0.0f;
                lo[i]=std::min(lo[i],fallback);hi[i]=std::max(hi[i],fallback);
            }
        }
        return std::make_pair(Vec3{lo[0],lo[1],lo[2]},Vec3{hi[0],hi[1],hi[2]});
    };
    const auto nearest=[](Vec3 wanted,const std::pair<Vec3,Vec3>& bounds) {
        return Vec3{std::clamp(wanted.x,bounds.first.x,bounds.second.x),
            std::clamp(wanted.y,bounds.first.y,bounds.second.y),
            std::clamp(wanted.z,bounds.first.z,bounds.second.z)};
    };
    {
        const auto index=static_cast<std::size_t>(selectedMesh-model.meshes.data());
        const auto& mesh=*selectedMesh;auto& plan=plans.at(selectedMesh);auto& existing=current.meshes[index];
        const auto topology=ascii::prepareTangentBasis(existing,out.job());
        auto source=std::make_shared<AsciiTangentSource>(*existing.asciiTangentSource);
        if(source->faceTexcoords64.empty()) for(const auto& face:existing.faces) {
            std::array<std::array<double,2>,3> precise;
            for(std::size_t c=0;c<3u;++c) {const auto uv=source->generatedTexcoords[face.indices[c]];precise[c]={uv.x,uv.y};}
            source->faceTexcoords64.push_back(precise);
        }
        const auto derivative=[&](std::size_t face,const std::array<std::array<double,2>,3>& precise,std::size_t axis) {
            const auto& ids=existing.faces[face].indices;
            const auto a=source->generatedPositions[ids[0]],b=source->generatedPositions[ids[1]],c=source->generatedPositions[ids[2]];
            const Vec3 p{b.x-a.x,b.y-a.y,b.z-a.z},q{c.x-a.x,c.y-a.y,c.z-a.z};
            return ascii::tangentUvDerivative(p,q,{source->generatedTexcoords[ids[0]],
                source->generatedTexcoords[ids[1]],source->generatedTexcoords[ids[2]]},precise,axis);
        };
        std::vector<std::vector<TangentBasis>> targets(existing.vertices.size());
        for(std::size_t f=0;f<mesh.faces.size();++f) for(std::size_t c=0;c<3u;++c)
            targets[rows[index][f][c]].push_back(plan.vertices()[mesh.faces[f].indices[c]].tangentBasis);
        std::vector<std::vector<std::size_t>> contributorsByRow(targets.size()),affectedByFace(existing.faces.size());
        for(std::size_t row=0;row<targets.size();++row) if(!targets[row].empty()) {
            const auto writer=topology.writers[row],group=topology.vertexPositions[row];
            auto& contributors=contributorsByRow[row];contributors.push_back(writer);
            for(auto at=topology.offsets[group];at<topology.offsets[group+1u];++at) {
                const auto other=topology.incident[at].first;
                if(other!=writer && (source->faceSmoothing[writer]&source->faceSmoothing[other])) contributors.push_back(other);
            }
            for(const auto face:contributors) affectedByFace[face].push_back(row);
        }
        const auto rowError=[&](std::size_t row,const TangentBasis& value) {
            if(independent && contributorsByRow[row].size()!=1u) return 0.0;
            double error{};
            for(const auto& target:targets[row]) error+=difference(value.bitangent,target.bitangent)+
                difference(value.tangent,target.tangent)+difference(value.normal,target.normal);
            return error;
        };
        std::vector<double> errors(targets.size());
        for(std::size_t row=0;row<targets.size();++row) errors[row]=rowError(row,existing.vertices[row].tangentBasis);
        const auto basisFor=[&](std::size_t face,const std::array<std::array<double,2>,3>& precise) {
            TangentBasis value;value.bitangent=normalize(derivative(face,precise,0u));
            value.tangent=normalize(derivative(face,precise,1u));
            value.normal=ascii::tangentBasisNormal(value.bitangent,value.tangent,{});
            const auto n=source->firstCornerNormals[face];
            if((value.normal.x*n.x+value.normal.y*n.y)+value.normal.z*n.z<0.0f) {
                value.tangent={-value.tangent.x,-value.tangent.y,-value.tangent.z};
                value.normal={-value.normal.x,-value.normal.y,-value.normal.z};
            }
            return value;
        };
        const auto faceError=[&](const TangentBasis& value) {
            return difference(value.bitangent,faceTarget->bitangent)+
                difference(value.tangent,faceTarget->tangent)+
                difference(normalize(value.normal),normalize(faceTarget->normal));
        };
        const auto retain=[&](const TangentFit& value) {
            if(plan.preciseFaceTexcoords.empty()) {
                plan.preciseFaceTexcoords.resize(mesh.faces.size());
                for(std::size_t f=0;f<mesh.faces.size();++f) for(std::size_t c=0;c<3u;++c) {
                    const auto uv=plan.vertices()[mesh.faces[f].indices[c]].texcoord0;
                    plan.preciseFaceTexcoords[f][c]={uv.x,uv.y};
                }
            }
            plan.preciseFaceTexcoords[selectedFace]=value;
        };
        std::size_t fittedFace{};
        while(plan.faceOrder[source->authoredFaceIndices[fittedFace]]!=selectedFace) ++fittedFace;
        auto fittedRows=affectedByFace[fittedFace];
        std::sort(fittedRows.begin(),fittedRows.end());
        fittedRows.erase(std::unique(fittedRows.begin(),fittedRows.end()),fittedRows.end());
        // Product cells depend only on the fixed packed UVs and position
        // edges. Reuse their witnesses while the residual fit converges.
        std::array<std::optional<std::vector<std::array<double,3>>>,2> productWitnesses;
        std::vector<std::uint32_t> cacheKey;
        std::vector<std::vector<std::uint32_t>> visitedFits;
        std::vector<TangentBasis> rawBases(existing.faces.size());
        for(std::size_t face=0;face<rawBases.size();++face) rawBases[face]=basisFor(face,source->faceTexcoords64[face]);
        auto normalFitTarget=rawBases[fittedFace];
        if(normalTarget) {
            normalFitTarget.normal=*normalTarget;
            faceTarget=&normalFitTarget;
        }
        std::vector<ascii::TangentComponent> affectedComponents;
        std::vector<bool> reoriented(rawBases.size());
        reoriented[fittedFace]=true;
        for(const auto& group:topology.singular)
            if((std::find(group.faces.begin(),group.faces.end(),fittedFace)!=group.faces.end() ||
                std::find(group.boundary.begin(),group.boundary.end(),fittedFace)!=group.boundary.end()) &&
               std::all_of(group.faces.begin(),group.faces.end(),
                   [&](std::size_t face){return face==fittedFace || same(rawBases[face].normal,Vec3{});})) {
                affectedComponents.push_back(group);
                for(const auto face:group.faces) reoriented[face]=true;
            }
        std::vector<std::size_t> reorientedFaces;
        for(std::size_t face=0;face<reoriented.size();++face)
            if(reoriented[face]) reorientedFaces.push_back(face);
        auto proposedBases=rawBases;
        for(;;) {
            double affectedError{};
            {
                const auto face=fittedFace;
                std::set<std::size_t> dependentFaces{face};
                std::set<std::size_t> dependentRows(affectedByFace[face].begin(),affectedByFace[face].end());
                for(const auto& group:topology.singular) {
                    const bool member=std::find(group.faces.begin(),group.faces.end(),face)!=group.faces.end();
                    const bool boundary=std::find(group.boundary.begin(),group.boundary.end(),face)!=group.boundary.end();
                    const bool change=(member && std::all_of(group.faces.begin(),group.faces.end(),
                        [&](std::size_t f){return f==face || same(rawBases[f].normal,Vec3{});})) ||
                        (boundary && std::all_of(group.faces.begin(),group.faces.end(),
                        [&](std::size_t f){return same(rawBases[f].normal,Vec3{});}));
                    if(change) {
                        dependentFaces.insert(group.faces.begin(),group.faces.end());
                        dependentFaces.insert(group.boundary.begin(),group.boundary.end());
                        for(const auto f:group.faces)
                            dependentRows.insert(affectedByFace[f].begin(),affectedByFace[f].end());
                    }
                }
                for(const auto row:dependentRows) {
                    dependentFaces.insert(contributorsByRow[row].begin(),contributorsByRow[row].end());
                    affectedError+=errors[row];
                }
                const bool coupled=std::any_of(dependentRows.begin(),dependentRows.end(),
                    [&](std::size_t row){return contributorsByRow[row].size()!=1u;});
                cacheKey={static_cast<std::uint32_t>(index),static_cast<std::uint32_t>(selectedFace),
                    independent && coupled?1u:0u,faceTarget?1u:0u};
                const auto vectorKey=[&](Vec3 value) {
                    cacheKey.insert(cacheKey.end(),{bits(value.x),bits(value.y),bits(value.z)});
                };
                const auto basisKey=[&](const TangentBasis& value) {
                    vectorKey(value.bitangent);vectorKey(value.tangent);vectorKey(value.normal);
                };
                if(faceTarget) basisKey(*faceTarget);
                vectorKey(source->firstCornerNormals[face]);
                for(std::size_t c=0;c<3u;++c) {
                    vectorKey(source->generatedPositions[existing.faces[face].indices[c]]);
                    for(const auto value:source->faceTexcoords64[face][c]) {
                        std::uint64_t word;std::memcpy(&word,&value,sizeof(word));
                        cacheKey.push_back(static_cast<std::uint32_t>(word));
                        cacheKey.push_back(static_cast<std::uint32_t>(word>>32u));
                    }
                }
                cacheKey.push_back(static_cast<std::uint32_t>(dependentFaces.size()));
                for(const auto f:dependentFaces) {cacheKey.push_back(static_cast<std::uint32_t>(f));basisKey(rawBases[f]);}
                for(const auto row:dependentRows) {
                    cacheKey.push_back(static_cast<std::uint32_t>(row));
                    cacheKey.push_back(static_cast<std::uint32_t>(contributorsByRow[row].size()));
                    for(const auto f:contributorsByRow[row]) cacheKey.push_back(static_cast<std::uint32_t>(f));
                    cacheKey.push_back(static_cast<std::uint32_t>(targets[row].size()));
                    for(const auto& target:targets[row]) basisKey(target);
                }
                const auto cached=cache.find(cacheKey);
                if(cached!=cache.end()) {
                    for(auto& key:visitedFits) cache.emplace(std::move(key),cached->second);
                    if(cached->second) retain(*cached->second);
                    return;
                }
                visitedFits.push_back(cacheKey);
            }
            auto bases=rawBases;
            ascii::orientSingularTangentComponents(topology.singular,bases,out.job());
            std::vector<std::array<std::vector<Vec3>,2>> hints(bases.size());
            std::vector<std::vector<Vec3>> normalHints(bases.size());
            if(faceTarget) {
                hints[fittedFace][0].push_back(faceTarget->bitangent);
                hints[fittedFace][1].push_back(faceTarget->tangent);
                normalHints[fittedFace].push_back(normalize(faceTarget->normal));
            }
            for(const auto row:fittedRows) {
                const auto& actual=existing.vertices[row].tangentBasis;
                for(const auto& target:targets[row]) {
                    if(difference(actual.bitangent,target.bitangent)+difference(actual.tangent,target.tangent)+
                       difference(actual.normal,target.normal)==0.0) continue;
                    const auto& contributors=contributorsByRow[row];
                    {
                        Vec3 sum{};std::map<std::size_t,std::size_t> counts;
                        for(const auto face:contributors) {sum=add(sum,bases[face].normal);++counts[face];}
                        const auto wanted=target.normal;
                        const double length=std::sqrt(double(wanted.x)*wanted.x+double(wanted.y)*wanted.y+double(wanted.z)*wanted.z);
                        const std::array<double,3> direction{wanted.x/length,wanted.y/length,wanted.z/length};
                        for(const auto& entry:counts) {
                            const auto face=entry.first;
                            if(face!=fittedFace) continue;
                            const double count=entry.second;
                            const auto value=bases[face].normal;
                            std::array<double,3> rest{};double projection{},squared{},radius{};
                            for(std::size_t c=0;c<3u;++c) {
                                rest[c]=double(component(sum,c))-count*component(value,c);
                                projection+=rest[c]*direction[c];squared+=rest[c]*rest[c];
                                radius+=count*count*double(component(value,c))*component(value,c);
                            }
                            const auto root=std::sqrt(std::max(0.0,projection*projection-squared+radius));
                            for(const double scale:{std::max(0.0,projection-root),std::max(0.0,projection+root)})
                                normalHints[face].push_back({float((scale*direction[0]-rest[0])/count),
                                    float((scale*direction[1]-rest[1])/count),float((scale*direction[2]-rest[2])/count)});
                            normalHints[face].push_back(wanted);
                        }
                    }
                    for(std::size_t axis=0;axis<2u;++axis) {
                        Vec3 sum{};std::map<std::size_t,std::size_t> counts;
                        for(const auto face:contributors) {sum=add(sum,axis?bases[face].tangent:bases[face].bitangent);++counts[face];}
                        const auto wanted=axis?target.tangent:target.bitangent;
                        const double length=std::sqrt(double(wanted.x)*wanted.x+double(wanted.y)*wanted.y+double(wanted.z)*wanted.z);
                        const std::array<double,3> direction{wanted.x/length,wanted.y/length,wanted.z/length};
                        for(const auto& entry:counts) {
                            const auto face=entry.first;
                            if(face!=fittedFace) continue;
                            const double count=entry.second;
                            const auto value=axis?bases[face].tangent:bases[face].bitangent;
                            std::array<double,3> rest{};double projection{},squared{};
                            for(std::size_t c=0;c<3u;++c) {
                                rest[c]=double(component(sum,c))-count*component(value,c);
                                projection+=rest[c]*direction[c];squared+=rest[c]*rest[c];
                            }
                            const auto root=std::sqrt(std::max(0.0,projection*projection-squared+count*count));
                            for(const double scale:{std::max(0.0,projection-root),std::max(0.0,projection+root)}) {
                                hints[face][axis].push_back({float((scale*direction[0]-rest[0])/count),
                                    float((scale*direction[1]-rest[1])/count),float((scale*direction[2]-rest[2])/count)});
                            }
                            hints[face][axis].push_back(wanted);
                        }
                    }
                }
            }
            // Repeated corners often request the same direction. Keep the
            // original order while solving each exact float direction once.
            const auto uniqueDirections=[&](std::vector<Vec3>& directions) {
                std::set<std::array<std::uint32_t,3>> seen;
                directions.erase(std::remove_if(directions.begin(),directions.end(),[&](Vec3 value) {
                    return !seen.insert({rawBits(value.x),rawBits(value.y),rawBits(value.z)}).second;
                }),directions.end());
            };
            for(auto& axis:hints[fittedFace]) uniqueDirections(axis);
            uniqueDirections(normalHints[fittedFace]);
            bool accepted=false;
            for(std::size_t face=0;face<hints.size();++face) {
                if(plan.faceOrder[source->authoredFaceIndices[face]]!=selectedFace ||
                   (hints[face][0].empty() && hints[face][1].empty())) continue;
                using Coordinates=std::array<double,3>;
                const auto originFit=source->faceTexcoords64[face];
                std::array<std::vector<Coordinates>,2> choices;
                std::array<std::vector<Vec3>,2> axes;
                const auto& ids=existing.faces[face].indices;
                const auto a=source->generatedPositions[ids[0]],b=source->generatedPositions[ids[1]],c=source->generatedPositions[ids[2]];
                const Vec3 p{b.x-a.x,b.y-a.y,b.z-a.z},q{c.x-a.x,c.y-a.y,c.z-a.z};
                for(std::size_t axis=0;axis<2u;++axis) {
                    std::set<std::array<std::uint32_t,3>> seen;
                    const auto remember=[&](Coordinates values) {
                        auto precise=source->faceTexcoords64[face];
                        for(std::size_t c=0;c<3u;++c) {
                            const auto uv=source->generatedTexcoords[ids[c]];
                            if(rawBits(static_cast<float>(values[c]))!=rawBits(axis?uv.y:uv.x)) return;
                            precise[c][axis]=values[c];
                        }
                        const auto value=normalize(derivative(face,precise,axis));
                        const auto rounded=normalize(value);
                        const Vec3 reverse{-rounded.x,-rounded.y,-rounded.z};
                        if(!faceTarget && !mesh.render) for(const auto row:affectedByFace[face])
                            if(contributorsByRow[row].size()==1u) for(const auto& target:targets[row]) {
                                const auto wanted=axis?target.tangent:target.bitangent;
                                const auto& current=existing.vertices[row].tangentBasis;
                                const auto actual=axis?current.tangent:current.bitangent;
                                const auto bound=std::max(1.0,difference(actual,wanted));
                                const auto error=axis?std::min(difference(rounded,wanted),difference(reverse,wanted)):
                                    difference(rounded,wanted);
                                if(error>bound) return;
                            }
                        if(seen.insert({bits(value.x),bits(value.y),bits(value.z)}).second) {
                            choices[axis].push_back(values);axes[axis].push_back(value);
                        }
                    };
                    Coordinates initial{},low{},lower{},upper{};
                    for(std::size_t c=0;c<3u;++c) {
                        initial[c]=source->faceTexcoords64[face][c][axis];
                        const auto uv=source->generatedTexcoords[ids[c]];const float rounded=axis?uv.y:uv.x;low[c]=rounded;
                        lower[c]=std::nextafter((double(rounded)+std::nextafter(rounded,-std::numeric_limits<float>::infinity()))*0.5,double(rounded));
                        upper[c]=std::nextafter((double(rounded)+std::nextafter(rounded,std::numeric_limits<float>::infinity()))*0.5,double(rounded));
                        if(rounded==0.0f) (std::signbit(rounded)?upper[c]:lower[c])=double(rounded);
                    }
                    for(const auto x:{initial[0],low[0],lower[0],upper[0]})
                        for(const auto y:{initial[1],low[1],lower[1],upper[1]})
                            for(const auto z:{initial[2],low[2],lower[2],upper[2]}) remember({x,y,z});
                    // Interior UV residuals can cross several product cells
                    // together even when every one-coordinate move is worse.
                    // Seed those combinations before the directional refinement.
                    const auto& faceUv=source->generatedTexcoords;
                    const auto ua=faceUv[ids[0]],ub=faceUv[ids[1]],uc=faceUv[ids[2]];
                    if(interior && (ub.x-ua.x)*(uc.y-ua.y)-(ub.y-ua.y)*(uc.x-ua.x)==0.0f) {
                        // A fixed low-discrepancy sequence covers unequal
                        // residuals without aligning all three coordinates on
                        // the same rounding boundaries.
                        static const auto seeds=[] {
                            std::array<Coordinates,512u> result;
                            const std::array<std::size_t,3> bases{2u,3u,5u};
                            for(std::size_t i=0;i<result.size();++i)
                                for(std::size_t c=0;c<3u;++c) {
                                    double value{},scale=1.0;
                                    for(auto digit=i+1u;digit;digit/=bases[c]) {
                                        scale/=bases[c];value+=double(digit%bases[c])*scale;
                                    }
                                    result[i][c]=value;
                                }
                            return result;
                        }();
                        for(const auto& seed:seeds)
                            remember({lower[0]+(upper[0]-lower[0])*seed[0],
                                lower[1]+(upper[1]-lower[1])*seed[1],
                                lower[2]+(upper[2]-lower[2])*seed[2]});
                    }
                    if(!interior && (!mesh.render || faceTarget || source->faceSmoothing[face]==0u) && low[1]==low[2] && low[0]!=low[1]) {
                        if(productWitnesses[axis]) {
                            for(const auto& witness:*productWitnesses[axis]) remember(witness);
                        } else {
                        if(mesh.render) productWitnesses[axis].emplace();
                        const auto rememberWitness=[&](Coordinates witness) {
                            if(mesh.render) productWitnesses[axis]->push_back(witness);
                            remember(witness);
                        };
                        // Equal UV corners leave two independently rounded
                        // products. Enumerate their cells together, retaining
                        // a shared first-corner residual that realizes both.
                        const float base=float(low[1])-float(low[0]);
                        const auto coefficients=[&](Vec3 vector,std::size_t corner) {
                            const double first=base+(lower[corner]-low[corner])-(upper[0]-low[0]);
                            const double last=base+(upper[corner]-low[corner])-(lower[0]-low[0]);
                            std::set<double> boundaries{first,last};
                            for(std::size_t axis=0;axis<3u;++axis) {
                                const auto product=[&](double value){return float(component(vector,axis)*value);};
                                double cursor=first;
                                while(product(cursor)!=product(last)) {
                                    out.job().check();const auto value=product(cursor);
                                    double inside=cursor,outside=last;
                                    while(std::nextafter(inside,outside)!=outside) {
                                        const double middle=inside+(outside-inside)*0.5;
                                        if(product(middle)==value) inside=middle;else outside=middle;
                                    }
                                    boundaries.insert(inside);boundaries.insert(outside);cursor=outside;
                                }
                            }
                            std::vector<std::pair<double,double>> result;
                            std::map<std::array<std::uint32_t,3>,std::size_t> cells;
                            const auto append=[&](double value) {
                                const auto key=std::array<std::uint32_t,3>{bits(float(vector.x*value)),
                                    bits(float(vector.y*value)),bits(float(vector.z*value))};
                                const auto entry=cells.emplace(key,result.size());
                                if(entry.second) result.push_back({value,value});
                                else {auto& cell=result[entry.first->second];cell.first=std::min(cell.first,value);cell.second=std::max(cell.second,value);}
                            };
                            auto previous=*boundaries.begin();append(previous);
                            for(const auto value:boundaries) {append(previous+(value-previous)*0.5);append(value);previous=value;}
                            return result;
                        };
                        const auto first=coefficients(q,1u),second=coefficients(p,2u);
                        // Prune product cells with the independent rounded-axis
                        // limits before enumerating their joint UV residuals.
                        struct ProductRange {std::size_t begin,end,left{},right{};Vec3 low,high;};
                        const double residualError=8.0*std::numeric_limits<double>::epsilon()*
                            std::max({std::abs(low[0]),std::abs(low[1]),std::abs(low[2])});
                        const auto productBounds=[&](Vec3 vector,double begin,double end) {
                            std::array<float,3> lo{},hi{};
                            for(std::size_t i=0;i<3u;++i) {
                                const auto a=float(component(vector,i)*(begin-residualError));
                                const auto b=float(component(vector,i)*(end+residualError));
                                lo[i]=std::min(a,b);hi[i]=std::max(a,b);
                            }
                            return std::make_pair(Vec3{lo[0],lo[1],lo[2]},Vec3{hi[0],hi[1],hi[2]});
                        };
                        std::vector<ProductRange> products;products.reserve(second.size()*2u);
                        const auto build=[&](const auto& self,std::size_t begin,std::size_t end)->std::size_t {
                            const auto id=products.size();const auto bounds=productBounds(p,second[begin].first,second[end-1u].second);
                            products.push_back({begin,end,0u,0u,bounds.first,bounds.second});
                            if(end-begin>1u) {
                                const auto middle=begin+(end-begin)/2u;
                                const auto left=self(self,begin,middle),right=self(self,middle,end);
                                products[id].left=left;products[id].right=right;
                            }
                            return id;
                        };
                        build(build,0u,second.size());
                        for(const auto& u:first) {
                            const auto ub=productBounds(q,u.first,u.second);
                            const auto visit=[&](const auto& self,std::size_t id)->void {
                                const auto& node=products[id];
                                auto bounds=normalizedBounds({ub.first.x-node.high.x,ub.first.y-node.high.y,ub.first.z-node.high.z},
                                    {ub.second.x-node.low.x,ub.second.y-node.low.y,ub.second.z-node.low.z});
                                bounds=normalizedBounds(bounds.first,bounds.second);
                                if(!faceTarget && !mesh.render) for(const auto row:affectedByFace[face])
                                    if(contributorsByRow[row].size()==1u) for(const auto& target:targets[row]) {
                                        const auto wanted=axis?target.tangent:target.bitangent;
                                        const auto& current=existing.vertices[row].tangentBasis;
                                        const auto actual=axis?current.tangent:current.bitangent;
                                        const Vec3 reverse{-wanted.x,-wanted.y,-wanted.z};
                                        const auto error=axis?std::min(difference(nearest(wanted,bounds),wanted),
                                            difference(nearest(reverse,bounds),reverse)):difference(nearest(wanted,bounds),wanted);
                                        if(error>std::max(1.0,difference(actual,wanted))) return;
                                    }
                                if(node.end-node.begin>1u) {self(self,node.left);self(self,node.right);return;}
                                const auto v=second[node.begin];
                                const double left=std::max({lower[0]-low[0],lower[1]-low[1]-(u.second-base),lower[2]-low[2]-(v.second-base)});
                                const double right=std::min({upper[0]-low[0],upper[1]-low[1]-(u.first-base),upper[2]-low[2]-(v.first-base)});
                                if(left>right) return;
                                const double origin=left+(right-left)*0.5;
                                const double ul=std::max(u.first,base+(lower[1]-low[1])-origin);
                                const double uh=std::min(u.second,base+(upper[1]-low[1])-origin);
                                const double vl=std::max(v.first,base+(lower[2]-low[2])-origin);
                                const double vh=std::min(v.second,base+(upper[2]-low[2])-origin);
                                const double uc=ul+(uh-ul)*0.5,vc=vl+(vh-vl)*0.5;
                                // Reassembling a residual can leave a tiny
                                // cancellation remainder outside a zero UV's
                                // float cell. Project the witness back into its
                                // exact cells before evaluating its derivatives.
                                rememberWitness({std::clamp(low[0]+origin,lower[0],upper[0]),
                                    std::clamp(low[1]+origin+(uc-base),lower[1],upper[1]),
                                    std::clamp(low[2]+origin+(vc-base),lower[2],upper[2])});
                            };
                            visit(visit,0u);
                        }
                        }
                    }
                    std::vector<Coordinates> origins{initial,low};
                    if(interior) for(std::size_t mask=0;mask<8u;++mask) {
                        Coordinates origin;
                        for(std::size_t c=0;c<3u;++c) origin[c]=(mask&(1u<<c))?upper[c]:lower[c];
                        if(std::find(origins.begin(),origins.end(),origin)==origins.end()) origins.push_back(origin);
                    }
                    for(const auto& origin:origins) {
                    const float firstBase=float(low[1])-float(low[0]),secondBase=float(low[2])-float(low[0]);
                    const double first=firstBase+(origin[1]-low[1])-(origin[0]-low[0]);
                    const double second=secondBase+(origin[2]-low[2])-(origin[0]-low[0]);
                    for(std::size_t c=0;c<3u;++c) {
                        std::set<double> values{origin[c],low[c],lower[c],upper[c]};
                        for(const auto target:hints[face][axis]) for(std::size_t i=0;i<3u;++i) for(std::size_t j=i+1u;j<3u;++j) {
                            const double A=double(component(q,i))*component(target,j)-double(component(q,j))*component(target,i);
                            const double B=double(component(p,i))*component(target,j)-double(component(p,j))*component(target,i);
                            const double slope=c==0u?B-A:c==1u?A:-B;
                            if(slope==0.0) continue;
                            const double value=origin[c]-(A*first-B*second)/slope;
                            if(value>=lower[c] && value<=upper[c]) values.insert(value);
                        }
                        for(const auto target:normalHints[face]) {
                            const double A=double(q.x)*target.x+double(q.y)*target.y+double(q.z)*target.z;
                            const double B=double(p.x)*target.x+double(p.y)*target.y+double(p.z)*target.z;
                            const double slope=c==0u?B-A:c==1u?A:-B;
                            if(slope==0.0) continue;
                            const double value=origin[c]-(A*first-B*second)/slope;
                            if(value>=lower[c] && value<=upper[c]) values.insert(value);
                        }
                        const auto state=[&](double value) {
                            auto precise=source->faceTexcoords64[face];
                            for(std::size_t k=0;k<3u;++k) precise[k][axis]=origin[k];
                            precise[c][axis]=value;
                            const auto result=derivative(face,precise,axis);
                            return std::array<std::uint32_t,3>{bits(result.x),bits(result.y),bits(result.z)};
                        };
                        const auto roots=values;
                        for(const auto value:roots) {
                            const auto original=state(value);
                            for(const auto end:{lower[c],upper[c]}) if(state(end)!=original) {
                                double inside=value,outside=end;
                                while(std::nextafter(inside,outside)!=outside) {
                                    const double middle=inside+(outside-inside)*0.5;
                                    if(state(middle)==original) inside=middle;else outside=middle;
                                }
                                values.insert(inside);values.insert(outside);
                            }
                        }
                        for(const auto value:values) {auto coordinates=origin;coordinates[c]=value;remember(coordinates);}
                    }
                    }
                }
                const auto& directRows=fittedRows;
                const bool changesBoundary=std::any_of(topology.singular.begin(),topology.singular.end(),
                    [&](const auto& group) {
                        return std::find(group.boundary.begin(),group.boundary.end(),face)!=group.boundary.end() &&
                            std::all_of(group.faces.begin(),group.faces.end(),[&](std::size_t f){return same(rawBases[f].normal,Vec3{});});
                    });
                double directError{};
                for(const auto row:directRows) directError+=errors[row];
                if(faceTarget) directError=faceError(rawBases[face]);
                double fixedBitangentError{};
                struct AxisRange {std::size_t begin,end,left{},right{};Vec3 low,high;};
                std::vector<std::size_t> axisOrder(axes[1].size());std::iota(axisOrder.begin(),axisOrder.end(),0u);
                std::vector<AxisRange> ranges;ranges.reserve(axes[1].size()*2u);
                const auto range=[&](const auto& self,std::size_t begin,std::size_t end)->std::size_t {
                    const auto id=ranges.size();Vec3 low=axes[1][axisOrder[begin]],high=low;
                    for(auto at=begin+1u;at<end;++at) {
                        const auto value=axes[1][axisOrder[at]];
                        low={std::min(low.x,value.x),std::min(low.y,value.y),std::min(low.z,value.z)};
                        high={std::max(high.x,value.x),std::max(high.y,value.y),std::max(high.z,value.z)};
                    }
                    ranges.push_back({begin,end,0u,0u,low,high});
                    if(end-begin>1u) {
                        std::size_t axis{};
                        for(std::size_t i=1;i<3u;++i)
                            if(component(high,i)-component(low,i)>component(high,axis)-component(low,axis)) axis=i;
                        const auto middle=begin+(end-begin)/2u;
                        std::nth_element(axisOrder.begin()+begin,axisOrder.begin()+middle,axisOrder.begin()+end,
                            [&](std::size_t u,std::size_t v) {
                                const auto x=component(axes[1][u],axis),y=component(axes[1][v],axis);
                                return x<y || (x==y && u<v);
                            });
                        const auto left=self(self,begin,middle),right=self(self,middle,end);
                        ranges[id].left=left;ranges[id].right=right;
                    }
                    return id;
                };
                range(range,0u,axes[1].size());
                const auto cannotImprove=[&](Vec3 bitangent,const AxisRange& node) {
                    if(!faceTarget && (changesBoundary || same(rawBases[face].normal,Vec3{}))) return false;
                    if(fixedBitangentError>=directError) return true;
                    std::array<float,3> low{},high{};
                    for(std::size_t i=0;i<3u;++i) {
                        const auto j=(i+1u)%3u,k=(i+2u)%3u;
                        const auto bj=component(bitangent,j),bk=component(bitangent,k);
                        low[i]=float(double(bj)*component(bj<0.0f?node.high:node.low,k)-
                            double(bk)*component(bk<0.0f?node.low:node.high,j));
                        high[i]=float(double(bj)*component(bj<0.0f?node.low:node.high,k)-
                            double(bk)*component(bk<0.0f?node.high:node.low,j));
                    }
                    // Zero or reversed crosses can change component orientation.
                    if(!faceTarget && low[0]<=0.0f && high[0]>=0.0f && low[1]<=0.0f && high[1]>=0.0f &&
                       low[2]<=0.0f && high[2]>=0.0f) return false;
                    const auto n=source->firstCornerNormals[face];
                    std::array<float,3> alignLow{},alignHigh{};
                    for(std::size_t i=0;i<3u;++i) {
                        const auto c=component(n,i);
                        alignLow[i]=c*(c<0.0f?high[i]:low[i]);alignHigh[i]=c*(c<0.0f?low[i]:high[i]);
                    }
                    const float alignMin=(alignLow[0]+alignLow[1])+alignLow[2],alignMax=(alignHigh[0]+alignHigh[1])+alignHigh[2];
                    auto tangentLow=node.low,tangentHigh=node.high;
                    std::array<float,3> tl{},th{};
                    for(std::size_t i=0;i<3u;++i) {
                        tl[i]=component(tangentLow,i);th[i]=component(tangentHigh,i);
                        if(alignMax<0.0f) {
                            const auto value=low[i];low[i]=-high[i];high[i]=-value;
                            const auto t=tl[i];tl[i]=-th[i];th[i]=-t;
                        } else if(alignMin<0.0f) {
                            const auto extent=std::max(std::abs(low[i]),std::abs(high[i]));low[i]=-extent;high[i]=extent;
                            const auto t=std::max(std::abs(tl[i]),std::abs(th[i]));tl[i]=-t;th[i]=t;
                        }
                    }
                    tangentLow={tl[0],tl[1],tl[2]};tangentHigh={th[0],th[1],th[2]};
                    const Vec3 normalLow{low[0],low[1],low[2]},normalHigh{high[0],high[1],high[2]};
                    if(faceTarget) return fixedBitangentError+
                        difference(nearest(faceTarget->tangent,{tangentLow,tangentHigh}),faceTarget->tangent)+
                        difference(nearest(normalize(faceTarget->normal),normalizedBounds(normalLow,normalHigh)),
                            normalize(faceTarget->normal))>=directError;
                    double bound=fixedBitangentError;
                    for(const auto row:directRows) {
                        if(independent && contributorsByRow[row].size()!=1u) continue;
                        Vec3 tl{},th{},nl{},nh{};
                        for(const auto f:contributorsByRow[row]) {
                            tl=add(tl,f==face?tangentLow:bases[f].tangent);
                            th=add(th,f==face?tangentHigh:bases[f].tangent);
                            nl=add(nl,f==face?normalLow:bases[f].normal);
                            nh=add(nh,f==face?normalHigh:bases[f].normal);
                        }
                        const auto tangent=normalizedBounds(tl,th),normal=normalizedBounds(nl,nh);
                        for(const auto& target:targets[row]) bound+=difference(nearest(target.tangent,tangent),target.tangent)+
                            difference(nearest(target.normal,normal),target.normal);
                    }
                    return bound>=directError;
                };
                const auto considerCoordinates=[&](const Coordinates& u,const Coordinates& v,Vec3 bitangent,Vec3 tangent) {
                    out.job().check();
                    TangentBasis newRaw;newRaw.bitangent=bitangent;newRaw.tangent=tangent;
                    newRaw.normal=ascii::tangentBasisNormal(newRaw.bitangent,newRaw.tangent,{});
                    const auto n=source->firstCornerNormals[face];
                    if((newRaw.normal.x*n.x+newRaw.normal.y*n.y)+newRaw.normal.z*n.z<0.0f) {
                        newRaw.tangent={-newRaw.tangent.x,-newRaw.tangent.y,-newRaw.tangent.z};
                        newRaw.normal={-newRaw.normal.x,-newRaw.normal.y,-newRaw.normal.z};
                    }
                    if(faceTarget && !(faceError(newRaw)<directError)) return false;
                    if(!faceTarget && !changesBoundary && !same(rawBases[face].normal,Vec3{}) && !same(newRaw.normal,Vec3{})) {
                        // A nonzero face normal keeps its singular component
                        // unoriented. Independent rows give an exact lower
                        // bound before copying or gathering the other rows.
                        double bound{};
                        const auto normal=normalize(newRaw.normal);
                        for(const auto row:directRows) {
                            if(contributorsByRow[row].size()==1u)
                                for(const auto& target:targets[row]) bound+=difference(normal,target.normal);
                        }
                        if(bound>=directError) return false;
                    }
                    const bool orientationChanged=changesBoundary || same(rawBases[face].normal,Vec3{}) || same(newRaw.normal,Vec3{});
                    auto affected=directRows;
                    if(orientationChanged) {
                        // Singular components are disjoint and their boundary
                        // faces are nonsingular. Reset only the components
                        // this face can reorient, retaining other row inputs.
                        for(const auto f:reorientedFaces) proposedBases[f]=rawBases[f];
                        proposedBases[face]=newRaw;
                        ascii::orientSingularTangentComponents(affectedComponents,proposedBases,out.job());
                        affected.clear();
                        for(const auto f:reorientedFaces)
                            if(!same(bases[f].bitangent,proposedBases[f].bitangent) ||
                               !same(bases[f].tangent,proposedBases[f].tangent) || !same(bases[f].normal,proposedBases[f].normal))
                                affected.insert(affected.end(),affectedByFace[f].begin(),affectedByFace[f].end());
                        std::sort(affected.begin(),affected.end());affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
                    }
                    const auto contribution=[&](std::size_t f)->const TangentBasis& {
                        return orientationChanged?(reoriented[f]?proposedBases[f]:bases[f]):f==face?newRaw:bases[f];
                    };
                    std::vector<TangentBasis> changed;std::vector<double> changedErrors;double before{},after{};
                    for(const auto row:affected) {
                        TangentBasis value;
                        for(const auto f:contributorsByRow[row]) {
                            const auto& basis=contribution(f);
                            value.bitangent=add(value.bitangent,basis.bitangent);
                            value.tangent=add(value.tangent,basis.tangent);
                            value.normal=add(value.normal,basis.normal);
                        }
                        value.bitangent=normalize(value.bitangent);value.tangent=normalize(value.tangent);
                        value.normal=normalize(value.normal);value.valid=true;changed.push_back(value);
                        const auto error=rowError(row,value);changedErrors.push_back(error);
                        before+=errors[row];after+=error;
                    }
                    if(faceTarget) {before=faceError(rawBases[face]);after=faceError(newRaw);}
                    if(!(after<before)) return false;
                    for(std::size_t c=0;c<3u;++c) source->faceTexcoords64[face][c]={u[c],v[c]};
                    if(plan.preciseFaceTexcoords.empty()) {
                        plan.preciseFaceTexcoords.resize(mesh.faces.size());
                        for(std::size_t f=0;f<mesh.faces.size();++f) for(std::size_t c=0;c<3u;++c) {
                            const auto uv=plan.vertices()[mesh.faces[f].indices[c]].texcoord0;
                            plan.preciseFaceTexcoords[f][c]={uv.x,uv.y};
                        }
                    }
                    const auto original=plan.faceOrder[source->authoredFaceIndices[face]];
                    for(std::size_t c=0;c<3u;++c)
                        plan.preciseFaceTexcoords[original][plan.sourceCorner(original,c)]=source->faceTexcoords64[face][c];
                    for(std::size_t i=0;i<affected.size();++i) {
                        existing.vertices[affected[i]].tangentBasis=changed[i];errors[affected[i]]=changedErrors[i];
                    }
                    affectedError+=after-before;
                    directError=0.0;for(const auto row:directRows) directError+=errors[row];
                    if(faceTarget) directError=faceError(newRaw);
                    rawBases[face]=newRaw;
                    if(orientationChanged) {
                        for(const auto f:reorientedFaces) bases[f]=proposedBases[f];
                    } else bases[face]=newRaw;
                    existing.asciiTangentSource=source;accepted=true;
                    return true;
                };
                const auto considerPair=[&](std::size_t ui,std::size_t vi) {
                    return considerCoordinates(choices[0][ui],choices[1][vi],axes[0][ui],axes[1][vi]);
                };
                for(std::size_t ui=0;ui<choices[0].size();++ui) {
                    fixedBitangentError=0.0;
                    if(faceTarget) fixedBitangentError=difference(axes[0][ui],faceTarget->bitangent);
                    else for(const auto row:directRows) {
                        if(independent && contributorsByRow[row].size()!=1u) continue;
                        Vec3 value{};
                        for(const auto f:contributorsByRow[row]) value=add(value,f==face?axes[0][ui]:bases[f].bitangent);
                        value=normalize(value);
                        for(const auto& target:targets[row]) fixedBitangentError+=difference(value,target.bitangent);
                    }
                    // Singular orientation can flip tangent contributions,
                    // but never bitangents. Their error alone bounds the full
                    // affected component even when its raw normal is zero.
                    if(!faceTarget && fixedBitangentError>=affectedError) continue;
                    const auto visit=[&](const auto& self,std::size_t id)->void {
                        const auto& node=ranges[id];
                        if(node.end-node.begin==1u) considerPair(ui,axisOrder[node.begin]);
                        else if(!cannotImprove(axes[0][ui],node)) {self(self,node.left);self(self,node.right);}
                    };
                    visit(visit,0u);
                }
                // Give coupled UV coordinates two settling passes per scalar
                // before extending their residual direction. Independent rows
                // and the final interior pass start from settled contributions
                // and can use the line search immediately.
                const auto fitted=source->faceTexcoords64[face];
                const bool settled=interior || visitedFits.size()>=2u*originFit.size()*originFit[0].size();
                if(mesh.render && fitted!=originFit && (settled ||
                    std::all_of(directRows.begin(),directRows.end(),
                        [&](std::size_t row){return contributorsByRow[row].size()==1u;}))) {
                    for(double scale=2.0;;scale*=2.0) {
                        auto precise=fitted;
                        bool retained=true;
                        for(std::size_t c=0;c<3u;++c) for(std::size_t axis=0;axis<2u;++axis) {
                            precise[c][axis]=originFit[c][axis]+scale*(fitted[c][axis]-originFit[c][axis]);
                            const auto uv=source->generatedTexcoords[ids[c]];
                            retained=retained && rawBits(static_cast<float>(precise[c][axis]))==rawBits(axis?uv.y:uv.x);
                        }
                        if(!retained) break;
                        const Coordinates u{precise[0][0],precise[1][0],precise[2][0]},
                            v{precise[0][1],precise[1][1],precise[2][1]};
                        considerCoordinates(u,v,normalize(derivative(face,precise,0u)),
                            normalize(derivative(face,precise,1u)));
                    }
                }
            }
            if(!mesh.render || !accepted) break;
        }
        const auto fitted=plan.preciseFaceTexcoords.empty()?std::nullopt:
            std::optional<TangentFit>{plan.preciseFaceTexcoords[selectedFace]};
        for(auto& key:visitedFits) cache.emplace(std::move(key),fitted);
    }
}

void reconstructCyclicTangentDirections(Writer& out,const Model& model,const Model& frames,
                                        std::map<const Mesh*,MeshReconstruction>& plans,bool interior=false) {
    auto current=frames;auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
    auto best=measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    constexpr double rounding=8.0*std::numeric_limits<float>::epsilon();
    const auto rank=[](double error) {return std::floor(std::sqrt(error)/(8.0*std::numeric_limits<float>::epsilon())+0.5);};
    TangentFitCache cache;
    const auto difference=[](Vec3 a,Vec3 b) {
        const double x=double(a.x)-b.x,y=double(a.y)-b.y,z=double(a.z)-b.z;return (x*x+y*y)+z*z;
    };
    const auto outsideTolerance=[](const TangentBasis& value,const TangentBasis& target) {
        const auto outside=[](Vec3 a,Vec3 b) {
            const bool zeroA=same(a,Vec3{}),zeroB=same(b,Vec3{});
            const double x=double(a.y)*b.z-double(a.z)*b.y;
            const double y=double(a.z)*b.x-double(a.x)*b.z;
            const double z=double(a.x)*b.y-double(a.y)*b.x;
            const double dot=(double(a.x)*b.x+double(a.y)*b.y)+double(a.z)*b.z;
            return zeroA!=zeroB || std::atan2(std::sqrt((x*x+y*y)+z*z),dot)>0.017453292519943295;
        };
        return outside(value.bitangent,target.bitangent) || outside(value.tangent,target.tangent) ||
            outside(value.normal,target.normal);
    };
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];const auto found=plans.find(&mesh);
        if(found==plans.end() || !tangentPrecisionMesh(mesh)) continue;
        std::set<std::size_t> selected;
        const auto& existing=current.meshes[index];const auto& source=*existing.asciiTangentSource;
        const auto topology=ascii::prepareTangentBasis(existing,out.job());
        std::vector<bool> collapsed(existing.faces.size());
        for(const auto& component:topology.singular)
            for(const auto face:component.faces) collapsed[face]=true;
        const auto select=[&](std::size_t face) {
            if(!interior || collapsed[face])
                selected.insert(found->second.faceOrder[source.authoredFaceIndices[face]]);
        };
        for(std::size_t f=0;f<mesh.faces.size();++f) for(std::size_t c=0;c<3u;++c) {
            const auto row=rows[index][f][c];
            const auto& value=existing.vertices[row].tangentBasis;
            const auto& target=found->second.vertices()[mesh.faces[f].indices[c]].tangentBasis;
            const auto error=difference(value.bitangent,target.bitangent)+difference(value.tangent,target.tangent)+
                difference(value.normal,target.normal);
            // Spend the final interior search on unresolved one-degree corners.
            if(interior?!outsideTolerance(value,target):error<=rounding*rounding) continue;
            const auto writer=topology.writers[row],group=topology.vertexPositions[row];
            select(writer);
            for(auto at=topology.offsets[group];at<topology.offsets[group+1u];++at) {
                const auto face=topology.incident[at].first;
                if(face!=writer && (source.faceSmoothing[writer]&source.faceSmoothing[face]))
                    select(face);
            }
        }
        const auto localError=[&](const auto& trial) {
            const std::map<const Mesh*,MeshReconstruction> changedPlans{{&mesh,trial.at(&mesh)}};
            auto geometry=frames;
            const auto generated=generateGeometry(geometry,model,changedPlans,false,out.advanced(),out.job(),false);
            return measureTangentBasis(geometry,model,generated,changedPlans,out.job());
        };
        auto localBest=localError(plans);
        for(const auto face:selected) for(;;) {
            const auto previous=best;
            const auto original=plans.at(&mesh).cornerRotations[face];
            for(std::size_t rotation=0;rotation<3u;++rotation) {
                auto trial=plans;
                trial.at(&mesh).cornerRotations[face]=static_cast<std::uint8_t>((original+rotation)%3u);
                reconstructTangentDirections(out,model,frames,trial,&mesh,face,cache,false,nullptr,interior);
                auto seeded=trial;
                reconstructTangentDirections(out,model,frames,seeded,&mesh,face,cache,true,nullptr,interior);
                if(seeded.at(&mesh).preciseFaceTexcoords!=trial.at(&mesh).preciseFaceTexcoords) {
                    for(const auto neighbor:selected) if(neighbor!=face) {
                        bool shares=false;
                        for(const auto a:mesh.faces[face].indices) for(const auto b:mesh.faces[neighbor].indices)
                            shares=shares || same(mesh.vertices[a].position,mesh.vertices[b].position);
                        if(shares) reconstructTangentDirections(out,model,frames,seeded,&mesh,neighbor,cache,false,nullptr,interior);
                    }
                    if(localError(seeded)<localError(trial)) trial=std::move(seeded);
                }
                if(interior) {
                    std::vector<Vec3> normalTargets;
                    for(const auto vertex:mesh.faces[face].indices) {
                        const auto value=trial.at(&mesh).vertices()[vertex].tangentBasis.normal;
                        if(std::none_of(normalTargets.begin(),normalTargets.end(),[&](Vec3 n){return same(n,value);}))
                            normalTargets.push_back(value);
                    }
                    std::vector<std::size_t> neighbors;
                    for(const auto neighbor:selected) if(neighbor!=face) {
                        bool shares=false;
                        for(const auto a:mesh.faces[face].indices) for(const auto b:mesh.faces[neighbor].indices)
                            shares=shares || same(mesh.vertices[a].position,mesh.vertices[b].position);
                        if(shares) neighbors.push_back(neighbor);
                    }
                    if(!neighbors.empty()) for(const auto& target:normalTargets) {
                        auto coordinated=trial;
                        reconstructTangentDirections(out,model,frames,coordinated,&mesh,face,cache,false,nullptr,true,&target);
                        if(coordinated.at(&mesh).preciseFaceTexcoords==trial.at(&mesh).preciseFaceTexcoords) continue;
                        for(const auto neighbor:neighbors)
                            reconstructTangentDirections(out,model,frames,coordinated,&mesh,neighbor,cache,false,nullptr,true);
                        reconstructTangentDirections(out,model,frames,coordinated,&mesh,face,cache,false,nullptr,true);
                        if(localError(coordinated)<localError(trial)) trial=std::move(coordinated);
                    }
                }
                const auto proposedError=localError(trial);
                if(!(proposedError<localBest)) continue;
                auto rebuilt=frames;auto generated=generateGeometry(rebuilt,model,trial,false,out.advanced(),out.job());
                const auto error=measureTangentBasis(rebuilt,model,generated,trial,out.job());
                if(!(mesh.render?error<best:rank(error)<rank(best)) || !preservesSkinChannels(model,generated,plans)) continue;
                processAsciiSkinSeams(rebuilt,out.job());
                bool retained=true;
                for(std::size_t i=0;i<model.meshes.size() && retained;++i) {
                    const auto entry=plans.find(&model.meshes[i]);
                    if(entry==plans.end() || !entry->second.hasNormals) continue;
                    for(std::size_t f=0;f<model.meshes[i].faces.size() && retained;++f) for(std::size_t c=0;c<3u;++c) {
                        const auto target=entry->second.vertices()[model.meshes[i].faces[f].indices[c]].normal;
                        const auto before=current.meshes[i].normalAt(rows[i][f][c]);
                        const auto after=rebuilt.meshes[i].normalAt(generated[i][f][c]);
                        retained=retained && difference(after,target)<=std::max(difference(before,target),rounding*rounding);
                        if(interior && (model.meshes[i].mdxBitmap&0x80u)) {
                            const auto& wanted=entry->second.vertices()[model.meshes[i].faces[f].indices[c]].tangentBasis;
                            const auto& oldBasis=current.meshes[i].vertices[rows[i][f][c]].tangentBasis;
                            const auto& newBasis=rebuilt.meshes[i].vertices[generated[i][f][c]].tangentBasis;
                            retained=retained && (!outsideTolerance(newBasis,wanted) || outsideTolerance(oldBasis,wanted));
                        }
                    }
                }
                if(retained) {
                    plans=std::move(trial);current=std::move(rebuilt);rows=std::move(generated);
                    best=error;localBest=proposedError;
                }
            }
            if(mesh.render || !(rank(best)<rank(previous))) break;
        }
    }
    // A collapsed UV pair can require two tangent orientations to change
    // together. Rank those moves using retained row sums, then recover actual
    // UV residuals and accept only their complete reconstructed result.
    if(!interior) {
    auto bestNormal=measureSeamNormals(current,model,rows,plans);
    const auto normalize=[](Vec3 v) {
        const float length=std::sqrt((v.x*v.x+v.y*v.y)+v.z*v.z);
        if(double(length)<1e-9) return Vec3{1.0f,0.0f,0.0f};
        const float scale=1.0f/length;return Vec3{v.x*scale,v.y*scale,v.z*scale};
    };
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];const auto found=plans.find(&mesh);
        if(found==plans.end() || !tangentPrecisionMesh(mesh)) continue;
        const auto& existing=current.meshes[index];const auto& source=*existing.asciiTangentSource;
        const auto topology=ascii::prepareTangentBasis(existing,out.job());
        auto bases=ascii::tangentFaceBases(existing,out.job());
        ascii::orientSingularTangentComponents(topology.singular,bases,out.job());
        std::vector<bool> singular(bases.size());
        for(const auto& component:topology.singular)
            for(const auto face:component.faces) singular[face]=true;
        std::vector<std::vector<Vec3>> targets(existing.vertices.size());
        for(std::size_t face=0;face<mesh.faces.size();++face) for(std::size_t c=0;c<3u;++c)
            targets[rows[index][face][c]].push_back(found->second.vertices()[mesh.faces[face].indices[c]].tangentBasis.tangent);
        std::vector<std::vector<std::size_t>> contributors(targets.size()),readers(bases.size());
        std::set<std::pair<std::size_t,std::size_t>> pairs;
        for(std::size_t row=0;row<targets.size();++row) if(!targets[row].empty()) {
            const auto writer=topology.writers[row],group=topology.vertexPositions[row];
            auto& faces=contributors[row];faces.push_back(writer);
            for(auto at=topology.offsets[group];at<topology.offsets[group+1u];++at) {
                const auto other=topology.incident[at].first;
                if(other!=writer && (source.faceSmoothing[writer]&source.faceSmoothing[other])) faces.push_back(other);
            }
            for(const auto face:faces) readers[face].push_back(row);
            for(const auto a:faces) if(singular[a])
                for(const auto b:faces) if(a<b && singular[b]) pairs.emplace(a,b);
        }
        const auto rowError=[&](std::size_t row,std::size_t first,std::size_t second) {
            Vec3 sum{};
            for(const auto face:contributors[row]) {
                auto v=bases[face].tangent;
                if(face==first || face==second) v={-v.x,-v.y,-v.z};
                sum={sum.x+v.x,sum.y+v.y,sum.z+v.z};
            }
            const auto value=normalize(sum);double error{};
            for(const auto target:targets[row])
                error+=std::floor(difference(value,target)/(rounding*rounding)+0.5);
            return error;
        };
        std::vector<std::tuple<double,std::size_t,std::size_t>> ranked;
        for(const auto& pair:pairs) {
            auto affected=readers[pair.first];
            affected.insert(affected.end(),readers[pair.second].begin(),readers[pair.second].end());
            std::sort(affected.begin(),affected.end());affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
            double before{},after{};
            for(const auto row:affected) {
                before+=rowError(row,bases.size(),bases.size());
                after+=rowError(row,pair.first,pair.second);
            }
            if(after<before) ranked.emplace_back(after-before,pair.first,pair.second);
        }
        std::sort(ranked.begin(),ranked.end());
        std::vector<std::size_t> authored(bases.size());
        for(std::size_t face=0;face<bases.size();++face)
            authored[face]=found->second.faceOrder[source.authoredFaceIndices[face]];
        for(const auto& pair:ranked) {
            out.job().check();auto trial=plans;
            const std::array<std::size_t,2> faces{std::get<1>(pair),std::get<2>(pair)};
            for(const auto face:faces) {
                auto target=bases[face];target.tangent={-target.tangent.x,-target.tangent.y,-target.tangent.z};
                reconstructTangentDirections(out,model,frames,trial,&mesh,authored[face],cache,false,&target);
            }
            for(const auto face:faces)
                reconstructTangentDirections(out,model,frames,trial,&mesh,authored[face],cache);
            auto rebuilt=frames;
            const auto generated=generateGeometry(rebuilt,model,trial,false,out.advanced(),out.job());
            const auto error=measureTangentBasis(rebuilt,model,generated,trial,out.job());
            if(!(error<best) || !preservesSkinChannels(model,generated,plans)) continue;
            processAsciiSkinSeams(rebuilt,out.job());
            const auto normalError=measureSeamNormals(rebuilt,model,generated,trial);
            if(normalError<=bestNormal) {
                plans=std::move(trial);current=std::move(rebuilt);rows=generated;
                best=error;bestNormal=normalError;
            }
        }
    }
    }
    measureSeamNormals(current,model,rows,plans);
}

void reconstructRetainedSeamRows(Writer& out,const Model& model,const Model& frames,
                                std::map<const Mesh*,MeshReconstruction>& plans) {
    using Plans=std::map<const Mesh*,MeshReconstruction>;
    for(;;) {
        auto current=frames;
        const auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
        auto bestTangent=measureTangentBasis(current,model,rows,plans,out.job());
        AsciiSkinSeamCache seamCache;
        processAsciiSkinSeams(current,out.job(),seamCache);
        auto best=measureSeamNormals(current,model,rows,plans);
        const auto initial=std::make_pair(best,bestTangent);
        struct Candidate {const Mesh* mesh;Vec3 point;std::vector<std::uint32_t> masks;};
        std::vector<Candidate> candidates;
        for(std::size_t index=0;index<model.meshes.size();++index) {
            const auto& mesh=model.meshes[index];const auto found=plans.find(&mesh);
            if(found==plans.end() || mesh.animated || !found->second.hasNormals ||
               found->second.explicitNormals || mesh.asciiNormalPositionSource) continue;
            std::vector<bool> seamCorners;
            for(const auto& face:rows[index]) for(const auto row:face)
                seamCorners.push_back(!same(current.meshes[index].vertices[row].normal,
                    current.meshes[index].normalAt(row)));
            const auto& plan=found->second;
            const auto fitted=retainedRowSmoothingCandidates(mesh,plan.vertices(),plan.topology.sourceFaces,
                plan.smoothing.masks,seamCorners,out.job());
            Vec3 position{};Quaternion orientation{};
            auto ancestor=std::optional<std::size_t>{mesh.nodeIndex};
            while(ancestor) {
                const auto& node=frames.nodes[*ancestor];
                const auto rotated=ascii::engineModelRotateVector(node.orientation,position);
                position={rotated.x+node.position.x,rotated.y+node.position.y,rotated.z+node.position.z};
                orientation=ascii::engineModelQuaternionProduct(node.orientation,orientation);
                ancestor=node.parent;
            }
            for(const auto& candidate:fitted) {
                const auto point=ascii::engineModelRotateVector(orientation,plan.sourcePositions[candidate.source]);
                candidates.push_back({&mesh,{point.x+position.x,point.y+position.y,point.z+position.z},candidate.masks});
            }
        }
        const auto consider=[&](Plans trial) {
            auto rebuilt=frames;
            const auto generated=generateGeometry(rebuilt,model,trial,false,out.advanced(),out.job());
            if(!preservesSkinChannels(model,generated,trial)) return;
            const auto tangent=measureTangentBasis(rebuilt,model,generated,trial,out.job());
            processAsciiSkinSeams(rebuilt,out.job(),seamCache);
            const auto normal=measureSeamNormals(rebuilt,model,generated,trial);
            if(normal<=best && tangent<=bestTangent && (normal<best || tangent<bestTangent)) {
                best=normal;bestTangent=tangent;plans=std::move(trial);
            }
        };
        // Fit shared-row contributors after the existing precision and ordering
        // refinements. Their final seam output judges both additions and removals.
        for(const auto& candidate:candidates) {
            auto trial=plans;trial.at(candidate.mesh).smoothing.masks=candidate.masks;
            consider(std::move(trial));
        }
        // Both sides can require a different precursor together, even when
        // changing either mesh alone makes the completed seam result worse.
        std::stable_sort(candidates.begin(),candidates.end(),
            [](const Candidate& a,const Candidate& b){return a.point.x<b.point.x;});
        constexpr float tolerance=0.0010000000474974513f;
        for(std::size_t a=0;a<candidates.size();++a)
            for(std::size_t b=a+1u;b<candidates.size() && candidates[b].point.x-candidates[a].point.x<tolerance;++b) {
                const auto& first=candidates[a];const auto& second=candidates[b];
                if(first.mesh==second.mesh || !(std::abs(first.point.y-second.point.y)<tolerance) ||
                   !(std::abs(first.point.z-second.point.z)<tolerance)) continue;
                auto trial=plans;
                trial.at(first.mesh).smoothing.masks=first.masks;trial.at(second.mesh).smoothing.masks=second.masks;
                consider(std::move(trial));
            }
        if(std::make_pair(best,bestTangent)==initial) break;
    }
}

} // namespace

void reconstructSkinSeamSmoothing(Writer& out,const Model& model) {
    // Standard has no retained basis array. Its explicit final normals still
    // need the geometric smoothing inputs used before the skin-seam pass.
    const bool reconstructTangents=!out.advanced() && std::any_of(model.meshes.begin(),model.meshes.end(),
        [&](const Mesh& mesh) {
            return out.explicitNormals(mesh) && (mesh.mdxBitmap&0x80u) && !mesh.animated && !mesh.dangly;
        });
    if(!reconstructTangents && std::all_of(model.meshes.begin(),model.meshes.end(),
        [&](const Mesh& mesh) {return out.explicitNormals(mesh);})) return;
    if(std::none_of(model.meshes.begin(),model.meshes.end(),[](const Mesh& mesh){return mesh.skinned;})) {
        if(!reconstructTangents && !out.refineTangentRounding() && !out.refineNormalRounding() &&
           std::none_of(model.meshes.begin(),model.meshes.end(),tangentPrecisionMesh) &&
           std::none_of(model.meshes.begin(),model.meshes.end(),normalPrecisionMesh)) return;
        for(const auto& mesh:model.meshes) {
            const auto& node=model.nodes[mesh.nodeIndex];
            if(!node.lightsaber) out.meshReconstructions.emplace(&mesh,reconstructMesh(out,node,mesh,true,true,reconstructTangents));
        }
        auto frames=model;
        frames.animations.clear();
        for(const auto& entry:out.meshReconstructions) {
            auto& mesh=frames.meshes[entry.first-model.meshes.data()];
            mesh.vertices.clear();mesh.indices.clear();
        }
        if(!out.advanced()) reconstructExportedNodeFrames(frames,out.job());
        reconstructTangentSmoothing(out,model,frames,out.meshReconstructions);
        reconstructCyclicCornerPass(out,model,frames,out.meshReconstructions,nullptr,CornerSearch::Cancellation,true);
        reconstructFaceOrder(out,model,frames,out.meshReconstructions);
        reconstructTangentRounding(out,model,frames,out.meshReconstructions);
        reconstructNormalRoundingPass(out,model,frames,out.meshReconstructions);
        reconstructTangentUvPrecision(out,model,frames,out.meshReconstructions);
        reconstructNormalPositionPrecision(out,model,frames,out.meshReconstructions);
        reconstructCyclicCornerPass(out,model,frames,out.meshReconstructions,nullptr,CornerSearch::TangentAxes,true);
        reconstructCyclicCornerPass(out,model,frames,out.meshReconstructions,nullptr,CornerSearch::TangentOrder,false);
        reconstructTangentUvPrecision(out,model,frames,out.meshReconstructions,true);
        reconstructSeamSmoothing(out,model,frames,out.meshReconstructions);
        reconstructCyclicTangentDirections(out,model,frames,out.meshReconstructions);
        reconstructTangentSmoothing(out,model,frames,out.meshReconstructions);
        reconstructCyclicTangentDirections(out,model,frames,out.meshReconstructions,true);
        return;
    }
    using Plans=std::map<const Mesh*,MeshReconstruction>;
    const auto reconstruct=[&](bool refine) {
        Plans result;
        for(const auto& mesh:model.meshes) {
            const auto& node=model.nodes[mesh.nodeIndex];
            if(!node.lightsaber) result.emplace(&mesh,reconstructMesh(out,node,mesh,refine,false,reconstructTangents));
        }
        return result;
    };
    auto& plans=out.meshReconstructions;
    plans=reconstruct(false);
    auto frames=model;
    frames.animations.clear();
    // These channels are regenerated for every candidate. Keep their frame
    // metadata without copying the compiled vertex and index payloads.
    for(const auto& entry:plans) {
        auto& mesh=frames.meshes[entry.first-model.meshes.data()];
        mesh.vertices.clear();mesh.indices.clear();
    }
    if(!out.advanced()) reconstructExportedNodeFrames(frames,out.job());
    if(std::none_of(plans.begin(),plans.end(),[](const auto& entry) {
        return entry.second.hasNormals && !entry.second.explicitNormals && !entry.first->animated;
    })) {
        return;
    }
    enum class PrecursorFit { Smooth,SplitOpposing,Unscored,RefineExisting,RetainedVertices };
    AsciiSkinSeamCache seamCache;
    const auto precursor=[&](const Plans& source,bool refine,PrecursorFit fit=PrecursorFit::Smooth) {
        // Saved normals can be outputs of ProcessSkinSeams. Reconstruct
        // geometric precursors only where that producer changes a corner.
        auto smooth=frames;
        auto rows=generateGeometry(smooth,model,source,true,out.advanced(),out.job());
        processAsciiSkinSeams(smooth,out.job(),seamCache);
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
            const auto flatRows=generateGeometry(flat,model,flatPlans,false,out.advanced(),out.job());
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
            rows=generateGeometry(smooth,model,hybrid,false,out.advanced(),out.job());
            processAsciiSkinSeams(smooth,out.job(),seamCache);
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
                    targets.push_back(seam?before:plan.vertices()[mesh.faces[face].indices[corner]].normal);
                    ignored.push_back(seam);
                    adjusted|=seam;
                }
            if(adjusted) {
                const auto& topology=plan.topology;
                const bool ignoreSeams=fit==PrecursorFit::Unscored || fit==PrecursorFit::RefineExisting ||
                    fit==PrecursorFit::RetainedVertices;
                plan.smoothing=out.smoothing(mesh,plan.vertices(),topology.sourceVertices,
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
    // Shared compiled rows retain smoothing evidence from before seams made
    // distinct geometric normals equal. Score that precursor after seams.
    auto retained=plans;
    for(auto& entry:retained) {
        const auto& mesh=*entry.first;
        auto& plan=entry.second;
        if(mesh.animated || !plan.hasNormals || plan.explicitNormals || mesh.asciiNormalPositionSource) continue;
        const std::vector<bool> ignored(mesh.faces.size()*3u,true);
        plan.smoothing=out.smoothing(mesh,plan.vertices(),plan.topology.sourceVertices,
            plan.topology.sourceFaces,out.job(),nullptr,false,&ignored,nullptr,true);
    }
    alternatives.push_back(std::move(retained));

    auto baseline=frames;
    const auto baselineRows=generateGeometry(baseline,model,plans,false,out.advanced(),out.job());
    processAsciiSkinSeams(baseline,out.job(),seamCache);
    auto best=measureSeamNormals(baseline,model,baselineRows,plans);
    const auto consider=[&](Plans trial) {
        auto rebuilt=frames;
        const auto rows=generateGeometry(rebuilt,model,trial,false,out.advanced(),out.job());
        processAsciiSkinSeams(rebuilt,out.job(),seamCache);
        // NodeVertex equality excludes skin influences. Changed smoothing
        // may create new welds, which must still retain every corner's weights.
        if(!preservesSkinChannels(model,rows,plans)) return;
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

    double bestTangent{};
    reconstructTangentSmoothing(out,model,frames,plans);
    for(;;) {
        reconstructCyclicCorners(out,model,frames,plans);
        auto current=frames;
        auto rows=generateGeometry(current,model,plans,false,out.advanced(),out.job());
        bestTangent=measureTangentBasis(current,model,rows,plans,out.job());
        processAsciiSkinSeams(current,out.job(),seamCache);
        best=measureSeamNormals(current,model,rows,plans);
        std::vector<double> tangentErrors(model.meshes.size());
        for(std::size_t index=0;index<model.meshes.size();++index) {
            const auto& mesh=model.meshes[index];
            const auto found=plans.find(&mesh);
            if((mesh.mdxBitmap&0x80u) && !mesh.animated && !mesh.dangly && found!=plans.end())
                tangentErrors[index]=measureMeshTangents(current.meshes[index],mesh,rows[index],found->second);
        }
        auto rebuilt=current;
        auto generated=rows;
        std::vector<std::size_t> previousMeshes;
        bool refitted=false;
        const auto considerRefit=[&](Plans trial) {
            Plans changed;
            for(const auto& entry:trial)
                if(entry.second.smoothing.masks!=plans.at(entry.first).smoothing.masks ||
                   entry.second.cornerRotations!=plans.at(entry.first).cornerRotations)
                    changed.emplace(entry);
            // Reuse the trial model, restoring only meshes changed by the
            // preceding proposal. Tangents are local to each changed mesh.
            for(const auto index:previousMeshes) {
                rebuilt.meshes[index]=current.meshes[index];
                generated[index]=rows[index];
            }
            previousMeshes.clear();
            for(const auto& entry:changed) {
                const auto index=static_cast<std::size_t>(entry.first-model.meshes.data());
                rebuilt.meshes[index]=frames.meshes[index];
                previousMeshes.push_back(index);
            }
            const bool seamChanged=std::any_of(changed.begin(),changed.end(),
                [](const auto& entry){return entry.first->render;});
            const auto changedRows=generateGeometry(rebuilt,model,changed,false,out.advanced(),out.job(),seamChanged);
            for(const auto& entry:changed) {
                const auto index=static_cast<std::size_t>(entry.first-model.meshes.data());
                generated[index]=changedRows[index];
            }
            if(!preservesSkinChannels(model,generated,plans)) return;
            auto errors=tangentErrors;
            for(const auto& entry:changed) {
                const auto& mesh=*entry.first;
                if(!(mesh.mdxBitmap&0x80u) || mesh.animated || mesh.dangly) continue;
                const auto index=static_cast<std::size_t>(&mesh-model.meshes.data());
                rebuildAsciiTangentBasis(rebuilt.meshes[index],out.job());
                errors[index]=measureMeshTangents(rebuilt.meshes[index],mesh,generated[index],entry.second);
            }
            const auto tangentError=std::accumulate(errors.begin(),errors.end(),0.0);
            if(tangentError>bestTangent) return;
            if(seamChanged)
                processAsciiSkinSeams(rebuilt,out.job(),seamCache);
            else
                for(std::size_t index=0;index<model.meshes.size();++index)
                    if(model.meshes[index].render)
                        rebuilt.meshes[index].asciiNormalPool=current.meshes[index].asciiNormalPool;
            const auto normalError=measureSeamNormals(rebuilt,model,generated,trial);
            if(normalError<=best && tangentError<=bestTangent &&
               (normalError<best || tangentError<bestTangent)) {
                best=normalError;bestTangent=tangentError;
                plans=std::move(trial);refitted=true;
                current=rebuilt;rows=generated;tangentErrors=std::move(errors);
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
            const auto seamRows=generateGeometry(seams,model,plans,false,out.advanced(),out.job());
            processAsciiSkinSeams(seams,out.job(),seamCache);
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
                const auto candidates=sharedSeamSmoothingCandidates(mesh,found->second.vertices(),
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
                    auto coupled=sharedSeamSmoothingCandidates(mesh,found->second.vertices(),
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
                // Different bit assignments can describe the same joins.
                // Source-normal and tangent contributions only meet at
                // equal positions, so score each resulting relation once.
                std::map<std::array<std::uint32_t,3>,std::set<std::size_t>> incident;
                const auto& plan=found->second;
                for(std::size_t face=0;face<plan.topology.sourceFaces.size();++face)
                    for(const auto source:plan.topology.sourceFaces[face]) {
                        const auto p=plan.sourcePositions[source];
                        incident[{bits(p.x),bits(p.y),bits(p.z)}].insert(face);
                    }
                std::set<std::pair<std::size_t,std::size_t>> relations;
                for(const auto& entry:incident)
                    for(auto first=entry.second.begin();first!=entry.second.end();++first)
                        for(auto second=std::next(first);second!=entry.second.end();++second)
                            relations.emplace(*first,*second);
                std::set<std::vector<bool>> evaluated;
                const auto considerMasks=[&](const std::vector<std::uint32_t>& candidate) {
                    std::vector<bool> joined;
                    joined.reserve(relations.size());
                    for(const auto& relation:relations)
                        joined.push_back((candidate[relation.first]&candidate[relation.second])!=0u);
                    if(!evaluated.insert(std::move(joined)).second) return;
                    const auto previous=std::make_pair(best,bestTangent);
                    auto trial=plans;
                    trial.at(&mesh).smoothing.masks=candidate;
                    considerRefit(std::move(trial));
                    if(previous!=std::make_pair(best,bestTangent)) evaluated.clear();
                };
                // Complete contributor groups first, then retain accepted
                // joins as the remaining proposals are considered.
                std::vector<std::pair<std::size_t,const std::vector<std::uint32_t>*>> ordered;
                const auto& baselineMasks=plans.at(&mesh).smoothing.masks;
                for(const auto& candidate:masks) {
                    std::size_t changed{};
                    for(std::size_t face=0;face<candidate.size();++face)
                        changed+=candidate[face]!=baselineMasks[face];
                    ordered.emplace_back(changed,&candidate);
                }
                std::stable_sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) {
                    return a.first>b.first;
                });
                for(const auto& entry:ordered) {
                    const auto& candidate=*entry.second;
                    auto combined=candidate;
                    for(std::size_t face=0;face<combined.size();++face)
                        combined[face]|=plans.at(&mesh).smoothing.masks[face];
                    considerMasks(combined);
                    if(combined!=candidate) considerMasks(candidate);
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
        const bool tangentRefitted=reconstructTangentSmoothing(out,model,frames,plans);
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
                out.smoothing(mesh,plan.vertices(),plan.topology.sourceVertices,
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
                    auto refined=out.smoothing(mesh,plan.vertices(),plan.topology.sourceVertices,
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
    reconstructTangentUvPrecision(out,model,frames,plans,true);
    reconstructTangentUvDomains(out,model,frames,plans);
    reconstructNormalPositionPrecision(out,model,frames,plans);
    reconstructSeamSmoothing(out,model,frames,plans);
    reconstructTangentUvDomains(out,model,frames,plans);
    reconstructCyclicTangentDirections(out,model,frames,plans);
    reconstructTangentUvDomains(out,model,frames,plans);
    reconstructRetainedSeamRows(out,model,frames,plans);
    reconstructCyclicTangentDirections(out,model,frames,plans,true);
}

ReconstructedMesh writeMesh(Writer& out,const Model& model,const Node& node,const Mesh& mesh) {
    materialFields(out,model,node,mesh);
    if(out.advanced()) out.line("totalarea "+out.scalar(mesh.totalArea));
    if(out.advanced() && mesh.engineBounds) {
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
    const auto& vertices=reconstruction.vertices();
    auto& result=reconstruction.topology;
    const auto& reconstructed=reconstruction.smoothing;
    const auto* animation=node.animmesh?&*node.animmesh:nullptr;
    const bool hasNormals=reconstruction.hasNormals;
    // Reconstruction may have solved the geometric tangent inputs; output
    // normals remain the explicitly requested final values.
    reconstruction.explicitNormals=!animation && hasNormals && out.explicitNormals(mesh);
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
    if(out.advanced()) {
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
    } else {
        if(hasColors) {
            out.line("colors "+std::to_string(result.sourceVertices.size()));
            bool alpha=false;
            for(const auto index:result.sourceVertices) {
                const auto& color=vertices[index].color;
                out.line(out.vector({float(color[0])/255.0f,float(color[1])/255.0f,float(color[2])/255.0f}));
                alpha|=color[3]!=255u;
            }
            if(alpha) out.warning("Mesh "+node.name+": Standard ASCII colors do not retain vertex alpha; Advanced ASCII does.");
        }
        if(mesh.cpuPositions && !std::equal(mesh.cpuPositions->begin(),mesh.cpuPositions->end(),vertices.begin(),
            [](Vec3 position,const Vertex& vertex) {return same(position,vertex.position);}))
            out.warning("Mesh "+node.name+": Standard ASCII exports rendering positions; Advanced ASCII also retains independent CPU positions.");
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
