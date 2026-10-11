#pragma once

#include <model/AsciiExport.hpp>

namespace neomdl::model::ascii_export {

class Writer {
public:
    Writer(GameVersion game, const neoshared::bif::JobControl& job);
    void line(const std::string& text = {});
    void warning(const std::string& text);
    std::string scalar(float value) const;
    std::string vector(Vec3 value) const;
    std::string quaternion(Quaternion value);
    std::string token(const std::string& value, const std::string& context) const;
    void indent(unsigned spaces) noexcept { indent_ = spaces; }
    GameVersion game() const noexcept { return game_; }
    const neoshared::bif::JobControl& job() const noexcept { return job_; }
    AsciiExportResult finish();
private:
    GameVersion game_;
    const neoshared::bif::JobControl& job_;
    unsigned indent_{};
    AsciiExportResult result_;
};

struct ReconstructedMesh {
    std::vector<std::size_t> sourceVertices;
    std::vector<std::array<std::uint32_t,3>> sourceFaces;
};

void writeControllers(Writer&, const Node&, std::uint16_t fieldFlags);
void writeNodeFields(Writer&, const Model&, const Node&);
ReconstructedMesh writeMesh(Writer&, const Model&, const Node&, const Mesh&);
void writeSkin(Writer&, const Model&, const Mesh&, const ReconstructedMesh&);
void writeLightsaber(Writer&, const Node&);

} // namespace neomdl::model::ascii_export
