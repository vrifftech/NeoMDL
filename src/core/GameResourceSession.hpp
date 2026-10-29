#pragma once

#include "core/ModelDocument.hpp"

#include <neoshared/game/Installation.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

// Read-only installation-backed resource session for the standalone viewer.
// neoshared owns the archive-format implementation; NeoMDL only exposes its
// narrow (ResRef, resource type) lookup boundary to ModelDocument.
struct ModuleScope {
    std::string name;
    // Highest priority first. Explicit host policy; save/CURRENTGAME state is
    // not silently mounted, and unrelated modules never participate. Relative
    // paths are installation-root-relative, never working-directory-relative.
    std::vector<std::filesystem::path> archives;
};

struct GameResourceEntry {
    std::string resref;
    std::uint16_t type{};
    std::string source;
};

class GameResourceSession final {
public:
    GameResourceSession();
    ~GameResourceSession();

    GameResourceSession(const GameResourceSession&) = delete;
    GameResourceSession& operator=(const GameResourceSession&) = delete;
    GameResourceSession(GameResourceSession&&) noexcept;
    GameResourceSession& operator=(GameResourceSession&&) noexcept;

    bool open(const neoshared::game::GameInstall& installation,
              std::string& error, const neoshared::bif::JobControl& job = resourceLoadJob());
    // Transactional: a failed mount leaves this session and captured resolvers
    // intact. Scope snapshots share only the read-only installation catalog.
    bool openModule(const GameResourceSession& installation,
                    const ModuleScope& scope, std::string& error,
                    const neoshared::bif::JobControl& job = resourceLoadJob());
    std::vector<ModuleScope> modules() const;
    const ModuleScope* moduleScope() const noexcept;
    const std::string& scopeIdentity() const noexcept;
    std::vector<std::string> areaResrefs() const;
    std::vector<GameResourceEntry> resources(const std::vector<std::uint16_t>& types) const;
    void clear() noexcept;

    bool isOpen() const noexcept;
    const neoshared::game::GameInstall* installation() const noexcept;
    const std::vector<std::string>& diagnostics() const noexcept;

    ResourceResolver resolver() const;
    // Voice stream candidates are a separate engine path from archive lookup.
    // The type argument selects engine source type 8 or its type-4 retry.
    ResourceResolver voiceResolver() const;
    std::optional<neoshared::ResourceDocument> resolve(
        const std::string& resref, std::uint16_t type) const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace neomdl
