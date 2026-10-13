#include <model/AsciiModelInternal.hpp>
#include <model/EngineArithmetic.hpp>

#include <algorithm>
#include <cstring>
#include <functional>

namespace neomdl::model::ascii {
namespace {

std::uint32_t word(const std::uint8_t* data) noexcept {
    return std::uint32_t(data[0]) | (std::uint32_t(data[1])<<8u) |
           (std::uint32_t(data[2])<<16u) | (std::uint32_t(data[3])<<24u);
}
float scalar(const std::uint8_t* data) noexcept {
    const auto bits=word(data);
    float value;
    std::memcpy(&value,&bits,sizeof(value));
    return value;
}
std::int32_t integer(const std::uint8_t* data) noexcept {
    const auto bits=word(data);
    std::int32_t value;
    std::memcpy(&value,&bits,sizeof(value));
    return value;
}

struct WeightRow {
    std::vector<std::pair<std::string,float>> values{
        {"ROOT",0.0f},{"ROOT",0.0f},{"ROOT",0.0f},{"ROOT",0.0f}};
    std::array<std::int32_t,4> indices{{-1,-1,-1,-1}};
    // Engine Parse leaves lane zero untouched until its float conversion;
    // distinguish a retained/appended row from uninitialized engine storage.
    bool firstWeightDefined{};
};

Quaternion inverse(Quaternion value) noexcept {
    // Engine operator-(Quaternion) negates w, retaining the vector part.
    // K1 0x665224; this is the same rotation as the usual conjugate.
    value.w=-value.w;
    return value;
}
Quaternion product(Quaternion a,Quaternion b) noexcept {
    // K1 0x665234 and the corresponding K2 Quaternion multiply.
    return {((b.w*a.x+a.w*b.x)+a.y*b.z)-b.y*a.z,
            b.x*a.z+(b.w*a.y+(a.w*b.y-a.x*b.z)),
            b.w*a.z+((a.x*b.y+a.w*b.z)-b.x*a.y),
            ((a.w*b.w-a.x*b.x)-a.y*b.y)-a.z*b.z};
}
Vec3 rotate(Quaternion q,Vec3 p) noexcept {
    // Both GatherBones callers use the engine identity shortcut.
    if(q.w==1.0f || q.w==-1.0f) return p;
    const float xx=q.x*q.x,yy=q.y*q.y,zz=q.z*q.z;
    const float xy=q.x*q.y,xz=q.x*q.z,yz=q.y*q.z;
    const float xw=q.x*q.w,yw=q.y*q.w,zw=q.z*q.w;
    return {((1.0f-2.0f*(yy+zz))*p.x+2.0f*(xy-zw)*p.y)+2.0f*(xz+yw)*p.z,
            (2.0f*(xy+zw)*p.x+(1.0f-2.0f*(xx+zz))*p.y)+2.0f*(yz-xw)*p.z,
            (2.0f*(xz-yw)*p.x+2.0f*(yz+xw)*p.y)+(1.0f-2.0f*(xx+yy))*p.z};
}
Vec3 add(Vec3 a,Vec3 b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 negative(Vec3 value) noexcept { return {-value.x,-value.y,-value.z}; }

} // namespace

Quaternion engineModelQuaternionProduct(Quaternion left,Quaternion right) noexcept {
    return product(left,right);
}

Vec3 engineModelRotateVector(Quaternion orientation,Vec3 value) noexcept {
    return rotate(orientation,value);
}

static bool parseSkinValue(const Line& line,Reader& reader,RawNode& raw,GameVersion) {
    const auto& field=line.tokens.front();
    auto& mesh=*raw.mesh;
    if(equal(field,"weights")) {
        // Parse<SkinVertexWeight>: K1 0x5ad44c, K2 0x7ab300. A text
        // row has four ordered name/weight pairs; trailing slots are ROOT/0.
        std::vector<WeightRow> rows(raw.weights.size());
        for(std::size_t i=0;i<rows.size();++i) {
            rows[i].values=raw.weights[i];
            rows[i].firstWeightDefined=true;
            if(!raw.skinWeightIndices.empty()) rows[i].indices=raw.skinWeightIndices.at(i);
        }





        constexpr std::size_t pointerBytes=4u;
        constexpr std::size_t stride=52u;
        readArray(line,reader,rows,stride,[](const Line& entry,const Reader& input,WeightRow& row) {
            // Parse resets the trailing pairs before one sequential sscanf;
            // a failed later conversion does not resume at another token.
            // The first weight remains the destination's previous value.
            for(std::size_t i=1;i<4u;++i) row.values[i]={"ROOT",0.0f};
            row.indices[0]=row.indices[1]=-1;
            const char* cursor=entry.text.c_str();
            for(std::size_t i=0;i<4u;++i) {
                int first{},last{};
                std::sscanf(cursor," %n%*s%n",&first,&last);
                if(last==0) {
                    if(i==0u) input.fail(entry,"ASCII skin weight requires the engine uninitialized bone name");
                    break;
                }
                row.values[i].first.assign(cursor+first,static_cast<std::size_t>(last-first));
                cursor+=last;
                int consumed{};
                if(std::sscanf(cursor,"%f%n",&row.values[i].second,&consumed)!=1) {
                    if(i==0u && !row.firstWeightDefined)
                        input.fail(entry,"ASCII skin weight requires the engine uninitialized first weight");
                    break;
                }
                if(i==0u) row.firstWeightDefined=true;
                cursor+=consumed;
            }
        },[&](const std::uint8_t* data) {
            WeightRow row;
            row.firstWeightDefined=true;
            for(std::size_t i=0;i<4u;++i) {
                row.values[i].second=scalar(data+pointerBytes*4u+i*4u);
                row.indices[i]=integer(data+pointerBytes*4u+16u+i*4u);
            }
            // The unresolved form owns live char* names, not file-relative
            // offsets. Reading those requires the original process capture.
            if(row.indices[0]==-1)
                reader.fail(line,"Packed skin weights contain unresolved engine name pointers; the engine process name data is required");
            return row;
        },[&](const Line& header,const Reader& input) {





            for(const auto& row:rows)
                if(!row.firstWeightDefined) RejectIncompleteArray{}(header,input);
        });
        raw.weights.resize(rows.size());raw.skinWeightIndices.resize(rows.size());
        for(std::size_t i=0;i<rows.size();++i) {
            raw.weights[i]=std::move(rows[i].values);
            raw.skinWeightIndices[i]=rows[i].indices;
        }
        return true;
    }
    if(equal(field,"qbone_ref_inv")) {
        readArray(line,reader,mesh.inverseBindOrientations,16u,
                  [](const Line& entry,const Reader& input,Quaternion& value) {
            Vec3 axis{};
            float angle;
            const auto fields=std::sscanf(entry.text.c_str(),"%f %f %f %f",
                                          &axis.x,&axis.y,&axis.z,&angle);
            // Parse<Quaternion>, K1 0x5a0b80, checks the zero axis before
            // reading the angle. Short zero-axis rows therefore have a
            // defined identity result. Packed rows are w,x,y,z.
            if(axis.x==0.0f && axis.y==0.0f && axis.z==0.0f) {
                value=Quaternion{};
                return;
            }
            if(fields!=4)
                input.fail(entry,"ASCII quaternion requires the engine uninitialized angle value");
            value=engineAxisAngle(axis,angle);
        },[](const std::uint8_t* data) {
            return Quaternion{scalar(data+4u),scalar(data+8u),scalar(data+12u),scalar(data)};
        },[](const Line&,const Reader&) {
            // Parse<List<Quaternion>> returns at physical EOF. SetSize has
            // already retained old rows and constructed new identity rows.
        });
        return true;
    }
    if(equal(field,"tbone_ref_inv")) {
        readArray(line,reader,mesh.inverseBindTranslations,12u,
                  [](const Line& entry,const Reader&,Vec3& value) {
            // Parse<Vector> writes directly into the retained array row.
            std::sscanf(entry.text.c_str(),"%f %f %f",&value.x,&value.y,&value.z);
        },[](const std::uint8_t* data) {
            return Vec3{scalar(data),scalar(data+4u),scalar(data+8u)};
        },[](const Line&,const Reader&) {
            // Parse<List<Vector>> likewise retains its initialized rows;
            // newly allocated inverse translations are engine zero vectors.
        });
        return true;
    }
    if(equal(field,"boneconstantindices")) {
        // Parse<List<int>> also returns at physical EOF with its retained
        // initialized tail intact (K2 Desktop 0xf16ee..0xf17dd).
        readScalarArray(line,reader,mesh.boneConstantIndices,4u,
                  [](const Line& entry,const Reader&,std::int32_t& value) {
            return scanInteger(entry.text.c_str(),value);
        },[](const std::uint8_t* data) { return integer(data); },true);
        return true;
    }
    return false;
}

bool parseRetainedSkin(const Line& line,Reader& reader,RawNode& raw) {
    const auto field=lower(line.tokens.front());
    if(field!="skinbonemap" && field!="skinpalette" && field!="skinbindrotations" &&
       field!="skinbindtranslations" && field!="skinweights") return false;
    raw.advancedAscii=true;
    if(!raw.skinData) raw.skinData.emplace();
    auto& skin=*raw.skinData;
    if(field=="skinbonemap") readArray(line,reader,skin.boneToPalette,4u,
        [](const Line& row,const Reader&,float& value) {value=parseFloat(row.tokens[0]);},
        [](const std::uint8_t* p) {return scalar(p);});
    else if(field=="skinpalette") {
        std::vector<std::int16_t> palette;
        readArray(line,reader,palette,2u,
            [](const Line& row,const Reader&,std::int16_t& value) {value=static_cast<std::int16_t>(parseInt(row.tokens[0]));},
            [](const std::uint8_t* p) {return static_cast<std::int16_t>(p[0]|(std::uint16_t(p[1])<<8u));});
        for(std::size_t i=0;i<skin.paletteToBone.size();++i) skin.paletteToBone[i]=palette.at(i);
    } else if(field=="skinbindrotations") readArray(line,reader,skin.inverseOrientations,16u,
        [](const Line& row,const Reader&,Quaternion& q) {
            std::sscanf(row.text.c_str(),"%f %f %f %f",&q.x,&q.y,&q.z,&q.w);
        },[](const std::uint8_t* p) {return Quaternion{scalar(p),scalar(p+4u),scalar(p+8u),scalar(p+12u)};});
    else if(field=="skinbindtranslations") readArray(line,reader,skin.inverseTranslations,12u,
        [](const Line& row,const Reader&,Vec3& v) {
            std::sscanf(row.text.c_str(),"%f %f %f",&v.x,&v.y,&v.z);
        },[](const std::uint8_t* p) {return Vec3{scalar(p),scalar(p+4u),scalar(p+8u)};});
    else readArray(line,reader,raw.skinChannels,32u,
        [](const Line& row,const Reader&,EngineFloatSkinChannels& value) {
            for(std::size_t i=0;i<4u;++i) {
                value.weights[i]=parseFloat(row.tokens[i]);
                value.indices[i]=parseFloat(row.tokens[i+4u]);
            }
        },[](const std::uint8_t* p) {
            EngineFloatSkinChannels value;
            for(std::size_t i=0;i<4u;++i) {value.weights[i]=scalar(p+i*4u);value.indices[i]=scalar(p+16u+i*4u);}
            return value;
        });
    return true;
}

bool parseSkinField(const Line& initial,Reader& reader,RawNode& raw,GameVersion game) {
    if(parseRetainedSkin(initial,reader,raw)) return true;
    bool handled=false;


    for(const auto* field:{"weights","qbone_ref_inv","tbone_ref_inv","boneconstantindices"}) {
        const auto line=reader.fieldLine(initial.bufferOffset);
        if(equal(line.tokens.front(),field))
            handled=parseSkinValue(line,reader,raw,game) || handled;
    }
    return handled;
}

void buildSkin(Model& model,RawNode& raw,Mesh& mesh,
               const std::vector<std::size_t>& sources,
               const std::vector<std::size_t>& partPath,
               const neoshared::bif::JobControl& job) {
    // PartSkin::PostProcess: K1 0x5e3ed4, K2 0x711850. Existing inverse
    // translations skip this first-instance producer, including its pools.
    if(!mesh.inverseBindTranslations.empty()) return;
    // After vertex generation, a nonpositive signed count selects a null
    // NodeVertex and still reads its attributes at +0x54. There is no engine
    // empty-skin stream to publish (K1 Desktop 0x363d55..0x363d9d, K2 Desktop
    // 0x105c5b..0x105c9f, K1 ARM 0x5e45c8..0x5e4610, K2 0x712846..0x712892).
    if(static_cast<std::int16_t>(mesh.enginePrelightVertexCount)<=0)
        throw ModelError("Cannot compile a generated skin with a nonpositive engine vertex count");

    std::vector<std::size_t> depthFirst;
    std::shared_ptr<AsciiSkinSource> packedSource;
    if(raw.skinData) {
        packedSource=std::make_shared<AsciiSkinSource>(*raw.skinData);
        packedSource->boneConstantCount=mesh.boneConstantIndices.size();
        const auto visit=[&](const auto& self,std::size_t node)->void {
            depthFirst.push_back(node);
            for(const auto child:model.nodes[node].children) self(self,child);
        };
        visit(visit,partPath.front());
        mesh.inverseBindOrientations=packedSource->inverseOrientations;
        mesh.inverseBindTranslations=packedSource->inverseTranslations;
        for(std::size_t i=0;i<packedSource->inverseOrientations.size();++i)
            mesh.inverseBindOrientations.at(depthFirst.at(i))=packedSource->inverseOrientations[i];
        for(std::size_t i=0;i<packedSource->inverseTranslations.size();++i)
            mesh.inverseBindTranslations.at(depthFirst.at(i))=packedSource->inverseTranslations[i];
    } else {
        std::vector<Vec3> localPositions(model.nodes.size());
        std::vector<Quaternion> localOrientations(model.nodes.size());
        for(std::size_t i=0;i<model.nodes.size();++i)
            constructedNodeFrame(model.nodes[i],localPositions[i],localOrientations[i],
                model.game);

        // CreateInstanceTreeR constructs a distinct Part for every child link.
        // PartSkin climbs that instance's parent chain, which can differ from
        // the MdlNode's last authored parent after repeated parent directives.
        auto root=partPath.back();
        auto frameOrientation=inverse(localOrientations[root]);
        auto framePosition=rotate(frameOrientation,negative(localPositions[root]));
        for(auto depth=partPath.size()-1u;depth!=0u;--depth) {
            root=partPath[depth-1u];
            const auto parentInverse=inverse(localOrientations[root]);
            framePosition=add(framePosition,rotate(frameOrientation,
                                rotate(parentInverse,negative(localPositions[root]))));
            frameOrientation=product(frameOrientation,parentInverse);
        }

        std::vector<Vec3> inversePositions(model.nodes.size());
        std::vector<Quaternion> inverseOrientations(model.nodes.size());
        std::vector<Vec3> engineInversePositions;
        std::vector<Quaternion> engineInverseOrientations;
        std::function<void(std::size_t,Vec3,Quaternion)> gather;
        gather=[&](std::size_t node,Vec3 parentPosition,Quaternion parentOrientation) {
            job.check();
            const auto position=add(parentPosition,rotate(parentOrientation,localPositions[node]));
            const auto orientation=product(parentOrientation,localOrientations[node]);
            depthFirst.push_back(node);
            inverseOrientations[node]=inverse(orientation);
            inversePositions[node]=rotate(inverseOrientations[node],negative(position));
            // GatherBones appends one row per Part visit, before descending.
            // Two Parts sharing a node can have different ancestor frames;
            // retaining by node index would overwrite the earlier engine row.
            engineInverseOrientations.push_back(inverseOrientations[node]);
            engineInversePositions.push_back(inversePositions[node]);
            for(const auto child:model.nodes[node].children) gather(child,position,orientation);
        };
        gather(root,framePosition,frameOrientation);

        // Engine pools are indexed by depth-first part position. Model retains
        // stable node indices, so translate both generated binds and palette
        // entries together; node IDs used by animation are independent.
        auto engineOrientations=std::move(mesh.inverseBindOrientations);
        engineOrientations.insert(engineOrientations.end(),engineInverseOrientations.begin(),
                                  engineInverseOrientations.end());
        packedSource=std::make_shared<AsciiSkinSource>();
        packedSource->inverseOrientations=engineOrientations;
        packedSource->inverseTranslations=std::move(engineInversePositions);
        // Engine assigns only used palette slots. The packed encoder's explicit
        // zero-padding policy supplies otherwise uninitialized storage bytes.
        packedSource->paletteToBone.fill(0);
        packedSource->boneConstantCount=depthFirst.size();
        mesh.inverseBindOrientations.resize(engineOrientations.size());
        mesh.inverseBindTranslations=std::move(inversePositions);
        // PartSkin appends generated quaternions even when qbone_ref_inv alone
        // supplied a prefix. Runtime indexing still starts at zero; preserve
        // that prefix instead of requiring equally sized q/t arrays. Remap the
        // consumed DFS prefix together with the translations and retain the
        // unconsumed appended tail for inspection.
        for(std::size_t i=0;i<depthFirst.size();++i)
            mesh.inverseBindOrientations[depthFirst[i]]=engineOrientations[i];
        std::copy(engineOrientations.begin()+static_cast<std::ptrdiff_t>(depthFirst.size()),
                  engineOrientations.end(),
                  mesh.inverseBindOrientations.begin()+static_cast<std::ptrdiff_t>(depthFirst.size()));
    }
    mesh.hasInverseBindPose=!raw.skinData || (!mesh.inverseBindOrientations.empty() &&
        mesh.inverseBindOrientations.size()==mesh.inverseBindTranslations.size());

    const auto findBone=[&](const std::string& name) -> std::int32_t {
        // FindPartIndexDFS: ROOT is a special token, then case-insensitive
        // first match in the final sibling-ordered tree.
        if(equal(name,"root")) return 0;
        for(std::size_t i=0;i<depthFirst.size();++i)
            if(equal(name,model.nodes[depthFirst[i]].name)) return static_cast<std::int32_t>(i);
        return -1;
    };
    std::vector<std::int32_t> slots(depthFirst.size(),-1);
    std::int32_t nextSlot{};
    auto floats=std::make_shared<std::vector<EngineFloatSkinChannels>>(sources.size());
    auto cpu=std::make_shared<EngineCpuSkinSource>();
    cpu->unsignedStripBoneIndices=model.game==GameVersion::Kotor1;
    cpu->indices=mesh.engineDrawIndices;cpu->draws=mesh.engineDraws;
    cpu->stripVertices=mesh.engineDrawIndices;
    cpu->positions.reserve(sources.size());cpu->normals.reserve(sources.size());
    cpu->weights.resize(sources.size());cpu->stripBoneIndices.resize(sources.size());
    cpu->triangleBoneIndices.resize(sources.size());
    mesh.skinInfluences.resize(sources.size());
    mesh.skinBoneMap.fill(-1);
    mesh.skinBoneMapSize=17u;
    for(std::size_t vertex=0;vertex<sources.size();++vertex) {
        job.check();
        const auto source=sources[vertex];
        auto& channels=(*floats)[vertex];
        auto& influence=mesh.skinInfluences[vertex];
        if(raw.skinData) {
            channels=raw.skinChannels.at(source);
            influence.weights=channels.weights;
            for(std::size_t lane=0;lane<4u;++lane) {
                influence.boneIndices[lane]=static_cast<std::uint16_t>(engineFloatToInt(channels.indices[lane]));
                if(channels.weights[lane]!=0.0f) {
                    const auto slot=static_cast<std::size_t>(engineFloatToInt(channels.indices[lane]));
                    mesh.skinBoneMap.at(slot)=static_cast<std::int16_t>(depthFirst.at(
                        static_cast<std::size_t>(packedSource->paletteToBone.at(slot))));
                }
            }
        } else {
            const auto& weights=raw.weights.at(source);
            auto bones=raw.skinWeightIndices.at(source);
            if(bones[0]==-1)
                for(std::size_t channel=0;channel<4u;++channel)
                    bones[channel]=findBone(weights[channel].first);
            channels.indices.fill(-1.0f);
            influence.boneIndices.fill(0xffffu);
            for(std::size_t channel=0;channel<4u;++channel) {
                const auto weight=weights[channel].second;
                channels.weights[channel]=weight;
                influence.weights[channel]=weight;
                if(weight==0.0f) continue;
                auto& slot=slots.at(static_cast<std::size_t>(bones[channel]));
                if(slot<0) slot=nextSlot++;
                channels.indices[channel]=static_cast<float>(slot);
                influence.boneIndices[channel]=static_cast<std::uint16_t>(slot);
                mesh.skinBoneMap.at(static_cast<std::size_t>(slot))=
                    static_cast<std::int16_t>(depthFirst.at(static_cast<std::size_t>(bones[channel])));
                packedSource->paletteToBone.at(static_cast<std::size_t>(slot))=
                    static_cast<std::int16_t>(bones[channel]);
            }
        }
        cpu->positions.push_back(mesh.vertices[vertex].position);
        cpu->normals.push_back(mesh.vertices[vertex].normal);
        cpu->weights[vertex]=channels.weights;
        cpu->stripBoneIndices[vertex]=channels.indices;
        // The legacy trailing-triangle consumer reads ushort lanes from
        // this same float4 storage. No such tail is generated by the shared
        // indexed-triangle builder, but retain the byte interpretation.
        for(std::size_t channel=0;channel<4u;++channel) {
            std::uint32_t bits;
            std::memcpy(&bits,&channels.indices[channel/2u],sizeof(bits));
            cpu->triangleBoneIndices[vertex][channel]=
                static_cast<std::uint16_t>(bits>>((channel%2u)*16u));
        }
    }
    // PartSkin writes the extra MDX row after the N-vertex palette loop:
    // weights=(1,0,0,0), indices=(0,0,0,0). An authored draw batch can
    // reference it through the VP path. CPU skin output pools remain N
    // entries; their separate producer does not initialize row N.
    if(mesh.vertices.size()>mesh.enginePrelightVertexCount) {
        EngineFloatSkinChannels sentinel;
        sentinel.weights={{1.0f,0.0f,0.0f,0.0f}};
        sentinel.indices.fill(0.0f);
        floats->push_back(sentinel);
        SkinInfluence influence;
        influence.weights=sentinel.weights;
        influence.boneIndices.fill(0u);
        mesh.skinInfluences.push_back(influence);
    }
    // The producer appends two float4 channels to its interleaved row;
    // consumers receive the generated semantic pools directly here.
    mesh.mdxWeightOffset=mesh.mdxRowSize;
    mesh.mdxBoneIndexOffset=mesh.mdxRowSize+16u;
    mesh.mdxRowSize+=32u;
    mesh.hasSkinChannels=true;mesh.hasSkinBoneMap=true;
    if(!raw.skinData)
        for(const auto slot:slots) packedSource->boneToPalette.push_back(static_cast<float>(slot));
    mesh.asciiSkinSource=std::move(packedSource);
    mesh.builtCpuSkinSource=std::move(cpu);
    mesh.builtFloatSkinChannels=std::move(floats);
    raw.weights.clear();raw.skinWeightIndices.clear();
}

} // namespace neomdl::model::ascii
