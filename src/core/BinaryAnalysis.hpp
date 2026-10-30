#pragma once

#include "core/ResourceContext.hpp"
#include "model/PackedLayout.hpp"

namespace neomdl {

struct BinaryAnalysis {
    std::array<std::filesystem::path,2> paths;
    std::vector<std::uint8_t> mdl;
    std::optional<std::vector<std::uint8_t>> mdx;
    model::PackedLayout layout;
};

BinaryAnalysis readBinaryAnalysis(const std::filesystem::path& path,
    const neoshared::bif::JobControl& job = {});
BinaryAnalysis readBinaryAnalysis(const std::filesystem::path& path,
    std::vector<std::uint8_t> primary, const neoshared::bif::JobControl& job = {});
std::string binaryAnalysisReport(const BinaryAnalysis& analysis,
    const neoshared::bif::JobControl& job = {});

struct BinaryAnalysisResult {
    std::filesystem::path path;
    std::vector<std::filesystem::path> inputs;
    std::string report, error;
    double seconds{};
};
BinaryAnalysisResult analyzeBinaryFile(const std::filesystem::path& path,
    const neoshared::bif::JobControl& job = {});
std::string binaryBatchReport(const std::vector<BinaryAnalysisResult>& results);

} // namespace neomdl
