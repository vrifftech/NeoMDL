#include <model/AsciiModelInternal.hpp>
#include <model/AsciiControllerFields.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace neomdl::model::ascii {
namespace {

std::uint32_t bits(float value) noexcept {
    std::uint32_t result;
    std::memcpy(&result,&value,sizeof(result));
    return result;
}

float asFloat(std::uint32_t value) noexcept {
    float result;
    std::memcpy(&result,&value,sizeof(result));
    return result;
}

std::int32_t asSignedWord(std::uint32_t value) noexcept {
    std::int32_t result;
    std::memcpy(&result,&value,sizeof(result));
    return result;
}

} // namespace

Quaternion engineAxisAngle(Vec3 axis,float angle) {
    // Quaternion(Vector,float): K1 0x668870; K2 0x83b430. The tiny-axis
    // replacement and angle-zero branch belong to the engine constructor.
    if(angle==0.0f) return {};
    float x=axis.x,y=axis.y,z=axis.z;
    const float length=std::sqrt((x*x+y*y)+z*z);
    if(static_cast<double>(length)<1.0e-9) {
        x=1.0f;y=0.0f;z=0.0f;
    } else {
        const float inverse=1.0f/length;
        x*=inverse;y*=inverse;z*=inverse;
    }
    const float halfAngle=angle*0.5f;
    const float sine=std::sin(halfAngle);
    return {x*sine,y*sine,z*sine,std::cos(halfAngle)};
}

namespace {

struct Key {
    std::array<std::uint32_t,10> words{};
    std::int32_t count{};
};

const char* fieldArguments(const Line& line) {
    const char* text=line.text.c_str();
    while(*text==' ' || *text=='\t') ++text;
    return text+line.tokens.front().size();
}

void convertQuaternion(Key& key,std::size_t first) {
    const auto q=engineAxisAngle({asFloat(key.words[first]),asFloat(key.words[first+1u]),
                                 asFloat(key.words[first+2u])},asFloat(key.words[first+3u]));
    key.words[first]=bits(q.x);key.words[first+1u]=bits(q.y);
    key.words[first+2u]=bits(q.z);key.words[first+3u]=bits(q.w);
}

void parseKey(const char* text,Key& key) {
    // Parse(AurGenericKey): ten %f conversions, stopping at the first token
    // not read by sscanf. Five columns mean time plus axis-angle.
    std::array<float,10> values{};
    key.count=std::sscanf(text,"%f %f %f %f %f %f %f %f %f %f",
        &values[0],&values[1],&values[2],&values[3],&values[4],
        &values[5],&values[6],&values[7],&values[8],&values[9]);
    for(std::int32_t i=0;i<key.count;++i) key.words[i]=bits(values[i]);
    if(key.count==5u) convertQuaternion(key,1u);
}

std::uint32_t littleWord(const std::vector<std::uint8_t>& bytes,std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset+1u])<<8u) |
        (static_cast<std::uint32_t>(bytes[offset+2u])<<16u) |
        (static_cast<std::uint32_t>(bytes[offset+3u])<<24u);
}

std::vector<Key> readKeys(const Line& line,Reader& reader) {
    // K2 Parse<AurGenericKey> 0x79da10 accepts counted text, endlist text,
    // and a count plus marker followed by raw 44-byte AurGenericKey records.
    // sscanf's second-conversion count selects raw input; it does not test
    // the marker's spelling.
    std::vector<Key> result;
    int count{};
    char marker{};
    const auto fields=std::sscanf(fieldArguments(line),"%d %c",&count,&marker);
    if(fields==2) {
        const auto bytes=reader.takeBytes(static_cast<std::size_t>(count)*44u,line);
        for(long i=0;i<count;++i) {
            Key key;
            const auto offset=static_cast<std::size_t>(i)*44u;



            key.count=asSignedWord(littleWord(bytes,offset+40u));
            for(std::size_t j=0;j<key.words.size();++j)
                key.words[j]=littleWord(bytes,offset+j*4u);
            result.push_back(key);
        }
        reader.finishPackedArray(line);
    } else if(fields==1) {
        for(long i=0;i<count;++i) {
            Line row;
            if(!reader.next(row)) reader.fail(line,"Incomplete controller key array");
            Key key;
            parseKey(row.text.c_str(),key);
            result.push_back(key);
        }
    } else {
        Line row;
        while(reader.next(row)) {
            // Engine list terminator uses case-sensitive strncmp(...,7).
            if(row.tokens.front().compare(0u,7u,"endlist")==0) break;



            auto key=result.empty()?Key{}:result.back();
            parseKey(row.text.c_str(),key);
            result.push_back(key);
        }
    }
    return result;
}

void cullQuaternionKeys(std::vector<Key>& keys) {
    // K2 CullUnnecessaryKeys 0x79d2b0 visits the interior triples in reverse
    // order, excluding the final key. Its default cullvalueforkeys is 1e-4.
    for(auto i=static_cast<std::ptrdiff_t>(keys.size())-2;i>=2;--i) {
        const auto index=static_cast<std::size_t>(i);
        bool same=true;
        for(std::size_t component=1u;component<=4u;++component) {
            const float center=asFloat(keys[index-1u].words[component]);
            if(!(std::abs(asFloat(keys[index].words[component])-center)<0.0001f) ||
               !(std::abs(asFloat(keys[index-2u].words[component])-center)<0.0001f)) {
                same=false;
                break;
            }
        }
        if(same) keys.erase(keys.begin()+static_cast<std::ptrdiff_t>(index-1u));
    }
}

void compressQuaternionKeys(std::vector<Key>& keys) {
    // K2 CompressQuaternionKey 0x79c7a0 makes w positive, quantizes x/y to
    // 11 bits and z to 10 bits, and retains the packed bits in the float pool.
    for(auto& key:keys) {
        float x=asFloat(key.words[1u]),y=asFloat(key.words[2u]),z=asFloat(key.words[3u]);
        if(asFloat(key.words[4u])<0.0f) {x=-x;y=-y;z=-z;}
        const auto quantize=[](float component,float maximum) {
            const float value=((component+1.0f)*maximum)*0.5f;
            // K2 minss(value,maximum) selects its second operand for NaN.
            // Embedded AurGenericKey rows can supply that value directly.
            return static_cast<std::uint32_t>(std::max(0.0f,std::min(maximum,value)));
        };
        key.words[1u]=quantize(x,2046.0f) | (quantize(y,2046.0f)<<11u) |
            (quantize(z,1022.0f)<<22u);
        key.count=2u;
    }
}

ControllerValueKind valueKind(std::uint8_t flags) noexcept {
    switch(flags&0x0fu) {
    case 1u:return ControllerValueKind::Float;
    case 2u:return ControllerValueKind::PackedQuaternion;
    case 3u:return ControllerValueKind::Vector;
    case 4u:return ControllerValueKind::Quaternion;
    default:return ControllerValueKind::Unknown;
    }
}

void appendController(Node& node,const ControllerField& field,const std::vector<Key>& keys,
                      std::uint8_t flags) {
    Controller controller;
    controller.type=field.type;
    // The keyed base position/orientation producers retain MdlNode +0x10/
    // +0x1c in the x86 record. Constants and derived fields use -1.
    controller.sourceOffset=field.keyedSourceOffset;
    controller.rowCount=static_cast<std::uint16_t>(keys.size());
    controller.timeIndex=static_cast<std::uint16_t>(node.controllerDataWords.size());
    controller.rawFlags=flags;
    controller.valueKind=valueKind(flags);
    controller.interpolation=(flags&0x10u)!=0u
        ? ControllerInterpolation::Bezier : ControllerInterpolation::Linear;
    for(const auto& key:keys) node.controllerDataWords.push_back(key.words[0u]);
    controller.dataIndex=static_cast<std::uint16_t>(node.controllerDataWords.size());
    for(const auto& key:keys)
        for(std::int32_t i=1;i<key.count;++i) {
            // A raw count of 11 copies the count word itself at +40 as
            // the last value; it is still inside the 44-byte record.
            node.controllerDataWords.push_back(i==static_cast<std::int32_t>(key.words.size())
                ? static_cast<std::uint32_t>(key.count) : key.words[i]);
        }
    node.controllers.push_back(controller);
}

} // namespace

void parseBaseTransformFields(const Line& line,Reader& reader,RawNode& raw) {
    const auto keyword=lower(line.tokens.front());
    const auto* arguments=fieldArguments(line);
    if(keyword=="nodeorientation") {
        raw.advancedAscii=true;
        auto& q=raw.node.orientation;
        std::sscanf(arguments,"%f %f %f %f",&q.x,&q.y,&q.z,&q.w);
    } else if(keyword=="position" || keyword=="nodeposition") {
        raw.advancedAscii|=keyword=="nodeposition";
        std::sscanf(arguments,"%f %f %f",&raw.node.position.x,
                    &raw.node.position.y,&raw.node.position.z);
    } else if(keyword=="orientation") {
        float x{},y{},z{},angle;
        const auto count=std::sscanf(arguments,"%f %f %f %f",&x,&y,&z,&angle);
        if(x==0.0f && y==0.0f && z==0.0f) raw.node.orientation={};
        else {
            if(count!=4) reader.fail(line,"Incomplete orientation record");
            raw.node.orientation=engineAxisAngle({x,y,z},angle);
        }
    }
}

bool parseControllerExtension(const Line& line,Reader& reader,RawNode& raw) {
    const auto field=lower(line.tokens.front());
    if(field=="controllerdata") {
        raw.controllerDataBase=raw.node.controllerDataWords.size();
        std::vector<std::uint32_t> words;
        readArray(line,reader,words,4u,
            [](const Line& row,const Reader&,std::uint32_t& value) {value=parseUInt(row.tokens[0]);},
            [](const std::uint8_t* p) {return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8u)|
                (std::uint32_t(p[2])<<16u)|(std::uint32_t(p[3])<<24u);});
        raw.node.controllerDataWords.insert(raw.node.controllerDataWords.end(),words.begin(),words.end());
    } else if(field=="controllerrecord") {
        Controller c;
        c.type=parseUInt(line.tokens[1]);c.sourceOffset=static_cast<std::int16_t>(parseInt(line.tokens[2]));
        c.rowCount=static_cast<std::uint16_t>(parseUInt(line.tokens[3]));
        c.timeIndex=static_cast<std::uint16_t>(raw.controllerDataBase+parseUInt(line.tokens[4]));
        c.dataIndex=static_cast<std::uint16_t>(raw.controllerDataBase+parseUInt(line.tokens[5]));
        c.rawFlags=static_cast<std::uint8_t>(parseUInt(line.tokens[6]));
        c.valueKind=valueKind(c.rawFlags);
        c.interpolation=(c.rawFlags&0xf0u)==0x10u?ControllerInterpolation::Bezier:
            (c.rawFlags&0xf0u)==0u?ControllerInterpolation::Linear:ControllerInterpolation::Unknown;
        raw.node.controllers.push_back(c);
    } else if(field=="orientationpackedkey" || field=="orientationquatkey") {
        const bool packed=field=="orientationpackedkey";
        const auto offset=static_cast<std::int16_t>(parseInt(line.tokens[2]));
        auto header=line;header.tokens.resize(2u);
        std::vector<Key> keys;
        readArray(header,reader,keys,0u,
            [&](const Line& row,const Reader&,Key& key) {
                key.words[0]=bits(parseFloat(row.tokens[0]));
                key.count=packed?2:5;
                for(int i=1;i<key.count;++i)
                    key.words[i]=packed?parseUInt(row.tokens[i]):bits(parseFloat(row.tokens[i]));
            },[](const std::uint8_t*) {return Key{};});
        std::uint8_t flags=packed?2u:4u;
        if(raw.compressQuaternions && *raw.compressQuaternions!=packed) {
            if(*raw.compressQuaternions) {compressQuaternionKeys(keys);flags=2u;}
            else {
                for(auto& key:keys) {
                    const auto q=unpackEngineControllerQuaternion(key.words[1],reader.game());
                    key.words[1]=bits(q.x);key.words[2]=bits(q.y);key.words[3]=bits(q.z);key.words[4]=bits(q.w);
                    key.count=5;
                }
                flags=4u;
            }
        }
        appendController(raw.node,{"orientation",kControllerOrientation,kNodeBase,offset},keys,flags);
    } else return false;
    raw.advancedAscii=true;
    return true;
}

bool parseControllerField(const Line& line,Reader& reader,RawNode& raw,GameVersion,
                          std::string_view group,bool updateBaseTransform) {
    const auto keyword=lower(line.tokens.front());
    for(const auto& field:controllerFields) {
        if(!group.empty() && field.name!=group) continue;
        if((raw.node.flags&field.nodeFlag)==0u) continue;
        const bool single=keyword==field.name;
        const bool linear=keyword==std::string(field.name)+"key";
        const bool bezier=keyword==std::string(field.name)+"bezierkey";
        if(!single && !linear && !bezier) continue;

        if(single) {
            const auto* arguments=fieldArguments(line);
            if(updateBaseTransform && field.nodeFlag==kNodeBase)
                parseBaseTransformFields(line,reader,raw);
            Key source;
            parseKey(arguments,source);
            const auto columns=source.count;
            if(columns==4u) convertQuaternion(source,0u);


            std::uint8_t flags=1u;
            if(columns==1u || columns==3u || columns==4u)
                flags=static_cast<std::uint8_t>(columns);
            // AddGenericControllerSingle prefixes an implicit time-zero
            // word to the parsed values (K1 0x5a1cd0, K2 0x79ce20).
            Controller controller;
            controller.type=field.type;controller.sourceOffset=-1;
            controller.rowCount=1u;
            controller.timeIndex=static_cast<std::uint16_t>(raw.node.controllerDataWords.size());
            raw.node.controllerDataWords.push_back(bits(0.0f));
            controller.dataIndex=static_cast<std::uint16_t>(raw.node.controllerDataWords.size());
            controller.rawFlags=flags;controller.valueKind=valueKind(flags);
            controller.interpolation=ControllerInterpolation::Linear;
            for(std::int32_t i=0;i<columns;++i) {



                raw.node.controllerDataWords.push_back(i==static_cast<std::int32_t>(source.words.size()-1u)
                    ? static_cast<std::uint32_t>(columns+1) : source.words[i]);
            }
            raw.node.controllers.push_back(controller);
            return true;
        }





        auto keys=readKeys(line,reader);
        if(keys.empty()) return true; // Both K2 builders return for an empty list.
        const auto columns=keys.front().count;
        std::uint8_t flags;
        if(bezier) {
            // AddBezierController copies each row's value/tangent words
            // verbatim and encodes (columnCount-1)/3 plus the Bezier bit.
            // DEC wraps the raw 32-bit count before signed division.
            const auto valueColumns=asSignedWord(static_cast<std::uint32_t>(columns)-1u);
            flags=static_cast<std::uint8_t>(
                (valueColumns/3)|0x10);
        } else {
            flags=columns==4u ? 3u : 1u;
            if(columns==5u) {
                cullQuaternionKeys(keys);
                flags=4u;
                if(raw.compressQuaternions.value_or((raw.modelClassification&0x0200u)==0u)) {
                    compressQuaternionKeys(keys);
                    flags=2u;
                }
            }
        }



        for(const auto& key:keys)
            if(key.count>static_cast<std::int32_t>(key.words.size()+1u))
                reader.fail(line,"AurGenericKey column count exceeds its 44-byte record");
        appendController(raw.node,field,keys,flags);
        return true;
    }
    return false;
}

} // namespace neomdl::model::ascii
