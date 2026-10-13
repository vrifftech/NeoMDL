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

void headerTransforms(Writer& writer,const Node& node) {
    writer.line("nodeposition "+writer.vector(node.position));
    const auto& q=node.orientation;
    writer.line("nodeorientation "+writer.vector({q.x,q.y,q.z})+" "+writer.scalar(q.w));
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
        writeControllers(writer,node,flags);
        if(!animation)headerTransforms(writer,node);
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
    const auto animationCount=options.includeAnimations?model.animations.size():0u;
    job.update(0u,animationCount+1u,"Exporting ASCII model: "+name);
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
    if(model.compressQuaternions)
        writer.line("compressquaternions "+std::to_string(*model.compressQuaternions));
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
    for(std::size_t i=0;i<animationCount;++i) {
        const auto& animation=model.animations[i];
        job.update(i+1u,animationCount+1u,"Exporting animation: "+animation.name);
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
    job.update(animationCount+1u,animationCount+1u,"ASCII model export complete");
    return writer.finish();
}

} // namespace neomdl::model
