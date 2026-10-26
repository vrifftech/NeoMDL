#include <model/AsciiModelInternal.hpp>
#include <model/AsciiFixedString.hpp>
#include <model/EngineArithmetic.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory_resource>
#include <numeric>
#include <unordered_map>

namespace neomdl::model::ascii {
struct GeneratedVertex {
    Vec3 position,normal,color,cpuPosition;
    std::uint8_t alpha{255u};
    TangentBasis tangentBasis;
    std::array<Vec3,4> texcoords{};
    float constraint{};
    std::size_t source{},normalSource{},previousAtPosition{};
};
struct CyclicMeshData {
    std::vector<std::array<GeneratedVertex,3>> corners;
    std::vector<std::array<std::size_t,3>> groups;
    std::size_t groupCount{};
    std::optional<EngineMeshBounds> bounds;
    std::vector<std::array<NormalFaceCross,3>> crosses;
    std::vector<std::array<std::size_t,3>> normalGroups;
    struct NormalGroup {
        mutable std::vector<std::size_t> faces,signature;
        mutable Vec3 normal;
    };
    std::vector<NormalGroup> normals;
};
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
std::vector<std::array<std::size_t,3>> smoothNormals(const RawNode& raw,std::vector<Vec3>& normals,
                                                   const neoshared::bif::JobControl& job) {
    std::vector<std::array<std::size_t,3>> indices(raw.faces.size());
    normals.clear();
    std::vector<std::size_t> offsets(raw.vertices.size()+1u);
    for(const auto& face:raw.faces)
        for(std::size_t corner=0;corner<3u;++corner)
            if(std::find(face.vertices.begin()+corner+1u,face.vertices.end(),
                         face.vertices[corner])==face.vertices.end())
                ++offsets.at(static_cast<std::size_t>(face.vertices[corner])+1u);
    std::partial_sum(offsets.begin(),offsets.end(),offsets.begin());
    auto next=offsets;
    std::vector<std::pair<std::size_t,std::size_t>> incident(offsets.back());
    std::vector<NormalFaceCross> crosses;
    for(std::size_t face=0;face<raw.faces.size();++face) {
        const auto& candidate=raw.faces[face];
        const auto a=raw.vertices.at(candidate.vertices[0]),
            b=raw.vertices.at(candidate.vertices[1]),c=raw.vertices.at(candidate.vertices[2]);
        crosses.push_back(raw.normalPositionPrecision==64u?
            preciseNormalCross(candidate,raw):normalFaceCross(a,b,c));
        // Keep incident faces contiguous and in authored order. The engine
        // chooses the last matching corner of a repeated source vertex.
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto vertex=candidate.vertices[corner];
            if(std::find(candidate.vertices.begin()+corner+1u,candidate.vertices.end(),
                         vertex)==candidate.vertices.end())
                incident[next[vertex]++]={face,corner};
        }
    }
    for(std::size_t i=0;i<raw.faces.size();++i) {
        job.check();
        const auto& current=raw.faces[i];
        for(std::size_t corner=0;corner<3u;++corner) {
            Vec3 sum{}; bool reused=false;
            const auto vertex=current.vertices[corner];
            for(auto at=offsets[vertex];at<offsets[vertex+1u];++at) {
                const auto& entry=incident[at];
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
                indices[i][corner]=normals.size();
                normals.push_back(normalizeVertexNormal(sum));
            }
        }
    }
    return indices;
}

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
        indices=smoothNormals(raw,raw.normals,job);
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


struct PositionHash {
    std::size_t operator()(const std::array<std::uint32_t,3>& key) const {
        std::size_t value{};
        for(const auto component:key) value^=component+0x9e3779b9u+(value<<6u)+(value>>2u);
        return value;
    }
};
// Position groups are assigned in first-occurrence order. A contiguous
// index keeps the temporary welding and tangent tables in contiguous storage.
class PositionIndex {
    std::vector<std::array<std::uint32_t,3>> keys;
    std::vector<std::size_t> slots;
    std::size_t empty;
public:
    explicit PositionIndex(std::size_t count):empty(count) {
        std::size_t capacity=1u;
        while(capacity<count*2u) capacity*=2u;
        slots.resize(capacity,empty);keys.reserve(count);
    }
    std::size_t index(Vec3 position) {
        const auto key=positionKey(position);
        const auto mask=slots.size()-1u;
        std::uint64_t hash=PositionHash{}(key);
        hash^=hash>>30u;hash*=0xbf58476d1ce4e5b9ull;
        hash^=hash>>27u;hash*=0x94d049bb133111ebull;
        hash^=hash>>31u;
        auto slot=static_cast<std::size_t>(hash)&mask;
        while(slots[slot]!=empty && keys[slots[slot]]!=key) slot=(slot+1u)&mask;
        if(slots[slot]==empty) {
            slots[slot]=keys.size();keys.push_back(key);
        }
        return slots[slot];
    }
    std::size_t size() const {return keys.size();}
};
bool same(const GeneratedVertex& a,const GeneratedVertex& b,bool dangly) {
    if(!same(a.position,b.position) || !same(a.normal,b.normal) || !same(a.color,b.color) ||
       a.alpha!=b.alpha || !same(a.cpuPosition,b.cpuPosition) ||
       std::memcmp(&a.tangentBasis.bitangent,&b.tangentBasis.bitangent,sizeof(Vec3))!=0 ||
       std::memcmp(&a.tangentBasis.tangent,&b.tangentBasis.tangent,sizeof(Vec3))!=0 ||
       std::memcmp(&a.tangentBasis.normal,&b.tangentBasis.normal,sizeof(Vec3))!=0) return false;
    for(std::size_t i=0;i<4u;++i) if(!same(a.texcoords[i],b.texcoords[i])) return false;
    return !dangly || a.constraint==b.constraint;
}

void authoredVertexData(const RawNode& raw,const Mesh& mesh,std::size_t face,std::size_t corner,GeneratedVertex& vertex) {
    const auto index=[&](const auto& indices) {
        return indices.empty()?vertex.source:indices.at(face)[corner];
    };
    vertex.cpuPosition=raw.cpuPositions.empty()?vertex.position:raw.cpuPositions.at(index(raw.cpuIndices));
    if(!raw.colors.empty()) vertex.color=raw.colors.at(index(raw.colorIndices));
    if(!raw.packedColors.empty()) {
        const auto& color=raw.packedColors.at(index(raw.colorIndices));
        vertex.color={float(color[0])/255.0f,float(color[1])/255.0f,float(color[2])/255.0f};
        vertex.alpha=color[3];
    }
    if(!raw.tangentBases.empty() && mesh.asciiTangents.value_or(true))
        vertex.tangentBasis=raw.tangentBases.at(index(raw.tangentIndices));
}

GeneratedVertex cornerVertex(const RawNode& raw,const Mesh& mesh,std::size_t face,std::size_t corner) {
    GeneratedVertex vertex;
    vertex.source=raw.faces[face].vertices[corner];
    vertex.position=raw.vertices.at(vertex.source);
    vertex.color={1.0f,1.0f,1.0f};
    authoredVertexData(raw,mesh,face,corner,vertex);
    for(std::size_t channel=0;channel<4u;++channel)
        if(!raw.texcoords[channel].empty())
            vertex.texcoords[channel]=raw.texcoords[channel].at(raw.faces[face].texcoords[channel][corner]);
    if(raw.node.flags==0x121u) vertex.constraint=raw.constraints.at(vertex.source);
    return vertex;
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
            vertex.color={1.0f,1.0f,1.0f};
            authoredVertexData(raw,mesh,i,corner,vertex);
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

void removeCollapsedFaces(const RawNode& raw,Mesh& mesh,std::vector<std::size_t>& faceSources) {
    const bool dangly=raw.node.flags==0x121u,skin=raw.node.flags==0x61u;
    // InternalGenVertices removes duplicate-index faces backwards, except AABB.
    // BuildNeighborLists subsequently continues to index the raw face array.
    if(raw.node.flags!=0x221u && !dangly && !skin)
        for(std::size_t i=mesh.faces.size();i--;) {
            const auto& v=mesh.faces[i].indices;
            if(v[0]==v[1] || v[0]==v[2] || v[1]==v[2]) {
                mesh.faces.erase(mesh.faces.begin()+static_cast<std::ptrdiff_t>(i));
                faceSources.erase(faceSources.begin()+static_cast<std::ptrdiff_t>(i));
            }
        }
}

std::vector<GeneratedVertex> generateVertices(RawNode& raw,Mesh& mesh,
        const std::vector<std::array<std::size_t,3>>& normals,
        const neoshared::bif::JobControl& job,
        std::vector<std::array<std::int16_t,3>>* authoredCorners=nullptr) {
    // K1 0x5c358c, K2 0x6e1640; NodeVertex equality K1 0x5c8cac,
    // equal(Vector) 0x664744 compares all three components exactly, including
    // the third authoring UV coordinate that is omitted from final MDX rows.
    const auto noVertex=raw.faces.size()*3u;
    std::vector<GeneratedVertex> vertices;
    vertices.reserve(noVertex);
    PositionIndex positions(raw.vertices.size());
    std::vector<std::size_t> sourcePositions;
    sourcePositions.reserve(raw.vertices.size());
    for(const auto position:raw.vertices)
        sourcePositions.push_back(positions.index(position));
    std::vector<std::size_t> firstAtPosition(positions.size(),noVertex);
    mesh.faces.resize(raw.faces.size());
    raw.tangentFaceSources.resize(raw.faces.size());
    const bool dangly=raw.node.flags==0x121u;
    const bool skin=raw.node.flags==0x61u;
    for(std::size_t i=0;i<raw.faces.size();++i) {
        job.check();
        raw.tangentFaceSources[i]=i;
        auto& face=mesh.faces[i];face.surfaceMaterial=raw.faces[i].material;
        for(std::size_t corner=0;corner<3u;++corner) {
            auto vertex=cornerVertex(raw,mesh,i,corner);
            vertex.normal=raw.normals.at(normals[i][corner]);
            // Distinct authored normal indices retain intentional vertex
            // splits even when their final vectors became equal at a seam.
            vertex.normalSource=normals[i][corner];
            auto& first=firstAtPosition[sourcePositions[vertex.source]];
            const auto matches=[&](std::size_t index){return same(vertices[index],vertex,dangly) &&
                    (!mesh.explicitNormals || vertices[index].normalSource==vertex.normalSource) &&
                    (!skin || (raw.skinData?
                        raw.skinChannels.at(vertices[index].source).weights==raw.skinChannels.at(vertex.source).weights &&
                        raw.skinChannels.at(vertices[index].source).indices==raw.skinChannels.at(vertex.source).indices:
                        raw.weights.at(vertices[index].source)==raw.weights.at(vertex.source)));};
            auto index=first;
            // Equal records are already welded, so at most one row can match.
            while(index!=noVertex && !matches(index)) index=vertices[index].previousAtPosition;
            if(index==noVertex) {
                index=vertices.size();vertex.previousAtPosition=first;first=index;
                vertices.push_back(vertex);
            }
            face.indices[corner]=static_cast<std::int16_t>(index);
        }
        if(authoredCorners) authoredCorners->push_back(face.indices);
    }
    removeCollapsedFaces(raw,mesh,raw.tangentFaceSources);
    return vertices;
}
void neighbors(RawNode& raw,Mesh& mesh) {
    // K1 0x5c3274; K2 0x6e1330. The first nonnegative narrowed match for each
    // undirected source-index edge wins. Planes use raw authored coordinates.
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::vector<std::size_t>> incident;
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        const auto& v=raw.faces[i].vertices;
        for(std::size_t a=0;a<3u;++a)
            for(std::size_t b=a+1u;b<3u;++b) {
                auto& faces=incident[std::minmax(v[a],v[b])];
                if(faces.empty() || faces.back()!=i) faces.push_back(i);
            }
    }
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        const auto& source=raw.faces[i];auto& face=mesh.faces[i];
        const auto weighted=faceCross(source,raw.vertices);
        face.normal=raw.node.flags==0xa1u?normalizeAnimNormal(weighted):normal(weighted);
        const auto p=raw.vertices.at(source.vertices[0]);
        face.planeDistance=-((face.normal.x*p.x+face.normal.y*p.y)+face.normal.z*p.z);
        for(std::size_t edge=0;edge<3u;++edge) {
            face.adjacentFaces[edge]=-1;
            const auto a=source.vertices[edge],b=source.vertices[(edge+1u)%3u];
            for(const auto j:incident.at(std::minmax(a,b))) {
                if(i==j) continue;
                // A negative narrowed match does not end the engine search;
                // every subsequent other face starts again with -1.
                // K1 0x34e0fc/0x5c33c4; K2 0xb68e7/0x6e1563.
                const auto narrowed=static_cast<std::int16_t>(j);
                // NW tests only the ushort -1 sentinel (0x453a3c),
                // whereas the KOTOR builder requires a nonnegative short.
                if(raw.node.flags==0xa1u?narrowed!=-1:narrowed>=0) {
                    face.adjacentFaces[edge]=narrowed;break;
                }
                if(j+1u==mesh.faces.size() || (j+2u==mesh.faces.size() && i+1u==mesh.faces.size()))
                    face.adjacentFaces[edge]=narrowed;
            }
        }
    }
}

void generateLightsaber(RawNode& raw) {



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
}

void lightsaber(RawNode& raw,Mesh& mesh) {
    if(!raw.retainedLightsaber) generateLightsaber(raw);
    auto& out=*raw.node.lightsaber;
    out.postprocessed=out.positions.size()==LightsaberNodeData::generatedVertexCount;
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
    else if(key=="tangentpositionprecision") integer(line,raw.tangentPositionPrecision);
    else if(key=="normals" && raw.node.flags!=0xa1u) {
        vectorList(line,reader,raw.normals);
        mesh.explicitNormals=true;
    }
    else if(key=="normalindices" || key=="colorindices" || key=="tangentindices" || key=="cpuindices")
        readArray(line,reader,key=="normalindices"?raw.normalIndices:
            key=="colorindices"?raw.colorIndices:key=="tangentindices"?raw.tangentIndices:raw.cpuIndices,12u,
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
    else if(key=="colorsrgba") readArray(line,reader,raw.packedColors,4u,
        [](const Line& row,const Reader&,std::array<std::uint8_t,4>& value) {
            unsigned r{},g{},b{},a{};
            std::sscanf(row.text.c_str(),"%u %u %u %u",&r,&g,&b,&a);
            value={{std::uint8_t(r),std::uint8_t(g),std::uint8_t(b),std::uint8_t(a)}};
        },[](const std::uint8_t* p) {return std::array<std::uint8_t,4>{{p[0],p[1],p[2],p[3]}};});
    else if(key=="hastangentbasis") {
        bool enabled{};boolean(line,enabled);mesh.asciiTangents=enabled;
    }
    else if(key=="tangentbasis") readArray(line,reader,raw.tangentBases,36u,
        [](const Line& row,const Reader&,TangentBasis& value) {
            auto& b=value.bitangent;auto& t=value.tangent;auto& n=value.normal;
            std::sscanf(row.text.c_str(),"%f %f %f %f %f %f %f %f %f",
                &b.x,&b.y,&b.z,&t.x,&t.y,&t.z,&n.x,&n.y,&n.z);
            value.valid=usableTangentBasis(value);
        },[](const std::uint8_t* p) {
            TangentBasis value{vector(p),vector(p+12u),vector(p+24u),false};
            value.valid=usableTangentBasis(value);return value;
        });
    else if(key=="cpuverts") vectorList(line,reader,raw.cpuPositions);
    else if(key=="faceplanes") readArray(line,reader,raw.facePlanes,16u,
        [](const Line& row,const Reader&,std::array<float,4>& value) {
            std::sscanf(row.text.c_str(),"%f %f %f %f",&value[0],&value[1],&value[2],&value[3]);
        },[](const std::uint8_t* p) {return std::array<float,4>{{real(p),real(p+4u),real(p+8u),real(p+12u)}};});
    else if(key=="spherecenter") {raw.sphereCenter.emplace();vector(line,*raw.sphereCenter);}
    else if(key=="sphereradius") {raw.sphereRadius.emplace();scalar(line,*raw.sphereRadius);}
    else if(key=="totalarea") {scalar(line,mesh.totalArea);raw.retainedTotalArea=true;}
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
    if(raw.node.lightsaber && (extension=="saberverts" || extension=="sabernormals" ||
            extension=="saberuvs" || extension=="saberoriginalverts")) {
        raw.advancedAscii=raw.retainedLightsaber=true;
        auto& saber=*raw.node.lightsaber;
        if(extension=="saberuvs") readArray(initial,reader,saber.texcoords,8u,
            [](const Line& row,const Reader&,Vec2& value) {
                value={parseFloat(row.tokens.at(0)),parseFloat(row.tokens.at(1))};
            },[](const std::uint8_t* p) {return Vec2{real(p),real(p+4u)};});
        else vectorList(initial,reader,extension=="saberverts"?saber.positions:
            extension=="sabernormals"?saber.normals:saber.originalPositions);
        return true;
    }
    if(extension=="tangentuvprecision" || extension=="normalpositionprecision" || extension=="tangentpositionprecision")
        return parseMeshValue(initial,reader,raw,game);
    if(extension=="hastangentbasis" || extension=="tangentbasis" || extension=="tangentindices" ||
       extension=="colorsrgba" || extension=="colorindices" || extension=="cpuverts" ||
       extension=="cpuindices" || extension=="faceplanes" || extension=="spherecenter" || extension=="sphereradius" ||
       extension=="totalarea") {
        raw.advancedAscii=true;
        return parseMeshValue(initial,reader,raw,game);
    }
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
        const std::vector<GeneratedVertex>& generated,bool animated,const neoshared::bif::JobControl& job) {
    // NW's authored tangent4 pool makes CalculateTangentSpaceBasis return
    // immediately (0x457710). Do not rebuild it as a KOTOR nine-float basis.
    if((mesh.dangly && !mesh.asciiTangents.value_or(false)) ||
       (animated && !raw.animTangents.empty())) return;
    // Explicit normals describe the final shading channel. Tangent
    // orientation uses geometric normals from before ProcessSkinSeams.
    std::vector<Vec3> geometricNormals;
    std::vector<std::array<std::size_t,3>> geometricIndices;
    const bool finalNormals=mesh.explicitNormals && !animated;
    if(finalNormals) geometricIndices=smoothNormals(raw,geometricNormals,job);
    auto source=std::make_shared<AsciiTangentSource>();
    source->authoredFaceIndices=raw.tangentFaceSources;
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        source->faceSmoothing.push_back(raw.faces[i].smoothing);
        source->firstCornerNormals.push_back(finalNormals?geometricNormals.at(geometricIndices[i][0]):
            raw.normals.at(normalIndices[i][0]));
        if(raw.tangentUvPrecision==64u) {
            const auto& uv=raw.faces[source->authoredFaceIndices[i]].texcoords[0];
            source->faceTexcoords64.push_back({raw.tangentTexcoords64.at(uv[0]),
                raw.tangentTexcoords64.at(uv[1]),raw.tangentTexcoords64.at(uv[2])});
        }
        if(raw.tangentPositionPrecision==64u) {
            const auto& positions=raw.faces[i].vertices;
            source->facePositions64.push_back({raw.normalPositions64.at(positions[0]),
                raw.normalPositions64.at(positions[1]),raw.normalPositions64.at(positions[2])});
        }
    }
    for(const auto& vertex:generated) {
        source->generatedPositions.push_back(vertex.position);
        source->generatedTexcoords.push_back({vertex.texcoords[0].x,vertex.texcoords[0].y});
    }
    mesh.asciiTangentSource=std::move(source);
}

void publishGeometryVertices(Mesh& mesh,const std::vector<GeneratedVertex>& generated) {
    mesh.enginePrelightVertexCount=static_cast<std::uint16_t>(generated.size());
    mesh.vertices.clear();
    mesh.vertices.reserve(mesh.enginePrelightVertexCount);
    for(std::size_t i=0;i<mesh.enginePrelightVertexCount;++i) {
        const auto& source=generated[i];
        Vertex vertex;vertex.position=source.position;vertex.normal=source.normal;
        mesh.vertices.push_back(vertex);
    }
    mesh.asciiNormalPool.reset();
}

std::shared_ptr<const CyclicMeshData> prepareCyclicMesh(
        RawNode raw,Mesh mesh,const neoshared::bif::JobControl& job) {
    processVertices(raw,mesh,job);
    auto cache=std::make_shared<CyclicMeshData>();
    cache->bounds=mesh.engineBounds;
    cache->corners.resize(raw.faces.size());cache->groups.resize(raw.faces.size());
    cache->crosses.resize(raw.faces.size());cache->normalGroups.resize(raw.faces.size());
    std::vector<std::vector<std::size_t>> incident(raw.vertices.size());
    for(std::size_t face=0;face<raw.faces.size();++face) {
        for(std::size_t rotation=0;rotation<3u;++rotation) {
            auto rotated=raw.faces[face];
            for(std::size_t corner=0;corner<3u;++corner)
                rotated.vertices[corner]=raw.faces[face].vertices[(corner+rotation)%3u];
            cache->crosses[face][rotation]=raw.normalPositionPrecision==64u?
                preciseNormalCross(rotated,raw):normalFaceCross(raw.vertices[rotated.vertices[0]],
                    raw.vertices[rotated.vertices[1]],raw.vertices[rotated.vertices[2]]);
        }
        for(const auto vertex:raw.faces[face].vertices) {
            auto& faces=incident[vertex];
            if(faces.empty() || faces.back()!=face) faces.push_back(face);
        }
    }
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::size_t> normalGroups;
    for(std::size_t face=0;face<raw.faces.size();++face) {
        const auto& value=raw.faces[face];
        if(value.smoothing==0u) {
            cache->normalGroups[face].fill(cache->normals.size());
            cache->normals.push_back({{face},{raw.faces.size()*3u},{}});
            continue;
        }
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto vertex=value.vertices[corner];
            const auto entry=normalGroups.emplace(std::make_pair(vertex,value.smoothing),cache->normals.size());
            cache->normalGroups[face][corner]=entry.first->second;
            if(entry.second) {
                CyclicMeshData::NormalGroup group;
                for(const auto other:incident[vertex])
                    if(value.smoothing&raw.faces[other].smoothing) group.faces.push_back(other);
                group.signature.resize(group.faces.size(),raw.faces.size()*3u);
                cache->normals.push_back(std::move(group));
            }
        }
    }
    PositionIndex positions(raw.vertices.size());
    std::vector<std::size_t> sourcePositions;
    for(const auto position:raw.vertices) sourcePositions.push_back(positions.index(position));
    const auto end=raw.faces.size()*3u;
    std::vector<std::size_t> first(positions.size(),end);
    const bool dangly=raw.node.flags==0x121u,skin=raw.node.flags==0x61u;
    for(std::size_t face=0;face<raw.faces.size();++face) {
        job.check();
        for(std::size_t corner=0;corner<3u;++corner) {
            auto& vertex=cache->corners[face][corner];vertex=cornerVertex(raw,mesh,face,corner);
            auto& head=first[sourcePositions[vertex.source]];
            auto at=head;
            for(;at!=end;at=cache->corners[at/3u][at%3u].previousAtPosition) {
                const auto& prior=cache->corners[at/3u][at%3u];
                if(same(prior,vertex,dangly) && (!skin || (raw.skinData?
                    raw.skinChannels.at(prior.source).weights==raw.skinChannels.at(vertex.source).weights &&
                    raw.skinChannels.at(prior.source).indices==raw.skinChannels.at(vertex.source).indices:
                    raw.weights.at(prior.source)==raw.weights.at(vertex.source)))) break;
            }
            if(at==end) {
                cache->groups[face][corner]=cache->groupCount++;
                vertex.previousAtPosition=head;head=face*3u+corner;
            } else cache->groups[face][corner]=cache->groups[at/3u][at%3u];
        }
    }
    return cache;
}

std::vector<std::array<std::int16_t,3>> generateCyclicMeshGeometry(
        RawNode& raw,Mesh& mesh,const CyclicMeshData& cache,
        const std::vector<std::size_t>& faceOrder,const std::vector<std::uint8_t>& rotations,
        const neoshared::bif::JobControl& job) {
    // Only contributors whose order or cyclic start changed need a new sum.
    // Preserve each sum's authored order and the engine's mixed precision.
    std::vector<std::size_t> rank(faceOrder.size());
    for(std::size_t face=0;face<faceOrder.size();++face) rank[faceOrder[face]]=face;
    for(const auto& group:cache.normals) {
        std::sort(group.faces.begin(),group.faces.end(),[&](auto a,auto b){return rank[a]<rank[b];});
        bool changed=false;
        for(std::size_t i=0;i<group.faces.size();++i) {
            const auto face=group.faces[i],state=face*3u+rotations[face];
            changed=changed || group.signature[i]!=state;group.signature[i]=state;
        }
        if(changed) {
            Vec3 sum{};
            for(const auto face:group.faces) sum=cache.crosses[face][rotations[face]].accumulate(sum);
            group.normal=normalizeVertexNormal(sum);
        }
    }
    std::vector<std::array<std::size_t,3>> normals(raw.faces.size());
    std::vector<std::size_t> firstFace(cache.normals.size(),raw.faces.size()),normalIndex(cache.normals.size());
    raw.normals.clear();mesh.engineBounds=cache.bounds;
    for(std::size_t face=0;face<raw.faces.size();++face) {
        const auto original=faceOrder[face];
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto group=cache.normalGroups[original][(corner+rotations[original])%3u];
            if(raw.faces[face].smoothing!=0u && firstFace[group]<face) normals[face][corner]=normalIndex[group];
            else {
                // Repeated vertices in the first face publish distinct rows;
                // later faces reuse that face's last matching corner.
                firstFace[group]=face;normalIndex[group]=raw.normals.size();
                normals[face][corner]=raw.normals.size();raw.normals.push_back(cache.normals[group].normal);
            }
        }
    }
    const auto end=raw.faces.size()*3u;
    std::vector<std::size_t> first(cache.groupCount,end);
    std::vector<GeneratedVertex> generated;generated.reserve(end);
    std::vector<std::array<std::int16_t,3>> corners;corners.reserve(raw.faces.size());
    mesh.faces.resize(raw.faces.size());raw.tangentFaceSources.resize(raw.faces.size());
    for(std::size_t face=0;face<raw.faces.size();++face) {
        job.check();
        raw.tangentFaceSources[face]=face;
        auto& output=mesh.faces[face];output.surfaceMaterial=raw.faces[face].material;
        const auto original=faceOrder[face];
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto from=(corner+rotations[original])%3u;
            const auto normal=raw.normals.at(normals[face][corner]);
            auto& head=first[cache.groups[original][from]];
            auto at=head;
            while(at!=end && !same(generated[at].normal,normal)) at=generated[at].previousAtPosition;
            if(at==end) {
                at=generated.size();
                auto vertex=cache.corners[original][from];
                vertex.normal=normal;vertex.normalSource=normals[face][corner];
                vertex.previousAtPosition=head;head=at;
                generated.push_back(vertex);
            }
            output.indices[corner]=static_cast<std::int16_t>(at);
        }
        corners.push_back(output.indices);
    }
    removeCollapsedFaces(raw,mesh,raw.tangentFaceSources);
    retainNormalPositionSource(raw,mesh);
    retainTangentSource(raw,mesh,normals,generated,false,job);
    publishGeometryVertices(mesh,generated);
    return corners;
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
    retainTangentSource(raw,mesh,normals,generated,animated,job);
    if(animated)
        for(const auto& face:mesh.faces) corners.push_back(face.indices);
    publishGeometryVertices(mesh,generated);
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
            if(!raw.colors.empty() || !raw.packedColors.empty()) mesh.mdxRowSize+=4u;
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
    if(raw.sphereCenter) mesh.engineBounds->sphereCenter=*raw.sphereCenter;
    if(raw.sphereRadius) mesh.engineBounds->sphereRadius=*raw.sphereRadius;
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
    if(!raw.facePlanes.empty())
        for(std::size_t i=0;i<mesh.faces.size();++i) {
            const auto& plane=raw.facePlanes.at(raw.tangentFaceSources[i]);
            mesh.faces[i].normal={plane[0],plane[1],plane[2]};
            mesh.faces[i].planeDistance=plane[3];
        }
    retainTangentSource(raw,mesh,normalIndices,generated,animated,job);
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
        if(!raw.colors.empty() || !raw.packedColors.empty()) {
            // K1/K2 GL consumes four unsigned bytes, including alpha.
            vertex.color={{static_cast<std::uint8_t>(engineFloatToInt(source.color.x*255.0f+0.5f)),
                static_cast<std::uint8_t>(engineFloatToInt(source.color.y*255.0f+0.5f)),
                static_cast<std::uint8_t>(engineFloatToInt(source.color.z*255.0f+0.5f)),source.alpha}};
        }
        vertex.tangentBasis=source.tangentBasis;
        mesh.vertices.push_back(vertex);mesh.cpuPositions->push_back(source.cpuPosition);
        raw.generatedVertexSources.push_back(source.source);raw.generatedVertexColors.push_back(source.color);
        if(mesh.dangly) mesh.dangly->constraints.push_back(source.constraint);
    }
    if(mesh.dangly) {
        mesh.dangly->restPositions.emplace();
        for(const auto& vertex:mesh.vertices) mesh.dangly->restPositions->push_back(vertex.position);
    }


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
    if(!raw.colors.empty() || !raw.packedColors.empty()) {
        mesh.hasVertexColors=true;mesh.mdxBitmap|=0x40u;
        mesh.mdxColorOffset=mesh.mdxRowSize;mesh.mdxRowSize+=4u;
    }
    if(!raw.tangentBases.empty() && mesh.asciiTangents.value_or(true)) {
        mesh.mdxBitmap|=0x80u;mesh.mdxTangentOffset=mesh.mdxRowSize;mesh.mdxRowSize+=36u;
        mesh.validTangentVertices=static_cast<std::size_t>(std::count_if(mesh.vertices.begin(),mesh.vertices.end(),
            [](const Vertex& vertex) {return vertex.tangentBasis.valid;}));
        mesh.asciiTangents=true;
    }
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
    // Advanced's stored total area remains independent of geometry rebuilding.
    if(!raw.retainedTotalArea && !mesh.dangly && !mesh.skinned && !animated)
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

NormalPositionProducts normalPositionFaceProducts(const std::array<Vec3,3>& positions,
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
    return {{{p[1]*q[2],p[2]*q[0],p[0]*q[1]},
        {p[2]*q[1],p[0]*q[2],p[1]*q[0]}}};
}

Vec3 normalPositionFaceCross(const std::array<Vec3,3>& positions,
        const std::array<std::array<double,3>,3>& precise) {
    // Keep authored products precise until their difference is stored. Rounding
    // each product first erases residual directions between opposing faces.
    const auto p=normalPositionFaceProducts(positions,precise);
    return {static_cast<float>(p[0][0]-p[1][0]),static_cast<float>(p[0][1]-p[1][1]),
        static_cast<float>(p[0][2]-p[1][2])};
}

std::vector<TangentComponent> singularTangentComponents(const std::vector<TangentFace>& faces,
        const neoshared::bif::JobControl& job) {
    std::vector<bool> singular(faces.size());
    for(std::size_t i=0;i<faces.size();++i) {
        job.check();
        const auto& uv=faces[i].texcoords;
        const float pu=uv[1].x-uv[0].x,qu=uv[2].x-uv[0].x;
        const float pv=uv[1].y-uv[0].y,qv=uv[2].y-uv[0].y;
        singular[i]=pu*qv-pv*qu==0.0f;
    }
    if(std::none_of(singular.begin(),singular.end(),[](bool value){return value;})) return {};

    using Endpoint=std::array<float,5>;
    using Edge=std::array<Endpoint,2>;
    struct EdgeHash {
        std::size_t operator()(const Edge& edge) const {
            std::size_t result{};
            for(const auto& endpoint:edge) for(const auto value:endpoint) {
                std::uint32_t word{};
                if(value!=0.0f) std::memcpy(&word,&value,sizeof(word));
                result^=word+0x9e3779b9u+(result<<6u)+(result>>2u);
            }
            return result;
        }
    };
    const auto end=faces.size()*3u;
    struct Incident {std::size_t face,next;};
    struct EdgeFaces {std::size_t first,last;};
    std::vector<Incident> incident;
    incident.reserve(end);
    std::pmr::monotonic_buffer_resource edgeMemory;
    std::pmr::unordered_map<Edge,EdgeFaces,EdgeHash> edges{&edgeMemory};
    edges.reserve(3u*static_cast<std::size_t>(std::count(singular.begin(),singular.end(),true)));
    const auto append=[&](EdgeFaces& list,std::size_t face) {
        const auto next=incident.size();
        incident.push_back({face,end});
        incident[list.last].next=next;list.last=next;
    };
    // Only edges touching singular faces participate in a component or its
    // boundary. Index those first, then attach their nonsingular neighbors.
    const auto visitEdges=[&](std::size_t i,const auto& visit) {
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
            visit(edge);
        }
    };
    for(std::size_t i=0;i<faces.size();++i)
        if(singular[i]) visitEdges(i,[&](const Edge& edge) {
            const auto next=incident.size();
            const auto entry=edges.try_emplace(edge,EdgeFaces{next,next});
            if(entry.second) incident.push_back({i,end});
            else append(entry.first->second,i);
        });
    for(std::size_t i=0;i<faces.size();++i)
        if(!singular[i]) visitEdges(i,[&](const Edge& edge) {
            const auto found=edges.find(edge);
            if(found!=edges.end()) append(found->second,i);
        });
    std::vector<std::size_t> parents(faces.size());
    for(std::size_t i=0;i<faces.size();++i) parents[i]=i;
    const auto root=[&](std::size_t i) {
        while(parents[i]!=i) {parents[i]=parents[parents[i]];i=parents[i];}
        return i;
    };
    for(const auto& entry:edges) {
        job.check();
        auto first=faces.size();
        for(auto at=entry.second.first;at!=end;at=incident[at].next) if(singular[incident[at].face]) {
            const auto face=incident[at].face;
            if(first==faces.size()) first=face;
            else parents[root(face)]=root(first);
        }
    }
    std::vector<std::vector<std::size_t>> components(faces.size()),boundaries(faces.size());
    for(std::size_t i=0;i<faces.size();++i) if(singular[i]) components[root(i)].push_back(i);
    for(const auto& entry:edges) {
        job.check();
        // Singular faces create every edge list before its boundary readers.
        const auto owner=root(incident[entry.second.first].face);
        for(auto at=entry.second.first;at!=end;at=incident[at].next) {
            const auto face=incident[at].face;
            if(!singular[face]) boundaries[owner].push_back(face);
        }
    }
    std::vector<TangentComponent> result;
    for(std::size_t component=0;component<components.size();++component) {
        if(components[component].empty()) continue;
        auto& boundary=boundaries[component];
        std::sort(boundary.begin(),boundary.end());
        boundary.erase(std::unique(boundary.begin(),boundary.end()),boundary.end());
        result.push_back({std::move(components[component]),std::move(boundary)});
    }
    return result;
}

void orientSingularTangentComponents(const std::vector<TangentComponent>& components,
        std::vector<TangentBasis>& bases,const neoshared::bif::JobControl& job) {
    const auto dot=[](Vec3 a,Vec3 b) {
        return (static_cast<double>(a.x)*b.x+static_cast<double>(a.y)*b.y)+
               static_cast<double>(a.z)*b.z;
    };
    const auto collinear=[](Vec3 a,Vec3 b) {
        return static_cast<double>(a.y)*b.z-static_cast<double>(a.z)*b.y==0.0 &&
               static_cast<double>(a.z)*b.x-static_cast<double>(a.x)*b.z==0.0 &&
               static_cast<double>(a.x)*b.y-static_cast<double>(a.y)*b.x==0.0;
    };
    for(const auto& component:components) {
        job.check();
        const auto& members=component.faces;
        const auto reference=bases[members.front()].tangent;
        if(same(reference,Vec3{})) continue;
        bool eligible=true,opposed=false;
        for(const auto face:members) {
            const auto& basis=bases[face];
            if(!same(basis.normal,Vec3{}) || !collinear(reference,basis.tangent)) {eligible=false;break;}
            if(dot(reference,basis.tangent)<0.0) opposed=true;
        }
        if(!eligible || !opposed) continue;
        int direction=0;
        for(const auto face:component.boundary) {
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

void orientSingularTangentComponents(const std::vector<TangentFace>& faces,
        std::vector<TangentBasis>& bases,const neoshared::bif::JobControl& job) {
    orientSingularTangentComponents(singularTangentComponents(faces,job),bases,job);
}

TangentBasisTopology prepareTangentBasis(const Mesh& mesh,const neoshared::bif::JobControl& job) {
    const auto& source=*mesh.asciiTangentSource;
    std::vector<TangentFace> faces;
    faces.reserve(mesh.faces.size());
    for(const auto& face:mesh.faces) {
        job.check();
        TangentFace value;
        for(std::size_t corner=0;corner<3u;++corner) {
            const auto row=face.indices[corner];
            value.positions[corner]=source.generatedPositions.at(row);
            value.texcoords[corner]=source.generatedTexcoords.at(row);
        }
        faces.push_back(value);
    }
    return prepareTangentBasis(mesh,singularTangentComponents(faces,job));
}

TangentBasisTopology prepareTangentBasis(const Mesh& mesh,std::vector<TangentComponent> singular) {
    const auto& source=*mesh.asciiTangentSource;
    TangentBasisTopology result;
    result.singular=std::move(singular);
    PositionIndex positions(source.generatedPositions.size());
    result.vertexPositions.reserve(source.generatedPositions.size());
    for(const auto position:source.generatedPositions)
        result.vertexPositions.push_back(positions.index(position));
    result.offsets.resize(positions.size()+1u);
    for(const auto& face:mesh.faces)
        for(const auto index:face.indices)
            ++result.offsets[result.vertexPositions.at(static_cast<std::size_t>(index))+1u];
    std::partial_sum(result.offsets.begin(),result.offsets.end(),result.offsets.begin());
    auto next=result.offsets;
    result.incident.resize(result.offsets.back());
    result.writers.resize(source.generatedPositions.size());
    // Keep repeated positions and the exact face/corner accumulation order.
    for(std::size_t face=0;face<mesh.faces.size();++face)
        for(const auto index:mesh.faces[face].indices) {
            result.incident[next[result.vertexPositions[index]]++]={face,index};
            result.writers[index]=face;
        }
    return result;
}

// CalculateTangentSpaceBasis K1 0x5c56b4/K2 0x6e3f40 is called only after
// engine material initialization reports a bump map without a normal map.
// The loader retains the full temporary vertex/face inputs until that
// resource stage; they are distinct from the already published MDX rows.
std::vector<TangentBasis> tangentFaceBases(const Mesh& mesh,const neoshared::bif::JobControl& job) {
    const auto& source=*mesh.asciiTangentSource;
    const auto normalize=normalizeTangentBasis;
    const auto negate=[](Vec3 v){return Vec3{-v.x,-v.y,-v.z};};
    std::vector<TangentBasis> faceBasis;
    faceBasis.reserve(mesh.faces.size());
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
        if(!source.facePositions64.empty()) {
            const std::array<Vec2,3> uv{{auv,buv,cuv}};
            const std::array<std::array<double,2>,3> roundedUv{{{auv.x,auv.y},{buv.x,buv.y},{cuv.x,cuv.y}}};
            const auto& preciseUv=source.faceTexcoords64.empty()?roundedUv:source.faceTexcoords64[i];
            const std::array<Vec3,3> positions{{source.generatedPositions[a],source.generatedPositions[b],source.generatedPositions[c]}};
            basis.bitangent=normalize(tangentPositionDerivative(positions,source.facePositions64[i],uv,preciseUv,0u));
            basis.tangent=normalize(tangentPositionDerivative(positions,source.facePositions64[i],uv,preciseUv,1u));
        } else if(source.faceTexcoords64.empty()) {
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
    }
    return faceBasis;
}

void buildTangentBasis(Mesh& mesh,const TangentBasisTopology& topology,
        const neoshared::bif::JobControl& job) {
    const auto& source=*mesh.asciiTangentSource;
    // Preserve the extra MDX row's defined zero tangent channels.
    if(mesh.vertices.size()>mesh.enginePrelightVertexCount)
        mesh.vertices[mesh.enginePrelightVertexCount].tangentBasis.valid=true;
    const auto normalize=normalizeTangentBasis;
    const auto add=[](Vec3 a,Vec3 b){return Vec3{a.x+b.x,a.y+b.y,a.z+b.z};};
    auto faceBasis=tangentFaceBases(mesh,job);
    orientSingularTangentComponents(topology.singular,faceBasis,job);
    for(std::size_t i=0;i<mesh.faces.size();++i) {
        job.check();
        for(const auto index:mesh.faces[i].indices) {
            // Only the last face using a row publishes its final basis.
            if(topology.writers[index]!=i) continue;
            const auto position=source.generatedPositions.at(static_cast<std::size_t>(index));
            TangentBasis result;
            result.bitangent=add(faceBasis[i].bitangent,result.bitangent);
            result.tangent=add(faceBasis[i].tangent,result.tangent);
            result.normal=add(faceBasis[i].normal,result.normal);
            const auto group=topology.vertexPositions[index];
            for(auto at=topology.offsets[group];at<topology.offsets[group+1u];++at) {
                const auto& entry=topology.incident[at];
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
    ascii::buildTangentBasis(mesh,ascii::prepareTangentBasis(mesh,job),job);
}
} // namespace neomdl::model
