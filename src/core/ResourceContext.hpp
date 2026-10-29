#pragma once

#include <neoshared/ResourceDocument.hpp>
#include <neoshared/bif/JobControl.hpp>
#include <neoshared/texture/Operation.hpp>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

// A loading operation owns its callbacks only until its worker finishes.
// Retained resolver snapshots consult this thread's active operation at call
// time instead of retaining progress dialogs or cancellation-state references.
const neoshared::bif::JobControl& resourceLoadJob() noexcept;
class ResourceLoadScope final {
public:
    explicit ResourceLoadScope(const neoshared::bif::JobControl& job);
    ~ResourceLoadScope();
    ResourceLoadScope(const ResourceLoadScope&)=delete;
    ResourceLoadScope& operator=(const ResourceLoadScope&)=delete;
private:
    neoshared::bif::JobControl job_;
    const neoshared::bif::JobControl* previous_{};
    std::optional<neoshared::texture::OperationScope> textureOperation_;
};

// Resolver closures retained by documents must own their captures. Module
// sessions provide immutable mount snapshots; they do not follow UI selection.
using ResourceResolver = std::function<std::optional<neoshared::ResourceDocument>(
    const std::string&, std::uint16_t)>;

enum class DependencyStatus { Found, Missing, Failed };
struct ResourceDependency {
    std::string requester, resref;
    std::uint16_t type{};
    DependencyStatus status{DependencyStatus::Missing};
    std::string identity, source, error;
    std::vector<std::filesystem::path> inputs;
};

// Metadata only: no byte payloads, archive readers, or GPU objects retained.
// Repeated identical requests are coalesced, including procedural TXI probes.
class ResourceTrace final {
public:
    void record(const std::string& requester, const std::string& name,
                std::uint16_t type, const neoshared::ResourceDocument* result,
                const std::string& error = {});
    std::vector<ResourceDependency> entries() const;
    std::vector<std::filesystem::path> inputs() const;
private:
    mutable std::mutex mutex_;
    std::vector<ResourceDependency> entries_;
    std::set<std::tuple<std::string,std::string,std::uint16_t,DependencyStatus,std::string,std::string>> keys_;
};
ResourceResolver traceResources(ResourceResolver resolver,
    std::shared_ptr<ResourceTrace> trace, std::string requester);
// Unique case-folded sibling lookup; ambiguity is an error, never permission
// to try a lower-priority mount. No implicit current-working-directory search.
ResourceResolver directoryResources(const std::filesystem::path& directory,
                                    ResourceResolver fallback = {});
std::optional<std::filesystem::path> uniqueResourceFile(
    const std::filesystem::path& directory, const std::string& filename);
std::optional<std::filesystem::path> uniqueResourceDirectory(
    const std::filesystem::path& directory, const std::string& name);
std::string resourceFileIdentity(const std::filesystem::path& path);
std::vector<std::uint8_t> readResourceFile(const std::filesystem::path& path);
std::vector<std::uint8_t> readResourceStream(std::istream& input,const std::filesystem::path& path);
std::string resourceKey(const std::string& name);
std::string resourceExtension(std::uint16_t type);

} // namespace neomdl
