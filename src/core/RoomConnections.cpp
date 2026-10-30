#include "core/AreaScene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace neomdl {

void AreaScene::setRoomVisibility(LightRoomId from,LightRoomId to,bool visible) {
    auto& first=rooms_.at(std::size_t(from-1));auto& second=rooms_.at(std::size_t(to-1));
    auto& forward=first.influencingRooms;auto& reverse=second.influencingRooms;
    if(visible) {
        if(std::find(forward.begin(),forward.end(),to)==forward.end()) {
            forward.push_back(to);reverse.push_back(from);
            // Appending the authored pair preserves native neighbor/light insertion order.
            visibilityBlocks_.push_back({first.name,{second.name}});
        }
    } else {
        forward.erase(std::remove(forward.begin(),forward.end(),to),forward.end());
        reverse.erase(std::remove(reverse.begin(),reverse.end(),from),reverse.end());
        const auto firstName=resourceKey(first.name),secondName=resourceKey(second.name);
        for(auto& block:visibilityBlocks_) {
            const auto source=resourceKey(block.room);
            if(source!=firstName&&source!=secondName)continue;
            const auto& target=source==firstName?secondName:firstName;
            block.visible.erase(std::remove_if(block.visible.begin(),block.visible.end(),
                [&](const auto& name){return resourceKey(name)==target;}),block.visible.end());
        }
    }
}

std::vector<std::uint8_t> AreaScene::visibilityBytes() const {
    std::ostringstream text;
    for(const auto& block:visibilityBlocks_) {
        text<<block.room<<' '<<block.visible.size()<<"\r\n";
        for(const auto& target:block.visible)text<<"  "<<target<<"\r\n";
    }
    const auto value=text.str();return {value.begin(),value.end()};
}

std::vector<RoomConnection> AreaScene::roomConnections(LightRoomId id) const {
    const auto& source=rooms_.at(std::size_t(id-1));std::vector<RoomConnection> result;
    if(!source.walkmesh)return result;
    const auto endpoints=[](const neoshared::bwm::BwmFile& mesh,std::uint32_t edge) {
        const auto& face=mesh.faces.at(edge/3);const auto& a=mesh.vertices.at(face.vertices[edge%3]);
        const auto& b=mesh.vertices.at(face.vertices[(edge%3+1)%3]);
        return std::pair<model::Vec3,model::Vec3>{{a[0],a[1],a[2]},{b[0],b[1],b[2]}};
    };
    const auto squared=[](model::Vec3 a,model::Vec3 b) {
        const auto x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return x*x+y*y+z*z;
    };
    for(const auto& edge:source.walkmesh->edges) {
        RoomConnection connection;connection.edge=edge.index;connection.target=edge.transition;
        const auto points=endpoints(*source.walkmesh,edge.index);connection.first=points.first;connection.second=points.second;
        if(edge.transition>=0&&std::size_t(edge.transition)<rooms_.size()) {
            const auto& target=rooms_[std::size_t(edge.transition)];
            if(target.walkmesh) {
                auto closest=std::numeric_limits<float>::max();
                for(const auto& other:target.walkmesh->edges) {
                    const auto points=endpoints(*target.walkmesh,other.index);
                    const auto error=std::max(squared(connection.first,points.second),squared(connection.second,points.first));
                    if(error<closest) {
                        closest=error;connection.targetEdge=other.index;connection.targetFirst=points.first;connection.targetSecond=points.second;
                        connection.gap=std::sqrt(error);connection.aligned=error<0.01f;connection.reciprocal=other.transition==std::int32_t(id-1);
                    }
                }
            }
        }
        result.push_back(connection);
    }
    return result;
}
}
