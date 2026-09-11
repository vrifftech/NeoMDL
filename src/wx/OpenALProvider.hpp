#pragma once
#include <neoshared/audio/OpenALPlayback.hpp>
#include <memory>
namespace neomdl::ui {
std::unique_ptr<neoshared::audio::OpenALPlayback> makeOpenALPlayback();
}
