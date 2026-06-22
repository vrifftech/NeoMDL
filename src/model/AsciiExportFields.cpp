#include <model/AsciiExportInternal.hpp>
#include <model/Emitter.hpp>

#include <functional>
#include <utility>

namespace neomdl::model::ascii_export {
namespace {

std::string signedWord(std::uint32_t value) {
    const auto number=value<0x80000000u ? static_cast<std::int64_t>(value)
        : static_cast<std::int64_t>(value)-0x100000000ll;
    return std::to_string(number);
}

void boolean(Writer& writer,const std::string& name,bool value) {
    writer.line(name+(value?" 1":" 0"));
}

void integer(Writer& writer,const std::string& name,std::int32_t value) {
    writer.line(name+" "+std::to_string(value));
}

void resource(Writer& writer,const std::string& name,const std::string& value) {
    // Emitter string members start empty in both engine constructors. The
    // engine %s grammar has no quoted empty-string token.
    if(value.empty()) return;
    writer.token(value,name);
    writer.line(name+" "+value);
}

void floatList(Writer& writer,const std::string& name,const std::vector<float>& values) {
    writer.line(name+" "+std::to_string(values.size()));
    for(const auto value:values) {
        writer.job().check();
        writer.line(writer.scalar(value));
    }
}

void light(Writer& writer,const LightNodeData& value) {
    // K1 0x5a2b24 / K2 0x79ef50: direct fields and counted flare lists.
    integer(writer,"ambientonly",value.ambientOnly);
    integer(writer,"shadow",value.castsShadow);
    writer.line("nDynamicType "+std::to_string(value.dynamicType));
    integer(writer,"affectdynamic",value.affectDynamic);
    writer.line("lightpriority "+std::to_string(value.priority));
    integer(writer,"generateflare",value.generateFlare);
    integer(writer,"fadingLight",value.fading);
    writer.line("flareradius "+writer.scalar(value.flareRadius));
    writer.line("texturenames "+std::to_string(value.textureNames.size()));
    for(const auto& texture:value.textureNames) {
        writer.job().check();
        writer.token(texture,"light flare texture");
        writer.line(texture);
    }
    floatList(writer,"flaresizes",value.flareSizes);
    floatList(writer,"flarepositions",value.flarePositions);
    writer.line("flarecolorshifts "+std::to_string(value.flareColorShifts.size()));
    for(const auto color:value.flareColorShifts) {
        writer.job().check();
        writer.line(writer.vector(color));
    }
}

void emitter(Writer& writer,const EmitterNodeData& value) {
    // K1 0x5a4480 / K2 0x7a0410. The engine setters expose exactly these
    // 13 persistent flag bits; the other raw words mirror typed fields.
    const auto flags=value.rawWords.back();
    if(flags&~0x1fffu)
        throw ModelError("ASCII MDL cannot represent unknown emitter flag bits");
    resource(writer,"update",value.updateMode);
    resource(writer,"render",value.renderMode);
    resource(writer,"blend",value.blendMode);
    resource(writer,"texture",value.texture);
    resource(writer,"chunkName",value.chunkName);
    resource(writer,"m_sDepthTextureName",value.depthTexture);
    writer.line("spawntype "+signedWord(value.spawnType));
    writer.line("xgrid "+signedWord(value.xGrid));
    writer.line("ygrid "+signedWord(value.yGrid));
    writer.line("numBranches "+signedWord(value.branchCount));
    integer(writer,"controlptsmoothing",value.controlPointSmoothing);
    integer(writer,"loop",value.loop);
    integer(writer,"twosidedtex",value.twoSidedTexture);
    writer.line("deadspace "+writer.scalar(value.deadSpace));
    writer.line("blastRadius "+writer.scalar(value.blastRadius));
    writer.line("blastLength "+writer.scalar(value.blastLength));
    writer.line("renderorder "+std::to_string(value.renderOrder));
    boolean(writer,"m_bFrameBlending",value.frameBlending);
    static constexpr std::pair<const char*,std::uint32_t> fields[]{
        {"p2p",emitter_flags::PointToPoint},
        {"p2p_sel",emitter_flags::PointToPointBezier},
        {"affectedByWind",emitter_flags::AffectedByWind},
        {"m_isTinted",emitter_flags::Tinted},
        {"bounce",emitter_flags::Bounce},
        {"inherit",emitter_flags::Inherit},
        {"inheritvel",emitter_flags::InheritVelocity},
        {"inherit_local",emitter_flags::InheritLocal},
        {"splat",emitter_flags::Splat},
        {"inherit_part",emitter_flags::InheritPart},
        {"depth_texture",emitter_flags::DepthTexture}
    };
    for(const auto& field:fields) boolean(writer,field.first,(flags&field.second)!=0u);
    const auto random=((flags&emitter_flags::RandomFrame)?1u:0u) |
                      ((flags&emitter_flags::RandomOrder)?2u:0u);
    writer.line("random "+std::to_string(random));
}

void aabb(Writer& writer,const AabbNodeData& tree) {
    // K1 0x5a1004 / K2 0x79be90: one inline root row followed by the two
    // child subtrees whenever faceIndex is -1. Explicit masks avoid the
    // engine center-difference mask producer changing an authored mask.
    if(!tree.valid || !tree.root)
        throw ModelError("ASCII MDL requires a valid, nonempty AABB tree");
    std::vector<bool> written(tree.nodes.size());
    std::function<void(std::size_t,bool)> write;
    write=[&](std::size_t index,bool root) {
        writer.job().check();
        if(index>=tree.nodes.size() || written[index])
            throw ModelError("ASCII MDL cannot represent invalid or shared AABB child links");
        written[index]=true;
        const auto& node=tree.nodes[index];
        if(node.leaf() && node.splitMask!=0u)
            writer.warning("ASCII MDL discards the unused split mask on an AABB leaf");
        writer.line(std::string(root?"aabb ":"")+writer.vector(node.minimum)+" "+
                    writer.vector(node.maximum)+" "+std::to_string(node.faceIndex)+" "+
                    signedWord(node.splitMask));
        if(!node.leaf()) {
            if(node.firstChild<0 || node.secondChild<0)
                throw ModelError("ASCII MDL requires two children for each internal AABB node");
            write(static_cast<std::size_t>(node.firstChild),false);
            write(static_cast<std::size_t>(node.secondChild),false);
        }
    };
    write(*tree.root,true);
}

} // namespace

void writeNodeFields(Writer& writer,const Model&,const Node& node) {
    if(node.light) light(writer,*node.light);
    if(node.emitter) emitter(writer,*node.emitter);
    if(node.reference) {
        if(node.reference->model.empty())
            throw ModelError("ASCII MDL cannot represent an empty referenced model name");
        resource(writer,"refModel",node.reference->model);
        integer(writer,"reattachable",node.reference->reattachable);
    }
    if(node.aabb) aabb(writer,*node.aabb);
}

} // namespace neomdl::model::ascii_export
