#include <model/Model.hpp>
#include <model/AsciiExport.hpp>
#include <model/AsciiModel.hpp>
#include <model/PackedExport.hpp>
#include <core/ModelDocument.hpp>
#include <neoshared/ResourceDocument.hpp>
#include <neoshared/game/Installation.hpp>
#include <neoshared/texture/FileUtil.hpp>

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    const auto printUsage = [] {
        std::cout << "Usage: neomdl-info [--game=k1|--game=k2] [--export-ascii output.mdl] [--explicit-normals|--smoothing-groups] [--export-packed output.mdl] <model.mdl> [model.mdx]\n"
                     "ASCII models use the shared x86-layout builder and require an explicit target game.\n"
                     "ASCII export writes one model asset and its own animations to a separate file.\n"
                     "--explicit-normals preserves normals in a NeoMDL ASCII extension.\n"
                     "--smoothing-groups reconstructs smoothing groups instead of explicit mesh normals.\n"
                     "Without either option, export preserves the mesh's input mode; compiled meshes use smoothing groups.\n"
                     "Packed export compiles ASCII input to an x86 MDL and same-name MDX pair.\n";
    };
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        printUsage();
        return 0;
    }
    std::optional<neomdl::model::GameVersion> game;
    std::optional<std::filesystem::path> asciiOutput;
    std::optional<std::filesystem::path> packedOutput;
    neomdl::model::AsciiExportOptions asciiOptions;
    std::vector<std::filesystem::path> paths;
    for(int i=1;i<argc;++i) {
        const std::string_view argument(argv[i]);
        if(argument=="--game=k1")game=neomdl::model::GameVersion::Kotor1;
        else if(argument=="--game=k2")game=neomdl::model::GameVersion::Kotor2;
        else if(argument=="--explicit-normals")asciiOptions.explicitNormals=true;
        else if(argument=="--smoothing-groups")asciiOptions.explicitNormals=false;
        else if(argument=="--export-ascii") {
            if(i+1>=argc) {printUsage();return 2;}
            asciiOutput=std::filesystem::u8path(argv[++i]);
        } else if(argument.substr(0u,15u)=="--export-ascii=") {
            asciiOutput=std::filesystem::u8path(std::string(argument.substr(15u)));
        } else if(argument=="--export-packed") {
            if(i+1>=argc) {printUsage();return 2;}
            packedOutput=std::filesystem::u8path(argv[++i]);
        } else if(argument.substr(0u,16u)=="--export-packed=") {
            packedOutput=std::filesystem::u8path(std::string(argument.substr(16u)));
        }
        else if(argument.substr(0u,2u)=="--") {
            std::cerr<<"Unknown option: "<<argument<<'\n';return 2;
        } else paths.push_back(std::filesystem::u8path(argv[i]));
    }
    if (paths.size() != 1u && paths.size() != 2u) {
        printUsage();
        return 2;
    }
    try {
        std::optional<std::filesystem::path> mdx;
        if (paths.size() == 2u) mdx = paths[1];
        neomdl::ModelDocument document;
        neomdl::model::Model model;
        if(packedOutput) {
            // Use the same resource-aware construction as the editor: TXI
            // bump materials change the authored MDX channel layout.
            document.openFile(paths[0],{},{},game);
            // converttobinary spawns before Write (K2 0x72f2c9..0x72f34e).
            // NewCAurObject runs seams after Part::PostProcess (0x6ede8f).
            neomdl::model::processAsciiSkinSeams(document.model());
            model=document.sourceModel();
        } else model=neomdl::model::readModelFile(paths[0], mdx, {}, game);
        std::cout << neomdl::model::modelSummary(model) << '\n';
        for (const auto& warning : model.warnings) std::cout << "warning: " << warning << '\n';
        auto protectedPaths=packedOutput?document.openPaths():paths;
        if(asciiOutput&&!packedOutput&&!mdx) {
            auto companion=paths[0];companion.replace_extension(".mdx");
            companion=neoshared::game::findExistingPathCaseInsensitive(companion);
            if(!companion.empty()) protectedPaths.push_back(std::move(companion));
        }
        if(asciiOutput) {
            neoshared::checkResourceOutput(*asciiOutput,protectedPaths);
            protectedPaths.push_back(*asciiOutput);
        }
        std::filesystem::path packedCompanion;
        if(packedOutput) {
            neoshared::checkResourceOutput(*packedOutput,protectedPaths);
            protectedPaths.push_back(*packedOutput);
            packedCompanion=*packedOutput;packedCompanion.replace_extension(".mdx");
            neoshared::checkResourceOutput(packedCompanion,protectedPaths);
        }
        if(asciiOutput) {
            const auto exported=neomdl::model::exportAsciiModel(model,{},asciiOptions);
            const std::vector<std::uint8_t> bytes(exported.text.begin(),exported.text.end());
            neoshared::texture::writeFileBytes(*asciiOutput,bytes);
            for(const auto& warning:exported.warnings) std::cout<<"export warning: "<<warning<<'\n';
            std::cout<<"ASCII model exported to "<<neoshared::texture::pathToUtf8(*asciiOutput)<<'\n';
        }
        if(packedOutput) {
            const auto exported=neomdl::model::exportPackedModel(model);
            neoshared::texture::writeFileBytes(packedCompanion,exported.mdx);
            neoshared::texture::writeFileBytes(*packedOutput,exported.mdl);
            std::cout<<"Packed model exported to "<<neoshared::texture::pathToUtf8(*packedOutput)
                     <<" and "<<neoshared::texture::pathToUtf8(packedCompanion)<<'\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NeoMDL: " << error.what() << '\n';
        return 1;
    }
}
