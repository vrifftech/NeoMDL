#pragma once
#include "render/GlApi.hpp"
#include <neoshared/material/Material.hpp>
#include <material/ProceduralTexture.hpp>
#include <material/ProcedureStorage.hpp>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neomdl::render {
struct MeshDrawParameters;
struct GpuTexture {
    gl::UInt id{};
    bool cube{}, volume{}, engineNormal{};
    std::uint32_t width{},height{};
    std::uint64_t order{};
    std::uint32_t depth{1};
    std::optional<material::DirtyTextureInputs> dirtyInputs;
    unsigned dirtySlot{};
};
struct ProcedureRegistration {
    const neoshared::texture::TextureData* source;
    neoshared::material::TextureMetadataPtr metadata;
};
// Context-local image/controller ownership. Source resources and metadata stay
// immutable; clear with this context current before replacing those resources.
enum class ProcedureMipPath { SgiAutomatic, LegacyByteKernels };
// All GLU uploads use the bundled portable byte kernel. Unspecified retains
// the default kernel rather than selecting an external library.
enum class ProcedureGluPath { Unspecified, EmbeddedByte };
class TextureCache final {
public:
    bool initialize(const gl::Loader& loader,std::string& error);
    // MakeCurrent/registration: upload and initialize, but do NOT call Control.
    GpuTexture get(const neoshared::texture::TextureData& texture,
                   const neoshared::material::TextureMetadata* metadata,
                   std::size_t layer,std::string& error);
    // AurTextureMakeCurrent's pending stack: register first requests forward,
    // construct their controllers in reverse. Repeated resource identities do
    // not add another pending entry or initialize an existing controller again.
    bool makeCurrent(const std::vector<ProcedureRegistration>&,std::string& error);
    const material::ProcedureDefinition& definitionFor(
        const neoshared::texture::TextureData&,
        const neoshared::material::TextureMetadataPtr&,material::EngineMaterialGame);
    std::uint64_t orderFor(const neoshared::texture::TextureData& texture,
                           std::size_t layer = 0u);
    // Scene::Render -> AurTextureForceControllerUpdate, before bucket drawing.
    // Only textures used since the preceding call are updated, in engine hash
    // order, preserving registration order for equal hashes. Runtime options
    // and a shared RNG can be supplied through context().
    bool beginFrame(float elapsed,std::string& error);
    material::ProcedureContext& context() noexcept { return procedureContext_; }
    void setEngineRuntime(material::EngineMaterialGame) noexcept;
    // Retained for existing hosts that explicitly enable/disable K2 execution.
    void setEngineK2(bool enabled) noexcept;
    // Construction-time capability choice, not a silent fallback after GL
    // failure. Clear this context before changing it. Cube mipmaps require GLU
    // independently of the SGI capability, on every supported host.
    bool setProcedureMipPath(ProcedureMipPath,std::string& error);
    ProcedureMipPath procedureMipPath() const noexcept {return mipPath_;}
    bool setProcedureGluPath(ProcedureGluPath,std::string& error);
    ProcedureGluPath procedureGluPath() const noexcept {return gluPath_;}
    // Bind's TID is selected BEFORE Control changes it. Pixel generation,
    // however, uploads into that selected texture before the draw samples it.
    GpuTexture bind(gl::UInt registeredId,std::string& error);
    // AurTextureGetTID: select current TID without Control or marking used.
    GpuTexture current(gl::UInt registeredId,std::string& error);
    bool bind(MeshDrawParameters&,std::string& error);
    // Explicit area remount/reconfiguration boundary. Only Dirty controllers
    // are invalidated; unrelated texture state and shared RNG are retained.
    void setDirtyAreaInputs(const std::array<material::DirtyTextureInputs,3>& inputs,
                            const std::string& scopeIdentity);
    // Material-owner borrows include pooled chunk allocations. Releasing the
    // final borrower retires only that source/controller, preserving context.
    // Retain/release do no GL work and do not retain the owner allocation.
    void retain(const void* owner,const neoshared::texture::TextureData&);
    void release(const void* owner);
    void flushRetiredTextures(); // this cache's context must be current
    void clear();
    void setTextureDeletionCallback(std::function<void(gl::UInt)> callback) {
        textureDeleted_=std::move(callback);
    }
    std::size_t size() const noexcept;
    bool hasProcedures() const noexcept { return !procedures_.empty(); }
private:
    struct DefinitionRecord {
        neoshared::material::TextureMetadataPtr metadata;
        material::EngineMaterialGame game;
        material::ProcedureDefinition definition;
    };
    struct ProcedureRecord {
        const neoshared::texture::TextureData* source{};
        neoshared::material::TextureMetadata metadata;
        material::ProceduralTexture controller;
        // Engine-order frames for a strip source; shared decoder data untouched.
        neoshared::texture::TextureData frames;
        material::ProcedureStorageDescriptor storage;
        material::ProcedureCubeLayout cubeLayout{material::ProcedureCubeLayout::OrdinaryMapped};
        bool tiled{}, normalUpload{}, used{true}, pixelStorage{}, cube{};
        std::uint16_t engineHash{};
        std::uint64_t uploadedRevision{};
        std::string fault;
    };
    GpuTexture upload(const neoshared::texture::TextureData&,
                      const neoshared::material::TextureMetadata*,std::size_t,std::string&,bool engineProcedure=false);
    GpuTexture uploadProcessed(const neoshared::texture::TextureData&,
                      const neoshared::material::TextureMetadata*,std::size_t,std::string&,bool engineProcedure);
    bool uploadProcedure(ProcedureRecord&,std::string&);
    bool uploadPixelTids(ProcedureRecord&,std::string&);
    GpuTexture selected(ProcedureRecord&,std::string&);
    GpuTexture bind(ProcedureRecord&,std::string&);
    gl::Api api_;
    std::function<void(gl::UInt)> textureDeleted_;
    using TexImage3DFn=void (NEOMDL_GL_CALL*)(gl::Enum,gl::Int,gl::Int,gl::Size,gl::Size,gl::Size,gl::Int,gl::Enum,gl::Enum,const void*);
    using CompressedTexImage2DFn=void (NEOMDL_GL_CALL*)(gl::Enum,gl::Int,gl::Enum,gl::Size,gl::Size,gl::Int,gl::Size,const void*);
    TexImage3DFn texImage3D_{};
    CompressedTexImage2DFn compressedTexImage2D_{};
    bool ready_{}, engineRuntime_{true};
    material::EngineMaterialGame engineGame_{material::EngineMaterialGame::Kotor2};
    bool automaticMipmaps() const noexcept {
        return mipPath_==ProcedureMipPath::SgiAutomatic;
    }
    ProcedureMipPath mipPath_{ProcedureMipPath::SgiAutomatic};
    ProcedureGluPath gluPath_{ProcedureGluPath::EmbeddedByte};
    std::unordered_map<const neoshared::texture::TextureData*,
        std::unordered_map<std::size_t,GpuTexture>> entries_;
    std::unordered_map<const neoshared::texture::TextureData*,std::uint64_t> orders_;
    std::unordered_map<const neoshared::texture::TextureData*,std::unique_ptr<ProcedureRecord>> procedures_;
    std::unordered_map<const neoshared::texture::TextureData*,DefinitionRecord> definitions_;
    std::unordered_map<const neoshared::texture::TextureData*,std::string> failures_;
    std::unordered_map<gl::UInt,ProcedureRecord*> procedureIds_;
    material::ProcedureContext procedureContext_;
    std::unordered_set<const neoshared::texture::TextureData*> dirtySources_;
    std::string dirtyScope_;
    std::unordered_map<const void*,std::unordered_set<const neoshared::texture::TextureData*>> borrowers_;
    std::unordered_map<const neoshared::texture::TextureData*,std::size_t> borrowCounts_;
    std::vector<gl::UInt> retiredTextures_;
    std::uint64_t nextOrder_{1u};
};
} // namespace neomdl::render
