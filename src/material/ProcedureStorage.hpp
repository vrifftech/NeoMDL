#pragma once
#include <material/ProceduralTexture.hpp>
#include <limits>

namespace neomdl::material {
// Engine upload properties are independent. In particular, a processed TPC
// resource may carry raw or block-compressed storage, and either 2D or cube
// targets, without changing the controller type parsed from TXI.
enum class ProcedureStorageProvenance { DecodedPixels, ProcessedPayload };
enum class ProcedureSourceEncoding { Gray8, Gray16, Rgb8, Rgba8, PackedRgba4444 };
enum class ProcedureCompressionCodec { None, Bc1, Bc3 };
enum class ProcedureTextureTarget { Texture2D, CubeMap, Volume3D };
enum class ProcedureMipOwnership { SourceProvided, RuntimeGenerated };
enum class ProcedureUploadAlgorithm {
    DirectSourceLevels,
    DirectPixels,
    HeightToNormal,
    SgiAutomatic,
    PortableGluByte,
    AurImageScale,
    Volume3D
};
struct ProcedureStorageDescriptor {
    ProcedureStorageProvenance provenance{ProcedureStorageProvenance::DecodedPixels};
    ProcedureSourceEncoding encoding{ProcedureSourceEncoding::Rgba8};
    ProcedureCompressionCodec compression{ProcedureCompressionCodec::None};
    ProcedureTextureTarget target{ProcedureTextureTarget::Texture2D};
    ProcedureMipOwnership mipOwnership{ProcedureMipOwnership::RuntimeGenerated};
    ProcedureUploadAlgorithm initialUpload{ProcedureUploadAlgorithm::DirectPixels};
    ProcedureUploadAlgorithm updateUpload{ProcedureUploadAlgorithm::DirectPixels};
    unsigned textureIdCount{1};
    unsigned faceCount{1};
};
ProcedureStorageDescriptor procedureStorageDescriptor(
    const neoshared::texture::TextureData&,const ProcedureDefinition&,bool cube);

// Recover the full controller image, not one TID or one cubemap face. Shared
// images stay immutable and in engine row order. Uncompressed processed storage
// supplies its exact bytes; block-compressed storage has no writable controller
// pixel span and remains a direct/source-level path.
ProcedureImage procedureSourceImage(const neoshared::texture::TextureData&,
                                   const ProcedureDefinition&,bool cube);

enum class ProcedureCubeLayout { OrdinaryMapped, ProcessedDirect };
// TID-major; each entry is either one 2D image or six GL-target-order faces.
// OrdinaryMapped reproduces CreateCubeTexture's crop/rotation table. The
// processed cube caller stores and uploads its six faces directly.
std::vector<std::vector<ProcedureImage>> procedureTidImages(
    const ProcedureImage&,unsigned numX,unsigned numY,bool cube,
    ProcedureCubeLayout=ProcedureCubeLayout::OrdinaryMapped);

enum class ProcedureMipKernel { AurUpdate, GluByte };
// Explicit legacy upload kernels. GluByte uses the bundled GLU UBYTE scaler,
// including nearest-power rescaling. AurUpdate first applies the
// rectangular ImageScale-to-square branch, then the Aur mip kernel;
// packed-4444 retains the engine nibble averaging.
std::vector<ProcedureImage> procedureMipLevels(const ProcedureImage&,ProcedureMipKernel,
    unsigned maxTextureSize=std::numeric_limits<unsigned>::max());
} // namespace neomdl::material
