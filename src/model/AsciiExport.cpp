#include <model/AsciiExportInternal.hpp>
#include <model/AsciiModelInternal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_map>

namespace neomdl::model::ascii_export {

Writer::Writer(GameVersion game,const neoshared::bif::JobControl& job,const AsciiExportOptions& options)
    :game_(game),job_(job),options_(options) {}

void Writer::explicitNormalNote() {
    warning("Explicit final normals are a NeoMDL ASCII extension. Remove normals and normalindices to recalculate from geometry and smoothing groups.");
}

void Writer::line(const std::string& text) {
    job_.check();
    if(!text.empty())result_.text.append(indent_,' ');
    result_.text+=text;
    result_.text+='\n';
}

void Writer::warning(const std::string& text) {
    if(std::find(result_.warnings.begin(),result_.warnings.end(),text)!=result_.warnings.end())return;
    result_.warnings.push_back(text);
}

std::string Writer::scalar(float value) const {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream<<std::setprecision(std::numeric_limits<float>::max_digits10)<<value;
    return stream.str();
}

std::string Writer::scalarDouble(double value) const {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream<<std::setprecision(std::numeric_limits<double>::max_digits10)<<value;
    return stream.str();
}

std::string Writer::vector(Vec3 value) const {
    return scalar(value.x)+" "+scalar(value.y)+" "+scalar(value.z);
}

std::string Writer::quaternion(Quaternion value) {
    // Authoring inverse of Quaternion(Vector,angle). A engine rotation key
    // stores a quaternion, while the text grammar consumes an axis and radians.
    const double axisLength=std::sqrt(double(value.x)*value.x+double(value.y)*value.y+double(value.z)*value.z);
    if(!std::isfinite(axisLength)||!std::isfinite(value.w))
        throw ModelError("ASCII export cannot express a non-finite rotation as an axis and angle");
    warning("Rotations are generated as axis-angle values; engine floating-point conversion and quaternion key compression can change their stored words.");
    if(axisLength==0.0) {
        if(value.w==0.0f)throw ModelError("ASCII export cannot express a zero quaternion as a rotation");
        return value.w<0.0f?"1 0 0 "+scalar(static_cast<float>(2.0*std::acos(-1.0))):"1 0 0 0";
    }
    return vector({static_cast<float>(value.x/axisLength),static_cast<float>(value.y/axisLength),
                   static_cast<float>(value.z/axisLength)})+" "+
        scalar(static_cast<float>(2.0*std::atan2(axisLength,double(value.w))));
}

std::string Writer::token(const std::string& value,const std::string& context) const {
    // Engine %s fields have no quote/escape syntax. Inventing one would
    // silently change names or split an exported field into multiple tokens.
    if(value.empty()||value.find_first_of(" \t\r\n\v\f\0",0u,7u)!=std::string::npos)
        throw ModelError("ASCII export cannot represent "+context+" as one engine text token");
    return value;
}

AsciiExportResult Writer::finish() {
    // Engine counted arrays consume physical lines. Diagnostics belong before
    // the command stream, never between a list header and its rows.
    std::string notes;
    for(const auto& warning:result_.warnings) {
        std::istringstream lines(warning);
        std::string part;
        while(std::getline(lines,part))notes+="# Export note: "+part+'\n';
    }
    result_.text.insert(0u,notes);
    return std::move(result_);
}

namespace {

std::string folded(std::string text) {
    for(auto& c:text)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
    return text;
}

void commandLiteral(const std::string& value,const std::string& context) {
    // FuncInterp evaluates unquoted brackets before locating the callback.
    // Paired double quotes remain literal bytes but shield their contents.
    // Its unterminated-quote path can scan beyond the terminator, so it cannot
    // provide a defined representation of an arbitrary stored name.
    for(std::size_t i=0;i<value.size();++i) {
        if(value[i]=='"') {
            i=value.find('"',i+1u);
            if(i==std::string::npos)
                throw ModelError("ASCII export cannot represent "+context+" with an unterminated engine command quote");
        } else if(value[i]=='[')
            throw ModelError("ASCII export cannot represent "+context+" containing a engine command expression");
    }
}

std::string nodeType(std::uint16_t flags) {
    switch(flags) {
    case kNodeBase:return "dummy";
    case kNodeBase|kNodeLight:return "light";
    case kNodeBase|kNodeEmitter:return "emitter";
    case kNodeBase|kNodeCamera:return "camera";
    case kNodeBase|kNodeReference:return "reference";
    case kNodeBase|kNodeMesh:return "trimesh";
    case kNodeBase|kNodeMesh|kNodeSkin:return "skin";
    case kNodeBase|kNodeMesh|kNodeAnimation:return "animmesh";
    case kNodeBase|kNodeMesh|kNodeDangly:return "danglymesh";
    case kNodeBase|kNodeMesh|kNodeAabb:return "aabb";
    case kNodeBase|kNodeMesh|kNodeLightsaber:return "lightsaber";
    default:throw ModelError("ASCII export has no engine node type for flags "+std::to_string(flags));
    }
}

const Controller* directController(const Node& node,std::uint32_t type) {
    const Controller* result=nullptr;
    for(const auto& controller:node.controllers)
        if(controller.type==type && controller.sourceOffset==-1 && controller.rowCount==1u &&
           (controller.rawFlags&0x10u)==0u && controller.timeIndex<node.controllerDataWords.size() &&
           node.controllerDataWords[controller.timeIndex]==0u) {
            if(type==kControllerPosition && controller.rawFlags!=3u)continue;
            if(type==kControllerOrientation && controller.rawFlags!=2u && controller.rawFlags!=4u)continue;
            result=&controller;
        }
    return result;
}

float controllerFloat(const Node& node,std::size_t index) {
    const auto word=node.controllerDataWords.at(index);
    float value;
    std::memcpy(&value,&word,sizeof(value));
    return value;
}

void headerTransforms(Writer& writer,const Node& node) {
    // Keyed fields do not populate the static node header. Emit its nondefault
    // frame when no single-value field already supplies it. The engine syntax
    // couples these fields to time-zero controllers, so disclose that change.
    const auto differs=[](float header,float controller) {
        return header!=controller || (header==0.0f && std::signbit(header)!=std::signbit(controller));
    };
    // A scalar direct position is a defined partial header write. Replay the
    // original sequence component by component: a later scalar replaces X
    // while retaining Y/Z from an earlier vector or the +0 constructor.
    // Adding a full position field unnecessarily introduces another engine
    // controller and changes both the record list and its data pool.
    std::array<float,3> position{};
    std::array<bool,3> positionWritten{};
    for(const auto& controller:node.controllers) {
        if(controller.type!=kControllerPosition || controller.sourceOffset!=-1 ||
           controller.rowCount!=1u || (controller.rawFlags!=1u && controller.rawFlags!=3u) ||
           controller.timeIndex>=node.controllerDataWords.size() ||
           node.controllerDataWords[controller.timeIndex]!=0u) continue;
        const auto width=controller.rawFlags==3u?3u:1u;
        for(std::size_t lane=0;lane<width;++lane) {
            position[lane]=controllerFloat(node,static_cast<std::size_t>(controller.dataIndex)+lane);
            positionWritten[lane]=true;
        }
    }
    const std::array<float,3> storedPosition{node.position.x,node.position.y,node.position.z};
    bool added=false,positionConflict=false,positionNeedsHeader=false;
    for(std::size_t lane=0;lane<3u;++lane) {
        if(!differs(storedPosition[lane],position[lane])) continue;
        if(positionWritten[lane])positionConflict=true;
        else positionNeedsHeader=true;
    }
    if(positionNeedsHeader) {
        writer.line("position "+writer.vector(node.position));added=true;
    }
    if(!directController(node,kControllerOrientation) &&
       (node.orientation.x!=0.0f||node.orientation.y!=0.0f||node.orientation.z!=0.0f||node.orientation.w!=1.0f)) {
        writer.line("orientation "+writer.quaternion(node.orientation));added=true;
    }
    if(added)writer.warning("Node '"+node.name+"': static header transforms require time-zero fields in engine ASCII.");
    if(positionConflict)
        writer.warning("Node '"+node.name+"': its stored position header differs from the last direct controller for that component; engine ASCII derives each written component from its last direct field.");
    if(const auto* rotation=directController(node,kControllerOrientation)) {
        const auto at=rotation->dataIndex;
        const auto q=rotation->rawFlags==2u?unpackEngineControllerQuaternion(node.controllerDataWords.at(at),writer.game()):
            Quaternion{controllerFloat(node,at),controllerFloat(node,at+1u),controllerFloat(node,at+2u),controllerFloat(node,at+3u)};
        if(node.orientation.x!=q.x||node.orientation.y!=q.y||node.orientation.z!=q.z||node.orientation.w!=q.w)
            writer.warning("Node '"+node.name+"': its stored orientation header differs from the last direct controller; engine ASCII derives the header from that controller.");
    }
}

void classifications(Writer& writer,const Model& model) {
    const auto flags=model.classification;
    constexpr std::uint16_t supported=0x06ffu;
    if((flags&~supported)!=0u || ((flags&0x0400u)!=0u && (flags&0x0020u)==0u))
        throw ModelError("ASCII export cannot represent the model classification bits with engine commands");
    const std::pair<std::uint16_t,const char*> commands[]{
        {0x0001u,"effect"},{0x0002u,"forcedistortion"},{0x0004u,"character"},
        {0x0008u,"door"},{0x0010u,"lightsaber"},{0x0040u,"ambient"},{0x0080u,"heatdistortion"}};
    for(const auto& command:commands)if(flags&command.first)writer.line(std::string("classification ")+command.second);
    if(flags&0x0400u)writer.line("classification placeable");
    else if(flags&0x0020u)writer.line("classification item");
    if(flags&0x0200u)writer.line("secondaryclassification sensitive");
    if(model.subclassification!=0u)writer.warning("Compiled subclassification "+std::to_string(model.subclassification)+
        " has no command in the ASCII grammar.");
}

std::uint16_t animationFieldFlags(const Model& model,const Node& node) {
    if(node.flags!=kNodeBase)return node.flags;
    const auto key=folded(node.name);
    for(const auto& original:model.nodes)
        if(original.id==node.id && folded(original.name)==key)return original.flags;
    const Node* named=nullptr;
    for(const auto& original:model.nodes)if(folded(original.name)==key) {
        if(named)throw ModelError("ASCII export cannot resolve animation node type for ambiguous name '"+node.name+"'");
        named=&original;
    }
    if(named)return named->flags;
    // A base animation node can legitimately describe a superclass bone not
    // present in this asset. Only base controller keywords apply in that case.
    return kNodeBase;
}

void writeTree(Writer& writer,const Model& model,const std::vector<Node>& nodes,bool animation) {
    if(nodes.empty())throw ModelError("ASCII export requires the model/animation's engine root node");
    std::optional<std::size_t> root;
    for(std::size_t i=0;i<nodes.size();++i)if(!nodes[i].parent) {
        if(root)throw ModelError("ASCII export cannot retain multiple roots: parent NULL replaces the engine tree");
        root=i;
    }
    if(!root)throw ModelError("ASCII export cannot represent a tree without a root");
    std::vector<bool> emitted(nodes.size(),false);
    std::unordered_map<std::string,std::size_t> firstNames;
    std::function<void(std::size_t)> writeNode=[&](std::size_t index) {
        writer.job().check();
        if(index>=nodes.size()||emitted[index])throw ModelError("ASCII export found inconsistent engine tree links");
        const auto& node=nodes[index];
        const auto name=writer.token(node.name,"node name");
        const auto flags=animation?animationFieldFlags(model,node):node.flags;
        writer.indent(0);
        writer.line("node "+nodeType(flags)+" "+name);
        writer.indent(2);
        if(node.parent) {
            if(*node.parent>=nodes.size())throw ModelError("ASCII export parent index is outside its tree");
            const auto& parent=nodes[*node.parent];
            const auto found=firstNames.find(folded(parent.name));
            if(folded(parent.name)=="null"||found==firstNames.end()||found->second!=*node.parent)
                throw ModelError("ASCII export parent '"+parent.name+"' is ambiguous under engine name lookup");
            writer.line("parent "+writer.token(parent.name,"parent name"));
        } else writer.line("parent NULL");
        firstNames.emplace(folded(name),index);emitted[index]=true;
        if(!animation)headerTransforms(writer,node);
        writeControllers(writer,node,flags);
        if(!animation) {
            writeNodeFields(writer,model,node);
            if(node.mesh) {
                if(*node.mesh>=model.meshes.size())throw ModelError("ASCII export mesh index is outside its model");
                const auto& mesh=model.meshes[*node.mesh];
                const auto generated=writeMesh(writer,model,node,mesh);
                if(mesh.skinned)writeSkin(writer,model,mesh,generated);
            }
        } else if(node.animmesh) {
            // Typed animmesh animation nodes retain their own mesh extension;
            // their geometry is not an independently drawable model mesh.
            if(!node.animmesh->authoringMesh)
                throw ModelError("Cannot export animmesh animation node "+node.name+": its static authoring geometry is unavailable");
            writeMesh(writer,model,node,*node.animmesh->authoringMesh);
        }
        writer.indent(0);writer.line("endnode");writer.line();
        for(const auto child:node.children) {
            if(child>=nodes.size()||nodes[child].parent!=std::optional<std::size_t>(index))
                throw ModelError("ASCII export found inconsistent engine child/parent links");
            writeNode(child);
        }
    };
    writeNode(*root);
    if(std::find(emitted.begin(),emitted.end(),false)!=emitted.end())
        throw ModelError("ASCII export cannot retain nodes disconnected from the engine root");
}

} // namespace

void reconstructExportedNodeFrames(Model& model,const neoshared::bif::JobControl& job) {
    for(auto& node:model.nodes) {
        Writer writer(model.game,job);
        headerTransforms(writer,node);
        writeControllers(writer,node,node.flags);
        const auto text=writer.finish().text;
        const std::vector<std::uint8_t> bytes(text.begin(),text.end());
        ascii::Reader reader(bytes,model.game,node.name,job);
        ascii::RawNode raw;
        raw.node.flags=node.flags;
        raw.node.name=node.name;
        raw.modelClassification=model.classification;
        ascii::initializeNode(raw,model.game);
        ascii::Line line;
        while(reader.next(line))
            ascii::parseControllerField(line,reader,raw,model.game);
        node.position=raw.node.position;
        node.orientation=raw.node.orientation;
    }
}

} // namespace neomdl::model::ascii_export

namespace neomdl::model {

AsciiExportResult exportAsciiModel(const Model& model,const neoshared::bif::JobControl& job,
                                  const AsciiExportOptions& options) {
    using namespace ascii_export;
    Writer writer(model.game,job,options);
    const auto& name=model.name;
    // newmodel receives the complete command argument, not an sscanf token.
    // Empty names and internal whitespace are defined constructor inputs.
    // FuncInterp removes leading space/'=' and trailing space/tab; a physical
    // line break, NUL or overlong engine fixed field cannot retain the name.
    if(name.size()>=32u || name.find_first_of("\r\n\0",0u,3u)!=std::string::npos ||
       (!name.empty() && (name.front()==' ' || name.front()=='=' || name.back()==' ' || name.back()=='\t')))
        throw ModelError("ASCII export cannot represent the model name as a bounded engine command argument");
    commandLiteral(name,"model name");
    // setsupermodel scans and ignores its first token; newanim likewise ignores
    // the model target after its animation name. Keep those placeholders single
    // tokens so whitespace in the actual name cannot shift a later argument.
    const auto modelTarget=name.empty() || name.find_first_of(" \t\v\f\"[")!=std::string::npos
        ? std::string("_") : name;
    job.update(0u,model.animations.size()+1u,"Exporting ASCII model: "+name);
    if(model.collapsedRoomParts||!model.sceneNodeAdmission.empty())
        throw ModelError("ASCII export requires the source model asset, not a collapsed scene partition");
    writer.line("# NeoMDL generated ASCII model");
    writer.line(std::string("# Target game: ")+(model.game==GameVersion::Kotor1?"K1":"K2"));
    writer.line("# Compiled ordering and source information lost during construction cannot be recovered.");
    for(const auto& warning:model.warnings)writer.warning("Decoder: "+warning);




    if(model.asciiBuilt) {
        std::vector<bool> retained(model.asciiMdxAllocations.size(),false);
        for(const auto& mesh:model.meshes)
            if(mesh.asciiMdxAllocation)retained.at(*mesh.asciiMdxAllocation)=true;
        for(const auto& animation:model.animations)
            for(const auto& node:animation.nodes)
                if(node.animmesh && node.animmesh->authoringMesh && node.animmesh->authoringMesh->asciiMdxAllocation)
                    retained.at(*node.animmesh->authoringMesh->asciiMdxAllocation)=true;
        if(std::find(retained.begin(),retained.end(),false)!=retained.end())
            writer.warning("Discarded-root mesh allocations cannot be generated from the surviving geometry. Reload loses their engine MDX pool history, which participates in K1 procedural-UV admission.");
    }
    writer.line("newmodel "+name);
    // The constructor leaves this name empty (K2 0x6ed794); omitting the
    // command preserves that state. FindModel does not exclude NONE
    // (K1 Desktop 0x3341df..0x3341f1), so its normalized resource view is not
    // a source for the engine authoring identity.
    if(!model.engineSupermodelName.empty()) {
        if(model.engineSupermodelName.size()>=32u)
            throw ModelError("ASCII export cannot represent the supermodel name in its engine 32-byte field");
        commandLiteral(model.engineSupermodelName,"supermodel name");
        writer.line("setsupermodel "+modelTarget+" "+writer.token(model.engineSupermodelName,"supermodel"));
    }
    classifications(writer,model);
    writer.line("ignorefog "+std::to_string(model.fog?0:1));
    writer.line("setanimationscale "+writer.scalar(model.animationScale));
    writer.line("beginmodelgeom "+modelTarget);
    reconstructSkinSeamSmoothing(writer,model);
    writeTree(writer,model,model.nodes,false);
    if(model.engineBounds.valid) {
        writer.line("bmin "+writer.vector(model.engineBounds.minimum));
        writer.line("bmax "+writer.vector(model.engineBounds.maximum));
    }
    writer.line("radius "+writer.scalar(model.engineRadius));
    if(model.animationRootNode) {
        if(*model.animationRootNode>=model.nodes.size())throw ModelError("ASCII export animation root is outside its tree");
        writer.line("sModelAnimationRootName "+writer.token(model.nodes[*model.animationRootNode].name,"animation root"));
    } else
        writer.warning("The absent model animation-root selection cannot be preserved: engine ASCII load assigns the tree root, enabling animation controllers that compiled playback leaves disabled.");
    writer.line("endmodelgeom "+modelTarget);writer.line();
    for(std::size_t i=0;i<model.animations.size();++i) {
        const auto& animation=model.animations[i];
        job.update(i+1u,model.animations.size()+1u,"Exporting animation: "+animation.name);
        const auto clip=writer.token(animation.name,"animation name");
        if(clip.size()>=32u)
            throw ModelError("ASCII export cannot represent the animation name in its engine 32-byte field");
        if(clip.front()=='=')
            throw ModelError("ASCII export cannot represent an animation name beginning with the engine command delimiter '='");
        commandLiteral(clip,"animation name");
        writer.line("newanim "+clip+" "+modelTarget);
        writer.line("length "+writer.scalar(animation.length));
        writer.line("transtime "+writer.scalar(animation.transitionTime));
        if(!animation.root.empty())writer.line("animroot "+writer.token(animation.root,"animation root"));
        if(!std::is_sorted(animation.events.begin(),animation.events.end(),[](const auto& a,const auto& b){return a.time<b.time;}))
            writer.warning("Animation '"+animation.name+"': engine ASCII load sorts its out-of-order compiled events by time.");
        // The engine reader inserts before equal-time events. Reverse their
        // serialized order so an load retains the stored event order.
        for(auto event=animation.events.rbegin();event!=animation.events.rend();++event)
            writer.line("event "+writer.scalar(event->time)+(event->name.empty()?std::string{}:" "+writer.token(event->name,"event name")));
        writeTree(writer,model,animation.nodes,true);
        writer.line("doneanim "+clip+" "+modelTarget);writer.line();
    }
    writer.line("donemodel "+modelTarget);
    job.update(model.animations.size()+1u,model.animations.size()+1u,"ASCII model export complete");
    return writer.finish();
}

} // namespace neomdl::model
