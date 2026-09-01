#pragma once
#include "render/GlApi.hpp"
#include "render/MaterialPass.hpp"
#include <material/DirtyMaterial.hpp>
namespace neomdl::render {
struct MeshDrawParameters;
// The caller's Mat Bind precedes this helper. Remaining virtual Binds occur in
// engine dispatcher order, including textures not sampled by the selected FP.
bool prepareDirtyBindings(gl::Api&,MeshDrawParameters&,const material::DirtyMaterialPlan&,
                          EngineMaterialState&,std::string& error);
} // namespace neomdl::render
