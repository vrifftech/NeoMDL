#include "core/StaticPrelighting.hpp"
#include <model/EnginePrelighting.hpp>
#include <cmath>

namespace neomdl {
namespace {
std::uint8_t encode(float value) noexcept {
    // The engine max/min and float-to-integer conversion produce byte zero
    // for nonpositive/NaN and round finite positive channels with +0.5.
    if(!(value>0.0f))return 0;
    if(value>=1.0f)return 255;
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(value*255.0f+0.5f));
}
}
StaticPrelightUpdate updateStaticPrelightPool(StaticPrelightPool& pool,
    const model::Mesh& mesh,const HitPartTransform& frame,model::Vec3 self,
    bool decal,const std::vector<StaticPrelightLight>& lights,std::int32_t tileMode) {
    if(tileMode!=0 || mesh.enginePrelightVertexCount==0)return StaticPrelightUpdate::Skipped;
    if(lights.empty() && self.x==0.0f && self.y==0.0f && self.z==0.0f) {
        if(!pool.active)return StaticPrelightUpdate::Skipped;
        pool.active=false;pool.colors.clear();++pool.generation;
        return StaticPrelightUpdate::Deleted;
    }
    const auto source=model::decodeEnginePrelightSource(mesh);
    pool.colors.resize(source.vertices.size());
    pool.active=true;
    for(std::size_t i=0;i<source.vertices.size();++i) {
        const auto& vertex=source.vertices[i];
        const auto normal=engineRotate(frame.orientation,vertex.normal);
        auto position=engineRotate(frame.orientation,vertex.position);
        position.x+=frame.position.x;position.y+=frame.position.y;position.z+=frame.position.z;
        model::Vec3 sum=decal?model::Vec3{1,1,1}:self;
        for(const auto& light:lights) {
            if(light.radius==0.0f)continue;
            model::Vec3 direction{light.position.x-position.x,light.position.y-position.y,
                                  light.position.z-position.z};
            float distance=std::sqrt((direction.x*direction.x+direction.y*direction.y)+direction.z*direction.z);
            if(distance==0.0f)distance=0.000001f;
            const float attenuation=1.0f-distance/light.radius;
            if(attenuation<0.0f)continue;
            float dot=1.0f;
            model::Vec3 material=mesh.ambient;
            if(!light.ambientOnly) {
                const float reciprocal=1.0f/distance;
                direction.x*=reciprocal;direction.y*=reciprocal;direction.z*=reciprocal;
                dot=(normal.x*direction.x+normal.y*direction.y)+normal.z*direction.z;
                if(dot<0.0f)continue;
                material=mesh.diffuse;
            }
            const float factor=attenuation*dot;
            sum.x+=(light.color.x*factor)*material.x;
            sum.y+=(light.color.y*factor)*material.y;
            sum.z+=(light.color.z*factor)*material.z;
        }
        if(source.authoredColors) {
            sum.x*=static_cast<float>(vertex.color[0])/255.0f;
            sum.y*=static_cast<float>(vertex.color[1])/255.0f;
            sum.z*=static_cast<float>(vertex.color[2])/255.0f;
        }
        pool.colors[i]={{encode(sum.x),encode(sum.y),encode(sum.z),255}};
    }
    ++pool.generation;
    return StaticPrelightUpdate::Updated;
}
}
