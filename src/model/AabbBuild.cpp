#include <model/AabbBuild.hpp>

namespace neomdl::model {

neoshared::bwm::BwmFile collisionGeometry(const Mesh& mesh) {
    neoshared::bwm::BwmFile result;
    for(const auto& vertex:mesh.cpuPositions.value()) result.vertices.push_back({vertex.x,vertex.y,vertex.z});
    for(const auto& face:mesh.faces) result.faces.push_back({{
        static_cast<std::uint16_t>(face.indices[0]),static_cast<std::uint16_t>(face.indices[1]),
        static_cast<std::uint16_t>(face.indices[2])},face.surfaceMaterial});
    return result;
}

AabbNodeData buildAabbTree(const Mesh& mesh,const neoshared::bif::JobControl& job) {
    auto geometry=collisionGeometry(mesh);neoshared::bwm::buildAabb(geometry,job);
    AabbNodeData result;result.valid=true;
    if(!geometry.nodes.empty()) result.root=static_cast<std::size_t>(geometry.root);
    for(const auto& node:geometry.nodes) result.nodes.push_back({
        {node.minimum[0],node.minimum[1],node.minimum[2]},
        {node.maximum[0],node.maximum[1],node.maximum[2]},
        node.firstChild,node.secondChild,node.face,static_cast<std::uint32_t>(node.splitMask),0u});
    return result;
}

void rebuildAabbTrees(Model& model,const neoshared::bif::JobControl& job) {
    for(auto& node:model.nodes) if(node.aabb) node.aabb=buildAabbTree(model.meshes[*node.mesh],job);
}

} // namespace neomdl::model
