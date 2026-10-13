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
        out.warning("Mesh "+node.name+": engine triangle draw order differs from MDL face order; ASCII retains MDL face order");
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

MeshReconstruction reconstructMesh(Writer& out,const Node& node,const Mesh& mesh,bool refine=true) {
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
    if(mesh.skinned) {
        // Engine NodeVertex equality deliberately excludes skin influences.
        // Splitting a source vertex on weights alone cannot preserve two
        // otherwise identical generated vertices. Do not invent UV.z tags.
        std::map<std::vector<std::uint32_t>,std::size_t> generated;
        for(std::size_t i=0;i<vertices.size();++i) {
            out.job().check();
            const auto& vertex=vertices[i];
            std::vector<std::uint32_t> key;
            for(const auto value:{vertex.position.x,vertex.position.y,vertex.position.z,
                                  vertex.normal.x,vertex.normal.y,vertex.normal.z}) key.push_back(bits(value));
            for(std::size_t channel=0;channel<4u;++channel) {
                const auto value=uv(vertex,channel);
                key.push_back(bits(value.x));key.push_back(bits(value.y));
            }
            const auto inserted=generated.emplace(std::move(key),i);
            if(!inserted.second) {
                const auto& previous=skin.at(inserted.first->second);
                const auto& current=skin.at(i);
                if(previous.weights!=current.weights || previous.indices!=current.indices)
                    throw ModelError("Cannot export skin mesh "+node.name+
                        ": identical generated vertex attributes carry different weight/index channels, which engine ASCII vertex merging cannot preserve");
            }
        }
    }
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
    auto topologies=reconstructTopologies(mesh,vertices,sourceAttributes,out.job());
    std::size_t selected{};
    ReconstructedSmoothing reconstructed;
    double selectedSphereError{};
    for(std::size_t i=0;i<topologies.size();++i) {
        const auto& topology=topologies[i];
        ReconstructedSmoothing candidate;
        if(!animation && hasNormals)
            candidate=reconstructSmoothing(mesh,vertices,topology.sourceVertices,topology.sourceFaces,out.job(),nullptr,refine);
        else candidate.masks.assign(mesh.faces.size(),animation?1u:0u);
        const auto candidateSphereError=sphereError(mesh,vertices,topology);
        // Source identity is not retained uniquely. A point-join candidate
        // must improve a stored result without worsening the other one.
        // Explicit normals are independent of these source-position joins.
        if(i==0u || ((explicitNormals || candidate.squaredNormalError<=reconstructed.squaredNormalError) &&
                     candidateSphereError<=selectedSphereError &&
                     ((!explicitNormals && candidate.squaredNormalError<reconstructed.squaredNormalError) ||
                      candidateSphereError<selectedSphereError))) {
            selected=i;
            selectedSphereError=candidateSphereError;
            reconstructed=std::move(candidate);
        }
    }
    auto& topology=topologies[selected];
    return {std::move(vertices),{std::move(topology.sourceVertices),std::move(topology.sourceFaces)},
        std::move(reconstructed),topology.changedAdjacency,hasNormals,explicitNormals,
        std::vector<std::uint8_t>(mesh.faces.size(),0u)};
}

using CornerRows=std::vector<std::vector<std::array<std::int16_t,3>>>;

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
        ascii::RawNode raw;
        rebuilt.meshes[index].explicitNormals=plan.explicitNormals;
        raw.node=source.nodes[mesh.nodeIndex];
        const auto* animation=raw.node.animmesh?&*raw.node.animmesh:nullptr;
        const bool colors=mesh.hasVertexColors || (mesh.enginePrelightInput && (mesh.mdxBitmap&0x40u));
        for(const auto vertex:plan.topology.sourceVertices) {
            const auto& value=plan.vertices[vertex];
            raw.vertices.push_back(value.position);
            if(animation && plan.hasNormals) raw.normals.push_back(value.normal);
            if(colors) raw.colors.push_back({float(value.color[0])/255.0f,
                float(value.color[1])/255.0f,float(value.color[2])/255.0f});
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
        for(std::size_t face=0;face<mesh.faces.size();++face) {
            ascii::RawFace value;
            for(std::size_t corner=0;corner<3u;++corner)
                value.vertices[corner]=plan.topology.sourceFaces[face][plan.sourceCorner(face,corner)];
            value.smoothing=smooth && !animation && plan.hasNormals?1u:plan.smoothing.masks[face];
            value.material=mesh.faces[face].surfaceMaterial;
            for(auto& channel:value.texcoords)
                for(std::size_t corner=0;corner<3u;++corner)
                    channel[corner]=static_cast<std::uint32_t>(
                        mesh.faces[face].indices[plan.sourceCorner(face,corner)]);
            if(plan.explicitNormals) raw.normalIndices.push_back(value.texcoords[0]);
            raw.faces.push_back(value);
        }
        if(animation) {
            raw.samplePeriod=animation->samplePeriod;
            for(std::size_t frame=0;frame<animation->positionSampleCount;++frame)
                for(const auto vertex:plan.topology.sourceVertices)
                    raw.animatedVertices.push_back(animation->positionSamples.at(
                        vertex*animation->positionSampleCount+frame));
            for(std::size_t frame=0;frame<animation->texcoordSampleCount;++frame)
                for(std::size_t vertex=0;vertex<plan.vertices.size();++vertex) {
                    const auto value=animation->texcoordSamples.at(vertex*animation->texcoordSampleCount+frame);
                    raw.animatedTexcoords.push_back({value.x,value.y,0.0f});
                }
        }
        rows[index]=ascii::generateAsciiMeshGeometry(rebuilt,raw,rebuilt.meshes[index],job);
        for(std::size_t face=0;face<rows[index].size();++face) {
            const auto generated=rows[index][face];
            for(std::size_t corner=0;corner<3u;++corner)
                rows[index][face][plan.sourceCorner(face,corner)]=generated[corner];
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
        std::map<std::int16_t,std::size_t> first;
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto source=static_cast<std::size_t>(mesh.faces[face].indices[corner]);
                const auto inserted=first.emplace(rows[index][face][corner],source);
                const auto& previous=channels[inserted.first->second];
                const auto& current=channels[source];
                if(previous.weights!=current.weights || previous.indices!=current.indices) return false;
            }
    }
    return true;
}

double measureSeamNormals(const Model& rebuilt,const Model& source,const CornerRows& rows,
                         std::map<const Mesh*,MeshReconstruction>& plans) {
    double error{};
    for(std::size_t index=0;index<source.meshes.size();++index) {
        const auto& mesh=source.meshes[index];
        const auto found=plans.find(&mesh);
        if(found==plans.end() || !found->second.hasNormals) continue;
        auto& plan=found->second;
        auto& result=plan.smoothing;
        result.squaredNormalError=0.0;
        result.changedCornerNormals=0u;
        result.maxNormalComponentDifference=0.0f;
        for(std::size_t face=0;face<mesh.faces.size();++face)
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto target=plan.vertices[mesh.faces[face].indices[corner]].normal;
                const auto actual=rebuilt.meshes[index].normalAt(rows[index][face][corner]);
                const double x=double(actual.x)-target.x,y=double(actual.y)-target.y,z=double(actual.z)-target.z;
                result.squaredNormalError+=(x*x+y*y)+z*z;
                const float delta=std::max({std::abs(actual.x-target.x),std::abs(actual.y-target.y),
                    std::abs(actual.z-target.z)});
                result.changedCornerNormals+=delta!=0.0f;
                result.maxNormalComponentDifference=std::max(result.maxNormalComponentDifference,delta);
            }
        error+=result.squaredNormalError;
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

void reconstructCyclicCorners(Writer& out,const Model& model,const Model& frames,
                              std::map<const Mesh*,MeshReconstruction>& plans,
                              const Mesh* changedMesh=nullptr) {
    auto current=frames;
    auto rows=generateGeometry(current,model,plans,false,out.job());
    auto best=measureTangentBasis(current,model,rows,plans,out.job());
    processAsciiSkinSeams(current,out.job());
    best+=measureSeamNormals(current,model,rows,plans);
    std::vector<std::pair<const Mesh*,std::size_t>> candidates;
    std::vector<std::array<std::size_t,2>> pairs;
    for(std::size_t index=0;index<model.meshes.size();++index) {
        const auto& mesh=model.meshes[index];
        if(changedMesh && (changedMesh->render?!mesh.render:&mesh!=changedMesh)) continue;
        const auto found=plans.find(&mesh);
        if(found==plans.end() || mesh.animated || !found->second.hasNormals ||
           found->second.explicitNormals) continue;
        const auto& plan=found->second;
        std::vector<bool> cancelled(plan.topology.sourceVertices.size(),false);
        std::vector<bool> singular(mesh.faces.size(),false);
        std::vector<Vec3> cancelledTangents;
        const auto cancellationOutput=[](Vec3 normal) {
            return same(normal,{}) || same(normal,{1.0f,0.0f,0.0f});
        };
        const bool tangents=(mesh.mdxBitmap&0x80u) && !mesh.dangly;
        for(std::size_t face=0;face<mesh.faces.size();++face) {
            bool changedBasis=false;
            for(std::size_t corner=0;corner<3u;++corner) {
                const auto actual=current.meshes[index].normalAt(rows[index][face][corner]);
                const auto target=plan.vertices[mesh.faces[face].indices[corner]].normal;
                const auto source=plan.topology.sourceFaces[face][corner];
                if(!same(actual,target) && (cancellationOutput(actual) || cancellationOutput(target)))
                    cancelled[source]=true;
                if(tangents) {
                    const auto& generated=current.meshes[index].vertices[rows[index][face][corner]];
                    const auto& retained=plan.vertices[mesh.faces[face].indices[corner]].tangentBasis;
                    const auto inspect=[&](Vec3 value,Vec3 wanted) {
                        if(same(value,wanted)) return;
                        changedBasis=true;
                        if(cancellationOutput(value) || cancellationOutput(wanted))
                            cancelledTangents.push_back(generated.position);
                    };
                    inspect(generated.tangentBasis.bitangent,retained.bitangent);
                    inspect(generated.tangentBasis.tangent,retained.tangent);
                    inspect(generated.tangentBasis.normal,retained.normal);
                }
            }
            if(changedBasis) {
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
        std::vector<std::vector<std::size_t>> incident(cancelled.size());
        for(std::size_t face=0;face<mesh.faces.size();++face) {
            const auto& vertices=plan.topology.sourceFaces[face];
            if(singular[face] || std::any_of(vertices.begin(),vertices.end(),
                [&](auto vertex){return cancelled[vertex];})) {
                for(const auto vertex:vertices)
                    if(singular[face] || cancelled[vertex]) incident[vertex].push_back(candidates.size());
                candidates.emplace_back(&mesh,face);
            }
        }
        for(const auto& faces:incident)
            for(std::size_t first=0;first<faces.size();++first)
                for(std::size_t second=first+1u;second<faces.size();++second)
                    if(faces[first]!=faces[second]) pairs.push_back({faces[first],faces[second]});
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
            const auto consider=[&](std::map<const Mesh*,MeshReconstruction> trial) {
                std::map<const Mesh*,MeshReconstruction> changed;
                for(const auto& entry:trial)
                    if(entry.second.cornerRotations!=plans.at(entry.first).cornerRotations)
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
                auto error=measureTangentBasis(rebuilt,model,generated,trial,out.job(),&changed);
                if(std::any_of(changed.begin(),changed.end(),[](const auto& entry){return entry.first->render;}))
                    processAsciiSkinSeams(rebuilt,out.job());
                else {
                    // CreateSkinNodeList excludes non-rendered meshes. Changing
                    // only those leaves every ordered seam input unchanged.
                    for(std::size_t index=0;index<model.meshes.size();++index)
                        if(model.meshes[index].render)
                            rebuilt.meshes[index].asciiNormalPool=current.meshes[index].asciiNormalPool;
                }
                error+=measureSeamNormals(rebuilt,model,generated,trial);
                if(error<best) {
                    best=error;selected=std::move(trial);improved=true;
                    selectedGeometry=std::move(rebuilt);selectedRows=std::move(generated);
                }
            };
            for(const auto index:group.second) {
                const auto& candidate=candidates[index];
                for(std::uint8_t rotation=0u;rotation<3u;++rotation) {
                    if(plans.at(candidate.first).cornerRotations[candidate.second]==rotation) continue;
                    auto trial=plans;
                    trial.at(candidate.first).cornerRotations[candidate.second]=rotation;
                    consider(std::move(trial));
                }
            }
            // Opposing contributions can require two cyclic starts to move
            // together. Only pair affected faces incident to the same source.
            if(!improved) for(const auto& pair:pairs) {
                const auto& first=candidates[pair[0]];
                const auto& second=candidates[pair[1]];
                if((first.first->render?nullptr:first.first)!=group.first) continue;
                for(std::uint8_t a=0u;a<3u;++a)
                    for(std::uint8_t b=0u;b<3u;++b) {
                        if(plans.at(first.first).cornerRotations[first.second]==a ||
                           plans.at(second.first).cornerRotations[second.second]==b) continue;
                        auto trial=plans;
                        trial.at(first.first).cornerRotations[first.second]=a;
                        trial.at(second.first).cornerRotations[second.second]=b;
                        consider(std::move(trial));
                    }
            }
            if(improved) {
                plans=std::move(selected);current=std::move(selectedGeometry);rows=std::move(selectedRows);
            }
        } while(improved);
    }
}

} // namespace

void reconstructSkinSeamSmoothing(Writer& out,const Model& model) {
    if(std::none_of(model.meshes.begin(),model.meshes.end(),[](const Mesh& mesh){return mesh.skinned;})) return;
    using Plans=std::map<const Mesh*,MeshReconstruction>;
    const auto reconstruct=[&](bool refine) {
        Plans result;
        for(const auto& mesh:model.meshes) {
            const auto& node=model.nodes[mesh.nodeIndex];
            if(!node.lightsaber) result.emplace(&mesh,reconstructMesh(out,node,mesh,refine));
        }
        return result;
    };
    auto& plans=out.meshReconstructions;
    plans=reconstruct(false);
    if(std::none_of(plans.begin(),plans.end(),[](const auto& entry) {
        return entry.second.hasNormals && !entry.second.explicitNormals && !entry.first->animated;
    })) return;
    auto frames=model;
    reconstructExportedNodeFrames(frames,out.job());
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
                if(mesh.animated || !plan.hasNormals || plan.explicitNormals) continue;
                plan.smoothing.masks.assign(mesh.faces.size(),0u);
                hybrid.at(&mesh).smoothing.masks.assign(mesh.faces.size(),1u);
            }
            auto flat=frames;
            const auto flatRows=generateGeometry(flat,model,flatPlans,false,out.job());
            bool split=false;
            for(std::size_t index=0;index<model.meshes.size();++index) {
                const auto& mesh=model.meshes[index];
                const auto found=hybrid.find(&mesh);
                if(found==hybrid.end() || mesh.animated || !found->second.hasNormals || found->second.explicitNormals) continue;
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
            if(found==candidate.end() || mesh.animated || !found->second.hasNormals || found->second.explicitNormals) continue;
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
                       found->second.explicitNormals) continue;
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
            for(std::size_t index=0;index<model.meshes.size();++index) {
                const auto& mesh=model.meshes[index];
                const auto found=plans.find(&mesh);
                if(found==plans.end() || mesh.animated || !found->second.hasNormals ||
                   found->second.explicitNormals) continue;
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
                for(const auto& masks:candidates) {
                    auto trial=plans;
                    trial.at(&mesh).smoothing.masks=masks;
                    considerRefit(std::move(trial));
                }
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
                   found->second.explicitNormals) continue;
                const auto& plan=found->second;
                std::vector<std::vector<std::uint32_t>> candidates;
                reconstructSmoothing(mesh,plan.vertices,plan.topology.sourceVertices,
                    plan.topology.sourceFaces,out.job(),nullptr,false,nullptr,&plan.smoothing.masks,
                    false,nullptr,&plan.cornerRotations,&candidates);
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
}

ReconstructedMesh writeMesh(Writer& out,const Model& model,const Node& node,const Mesh& mesh) {
    materialFields(out,model,node,mesh);
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
    if(reconstructed.changedCornerNormals && !reconstruction.explicitNormals)
        out.warning("Mesh "+node.name+": reconstructed smoothing changes "+
            std::to_string(reconstructed.changedCornerNormals)+" corner normals; maximum component difference "+
            out.scalar(reconstructed.maxNormalComponentDifference));
    if(!hasNormals && !vertices.empty())
        out.warning("Mesh "+node.name+": no compiled normal channel exists; source connectivity is retained and face normals are generated");
    if(mesh.cpuPositions) {
        bool different=mesh.cpuPositions->size()!=vertices.size();
        for(std::size_t i=0;i<vertices.size() && !different;++i)
            different=!same(mesh.cpuPositions->at(i),vertices[i].position);
        if(different)
            out.warning("Mesh "+node.name+": the independent MDL CPU position pool differs from the rendering positions; ASCII has one source vertex pool and exports the rendering positions");
    }
    diagnoseTopology(out,node,mesh);
    out.line("verts "+std::to_string(result.sourceVertices.size()));
    for(const auto index:result.sourceVertices) out.line(out.vector(vertices[index].position));
    if(reconstruction.explicitNormals) {
        out.explicitNormalNote();
        out.line("normals "+std::to_string(vertices.size()));
        for(const auto& vertex:vertices) out.line(out.vector(vertex.normal));
        out.line("normalindices "+std::to_string(mesh.faces.size()));
        for(std::size_t i=0;i<mesh.faces.size();++i) {
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
        const auto count=vertices.size();
        out.line(std::string("tverts")+(channel?std::to_string(channel):std::string{})+" "+std::to_string(count));
        for(std::size_t i=0;i<count;++i) {
            const auto value=uv(vertices[i],channel);
            out.line(out.scalar(value.x)+" "+out.scalar(value.y)+" 0");
        }
        if(channel) {
            out.line("texindices"+std::to_string(channel)+" "+std::to_string(mesh.faces.size()));
            for(std::size_t i=0;i<mesh.faces.size();++i) {
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
    if(hasColors) {
        out.line("colors "+std::to_string(result.sourceVertices.size()));
        bool alpha=false;
        for(const auto index:result.sourceVertices) {
            const auto& color=vertices[index].color;
            out.line(out.vector({float(color[0])/255.0f,float(color[1])/255.0f,float(color[2])/255.0f}));
            alpha|=color[3]!=255u;
        }
        if(!animation)
            out.warning("Mesh "+node.name+": compiled RGB colors are exported for editing, but the engine ordinary/skin/dangly ASCII builders do not emit a compiled vertex-color channel");
        if(alpha) out.warning("Mesh "+node.name+": compiled vertex alpha has no colors-array component in the engine ASCII grammar");
    }
    out.line("faces "+std::to_string(mesh.faces.size()));
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        const auto& sourceFace=result.sourceFaces[i];
        const auto& compiled=mesh.faces[i];
        const auto a=reconstruction.sourceCorner(i,0u),b=reconstruction.sourceCorner(i,1u),
                   c=reconstruction.sourceCorner(i,2u);
        out.line(std::to_string(sourceFace[a])+" "+std::to_string(sourceFace[b])+" "+std::to_string(sourceFace[c])+" "+
            std::to_string(static_cast<std::int32_t>(smoothing[i]))+" "+
            std::to_string(static_cast<std::uint32_t>(compiled.indices[a]))+" "+
            std::to_string(static_cast<std::uint32_t>(compiled.indices[b]))+" "+
            std::to_string(static_cast<std::uint32_t>(compiled.indices[c]))+" "+
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
