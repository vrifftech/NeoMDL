#include "core/RoomSurfaceMesh.hpp"
#include "core/SceneQueries.hpp"
#include "core/Engine2DA.hpp"
#include <twoda/TwoDAFile.hpp>

#include <cmath>

namespace neomdl {
namespace {
model::Vec3 vector(neoshared::bwm::Vector v) {return {v[0],v[1],v[2]};}
int hitNode(const neoshared::bwm::BwmFile& mesh,std::int32_t index,
            model::Vec3 start,model::Vec3& end,RoomSurfaceHitInfo& info) {
    const auto& node=mesh.nodes.at(static_cast<std::size_t>(index));
    const model::Bounds bounds{vector(node.minimum),vector(node.maximum),true};
    if(!engineHitBox(start,end,bounds))return 0;
    if(node.face!=-1) {
        const auto& face=mesh.faces.at(static_cast<std::size_t>(node.face));

        const auto surface=1u<<(face.material&31u);
        if(!(info.surfaceMask&surface))return 0;
        std::array<model::Vec3,3> vertices;
        for(std::size_t i=0;i<3;++i)vertices[i]=vector(mesh.vertices.at(face.vertices[i]));
        if(!engineHitTriangle(vertices,start,end))return 0;
        info.face=node.face;info.surface=surface;return 1;
    }
    const bool first=(static_cast<std::uint32_t>(node.splitMask)&info.directionMask)!=0;
    const auto count=hitNode(mesh,first?node.firstChild:node.secondChild,start,end,info);
    return count+hitNode(mesh,first?node.secondChild:node.firstChild,start,end,info);
}
bool roomHit(const neoshared::bwm::BwmFile& mesh,model::Vec3 start,
             model::Vec3 end,RoomSurfaceHitInfo& info,std::uint32_t mask) {
    if(mesh.nodes.empty())return false; // engine empty-tree branch
    info.surfaceMask=mask;
    auto count=hitNode(mesh,mesh.root,start,end,info);


    if(info.face==-1) {
        const float offset=(1.0f/std::sqrt(2.0f))*0.001f;
        start.x+=offset;start.y+=offset;end.x+=offset;end.y+=offset;
        count=hitNode(mesh,mesh.root,start,end,info);
    }
    if(count>0) {info.hit=true;info.position=end;}
    return info.hit;
}
}
void RoomSurfaceHitInfo::clearOutput() noexcept {
    hit=false;position={};surface=0;
}
bool engineRoomHitAll(const neoshared::bwm::BwmFile& mesh,
                      model::Vec3 start,model::Vec3 end,RoomSurfaceHitInfo& info) {
    return roomHit(mesh,start,end,info,0xffffffffu);
}
std::uint32_t engineWalkableMaterialMask(const std::vector<std::uint8_t>& bytes) {
    neo2da::TwoDAFile table;table.loadBytes(bytes);
    std::uint32_t result=0;
    for(std::size_t i=0;i<32;++i) {
        int entry=0;
        // The mesh constructor consumes the typed destination even on failure.
        engine2da::getInt(table,static_cast<int>(i),"walkcheck",entry);
        if(entry==1)result+=1u<<i;
    }
    return result;
}
bool engineRoomHitWalkable(const neoshared::bwm::BwmFile& mesh,
    model::Vec3 start,model::Vec3 end,RoomSurfaceHitInfo& info,std::uint32_t walkableMaterials) {
    return roomHit(mesh,start,end,info,walkableMaterials);
}
float engineRoomFaceHeight(const neoshared::bwm::BwmFile& mesh,std::int32_t face,model::Vec3 point) {
    const auto index=static_cast<std::size_t>(face);
    const auto& normal=mesh.faceNormals.at(index);
    // PlaneHeightAtPoint's exact zero branch and operation order are shared by
    // all four executables. The caller's point Z is not used by the plane solve.
    if(normal[2]==0.0f)return 0.0f;
    return -((normal[0]*point.x+mesh.faceDistances.at(index))+normal[1]*point.y)/normal[2];
}
bool engineRoomContainsPoint(const neoshared::bwm::BwmFile& mesh,
                             model::Vec3 position,std::uint32_t walkableMaterials) {
    model::Vec3 start{position.x,position.y,position.z+1000.0f};
    model::Vec3 end{position.x,position.y,position.z-1000.0f};
    RoomSurfaceHitInfo info;
    return roomHit(mesh,start,end,info,walkableMaterials);
}
} // namespace neomdl
