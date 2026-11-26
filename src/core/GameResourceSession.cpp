#include "core/GameResourceSession.hpp"
#include "core/Engine2DA.hpp"
#include <twoda/TwoDAFile.hpp>
#include "core/DialogueRuntime.hpp"
#include <fstream>
#include <neoshared/bif/KeyBifArchive.hpp>
#include <neoshared/bif/LooseArchiveCatalog.hpp>
#include <neoshared/bif/ResourceQuery.hpp>
#include <neoshared/game/OverrideAliases.hpp>
#include <neoshared/PathUtf8.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace neomdl {
namespace {
using Path=std::filesystem::path;
using Key=std::pair<std::string,std::uint16_t>;
using Index=std::map<Key,std::size_t>;
std::string lower(std::string s) {for(auto& c:s) if(c>='A'&&c<='Z') c=char(c+32);return s;}
std::string pathIdentity(const Path& p) {return neoshared::genericPathToUtf8(std::filesystem::weakly_canonical(p));}
std::string component(const std::string& s) {return std::to_string(s.size())+":"+s;}

using ArchiveFiles=std::map<std::string,std::vector<Path>>;
ArchiveFiles installationArchives(const Path& root,const std::map<std::string,Path>& aliases,
    const std::vector<Path>& contentRoots,const neoshared::bif::JobControl& job) {
    ArchiveFiles files;std::map<std::string,Path> directories;
    for(const auto& directory:std::filesystem::directory_iterator(root)) {
        job.check();
        const auto folder=lower(neoshared::pathToUtf8(directory.path().filename()));
        if(neoshared::bif::LooseArchiveCatalog::hasSupportedExtension(directory.path())&&directory.is_regular_file()) {
            files[folder].push_back(directory.path());continue;
        }
        if(directory.is_directory())directories.emplace(folder,directory.path());
    }
    const auto scan=[&](const std::string& folder,const Path& directory) {
        job.update(0,0,"Discovering archives: "+neoshared::pathToUtf8(directory));
        std::error_code error;std::filesystem::directory_iterator entry(directory,error),end;
        if(error&&error!=std::errc::no_such_file_or_directory&&error!=std::errc::not_a_directory)
            throw std::filesystem::filesystem_error("Cannot enumerate game archives",directory,error);
        for(;!error&&entry!=end;++entry) {
            const auto& file=*entry;
            job.check();
            if(neoshared::bif::LooseArchiveCatalog::hasSupportedExtension(file.path())&&file.is_regular_file())
                files[folder+"/"+lower(neoshared::pathToUtf8(file.path().filename()))].push_back(file.path());
        }
    };
    for(const auto& content:contentRoots)for(const auto* folder:{"modules","lips"})
        if(const auto directory=uniqueResourceDirectory(content,folder))scan(folder,*directory);
    for(const auto* folder:{"modules","rims","texturepacks","lips","override"}) {
        if(const auto alias=aliases.find(folder);alias!=aliases.end())scan(folder,alias->second);
        else if(const auto directory=directories.find(folder);directory!=directories.end())scan(folder,directory->second);
    }
    return files;
}

struct Mount {
    neoshared::bif::LooseArchiveCatalog catalog;
    Index index; // first archive in caller priority, independent of catalog sort
    std::vector<Path> paths;
    void indexArchive(const neoshared::bif::LooseArchiveInfo& archive,const neoshared::bif::JobControl& job) {
        Index entries;
        for(const auto n:archive.resourceIndices) {
            job.check();
            const auto& r=catalog.resources().at(n);
            entries.emplace(Key{lower(r.resref),r.type},n);
        }
        index.merge(entries);
    }
    void open(const Path& root,const std::vector<Path>& requested,const neoshared::bif::JobControl& job) {
        std::set<std::string> unique;
        for(const auto& requestedPath:requested) {
            job.check();
            const auto p=std::filesystem::weakly_canonical(requestedPath.is_absolute()?requestedPath:root/requestedPath);
            if(unique.insert(neoshared::genericPathToUtf8(p)).second)paths.push_back(p);
        }
        if(!catalog.openFiles(root,paths,job,neoshared::bif::GameProfile::KotOR))
            throw std::runtime_error(catalog.lastError());
        std::map<std::string,const neoshared::bif::LooseArchiveInfo*> archives;
        for(const auto& archive:catalog.archives()) archives.emplace(neoshared::genericPathToUtf8(archive.resolvedPath),&archive);
        for(std::size_t n=0;n<paths.size();++n) {
            const auto& path=paths[n];
            job.update(n,paths.size(),"Indexing mounted resources: "+neoshared::pathToUtf8(path));
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
        resourceLoadJob().check();
        if(const auto i=index.find(key);i!=index.end()) {
            // Unreadable high-priority resources are errors, not fallthrough.
            return i->second;
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
    std::shared_ptr<const neoshared::bif::ModFolder> mod;
    Mount priority;
    Mount images;
    Mount globals;
    Mount overrides;
    std::vector<Path> contentRoots;
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
            if(game==model::GameVersion::Kotor2)for(const auto& root:contentRoots) {
                neoshared::override_aliases::OverrideRoot voice{alias+" / "+neoshared::pathToUtf8(root),directory(root),{},roots.size(),false};
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
    Mount images;
    Mount archives;
    std::string identity;
    std::vector<std::string> diagnostics;
};
GameResourceSession::GameResourceSession()=default;
GameResourceSession::~GameResourceSession()=default;
GameResourceSession::GameResourceSession(GameResourceSession&&) noexcept=default;
GameResourceSession& GameResourceSession::operator=(GameResourceSession&&) noexcept=default;

bool GameResourceSession::open(const neoshared::game::GameInstall& supplied,std::string& error,
                               const neoshared::bif::JobControl& job,std::shared_ptr<const neoshared::bif::ModFolder> mod) {
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
        auto& b=*next->base;b.installation=supplied;b.mod=std::move(mod);
        b.game=*id==neoshared::game::GameId::Kotor1?model::GameVersion::Kotor1:model::GameVersion::Kotor2;
        neoshared::game::refreshDerivedPaths(*definition,b.installation);
        b.root=neoshared::game::normalizeConfiguredPath(b.installation.installPath);
        const auto keyPath=neoshared::game::findExistingPathCaseInsensitive(b.root/"chitin.key");
        if(!b.key.open(keyPath,{},{},job,b.game==model::GameVersion::Kotor1)||b.key.gameProfile()!=neoshared::bif::GameProfile::KotOR)
            throw std::runtime_error("Cannot index KotOR KEY: "+b.key.lastError());
        for(const auto& r:b.key.resources()) {
            job.check();
            b.keyIndex.emplace(Key{lower(r.resref),r.type},r.index);
        }
        job.update(0,0,"Discovering override aliases");
        auto aliases=neoshared::override_aliases::discover(b.root,
            *id==neoshared::game::GameId::Kotor1 ? neoshared::override_aliases::Game::Kotor :
                                                neoshared::override_aliases::Game::Kotor2);
        if(b.game==model::GameVersion::Kotor2) {
            std::ifstream table;table.exceptions(std::ios::failbit|std::ios::badbit);table.open(b.installation.tlkPath,std::ios::binary);
            std::array<unsigned char,12> header{};table.read(reinterpret_cast<char*>(header.data()),header.size());
            const auto language=std::uint32_t(header[8])|(std::uint32_t(header[9])<<8)|(std::uint32_t(header[10])<<16)|(std::uint32_t(header[11])<<24);
            const char* languages[]={"English","French","German","Italian","Spanish","English"};
            b.contentRoots=neoshared::override_aliases::contentRoots(b.root,neoshared::override_aliases::Game::Kotor2,language<6?languages[language]:"English");
            std::vector<neoshared::override_aliases::OverrideRoot> layered;
            for(const auto& root:b.contentRoots)if(const auto folder=uniqueResourceDirectory(root,"override"))
                layered.push_back({neoshared::pathToUtf8(root),*folder,{},layered.size(),false});
            layered.insert(layered.end(),aliases.begin(),aliases.end());aliases=std::move(layered);
        }
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
        for(auto& alias:aliases)alias.maximumDepth=neoshared::override_aliases::overrideDepth(
            b.game==model::GameVersion::Kotor1?neoshared::override_aliases::Game::Kotor:neoshared::override_aliases::Game::Kotor2);
        const auto directoryAliases=neoshared::override_aliases::directoryAliases(b.root,
            b.game==model::GameVersion::Kotor1?neoshared::override_aliases::Game::Kotor:neoshared::override_aliases::Game::Kotor2);
        const auto directory=[&](const std::string& name) {
            const auto found=directoryAliases.find(name);
            return neoshared::override_aliases::OverrideRoot{name,found==directoryAliases.end()?
                neoshared::game::findExistingPathCaseInsensitive(b.root/name):found->second,{},0,false,0};
        };
        std::vector<neoshared::override_aliases::OverrideRoot> roots;
        if(b.game==model::GameVersion::Kotor1)for(const auto* name:{"streammusic","streamwaves","movies","portraits","servervault","rims","errortex"})roots.push_back(directory(name));
        else for(const auto* name:{"movies","rims"})roots.push_back(directory(name));
        roots.insert(roots.end(),aliases.begin(),aliases.end());roots.push_back(directory("tempclient"));
        for(std::size_t n=0;n<roots.size();++n)roots[n].precedence=n;
        b.overrides.openOverrides(b.root,roots,job);

        job.update(0,0,"Discovering game archives");
        auto files=installationArchives(b.root,directoryAliases,b.contentRoots,job);std::set<std::string> names;
        for(auto it=files.begin();it!=files.end();) {
            if(neoshared::pathFromUtf8(it->first).extension()==".mzd") {
                if(b.game==model::GameVersion::Kotor1) {
                    auto name=neoshared::pathFromUtf8(it->first);name.replace_extension(".mod");
                    files[neoshared::genericPathToUtf8(name)]=std::move(it->second);
                }
                it=files.erase(it);
            } else ++it;
        }
        for(const auto& [file,paths]:files) {
            job.check();
            if(file.rfind("modules/",0)==0||file.rfind("rims/",0)==0) {
                const auto name=neoshared::bif::ModFolder::moduleName(neoshared::pathFromUtf8(file));
                if(!name.empty()&&name!="global")names.insert(name);
            }
        }
        if(b.mod)for(const auto& [name,archives]:b.mod->modules())names.insert(name);
        const auto append=[&](std::vector<Path>& target,const std::string& file) {
            if(const auto found=files.find(file);found!=files.end())
                for(const auto& path:found->second)if(!b.mod||!b.mod->archive(path))target.push_back(path);
        };
        std::vector<Path> priority,images;
        append(priority,"patch.erf");
        if(b.game==model::GameVersion::Kotor1)append(priority,"override/textures.erf");
        append(images,"rims/global.rim");
        b.priority.open(b.root,priority,job);b.images.open(b.root,images,job);
        for(const auto& name:names) {
            ModuleScope scope;scope.name=name;
            const auto add=[&](const std::string& folder,const std::string& file) {
                if(b.mod)if(auto path=b.mod->archive(file)) {scope.archives.push_back(*path);return;}
                append(scope.archives,folder.empty()?file:folder+"/"+file);
            };
            const bool mod=(b.mod&&b.mod->archive(name+".mod"))||files.count("modules/"+name+".mod");
            if(!mod) {
                add("modules",name+".rim");
                const auto split="modules/"+name+"_s.rim";
                if(b.mod&&b.mod->archive(name+"_s.rim"))add("modules",name+"_s.rim");
                else if(const auto found=files.find(split);found!=files.end()) {
                    const auto& variants=found->second;auto selected=variants.back();
                    for(auto path=variants.rbegin();path!=variants.rend();++path)
                        if(std::find(b.contentRoots.begin(),b.contentRoots.end(),path->parent_path().parent_path())!=b.contentRoots.end()) {selected=*path;break;}
                    scope.archives.push_back(selected);
                }
            }
            add("rims",name+"_adx.rim");add("rims",name+"_a.rim");
            if(mod)add("modules",name+".mod");
            else if(b.game==model::GameVersion::Kotor2)add("modules",name+"_dlg.erf");
            if(scope.archives.empty())continue;
            auto lips=b.contentRoots;const auto lipAlias=directoryAliases.find("lips");lips.push_back(lipAlias==directoryAliases.end()?b.root:lipAlias->second.parent_path());
            for(const auto& root:lips)for(const auto& file:{std::string("localization.mod"),name+"_loc.mod"})
                if(const auto found=files.find("lips/"+file);found!=files.end())for(const auto& path:found->second)
                    if(path.parent_path().parent_path()==root&&(!b.mod||!b.mod->archive(path)))scope.archives.push_back(path);
            if(b.game==model::GameVersion::Kotor1)add("","players.erf");
            if(!scope.archives.empty())b.modules.push_back(std::move(scope));
        }
        // Texture pack names are row 2 of the resolved table, including edits
        // supplied by the Mod folder. The GUI pack is mounted after world art.
        GameResourceSession bootstrap;bootstrap.state_=next;
        std::vector<Path> packs;
        if(auto resource=bootstrap.resolve("texpacks",0x7e1)) {
            neo2da::TwoDAFile table;table.loadBytes(resource->bytes);
            for(const auto* column:{"Gui","Texture"}) {
                std::string name;engine2da::getString(table,2,column,name);
                if(!name.empty())append(packs,"texturepacks/"+lower(name)+".erf");
            }
        }
        b.globals.open(b.root,packs,job);
        if(b.mod)b.diagnostics.push_back("Mod folder has priority: "+neoshared::pathToUtf8(b.mod->root()));

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
        b.diagnostics.push_back("Resource mounts: Mod folder > resource directories > priority archives > module/global RIM images > module/lip archives > tex packs > KEY/BIF. Textures choose source before format. Save state and unrelated modules are excluded.");
        next->diagnostics=b.diagnostics;next->identity="neomdl:installation:"+component(pathIdentity(keyPath));
        if(b.mod)next->identity+=component(b.mod->identity());
        for(const auto& root:b.overrides.catalog.archives())
            next->identity+=component(root.overrideAlias)+component(pathIdentity(root.resolvedPath))+
                component(root.overrideIniPath.empty()?std::string{}:pathIdentity(root.overrideIniPath));
        for(const auto* mount:{&b.priority,&b.images,&b.globals})for(const auto& path:mount->paths)next->identity+=component(pathIdentity(path));
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
        std::vector<Path> images,archives;
        for(const auto& path:scope.archives) {
            if(next->base->mod&&next->base->mod->ownsArchive(path))continue;
            (lower(neoshared::pathToUtf8(path.extension()))==".rim"?images:archives).push_back(path);
        }
        next->images.open(next->base->root,images,job);next->archives.open(next->base->root,archives,job);
        next->identity="neomdl:module:"+component(next->base->identity)+component(lower(scope.name));
        for(const auto& p:scope.archives)next->identity+=component(neoshared::genericPathToUtf8(p));
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
std::shared_ptr<const neoshared::bif::ModFolder> GameResourceSession::modFolder() const {return state_?state_->base->mod:nullptr;}
std::vector<ModuleScope> GameResourceSession::modules() const {return state_?state_->base->modules:std::vector<ModuleScope>{};}
const ModuleScope* GameResourceSession::moduleScope() const noexcept {return state_&&state_->module?&*state_->module:nullptr;}
const std::string& GameResourceSession::scopeIdentity() const noexcept {static const std::string empty;return state_?state_->identity:empty;}
std::vector<std::string> GameResourceSession::areaResrefs() const {
    std::set<std::string> names;
    if(state_&&state_->module) {
        for(const auto* mount:{&state_->images,&state_->archives})for(const auto& r:mount->catalog.resources())if(r.type==0x7dc)names.insert(lower(r.resref));
        if(state_->base->mod)for(const auto* r:state_->base->mod->resources(state_->module->name))if(r->type==0x7dc)names.insert(lower(r->resref));
    }
    return {names.begin(),names.end()};
}
std::vector<GameResourceEntry> GameResourceSession::resources(const std::vector<std::uint16_t>& types) const {
    std::map<Key,GameResourceEntry> entries;
    const auto& base=*state_->base;const auto scope=state_->module?state_->module->name:std::string{};
    const auto add=[&](const std::string& name,std::uint16_t type,const std::string& source) {
        if(base.mod&&base.mod->shadows(name,type,scope))return;
        if(std::find(types.begin(),types.end(),type)!=types.end())
            entries.emplace(Key{lower(name),type},GameResourceEntry{lower(name),type,source});
    };
    const auto mount=[&](const Mount& mounted) {
        for(const auto& [key,index]:mounted.index) {
            const auto& resource=mounted.catalog.resources().at(index);
            add(key.first,key.second,neoshared::pathToUtf8(mounted.catalog.archives().at(resource.archiveIndex).resolvedPath));
        }
    };
    if(base.mod)for(const auto* r:base.mod->resources(scope))if(std::find(types.begin(),types.end(),r->type)!=types.end())
        entries.emplace(Key{lower(r->resref),r->type},GameResourceEntry{lower(r->resref),r->type,"Mod folder: "+neoshared::pathToUtf8(base.mod->root())});
    mount(base.overrides);
    mount(base.priority);mount(state_->images);mount(base.images);mount(state_->archives);
    mount(base.globals);
    for(const auto& resource:base.key.resources())if(std::find(types.begin(),types.end(),resource.type)!=types.end())
        add(resource.resref,resource.type,neoshared::pathToUtf8(base.key.bifs().at(resource.bifIndex).resolvedPath));
    std::vector<GameResourceEntry> result;result.reserve(entries.size());
    for(auto& [key,entry]:entries)result.push_back(std::move(entry));
    return result;
}
ResourceResolver GameResourceSession::installationResolver() const {
    GameResourceSession session;session.state_=std::make_shared<State>();
    session.state_->base=state_->base;session.state_->identity=state_->base->identity;
    return session.resolver();
}
ResourceResolver GameResourceSession::resolver() const {
    const auto state=state_;
    if(!state) return {};
    return [state](const std::string& name,std::uint16_t type)->std::optional<neoshared::ResourceDocument> {
        const auto& job=resourceLoadJob();
        job.check();
        const Key key{resourceKey(name),type};auto& b=*state->base;
        const auto stamp=[&](neoshared::ResourceDocument r,const std::string& layer) {
            r.module=state->module?state->module->name:std::string{};
            r.identity=state->identity+component(layer)+component(r.identity);
            r.sourceDescription="["+(state->module?state->module->name:std::string("installation"))+" / "+layer+"] "+r.sourceDescription;
            return r;
        };
        const auto read=[&](const Mount& mount,std::size_t index,const std::string& layer) {
            const auto& item=mount.catalog.resources().at(index);
            const auto& owner=mount.catalog.archives().at(item.archiveIndex);
            neoshared::ResourceDocument r;
            const auto& source=item.overrideFile?item.directPath:owner.resolvedPath;
            r.fileName=item.fileName();r.type=item.type;r.sourcePath=source;
            r.priority=item.overrideFile?neoshared::ResourcePriority::Directory:
                lower(neoshared::pathToUtf8(source.extension()))==".rim"?neoshared::ResourcePriority::Image:neoshared::ResourcePriority::Archive;
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
        if(b.mod&&b.mod->shadows(key.first,key.second,state->module?state->module->name:std::string{}))
            return b.mod->resolve(key.first,key.second,state->module?state->module->name:std::string{},job);
        if(type==3006)return {}; // TXB is not a PC-game texture candidate.
        struct Location {const Mount* mount;std::size_t index;neoshared::ResourcePriority priority;};
        const auto locate=[&](const Key& wanted)->std::optional<Location> {
            for(const auto* mount:{&b.overrides,&b.priority,&state->images,&b.images,&state->archives,&b.globals})
                if(const auto index=mount->find(wanted))return Location{mount,*index,mount==&b.overrides?neoshared::ResourcePriority::Directory:
                    mount==&state->images||mount==&b.images?neoshared::ResourcePriority::Image:neoshared::ResourcePriority::Archive};
            const auto found=b.keyIndex.find(wanted);if(found==b.keyIndex.end())return {};
            return Location{nullptr,found->second,neoshared::ResourcePriority::Key};
        };
        const auto selected=locate(key);if(!selected)return {};
        if(neoshared::isTextureType(type)) {
            for(const auto format:neoshared::kotorTextureTypes)
                if(const auto other=locate({key.first,format});other&&
                    neoshared::texturePreference(other->priority,format)<
                    neoshared::texturePreference(selected->priority,type))return {};
        }
        if(selected->mount)return read(*selected->mount,selected->index,selected->mount==&b.overrides?"directory":
            selected->mount==&b.priority?"priority archive":selected->priority==neoshared::ResourcePriority::Image?"image":"archive");
        const auto index=selected->index;const auto& item=b.key.resources().at(index);
        const auto& owner=b.key.bifs().at(item.bifIndex);
        neoshared::ResourceDocument r;
        r.fileName=item.fileName();r.type=item.type;r.priority=neoshared::ResourcePriority::Key;
        r.identity=component(neoshared::genericPathToUtf8(owner.resolvedPath))+
            component(std::to_string(item.tableIndex))+component(std::to_string(item.resourceId))+
            component(std::to_string(item.type))+component(r.fileName);
        r.sourceDescription=neoshared::genericPathToUtf8(owner.resolvedPath)+" :: "+r.fileName+
            " (archive snapshot; Save As creates a separate file)";
        r.sourcePath=owner.resolvedPath;r.protectedInputs={b.key.keyPath(),owner.resolvedPath};
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
        const auto relative=std::filesystem::path(dialogueVoicePath(stem,b.game)).lexically_normal().generic_string();
        if(b.mod) {
            const auto folder=b.game==model::GameVersion::Kotor1?"streamwaves/":"streamvoice/";
            if(auto result=b.mod->resolveFile(folder+relative+".wav",job))return result;
            if(type==8)if(auto result=b.mod->resolveFile(folder+relative+".mp3",job))return result;
            if(b.mod->shadows(stem,type,std::string{}))return b.mod->resolve(stem,type,std::string{},job);
        }
        const auto& voices=b.voiceIndex(job);
        const auto nested=dialogueVoiceUsesOverridePaths(stem,b.game);
        const auto overrideCount=nested?std::min<std::size_t>(
            16u,
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
