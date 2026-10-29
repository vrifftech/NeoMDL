#pragma once

#include "wx/AsciiEditor.hpp"

namespace neomdl::ui {

enum class BinaryMeshPage { Properties, Vertices, Controllers, Transform };

std::optional<std::filesystem::path> editBinaryMeshFields(wxWindow* parent, const AuthoringJob& runJob,
    const std::filesystem::path& input = {}, std::optional<std::size_t> selectedOffset = {},
    BinaryMeshPage page = BinaryMeshPage::Properties);
void inspectBinaryFile(wxWindow* parent, const AuthoringJob& runJob);
void analyzeBinaryFiles(wxWindow* parent, const AuthoringJob& runJob);
void compareBinaryFiles(wxWindow* parent, const AuthoringJob& runJob);

} // namespace neomdl::ui
