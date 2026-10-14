#include "model/PackedLayout.hpp"
#include "model/PackedBytes.hpp"

#include <algorithm>
#include <unordered_set>

namespace neomdl::model {

const char* binaryTypeName(BinaryType type) {
    static const char* names[]{"Unclassified", "uint8", "int16", "uint16", "int32",
        "uint32", "float32", "String", "Offset32"};
    return names[static_cast<std::size_t>(type)];
}

std::size_t binaryTypeWidth(BinaryType type) {
    static const std::size_t widths[]{1,1,2,2,4,4,4,1,4};
    return widths[static_cast<std::size_t>(type)];
}

std::optional<BinaryLocation> PackedLayout::locate(std::size_t file, std::size_t offset) const {
    const auto& order = ordered[file];
    auto at = std::upper_bound(order.begin(),order.end(),offset,
        [&](auto value,auto index) { return value < structures[index].offset; });
    while (at != order.begin()) {
        const auto position = static_cast<std::size_t>(--at-order.begin());
        if (ends[file][position] <= offset) break;
        const auto index = *at; const auto& region = structures[index];
        if (offset >= region.offset + region.size()) continue;
        const auto relative = (offset-region.offset)%region.stride;
        BinaryLocation result{index,(offset-region.offset)/region.stride,{}};
        for (std::size_t i = 0; i < region.fields.size(); ++i) {
            const auto& field = region.fields[i];
            if (relative >= field.offset && relative-field.offset < field.size) { result.field = i; break; }
        }
        return result;
    }
    return std::nullopt;
}

std::size_t PackedLayout::mappedBytes(std::size_t file) const {
    std::size_t result = 0, end = 0;
    for (const auto index : ordered[file]) {
        const auto& region = structures[index];
        const auto next = region.offset+region.size();
        result += std::max(next,end)-std::max(end,region.offset);
        end = std::max(end,next);
    }
    return result;
}

namespace {
using Type = BinaryType;
using Fields = std::vector<BinaryField>;

void arrayFields(Fields& fields,std::size_t at,const std::string& name) {
    fields.push_back({at,4,Type::Offset32,name+" offset"});
    fields.push_back({at+4,4,Type::UInt32,name+" count"});
    fields.push_back({at+8,4,Type::UInt32,name+" capacity"});
}

Fields geometryFields() {
    return {{0,8,Type::UInt32,"Layout tokens"},{8,32,Type::String,"Name"},
        {40,4,Type::Offset32,"Root node"},{44,4,Type::UInt32,"Node count"},
        {76,1,Type::UInt8,"Geometry type"}};
}

Fields nodeFields() {
    Fields fields{{0,2,Type::UInt16,"Flags"},{2,2,Type::UInt16,"Part ID"},
        {4,4,Type::UInt32,"Name index"},{8,4,Type::Offset32,"Geometry header"},
        {12,4,Type::Offset32,"Parent"},{16,12,Type::Float32,"Position"},
        {28,16,Type::Float32,"Orientation (w,x,y,z)"}};
    arrayFields(fields,44,"Children"); arrayFields(fields,56,"Controllers");
    arrayFields(fields,68,"Controller data"); return fields;
}

class LayoutReader {
public:
    LayoutReader(const std::vector<std::uint8_t>& mdl,
        const std::optional<std::vector<std::uint8_t>>& mdx,const neoshared::bif::JobControl& job)
        : mdl_(mdl),mdx_(mdx),job_(job),whole_(mdl.data(),mdl.size(),"MDL",job) {}

    PackedLayout read() {
        // The supported layout tokens and wrapper convention are the decoder's PC layouts.
        base_ = whole_.contains(12,4) && packed::isModelToken(whole_.u32(12)) ? 12 : 0;
        whole_.require(base_,196,"model header");
        bytes_ = std::make_unique<packed::ByteView>(mdl_.data()+base_,mdl_.size()-base_,"MDL",job_);
        const auto first = bytes_->u32(0), second = bytes_->u32(4);
        if (first == packed::kK1ModelToken0 && second == packed::kK1ModelToken1) result_.game = GameVersion::Kotor1;
        else if (first == packed::kK2ModelToken0 && second == packed::kK2ModelToken1) result_.game = GameVersion::Kotor2;
        else throw ModelError("Structure inspection requires a compiled PC K1/K2 MDL.");
        result_.payloadOffset = base_; result_.name = bytes_->fixedString(8,32,"model name");
        if (base_) add(0,0,1,12,{},"File wrapper",{{0,4,Type::UInt32,"Binary marker"},
            {4,4,Type::UInt32,"MDL payload size"},{8,4,Type::UInt32,"MDX size"}});
        auto fields = geometryFields();
        fields.insert(fields.end(),{{80,2,Type::UInt16,"Classification"},{82,1,Type::UInt8,"Subclassification"},
            {83,1,Type::UInt8,"Fog"},{84,4,Type::UInt32,"Reference count"},
            {104,12,Type::Float32,"Bounds minimum"},{116,12,Type::Float32,"Bounds maximum"},
            {128,4,Type::Float32,"Radius"},{132,4,Type::Float32,"Animation scale"},
            {136,32,Type::String,"Supermodel"},{168,4,Type::Offset32,"Animation root"},
            {176,4,Type::UInt32,"MDX pool size"}});
        arrayFields(fields,88,"Animations"); arrayFields(fields,184,"Names");
        const auto model = header(0,196,{},"Model: "+result_.name,std::move(fields));
        const auto names = array(184,4,model,"Name offsets",{{0,4,Type::Offset32,"Name"}});
        for (std::size_t i = 0; i < names.second; ++i) {
            const auto offset = bytes_->u32(names.first+i*4);
            const auto name = bytes_->cString(offset,"node name"); names_.push_back(name);
            pool(offset,1,name.size()+1,model,"Name "+std::to_string(i)+": "+name,
                {{0,name.size()+1,Type::String,"Name"}});
        }
        nodes(bytes_->u32(40),model);
        const auto animations = array(88,4,model,"Animation offsets",{{0,4,Type::Offset32,"Animation"}});
        for (std::size_t i = 0; i < animations.second; ++i) {
            job_.check(); const auto at = bytes_->u32(animations.first+i*4);
            auto animationFields = geometryFields();
            animationFields.insert(animationFields.end(),{{80,4,Type::Float32,"Length"},
                {84,4,Type::Float32,"Transition time"},{88,32,Type::String,"Root name"}});
            arrayFields(animationFields,120,"Events");
            const auto animation = header(at,136,model,"Animation: "+bytes_->fixedString(at+8,32,"animation name"),std::move(animationFields));
            array(at+120,36,animation,"Events",{{0,4,Type::Float32,"Time"},{4,32,Type::String,"Name"}});
            nodes(bytes_->u32(at+40),animation);
        }
        for (std::size_t i = 0; i < result_.structures.size(); ++i) {
            const auto& region = result_.structures[i]; result_.ordered[region.file].push_back(i);
        }
        for (std::size_t file = 0; file < 2; ++file) {
            auto& order = result_.ordered[file];
            std::stable_sort(order.begin(),order.end(),[&](auto a,auto b) { return result_.structures[a].offset < result_.structures[b].offset; });
            std::size_t end = 0;
            for (auto index : order) {
                const auto& region = result_.structures[index]; end = std::max(end,region.offset+region.size());
                result_.ends[file].push_back(end);
            }
        }
        return std::move(result_);
    }

private:
    std::size_t add(std::size_t file,std::size_t offset,std::size_t count,std::size_t stride,
        std::optional<std::size_t> parent,std::string name,Fields fields) {
        job_.check();
        const auto& data = file == 0 ? mdl_ : *mdx_;
        packed::ByteView(data.data(),data.size(),file == 0 ? "MDL" : "MDX",job_)
            .requireArray(offset,count,stride,name.c_str());
        std::stable_sort(fields.begin(),fields.end(),[](const auto& a,const auto& b) { return a.offset < b.offset; });
        const auto index = result_.structures.size();
        result_.structures.push_back({std::move(name),file,offset,count,stride,parent,std::move(fields)});
        return index;
    }
    std::size_t header(std::size_t at,std::size_t size,std::optional<std::size_t> parent,std::string name,Fields fields) {
        return add(0,base_+at,1,size,parent,std::move(name),std::move(fields));
    }
    void pool(std::size_t at,std::size_t count,std::size_t stride,std::size_t parent,std::string name,Fields fields) {
        if (count && at != 0 && at != 0xffffffffu)
            add(0,base_+at,count,stride,parent,std::move(name),std::move(fields));
    }
    std::pair<std::size_t,std::size_t> array(std::size_t at,std::size_t stride,std::size_t parent,
        const std::string& name,Fields fields) {
        const auto offset = bytes_->u32(at), count = bytes_->u32(at+4);
        // Writers can retain a count/capacity while clearing the serialized pointer.
        if (offset == 0 || offset == 0xffffffffu) return {offset,0};
        pool(offset,count,stride,parent,name,std::move(fields)); return {offset,count};
    }
    void controllers(std::size_t at,std::size_t parent) {
        const auto records = array(at+56,16,parent,"Controllers",{{0,4,Type::UInt32,"Type"},
            {4,2,Type::Int16,"Source offset"},{6,2,Type::UInt16,"Row count"},
            {8,2,Type::UInt16,"Time index"},{10,2,Type::UInt16,"Value index"},{12,1,Type::UInt8,"Encoding"}});
        const auto data = bytes_->u32(at+68), words = bytes_->u32(at+72);
        Fields fields;
        for (std::size_t i = 0; i < records.second; ++i) {
            const auto entry = records.first+i*16; const auto count = bytes_->u16(entry+6);
            const auto flags = bytes_->u8(entry+12); const auto kind = flags&15;
            const std::size_t width = kind == 1 ? 1 : kind == 2 ? 1 : kind == 3 ? 3 : kind == 4 ? 4 : 0;
            const auto interpolation = flags&0xf0;
            const auto columns = interpolation == 0 ? width : interpolation == 0x10
                ? width*(kind == 1 || kind == 3 ? 3 : 1) : 0;
            const auto label = "Controller "+std::to_string(i);
            if (count) fields.push_back({bytes_->u16(entry+8)*4u,count*4u,Type::Float32,label+" times"});
            if (count && columns) fields.push_back({bytes_->u16(entry+10)*4u,count*columns*4u,
                kind == 2 ? Type::UInt32 : Type::Float32,label+(kind == 2 ? " packed quaternions" : " values")});
        }
        if (words) pool(data,1,static_cast<std::size_t>(words)*4,parent,"Controller data",std::move(fields));
    }
    void nodes(std::size_t root,std::size_t owner) {
        std::vector<std::pair<std::size_t,std::size_t>> pending;
        if (root != 0 && root != 0xffffffffu) pending.emplace_back(root,owner);
        while (!pending.empty()) {
            const auto [at,parent] = pending.back(); pending.pop_back();
            if (!nodes_.insert(at).second) continue; // Shared serialized nodes have one identity.
            const auto node = header(at,80,parent,"Node: "+names_.at(bytes_->u32(at+4)),nodeFields());
            controllers(at,node);
            const auto flags = bytes_->u16(at);
            if (flags&kNodeMesh) mesh(at+80,node,flags);
            if (flags&kNodeLight) light(at+80,node);
            if (flags&kNodeEmitter) emitter(at+80,node);
            if (flags&kNodeReference) header(at+80,36,node,"Reference",{{0,32,Type::String,"Model"},{32,4,Type::Int32,"Reattachable"}});
            const auto children = array(at+44,4,node,"Child offsets",{{0,4,Type::Offset32,"Child"}});
            for (auto i = children.second; i != 0; --i) pending.emplace_back(bytes_->u32(children.first+(i-1)*4),node);
        }
    }
    void light(std::size_t at,std::size_t parent) {
        Fields fields{{0,4,Type::Float32,"Flare radius"}};
        const char* names[]{"Texture handles","Flare sizes","Flare positions","Flare colors","Texture names"};
        for (std::size_t i = 0; i < 5; ++i) arrayFields(fields,4+i*12,names[i]);
        const char* switches[]{"Priority","Ambient only","Dynamic type","Affect dynamic","Shadow","Generate flare","Fading"};
        for (std::size_t i = 0; i < 7; ++i) fields.push_back({64+i*4,4,Type::Int32,switches[i]});
        const auto light = header(at,92,parent,"Light",std::move(fields));
        for (std::size_t i = 1; i < 4; ++i) array(at+4+i*12,i == 3 ? 12 : 4,light,names[i],
            {{0,i == 3 ? 12u : 4u,Type::Float32,"Value"}});
        const auto textures = array(at+52,4,light,"Flare texture offsets",{{0,4,Type::Offset32,"Name"}});
        for (std::size_t i = 0; i < textures.second; ++i) {
            const auto offset = bytes_->u32(textures.first+i*4);
            const auto length = bytes_->cString(offset,"flare texture").size()+1;
            pool(offset,1,length,light,"Flare texture "+std::to_string(i),{{0,length,Type::String,"Name"}});
        }
    }
    void emitter(std::size_t at,std::size_t parent) {
        header(at,224,parent,"Emitter",{{0,4,Type::Float32,"Dead space"},{4,4,Type::Float32,"Blast radius"},
            {8,4,Type::Float32,"Blast length"},{12,4,Type::UInt32,"Branch count"},{16,4,Type::Int32,"Control smoothing"},
            {20,4,Type::UInt32,"Grid X"},{24,4,Type::UInt32,"Grid Y"},{28,4,Type::UInt32,"Spawn type"},
            {32,32,Type::String,"Update mode"},{64,32,Type::String,"Render mode"},{96,32,Type::String,"Blend mode"},
            {128,32,Type::String,"Texture"},{160,16,Type::String,"Chunk"},{176,4,Type::Int32,"Two sided"},
            {180,4,Type::Int32,"Loop"},{184,2,Type::UInt16,"Render order"},{186,1,Type::UInt8,"Frame blending"},
            {187,32,Type::String,"Depth texture"},{220,4,Type::UInt32,"Flags"}});
    }
    void mesh(std::size_t at,std::size_t parent,std::uint16_t flags) {
        const std::size_t size = result_.game == GameVersion::Kotor1 ? 332 : 340;
        Fields fields{{0,8,Type::UInt32,"Function tokens"},{20,12,Type::Float32,"Bounds minimum"},
            {32,12,Type::Float32,"Bounds maximum"},{44,4,Type::Float32,"Sphere radius"},{48,12,Type::Float32,"Sphere center"},
            {60,12,Type::Float32,"Diffuse"},{72,12,Type::Float32,"Ambient"},{84,4,Type::UInt32,"Transparency hint"},
            {88,32,Type::String,"Base texture"},{120,32,Type::String,"Secondary texture"},
            {212,4,Type::Offset32,"Trailing index source"},{216,4,Type::UInt32,"Trailing index token"},
            {220,4,Type::UInt32,"Trailing index count"},{224,4,Type::UInt32,"Primitive"},
            {232,4,Type::UInt32,"Animate UV"},{236,8,Type::Float32,"UV direction"},
            {244,4,Type::Float32,"UV jitter"},{248,4,Type::Float32,"UV jitter speed"},
            {252,4,Type::UInt32,"MDX stride"},{256,4,Type::UInt32,"MDX channel bitmap"},
            {304,2,Type::UInt16,"Vertex count"},{306,2,Type::UInt16,"Texture count"},
            {size-16,4,Type::Float32,"Total area"},{size-8,4,Type::UInt32,"MDX data offset"},
            {size-4,4,Type::Offset32,"CPU vertex offset"}};
        arrayFields(fields,8,"Faces"); arrayFields(fields,152,"Raw index pointers");
        arrayFields(fields,164,"Leftover faces"); arrayFields(fields,176,"Index counts");
        arrayFields(fields,188,"Index sources"); arrayFields(fields,200,"Index tokens");
        const char* channels[]{"Position","Normal","Color","UV 0","UV 1","UV 2","UV 3","Tangent 0","Tangent 1","Tangent 2","Tangent 3"};
        for (std::size_t i = 0; i < 11; ++i) fields.push_back({260+i*4,4,Type::UInt32,std::string("MDX ")+channels[i]+" offset"});
        const char* switches[]{"Lightmap","Rotate texture","Background","Shadow","Beaming","Render"};
        for (std::size_t i = 0; i < 6; ++i) fields.push_back({308+i,1,Type::UInt8,switches[i]});
        if (size == 340) fields.insert(fields.end(),{{314,1,Type::UInt8,"Dirt enabled"},
            {316,2,Type::Int16,"Dirt texture"},{318,2,Type::Int16,"Dirt worldspace"},{320,1,Type::UInt8,"Hologram no draw"}});
        const auto mesh = header(at,size,parent,"Mesh",std::move(fields));
        array(at+8,32,mesh,"Faces",{{0,12,Type::Float32,"Normal"},{12,4,Type::Float32,"Plane distance"},
            {16,4,Type::UInt32,"Surface material"},{20,6,Type::Int16,"Adjacent faces"},{26,6,Type::Int16,"Vertex indices"}});
        const auto count = bytes_->u16(at+304); const auto cpu = bytes_->u32(at+size-4);
        pool(cpu,count,12,mesh,"CPU vertices",{{0,12,Type::Float32,"Position"}});
        array(at+164,4,mesh,"Leftover faces",{{0,4,Type::UInt32,"Face"}});
        const auto counts = array(at+176,4,mesh,"Index counts",{{0,4,Type::UInt32,"Count"}});
        const auto sources = array(at+188,4,mesh,"Index sources",{{0,4,Type::Offset32,"Source"}});
        array(at+200,4,mesh,"Index tokens",{{0,4,Type::UInt32,"Runtime token"}});
        for (std::size_t i = 0; i < sources.second; ++i) {
            const auto indices = bytes_->u32(counts.first+i*4);
            pool(bytes_->u32(sources.first+i*4),indices,2,mesh,"Index pool "+std::to_string(i),{{0,2,Type::UInt16,"Vertex"}});
        }
        const auto tail = bytes_->u32(at+220);
        if (tail) pool(bytes_->u32(at+212),tail,2,mesh,"Trailing indices",{{0,2,Type::UInt16,"Vertex"}});
        const auto extension = at+size;
        if (flags&kNodeSkin) skin(extension,mesh);
        if (flags&kNodeDangly) {
            Fields dangling{{12,4,Type::Float32,"Displacement"},{16,4,Type::Float32,"Tightness"},
                {20,4,Type::Float32,"Period"},{24,4,Type::Offset32,"Rest positions"}};
            arrayFields(dangling,0,"Constraints");
            const auto child = header(extension,28,mesh,"Dangly",std::move(dangling));
            array(extension,4,child,"Constraints",{{0,4,Type::Float32,"Constraint"}});
            pool(bytes_->u32(extension+24),count,12,child,"Rest positions",{{0,12,Type::Float32,"Position"}});
        }
        if (flags&kNodeAnimation) {
            Fields animated{{0,4,Type::Float32,"Sample period"},{40,4,Type::Offset32,"Position samples"},
                {44,4,Type::Offset32,"UV samples"},{48,4,Type::Int32,"Position frames"},{52,4,Type::Int32,"UV frames"}};
            arrayFields(animated,4,"Temporary positions"); arrayFields(animated,16,"Temporary UVs"); arrayFields(animated,28,"Temporary normals");
            const auto child = header(extension,56,mesh,"Animated mesh",std::move(animated));
            pool(bytes_->u32(extension+40),std::size_t(count)*bytes_->u32(extension+48),12,child,"Position samples",{{0,12,Type::Float32,"Position"}});
            pool(bytes_->u32(extension+44),std::size_t(count)*bytes_->u32(extension+52),8,child,"UV samples",{{0,8,Type::Float32,"UV"}});
        }
        if (flags&kNodeLightsaber) {
            const auto child = header(extension,20,mesh,"Lightsaber",{{0,4,Type::Offset32,"Positions"},
                {4,4,Type::Offset32,"UVs"},{8,4,Type::Offset32,"Normals"},{12,8,Type::UInt32,"Runtime handles"}});
            pool(bytes_->u32(extension),count,12,child,"Saber positions",{{0,12,Type::Float32,"Position"}});
            pool(bytes_->u32(extension+4),count,8,child,"Saber UVs",{{0,8,Type::Float32,"UV"}});
            pool(bytes_->u32(extension+8),count,12,child,"Saber normals",{{0,12,Type::Float32,"Normal"}});
        }
        if (flags&kNodeAabb) {
            const auto child = header(extension,4,mesh,"AABB",{{0,4,Type::Offset32,"Root"}});
            aabb(bytes_->u32(extension),child);
        }
        if (mdx_ && count && bytes_->u32(at+256) != 0 && !(flags&kNodeLightsaber)) {
            Fields channels;
            const auto bitmap = bytes_->u32(at+256);
            const auto channel = [&](std::size_t field,std::uint32_t bit,std::size_t width,Type type,const std::string& name) {
                if (bitmap&bit) channels.push_back({bytes_->u32(at+field),width,type,name});
            };
            channel(260,1,12,Type::Float32,"Position"); channel(264,0x20,12,Type::Float32,"Normal");
            channel(268,0x40,4,Type::UInt8,"Color");
            for (std::size_t i = 0; i < 4; ++i) {
                const auto offset = bytes_->u32(at+272+i*4);
                if (bitmap&(2u<<i)) channels.push_back({offset,8,Type::Float32,"UV "+std::to_string(i)});
                channel(288+i*4,0x80u<<i,36,Type::Float32,"Tangent basis "+std::to_string(i));
            }
            if (flags&kNodeSkin) {
                channels.push_back({bytes_->u32(extension+12),16,Type::Float32,"Skin weights"});
                channels.push_back({bytes_->u32(extension+16),16,Type::Float32,"Skin palette indices"});
            }
            add(1,bytes_->u32(at+size-8),count,bytes_->u32(at+252),mesh,"MDX vertices",std::move(channels));
        }
    }
    void skin(std::size_t at,std::size_t parent) {
        Fields fields{{12,4,Type::UInt32,"MDX weight offset"},{16,4,Type::UInt32,"MDX bone offset"},
            {20,4,Type::Offset32,"Forward map"},{24,4,Type::UInt32,"Forward map count"},
            {64,34,Type::Int16,"Bone palette"}};
        arrayFields(fields,28,"Inverse quaternions"); arrayFields(fields,40,"Inverse translations"); arrayFields(fields,52,"Bone constants");
        const auto skin = header(at,100,parent,"Skin",std::move(fields));
        pool(bytes_->u32(at+20),bytes_->u32(at+24),4,skin,"Forward bone map",{{0,4,Type::Float32,"Palette index"}});
        array(at+28,16,skin,"Inverse quaternions",{{0,16,Type::Float32,"Quaternion (w,x,y,z)"}});
        array(at+40,12,skin,"Inverse translations",{{0,12,Type::Float32,"Translation"}});
        array(at+52,4,skin,"Bone constants",{{0,4,Type::Int32,"Index"}});
    }
    void aabb(std::size_t root,std::size_t parent) {
        std::vector<std::pair<std::size_t,std::size_t>> pending;
        if (root != 0 && root != 0xffffffffu) pending.emplace_back(root,parent);
        while (!pending.empty()) {
            const auto [at,owner] = pending.back(); pending.pop_back();
            if (!aabbs_.insert(at).second) continue;
            const auto node = header(at,40,owner,"AABB node",{{0,12,Type::Float32,"Minimum"},
                {12,12,Type::Float32,"Maximum"},{24,4,Type::Offset32,"Left child"},{28,4,Type::Offset32,"Right child"},
                {32,4,Type::Int32,"Face"},{36,4,Type::UInt32,"Split mask"}});
            if (bytes_->i32(at+32) == -1) for (const auto field : {28u,24u}) {
                const auto child = bytes_->u32(at+field);
                if (child != 0 && child != 0xffffffffu) pending.emplace_back(child,node);
            }
        }
    }

    const std::vector<std::uint8_t>& mdl_;
    const std::optional<std::vector<std::uint8_t>>& mdx_;
    const neoshared::bif::JobControl& job_;
    packed::ByteView whole_;
    std::unique_ptr<packed::ByteView> bytes_;
    std::size_t base_{};
    PackedLayout result_;
    std::vector<std::string> names_;
    std::unordered_set<std::size_t> nodes_,aabbs_;
};
} // namespace

PackedLayout inspectPackedLayout(const std::vector<std::uint8_t>& mdl,
    const std::optional<std::vector<std::uint8_t>>& mdx,const neoshared::bif::JobControl& job) {
    return LayoutReader(mdl,mdx,job).read();
}

} // namespace neomdl::model
