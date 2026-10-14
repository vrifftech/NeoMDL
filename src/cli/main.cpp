#include <model/Model.hpp>
#include <model/AsciiExport.hpp>
#include <model/AsciiModel.hpp>
#include <model/PackedExport.hpp>
#include <core/ModelDocument.hpp>
#include <core/MeshReplacement.hpp>
#include <neoshared/ResourceDocument.hpp>
#include <neoshared/game/Installation.hpp>
#include <neoshared/texture/FileUtil.hpp>

#include <algorithm>
#include <iostream>
#include <iterator>
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
    };
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        printUsage();
        return 0;
    }
    std::optional<neomdl::model::GameVersion> game;
    std::optional<std::filesystem::path> asciiOutput;
    std::optional<std::filesystem::path> packedOutput;
    neomdl::model::AsciiExportOptions asciiOptions;
    bool includeCollisions=true,rebuildAabb=false;
    std::optional<std::filesystem::path> layout;
    neomdl::model::Vec3 placement;
    std::vector<std::string> collisionSources;
    struct NamedReplacement { std::filesystem::path donor; std::string mapping; };
    std::filesystem::path donorPath;
    std::vector<NamedReplacement> replacements;
    std::vector<std::filesystem::path> paths;
    for(int i=1;i<argc;++i) {
        const std::string_view argument(argv[i]);
        if(argument=="--game=k1")game=neomdl::model::GameVersion::Kotor1;
        else if(argument=="--game=k2")game=neomdl::model::GameVersion::Kotor2;
        else if(argument=="--geometry-only")asciiOptions.includeAnimations=false;
        else if(argument=="--model-only")includeCollisions=false;
        else if(argument=="--rebuild-aabb")rebuildAabb=true;
        else if(argument.substr(0,8)=="--donor=")donorPath=std::filesystem::u8path(std::string(argument.substr(8)));
        else if(argument.substr(0,10)=="--replace=")replacements.push_back({donorPath,std::string(argument.substr(10))});
        else if(argument.substr(0,17)=="--collision-from=")collisionSources.emplace_back(argument.substr(17));
        else if(argument.substr(0,9)=="--layout=")layout=std::filesystem::u8path(std::string(argument.substr(9)));
        else if(argument.substr(0,12)=="--placement=") {
            auto value=std::string(argument.substr(12));std::replace(value.begin(),value.end(),',',' ');
            std::istringstream(value)>>placement.x>>placement.y>>placement.z;
        }
        else if(argument=="--advanced-ascii")asciiOptions.advanced=true;
        else if(argument=="--explicit-normals")asciiOptions.explicitNormals=true;
        else if(argument=="--smoothing-groups")asciiOptions.explicitNormals=false;
        else if(argument=="--refine-tangent-rounding")asciiOptions.refineTangentRounding=true;
        else if(argument=="--refine-normal-rounding")asciiOptions.refineNormalRounding=true;
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
        const auto extension=neomdl::resourceKey(neoshared::texture::pathToUtf8(paths[0].extension()));
        if(extension==".wok" || extension==".pwk" || extension==".dwk") {
            auto kind=extension==".wok"?neomdl::CollisionKind::Room:
                extension==".pwk"?neomdl::CollisionKind::Placeable:neomdl::CollisionKind::DoorClosed;
            const auto stem=neomdl::resourceKey(neoshared::texture::pathToUtf8(paths[0].stem()));
            for(int state=1;state<=2;++state) {
                const auto suffix="_dwk"+std::to_string(state);
                if(extension==".dwk" && stem.size()>=suffix.size() &&
                   stem.compare(stem.size()-suffix.size(),suffix.size(),suffix)==0)
                    kind=static_cast<neomdl::CollisionKind>(state+1);
            }
            neoshared::ResourceDocument resource;resource.fileName=neoshared::texture::pathToUtf8(paths[0].filename());
            resource.bytes=neomdl::readResourceFile(paths[0]);resource.protectedInputs=paths;
            const auto collision=neomdl::readCollision(resource,kind);
            std::vector<neomdl::CollisionOutput> outputs;
            if(asciiOutput) {
                outputs.push_back({*asciiOutput,{collision.text.begin(),collision.text.end()}});
            }
            if(packedOutput) {
                auto mesh=neomdl::readCollisionAscii(collision.text,kind);
                neoshared::bwm::buildBwm(mesh.mesh,mesh.walkableMaterials);
                outputs.push_back({*packedOutput,neoshared::bwm::writeBwm(mesh.mesh)});
            }
            auto protectedPaths=paths;
            for(const auto& output:outputs) {
                neoshared::checkResourceOutput(output.path,protectedPaths);protectedPaths.push_back(output.path);
            }
            for(const auto& output:outputs) {
                neoshared::texture::writeFileBytes(output.path,output.bytes);
                std::cout<<"Collision exported to "<<neoshared::texture::pathToUtf8(output.path)<<'\n';
            }
            return 0;
        }
        std::optional<std::filesystem::path> mdx;
        if (paths.size() == 2u) mdx = paths[1];
        neomdl::ModelDocument document;
        neomdl::model::Model model;
        const bool documentInput=packedOutput || !replacements.empty();
        if(documentInput) {
            // Use the same resource-aware construction as the editor: TXI
            // bump materials change the authored MDX channel layout.
            document.openFile(paths[0],{},{},game,asciiOptions.includeAnimations,mdx);
            // converttobinary spawns before Write (K2 0x72f2c9..0x72f34e).
            // NewCAurObject runs seams after Part::PostProcess (0x6ede8f).
            neomdl::model::processAsciiSkinSeams(document.model());
            model=document.sourceModel();
        } else {
            model=neomdl::model::readModelFile(paths[0], mdx, {}, game, {},asciiOptions.includeAnimations);
            if(asciiOutput) neomdl::model::processAsciiSkinSeams(model);
        }
        std::optional<neomdl::model::AsciiExportResult> replacedAscii;
        std::vector<std::filesystem::path> donorInputs;
        if(!replacements.empty()) {
            std::map<std::string,std::shared_ptr<const neomdl::MeshDonor>> donors;
            std::vector<neomdl::MeshReplacement> mappings;
            for(const auto& replacement:replacements) {
                const auto key=neomdl::resourceFileIdentity(replacement.donor);
                auto found=donors.find(key);
                if(found==donors.end()) {
                    auto donor=std::make_shared<neomdl::MeshDonor>(neomdl::readMeshDonor(replacement.donor,model.game));
                    donorInputs.insert(donorInputs.end(),donor->inputs.begin(),donor->inputs.end());
                    found=donors.emplace(key,std::move(donor)).first;
                }
                const auto colon=replacement.mapping.find(':');
                mappings.push_back({replacement.mapping.substr(0,colon),replacement.mapping.substr(colon).substr(1),found->second});
            }
            std::string source;
            if(document.originalAscii() && model.advancedAscii==asciiOptions.advanced && asciiOptions.includeAnimations &&
               !asciiOptions.explicitNormals && !asciiOptions.refineTangentRounding && !asciiOptions.refineNormalRounding) {
                source=*document.originalAscii();
            } else {
                auto exported=neomdl::model::exportAsciiModel(model,{},asciiOptions);
                for(const auto& warning:exported.warnings) std::cout<<"export warning: "<<warning<<'\n';
                source=std::move(exported.text);
            }
            replacedAscii=neomdl::replaceMeshes(source,model,mappings,asciiOptions.advanced);
            for(const auto& warning:replacedAscii->warnings) std::cout<<"replacement warning: "<<warning<<'\n';
            replacedAscii->warnings.clear();
            auto edited=document.rebuildAscii(replacedAscii->text,{},game,asciiOptions.includeAnimations);
            document=std::move(*edited);
            model=document.sourceModel();
        }
        if(rebuildAabb) neomdl::model::rebuildAabbTrees(model);
        auto collisions=documentInput?document.collisions():neomdl::loadCollisions(
            neoshared::texture::pathToUtf8(paths[0].stem()),neomdl::directoryResources(std::filesystem::absolute(paths[0]).parent_path()));
        if(layout) {
            const auto bytes=neomdl::readResourceFile(*layout);
            placement=neomdl::collisionLayout(std::string(bytes.begin(),bytes.end())).at(neomdl::resourceKey(model.name));
        }
        const std::map<std::string,neomdl::CollisionKind> collisionKinds{
            {"wok",neomdl::CollisionKind::Room},{"pwk",neomdl::CollisionKind::Placeable},
            {"dwk0",neomdl::CollisionKind::DoorClosed},{"dwk1",neomdl::CollisionKind::DoorOpen1},{"dwk2",neomdl::CollisionKind::DoorOpen2}};
        std::map<std::string,std::size_t> nodes;
        for(std::size_t i=0;i<model.nodes.size();++i) if(model.nodes[i].mesh)
            nodes.emplace(neomdl::resourceKey(model.nodes[i].name),i);
        for(const auto& source:collisionSources) {
            const auto colon=source.find(':');
            const auto kind=collisionKinds.at(source.substr(0,colon));
            auto mesh=neomdl::collisionFromModel(model,nodes.at(neomdl::resourceKey(source.substr(colon+1))),kind,
                kind==neomdl::CollisionKind::Room?placement:neomdl::model::Vec3{});
            if(collisions.count(kind)) {
                const auto previous=neomdl::readCollisionAscii(collisions.at(kind).text,kind);
                mesh.mesh.usePoints=previous.mesh.usePoints;mesh.mesh.doorUsePoints=previous.mesh.doorUsePoints;
                mesh.walkableMaterials=previous.walkableMaterials;
            }
            collisions[kind].text=neomdl::writeCollisionAscii(mesh,kind);
        }
        std::cout << neomdl::model::modelSummary(model) << '\n';
        for (const auto& warning : model.warnings) std::cout << "warning: " << warning << '\n';
        auto protectedPaths=documentInput?document.openPaths():paths;
        protectedPaths.insert(protectedPaths.end(),donorInputs.begin(),donorInputs.end());
        for(const auto& [kind,collision]:collisions)
            protectedPaths.insert(protectedPaths.end(),collision.inputs.begin(),collision.inputs.end());
        if(layout) protectedPaths.push_back(*layout);
        if(!includeCollisions) collisions.clear();
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
            protectedPaths.push_back(packedCompanion);
        }
        std::vector<neomdl::CollisionOutput> companionOutputs;
        if(asciiOutput) companionOutputs=neomdl::collisionOutputs(collisions,*asciiOutput,false);
        if(packedOutput) {
            auto binary=neomdl::collisionOutputs(collisions,*packedOutput,true);
            companionOutputs.insert(companionOutputs.end(),std::make_move_iterator(binary.begin()),std::make_move_iterator(binary.end()));
        }
        for(const auto& output:companionOutputs) {
            neoshared::checkResourceOutput(output.path,protectedPaths);protectedPaths.push_back(output.path);
        }
        if(asciiOutput) {
            const auto exported=replacedAscii && !rebuildAabb?std::move(*replacedAscii):
                neomdl::model::exportAsciiModel(model,{},asciiOptions);
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
        for(const auto& output:companionOutputs) {
            neoshared::texture::writeFileBytes(output.path,output.bytes);
            std::cout<<"Collision companion exported to "<<neoshared::texture::pathToUtf8(output.path)<<'\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NeoMDL: " << error.what() << '\n';
        return 1;
    }
}
