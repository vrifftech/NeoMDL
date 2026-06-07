#include "model/EngineCpuSkin.hpp"
#include <array>
#include <limits>

namespace neomdl::model {
namespace {
std::uint32_t unsignedSkinIndex(float value) {


    if (!(value > 0.0f)) return 0u;
    if (value >= 4294967296.0f) return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(value);
}
}
RenderGeometry EngineCpuSkinPools::execute(const Mesh& mesh,const DeformedMesh& pose,
                                          const EngineCpuSkinSource& source,
                                          bool shell,float shellAmount) {


    if(positions_.size()<mesh.vertices.size()) {
        positions_.resize(mesh.vertices.size());normals_.resize(mesh.vertices.size());
    }
    std::array<std::array<float,12>,17> palette{};
    const auto& part=pose.skinPartFrame.values;
    for(unsigned slot=0;slot<palette.size();++slot) {
        const auto& world=pose.skinPalette[slot].rows;
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col)
            for(unsigned k=0;k<3;++k)
                palette[slot][row*4+col]+=part[row*4+k]*(world[k*4+col]-(col==3?part[12+k]:0.0f));
    }
    const auto vertex=[&](std::uint32_t index,bool triangle) {
        const auto& point=source.positions[index];
        const auto& normal=source.normals[index];
        Vec3 position{},weightedNormal{};
        for(unsigned k=0;k<4;++k) {
            const float weight=source.weights[index][k];
            if(weight==0.0f)continue; // the engine four-way loop's only skip
            const auto slot=triangle?static_cast<std::uint32_t>(source.triangleBoneIndices[index][k]):
                source.unsignedStripBoneIndices?unsignedSkinIndex(source.stripBoneIndices[index][k]):
                static_cast<std::uint32_t>(static_cast<std::int64_t>(source.stripBoneIndices[index][k]));
            const auto& rows=palette[slot];
            const auto component=[&](unsigned row,Vec3 value) {
                const auto at=row*4;return rows[at]*value.x+rows[at+1]*value.y+rows[at+2]*value.z;
            };
            position.x+=(component(0,point)+rows[3])*weight;
            position.y+=(component(1,point)+rows[7])*weight;
            position.z+=(component(2,point)+rows[11])*weight;
            weightedNormal.x+=component(0,normal)*weight;
            weightedNormal.y+=component(1,normal)*weight;
            weightedNormal.z+=component(2,normal)*weight;
        }
        // The shell argument is inspected in the triangle loop only. Strip
        // vertices remain undisplaced, including ones with nonzero normals.
        if(triangle && shell) {
            position.x+=weightedNormal.x*shellAmount;
            position.y+=weightedNormal.y*shellAmount;
            position.z+=weightedNormal.z*shellAmount;
        }
        positions_[index]=position;normals_[index]=weightedNormal;
    };
    for(const auto index:source.stripVertices)vertex(index,false);
    for(const auto index:source.triangleVertices)vertex(index,true);
    RenderGeometry result;result.indices=source.indices;
    result.vertices.resize(mesh.vertices.size());
    for(std::size_t index=0;index<mesh.vertices.size();++index) {
        auto& v=result.vertices[index];const auto& authored=mesh.vertices[index];
        v.position=positions_[index];v.normal=normals_[index];v.rawNormal=normals_[index];
        v.texcoord0=authored.texcoord0;v.texcoord1=authored.texcoord1;
        for(unsigned c=0;c<4;++c)v.color[c]=static_cast<float>(authored.color[c])/255.0f;
    }
    return result;
}
} // namespace neomdl::model
