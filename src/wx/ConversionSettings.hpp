#pragma once

#include "NeoSettings.hpp"
#include "core/ModelFilenames.hpp"

namespace neomdl::ui {

inline AsciiNaming asciiNamingPreference() {
    return static_cast<AsciiNaming>(neosettings::AppSettings("NeoMDL").readDouble("Conversion/AsciiNaming", 0));
}
inline void saveAsciiNamingPreference(AsciiNaming naming) {
    neosettings::AppSettings("NeoMDL").writeDouble("Conversion/AsciiNaming", static_cast<int>(naming));
}
inline constexpr const char* asciiNamingLabels[]{"name-ascii.mdl", "name.ascii.mdl", "name.mdl.ascii"};

} // namespace neomdl::ui
