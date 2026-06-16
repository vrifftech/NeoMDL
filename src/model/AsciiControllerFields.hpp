#pragma once

#include <model/Model.hpp>

#include <string_view>

namespace neomdl::model::ascii {

struct ControllerField {
    std::string_view name;
    std::uint32_t type;
    std::uint16_t nodeFlag;
    std::int16_t keyedSourceOffset{-1};
};






inline constexpr ControllerField controllerFields[] = {
    {"position",8u,kNodeBase,16}, {"orientation",20u,kNodeBase,28},
    {"scale",36u,kNodeBase},
    {"color",76u,kNodeLight}, {"radius",88u,kNodeLight},
    {"shadowradius",96u,kNodeLight}, {"verticaldisplacement",100u,kNodeLight},
    {"multiplier",140u,kNodeLight},
    {"selfillumcolor",100u,kNodeMesh}, {"alpha",132u,kNodeMesh},
    {"blurlength",180u,kNodeEmitter}, {"detonate",502u,kNodeEmitter},
    {"velocity",168u,kNodeEmitter}, {"randvel",140u,kNodeEmitter},
    {"birthrate",88u,kNodeEmitter}, {"m_frandombirthrate",240u,kNodeEmitter},
    {"colorstart",392u,kNodeEmitter}, {"colormid",284u,kNodeEmitter},
    {"colorend",380u,kNodeEmitter},
    {"percentstart",220u,kNodeEmitter}, {"percentmid",224u,kNodeEmitter},
    {"percentend",228u,kNodeEmitter},
    {"alphastart",84u,kNodeEmitter}, {"alphamid",216u,kNodeEmitter},
    {"alphaend",80u,kNodeEmitter},
    {"sizestart",144u,kNodeEmitter}, {"sizemid",232u,kNodeEmitter},
    {"sizeend",148u,kNodeEmitter},
    {"sizestart_y",152u,kNodeEmitter}, {"sizemid_y",236u,kNodeEmitter},
    {"sizeend_y",156u,kNodeEmitter},
    {"framestart",112u,kNodeEmitter}, {"frameend",108u,kNodeEmitter},
    {"lifeexp",120u,kNodeEmitter}, {"mass",124u,kNodeEmitter},
    {"spread",160u,kNodeEmitter}, {"particlerot",136u,kNodeEmitter},
    {"fps",104u,kNodeEmitter}, {"xsize",172u,kNodeEmitter},
    {"ysize",176u,kNodeEmitter}, {"bounce_co",92u,kNodeEmitter},
    {"lightningdelay",184u,kNodeEmitter}, {"lightningradius",188u,kNodeEmitter},
    {"lightningscale",192u,kNodeEmitter}, {"lightningsubdiv",196u,kNodeEmitter},
    {"lightningzigzag",200u,kNodeEmitter},
    {"p2p_bezier2",128u,kNodeEmitter}, {"p2p_bezier3",132u,kNodeEmitter},
    {"combinetime",96u,kNodeEmitter}, {"drag",100u,kNodeEmitter},
    {"grav",116u,kNodeEmitter}, {"threshold",164u,kNodeEmitter},
    {"targetsize",252u,kNodeEmitter}, {"numcontrolpts",256u,kNodeEmitter},
    {"controlptradius",260u,kNodeEmitter}, {"controlptdelay",264u,kNodeEmitter},
    {"tangentspread",268u,kNodeEmitter}, {"tangentlength",272u,kNodeEmitter},
};

} // namespace neomdl::model::ascii
