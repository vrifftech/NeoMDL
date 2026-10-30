#include "core/Conversion.hpp"
#include "core/AsciiMaterials.hpp"
#include "core/ModelDocument.hpp"
#include <model/AsciiModel.hpp>
#include <model/PackedExport.hpp>
#include <neoshared/PathUtf8.hpp>
#include <neoshared/texture/FileUtil.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace neomdl {
namespace {

constexpr CollisionKind kinds[]{CollisionKind::Placeable, CollisionKind::DoorClosed,
    CollisionKind::DoorOpen1, CollisionKind::DoorOpen2, CollisionKind::Room};
const std::map<std::string, CollisionKind> collisionKinds{
    {"wok",CollisionKind::Room},{"pwk",CollisionKind::Placeable},{"dwk0",CollisionKind::DoorClosed},
    {"dwk1",CollisionKind::DoorOpen1},{"dwk2",CollisionKind::DoorOpen2}};

std::optional<CollisionKind> standaloneCollision(const std::filesystem::path& input) {
    auto path = input;
    if (resourceKey(neoshared::pathToUtf8(path.extension())) == ".ascii") path.replace_extension();
    const auto extension = resourceKey(neoshared::pathToUtf8(path.extension()));
    if (extension == ".wok") return CollisionKind::Room;
    if (extension == ".pwk") return CollisionKind::Placeable;
    if (extension != ".dwk") return {};
    const auto stem = resourceKey(neoshared::pathToUtf8(path.stem()));
    for (int state = 1; state <= 2; ++state) {
        const auto suffix = "_dwk" + std::to_string(state);
        if (stem.size() >= suffix.size() && stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
            return static_cast<CollisionKind>(state + 1);
    }
    return CollisionKind::DoorClosed;
}

void append(std::vector<CollisionOutput>& outputs, std::vector<CollisionOutput> extra) {
    outputs.insert(outputs.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));
}

std::vector<CollisionOutput> buildConversion(const ConversionRequest& request, const ConversionOptions& options,
    ConversionReport& report, std::vector<std::filesystem::path>& inputs, const neoshared::bif::JobControl& job) {
    const auto bytes = readResourceFile(request.input);
    std::vector<CollisionOutput> outputs;
    if (const auto kind = standaloneCollision(request.input)) {
        neoshared::ResourceDocument resource;
        resource.fileName = neoshared::pathToUtf8(request.input.filename()); resource.bytes = bytes;
        const auto collision = readCollision(resource, *kind, job);
        if (request.asciiOutput) outputs.push_back({*request.asciiOutput, {collision.text.begin(), collision.text.end()}});
        if (request.packedOutput) {
            auto mesh = readCollisionAscii(collision.text, *kind);
            neoshared::bwm::buildBwm(mesh.mesh, mesh.walkableMaterials, job);
            outputs.push_back({*request.packedOutput, neoshared::bwm::writeBwm(mesh.mesh, job)});
        }
        report.summary = collisionLabel(*kind);
        return outputs;
    }

    auto trace = std::make_shared<ResourceTrace>();
    const auto directory = std::filesystem::absolute(request.input).parent_path();
    const auto resolver = traceResources(collisionResources(request.input, directoryResources(directory)), trace, report.source);
    const model::AsciiResourceResolver resources = [&](const std::string& name, const std::string& extension)
        -> std::optional<std::vector<std::uint8_t>> {
        if (extension == ".mdl" || extension == ".mdx") {
            if (auto resource = resolver(name, extension == ".mdl" ? kMdlResourceType : kMdxResourceType))
                return std::move(resource->bytes);
            return {};
        }
        const auto file = uniqueResourceFile(directory, name + extension);
        if (!file) return {};
        inputs.push_back(*file);
        return readResourceFile(*file);
    };
    std::vector<std::uint8_t> mdx;
    if (!model::isAsciiModelInput(bytes)) {
        if (request.mdx) mdx = readResourceFile(*request.mdx);
        else if (auto resource = resolver(modelFileStem(request.input), kMdxResourceType)) mdx = std::move(resource->bytes);
    }
    auto model = model::decodeModelBytes(bytes, mdx, report.source, job, options.game, resources, options.ascii.includeAnimations);
    const auto finish = [&] {
        finalizeAsciiMaterials(model, resolver, job);
        model::processAsciiSkinSeams(model, job);
    };
    finish();
    std::optional<model::AsciiExportResult> replacedAscii;
    if (!options.replacements.empty()) {
        std::map<std::string, std::shared_ptr<const MeshDonor>> donors;
        std::vector<MeshReplacement> mappings;
        for (const auto& replacement : options.replacements) {
            const auto key = resourceFileIdentity(replacement.donor);
            auto found = donors.find(key);
            if (found == donors.end()) {
                auto donor = std::make_shared<MeshDonor>(readMeshDonor(replacement.donor, model.game, job));
                inputs.insert(inputs.end(), donor->inputs.begin(), donor->inputs.end());
                found = donors.emplace(key, std::move(donor)).first;
            }
            const auto colon = replacement.mapping.find(':');
            mappings.push_back({replacement.mapping.substr(0,colon),replacement.mapping.substr(colon).substr(1),found->second});
        }
        std::string source;
        if (model.asciiBuilt && model.advancedAscii == options.ascii.advanced && options.ascii.includeAnimations &&
            !options.ascii.explicitNormals && !options.ascii.refineTangentRounding && !options.ascii.refineNormalRounding)
            source.assign(bytes.begin(), bytes.end());
        else {
            auto exported = model::exportAsciiModel(model, job, options.ascii);
            report.warnings.insert(report.warnings.end(), exported.warnings.begin(), exported.warnings.end());
            source = std::move(exported.text);
        }
        replacedAscii = replaceMeshes(source, model, mappings, options.ascii.advanced, job);
        report.warnings.insert(report.warnings.end(), replacedAscii->warnings.begin(), replacedAscii->warnings.end());
        replacedAscii->warnings.clear();
        model = model::decodeModelBytes({replacedAscii->text.begin(), replacedAscii->text.end()}, {}, report.source,
            job, options.game.value_or(model.game), resources, options.ascii.includeAnimations);
        finish();
    }
    if (request.packedOutput && options.game && model.game != *options.game) {
        model::AsciiExportOptions retained = options.ascii; retained.advanced = true;
        auto source = model::exportAsciiModel(model, job, retained);
        report.warnings.insert(report.warnings.end(), source.warnings.begin(), source.warnings.end());
        model = model::decodeModelBytes({source.text.begin(), source.text.end()}, {}, report.source,
            job, options.game, resources, options.ascii.includeAnimations);
        finish();
        replacedAscii.reset();
    }
    if (options.rebuildAabb) model::rebuildAabbTrees(model, job);
    auto collisions = loadCollisions(modelFileStem(request.input), resolver, job);
    auto placement = options.placement;
    if (options.layout) {
        const auto data = readResourceFile(*options.layout);
        placement = collisionLayout(std::string(data.begin(), data.end())).at(resourceKey(model.name));
    }
    std::map<std::string, std::size_t> nodes;
    for (std::size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].mesh) nodes.emplace(resourceKey(model.nodes[i].name), i);
    for (const auto& source : options.collisionSources) {
        const auto colon = source.find(':');
        const auto kind = collisionKinds.at(source.substr(0, colon));
        auto mesh = collisionFromModel(model, nodes.at(resourceKey(source.substr(colon + 1))), kind,
            kind == CollisionKind::Room ? placement : model::Vec3{}, job);
        if (collisions.count(kind)) {
            const auto previous = readCollisionAscii(collisions.at(kind).text, kind);
            mesh.mesh.usePoints = previous.mesh.usePoints; mesh.mesh.doorUsePoints = previous.mesh.doorUsePoints;
            mesh.walkableMaterials = previous.walkableMaterials;
        }
        collisions[kind].text = writeCollisionAscii(mesh, kind);
    }
    if (!options.includeCollisions) collisions.clear();
    report.game = model.game;
    report.summary = model::modelSummary(model);
    report.warnings.insert(report.warnings.end(), model.warnings.begin(), model.warnings.end());
    if (request.asciiOutput) {
        const auto exported = replacedAscii && !options.rebuildAabb ? std::move(*replacedAscii) :
            model::exportAsciiModel(model, job, options.ascii);
        outputs.push_back({*request.asciiOutput, {exported.text.begin(), exported.text.end()}});
        report.warnings.insert(report.warnings.end(), exported.warnings.begin(), exported.warnings.end());
        append(outputs, collisionOutputs(collisions, *request.asciiOutput, false, job));
    }
    if (request.packedOutput) {
        auto exported = model::exportPackedModel(model, job);
        auto companion = *request.packedOutput; companion.replace_extension(".mdx");
        outputs.push_back({companion, std::move(exported.mdx)});
        outputs.push_back({*request.packedOutput, std::move(exported.mdl)});
        append(outputs, collisionOutputs(collisions, *request.packedOutput, true, job));
    }
    const auto dependencies = trace->inputs();
    inputs.insert(inputs.end(), dependencies.begin(), dependencies.end());
    return outputs;
}

} // namespace

ConversionRequest planConversion(const std::filesystem::path& input, ConversionMode mode,
    const std::filesystem::path& directory, AsciiNaming naming) {
    ConversionRequest request; request.input = input;
    const bool collision = standaloneCollision(input).has_value();
    if (mode == ConversionMode::Opposite) {
        std::ifstream file(input, std::ios::binary); file.exceptions(std::ios::failbit | std::ios::badbit);
        std::vector<std::uint8_t> header(8); file.read(reinterpret_cast<char*>(header.data()), 8);
        const bool ascii = collision ? std::string(header.begin(), header.end()) != "BWM V1.0" : model::isAsciiModelInput(header);
        mode = ascii ? ConversionMode::Binary : ConversionMode::Ascii;
    }
    const auto destination = directory.empty() ? input.parent_path() : directory;
    auto source = input;
    if (collision && resourceKey(neoshared::pathToUtf8(source.extension())) == ".ascii") source.replace_extension();
    const auto stem = conversionFileStem(source);
    const auto extension = collision ? resourceKey(neoshared::pathToUtf8(source.extension())) : ".mdl";
    if (mode == ConversionMode::Ascii) request.asciiOutput = destination / std::filesystem::u8path(asciiModelFilename(stem, naming, extension));
    else request.packedOutput = destination / std::filesystem::u8path(stem + "-packed" + extension);
    return request;
}

std::filesystem::path conversionReportPath(const ConversionRequest& request) {
    auto path = request.asciiOutput ? *request.asciiOutput : *request.packedOutput;
    path += ".report.txt";
    return path;
}

std::vector<std::filesystem::path> conversionInputs(const ConversionRequest& request) {
    std::vector<std::filesystem::path> inputs{request.input};
    const auto directory = std::filesystem::absolute(request.input).parent_path();
    if (request.mdx) inputs.push_back(*request.mdx);
    else if (auto mdx = uniqueResourceFile(directory, modelFileStem(request.input) + ".mdx")) inputs.push_back(*mdx);
    for (const auto kind : kinds) {
        const auto name = collisionPath(request.input, kind).filename();
        if (auto file = uniqueResourceFile(directory, neoshared::pathToUtf8(name))) inputs.push_back(*file);
    }
    return inputs;
}

std::vector<std::filesystem::path> conversionOutputs(const ConversionRequest& request, const ConversionOptions& options) {
    std::vector<std::filesystem::path> outputs;
    const bool collision = standaloneCollision(request.input).has_value();
    std::set<CollisionKind> present;
    if (!collision && options.includeCollisions) {
        const auto directory = std::filesystem::absolute(request.input).parent_path();
        for (const auto kind : kinds)
            if (uniqueResourceFile(directory, neoshared::pathToUtf8(collisionPath(request.input, kind).filename()))) present.insert(kind);
        for (const auto& source : options.collisionSources) present.insert(collisionKinds.at(source.substr(0, source.find(':'))));
    }
    for (const auto& output : {request.asciiOutput, request.packedOutput}) if (output) {
        outputs.push_back(*output);
        for (const auto kind : present) outputs.push_back(collisionPath(*output, kind));
    }
    if (request.packedOutput && !collision) {
        auto companion = *request.packedOutput; companion.replace_extension(".mdx"); outputs.push_back(companion);
    }
    if (request.asciiOutput || request.packedOutput) outputs.push_back(conversionReportPath(request));
    return outputs;
}

void writeConversionReport(const std::filesystem::path& path, const ConversionReport& report,
                           const ConversionOptions& options) {
    std::ostringstream text;
    text << "NeoMDL " << NEOMDL_VERSION << " conversion report\nSource: " << report.source << "\nStatus: "
         << (report.error.empty() ? "Succeeded" : "Failed") << "\nElapsed seconds: " << std::fixed << std::setprecision(3) << report.seconds;
    if (report.game) text << "\nGame: " << (*report.game == model::GameVersion::Kotor1 ? "K1" : "K2");
    text << "\nASCII format: " << (options.ascii.advanced ? "Advanced" : "Standard")
         << "\nNormals: " << (!options.ascii.explicitNormals ? "Automatic" : *options.ascii.explicitNormals ? "Explicit" : "Smoothing groups")
         << "\nRefine tangent rounding: " << (options.ascii.refineTangentRounding ? "yes" : "no")
         << "\nRefine normal rounding: " << (options.ascii.refineNormalRounding ? "yes" : "no")
         << "\nOwn animations: " << (options.ascii.includeAnimations ? "included" : "omitted")
         << "\nCollision companions: " << (options.includeCollisions ? "included when present" : "omitted")
         << "\nRebuild MDL AABB trees: " << (options.rebuildAabb ? "yes" : "no") << '\n';
    for (const auto& mapping : options.replacements)
        text << "Replacement: " << mapping.mapping << " from " << neoshared::pathToUtf8(mapping.donor) << '\n';
    for (const auto& source : options.collisionSources) text << "Collision geometry: " << source << '\n';
    if (options.layout) text << "Layout: " << neoshared::pathToUtf8(*options.layout) << '\n';
    for (const auto& output : report.outputs) text << "Output: " << neoshared::pathToUtf8(output) << '\n';
    if (!report.error.empty()) text << "Error: " << report.error << '\n';
    text << '\n' << report.summary << '\n';
    for (const auto& warning : report.warnings) text << "Warning: " << warning << '\n';
    const auto contents = text.str();
    neoshared::texture::writeFileBytes(path, {contents.begin(), contents.end()});
}

ConversionReport convertFile(const ConversionRequest& request, const ConversionOptions& options,
    const std::vector<std::filesystem::path>& batchInputs, const neoshared::bif::JobControl& job) {
    ResourceLoadScope scope(job, true);
    const auto started = std::chrono::steady_clock::now();
    ConversionReport report; report.source = neoshared::pathToUtf8(request.input); report.game = options.game;
    auto inputs = conversionInputs(request);
    inputs.insert(inputs.end(), batchInputs.begin(), batchInputs.end());
    if (options.layout) inputs.push_back(*options.layout);
    std::sort(inputs.begin(),inputs.end());inputs.erase(std::unique(inputs.begin(),inputs.end()),inputs.end());
    const bool exporting = request.asciiOutput || request.packedOutput;
    if (exporting) neoshared::checkResourceOutput(conversionReportPath(request), inputs);
    const auto checkedReportInputs=inputs;
    try {
        auto outputs = buildConversion(request, options, report, inputs, job);
        std::sort(inputs.begin(),inputs.end());inputs.erase(std::unique(inputs.begin(),inputs.end()),inputs.end());
        if (exporting) {
            std::vector<std::filesystem::path> discovered;
            std::set_difference(inputs.begin(),inputs.end(),checkedReportInputs.begin(),checkedReportInputs.end(),std::back_inserter(discovered));
            neoshared::checkResourceOutput(conversionReportPath(request), discovered);
            inputs.push_back(conversionReportPath(request));
        }
        std::vector<std::filesystem::path> destinations;
        for (const auto& output : outputs) destinations.push_back(output.path);
        neoshared::checkResourceOutputs(destinations, inputs);
        job.check();
        for (const auto& output : outputs) {
            neoshared::texture::writeFileBytes(output.path, output.bytes);
            report.outputs.push_back(output.path);
        }
    } catch (const neoshared::bif::JobCancelled&) { throw; }
    catch (const std::exception& error) { report.error = error.what(); }
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (exporting) writeConversionReport(conversionReportPath(request), report, options);
    return report;
}

} // namespace neomdl
