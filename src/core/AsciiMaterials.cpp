#include "core/AsciiMaterials.hpp"
#include "core/ModelDocument.hpp"
#include <model/AsciiModel.hpp>
#include <model/UvAnimation.hpp>

namespace neomdl {

void finalizeAsciiMaterials(model::Model& model, const ResourceResolver& resolver,
                            const neoshared::bif::JobControl& job) {
    // Compile against a fresh engine texture cache. InitializeTextures creates
    // pending texture references and reads TXI before any image load;
    // HasBumpMap accepts a named pending bump without inspecting its pixels or
    // bump kind. This producer needs metadata, not decoded image pixels.
    std::unordered_map<std::string,bool> compilerBumpMaps;
    const auto compilerHasBumpMap=[&](const std::string& primary,model::GameVersion game) {
        const auto key=neoshared::material::textureResourceKey(primary);
        if(key.empty() || !resolver)return false;
        const auto cacheKey=std::to_string(static_cast<int>(game))+":"+key;
        const auto found=compilerBumpMaps.find(cacheKey);
        if(found!=compilerBumpMaps.end())return found->second;
        job.check();
        std::string source;
        if(const auto sidecar=resolver(key,kTxiResourceType)) {
            source=std::string(sidecar->bytes.begin(),sidecar->bytes.end());
        } else if(const auto processed=resolver(key,kTpcResourceType)) {
            // GetResource resolves missing TXI through the processed texture
            // (K1 0x377e6c, K2 0x133640), without decoding its pixels.
            source=neoshared::texture::readTpcTxiBytes(processed->bytes);
        }
        const auto txi=neoshared::material::parseAsciiMaterialTxi(source,
            game==model::GameVersion::Kotor1);
        const bool hasBump=txi.engineTextureNames &&
            !neoshared::material::textureResourceKey(txi.engineTextureNames->bumpMap).empty();
        compilerBumpMaps.emplace(cacheKey,hasBump);
        return hasBump;
    };
    if(!model.asciiBuilt||model.asciiMaterialsFinalized)return;
    for(auto& allocation:model.asciiMdxAllocations) {
        if(allocation.materialTangents && !(allocation.source->mdxBitmap&0x80u) &&
           (allocation.source->asciiTangents?*allocation.source->asciiTangents:
                compilerHasBumpMap(allocation.primaryTexture,model.game))) {
            // Cached reference assets copy the Model while retaining its
            // source snapshots. Finalize this copy's deferred producer
            // once, without changing another copy's completed stride.
            // Contained model-owned normal/UV pools keep their identity.
            allocation.source=std::make_shared<model::Mesh>(*allocation.source);
            model::rebuildAsciiTangentBasis(*allocation.source,job);
            allocation.rowSize=allocation.source->mdxRowSize;
        }
    }
    for(std::size_t i=0;i<model.meshes.size();++i) {
        auto& mesh=model.meshes[i];
        if(mesh.asciiTangentSource && !(mesh.mdxBitmap&0x80u) &&
           (mesh.asciiTangents?*mesh.asciiTangents:compilerHasBumpMap(mesh.texture0,model.game)))
            model::rebuildAsciiTangentBasis(mesh,job);
        if(mesh.asciiMdxAllocation) {
            auto& allocation=model.asciiMdxAllocations[*mesh.asciiMdxAllocation];
            allocation.rowSize=mesh.mdxRowSize;
            allocation.source=std::make_shared<model::Mesh>(mesh);
        }
    }
    for(auto& animation:model.animations)
        for(auto& node:animation.nodes)
            if(node.animmesh && node.animmesh->authoringMesh &&
               node.animmesh->authoringMesh->asciiMdxAllocation)
                node.animmesh->authoringMesh=model.asciiMdxAllocations[
                    *node.animmesh->authoringMesh->asciiMdxAllocation].source;
    model::initializeProceduralUvPools(model);
    model.asciiMaterialsFinalized=true;
}

} // namespace neomdl
