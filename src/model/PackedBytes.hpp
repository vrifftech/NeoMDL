#pragma once

#include "model/Model.hpp"

#include <cstring>
#include <sstream>

namespace neomdl::model::packed {

inline constexpr std::uint32_t kK1ModelToken0 = 4273776u;
inline constexpr std::uint32_t kK2ModelToken0 = 4285200u;
inline constexpr std::uint32_t kK1ModelToken1 = 4216096u;
inline constexpr std::uint32_t kK2ModelToken1 = 4216320u;

inline bool isModelToken(std::uint32_t token) noexcept {
    return token == kK1ModelToken0 || token == kK2ModelToken0;
}

inline std::string sourcePrefix(const std::string& label) {
    return label.empty() ? std::string{} : label + ": ";
}

class ByteView final {
public:
    ByteView(const std::uint8_t* data, std::size_t size, std::string label,
             const neoshared::bif::JobControl& job)
        : data_(data), size_(size), label_(std::move(label)), job_(job) {}

    std::size_t size() const noexcept { return size_; }
    bool contains(std::size_t offset, std::size_t length) const noexcept {
        return offset <= size_ && length <= size_ - offset;
    }

    bool containsArray(std::size_t offset, std::size_t count, std::size_t stride) const noexcept {
        return stride != 0u && offset <= size_ && count <= (size_ - offset) / stride;
    }

    void requireArray(std::size_t offset, std::size_t count, std::size_t stride, const char* what) const {
        if (!containsArray(offset, count, stride))
            throw ModelError(sourcePrefix(label_) + what + " is outside the bounded input");
        require(offset, count * stride, what);
    }

    void require(std::size_t offset, std::size_t length, const char* what) const {
        if ((reads_++ & 1023u) == 0u) job_.check();
        if (!contains(offset, length)) {
            std::ostringstream text;
            text << sourcePrefix(label_) << what << " is outside the bounded input (offset "
                 << offset << ", size " << length << ", input " << size_ << ")";
            throw ModelError(text.str());
        }
    }

    std::uint8_t u8(std::size_t offset, const char* what = "byte") const {
        require(offset, 1u, what);
        return data_[offset];
    }

    std::uint16_t u16(std::size_t offset, const char* what = "uint16") const {
        require(offset, 2u, what);
        return static_cast<std::uint16_t>(data_[offset]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset + 1u]) << 8u);
    }

    std::int16_t i16(std::size_t offset, const char* what = "int16") const {
        return static_cast<std::int16_t>(u16(offset, what));
    }

    std::uint32_t u32(std::size_t offset, const char* what = "uint32") const {
        require(offset, 4u, what);
        return static_cast<std::uint32_t>(data_[offset]) |
               (static_cast<std::uint32_t>(data_[offset + 1u]) << 8u) |
               (static_cast<std::uint32_t>(data_[offset + 2u]) << 16u) |
               (static_cast<std::uint32_t>(data_[offset + 3u]) << 24u);
    }

    std::int32_t i32(std::size_t offset, const char* what = "int32") const {
        return static_cast<std::int32_t>(u32(offset, what));
    }

    float f32(std::size_t offset, const char* what = "float") const {
        const auto bits = u32(offset, what);
        float result{};
        static_assert(sizeof(result) == sizeof(bits), "32-bit float required");
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    std::uint32_t f32Bits(std::size_t offset, const char* what = "float word") const {
        return u32(offset, what);
    }

    Vec2 vec2(std::size_t offset, const char* what) const {
        require(offset, 8u, what);
        return {f32(offset, what), f32(offset + 4u, what)};
    }

    Vec3 vec3(std::size_t offset, const char* what) const {
        require(offset, 12u, what);
        return {f32(offset, what), f32(offset + 4u, what), f32(offset + 8u, what)};
    }

    std::string fixedString(std::size_t offset, std::size_t length, const char* what) const {
        require(offset, length, what);
        std::size_t count = 0u;
        while (count < length && data_[offset + count] != 0u) ++count;
        // Engine name/resource buffers are byte strings. Display filtering
        // here would change lookup identity before any consumer sees it.
        return std::string(reinterpret_cast<const char*>(data_ + offset), count);
    }

    std::string cString(std::size_t offset, const char* what) const {
        require(offset, 1u, what);
        const auto available = size_ - offset;
        std::size_t count = 0u;
        while (count < available && data_[offset + count] != 0u) ++count;
        if (count == available) {
            throw ModelError(sourcePrefix(label_) + std::string(what) + " is not null-terminated within the input");
        }
        return std::string(reinterpret_cast<const char*>(data_ + offset), count);
    }

private:
    const std::uint8_t* data_{};
    std::size_t size_{};
    std::string label_;
    const neoshared::bif::JobControl& job_;
    mutable std::size_t reads_{};
};

} // namespace neomdl::model::packed
