#include <core/MeshReplacement.hpp>
#include <core/ResourceContext.hpp>
#include <model/AsciiExportInternal.hpp>
#include <model/AsciiSource.hpp>
#include <neoshared/texture/FileUtil.hpp>

#include <cmath>
#include <set>

namespace neomdl {

MeshDonor readMeshDonor(const std::filesystem::path& path, model::GameVersion asciiGame,
                       const neoshared::bif::JobControl& job) {
    ResourceLoadScope scope(job);
    MeshDonor donor;
    donor.path = std::filesystem::absolute(path);
    donor.inputs.push_back(donor.path);
    const model::AsciiResourceResolver resources = [&](const std::string& name, const std::string& extension)
        -> std::optional<std::vector<std::uint8_t>> {
        const auto file = uniqueResourceFile(donor.path.parent_path(), name + extension);
        if (!file) return {};
        donor.inputs.push_back(*file);
        return readResourceFile(*file);
    };
    job.update(0, 0, "Reading donor mesh geometry");
    const auto mdl = readResourceFile(donor.path);
    std::vector<std::uint8_t> mdx;
    if (!model::isAsciiModelInput(mdl))
        if (auto bytes = resources(neoshared::texture::pathToUtf8(path.stem()), ".mdx")) mdx = std::move(*bytes);
    donor.model = model::decodeModelBytes(mdl, mdx, neoshared::texture::pathToUtf8(path),
                                         job, asciiGame, resources, false);
    return donor;
}

std::map<std::string, std::size_t> namedMeshes(const model::Model& asset,
                                            std::optional<std::uint16_t> flags) {
    std::map<std::string, std::size_t> counts, result;
    for (const auto& node : asset.nodes) ++counts[model::ascii::lower(node.name)];
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const auto& node = asset.nodes[i];
        const auto name = model::ascii::lower(node.name);
        if (node.mesh && counts.at(name) == 1 && (!flags || node.flags == *flags)) result.emplace(name, i);
    }
    return result;
}

model::AsciiExportResult replaceMeshes(const std::string& text, const model::Model& target,
    const std::vector<MeshReplacement>& mappings, bool advanced, const neoshared::bif::JobControl& job) {
    using model::ascii::SourceText;
    static const std::set<std::string> fields{
        "texture0", "texture1", "bitmap", "verts", "normalpositionprecision", "normals", "normalindices",
        "tangentuvprecision", "tverts", "tverts1", "tverts2", "tverts3", "texindices1", "texindices2", "texindices3",
        "colors", "colorsrgba", "colorindices", "hastangentbasis", "tangentbasis", "tangentindices",
        "cpuverts", "cpuindices", "faceplanes", "spherecenter", "sphereradius", "totalarea", "faces",
        "vertexindicescount", "leftoverfaces", "vertexindices", "diffuse", "ambient", "shadow", "render",
        "beaming", "rotatetexture", "lightmapped", "m_bisbackgroundgeometry", "transparencyhint", "animateuv",
        "uvdirectionx", "uvdirectiony", "uvjitter", "uvjitterspeed", "bmin", "bmax",
        "dirt_enabled", "dirt_texture", "dirt_worldspace", "hologram_donotdraw",
        "sampleperiod", "animverts", "animtverts", "tangents", "mirrorlist",
        "displacement", "tightness", "period", "constraints", "weights", "qbone_ref_inv", "tbone_ref_inv",
        "boneconstantindices", "skinbonemap", "skinpalette", "skinbindrotations", "skinbindtranslations",
        "skinweights", "aabb", "saberverts", "sabernormals", "saberuvs", "saberoriginalverts"};
    const auto targets = namedMeshes(target);
    struct Replacement { std::size_t target; const MeshReplacement* mapping; };
    std::map<std::string, Replacement> replacements;
    for (const auto& mapping : mappings) {
        const auto name = model::ascii::lower(mapping.target);
        replacements[name] = {targets.at(name), &mapping};
    }
    model::AsciiExportOptions options;
    options.advanced = advanced;
    options.explicitNormals = true;
    model::AsciiExportResult result;
    SourceText source(text);
    source.materializeTangentPolicies();
    const auto blocks = source.nodes();
    const auto pose = model::constructedBindPose(target, job);
    auto bounds = target.engineBounds;
    auto radius = target.engineRadius;
    std::map<std::pair<const MeshDonor*, std::size_t>, model::AsciiExportResult> payloads;
    std::size_t completed = 0;
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        const auto found = replacements.find(model::ascii::lower(block->name));
        if (!block->geometry || found == replacements.end()) continue;
        const auto& mapping = *found->second.mapping;
        const auto targetIndex = found->second.target;
        const auto& donor = mapping.donor->model;
        const auto donorIndex = namedMeshes(donor, target.nodes[targetIndex].flags).at(model::ascii::lower(mapping.mesh));
        const auto& node = donor.nodes[donorIndex];
        const auto& mesh = donor.meshes[*node.mesh];
        job.update(completed++, replacements.size(), "Replacing mesh: " + block->name);
        const auto key = std::make_pair(mapping.donor.get(), donorIndex);
        auto payload = payloads.find(key);
        if (payload == payloads.end()) {
            model::ascii_export::Writer writer(target.game, job, options);
            writer.indent(2);
            const auto geometry = model::ascii_export::writeMesh(writer, donor, node, mesh);
            if (mesh.skinned)
                model::ascii_export::writeStandardSkin(writer, donor, mesh, geometry);
            auto exported = writer.finish();
            // A geometry-only ASCII donor has not resolved material/TXI choices.
            // Keep an automatic tangent choice automatic in the target context.
            if (advanced && mesh.asciiTangentSource && !donor.asciiMaterialsFinalized &&
                !mesh.asciiTangents && !(mesh.mdxBitmap & 0x80u)) {
                SourceText material(exported.text);
                material.lines.erase(std::remove_if(material.lines.begin(), material.lines.end(), [](const auto& line) {
                    return SourceText::key(line) == "hastangentbasis";
                }), material.lines.end());
                exported.text = material.text();
            }
            payload = payloads.emplace(key, std::move(exported)).first;
            result.warnings.insert(result.warnings.end(), payload->second.warnings.begin(), payload->second.warnings.end());
        }
        std::vector<std::string> body;
        for (auto i = block->begin + 1; i < block->end; i = source.nextRecord(i))
            if (!fields.count(SourceText::key(source.lines[i])))
                body.insert(body.end(), source.lines.begin() + i, source.lines.begin() + source.nextRecord(i));
        const SourceText replacement(payload->second.text);
        body.insert(body.end(), replacement.lines.begin(), replacement.lines.end());
        source.lines.erase(source.lines.begin() + block->begin + 1, source.lines.begin() + block->end);
        source.lines.insert(source.lines.begin() + block->begin + 1, body.begin(), body.end());

        // Keep the target's existing animated envelope and expand it to contain
        // each replacement at its target bind transform; never copy donor bounds globally.
        const auto& box = mesh.engineBounds.value().box;
        for (unsigned corner = 0; corner < 8; ++corner) {
            const auto point = model::transformPoint(pose.worldTransforms[targetIndex], {
                corner & 1 ? box.maximum.x : box.minimum.x,
                corner & 2 ? box.maximum.y : box.minimum.y,
                corner & 4 ? box.maximum.z : box.minimum.z});
            bounds.minimum = {std::min(bounds.minimum.x, point.x), std::min(bounds.minimum.y, point.y), std::min(bounds.minimum.z, point.z)};
            bounds.maximum = {std::max(bounds.maximum.x, point.x), std::max(bounds.maximum.y, point.y), std::max(bounds.maximum.z, point.z)};
            radius = std::max(radius, std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z));
        }
    }
    model::ascii_export::Writer writer(target.game, job);
    for (std::size_t i = 0; i < source.lines.size();) {
        const auto field = SourceText::key(source.lines[i]);
        if (field == "bmin" || field == "bmax" || field == "radius") {
            source.lines.erase(source.lines.begin() + i);
        } else if (field == "endmodelgeom") {
            source.lines.insert(source.lines.begin() + i, {"  bmin " + writer.vector(bounds.minimum),
                "  bmax " + writer.vector(bounds.maximum), "  radius " + writer.scalar(radius)});
            break;
        } else i = source.nextRecord(i);
    }
    result.text = source.text();
    return result;
}

} // namespace neomdl
