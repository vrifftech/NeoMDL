#include <model/AsciiModelInternal.hpp>
#include <model/AsciiControllerFields.hpp>
#include <model/AsciiFixedString.hpp>
#include <model/Emitter.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace neomdl::model::ascii {
namespace {

const char* fieldArguments(const Line& line) {
    const auto start=line.text.find_first_not_of(" \t");
    return line.text.c_str()+start+line.tokens[0].size();
}

// The engine direct integer fields accept %d, then TRUE/FALSE if %d did
// not write a value. In contrast, emitter Enable* fields accept only true/1.
std::optional<std::int32_t> integerValue(const Line& line) {
    std::int32_t value;
    if(scanInteger(fieldArguments(line),value)) return value;
    return std::nullopt;
}
void directInteger(const Line& line, std::int32_t& field) {
    // These %d destinations are copied unchanged by the engine node writer;
    // they are not C++ bools (K2 0x79fa99, 0x7a184b, 0x7ac0f9).
    if (const auto value=integerValue(line)) field=*value;
}
void directFloat(const Line& line, float& field) {
    std::sscanf(fieldArguments(line),"%f",&field);
}
template<std::size_t N>
void directString(const Line& line, std::string& field,std::array<std::uint8_t,N>& retained) {
    const auto previous=field;
    scanString(fieldArguments(line),field);
    retainAsciiFixedString(retained,previous,field);
}
bool enabled(const Line& line) {
    bool value=false;
    scanBoolean(fieldArguments(line),value);
    return value;
}

float packedFloat(const std::uint8_t* bytes) {
    const auto bits=std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1])<<8u) |
        (std::uint32_t(bytes[2])<<16u) | (std::uint32_t(bytes[3])<<24u);
    float value;
    std::memcpy(&value,&bits,sizeof(value));
    return value;
}

// Parse<float>/Parse<Vector> share counted, endlist and embedded raw-data
// forms in K1 0x5a3c48/0x5a400c and K2 0x6d10e0/0x722be0. Parse<char*>
// (K1 0x5a1260; K2 0x79c130) supports the two text forms only.
void floatList(const Line& line, Reader& reader, std::vector<float>& values) {
    readScalarArray(line,reader,values,4u,[](const Line& row,const Reader&,float& value) {
        return std::sscanf(row.text.c_str(),"%f",&value)==1;
    },[](const std::uint8_t* bytes) {return packedFloat(bytes);},true);
}
void vectorList(const Line& line, Reader& reader, std::vector<Vec3>& values) {
    readArray(line,reader,values,12u,[](const Line& row,const Reader&,Vec3& value) {
        std::sscanf(row.text.c_str(),"%f %f %f",&value.x,&value.y,&value.z);
    },[](const std::uint8_t* bytes) {
        return Vec3{packedFloat(bytes),packedFloat(bytes+4u),packedFloat(bytes+8u)};
    },[](const Line&,const Reader&) {
        // Engine Vector arrays return at EOF with copied old rows and
        // constructor-zeroed new rows (K1 0x5a43e8; K2 0x722fba).
    });
}
void stringList(const Line& line, Reader& reader, std::vector<std::string>& values) {
    readArray(line,reader,values,0u,[](const Line& row,const Reader&,std::string& value) {
        // Parse<char*> allocates a new buffer for every row and scans %s.
        // Its word whitespace includes form feed and vertical tab.
        std::string word;
        scanString(row.text.c_str(),word);
        value=std::move(word);
    },[](const std::uint8_t*) {return std::string{};});
}

bool animMeshField(const Line& line,Reader& reader,RawNode& raw) {
    const auto field=lower(line.tokens.front());
    // NW's animmesh parser delegates these static construction inputs to
    // TriMesh (0x44db69; fields 0x44d680..0x44d6c0). Keep the authoring
    // extension scoped to animmeshes instead of changing KOTOR mesh grammar.
    if(field=="normals") {
        readArray(line,reader,raw.normals,12u,[](const Line& row,const Reader&,Vec3& value) {
            std::sscanf(row.text.c_str(),"%f %f %f",&value.x,&value.y,&value.z);
        },[](const std::uint8_t* bytes) {
            return Vec3{packedFloat(bytes),packedFloat(bytes+4u),packedFloat(bytes+8u)};
        });
    } else if(field=="tangents") {
        // Vector4 allocation initializes all four components to zero;
        // counted replacements and uncounted append copies retain them.
        // NW 0x44dec4..0x44ded8, 0x44ef50..0x44ef9e, 0x4466c0.
        readArray(line,reader,raw.animTangents,16u,
            [](const Line& row,const Reader&,std::array<float,4>& value) {
                std::sscanf(row.text.c_str(),"%f %f %f %f",
                            &value[0],&value[1],&value[2],&value[3]);
            },[](const std::uint8_t* bytes) {
                return std::array<float,4>{{packedFloat(bytes),packedFloat(bytes+4u),
                    packedFloat(bytes+8u),packedFloat(bytes+12u)}};
            });
    } else if(field=="mirrorlist") {
        readScalarArray(line,reader,raw.animMirrorList,4u,
            [](const Line& row,const Reader&,std::int32_t& value) {
                return scanInteger(row.text.c_str(),value);
            },[](const std::uint8_t* bytes) {
                const auto bits=std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1])<<8u) |
                    (std::uint32_t(bytes[2])<<16u) | (std::uint32_t(bytes[3])<<24u);
                std::int32_t value;
                std::memcpy(&value,&bits,sizeof(value));
                return value;
            });
    } else return false;
    return true;
}

std::int32_t parseAabb(const Line& line, std::size_t first, Reader& reader,
                       AabbNodeData& data) {
    // Both games parse six bounds, a face index, and an optional split mask.
    // A face index of -1 recursively consumes exactly two following records.
    AabbTreeNode node;
    const auto begin=line.text.find_first_not_of(" \t");
    const auto text=line.text.c_str()+(begin==std::string::npos?line.text.size():begin)+
        (first?line.tokens[0].size():0u);
    int faceIndex{},splitMask{};
    const auto fields=std::sscanf(text,"%f %f %f %f %f %f %d %d",
        &node.minimum.x,&node.minimum.y,&node.minimum.z,
        &node.maximum.x,&node.maximum.y,&node.maximum.z,&faceIndex,&splitMask);
    if(fields<7) reader.fail(line,"Incomplete AABB record");
    node.faceIndex=faceIndex;
    const auto index=static_cast<std::int32_t>(data.nodes.size());
    data.nodes.push_back(node);
    if (node.faceIndex==-1) {
        Line child;
        if (!reader.next(child)) reader.fail(line,"Missing first AABB child");
        const auto firstChild=parseAabb(child,0u,reader,data);
        if (!reader.next(child)) reader.fail(line,"Missing second AABB child");
        const auto secondChild=parseAabb(child,0u,reader,data);
        data.nodes[index].firstChild=firstChild;
        data.nodes[index].secondChild=secondChild;
        if (fields==8) {
            data.nodes[index].splitMask=static_cast<std::uint32_t>(splitMask);
        } else {
            const auto& a=data.nodes[firstChild];
            const auto& b=data.nodes[secondChild];
            const Vec3 delta{(b.minimum.x+b.maximum.x)-(a.minimum.x+a.maximum.x),
                             (b.minimum.y+b.maximum.y)-(a.minimum.y+a.maximum.y),
                             (b.minimum.z+b.maximum.z)-(a.minimum.z+a.maximum.z)};
            if (std::abs(delta.x)>std::abs(delta.y) && std::abs(delta.x)>std::abs(delta.z))
                data.nodes[index].splitMask=delta.x>0.0f?kAabbPositiveX:kAabbNegativeX;
            else if (std::abs(delta.y)>std::abs(delta.z))
                data.nodes[index].splitMask=delta.y>0.0f?kAabbPositiveY:kAabbNegativeY;
            else data.nodes[index].splitMask=delta.z>0.0f?kAabbPositiveZ:kAabbNegativeZ;
        }
    }
    return index;
}

bool lightField(const Line& line, Reader& reader, LightNodeData& light) {
    const auto field=lower(line.tokens[0]);
    if (field=="ambientonly") directInteger(line,light.ambientOnly);
    else if (field=="shadow") directInteger(line,light.castsShadow);
    else if (field=="isdynamic" || field=="ndynamictype") {
        if (const auto value=integerValue(line)) {
            light.dynamicType=*value;
            light.dynamic=*value!=0;
        }
    } else if (field=="affectdynamic") directInteger(line,light.affectDynamic);
    else if (field=="lightpriority") {
        if (const auto value=integerValue(line)) light.priority=*value;
    } else if (field=="generateflare") directInteger(line,light.generateFlare);
    else if (field=="fadinglight") directInteger(line,light.fading);
    else if (field=="flareradius") directFloat(line,light.flareRadius);
    else if (field=="texturenames") stringList(line,reader,light.textureNames);
    else if (field=="flaresizes") floatList(line,reader,light.flareSizes);
    else if (field=="flarepositions") floatList(line,reader,light.flarePositions);
    else if (field=="flarecolorshifts") vectorList(line,reader,light.flareColorShifts);
    else return false;
    return true;
}

bool emitterField(const Line& line, EmitterNodeData& emitter) {
    const auto field=lower(line.tokens[0]);
    // K1 0x5546f8..0x5547c8 / K2 0x77b6a0..0x77b834: setters OR
    // bits; false authoring entries never undo a previously enabled flag.
    static constexpr std::pair<std::string_view,std::uint32_t> flags[]{
        {"p2p",emitter_flags::PointToPoint}, {"affectedbywind",emitter_flags::AffectedByWind},
        {"m_istinted",emitter_flags::Tinted}, {"bounce",emitter_flags::Bounce},
        {"inherit",emitter_flags::Inherit}, {"inheritvel",emitter_flags::InheritVelocity},
        {"inherit_local",emitter_flags::InheritLocal}, {"splat",emitter_flags::Splat},
        {"inherit_part",emitter_flags::InheritPart}, {"depth_texture",emitter_flags::DepthTexture}
    };
    for (const auto& flag:flags) if (field==flag.first) {
        if (enabled(line)) emitter.rawWords.back()|=flag.second;
        return true;
    }
    if (field=="p2p_sel") {
        if (integerValue(line)==1) emitter.rawWords.back()|=emitter_flags::PointToPointBezier;
    } else if (field=="random") {
        if (const auto value=integerValue(line)) {
            if (*value&1) emitter.rawWords.back()|=emitter_flags::RandomFrame;
            if (*value&2) emitter.rawWords.back()|=emitter_flags::RandomOrder;
        }
    } else if (field=="update") directString(line,emitter.updateMode,emitter.asciiUpdateMode);
    else if (field=="render") directString(line,emitter.renderMode,emitter.asciiRenderMode);
    else if (field=="blend") directString(line,emitter.blendMode,emitter.asciiBlendMode);
    else if (field=="texture") directString(line,emitter.texture,emitter.asciiTexture);
    else if (field=="chunkname") directString(line,emitter.chunkName,emitter.asciiChunkName);
    else if (field=="m_sdepthtexturename") directString(line,emitter.depthTexture,emitter.asciiDepthTexture);
    else if (field=="spawntype") {
        if (const auto value=integerValue(line)) emitter.spawnType=static_cast<std::uint32_t>(*value);
    } else if (field=="xgrid") {
        if (const auto value=integerValue(line)) emitter.xGrid=static_cast<std::uint32_t>(*value);
    } else if (field=="ygrid") {
        if (const auto value=integerValue(line)) emitter.yGrid=static_cast<std::uint32_t>(*value);
    } else if (field=="numbranches") {
        if (const auto value=integerValue(line)) emitter.branchCount=static_cast<std::uint32_t>(*value);
    } else if (field=="controlptsmoothing") directInteger(line,emitter.controlPointSmoothing);
    else if (field=="loop") directInteger(line,emitter.loop);
    else if (field=="twosidedtex") directInteger(line,emitter.twoSidedTexture);
    else if (field=="deadspace") directFloat(line,emitter.deadSpace);
    else if (field=="blastradius") directFloat(line,emitter.blastRadius);
    else if (field=="blastlength") directFloat(line,emitter.blastLength);
    else if (field=="renderorder") {
        int value;
        if (std::sscanf(fieldArguments(line),"%d",&value)==1)
            emitter.renderOrder=static_cast<std::uint16_t>(value);
    } else if (field=="m_bframeblending") {
        scanBoolean(fieldArguments(line),emitter.frameBlending);
    } else return false;
    return true;
}
} // namespace

void initializeNode(RawNode& raw, GameVersion game) {
    const auto flags=raw.node.flags;
    if (flags&kNodeMesh) {
        raw.mesh.emplace();
        // Engine startup globals: K1 0x1d9440, K2 0x26cd57.
        raw.mesh->diffuse={0.8f,0.8f,0.8f};
        raw.mesh->ambient={0.2f,0.2f,0.2f};
        raw.mesh->castsShadow=true;
        if(flags&kNodeAabb) {



            raw.mesh->castsShadow=false;
            raw.mesh->render=false;
        }
        raw.mesh->skinned=(flags&kNodeSkin)!=0u;
        raw.mesh->animated=(flags&kNodeAnimation)!=0u;
        raw.mesh->engineBounds.emplace();
        raw.mesh->engineBounds->box.valid=true;
        if (game==GameVersion::Kotor2) {
            auto& material=raw.mesh->k2Material.emplace();
            // K2 MdlNodeTriMesh 0x6f2cd3 initializes both signed shorts to 1.
            material.dirtTexture=1;
            material.dirtWorldspace=1;
        }
        if (flags&kNodeDangly) raw.mesh->dangly.emplace();
    }
    // K1 MdlNodeLight 0x593664 and K2 0x6f4070 have identical authored defaults.
    if (flags&kNodeLight) {
        auto& light=raw.node.light.emplace();
        light.dynamicType=1;
        light.dynamic=true;
        light.affectDynamic=true;
        light.castsShadow=true;
        light.priority=5;
        light.fading=true;
    }
    // MdlNodeEmitter constructors zero the shared authored extension.
    if (flags&kNodeEmitter) raw.node.emitter.emplace();
    if (flags&kNodeCamera) raw.node.camera.emplace();
    if (flags&kNodeReference) raw.node.reference.emplace();
    if (flags&kNodeTrigger) raw.node.trigger=TriggerNodeData{true,false,false};
    if (flags&kNodeLightsaber) raw.node.lightsaber.emplace();
    if (flags&kNodeAabb) raw.node.aabb.emplace();
    if (flags&kNodeAnimation) raw.node.animmesh.emplace();
}

bool parseNodeValue(const Line& line, Reader& reader, RawNode& raw) {
    if(raw.node.flags==0xa1u && animMeshField(line,reader,raw)) return true;
    if (raw.node.light && lightField(line,reader,*raw.node.light)) return true;
    if (raw.node.emitter && emitterField(line,*raw.node.emitter)) return true;
    const auto field=lower(line.tokens[0]);
    if (raw.node.reference) {
        if (field=="refmodel") {
            directString(line,raw.node.reference->model,raw.node.reference->asciiModel);return true;
        }
        if (field=="reattachable") {directInteger(line,raw.node.reference->reattachable);return true;}
    }
    if (raw.mesh && raw.mesh->dangly) {
        auto& dangly=*raw.mesh->dangly;
        if (field=="displacement") {directFloat(line,dangly.displacement);return true;}
        if (field=="period") {directFloat(line,dangly.period);return true;}
        if (field=="tightness") {directFloat(line,dangly.tightness);return true;}
        if (field=="constraints") {floatList(line,reader,raw.constraints);return true;}
    }
    if (raw.node.aabb && field=="aabb") {
        auto& tree=*raw.node.aabb;
        tree.nodes.clear();
        tree.root=static_cast<std::size_t>(parseAabb(line,1u,reader,tree));
        tree.valid=true;
        return true;
    }
    return false;
}

bool parseNodeField(const Line& header, Reader& reader, RawNode& raw, GameVersion game) {
    const auto offset=header.bufferOffset;
    bool parsed=false;
    const auto controllers=[&](std::uint16_t flag) {
        for(const auto& field:controllerFields) if(field.nodeFlag==flag) {
            const auto line=reader.fieldLine(offset);
            parsed=parseControllerField(line,reader,raw,game,field.name,false)||parsed;
        }
    };
    if(raw.node.light) {
        controllers(kNodeLight);
        // Engine Light re-reads its retained resource-buffer pointer after
        // each array, then delegates to Base. Earlier stages are not retried.
        static constexpr std::string_view fields[]{
            "ambientonly","shadow","isdynamic","ndynamictype","affectdynamic",
            "lightpriority","generateflare","texturenames","flaresizes",
            "flarepositions","flarecolorshifts","flareradius","fadinglight"};
        for(const auto field:fields) {
            const auto line=reader.fieldLine(offset);
            if(equal(line.tokens.front(),field))
                parsed=lightField(line,reader,*raw.node.light)||parsed;
        }
    } else if(raw.node.emitter) {
        parsed=emitterField(reader.fieldLine(offset),*raw.node.emitter);
        controllers(kNodeEmitter);
    } else if(raw.mesh && raw.mesh->dangly) {
        for(const auto field:{"displacement","period","tightness","constraints"}) {
            const auto line=reader.fieldLine(offset);
            if(equal(line.tokens.front(),field)) parsed=parseNodeValue(line,reader,raw)||parsed;
        }
    } else {
        parsed=parseNodeValue(reader.fieldLine(offset),reader,raw);
    }
    return parsed;
}
} // namespace neomdl::model::ascii
