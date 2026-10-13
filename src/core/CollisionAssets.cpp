#include "core/CollisionAssets.hpp"
#include <model/AsciiModelInternal.hpp>
#include <neoshared/texture/FileUtil.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace neomdl {
namespace {
using Vector=neoshared::bwm::Vector;
constexpr CollisionKind kinds[]{CollisionKind::Placeable,CollisionKind::DoorClosed,
    CollisionKind::DoorOpen1,CollisionKind::DoorOpen2,CollisionKind::Room};
constexpr const char* hookNames[]{"use01","use02","dooruse01","dooruse02"};
model::Vec3 point(Vector v) {return {v[0],v[1],v[2]};}
Vector vector(model::Vec3 v) {return {v.x,v.y,v.z};}
}

const char* collisionLabel(CollisionKind kind) {
    constexpr const char* names[]{"Placeable PWK","Door closed DWK 0","Door open DWK 1","Door open DWK 2","Room WOK"};
    return names[static_cast<int>(kind)];
}

std::filesystem::path collisionPath(const std::filesystem::path& model,CollisionKind kind) {
    auto result=model;
    if(kind==CollisionKind::Room) return result.replace_extension(".wok");
    if(kind==CollisionKind::Placeable) return result.replace_extension(".pwk");
    result.replace_extension();result+="_dwk"+std::to_string(static_cast<int>(kind)-1)+".dwk";return result;
}

CollisionMesh readCollisionAscii(const std::string& text,CollisionKind kind) {
    CollisionMesh result;result.mesh.type=kind==CollisionKind::Room?1u:0u;
    auto& mesh=result.mesh;
    std::istringstream input(text);
    int hook=-1;
    for(std::string line;std::getline(input,line);) {
        if(const auto comment=line.find('#');comment!=std::string::npos) line.resize(comment);
        std::istringstream field(line);std::string key;field>>key;key=model::ascii::lower(key);
        if(key=="node") {
            field.exceptions(std::ios::failbit|std::ios::badbit);
            std::string type,name;field>>type>>name;hook=-1;name=model::ascii::lower(name);
            for(int i=0;i<4;++i) if(name==hookNames[i]) hook=i;
        } else if(key=="position") {
            field.exceptions(std::ios::failbit|std::ios::badbit);
            auto& value=hook<0?mesh.position:hook<2?mesh.usePoints[hook]:mesh.doorUsePoints[hook-2];
            field>>value[0]>>value[1]>>value[2];
        } else if(key=="walkablematerials") {
            result.walkableMaterials.clear();std::string material;
            while(field>>material) result.walkableMaterials.insert(model::ascii::parseUInt(material));
        } else if(key=="verts" || key=="faces" || key=="roomlinks") {
            field.exceptions(std::ios::failbit|std::ios::badbit);
            std::size_t count;field>>count;
            for(std::size_t i=0;i<count;++i) {
                std::getline(input,line);std::istringstream row(line);
                row.exceptions(std::ios::failbit|std::ios::badbit);
                if(key=="verts") {Vector v;row>>v[0]>>v[1]>>v[2];mesh.vertices.push_back(v);}
                else if(key=="faces") {
                    neoshared::bwm::Face face;std::uint32_t ignored;
                    row>>face.vertices[0]>>face.vertices[1]>>face.vertices[2];
                    for(int j=0;j<4;++j) row>>ignored;
                    row>>face.material;mesh.faces.push_back(face);
                } else {neoshared::bwm::Edge edge;row>>edge.index>>edge.transition;mesh.edges.push_back(edge);}
            }
        }
    }
    return result;
}

std::string writeCollisionAscii(const CollisionMesh& source,CollisionKind kind) {
    const auto& mesh=source.mesh;
    std::ostringstream text;text<<std::setprecision(9);
    const auto writeVector=[&](const Vector& value) {text<<value[0]<<' '<<value[1]<<' '<<value[2];};
    text<<"# NeoMDL "<<collisionLabel(kind)<<" ASCII\n"
        <<"# Vertices are stored coordinates; position is the BWM header, not an additional translation.\n";
    if(kind==CollisionKind::Room) {
        text<<"walkablematerials";
        for(auto material:source.walkableMaterials) text<<' '<<material;
        text<<'\n';
    }
    text<<"node trimesh WALKMESH\n  parent NULL\n  position ";writeVector(mesh.position);
    text<<"\n  verts "<<mesh.vertices.size()<<'\n';
    for(const auto& vertex:mesh.vertices) {text<<"    ";writeVector(vertex);text<<'\n';}
    text<<"  faces "<<mesh.faces.size()<<'\n';
    for(const auto& face:mesh.faces)
        text<<"    "<<face.vertices[0]<<' '<<face.vertices[1]<<' '<<face.vertices[2]<<" 1 0 0 0 "<<face.material<<'\n';
    if(kind==CollisionKind::Room) {
        const auto count=std::count_if(mesh.edges.begin(),mesh.edges.end(),[](const auto& edge){return edge.transition!=-1;});
        text<<"  roomlinks "<<count<<'\n';
        for(const auto& edge:mesh.edges) if(edge.transition!=-1) text<<"    "<<edge.index<<' '<<edge.transition<<'\n';
    }
    text<<"endnode\n";
    for(std::size_t i=0;i<4;++i) {
        text<<"node dummy "<<hookNames[i]<<"\n  parent WALKMESH\n  position ";
        writeVector(i<2?mesh.usePoints[i]:mesh.doorUsePoints[i-2]);text<<"\nendnode\n";
    }
    return text.str();
}

CollisionAsset readCollision(const neoshared::ResourceDocument& resource,CollisionKind kind,
                            const neoshared::bif::JobControl& job) {
    const std::string text(resource.bytes.begin(),resource.bytes.end());
    CollisionAsset result;result.inputs=resource.protectedInputs;
    if(text.compare(0,8,"BWM V1.0")!=0) {
        result.text=text;
        for(const auto& path:resource.protectedInputs)
            if(resource.identity==resourceFileIdentity(path)) result.asciiPath=path;
    }
    else {
        CollisionMesh source;source.mesh=neoshared::bwm::readBwm(resource.bytes,resource.fileName,job);
        if(kind==CollisionKind::Room) {
            source.walkableMaterials.clear();
            for(std::size_t i=0;i<source.mesh.adjacency.size();++i)
                source.walkableMaterials.insert(source.mesh.faces[i].material);
        }
        result.text=writeCollisionAscii(source,kind);
    }
    return result;
}

CollisionSet loadCollisions(const std::string& model,const ResourceResolver& resolver,
                           const neoshared::bif::JobControl& job) {
    CollisionSet result;
    if(resolver) for(const auto kind:kinds) {
        const auto path=collisionPath(std::filesystem::u8path(model+".mdl"),kind);
        const auto type=kind==CollisionKind::Room?0x7e0:kind==CollisionKind::Placeable?0x805:0x804;
        if(const auto resource=resolver(neoshared::texture::pathToUtf8(path.stem()),type))
            result.emplace(kind,readCollision(*resource,kind,job));
    }
    return result;
}

std::vector<CollisionOutput> collisionOutputs(const CollisionSet& assets,
    const std::filesystem::path& model,bool binary,const neoshared::bif::JobControl& job) {
    std::vector<CollisionOutput> result;
    for(const auto& [kind,asset]:assets) {
        CollisionOutput output;output.path=collisionPath(model,kind);
        if(binary) {
            auto source=readCollisionAscii(asset.text,kind);
            neoshared::bwm::buildBwm(source.mesh,source.walkableMaterials,job);
            output.bytes=neoshared::bwm::writeBwm(source.mesh,job);
        } else output.bytes.assign(asset.text.begin(),asset.text.end());
        result.push_back(std::move(output));
    }
    return result;
}

CollisionMesh collisionFromModel(const model::Model& asset,std::size_t nodeIndex,
    CollisionKind kind,model::Vec3 placement,const neoshared::bif::JobControl& job) {
    CollisionMesh result;result.mesh=model::collisionGeometry(asset.meshes[*asset.nodes[nodeIndex].mesh]);
    auto& mesh=result.mesh;mesh.type=kind==CollisionKind::Room?1u:0u;mesh.position=vector(placement);
    const auto pose=model::bindPose(asset,job);
    for(auto& vertex:mesh.vertices) {
        auto v=model::transformPoint(pose.worldTransforms[nodeIndex],point(vertex));
        vertex={v.x+placement.x,v.y+placement.y,v.z+placement.z};
    }
    const char* state=kind==CollisionKind::DoorClosed?"closed":kind==CollisionKind::DoorOpen1?"open1":"open2";
    for(std::size_t i=0;i<asset.nodes.size();++i) {
        const auto name=model::ascii::lower(asset.nodes[i].name);
        for(std::size_t hook=0;hook<2;++hook) {
            const auto number=std::to_string(hook+1);
            const bool match=kind==CollisionKind::Placeable?
                name.find("pwk_use0"+number)!=std::string::npos || name.find("pwk_dp_use_0"+number)!=std::string::npos:
                kind!=CollisionKind::Room && name.find(std::string("dwk_dp_")+state+"_0"+number)!=std::string::npos;
            if(match) {
                mesh.usePoints[hook]=vector(model::transformPoint(pose.worldTransforms[i],{}));
                if(kind!=CollisionKind::Placeable) mesh.doorUsePoints[hook]=mesh.usePoints[hook];
            }
        }
    }
    return result;
}

model::Vec3 collisionLocalPoint(const model::Mat4& world,model::Vec3 point,model::Vec3 placement) {
    const auto& m=world.values;
    const Vector a{m[0],m[1],m[2]},b{m[4],m[5],m[6]},c{m[8],m[9],m[10]};
    const Vector v{point.x-placement.x-m[12],point.y-placement.y-m[13],point.z-placement.z-m[14]};
    const auto cross=[](Vector x,Vector y) {return Vector{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};};
    const auto dot=[](Vector x,Vector y) {return x[0]*y[0]+x[1]*y[1]+x[2]*y[2];};
    const auto determinant=dot(a,cross(b,c));
    return {dot(v,cross(b,c))/determinant,dot(v,cross(c,a))/determinant,dot(v,cross(a,b))/determinant};
}

std::map<std::string,model::Vec3> collisionLayout(const std::string& text) {
    std::istringstream input(text);std::map<std::string,model::Vec3> result;
    for(std::string line;std::getline(input,line);) {
        std::istringstream header(line);std::string key;header>>key;
        if(model::ascii::equal(key,"roomcount")) {
            header.exceptions(std::ios::failbit|std::ios::badbit);
            std::size_t count;header>>count;
            for(std::size_t i=0;i<count;++i) {
                std::getline(input,line);std::istringstream row(line);std::string name;model::Vec3 position;
                row.exceptions(std::ios::failbit|std::ios::badbit);
                row>>name>>position.x>>position.y>>position.z;result.emplace(model::ascii::lower(name),position);
            }
        }
    }
    return result;
}

} // namespace neomdl
