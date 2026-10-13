#include <model/AsciiExportInternal.hpp>
#include <model/AsciiControllerFields.hpp>
#include <model/AsciiModelInternal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace neomdl::model::ascii_export {
namespace {

float wordFloat(std::uint32_t word) noexcept {
    float value;
    std::memcpy(&value,&word,sizeof(value));
    return value;
}

std::uint32_t orderedFloat(float value) noexcept {
    std::uint32_t word;
    std::memcpy(&word,&value,sizeof(word));
    return word^((word&0x80000000u)!=0u?0xffffffffu:0x80000000u);
}

float orderedValue(std::uint32_t ordered) noexcept {
    return wordFloat(ordered^((ordered&0x80000000u)!=0u?0x80000000u:0xffffffffu));
}

std::uint32_t quantizedComponent(float component,float maximum) noexcept {
    const float value=((component+1.0f)*maximum)*0.5f;
    return static_cast<std::uint32_t>(std::max(0.0f,std::min(maximum,value)));
}

std::uint32_t packedQuaternion(Quaternion value) noexcept {
    if(value.w<0.0f) {value.x=-value.x;value.y=-value.y;value.z=-value.z;}
    return quantizedComponent(value.x,2046.0f) |
        (quantizedComponent(value.y,2046.0f)<<11u) |
        (quantizedComponent(value.z,1022.0f)<<22u);
}

struct ComponentInterval {float lower,upper;};

struct AxisAngle {Vec3 axis;float angle;};

AxisAngle axisAngle(Quaternion value) {
    const double length=std::sqrt(double(value.x)*value.x+
        double(value.y)*value.y+double(value.z)*value.z);
    return {length==0.0?Vec3{1.0f,0.0f,0.0f}:
        Vec3{float(value.x/length),float(value.y/length),float(value.z/length)},
        static_cast<float>(2.0*std::atan2(length,double(value.w)))};
}

std::string axisAngleText(Writer& writer,AxisAngle value) {
    return writer.vector(value.axis)+" "+writer.scalar(value.angle);
}

double rotationDistance(Quaternion left,Quaternion right) {
    std::array<double,4> a{{left.x,left.y,left.z,left.w}},
        b{{right.x,right.y,right.z,right.w}};
    double aa=0.0,bb=0.0,ab=0.0;
    for(std::size_t i=0;i<a.size();++i) {
        aa+=a[i]*a[i];bb+=b[i]*b[i];ab+=a[i]*b[i];
    }
    const double scaleA=1.0/std::sqrt(aa),scaleB=(ab<0.0?-1.0:1.0)/std::sqrt(bb);
    double squared=0.0;
    for(std::size_t i=0;i<a.size();++i) {
        const double difference=a[i]*scaleA-b[i]*scaleB;
        squared+=difference*difference;
    }
    return squared;
}

ComponentInterval componentInterval(std::uint32_t bin,float maximum) noexcept {
    const auto boundary=[&](bool after) {
        auto first=orderedFloat(-1.0f),last=orderedFloat(1.0f)+1u;
        while(first<last) {
            const auto middle=first+(last-first)/2u;
            const auto value=quantizedComponent(orderedValue(middle),maximum);
            if(value<bin || (after && value==bin))first=middle+1u;
            else last=middle;
        }
        return first;
    };
    return {orderedValue(boundary(false)),orderedValue(boundary(true)-1u)};
}

std::optional<AxisAngle> packedAxisAngle(GameVersion game,std::uint32_t word) {
    const std::array<std::uint32_t,3> bins{{word&0x7ffu,(word>>11u)&0x7ffu,word>>22u}};
    if(bins[0]>2046u || bins[1]>2046u || bins[2]>1022u)return {};

    const auto encode=[&](Quaternion value)->std::optional<AxisAngle> {
        auto result=axisAngle(value);
        auto reconstructed=ascii::engineAxisAngle(result.axis,result.angle);
        if(packedQuaternion(reconstructed)==word)return result;
        // Rounding pi upward changes the sign used by CompressQuaternionKey.
        if(reconstructed.w<0.0f) {
            result.angle=std::nextafter(result.angle,0.0f);
            reconstructed=ascii::engineAxisAngle(result.axis,result.angle);
            if(packedQuaternion(reconstructed)==word)return result;
        }
        return {};
    };
    if(auto value=encode(unpackEngineControllerQuaternion(word,game)))return value;

    const std::array<ComponentInterval,3> intervals{{componentInterval(bins[0],2046.0f),
        componentInterval(bins[1],2046.0f),componentInterval(bins[2],1022.0f)}};
    std::array<double,3> middle{},nearest{};
    double middleLength=0.0,nearestLength=0.0;
    for(std::size_t i=0;i<intervals.size();++i) {
        middle[i]=(double(intervals[i].lower)+intervals[i].upper)*0.5;
        nearest[i]=intervals[i].lower>0.0f?intervals[i].lower:
            (intervals[i].upper<0.0f?intervals[i].upper:0.0f);
        middleLength+=middle[i]*middle[i];
        nearestLength+=nearest[i]*nearest[i];
    }
    const auto point=[&](const std::array<double,3>& components)->std::optional<AxisAngle> {
        Quaternion value{float(components[0]),float(components[1]),float(components[2]),0.0f};
        const double squared=double(value.x)*value.x+double(value.y)*value.y+
            double(value.z)*value.z;
        if(squared>1.0)return {};
        value.w=static_cast<float>(std::sqrt(1.0-squared));
        return encode(value);
    };
    if(middleLength<=1.0) {
        if(auto text=point(middle))return text;
    } else if(nearestLength<1.0) {
        // Intersect the cell's nearest-to-zero point and midpoint with the
        // unit sphere, then select the interior of the feasible segment.
        double a=0.0,b=0.0;
        for(std::size_t i=0;i<middle.size();++i) {
            const double direction=middle[i]-nearest[i];
            a+=direction*direction;
            b+=2.0*nearest[i]*direction;
        }
        const double c=nearestLength-1.0;
        const double fraction=-c/(b+std::sqrt(b*b-4.0*a*c));
        std::array<double,3> interior{};
        for(std::size_t i=0;i<interior.size();++i)
            interior[i]=nearest[i]+fraction*(middle[i]-nearest[i]);
        if(auto text=point(interior))return text;
    }
    return point(nearest);
}

AxisAngle approximatePackedAxisAngle(GameVersion game,std::uint32_t word) {
    const auto target=unpackEngineControllerQuaternion(word,game);
    auto result=axisAngle(target);
    const auto original=ascii::engineAxisAngle(result.axis,result.angle);
    const auto x=word&0x7ffu,y=(word>>11u)&0x7ffu,z=word>>22u;
    // At a half-turn the opposite quaternion is the same rotation, but its
    // quantization cell can intersect the unit sphere when this one cannot.
    if(target.w==0.0f) {
        // Spare component codes decode through normalization but cannot be
        // emitted. Their opposite lies at the writable endpoint.
        const auto mirrored=(2046u-std::min(x,2046u))|
            ((2046u-std::min(y,2046u))<<11u)|((1022u-std::min(z,1022u))<<22u);
        if(auto candidate=packedAxisAngle(game,mirrored)) {
            const auto previous=unpackEngineControllerQuaternion(packedQuaternion(original),game);
            if(rotationDistance(target,unpackEngineControllerQuaternion(mirrored,game))<
               rotationDistance(target,previous)) {
                result=*candidate;
                const auto rebuilt=ascii::engineAxisAngle(result.axis,result.angle);
                const double dot=double(original.x)*rebuilt.x+double(original.y)*rebuilt.y+
                    double(original.z)*rebuilt.z+double(original.w)*rebuilt.w;
                if(dot<0.0) {
                    // Keep the previous input hemisphere for quaternion-key culling.
                    const AxisAngle equivalent{{-result.axis.x,-result.axis.y,-result.axis.z},
                        static_cast<float>(2.0*std::acos(-1.0)-result.angle)};
                    if(packedQuaternion(ascii::engineAxisAngle(equivalent.axis,equivalent.angle))==mirrored)
                        result=equivalent;
                }
            }
        }
    }
    return result;
}

std::string controllerDescription(const Node& node,const Controller& controller) {
    return "ASCII export node '"+node.name+"' controller "+std::to_string(controller.type);
}

const ascii::ControllerField* controllerField(const Controller& controller,std::uint16_t fieldFlags) {
    for(const auto& field:ascii::controllerFields)
        if(field.type==controller.type && (fieldFlags&field.nodeFlag)!=0u) return &field;
    return nullptr;
}

std::size_t wordsPerRow(const Controller& controller) {
    // NewController's low nibble is its value representation; bit 0x10
    // selects the Bezier records. AurGenericKey text has ten
    // slots including time, admitting scalar and vector Bezier records.
    switch(controller.rawFlags) {
    case 0x01u: case 0x02u:return 1u;
    case 0x03u: case 0x11u:return 3u;
    case 0x04u:return 4u;
    case 0x13u:return 9u;
    default:return 0u;
    }
}

std::string rowValues(Writer& writer,const Node& node,const Controller& controller,
                      std::size_t row,std::size_t width,bool keyed=false) {
    const auto start=static_cast<std::size_t>(controller.dataIndex)+row*width;
    if(!writer.advanced()) {
        if(controller.rawFlags==0x02u) {
            if(keyed) {
                if(auto value=packedAxisAngle(writer.game(),node.controllerDataWords[start]))
                    return axisAngleText(writer,*value);
                writer.warning("Some packed rotation keys could not be reproduced by the ASCII "
                    "axis-angle constructor; their exported rotations can change stored words.");
                return axisAngleText(writer,approximatePackedAxisAngle(writer.game(),
                    node.controllerDataWords[start]));
            }
            return writer.quaternion(unpackEngineControllerQuaternion(
                node.controllerDataWords[start],writer.game()));
        }
        if(controller.rawFlags==0x04u)
            return writer.quaternion({wordFloat(node.controllerDataWords[start]),
                wordFloat(node.controllerDataWords[start+1u]),
                wordFloat(node.controllerDataWords[start+2u]),
                wordFloat(node.controllerDataWords[start+3u])});

    }
    // Preserve the engine row layout verbatim, including Bezier value and
    // tangent words. No evaluation, key sorting or resampling occurs here.
    std::string values;
    for(std::size_t column=0;column<width;++column) {
        if(column!=0u)values+=' ';
        values+=writer.scalar(wordFloat(node.controllerDataWords[start+column]));
    }
    return values;
}

void writeStandardControllers(Writer& writer,const Node& node,std::uint16_t fieldFlags) {
    for(const auto& controller:node.controllers) {
        writer.job().check();
        const auto* named=controllerField(controller,fieldFlags);
        if(!named)throw ModelError(controllerDescription(node,controller)+" has no Standard ASCII field for its node type; use Advanced ASCII");
        const auto& field=*named;
        const auto width=wordsPerRow(controller);
        if(width==0u)throw ModelError(controllerDescription(node,controller)+" has no Standard ASCII encoding; use Advanced ASCII");
        const auto rows=static_cast<std::size_t>(controller.rowCount);
        if(rows==0u)
            throw ModelError(controllerDescription(node,controller)+
                " has zero rows; the ASCII builder would omit this controller");
        const auto times=static_cast<std::size_t>(controller.timeIndex);
        const auto data=static_cast<std::size_t>(controller.dataIndex);
        if(times>node.controllerDataWords.size() || rows>node.controllerDataWords.size()-times ||
           data>node.controllerDataWords.size() || rows*width>node.controllerDataWords.size()-data)
            throw ModelError(controllerDescription(node,controller)+
                " references words outside its retained controller data");

        const bool bezier=(controller.rawFlags&0x10u)!=0u;
        const std::string name(field.name);
        // A scalar position is a defined partial header write. A short
        // orientation is defined only for a zero axis: the engine parser
        // stores identity before reading the absent angle. Preserve those
        // direct fields so keyed spelling cannot introduce a base offset.
        const bool shortZeroAxis=field.type==kControllerOrientation &&
            (controller.rawFlags==0x01u || controller.rawFlags==0x03u) &&
            wordFloat(node.controllerDataWords[data])==0.0f &&
            (controller.rawFlags==0x01u ||
             (wordFloat(node.controllerDataWords[data+1u])==0.0f &&
              wordFloat(node.controllerDataWords[data+2u])==0.0f));
        const bool directShape=field.nodeFlag!=kNodeBase ||
            (field.type!=kControllerPosition && field.type!=kControllerOrientation) ||
            (field.type==kControllerPosition &&
                (controller.rawFlags==0x01u || controller.rawFlags==0x03u)) ||
            shortZeroAxis ||
            (field.type==kControllerOrientation &&
                (controller.rawFlags==0x02u || controller.rawFlags==0x04u));
        // Single-value fields imply time +0 and no source offset. Position
        // keys retain the engine +0x10 base-position input even with one row
        // at +0; spelling those as a direct field changes their meaning.
        // Preserve other times, including negative zero, as explicit keys.
        if(directShape && controller.sourceOffset==-1 && !bezier && rows==1u &&
           node.controllerDataWords[times]==0u) {
            writer.line(name+" "+rowValues(writer,node,controller,0u,width));
            continue;
        }
        // Scalar/vector Control adds the MdlNode source selected by this
        // offset. Engine keyed fields have fixed offsets (position +16,
        // orientation +28, generic fields -1). For example, spelling an
        // absolute compiled position key as positionkey would add the static
        // header to every sample. A direct field cannot retain multiple rows
        // or a time other than +0, so do not silently change that meaning.
        // Quaternion kinds have a different consumer that ignores this offset.
        if((controller.rawFlags==0x01u || controller.rawFlags==0x03u ||
            controller.rawFlags==0x11u || controller.rawFlags==0x13u) &&
           controller.sourceOffset!=field.keyedSourceOffset)
            throw ModelError(controllerDescription(node,controller)+
                " cannot preserve source offset "+std::to_string(controller.sourceOffset)+
                " with a keyed ASCII field requiring "+std::to_string(field.keyedSourceOffset));
        writer.line(name+(bezier?"bezierkey ":"key ")+std::to_string(rows));
        for(std::size_t row=0;row<rows;++row) {
            writer.job().check();
            writer.line(writer.scalar(wordFloat(node.controllerDataWords[times+row]))+
                " "+rowValues(writer,node,controller,row,width,true));
        }
    }
}

} // namespace

void writeControllers(Writer& writer,const Node& node,std::uint16_t fieldFlags) {
    if(!writer.advanced()) {writeStandardControllers(writer,node,fieldFlags);return;}
    const auto needsRecord=[&](const Controller& c) {
        const auto* field=controllerField(c,fieldFlags);
        const bool quaternion=c.rawFlags==2u || c.rawFlags==4u;
        const bool rotation=c.type==kControllerOrientation;
        const bool direct=c.sourceOffset==-1 && c.rowCount==1u &&
            (c.rawFlags&0x10u)==0u && node.controllerDataWords.at(c.timeIndex)==0u;
        return !field || wordsPerRow(c)==0u || c.rowCount==0u || rotation!=quaternion ||
            (!rotation && !direct && c.sourceOffset!=field->keyedSourceOffset);
    };
    const bool records=std::any_of(node.controllers.begin(),node.controllers.end(),needsRecord);
    if(records) {
        writer.line("controllerdata "+std::to_string(node.controllerDataWords.size()));
        for(const auto word:node.controllerDataWords) writer.line(std::to_string(word));
    }
    for(const auto& controller:node.controllers) {
        writer.job().check();
        if(records) {
            writer.line("controllerrecord "+std::to_string(controller.type)+" "+
                std::to_string(controller.sourceOffset)+" "+std::to_string(controller.rowCount)+" "+
                std::to_string(controller.timeIndex)+" "+std::to_string(controller.dataIndex)+" "+
                std::to_string(controller.rawFlags));
            continue;
        }
        const auto* named=controllerField(controller,fieldFlags);
        const auto width=wordsPerRow(controller);
        const bool rotation=controller.type==kControllerOrientation;
        const auto& field=*named;
        const auto rows=static_cast<std::size_t>(controller.rowCount);
        const auto times=static_cast<std::size_t>(controller.timeIndex);
        const auto data=static_cast<std::size_t>(controller.dataIndex);
        if(times>node.controllerDataWords.size() || rows>node.controllerDataWords.size()-times ||
           data>node.controllerDataWords.size() || rows*width>node.controllerDataWords.size()-data)
            throw ModelError(controllerDescription(node,controller)+
                " references words outside its retained controller data");

        if(rotation) {
            const bool packed=controller.rawFlags==2u;
            writer.line(std::string(packed?"orientationpackedkey ":"orientationquatkey ")+
                std::to_string(rows)+" "+std::to_string(controller.sourceOffset));
            for(std::size_t row=0;row<rows;++row)
                writer.line(writer.scalar(wordFloat(node.controllerDataWords[times+row]))+" "+
                    (packed?std::to_string(node.controllerDataWords[data+row]):
                        rowValues(writer,node,controller,row,width)));
            continue;
        }
        const bool bezier=(controller.rawFlags&0x10u)!=0u;
        const std::string name(field.name);
        if(controller.sourceOffset==-1 && !bezier && rows==1u && node.controllerDataWords[times]==0u) {
            writer.line(name+" "+rowValues(writer,node,controller,0u,width));
            continue;
        }
        writer.line(name+(bezier?"bezierkey ":"key ")+std::to_string(rows));
        for(std::size_t row=0;row<rows;++row) {
            writer.job().check();
            writer.line(writer.scalar(wordFloat(node.controllerDataWords[times+row]))+
                " "+rowValues(writer,node,controller,row,width));
        }
    }
}

} // namespace neomdl::model::ascii_export
