#pragma once

#include "core/ResourceContext.hpp"
#include "core/CollisionAssets.hpp"
#include <model/Model.hpp>
#include <model/Lighting.hpp>
#include <neoshared/material/Material.hpp>
#include <neoshared/texture/Image.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neomdl {

struct ModelFileSource {
    std::filesystem::path path;
    neoshared::ResourceDocument resource;
};
ModelFileSource readModelFileSource(const std::filesystem::path& path,
    const neoshared::bif::JobControl& job = resourceLoadJob());

class ModelDocument;
enum class VisualAttachmentKind { None, CreatureHead, EquippedItem };
struct VisualModelAttachment {
    std::shared_ptr<const ModelDocument> document;
    std::string hook;
    std::string texture;
    std::string environmentMap;
    VisualAttachmentKind kind{VisualAttachmentKind::None};
    std::uint32_t equipmentSlot{};
    std::uint8_t equipmentItemType{};
    std::optional<std::size_t> parentAttachment;
    float scale{1.0f};
};

inline constexpr std::uint16_t kMdlResourceType = 0x07D2u;
inline constexpr std::uint16_t kMdxResourceType = 0x0BC0u;
inline constexpr std::uint16_t kTpcResourceType = 0x0BBFu;
inline constexpr std::uint16_t kTxbResourceType = 0x0BBEu;
inline constexpr std::uint16_t kTgaResourceType = 0x0003u;
inline constexpr std::uint16_t kDdsResourceType = 0x07F1u;
inline constexpr std::uint16_t kTxiResourceType = 0x07E6u;

// Standalone authoring resource context. The installed game's Aurora bridge
// has no .lmt type mapping; loose loads retain their captured directory.
model::AsciiResourceResolver looseAsciiResources(const std::filesystem::path& directory);

struct ReferenceInstance {
    std::optional<std::size_t> parentInstance;
    std::size_t parentNodeIndex{};
    std::string parentNodeName;
    std::string resref;
    bool reattachable{};
    neomdl::model::Model model;
    std::vector<neomdl::model::Model> supermodels;
    neomdl::model::Pose pose;
    bool visualAttachment{}; // loaded head: bind attachment, not a randomized PartReference clock
    VisualAttachmentKind attachmentKind{VisualAttachmentKind::None};
    std::uint32_t equipmentSlot{};
    float visualScale{1.0f};
    bool visualAttachmentDetached{};
    std::string appearanceEnvironmentMap;
    std::vector<std::string> authoredPrimaryTextures;
    std::uint8_t equipmentItemType{};
};

class ModelDocument final {
public:
    // Loose files resolve same-directory companions first, then the optional
    // installation/host resolver. This preserves mod-folder overrides while
    // allowing standalone NeoMDL to obtain base-game MDX, model and texture
    // resources from a discovered installation.
    // Loading callbacks are scoped to this operation and are never retained by
    // the document or its resource resolver. Cancellation propagates to callers.
    bool openFile(const std::filesystem::path& path,
                  const ResourceResolver& fallbackResolver = {},
                  const neoshared::bif::JobControl& job = resourceLoadJob(),
                  std::optional<model::GameVersion> asciiGame = std::nullopt,
                  bool includeAnimations = true,
                  const std::optional<std::filesystem::path>& mdxPath = {});
    bool openFile(ModelFileSource source,
                  const ResourceResolver& fallbackResolver = {},
                  const neoshared::bif::JobControl& job = resourceLoadJob(),
                  std::optional<model::GameVersion> asciiGame = std::nullopt,
                  bool includeAnimations = true,
                  const std::optional<std::filesystem::path>& mdxPath = {});
    bool openResource(neoshared::ResourceDocument resource, const ResourceResolver& resolver,
                      const neoshared::bif::JobControl& job = resourceLoadJob(),
                      std::optional<model::GameVersion> asciiGame = std::nullopt,
                      const model::AsciiResourceResolver& asciiResources = {},
                      bool includeAnimations = true, bool includeCollisions = true);
    std::unique_ptr<ModelDocument> rebuildAscii(const std::string& text,
        const neoshared::bif::JobControl& job = resourceLoadJob(),
        std::optional<model::GameVersion> game = std::nullopt,
        bool includeAnimations = true) const;
    const std::optional<std::string>& originalAscii() const noexcept { return originalAscii_; }
    const CollisionSet& collisions() const noexcept { return collisions_; }

    // Immutable scene partition over an already resolved asset. The supplied
    // model keeps original node/mesh indices and sceneNodeAdmission records
    // the surviving Parts. References are filtered/remapped by ownership;
    // images, metadata and chunk assets remain shared with the source document.
    static std::shared_ptr<ModelDocument> sceneView(
        std::shared_ptr<const ModelDocument> source, model::Model instanceModel,
        const std::string& identitySuffix);

    // Base-appearance visual assembly. Body textures and an optional head
    // attachment remain per-view; decoded source assets are never mutated.
    static std::shared_ptr<ModelDocument> visualView(
        std::shared_ptr<const ModelDocument> source, const std::string& suffix,
        const std::string& texture = {}, std::shared_ptr<const ModelDocument> head = {},
        const std::string& headTexture = {});
    static std::shared_ptr<ModelDocument> visualView(
        std::shared_ptr<const ModelDocument> source, const std::string& suffix,
        const std::string& texture, const std::vector<VisualModelAttachment>& attachments,
        const std::string& environmentMap = {});
    // Creature alignment changes replace material bindings on the existing
    // visual instance without restarting its animation or attachment clocks.
    void applyVisualTextures(const std::string& bodyTexture, const std::string& headTexture);
    const std::string& visualEnvironmentMap() const noexcept { return appearanceEnvironmentMap_; }
    static std::unique_ptr<ModelDocument> emptySceneDocument(const std::string& identity,
                                                           const std::string& title,
                                                           model::GameVersion game = model::GameVersion::Kotor2);

    ResourceResolver resourceResolver() const { return resolver_; }
    std::vector<ResourceDependency> dependencies() const {
        return trace_ ? trace_->entries() : std::vector<ResourceDependency>{};
    }
    bool isOpen() const noexcept { return open_; }
    // Module-only scene tabs have no inspection Gob, bounds, or query owner.
    bool hasModelOwner() const noexcept { return open_ && !model_.nodes.empty(); }
    const std::string& identity() const noexcept { return identity_; }
    const std::string& title() const noexcept { return title_; }
    const std::string& sourceDescription() const noexcept { return sourceDescription_; }
    const std::filesystem::path& path() const noexcept { return path_; }
    // Live consumers index one node/mesh per engine Part occurrence. Exports
    // retain the finalized MdlNode graph, including shared node identities.
    const neomdl::model::Model& model() const noexcept { return model_; }
    const neomdl::model::Model& sourceModel() const noexcept {
        return sourceModel_ ? *sourceModel_ : model_;
    }



    void markRoomModel() noexcept {
        (sourceModel_ ? *sourceModel_ : model_).classification |= 0x100u;
        model_.classification |= 0x100u;
    }
    std::size_t supermodelCount() const noexcept { return supermodels_.size(); }
    std::size_t modelHierarchySize() const noexcept { return 1u + supermodels_.size(); }
    const neomdl::model::Model& modelAt(std::size_t hierarchyIndex) const;
    const std::vector<ReferenceInstance>& references() const noexcept { return references_; }
    std::size_t referenceHierarchySize(std::size_t instanceIndex) const;
    const neomdl::model::Model& referenceModelAt(
        std::size_t instanceIndex, std::size_t hierarchyIndex) const;
    std::optional<neomdl::model::AnimationReference> findReferenceAnimation(
        std::size_t instanceIndex, const std::string& name) const;
    neomdl::model::Pose evaluateReferenceAnimation(
        std::size_t instanceIndex,
        const neomdl::model::AnimationReference& animation,
        float time, bool loop = true,
        const neoshared::bif::JobControl& job = {}) const;

    const std::vector<neomdl::model::AnimationReference>& animations() const noexcept {
        return animations_;
    }
    const neomdl::model::AnimationReference& animationReference(
        std::size_t clipIndex) const;
    const neomdl::model::Animation& animation(std::size_t clipIndex) const;
    const neomdl::model::Model& animationModel(std::size_t clipIndex) const;
    std::optional<neomdl::model::AnimationReference> findAnimation(
        const std::string& name) const;
    neomdl::model::Pose evaluateAnimation(std::size_t clipIndex, float time,
                                             bool loop = true) const;
    std::vector<neomdl::model::AnimationEventOccurrence> animationEventsBetween(
        std::size_t clipIndex, float startTime, float endTime,
        std::uint32_t flags = 0u) const;

    std::vector<std::optional<neomdl::model::Mat4>> referenceBases(
        const neomdl::model::Pose& mainPose) const;
    // Static authored attachments retain GetWorld's position/quaternion;
    // engine consumers must not recover them from referenceBases matrices.
    std::vector<std::optional<neomdl::model::EngineFrame>> referenceFrames(
        const neomdl::model::Pose& mainPose) const;


    const std::vector<std::string>& messages() const noexcept { return messages_; }
    void appendDiagnostic(std::string message);
    const neoshared::texture::TextureData* texture(const std::string& resref) const;
    // Preview-only diffuse replacement; authored models and export resources stay intact.
    void setTextureOverride(std::shared_ptr<const neoshared::texture::TextureData> texture);
    const std::shared_ptr<const neoshared::texture::TextureData>& textureOverride() const noexcept {
        return textureOverride_;
    }
    std::size_t loadedTextureCount() const noexcept {
        return textures_.size() + (resourceParent_ ? resourceParent_->loadedTextureCount() : 0u);
    }
    // Resource resolution order, used after authored material registration for
    // remaining runtime resources; not a reassembly of game-global history.
    const std::vector<std::string>& textureLoadOrder() const noexcept { return textureLoadOrder_; }
    // Runtime effects retain the document's original scope. A supplied resolver
    // is used only for documents opened without one; retained closures must own
    // their captures (also required of openResource's resolver).
    bool loadRuntimeTexture(const std::string& resref, const ResourceResolver&,
                            std::string& error);
    bool runtimeTextureTxiAvailable(const std::string& resref) const;
    // Read-only metadata snapshots. No image payloads or GPU objects are copied.
    neoshared::material::TextureMetadataPtr textureMetadata(const std::string& resref) const;
    std::size_t materialCount() const noexcept { return materials_.size(); }
    const neoshared::material::Material& material(std::size_t meshIndex) const;
    const neoshared::material::Material& referenceMaterial(
        std::size_t instanceIndex, std::size_t meshIndex) const;
    std::vector<std::filesystem::path> openPaths() const;
    // Immutable, shared model assets. A particle owns pose/time, not another
    // resource lookup. Child chunk documents never recursively spawn chunks.
    const ModelDocument* chunkModel(const std::string& resref) const;
    std::size_t loadedChunkModelCount() const noexcept {
        return chunkModels_.size() + (resourceParent_ ? resourceParent_->loadedChunkModelCount() : 0u);
    }

private:
    std::vector<const neomdl::model::Model*> modelHierarchy() const;
    void loadSupermodelsFromResolver(const ResourceResolver& resolver,
                                     const std::string& initialResref);
    void rebuildAnimations();
    void loadReferencesFromResolver(const ResourceResolver& resolver,
                                    const std::string& rootResref);
    void loadTexturesFromResolver(const ResourceResolver& resolver,
                                  const std::vector<std::string>& names = {});
    void appendParserWarnings(const neomdl::model::Model& model,
                              const std::string& prefix = {});
    bool appendLoadedSupermodels(const neomdl::model::Model& model,
                                  std::vector<neomdl::model::Model>& destination,
                                  const std::string& prefix);
    std::vector<std::string> referencedTextures() const;
    void rebuildMaterials();
    std::vector<std::string> auxiliaryTextures();
    void instantiateParts();

    void loadChunkModels(const ResourceResolver& resolver);
    bool runtimeScopePinned_{true};
    ResourceResolver resolver_;
    model::AsciiResourceResolver asciiResources_;
    std::optional<std::string> originalAscii_;
    CollisionSet collisions_;
    std::shared_ptr<ResourceTrace> trace_;
    std::shared_ptr<const ModelDocument> resourceParent_;
    bool allowChunkModels_{true};
    std::map<std::string,std::shared_ptr<ModelDocument>> chunkModels_;
    bool open_{};
    std::string identity_;
    std::string title_;
    std::string sourceDescription_;
    std::filesystem::path path_;
    std::shared_ptr<neomdl::model::Model> sourceModel_;
    neomdl::model::Model model_;
    std::vector<neomdl::model::Model> supermodels_;
    std::vector<ReferenceInstance> references_;
    std::vector<neomdl::model::AnimationReference> animations_;
    std::unordered_map<std::string, neoshared::texture::TextureData> textures_;
    std::shared_ptr<const neoshared::texture::TextureData> textureOverride_;
    neoshared::material::TextureMetadataPtr textureOverrideMetadata_;
    std::vector<std::string> textureLoadOrder_;
    std::unordered_map<std::string, neoshared::material::TextureMetadataPtr> textureMetadata_;
    std::unordered_map<std::string, bool> textureTxiAvailable_;
    std::vector<neoshared::material::Material> materials_;
    std::vector<std::vector<neoshared::material::Material>> referenceMaterials_;
    std::vector<std::string> messages_;
    std::string appearanceEnvironmentMap_;
};

} // namespace neomdl
