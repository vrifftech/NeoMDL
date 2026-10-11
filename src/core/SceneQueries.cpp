#include "core/SceneQueries.hpp"
#include "core/ModelDocument.hpp"
#include "core/ReferenceScene.hpp"
#include "core/AreaScene.hpp"
#include "core/TriggerScene.hpp"
#include "core/SceneOwners.hpp"
#include "core/RoomSurfaceMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace neomdl {
namespace {
using namespace model;
Vec3 add(Vec3 a, Vec3 b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) noexcept { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a,float b) noexcept { return {a.x*b,a.y*b,a.z*b}; }
float dot(Vec3 a,Vec3 b) noexcept { return (a.x*b.x+a.y*b.y)+a.z*b.z; }
Vec3 cross(Vec3 a,Vec3 b) noexcept {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
float magnitude(Vec3 a) noexcept { return std::sqrt(dot(a,a)); }
bool finite(Vec3 v) noexcept {
    return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
}
void require(bool valid,const char* what) {
    if (!valid) throw ModelError(std::string("Engine scene hit query: ")+what);
}
void validateFrame(const HitPartTransform& f) {
    require(finite(f.position)&&std::isfinite(f.orientation.x)&&
        std::isfinite(f.orientation.y)&&std::isfinite(f.orientation.z)&&
        std::isfinite(f.orientation.w)&&std::isfinite(f.scale),"non-finite part frame");
}
void validateBox(const Bounds& b) {
    require(b.valid&&finite(b.minimum)&&finite(b.maximum),"missing/non-finite authored bounds");
}
Bounds scaledBox(Bounds b,float scale,Vec3 expansion={}) {
    b.minimum=sub(mul(b.minimum,scale),expansion);
    b.maximum=add(mul(b.maximum,scale),expansion);
    // Do not reorder a negative-scale box: the engine query does not.
    return b;
}
std::size_t positionCount(const Mesh& mesh) {
    return mesh.cpuPositions ? mesh.cpuPositions->size() : mesh.vertices.size();
}
Vec3 position(const Mesh& mesh,std::int16_t index) {
    require(mesh.cpuPositions.has_value(),"missing authored MDL hit-position pool");
    require(index>=0&&static_cast<std::size_t>(index)<mesh.cpuPositions->size(),
            "invalid signed CPU face index / missing MDL position pool");
    const auto i=static_cast<std::size_t>(index);
    const Vec3 v=(*mesh.cpuPositions)[i];
    require(finite(v),"non-finite CPU position");
    return v;
}


bool polygon(const Mesh& mesh,const MeshFace& face,float vertexScale,
             Vec3 start,Vec3 end,Vec3& output) {
    require(finite(face.normal)&&std::isfinite(face.planeDistance),"invalid authored face plane");
    const float from=dot(face.normal,start)+face.planeDistance;
    const float to=dot(face.normal,end)+face.planeDistance;
    if (from<0.0f||to>0.0f||from==to) return false;
    const float t=from/(from-to);
    const Vec3 p=add(mul(start,1.0f-t),mul(end,t));
    std::array<Vec3,3> v;
    for (std::size_t k=0;k<3;++k) v[k]=mul(position(mesh,face.indices[k]),vertexScale);
    for (std::size_t k=0;k<3;++k) {
        const Vec3 edge=sub(v[(k+1)%3],v[k]);
        if (dot(face.normal,cross(edge,sub(p,v[k]))) < -1.0e-5f) return false;
    }
    output=p;return true;
}
// Explicit traversal stack avoids engine recursion overrunning the host stack
// on a malformed/deep model. AABB end-shortening and split order are unchanged.
bool aabb(const Model& model,std::size_t nodeIndex,float scale,Vec3 start,Vec3& end,
          SceneHitInfo& info,const SceneHitSettings& settings) {
    const auto& node=model.nodes.at(nodeIndex);
    require(node.mesh&&*node.mesh<model.meshes.size(),"AABB has no mesh");
    if (!node.aabb||!node.aabb->root) return false;
    const auto& tree=*node.aabb;
    require(tree.valid&&*tree.root<tree.nodes.size(),"invalid AABB tree");
    const auto& mesh=model.meshes[*node.mesh];
    std::uint32_t direction=0;
    direction|=end.x>start.x?kAabbPositiveX:kAabbNegativeX;
    direction|=end.y>start.y?kAabbPositiveY:kAabbNegativeY;
    direction|=end.z>start.z?kAabbPositiveZ:kAabbNegativeZ;
    struct Visit { std::size_t index; bool exit; };
    std::vector<Visit> stack{{*tree.root,false}};
    std::vector<bool> active(tree.nodes.size()),visited(tree.nodes.size());
    bool hit=false;
    while (!stack.empty()) {
        const auto item=stack.back();stack.pop_back();
        require(item.index<tree.nodes.size(),"AABB child outside node array");
        if (item.exit) { active[item.index]=false;continue; }
        require(!active[item.index]&&!visited[item.index],"cyclic/shared AABB tree");
        visited[item.index]=true;
        const auto& n=tree.nodes[item.index];
        Bounds box{n.minimum,n.maximum,true};validateBox(box);
        if (!engineHitBox(start,end,scaledBox(box,scale))) continue;
        if (n.leaf()) {
            require(n.faceIndex>=0&&static_cast<std::size_t>(n.faceIndex)<mesh.faces.size(),
                    "AABB face outside face array");
            const auto& face=mesh.faces[static_cast<std::size_t>(n.faceIndex)];
            const std::uint32_t mask=std::uint32_t{1}<<(face.surfaceMaterial&31u);
            if (!(info.surfaceMask&mask)) continue;
            Vec3 p;
            // Engine AABB scales the boxes, NOT its leaf vertices.
            if (!polygon(mesh,face,1.0f,start,end,p)) continue;
            end=p;info.clearOutput();info.position=p;info.localPosition=p;
            info.normal=face.normal;info.surface=mask;info.face=n.faceIndex;
            hit=true;continue;
        }
        active[item.index]=true;stack.push_back({item.index,true});
        bool forward=(n.splitMask&direction)!=0;
        if (settings.aabbTestOpposite) forward=!forward;
        const auto first=forward?n.firstChild:n.secondChild;
        const auto second=forward?n.secondChild:n.firstChild;
        if (second>=0) stack.push_back({static_cast<std::size_t>(second),false});
        if (first>=0) stack.push_back({static_cast<std::size_t>(first),false});
    }
    return hit;
}





bool animmeshGeometry(const Mesh& mesh,const std::vector<Vec3>* positions,
                      float scale,Vec3 start,Vec3& end) {
    require(positions && positions->size()==mesh.vertices.size(),
            "animmesh requires its current defined local hit-position pool");
    bool found=false;
    for (const auto& face:mesh.faces) {
        std::array<Vec3,3> v;
        for (std::size_t k=0;k<3;++k) {
            const auto index=face.indices[k];
            require(index>=0 && static_cast<std::size_t>(index)<positions->size(),
                    "invalid signed animmesh face index");
            v[k]=mul((*positions)[static_cast<std::size_t>(index)],scale);
            require(finite(v[k]),"non-finite animmesh hit position");
        }
        if(engineHitTriangle(v,start,end))found=true;
    }
    return found;
}
int geometry(const Model& model,std::size_t index,float scale,Vec3 start,Vec3& end,
             SceneHitInfo& info,const SceneHitSettings& settings,
             const std::vector<Vec3>* animmeshPositions=nullptr) {
    const auto& node=model.nodes.at(index);
    if (node.aabb) return aabb(model,index,scale,start,end,info,settings)?1:0;
    if (!node.mesh) return 0;
    require(*node.mesh<model.meshes.size(),"mesh outside mesh array");
    const auto& mesh=model.meshes[*node.mesh];
    // AsPartSkin and the authored lightsaber mask are early-outs in the
    // virtual geometry consumer; Part::HitCheck still visits their children.
    if (mesh.skinned||(node.flags&kNodeSkin)||((node.flags&0x821u)==0x821u)) return 0;
    if (node.flags&kNodeAnimation) {
        require(mesh.animated && node.animmesh.has_value(),"missing decoded animmesh hit geometry");
        // Part::HitCheck aliases the output pointer to its local end argument.
        // PartAnimMesh::HitCheckGeom never writes CHitInfo (including normal,
        // face/surface/localPosition); outer Part/Gob transforms still apply.
        return animmeshGeometry(mesh,animmeshPositions,scale,start,end)?1:0;
    }
    if (mesh.vertices.empty()&&positionCount(mesh)==0) return 0;
    require(mesh.engineBounds.has_value(),"missing authored mesh query bounds");
    validateBox(mesh.engineBounds->box);
    auto box=scaledBox(mesh.engineBounds->box,scale,
        info.boundingBoxesOnly?settings.boxExpansion:Vec3{});
    if (info.boundingBoxesOnly) {
        Vec3 p=end;
        const int face=engineHitBox(start,end,box,&p);
        if (!face) return 0;
        if (face==-1&&info.ignoreBoxStartInside) {



            info.position=p;
            return -1;
        }
        info.clearOutput();end=p;info.position=p;
        switch (face) {
        case 1:info.normal.x=-1;break;case 2:info.normal.x=1;break;
        case 3:info.normal.y=-1;break;case 4:info.normal.y=1;break;
        case 5:info.normal.z=-1;break;case 6:info.normal.z=1;break;
        default:break;
        }
        return face;
    }
    if (!engineHitBox(start,end,box)) return 0;
    bool found=false;
    for (const auto& face:mesh.faces) {
        Vec3 p;
        if (polygon(mesh,face,scale,start,end,p)) { end=p;found=true; }
    }
    if (!found) return 0;
    info.clearOutput();info.position=end;info.localPosition=end;
    return 1; // authored triangle normal/material/face are NOT returned here
}
// K1 Desktop 0x32893c / K2 Desktop 0xaceb6. Each child sees the shortened parent-local endpoint. Local
// translation is multiplied by the cumulative scale; inverse rotations and
// segment coordinates are NOT divided by it.
bool partTree(const Model& model,const std::vector<HitPartTransform>& parts,
              std::size_t root,float inheritedScale,Vec3 start,Vec3& end,
              SceneHitInfo& info,const SceneHitSettings& settings,
              const AnimMeshHitPools& animmeshPools,
              bool detached=false) {
    require(root<model.nodes.size()&&parts.size()==model.nodes.size(),"invalid part tree inputs");
    struct Frame {
        std::size_t node{},nextChild{};float scale{};
        Vec3 start{},end{},offset{};Quaternion orientation{};bool found{};
    };
    std::vector<Frame> stack;
    std::vector<bool> active(model.nodes.size());
    auto enter=[&](std::size_t n,float parentScale,Vec3 a,Vec3 b) {
        require(n<model.nodes.size()&&!active[n],"invalid/cyclic part tree");
        active[n]=true;const auto& transform=parts[n];validateFrame(transform);
        Frame f;f.node=n;f.scale=parentScale*transform.scale;f.orientation=transform.orientation;
        require(std::isfinite(f.scale),"part scale overflow");
        f.offset=mul(transform.position,f.scale);
        const auto inverse=engineInverse(f.orientation);
        f.start=engineRotate(inverse,sub(a,f.offset));
        f.end=engineRotate(inverse,sub(b,f.offset));
        const auto mesh=model.nodes[n].mesh;
        const auto* positions=mesh && *mesh<animmeshPools.size() && animmeshPools[*mesh]
            ? &*animmeshPools[*mesh] : nullptr;
        const int own=geometry(model,n,f.scale,f.start,f.end,info,settings,positions);
        f.found=own!=0&&!(own==-1&&info.ignoreBoxStartInside);
        if (f.found) info.part=model.nodes[n].name;
        stack.push_back(f);
    };
    enter(root,inheritedScale,start,end);
    while (!stack.empty()) {
        auto& f=stack.back();const auto& node=model.nodes[f.node];
        if (!detached&&f.nextChild<node.children.size()) {
            const auto child=node.children[f.nextChild++];
            const auto scale=f.scale;const auto a=f.start,b=f.end;
            enter(child,scale,a,b);continue;
        }
        const bool found=f.found;
        Vec3 result=f.end;
        if (found) {
            result=add(engineRotate(f.orientation,f.end),f.offset);
            info.position=result;info.normal=engineRotate(f.orientation,info.normal);
        }
        active[f.node]=false;stack.pop_back();
        if (stack.empty()) { if (found) end=result;return found; }
        if (found) { stack.back().end=result;stack.back().found=true; }
    }
    return false;
}
void validatePart(const HitStaticPart& p) {
    require(p.model&&p.node<p.model->nodes.size(),"invalid registered static part");
    validateFrame(p.transform);
}
void validateRoom(const HitRoom& room) {
    validateBox(room.bounds);
    if(room.geometry) {
        for (const auto& p:room.geometry->meshes) validatePart(p);
        if (room.geometry->aabb) validatePart(*room.geometry->aabb);
    }
    for (const auto& t:room.triggers) require(t.valid,"invalid registered trigger");
}
bool staticPart(const HitStaticPart& p,Vec3 start,Vec3& end,SceneHitInfo& info,
                const SceneHitSettings& settings) {
    const auto& frame=p.transform;
    const auto offset=mul(frame.position,frame.scale);
    const auto inverse=engineInverse(frame.orientation);
    const auto localStart=engineRotate(inverse,sub(start,offset));
    auto localEnd=engineRotate(inverse,sub(end,offset));
    const int own=geometry(*p.model,p.node,frame.scale,localStart,localEnd,info,settings,
        p.animmeshPositions ? &*p.animmeshPositions : nullptr);
    if (!own||(own==-1&&info.ignoreBoxStartInside)) return false;
    end=add(engineRotate(frame.orientation,localEnd),offset);
    info.position=end;info.normal=engineRotate(frame.orientation,info.normal);
    info.part=p.model->nodes[p.node].name;
    return true;
}
std::vector<std::size_t> rootsOf(const Model& model, bool allowForest = false) {
    std::vector<std::size_t> roots;
    for (std::size_t i=0;i<model.nodes.size();++i)
        if (!model.nodes[i].parent && (model.sceneNodeAdmission.empty() ||
            (i < model.sceneNodeAdmission.size() && model.sceneNodeAdmission[i]))) roots.push_back(i);
    if (allowForest && model.collapsedRoomParts && model.animationSynchronizationModel) {
        // CollapsePartTree appends breadth-first. The immutable source retains
        // that order even though the surviving ownerless Parts are now a forest.
        const auto& source=*model.animationSynchronizationModel;
        require(source.nodes.size()==model.nodes.size(),"stale collapsed source");
        std::vector<std::size_t> queue;
        for(std::size_t i=0;i<source.nodes.size();++i)if(!source.nodes[i].parent)queue.push_back(i);
        std::vector<bool> seen(source.nodes.size());std::vector<std::size_t> ordered;
        for(std::size_t at=0;at<queue.size();++at) {
            const auto i=queue[at];require(i<source.nodes.size()&&!seen[i],"invalid source Part tree");seen[i]=true;
            if(std::find(roots.begin(),roots.end(),i)!=roots.end())ordered.push_back(i);
            queue.insert(queue.end(),source.nodes[i].children.begin(),source.nodes[i].children.end());
        }
        require(ordered.size()==roots.size(),"unreachable collapsed Part");roots=std::move(ordered);
    }
    require(allowForest || roots.size()<=1,"engine Gob requires one root part");
    return roots;
}
void validateOwner(const HitOwner& value) {
    require(value.model&&value.parts.size()==value.model->nodes.size(),"invalid owner registration");
    validateFrame({value.position,value.orientation,value.scale});
    require(finite(value.sphere.center)&&std::isfinite(value.sphere.radius)&&
        std::isfinite(value.alpha)&&std::isfinite(value.doorAlpha),"invalid owner query state");
    for (const auto& frame:value.parts) validateFrame(frame);
    rootsOf(*value.model);
}
void validateRooms(const std::vector<HitRoom>& rooms) {
    std::unordered_set<HitRoomId> ids;
    for (const auto& room:rooms) {validateRoom(room);require(ids.insert(room.id).second,"duplicate room ID");}
}
} // namespace

void SceneHitInfo::clearOutput() noexcept {
    type=Type::None;position={};localPosition={};surface=0;owner.reset();face=-1;
    normal={};part.clear();triggerOwner=0;room.reset();
}
void SceneHitInfo::reset() { *this=SceneHitInfo{}; }

int engineHitBox(model::Vec3 start,model::Vec3 end,model::Bounds b,
                 model::Vec3* output) noexcept {
    const float a[3]{start.x,start.y,start.z},z[3]{end.x,end.y,end.z};
    const float lo[3]{b.minimum.x,b.minimum.y,b.minimum.z};
    const float hi[3]{b.maximum.x,b.maximum.y,b.maximum.z};
    unsigned from=0,to=0;
    for (int i=0;i<3;++i) {
        const unsigned low=1u<<static_cast<unsigned>(5-2*i),high=low>>1;
        if (a[i]<lo[i]) from|=low;else if (a[i]>hi[i]) from|=high;
        if (z[i]<lo[i]) to|=low;else if (z[i]>hi[i]) to|=high;
    }
    if (!from) return -1;
    if (from&to) return 0;
    for (int axis=0;axis<3;++axis) {
        const unsigned low=1u<<static_cast<unsigned>(5-2*axis),high=low>>1;
        if (!(from&(low|high))) continue;
        const bool lower=(from&low)!=0;
        const float bound=lower?lo[axis]:hi[axis];
        const float delta=z[axis]-a[axis];
        if (delta==0.0f) continue;
        const float t=(bound-a[axis])/delta;
        if (t<0.0f||t>1.0f) continue;
        float p[3]{};bool inside=true;
        for (int j=0;j<3;++j) {
            p[j]=j==axis?bound:a[j]*(1.0f-t)+z[j]*t;
            if (j!=axis&&(p[j]<lo[j]||p[j]>hi[j])) inside=false;
        }
        if (inside) {
            if (output) *output={p[0],p[1],p[2]};
            return axis*2+(lower?1:2);
        }
    }
    return 0;
}
bool engineHitTriangle(const std::array<model::Vec3,3>& v,
                       model::Vec3 start,model::Vec3& end) {
    // Animmesh, triggers and WOK triangles use the same derived-plane
    // polyhit overload: K1 0x3a67b8 / K2 Desktop 0x315237.
    const auto hit=model::hitTestTriggerTriangle(v,start,end);
    if(!hit)return false;
    end=hit->position;return true;
}
bool engineHitSphere(model::Vec3 start,model::Vec3 end,HitSphere sphere) noexcept {
    const auto d=sub(end,start);const float lengthSquared=dot(d,d);
    if (static_cast<double>(lengthSquared)<1.0e-9) return false;
    float t=dot(sub(sphere.center,start),d)/lengthSquared;
    t=std::max(0.0f,std::min(1.0f,t));
    const auto p=add(start,mul(d,t));
    return sphere.radius>magnitude(sub(sphere.center,p)); // tangent is a miss
}
model::Vec3 engineNormalize(model::Vec3 value) noexcept {
    // Vector::Normalize K1 Desktop 0x3a5b92; K2 Desktop 0x313640.
    const float length=std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
    if(double(length)<1.0e-9)return {1,0,0};
    return mul(value,1.0f/length);
}
model::Quaternion engineRotationArc(model::Vec3 from,model::Vec3 to) noexcept {
    // RotationArc K1 Desktop 0x3a5c0e / K2 Desktop 0x314e16.
    from=engineNormalize(from);to=engineNormalize(to);
    const float cosine=dot(from,to);
    if(cosine<=-1.0f)return {1,0,0,0};
    const auto axis=cross(from,to);
    const float s=std::sqrt((cosine+1.0f)*2.0f);
    return {axis.x/s,axis.y/s,axis.z/s,s*0.5f};
}
model::Quaternion engineInverse(model::Quaternion q) noexcept {
    return model::engineQuaternionInverse(q);
}
model::Vec3 engineRotate(model::Quaternion q,model::Vec3 v) noexcept {
    return model::engineQuaternionRotate(q,v);
}
HitPartTransform engineRigidFrame(const model::Mat4& m) {
    for (const auto v:m.values) require(std::isfinite(v),"non-finite owner frame");
    const auto& a=m.values;
    Vec3 x{a[0],a[1],a[2]},y{a[4],a[5],a[6]},z{a[8],a[9],a[10]};
    constexpr float tolerance=0.0002f;
    require(std::abs(dot(x,x)-1)<tolerance&&std::abs(dot(y,y)-1)<tolerance&&
        std::abs(dot(z,z)-1)<tolerance&&std::abs(dot(x,y))<tolerance&&
        std::abs(dot(x,z))<tolerance&&std::abs(dot(y,z))<tolerance&&
        dot(cross(x,y),z)>1-tolerance&&std::abs(a[3])<tolerance&&
        std::abs(a[7])<tolerance&&std::abs(a[11])<tolerance&&
        std::abs(a[15]-1)<tolerance,"owner frame must be rigid (scale is separate)");
    Quaternion q;const float trace=a[0]+a[5]+a[10];
    if (trace>0) {
        const float s=std::sqrt(trace+1.0f)*2.0f;
        q={ (a[6]-a[9])/s,(a[8]-a[2])/s,(a[1]-a[4])/s,0.25f*s };
    } else if (a[0]>a[5]&&a[0]>a[10]) {
        const float s=std::sqrt(1+a[0]-a[5]-a[10])*2;
        q={0.25f*s,(a[4]+a[1])/s,(a[8]+a[2])/s,(a[6]-a[9])/s};
    } else if (a[5]>a[10]) {
        const float s=std::sqrt(1+a[5]-a[0]-a[10])*2;
        q={(a[4]+a[1])/s,0.25f*s,(a[9]+a[6])/s,(a[8]-a[2])/s};
    } else {
        const float s=std::sqrt(1+a[10]-a[0]-a[5])*2;
        q={(a[8]+a[2])/s,(a[9]+a[6])/s,0.25f*s,(a[1]-a[4])/s};
    }
    return {{a[12],a[13],a[14]},q,1.0f};
}
// Part::GetWorld (K1 Desktop 0x32a22c, K2 Desktop 0xa820e) walks from the
// selected part to its parents. Part scales affect the accumulated position;
// the containing Gob's effective scale follows, without changing orientation.
HitPartTransform enginePartWorld(const model::Model& model,const model::Pose& pose,
    const model::Mat4& ownerFrame,float ownerScale,std::size_t node) {
    auto owner=engineRigidFrame(ownerFrame);
    owner.scale=ownerScale;
    return model::enginePartWorld(model,pose,owner,node);
}
std::vector<HitPartTransform> engineHitPose(const model::Model& model,
                                           const model::Pose* pose) {
    std::vector<HitPartTransform> frames;frames.reserve(model.nodes.size());
    if (pose) require(pose->positions.size()==model.nodes.size()&&
        pose->orientations.size()==model.nodes.size()&&pose->scales.size()==model.nodes.size(),
        "incomplete pose");
    for (std::size_t i=0;i<model.nodes.size();++i) {
        const auto& n=model.nodes[i];
        frames.push_back(pose?HitPartTransform{pose->positions[i],pose->orientations[i],pose->scales[i]}
                             :HitPartTransform{n.position,n.orientation,1});
        validateFrame(frames.back());
    }
    return frames;
}
std::vector<HitPartTransform> engineCollapsedHitPose(const model::Model& model,
    const model::Pose& pose,const model::Mat4& ownerFrame) {
    return engineCollapsedHitPose(model,pose,engineRigidFrame(ownerFrame));
}
std::vector<HitPartTransform> engineCollapsedHitPose(const model::Model& model,
    const model::Pose& pose,const model::EngineFrame& world) {
    auto result=engineHitPose(model,&pose);
    struct Item {std::size_t node;HitPartTransform parent;};
    std::vector<Item> queue;
    for(auto root:rootsOf(model,model.collapsedRoomParts))queue.push_back({root,world});
    std::vector<bool> seen(model.nodes.size());
    for(std::size_t i=0;i<queue.size();++i) {
        const auto item=queue[i];
        require(item.node<model.nodes.size()&&!seen[item.node],"invalid collapsed room tree");
        seen[item.node]=true;auto& frame=result[item.node];
        frame.position=add(engineRotate(item.parent.orientation,frame.position),item.parent.position);
        frame.orientation=model::engineQuaternionProduct(item.parent.orientation,frame.orientation);
        validateFrame(frame); // no matrix decomposition or quaternion repair
        for(auto child:model.nodes[item.node].children)queue.push_back({child,frame});
    }
    return result;
}
static HitSphere minimumHitSphere(const model::Model& model,
    const std::vector<HitPartTransform>& parts,float ownerScale,
    std::optional<float> constructedEmitterRadius) {
    require(parts.size()==model.nodes.size()&&std::isfinite(ownerScale),"invalid minimum-sphere inputs");
    const auto roots=rootsOf(model);if (roots.empty()) return {};
    struct Visit {std::size_t n;bool exit;};
    std::vector<Visit> stack{{roots.front(),false}};
    std::vector<std::uint8_t> visited(model.nodes.size());
    std::vector<HitSphere> spheres(model.nodes.size(),HitSphere{{},0});
    std::vector<bool> containsMesh(model.nodes.size());
    while (!stack.empty()) {
        const auto item=stack.back();stack.pop_back();
        require(item.n<model.nodes.size(),"invalid minimum-sphere child");
        const auto& n=model.nodes[item.n];
        if (!item.exit) {
            require(!visited[item.n],"cyclic/shared minimum-sphere part");
            visited[item.n]=1;validateFrame(parts[item.n]);
            stack.push_back({item.n,true});
            for (auto i=n.children.rbegin();i!=n.children.rend();++i) stack.push_back({*i,false});
            continue;
        }
        HitSphere result{{},0};
        // TreeSetGeometryFlags (K1 Desktop 0x329166, K2 Desktop 0xaf480) records
        // AsPartTriMesh in this subtree. Part::GetMinimumSphere traverses
        // children only when that flag is set; alwaysdominspheres defaults to
        // zero (K1 0x48c908, K2 0x5e9734).
        // This is independent of the mesh render flag.
        containsMesh[item.n]=n.mesh.has_value();
        for(const auto child:n.children)
            containsMesh[item.n]=containsMesh[item.n]||containsMesh[child];


        if (n.emitter&&(model.classification&1u)) {
            if(!constructedEmitterRadius)
                throw ModelError("Engine minimum sphere: classification-one emitter bounds need runtime input");
            result.radius=*constructedEmitterRadius;
        }
        if(containsMesh[item.n])
        for (const auto child:n.children) {
            const auto& p=parts.at(child);auto next=spheres.at(child);
            next.center=add(engineRotate(p.orientation,next.center),p.position);
            const float d=magnitude(sub(result.center,next.center));
            const float big=std::max(result.radius,next.radius),small=std::min(result.radius,next.radius);
            if (big>=d+small||d==0.0f) {
                if (next.radius>result.radius) result.center=next.center;
                result.radius=big;
            } else {
                const float r=result.radius/d;
                const float f=((1.0f-next.radius/d)+r)*0.5f;
                result.center=add(next.center,mul(sub(result.center,next.center),f));
                result.radius=((r+1.0f)-f)*d;
            }
        }
        if (n.mesh) {
            require(*n.mesh<model.meshes.size(),"minimum-sphere mesh outside array");
            const auto& mesh=model.meshes[*n.mesh];
            require(mesh.engineBounds.has_value(),"missing authored minimum sphere");
            const auto& authored=*mesh.engineBounds;
            require(finite(authored.sphereCenter)&&std::isfinite(authored.sphereRadius),
                    "non-finite authored sphere");
            if (result.radius<=authored.sphereRadius) result.center=authored.sphereCenter;
            const float d=magnitude(sub(result.center,authored.sphereCenter));
            if (d==0.0f||result.radius>=d+authored.sphereRadius)
                result.radius=std::max(result.radius,authored.sphereRadius);
            else {
                const float t=((d-result.radius)+authored.sphereRadius)*0.5f;
                result.center=add(result.center,mul(sub(authored.sphereCenter,result.center),t/d));
                result.radius=(result.radius+d)-t;
            }
        }
        spheres[item.n]=result;visited[item.n]=2;
    }
    auto sphere=spheres[roots.front()];
    sphere.center=mul(add(sphere.center,parts[roots.front()].position),ownerScale);
    sphere.radius=(sphere.radius*ownerScale)*1.35f;
    return sphere;
}
HitSphere engineMinimumHitSphere(const model::Model& model,
    const std::vector<HitPartTransform>& parts,float ownerScale) {
    return minimumHitSphere(model,parts,ownerScale,{});
}
HitSphere engineConstructedOwnerSphere(const model::Model& model) {










    auto parts=engineHitPose(model,nullptr);
    for(std::size_t i=0;i<model.nodes.size();++i)
        model::constructedNodeFrame(model.nodes[i],parts[i].position,parts[i].orientation,
                                    model.game);
    return minimumHitSphere(model,parts,1.0f,1.0f);
}

float engineOwnerCameraDistance(HitSphere sphere,model::Vec3 ownerPosition,
    model::Quaternion ownerOrientation,model::Vec3 cameraPosition) noexcept {
    // K1 Desktop ProcessParts 0x35fd2e..0x35fdc2. The cached center already
    // contains the scale used by GetMinimumSphere; no current scale is added.
    return magnitude(sub(cameraPosition,
        add(engineRotate(ownerOrientation,sphere.center),ownerPosition)));
}

model::Bounds engineStaticHitBounds(const model::Mesh& mesh,const HitPartTransform& frame) {
    validateFrame(frame);
    Bounds bounds{{100000,100000,100000},{-100000,-100000,-100000},true};
    // ComputePartBBox reads the signed mesh-header count (K1 0x5d2960;
    // K2 0x7babb6). A referenced MDX sentinel is drawable storage, not an
    // additional vertex in this bounds loop.
    const auto count=static_cast<std::int16_t>(mesh.enginePrelightVertexCount);
    const auto rounded=[](float x) { return 0.001f*std::floor(x/0.001f+0.5f); };
    for (std::int32_t i=0;i<count;++i) {
        const auto p=add(engineRotate(frame.orientation,mesh.vertices[static_cast<std::size_t>(i)].position),frame.position);
        require(finite(p),"non-finite static drawing position");
        const Vec3 v{rounded(p.x),rounded(p.y),rounded(p.z)};
        bounds.minimum={std::min(bounds.minimum.x,v.x),std::min(bounds.minimum.y,v.y),std::min(bounds.minimum.z,v.z)};
        bounds.maximum={std::max(bounds.maximum.x,v.x),std::max(bounds.maximum.y,v.y),std::max(bounds.maximum.z,v.z)};
    }
    return bounds;
}

void SceneQueries::clear() noexcept {
    owners_.clear();detachedOwners_.clear();hostOwners_.clear();rooms_.clear();bsp_.clear();root_.reset();++revision_;
}
AnimMeshHitPools engineAnimMeshHitPools(const model::Model& model,const model::Pose& pose) {
    AnimMeshHitPools result(model.meshes.size());
    for(std::size_t i=0;i<model.meshes.size();++i) {
        if(!model.meshes[i].animated)continue;
        require(i<pose.deformedMeshes.size() && pose.deformedMeshes[i].animmesh.has_value(),
                "missing live animmesh instance in hit snapshot");
        const auto& state=*pose.deformedMeshes[i].animmesh;
        if(state.positionsDefined)result[i]=state.localPositions;
    }
    return result;
}

std::vector<HitStaticPart> engineStaticHitParts(const Model& asset,const Pose& pose,
        const model::EngineFrame& frame,const neoshared::bif::JobControl& job) {
    std::vector<HitStaticPart> result;
    const auto collapsed=engineCollapsedHitPose(asset,pose,frame);
    const auto animated=engineAnimMeshHitPools(asset,pose);
    auto queue=rootsOf(asset,asset.collapsedRoomParts);std::vector<bool> seen(asset.nodes.size());
    // CollapsePartTree appends children to its growing list: breadth first.
    for(std::size_t i=0;i<queue.size();++i) {
        job.check();
        const auto index=queue[i];require(index<asset.nodes.size()&&!seen[index],"invalid collapsed room tree");seen[index]=true;
        const auto& node=asset.nodes[index];queue.insert(queue.end(),node.children.begin(),node.children.end());
        if(!node.mesh)continue;
        require(*node.mesh<asset.meshes.size()&&index<pose.worldTransforms.size(),"invalid room mesh");
        HitStaticPart part;part.model=&asset;part.node=index;
        part.transform=collapsed[index];
        if(*node.mesh<animated.size())part.animmeshPositions=animated[*node.mesh];
        part.worldBounds=engineStaticHitBounds(asset.meshes[*node.mesh],part.transform);
        result.push_back(std::move(part));
    }
    return result;
}

void SceneQueries::synchronize(const ModelDocument& document,const model::Pose& pose,
        const ReferenceScene& references,const AreaScene& area,const TriggerScene& triggers,
        const ParticleScene* inspectionParticles) {
    struct PendingOwner { HitOwner value; bool registered; };
    std::vector<PendingOwner> pending;
    const auto append=[&](HitOwnerId id,const Model& asset,const Pose& current,const model::EngineFrame& frame,
                          bool registered=true) {
        HitOwner value;
        const auto* old=owner(id);
        if(old) {
            require(std::find(hostOwners_.begin(),hostOwners_.end(),id)!=hostOwners_.end(),
                    "host owner identity collides with external registration");
            if(old->model==&asset) {
                value.sphere=old->sphere;value.alpha=old->alpha;value.doorAlpha=old->doorAlpha;
                value.ignoreHitCheck=old->ignoreHitCheck;value.runtimeExclusionFlag=old->runtimeExclusionFlag;
            }
        }
        value.id=id;value.model=&asset;value.parts=engineHitPose(asset,&current);
        value.animmeshPositions=engineAnimMeshHitPools(asset,current);
        value.position=frame.position;value.orientation=frame.orientation;


        value.scale=frame.scale;
        if(!old||old->model!=&asset) {
            value.sphere=engineConstructedOwnerSphere(asset);
        }
        pending.push_back({std::move(value),registered});
    };
    if(document.hasModelOwner())append(0,document.model(),pose,references.mainFrame());
    for(std::size_t i=0;i<references.instances().size();++i) {
        const auto& instance=references.instances()[i];
        require(i<document.references().size(),"stale reference identity");
        const auto& definition=document.references()[i];
        append(i+1u,definition.model,instance.pose,instance.frame,
               instance.active&&instance.sceneMember);
    }
    for(const auto& object:area.objects()) {
        const auto& document=*object->document();
        const auto visualInputs=[&](std::optional<std::size_t> reference={}) {
            const auto& effects=object->engineOwnerVisualInputs(reference);
            // These are retained Gob fields, also read when color shifting is
            // disabled. The independent door-alpha field is not overwritten.
            pending.back().value.alpha=effects.colorShift[3];
            pending.back().value.ignoreHitCheck=effects.distortion;
        };
        if(!object->staticPrimary()) {
            append(object->ownerId(),document.model(),object->pose(),object->frame());
            visualInputs();
        }
        for(std::size_t i=0;i<object->references().instances().size();++i) {
            const auto& ref=object->references().instances()[i];
            const auto& definition=document.references()[i];
            append(object->ownerId(i),definition.model,ref.pose,ref.frame,
                   ref.active&&ref.sceneMember);
            visualInputs(i);
        }
    }
    const auto registeredChunks=collectParticleChunkOwners(inspectionParticles,area);
    const auto appendChunk=[&](const SceneOwnerView& chunk,bool registered) {
        const auto* old=owner(chunk.id);
        const bool constructed=!old||old->model!=chunk.asset;
        append(chunk.id,*chunk.asset,*chunk.pose,chunk.frame,registered);
        // SetObjectScale on pool reuse changes the hit transform, not the
        // constructed Gob +0x164 radius retained by ChunkyParticle.
        if(constructed)pending.back().value.sphere.radius=chunk.lightSphere.radius;
    };
    std::unordered_set<HitOwnerId> registeredChunkIds;
    for(const auto& chunk:registeredChunks) {
        registeredChunkIds.insert(chunk.id);appendChunk(chunk,true);
    }
    for(const auto& chunk:collectParticleChunkOwners(inspectionParticles,area,true,false))
        if(registeredChunkIds.find(chunk.id)==registeredChunkIds.end())appendChunk(chunk,false);
    std::vector<HitRoom> rooms;
    for(const auto& source:area.rooms()) {
        HitRoom room;room.id=source.id;room.bounds=source.bounds;
        room.geometry=source.hitGeometry;
        for(const auto& volume:triggers.volumes()) {
            const Vec3 center{(volume.bounds.minimum.x+volume.bounds.maximum.x)*0.5f,
                (volume.bounds.minimum.y+volume.bounds.maximum.y)*0.5f,
                (volume.bounds.minimum.z+volume.bounds.maximum.z)*0.5f};
            if(center.x>=room.bounds.minimum.x&&center.x<=room.bounds.maximum.x&&
               center.y>=room.bounds.minimum.y&&center.y<=room.bounds.maximum.y)
                room.triggers.push_back(volume); // no light-style all-room fallback
        }
        rooms.push_back(std::move(room));
    }
    // SetScene(null) unlists a Gob without destroying its sphere or hit flags.
    // Keep allocated detached references/chunks separately; only an absent
    // allocation is destroyed. Explicit external owners remain untouched.
    for(const auto& item:pending)validateOwner(item.value);
    validateRooms(rooms);
    for(const auto id:hostOwners_)if(std::none_of(pending.begin(),pending.end(),
        [id](const auto& v){return v.value.id==id;}))removeOwner(id);
    hostOwners_.clear();
    for(auto& item:pending) {
        hostOwners_.push_back(item.value.id);
        storeOwnerRegistration(std::move(item.value),item.registered);
    }
    rooms_=std::move(rooms);++revision_;
}

void SceneQueries::registerOwner(HitOwner value) {
    validateOwner(value);
    storeOwnerRegistration(std::move(value),true);
}
void SceneQueries::storeOwnerRegistration(HitOwner value,bool registered) {
    auto& target=registered?owners_:detachedOwners_;
    auto& previousList=registered?detachedOwners_:owners_;
    previousList.erase(std::remove_if(previousList.begin(),previousList.end(),
        [&](const HitOwner& old){return old.id==value.id;}),previousList.end());
    const auto previous=std::find_if(target.begin(),target.end(),
        [&](const HitOwner& old){return old.id==value.id;});
    if(previous!=target.end())*previous=std::move(value);
    else target.push_back(std::move(value));
    ++revision_;
}
bool SceneQueries::removeOwner(HitOwnerId id) noexcept {
    const auto old=owners_.size()+detachedOwners_.size();
    owners_.erase(std::remove_if(owners_.begin(),owners_.end(),
        [id](const HitOwner& value){return value.id==id;}),owners_.end());
    detachedOwners_.erase(std::remove_if(detachedOwners_.begin(),detachedOwners_.end(),
        [id](const HitOwner& value){return value.id==id;}),detachedOwners_.end());
    if (old==owners_.size()+detachedOwners_.size()) return false;
    ++revision_;return true;
}
const HitOwner* SceneQueries::owner(HitOwnerId id) const noexcept {
    const auto it=std::find_if(owners_.begin(),owners_.end(),[id](const HitOwner& o){return o.id==id;});
    if(it!=owners_.end())return &*it;
    const auto detached=std::find_if(detachedOwners_.begin(),detachedOwners_.end(),
        [id](const HitOwner& value){return value.id==id;});
    return detached==detachedOwners_.end()?nullptr:&*detached;
}
HitOwner* SceneQueries::owner(HitOwnerId id) noexcept {
    return const_cast<HitOwner*>(static_cast<const SceneQueries*>(this)->owner(id));
}
void SceneQueries::setRooms(std::vector<HitRoom> rooms) {
    validateRooms(rooms);
    rooms_=std::move(rooms);++revision_;
}
void SceneQueries::setBsp(std::vector<HitBspNode> nodes,std::optional<std::size_t> root) {
    if (root) require(*root<nodes.size(),"invalid BSP root");
    for (const auto& node:nodes) {
        if (node.contents) validateRoom(*node.contents);
        require(finite(node.normal)&&std::isfinite(node.distance),"invalid BSP plane");
        if (node.negative) require(*node.negative<nodes.size(),"invalid BSP child");
        if (node.positive) require(*node.positive<nodes.size(),"invalid BSP child");
    }
    // Validate the complete reachable graph before committing it.
    if (root) {
        std::vector<std::size_t> pending{*root};std::vector<bool> seen(nodes.size());
        while (!pending.empty()) {
            const auto i=pending.back();pending.pop_back();
            require(!seen[i],"cyclic/shared BSP node");seen[i]=true;
            if (!nodes[i].leaf) {
                if(nodes[i].positive) pending.push_back(*nodes[i].positive);
                if(nodes[i].negative) pending.push_back(*nodes[i].negative);
            }
        }
    }
    bsp_=std::move(nodes);root_=root;++revision_;
}
bool SceneQueries::hitContents(const HitRoom& room,Vec3 start,Vec3& end,
                               SceneHitInfo& info,bool leaf,bool staticAabb) const {
    bool found=false;
    if (info.surfaceMask&0x40000000u) for (const auto& trigger:room.triggers) {
        if (!engineHitBox(start,end,trigger.bounds)) continue;
        if (const auto hit=hitTestK2Trigger(trigger,start,end)) {
            // PartTrigger writes only these outputs, not ClearOutput.
            end=hit->position;info.position=end;info.normal=hit->normal;
            info.surface=0x40000000u;info.triggerOwner=hit->owner;found=true;
        }
    }
    const auto* geometry=room.geometry.get();
    if (settings_.staticTriMeshes&&geometry) for (const auto& mesh:geometry->meshes) {
        validateBox(mesh.worldBounds);
        if (engineHitBox(start,end,mesh.worldBounds)&&staticPart(mesh,start,end,info,settings_)) found=true;
    }
    const auto* aabb=geometry&&geometry->aabb?&*geometry->aabb:nullptr;
    if (staticAabb&&aabb)
        if (staticPart(*aabb,start,end,info,settings_)) found=true;
    if (staticAabb&&!aabb&&settings_.floorFallback&&leaf&&start.z>0&&end.z<0) {
        // Engine optional debug floor changes ONLY the endpoint. In particular
        // it supplies no invented unit normal, surface, face, or ClearOutput.
        const auto delta=sub(end,start);
        end=add(start,mul(delta,-start.z/delta.z));found=true;
    }


    return found;
}
bool SceneQueries::hitOwner(HitOwnerId id,Vec3 start,Vec3 end,SceneHitInfo& info,
                            std::optional<std::size_t> root) const {
    require(finite(start)&&finite(end),"non-finite segment");
    const auto* value=owner(id);if (!value||!value->model) return false;
    const auto& model=*value->model;
    if (model.classification&0x100u) return false;
    if (info.excludeRuntimeFlag&&value->runtimeExclusionFlag) return false;
    if (info.checkAlpha) {
        const float alpha=(model.classification&8u)?value->doorAlpha:value->alpha;
        if (!(alpha>settings_.minimumOwnerAlpha)) return false;
    }
    const auto inverse=engineInverse(value->orientation);
    const auto a=engineRotate(inverse,sub(start,value->position));
    auto b=engineRotate(inverse,sub(end,value->position));
    if (!engineHitSphere(a,b,value->sphere)) return false;



    const float initialScale=root?1.0f:value->scale;
    if (!root) {
        const auto roots=rootsOf(model);if (roots.empty()) return false;root=roots.front();
    }
    if (!partTree(model,value->parts,*root,initialScale,a,b,info,settings_,value->animmeshPositions)) return false;
    info.position=add(engineRotate(value->orientation,b),value->position);
    info.normal=engineRotate(value->orientation,info.normal);
    return true;
}
bool SceneQueries::hit(Vec3 start,Vec3 end,SceneHitInfo& info) const {
    return hitVisual(start,end,info,settings_.staticAabb);
}
bool SceneQueries::hitVisual(Vec3 start,Vec3 end,SceneHitInfo& info,bool staticAabb) const {
    require(finite(start)&&finite(end),"non-finite segment");
    require(finite(settings_.boxExpansion)&&std::isfinite(settings_.minimumOwnerAlpha),"invalid query globals");
    std::optional<HitOwnerId> lastOwner;
    if (!info.skipStatic) {
        if (!rooms_.empty()) {
            for (const auto& room:rooms_) {
                if (!engineHitBox(start,end,room.bounds)) continue;
                if (hitContents(room,start,end,info,true,staticAabb)) {
                    info.type=SceneHitInfo::Type::Static;info.position=end;info.room=room.id;
                }
            }
        } else if (root_) {
            // BSPHitCheck visits the node callback before its children, then
            // near side first. A near-child hit skips the far child completely.
            struct Frame {
                std::size_t index{};Vec3 from{},to{},split{};bool own{},child{};
                int stage{};std::optional<std::size_t> first,second;bool crossing{};
            };
            std::vector<Frame> stack{{*root_,start,end,{},{},{},0,{},{}}};
            bool returned=false;Vec3 result=end;
            while (!stack.empty()) {
                auto& f=stack.back();const auto& n=bsp_[f.index];
                if (f.stage==0) {
                    f.own=n.contents&&hitContents(*n.contents,f.from,f.to,info,n.leaf,staticAabb);
                    if (n.leaf) f.stage=3;
                    else {
                        const float a=dot(n.normal,f.from)+n.distance;
                        const float b=dot(n.normal,f.to)+n.distance;
                        if (a<=0&&b<=0) f.first=n.negative;
                        else if (a>0&&b>0) f.first=n.positive;
                        else {
                            const auto delta=sub(f.to,f.from);const float den=dot(n.normal,delta);
                            const float t=den!=0.0f?-a/den:0.0f;
                            f.split=add(f.from,mul(delta,t));f.crossing=true;
                            f.first=a>0?n.positive:n.negative;
                            f.second=a>0?n.negative:n.positive;
                        }
                        f.stage=1;
                        if (f.first) {
                            const auto child=*f.first;const auto from=f.from,to=f.crossing?f.split:f.to;
                            stack.push_back({child,from,to,{},{},{},0,{},{}});continue;
                        }
                        returned=false;
                    }
                }
                if (f.stage==1) {
                    if (returned) { f.child=true;f.to=result;f.stage=3; }
                    else if (f.second) {
                        f.stage=2;const auto child=*f.second;const auto from=f.split,to=f.to;
                        stack.push_back({child,from,to,{},{},{},0,{},{}});continue;
                    } else f.stage=3;
                } else if (f.stage==2) {if(returned){f.child=true;f.to=result;}f.stage=3;}
                if (f.stage==3) {
                    returned=f.own||f.child;result=f.to;stack.pop_back();
                }
            }
            if (returned) {end=result;info.position=end;info.type=SceneHitInfo::Type::Static;}
        }
    }
    if (!info.staticOnly) for (const auto& value:owners_) {
        const auto classification=value.model->classification;
        if (!info.includeClassificationOne&&(classification&1u)) continue;
        if (value.ignoreHitCheck||(classification&0x82u)) continue;
        if (info.classificationFilter&&((classification&0x14u)||((classification&0x420u)==0x20u))) continue;
        if (std::find(info.ignoredOwners.begin(),info.ignoredOwners.end(),value.id)!=info.ignoredOwners.end()) continue;
        if (!hitOwner(value.id,start,end,info)) continue;
        end=info.position;lastOwner=value.id;info.owner=value.id;info.type=SceneHitInfo::Type::Owner;
    }



    info.owner=lastOwner;
    return info.type!=SceneHitInfo::Type::None;
}
std::optional<SceneHitInfo> SceneQueries::hit(Vec3 start,Vec3 end) const {
    SceneHitInfo info;if (!hit(start,end,info)) return {};
    return info;
}
SceneHitInfo::Type SceneQueries::gameHit(const AreaScene& area,Vec3 start,Vec3 end,
                                        SceneHitInfo& info) const {
    RoomSurfaceHitInfo roomInfo;
    bool roomHit=false;
    for(const auto& room:area.rooms()) {
        roomInfo.clearOutput();
        if(room.walkmesh&&engineRoomHitAll(*room.walkmesh,start,end,roomInfo)) {
            end=roomInfo.position;roomHit=true;
        }
    }
    // Engine temporarily changes hitcheck_static_bsp_parts and restores it.
    // A per-call input preserves that lifetime without mutating shared state.
    if(hitVisual(start,end,info,false))return info.type;
    return roomHit?SceneHitInfo::Type::Static:SceneHitInfo::Type::None;
}
bool SceneQueries::creatureCanSee(const AreaScene& area,const GameSightActor& observer,
    const GameSightActor* target,const GameSightLookup& lookup,SceneHitInfo& info,
    model::GameVersion game) const {
    if(!target)return false;
    auto start=observer.position;start.z+=1.5f;
    auto end=target->position;end.z+=target->height;
    info.clearOutput();info.classificationFilter=true;info.ignoredOwners.clear();
    const auto ignore=[&](std::optional<HitOwnerId> id) {
        // A engine null GetAurObject is appended too, but cannot match a Gob.
        if(id)info.ignoredOwners.push_back(*id);
    };
    ignore(observer.bodyOwner);ignore(observer.headOwner);
    ignore(target->bodyOwner);ignore(target->headOwner);
    for(;;) {
        info.clearOutput();
        const auto type=gameHit(area,start,end,info);
        if(type!=SceneHitInfo::Type::Owner)return type!=SceneHitInfo::Type::Static;
        const auto object=lookup(*info.owner);


        if(!object)return game==model::GameVersion::Kotor1;
        if(object->kind==GameSightObject::Kind::Door) {
            if(object->currentServerDoorOpenState&&*object->currentServerDoorOpenState==0)
                return false;
        } else if(object->kind!=GameSightObject::Kind::Placeable)return true;
        ignore(object->bodyOwner);
    }
}
} // namespace neomdl
