#pragma once

#include "core/LightScene.hpp"
#include "render/ShaderMeshRenderer.hpp"

namespace neomdl::render {
LightColorSource k2LightColorSource(const MeshDrawParameters& parameters) noexcept;
// The engine bump getter returns a raw PartLight, separate from either
// fixed-function or vertex-program illumination color scales.
void setEngineBumpLight(const RuntimeLight* light,MeshDrawParameters& parameters);
// Appends an already-resolved owner's lights. Existing preview/headlight
// slots are preserved; no fade state is advanced during graphics submission.
void appendK2Lights(const std::vector<ActiveLight>& lights,
                    const model::Mat4& view, MeshDrawParameters& parameters,
                    LightColorSource source = LightColorSource::FixedFunction);
// Chooses the engine constant family for the actual draw path. This adapts
// engine world-position constants to the renderer's existing eye-space inputs.
// The separately labeled inspection headlight is not a engine headlight port.
void appendK2OwnerLights(const LightScene& scene, LightOwnerId owner,
                        const model::Mat4& view, MeshDrawParameters& parameters);
} // namespace neomdl::render
