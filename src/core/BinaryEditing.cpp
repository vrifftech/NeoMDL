#include "core/BinaryEditing.hpp"
#include "core/ModelFilenames.hpp"
#include <neoshared/PathUtf8.hpp>

#include <algorithm>

namespace neomdl {

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
    const std::map<std::size_t,BinaryMeshEdit>& edits, const std::filesystem::path& output) {
    auto mdl = file.mdl;
    for (const auto& [index,edit] : edits) {
        const auto base = *file.model.nodes[index].packedOffset + 80;
        for (std::size_t channel = 0; channel < 2; ++channel) if (edit.textures[channel]) {
            std::array<std::uint8_t,32> name{};
            for (std::size_t i = 0; i < edit.textures[channel]->size(); ++i)
                name.at(i) = static_cast<std::uint8_t>((*edit.textures[channel])[i]);
            std::copy(name.begin(),name.end(),mdl.begin()+base+88+channel*32);
        }
        for (std::size_t flag = 0; flag < 5; ++flag)
            if (edit.flags[flag]) mdl[base+309+flag] = *edit.flags[flag] ? 1 : 0;
    }
    std::vector<CollisionOutput> outputs{{output,std::move(mdl)}};
    if (file.mdx) {
        auto path = output; path.replace_extension(".mdx"); outputs.push_back({path,*file.mdx});
    }
    for (const auto& [kind,bytes] : file.collisions) outputs.push_back({collisionPath(output,kind),bytes});
    return outputs;
}

} // namespace neomdl
