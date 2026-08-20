#include "core/LensFlares.hpp"
#include <algorithm>
#include <cmath>

namespace neomdl {
namespace {
using model::Vec3;
bool finite(Vec3 v) {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
float magnitude(Vec3 v) {return std::sqrt((v.x*v.x+v.y*v.y)+v.z*v.z);}
void require(bool okay,const char* message) {
    if(!okay)throw model::ModelError(std::string("Engine lens flares: ")+message);
}
std::uint8_t channel(float color) {
    require(std::isfinite(color),"undefined/non-finite flare color");
    // Engine cvttss2si after saturation, NOT nearest-byte rounding.
    return static_cast<std::uint8_t>(std::clamp(color,0.0f,1.0f)*255.0f);
}
}
LensFlareFrame updateEngineLensFlares(LightScene& lights,const SceneQueries& queries,
    const LensFlareCamera& camera,float elapsed,const LensFlareSettings& settings) {
    LensFlareFrame output;
    if(!settings.enabled)return output;
    require(std::isfinite(elapsed)&&elapsed>=0.0f,"invalid frame elapsed time");
    require(finite(camera.position)&&std::isfinite(camera.orientation.x)&&
        std::isfinite(camera.orientation.y)&&std::isfinite(camera.orientation.z)&&
        std::isfinite(camera.orientation.w),"invalid camera");
    require(std::isfinite(settings.visibilitySpeed)&&std::isfinite(settings.sizeScale),"invalid globals");
    if(!lights.roomCollectionDiagnostic().empty()) {
        output.diagnostics.push_back(lights.roomCollectionDiagnostic());return output;
    }
    // Copy IDs, as the original copies GetLensFlareLights' list. Visibility is
    // on the registered light and survives ordinary pose/controller updates.
    const auto ids=lights.collectedFlareLights();
    for(const auto id:ids) {
        auto* light=lights.find(id);if(!light)continue;
        try {
            require(light->flareDiagnostic.empty(),light->flareDiagnostic.c_str());
            require(finite(light->position)&&finite(light->color)&&std::isfinite(light->flareRadius)&&
                std::isfinite(light->flareVisibility),"invalid light inputs");
            const Vec3 delta{light->position.x-camera.position.x,
                             light->position.y-camera.position.y,
                             light->position.z-camera.position.z};
            if(magnitude(delta)>light->flareRadius) {++output.outsideRadius;continue;}
            if(settings.hitCheck) {
                SceneHitInfo info;info.surfaceMask=0xbfffffffu;info.ignoreBoxStartInside=true;
                ++output.queried;
                const bool occluded=settings.cameraToLight
                    ? queries.hit(camera.position,light->position,info)
                    : queries.hit(light->position,camera.position,info);
                const float change=elapsed*settings.visibilitySpeed;
                if(occluded) {
                    ++output.occluded;
                    light->flareVisibility=std::max(0.0f,light->flareVisibility-change);
                    if(light->flareVisibility==0.0f)continue;
                } else light->flareVisibility=std::min(1.0f,light->flareVisibility+change);
            }
            auto eye=engineRotate(engineInverse(camera.orientation),delta);
            const float distance=magnitude(eye);
            // Engine Vector/scalar division computes one float32 reciprocal
            // before multiplying all components (K1 Desktop 0x3df18f..0x3df1a2).
            if(distance!=0.0f) {
                const float reciprocal=1.0f/distance;
                eye.x*=reciprocal;eye.y*=reciprocal;eye.z*=reciprocal;
            }
            // planelineintersection((0,0,-1),2,0,eye), negated. Its parallel
            // branch returns start=0; do not invent a screen clamp or cull.
            const float t=eye.z!=0.0f?2.0f/eye.z:0.0f;
            const model::Vec2 projected{-eye.x*t,-eye.y*t};
            const float facing=std::max(0.0f,-eye.z);
            const float angular=facing*facing;
            const float remaining=light->flareRadius-distance;
            const float inverseRadius=light->flareRadius!=0.0f?1.0f/light->flareRadius:1.0f;
            for(std::size_t i=0;i<light->flares.size();++i) {
                const auto& element=light->flares[i];
                require(!element.texture.empty(),"flare element has no texture");
                require(std::isfinite(element.size)&&std::isfinite(element.position)&&finite(element.colorShift),
                        "invalid authored flare element");
                LensFlareQuad quad;quad.light=id;quad.instanceIndex=light->instanceIndex;
                quad.element=i;quad.texture=element.texture;
                quad.center={projected.x*element.position,projected.y*element.position};
                // Preserve the engine operation order, especially byte boundaries.
                Vec3 c{light->color.x+element.colorShift.x,light->color.y+element.colorShift.y,
                       light->color.z+element.colorShift.z};
                const auto color=[&](float v) {
                    v*=light->flareVisibility;v*=angular;v*=remaining;v*=inverseRadius;
                    return channel(v);
                };
                quad.color={{color(c.x),color(c.y),color(c.z),255}};
                quad.halfSize=((remaining*element.size)/light->flareRadius)*settings.sizeScale;
                require(std::isfinite(quad.halfSize)&&std::isfinite(quad.center.x)&&std::isfinite(quad.center.y),
                        "undefined flare geometry (including coincident zero-radius light)");
                // Black RGB is NOT a no-op with GL_ONE/GL_ONE alpha blending.
                output.quads.push_back(std::move(quad));
            }
        } catch(const model::ModelError& error) {
            output.diagnostics.push_back("Light "+std::to_string(id)+": "+error.what());
        }
    }
    return output;
}
void buildLensFlareQuad(const LensFlareQuad& flare,model::Mesh& mesh) {
    require(std::isfinite(flare.center.x)&&std::isfinite(flare.center.y)&&std::isfinite(flare.halfSize),
            "invalid submitted quad");
    mesh.vertices.resize(4);mesh.indices={0,1,2,0,2,3};mesh.hasVertexColors=true;
    mesh.faces.clear();mesh.skinned=false;mesh.dangly.reset();mesh.cpuPositions.reset();
    const float x=flare.center.x,y=flare.center.y,s=flare.halfSize;
    const model::Vec3 points[4]{{x-s,y-s,-2},{x+s,y-s,-2},{x+s,y+s,-2},{x-s,y+s,-2}};
    const model::Vec2 uv[4]{{0,0},{1,0},{1,1},{0,1}};
    for(std::size_t i=0;i<4;++i) {
        mesh.vertices[i]={};mesh.vertices[i].position=points[i];
        mesh.vertices[i].texcoord0=uv[i];mesh.vertices[i].normal={0,0,1};
        mesh.vertices[i].color=flare.color;
    }
}
} // namespace neomdl
