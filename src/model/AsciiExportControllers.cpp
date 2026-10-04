#include <model/AsciiExportInternal.hpp>
#include <model/AsciiControllerFields.hpp>

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

const ascii::ControllerField& controllerField(const Node& node,const Controller& controller,
                                               std::uint16_t fieldFlags) {
    for(const auto& field:ascii::controllerFields)
        if(field.type==controller.type && (fieldFlags&field.nodeFlag)!=0u) return field;
    throw ModelError(controllerDescription(node,controller)+" has no ASCII field for its node type");
}

std::size_t wordsPerRow(const Node& node,const Controller& controller) {
    // NewController's low nibble is its value representation; bit 0x10
    // selects the Bezier records. AurGenericKey text has ten
    // slots including time, admitting scalar and vector Bezier records.
    switch(controller.rawFlags) {
    case 0x01u: case 0x02u:return 1u;
    case 0x03u: case 0x11u:return 3u;
    case 0x04u:return 4u;
    case 0x13u:return 9u;
    default:throw ModelError(controllerDescription(node,controller)+
        " has an encoding that the ASCII controller grammar cannot represent ("+
        std::to_string(controller.rawFlags)+")");
    }
}

std::string rowValues(Writer& writer,const Node& node,const Controller& controller,
                      std::size_t row,std::size_t width) {
    const auto start=static_cast<std::size_t>(controller.dataIndex)+row*width;
    if(controller.rawFlags==0x02u)
        return writer.quaternion(unpackEngineControllerQuaternion(
            node.controllerDataWords[start],writer.game()));
    if(controller.rawFlags==0x04u)
        return writer.quaternion({wordFloat(node.controllerDataWords[start]),
            wordFloat(node.controllerDataWords[start+1u]),
            wordFloat(node.controllerDataWords[start+2u]),
            wordFloat(node.controllerDataWords[start+3u])});

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
    for(const auto& controller:node.controllers) {
        writer.job().check();
        const auto& field=controllerField(node,controller,fieldFlags);
        const auto width=wordsPerRow(node,controller);
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
        // Quaternion kinds have a different consumer that ignores this
        // offset; their existing warned axis-angle conversion is unchanged.
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
                " "+rowValues(writer,node,controller,row,width));
        }
    }
}

} // namespace neomdl::model::ascii_export
