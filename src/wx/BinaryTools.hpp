#pragma once

#include "wx/AsciiEditor.hpp"
#include <map>
#include <set>

namespace neomdl::ui {

enum class BinaryMeshPage { Properties, Vertices, Controllers, Transform };

struct BinaryEditResult {
    std::filesystem::path path;
    std::size_t nodeOffset;
    std::map<std::size_t,std::size_t> nodeOffsets;
};

std::optional<BinaryEditResult> editBinaryMeshFields(wxWindow* parent, const AuthoringJob& runJob,
    const std::filesystem::path& input = {}, std::optional<std::size_t> selectedOffset = {},
    BinaryMeshPage page = BinaryMeshPage::Properties, const std::set<std::size_t>& selectedOffsets = {});
void inspectBinaryFile(wxWindow* parent, const AuthoringJob& runJob);
void analyzeBinaryFiles(wxWindow* parent, const AuthoringJob& runJob);
void compareBinaryFiles(wxWindow* parent, const AuthoringJob& runJob);

} // namespace neomdl::ui
