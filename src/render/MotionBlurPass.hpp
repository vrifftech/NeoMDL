#pragma once
#include "core/MotionBlurScene.hpp"
#include "render/ShaderMeshRenderer.hpp"

namespace neomdl::render {
// The scene's DoBlurBuckets list holds Gobs, not a skin-only mesh queue.
// Captured submissions keep document/pose references borrowed for THIS frame.
class MotionBlurQueue final {
public:
    void clear() noexcept { batches_.clear(); }
    bool record(MotionBlurScene&,const std::vector<MeshSubmission>&,
                const std::vector<std::size_t>& completedIndices,std::string& error);
    const std::vector<MeshSubmission>& batch(std::size_t ordinal) const;
private:
    std::vector<std::vector<MeshSubmission>> batches_;
};
// PartTriMesh::Draw and PartAnimMesh::Draw ignore the blur Boolean. Only
// PartSkin takes RenderSkinnedVPBlur.
MeshSubmission engineBlurSubmission(const MeshSubmission&,const MotionBlurDraw&,
                                    float sceneElapsed);
using BlurOwnerPreparation = std::function<bool(const MotionBlurDraw&,std::string&)>;
bool drawMotionBlurOwners(ShaderMeshRenderer&,MotionBlurScene&,const MotionBlurQueue&,
    const SubmissionPreparation&,const BlurOwnerPreparation&,std::string& error,
    float sceneElapsed);
} // namespace neomdl::render
