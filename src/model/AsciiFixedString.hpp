#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace neomdl::model {

// Engine fixed fields use prefix writes, retaining initialized bytes after
// a shorter replacement's NUL. Unknown constructor/heap tails remain zero.
// Preserve the semantic std::string API and retain only authoring history.
template<std::size_t N>
void retainAsciiFixedString(std::array<std::uint8_t,N>& bytes,
                            const std::string& previous,const std::string& value) {
    const auto write=[&](const std::string& text) {
        const auto count=std::min(text.size(),N);
        std::copy_n(text.begin(),count,bytes.begin());
        if(count<N) bytes[count]=0u;
    };
    write(previous);
    write(value);
}

} // namespace neomdl::model
