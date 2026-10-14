#include <core/Conversion.hpp>
#include <neoshared/PathUtf8.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string_view>

int main(int argc, char** argv) {
    const auto printUsage = [] {
        std::cout << "Usage: neomdl-info [--game=k1|--game=k2] [--geometry-only] [--export-ascii output.mdl] [--advanced-ascii] [--explicit-normals|--smoothing-groups] [--refine-tangent-rounding] [--refine-normal-rounding] [--export-packed output.mdl] <model.mdl> [model.mdx]\n"
                     "ASCII models use the shared x86-layout builder and require an explicit target game.\n"
                     "ASCII export writes one model asset and its own animations to a separate file.\n"
                     "Standard ASCII is the default. --advanced-ascii exports stored-data preservation fields.\n"
                     "--geometry-only omits the model's own animations; binary input skips animation decoding.\n"
                     "--explicit-normals preserves normals in a NeoMDL ASCII extension.\n"
                     "--smoothing-groups reconstructs smoothing groups instead of explicit mesh normals.\n"
                     "Without either option, export preserves the mesh's input mode; compiled meshes use smoothing groups.\n"
                     "--refine-tangent-rounding allows smoothing export to move selected source coordinates by one float step to improve tangents.\n"
                     "--refine-normal-rounding allows the same position adjustment to improve normals on overlapping opposite-wound triangles.\n"
                     "Packed export compiles ASCII input to an x86 MDL and same-name MDX pair.\n";
        std::cout << "PWK, _dwk0/1/2.DWK and WOK companions load and export with the model; --model-only omits them.\n"
                     "--collision-from=wok|pwk|dwk0|dwk1|dwk2:NODE creates/replaces a companion from a model mesh.\n"
                     "--layout=area.lyt or --placement=X,Y,Z supplies room placement for --collision-from=wok:NODE.\n"
                     "--rebuild-aabb regenerates every MDL AABB tree from its collision faces.\n"
                     "A standalone .wok, .pwk or .dwk input also supports --export-ascii and --export-packed.\n";
        std::cout << "--donor=donor.mdl --replace=TARGET:DONOR_MESH replaces a uniquely named mesh of the same type.\n"
                     "Repeat --replace to queue mappings; --donor selects the file for subsequent mappings.\n"
                     "The last mapping for a target wins. Names are case-insensitive. Donor animations are not loaded.\n"
                     "Geometry, normals, UVs, materials and skin weights transfer; target transforms and controllers stay.\n"
                     "Skin bones match by name in the target skeleton; texture files are not copied.\n";
        std::cout << "--convert=ascii|binary|auto [--output-dir=DIR] FILE... converts one or more files.\n"
                     "auto detects the input format and writes its opposite. Outputs default beside each input.\n"
                     "--ascii-names=dash|dot|suffix selects name-ascii.mdl, name.ascii.mdl or name.mdl.ascii.\n"
                     "Every conversion saves OUTPUT.report.txt with timing, options, warnings and errors.\n"
                     "Batch files run in order; failed conversions are reported and remaining inputs continue.\n";
    };
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        printUsage();
        return 0;
    }
    neomdl::ConversionOptions options;
    std::optional<std::filesystem::path> asciiOutput, packedOutput;
    std::optional<neomdl::ConversionMode> mode;
    neomdl::AsciiNaming naming = neomdl::AsciiNaming::Dash;
    std::filesystem::path directory, donorPath;
    std::vector<std::filesystem::path> paths;
    try {
        for(int i=1;i<argc;++i) {
            const std::string_view argument(argv[i]);
            if(argument.substr(0,10)=="--convert=") {
                const std::map<std::string,neomdl::ConversionMode> modes{{"ascii",neomdl::ConversionMode::Ascii},
                    {"binary",neomdl::ConversionMode::Binary},{"auto",neomdl::ConversionMode::Opposite}};
                mode=modes.at(std::string(argument.substr(10)));
            }
            else if(argument.substr(0,13)=="--output-dir=")directory=std::filesystem::u8path(std::string(argument.substr(13)));
            else if(argument.substr(0,14)=="--ascii-names=") {
                const std::map<std::string,neomdl::AsciiNaming> names{{"dash",neomdl::AsciiNaming::Dash},
                    {"dot",neomdl::AsciiNaming::Dot},{"suffix",neomdl::AsciiNaming::Suffix}};
                naming=names.at(std::string(argument.substr(14)));
            }
            else if(argument=="--game=k1")options.game=neomdl::model::GameVersion::Kotor1;
            else if(argument=="--game=k2")options.game=neomdl::model::GameVersion::Kotor2;
            else if(argument=="--geometry-only")options.ascii.includeAnimations=false;
            else if(argument=="--model-only")options.includeCollisions=false;
            else if(argument=="--rebuild-aabb")options.rebuildAabb=true;
            else if(argument.substr(0,8)=="--donor=")donorPath=std::filesystem::u8path(std::string(argument.substr(8)));
            else if(argument.substr(0,10)=="--replace=")options.replacements.push_back({donorPath,std::string(argument.substr(10))});
            else if(argument.substr(0,17)=="--collision-from=")options.collisionSources.emplace_back(argument.substr(17));
            else if(argument.substr(0,9)=="--layout=")options.layout=std::filesystem::u8path(std::string(argument.substr(9)));
            else if(argument.substr(0,12)=="--placement=") {
                auto value=std::string(argument.substr(12));std::replace(value.begin(),value.end(),',',' ');
                std::istringstream(value)>>options.placement.x>>options.placement.y>>options.placement.z;
            }
            else if(argument=="--advanced-ascii")options.ascii.advanced=true;
            else if(argument=="--explicit-normals")options.ascii.explicitNormals=true;
            else if(argument=="--smoothing-groups")options.ascii.explicitNormals=false;
            else if(argument=="--refine-tangent-rounding")options.ascii.refineTangentRounding=true;
            else if(argument=="--refine-normal-rounding")options.ascii.refineNormalRounding=true;
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
        if (paths.empty() || (!mode && paths.size() > 2)) {
            printUsage(); return 2;
        }
        const auto modelInput=[](const std::filesystem::path& path) {
            neomdl::ConversionRequest request;request.input=path;
            if(neomdl::resourceKey(neoshared::pathToUtf8(path.extension()))==".mdx") {
                request.mdx=path;
                request.input=neomdl::uniqueResourceFile(std::filesystem::absolute(path).parent_path(),
                    neomdl::modelFileStem(path)+".mdl").value();
            }
            return request;
        };
        std::vector<neomdl::ConversionRequest> requests;
        if (mode) {
            for (const auto& path : paths) {
                const auto input=modelInput(path);
                auto request=neomdl::planConversion(input.input,*mode,directory,naming);request.mdx=input.mdx;
                if(asciiOutput)request.asciiOutput=asciiOutput;
                if(packedOutput)request.packedOutput=packedOutput;
                requests.push_back(std::move(request));
            }
            if (!directory.empty()) std::filesystem::create_directories(directory);
        } else {
            auto request=modelInput(paths[0]);
            if(paths.size()==2)request.mdx=paths[1];
            request.asciiOutput=asciiOutput; request.packedOutput=packedOutput;
            requests.push_back(std::move(request));
        }
        std::vector<std::filesystem::path> inputs;
        for (const auto& request : requests) {
            auto files=neomdl::conversionInputs(request); inputs.insert(inputs.end(),files.begin(),files.end());
        }
        for (const auto& replacement:options.replacements) {
            neomdl::ConversionRequest donor; donor.input=replacement.donor;
            auto files=neomdl::conversionInputs(donor); inputs.insert(inputs.end(),files.begin(),files.end());
        }
        if(options.layout)inputs.push_back(*options.layout);
        auto destinations=inputs;
        for(const auto& request:requests)
            for(const auto& output:neomdl::conversionOutputs(request,options)) {
                neoshared::checkResourceOutput(output,destinations); destinations.push_back(output);
            }
        int failures=0;
        for(const auto& request:requests) {
            const auto result=neomdl::convertFile(request,options,inputs);
            std::cout<<result.source<<'\n'<<result.summary<<'\n';
            for(const auto& warning:result.warnings)std::cout<<"warning: "<<warning<<'\n';
            for(const auto& output:result.outputs)std::cout<<"Exported: "<<neoshared::pathToUtf8(output)<<'\n';
            if(!result.error.empty()) {std::cerr<<"NeoMDL: "<<result.error<<'\n';++failures;}
            if(request.asciiOutput||request.packedOutput)
                std::cout<<"Report: "<<neoshared::pathToUtf8(neomdl::conversionReportPath(request))<<'\n';
        }
        return failures?1:0;
    } catch(const std::exception& error) {
        std::cerr<<"NeoMDL: "<<error.what()<<'\n'; return 1;
    }
}
