#pragma once
#include <model/Model.hpp>
#include <neoshared/bwm/BwmFile.hpp>

namespace neomdl {
std::vector<std::uint8_t> moveLayoutRoom(const std::vector<std::uint8_t>& bytes,std::size_t room,model::Vec3 position);
void translateRoomWalkmesh(neoshared::bwm::BwmFile& mesh,model::Vec3 offset);
}
