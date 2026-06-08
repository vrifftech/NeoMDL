#pragma once
#include <model/Model.hpp>

namespace neomdl::model {













void controlAnimMesh(const AnimMeshData& data, float secondTime, float blend,
                     AnimMeshState& state);
AnimMeshState initializeAnimMesh(const AnimMeshData& data);

// Refresh model-space inspection geometry without resetting local history.
void rebuildAnimMeshGeometry(const Model& model, Pose& pose);
// Apply one synchronized animation node to its target instance. A non-animmesh
// target is an invalid engine dispatch, not permission to morph an ordinary mesh.
void controlAnimMeshNode(const Model& model, const Node& source,
                         std::size_t targetNode, float secondTime, float blend,
                         Pose& pose);

} // namespace neomdl::model
