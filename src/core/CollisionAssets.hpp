#pragma once

#include "core/ResourceContext.hpp"
#include <model/AabbBuild.hpp>
#include <map>

namespace neomdl {

enum class CollisionKind { Placeable, DoorClosed, DoorOpen1, DoorOpen2, Room };
struct CollisionAsset {
    std::string text;
    std::optional<std::filesystem::path> asciiPath;
    std::vector<std::filesystem::path> inputs;
};
using CollisionSet = std::map<CollisionKind,CollisionAsset>;
struct CollisionMesh {
    neoshared::bwm::BwmFile mesh;
    // Conventional KOTOR authoring materials. Imported rooms instead retain
    // the material membership of their serialized walkable-face prefix.
    std::set<std::uint32_t> walkableMaterials{0,1,3,4,5,6,9,10,11,12,13,14,16,18,20};
};
struct CollisionOutput {std::filesystem::path path;std::vector<std::uint8_t> bytes;};

const char* collisionLabel(CollisionKind kind);
std::filesystem::path collisionPath(const std::filesystem::path& model,CollisionKind kind);
ResourceResolver collisionResources(const std::filesystem::path& model, ResourceResolver resolver);
CollisionMesh readCollisionAscii(const std::string& text,CollisionKind kind);
std::string writeCollisionAscii(const CollisionMesh& mesh,CollisionKind kind);
CollisionAsset readCollision(const neoshared::ResourceDocument& resource,CollisionKind kind,
                            const neoshared::bif::JobControl& job = {});
CollisionSet loadCollisions(const std::string& model,const ResourceResolver& resolver,
                           const neoshared::bif::JobControl& job = {});
std::vector<CollisionOutput> collisionOutputs(const CollisionSet& assets,
    const std::filesystem::path& model,bool binary,const neoshared::bif::JobControl& job = {});
CollisionMesh collisionFromModel(const model::Model& model,std::size_t node,
    CollisionKind kind,model::Vec3 placement = {},const neoshared::bif::JobControl& job = {});
model::Vec3 collisionLocalPoint(const model::Mat4& world,model::Vec3 point,model::Vec3 placement);
std::map<std::string,model::Vec3> collisionLayout(const std::string& text);

} // namespace neomdl
