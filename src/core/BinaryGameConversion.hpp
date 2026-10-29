#pragma once

#include <neoshared/bif/JobControl.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace neomdl {

// Switch the compiled PC MDL layout between K1 and K2. MDX bytes do not change.
// Return original-to-converted absolute node offsets for editor selection.
std::map<std::size_t,std::size_t> switchBinaryGame(std::vector<std::uint8_t>& mdl,
    const neoshared::bif::JobControl& job = {});

} // namespace neomdl
