#include <model/UvAnimation.hpp>
#include <material/ProceduralTexture.hpp>
#include <material/Runtime.hpp>
#include "core/ModelDocument.hpp"
#include <model/AsciiModel.hpp>

#include <neoshared/PathUtf8.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace neomdl {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    return value;
}

std::string resourceStem(const std::string& name) {
    auto leaf = name;
    const auto slash = leaf.find_last_of("/\\");
    if (slash != std::string::npos) leaf.erase(0, slash + 1u);
    const auto dot = leaf.find_last_of('.');
    if (dot != std::string::npos) leaf.erase(dot);
    return leaf;
}

std::optional<std::filesystem::path> uniqueCaseInsensitiveFile(
    const std::filesystem::path& directory, const std::string& filename) {
    try { return uniqueResourceFile(directory, filename); }
    catch(const neoshared::bif::JobCancelled&) { throw; }
    catch(const std::runtime_error& error) { throw model::ModelError(error.what()); }
}

std::string modelResourceExtension(std::uint16_t type) {
    switch (type) {
    case kMdlResourceType: case kMdxResourceType: case kTpcResourceType:
    case kTxbResourceType: case kTgaResourceType: case kDdsResourceType:
    case kTxiResourceType: return "." + resourceExtension(type);
    default: return {};
    }
}

neoshared::ResourceDocument readLocalResource(const std::filesystem::path& path,
                                             std::uint16_t type) {
    neoshared::ResourceDocument resource;
    resource.fileName = neoshared::pathToUtf8(path.filename());
    resource.type = type;
    resource.bytes = readResourceFile(path);
    resource.identity = resourceFileIdentity(path);
    resource.sourceDescription = neoshared::genericPathToUtf8(path);
    resource.protectedInputs.push_back(path);
    return resource;
}

ResourceResolver directoryResourceResolver(const std::filesystem::path& directory) {
    const auto root = std::filesystem::absolute(directory).lexically_normal();
    return [root](const std::string& name, std::uint16_t type)
            -> std::optional<neoshared::ResourceDocument> {
        resourceLoadJob().update(0u, 0u, "Reading model resource: " + name);
        const auto extension = modelResourceExtension(type);
        if (extension.empty()) return std::nullopt;
        // Resolver input is already a resource stem. Preserve its dots:
        // an explicitly selected foo.ascii.mdl
        // and its foo.ascii.mdx companion must not resolve as foo.mdl/foo.mdx.
        auto stem = name;
        const auto slash = stem.find_last_of("/\\");
        if (slash != std::string::npos) stem.erase(0u, slash + 1u);
        if (stem.empty()) return std::nullopt;
        const auto path = uniqueCaseInsensitiveFile(root, stem + extension);
        if (!path) return std::nullopt;

        return readLocalResource(*path, type);
    };
}

ResourceResolver overlayResolver(ResourceResolver first, ResourceResolver second) {
    if (!first) return second;
    if (!second) return first;
    return [first = std::move(first), second = std::move(second)](
               const std::string& name, std::uint16_t type)
            -> std::optional<neoshared::ResourceDocument> {
        if (auto resource = first(name, type)) return resource;
        return second(name, type);
    };
}

std::string hierarchyResref(const std::string& name) {
    // Authored model dependencies are already ResRefs, not filenames.
    // ReadSync(char*) forwards the complete name independently of its .mdl
    // and .mdx resource types in all four references. Dots distinguish models.
    auto result = name;
    const auto slash = result.find_last_of("/\\");
    if (slash != std::string::npos) result.erase(0u, slash + 1u);
    const auto folded = lower(result);
    // Both engine sentinel spellings are accepted by the resource adapter.
    // NONE remains a valid asset name in both games.
    if (folded == "null" || folded == "nullptr") return {};
    return result;
}

std::string bytesToText(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

struct ReferenceAsset {
    neomdl::model::Model model;
    std::vector<neomdl::model::Model> supermodels;
};

// FindModel searches live registered models before requesting a resource.
// Retained superclass chains are the live registrations known at document
// handoff. Preserve their declared names and remaining inheritance suffix;
// a resource alias need not equal the registered Model name. This deliberately
// does not reconstruct unrelated registrations or guess engine midpoint
// selection when more than one retained model has the same name.
class RetainedReferenceAssets {
public:
    void remember(const std::vector<neomdl::model::Model>& chain,
                  std::shared_ptr<const ReferenceAsset> owner={}) {
        for (std::size_t index=0;index<chain.size();++index) {
            const auto key=lower(chain[index].name);
            if (key.empty()) continue;
            const auto found=assets_.find(key);
            if (found!=assets_.end()) { found->second={}; continue; }
            assets_.emplace(key,Candidate{&chain,index,owner});
        }
    }
    std::shared_ptr<ReferenceAsset> find(const std::string& key) const {
        const auto found=assets_.find(key);
        if (found==assets_.end() || !found->second.chain) return {};
        const auto& candidate=found->second;
        const auto& chain=*candidate.chain;
        auto asset=std::make_shared<ReferenceAsset>();
        asset->model=chain[candidate.index];
        asset->supermodels.assign(chain.begin()+candidate.index+1u,chain.end());
        return asset;
    }
private:
    struct Candidate {
        const std::vector<neomdl::model::Model>* chain{};
        std::size_t index{};
        // The document's root chain stays stable throughout either loader.
        // Newly decoded reference chains retain their owner even if a later
        // cache insertion throws; no candidate borrows a temporary asset.
        std::shared_ptr<const ReferenceAsset> owner;
    };
    std::unordered_map<std::string,Candidate> assets_;
};

template <typename Visitor>
void visitModelPartOccurrences(const neomdl::model::Model& model, Visitor&& visitor) {
    std::vector<bool> active(model.nodes.size(), false);
    std::size_t partIndex{};
    std::function<void(std::size_t)> walk = [&](std::size_t index) {
        resourceLoadJob().check();
        if (index >= model.nodes.size() || active[index])
            throw model::ModelError("Invalid reference Part tree");
        active[index] = true;
        // CreateReferenceObjects visits every live Part, including repeated
        // source nodes. Use the same root/DFS occurrence order as
        // instantiateModelParts; a nested lookup can change a later choice.
        visitor(partIndex++, model.nodes[index]);
        for (const auto child : model.nodes[index].children) walk(child);
        active[index] = false;
    };
    for (std::size_t index = 0u; index < model.nodes.size(); ++index)
        if (!model.nodes[index].parent) walk(index);
}

} // namespace

model::AsciiResourceResolver looseAsciiResources(const std::filesystem::path& directory) {
    const auto root=std::filesystem::absolute(directory).lexically_normal();
    return [root](const std::string& name,const std::string& extension)
        ->std::optional<std::vector<std::uint8_t>> {
        resourceLoadJob().check();
        const auto file=uniqueResourceFile(root,name+extension);
        if(!file)return {};
        return readResourceFile(*file);
    };
}

bool ModelDocument::openFile(const std::filesystem::path& requestedPath,
                             const ResourceResolver& fallbackResolver,
                             const neoshared::bif::JobControl& job,
                             std::optional<model::GameVersion> asciiGame) {
    const ResourceLoadScope loading(job);
    job.update(0u, 0u, "Opening model: " + neoshared::pathToUtf8(requestedPath.filename()));
    auto mdlPath = requestedPath;
    if (lower(neoshared::pathToUtf8(mdlPath.extension())) == ".mdx") {
        const auto sibling = uniqueCaseInsensitiveFile(
            mdlPath.parent_path().empty() ? std::filesystem::path{"."} : mdlPath.parent_path(),
            neoshared::pathToUtf8(mdlPath.stem()) + ".mdl");
        if (!sibling) throw neomdl::model::ModelError(
            "Opening an MDX requires a unique same-name MDL companion.");
        mdlPath = *sibling;
    }
    if (lower(neoshared::pathToUtf8(mdlPath.extension())) != ".mdl") {
        throw neomdl::model::ModelError(
            "NeoMDL opens .mdl models and their .mdx companions.");
    }

    const auto directory = mdlPath.parent_path().empty()
        ? std::filesystem::path{"."} : mdlPath.parent_path();
    const auto localResolver = directoryResourceResolver(directory);
    auto primary = readLocalResource(mdlPath, kMdlResourceType);

    const auto resolver = overlayResolver(localResolver, fallbackResolver);
    // AurResGet's standalone authoring path can open .lmt sidecars. The
    // installed game's resource callback has no .lmt type mapping.
    openResource(std::move(primary), resolver,job,asciiGame,looseAsciiResources(directory));
    // Local documents retain their filesystem identity even though companion
    // resources may have come from an installed-game archive session.
    runtimeScopePinned_=bool(fallbackResolver);
    path_ = mdlPath;
    return true;
}

bool ModelDocument::openResource(neoshared::ResourceDocument resource,
                                 const ResourceResolver& suppliedResolver,
                                 const neoshared::bif::JobControl& job,
                                 std::optional<model::GameVersion> asciiGame,
                                 const model::AsciiResourceResolver& asciiResources) {
    const ResourceLoadScope loading(job);
    job.update(0u, 0u, "Resolving model companions: " + resource.fileName);
    auto trace=std::make_shared<ResourceTrace>();
    trace->record(resource.identity,resourceStem(resource.fileName),resource.type,&resource);
    const auto resolver=traceResources(suppliedResolver,trace,resource.identity);
    const auto stem = resourceStem(resource.fileName);
    if (stem.empty()) {
        throw neomdl::model::ModelError("The selected resource has no usable model ResRef.");
    }

    std::vector<std::string> lookupMessages;
    std::vector<std::uint8_t> mdx;
    neoshared::ResourceDocument mdlResource;
    if (resource.type == kMdlResourceType) {
        mdlResource = std::move(resource);
        if (resolver && !model::isAsciiModelInput(mdlResource.bytes)) {
            // Missing is a valid CPU-only model case. Ambiguous, corrupt or
            // wrong-type companions are not permission to silently omit MDX.
            if (auto companion = resolver(stem, kMdxResourceType)) mdx = std::move(companion->bytes);
        }
    } else if (resource.type == kMdxResourceType) {
        mdx = std::move(resource.bytes);
        if (!resolver) {
            throw neomdl::model::ModelError(
                "Opening an MDX resource requires the host resource resolver to find its MDL companion.");
        }
        auto companion = resolver(stem, kMdlResourceType);
        if (!companion) {
            throw neomdl::model::ModelError(
                "Opening an MDX resource requires a same-name MDL companion in the game session.");
        }
        mdlResource = std::move(*companion);
        for (const auto& input : resource.protectedInputs) {
            if (std::find(mdlResource.protectedInputs.begin(), mdlResource.protectedInputs.end(), input) ==
                mdlResource.protectedInputs.end()) {
                mdlResource.protectedInputs.push_back(input);
            }
        }
    } else {
        throw neomdl::model::ModelError(
            "NeoMDL requires an MDL resource; MDX resources are accepted only with a same-name MDL companion.");
    }

    // FindModel is called while ASCII commands are being interpreted. Route
    // those model/MDX reads through this document's captured resource resolver;
    // authoring sidecars retain their existing standalone resource callback.
    const model::AsciiResourceResolver loadResources=
        [resolver,asciiResources](const std::string& name,const std::string& extension)
            ->std::optional<std::vector<std::uint8_t>> {
        if(resolver && (extension==".mdl" || extension==".mdx")) {
            auto resource=resolver(name,extension==".mdl"?kMdlResourceType:kMdxResourceType);
            if(resource) return std::move(resource->bytes);
            return {};
        }
        return asciiResources ? asciiResources(name,extension) : std::nullopt;
    };
    auto nextModel = neomdl::model::decodeModelBytes(
        mdlResource.bytes, mdx, mdlResource.sourceDescription, job,asciiGame,loadResources);
    originalAscii_ = nextModel.asciiBuilt
        ? std::optional<std::string>{std::string(mdlResource.bytes.begin(), mdlResource.bytes.end())}
        : std::nullopt;
    asciiResources_=loadResources;
    runtimeScopePinned_=true;resolver_=resolver;trace_=std::move(trace);
    resourceParent_.reset();
    sourceModel_.reset();
    open_ = false;
    textures_.clear();
    textureLoadOrder_.clear();
    textureMetadata_.clear();
    textureTxiAvailable_.clear();
    materials_.clear();
    referenceMaterials_.clear();
    appearanceEnvironmentMap_.clear();
    messages_ = std::move(lookupMessages);
    supermodels_.clear();
    references_.clear();
    chunkModels_.clear();
    animations_.clear();
    model_ = std::move(nextModel);
    path_.clear();
    identity_ = mdlResource.identity.empty() ? "neomdl:resource:" + stem : mdlResource.identity;
    title_ = mdlResource.fileName.empty() ? stem + ".mdl" : mdlResource.fileName;
    sourceDescription_ = mdlResource.sourceDescription;
    appendParserWarnings(model_);
    job.update(0u, 0u, "Loading supermodels: " + stem);
    loadSupermodelsFromResolver(resolver, stem);
    rebuildAnimations();
    job.update(0u, 0u, "Loading reference models: " + stem);
    loadReferencesFromResolver(resolver, stem);
    job.update(0u, 0u, "Loading particle models: " + stem);
    loadChunkModels(resolver);
    loadTexturesFromResolver(resolver);
    rebuildMaterials();
    const auto auxiliary=auxiliaryTextures();
    if(!auxiliary.empty()) { loadTexturesFromResolver(resolver,auxiliary); rebuildMaterials(); }
    // Compile against a fresh engine texture cache. InitializeTextures creates
    // pending texture references and reads TXI before any image load;
    // HasBumpMap accepts a named pending bump without inspecting its pixels or
    // bump kind. The decoded materials above remain the runtime render view.
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
            source=bytesToText(sidecar->bytes);
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
    const auto finalizeAscii=[&](model::Model& model) {
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
    };
    finalizeAscii(model_);
    for(std::size_t i=0;i<references_.size();++i)
        finalizeAscii(references_[i].model);
    instantiateParts();
    job.update(1u, 1u, "Loaded model: " + stem);
    open_ = true;
    return true;
}

std::unique_ptr<ModelDocument> ModelDocument::rebuildAscii(const std::string& text,
    const neoshared::bif::JobControl& job) const {
    neoshared::ResourceDocument resource;
    resource.fileName = title_;
    resource.type = kMdlResourceType;
    resource.bytes.assign(text.begin(), text.end());
    resource.identity = identity_;
    resource.sourceDescription = title_ + " (edited ASCII)";
    resource.protectedInputs = openPaths();
    auto result = std::make_unique<ModelDocument>();
    result->openResource(std::move(resource), resolver_, job, model_.game, asciiResources_);
    result->path_ = path_;
    result->runtimeScopePinned_ = runtimeScopePinned_;
    return result;
}

void ModelDocument::instantiateParts() {
    // CreateInstanceTreeR makes a Part for every child-list entry. Keep the
    // finalized source graph separate: compiling the Part copies would lose
    // MdlNode sharing and publish the same model-owned MDX allocation again.
    sourceModel_=std::make_shared<model::Model>(std::move(model_));
    model_=model::instantiateModelParts(sourceModel_,resourceLoadJob());
    const auto sourceMaterials=std::move(materials_);
    materials_.clear();materials_.reserve(model_.partSourceMeshes.size());
    for(const auto source:model_.partSourceMeshes)
        materials_.push_back(sourceMaterials[source]);

    // Discovery already selected one asset for each reference Part, in
    // engine creation order. Expanding shared MdlNodes here would replay an
    // earlier selection after nested resource reads changed the registry.
    // Keep each selected source graph intact for model-owned producers/export.
    for(std::size_t index=0;index<references_.size();++index) {
        auto& instance=references_[index];
        auto source=std::make_shared<const model::Model>(std::move(instance.model));
        instance.model=model::instantiateModelParts(std::move(source),resourceLoadJob());
        instance.pose=model::constructedBindPose(instance.model,resourceLoadJob());
        auto sourceMaterials=std::move(referenceMaterials_[index]);
        auto& materials=referenceMaterials_[index];
        materials.clear();
        materials.reserve(instance.model.partSourceMeshes.size());
        for(const auto mesh:instance.model.partSourceMeshes)
            materials.push_back(sourceMaterials[mesh]);
    }
}

std::shared_ptr<ModelDocument> ModelDocument::sceneView(
    std::shared_ptr<const ModelDocument> source, model::Model instanceModel,
    const std::string& identitySuffix) {
    resourceLoadJob().update(0u, 0u, "Assembling model scene: " + identitySuffix);
    if (!source || !source->isOpen() ||
        instanceModel.nodes.size() != source->model().nodes.size() ||
        instanceModel.meshes.size() != source->model().meshes.size() ||
        instanceModel.sceneNodeAdmission.size() != instanceModel.nodes.size())
        throw model::ModelError("Invalid source or node/mesh map for scene model view");
    auto result = std::make_shared<ModelDocument>();
    result->resourceParent_ = source;
    result->appearanceEnvironmentMap_=source->appearanceEnvironmentMap_;
    result->resolver_ = source->resolver_;result->asciiResources_=source->asciiResources_;result->trace_ = source->trace_;
    result->runtimeScopePinned_=source->runtimeScopePinned_;
    result->open_ = true;
    result->identity_ = source->identity_ + identitySuffix;
    result->title_ = source->title_;
    result->sourceDescription_ = source->sourceDescription_;
    result->path_ = source->path_;
    result->sourceModel_ = source->sourceModel_;
    instanceModel.animationSynchronizationModel =
        std::shared_ptr<const model::Model>(source, &source->model());
    result->model_ = std::move(instanceModel);
    result->supermodels_ = source->supermodels_;
    result->textureLoadOrder_ = source->textureLoadOrder_;
    result->messages_ = source->messages_;
    result->allowChunkModels_ = false;

    const auto count = source->references_.size();
    std::vector<std::uint8_t> visited(count), admitted(count);
    std::function<bool(std::size_t)> keep = [&](std::size_t index) {
        resourceLoadJob().check();
        if (index >= count) return false;
        if (visited[index] == 2u) return admitted[index] != 0u;
        if (visited[index] == 1u)
            throw model::ModelError("Cycle in scene reference ownership");
        visited[index] = 1u;
        const auto& ref = source->references_[index];
        const bool present = ref.parentInstance ? keep(*ref.parentInstance)
            : ref.parentNodeIndex < result->model_.sceneNodeAdmission.size() &&
              result->model_.sceneNodeAdmission[ref.parentNodeIndex];
        admitted[index] = present ? 1u : 0u;
        visited[index] = 2u;
        return present;
    };
    std::vector<std::optional<std::size_t>> remap(count);
    for (std::size_t i = 0; i < count; ++i) if (keep(i)) {
        remap[i] = result->references_.size();
        result->references_.push_back(source->references_[i]);
    }
    for (auto& ref : result->references_) if (ref.parentInstance)
        ref.parentInstance = remap.at(*ref.parentInstance);
    result->rebuildAnimations();
    result->rebuildMaterials();
    return result;
}

std::unique_ptr<ModelDocument> ModelDocument::emptySceneDocument(
    const std::string& identity,const std::string& title,model::GameVersion game) {
    auto result=std::make_unique<ModelDocument>();result->open_=true;
    result->model_.game=game;result->model_.name="area scene";
    result->identity_=identity;result->title_=title;result->sourceDescription_="Module scene (no inspection-model geometry)";
    result->trace_=std::make_shared<ResourceTrace>();return result;
}

std::shared_ptr<ModelDocument> ModelDocument::visualView(
    std::shared_ptr<const ModelDocument> source,const std::string& suffix,
    const std::string& texture,std::shared_ptr<const ModelDocument> head,const std::string& headTexture) {
    std::vector<VisualModelAttachment> attachments;
    if(head) {
        VisualModelAttachment item;item.document=std::move(head);item.hook="headhook";
        item.texture=headTexture;item.kind=VisualAttachmentKind::CreatureHead;
        attachments.push_back(std::move(item));
    }
    return visualView(std::move(source),suffix,texture,attachments);
}

std::shared_ptr<ModelDocument> ModelDocument::visualView(
    std::shared_ptr<const ModelDocument> source,const std::string& suffix,
    const std::string& texture,const std::vector<VisualModelAttachment>& attachments,
    const std::string& environmentMap) {
    resourceLoadJob().update(0u, 0u, "Assembling creature appearance: " + suffix);
    if(!source||!source->isOpen())throw model::ModelError("Visual view requires a resolved source document");
    auto asset=source->model();
    if(asset.sceneNodeAdmission.empty())asset.sceneNodeAdmission.assign(asset.nodes.size(),1u);



    if(!texture.empty())for(auto& mesh:asset.meshes)
        if(asset.game!=model::GameVersion::Kotor2 || lower(mesh.texture0)!="p_baodur_arm")
            mesh.texture0=resourceKey(texture);
    auto result=sceneView(source,std::move(asset),suffix);
    result->appearanceEnvironmentMap_=environmentMap;
    std::vector<std::size_t> roots;
    for(const auto& attachment:attachments) {
        resourceLoadJob().check();
        const auto& document=*attachment.document;
        const auto parent=attachment.parentAttachment
            ?std::optional<std::size_t>{roots.at(*attachment.parentAttachment)}:std::nullopt;
        const auto& nodes=parent?result->references_[*parent].model.nodes:result->model_.nodes;
        const auto hook=std::find_if(nodes.begin(),nodes.end(),[&](const auto& n){return lower(n.name)==lower(attachment.hook);});
        ReferenceInstance root;root.parentInstance=parent;root.parentNodeName=attachment.hook;


        root.visualAttachmentDetached=hook==nodes.end();
        if(hook!=nodes.end())root.parentNodeIndex=std::size_t(hook-nodes.begin());
        root.resref=document.model().name;root.model=document.model();root.supermodels=document.supermodels_;
        root.visualAttachment=true;root.reattachable=false;
        root.attachmentKind=attachment.kind;root.equipmentSlot=attachment.equipmentSlot;
        root.equipmentItemType=attachment.equipmentItemType;
        root.visualScale=attachment.scale;root.appearanceEnvironmentMap=attachment.environmentMap;
        for(auto& mesh:root.model.meshes) {
            root.authoredPrimaryTextures.push_back(mesh.texture0);
            if(!attachment.texture.empty()&&(root.model.game!=model::GameVersion::Kotor2 ||
                lower(mesh.texture0)!="p_baodur_arm"))
                mesh.texture0=resourceKey(attachment.texture);
        }
        root.pose=model::constructedBindPose(root.model, resourceLoadJob());
        const auto base=result->references_.size();roots.push_back(base);result->references_.push_back(std::move(root));
        for(auto ref:document.references_) {
            resourceLoadJob().check();
            ref.parentInstance=ref.parentInstance?*ref.parentInstance+base+1:base;
            ref.visualScale*=attachment.scale;
            result->references_.push_back(std::move(ref));
        }
    }
    result->loadTexturesFromResolver(result->resolver_);
    result->rebuildMaterials();
    result->loadTexturesFromResolver(result->resolver_,result->auxiliaryTextures());
    std::vector<std::string> environmentTextures;
    if(!environmentMap.empty())environmentTextures.push_back(environmentMap);
    for(const auto& attachment:attachments)if(!attachment.environmentMap.empty())
        environmentTextures.push_back(attachment.environmentMap);
    if(!environmentTextures.empty())result->loadTexturesFromResolver(result->resolver_,environmentTextures);
    result->rebuildMaterials();return result;
}

void ModelDocument::applyVisualTextures(const std::string& bodyTexture,const std::string& headTexture) {


    if(resourceParent_)for(std::size_t i=0;i<model_.meshes.size();++i) {
        auto& mesh=model_.meshes[i];
        if(model_.game==model::GameVersion::Kotor2 && lower(mesh.texture0)=="p_baodur_arm")continue;
        mesh.texture0=bodyTexture.empty()?resourceParent_->model().meshes[i].texture0:resourceKey(bodyTexture);
    }
    for(auto& reference:references_)if(reference.attachmentKind==VisualAttachmentKind::CreatureHead)
        for(std::size_t i=0;i<reference.model.meshes.size();++i) {
            auto& mesh=reference.model.meshes[i];
            if(reference.model.game==model::GameVersion::Kotor2 && lower(mesh.texture0)=="p_baodur_arm")continue;
            mesh.texture0=headTexture.empty()?reference.authoredPrimaryTextures[i]:resourceKey(headTexture);
        }
    loadTexturesFromResolver(resolver_);rebuildMaterials();
    loadTexturesFromResolver(resolver_,auxiliaryTextures());rebuildMaterials();
}

void ModelDocument::appendDiagnostic(std::string message) {
    if (!message.empty()) messages_.push_back(std::move(message));
}

const neoshared::texture::TextureData* ModelDocument::texture(const std::string& resref) const {
    const auto found = textures_.find(lower(resourceStem(resref)));
    return found == textures_.end()
        ? (resourceParent_ ? resourceParent_->texture(resref) : nullptr) : &found->second;
}

neoshared::material::TextureMetadataPtr ModelDocument::textureMetadata(
    const std::string& resref) const {
    const auto found = textureMetadata_.find(neoshared::material::textureResourceKey(resref));
    return found == textureMetadata_.end()
        ? (resourceParent_ ? resourceParent_->textureMetadata(resref) : nullptr) : found->second;
}

const neoshared::material::Material& ModelDocument::material(std::size_t meshIndex) const {
    if (meshIndex >= materials_.size())
        throw neomdl::model::ModelError("Material mesh index is outside the loaded model");
    return materials_[meshIndex];
}

const neoshared::material::Material& ModelDocument::referenceMaterial(
    std::size_t instanceIndex, std::size_t meshIndex) const {
    if (instanceIndex >= referenceMaterials_.size() ||
        meshIndex >= referenceMaterials_[instanceIndex].size())
        throw neomdl::model::ModelError("Material index is outside the referenced model");
    return referenceMaterials_[instanceIndex][meshIndex];
}

void ModelDocument::rebuildMaterials() {
    resourceLoadJob().update(0u, 0u, "Building model materials");
    materials_.clear();
    referenceMaterials_.clear();

    // Parse once per decoded resource; mesh bindings share const metadata. Use
    // sorted keys so diagnostics are stable across standard-library platforms.
    std::vector<std::string> keys;
    keys.reserve(textures_.size());
    for (const auto& item : textures_) keys.push_back(item.first);
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        resourceLoadJob().check();
        if(textureMetadata_.count(key)) continue;
        auto metadata = std::make_shared<const neoshared::material::TextureMetadata>(
            neoshared::material::makeTextureMetadata(textures_.at(key)));
        for (const auto& issue : metadata->txi.issues) {
            if (issue.severity == neoshared::texture::TxiIssueSeverity::Info) continue;
            messages_.push_back("Material " + key + ": TXI line " +
                std::to_string(issue.lineNumber) + " (" + issue.key + "): " + issue.message);
        }
        textureMetadata_.emplace(key, std::move(metadata));
    }
    const neoshared::material::TextureMetadataLookup lookup = [this](const std::string& key) {
        return textureMetadata(key);
    };
    const auto build = [&lookup](const neomdl::model::Model& model) {
        std::vector<neoshared::material::Material> result;
        result.reserve(model.meshes.size());
        for (const auto& mesh : model.meshes) {
            resourceLoadJob().check();
            result.push_back(neoshared::material::makeMaterial(mesh.texture0, mesh.texture1, lookup));
        }
        return result;
    };
    materials_ = build(model_);
    referenceMaterials_.reserve(references_.size());
    for (const auto& reference : references_) referenceMaterials_.push_back(build(reference.model));
}

std::vector<std::filesystem::path> ModelDocument::openPaths() const {
    auto result=trace_?trace_->inputs():std::vector<std::filesystem::path>{};
    if(!path_.empty()&&std::find(result.begin(),result.end(),path_)==result.end()) result.push_back(path_);
    return result;
}

const neomdl::model::Model& ModelDocument::modelAt(std::size_t hierarchyIndex) const {
    if (hierarchyIndex == 0u) return model_;
    if (hierarchyIndex - 1u < supermodels_.size()) return supermodels_[hierarchyIndex - 1u];
    throw neomdl::model::ModelError("Model hierarchy index is outside the loaded supermodel chain");
}

std::size_t ModelDocument::referenceHierarchySize(std::size_t instanceIndex) const {
    if (instanceIndex >= references_.size())
        throw neomdl::model::ModelError("Reference instance index is outside the loaded hierarchy");
    return 1u + references_[instanceIndex].supermodels.size();
}

const neomdl::model::Model& ModelDocument::referenceModelAt(
    std::size_t instanceIndex, std::size_t hierarchyIndex) const {
    if (instanceIndex >= references_.size())
        throw neomdl::model::ModelError("Reference instance index is outside the loaded hierarchy");
    const auto& reference = references_[instanceIndex];
    if (hierarchyIndex == 0u) return reference.model;
    if (hierarchyIndex - 1u < reference.supermodels.size())
        return reference.supermodels[hierarchyIndex - 1u];
    throw neomdl::model::ModelError(
        "Reference model hierarchy index is outside the loaded supermodel chain");
}

std::optional<neomdl::model::AnimationReference> ModelDocument::findReferenceAnimation(
    std::size_t instanceIndex, const std::string& name) const {
    if (instanceIndex >= references_.size()) return std::nullopt;
    const auto& reference = references_[instanceIndex];
    std::vector<const neomdl::model::Model*> hierarchy;
    hierarchy.reserve(1u + reference.supermodels.size());
    hierarchy.push_back(&reference.model);
    for (const auto& model : reference.supermodels) hierarchy.push_back(&model);
    return neomdl::model::findAnimation(hierarchy, name);
}

neomdl::model::Pose ModelDocument::evaluateReferenceAnimation(
    std::size_t instanceIndex,
    const neomdl::model::AnimationReference& animation,
    float time, bool loop, const neoshared::bif::JobControl& job) const {
    const auto& reference = references_.at(instanceIndex);
    const auto& owner = referenceModelAt(instanceIndex, animation.modelIndex);
    return neomdl::model::evaluateAnimation(
        reference.model, owner, animation.animationIndex,
        time, animation.positionScale, loop, job);
}

std::vector<const neomdl::model::Model*> ModelDocument::modelHierarchy() const {
    std::vector<const neomdl::model::Model*> result;
    result.reserve(modelHierarchySize());
    result.push_back(&model_);
    for (const auto& supermodel : supermodels_) result.push_back(&supermodel);
    return result;
}

const neomdl::model::AnimationReference& ModelDocument::animationReference(
    std::size_t clipIndex) const {
    if (clipIndex >= animations_.size())
        throw neomdl::model::ModelError("Animation clip index is outside the resolved animation list");
    return animations_[clipIndex];
}

const neomdl::model::Model& ModelDocument::animationModel(std::size_t clipIndex) const {
    return modelAt(animationReference(clipIndex).modelIndex);
}

const neomdl::model::Animation& ModelDocument::animation(std::size_t clipIndex) const {
    const auto& reference = animationReference(clipIndex);
    const auto& owner = modelAt(reference.modelIndex);
    if (reference.animationIndex >= owner.animations.size())
        throw neomdl::model::ModelError("Resolved animation index is outside its owner model");
    return owner.animations[reference.animationIndex];
}

std::optional<neomdl::model::AnimationReference> ModelDocument::findAnimation(
    const std::string& name) const {
    return neomdl::model::findAnimation(modelHierarchy(), name);
}

neomdl::model::Pose ModelDocument::evaluateAnimation(
    std::size_t clipIndex, float time, bool loop) const {
    const auto& reference = animationReference(clipIndex);
    return neomdl::model::evaluateAnimation(
        model_, modelAt(reference.modelIndex), reference.animationIndex,
        time, reference.positionScale, loop);
}

std::vector<neomdl::model::AnimationEventOccurrence> ModelDocument::animationEventsBetween(
    std::size_t clipIndex, float startTime, float endTime, std::uint32_t flags) const {
    return neomdl::model::animationEventsBetween(
        animation(clipIndex), startTime, endTime, flags);
}

void ModelDocument::rebuildAnimations() {
    animations_ = neomdl::model::enumerateAnimations(modelHierarchy());
}

void ModelDocument::loadSupermodelsFromResolver(
    const ResourceResolver& resolver, const std::string& initialResref) {
    if (appendLoadedSupermodels(model_, supermodels_, "Supermodel ")) return;
    const auto initialSupermodel = hierarchyResref(model_.supermodel);
    if (!resolver) {
        if (!initialSupermodel.empty())
            messages_.push_back("No host resource resolver is available for supermodel: " +
                                initialSupermodel);
        return;
    }

    std::unordered_set<std::string> visited;
    if (!initialResref.empty()) visited.emplace(lower(hierarchyResref(initialResref)));
    const neomdl::model::Model* current = &model_;
    while (true) {
        resourceLoadJob().check();
        if (appendLoadedSupermodels(*current, supermodels_, "Supermodel ")) return;
        const auto next = hierarchyResref(current->supermodel);
        if (next.empty()) return;
        const auto key = lower(next);
        if (!visited.emplace(key).second) {
            messages_.push_back("Supermodel cycle stopped at: " + next);
            return;
        }
        auto mdl = resolver(next, kMdlResourceType);
        if (!mdl) {
            messages_.push_back("Supermodel resource was not found in the game session: " + next);
            return;
        }
        std::vector<std::uint8_t> mdx;
        if (!model::isAsciiModelInput(mdl->bytes)) {
            if (auto companion = resolver(next, kMdxResourceType))
                mdx = std::move(companion->bytes);
        }
        auto decoded = neomdl::model::decodeModelBytes(
            mdl->bytes, mdx,
            mdl->sourceDescription.empty() ? next + ".mdl" : mdl->sourceDescription, resourceLoadJob(),model_.game,asciiResources_);
        supermodels_.push_back(std::move(decoded));
        appendParserWarnings(supermodels_.back(), "Supermodel " + next + ": ");
        current = &supermodels_.back();
    }
}

void ModelDocument::loadReferencesFromResolver(
    const ResourceResolver& resolver, const std::string& rootResref) {
    std::unordered_map<std::string, std::shared_ptr<ReferenceAsset>> cache;
    std::unordered_map<std::uint64_t, std::shared_ptr<ReferenceAsset>> registeredCache;
    model::retainAsciiModelInstance(model_);
    RetainedReferenceAssets retained;
    retained.remember(supermodels_);
    std::unordered_set<std::string> missing;
    bool warnedMissingResolver=false;

    const auto decode = [&](const std::string& resref)
            -> std::optional<neomdl::model::Model> {
        auto mdl = resolver(resref, kMdlResourceType);
        if (!mdl) return std::nullopt;
        std::vector<std::uint8_t> mdx;
        if (!model::isAsciiModelInput(mdl->bytes)) {
            if (auto companion = resolver(resref, kMdxResourceType))
                mdx = std::move(companion->bytes);
        }
        return neomdl::model::decodeModelBytes(
            mdl->bytes, mdx,
            mdl->sourceDescription.empty() ? resref + ".mdl" : mdl->sourceDescription, resourceLoadJob(),model_.game,asciiResources_);
    };

    const auto load = [&](const std::string& resref)
            -> std::shared_ptr<const ReferenceAsset> {
        resourceLoadJob().update(0u, 0u, "Loading reference model: " + resref);
        if (model_.asciiLoadSession) {
            try {
                auto selected=model::resolveAsciiModelInstance(
                    model_,resref,resourceLoadJob(),asciiResources_);
                if (!selected) return {};
                const auto found=registeredCache.find(selected->asciiRegistrationId);
                if (found!=registeredCache.end()) return found->second;
                auto asset=std::make_shared<ReferenceAsset>();
                asset->model=std::move(*selected);
                appendParserWarnings(asset->model,"Reference "+resref+": ");
                appendLoadedSupermodels(asset->model,asset->supermodels,
                    "Reference "+resref+" supermodel ");
                registeredCache.emplace(asset->model.asciiRegistrationId,asset);
                return asset;
            } catch (const neoshared::bif::JobCancelled&) {
                throw;
            } catch (const std::exception& error) {
                messages_.push_back("Unable to load reference model " + resref + ": " + error.what());
                return {};
            }
        }
        const auto key = lower(hierarchyResref(resref));
        const auto cached = cache.find(key);
        if (cached != cache.end()) return cached->second;
        if (auto registered=retained.find(key)) {
            cache.emplace(key,registered);
            return registered;
        }
        if (missing.find(key) != missing.end()) return {};
        if (!resolver) {
            if (!warnedMissingResolver)
                messages_.push_back("No host resource resolver is available for referenced models.");
            warnedMissingResolver=true;
            missing.emplace(key);
            return {};
        }
        const auto decoded = decode(resref);
        if (!decoded) {
            missing.emplace(key);
            return {};
        }
        auto asset = std::make_shared<ReferenceAsset>();
        asset->model = *decoded;
        appendParserWarnings(asset->model, "Reference " + resref + ": ");

        std::unordered_set<std::string> visited{key};
        const neomdl::model::Model* current = &asset->model;
        while (true) {
            resourceLoadJob().check();
            if (appendLoadedSupermodels(*current, asset->supermodels,
                    "Reference " + resref + " supermodel ")) break;
            const auto next = hierarchyResref(current->supermodel);
            if (next.empty()) break;
            const auto nextKey = lower(next);
            if (!visited.emplace(nextKey).second) {
                messages_.push_back("Reference " + resref +
                    " supermodel cycle stopped at: " + next);
                break;
            }
            const auto inherited = decode(next);
            if (!inherited) {
                messages_.push_back("Reference " + resref +
                    " supermodel resource was not found in the game session: " + next);
                break;
            }
            asset->supermodels.push_back(*inherited);
            appendParserWarnings(asset->supermodels.back(),
                "Reference " + resref + " supermodel " + next + ": ");
            current = &asset->supermodels.back();
        }
        retained.remember(asset->supermodels,asset);
        cache.emplace(key, asset);
        return asset;
    };

    std::function<void(const neomdl::model::Model&, std::optional<std::size_t>,
                       std::unordered_set<std::string>)> visit;
    visit = [&](const neomdl::model::Model& owner,
                std::optional<std::size_t> parentInstance,
                std::unordered_set<std::string> chain) {
        visitModelPartOccurrences(owner, [&](std::size_t partIndex,
                                             const neomdl::model::Node& node) {
            if (!node.reference) return;
            const auto resref = hierarchyResref(node.reference->model);
            if (resref.empty()) {
                messages_.push_back("Reference node " + node.name + " has no usable model ResRef.");
                return;
            }
            const auto key = lower(resref);
            if (chain.find(key) != chain.end()) {
                messages_.push_back("Reference cycle stopped at: " + resref);
                return;
            }
            const auto asset = load(resref);
            if (!asset) {
                messages_.push_back(
                    "Reference model resource was not found in the game session: " + resref);
                return;
            }
            const auto instanceIndex = references_.size();
            references_.push_back(ReferenceInstance{
                parentInstance, partIndex, node.name, resref,
                node.reference->reattachable != 0, asset->model, asset->supermodels,
                neomdl::model::constructedBindPose(asset->model, resourceLoadJob()),false,VisualAttachmentKind::None,0u,1.0f,false,{},{}});
            auto nextChain = chain;
            nextChain.emplace(key);
            visit(asset->model, instanceIndex, std::move(nextChain));
        });
    };

    std::unordered_set<std::string> initial;
    const auto rootKey = lower(hierarchyResref(rootResref));
    if (!rootKey.empty()) initial.emplace(rootKey);
    visit(model_, std::nullopt, std::move(initial));
}

const ModelDocument* ModelDocument::chunkModel(const std::string& resref) const {
    const auto found=chunkModels_.find(lower(hierarchyResref(resref)));
    return found==chunkModels_.end()
        ? (resourceParent_ ? resourceParent_->chunkModel(resref) : nullptr) : found->second.get();
}
void ModelDocument::loadChunkModels(const ResourceResolver& resolver) {
    if(!allowChunkModels_) return;
    std::map<std::string,std::string> names;
    const auto collect=[&](const neomdl::model::Model& model) {
        for(const auto& node:model.nodes) if(node.emitter) {
            const auto name=hierarchyResref(node.emitter->chunkName);
            if(!name.empty()) names.emplace(lower(name),name);
        }
    };
    collect(model_);for(const auto& reference:references_) collect(reference.model);
    std::vector<std::pair<std::string,std::string>> pending(names.begin(),names.end());
    for(std::size_t cursor=0;cursor<pending.size();++cursor) {
        const auto name=pending[cursor];
        resourceLoadJob().update(cursor, pending.size(), "Loading particle model: " + name.second);
        try {
            auto resource=resolver ? resolver(name.second,kMdlResourceType) : std::nullopt;
            if(!resource) {messages_.push_back("Chunk model not found: "+name.second);continue;}
            if(resource->fileName.empty()) resource->fileName=name.second+".mdl";
            resource->type=kMdlResourceType;
            auto chunk=std::make_shared<ModelDocument>();chunk->allowChunkModels_=false;
            chunk->openResource(std::move(*resource),resolver,resourceLoadJob(),model_.game,asciiResources_);
            const auto queue=[&](const neomdl::model::Model& model) {
                for(const auto& node:model.nodes) if(node.emitter) {
                    const auto child=hierarchyResref(node.emitter->chunkName);
                    if(!child.empty() && names.emplace(lower(child),child).second)
                        pending.emplace_back(lower(child),child);
                }
            };
            queue(chunk->model());for(const auto& reference:chunk->references()) queue(reference.model);
            for(const auto& message:chunk->messages()) messages_.push_back("Chunk "+name.second+": "+message);
            chunkModels_.emplace(name.first,std::move(chunk));
        } catch (const neoshared::bif::JobCancelled&) {
            throw;
        } catch (const std::exception& error) {
            messages_.push_back("Unable to load chunk model "+name.second+": "+error.what());
        }
    }
}

void ModelDocument::appendParserWarnings(const neomdl::model::Model& model,
                                         const std::string& prefix) {
    for (const auto& warning : model.warnings) messages_.push_back(prefix + warning);
}

bool ModelDocument::appendLoadedSupermodels(const neomdl::model::Model& model,
    std::vector<neomdl::model::Model>& destination, const std::string& prefix) {
    // Hold the chain before appending: model can itself be an element of
    // destination, whose storage may move. Its pointer identity was already
    // checked by the loader; equal model names do not imply a cycle.
    const auto retained = model.asciiSupermodels;
    if (!retained) return false;
    for (const auto& inherited : *retained) {
        resourceLoadJob().check();
        destination.push_back(inherited);
        appendParserWarnings(destination.back(), prefix + inherited.name + ": ");
    }
    return true;
}

std::vector<std::string> ModelDocument::auxiliaryTextures() {
    std::unordered_set<std::string> keys;
    const auto collect=[&](const neoshared::material::Material& m) {
        for(const auto* binding:{&m.bumpMap,&m.environmentMap,&m.bumpyShiny})
            if(!binding->key.empty() && !textures_.count(binding->key))
                keys.insert(binding->key);
    };
    for(const auto& m:materials_) collect(m);
    for(const auto& instance:referenceMaterials_) for(const auto& m:instance) collect(m);
    const neoshared::material::TextureMetadataLookup lookup=[this](const std::string& key) {
        return textureMetadata(key);
    };
    const auto collectAllocations=[&](const model::Model& model) {
        for(const auto& allocation:model.asciiMdxAllocations)
            if(allocation.materialTangents)
                collect(neoshared::material::makeMaterial(allocation.primaryTexture,{},lookup));
    };
    collectAllocations(model_);
    for(const auto& reference:references_) collectAllocations(reference.model);
    std::vector<std::string> result(keys.begin(),keys.end());
    std::sort(result.begin(),result.end());return result;
}

std::vector<std::string> ModelDocument::referencedTextures() const {
    std::map<std::string, std::string> unique;
    const auto remember = [&unique](const std::string& name) {
        const auto stem = resourceStem(name);
        if (!stem.empty()) unique.emplace(lower(stem), stem);
    };
    const auto rememberModel = [&remember](const neomdl::model::Model& model) {
        for(const auto& allocation:model.asciiMdxAllocations)
            if(allocation.materialTangents) remember(allocation.primaryTexture);
        for (const auto& mesh : model.meshes) {
            remember(mesh.texture0);
            remember(mesh.texture1);
        }
        for (const auto& node : model.nodes) {
            if (node.light) {
                for (const auto& texture : node.light->textureNames) remember(texture);
            }
            if (node.emitter) {
                remember(node.emitter->texture);
                remember(node.emitter->depthTexture);
            }
        }
    };
    rememberModel(model_);
    for (const auto& reference : references_) rememberModel(reference.model);
    std::vector<std::string> result;
    result.reserve(unique.size());
    for (const auto& entry : unique) result.push_back(entry.second);
    return result;
}

void ModelDocument::loadTexturesFromResolver(const ResourceResolver& resolver,
                                              const std::vector<std::string>& names) {
    if (!resolver) {
        for (const auto& name : (names.empty() ? referencedTextures() : names))
            messages_.push_back("No host resource resolver is available for texture: " + name);
        return;
    }
    struct Candidate { std::uint16_t type; const char* extension; };
    static constexpr Candidate candidates[] = {
        {kTpcResourceType, ".tpc"}, {kTxbResourceType, ".txb"},
        {kTgaResourceType, ".tga"}, {kDdsResourceType, ".dds"},
    };
    const auto requestedNames = names.empty() ? referencedTextures() : names;
    std::size_t textureIndex = 0u;
    for (const auto& name : requestedNames) {
        resourceLoadJob().update(textureIndex++, requestedNames.size(), "Loading model texture: " + name);
        if(textures_.count(lower(resourceStem(name)))) continue;
        bool resolved = false;
        bool loaded = false;
        bool failed = false;
        for (const auto& candidate : candidates) {
            try {
                auto resource = resolver(name, candidate.type);
                if (!resource) continue;
                resolved = true;
                std::string txi;
                bool txiAvailable=candidate.type==kTpcResourceType || candidate.type==kTxbResourceType;
                if (candidate.type == kTgaResourceType || candidate.type == kDdsResourceType) {
                    if (auto sidecar = resolver(name, kTxiResourceType)) {
                        txi = bytesToText(sidecar->bytes);
                        txiAvailable=true; // an empty existing sidecar is valid
                    }
                }
                resourceLoadJob().update(textureIndex - 1u, requestedNames.size(), "Decoding model texture: " + name);
                auto decoded = neoshared::texture::decodeTextureBytes(
                    resource->bytes, std::filesystem::u8path(name + candidate.extension), std::move(txi));
                if (!decoded.hasPixels()) {
                    messages_.push_back("Resolved texture has no displayable pixels: " + name);
                    break;
                }
                textureTxiAvailable_[lower(resourceStem(name))]=txiAvailable || !decoded.txi.empty();
                const auto key=lower(resourceStem(name));
                textures_.emplace(key, std::move(decoded));
                textureLoadOrder_.push_back(key);
                loaded = true;
                break;
            } catch (const neoshared::bif::JobCancelled&) {
                throw;
            } catch (const std::exception& error) {
                messages_.push_back("Unable to decode texture " + name + ": " + error.what());
                failed = true;
                break;
            }
        }
        if (!loaded && !resolved && !failed) {
            try {
                if(auto resource=resolver(name,kTxiResourceType)) {
                    resolved=true;
                    auto decoded=material::constructProcedureSource(bytesToText(resource->bytes),name,
                        model_.game==model::GameVersion::Kotor1 ? material::EngineMaterialGame::Kotor1 :
                            material::EngineMaterialGame::Kotor2);
                    const auto key=lower(resourceStem(name));
                    textures_.emplace(key,std::move(decoded));
                    textureLoadOrder_.push_back(key);
                    textureTxiAvailable_[key]=true;
                    loaded=true;
                }
            } catch (const neoshared::bif::JobCancelled&) {
                throw;
            } catch (const std::exception& error) {
                messages_.push_back("Unable to construct procedural texture "+name+": "+error.what());
                failed = true;
            }
        }
        if (!loaded && !resolved && !failed && textures_.find(lower(resourceStem(name))) == textures_.end())
            messages_.push_back("Texture resource was not found in the game session: " + name);
    }
}

bool ModelDocument::runtimeTextureTxiAvailable(const std::string& resref) const {
    const auto found=textureTxiAvailable_.find(neoshared::material::textureResourceKey(resref));
    return found!=textureTxiAvailable_.end() ? found->second :
        (resourceParent_ && resourceParent_->runtimeTextureTxiAvailable(resref));
}

bool ModelDocument::loadRuntimeTexture(const std::string& resref,
    const ResourceResolver& supplied,std::string& error) {
    error.clear();
    const auto key=neoshared::material::textureResourceKey(resref);
    if(!open_ || key.empty()) {error="Open a model and supply a valid runtime texture ResRef";return false;}
    try {
        auto resolver=resolver_;
        if(!runtimeScopePinned_||!resolver) resolver=overlayResolver(resolver,traceResources(supplied,trace_,identity_));
        loadTexturesFromResolver(resolver,{key});
        const auto* data=texture(key);
        if(!data || !data->hasPixels()) {error="Runtime texture not resolved: "+key;return false;}
        if(textureMetadata_.find(key)==textureMetadata_.end())
            textureMetadata_.emplace(key,std::make_shared<neoshared::material::TextureMetadata>(neoshared::material::makeTextureMetadata(*data)));
        // A previously loaded TGA may acquire its explicit TXI before an owner
        // effect is enabled. Never infer availability from a non-null wrapper.
        if(!runtimeTextureTxiAvailable(key) && resolver) {
            if(auto txi=resolver(key,kTxiResourceType)) {
                if(textures_.find(key)==textures_.end()) textures_.emplace(key,*data);
                auto& stored=textures_.at(key);
                stored.txi=bytesToText(txi->bytes);
                stored.txiStorage=neoshared::texture::TxiStorage::Sidecar;
                textureMetadata_[key]=std::make_shared<neoshared::material::TextureMetadata>(neoshared::material::makeTextureMetadata(stored));
                textureTxiAvailable_[key]=true;
            }
        }
        return true;
    } catch (const neoshared::bif::JobCancelled&) {
        throw;
    } catch (const std::exception& exception) {
        error="Runtime texture "+key+": "+exception.what();return false;
    }
}

std::vector<std::optional<neomdl::model::Mat4>> ModelDocument::referenceBases(
    const neomdl::model::Pose& mainPose) const {
    std::vector<std::optional<neomdl::model::Mat4>> bases(references_.size());
    for(std::size_t index=0;index<references_.size();++index) {
        const auto& reference=references_[index];
        if(reference.parentInstance) {
            const auto parent=*reference.parentInstance;
            if(parent>=index || !bases[parent] || reference.parentNodeIndex>=references_[parent].pose.worldTransforms.size()) continue;
            bases[index]=neomdl::model::multiply(*bases[parent],references_[parent].pose.worldTransforms[reference.parentNodeIndex]);
        } else if(reference.parentNodeIndex<mainPose.worldTransforms.size())
            bases[index]=mainPose.worldTransforms[reference.parentNodeIndex];
    }
    return bases;
}

std::vector<std::optional<neomdl::model::EngineFrame>> ModelDocument::referenceFrames(
    const neomdl::model::Pose& mainPose) const {
    namespace m=neomdl::model;
    std::vector<std::optional<m::EngineFrame>> frames(references_.size());
    for(std::size_t index=0;index<references_.size();++index) {
        const auto& reference=references_[index];
        if(reference.visualAttachmentDetached)continue;
        m::EngineFrame owner;
        const m::Model* target=&model_;
        const m::Pose* pose=&mainPose;
        if(reference.parentInstance) {
            const auto parent=*reference.parentInstance;
            if(parent>=index || !frames[parent] ||
               reference.parentNodeIndex>=references_[parent].pose.worldTransforms.size())continue;
            owner=*frames[parent];
            target=&references_[parent].model;
            pose=&references_[parent].pose;
        } else if(reference.parentNodeIndex>=mainPose.worldTransforms.size())continue;
        auto frame=m::enginePartWorld(*target,*pose,owner,reference.parentNodeIndex);
        // AttachToObject inherits its target's effective scale. Appearance
        // attachments instead retain the assembly's own cumulative scale.
        frame.scale=reference.visualAttachment?reference.visualScale:owner.scale;
        frames[index]=frame;
    }
    return frames;
}
} // namespace neomdl
