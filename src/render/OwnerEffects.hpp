#pragma once

#include "core/OwnerEffects.hpp"
#include "render/ShaderMeshRenderer.hpp"

namespace neomdl::render {
struct OwnerEffectTextures {
    gl::UInt hologramFilter{}; // Scene::Scene's actual "Holotex", never generated
    gl::UInt shell{};
    material::TextureMetadataPtr shellMetadata;
    bool shellTxiAvailable{};
};

bool validOwnerVisualEffects(const OwnerVisualEffects&, std::string& error);
// RenderHologramEffect's numeric dispatch; PartSkin has its own consumer.
bool hologramTriMeshPathDraws(material::TriMeshRenderPath) noexcept;
// Gob::EnableRenderBumpedOut parses the value with strcmp, not case folding.
material::RuntimeMaterial shellMaterial(const material::TxiMetadata&);
// Desktop DynamicPartCalculateMaterial's independent door factor. The caller
// selects classification bit 0x08 and supplies its retained Gob alpha. Apply
// once after configureOwnerVisualEffects, before the actual material upload.
// Desktop white-color bumped-out and skinned blur programs do not consume it.
void applyEngineDoorAlpha(MeshDrawParameters&, float alpha) noexcept;
// Transactional: on failure the submission is unchanged. Called after ordinary
// material selection, before queue insertion. The optional shell follows this
// part's primary draw, including a deliberately omitted hologram primary.
bool configureOwnerVisualEffects(const OwnerVisualEffects&, const OwnerEffectTextures&,
    const material::Material& authoredMaterial, const model::NodeMaterialState&,
    MeshSubmission&, std::string& error);
} // namespace neomdl::render
