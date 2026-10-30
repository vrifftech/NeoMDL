#include "core/BinaryGameConversion.hpp"
#include <model/PackedBytes.hpp>
#include <model/PackedExportInternal.hpp>
#include <model/PackedLayout.hpp>

#include <algorithm>

namespace neomdl {

std::map<std::size_t,std::size_t> switchBinaryGame(std::vector<std::uint8_t>& mdl,
    const neoshared::bif::JobControl& job) {
    const auto layout=model::inspectPackedLayout(mdl,std::nullopt,job);
    const bool k2=layout.game==model::GameVersion::Kotor1;
    const std::ptrdiff_t delta=k2 ? 8 : -8;
    const auto base=layout.payloadOffset;
    std::vector<std::size_t> cuts;
    for (const auto& region : layout.structures)
        if (region.file==0 && region.name=="Mesh") cuts.push_back(region.offset+316);
    std::sort(cuts.begin(),cuts.end());
    const auto relocate=[&](std::size_t offset) {
        const auto count=std::upper_bound(cuts.begin(),cuts.end(),offset)-cuts.begin();
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(offset)+delta*count);
    };
    model::packed::ByteView source(mdl.data(),mdl.size(),"Binary game conversion",job);
    model::packed_export::Buffer output;
    output.bytes.reserve(relocate(mdl.size()));
    job.update(0,2,k2 ? "Writing K2 binary layout" : "Writing K1 binary layout");
    std::size_t cursor{};
    for (const auto cut : cuts) {
        job.check();
        output.bytes.insert(output.bytes.end(),mdl.begin()+cursor,mdl.begin()+cut);
        if (k2) output.bytes.insert(output.bytes.end(),8,0);
        cursor=k2 ? cut : cut+8;
    }
    output.bytes.insert(output.bytes.end(),mdl.begin()+cursor,mdl.end());
    job.update(1,2,"Relocating binary model pointers");
    std::map<std::size_t,std::size_t> nodes;
    for (const auto& region : layout.structures) {
        job.check();
        if (region.file!=0) continue;
        for (std::size_t row=0;row<region.count;++row)
            for (const auto& field : region.fields) if (field.type==model::BinaryType::Offset32) {
                const auto at=region.offset+row*region.stride+field.offset;
                const auto pointer=source.u32(at);
                output.u32(relocate(at),pointer==0 || pointer==0xffffffffu ? pointer :
                    static_cast<std::uint32_t>(relocate(base+pointer)-base));
            }
        if (region.name=="Mesh") {
            const auto at=relocate(region.offset);
            // ResetTriMesh replaces these process callbacks in both engines.
            output.u32(at,0);output.u32(at+4,0);
            std::fill_n(output.bytes.begin()+at+314,k2 ? 10 : 2,0);
            if (k2) {output.u16(at+316,1);output.u16(at+318,1);}
        } else if (region.name.rfind("Animation: ",0)==0) {
            const auto at=relocate(region.offset);
            output.u32(at,0);output.u32(at+4,0);
        } else if (region.name.rfind("Node: ",0)==0) nodes.emplace(region.offset,relocate(region.offset));
    }
    output.u32(base,k2 ? model::packed::kK2ModelToken0 : model::packed::kK1ModelToken0);
    output.u32(base+4,k2 ? model::packed::kK2ModelToken1 : model::packed::kK1ModelToken1);
    if (base) output.u32(4,static_cast<std::uint32_t>(output.bytes.size()-base));
    mdl=std::move(output.bytes);
    job.update(2,2,k2 ? "K2 binary model ready" : "K1 binary model ready");
    return nodes;
}

} // namespace neomdl
