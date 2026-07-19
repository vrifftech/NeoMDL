#include "core/DialogueObjectAnimation.hpp"
#include <cstdio>

namespace neomdl {
namespace detail {
namespace {
std::uint16_t placeableClientAnimation(std::uint16_t id) noexcept {
    // K1 Desktop 0x5e2ec matches K2 Desktop 0x251baa.
    switch(id) {
    case 10000u:return 304u;
    case 10014u:case 10077u:case 10213u:case 10214u:case 10215u:case 10216u:return 305u;
    case 10072u:return 307u;
    case 10073u:return 308u;
    case 10074u:return 309u;
    case 10075u:return 310u;
    case 10076u:return 311u;
    case 10106u:return 316u;
    case 10107u:return 317u;
    case 10108u:return 318u;
    case 10110u:return 319u;
    case 10111u:return 320u;
    case 10112u:return 321u;
    case 10113u:return 322u;
    case 10114u:return 323u;
    case 10115u:return 324u;
    case 10116u:return 325u;
    default:return id;
    }
}
std::uint16_t doorClientAnimation(std::uint16_t id,std::int32_t selector) noexcept {
    // K1 Desktop 0x5bf32 matches K2 Desktop 0x15acf0.
    switch(id) {
    case 10014u:case 10077u:case 10213u:case 10214u:case 10215u:case 10216u:return 328u;
    case 10022u:return 333u;
    case 10050u:return 331u;
    case 10051u:return 332u;
    case 10072u:return selector?330u:327u;
    case 10153u:return 366u;
    case 10269u:return 344u;
    default:return id;
    }
}
}
DialogueObjectAnimationProjection dialogueObjectAnimation(
    std::uint16_t id,bool door,std::int32_t doorSelector) {
    DialogueObjectAnimationProjection result;
    result.clientAnimation=door?doorClientAnimation(id,doorSelector):placeableClientAnimation(id);
    result.synthesizeCuts=!door;result.defaultOutsideCuts=!door;
    if(door) {
        // K1 0x5bdc6 / K2 Desktop 0x15ab3a ignores
        // incoming flags and chooses its own fixed Gob command.
        switch(id) {
        case 10022u:case 10050u:case 10051u:case 10072u:case 10153u:
            result.flags=2u;return result;
        case 329u:case 334u:case 335u:case 336u:case 337u:
        case 10014u:case 10077u:case 10213u:case 10214u:case 10215u:case 10216u:
            result.flags=3u;return result;
        default:result.playEmpty=true;return result;
        }
    }
    // K1 0x5e0d2 matches K2 Desktop 0x2518de.
    switch(id) {
    case 10000u:case 10072u:case 10073u:case 10074u:case 10075u:case 10076u:
    case 10106u:case 10107u:case 10108u:case 10110u:case 10111u:case 10112u:
    case 10113u:case 10114u:case 10115u:case 10116u:
        return result;
    case 306u:case 312u:case 313u:case 314u:case 315u:
    case 10014u:case 10077u:case 10213u:case 10214u:case 10215u:case 10216u:
        result.flags=1u;return result;
    default:break;
    }
    // Engine range checks include the unused numeric gaps between cut bands.
    if(static_cast<std::uint16_t>(id-1000u)<328u)result.flags=1u;
    else if(static_cast<std::uint16_t>(id-1400u)>=328u)result.playEmpty=true;
    return result;
}
std::string dialogueObjectAnimationName(std::uint16_t id,
    const DialogueObjectAnimationProjection& projection,const std::string& tableName) {
    // K1 Desktop placeable GetAnimationName 0x5de66 matches K2 0x251678.
    // The door caller retains an empty table result; it never synthesizes cuts.
    if(!tableName.empty())return tableName;
    const auto offset=static_cast<std::uint16_t>(id-1000u);
    if(projection.synthesizeCuts&&offset<=727u&&offset%200u<128u) {
        constexpr const char* suffix[]={"","w","l","wl"};
        char buffer[16];std::snprintf(buffer,sizeof(buffer),"cut%03u%s",offset%200u+1u,suffix[offset/200u]);
        return buffer;
    }
    return projection.defaultOutsideCuts&&offset>727u?"default":std::string{};
}
} // namespace detail
} // namespace neomdl
