#pragma once

#include <model/AsciiExport.hpp>
#include <model/AsciiExportSmoothing.hpp>

#include <map>

namespace neomdl::model::ascii_export {

struct ReconstructedMesh {
    std::vector<std::size_t> sourceVertices;
    std::vector<std::array<std::uint32_t,3>> sourceFaces;
};

struct MeshReconstruction {
    std::vector<Vertex> vertices;
    ReconstructedMesh topology;
    ReconstructedSmoothing smoothing;
    std::size_t changedAdjacency{};
    bool hasNormals{};
    bool explicitNormals{};
    std::vector<std::uint8_t> cornerRotations;
    std::size_t sourceCorner(std::size_t face,std::size_t corner) const {
        return (corner+cornerRotations[face])%3u;
    }
};

class Writer {
public:
    Writer(GameVersion game, const neoshared::bif::JobControl& job, const AsciiExportOptions& options = {});
    void line(const std::string& text = {});
    void warning(const std::string& text);
    std::string scalar(float value) const;
    std::string vector(Vec3 value) const;
    std::string quaternion(Quaternion value);
    std::string token(const std::string& value, const std::string& context) const;
    void indent(unsigned spaces) noexcept { indent_ = spaces; }
    GameVersion game() const noexcept { return game_; }
    const neoshared::bif::JobControl& job() const noexcept { return job_; }
    bool explicitNormals(const Mesh& mesh) const noexcept {
        return options_.explicitNormals.value_or(mesh.explicitNormals);
    }
    void explicitNormalNote();
    AsciiExportResult finish();
    std::map<const Mesh*,MeshReconstruction> meshReconstructions;
private:
    GameVersion game_;
    const neoshared::bif::JobControl& job_;
    AsciiExportOptions options_;
    unsigned indent_{};
    AsciiExportResult result_;
};

void reconstructSkinSeamSmoothing(Writer&, const Model&);
void reconstructExportedNodeFrames(Model&, const neoshared::bif::JobControl&);

void writeControllers(Writer&, const Node&, std::uint16_t fieldFlags);
void writeNodeFields(Writer&, const Model&, const Node&);
ReconstructedMesh writeMesh(Writer&, const Model&, const Node&, const Mesh&);
void writeSkin(Writer&, const Model&, const Mesh&, const ReconstructedMesh&);
void writeLightsaber(Writer&, const Node&, bool explicitNormals);

} // namespace neomdl::model::ascii_export
