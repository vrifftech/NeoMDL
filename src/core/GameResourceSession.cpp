#include "core/GameResourceSession.hpp"
#include "core/DialogueRuntime.hpp"
#include "core/DialogueAudioSource.hpp"
#include <fstream>
#include <neoshared/bif/KeyBifArchive.hpp>
#include <neoshared/bif/LooseArchiveCatalog.hpp>
#include <neoshared/bif/ResourceDocument.hpp>
#include <neoshared/bif/ResourceQuery.hpp>
#include <neoshared/game/OverrideAliases.hpp>
#include <neoshared/PathUtf8.hpp>
#include <algorithm>
#include <map>
#include <limits>
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
        for(const auto& archive:catalog.archives()) archives.emplace(pathIdentity(archive.resolvedPath),&archive);
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
struct InstallationState {
    neoshared::game::GameInstall installation;
    model::GameVersion game{model::GameVersion::Kotor2};
    Path root;
    neoshared::bif::KeyBifArchive key;
    Index keyIndex;
    Mount globals;
    Mount overrides;
    Mount voices;
    std::vector<std::map<std::string,std::vector<std::size_t>>> voicePaths;
    std::string identity;
    std::vector<ModuleScope> modules;
    std::vector<std::string> diagnostics;
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
        if(!definition||!neoshared::game::isValidGameInstallation(*definition,supplied.installPath))
            throw std::runtime_error("The selected game directory is not a validated installation");
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
        const auto paths=neoshared::bif::LooseArchiveCatalog::scanForArchiveFiles(
            b.root,std::numeric_limits<std::size_t>::max(),job);
        std::map<std::string,Path> globalFiles;
        std::map<std::string,std::map<std::string,Path>> grouped;
        for(const auto& p:paths) {
            job.check();
            // Discovery is installation-root scoped, not a recursive save-game
            // search. Nested backups can contain their own Modules directory.
            const auto relative=p.lexically_relative(b.root);
            if(std::distance(relative.begin(),relative.end())!=2)continue;
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
        if(neoshared::game::isDirectoryPath(b.installation.overridePath) &&
           std::none_of(aliases.begin(),aliases.end(),[&](const auto& root) {
               return pathIdentity(root.path)==pathIdentity(b.installation.overridePath);
           })) {
            aliases.insert(aliases.begin(),{"CONFIGURED OVERRIDE",b.installation.overridePath,{},0,false});
            for(std::size_t n=0;n<aliases.size();++n)aliases[n].precedence=n;
        }
        b.overrides.openOverrides(b.root,aliases,job);
        // Preserve complete directory/revision snapshots through the shared
        // reader. Engine voice callers select relative stream paths, rather
        // than treating nested voice files as flat archive ResRefs.
        job.update(0,0,"Indexing dialogue voice streams");
        std::vector<neoshared::override_aliases::OverrideRoot> voices;
        const std::string voiceFolder=b.game==model::GameVersion::Kotor1?"streamwaves":"streamvoice";
        const std::string voiceAlias=b.game==model::GameVersion::Kotor1?"STREAMWAVES":"STREAMVOICE";
        const auto voiceDirectory=[&](const Path& parent) {
            if(!std::filesystem::is_directory(parent)) return parent/voiceFolder;
            return uniqueResourceDirectory(parent,voiceFolder).value_or(parent/voiceFolder);
        };
        voices.push_back({voiceAlias,voiceDirectory(b.root),{},0,false});
        // K1 Play resolves HD0:STREAMWAVES once; its InitializeSource has
        // no K2 streamvoice override-directory loop (Desktop 0x3bfdae).
        if(b.game==model::GameVersion::Kotor2)for(const auto& root:aliases) {
            auto voice=root;voice.path=voiceDirectory(root.path);
            voice.alias=voiceAlias+" / "+root.alias;voice.precedence=voices.size();
            voices.push_back(std::move(voice));
        }
        b.voices.openOverrides(b.root,voices,job);
        for(const auto& archive:b.voices.catalog.archives()) {
            std::map<std::string,std::vector<std::size_t>> index;
            for(const auto n:archive.resourceIndices) {
                job.check();
                const auto& item=b.voices.catalog.resources().at(n);
                if(item.type==4 || item.type==8)
                    index[lower(neoshared::genericPathToUtf8(item.directPath.lexically_relative(archive.resolvedPath)))].push_back(n);
            }
            b.voicePaths.push_back(std::move(index));
        }

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
        for(const auto& p:next->mount.paths) next->identity+=component(pathIdentity(p));
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
            auto r=neoshared::bif::readResourceDocument(b.key,mount.catalog,{neoshared::bif::ResourceSource::LooseArchive,index},job);
            r.protectedInputs={owner.resolvedPath};
            if(item.overrideFile) {
                r.protectedInputs.push_back(item.directPath);
                if(!owner.overrideIniPath.empty())r.protectedInputs.push_back(owner.overrideIniPath);
            }
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
        auto r=neoshared::bif::readResourceDocument(b.key,b.globals.catalog,{neoshared::bif::ResourceSource::KeyBif,index},job);
        r.protectedInputs={b.key.keyPath(),b.key.bifs().at(item.bifIndex).resolvedPath};
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
        const auto relative=std::filesystem::path(dialogueVoicePath(stem,b.game)).lexically_normal().generic_string();
        const auto nested=dialogueVoiceUsesOverridePaths(stem,b.game);
        const auto overrideCount=nested?std::min<std::size_t>(
            32u,
            b.voicePaths.empty()?0u:b.voicePaths.size()-1u):0u;
        const auto read=[&](std::size_t root,const char* extension)
            ->std::optional<neoshared::ResourceDocument> {
            job.check();
            if(root>=b.voicePaths.size()) return {};
            const auto found=b.voicePaths[root].find(relative+extension);
            if(found==b.voicePaths[root].end()) return {};
            if(found->second.size()!=1)
                throw std::runtime_error("Ambiguous dialogue voice path: "+relative+extension);
            const auto index=found->second.front();
            const auto& item=b.voices.catalog.resources().at(index);
            // InitializeSource tries the next engine path when fopen fails.
            std::ifstream probe(item.directPath,std::ios::binary);
            if(!probe) return {};
            probe.close();
            auto result=neoshared::bif::readResourceDocument(b.key,b.voices.catalog,
                {neoshared::bif::ResourceSource::LooseArchive,index},job);
            const auto& owner=b.voices.catalog.archives().at(item.archiveIndex);
            result.identity=state->identity+component("voice")+component(result.identity);
            result.sourceDescription="[engine voice / "+owner.overrideAlias+"] "+result.sourceDescription;
            return result;
        };
        const auto wav=[&]() ->std::optional<neoshared::ResourceDocument> {
            for(std::size_t n=1;n<=overrideCount;++n)
                if(auto result=read(n,".wav")) return result;
            return read(0,".wav");
        };
        if(type==4) return wav();
        if(type!=8) return {};
        if(b.game==model::GameVersion::Kotor1) {
            // The desktop compressed-voice request opens the base WAV stream.
            return read(0,".wav");
        }




        auto first=wav();
        if(first && first->bytes.size()>=4u &&
            std::equal(first->bytes.begin(),first->bytes.begin()+4,"RIFF")) return first;
        if(auto base=read(0,".wav")) return base;
        for(std::size_t n=1;n<=overrideCount;++n)
            if(auto result=read(n,".mp3")) return result;
        return {};
    };
}
std::optional<neoshared::ResourceDocument> GameResourceSession::resolve(const std::string& resref,std::uint16_t type) const {
    const auto lookup=resolver();return lookup?lookup(resref,type):std::nullopt;
}
} // namespace neomdl
