#include "model/Shadow.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <utility>

namespace neomdl::model {
namespace {
Vec3 add(Vec3 a,Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a,float s) { return {a.x*s,a.y*s,a.z*s}; }
float dot(Vec3 a,Vec3 b) { return (a.x*b.x+a.y*b.y)+a.z*b.z; }
bool finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
void require(bool value,const char* message) { if (!value) throw ModelError(message); }
Vec3 unit(Vec3 a) {
    const float length=std::sqrt(dot(a,a));
    // Desktop normalize(Vector const&) 0x313428; its double threshold is 1e-9.
    return static_cast<double>(length)<1e-9 ? Vec3{1,0,0} : mul(a,1.0f/length);
}
float side(ShadowPlane p,Vec3 v) { return dot(p.normal,v)+p.distance; }
Vec3 intersection(ShadowPlane p,Vec3 a,Vec3 b) {
    const auto delta=sub(b,a);
    const auto denominator=dot(p.normal,delta);
    const float t=denominator==0.0f ? 0.0f : -side(p,a)/denominator;
    return add(a,mul(delta,t));
}
Vertex vertex(Vec3 position) { Vertex v;v.position=position;v.normal={0,0,1};return v; }
}
Mesh buildShadowVolume(const Mesh& mesh,const ShadowProjectionInput& input,
                       const DanglyMeshState* dangly) {
    Mesh result;
    if (!mesh.castsShadow && !mesh.beaming) return result;
    // These are engine no-output branches in BOTH supplied builds. The
    // surviving virtual calls resolve to AsVertexPrimitive*, not draw methods.
    if (mesh.skinned || input.settings.directPrimitivePath) return result;
    const std::vector<Vec3>* positions = nullptr;
    if (mesh.dangly && dangly && dangly->initialized) {
        require(dangly->localPositions.size()==mesh.vertices.size(),
                "Dangly shadow local-position pool has the wrong size");
        positions=&dangly->localPositions;
    }
    else if (mesh.dangly && mesh.dangly->restPositions) {
        require(mesh.dangly->restPositions->size()==mesh.vertices.size(),
                "Dangly engine rest-position pool has the wrong size");
        positions=&*mesh.dangly->restPositions;
    } else if (mesh.cpuPositions) {
        require(mesh.cpuPositions->size()==mesh.vertices.size(),
                "Engine CPU shadow position pool has the wrong size");
        positions=&*mesh.cpuPositions;
    }
    require(finite(input.localLight)&&finite(input.plane.normal)&&
            std::isfinite(input.plane.distance)&&std::isfinite(input.settings.nearCapOffset)&&
            std::isfinite(input.projectionDistance)&&input.projectionDistance>0.0f,
            "Invalid CPU shadow inputs");
    require(mesh.vertices.size()<=32767u && mesh.faces.size()<=32767u,
            "CPU shadow source exceeds engine signed-index limits");
    const auto direction=unit(mul(input.localLight,-1.0f));
    auto nearCap=mul(direction,input.settings.nearCapOffset);
    auto farCap=add(input.localLight,mul(direction,input.projectionDistance));
    if (!mesh.beaming) {
        if (side(input.plane,farCap)>0.0f) {
            if (input.plane.distance>0.0f) return result;
            farCap=intersection(input.plane,nearCap,farCap);
        }
        if (side(input.plane,nearCap)>0.0f)
            nearCap=intersection(input.plane,nearCap,farCap);
    }
    std::vector<float> facing;
    facing.reserve(mesh.faces.size());
    for (const auto& face:mesh.faces) {
        require(finite(face.normal)&&std::isfinite(face.planeDistance),"Non-finite shadow face plane");
        for (const auto v:face.indices)
            require(v>=0&&static_cast<std::size_t>(v)<mesh.vertices.size(),"Invalid shadow face vertex");
        for (const auto a:face.adjacentFaces)
            require(a==-1||(a>=0&&static_cast<std::size_t>(a)<mesh.faces.size()),"Invalid shadow adjacency");
        const float distance=dot(face.normal,input.localLight)+face.planeDistance;
        require(std::isfinite(distance),"Shadow face distance overflow");
        facing.push_back(distance);
    }
    std::vector<std::pair<std::uint32_t,std::uint32_t>> edges;
    std::vector<std::uint32_t> remap(mesh.vertices.size(),0u);
    for (std::size_t f=0;f<mesh.faces.size();++f) {
        if (facing[f]<0.0f) continue;
        const auto& face=mesh.faces[f];
        for (std::size_t e=0;e<3u;++e) {
            const auto adjacent=face.adjacentFaces[e];
            if (adjacent!=-1 && facing[f]*facing[static_cast<std::size_t>(adjacent)]>0.0f) continue;
            const auto a=static_cast<std::uint32_t>(face.indices[e]);
            const auto b=static_cast<std::uint32_t>(face.indices[(e+1u)%3u]);
            edges.emplace_back(a,b);remap[a]=remap[b]=1u;
        }
    }
    if (edges.empty()) return result;
    std::uint32_t count=0;
    for (std::size_t index=0;index<mesh.vertices.size();++index) if (remap[index]) {
        auto nearPoint=positions ? (*positions)[index] : mesh.vertices[index].position;
        require(finite(nearPoint),"Non-finite shadow vertex");
        const auto ray=unit(sub(nearPoint,input.localLight));
        auto farPoint=add(input.localLight,mul(ray,input.projectionDistance));
        if (!input.settings.disableVertexClipping) {
            if (side(input.plane,nearPoint)>0.0f) nearPoint=intersection(input.plane,input.localLight,nearPoint);
            if (side(input.plane,farPoint)>0.0f) farPoint=intersection(input.plane,input.localLight,farPoint);
        }
        if (input.settings.clampReversedExtrusion && dot(sub(nearPoint,farPoint),ray)>0.0f) farPoint=nearPoint;
        require(finite(nearPoint)&&finite(farPoint),"CPU shadow projection overflow");
        remap[index]=count++;
        result.vertices.push_back(vertex(nearPoint));result.vertices.push_back(vertex(farPoint));
    }
    require(finite(nearCap)&&finite(farCap),"CPU shadow cap overflow");
    result.vertices.push_back(vertex(nearCap));result.vertices.push_back(vertex(farCap));
    const auto nc=2u*count,fc=nc+1u;
    require(fc<=65535u,"CPU shadow output exceeds engine unsigned-index limits");
    result.indices.reserve(edges.size()*6u);
    for (std::size_t e=0;e<edges.size();++e) {
        const auto a=2u*remap[edges[e].first],b=2u*remap[edges[e].second];
        if ((e&1u)==0u) result.indices.insert(result.indices.end(),{nc,a,b,a+1u,b+1u,fc});
        else result.indices.insert(result.indices.end(),{fc,b+1u,a+1u,b,a,nc});
    }
    return result;
}

namespace {
Mat4 uniformScale(float value) {
    auto result=identityMatrix();
    result.values[0]=result.values[5]=result.values[10]=value;
    return result;
}
void engineShadowFrame(const EngineFrame& frame,Vec3 light,ShadowPlane plane,
                       Vec3& localLight,ShadowPlane& localPlane) {
    const auto inverse=engineQuaternionInverse(frame.orientation);
    localLight=engineQuaternionRotate(inverse,sub(light,frame.position));
    // Plane::Transform retains the transformed point as well as its normal;
    // d += dot(n,p) assumes an orthonormal quaternion and is not equivalent.
    // K1 Desktop 0x3a4510..0x3a465a, called by PartVolumeProject at 0x34b953.
    localPlane.normal=engineQuaternionRotate(inverse,plane.normal);
    const Vec3 point{-plane.normal.x*plane.distance,-plane.normal.y*plane.distance,
                     -plane.normal.z*plane.distance};
    const auto localPoint=engineQuaternionRotate(inverse,sub(point,frame.position));
    localPlane.distance=-dot(localPlane.normal,localPoint);
}
bool zero(Vec3 value) { return value.x==0.0f && value.y==0.0f && value.z==0.0f; }
void finiteMatrix(const Mat4& matrix) {
    for (const float value:matrix.values)
        require(std::isfinite(value),"Non-finite shadow draw transform");
}
bool engineNoGeometry(const Node& node,const Mesh& mesh) {
    // Lightsaber's inherited Project rejects its authored node first. Trigger
    // uses Part::Project (empty); AABB inherits the ordinary TriMesh path.
    if (node.lightsaber || node.trigger || mesh.skinned) return true;




    if(node.flags&kNodeAnimation)
        require(mesh.animated && node.animmesh.has_value(),"Missing decoded animmesh shadow part");
    return false;
}
}
std::vector<ShadowVolume> buildGobShadowVolumes(
        const Model& model,const Pose* pose,const DanglySimulationState* dangly,
        const GobShadowInput& input) {
    // K1 Desktop Gob::PartVolumeProject (0x34b83c) shares the K2 transform,
    // enabled-subtree, beaming-event and forward-child traversal contracts.
    require(std::isfinite(input.ownerFrame.scale),"Non-finite Gob shadow scale");
    require(std::isfinite(input.projectionDistance)&&input.projectionDistance>0.0f,
            "Invalid Gob shadow projection distance");
    if (pose) require(pose->positions.size()==model.nodes.size() &&
                      pose->orientations.size()==model.nodes.size() &&
                      pose->scales.size()==model.nodes.size(),"Incomplete shadow pose");
    if (input.enabledParts)
        require(input.enabledParts->size()==model.nodes.size(),"Incomplete shadow part-enable state");
    Vec3 ownerLight;ShadowPlane ownerPlane;
    engineShadowFrame(input.ownerFrame,input.worldLight,input.worldPlane,ownerLight,ownerPlane);
    std::vector<ShadowVolume> result;
    if (model.nodes.empty()) return result;
    std::size_t root=model.nodes.size();
    for (std::size_t n=0;n<model.nodes.size();++n) if (!model.nodes[n].parent) {
        require(root==model.nodes.size(),"Engine shadow Gob requires one part-tree root");root=n;
    }
    require(root<model.nodes.size(),"Shadow part tree has no root");
    // Validate the complete graph before generating geometry, including disabled
    // subtrees. This is host safety, not an alterengine engine traversal order.
    std::vector<std::uint8_t> visited(model.nodes.size());
    std::vector<std::size_t> pending{root};
    while (!pending.empty()) {
        const auto n=pending.back();pending.pop_back();
        require(n<model.nodes.size() && !visited[n],"Cyclic or duplicated shadow part tree");
        visited[n]=1;
        for (const auto child:model.nodes[n].children) {
            require(child<model.nodes.size() && model.nodes[child].parent &&
                    *model.nodes[child].parent==n,"Inconsistent shadow part parent");
            pending.push_back(child);
        }
    }
    for (const auto value:visited) require(value!=0,"Disconnected shadow part tree");
    struct Frame { std::size_t node; Vec3 light; ShadowPlane plane; Mat4 draw; };
    std::vector<Frame> stack{{root,ownerLight,ownerPlane,
        multiply(nodeTransform(input.ownerFrame.position,input.ownerFrame.orientation),
                 uniformScale(input.ownerFrame.scale))}};
    while (!stack.empty()) {
        auto frame=std::move(stack.back());stack.pop_back();
        const auto n=frame.node;
        const auto& node=model.nodes[n];
        if (input.enabledParts && !(*input.enabledParts)[n]) continue;
        const auto position=pose ? pose->positions[n] : node.position;
        const auto orientation=pose ? pose->orientations[n] : node.orientation;
        const float scale=pose ? pose->scales[n] : 1.0f;
        require(std::isfinite(scale),"Non-finite shadow part scale");
        const auto local=nodeTransform(position,orientation);
        Vec3 light;ShadowPlane plane;
        engineShadowFrame({position,orientation,scale},frame.light,frame.plane,light,plane);
        const auto world=multiply(frame.draw,multiply(local,uniformScale(scale)));
        finiteMatrix(world);
        if ((!zero(light) || input.settings.directPrimitivePath) && node.mesh) {
            require(*node.mesh<model.meshes.size(),"Invalid shadow part mesh index");
            const auto& mesh=model.meshes[*node.mesh];
            require(mesh.nodeIndex==n,"Shadow mesh/part association is inconsistent");



            const bool beginsBeaming=mesh.beaming && !node.trigger;
            Mesh geometry;
            if ((mesh.castsShadow || mesh.beaming) && !engineNoGeometry(node,mesh) &&
                !input.settings.directPrimitivePath) {
                const auto* state=dangly && *node.mesh<dangly->meshes.size()
                    ? &dangly->meshes[*node.mesh] : nullptr;
                ShadowProjectionInput projection{light,input.projectionDistance,plane,input.settings};
                geometry=buildShadowVolume(mesh,projection,state);
            }
            if (beginsBeaming || !geometry.indices.empty())
                result.push_back({std::move(geometry),world,n,*node.mesh,0u,mesh.beaming,beginsBeaming});
        }
        // Reverse push retains the engine forward child-list visitation.
        for (auto it=node.children.rbegin();it!=node.children.rend();++it)
            stack.push_back({*it,light,plane,world});
    }
    return result;
}
std::vector<ShadowVolume> buildDynamicMeshProjections(
        const std::vector<MeshProjectionSource>& sources,
        const std::vector<MeshProjectionLight>& lights,
        const MeshProjectionInput& input) {
    require(finite(input.cameraFarPlane.normal)&&std::isfinite(input.cameraFarPlane.distance)&&
            std::isfinite(input.minimumShadowLength)&&input.minimumShadowLength>0.0f,
            "Invalid dynamic mesh-projection inputs");
    auto plane=input.cameraFarPlane;plane.distance+=0.05f;
    require(std::isfinite(plane.distance),"Mesh-projection plane overflow");
    std::vector<ShadowVolume> result;
    for (std::size_t l=0;l<lights.size();++l) {
        const auto& light=lights[l];
        require(finite(light.position)&&std::isfinite(light.radius)&&light.radius>=0.0f&&
                std::isfinite(light.shadowRadius),"Invalid mesh-projection light");
        const float distance=std::max(input.minimumShadowLength,
            light.shadowRadius>0.0f ? light.shadowRadius : light.radius);
        for (std::size_t m=0;m<sources.size();++m) {
            const auto& source=sources[m];
            require(source.mesh && source.node,"Missing registered mesh-projection source");
            Vec3 localLight;ShadowPlane localPlane;
            require(rigidShadowFrame(source.world,light.position,plane,localLight,localPlane),
                    "Registered mesh projection requires a rigid Part position/orientation");
            const Vec3 origin{source.world.values[12],source.world.values[13],source.world.values[14]};
            const auto delta=sub(origin,light.position);

            if (!(std::sqrt(dot(delta,delta))<distance+2.0f)) continue;
            if ((!source.mesh->castsShadow && !source.mesh->beaming) ||
                engineNoGeometry(*source.node,*source.mesh)) continue;
            ShadowProjectionInput projection{localLight,distance,localPlane,input.settings};
            auto geometry=buildShadowVolume(*source.mesh,projection,source.dangly);
            if (!geometry.indices.empty())
                result.push_back({std::move(geometry),source.world,source.mesh->nodeIndex,m,l,source.mesh->beaming});
        }
    }
    return result;
}
Mesh buildShadowFadeFan(Vec3 scenePosition,Vec3 lightPosition,float radius,float weight) {
    require(finite(scenePosition)&&finite(lightPosition)&&std::isfinite(radius)&&radius>=0.0f&&
            std::isfinite(weight)&&weight>=0.0f&&weight<=1.0f,"Invalid shadow fade inputs");
    Mesh result;
    if (radius==0.0f) return result;
    Vec3 center=scenePosition;
    float extent=50.0f;
    if (radius<=50.0f) {
        center={lightPosition.x,lightPosition.y,scenePosition.z};
        const float height=std::abs(lightPosition.z-scenePosition.z);
        extent=std::sqrt(std::max(0.0f,radius*radius-height*height));
    }
    auto origin=vertex(center);origin.color={0,0,0,static_cast<std::uint8_t>(weight*0.5f*255.0f)};
    result.vertices.push_back(origin);result.indices.push_back(0);
    for (std::uint32_t i=0;i<10u;++i) {
        const float angle=static_cast<float>(i)*0.6981317400932312f;
        auto v=vertex({center.x+std::cos(angle)*extent,center.y+std::sin(angle)*extent,center.z});
        v.color={0,0,0,0};result.vertices.push_back(v);result.indices.push_back(i+1u);
    }
    return result;
}
bool rigidShadowFrame(const Mat4& world,Vec3 light,ShadowPlane plane,Vec3& localLight,ShadowPlane& localPlane) noexcept {
    const auto& a=world.values;
    for (const auto v:a) if (!std::isfinite(v)) return false;
    if (!finite(light)||!finite(plane.normal)||!std::isfinite(plane.distance)) return false;
    const Vec3 x{a[0],a[1],a[2]},y{a[4],a[5],a[6]},z{a[8],a[9],a[10]},translation{a[12],a[13],a[14]};
    // Host validation tolerance, not an engine scale approximation. All accepted
    // transforms must represent a proper rigid frame generated by pose quaternions.
    constexpr float tolerance=1e-5f;
    if (std::abs(dot(x,x)-1)>tolerance||std::abs(dot(y,y)-1)>tolerance||std::abs(dot(z,z)-1)>tolerance||
        std::abs(dot(x,y))>tolerance||std::abs(dot(x,z))>tolerance||std::abs(dot(y,z))>tolerance||
        a[3]!=0.0f||a[7]!=0.0f||a[11]!=0.0f||a[15]!=1.0f) return false;
    const Vec3 cross{y.y*z.z-y.z*z.y,y.z*z.x-y.x*z.z,y.x*z.y-y.y*z.x};
    if (dot(x,cross)<0.0f) return false;
    const auto delta=sub(light,translation);
    localLight={dot(x,delta),dot(y,delta),dot(z,delta)};
    localPlane={{dot(x,plane.normal),dot(y,plane.normal),dot(z,plane.normal)},plane.distance+dot(plane.normal,translation)};
    return finite(localLight)&&finite(localPlane.normal)&&std::isfinite(localPlane.distance);
}
} // namespace neomdl::model
