#pragma once
#include "core/ResourceContext.hpp"
#include <model/Model.hpp>
#include <neoshared/bwm/BwmFile.hpp>
#include <map>

namespace neomdl {
struct PlanarTransform {
    float xx{1},xy{},yx{},yy{1};
    bool mirrored{};
    model::Vec3 direction(model::Vec3 value) const;
    model::Vec3 point(model::Vec3 value,model::Vec3 pivot={}) const;
    model::Quaternion orientation(model::Quaternion value) const;
    model::Quaternion facing(model::Quaternion value) const;
};
PlanarTransform planarTransform(float degrees,bool flipX=false,bool flipY=false);
void transformWalkmesh(neoshared::bwm::BwmFile& mesh,const PlanarTransform& transform,model::Vec3 pivot={});
void transformBinaryModel(std::vector<std::uint8_t>& mdl,std::optional<std::vector<std::uint8_t>>& mdx,
    const PlanarTransform& transform,const std::map<std::string,std::string>& names={});
std::vector<neoshared::ResourceDocument> transformModelResources(const std::string& sourceName,
    const std::string& outputName,const ResourceResolver& resolver,const PlanarTransform& transform,
    std::vector<neoshared::ResourceDocument>* originals=nullptr);
}
