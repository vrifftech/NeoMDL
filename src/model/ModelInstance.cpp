#include <model/Model.hpp>

#include <functional>

namespace neomdl::model {

Model instantiateModelParts(std::shared_ptr<const Model> sourceModel,
                             const neoshared::bif::JobControl& job) {
    job.check();
    const auto& source=*sourceModel;
    Model result=source;
    result.partSourceModel=sourceModel;
    result.nodes.clear();
    result.meshes.clear();
    result.partSourceNodes.clear();
    result.partSourceMeshes.clear();
    result.animationRootNode.reset();






    std::function<std::size_t(std::size_t,std::optional<std::size_t>)> create;
    create=[&](std::size_t sourceIndex,std::optional<std::size_t> parent) {
        job.check();
        const auto partIndex=result.nodes.size();
        result.nodes.push_back(source.nodes[sourceIndex]);
        result.partSourceNodes.push_back(sourceIndex);
        result.nodes[partIndex].parent=parent;
        result.nodes[partIndex].children.clear();
        if(source.animationRootNode==sourceIndex && !result.animationRootNode)
            result.animationRootNode=partIndex;

        if(const auto sourceMesh=source.nodes[sourceIndex].mesh) {
            const auto meshIndex=result.meshes.size();
            result.nodes[partIndex].mesh=meshIndex;
            result.meshes.push_back(source.meshes[*sourceMesh]);
            result.partSourceMeshes.push_back(*sourceMesh);
            auto& mesh=result.meshes.back();
            mesh.nodeIndex=partIndex;
            if(mesh.asciiSkinSource) {
                // GatherBones appends one inverse frame for every live Part
                // visit; skin palette entries index that DFS list, not node
                // IDs or the canonical node table. ASCII construction keeps
                // the engine pools separately from its inspection remapping.
                // Copied meshes retain shared model-owned normal and UV pools,
                // while Pose allocates deformation state per mesh occurrence.
                const auto& engine=*mesh.asciiSkinSource;
                mesh.inverseBindOrientations=engine.inverseOrientations;
                mesh.inverseBindTranslations=engine.inverseTranslations;
                mesh.skinBoneMap=engine.paletteToBone;
            }
        }
        for(const auto child:source.nodes[sourceIndex].children) {
            const auto childPart=create(child,partIndex);
            result.nodes[partIndex].children.push_back(childPart);
        }
        return partIndex;
    };
    for(std::size_t sourceIndex=0;sourceIndex<source.nodes.size();++sourceIndex)
        if(!source.nodes[sourceIndex].parent) create(sourceIndex,std::nullopt);

    const auto pose=constructedBindPose(result,job);
    result.worldTransforms=pose.worldTransforms;
    result.bounds=pose.bounds;
    return result;
}

} // namespace neomdl::model
