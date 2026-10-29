#include "core/BinaryEditing.hpp"
#include "core/ModelFilenames.hpp"
#include <model/AsciiModelInternal.hpp>
#include <model/PackedExportInternal.hpp>
#include <neoshared/PathUtf8.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace neomdl {
namespace {

std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint32_t value{};
    for (unsigned i=0;i<4;++i) value |= std::uint32_t(bytes[at+i]) << (8*i);
    return value;
}

std::vector<model::TangentBasis> tangentSums(model::Mesh mesh,
    const neoshared::bif::JobControl& job) {
    auto source = std::make_shared<model::AsciiTangentSource>();
    for (const auto& vertex : mesh.vertices) {
        source->generatedPositions.push_back(vertex.position);
        source->generatedTexcoords.push_back(vertex.texcoord0);
    }
    std::vector<model::ascii::TangentFace> triangles;
    for (const auto& face : mesh.faces) {
        source->firstCornerNormals.push_back(mesh.normalAt(face.indices[0]));
        model::ascii::TangentFace triangle;
        for (unsigned corner=0;corner<3;++corner) {
            const auto& vertex=mesh.vertices[face.indices[corner]];
            triangle.positions[corner]=vertex.position;
            triangle.texcoords[corner]=vertex.texcoord0;
        }
        triangles.push_back(triangle);
    }
    // Reuse the geometry arithmetic without parsing or rebuilding ASCII.
    mesh.asciiTangentSource=std::move(source);
    auto faces=model::ascii::tangentFaceBases(mesh,job);
    model::ascii::orientSingularTangentComponents(triangles,faces,job);
    std::vector<model::TangentBasis> sums(mesh.vertices.size());
    const auto add=[](model::Vec3& a,model::Vec3 b) {a.x+=b.x;a.y+=b.y;a.z+=b.z;};
    for (std::size_t i=0;i<mesh.faces.size();++i)
        for (const auto index : mesh.faces[i].indices) {
            add(sums[index].bitangent,faces[i].bitangent);
            add(sums[index].tangent,faces[i].tangent);
            add(sums[index].normal,faces[i].normal);
        }
    return sums;
}

model::Bounds geometryBounds(const std::vector<model::Vec3>& positions) {
    model::Bounds bounds{positions.front(),positions.front(),true};
    for (const auto p : positions) {
        bounds.minimum.x=std::min(bounds.minimum.x,p.x);bounds.maximum.x=std::max(bounds.maximum.x,p.x);
        bounds.minimum.y=std::min(bounds.minimum.y,p.y);bounds.maximum.y=std::max(bounds.maximum.y,p.y);
        bounds.minimum.z=std::min(bounds.minimum.z,p.z);bounds.maximum.z=std::max(bounds.maximum.z,p.z);
    }
    return bounds;
}

void updateBinaryGeometry(model::packed_export::Buffer& mdl,const BinaryModelFile& file,
                          std::size_t index,const BinaryMeshEdit& edit) {
    const auto& node=file.model.nodes[index];const auto& mesh=file.model.meshes[*node.mesh];
    const auto base=*node.packedOffset+80;
    std::vector<model::Vec3> positions;
    for (const auto& vertex : mesh.vertices) positions.push_back(vertex.position);
    const auto bounds=geometryBounds(positions);
    const model::Vec3 center{(bounds.minimum.x+bounds.maximum.x)*0.5f,
        (bounds.minimum.y+bounds.maximum.y)*0.5f,(bounds.minimum.z+bounds.maximum.z)*0.5f};
    float radius{};
    for (const auto p : positions) radius=std::max(radius,std::hypot(p.x-center.x,p.y-center.y,p.z-center.z));
    mdl.vec3(base+20,bounds.minimum);mdl.vec3(base+32,bounds.maximum);
    mdl.f32(base+44,radius);mdl.vec3(base+48,center);
    if (mesh.cpuPositions && !mesh.cpuPositions->empty()) {
        auto cpu=*mesh.cpuPositions;
        const auto pool=12+word(file.mdl,base+(file.model.game==model::GameVersion::Kotor1 ? 328u : 336u));
        for (const auto row : edit.positionRows) {cpu[row]=positions[row];mdl.vec3(pool+row*12,cpu[row]);}
        positions=std::move(cpu);
    }
    if (mesh.dangly) {
        const auto extension=*node.packedOffset+(file.model.game==model::GameVersion::Kotor1 ? 412u : 420u);
        const auto rest=12+word(file.mdl,extension+24);
        for (const auto row : edit.positionRows) mdl.vec3(rest+row*12,mesh.vertices[row].position);
    }
    const auto faces=12+word(file.mdl,base+8);
    for (std::size_t i=0;i<mesh.faces.size();++i) {
        const auto& face=mesh.faces[i];
        if (!std::any_of(face.indices.begin(),face.indices.end(),[&](auto row) {return edit.positionRows.count(row);})) continue;
        const auto a=positions[face.indices[0]],b=positions[face.indices[1]],c=positions[face.indices[2]];
        const model::Vec3 p{b.x-a.x,b.y-a.y,b.z-a.z},q{c.x-a.x,c.y-a.y,c.z-a.z};
        const auto normal=model::normalize({p.y*q.z-p.z*q.y,p.z*q.x-p.x*q.z,p.x*q.y-p.y*q.x});
        mdl.vec3(faces+i*32,normal);mdl.f32(faces+i*32+12,-(normal.x*a.x+normal.y*a.y+normal.z*a.z));
    }
    if (node.aabb && node.aabb->root) {
        const auto& tree=*node.aabb;
        std::function<model::Bounds(std::size_t)> update=[&](std::size_t at) {
            const auto& entry=tree.nodes[at];model::Bounds box;
            if (entry.leaf()) {
                const auto& face=mesh.faces[entry.faceIndex];
                box=geometryBounds({positions[face.indices[0]],positions[face.indices[1]],positions[face.indices[2]]});
            } else {
                const auto left=update(entry.firstChild),right=update(entry.secondChild);
                box=geometryBounds({left.minimum,left.maximum,right.minimum,right.maximum});
            }
            mdl.vec3(12+entry.sourceOffset,box.minimum);mdl.vec3(12+entry.sourceOffset+12,box.maximum);return box;
        };
        update(*tree.root);
    }
}

} // namespace

BinaryModelFile readBinaryModelFile(const std::filesystem::path& path,
    const neoshared::bif::JobControl& job) {
    ResourceLoadScope scope(job);
    BinaryModelFile file; file.path = std::filesystem::absolute(path);
    file.inputs.push_back(file.path); file.mdl = readResourceFile(file.path);
    const auto directory = file.path.parent_path();
    if (const auto mdx = uniqueResourceFile(directory,modelFileStem(path)+".mdx")) {
        file.inputs.push_back(*mdx); file.mdx = readResourceFile(*mdx);
    }
    const std::vector<std::uint8_t> emptyMdx;
    file.model = model::decodePackedModelBytes(file.mdl,file.mdx ? *file.mdx : emptyMdx,
        neoshared::pathToUtf8(path),job,false);
    for (const auto kind : {CollisionKind::Placeable,CollisionKind::DoorClosed,
        CollisionKind::DoorOpen1,CollisionKind::DoorOpen2,CollisionKind::Room}) {
        if (const auto companion = uniqueResourceFile(directory,neoshared::pathToUtf8(collisionPath(path,kind).filename()))) {
            file.inputs.push_back(*companion); file.collisions[kind] = readResourceFile(*companion);
        }
    }
    return file;
}

std::string binaryMeshTexture(const BinaryModelFile& file, std::size_t node, std::size_t channel) {
    const auto begin = file.mdl.begin() + *file.model.nodes[node].packedOffset + 80 + 88 + channel*32;
    return {begin,std::find(begin,begin+32,0)};
}

void setBinaryStaticController(BinaryModelFile& file,std::size_t index,std::uint32_t type,
                               const std::vector<float>& values) {
    const auto& node=file.model.nodes[index];const auto at=*node.packedOffset;
    model::packed_export::Buffer out{file.mdl};
    const auto count=word(file.mdl,at+60),words=word(file.mdl,at+72);
    const auto data=out.append((words+1+values.size())*4);
    for (std::size_t i=0;i<words;++i) out.u32(data+i*4,word(file.mdl,12+word(file.mdl,at+68)+i*4));
    out.f32(data+words*4,0.0f);
    for (std::size_t i=0;i<values.size();++i) out.f32(data+(words+1+i)*4,values[i]);
    auto controller=std::find_if(node.controllers.begin(),node.controllers.end(),[&](const auto& c) {return c.type==type;});
    std::size_t record;
    if (controller==node.controllers.end()) {
        const auto keys=out.append((count+1)*16);
        for (std::size_t i=0;i<count*16;++i) out.u8(keys+i,file.mdl[12+word(file.mdl,at+56)+i]);
        out.u32(at+56,keys-12);out.u32(at+60,count+1);out.u32(at+64,count+1);record=keys+count*16;
    } else record=12+word(file.mdl,at+56)+static_cast<std::size_t>(controller-node.controllers.begin())*16;
    out.u32(record,type);out.u16(record+4,0xffffu);out.u16(record+6,1);
    out.u16(record+8,static_cast<std::uint16_t>(words));out.u16(record+10,static_cast<std::uint16_t>(words+1));
    out.u8(record+12,static_cast<std::uint8_t>(values.size()));
    out.u32(at+68,data-12);out.u32(at+72,static_cast<std::uint32_t>(words+1+values.size()));
    out.u32(at+76,static_cast<std::uint32_t>(words+1+values.size()));
    if (type==model::kControllerPosition) out.vec3(at+16,{values[0],values[1],values[2]});
    if (type==model::kControllerOrientation) out.quaternion(at+28,{values[0],values[1],values[2],values[3]});
    out.u32(4,static_cast<std::uint32_t>(out.bytes.size()-12));file.mdl=std::move(out.bytes);
    const std::vector<std::uint8_t> empty;
    file.model=model::decodePackedModelBytes(file.mdl,file.mdx ? *file.mdx : empty,neoshared::pathToUtf8(file.path),{},false);
}

std::vector<CollisionOutput> binaryEditOutputs(const BinaryModelFile& file,
    const std::map<std::size_t,BinaryMeshEdit>& edits, const std::filesystem::path& output,
    const neoshared::bif::JobControl& job) {
    model::packed_export::Buffer mdl{file.mdl};
    std::optional<model::packed_export::Buffer> mdx;
    if (file.mdx) mdx.emplace(model::packed_export::Buffer{*file.mdx});
    for (const auto& [index,edit] : edits) {
        const auto base = *file.model.nodes[index].packedOffset + 80;
        for (std::size_t channel = 0; channel < 2; ++channel) if (edit.textures[channel]) {
            std::array<std::uint8_t,32> name{};
            for (std::size_t i = 0; i < edit.textures[channel]->size(); ++i)
                name.at(i) = static_cast<std::uint8_t>((*edit.textures[channel])[i]);
            mdl.copy(base+88+channel*32,name.data(),name.size());
        }
        for (std::size_t flag = 0; flag < edit.flags.size(); ++flag)
            if (edit.flags[flag]) mdl.u8(base+308+flag,*edit.flags[flag] ? 1 : 0);
        for (const auto& [offset,bytes] : edit.headerFields) mdl.copy(base+offset,bytes.data(),bytes.size());
        if (!edit.positionRows.empty()) updateBinaryGeometry(mdl,file,index,edit);
        if (edit.tangentBasis || edit.regenerateTangents) {
            const auto& mesh=file.model.meshes[*file.model.nodes[index].mesh];
            const bool stored=(mesh.mdxBitmap&0x80u)!=0;
            const bool enabled=edit.tangentBasis.value_or(stored);
            mdl.u32(base+256,enabled ? mesh.mdxBitmap|0x80u : mesh.mdxBitmap&~0x80u);
            if (!enabled) mdl.u32(base+288,0xffffffffu);
            else if (!stored || edit.regenerateTangents) {
                const auto offsetField=base+(file.model.game==model::GameVersion::Kotor1 ? 324u : 332u);
                const auto source=word(file.mdl,offsetField);
                const auto tangent=stored ? mesh.mdxTangentOffset : mesh.mdxRowSize;
                const auto stride=mesh.mdxRowSize+(stored ? 0u : 36u);
                const auto rows=static_cast<std::size_t>(mesh.enginePrelightVertexCount)+1;
                const auto destination=stored ? source : mdx->append(rows*stride);
                // Adding the channel relocates this mesh; regeneration reuses its stream.
                if (!stored) {
                    for (std::size_t row=0;row<rows;++row)
                        mdx->copy(destination+row*stride,file.mdx->data()+source+row*mesh.mdxRowSize,mesh.mdxRowSize);
                    mdl.u32(offsetField,destination);
                    mdl.u32(base+252,stride);
                    mdl.u32(base+288,tangent);
                    mdl.u32(8,static_cast<std::uint32_t>(mdx->bytes.size()));
                    mdl.u32(12+0xb0,static_cast<std::uint32_t>(mdx->bytes.size()));
                }
                const auto sums=tangentSums(mesh,job);
                for (const auto& face : mesh.faces)
                    for (const auto row : face.indices) {
                        const auto at=destination+static_cast<std::size_t>(row)*stride+tangent;
                        mdx->vec3(at,model::ascii::normalizeTangentBasis(sums[row].bitangent));
                        mdx->vec3(at+12,model::ascii::normalizeTangentBasis(sums[row].tangent));
                        mdx->vec3(at+24,model::ascii::normalizeTangentBasis(sums[row].normal));
                    }
            }
        }
    }
    if (std::any_of(edits.begin(),edits.end(),[](const auto& entry) {
        return !entry.second.positionRows.empty()||entry.second.transformChanged||entry.second.bindingChanged;
    })) {
        const std::vector<std::uint8_t> empty;
        const auto current=model::decodePackedModelBytes(mdl.bytes,mdx ? mdx->bytes : empty,
            neoshared::pathToUtf8(file.path),job,false);
        const auto bounds=model::constructedBindPose(current,job).bounds;
        mdl.vec3(12+0x68,bounds.minimum);mdl.vec3(12+0x74,bounds.maximum);
        mdl.f32(12+0x80,std::hypot(std::max(std::abs(bounds.minimum.x),std::abs(bounds.maximum.x)),
            std::max(std::abs(bounds.minimum.y),std::abs(bounds.maximum.y)),
            std::max(std::abs(bounds.minimum.z),std::abs(bounds.maximum.z))));
    }
    std::vector<CollisionOutput> outputs{{output,std::move(mdl.bytes)}};
    if (mdx) {
        auto path = output; path.replace_extension(".mdx"); outputs.push_back({path,std::move(mdx->bytes)});
    }
    for (const auto& [kind,bytes] : file.collisions) outputs.push_back({collisionPath(output,kind),bytes});
    return outputs;
}

} // namespace neomdl
