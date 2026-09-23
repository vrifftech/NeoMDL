#include <model/AsciiModelInternal.hpp>
#include <model/UvAnimation.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <sstream>

namespace neomdl::model::ascii {

Reader::Reader(const std::vector<std::uint8_t>& bytes, GameVersion game, const std::string& sourceLabel,
               const neoshared::bif::JobControl& job,
               const AsciiResourceResolver& resources)
    : bytes_(bytes), game_(game), sourceLabel_(sourceLabel), job_(job), resources_(resources) {}

bool Reader::next(Line& line) {
    job_.check();
    if(cursor_==bytes_.size()) return false;
    line.number=++lineNumber_;
    line.tokens.clear();
    const auto start=cursor_;



    while(cursor_<bytes_.size() && bytes_[cursor_]!='\n' &&
          (game_!=GameVersion::Kotor1 || bytes_[cursor_]!='\r')) ++cursor_;
    const auto end=cursor_;
    line.text.assign(reinterpret_cast<const char*>(bytes_.data()+start),end-start);
    if(const auto terminator=line.text.find('\0');terminator!=std::string::npos)
        line.text.resize(terminator);
    if(cursor_<bytes_.size()) {
        const auto terminator=bytes_[cursor_++];
        if(terminator=='\r' && cursor_<bytes_.size() && bytes_[cursor_]=='\n') ++cursor_;
    }



    const auto written=end-start+(end<bytes_.size()?1u:0u);
    if(buffer_.size()<written+1u) buffer_.resize(written+1u);
    std::copy_n(bytes_.begin()+static_cast<std::ptrdiff_t>(start),written,buffer_.begin());
    buffer_[written]=0u;
    line.bufferOffset=0u;
    auto position=start;
    while(position<end && bytes_[position]!=0u) {
        while(position<end && (bytes_[position]==' ' || bytes_[position]=='\t' ||
              (!line.tokens.empty() && bytes_[position]=='\r'))) ++position;
        const auto token=position;
        while(position<end && bytes_[position]!=0u && bytes_[position]!=' ' &&
              bytes_[position]!='\t' && bytes_[position]!='\r') ++position;
        // Engine field tokens stop at CR, but LoadMaxTree/ParseNode skip
        // only space/tab before dispatch. A leading CR therefore denotes
        // an empty field, not permission to parse a later command on it.
        if(position!=token || (line.tokens.empty() && position<end && bytes_[position]=='\r'))
            line.tokens.emplace_back(reinterpret_cast<const char*>(bytes_.data()+token),position-token);
        if(position<end && bytes_[position]==0u) break;
        if(position<end && bytes_[position]=='\r') ++position;
    }
    if(line.tokens.empty()) line.tokens.emplace_back();
    if((lineNumber_&255u)==0u) job_.update(cursor_,bytes_.size(),"Parsing ASCII model: "+sourceLabel_);
    return true;
}

Line Reader::fieldLine(std::size_t offset) const {
    Line line;
    line.number=lineNumber_;
    line.bufferOffset=offset;
    if(offset<buffer_.size()) {
        auto end=offset;
        while(end<buffer_.size() && buffer_[end]!=0u) ++end;
        line.text.assign(reinterpret_cast<const char*>(buffer_.data()+offset),end-offset);
    }
    // Unlike ParseNode's initial skipspace, subsequent engine field stages
    // dereference the retained pointer without skipping new indentation.
    std::size_t position{};
    const auto delimiter=[](char value) {
        return value==' ' || value=='\t' || value=='\r' || value=='\n';
    };
    do {
        const auto start=position;
        while(position<line.text.size() && !delimiter(line.text[position])) ++position;
        line.tokens.emplace_back(line.text.substr(start,position-start));
        while(position<line.text.size() && delimiter(line.text[position])) ++position;
    } while(position<line.text.size());
    return line;
}

void Reader::finishPackedArray(const Line& header) {
    const auto leading=header.text.find_first_not_of(" \t");
    const auto offset=header.bufferOffset+(leading==std::string::npos?header.text.size():leading)+
                      header.tokens.front().size();
    // Packed Parse<T> clears the old argument pointer after copying its
    // payload, even though GetData has already overwritten the buffer.
    if(offset<buffer_.size()) buffer_[offset]=0u;
}

std::vector<std::uint8_t> Reader::takeBytes(std::size_t count, const Line& line) {
    job_.check();
    if(count>bytes_.size()-cursor_) fail(line,"Truncated embedded array");
    std::vector<std::uint8_t> result(bytes_.begin()+static_cast<std::ptrdiff_t>(cursor_),
                                   bytes_.begin()+static_cast<std::ptrdiff_t>(cursor_+count));





    const auto consumed=count==0u && cursor_<bytes_.size()?1u:count;
    if(consumed!=0u) {
        if(buffer_.size()<consumed+1u) buffer_.resize(consumed+1u);
        std::copy_n(bytes_.begin()+static_cast<std::ptrdiff_t>(cursor_),consumed,buffer_.begin());
        buffer_[consumed]=0u;
    }
    cursor_+=consumed;
    return result;
}

[[noreturn]] void Reader::fail(const Line& line, const std::string& message) const {
    throw ModelError((sourceLabel_.empty()?std::string{}:sourceLabel_+": ")+
                     "ASCII MDL line "+std::to_string(line.number)+": "+message);
}

std::string lower(std::string value) {
    for(auto& character:value) if(character>='A' && character<='Z') character=static_cast<char>(character-'A'+'a');
    return value;
}
bool equal(std::string_view left, std::string_view right) noexcept {
    if(left.size()!=right.size()) return false;
    for(std::size_t i=0;i<left.size();++i) {
        const auto a=left[i]>='A' && left[i]<='Z'?left[i]-'A'+'a':left[i];
        const auto b=right[i]>='A' && right[i]<='Z'?right[i]-'A'+'a':right[i];
        if(a!=b) return false;
    }
    return true;
}

float parseFloat(const std::string& token) {
    float value;
    if(std::sscanf(token.c_str(),"%f",&value)!=1) throw ModelError("Invalid ASCII MDL float: "+token);
    return value;
}
std::int32_t parseInt(const std::string& token) {
    int value;
    if(std::sscanf(token.c_str(),"%d",&value)!=1) throw ModelError("Invalid ASCII MDL integer: "+token);
    return static_cast<std::int32_t>(value);
}
std::uint32_t parseUInt(const std::string& token) {
    unsigned value;
    if(std::sscanf(token.c_str(),"%u",&value)!=1) throw ModelError("Invalid ASCII MDL unsigned integer: "+token);
    return static_cast<std::uint32_t>(value);
}
bool scanInteger(const char* text, std::int32_t& value) {
    if(std::sscanf(text,"%d",&value)==1) return true;
    std::istringstream input(text);
    std::string token;
    input>>token;
    if(equal(token,"true")) value=1;
    else if(equal(token,"false")) value=0;
    else return false;
    return true;
}
void scanBoolean(const char* text, bool& value) {
    std::istringstream input(text);
    std::string token;
    input>>token;
    if(equal(token,"true") || token=="1") value=true;
    else if(equal(token,"false") || token=="0") value=false;
}
void scanString(const char* text, std::string& value) {
    // Engine Parse(char*, char*) preserves its destination when %s reaches
    // EOF without reading a word, including an empty argument string.
    std::istringstream input(text);
    std::string token;
    if(input>>token) value=std::move(token);
}
void requireTokens(const Line& line, std::size_t count, const Reader& reader) {
    if(line.tokens.size()<count) reader.fail(line,"Incomplete "+line.tokens.front()+" record");
}

namespace {

bool prefix(const std::string& value, const char* text) {
    return value.compare(0u,std::char_traits<char>::length(text),text)==0;
}

const char* fieldStart(const Line& line) noexcept {
    auto text=line.text.c_str();
    while(*text==' ' || *text=='\t') ++text;
    return text;
}

std::string commandArguments(Line& line) {
    // FuncInterp admits '=' between a console command and its argument text.
    // Preserve that text for callbacks such as newmodel/classification which
    // receive the complete argument, rather than an sscanf %s token.
    const auto start=line.text.find_first_not_of(" \t");
    if(start==std::string::npos || line.text[start]=='#') return {};
    // FuncInterp skips leading tabs, but only space and '=' delimit its
    // command name (K1 0x552294; K2 0x72d220).
    const auto end=line.text.find_first_of(" =",start);
    const auto command=line.text.substr(start,end==std::string::npos?end:end-start);
    std::string arguments;
    if(end!=std::string::npos) {
        const auto first=line.text.find_first_not_of(" =",end);
        if(first!=std::string::npos) arguments=line.text.substr(first);
    }
    const auto last=arguments.find_last_not_of(" \t\r\n");
    if(last==std::string::npos) arguments.clear();
    else arguments.resize(last+1u);
    line.tokens.assign(1u,command);
    std::istringstream tokens(arguments);
    std::string token;
    while(tokens>>token) line.tokens.push_back(std::move(token));
    return arguments;
}

std::uint16_t nodeFlags(const std::string& type) {
    // ParseNode falls through to MdlNode for every unrecognized type, including
    // dummy. K1 ParseNode 0x5ae204; K2 ParseNode 0x7ac270.
    if(equal(type,"light")) return kNodeBase|kNodeLight;
    if(equal(type,"emitter")) return kNodeBase|kNodeEmitter;
    if(equal(type,"camera")) return kNodeBase|kNodeCamera;
    if(equal(type,"reference")) return kNodeBase|kNodeReference;
    if(equal(type,"trimesh")) return kNodeBase|kNodeMesh;
    if(equal(type,"skin")) return kNodeBase|kNodeMesh|kNodeSkin;
    if(equal(type,"animmesh")) return kNodeBase|kNodeMesh|kNodeAnimation;
    if(equal(type,"danglymesh")) return kNodeBase|kNodeMesh|kNodeDangly;
    if(equal(type,"aabb")) return kNodeBase|kNodeMesh|kNodeAabb;
    if(equal(type,"lightsaber")) return kNodeBase|kNodeMesh|kNodeLightsaber;
    return kNodeBase;
}

std::optional<std::size_t> findRawNode(const std::vector<RawNode>& nodes,
                                       std::optional<std::size_t> root,
                                       const std::string& name) {
    if(!root) return std::nullopt;
    const auto& node=nodes[*root].node;
    if(equal(node.name,name)) return root;
    for(const auto child:node.children)
        if(const auto found=findRawNode(nodes,child,name)) return found;
    return std::nullopt;
}

std::optional<std::size_t> findBuiltNode(const Model& model,
                                         std::optional<std::size_t> root,
                                         const std::string& name) {
    if(!root) return std::nullopt;
    const auto& node=model.nodes[*root];
    if(equal(node.name,name)) return root;
    for(const auto child:node.children)
        if(const auto found=findBuiltNode(model,child,name)) return found;
    return std::nullopt;
}

void retainCurrentTree(std::vector<RawNode>& nodes, std::optional<std::size_t>& root) {
    // parent NULL assigns the tree's root pointer. Nodes attached to an older
    // root remain allocations, but are no longer part of the loaded tree.
    std::vector<bool> retained(nodes.size());
    const auto visit=[&](const auto& self,std::size_t index) -> void {
        retained[index]=true;
        for(const auto child:nodes[index].node.children) self(self,child);
    };
    visit(visit,root.value());
    std::vector<std::size_t> remap(nodes.size());
    std::vector<RawNode> current;
    for(std::size_t index=0;index<nodes.size();++index) if(retained[index]) {
        remap[index]=current.size();
        current.push_back(std::move(nodes[index]));
    }
    for(auto& raw:current) {
        if(raw.node.parent) raw.node.parent=remap[*raw.node.parent];
        for(auto& child:raw.node.children) child=remap[child];
    }
    root=remap[*root];
    nodes=std::move(current);
}

void parseNode(const Line& header, Reader& reader, Model& model,
               std::vector<RawNode>& nodes, std::optional<std::size_t>& root,
               std::uint32_t& nodeCounter, bool animation,
               std::array<std::vector<Vec3>,3>& textureIndices) {
    // Model/Animation::InternalParseField pass the bytes immediately after
    // the case-sensitive "node" prefix to ParseNode's two %s conversions.
    std::vector<char> type(header.text.size()+1u),name(header.text.size()+1u);
    if(std::sscanf(fieldStart(header)+4u,"%s %s",type.data(),name.data())!=2)
        reader.fail(header,"Incomplete node record");
    const auto index=nodes.size();
    nodes.emplace_back();
    auto& raw=nodes.back();
    raw.node.flags=nodeFlags(type.data());
    raw.node.name=name.data();
    raw.animation=animation;
    raw.modelClassification=model.classification;
    initializeNode(raw,model.game);
    if(raw.mesh) raw.textureIndices=textureIndices;
    Line line;
    while(reader.next(line)) {
        const auto& field=line.tokens.front();
        if(prefix(field,"endnode")) break;




        const auto first=line.text.find_first_not_of(" \t");
        const auto offset=first==std::string::npos?line.text.size():first;
        // Engine typed stages and Base retain this initial skipspace offset
        // even when a nested array overwrites the shared resource buffer.
        parseNodeField(reader.fieldLine(offset),reader,raw,model.game);
        if(raw.mesh) parseMeshField(reader.fieldLine(offset),reader,raw,model.game);
        parseBaseTransformFields(reader.fieldLine(offset),reader,raw);
        for(const auto group:{"position","orientation","scale"})
            parseControllerField(reader.fieldLine(offset),reader,raw,model.game,group,false);
        const auto parentLine=reader.fieldLine(offset);
        if(prefix(parentLine.tokens.front(),"parent")) {
            std::vector<char> parent(parentLine.text.size()+1u);
            if(std::sscanf(parentLine.text.c_str()+6u,"%s",parent.data())!=1)
                reader.fail(parentLine,"Incomplete parent record");
            raw.parentName=parent.data();
            if(equal(raw.parentName,"NULL")) {
                raw.node.parent.reset();
                root=index;
            }
            else {
                raw.node.parent=findRawNode(nodes,root,raw.parentName);
                if(raw.node.parent) nodes[*raw.node.parent].node.children.push_back(index);
            }
        }
    }



    if(!raw.node.parent && root!=index) reader.fail(header,"Node has no parent in the current model tree: "+raw.node.name);
    // K2 ParseNode 0x7ac90b appends model-node names without deduplication.
    // Animation names reverse-search that same table (0x7aca00), retaining
    // the last matching index even when earlier model roots were discarded.
    auto nameIndex=model.engineNames.size();
    if(animation) {
        for(auto candidate=model.engineNames.size();candidate!=0u;--candidate)
            if(equal(model.engineNames[candidate-1u],raw.node.name)) {
                nameIndex=candidate-1u;
                break;
            }
    }
    if(nameIndex==model.engineNames.size()) model.engineNames.push_back(raw.node.name);
    raw.node.engineNameIndex=static_cast<std::uint32_t>(nameIndex);
    // Engine lookup translates this index through the model's name table;
    // animation declarations can reuse an entry with different letter case.
    raw.node.name=model.engineNames[nameIndex];
    raw.node.id=static_cast<std::uint16_t>(nodeCounter++);
    // MdlNodeEmitter::InternalPostProcess changes the owning model header
    // immediately when this node closes, before later header fields parse.
    if(!animation && raw.node.emitter && model.engineRadius<40.0f) model.engineRadius=40.0f;
    if(!animation && raw.mesh && raw.mesh->skinned)
        postProcessSkinNode(raw,reader.job());
    else if(raw.mesh && (!animation || (raw.node.flags&kNodeAnimation))) {





        buildMesh(model,raw,index,reader.job());
        auto& mesh=*raw.mesh;
        if(mesh.mdxRowSize!=0u) {
            mesh.asciiMdxAllocation=model.asciiMdxAllocations.size();
            model.asciiMdxAllocations.push_back({mesh.enginePrelightVertexCount,
                mesh.mdxRowSize,mesh.asciiTangentSource?mesh.texture0:std::string{},
                mesh.asciiTangentSource!=nullptr,std::make_shared<Mesh>(mesh)});
        }
        // Engine typed clip nodes own their generated mesh as well as samples;
        // keep that source geometry outside the model's rendered mesh list.
        if(animation)
            raw.node.animmesh->authoringMesh=std::make_shared<Mesh>(mesh);
    }
    if(raw.mesh) {
        // The engine texindices1/2/3 lists are shared parser scratch. The
        // active NW animmesh builder now clears them through its temporary
        // cleanup (0x454567 -> 0x4542a0), including typed clip nodes. Other
        // typed clips retain KOTOR's base conversion without PostProcess;
        // saber PostProcess also retains the shared lists.
        if((raw.node.flags&kNodeAnimation) ||
           (!animation && !(raw.node.flags&kNodeLightsaber))) {
            for(auto& channel:textureIndices) channel.clear();
        } else textureIndices=raw.textureIndices;
    }
}

void classification(Model& model, const std::string& value) {
    // Shared OR semantics, not replacement: K1 0x5af7b4, K2 0x7adbf0.
    const std::pair<const char*,std::uint16_t> flags[]{
        {"forcedistortion",0x0002u},{"heatdistortion",0x0080u},
        {"character",0x0004u},{"ambient",0x0040u},{"effect",0x0001u},
        {"effects",0x0001u},{"track",0x0001u},{"door",0x0008u},
        {"lightsaber",0x0010u},{"item",0x0020u},{"placeable",0x0420u}};
    for(const auto& entry:flags) if(equal(value,entry.first)) model.classification|=entry.second;
}

void parseAnimationField(const Line& line, Reader& reader, Animation& animation) {
    const auto field=lower(line.tokens.front());
    if(field=="length" || field=="transtime") {
        std::sscanf(fieldStart(line)+field.size(),"%f",
            field=="length"?&animation.length:&animation.transitionTime);
    } else if(prefix(line.tokens.front(),"animlength")) {
        std::vector<char> ignored(line.text.size()+1u);
        std::sscanf(fieldStart(line)+10u,"%s %f",ignored.data(),&animation.length);
    } else if(field=="animroot") {
        scanString(fieldStart(line)+8u,animation.root);
    } else if(prefix(line.tokens.front(),"event")) {
        AnimationEvent event;
        int consumed{};
        const auto text=fieldStart(line)+5u;
        if(std::sscanf(text,"%f%n",&event.time,&consumed)!=1)
            reader.fail(line,"Incomplete event record");
        std::istringstream name(text+consumed);
        name>>event.name;
        // Both loaders insert before the first equal-time event.
        auto insertion=animation.events.begin();
        while(insertion!=animation.events.end() && event.time>insertion->time) ++insertion;
        animation.events.insert(insertion,std::move(event));
    }
}

void loadTree(Reader& reader, Model& model, std::vector<RawNode>& nodes,
                std::optional<std::size_t>& root, std::uint32_t& nodeCounter,
                Animation* animation,
                std::string& animationRootName,
                std::array<std::vector<Vec3>,3>& textureIndices) {
    // LoadMaxTree keeps this %s destination in its own stack frame. A
    // repeated empty field retains a name written earlier in the same block.
    std::string rootNameScratch;
    Line line;
    while(reader.next(line)) {
        const auto& field=line.tokens.front();
        // LoadMaxTree terminates on these prefixes, independently of case-
        // insensitive field comparisons (K1 0x5af488, K2 0x7ad83d).
        if(prefix(field,"done") || prefix(field,"end")) break;
        if(prefix(field,"node")) parseNode(line,reader,model,nodes,root,nodeCounter,
                                          animation!=nullptr,textureIndices);
        else if(animation) parseAnimationField(line,reader,*animation);
        else if(equal(field,"sModelAnimationRootName")) {
            scanString(fieldStart(line)+23u,rootNameScratch);
            animationRootName=rootNameScratch;
        } else if(equal(field,"bmin") || equal(field,"bmax")) {
            auto& point=equal(field,"bmin")?model.engineBounds.minimum:model.engineBounds.maximum;
            std::sscanf(fieldStart(line)+4u,"%f %f %f",&point.x,&point.y,&point.z);
            model.engineBounds.valid=true;
        } else if(equal(field,"radius")) {
            std::sscanf(fieldStart(line)+6u,"%f",&model.engineRadius);
        }
    }
}

} // namespace
} // namespace neomdl::model::ascii

namespace neomdl::model {
namespace {

struct AsciiLoadContext;
std::shared_ptr<Model> decodeAsciiModelInContext(
    const std::vector<std::uint8_t>& bytes, GameVersion game,
    const std::string& sourceLabel, const neoshared::bif::JobControl& job,
    const AsciiResourceResolver& resources,
    AsciiLoadContext& context);

struct AsciiLoadContext {
    // FindModel searches registered models before it reads a resource.
    // newmodel replaces the current-model pointer without releasing its old
    // registered object (K2 0x7adb30..0x7adb6b). Explicit superclass references
    // have a separate engine count: DelRef removes a registration at zero.
    // The context owns the models; raw superclass edges do not create C++
    // shared-pointer cycles.
    struct Registration {
        std::shared_ptr<Model> model;
        std::uint32_t references{};
        Model* superclass{};
    };
    std::vector<Registration> models;
    // Preserve engine temporary UV-index lists across nodes and nested reads
    // in this load. No prior external process-global contents are assumed.
    std::array<std::vector<Vec3>,3> textureIndices;

    auto registration(Model* model) {
        return std::find_if(models.begin(),models.end(),
            [model](const Registration& entry) { return entry.model.get()==model; });
    }

    void insert(const std::shared_ptr<Model>& model,Model* superclass=nullptr) {



        const auto name=ascii::lower(model->name);
        const auto position=std::upper_bound(models.begin(),models.end(),name,
            [](const std::string& value,const Registration& candidate) {
                return value<ascii::lower(candidate.model->name);
            });
        models.insert(position,{model,model->engineReferenceCount,superclass});
    }

    void release(Model* model) {
        const auto entry=registration(model);
        if(--entry->references!=0u) return;
        const bool asciiBuilt=entry->model->asciiBuilt;
        const auto superclass=entry->superclass;
        // Compiled MaxTree::foobar releases its superclass before invoking
        // InternalDestructor (K2 0x6ec602..0x6ec62c). ASCII destruction removes
        // this exact registration first (0x6ed507..0x6ed569).
        if(!asciiBuilt && superclass) release(superclass);
        models.erase(registration(model));
        if(asciiBuilt && superclass) release(superclass);
    }

    void detachSuperclass(Model* model) {
        const auto entry=registration(model);
        const auto superclass=entry->superclass;
        entry->superclass=nullptr;
        if(superclass) release(superclass);
    }

    void attachSuperclass(Model* model,Model* superclass) {
        registration(model)->superclass=superclass;
        if(superclass) ++registration(superclass)->references;
    }

    void retainSuperclasses(Model& model) {
        auto chain=std::make_shared<std::vector<Model>>();
        std::vector<const Model*> visited{&model};
        auto* current=&model;
        while(true) {
            const auto entry=registration(current);
            if(entry==models.end() || !entry->superclass) break;
            current=entry->superclass;
            if(std::find(visited.begin(),visited.end(),current)!=visited.end()) {
                model.warnings.push_back("Loaded supermodel pointer cycle stopped at: "+current->name);
                break;
            }
            if(chain->size()>=64u) {
                model.warnings.push_back("Loaded supermodel chain exceeded the 64-model safety limit at: "+current->name);
                break;
            }
            visited.push_back(current);
            chain->push_back(*current);
            // The outer chain supplies ownership without retaining registry
            // edges or creating shared_ptr cycles between models.
            chain->back().asciiSupermodels.reset();
        }
        model.asciiSupermodels=std::move(chain);
    }

    std::shared_ptr<Model> find(const std::string& name, GameVersion game,
        const neoshared::bif::JobControl& job, const AsciiResourceResolver& resources) {
        if(ascii::equal(name,"NULL")) return {};
        const auto key=ascii::lower(name);
        std::ptrdiff_t first=0,last=static_cast<std::ptrdiff_t>(models.size())-1;
        while(first<=last) {
            const auto middle=(first+last)/2;
            const auto comparison=key.compare(ascii::lower(models[middle].model->name));
            if(comparison==0) return models[middle].model;
            if(comparison<0) last=middle-1;
            else first=middle+1;
        }
        if(!resources) return {};
        const auto bytes=resources(name,".mdl");
        if(!bytes) return {};
        if(isAsciiModelInput(*bytes))
            return decodeAsciiModelInContext(*bytes,game,name+".mdl",job,resources,*this);
        const auto mdx=resources(name,".mdx");
        auto model=std::make_shared<Model>(decodeModelBytes(*bytes,
            mdx ? *mdx : std::vector<std::uint8_t>{},name+".mdl",{},job,
            game,resources));
        // Packed Reset resolves and references its superclass before its own
        // registration (K1 Desktop 0x322343/0x322356 before 0x3224dc).
        const auto superclass=find(model->engineSupermodelName,game,job,resources);
        if(superclass) ++registration(superclass.get())->references;
        insert(model,superclass.get());
        return model;
    }
};

std::shared_ptr<Model> decodeAsciiModelInContext(const std::vector<std::uint8_t>& bytes, GameVersion game,
                            const std::string& sourceLabel,
                            const neoshared::bif::JobControl& job,
                            const AsciiResourceResolver& resources,
                            AsciiLoadContext& context) {
    using namespace ascii;
    Reader reader(bytes,game,sourceLabel,job,resources);
    std::shared_ptr<Model> current;
    std::shared_ptr<Model> superclass;
    std::vector<RawNode> modelNodes;
    std::optional<std::size_t> modelRoot;
    std::uint32_t modelNodeCounter{};
    std::string modelAnimationRootName;
    Line line;
    job.update(0u,bytes.size(),"Parsing ASCII model: "+sourceLabel);
    while(reader.next(line)) {
        const auto arguments=commandArguments(line);
        const auto& command=line.tokens.front();
        // FuncInterp's console-command comparison is case-sensitive.
        if(command=="newmodel") {
            superclass.reset();
            current=std::make_shared<Model>();
            current->game=game;
            current->asciiBuilt=true;
            current->fog=true;
            current->engineBounds={{-5.0f,-5.0f,-1.0f},{5.0f,5.0f,10.0f},true};
            current->engineRadius=7.0f;
            current->name=arguments;
            context.insert(current);
            modelNodes.clear();
            modelRoot.reset();
            modelNodeCounter=0u;
            modelAnimationRootName.clear();
        } else if(command=="beginmodelgeom") {
            auto& model=*current;



            if(superclass) modelNodeCounter=superclass->engineNodeCount+1u;
            loadTree(reader,model,modelNodes,modelRoot,modelNodeCounter,nullptr,
                       modelAnimationRootName,context.textureIndices);
            retainCurrentTree(modelNodes,modelRoot);
            model.nodes.clear();
            for(const auto& raw:modelNodes) model.nodes.push_back(raw.node);
            model.meshes.clear();
            for(std::size_t index=0;index<modelNodes.size();++index) {
                auto& raw=modelNodes[index];
                if(!raw.mesh) continue;
                raw.node.mesh=model.meshes.size();
                // Parsed nodes can survive another beginmodelgeom block.
                // Their generated streams are built once at endnode, not
                // appended or moved from again when a block finishes.
                model.meshes.push_back(*raw.mesh);
                model.meshes.back().nodeIndex=index;
                model.nodes[index]=raw.node;
            }
            model.engineNodeCount=modelNodeCounter;
            finalizeNodeTree(model,modelNodes,nullptr);
            if(superclass) synchronizeAsciiModelTree(model,*superclass);
            // Preserve the immediate ID writes for a later geometry block.
            for(std::size_t index=0;index<modelNodes.size();++index)
                modelNodes[index].node.id=model.nodes[index].id;
            // LoadMaxTree reorders the same tree that subsequent geometry
            // blocks search when resolving parent names.
            for(std::size_t index=0;index<modelNodes.size();++index)
                modelNodes[index].node.children=model.nodes[index].children;
            model.animationRootNode=findBuiltNode(model,modelRoot,modelAnimationRootName);
            if(!model.animationRootNode) model.animationRootNode=modelRoot;
        } else if(command=="newanim") {
            requireTokens(line,2u,reader);
            auto& model=(*current);
            Animation animation;
            animation.name=line.tokens[1];
            animation.length=1.0f;
            animation.transitionTime=0.25f;
            std::vector<RawNode> nodes;
            std::optional<std::size_t> root;
            std::uint32_t nodeCounter{};
            std::string ignoredRoot;
            loadTree(reader,model,nodes,root,nodeCounter,&animation,ignoredRoot,
                       context.textureIndices);
            animation.engineNodeCount=nodeCounter;
            retainCurrentTree(nodes,root);
            for(const auto& raw:nodes) animation.nodes.push_back(raw.node);
            finalizeNodeTree(model,nodes,&animation);
            model.animations.push_back(std::move(animation));
        } else if(command=="setsupermodel") {
            requireTokens(line,3u,reader);
            current->engineSupermodelName=line.tokens[2];
            current->supermodel=equal(line.tokens[2],"NULL")?std::string{}:lower(line.tokens[2]);
            // setsupermodel releases the previous model before FindModel and
            // rejects the current model itself (K2 0x7ae432..0x7ae476).
            context.detachSuperclass(current.get());
            superclass.reset();
            superclass=context.find(current->engineSupermodelName,game,job,resources);
            if(superclass==current) superclass.reset();
            context.attachSuperclass(current.get(),superclass.get());
        } else if(command=="classification") {
            classification((*current),arguments);
        } else if(command=="secondaryclassification") {
            if(equal(arguments,"sensitive")) (*current).classification|=0x0200u;
        } else if(command=="ignorefog") {
            requireTokens(line,2u,reader);
            if(parseInt(line.tokens[1])==1) (*current).fog=false;
        } else if(command=="setanimationscale") {
            // Both setanimationscale implementations initialize the sscanf
            // destination to one before scanning the optional value.
            float scale=1.0f;
            if(line.tokens.size()>1u) std::sscanf(line.tokens[1].c_str(),"%f",&scale);
            (*current).animationScale=scale;
        }
        // donemodel is an actual no-op in both executables. Unknown console
        // commands have no model effect; no extra authoring dialect is added.
    }
    if(!current) reader.fail(line,"The input did not construct a model");
    auto& model=*current;
    model.declaredAnimationCount=static_cast<std::uint32_t>(model.animations.size());
    // CreateInstanceTree calls the final root's Part::PostProcess. Skin
    // overrides it without traversing children; discarded roots and skin
    // descendants therefore never generate their deferred MDX streams.
    std::vector<std::size_t> partPath;
    const auto postProcess=[&](const auto& self,std::size_t index)->void {
        // CreateInstanceTreeR assigns the Part parent for each child-list
        // occurrence. Repeated parent fields can leave the same MdlNode on
        // two paths; its last node.parent is not the first Part's ancestry.
        partPath.push_back(index);
        auto& raw=modelNodes[index];
        if(raw.node.mesh && raw.mesh->skinned) {
            auto& mesh=model.meshes[*raw.node.mesh];
            // PartSkin::PostProcess is a first-instance producer on the
            // shared node (K2 0x711896/0x711915; K1 0x5e3f18/0x5e3f80).
            // Repeated child links must not rebuild a moved source mesh.
            if(mesh.enginePrelightVertexCount==0u && mesh.inverseBindTranslations.empty()) {
                buildMesh(model,raw,index,job);
                mesh=std::move(*raw.mesh);
                buildSkin(model,raw,mesh,raw.generatedVertexSources,partPath,job);
            }
        } else for(const auto child:model.nodes[index].children) self(self,child);
        partPath.pop_back();
    };
    for(std::size_t index=0;index<model.nodes.size();++index)
        if(!model.nodes[index].parent) postProcess(postProcess,index);
    initializeAsciiSkinSeamPools(model);
    initializeProceduralUvPools(model);
    const auto pose=bindPose(model,job);
    model.worldTransforms=pose.worldTransforms;
    model.bounds=pose.bounds;
    job.update(bytes.size(),bytes.size(),"Decoded ASCII model: "+sourceLabel);
    return current;
}

} // namespace

Model decodeAsciiModelBytes(const std::vector<std::uint8_t>& bytes, GameVersion game,
                            const std::string& sourceLabel,
                            const neoshared::bif::JobControl& job,
                            const AsciiResourceResolver& resources) {
    AsciiLoadContext context;
    auto model=decodeAsciiModelInContext(bytes,game,sourceLabel,job,resources,context);
    context.retainSuperclasses(*model);
    return std::move(*model);
}

} // namespace neomdl::model
