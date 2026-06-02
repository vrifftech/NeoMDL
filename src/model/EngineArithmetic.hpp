#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace neomdl::model {
// The inspected x86 CVTTSS2SI uses the indefinite INT_MIN result for invalid
// conversions.
inline std::int32_t engineFloatToInt(float value) noexcept {
    if(value>=2147483648.0f)
        return std::numeric_limits<std::int32_t>::min();
    if(!(value>-2147483648.0f))
        return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}
// Preserve the desktop signed-division contract, including invalid operands.
inline std::int32_t engineSignedRemainder(std::int32_t value,std::int32_t divisor) noexcept {
    return value%divisor;
}
inline std::int32_t engineIntAdd(std::int32_t left,std::int32_t right) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(left)+
        static_cast<std::uint32_t>(right));
}
inline std::int32_t engineIntAbs(std::int32_t value) noexcept {
    return value<0 ? static_cast<std::int32_t>(0u-static_cast<std::uint32_t>(value)) : value;
}
}
