#include "core/ResourceContext.hpp"
#include <neoshared/PathUtf8.hpp>
#include <neoshared/bif/KeyBifArchive.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <stdexcept>

namespace neomdl {
namespace {
thread_local const neoshared::bif::JobControl* activeLoadJob{};
using DirectoryFiles=std::map<std::string,std::vector<std::filesystem::directory_entry>>;
thread_local std::map<std::filesystem::path,DirectoryFiles> loadDirectories;
std::string lowerResourceName(std::string value) {
    for(auto& c:value) if(c>='A'&&c<='Z') c=char(c+('a'-'A'));
    return value;
}
}
const neoshared::bif::JobControl& resourceLoadJob() noexcept {
    static const neoshared::bif::JobControl idle;
    return activeLoadJob?*activeLoadJob:idle;
}
ResourceLoadScope::ResourceLoadScope(const neoshared::bif::JobControl& job)
    :job_(job),previous_(activeLoadJob) {
    // Texture codecs and chunked file reads already cooperate through this
    // shared operation hook. Nested loads inherit the bridge and supply their
    // current JobControl without keeping a UI callback in a decoded document.
    // A new load phase refreshes names, including successive batch conversions.
    loadDirectories.clear();
    if(!previous_) {
        textureOperation_.emplace([prior=neoshared::texture::operationCheckpoint] {
            if(prior)prior();
            resourceLoadJob().check();
        });
    }
    activeLoadJob=&job_;
}
ResourceLoadScope::~ResourceLoadScope() {activeLoadJob=previous_;if(!previous_)loadDirectories.clear();}
std::string resourceKey(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\:\r\n")!=std::string::npos ||
        name=="." || name==".." || name.find('\0')!=std::string::npos)
        throw std::runtime_error("Invalid resource ResRef: "+name);
    return lowerResourceName(name);
}
std::optional<std::filesystem::path> uniqueResourceFile(
    const std::filesystem::path& directory,const std::string& filename) {
    const auto wanted=lowerResourceName(filename);
    std::optional<std::filesystem::path> selected;
    const auto match=[&](const std::filesystem::directory_entry& entry) {
        if(!entry.is_regular_file())return;
        if(selected)throw std::runtime_error("Ambiguous resource in "+neoshared::pathToUtf8(directory)+": "+wanted);
        selected=entry.path();
    };
    if(activeLoadJob) {
        const auto root=std::filesystem::absolute(directory).lexically_normal();
        auto found=loadDirectories.find(root);
        if(found==loadDirectories.end()) {
            DirectoryFiles files;
            for(const auto& entry:std::filesystem::directory_iterator(root)) {
                resourceLoadJob().check();
                files[lowerResourceName(neoshared::pathToUtf8(entry.path().filename()))].push_back(entry);
            }
            found=loadDirectories.emplace(root,std::move(files)).first;
        }
        const auto file=found->second.find(wanted);
        if(file!=found->second.end())for(const auto& entry:file->second)match(entry);
        return selected;
    }
    std::error_code error;
    for(std::filesystem::directory_iterator it(directory,error),end;!error&&it!=end;it.increment(error)) {
        resourceLoadJob().check();
        if(lowerResourceName(neoshared::pathToUtf8(it->path().filename()))!=wanted) continue;
        match(*it);
    }
    if(error) throw std::runtime_error("Resource directory: "+error.message());
    return selected;
}
std::optional<std::filesystem::path> uniqueResourceDirectory(
    const std::filesystem::path& directory,const std::string& name) {
    const auto wanted=lowerResourceName(name);
    std::optional<std::filesystem::path> selected;
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        resourceLoadJob().check();
        if(lowerResourceName(neoshared::pathToUtf8(entry.path().filename()))!=wanted || !entry.is_directory()) continue;
        if(selected) throw std::runtime_error("Ambiguous resource directory in "+neoshared::pathToUtf8(directory)+": "+wanted);
        selected=entry.path();
    }
    return selected;
}
std::string resourceFileIdentity(const std::filesystem::path& path) {
    return "neomdl:file:"+neoshared::genericPathToUtf8(std::filesystem::weakly_canonical(path));
}
std::vector<std::uint8_t> readResourceFile(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("Unable to open file: "+neoshared::pathToUtf8(path));
    return readResourceStream(in,path);
}
std::vector<std::uint8_t> readResourceStream(std::istream& in,const std::filesystem::path& path) {
    in.seekg(0,std::ios::end);
    const auto end=in.tellg();
    if(end<0) throw std::runtime_error("Unable to size file: "+neoshared::pathToUtf8(path));
    std::vector<std::uint8_t> bytes;
    if(static_cast<std::uintmax_t>(end)>bytes.max_size())
        throw std::runtime_error("File is too large for this process: "+neoshared::pathToUtf8(path));
    bytes.resize(static_cast<std::size_t>(end));
    in.seekg(0);
    if(!in) throw std::runtime_error("Unable to seek file: "+neoshared::pathToUtf8(path));
    for(std::size_t offset=0;offset<bytes.size();) {
        resourceLoadJob().check();
        const auto count=std::min<std::size_t>(1024u*1024u,bytes.size()-offset);
        in.read(reinterpret_cast<char*>(bytes.data()+offset),static_cast<std::streamsize>(count));
        if(static_cast<std::size_t>(in.gcount())!=count)
            throw std::runtime_error("Unable to read complete file: "+neoshared::pathToUtf8(path));
        offset+=count;
    }
    return bytes;
}
std::string resourceExtension(std::uint16_t t) {
    switch(t) {
    case 0x7d2: case 0xbc0: case 0xbbf: case 0xbbe: case 3: case 0x7f1:
    case 0x7e6: case 0x7dc: case 0x7de: case 0x7e1: case 0x7e7: case 0x7eb:
    case 0x7fc: case 0x7fa: case 0x7f0: case 0x7e9: case 0xbb8: case 0xbb9:
    case 4: case 8: case 0x7ed: case 0xbbc: case 0x7e0: case 0x804: case 0x805:
        return neoshared::bif::resourceTypeExtension(t,neoshared::bif::GameProfile::KotOR);
    default:return {};
    }
}
void ResourceTrace::record(const std::string& requester,const std::string& name,
    std::uint16_t type,const neoshared::ResourceDocument* result,const std::string& error) {
    ResourceDependency d;
    d.requester=requester;d.resref=name;d.type=type;d.error=error;
    d.status=!error.empty()?DependencyStatus::Failed:result?DependencyStatus::Found:DependencyStatus::Missing;
    if(result) {d.identity=result->identity;d.source=result->sourceDescription;d.inputs=result->protectedInputs;}
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key=std::make_tuple(d.requester,d.resref,d.type,d.status,d.identity,d.error);
    if(keys_.count(key))return;
    entries_.push_back(std::move(d));
    try {keys_.insert(key);}catch(...) {entries_.pop_back();throw;}
}
std::vector<ResourceDependency> ResourceTrace::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);return entries_;
}
std::vector<std::filesystem::path> ResourceTrace::inputs() const {
    std::vector<std::filesystem::path> result;
    for(const auto& d:entries()) for(const auto& p:d.inputs)
        if(std::find(result.begin(),result.end(),p)==result.end()) result.push_back(p);
    return result;
}
ResourceResolver traceResources(ResourceResolver resolver,std::shared_ptr<ResourceTrace> trace,std::string requester) {
    if(!resolver) return {};
    if(!trace)throw std::invalid_argument("A traced resolver requires a provenance sink");
    return [resolver=std::move(resolver),trace=std::move(trace),requester=std::move(requester)](
        const std::string& name,std::uint16_t type)->std::optional<neoshared::ResourceDocument> {
        try {resourceLoadJob().check();auto r=resolver(resourceKey(name),type);
            resourceLoadJob().check();
            if(r&&r->type!=type)throw std::runtime_error("Resolver returned the wrong resource type: "+name);
            trace->record(requester,name,type,r?&*r:nullptr);return r;}
        catch(const neoshared::bif::JobCancelled&) {throw;}
        catch(const std::exception& e) {trace->record(requester,name,type,nullptr,e.what());throw;}
    };
}
ResourceResolver directoryResources(const std::filesystem::path& directory,ResourceResolver fallback) {
    if(directory.empty()) throw std::runtime_error("An explicit resource directory is required");
    const auto root=std::filesystem::absolute(directory).lexically_normal();
    return [root,fallback=std::move(fallback)](const std::string& name,std::uint16_t type)
        ->std::optional<neoshared::ResourceDocument> {
        resourceLoadJob().check();
        const auto stem=resourceKey(name),ext=resourceExtension(type);
        if(!ext.empty()) {
            const auto selected=uniqueResourceFile(root,stem+"."+ext);
            if(selected) {
                neoshared::ResourceDocument r;r.type=type;r.fileName=neoshared::pathToUtf8(selected->filename());
                r.identity=resourceFileIdentity(*selected);
                r.sourceDescription=neoshared::genericPathToUtf8(*selected);r.protectedInputs={*selected};
                resourceLoadJob().update(0,0,"Reading "+r.fileName);
                r.bytes=readResourceFile(*selected);
                resourceLoadJob().check();return r;
            }
        }
        return fallback?fallback(stem,type):std::nullopt;
    };
}
} // namespace neomdl
