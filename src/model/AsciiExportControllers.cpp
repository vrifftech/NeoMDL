#include <model/AsciiExportInternal.hpp>
#include <model/AsciiControllerFields.hpp>

#include <algorithm>
#include <cstring>

namespace neomdl::model::ascii_export {
namespace {

float wordFloat(std::uint32_t word) noexcept {
    float value;
    std::memcpy(&value,&word,sizeof(value));
    return value;
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
                      std::size_t row,std::size_t width) {
    const auto start=static_cast<std::size_t>(controller.dataIndex)+row*width;
    // Preserve the engine row layout verbatim, including Bezier value and
    // tangent words. No evaluation, key sorting or resampling occurs here.
    std::string values;
    for(std::size_t column=0;column<width;++column) {
        if(column!=0u)values+=' ';
        values+=writer.scalar(wordFloat(node.controllerDataWords[start+column]));
    }
    return values;
}

} // namespace

void writeControllers(Writer& writer,const Node& node,std::uint16_t fieldFlags) {
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
