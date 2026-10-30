#include "core/ModelTransform.hpp"
#include <model/PackedBytes.hpp>
#include <model/PackedExportInternal.hpp>
#include <model/PackedLayout.hpp>
#include <algorithm>
#include <cmath>

namespace neomdl {
namespace {
using model::Vec3;
using model::packed::ByteView;
using model::packed_export::Buffer;
std::pair<Vec3,Vec3> transformedBox(Vec3 low,Vec3 high,const PlanarTransform& transform,Vec3 pivot={}) {
    auto minimum=transform.point(low,pivot),maximum=minimum;
    for(unsigned i=1;i<8;++i) {
        const auto p=transform.point({i&1?high.x:low.x,i&2?high.y:low.y,i&4?high.z:low.z},pivot);
        minimum={std::min(minimum.x,p.x),std::min(minimum.y,p.y),std::min(minimum.z,p.z)};
        maximum={std::max(maximum.x,p.x),std::max(maximum.y,p.y),std::max(maximum.z,p.z)};
    }
    return {minimum,maximum};
}
std::vector<std::pair<std::size_t,std::string>> dependencies(const std::vector<std::uint8_t>& bytes,const model::PackedLayout& layout) {
    ByteView source(bytes.data(),bytes.size(),"Model transform",{});
    std::vector<std::pair<std::size_t,std::string>> result;
    const auto add=[&](std::size_t at) {auto name=resourceKey(source.fixedString(at,32,"model reference"));if(!name.empty()&&name!="null")result.emplace_back(at,std::move(name));};
    add(layout.payloadOffset+136);
    for(const auto& region:layout.structures)if(region.name=="Reference")add(region.offset);
    return result;
}
}

Vec3 PlanarTransform::direction(Vec3 value) const {return {xx*value.x+xy*value.y,yx*value.x+yy*value.y,value.z};}
Vec3 PlanarTransform::point(Vec3 value,Vec3 pivot) const {
    const auto p=direction({value.x-pivot.x,value.y-pivot.y,0});return {p.x+pivot.x,p.y+pivot.y,value.z};
}
model::Quaternion PlanarTransform::orientation(model::Quaternion value) const {
    // Conjugate each local frame by the same basis change as its geometry.
    const auto v=direction({value.x,value.y,value.z});const float sign=mirrored?-1.0f:1.0f;
    return {sign*v.x,sign*v.y,sign*v.z,value.w};
}
model::Quaternion PlanarTransform::facing(model::Quaternion value) const {
    const auto angle=std::atan2(mirrored?-yx:yx,mirrored?-xx:xx);
    const model::Quaternion rotation{0,0,std::sin(angle/2),std::cos(angle/2)};
    return mirrored?model::engineQuaternionProduct(orientation(value),rotation):model::engineQuaternionProduct(rotation,value);
}
PlanarTransform planarTransform(float degrees,bool flipX,bool flipY) {
    const auto angle=std::remainder(degrees,360.0f);float c,s;
    if(angle==0) {c=1;s=0;}else if(angle==90) {c=0;s=1;}else if(angle==-90) {c=0;s=-1;}else if(std::abs(angle)==180) {c=-1;s=0;}
    else {c=std::cos(angle*0.017453292519943295f);s=std::sin(angle*0.017453292519943295f);}
    const float x=flipX?-1.0f:1.0f,y=flipY?-1.0f:1.0f;
    return {c*x,-s*y,s*x,c*y,flipX!=flipY};
}

void transformWalkmesh(neoshared::bwm::BwmFile& mesh,const PlanarTransform& transform,Vec3 pivot) {
    const auto rotatedPivot=transform.direction(pivot);const Vec3 offset{pivot.x-rotatedPivot.x,pivot.y-rotatedPivot.y,pivot.z-rotatedPivot.z};
    for(auto& v:mesh.vertices) {const auto p=transform.point({v[0],v[1],v[2]},pivot);v={p.x,p.y,p.z};}
    for(std::size_t i=0;i<mesh.faceNormals.size();++i) {
        auto& v=mesh.faceNormals[i];const auto n=transform.direction({v[0],v[1],v[2]});v={n.x,n.y,n.z};
        mesh.faceDistances[i]-=(n.x*offset.x+n.y*offset.y)+n.z*offset.z;
    }
    for(auto& node:mesh.nodes) {
        const auto box=transformedBox({node.minimum[0],node.minimum[1],node.minimum[2]},{node.maximum[0],node.maximum[1],node.maximum[2]},transform,pivot);
        node.minimum={box.first.x,box.first.y,box.first.z};node.maximum={box.second.x,box.second.y,box.second.z};
    }
    if(transform.mirrored) {
        const auto edge=[](std::uint32_t index){return index/3*3+2-index%3;};
        for(auto& face:mesh.faces)std::swap(face.vertices[1],face.vertices[2]);
        for(auto& row:mesh.adjacency) {std::swap(row[0],row[2]);for(auto& adjacent:row)if(adjacent>=0)adjacent=std::int32_t(edge(std::uint32_t(adjacent)));}
        for(auto& boundary:mesh.edges)boundary.index=edge(boundary.index);
        std::size_t begin=0;for(const auto end:mesh.perimeters) {std::reverse(mesh.edges.begin()+begin,mesh.edges.begin()+end);begin=end;}
    }
}

void transformBinaryModel(std::vector<std::uint8_t>& mdl,std::optional<std::vector<std::uint8_t>>& mdx,
        const PlanarTransform& transform,const std::map<std::string,std::string>& names) {
    const auto layout=model::inspectPackedLayout(mdl,mdx);const auto base=layout.payloadOffset;
    ByteView source(mdl.data(),mdl.size(),"Model transform",{});Buffer output{mdl};
    std::optional<Buffer> stream;if(mdx)stream.emplace(Buffer{*mdx});
    const auto vector=[&](std::size_t at){output.vec3(at,transform.direction(source.vec3(at,"vector")));};
    const auto quaternion=[&](std::size_t at) {const auto v=transform.orientation({source.f32(at+4),source.f32(at+8),source.f32(at+12),source.f32(at)});output.quaternion(at,v);};
    const auto box=[&](std::size_t at) {const auto bounds=transformedBox(source.vec3(at,"minimum"),source.vec3(at+12,"maximum"),transform);output.vec3(at,bounds.first);output.vec3(at+12,bounds.second);};
    const auto drawCount=[&](const model::BinaryStructure& region) {
        const auto mesh=layout.structures[*region.parent].offset;const auto primitive=region.name=="Trailing indices"?3u:source.u32(mesh+224);
        const auto allocated=(region.count+1)&~std::size_t(1);return primitive==3?allocated/3*3:allocated;
    };
    box(base+104);
    std::map<std::size_t,std::size_t> vertexCounts;
    for(const auto& region:layout.structures) {
        if(region.name.rfind("Index pool ",0)==0||region.name=="Trailing indices") {
            const auto count=drawCount(region);
            for(std::size_t i=0;i<count;++i)vertexCounts[*region.parent]=std::max(vertexCounts[*region.parent],std::size_t(source.u16(region.offset+i*2))+1);
        }
    }
    for(const auto& region:layout.structures) {
        const auto at=region.offset;
        if(region.file==1) {
            const auto count=std::max(region.count,vertexCounts[*region.parent]);
            ByteView input(mdx->data(),mdx->size(),"Model transform MDX",{});
            for(std::size_t row=0;row<count;++row)for(const auto& field:region.fields) {
                if(field.name!="Position"&&field.name!="Normal"&&field.name.rfind("Tangent basis ",0)!=0)continue;
                for(std::size_t offset=0;offset<field.size;offset+=12) {const auto pos=at+row*region.stride+field.offset+offset;stream->vec3(pos,transform.direction(input.vec3(pos,"vertex channel")));}
            }
        } else if(region.name.rfind("Node: ",0)==0) {vector(at+16);quaternion(at+28);}
        else if(region.name=="Mesh") {box(at+20);vector(at+48);}
        else if(region.name=="AABB node")box(at);
        else if(region.name=="CPU vertices"||region.name=="Rest positions"||region.name=="Position samples"||region.name=="Saber positions"||region.name=="Saber normals"||region.name=="Inverse translations")
            for(std::size_t i=0;i<region.count;++i)vector(at+i*region.stride);
        else if(region.name=="Inverse quaternions")for(std::size_t i=0;i<region.count;++i)quaternion(at+i*region.stride);
        else if(region.name=="Faces")for(std::size_t i=0;i<region.count;++i) {
            const auto face=at+i*region.stride;vector(face);
            if(transform.mirrored) {output.u16(face+28,source.u16(face+30));output.u16(face+30,source.u16(face+28));output.u16(face+20,source.u16(face+24));output.u16(face+24,source.u16(face+20));}
        }
        else if(region.name=="Controllers") {
            const auto node=layout.structures[*region.parent].offset;const auto data=base+source.u32(node+68);const auto words=source.u32(node+72);
            Buffer values{{mdl.begin()+data,mdl.begin()+data+words*4}};
            for(std::size_t i=0;i<region.count;++i) {
                const auto record=at+i*16;const auto type=source.u32(record);const auto rows=source.u16(record+6);const auto first=source.u16(record+10);const auto flags=source.u8(record+12);
                if(type==model::kControllerPosition&&(flags&15)==3&&((flags&0xf0)==0||(flags&0xf0)==0x10))for(std::size_t row=0;row<std::size_t(rows)*((flags&0xf0)==0x10?3:1);++row) {
                    const auto pos=(first+row*3)*4;values.vec3(pos,transform.direction(source.vec3(data+pos,"position key")));
                }
                if(type==model::kControllerOrientation&&rows&&(flags&0xf0)==0&&((flags&15)==2||(flags&15)==4)) {
                    if((flags&15)==2) {
                        const auto position=values.append(std::size_t(rows)*16);output.u16(record+10,std::uint16_t(position/4));output.u8(record+12,4);
                        for(std::size_t row=0;row<rows;++row) {const auto q=transform.orientation(model::unpackEngineControllerQuaternion(source.u32(data+(first+row)*4),layout.game));values.f32(position+row*16,q.x);values.f32(position+row*16+4,q.y);values.f32(position+row*16+8,q.z);values.f32(position+row*16+12,q.w);}
                    } else for(std::size_t row=0;row<rows;++row) {
                        const auto pos=(first+row*4)*4;const auto q=transform.orientation({source.f32(data+pos),source.f32(data+pos+4),source.f32(data+pos+8),source.f32(data+pos+12)});
                        values.f32(pos,q.x);values.f32(pos+4,q.y);values.f32(pos+8,q.z);values.f32(pos+12,q.w);
                    }
                }
            }
            if(values.bytes.size()==words*4)output.copy(data,values.bytes.data(),values.bytes.size());
            else {const auto target=output.append(values.bytes.size());output.copy(target,values.bytes.data(),values.bytes.size());output.u32(node+68,std::uint32_t(target-base));output.u32(node+72,std::uint32_t(values.bytes.size()/4));output.u32(node+76,std::uint32_t(values.bytes.size()/4));}
        }
        else if(transform.mirrored&&(region.name.rfind("Index pool ",0)==0||region.name=="Trailing indices")) {
            const auto mesh=layout.structures[*region.parent].offset;const auto primitive=region.name=="Trailing indices"?3u:source.u32(mesh+224);
            if(primitive==3)for(std::size_t i=0;i<drawCount(region);i+=3) {output.u16(at+(i+1)*2,source.u16(at+(i+2)*2));output.u16(at+(i+2)*2,source.u16(at+(i+1)*2));}
            else if(primitive==4) {
                // Degenerate end vertices reverse strip parity without adding visible triangles.
                const auto count=(region.count+1)&~std::size_t(1);const auto target=output.append((count+2)*2);
                output.u16(target,source.u16(at));output.copy(target+2,mdl.data()+at,count*2);output.u16(target+(count+1)*2,source.u16(at+(count-1)*2));
                const auto pool=std::stoul(region.name.substr(11));output.u32(base+source.u32(mesh+188)+pool*4,std::uint32_t(target-base));output.u32(base+source.u32(mesh+176)+pool*4,std::uint32_t(count+2));
            } else if(primitive==5) {const auto count=drawCount(region);for(std::size_t i=1;i<count;++i)output.u16(at+i*2,source.u16(at+(count-i)*2));}
        }
    }
    for(const auto& [at,name]:dependencies(mdl,layout))if(const auto found=names.find(name);found!=names.end()) {std::fill_n(output.bytes.begin()+at,32,0);output.string(at,found->second,32);}
    if(base)output.u32(4,std::uint32_t(output.bytes.size()-base));
    mdl=std::move(output.bytes);if(stream)mdx=std::move(stream->bytes);
}

std::vector<neoshared::ResourceDocument> transformModelResources(const std::string& sourceName,const std::string& outputName,
        const ResourceResolver& resolver,const PlanarTransform& transform,std::vector<neoshared::ResourceDocument>* originals) {
    constexpr std::uint16_t mdlType=2002,mdxType=3008;
    const auto marker="Transformed model: "+outputName;
    std::map<std::string,std::string> names{{resourceKey(sourceName),outputName}};
    std::vector<std::string> pending{resourceKey(sourceName)};
    std::map<std::string,neoshared::ResourceDocument> sources;std::map<std::string,std::optional<neoshared::ResourceDocument>> streams;
    sources.emplace(pending.front(),resolver(pending.front(),mdlType).value());
    std::size_t serial=0;
    for(std::size_t i=0;i<pending.size();++i) {
        const auto name=pending[i];const auto& source=sources.at(name);
        for(const auto& [at,dependency]:dependencies(source.bytes,model::inspectPackedLayout(source.bytes,std::nullopt)))if(!names.count(dependency)) {
            auto resolved=resolver(dependency,mdlType).value();std::string destination=dependency;
            if(resolved.sourceDescription!=marker)do {destination=outputName.substr(0,10)+"_t"+std::to_string(++serial);}while(resolver(destination,mdlType));
            names.emplace(dependency,destination);pending.push_back(dependency);
            sources.emplace(dependency,std::move(resolved));
        }
        streams.emplace(name,resolver(name,mdxType));
    }
    std::vector<neoshared::ResourceDocument> outputs;
    for(const auto& name:pending) {
        auto source=std::move(sources.at(name));auto stream=std::move(streams.at(name));
        if(originals&&names.at(name)==name) {originals->push_back(source);if(stream)originals->push_back(*stream);}
        std::optional<std::vector<std::uint8_t>> mdx;if(stream)mdx=std::move(stream->bytes);
        transformBinaryModel(source.bytes,mdx,transform,names);source.fileName=names.at(name)+".mdl";source.identity=marker+"#"+names.at(name);source.sourceDescription=marker;outputs.push_back(std::move(source));
        if(stream) {stream->bytes=std::move(*mdx);stream->fileName=names.at(name)+".mdx";stream->identity=marker+"#"+names.at(name)+".mdx";stream->sourceDescription=marker;outputs.push_back(std::move(*stream));}
    }
    return outputs;
}
}
