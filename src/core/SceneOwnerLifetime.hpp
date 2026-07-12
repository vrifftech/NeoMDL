#pragma once
#include <memory>
#include <optional>
#include <stdexcept>

namespace neomdl {
// Engine late buckets borrow Gob pointers. Chunk destruction does not remove
// those entries; observe that boundary without retaining or purging the Gob.
// An absent token is an owner whose lifetime is managed by its scene document.
using SceneOwnerLifetime = std::optional<std::weak_ptr<const void>>;

inline void requireSceneOwnerLifetime(const SceneOwnerLifetime& lifetime) {
    if(lifetime && lifetime->expired())
        throw std::runtime_error("Engine scene owner was destroyed after bucket admission; the late Gob pointer is invalid");
}
} // namespace neomdl
