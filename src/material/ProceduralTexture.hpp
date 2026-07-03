#pragma once
#include <material/Material.hpp>
#include <material/Runtime.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace neomdl::material {



struct ProcedureDefinition {
    EngineMaterialGame game{EngineMaterialGame::Kotor2};
    std::optional<ProcedureType> type;
    int defaultWidth{2}, defaultHeight{2}, numX{1}, numY{1};
    bool distort{}, distortAngle{true};
    // Texture-owned flags survive replacement of the controller object.
    bool volumeTexture{}, bumpTexture{};
    float distortionAmplitude{5.0f}, speed{1.0f}, fps{1.0f};
    int waterWidth{31}, waterHeight{31}, arturoWidth{15}, arturoHeight{15};
    float forceCycleSpeed{57.2957763671875f}, angleCycleSpeed{2.864788770675659f};
    std::vector<float> channelScale, channelTranslate;
};
ProcedureDefinition procedureDefinition(const TxiMetadata&,
    EngineMaterialGame = EngineMaterialGame::Kotor2);

struct DirtyTextureInputs {
    // DirtyTextureController::Init(int,int,int,int). An area host supplies the
    // selected area color/size/parameter/algorithm tuple. No area: 0,16,0,0.
    std::uint32_t color{};
    int size{16}, parameter{}, algorithm{};
};
struct ProcedureContext {
    // AurTextureMakeCurrent's wrapping 32-bit counter, shared by all textures.
    std::int32_t token{};
    float elapsed{};
    bool enabled{true}, randomImpulses{true}, filterWater{true};
    bool lowQualityWater{}, integerWaterFilter{true};
    float ringFrequency{60.0f}, ringPhaseScale{25.0f}, ringAmplitude{0.03f}, ringSpeed{1.0f};
    std::array<DirtyTextureInputs,3> dirty;
    // The engine uses a process RNG, NOT one stream per texture. K1 Desktop loads
    // rand (0x596cfc); K2 Desktop calls rand_wincompatible (0x4a8151). A supplied
    // source returns the engine result without masking. The seeded host stream
    // below reproduces K2's generator, not the unavailable K1 libc load.
    std::function<std::uint32_t()> random;
    std::uint32_t randomState{1u};
    // Arturo's function-static variables are shared even across distinct TIDs.
    float arturoElapsed{};
    int arturoPhase{-1};
    std::uint32_t nextRandom();
};
struct ProcedureImage {
    std::uint32_t width{}, height{}, depth{1};
    unsigned bytesPerPixel{4};
    // Engine row order, not the shared editor's top-left image orientation.
    std::vector<std::uint8_t> bytes;
};
enum class ProcedureChange { None, Image, Frame, InvalidTexture };

class ProceduralTexture final {
public:
    // Source is the post-loader, pre-controller image. Initial GL upload occurs
    // BEFORE Init for ordinary textures. Dirty Init itself replaces the image.
    bool initialize(const ProcedureDefinition&, const ProcedureImage& source,
                    ProcedureContext&, std::string& error);
    ProcedureChange control(ProcedureContext&, std::string& error);
    const ProcedureDefinition& definition() const noexcept { return definition_; }
    const ProcedureImage& image() const noexcept { return image_; }
    const std::vector<std::uint8_t>& heightImage() const noexcept { return height_; }
    std::uint32_t frame() const noexcept { return frame_; }
    std::uint64_t revision() const noexcept { return revision_; }
    const std::optional<DirtyTextureInputs>& initializedDirtyInputs() const noexcept { return dirtyInputs_; }
    bool initialized() const noexcept { return initialized_; }
    bool validTexture() const noexcept { return validTexture_; }
private:
    std::optional<DirtyTextureInputs> dirtyInputs_;
    ProcedureDefinition definition_;
    ProcedureImage image_, snapshot_;
    std::vector<std::uint8_t> height_, from_, target_, life_, rowsWritten_;
    std::vector<float> current_, previous_, scratch_;
    std::int32_t lastToken_{};
    std::uint32_t frame_{};
    std::uint64_t revision_{};
    float phase_{}, forceAngle_{}, waveAngle_{};
    unsigned waterCounter_{};
    int waterPhase_{2};
    bool initialized_{}, validTexture_{true};
    std::int32_t integerValue(float) const noexcept;
    std::uint8_t byteValue(float) const noexcept;
    float cosine(float) const noexcept;
    float sine(float) const noexcept;
    void heightToImage();
    void distortImage();
    void buildPerlin(std::vector<std::uint8_t>&, ProcedureContext&, int maximum = -1);
    void buildArturo(ProcedureContext&);
    void wave(ProcedureContext&);
    bool water(ProcedureContext&);
    bool arturo(ProcedureContext&, std::string&);
    void life();
    void ring(ProcedureContext&);
    void dirty(ProcedureContext&);
};

// Engine height-to-normal upload, including its own alpha-weighted mip chain.
// Returned bytes are BGRA, as passed to glTexImage2D in both reference builds.
// Height fields are byte-addressed, NOT an invented extraction of red from RGBA.
std::vector<ProcedureImage> procedureNormalLevels(const ProcedureImage&,float scale,
                                                bool authoredNormal = false);

// Engine ConstructImage: missing image + constructed controller -> RGBA 0x7f.
// Returns a decoded-style source for the existing resource pipeline. It does
// not run Init, consume randomness, or attach a renderer to neoshared.
neoshared::texture::TextureData constructProcedureSource(const std::string& txi,
    const std::string& resref, EngineMaterialGame = EngineMaterialGame::Kotor2);
} // namespace neomdl::material
