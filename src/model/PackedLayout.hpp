#pragma once

#include "model/Model.hpp"

namespace neomdl::model {

enum class BinaryType { Bytes, UInt8, Int16, UInt16, Int32, UInt32, Float32, String, Offset32 };
const char* binaryTypeName(BinaryType type);
std::size_t binaryTypeWidth(BinaryType type);

struct BinaryField {
    std::size_t offset, size;
    BinaryType type;
    std::string name;
};

// Fields describe one record, even for large vertex/controller arrays.
// Parentage expresses ownership, not physical containment in the file.
struct BinaryStructure {
    std::string name;
    std::size_t file, offset, count, stride;
    std::optional<std::size_t> parent;
    std::vector<BinaryField> fields;
    std::size_t size() const { return count * stride; }
};

struct BinaryLocation {
    std::size_t structure, record;
    std::optional<std::size_t> field;
};

struct PackedLayout {
    GameVersion game{};
    std::string name;
    std::size_t payloadOffset{};
    std::vector<BinaryStructure> structures;
    std::array<std::vector<std::size_t>,2> ordered;
    std::array<std::vector<std::size_t>,2> ends;

    std::optional<BinaryLocation> locate(std::size_t file, std::size_t offset) const;
    std::size_t mappedBytes(std::size_t file) const;
};

PackedLayout inspectPackedLayout(const std::vector<std::uint8_t>& mdl,
    const std::optional<std::vector<std::uint8_t>>& mdx,
    const neoshared::bif::JobControl& job = {});

} // namespace neomdl::model
