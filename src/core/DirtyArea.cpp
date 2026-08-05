#include "core/DirtyArea.hpp"
#include <gff/GFFFile.hpp>
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace neomdl {
std::array<material::DirtyTextureInputs,3> DirtyAreaState::controllerInputs() const {
    std::array<material::DirtyTextureInputs,3> out;
    for(std::size_t i=0;i<slots.size();++i) {
        const auto& s=slots[i];
        // DirtyTextureController::Init(): size zero replaces the ENTIRE tuple.
        if(s.size==0)continue;
        out[i]={static_cast<std::uint32_t>(s.argb),s.size,s.formula==0?1:s.formula==1?0:s.formula,s.function};
    }
    return out;
}
DirtyAreaState readDirtyArea(const neogff::GffStruct& root) {
    DirtyAreaState result;
    const auto read=[&](const std::string& name,std::int32_t fallback) {
        // CResGFF::GetFieldByLabel copies exactly 16 bytes before comparing.
        // DirtyFormulaThree is 17 characters; its on-disk label is truncated.
        const auto label=name.substr(0,16);
        const auto* f=root.GetFieldByLabel(label);
        if(root.GetFieldByLabel(label,1))throw std::runtime_error("Duplicate ARE field: "+name);
        const auto* value=dynamic_cast<const neogff::GffIntField*>(f);
        if(f&&!value)throw std::runtime_error("ARE "+name+" must have GFF INT type");
        return value?value->value:fallback;
    };
    const char* suffix[]={"One","Two","Three"};
    for(unsigned i=0;i<3;++i) {
        auto& s=result.slots[i];const std::string n=suffix[i];
        s.argb=read("DirtyARGB"+n,0);s.size=read("DirtySize"+n,16);
        s.formula=read("DirtyFormula"+n,1);s.function=read("DirtyFunc"+n,0);
    }
    // Preserve even out-of-domain engine values. Validation at controller
    // construction prevents a malicious ARE from allocating an invalid volume.
    return result;
}
std::string describeDirtyArea(const DirtyAreaState& state) {
    std::ostringstream out;const auto inputs=state.controllerInputs();
    for(unsigned i=0;i<3;++i) {
        const auto& s=state.slots[i];const auto& v=inputs[i];
        out<<"Dirty "<<i+1<<": ARGB 0x"<<std::hex<<std::setw(8)<<std::setfill('0')
           <<static_cast<std::uint32_t>(s.argb)<<std::dec<<", size "<<s.size
           <<", formula "<<s.formula<<", func "<<s.function
           <<" -> controller ("<<v.color<<", "<<v.size<<", "<<v.parameter<<", "<<v.algorithm<<")\n";
    }
    return out.str();
}
} // namespace neomdl
