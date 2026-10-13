#pragma once

#include <model/Model.hpp>
#include <neoshared/bwm/BwmFile.hpp>

namespace neomdl::model {

neoshared::bwm::BwmFile collisionGeometry(const Mesh& mesh);
AabbNodeData buildAabbTree(const Mesh& mesh,const neoshared::bif::JobControl& job = {});
void rebuildAabbTrees(Model& model,const neoshared::bif::JobControl& job = {});

} // namespace neomdl::model
