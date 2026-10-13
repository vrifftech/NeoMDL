#include <model/AsciiModelInternal.hpp>
#include <model/AsciiFixedString.hpp>
#include <model/EngineArithmetic.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace neomdl::model::ascii {
namespace {

std::string arguments(const Line& line, std::size_t first=1u) {
    std::string result;
    for (std::size_t i=first;i<line.tokens.size();++i) {
        if (i!=first) result+=' ';
        result+=line.tokens[i];
    }
    return result;
}
std::uint32_t word(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8u) |
        (std::uint32_t(p[2])<<16u) | (std::uint32_t(p[3])<<24u);
}
float real(const std::uint8_t* p) {
    const auto bits=word(p); float value;
    std::memcpy(&value,&bits,sizeof(value)); return value;
}
Vec3 vector(const std::uint8_t* p) { return {real(p),real(p+4u),real(p+8u)}; }
void vector(const Line& line, Vec3& value, std::size_t first=1u) {
    std::sscanf(arguments(line,first).c_str(),"%f %f %f",&value.x,&value.y,&value.z);
}
void scalar(const Line& line,float& value) {
    std::sscanf(arguments(line).c_str(),"%f",&value);
}
void integer(const Line& line,std::uint32_t& value) {
    auto number=static_cast<std::int32_t>(value);
    scanInteger(arguments(line).c_str(),number);
    value=static_cast<std::uint32_t>(number);
}
// Mesh bool fields use only true/false/1/0, unlike animateuv/transparencyhint.
void boolean(const Line& line,bool& value) {
    scanBoolean(arguments(line).c_str(),value);
}

// K1 Parse<Vector> 0x5a400c; K2 0x722be0: a numeric count followed by any
// additional character selects raw float32 data. No count selects endlist.
void vectorList(const Line& header,Reader& reader,std::vector<Vec3>& values,
                std::vector<std::array<double,2>>* precise=nullptr,
                std::vector<std::array<double,3>>* precisePositions=nullptr) {
    const auto read=[&](const Line& row,std::size_t index) {
        vector(row,values[index],0u);
        if(precise) {
            auto& uv=(*precise)[index];
            std::sscanf(arguments(row,0u).c_str(),"%lf %lf",&uv[0],&uv[1]);
        }
        if(precisePositions) {
            auto& position=(*precisePositions)[index];
            std::sscanf(arguments(row,0u).c_str(),"%lf %lf %lf",&position[0],&position[1],&position[2]);
        }
    };
    int count{}; char marker{};
    const auto fields=std::sscanf(arguments(header).c_str(),"%d %c",&count,&marker);
    if(fields>0) {
        values.resize(static_cast<std::size_t>(count));
        if(precise) precise->resize(values.size());
        if(precisePositions) precisePositions->resize(values.size());
        if(fields==2) {
            const auto data=reader.takeBytes(values.size()*12u,header);
            for(std::size_t i=0;i<values.size();++i) {
                values[i]=vector(data.data()+i*12u);
                if(precise) (*precise)[i]={values[i].x,values[i].y};
                if(precisePositions) (*precisePositions)[i]={values[i].x,values[i].y,values[i].z};
            }
            reader.finishPackedArray(header);
        } else {
            for(std::size_t i=0;i<values.size();++i) {
                Line row;
                // Parse<Vector> returns at physical EOF, retaining copied
                // old rows and the constructor-zeroed new vector rows.
                if(!reader.next(row)) return;
                read(row,i);
            }
        }
        return;
    }
    std::size_t i{}; Line row;
    while(reader.next(row)) {
        if(row.tokens.front().compare(0u,7u,"endlist")==0) return;
        if(i==values.size()) {
            values.push_back(i?values.back():Vec3{});
            if(precise) precise->push_back(i?precise->back():std::array<double,2>{});
            if(precisePositions) precisePositions->push_back(i?precisePositions->back():std::array<double,3>{});
        }
        read(row,i++);
    }
}
void face(const Line& row,RawFace& value) {
    int v0{},v1{},v2{},smooth{},t0{},t1{},t2{},material{};
    std::sscanf(arguments(row,0u).c_str(),"%d %d %d %d %d %d %d %d",
        &v0,&v1,&v2,&smooth,&t0,&t1,&t2,&material);
    value.vertices={{std::uint32_t(v0),std::uint32_t(v1),std::uint32_t(v2)}};
    value.smoothing=static_cast<std::uint32_t>(smooth);
    value.texcoords[0]={{std::uint32_t(t0),std::uint32_t(t1),std::uint32_t(t2)}};
    value.material=static_cast<std::uint32_t>(material);
}
void faceList(const Line& header,Reader& reader,std::vector<RawFace>& values) {
    // Parse<MaxFace_PostProcess> and Parse(MaxFace_PostProcess), K1
    // 0x5ac78c/0x5a0d2c, K2 0x7aa4c0/0x79baf0: eight source integers.
    int count{};char marker{};
    const auto fields=std::sscanf(arguments(header).c_str(),"%d %c",&count,&marker);
    if(fields==2) {
        if(count==0) {
            values.clear();


            reader.takeBytes(0u,header);
            reader.finishPackedArray(header);
            return;
        }
        reader.fail(header,"Packed engine face records contain process pointers and cannot be relocated from an ASCII resource");
    }
    if(fields==1) {
        values.resize(static_cast<std::size_t>(count));
        for(auto& value:values) {
            Line row;
            if(!reader.next(row)) reader.fail(header,"Unfinished counted face array");
            face(row,value);
        }
        return;
    }
    std::size_t i{}; Line row;
    while(reader.next(row)) {
        if(row.tokens.front().compare(0u,7u,"endlist")==0) return;
        if(i==values.size()) values.emplace_back();
        face(row,values[i++]);
    }
}


void indexArrays(const Line& header,Reader& reader,std::vector<std::vector<std::uint32_t>>& values) {
    // Parse<unsigned int*> K1 0x5a15b0/K2 0x79c4b0: each pointer is a
    // counted array introduced by its own line; this dialect has no endlist.
    int count{};std::sscanf(arguments(header).c_str(),"%d",&count);
    values.resize(static_cast<std::size_t>(count));
    for(auto& batch:values) {
        Line row;
        if(!reader.next(row)) reader.fail(header,"Unfinished vertex-index array");
        int entries{};std::sscanf(arguments(row,0u).c_str(),"%d",&entries);
        batch.resize(static_cast<std::size_t>(entries));
        for(auto& entry:batch) {
            if(!reader.next(row)) reader.fail(header,"Unfinished vertex-index array");
            int value{};std::sscanf(arguments(row,0u).c_str(),"%d",&value);
            entry=static_cast<std::uint32_t>(value);
        }
    }
}

Vec3 subtract(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 cross(Vec3 a,Vec3 b) {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float magnitude(Vec3 v) {return std::sqrt((v.x*v.x+v.y*v.y)+v.z*v.z);}
Vec3 normal(Vec3 v) {
    const float length=magnitude(v);
    if(length==0.0f) return {1.0f,0.0f,0.0f}; // NormalOf, K1 0x663acc.
    const float scale=1.0f/length; return {v.x*scale,v.y*scale,v.z*scale};
}
bool same(Vec3 a,Vec3 b) {return a.x==b.x && a.y==b.y && a.z==b.z;}
// Numeric equality treats both zero signs as the same position.
std::array<std::uint32_t,3> positionKey(Vec3 position) {
    std::array<std::uint32_t,3> key;
    std::size_t axis{};
    for(const auto value:{position.x,position.y,position.z}) {
        std::memcpy(&key[axis],&value,sizeof(value));
        if(value==0.0f) key[axis]=0u;
        ++axis;
    }
    return key;
}
Vec3 faceCross(const RawFace& face,const std::vector<Vec3>& vertices) {
    const auto a=vertices.at(face.vertices[0]);
    const auto b=vertices.at(face.vertices[1]);
    const auto c=vertices.at(face.vertices[2]);
    return cross(subtract(b,a),subtract(c,b));
}

NormalFaceCross preciseNormalCross(const RawFace& face,const RawNode& raw) {
    std::array<Vec3,3> positions;
    std::array<std::array<double,3>,3> precise;
    for(std::size_t corner=0;corner<3u;++corner) {
        positions[corner]=raw.vertices.at(face.vertices[corner]);
        precise[corner]=raw.normalPositions64.at(face.vertices[corner]);
    }
    const auto value=normalPositionFaceCross(positions,precise);
    return {value.x,value.y,value.z};
}

// ProcessVerts K1 0x5c3950, K2 0x6e1a50. Smoothing uses source vertex
// indices, not equal positions; contributions are unnormalized face crosses.
// A previously processed corner with exactly the same smoothing mask reuses
// its normal. Overlapping masks otherwise contribute in source face order.
std::vector<std::array<std::size_t,3>> processVertices(RawNode& raw,Mesh& mesh,
                                                     const neoshared::bif::JobControl& job) {
    if(mesh.lightmapped && raw.textureIndices[0].empty())
        for(const auto& face:raw.faces) raw.textureIndices[0].push_back({
            static_cast<float>(static_cast<std::int32_t>(face.texcoords[0][0])),
            static_cast<float>(static_cast<std::int32_t>(face.texcoords[0][1])),
            static_cast<float>(static_cast<std::int32_t>(face.texcoords[0][2]))});
    for(std::size_t i=0;i<raw.faces.size();++i)
        for(std::size_t channel=0;channel<3u;++channel)
            if(!raw.textureIndices[channel].empty()) {
                const auto indices=raw.textureIndices[channel].at(i);
                // ProcessVerts uses signed CVTSI2SS/CVTTSS2SI for these
                // authoring indices (K2 0x6e20d1, 0x6e1b73). Preserve
                // x86's defined INT_MIN result for NaN/overflow even when
                // the corresponding UV stream is absent and ignores it.
                raw.faces[i].texcoords[channel+1u]={{
                    static_cast<std::uint32_t>(engineFloatToInt(indices.x)),
                    static_cast<std::uint32_t>(engineFloatToInt(indices.y)),
                    static_cast<std::uint32_t>(engineFloatToInt(indices.z))}};
            }
    std::vector<std::array<std::size_t,3>> indices(raw.faces.size());
    if(mesh.explicitNormals) {
        for(std::size_t i=0;i<raw.faces.size();++i) {
            job.check();
            const auto& source=raw.normalIndices.empty()?raw.faces[i].vertices:raw.normalIndices.at(i);
            for(std::size_t corner=0;corner<3u;++corner) indices[i][corner]=source[corner];
        }
    } else {
        raw.normals.clear();
        std::vector<std::vector<std::pair<std::size_t,std::size_t>>> incident(raw.vertices.size());
        std::vector<NormalFaceCross> crosses;
        for(std::size_t face=0;face<raw.faces.size();++face) {
            const auto& candidate=raw.faces[face];
            const auto a=raw.vertices.at(candidate.vertices[0]),
                b=raw.vertices.at(candidate.vertices[1]),c=raw.vertices.at(candidate.vertices[2]);
            crosses.push_back(raw.normalPositionPrecision==64u?
                preciseNormalCross(candidate,raw):normalFaceCross(a,b,c));
            // The engine chooses the last matching corner of a repeated vertex.
            for(std::size_t corner=0;corner<3u;++corner) {
                auto& faces=incident.at(candidate.vertices[corner]);
                if(!faces.empty() && faces.back().first==face) faces.back().second=corner;
                else faces.emplace_back(face,corner);
            }
        }
        for(std::size_t i=0;i<raw.faces.size();++i) {
            job.check();
            const auto& current=raw.faces[i];
            for(std::size_t corner=0;corner<3u;++corner) {
                Vec3 sum{}; bool reused=false;
                for(const auto& entry:incident[current.vertices[corner]]) {
                    const auto j=entry.first;
                    const auto& candidate=raw.faces[j];
                    if(i!=j && !(current.smoothing&candidate.smoothing)) continue;
                    const auto weighted=crosses[j];
                    sum=weighted.accumulate(sum);
                    if(j<i && current.smoothing==candidate.smoothing) {
                        indices[i][corner]=indices[j][entry.second];
                        reused=true;break;
                    }
                }
                if(!reused) {
                    indices[i][corner]=raw.normals.size();
                    raw.normals.push_back(normalizeVertexNormal(sum));
                }
            }
        }
    }
    auto& bounds=*mesh.engineBounds;
    Vec3 center{};
    for(const auto p:raw.vertices) {
        bounds.box.minimum={std::min(bounds.box.minimum.x,p.x),std::min(bounds.box.minimum.y,p.y),std::min(bounds.box.minimum.z,p.z)};
        bounds.box.maximum={std::max(bounds.box.maximum.x,p.x),std::max(bounds.box.maximum.y,p.y),std::max(bounds.box.maximum.z,p.z)};
        center={center.x+p.x,center.y+p.y,center.z+p.z};
    }
    if(!raw.vertices.empty()) {
        const auto scale=1.0f/static_cast<float>(raw.vertices.size());
        center={center.x*scale,center.y*scale,center.z*scale};
    }
    bounds.sphereCenter=center;
    for(const auto p:raw.vertices) {
        const auto radius=magnitude(subtract(p,center));
        if(radius>bounds.sphereRadius) bounds.sphereRadius=radius;
    }
    return indices;
}

struct GeneratedVertex {
    Vec3 position,normal,color;
    std::array<Vec3,4> texcoords{};
    float constraint{};
    std::size_t source{};
};
bool same(const GeneratedVertex& a,const GeneratedVertex& b,bool dangly) {
    if(!same(a.position,b.position) || !same(a.normal,b.normal) || !same(a.color,b.color)) return false;
    for(std::size_t i=0;i<4u;++i) if(!same(a.texcoords[i],b.texcoords[i])) return false;
    return !dangly || a.constraint==b.constraint;
}

// NWN:EE's active animmesh builder supplies the missing KOTOR ASCII producer.
// Keep its temporary sample identities separate from the KOTOR MDX layout:
// normals and the third UV coordinate affect welding but are not sample pools.
struct GeneratedAnimVertex {
    GeneratedVertex vertex;
    std::vector<Vec3> positions,texcoords,normals;
    std::array<float,4> tangent{};
    std::uint8_t mirror{};
};
bool same(const std::vector<Vec3>& a,const std::vector<Vec3>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),
        [](Vec3 left,Vec3 right){return same(left,right);});
}
Vec3 normalizeAnimNormal(Vec3 value) {
    // NW Vector::Normalize 0x5b3700: the near-unit squared-length interval
    // returns the input unchanged; zero alone becomes +X.
    const float squared=(value.x*value.x+value.y*value.y)+value.z*value.z;
    if(squared==1.0f || (squared>=0.999989986419677734375f &&
                        squared<=1.000010013580322265625f)) return value;
    if(squared==0.0f) return {1.0f,0.0f,0.0f};
    const float scale=std::sqrt(1.0f/squared);
    return {value.x*scale,value.y*scale,value.z*scale};
}
std::vector<std::array<std::size_t,3>> processAnimVertices(RawNode& raw,Mesh& mesh,
        const neoshared::bif::JobControl& job) {
    // NW ProcessVerts 0x454dd0 supplies the normal-index domain used by the
    // animated-normal producer. Authored normals index directly by raw vertex.
    const bool authoredNormals=!raw.normals.empty();
    std::vector<std::array<std::size_t,3>> indices(raw.faces.size());
    for(std::size_t i=0;i<raw.faces.size();++i) {
        job.check();
        const auto& face=raw.faces[i];
        for(std::size_t corner=0;corner<3u;++corner) {
            if(authoredNormals) {indices[i][corner]=face.vertices[corner];continue;}
            Vec3 sum=faceCross(face,raw.vertices);
            const auto faceNormal=normalizeAnimNormal(sum);
            bool reused=false;
            if(face.smoothing!=0u)
                for(std::size_t j=0;j<raw.faces.size();++j) {
                    if(i==j) continue;
                    const auto& candidate=raw.faces[j];
                    if(!(face.smoothing&candidate.smoothing)) continue;
                    int shared=2;
                    while(shared>=0 && candidate.vertices[static_cast<std::size_t>(shared)]!=face.vertices[corner]) --shared;
                    if(shared<0) continue;
                    if(j<i) {
                        const auto index=indices[j][static_cast<std::size_t>(shared)];
                        const auto prior=raw.normals.at(index);
                        const float dot=(faceNormal.x*prior.x+faceNormal.y*prior.y)+faceNormal.z*prior.z;
                        // The engine double -0.99 comparison rejects only a
                        // strictly smaller dot product; it is not mask equality.
                        if(static_cast<double>(dot)<-0.99) continue;
                        indices[i][corner]=index;reused=true;break;
                    }
                    const auto weighted=faceCross(candidate,raw.vertices);
                    sum={sum.x+weighted.x,sum.y+weighted.y,sum.z+weighted.z};
                }
            if(!reused) {
                const float squared=(sum.x*sum.x+sum.y*sum.y)+sum.z*sum.z;
                const auto value=squared==0.0f?faceNormal:normalizeAnimNormal(sum);
                indices[i][corner]=raw.normals.size();raw.normals.push_back(value);
            }
        }
    }
    auto& bounds=*mesh.engineBounds;
    Vec3 center{};
    for(const auto value:raw.vertices) {
        bounds.box.minimum={std::min(bounds.box.minimum.x,value.x),std::min(bounds.box.minimum.y,value.y),std::min(bounds.box.minimum.z,value.z)};
        bounds.box.maximum={std::max(bounds.box.maximum.x,value.x),std::max(bounds.box.maximum.y,value.y),std::max(bounds.box.maximum.z,value.z)};
        center={center.x+value.x,center.y+value.y,center.z+value.z};
    }
    if(!raw.vertices.empty()) {
        const float scale=1.0f/static_cast<float>(raw.vertices.size());
        center={center.x*scale,center.y*scale,center.z*scale};
    }
    bounds.sphereCenter=center;
    float radiusSquared{};
    for(const auto value:raw.vertices) {
        const auto delta=subtract(value,center);
        const float squared=delta.z*delta.z+(delta.y*delta.y+delta.x*delta.x);
        radiusSquared=std::max(radiusSquared,squared);
    }
    bounds.sphereRadius=std::sqrt(radiusSquared);
    return indices;
}
std::vector<GeneratedVertex> generateAnimVertices(RawNode& raw,Mesh& mesh,
        const std::vector<std::array<std::size_t,3>>& normalIndices,
        const neoshared::bif::JobControl& job) {
    // InternalPostProcess 0x457860 and InternalGenVertices 0x455490 divide by
    // both static row counts unconditionally, including zero sample counts.
    if(raw.vertices.empty() || raw.texcoords[0].empty())
        throw ModelError("ASCII animmesh requires nonzero engine vertex and texture-coordinate divisors");
    auto& animation=raw.node.animmesh.emplace();
    animation.samplePeriod=raw.samplePeriod;
    animation.positionSampleCount=static_cast<std::int32_t>(raw.animatedVertices.size())/
        static_cast<std::int32_t>(raw.vertices.size());
    animation.texcoordSampleCount=static_cast<std::int32_t>(raw.animatedTexcoords.size())/
        static_cast<std::int32_t>(raw.texcoords[0].size());
    const auto positionFrames=static_cast<std::size_t>(animation.positionSampleCount);
    const auto texcoordFrames=static_cast<std::size_t>(animation.texcoordSampleCount);
    std::vector<Vec3> animatedNormals(positionFrames*raw.normals.size());
    for(std::size_t frame=0;frame<positionFrames;++frame) {
        const auto offset=frame*raw.vertices.size();
        for(std::size_t i=0;i<raw.faces.size();++i) {
            job.check();
            const auto& current=raw.faces[i];
            for(std::size_t corner=0;corner<3u;++corner) {
                Vec3 sum{};
                for(std::size_t j=0;j<raw.faces.size();++j) {
                    const auto& candidate=raw.faces[j];
                    if(std::find(candidate.vertices.begin(),candidate.vertices.end(),current.vertices[corner])==candidate.vertices.end() ||
                       (i!=j && !(current.smoothing&candidate.smoothing))) continue;
                    const auto a=raw.animatedVertices.at(offset+candidate.vertices[0]);
                    const auto b=raw.animatedVertices.at(offset+candidate.vertices[1]);
                    const auto c=raw.animatedVertices.at(offset+candidate.vertices[2]);
                    const auto weighted=cross(subtract(b,a),subtract(c,b));
                    sum={sum.x+weighted.x,sum.y+weighted.y,weighted.z+sum.z};
                }
                // Shared static normal indices are overwritten in face/corner
                // order, then every generated corner reads this final list.
                animatedNormals.at(frame*raw.normals.size()+normalIndices[i][corner])=normalizeAnimNormal(sum);
            }
        }
    }
    // The engine pass expands only the box, over ALL authored rows. A trailing
    // incomplete frame contributes to bounds but not to either sample pool.
    auto& box=mesh.engineBounds->box;
    for(const auto value:raw.animatedVertices) {
        // MINSS/MAXSS use new XYZ as destination and previous bounds as
        // source (0x457c29..0x457c59), retaining old on equality/unordered.
        box.minimum={std::min(box.minimum.x,value.x),std::min(box.minimum.y,value.y),std::min(box.minimum.z,value.z)};
        box.maximum={std::max(box.maximum.x,value.x),std::max(box.maximum.y,value.y),std::max(box.maximum.z,value.z)};
    }
    std::vector<GeneratedAnimVertex> generated;
    mesh.faces.resize(raw.faces.size());
    raw.tangentFaceSources.resize(raw.faces.size());
    for(std::size_t i=0;i<raw.faces.size();++i) {
        job.check();
        raw.tangentFaceSources[i]=i;
        const auto& sourceFace=raw.faces[i];
        auto& face=mesh.faces[i];face.surfaceMaterial=sourceFace.material;
        for(std::size_t corner=0;corner<3u;++corner) {
            GeneratedAnimVertex candidate;
            auto& vertex=candidate.vertex;
            vertex.source=sourceFace.vertices[corner];
            vertex.position=raw.vertices.at(vertex.source);
            const auto normalIndex=normalIndices[i][corner];
            vertex.normal=raw.normals.at(normalIndex);
            vertex.color=raw.colors.empty()?Vec3{1.0f,1.0f,1.0f}:raw.colors.at(vertex.source);
            if(!raw.animTangents.empty()) candidate.tangent=raw.animTangents.at(normalIndex);
            if(!raw.animMirrorList.empty()) candidate.mirror=static_cast<std::uint8_t>(raw.animMirrorList.at(i));
            const auto texcoordIndex=sourceFace.texcoords[0][corner];
            // Unlike the KOTOR ordinary builder, NW animmesh uses UV0's
            // corner index for every static texture-coordinate channel.
            for(std::size_t channel=0;channel<4u;++channel)
                if(!raw.texcoords[channel].empty())
                    vertex.texcoords[channel]=raw.texcoords[channel].at(texcoordIndex);
            for(std::size_t frame=0;frame<positionFrames;++frame) {
                candidate.positions.push_back(raw.animatedVertices.at(frame*raw.vertices.size()+vertex.source));
                candidate.normals.push_back(animatedNormals.at(frame*raw.normals.size()+normalIndex));
            }
            for(std::size_t frame=0;frame<texcoordFrames;++frame)
                candidate.texcoords.push_back(raw.animatedTexcoords.at(frame*raw.texcoords[0].size()+texcoordIndex));
            const auto found=std::find_if(generated.begin(),generated.end(),[&](const auto& existing) {
                return same(existing.vertex,vertex,false) && same(existing.positions,candidate.positions) &&
                    same(existing.texcoords,candidate.texcoords) && same(existing.normals,candidate.normals) &&
                    existing.tangent==candidate.tangent && existing.mirror==candidate.mirror;
            });
            const auto index=static_cast<std::size_t>(found-generated.begin());
            if(found==generated.end()) generated.push_back(std::move(candidate));
            face.indices[corner]=static_cast<std::int16_t>(index);
        }
    }
    // NW InternalGenVertices retains duplicate-index faces. The published
    // ushort count limits both packed sample pools, each vertex-major.
    animation.vertexCount=static_cast<std::uint16_t>(generated.size());
    for(std::size_t i=0;i<animation.vertexCount;++i) {
        const auto& vertex=generated[i];
        animation.positionSamples.insert(animation.positionSamples.end(),vertex.positions.begin(),vertex.positions.end());
        for(const auto uv:vertex.texcoords) animation.texcoordSamples.push_back({uv.x,uv.y});
        if(!raw.animTangents.empty()) animation.staticTangents.push_back(vertex.tangent);
    }
    std::vector<GeneratedVertex> vertices;
    vertices.reserve(generated.size());
    for(auto& vertex:generated) vertices.push_back(std::move(vertex.vertex));
    return vertices;
}

std::vector<GeneratedVertex> generateVertices(RawNode& raw,Mesh& mesh,
        const std::vector<std::array<std::size_t,3>>& normals,
        const neoshared::bif::JobControl& job,
        std::vector<std::array<std::int16_t,3>>* authoredCorners=nullptr) {
    // K1 0x5c358c, K2 0x6e1640; NodeVertex equality K1 0x5c8cac,
    // equal(Vector) 0x664744 compares all three components exactly, including
    // the third authoring UV coordinate that is omitted from final MDX rows.
    std::vector<GeneratedVertex> vertices;
    std::map<std::array<std::uint32_t,3>,std::vector<std::size_t>> positions;
    mesh.faces.resize(raw.faces.size());
    raw.tangentFaceSources.resize(raw.faces.size());
    const bool dangly=raw.node.flags==0x121u;
    const bool skin=raw.node.flags==0x61u;
    for(std::size_t i=0;i<raw.faces.size();++i) {
        job.check();
        raw.tangentFaceSources[i]=i;
        auto& face=mesh.faces[i];face.surfaceMaterial=raw.faces[i].material;
        for(std::size_t corner=0;corner<3u;++corner) {
            GeneratedVertex vertex;
            vertex.source=raw.faces[i].vertices[corner];
            vertex.position=raw.vertices.at(vertex.source);
            vertex.normal=raw.normals.at(normals[i][corner]);
            // Skin and dangly generation pass a null color pointer to their
            // NodeVertex constructors; authored colors cannot split those
            // vertices (dangly K1 0x5c7ea0, K2 0x6e6a40).
            vertex.color=(!skin && !dangly && !raw.colors.empty())?raw.colors.at(vertex.source):Vec3{1.0f,1.0f,1.0f};
            for(std::size_t channel=0;channel<4u;++channel)
                if(!raw.texcoords[channel].empty())
                    vertex.texcoords[channel]=raw.texcoords[channel].at(raw.faces[i].texcoords[channel][corner]);
            if(dangly) vertex.constraint=raw.constraints.at(vertex.source);
            auto& candidates=positions[positionKey(vertex.position)];
            const auto found=std::find_if(candidates.begin(),candidates.end(),
                [&](std::size_t index){return same(vertices[index],vertex,dangly);});
            const auto index=found==candidates.end()?vertices.size():*found;
            if(found==candidates.end()) {candidates.push_back(index);vertices.push_back(vertex);}
            face.indices[corner]=static_cast<std::int16_t>(index);
        }
        if(authoredCorners) authoredCorners->push_back(face.indices);
    }
    // InternalGenVertices removes duplicate-index faces backwards, except AABB.
    // BuildNeighborLists subsequently continues to index the raw face array.
    if(raw.node.flags!=0x221u && !dangly && !skin)
        for(std::size_t i=mesh.faces.size();i--;) {
            const auto& v=mesh.faces[i].indices;
            if(v[0]==v[1] || v[0]==v[2] || v[1]==v[2]) {
                mesh.faces.erase(mesh.faces.begin()+static_cast<std::ptrdiff_t>(i));
                raw.tangentFaceSources.erase(raw.tangentFaceSources.begin()+static_cast<std::ptrdiff_t>(i));
            }
        }
    return vertices;
}
void neighbors(RawNode& raw,Mesh& mesh) {
    // K1 0x5c3274; K2 0x6e1330. The first nonnegative narrowed match for each
    // undirected source-index edge wins. Planes use raw authored coordinates.
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        const auto& source=raw.faces[i];auto& face=mesh.faces[i];
        const auto weighted=faceCross(source,raw.vertices);
        face.normal=raw.node.flags==0xa1u?normalizeAnimNormal(weighted):normal(weighted);
        const auto p=raw.vertices.at(source.vertices[0]);
        face.planeDistance=-((face.normal.x*p.x+face.normal.y*p.y)+face.normal.z*p.z);
        for(std::size_t edge=0;edge<3u;++edge) {
            face.adjacentFaces[edge]=-1;
            const auto a=source.vertices[edge],b=source.vertices[(edge+1u)%3u];
            for(std::size_t j=0;j<mesh.faces.size();++j) {
                if(i==j) continue;
                // A negative narrowed match does not end the engine search;
                // every subsequent other face starts again with -1.
                // K1 0x34e0fc/0x5c33c4; K2 0xb68e7/0x6e1563.
                face.adjacentFaces[edge]=-1;
                const auto& v=raw.faces[j].vertices;
                if((v[0]==a && (v[1]==b||v[2]==b)) ||
                   (v[1]==a && (v[2]==b||v[0]==b)) ||
                   (v[2]==a && (v[0]==b||v[1]==b))) {
                    face.adjacentFaces[edge]=static_cast<std::int16_t>(j);
                    // NW tests only the ushort -1 sentinel (0x453a3c),
                    // whereas the KOTOR builder requires a nonnegative short.
                    if(raw.node.flags==0xa1u?face.adjacentFaces[edge]!=-1:face.adjacentFaces[edge]>=0) break;
                }
            }
        }
    }
}

void lightsaber(RawNode& raw,Mesh& mesh) {



    auto& out=*raw.node.lightsaber;
    out.positions.resize(LightsaberNodeData::generatedVertexCount);
    out.originalPositions.resize(LightsaberNodeData::generatedVertexCount+1u);
    out.normals.resize(LightsaberNodeData::generatedVertexCount);
    out.texcoords.resize(LightsaberNodeData::generatedVertexCount);
    const auto copyRaw=[&](std::size_t destination,std::size_t source) {
        out.positions[destination]=out.originalPositions[destination]=raw.vertices.at(source);
        out.normals[destination]=raw.normals.at(source);
        const auto uv=raw.texcoords[0].at(source);out.texcoords[destination]={uv.x,uv.y};
    };
    const auto copy=[&](std::size_t destination,std::size_t source) {
        out.positions[destination]=out.originalPositions[destination]=out.positions[source];
        out.normals[destination]=out.normals[source];out.texcoords[destination]=out.texcoords[source];
    };
    for(std::size_t half=0;half<2u;++half) {
        const auto base=half?LightsaberNodeData::backPieceOffset:LightsaberNodeData::frontPieceOffset;
        for(std::size_t code=0;code<LightsaberNodeData::pieceVertices;++code) {
            std::size_t source{};
            for(;;++source) {
                const auto color=raw.colors.at(source);
                const auto value=half?color.z:color.x;
                // The x86 authoring target uses CVTTSS2SI: NaN/overflow
                // yields INT_MIN and the scan continues to the next row.
                // K1 Desktop 0x34d39e/0x34d8e6; K2 Desktop 0xb8f42/0xb93c6.
                if(engineFloatToInt(value*255.0f+0.5f)==static_cast<std::int32_t>(code)) break;
            }
            copyRaw(base+code,source);
        }
        const auto segments=half?LightsaberNodeData::backSegments:LightsaberNodeData::frontSegments;
        auto destination=base+LightsaberNodeData::pieceVertices;
        for(std::size_t segment=0;segment<segments;++segment)
            for(std::size_t corner=0;corner<4u;++corner)
                // Both games retain the selected back-piece indices for
                // all four corners (K2 0x6e611b, 0x6e618d); corner zero
                // is back-piece row 88, just like the other three corners.
                copy(destination++,base+corner);
    }
    out.postprocessed=true;
    mesh.vertices.clear();mesh.indices.clear();mesh.engineDrawIndices.clear();mesh.engineDraws.clear();
    mesh.cpuPositions=out.originalPositions;
}

} // namespace

static bool parseMeshValue(const Line& line,Reader& reader,RawNode& raw,GameVersion game) {
    if(!raw.mesh) return false;
    auto& mesh=*raw.mesh;
    const auto key=lower(line.tokens.front());
    if(key=="verts") vectorList(line,reader,raw.vertices,nullptr,&raw.normalPositions64);
    else if(key=="normalpositionprecision") integer(line,raw.normalPositionPrecision);
    else if(key=="normals" && raw.node.flags!=0xa1u) {
        vectorList(line,reader,raw.normals);
        mesh.explicitNormals=true;
    }
    else if(key=="normalindices" && raw.node.flags!=0xa1u && raw.node.flags!=0x821u)
        readArray(line,reader,raw.normalIndices,12u,
            [](const Line& row,const Reader&,std::array<std::uint32_t,3>& value) {
                unsigned int x=value[0],y=value[1],z=value[2];
                std::sscanf(row.text.c_str(),"%u %u %u",&x,&y,&z);
                value={{x,y,z}};
            },[](const std::uint8_t* bytes) {
                return std::array<std::uint32_t,3>{{word(bytes),word(bytes+4u),word(bytes+8u)}};
            });
    else if(key=="tangentuvprecision") integer(line,raw.tangentUvPrecision);
    else if(key=="tverts") vectorList(line,reader,raw.texcoords[0],&raw.tangentTexcoords64);
    else if(key=="tverts1") vectorList(line,reader,raw.texcoords[1]);
    else if(key=="tverts2") vectorList(line,reader,raw.texcoords[2]);
    else if(key=="tverts3") vectorList(line,reader,raw.texcoords[3]);
    else if(key=="texindices1") vectorList(line,reader,raw.textureIndices[0]);
    else if(key=="texindices2") vectorList(line,reader,raw.textureIndices[1]);
    else if(key=="texindices3") vectorList(line,reader,raw.textureIndices[2]);
    else if(key=="colors") vectorList(line,reader,raw.colors);
    else if(key=="faces") {
        // InternalParseField parses this list only while it is empty. A later
        // faces directive neither replaces the first nonempty list nor
        // consumes its following lines (K1 0x5ab510; K2 0x7a90bb).
        if(raw.faces.empty()) faceList(line,reader,raw.faces);
        // InternalParseField allocates the final face list to the raw face
        // count (K2 0x7a9158); later duplicate removal changes count only.
        mesh.engineFaceCapacity=static_cast<std::uint32_t>(raw.faces.size());
    }


    else if(key=="vertexindicescount") readScalarArray(line,reader,raw.vertexIndicesCount,4u,
        [](const Line& row,const Reader&,std::int32_t& value){return scanInteger(row.text.c_str(),value);},
        [](const std::uint8_t* p){return static_cast<std::int32_t>(word(p));},true);
    else if(key=="leftoverfaces") {





        std::int32_t temporary{};bool initialized=false;
        readScalarArray(line,reader,raw.leftoverFaces,4u,
            [&](const Line& row,const Reader& input,std::uint32_t& value) {
                if(scanInteger(row.text.c_str(),temporary)) initialized=true;
                else if(!initialized)
                    input.fail(row,"ASCII scalar has no initialized engine value");
                value=static_cast<std::uint32_t>(temporary);
                return true;
            },[](const std::uint8_t* p){return word(p);},true);
    }
    else if(key=="vertexindices") indexArrays(line,reader,raw.vertexIndices);
    else if(key=="texture0" || key=="bitmap") {
        // The engine %s scan leaves the existing texture unchanged on EOF;
        // bitmap likewise copies its temporary only after a word was read.
        // K1 0x5aaa78/0x5aabc8; K2 0x7a8546/0x7a879b.
        const auto previous=mesh.texture0;
        scanString(arguments(line).c_str(),mesh.texture0);
        retainAsciiFixedString(mesh.asciiTexture0,previous,mesh.texture0);
    }
    else if(key=="texture1") {
        const auto previous=mesh.texture1;
        scanString(arguments(line).c_str(),mesh.texture1);
        retainAsciiFixedString(mesh.asciiTexture1,previous,mesh.texture1);
    }
    else if(game==GameVersion::Kotor2 && (key=="dirt_enabled" || key=="hologram_donotdraw")) {
        auto& field=key=="dirt_enabled"?mesh.k2Material->dirtEnabledRaw:mesh.k2Material->hologramDoNotDrawRaw;
        bool value=field!=0u;boolean(line,value);field=static_cast<std::uint8_t>(value);
    }
    else if(game==GameVersion::Kotor2 && (key=="dirt_texture" || key=="dirt_worldspace")) {
        auto& field=key=="dirt_texture"?mesh.k2Material->dirtTexture:mesh.k2Material->dirtWorldspace;
        int value=field;std::sscanf(arguments(line).c_str(),"%d",&value);field=static_cast<std::int16_t>(value);
    }
    else if(key=="diffuse") vector(line,mesh.diffuse);
    else if(key=="ambient") vector(line,mesh.ambient);
    else if(key=="shadow") boolean(line,mesh.castsShadow);
    else if(key=="render") boolean(line,mesh.render);
    else if(key=="beaming") boolean(line,mesh.beaming);
    else if(key=="rotatetexture") boolean(line,mesh.rotateTexture);
    else if(key=="lightmapped") boolean(line,mesh.lightmapped);
    else if(key=="m_bisbackgroundgeometry") boolean(line,mesh.backgroundGeometry);
    else if(key=="transparencyhint") integer(line,mesh.transparencyHint);
    else if(key=="animateuv") {
        // InternalParseField latches any nonzero input to one. Zero does not
        // clear an earlier enable; the four UV parameters are read only once
        // enabled (K1 0x5aadc4..0x5aadfc; K2 0x7a88ad..0x7a88cd).
        std::int32_t enabled{};
        scanInteger(arguments(line).c_str(),enabled);
        if(enabled!=0) mesh.uvAnimation.enabledRaw=1u;
    }
    else if(key=="uvdirectionx") {if(mesh.uvAnimation.enabledRaw) scalar(line,mesh.uvAnimation.direction.x);}
    else if(key=="uvdirectiony") {if(mesh.uvAnimation.enabledRaw) scalar(line,mesh.uvAnimation.direction.y);}
    else if(key=="uvjitter") {if(mesh.uvAnimation.enabledRaw) scalar(line,mesh.uvAnimation.jitter);}
    else if(key=="uvjitterspeed") {if(mesh.uvAnimation.enabledRaw) scalar(line,mesh.uvAnimation.jitterSpeed);}
    else if(key=="bmin") vector(line,mesh.engineBounds->box.minimum);
    else if(key=="bmax") vector(line,mesh.engineBounds->box.maximum);
    else if(raw.node.flags==0xa1u && key=="sampleperiod") scalar(line,raw.samplePeriod);
    else if(raw.node.flags==0xa1u && key=="animverts") vectorList(line,reader,raw.animatedVertices);
    else if(raw.node.flags==0xa1u && key=="animtverts") vectorList(line,reader,raw.animatedTexcoords);
    else return false;
    return true;
}

bool parseMeshField(const Line& initial,Reader& reader,RawNode& raw,GameVersion game) {
    if(!raw.mesh) return false;
    const auto offset=initial.bufferOffset;
    const auto extension=lower(initial.tokens.front());
    if(extension=="tangentuvprecision" || extension=="normalpositionprecision")
        return parseMeshValue(initial,reader,raw,game);
    if(raw.node.flags!=0xa1u && (extension=="normals" ||
            (extension=="normalindices" && raw.node.flags!=0x821u))) {
        parseMeshValue(initial,reader,raw,game);
        applyAsciiLightmap(raw,reader);
        return true;
    }
    // Preserve the explicit NW animmesh extension's existing producer; its
    // fields are outside the four KOTOR InternalParseField implementations.
    if(raw.node.flags==0xa1u && (extension=="sampleperiod" ||
            extension=="animverts" || extension=="animtverts")) {
        parseMeshValue(initial,reader,raw,game);
        applyAsciiLightmap(raw,reader);
        return true;
    }
    bool handled=false;
    if(raw.node.flags==0x61u) handled=parseSkinField(initial,reader,raw,game);
    const auto stage=[&](const char* field) {
        const auto line=reader.fieldLine(offset);
        if(equal(line.tokens.front(),field)) {
            handled=parseMeshValue(line,reader,raw,game) || handled;
        }
    };




    for(const auto* field:{"texture0","texture1","bitmap","verts","tverts",
            "animateuv","uvdirectionx","uvdirectiony","uvjitter","uvjitterspeed",
            "tverts1","tverts2","tverts3","colors","texindices1","texindices2","texindices3"})
        stage(field);
    applyAsciiLightmap(raw,reader);
    for(const auto* field:{"faces","diffuse","ambient","shadow","transparencyhint",
            "beaming","render","rotatetexture","bmin","bmax","vertexindicescount",
            "leftoverfaces","vertexindices","lightmapped","m_bIsBackgroundGeometry"})
        stage(field);
    if(game==GameVersion::Kotor2)
        for(const auto* field:{"dirt_enabled","dirt_texture","dirt_worldspace","hologram_donotdraw"})
            stage(field);
    // The scalar/key/bezier alternatives are exclusive within each group;
    // an array can expose the next group, but cannot restart its own group.
    for(const auto* group:{"selfillumcolor","alpha"}) {
        const auto line=reader.fieldLine(offset);
        handled=parseControllerField(line,reader,raw,game,group,false) || handled;
    }
    return handled;
}

void postProcessSkinNode(RawNode& raw,const neoshared::bif::JobControl& job) {
    // These lists are authored node fields even when an existing inverse
    // translation array makes PartSkin skip its later vertex producer.
    raw.mesh->engineIndexCounts=raw.vertexIndicesCount;
    raw.mesh->engineLeftoverFaces=raw.leftoverFaces;
    raw.mesh->engineRawIndexArrayCount=static_cast<std::uint32_t>(raw.vertexIndices.size());



    if(static_cast<std::int16_t>(raw.mesh->enginePrelightVertexCount)<=0)
        raw.processedSkinNormals=processVertices(raw,*raw.mesh,job);
}

void retainNormalPositionSource(const RawNode& raw,Mesh& mesh) {
    mesh.asciiNormalPositionSource.reset();
    if(raw.normalPositionPrecision!=64u) return;
    auto source=std::make_shared<AsciiNormalPositionSource>();
    source->positions=raw.normalPositions64;
    source->authoredFaceIndices=raw.tangentFaceSources;
    for(const auto& face:raw.faces) {
        source->faces.push_back(face.vertices);
        source->faceSmoothing.push_back(face.smoothing);
    }
    mesh.asciiNormalPositionSource=std::move(source);
}

void retainTangentSource(const RawNode& raw,Mesh& mesh,
        const std::vector<std::array<std::size_t,3>>& normalIndices,
        const std::vector<GeneratedVertex>& generated,bool animated) {
    // NW's authored tangent4 pool makes CalculateTangentSpaceBasis return
    // immediately (0x457710). Do not rebuild it as a KOTOR nine-float basis.
    if(mesh.dangly || (animated && !raw.animTangents.empty())) return;
    auto source=std::make_shared<AsciiTangentSource>();
    source->authoredFaceIndices=raw.tangentFaceSources;
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        source->faceSmoothing.push_back(raw.faces[i].smoothing);
        source->firstCornerNormals.push_back(raw.normals.at(normalIndices[i][0]));
        if(raw.tangentUvPrecision==64u) {
            const auto& uv=raw.faces[source->authoredFaceIndices[i]].texcoords[0];
            source->faceTexcoords64.push_back({raw.tangentTexcoords64.at(uv[0]),
                raw.tangentTexcoords64.at(uv[1]),raw.tangentTexcoords64.at(uv[2])});
        }
    }
    for(const auto& vertex:generated) {
        source->generatedPositions.push_back(vertex.position);
        source->generatedTexcoords.push_back({vertex.texcoords[0].x,vertex.texcoords[0].y});
    }
    mesh.asciiTangentSource=std::move(source);
}

std::vector<std::array<std::int16_t,3>> generateAsciiMeshGeometry(
        Model& model,RawNode& raw,Mesh& mesh,const neoshared::bif::JobControl& job) {
    const bool animated=raw.node.animmesh.has_value();
    const auto normals=animated?processAnimVertices(raw,mesh,job):processVertices(raw,mesh,job);
    std::vector<std::array<std::int16_t,3>> corners;
    corners.reserve(raw.faces.size());
    const auto generated=animated?generateAnimVertices(raw,mesh,normals,job):
        generateVertices(raw,mesh,normals,job,&corners);
    retainNormalPositionSource(raw,mesh);
    retainTangentSource(raw,mesh,normals,generated,animated);
    if(animated)
        for(const auto& face:mesh.faces) corners.push_back(face.indices);
    mesh.enginePrelightVertexCount=static_cast<std::uint16_t>(generated.size());
    mesh.vertices.clear();
    mesh.vertices.reserve(mesh.enginePrelightVertexCount);
    for(std::size_t i=0;i<mesh.enginePrelightVertexCount;++i) {
        const auto& source=generated[i];
        Vertex vertex;vertex.position=source.position;vertex.normal=source.normal;
        mesh.vertices.push_back(vertex);
    }
    mesh.asciiNormalPool.reset();
    if(animated) {
        mesh.animated=true;
        mesh.asciiMdxAllocation.reset();
        mesh.mdxRowSize=0u;
        // The animmesh producer publishes its static N+1 seam row only
        // when faces exist. Give that publication a real private ledger entry.
        if(!raw.faces.empty()) {
            mesh.mdxRowSize=24u;
            for(const auto& texcoords:raw.texcoords)
                if(!texcoords.empty()) mesh.mdxRowSize+=8u;
            if(!raw.colors.empty()) mesh.mdxRowSize+=4u;
            mesh.asciiMdxAllocation=model.asciiMdxAllocations.size();
            model.asciiMdxAllocations.push_back({mesh.enginePrelightVertexCount,
                mesh.mdxRowSize,{},false,std::make_shared<Mesh>(mesh)});
        }
    }
    return corners;
}

void buildMesh(Model& model,RawNode& raw,std::size_t nodeIndex,
               const neoshared::bif::JobControl& job) {
    if(!raw.mesh) return;
    auto& mesh=*raw.mesh;mesh.nodeIndex=nodeIndex;
    mesh.engineIndexCounts=raw.vertexIndicesCount;
    mesh.engineLeftoverFaces=raw.leftoverFaces;
    mesh.engineRawIndexArrayCount=static_cast<std::uint32_t>(raw.vertexIndices.size());
    const bool animated=raw.node.flags==0xa1u;
    if(animated && mesh.enginePrelightVertexCount!=0u) return;
    if(animated) mesh.animated=true;
    if(raw.node.flags==0x61u && !mesh.inverseBindTranslations.empty()) return;
    if(raw.node.flags==0x21u || raw.node.flags==0x221u) {
        if(mesh.texture0.empty()) {
            mesh.texture0="NULL";
            retainAsciiFixedString(mesh.asciiTexture0,{},mesh.texture0);
        }
        if(mesh.texture1.size()>=17u) {
            const auto previous=mesh.texture1;
            mesh.texture1.resize(16u);
            retainAsciiFixedString(mesh.asciiTexture1,previous,mesh.texture1);
        }
    }
    const auto normalIndices=mesh.skinned?std::move(raw.processedSkinNormals):
        animated?processAnimVertices(raw,mesh,job):processVertices(raw,mesh,job);
    // Dangly's PostProcess shares the no-face exit after ProcessVerts; no
    // vertex/index pools or MDX layout are generated (K1 0x5e96cc, K2 0x6e63a4).
    if(raw.faces.empty() && (raw.node.flags==0x21u || raw.node.flags==0x221u || mesh.dangly)) {
        // Dangly constraints already belong to the persistent node list.
        // Only InternalGenVertices replaces them with the welded list; the
        // no-face exit preserves every authored row (K2 0x6e63a4/0x6e6809).
        if(mesh.dangly) mesh.dangly->constraints=raw.constraints;
        retainNormalPositionSource(raw,mesh);
        return;
    }
    const auto generated=animated?generateAnimVertices(raw,mesh,normalIndices,job):
        generateVertices(raw,mesh,normalIndices,job);
    retainNormalPositionSource(raw,mesh);
    if(raw.node.flags==0x821u) {lightsaber(raw,mesh);return;}
    neighbors(raw,mesh);
    retainTangentSource(raw,mesh,normalIndices,generated,animated);
    // InternalPostProcess narrows the generated count before allocating and
    // copying the MDX/CPU rows. The temporary NodeVertex list remains full
    // for tangent construction (K1 0x5c620c/0x5c6420, K2 0x6e499f/0x6e4c55).
    mesh.enginePrelightVertexCount=static_cast<std::uint16_t>(generated.size());
    // Ordinary/AABB and dangly PostProcess select a null first NodeVertex
    // for nonpositive signed N, then read its attributes at +0x54. Their
    // no-face exits above are valid; a wrapped generated count is not.
    // K2 ordinary 0x6e49bc..0x6e49fd, dangly 0x6e63e7..0x6e646d.
    // Skin validates its own Part producer; NW animmesh is the backport.
    if(!animated && !mesh.skinned && static_cast<std::int16_t>(mesh.enginePrelightVertexCount)<=0)
        throw ModelError("Cannot compile a generated mesh with a nonpositive engine vertex count");
    const auto publishedCount=static_cast<std::size_t>(mesh.enginePrelightVertexCount);
    mesh.vertices.reserve(publishedCount);mesh.cpuPositions.emplace();
    raw.generatedVertexSources.clear();raw.generatedVertexColors.clear();
    for(std::size_t i=0;i<publishedCount;++i) {
        const auto& source=generated[i];
        Vertex vertex;vertex.position=source.position;vertex.normal=source.normal;
        vertex.texcoord0={source.texcoords[0].x,source.texcoords[0].y};
        vertex.texcoord1={source.texcoords[1].x,source.texcoords[1].y};
        vertex.texcoord2={source.texcoords[2].x,source.texcoords[2].y};
        vertex.texcoord3={source.texcoords[3].x,source.texcoords[3].y};
        if(animated && !raw.colors.empty()) {
            // NW GenerateVertexData 0x453ce1..0x453d35 publishes rounded
            // low-byte RGB and opaque alpha. Encode the same channel in
            // the target's interleaved x86 MDX row.
            vertex.color={{static_cast<std::uint8_t>(engineFloatToInt(source.color.x*255.0f+0.5f)),
                static_cast<std::uint8_t>(engineFloatToInt(source.color.y*255.0f+0.5f)),
                static_cast<std::uint8_t>(engineFloatToInt(source.color.z*255.0f+0.5f)),255u}};
        }
        mesh.vertices.push_back(vertex);mesh.cpuPositions->push_back(source.position);
        raw.generatedVertexSources.push_back(source.source);raw.generatedVertexColors.push_back(source.color);
        if(mesh.dangly) mesh.dangly->constraints.push_back(source.constraint);
    }
    if(mesh.dangly) mesh.dangly->restPositions=*mesh.cpuPositions;


    for(const auto& face:mesh.faces)
        for(const auto index:face.indices) mesh.indices.push_back(static_cast<std::uint32_t>(static_cast<std::int32_t>(index)));
    raw.vertexIndices.push_back(mesh.indices);
    raw.vertexIndicesCount.push_back(static_cast<std::int32_t>(mesh.indices.size()));
    // IODispatcher's writer publishes these authored counts independently
    // of RenderFlat's K2 triangle truncation, then copies the ushort pools.
    mesh.engineIndexCounts=raw.vertexIndicesCount;
    mesh.engineLeftoverFaces=raw.leftoverFaces;
    for(std::size_t batch=0;batch<raw.vertexIndices.size();++batch) {
        const auto count=static_cast<std::size_t>(raw.vertexIndicesCount.at(batch));
        // K2 RenderFlat submits only complete triangles; K1 retains the
        // authored count. Keep every index in the pool for following batches.
        const auto drawCount=animated && model.game==GameVersion::Kotor2?(count/3u)*3u:count;
        // Shared KOTOR-target pool producer, including the accepted animmesh
        // backport: source handle-list capacity survives a zero-N writer.
        // K2 BuildVertexIndices 0x6e3d1c..0x6e3d43.
        if(mesh.engineDraws.size()==mesh.engineIndexPoolCapacity)
            mesh.engineIndexPoolCapacity=mesh.engineIndexPoolCapacity?mesh.engineIndexPoolCapacity*2u:16u;
        mesh.engineDraws.push_back({3u,mesh.engineDrawIndices.size(),drawCount});
        // BuildVertexIndices converts every uint authoring row to a ushort
        // index-pool entry, including batches supplied before the generated
        // triangle batch (K1 0x5c5568; K2 0x6e3e90).
        for(std::size_t i=0;i<count;++i)
            mesh.engineDrawIndices.push_back(static_cast<std::uint16_t>(raw.vertexIndices[batch].at(i)));
    }
    mesh.indices=mesh.engineDrawIndices;
    // BuildVertexIndices clears the source pointer list after publishing
    // its render pools (K2 0x6e3ef6..0x6e3f1a). Earlier no-face/saber/skin
    // exits retain the authored list count and its exact-sized capacity.
    mesh.engineRawIndexArrayCount=0u;
    // NW BuildVertexIndices still publishes its empty generated batch, but
    // GenerateVertexData does not allocate static streams without faces.
    if(animated && raw.faces.empty()) {
        raw.animatedVertices.clear();raw.animatedTexcoords.clear();
        raw.animTangents.clear();raw.animMirrorList.clear();
        return;
    }
    // Publish the NW-generated logical rows through KOTOR's interleaved MDX
    // contract: signed N+1 rows, row N zeroed then xyz set to 1e7 (K1 ARM
    // 0x5c63b4..0x5c6400, 0x5c65c0..0x5c65e8; K2 x86 0x6e4bae..0x6e4e42).
    // The shared seam normal pool and MDX ledger represent that extra row;
    // animmesh CPU/sample/Part arrays keep N and never draw from the MDX tail.
    mesh.mdxBitmap=0x21u;mesh.mdxNormalOffset=12u;mesh.mdxRowSize=24u;
    for(std::size_t channel=0;channel<4u;++channel)
        if(!raw.texcoords[channel].empty()) {
            mesh.mdxBitmap|=2u<<channel;
            mesh.mdxTexcoordOffsets[channel]=mesh.mdxRowSize;
            mesh.mdxRowSize+=8u;++mesh.textureCount;
        }
    if(animated && !raw.colors.empty()) {
        mesh.hasVertexColors=true;mesh.mdxBitmap|=0x40u;
        mesh.mdxColorOffset=mesh.mdxRowSize;mesh.mdxRowSize+=4u;
    }
    // The generated ordinary/dangly stream does not emit NodeVertex colors;
    // the active NW animmesh producer does.
    auto prelight=std::make_shared<EnginePrelightSource>();
    prelight->authoredColors=mesh.hasVertexColors;
    for(const auto& vertex:mesh.vertices)
        prelight->vertices.push_back({vertex.position,vertex.normal,vertex.color});
    mesh.builtPrelightSource=std::move(prelight);
    // Rigid and VP skin draws can submit the generated MDX allocation's
    // zeroed N+1 row. Dangly and animmesh draws instead use N-row dynamic
    // position pools (dangly K1 0x594324..0x59437c); their row N is not a
    // defined draw source even though the static MDX seam row exists.
    // Ordinary MDX tail: K1 0x5c65c0..0x5c65e8; skin: 0x5e4990..0x5e49bc.
    if(!mesh.dangly && !animated && std::find(mesh.engineDrawIndices.begin(),mesh.engineDrawIndices.end(),
                 mesh.enginePrelightVertexCount)!=mesh.engineDrawIndices.end()) {
        Vertex sentinel;
        const float position=mesh.skinned?1000000.0f:10000000.0f;
        sentinel.position={position,position,position};
        mesh.vertices.push_back(sentinel);
    }
    // Dangly's distinct postprocess omits ComputeLocalSurfaceArea.
    if(!mesh.dangly && !mesh.skinned && !animated)
        for(const auto& face:mesh.faces) {
            const auto a=mesh.vertices.at(static_cast<std::uint16_t>(face.indices[0])).position;
            const auto b=mesh.vertices.at(static_cast<std::uint16_t>(face.indices[1])).position;
            const auto c=mesh.vertices.at(static_cast<std::uint16_t>(face.indices[2])).position;



            mesh.totalArea=magnitude(cross(subtract(c,a),subtract(b,a)))*0.5f+mesh.totalArea;
        }
    if(animated) {
        raw.animatedVertices.clear();raw.animatedTexcoords.clear();
        raw.animTangents.clear();raw.animMirrorList.clear();
    }
}

Vec3 tangentBasisNormal(Vec3 bitangent,Vec3 tangent,Vec3 faceArea) {
    if(faceArea.x==0.0f && faceArea.y==0.0f && faceArea.z==0.0f) {
        return {
            static_cast<float>(static_cast<double>(bitangent.y)*tangent.z-static_cast<double>(bitangent.z)*tangent.y),
            static_cast<float>(static_cast<double>(bitangent.z)*tangent.x-static_cast<double>(bitangent.x)*tangent.z),
            static_cast<float>(static_cast<double>(bitangent.x)*tangent.y-static_cast<double>(bitangent.y)*tangent.x)};
    }
    return cross(bitangent,tangent);
}

std::array<Vec3,2> normalPositionFaceProducts(const std::array<Vec3,3>& positions,
        const std::array<std::array<double,3>,3>& precise) {
    // Retain rounded edges and add only authored sub-float position residuals.
    const auto edge=[&](std::size_t first,std::size_t second) {
        const auto a=positions[first],b=positions[second];
        const std::array<float,3> lowA{{a.x,a.y,a.z}},lowB{{b.x,b.y,b.z}};
        std::array<double,3> result;
        for(std::size_t axis=0;axis<3u;++axis) {
            const float base=lowB[axis]-lowA[axis];
            const double residual=(precise[second][axis]-lowB[axis])-(precise[first][axis]-lowA[axis]);
            result[axis]=residual==0.0?static_cast<double>(base):static_cast<double>(base)+residual;
        }
        return result;
    };
    const auto p=edge(0u,1u),q=edge(1u,2u);
    return {{
        {static_cast<float>(p[1]*q[2]),static_cast<float>(p[2]*q[0]),static_cast<float>(p[0]*q[1])},
        {static_cast<float>(p[2]*q[1]),static_cast<float>(p[0]*q[2]),static_cast<float>(p[1]*q[0])}}};
}

Vec3 normalPositionFaceCross(const std::array<Vec3,3>& positions,
        const std::array<std::array<double,3>,3>& precise) {
    const auto products=normalPositionFaceProducts(positions,precise);
    return {products[0].x-products[1].x,products[0].y-products[1].y,products[0].z-products[1].z};
}

Vec3 tangentUvDerivative(Vec3 p,Vec3 q,const std::array<Vec2,3>& uv,
        const std::array<std::array<double,2>,3>& precise,std::size_t component) {
    const float a=component?uv[0].y:uv[0].x;
    const float b=component?uv[1].y:uv[1].x;
    const float c=component?uv[2].y:uv[2].x;
    const float firstBase=b-a,secondBase=c-a;
    // Retain the rounded coefficients and add only authored sub-float UV residuals.
    const double firstResidual=(precise[1][component]-b)-(precise[0][component]-a);
    const double secondResidual=(precise[2][component]-c)-(precise[0][component]-a);
    const double first=firstResidual==0.0?static_cast<double>(firstBase):
        static_cast<double>(firstBase)+firstResidual;
    const double second=secondResidual==0.0?static_cast<double>(secondBase):
        static_cast<double>(secondBase)+secondResidual;
    return {
        static_cast<float>(q.x*first)-static_cast<float>(p.x*second),
        static_cast<float>(q.y*first)-static_cast<float>(p.y*second),
        static_cast<float>(q.z*first)-static_cast<float>(p.z*second)};
}

void orientSingularTangentComponents(const std::vector<TangentFace>& faces,
        std::vector<TangentBasis>& bases,const neoshared::bif::JobControl& job) {
    std::vector<bool> singular(faces.size());
    for(std::size_t i=0;i<faces.size();++i) {
        job.check();
        const auto& uv=faces[i].texcoords;
        const float pu=uv[1].x-uv[0].x,qu=uv[2].x-uv[0].x;
        const float pv=uv[1].y-uv[0].y,qv=uv[2].y-uv[0].y;
        singular[i]=pu*qv-pv*qu==0.0f;
    }
    if(std::none_of(singular.begin(),singular.end(),[](bool value){return value;})) return;

    using Endpoint=std::array<float,5>;
    using Edge=std::array<Endpoint,2>;
    std::map<Edge,std::vector<std::size_t>> edges;
    for(std::size_t i=0;i<faces.size();++i) {
        job.check();
        std::array<Endpoint,3> endpoints;
        for(std::size_t corner=0;corner<3;++corner) {
            const auto p=faces[i].positions[corner];
            const auto uv=faces[i].texcoords[corner];
            endpoints[corner]={p.x,p.y,p.z,uv.x,uv.y};
        }
        for(std::size_t corner=0;corner<3;++corner) {
            Edge edge{endpoints[corner],endpoints[(corner+1)%3]};
            if(edge[1]<edge[0]) std::swap(edge[0],edge[1]);
            edges[edge].push_back(i);
        }
    }
    std::vector<std::size_t> parents(faces.size());
    for(std::size_t i=0;i<faces.size();++i) parents[i]=i;
    const auto root=[&](std::size_t i) {
        while(parents[i]!=i) {parents[i]=parents[parents[i]];i=parents[i];}
        return i;
    };
    for(const auto& entry:edges) {
        job.check();
        auto first=faces.size();
        for(const auto face:entry.second) if(singular[face]) {
            if(first==faces.size()) first=face;
            else parents[root(face)]=root(first);
        }
    }
    std::vector<std::vector<std::size_t>> components(faces.size()),boundaries(faces.size());
    for(std::size_t i=0;i<faces.size();++i) if(singular[i]) components[root(i)].push_back(i);
    for(const auto& entry:edges) {
        job.check();
        const auto& incident=entry.second;
        const auto first=std::find_if(incident.begin(),incident.end(),[&](std::size_t face){return singular[face];});
        if(first!=incident.end())
            for(const auto face:incident) if(!singular[face]) boundaries[root(*first)].push_back(face);
    }
    const auto dot=[](Vec3 a,Vec3 b) {
        return (static_cast<double>(a.x)*b.x+static_cast<double>(a.y)*b.y)+
               static_cast<double>(a.z)*b.z;
    };
    const auto collinear=[](Vec3 a,Vec3 b) {
        return static_cast<double>(a.y)*b.z-static_cast<double>(a.z)*b.y==0.0 &&
               static_cast<double>(a.z)*b.x-static_cast<double>(a.x)*b.z==0.0 &&
               static_cast<double>(a.x)*b.y-static_cast<double>(a.y)*b.x==0.0;
    };
    for(std::size_t component=0;component<components.size();++component) {
        job.check();
        const auto& members=components[component];
        if(members.empty()) continue;
        const auto reference=bases[members.front()].tangent;
        if(same(reference,Vec3{})) continue;
        bool eligible=true,opposed=false;
        for(const auto face:members) {
            const auto& basis=bases[face];
            if(!same(basis.normal,Vec3{}) || !collinear(reference,basis.tangent)) {eligible=false;break;}
            if(dot(reference,basis.tangent)<0.0) opposed=true;
        }
        if(!eligible || !opposed) continue;
        auto& boundary=boundaries[component];
        std::sort(boundary.begin(),boundary.end());
        boundary.erase(std::unique(boundary.begin(),boundary.end()),boundary.end());
        int direction=0;
        for(const auto face:boundary) {
            const auto projection=dot(reference,bases[face].tangent);
            const int sign=projection>0.0?1:projection<0.0?-1:0;
            if(sign==0) continue;
            if(direction!=0 && direction!=sign) {eligible=false;break;}
            direction=sign;
        }
        if(!eligible || direction==0) continue;
        for(const auto face:members)
            if(dot(reference,bases[face].tangent)*direction<0.0) {
                auto& tangent=bases[face].tangent;
                tangent={-tangent.x,-tangent.y,-tangent.z};
            }
    }
}

// CalculateTangentSpaceBasis K1 0x5c56b4/K2 0x6e3f40 is called only after
// engine material initialization reports a bump map without a normal map.
// The loader retains the full temporary vertex/face inputs until that
// resource stage; they are distinct from the already published MDX rows.
void buildTangentBasis(Mesh& mesh,const neoshared::bif::JobControl& job) {
    const auto& source=*mesh.asciiTangentSource;
    // The extra MDX row was memset to zero, including its tangent channels.
    // Preserve those defined zeros for engine skin-bump upload validation;
    // unlike ordinary unused rows, this basis needs no unknown pool bytes.
    if(mesh.vertices.size()>mesh.enginePrelightVertexCount)
        mesh.vertices[mesh.enginePrelightVertexCount].tangentBasis.valid=true;
    const auto normalize=[](Vec3 v) {
        const auto length=magnitude(v);
        // normalize(Vector) and Vector::Normalize use a DOUBLE 1e-9 limit,
        // unlike NormalOf's zero test (K1 constant at 0x6c3530).
        if(static_cast<double>(length)<1e-9) return Vec3{1.0f,0.0f,0.0f};
        const auto reciprocal=1.0f/length;
        return Vec3{v.x*reciprocal,v.y*reciprocal,v.z*reciprocal};
    };
    const auto add=[](Vec3 a,Vec3 b){return Vec3{a.x+b.x,a.y+b.y,a.z+b.z};};
    const auto negate=[](Vec3 v){return Vec3{-v.x,-v.y,-v.z};};
    std::vector<TangentBasis> faceBasis;
    std::vector<TangentFace> tangentFaces;
    faceBasis.reserve(mesh.faces.size());
    tangentFaces.reserve(mesh.faces.size());
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        job.check();
        const auto& face=mesh.faces[i];
        const auto a=static_cast<std::size_t>(face.indices[0]);
        const auto b=static_cast<std::size_t>(face.indices[1]);
        const auto c=static_cast<std::size_t>(face.indices[2]);
        const auto p=subtract(source.generatedPositions.at(b),source.generatedPositions.at(a));
        const auto q=subtract(source.generatedPositions.at(c),source.generatedPositions.at(a));
        const auto auv=source.generatedTexcoords.at(a),buv=source.generatedTexcoords.at(b),cuv=source.generatedTexcoords.at(c);
        const float pu=buv.x-auv.x,qu=cuv.x-auv.x;
        const float pv=buv.y-auv.y,qv=cuv.y-auv.y;
        TangentBasis basis;
        if(source.faceTexcoords64.empty()) {
            basis.bitangent=normalize({q.x*pu-p.x*qu,q.y*pu-p.y*qu,q.z*pu-p.z*qu});
            basis.tangent=normalize({q.x*pv-p.x*qv,q.y*pv-p.y*qv,q.z*pv-p.z*qv});
        } else {
            const std::array<Vec2,3> uv{{auv,buv,cuv}};
            const auto& precise=source.faceTexcoords64.at(i);
            basis.bitangent=normalize(tangentUvDerivative(p,q,uv,precise,0u));
            basis.tangent=normalize(tangentUvDerivative(p,q,uv,precise,1u));
        }
        const auto area=cross(p,subtract(source.generatedPositions.at(c),source.generatedPositions.at(b)));
        basis.normal=tangentBasisNormal(basis.bitangent,basis.tangent,area);
        const auto n=source.firstCornerNormals.at(i);
        const auto alignment=(basis.normal.x*n.x+basis.normal.y*n.y)+basis.normal.z*n.z;
        if(alignment<0.0f) {basis.tangent=negate(basis.tangent);basis.normal=negate(basis.normal);}
        faceBasis.push_back(basis);
        tangentFaces.push_back({{source.generatedPositions.at(a),source.generatedPositions.at(b),
                                 source.generatedPositions.at(c)},{auv,buv,cuv}});
    }
    orientSingularTangentComponents(tangentFaces,faceBasis,job);
    std::map<std::array<std::uint32_t,3>,std::vector<std::pair<std::size_t,std::int16_t>>> incident;
    for(std::size_t face=0;face<mesh.faces.size();++face)
        for(const auto index:mesh.faces[face].indices)
            incident[positionKey(source.generatedPositions.at(static_cast<std::size_t>(index)))].emplace_back(face,index);
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        job.check();
        for(const auto index:mesh.faces[i].indices) {
            const auto position=source.generatedPositions.at(static_cast<std::size_t>(index));
            TangentBasis result;
            result.bitangent=add(faceBasis[i].bitangent,result.bitangent);
            result.tangent=add(faceBasis[i].tangent,result.tangent);
            result.normal=add(faceBasis[i].normal,result.normal);
            for(const auto& entry:incident.at(positionKey(position))) {
                const auto other=entry.first;
                if(other!=i && same(position,source.generatedPositions.at(static_cast<std::size_t>(entry.second))) &&
                   (source.faceSmoothing.at(i)&source.faceSmoothing.at(other))) {
                    result.bitangent=add(result.bitangent,faceBasis[other].bitangent);
                    result.tangent=add(result.tangent,faceBasis[other].tangent);
                    result.normal=add(result.normal,faceBasis[other].normal);
                }
            }
            result.bitangent=normalize(result.bitangent);
            result.tangent=normalize(result.tangent);
            result.normal=normalize(result.normal);
            result.valid=true;
            // The engine transpose followed by column stores writes these
            // three vectors in bitangent/tangent/normal MDX order.
            mesh.vertices.at(static_cast<std::size_t>(index)).tangentBasis=result;
        }
    }
    mesh.mdxBitmap|=0x80u;
    if(mesh.skinned) {
        mesh.mdxTangentOffset=mesh.mdxWeightOffset;
        mesh.mdxWeightOffset+=36u;mesh.mdxBoneIndexOffset+=36u;
    } else mesh.mdxTangentOffset=mesh.mdxRowSize;
    mesh.mdxRowSize+=36u;
    mesh.validTangentVertices=static_cast<std::size_t>(std::count_if(mesh.vertices.begin(),mesh.vertices.end(),
        [](const Vertex& vertex){return vertex.tangentBasis.valid;}));
}

} // namespace neomdl::model::ascii

namespace neomdl::model {
void rebuildAsciiTangentBasis(Mesh& mesh,const neoshared::bif::JobControl& job) {
    ascii::buildTangentBasis(mesh,job);
}
} // namespace neomdl::model
