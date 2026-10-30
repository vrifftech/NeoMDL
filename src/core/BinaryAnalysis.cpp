#include "core/BinaryAnalysis.hpp"
#include "core/ModelFilenames.hpp"
#include "model/PackedBytes.hpp"
#include <neoshared/PathUtf8.hpp>

#include <chrono>
#include <iomanip>
#include <map>
#include <sstream>

namespace neomdl {

BinaryAnalysis readBinaryAnalysis(const std::filesystem::path& input,const neoshared::bif::JobControl& job) {
    ResourceLoadScope scope(job);
    return readBinaryAnalysis(input,readResourceFile(input),job);
}

BinaryAnalysis readBinaryAnalysis(const std::filesystem::path& input,
    std::vector<std::uint8_t> primary,const neoshared::bif::JobControl& job) {
    ResourceLoadScope scope(job);
    BinaryAnalysis result;
    const auto path = std::filesystem::absolute(input);
    const auto directory = path.parent_path();
    const bool mdxInput=resourceKey(neoshared::pathToUtf8(path.extension())) == ".mdx";
    result.paths[0] = mdxInput
        ? uniqueResourceFile(directory,modelFileStem(path)+".mdl").value() : path;
    result.mdl = mdxInput ? readResourceFile(result.paths[0]) : std::move(primary);
    if (mdxInput) {
        result.paths[1]=path;result.mdx=std::move(primary);
    } else if (const auto mdx = uniqueResourceFile(directory,modelFileStem(path)+".mdx")) {
        result.paths[1] = *mdx; result.mdx = readResourceFile(*mdx);
    }
    result.layout = model::inspectPackedLayout(result.mdl,result.mdx,job);
    return result;
}

std::string binaryAnalysisReport(const BinaryAnalysis& analysis,const neoshared::bif::JobControl& job) {
    const auto& layout = analysis.layout;
    std::ostringstream out;
    out << "Model: " << layout.name << "\nLayout: PC " << (layout.game == model::GameVersion::Kotor1 ? "K1" : "K2")
        << "\nMDL payload begins at absolute offset " << layout.payloadOffset << '\n';
    for (std::size_t file = 0; file < 2; ++file) {
        if (analysis.paths[file].empty()) { out << "MDX: not supplied\n"; continue; }
        const auto size = file == 0 ? analysis.mdl.size() : analysis.mdx->size();
        const auto mapped = layout.mappedBytes(file);
        out << (file == 0 ? "MDL: " : "MDX: ") << neoshared::pathToUtf8(analysis.paths[file])
            << "\n  " << size << " bytes; " << mapped << " in mapped structures; " << size-mapped << " outside mapped structures\n";
    }
    out << "Structures: " << layout.structures.size() << '\n';
    model::packed::ByteView bytes(analysis.mdl.data(),analysis.mdl.size(),"MDL",job);
    std::map<std::uint16_t,std::size_t> nodeTypes;
    std::map<std::pair<std::uint32_t,std::uint8_t>,std::size_t> controllers;
    std::map<std::size_t,std::string> emitters;
    std::map<std::string,std::size_t> emitterSignatures;
    for (std::size_t index = 0; index < layout.structures.size(); ++index) {
        job.check(); const auto& region = layout.structures[index];
        if (region.name.compare(0,6,"Node: ") == 0) {
            const auto flags = bytes.u16(region.offset); ++nodeTypes[flags];
            if (flags&model::kNodeEmitter) emitters.emplace(index,"(none)");
        }
        if (region.name != "Controllers") continue;
        std::ostringstream signature;
        for (std::size_t i = 0; i < region.count; ++i) {
            const auto at = region.offset+i*region.stride;
            const auto type = bytes.u32(at); const auto encoding = bytes.u8(at+12);
            ++controllers[{type,encoding}];
            if (i) signature << ", ";
            signature << type << "/0x" << std::hex << unsigned(encoding) << std::dec;
        }
        const auto emitter = emitters.find(*region.parent);
        if (emitter != emitters.end()) emitter->second = signature.str();
    }
    for (const auto& item : emitters) ++emitterSignatures[item.second];
    out << "\nNode flags (geometry and animation nodes):\n";
    for (const auto& [flags,count] : nodeTypes) out << "  0x" << std::hex << flags << std::dec << ": " << count << '\n';
    out << "Controller type / encoding frequencies:\n";
    for (const auto& [key,count] : controllers)
        out << "  " << key.first << " / 0x" << std::hex << unsigned(key.second) << std::dec << ": " << count << '\n';
    out << "Emitter controller signatures (type/encoding, stored order):\n";
    for (const auto& [signature,count] : emitterSignatures) out << "  " << count << "x " << signature << '\n';
    out << "\nStructure map (absolute offsets; end is exclusive):\n";
    for (std::size_t file = 0; file < 2; ++file) for (const auto index : layout.ordered[file]) {
        job.check();
        const auto& region = layout.structures[index];
        out << '[' << index << "] " << (file == 0 ? "MDL" : "MDX") << " 0x" << std::hex << region.offset
            << "..0x" << region.offset+region.size() << std::dec << " | " << region.name
            << " | " << region.count << " x " << region.stride << " bytes | parent ";
        if (region.parent) out << *region.parent; else out << "none";
        out << '\n';
        for (const auto& field : region.fields)
            out << "    +" << field.offset << " | " << field.size << " bytes | "
                << model::binaryTypeName(field.type) << " | " << field.name << '\n';
    }
    out << "\nUnclassified bytes include padding, runtime-only fields and allocations outside reachable structures.\n"
           "This is a stored-layout inventory, not a conversion or runtime-validity assessment.\n";
    return out.str();
}

BinaryAnalysisResult analyzeBinaryFile(const std::filesystem::path& path,const neoshared::bif::JobControl& job) {
    BinaryAnalysisResult result; result.path = path; result.inputs.push_back(path);
    const auto start = std::chrono::steady_clock::now();
    try {
        const auto analysis = readBinaryAnalysis(path,job);
        result.inputs.assign(analysis.paths.begin(),analysis.paths.end());
        result.report = binaryAnalysisReport(analysis,job);
    } catch (const neoshared::bif::JobCancelled&) { throw; }
    catch (const std::exception& error) { result.error = error.what(); }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    return result;
}

std::string binaryBatchReport(const std::vector<BinaryAnalysisResult>& results) {
    std::ostringstream out;
    out << "NeoMDL binary structure analysis\n\n";
    for (const auto& result : results) {
        out << neoshared::pathToUtf8(result.path) << " (" << std::fixed << std::setprecision(3) << result.seconds << " s)\n";
        if (result.error.empty()) out << result.report;
        else out << "ERROR: " << result.error << '\n';
        out << '\n';
    }
    return out.str();
}

} // namespace neomdl
