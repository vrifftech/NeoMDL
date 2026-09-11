#include "wx/OpenALProvider.hpp"
#include <wx/dynlib.h>
#include <stdexcept>
namespace neomdl::ui {
std::unique_ptr<neoshared::audio::OpenALPlayback> makeOpenALPlayback() {
#if defined(__APPLE__)
    const wxString path="/System/Library/Frameworks/OpenAL.framework/OpenAL";
#elif defined(_WIN32)
    const wxString path="OpenAL32.dll";
#else
    const wxString path="libopenal.so.1";
#endif
    auto provider=std::make_shared<wxDynamicLibrary>();
    if(!provider->Load(path,wxDL_NOW|wxDL_VERBATIM))
        throw std::runtime_error("The OpenAL scene-audio provider is unavailable");
    return std::make_unique<neoshared::audio::OpenALPlayback>([provider](const char* name) {
        return provider->GetSymbol(wxString::FromUTF8(name));
    });
}
}
