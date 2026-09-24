#pragma once

#include <model/PackedExport.hpp>

#include <cstring>
#include <unordered_map>

namespace neomdl::model::packed_export {

// Engine IODispatcher zeroes the destination MDL allocation and publishes
// unaligned, body-relative 32-bit offsets. Unspecified source padding is
// deterministically zero in this authoring output.
struct Buffer {
    std::vector<std::uint8_t> bytes;
    std::uint32_t append(std::size_t count) {
        const auto offset=static_cast<std::uint32_t>(bytes.size());
        bytes.resize(bytes.size()+count,0u);
        return offset;
    }
    void u8(std::size_t at,std::uint8_t value) { bytes[at]=value; }
    void u16(std::size_t at,std::uint16_t value) {
        for(unsigned i=0;i<2u;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8u*i));
    }
    void u32(std::size_t at,std::uint32_t value) {
        for(unsigned i=0;i<4u;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8u*i));
    }
    void f32(std::size_t at,float value) {
        std::uint32_t word;std::memcpy(&word,&value,sizeof(word));u32(at,word);
    }
    void vec3(std::size_t at,Vec3 value) { f32(at,value.x);f32(at+4u,value.y);f32(at+8u,value.z); }
    void vec2(std::size_t at,Vec2 value) { f32(at,value.x);f32(at+4u,value.y); }
    void quaternion(std::size_t at,Quaternion value) {
        f32(at,value.w);f32(at+4u,value.x);f32(at+8u,value.y);f32(at+12u,value.z);
    }
    void copy(std::size_t at,const void* source,std::size_t count) {
        if(count)std::memcpy(bytes.data()+at,source,count);
    }
    void string(std::size_t at,const std::string& value,std::size_t width) {
        if(value.size()>=width)
            throw ModelError("Packed model string exceeds its engine fixed-width field: "+value);
        copy(at,value.data(),value.size());
    }
    template<std::size_t N>
    void string(std::size_t at,const std::string& value,
                const std::array<std::uint8_t,N>& retained) {
        copy(at,retained.data(),N);
        string(at,value,N);
        // Explicitly terminate a later semantic edit without clearing the
        // initialized tail copied by the engine complete-record writer.
        u8(at+value.size(),0u);
    }
    std::uint32_t text(const std::string& value) {
        const auto at=append(value.size()+1u);copy(at,value.data(),value.size());return at;
    }
    std::uint32_t array(std::size_t at,std::size_t count,std::size_t stride) {
        const auto offset=append(count*stride);
        u32(at,offset);u32(at+4u,static_cast<std::uint32_t>(count));
        u32(at+8u,static_cast<std::uint32_t>(count));return offset;
    }
};

struct Context {
    const Model& model;
    const neoshared::bif::JobControl& job;
    Buffer mdl,mdx;
    std::size_t mdlUnwrittenBytes{};
    std::unordered_map<const Node*,std::uint32_t> nodeOffsets;
    std::unordered_map<const Mesh*,std::uint32_t> mdxOffsets;
    explicit Context(const Model& value,const neoshared::bif::JobControl& control)
        :model(value),job(control) {}
    bool k2() const noexcept { return model.game==GameVersion::Kotor2; }
    std::size_t meshEnd() const noexcept { return k2()?420u:412u; }
};

std::uint32_t writeNode(Context&,const std::vector<Node>&,std::size_t index,
                        std::uint32_t treeOffset,std::uint32_t parentOffset);
std::uint32_t writeAnimation(Context&,const Animation&);
std::size_t nodeRecordSize(const Context&,const Node&);
void writeNodeFields(Context&,const Node&,std::uint32_t nodeOffset);
void buildMdx(Context&);
void writeMesh(Context&,const Node&,const Mesh&,std::uint32_t nodeOffset);
void writeMeshSpecialized(Context&,const Node&,const Mesh&,std::uint32_t nodeOffset);

} // namespace neomdl::model::packed_export
