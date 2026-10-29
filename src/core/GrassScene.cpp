






#include "core/GrassScene.hpp"
#include "core/SceneAnimation.hpp"
#include "core/SceneQueries.hpp"
#include <gff/GFFFile.hpp>
#include <twoda/TwoDAFile.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <set>
#include <tuple>

namespace neomdl {
namespace {
using model::Vec2;using model::Vec3;
void require(bool b,const std::string& s) {if(!b)throw std::runtime_error("Grass: "+s);}
bool finite(Vec3 v) {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(Vec3 a,float s){return {a.x*s,a.y*s,a.z*s};}
float length(Vec3 a){return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
void extend(model::Bounds& b,Vec3 p) {
    require(finite(p),"non-finite face or bin position");
    if(!b.valid){b={p,p,true};return;}
    b.minimum={std::min(b.minimum.x,p.x),std::min(b.minimum.y,p.y),std::min(b.minimum.z,p.z)};
    b.maximum={std::max(b.maximum.x,p.x),std::max(b.maximum.y,p.y),std::max(b.maximum.z,p.z)};
}
float roundEngine(float v){return .001f*std::floor(v/.001f+.5f);}
float random01(model::ParticleRandom& random){return float(random.next()%10000u)/10000.f;}
Vec3 read3(const std::vector<float>& p,std::size_t i){require(i+3<=p.size(),"truncated position/tail array");return {p[i],p[i+1],p[i+2]};}
void write3(std::vector<float>& p,std::size_t i,Vec3 v){require(finite(v)&&i+3<=p.size(),"non-finite/invalid grass position");p[i]=v.x;p[i+1]=v.y;p[i+2]=v.z;}
std::string lower(std::string v){for(auto& c:v)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return v;}
float fieldFloat(const neogff::GffStruct& root,const char* name) {
    if(!root.GetFieldByLabel(name))return 0; // engine ReadFieldFLOAT default
    const auto* p=dynamic_cast<const neogff::GffFloatField*>(root.GetFieldByLabel(name));
    require(p,"missing/non-FLOAT ARE field "+std::string(name));return p->value;
}
Vec3 fieldColor(const neogff::GffStruct& root,const char* name) {
    if(!root.GetFieldByLabel(name))return {};
    const auto* p=dynamic_cast<const neogff::GffUInt32Field*>(root.GetFieldByLabel(name));
    require(p,"missing/non-DWORD ARE color "+std::string(name));
    return {float(p->value&255u)/255.f,float((p->value>>8u)&255u)/255.f,float((p->value>>16u)&255u)/255.f};
}
}
void validateGrassSettings(const GrassSettings& s) {
    require(std::isfinite(s.binSize)&&s.binSize>0,"invalid bin size");
    require(std::isfinite(s.nearSquared)&&std::isfinite(s.farSquared)&&s.nearSquared>=0&&s.farSquared>s.nearSquared,"invalid squared draw radii");
    require(std::isfinite(s.stiffness)&&s.stiffness>=0&&std::isfinite(s.alphaCutoff)&&s.alphaCutoff>=0&&s.alphaCutoff<=1,"invalid wind/alpha input");
    require(!s.areaWind||*s.areaWind<=2,"unsupported ARE WindPower");
    for(const auto& m:s.surfaces)if(m) {
        require(!m->texture.empty()&&m->texture.size()<=32&&m->texture.find_first_of("/\\:")==std::string::npos,"invalid grass texture ResRef");
        require(std::isfinite(m->density)&&m->density>=0&&std::isfinite(m->quadSize)&&m->quadSize>0,"invalid density/quad size");
        require(finite(m->ambient)&&finite(m->diffuse)&&finite(m->emissive),"non-finite grass color");
        for(float p:m->probabilities)require(std::isfinite(p)&&p>=0,"invalid atlas probability");
    }
}
GrassSettings grassSettingsFromArea(const std::vector<std::uint8_t>& bytes,const std::vector<std::uint8_t>& tableBytes,
    const neoshared::bif::JobControl& job,model::GameVersion game) {
    ResourceLoadScope work(job);job.update(0,0,"Decoding grass area settings");
    neogff::GffFile file;file.LoadBytes(bytes);job.check();require(file.root()&&file.filetype()=="ARE ","expected a packed ARE resource");
    const auto& root=*file.root();GrassSettings s;s.game=game;GrassMaterial m;
    m.density=fieldFloat(root,"Grass_Density");m.quadSize=fieldFloat(root,"Grass_QuadSize");
    m.diffuse=fieldColor(root,"Grass_Diffuse");m.ambient=fieldColor(root,"Grass_Ambient");
    // K1 LoadAreaHeader 0x19e1c2..0x19e479 has no emissive field.
    if(game==model::GameVersion::Kotor2)m.emissive=fieldColor(root,"Grass_Emissive");
    const auto* texture=dynamic_cast<const neogff::GffResRefField*>(root.GetFieldByLabel("Grass_TexName"));
    require(texture||!root.GetFieldByLabel("Grass_TexName"),"non-RESREF ARE Grass_TexName");
    m.texture=texture?texture->GetString():std::string{};
    if(m.texture.empty())m.texture="grass"; // CSWSArea::LoadAreaHeader 0x68d35e
    const char* probability[]={"Grass_Prob_LL","Grass_Prob_LR","Grass_Prob_UL","Grass_Prob_UR"};
    for(unsigned i=0;i<4;++i)m.probabilities[i]=fieldFloat(root,probability[i]);
    s.areaWind=0;
    if(const auto* wind=root.GetFieldByLabel("WindPower")) {
        if(const auto* p=dynamic_cast<const neogff::GffIntField*>(wind))
            s.areaWind=static_cast<std::uint8_t>(p->value); // ReadFieldINT, then store AL
        else require(false,"WindPower must be INT");
    }
    job.update(0,0,"Decoding grass surface table");
    neo2da::TwoDAFile table;table.loadBytes(tableBytes);job.check();std::optional<std::size_t> column;
    for(std::size_t c=0;c<table.columnCount();++c)if(lower(table.columnLabel(c))=="grass"){column=c;break;}
    // LoadArea visits exactly 32 row indices. GetINTEntry (K1 0x39b9c0)
    // uses DEFAULT for absent rows/columns and the engine format's integer
    // conversion for present cells, including ****.
    for(std::size_t row=0;row<32;++row) {
        int value=0;
        if(!column||row>=table.rowCount())value=std::atoi(table.defaultValue().c_str());
        else {
            const auto& text=table.cell(row,*column);
            if(table.nativeFormatName()=="Binary 2DA V2.b")std::sscanf(text.c_str(),"%i",&value);
            else if(text.size()>=3&&text[0]=='0'&&(text[1]=='x'||text[1]=='X')) {
                unsigned parsed=0;std::sscanf(text.c_str(),"%x",&parsed);value=static_cast<int>(parsed);
            } else value=std::atoi(text.c_str());
        }
        if(value&&m.density>0&&!m.texture.empty())s.surfaces[row]=m;
    }
    s.enabled=m.density>0&&!m.texture.empty();validateGrassSettings(s);return s;
}
void GrassScene::clear() noexcept {bins_.clear();activeBins_.clear();settings_={};}
void GrassScene::rebuild(const std::vector<GrassRoomSource>& rooms,const GrassSettings& settings,
    const neoshared::bif::JobControl& job) {
    ResourceLoadScope work(job);job.update(0,rooms.size(),"Preparing grass rooms");
    validateGrassSettings(settings);GrassScene next;next.settings_=settings;
    for(auto& material:next.settings_.surfaces)if(material) {
        float sum=0.0f;for(const auto weight:material->probabilities)sum+=weight;
        if(sum!=1.0f)for(auto& weight:material->probabilities)weight/=sum;
    }
    std::size_t totalBlades=0;
    std::size_t roomIndex=0;
    for(const auto& room:rooms) {
        job.update(roomIndex++,rooms.size(),"Preparing grass room geometry");
        require(room.document&&room.document->isOpen()&&room.pose&&room.bounds.valid,"invalid static room source");
        const auto& asset=room.document->model();require(asset.game==settings.game,"grass room and area game differ");
        const auto frames=engineCollapsedHitPose(asset,*room.pose,room.world);
        const auto dim=[&](float min,float max) {
            const float n=std::ceil((max-min)/settings.binSize);
            require(std::isfinite(n)&&n>=0&&double(n)<std::ldexp(1.0,std::numeric_limits<std::size_t>::digits),
                    "room grid dimension exceeds index storage");
            return std::size_t(n);
        };
        const auto nx=dim(room.bounds.minimum.x,room.bounds.maximum.x),ny=dim(room.bounds.minimum.y,room.bounds.maximum.y);
        const auto firstBin=next.bins_.size();
        std::map<std::pair<std::size_t,std::size_t>,std::size_t> index;
        std::vector<GrassSourceFace> faceOrder;
        std::set<std::pair<std::size_t,std::size_t>> expected;
        for(std::size_t part=0;part<asset.nodes.size();++part) {
            job.check();
            const auto& node=asset.nodes[part];if(!node.aabb||!node.mesh)continue;
            require(*node.mesh<asset.meshes.size(),"invalid AABB mesh index");
            const auto& mesh=asset.meshes[*node.mesh];
            for(std::size_t face=0;face<mesh.faces.size();++face) {
                job.check();
                const auto surface=mesh.faces[face].surfaceMaterial;
                if(surface<32&&settings.surfaces[surface]) {faceOrder.push_back({part,face});expected.emplace(part,face);}
            }
        }
        if(room.faceAdmissionOrder) {
            require(room.faceAdmissionOrder->size()==expected.size(),"captured face admission count mismatch");
            for(const auto& f:*room.faceAdmissionOrder)require(expected.erase({f.part,f.face})==1,"stale/duplicate captured face admission");
            faceOrder=*room.faceAdmissionOrder;
        }
        for(const auto& admitted:faceOrder) {
            job.check();
            const auto part=admitted.part;
            const auto& node=asset.nodes[part];if(!node.aabb||!node.mesh)continue;
            require(*node.mesh<asset.meshes.size()&&part<frames.size(),"invalid AABB Part");
            const auto& mesh=asset.meshes[*node.mesh];const auto& frame=frames[part];
            {
                const auto& face=mesh.faces.at(admitted.face);
                require(mesh.cpuPositions.has_value(),"grass face has no authored MDL CPU vertex pool");
                GrassTriangle tri;tri.part=part;tri.face=admitted.face;tri.normal=face.normal;tri.lightmap=mesh.texture1;
                tri.lightmapped=settings.lightmaps&&mesh.lightmapped;tri.colored=mesh.hasVertexColors;
                require(finite(tri.normal),"non-finite face normal");Vec3 centroid{};
                for(unsigned v=0;v<3;++v) {
                    const auto vi=face.indices[v];require(vi>=0&&std::size_t(vi)<mesh.vertices.size()&&std::size_t(vi)<mesh.cpuPositions->size(),"invalid signed AABB face index");
                    const auto& source=mesh.vertices[std::size_t(vi)];
                    centroid=add(centroid,source.position);
                    tri.positions[v]=add(engineRotate(frame.orientation,(*mesh.cpuPositions)[std::size_t(vi)]),frame.position);
                    tri.lightmapUvs[v]=source.texcoord1;tri.colors[v]=source.color;
                    require(finite(tri.positions[v]),"non-finite transformed CPU face");
                    if(tri.lightmapped)require(mesh.mdxTexcoordOffsets[1]!=0xffffffffu&&std::isfinite(source.texcoord1.x)&&std::isfinite(source.texcoord1.y),"grass lightmap face lacks authored MDX UV1");
                }
                centroid=add(engineRotate(frame.orientation,mul(centroid,1.f/3.f)),frame.position);require(finite(centroid),"invalid MDX face centroid");
                centroid={roundEngine(centroid.x),roundEngine(centroid.y),roundEngine(centroid.z)};
                const auto coordinate=[&](float value,float min,std::size_t n) {
                    const float at=std::floor((value-min)/settings.binSize);
                    require(std::isfinite(at)&&n,"engine grass face indexes an empty room grid");
                    if(at<=0)return std::size_t{};
                    if(double(at)>=double(n))return n-1;
                    return std::size_t(at);
                };
                const auto xy=std::make_pair(coordinate(centroid.x,room.bounds.minimum.x,nx),coordinate(centroid.y,room.bounds.minimum.y,ny));
                auto found=index.find(xy);
                if(found==index.end()) {
                    GrassBin b;b.room=room.room;b.identity=next.bins_.size()+1;b.document=room.document;
                    b.x=xy.first;b.y=xy.second;b.surface=face.surfaceMaterial;
                    found=index.emplace(xy,next.bins_.size()).first;next.bins_.push_back(std::move(b));
                }
                auto& bin=next.bins_[found->second];
                const float area=.5f*length(cross(sub(tri.positions[1],tri.positions[0]),sub(tri.positions[2],tri.positions[1])));
                const float count=area*settings.surfaces[face.surfaceMaterial]->density; // engine mulss then cvttss2si
                require(std::isfinite(count)&&count>=0&&double(count)<=std::numeric_limits<std::int32_t>::max(),
                        "face blade count exceeds engine signed storage");
                tri.bladeCount=std::size_t(count);if(!tri.bladeCount)continue; // first material still wins
                require(tri.bladeCount<=std::numeric_limits<std::size_t>::max()-totalBlades,
                        "grass blade count exceeds count storage");
                totalBlades+=tri.bladeCount;bin.bladeCount+=tri.bladeCount;
                for(auto p:tri.positions)extend(bin.bounds,p);
                bin.lightmapped=bin.lightmapped||tri.lightmapped;bin.colored=bin.colored||tri.colored;
                bin.triangles.push_back(std::move(tri));
            }
        }
        // SpawnRoom finishes/attaches the completed grid x-major, y-minor;
        // creation order only determines the first material owning each bin.
        std::sort(next.bins_.begin()+firstBin,next.bins_.end(),[](const auto& a,const auto& b){return std::tie(a.x,a.y)<std::tie(b.x,b.y);});
    }
    for(auto& b:next.bins_)if(b.bounds.valid) {
        job.check();
        b.center=mul(add(b.bounds.minimum,b.bounds.maximum),.5f);b.radius=.5f*length(sub(b.bounds.maximum,b.bounds.minimum));
        b.tailFloats=b.lightmapped?7:b.colored?6:5;
        // SortTriangles orders a copy for lightmap group counts only.
        // CreateArrays then traverses the original list for positions and RNG.
        // Never mutate the face-admission list here, including unlightmapped bins.
    }
    *this=std::move(next);
}
void GrassScene::build(GrassBin& b,model::ParticleRandom& random) {
    if(b.built||!b.bladeCount)return;
    const auto& m=*settings_.surfaces[b.surface];
    std::vector<float> pool;
    const auto floatsPerBlade=4u*(3u+b.tailFloats);
    require(b.bladeCount<=pool.max_size()/floatsPerBlade,"grass pool exceeds storage capacity");
    pool.resize(b.bladeCount*floatsPerBlade);
    auto groups=engineGrassGroups(b);
    std::vector<GrassFaceBuild> builds;builds.reserve(b.triangles.size());
    const auto before=random.state;std::size_t blade=0;
    for(std::size_t ordinal=0;ordinal<b.triangles.size();++ordinal) {
        const auto& t=b.triangles[ordinal];const auto firstBlade=blade;
        const double seed=std::trunc(t.positions[0].x+t.positions[1].y+t.positions[2].z); // two float addss
        require(std::isfinite(seed)&&seed>=double(std::numeric_limits<std::int32_t>::min())&&seed<=double(std::numeric_limits<std::int32_t>::max()),"face RNG seed outside engine signed range");
        random.seed(static_cast<std::uint32_t>(static_cast<std::int32_t>(seed)));
        for(std::size_t q=0;q<t.bladeCount;++q,++blade) {
            float w[3]={random01(random),random01(random),random01(random)};const float sum=w[0]+w[1]+w[2];
            // Engine Vector / scalar (K1 0x3df16a) returns the input for zero,
            // otherwise multiplies by a single rounded reciprocal.
            if(sum!=0){const float inverse=1.f/sum;for(float& a:w)a*=inverse;}
            Vec3 center{};Vec2 lm{};float rgba[4]{};
            for(unsigned i=0;i<3;++i) {center=add(center,mul(t.positions[i],w[i]));lm.x+=w[i]*t.lightmapUvs[i].x;lm.y+=w[i]*t.lightmapUvs[i].y;for(unsigned c=0;c<4;++c)rgba[c]+=w[i]*t.colors[i][c];}
            const float yawRandom=random01(random),yaw=(yawRandom+yawRandom)*3.14159265358979323846f;float r=random01(random);unsigned quadrant=0;
            for(unsigned i=0;i<4;++i){if(r<m.probabilities[i]){quadrant=i;break;}r-=m.probabilities[i];}
            const float variation=1.f+(random01(random)-.5f)*.4f;
            const float half=m.quadSize*.5f,halfYaw=yaw*.5f;
            const model::Quaternion rotation{0,0,std::sin(halfYaw),std::cos(halfYaw)};
            const Vec3 local[4]={{-half,0,0},{half,0,0},{half,0,m.quadSize},{-half,0,m.quadSize}};
            const float u=(quadrant&1u)?.5f:0.f,v=(quadrant&2u)?.5f:0.f;
            const Vec2 uv[4]={{u,v},{u+.5f,v},{u+.5f,v+.5f},{u,v+.5f}};
            std::uint32_t packed=0x7f000000u;
            if(t.colored){packed=0;for(unsigned c=0;c<4;++c)packed|=std::uint32_t(std::clamp(rgba[c],0.f,255.f))<<(8u*c);}
            for(unsigned c=0;c<4;++c) {
                auto rotated=engineRotate(rotation,local[c]);if(c>=2)rotated=mul(rotated,variation);
                const auto vertex=blade*4u+c;write3(pool,vertex*3u,add(center,rotated));
                const auto tail=b.bladeCount*12u+vertex*b.tailFloats;write3(pool,tail,t.normal);
                pool[tail+3]=uv[c].x;pool[tail+4]=uv[c].y;
                if(b.lightmapped){require(t.lightmapped,"engine mixed lightmap/color bin has undefined UV tail");pool[tail+5]=lm.x;pool[tail+6]=lm.y;}
                else if(b.colored)pool[tail+5]=static_cast<float>(packed); // numeric conversion, NOT bit_cast
            }
        }
        builds.push_back({ordinal,t.part,t.face,firstBlade,t.bladeCount,
            static_cast<std::uint32_t>(static_cast<std::int32_t>(seed)),random.state});
    }
    b.pool=std::move(pool);b.groups=std::move(groups);
    b.faceBuilds=std::move(builds);b.randomStateBefore=before;b.randomStateAfter=random.state;
    b.built=true;++b.revision;
}
void GrassScene::collect(Vec3 camera,const std::vector<std::uint64_t>& rooms,const std::vector<model::TriggerPlane>& planes,model::ParticleRandom& random) {
    require(finite(camera),"non-finite camera");
    for(const auto& p:planes)require(finite(p.normal)&&std::isfinite(p.distance),"non-finite frustum");
    for(auto& b:bins_){b.active=false;b.visible=false;b.opacity=0;}
    std::vector<std::size_t> active;
    // CollectBins visits room traversal order, then the room's registered
    // x/y grid order. This order also determines shared RNG consumption.
    for(const auto room:rooms)for(std::size_t i=0;i<bins_.size();++i) {
        auto& b=bins_[i];if(b.room!=room)continue;
        const float distance=std::max(0.f,length(sub(camera,b.center))-b.radius);b.minimumDistanceSquared=distance*distance;
        b.active=settings_.enabled&&b.minimumDistanceSquared<settings_.farSquared;
        b.visible=b.active;
        if(!b.active)continue;
        active.push_back(i);
        build(b,random);
        for(const auto& p:planes)if(p.normal.x*b.center.x+p.normal.y*b.center.y+p.normal.z*b.center.z+p.distance-b.radius>0){b.visible=false;break;}
        b.opacity=b.minimumDistanceSquared<settings_.nearSquared?1.f:1.f-(b.minimumDistanceSquared-settings_.nearSquared)/(settings_.farSquared-settings_.nearSquared);
    }
    // AurProcessActiveBins builds all new residents before releasing entries
    // absent from the previous active list (K1 0x35caea, K2 0x12e5d5).
    for(const auto i:activeBins_) {
        auto& b=bins_[i];if(b.active||!b.built)continue;
        b.pool.clear();b.pool.shrink_to_fit();b.groups.clear();b.faceBuilds.clear();b.built=false;++b.revision;
    }
    activeBins_=std::move(active);
}
void GrassScene::advanceWind(float seconds,const model::WindManager& wind) {
    // UpdateGrassForWind reads the current rendering scene's IsAnimating
    // predicate before mutating the resident pools in all four executables.
    if(!SceneAnimation::currentScene()->isAnimating())return;
    require(std::isfinite(seconds)&&seconds>=0,"invalid wind frame time");if(!settings_.wind||seconds==0)return;
    // UpdateGrassForWind reads WindManager +0x18 (global velocity), not
    // +0x24 (updateTimer's frame displacement). Its cap is independent of dt.
    const float dt=std::min(seconds,.1f),magnitude=length(wind.globalWind());
    const float cap=magnitude>5?5/magnitude:1;
    for(auto& b:bins_)if(b.visible&&b.built&&b.minimumDistanceSquared<settings_.nearSquared) {
        const float quadSize=settings_.surfaces[b.surface]->quadSize;
        // Both K2 builds compute this gain before the blade loop; K1 keeps
        // dt*force*quadSize*stiffness below. Reassociation changes pool values.
        const bool k2=settings_.game==model::GameVersion::Kotor2;
        const float gain=k2?(quadSize*dt)*settings_.stiffness:0.0f;
        for(std::size_t q=0;q<b.bladeCount;++q) {
            const auto at=q*12u;const auto p0=read3(b.pool,at),p1=read3(b.pool,at+3u);
            auto p2=read3(b.pool,at+6u),p3=read3(b.pool,at+9u);
            const Vec3 sample{(p0.x+p1.x)*.5f,(p0.y+p1.y)*.5f,p0.z};
            const auto force=add(add({p0.x-p3.x,p0.y-p3.y,0},
                mul(wind.getGlobalWindNoPoint(sample,1.f),cap)),wind.getGlobalPointWind(sample));
            if(force.x==0&&force.y==0&&force.z==0)continue; // engine 0x316ade
            if(k2) {
                p3.x+=force.x*gain;p3.y+=force.y*gain;
            } else {
                p3.x+=((dt*force.x)*quadSize)*settings_.stiffness;
                p3.y+=((dt*force.y)*quadSize)*settings_.stiffness;
            }
            p2.x=(p3.x-p0.x)+p1.x;p2.y=(p3.y-p0.y)+p1.y;
            write3(b.pool,at+6u,p2);write3(b.pool,at+9u,p3);
        }
        ++b.revision;
    }
}
std::vector<std::size_t> GrassScene::drawOrder() const {
    std::vector<std::size_t> out;for(std::size_t m=0;m<32;++m)if(settings_.surfaces[m])
        for(const auto i:activeBins_)if(bins_[i].surface==m&&bins_[i].visible&&bins_[i].built&&bins_[i].opacity>0)out.push_back(i);
    return out;
}
model::Mesh GrassScene::drawMesh(const GrassBin& b,const GrassGroup& g,bool lightmapped) {
    require(b.bladeCount<=std::numeric_limits<std::size_t>::max()/4u,"grass vertex count exceeds index storage");
    require(b.built&&g.firstVertex<=b.bladeCount*4u&&g.bladeCount<=b.bladeCount-g.firstVertex/4u,"invalid grass draw group");
    model::Mesh mesh;
    require(g.bladeCount<=(std::uint64_t(std::numeric_limits<std::uint32_t>::max())+1u)/4u&&
            g.bladeCount<=mesh.vertices.max_size()/4u&&g.bladeCount<=mesh.indices.max_size()/6u,
            "grass draw exceeds vertex/index storage");
    mesh.vertices.resize(g.bladeCount*4u);mesh.lightmapped=lightmapped;mesh.mdxNormalOffset=0;mesh.mdxTexcoordOffsets[0]=0;if(lightmapped)mesh.mdxTexcoordOffsets[1]=0;
    for(std::size_t n=0;n<mesh.vertices.size();++n) {
        const auto source=g.firstVertex+n;auto& v=mesh.vertices[n];v.position=read3(b.pool,source*3u);
        const auto tail=g.bladeCount*12u+source*b.tailFloats; // engine per-draw count, deliberately not b.bladeCount
        require(tail+(lightmapped?7u:5u)<=b.pool.size(),"engine grass attribute draw exceeds its allocation");
        v.normal=read3(b.pool,tail);v.texcoord0={b.pool[tail+3],b.pool[tail+4]};if(lightmapped)v.texcoord1={b.pool[tail+5],b.pool[tail+6]};
        v.color={{0,0,0,255}}; // array disabled; conventional current color set by DoBinBucket
    }
    for(std::uint32_t q=0;q<g.bladeCount;++q){const auto v=q*4u;mesh.indices.insert(mesh.indices.end(),{v,v+1,v+2,v,v+2,v+3});}
    return mesh;
}
GrassStatistics GrassScene::statistics() const noexcept {
    GrassStatistics s;s.bins=bins_.size();for(const auto& b:bins_){s.faces+=b.triangles.size();s.blades+=b.bladeCount;if(b.built){++s.residentBins;s.residentBlades+=b.bladeCount;}}return s;
}
} // namespace neomdl
