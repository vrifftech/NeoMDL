#include "core/ModelFilenames.hpp"
#include <model/AsciiModelInternal.hpp>
#include <neoshared/PathUtf8.hpp>

namespace neomdl {

bool asciiModelSuffix(const std::filesystem::path& path) {
    const auto name = model::ascii::lower(neoshared::pathToUtf8(path.filename()));
    return name.size() >= 10 && name.compare(name.size() - 10, 10, ".mdl.ascii") == 0;
}

std::string modelFileStem(const std::filesystem::path& path) {
    const auto name = neoshared::pathToUtf8(path);
    const auto leaf = std::filesystem::u8path(name.substr(name.find_last_of("/\\") + 1));
    return neoshared::pathToUtf8(asciiModelSuffix(leaf) ? leaf.stem().stem() : leaf.stem());
}

std::string conversionFileStem(const std::filesystem::path& path) {
    if (asciiModelSuffix(path)) return modelFileStem(path);
    auto stem = modelFileStem(path);
    const auto lower = model::ascii::lower(stem);
    for (const auto* suffix : {"-ascii", ".ascii"})
        if (lower.size() >= 6 && lower.compare(lower.size() - 6, 6, suffix) == 0) stem.resize(stem.size() - 6);
    return stem;
}

std::string asciiModelFilename(const std::string& stem, AsciiNaming naming, const std::string& extension) {
    const std::string suffixes[]{"-ascii" + extension, ".ascii" + extension, extension + ".ascii"};
    return stem + suffixes[static_cast<int>(naming)];
}

} // namespace neomdl
