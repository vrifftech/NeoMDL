#include "core/BinaryEditing.hpp"
#include "core/ModelFilenames.hpp"
#include <model/AsciiModelInternal.hpp>
#include <model/PackedExportInternal.hpp>
#include <neoshared/PathUtf8.hpp>

#include <algorithm>

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
        for (std::size_t flag = 0; flag < 5; ++flag)
            if (edit.flags[flag]) mdl.u8(base+309+flag,*edit.flags[flag] ? 1 : 0);
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
    std::vector<CollisionOutput> outputs{{output,std::move(mdl.bytes)}};
    if (mdx) {
        auto path = output; path.replace_extension(".mdx"); outputs.push_back({path,std::move(mdx->bytes)});
    }
    for (const auto& [kind,bytes] : file.collisions) outputs.push_back({collisionPath(output,kind),bytes});
    return outputs;
}

} // namespace neomdl
