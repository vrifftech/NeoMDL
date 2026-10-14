#pragma once

#include "wx/AsciiEditor.hpp"

namespace neomdl::ui {

void editBinaryMeshFields(wxWindow* parent, const AuthoringJob& runJob);
void inspectBinaryFile(wxWindow* parent, const AuthoringJob& runJob);
void compareBinaryFiles(wxWindow* parent, const AuthoringJob& runJob);

} // namespace neomdl::ui
