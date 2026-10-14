#pragma once

#include <filesystem>
#include <string>

namespace neomdl {

enum class AsciiNaming { Dash, Dot, Suffix };

bool asciiModelSuffix(const std::filesystem::path& path);
std::string modelFileStem(const std::filesystem::path& path);
std::string conversionFileStem(const std::filesystem::path& path);
std::string asciiModelFilename(const std::string& stem, AsciiNaming naming, const std::string& extension = ".mdl");

} // namespace neomdl
