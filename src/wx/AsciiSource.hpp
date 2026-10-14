#pragma once

#include "NeoWxUi.hpp"
#include <model/AsciiSource.hpp>

namespace neomdl::ui {

using model::ascii::Edits;
using model::ascii::SourceText;

inline wxArrayString textureNames(const SourceText& source) {
    std::map<std::string, std::string> names;
    for (const auto& texture : source.textures())
        if (!texture.policy) names.emplace(model::ascii::lower(texture.name), texture.name);
    wxArrayString result;
    for (const auto& [key, name] : names) result.Add(wxui::toWx(name));
    return result;
}

} // namespace neomdl::ui
