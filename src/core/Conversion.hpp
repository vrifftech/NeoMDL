#pragma once

#include "core/CollisionAssets.hpp"
#include "core/MeshReplacement.hpp"
#include "core/ModelFilenames.hpp"

namespace neomdl {

enum class ConversionMode { Ascii, Binary, Opposite };
struct ConversionRequest {
    std::filesystem::path input;
    std::optional<std::filesystem::path> mdx, asciiOutput, packedOutput;
};
struct ConversionOptions {
    std::optional<model::GameVersion> game;
    model::AsciiExportOptions ascii;
    bool includeCollisions{true}, rebuildAabb{};
    std::optional<std::filesystem::path> layout;
    model::Vec3 placement;
    std::vector<std::string> collisionSources;
    struct Replacement {std::filesystem::path donor; std::string mapping;};
    std::vector<Replacement> replacements;
};
struct ConversionReport {
    std::string source, summary, error;
    std::optional<model::GameVersion> game;
    std::vector<std::filesystem::path> outputs;
    std::vector<std::string> warnings;
    double seconds{};
};

ConversionRequest planConversion(const std::filesystem::path& input, ConversionMode mode,
    const std::filesystem::path& directory, AsciiNaming naming);
std::vector<std::filesystem::path> conversionInputs(const ConversionRequest& request);
std::vector<std::filesystem::path> conversionOutputs(const ConversionRequest& request, const ConversionOptions& options);
std::filesystem::path conversionReportPath(const ConversionRequest& request);
void writeConversionReport(const std::filesystem::path& path, const ConversionReport& report,
                           const ConversionOptions& options);

// One model at a time, using the editor's codecs and material finalization.
// Conversion failures are reported; cancellation propagates to the caller.
ConversionReport convertFile(const ConversionRequest& request, const ConversionOptions& options,
    const std::vector<std::filesystem::path>& batchInputs = {}, const neoshared::bif::JobControl& job = {});

} // namespace neomdl
