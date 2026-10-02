#include <model/AsciiExportInternal.hpp>
#include <model/EngineCpuSkin.hpp>
#include <model/UvAnimation.hpp>

#include <algorithm>
#include <cstring>
#include <map>

namespace neomdl::model::ascii_export {
namespace {

bool same(Vec3 a,Vec3 b) {
    return a.x==b.x && a.y==b.y && a.z==b.z;
}
std::uint32_t bits(float value) {
    // Engine equality treats positive and negative zero as the same value.
    if(value==0.0f) return 0u;
    std::uint32_t result;
    std::memcpy(&result,&value,sizeof(result));
    return result;
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
            // Use enabled compiled channels directly. The inspection decoder may
            // have repaired an invalid UV coordinate.
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
    if(!mesh.texture0.empty()) out.line("texture0 "+out.token(mesh.texture0,"mesh "+node.name+" texture0"));
    if(!mesh.texture1.empty()) out.line("texture1 "+out.token(mesh.texture1,"mesh "+node.name+" texture1"));
    out.line("diffuse "+out.vector(mesh.diffuse));
    out.line("ambient "+out.vector(mesh.ambient));
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

} // namespace

ReconstructedMesh writeMesh(Writer& out,const Model& model,const Node& node,const Mesh& mesh) {
    materialFields(out,model,node,mesh);
    const auto vertices=sourceVertices(mesh,node,out);
    const auto* animation=node.animmesh?&*node.animmesh:nullptr;
    if(animation && (mesh.mdxBitmap&2u)==0u)
        throw ModelError("Cannot export animmesh "+node.name+": its static UV source pool is unavailable; engine authoring requires tverts even when animtverts is empty");
    if(mesh.skinned && !mesh.engineCpuSkin && !mesh.builtFloatSkinChannels)
        throw ModelError("Cannot export skin mesh "+node.name+": engine float weight/index channels are unavailable");
    const auto skin=mesh.skinned?decodeEngineFloatSkinChannels(mesh):std::vector<EngineFloatSkinChannels>{};
    const bool hasNormals=!mesh.enginePrelightInput || (mesh.mdxBitmap&0x20u)!=0u;
    const bool hasColors=mesh.hasVertexColors || (mesh.enginePrelightInput && (mesh.mdxBitmap&0x40u)!=0u);
    ReconstructedMesh result;
    std::vector<std::uint32_t> sourceIndex(vertices.size(),0xffffffffu);
    std::map<std::vector<std::uint32_t>,std::uint32_t> canonical;
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
    const auto source=[&](std::size_t index) {
        if(sourceIndex.at(index)!=0xffffffffu) return sourceIndex[index];
        const auto& vertex=vertices.at(index);
        std::vector<std::uint32_t> key;
        const auto append=[&](Vec3 v){key.push_back(bits(v.x));key.push_back(bits(v.y));key.push_back(bits(v.z));};
        append(vertex.position);
        if(hasNormals) append(vertex.normal);
        else key.push_back(static_cast<std::uint32_t>(index));
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
        if(node.lightsaber)
            for(std::size_t channel=0;channel<4u;++channel) {
                const auto value=uv(vertex,channel);
                key.push_back(bits(value.x));key.push_back(bits(value.y));
            }
        if(hasColors || node.lightsaber) {
            std::uint32_t color{};
            for(std::size_t channel=0;channel<4u;++channel)
                color|=std::uint32_t(vertex.color[channel])<<(channel*8u);
            key.push_back(color);
        }
        if(mesh.dangly) {
            key.push_back(bits(mesh.dangly->constraints.at(index)));
            if(mesh.dangly->restPositions) append(mesh.dangly->restPositions->at(index));
        }
        if(mesh.skinned) {
            for(const auto value:skin.at(index).weights) key.push_back(bits(value));
            for(const auto value:skin.at(index).indices) key.push_back(bits(value));
        }
        const auto inserted=canonical.emplace(std::move(key),static_cast<std::uint32_t>(result.sourceVertices.size()));
        if(inserted.second) result.sourceVertices.push_back(index);
        sourceIndex[index]=inserted.first->second;
        return sourceIndex[index];
    };
    for(const auto& face:mesh.faces) {
        out.job().check();
        Triangle indices{};
        for(std::size_t corner=0;corner<3u;++corner) {
            if(face.indices[corner]<0)
                throw ModelError("Cannot export mesh "+node.name+": a engine face has a negative vertex index");
            indices[corner]=source(static_cast<std::size_t>(face.indices[corner]));
        }
        result.sourceFaces.push_back(indices);
    }
    for(std::size_t i=0;i<vertices.size();++i) {out.job().check();source(i);}

    if(!animation)
        out.warning("Mesh "+node.name+": source topology and smoothing groups are generated from compiled positions and normals; smoothing group 1 is used within the generated vertex splits, and identical rebuilt normals are not guaranteed");
    if(!hasNormals && !vertices.empty())
        out.warning("Mesh "+node.name+": no compiled normal channel exists; compiled vertex identities are kept distinct instead of inferring smooth joins");
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
        const auto count=node.lightsaber?result.sourceVertices.size():vertices.size();
        out.line(std::string("tverts")+(channel?std::to_string(channel):std::string{})+" "+std::to_string(count));
        for(std::size_t i=0;i<count;++i) {
            // Saber PostProcess reads raw UVs by source-vertex index, not by
            // a face's UV index. Keep its coordinate pool in canonical order.
            const auto value=uv(vertices[node.lightsaber?result.sourceVertices[i]:i],channel);
            out.line(out.scalar(value.x)+" "+out.scalar(value.y)+" 0");
        }
        if(channel) {
            out.line("texindices"+std::to_string(channel)+" "+std::to_string(mesh.faces.size()));
            for(std::size_t i=0;i<mesh.faces.size();++i) {
                const auto& face=mesh.faces[i];
                const auto& remapped=result.sourceFaces[i];
                out.line(std::to_string(node.lightsaber?remapped[0]:static_cast<std::uint32_t>(face.indices[0]))+" "+
                    std::to_string(node.lightsaber?remapped[1]:static_cast<std::uint32_t>(face.indices[1]))+" "+
                    std::to_string(node.lightsaber?remapped[2]:static_cast<std::uint32_t>(face.indices[2])));
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
    if(!mesh.texture1.empty() && (mesh.mdxBitmap&4u))
        out.warning("Mesh "+node.name+": secondary UVs contain any compiled lightmap transform; original .lmt data is unavailable and an active .lmt sidecar would be applied again during ASCII load");
    if(hasColors && !node.lightsaber) {
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
        out.line(std::to_string(sourceFace[0])+" "+std::to_string(sourceFace[1])+" "+std::to_string(sourceFace[2])+" 1 "+
            std::to_string(node.lightsaber?sourceFace[0]:static_cast<std::uint32_t>(compiled.indices[0]))+" "+
            std::to_string(node.lightsaber?sourceFace[1]:static_cast<std::uint32_t>(compiled.indices[1]))+" "+
            std::to_string(node.lightsaber?sourceFace[2]:static_cast<std::uint32_t>(compiled.indices[2]))+" "+
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
    return result;
}

} // namespace neomdl::model::ascii_export
