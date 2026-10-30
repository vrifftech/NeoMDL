#include "core/GameResourceSession.hpp"
#include "core/DialogueRuntime.hpp"
#include "core/DialogueAudioSource.hpp"
#include <fstream>
#include <neoshared/bif/KeyBifArchive.hpp>
#include <neoshared/bif/LooseArchiveCatalog.hpp>
#include <neoshared/bif/ResourceQuery.hpp>
#include <neoshared/game/OverrideAliases.hpp>
#include <neoshared/PathUtf8.hpp>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace neomdl {
namespace {
using Path=std::filesystem::path;
using Key=std::pair<std::string,std::uint16_t>;
using Index=std::map<Key,std::vector<std::size_t>>;
std::string lower(std::string s) {for(auto& c:s) if(c>='A'&&c<='Z') c=char(c+32);return s;}
std::string pathIdentity(const Path& p) {return neoshared::genericPathToUtf8(std::filesystem::weakly_canonical(p));}
std::string component(const std::string& s) {return std::to_string(s.size())+":"+s;}

std::vector<Path> installationArchives(const Path& root,const neoshared::bif::JobControl& job) {
    std::vector<Path> paths;
    for(const auto& directory:std::filesystem::directory_iterator(root)) {
        job.check();
        const auto folder=lower(neoshared::pathToUtf8(directory.path().filename()));
        if(folder!="modules"&&folder!="rims"&&folder!="texturepacks") continue;
        if(!std::filesystem::is_directory(directory.symlink_status())) continue;
        job.update(0,0,"Discovering archives: "+neoshared::pathToUtf8(directory.path()));
        for(const auto& file:std::filesystem::directory_iterator(
                directory.path(),std::filesystem::directory_options::skip_permission_denied)) {
            job.check();
            if(neoshared::bif::LooseArchiveCatalog::hasSupportedExtension(file.path())&&file.is_regular_file())
                paths.push_back(file.path());
        }
    }
    return paths;
}

struct Mount {
    neoshared::bif::LooseArchiveCatalog catalog;
    std::vector<Index> indices; // caller priority, independent of catalog sort
    std::vector<Path> paths;
    void indexArchive(const neoshared::bif::LooseArchiveInfo& archive,const neoshared::bif::JobControl& job) {
        Index index;
        for(const auto n:archive.resourceIndices) {
            job.check();
            const auto& r=catalog.resources().at(n);
            index[{lower(r.resref),r.type}].push_back(n);
        }
        indices.push_back(std::move(index));
    }
    void open(const Path& root,const std::vector<Path>& requested,const neoshared::bif::JobControl& job) {
        std::set<std::string> unique;
        for(const auto& requestedPath:requested) {
            job.check();
            const auto p=std::filesystem::weakly_canonical(requestedPath.is_absolute()?requestedPath:root/requestedPath);
            if(!unique.insert(neoshared::genericPathToUtf8(p)).second) throw std::runtime_error("Archive mounted twice: "+neoshared::pathToUtf8(p));
            paths.push_back(p);
        }
        if(!catalog.openFiles(root,paths,job,neoshared::bif::GameProfile::KotOR))
            throw std::runtime_error(catalog.lastError());
        std::map<std::string,const neoshared::bif::LooseArchiveInfo*> archives;
        for(const auto& archive:catalog.archives()) archives.emplace(neoshared::genericPathToUtf8(archive.resolvedPath),&archive);
        for(const auto& path:paths) {
            job.update(indices.size(),paths.size(),"Indexing mounted resources: "+neoshared::pathToUtf8(path));
            const auto a=archives.find(neoshared::genericPathToUtf8(path));
            if(a==archives.end()||!a->second->valid)
                throw std::runtime_error("Invalid mounted archive: "+neoshared::pathToUtf8(path));
            indexArchive(*a->second,job);
        }
    }
    void openOverrides(const Path& root,const std::vector<neoshared::override_aliases::OverrideRoot>& roots,
                       const neoshared::bif::JobControl& job) {
        if(!catalog.openFiles(root,{},job,neoshared::bif::GameProfile::KotOR) ||
           !catalog.addOverrideDirectories(roots,{},job,true))
            throw std::runtime_error(catalog.lastError());
        // Discovery order is the shared alias order, not path/catalog sorting.
        for(const auto& archive:catalog.archives()) {
            paths.push_back(archive.resolvedPath);
            indexArchive(archive,job);
        }
    }
    std::optional<std::size_t> find(const Key& key) const {
        for(std::size_t n=0;n<indices.size();++n) {
            resourceLoadJob().check();
            if(const auto i=indices[n].find(key);i!=indices[n].end()) {
                if(i->second.size()!=1) throw std::runtime_error("Ambiguous resource in "+neoshared::pathToUtf8(paths[n])+": "+key.first);
                // Unreadable high-priority resources are errors, not fallthrough.
                return i->second.front();
            }
        }
        return {};
    }
};
struct VoiceIndex {
    Mount mount;
    std::vector<std::map<std::string,std::vector<std::size_t>>> paths;
};
struct InstallationState {
    neoshared::game::GameInstall installation;
    model::GameVersion game{model::GameVersion::Kotor2};
    Path root;
    neoshared::bif::KeyBifArchive key;
    Index keyIndex;
    Mount globals;
    Mount overrides;
    std::vector<neoshared::override_aliases::OverrideRoot> overrideRoots;
    std::once_flag voiceOnce;
    std::unique_ptr<VoiceIndex> voices;
    std::string identity;
    std::vector<ModuleScope> modules;
    std::vector<std::string> diagnostics;

    const VoiceIndex& voiceIndex(const neoshared::bif::JobControl& job) {
        std::call_once(voiceOnce,[&] {
            job.update(0,0,"Indexing dialogue voice streams");
            auto next=std::make_unique<VoiceIndex>();
            std::vector<neoshared::override_aliases::OverrideRoot> roots;
            const std::string folder=game==model::GameVersion::Kotor1?"streamwaves":"streamvoice";
            const std::string alias=game==model::GameVersion::Kotor1?"STREAMWAVES":"STREAMVOICE";
            const auto directory=[&](const Path& parent) {
                if(!std::filesystem::is_directory(parent)) return parent/folder;
                return uniqueResourceDirectory(parent,folder).value_or(parent/folder);
            };
            roots.push_back({alias,directory(root),{},0,false});
            // K1 Play resolves HD0:STREAMWAVES once; its InitializeSource has
            // no K2 streamvoice override-directory loop (Desktop 0x3bfdae).
            if(game==model::GameVersion::Kotor2)for(const auto& overrideRoot:overrideRoots) {
                auto voice=overrideRoot;voice.path=directory(overrideRoot.path);
                voice.alias=alias+" / "+overrideRoot.alias;voice.precedence=roots.size();
                roots.push_back(std::move(voice));
            }
            next->mount.openOverrides(root,roots,job);
            for(const auto& archive:next->mount.catalog.archives()) {
                std::map<std::string,std::vector<std::size_t>> index;
                for(const auto n:archive.resourceIndices) {
                    job.check();
                    const auto& item=next->mount.catalog.resources().at(n);
                    if(item.type==4 || item.type==8)
                        index[lower(neoshared::genericPathToUtf8(item.directPath.lexically_relative(archive.resolvedPath)))].push_back(n);
                }
                next->paths.push_back(std::move(index));
            }
            job.check();
            voices=std::move(next);
        });
        return *voices;
    }
};
}
struct GameResourceSession::State final {
    std::shared_ptr<InstallationState> base;
    std::optional<ModuleScope> module;
    Mount mount;
    std::string identity;
    std::vector<std::string> diagnostics;
};
GameResourceSession::GameResourceSession()=default;
GameResourceSession::~GameResourceSession()=default;
GameResourceSession::GameResourceSession(GameResourceSession&&) noexcept=default;
GameResourceSession& GameResourceSession::operator=(GameResourceSession&&) noexcept=default;

bool GameResourceSession::open(const neoshared::game::GameInstall& supplied,std::string& error,
                               const neoshared::bif::JobControl& job) {
    ResourceLoadScope loading(job);
    error.clear();
    try {
        job.update(0,0,"Opening game installation");
        const auto id=neoshared::game::gameIdFromString(supplied.id);
        if(!id||(*id!=neoshared::game::GameId::Kotor1&&*id!=neoshared::game::GameId::Kotor2))
            throw std::runtime_error("Resource sessions require a KotOR/KotOR II installation");
        const auto* definition=neoshared::game::findGame(*id);
        if(!definition)throw std::runtime_error("Unknown game definition");
        auto next=std::make_shared<State>();next->base=std::make_shared<InstallationState>();
        auto& b=*next->base;b.installation=supplied;
        b.game=*id==neoshared::game::GameId::Kotor1?model::GameVersion::Kotor1:model::GameVersion::Kotor2;
        neoshared::game::refreshDerivedPaths(*definition,b.installation);
        b.root=neoshared::game::normalizeConfiguredPath(b.installation.installPath);
        const auto keyPath=neoshared::game::findExistingPathCaseInsensitive(b.root/"chitin.key");
        if(!b.key.open(keyPath,{},{},job)||b.key.gameProfile()!=neoshared::bif::GameProfile::KotOR)
            throw std::runtime_error("Cannot index KotOR KEY: "+b.key.lastError());
        for(const auto& r:b.key.resources()) {
            job.check();
            b.keyIndex[{lower(r.resref),r.type}].push_back(r.index);
        }
        job.update(0,0,"Discovering module and texture archives");
        const auto paths=installationArchives(b.root,job);
        std::map<std::string,Path> globalFiles;
        std::map<std::string,std::map<std::string,Path>> grouped;
        for(const auto& p:paths) {
            job.check();
            const auto folder=lower(neoshared::pathToUtf8(p.parent_path().filename()));
            const auto file=lower(neoshared::pathToUtf8(p.filename()));
            const auto stem=lower(neoshared::pathToUtf8(p.stem()));
            const auto ext=lower(neoshared::pathToUtf8(p.extension()));
            // Explicit preview quality A + GUI. Do not combine quality packs
            // by reverse alphabetical accident or mount save/module archives.
            if(folder=="texturepacks"&&(file=="swpc_tex_tpa.erf"||file=="swpc_tex_gui.erf")) {
                if(!globalFiles.emplace(file,p).second) throw std::runtime_error("Ambiguous global texture archive: "+file);
            }
            if(folder!="modules"&&folder!="rims") continue;
            if(ext!=".mod"&&ext!=".rim"&&ext!=".erf") continue;
            auto module=stem;
            if(ext==".rim"&&module.size()>2&&module.substr(module.size()-2)=="_s") module.resize(module.size()-2);
            if(ext==".erf"&&module.size()>4&&module.substr(module.size()-4)=="_dlg") module.resize(module.size()-4);
            else if(ext==".erf") continue;
            if(!grouped[module].emplace(file,p).second) throw std::runtime_error("Ambiguous module archive: "+file);
        }
        std::vector<Path> globalPaths;
        for(const auto& file:{"swpc_tex_gui.erf","swpc_tex_tpa.erf"})
            if(const auto i=globalFiles.find(file);i!=globalFiles.end()) globalPaths.push_back(i->second);
        b.globals.open(b.root,globalPaths,job);
        for(const auto& group:grouped) {
            job.check();
            ModuleScope scope;scope.name=group.first;
            // Viewer mounting policy: MOD override, split RIMs, dialogue ERF.
            // Callers may provide an explicit different archive sequence.
            for(const auto& suffix:{".mod","_s.rim",".rim","_dlg.erf"})
                if(const auto i=group.second.find(scope.name+suffix);i!=group.second.end()) scope.archives.push_back(i->second);
            if(group.second.count(scope.name+".mod")||group.second.count(scope.name+".rim")||group.second.count(scope.name+"_s.rim"))
                b.modules.push_back(std::move(scope));
        }
        job.update(0,0,"Discovering override aliases");
        auto aliases=neoshared::override_aliases::discover(b.root,
            *id==neoshared::game::GameId::Kotor1 ? neoshared::override_aliases::Game::Kotor :
                                                neoshared::override_aliases::Game::Kotor2);
        // A separately configured viewer directory keeps its pre-existing
        // precedence. The normal installation Override is already discovered
        // and does not move ahead of the shared INI/numbered-alias sequence.
        if(neoshared::game::isDirectoryPath(b.installation.overridePath)) {
            const auto configured=pathIdentity(b.installation.overridePath);
            if(std::none_of(aliases.begin(),aliases.end(),[&](const auto& root){return pathIdentity(root.path)==configured;})) {
                aliases.insert(aliases.begin(),{"CONFIGURED OVERRIDE",b.installation.overridePath,{},0,false});
                for(std::size_t n=0;n<aliases.size();++n)aliases[n].precedence=n;
            }
        }
        b.overrides.openOverrides(b.root,aliases,job);
        b.overrideRoots=std::move(aliases);

        for(const auto& root:b.overrides.catalog.archives()) {
            job.check();
            std::string description="Override #"+std::to_string(root.overridePrecedence)+" "+root.overrideAlias+": "+
                neoshared::genericPathToUtf8(root.resolvedPath);
            if(!root.overrideIniPath.empty())description+=" ["+neoshared::genericPathToUtf8(root.overrideIniPath)+"]";
            b.diagnostics.push_back(std::move(description));
            for(const auto& message:root.messages)b.diagnostics.push_back(root.overrideAlias+": "+message);
        }
        if(b.key.missingBifCount()) b.diagnostics.push_back(std::to_string(b.key.missingBifCount())+" KEY-referenced BIF archives are unavailable");
        for(const auto& issue:b.key.issues()) {
            job.check();
            b.diagnostics.push_back("KEY/BIF: "+issue.message);
        }
        b.diagnostics.push_back("Preview mounts: distinct configured override > shared override aliases in discovery order > selected module archives > GUI/quality-A textures > KEY/BIF. Alias discovery is shared with NeoBIF; configured-directory and archive layer precedence are explicit viewer policy. Save state, other modules and other texture qualities are excluded.");
        next->diagnostics=b.diagnostics;next->identity="neomdl:installation:"+component(pathIdentity(keyPath));
        for(const auto& root:b.overrides.catalog.archives())
            next->identity+=component(root.overrideAlias)+component(pathIdentity(root.resolvedPath))+
                component(root.overrideIniPath.empty()?std::string{}:pathIdentity(root.overrideIniPath));
        b.identity=next->identity;
        job.update(1,1,"Game installation ready");
        job.check();
        state_=std::move(next);return true;
    } catch(const neoshared::bif::JobCancelled&) {throw;}
      catch(const std::exception& e) {error=e.what();return false;}
}
bool GameResourceSession::openModule(const GameResourceSession& source,const ModuleScope& scope,std::string& error,
                                     const neoshared::bif::JobControl& job) {
    ResourceLoadScope loading(job);
    error.clear();
    try {
        job.update(0,0,"Opening module "+scope.name);
        if(!source.isOpen()) throw std::runtime_error("Open an installation before mounting a module");
        resourceKey(scope.name);
        if(scope.archives.empty()) throw std::runtime_error("A module scope requires an archive");
        auto next=std::make_shared<State>();next->base=source.state_->base;next->module=scope;
        next->mount.open(next->base->root,scope.archives,job);
        next->module->archives=next->mount.paths;
        next->identity="neomdl:module:"+component(next->base->identity)+component(lower(scope.name));
        for(const auto& p:next->mount.paths) next->identity+=component(neoshared::genericPathToUtf8(p));
        next->diagnostics=next->base->diagnostics;
        next->diagnostics.push_back("Active module: "+scope.name+" (immutable resolver snapshot)");
        job.update(1,1,"Module ready: "+scope.name);
        job.check();
        state_=std::move(next);return true;
    } catch(const neoshared::bif::JobCancelled&) {throw;}
      catch(const std::exception& e) {error=e.what();return false;}
}
void GameResourceSession::clear() noexcept {state_.reset();}
bool GameResourceSession::isOpen() const noexcept {return state_&&state_->base&&state_->base->key.isOpen();}
const neoshared::game::GameInstall* GameResourceSession::installation() const noexcept {return state_?&state_->base->installation:nullptr;}
const std::vector<std::string>& GameResourceSession::diagnostics() const noexcept {static const std::vector<std::string> empty;return state_?state_->diagnostics:empty;}
std::vector<ModuleScope> GameResourceSession::modules() const {return state_?state_->base->modules:std::vector<ModuleScope>{};}
const ModuleScope* GameResourceSession::moduleScope() const noexcept {return state_&&state_->module?&*state_->module:nullptr;}
const std::string& GameResourceSession::scopeIdentity() const noexcept {static const std::string empty;return state_?state_->identity:empty;}
std::vector<std::string> GameResourceSession::areaResrefs() const {
    std::set<std::string> names;
    if(state_&&state_->module) for(const auto& r:state_->mount.catalog.resources()) if(r.type==0x7dc) names.insert(lower(r.resref));
    return {names.begin(),names.end()};
}
std::vector<GameResourceEntry> GameResourceSession::resources(const std::vector<std::uint16_t>& types) const {
    std::map<Key,GameResourceEntry> entries;
    const auto add=[&](const std::string& name,std::uint16_t type,const std::string& source) {
        if(std::find(types.begin(),types.end(),type)!=types.end())
            entries.emplace(Key{lower(name),type},GameResourceEntry{lower(name),type,source});
    };
    const auto mount=[&](const Mount& mounted) {
        for(std::size_t i=0;i<mounted.indices.size();++i)for(const auto& [key,indices]:mounted.indices[i])
            add(key.first,key.second,neoshared::pathToUtf8(mounted.paths[i]));
    };
    const auto& base=*state_->base;
    mount(base.overrides);
    if(state_->module)mount(state_->mount);
    mount(base.globals);
    for(const auto& resource:base.key.resources())if(std::find(types.begin(),types.end(),resource.type)!=types.end())
        add(resource.resref,resource.type,neoshared::pathToUtf8(base.key.bifs().at(resource.bifIndex).resolvedPath));
    std::vector<GameResourceEntry> result;result.reserve(entries.size());
    for(auto& [key,entry]:entries)result.push_back(std::move(entry));
    return result;
}
ResourceResolver GameResourceSession::resolver() const {
    const auto state=state_;
    if(!state) return {};
    return [state](const std::string& name,std::uint16_t type)->std::optional<neoshared::ResourceDocument> {
        const auto& job=resourceLoadJob();
        job.check();
        const Key key{resourceKey(name),type};auto& b=*state->base;
        const auto stamp=[&](neoshared::ResourceDocument r,const std::string& layer) {
            r.identity=state->identity+component(layer)+component(r.identity);
            r.sourceDescription="["+(state->module?state->module->name:std::string("installation"))+" / "+layer+"] "+r.sourceDescription;
            return r;
        };
        const auto read=[&](const Mount& mount,std::size_t index,const std::string& layer) {
            const auto& item=mount.catalog.resources().at(index);
            const auto& owner=mount.catalog.archives().at(item.archiveIndex);
            neoshared::ResourceDocument r;
            const auto& source=item.overrideFile?item.directPath:owner.resolvedPath;
            r.fileName=item.fileName();r.type=item.type;
            r.identity=component(neoshared::genericPathToUtf8(source))+
                component(std::to_string(item.archiveResourceIndex))+component(std::to_string(item.resourceId))+
                component(std::to_string(item.type))+component(r.fileName);
            r.sourceDescription=neoshared::genericPathToUtf8(source);
            r.protectedInputs={owner.resolvedPath};
            if(item.overrideFile) {
                r.sourceDescription+=" ("+owner.overrideAlias+", precedence "+std::to_string(owner.overridePrecedence)+
                    ", root "+neoshared::genericPathToUtf8(owner.resolvedPath)+")";
                r.protectedInputs.push_back(item.directPath);
                if(!owner.overrideIniPath.empty()) {
                    r.protectedInputs.push_back(owner.overrideIniPath);
                    r.sourceDescription+=" ["+neoshared::genericPathToUtf8(owner.overrideIniPath)+"]";
                }
            } else r.sourceDescription+=" :: "+r.fileName+" (archive snapshot; Save As creates a separate file)";
            std::string error;
            if(!mount.catalog.readResource(index,r.bytes,error,job))throw std::runtime_error(error);
            return stamp(std::move(r),layer);
        };
        if(auto i=b.overrides.find(key)) {
            const auto& item=b.overrides.catalog.resources().at(*i);
            const auto& owner=b.overrides.catalog.archives().at(item.archiveIndex);
            return read(b.overrides,*i,"Override #"+std::to_string(owner.overridePrecedence)+" "+owner.overrideAlias);
        }
        if(state->module) if(auto i=state->mount.find(key)) return read(state->mount,*i,"module");
        if(auto i=b.globals.find(key)) return read(b.globals,*i,"global textures");
        const auto i=b.keyIndex.find(key);if(i==b.keyIndex.end()) return {};
        if(i->second.size()!=1) throw std::runtime_error("Ambiguous KEY resource: "+key.first);
        const auto index=i->second.front();const auto& item=b.key.resources().at(index);
        const auto& owner=b.key.bifs().at(item.bifIndex);
        neoshared::ResourceDocument r;
        r.fileName=item.fileName();r.type=item.type;
        r.identity=component(neoshared::genericPathToUtf8(owner.resolvedPath))+
            component(std::to_string(item.tableIndex))+component(std::to_string(item.resourceId))+
            component(std::to_string(item.type))+component(r.fileName);
        r.sourceDescription=neoshared::genericPathToUtf8(owner.resolvedPath)+" :: "+r.fileName+
            " (archive snapshot; Save As creates a separate file)";
        r.protectedInputs={b.key.keyPath(),owner.resolvedPath};
        r.bytes.reserve(item.size);
        std::string error;
        job.update(0,item.size,"Reading "+r.fileName);
        if(!b.key.streamResource(index,[&](const std::uint8_t* data,std::size_t size,std::string&) {
            r.bytes.insert(r.bytes.end(),data,data+size);
            job.update(r.bytes.size(),item.size,"Reading "+r.fileName);return true;
        },error,job))throw std::runtime_error(error);
        return stamp(std::move(r),"KEY/BIF");
    };
}
ResourceResolver GameResourceSession::voiceResolver() const {
    const auto state=state_;
    if(!state) return {};
    return [state](const std::string& name,std::uint16_t type)
        ->std::optional<neoshared::ResourceDocument> {
        const auto& job=resourceLoadJob();job.check();
        const auto stem=resourceKey(name);
        auto& b=*state->base;
        if(type!=4&&type!=8) return {};
        const auto& voices=b.voiceIndex(job);
        const auto relative=std::filesystem::path(dialogueVoicePath(stem,b.game)).lexically_normal().generic_string();
        const auto nested=dialogueVoiceUsesOverridePaths(stem,b.game);
        const auto overrideCount=nested?std::min<std::size_t>(
            32u,
            voices.paths.empty()?0u:voices.paths.size()-1u):0u;
        const auto read=[&](std::size_t root,const char* extension)
            ->std::optional<neoshared::ResourceDocument> {
            job.check();
            if(root>=voices.paths.size()) return {};
            const auto found=voices.paths[root].find(relative+extension);
            if(found==voices.paths[root].end()) return {};
            if(found->second.size()!=1)
                throw std::runtime_error("Ambiguous dialogue voice path: "+relative+extension);
            const auto index=found->second.front();
            const auto& item=voices.mount.catalog.resources().at(index);
            // InitializeSource tries the next engine path when fopen fails.
            std::ifstream input(item.directPath,std::ios::binary);
            if(!input) return {};
            const auto& owner=voices.mount.catalog.archives().at(item.archiveIndex);
            neoshared::ResourceDocument result;
            result.fileName=item.fileName();result.type=item.type;job.update(0,item.size,"Reading "+result.fileName);result.bytes=readResourceStream(input,item.directPath);
            job.update(result.bytes.size(),result.bytes.size(),"Read "+result.fileName);
            result.identity=state->identity+component("voice")+component(owner.overrideAlias)+component(neoshared::genericPathToUtf8(item.directPath));
            result.sourceDescription="[engine voice / "+owner.overrideAlias+"] "+neoshared::genericPathToUtf8(item.directPath);
            result.protectedInputs={b.key.keyPath(),owner.resolvedPath,item.directPath};
            if(!owner.overrideIniPath.empty())result.protectedInputs.push_back(owner.overrideIniPath);
            return result;
        };
        std::size_t wavRoot=0;
        const auto wav=[&]() ->std::optional<neoshared::ResourceDocument> {
            for(std::size_t n=1;n<=overrideCount;++n)
                if(auto result=read(n,".wav")) {wavRoot=n;return result;}
            return read(0,".wav");
        };
        if(type==4) return wav();
        if(b.game==model::GameVersion::Kotor1) {
            // The desktop compressed-voice request opens the base WAV stream.
            return read(0,".wav");
        }
        auto first=wav();
        if(first && (wavRoot==0 || (first->bytes.size()>=4u &&
            std::equal(first->bytes.begin(),first->bytes.begin()+4,"RIFF")))) return first;
        if(wavRoot!=0)if(auto base=read(0,".wav"))return base;
        for(std::size_t n=1;n<=overrideCount;++n)
            if(auto result=read(n,".mp3")) return result;
        return {};
    };
}
std::optional<neoshared::ResourceDocument> GameResourceSession::resolve(const std::string& resref,std::uint16_t type) const {
    const auto lookup=resolver();return lookup?lookup(resref,type):std::nullopt;
}
} // namespace neomdl
