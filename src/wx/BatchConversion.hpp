#pragma once

#include "wx/AsciiEditor.hpp"
#include "core/Conversion.hpp"

namespace neomdl::ui {

void showBatchConversion(wxWindow* parent, ConversionMode mode, const AuthoringJob& runJob);
void chooseConversionNaming(wxWindow* parent);

} // namespace neomdl::ui
