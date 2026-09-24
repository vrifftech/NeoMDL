#include <model/AsciiModelInternal.hpp>
#include <model/AsciiFixedString.hpp>

namespace neomdl::model::ascii {

void applyAsciiLightmap(RawNode& raw, Reader& reader) {
    auto& texture=raw.mesh->texture1;
    auto& coordinates=raw.texcoords[1];
    // InternalParseField: K1 0x5ab2c4, K2 0x7a8e28. The lookup belongs
    // to each mesh field invocation, not only to the texture1 keyword.
    if(texture.empty() || coordinates.empty() || !reader.resources()) return;

    std::vector<std::int32_t> offsets;
    {
        auto bytes=reader.resources()(texture,".lmt");
        if(!bytes) return;
        Reader sidecar(*bytes,reader.game(),texture+".lmt",reader.job(),reader.resources());
        Line line;
        while(sidecar.next(line)) {
            auto offset=std::size_t{};
            while(offset<line.text.size() && (line.text[offset]==' ' || line.text[offset]=='\t')) ++offset;
            line=sidecar.fieldLine(offset);
            if(equal(line.tokens.front(),"tverts1offset")) {
                // The shared engine Parse<int> retains old initialized
                // offsets if a later counted directive ends at EOF.
                readScalarArray(line,sidecar,offsets,4u,
                    [](const Line& row,const Reader&,std::int32_t& value) {
                        return scanInteger(row.text.c_str(),value);
                    },
                    [](const std::uint8_t* value) {
                        return static_cast<std::int32_t>(
                            std::uint32_t(value[0]) |
                            (std::uint32_t(value[1])<<8u) |
                            (std::uint32_t(value[2])<<16u) |
                            (std::uint32_t(value[3])<<24u));
                    },true);
            }
            // The offset parser shares this resource's line buffer. Its
            // last consumed row can supply the later texture1 check; the
            // parent MDL resource retains its own independent buffer.
            line=sidecar.fieldLine(offset);
            if(equal(line.tokens.front(),"texture1")) {
                const auto arguments=line.text.find(line.tokens.front())+line.tokens.front().size();
                const auto previous=texture;
                scanString(line.text.c_str()+arguments,texture);
                retainAsciiFixedString(raw.mesh->asciiTexture1,previous,texture);
            }
        }
    }

    // Both loaders release the nested resource before applying its integer
    // affine map. A replacement texture participates in the next field's
    // lookup; there is deliberately no cached or one-shot sidecar state.
    if(offsets.size()!=coordinates.size()*4u) return;
    for(std::size_t i=0;i<coordinates.size();++i) {
        const auto* offset=offsets.data()+i*4u;
        auto& coordinate=coordinates[i];
        const auto scale=static_cast<float>(offset[0]);
        coordinate.x=coordinate.x*scale+static_cast<float>(offset[1]);
        coordinate.y=coordinate.y*scale+static_cast<float>(offset[2]);
        coordinate.z*=scale;
        if(offset[3]!=0) {
            const auto reciprocal=1.0f/static_cast<float>(offset[3]);
            coordinate.x*=reciprocal;
            coordinate.y*=reciprocal;
            coordinate.z*=reciprocal;
        }
    }
}

} // namespace neomdl::model::ascii
