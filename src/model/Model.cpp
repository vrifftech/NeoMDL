#include <model/Lighting.hpp>
#include <model/Emitter.hpp>
#include "model/Model.hpp"
#include "model/AsciiModel.hpp"
#include <model/AnimationRuntime.hpp>
#include <model/AnimMesh.hpp>
#include <model/UvAnimation.hpp>
#include <model/EngineCpuSkin.hpp>
#include <model/EnginePrelighting.hpp>

#include "neoshared/PathUtf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace neomdl::model {
namespace {

constexpr std::uint32_t kK1ModelToken0 = 4273776u;
constexpr std::uint32_t kK2ModelToken0 = 4285200u;
constexpr std::uint32_t kK1ModelToken1 = 4216096u;
constexpr std::uint32_t kK2ModelToken1 = 4216320u;
constexpr std::size_t kWrapperSize = 12u;
constexpr std::size_t kModelHeaderSize = 196u;
constexpr std::size_t kAnimationHeaderSize = 136u;
constexpr std::size_t kAnimationEventSize = 36u;
constexpr std::size_t kNodeHeaderSize = 80u;
constexpr std::size_t kControllerSize = 16u;
constexpr std::size_t kK1MeshHeaderSize = 332u;
constexpr std::size_t kK2MeshHeaderSize = 340u;
constexpr std::size_t kFaceSize = 32u;
constexpr std::size_t kAabbNodeSize = 40u;
constexpr std::size_t kDanglyHeaderSize = 28u;
constexpr std::size_t kReferenceHeaderSize = 36u;
constexpr std::size_t kLightHeaderSize = 92u;
constexpr std::size_t kEmitterHeaderSize = 224u;

// MdlNodeSkin32 fields are relative to the triangle-mesh extension. K1's
// extension is eight bytes shorter: ResetSkin 0x321698 and both Draw consumers
// use these same channels/arrays at K2 offsets minus eight.
constexpr std::size_t kK2SkinWeightMdxOffset = 352u;
constexpr std::size_t kK2SkinBoneMdxOffset = 356u;
constexpr std::size_t kK2SkinInverseQuaternionArray = 368u;
constexpr std::size_t kK2SkinInverseTranslationArray = 380u;
constexpr std::size_t kK2SkinBoneConstantsArray = 392u;
constexpr std::size_t kK2SkinBoneMapOffset = 404u;
constexpr std::size_t kArrayDefinitionSize = 12u;

constexpr std::uint32_t kMdxVertex = 0x00000001u;
constexpr std::uint32_t kMdxTex0 = 0x00000002u;
constexpr std::uint32_t kMdxTex1 = 0x00000004u;
constexpr std::uint32_t kMdxNormal = 0x00000020u;

bool isModelToken(std::uint32_t token) noexcept {
    return token == kK1ModelToken0 || token == kK2ModelToken0;
}

std::string sourcePrefix(const std::string& label) {
    return label.empty() ? std::string{} : label + ": ";
}

class ByteView final {
public:
    ByteView(const std::uint8_t* data, std::size_t size, std::string label,
             const neoshared::bif::JobControl& job)
        : data_(data), size_(size), label_(std::move(label)), job_(job) {}

    std::size_t size() const noexcept { return size_; }
    bool contains(std::size_t offset, std::size_t length) const noexcept {
        return offset <= size_ && length <= size_ - offset;
    }

    void require(std::size_t offset, std::size_t length, const char* what) const {
        if ((reads_++ & 1023u) == 0u) job_.check();
        if (!contains(offset, length)) {
            std::ostringstream text;
            text << sourcePrefix(label_) << what << " is outside the bounded input (offset "
                 << offset << ", size " << length << ", input " << size_ << ")";
            throw ModelError(text.str());
        }
    }

    std::uint8_t u8(std::size_t offset, const char* what = "byte") const {
        require(offset, 1u, what);
        return data_[offset];
    }

    std::uint16_t u16(std::size_t offset, const char* what = "uint16") const {
        require(offset, 2u, what);
        return static_cast<std::uint16_t>(data_[offset]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset + 1u]) << 8u);
    }

    std::int16_t i16(std::size_t offset, const char* what = "int16") const {
        return static_cast<std::int16_t>(u16(offset, what));
    }

    std::uint32_t u32(std::size_t offset, const char* what = "uint32") const {
        require(offset, 4u, what);
        return static_cast<std::uint32_t>(data_[offset]) |
               (static_cast<std::uint32_t>(data_[offset + 1u]) << 8u) |
               (static_cast<std::uint32_t>(data_[offset + 2u]) << 16u) |
               (static_cast<std::uint32_t>(data_[offset + 3u]) << 24u);
    }

    std::int32_t i32(std::size_t offset, const char* what = "int32") const {
        return static_cast<std::int32_t>(u32(offset, what));
    }

    float f32(std::size_t offset, const char* what = "float") const {
        const auto bits = u32(offset, what);
        float result{};
        static_assert(sizeof(result) == sizeof(bits), "32-bit float required");
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    std::uint32_t f32Bits(std::size_t offset, const char* what = "float word") const {
        return u32(offset, what);
    }

    Vec2 vec2(std::size_t offset, const char* what) const {
        require(offset, 8u, what);
        return {f32(offset, what), f32(offset + 4u, what)};
    }

    Vec3 vec3(std::size_t offset, const char* what) const {
        require(offset, 12u, what);
        return {f32(offset, what), f32(offset + 4u, what), f32(offset + 8u, what)};
    }

    std::string fixedString(std::size_t offset, std::size_t length, const char* what) const {
        require(offset, length, what);
        std::size_t count = 0u;
        while (count < length && data_[offset + count] != 0u) ++count;
        // Engine name/resource buffers are byte strings. Display filtering
        // here would change lookup identity before any consumer sees it.
        return std::string(reinterpret_cast<const char*>(data_ + offset), count);
    }

    std::string cString(std::size_t offset, std::size_t maximum, const char* what) const {
        require(offset, 1u, what);
        const auto available = std::min(maximum, size_ - offset);
        std::size_t count = 0u;
        while (count < available && data_[offset + count] != 0u) ++count;
        if (count == available) {
            throw ModelError(sourcePrefix(label_) + std::string(what) + " is not null-terminated within its limit");
        }
        return std::string(reinterpret_cast<const char*>(data_ + offset), count);
    }

private:
    const std::uint8_t* data_{};
    std::size_t size_{};
    std::string label_;
    const neoshared::bif::JobControl& job_;
    mutable std::size_t reads_{};
};

bool finite(float value) noexcept { return std::isfinite(value); }

float wordAsFloat(std::uint32_t word) noexcept {
    float result{};
    static_assert(sizeof(result) == sizeof(word), "32-bit float required");
    std::memcpy(&result, &word, sizeof(result));
    return result;
}

Vec3 safeVec3(Vec3 value, std::vector<std::string>& warnings, const std::string& description) {
    if (finite(value.x) && finite(value.y) && finite(value.z)) return value;
    warnings.push_back(description + " contains a non-finite value; zero was substituted.");
    if (!finite(value.x)) value.x = 0.0f;
    if (!finite(value.y)) value.y = 0.0f;
    if (!finite(value.z)) value.z = 0.0f;
    return value;
}

Vec3 subtract(const Vec3& left, const Vec3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 add(const Vec3& left, const Vec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 cross(const Vec3& left, const Vec3& right) noexcept {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

Vec3 normalize(const Vec3& value) noexcept {
    const double lengthSquared = static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z;
    if (lengthSquared < 1.0e-24 || !std::isfinite(lengthSquared)) return {0.0f, 0.0f, 1.0f};
    const auto reciprocal = static_cast<float>(1.0 / std::sqrt(lengthSquared));
    return {value.x * reciprocal, value.y * reciprocal, value.z * reciprocal};
}

std::string normalizedResourceName(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    std::size_t first = 0u;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
    if (first != 0u) value.erase(0u, first);
    std::string folded = value;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    if (folded == "null" || folded == "none") return {};
    return value;
}

std::vector<std::uint8_t> readBoundedFile(const std::filesystem::path& path, std::size_t maximum,
                                       const neoshared::bif::JobControl& job) {
    job.check();
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw ModelError("Unable to inspect " + neoshared::pathToUtf8(path) + ": " + error.message());
    if (size > maximum) {
        throw ModelError("Refusing to read a model file larger than the configured limit: " +
                         neoshared::pathToUtf8(path));
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw ModelError("Model file is too large for this process: " + neoshared::pathToUtf8(path));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw ModelError("Unable to open model file: " + neoshared::pathToUtf8(path));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    for (std::size_t offset = 0; offset < bytes.size();) {
        job.update(offset, bytes.size(), "Reading " + neoshared::pathToUtf8(path.filename()));
        const auto count = std::min<std::size_t>(1024u * 1024u, bytes.size() - offset);
        input.read(reinterpret_cast<char*>(bytes.data() + offset), static_cast<std::streamsize>(count));
        if (static_cast<std::size_t>(input.gcount()) != count) {
            throw ModelError("Model file changed or could not be read completely: " + neoshared::pathToUtf8(path));
        }
        offset += count;
    }
    job.update(bytes.size(), bytes.size(), "Read " + neoshared::pathToUtf8(path.filename()));
    return bytes;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    return value;
}

std::optional<std::filesystem::path> caseInsensitiveSibling(
    const std::filesystem::path& source, const std::string& extension,
    const neoshared::bif::JobControl& job) {
    job.check();
    auto exact = source;
    exact.replace_extension(extension);
    std::error_code error;
    if (std::filesystem::is_regular_file(exact, error) && !error) return exact;
    error.clear();
    const auto directory = source.parent_path().empty() ? std::filesystem::path{"."} : source.parent_path();
    const auto desired = lowercase(neoshared::pathToUtf8(source.stem()) + extension);
    std::optional<std::filesystem::path> match;
    for (std::filesystem::directory_iterator iterator(directory, error), end; !error && iterator != end;
         iterator.increment(error)) {
        job.check();
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        if (lowercase(neoshared::pathToUtf8(iterator->path().filename())) != desired) continue;
        if (match) return std::nullopt; // Ambiguous on a case-sensitive host.
        match = iterator->path();
    }
    return match;
}

class Parser final {
public:
    Parser(const std::vector<std::uint8_t>& mdlBytes,
           const std::vector<std::uint8_t>& mdxBytes,
           std::string sourceLabel,
           const ParseLimits& limits, const neoshared::bif::JobControl& job)
        : mdlBytes_(mdlBytes), mdxBytes_(mdxBytes), label_(std::move(sourceLabel)), limits_(limits),
          job_(job), whole_(mdlBytes_.data(), mdlBytes_.size(), label_, job_) {}

    Model parse() {
        job_.update(0u, 5u, "Reading model header: " + label_);
        if (mdlBytes_.size() > limits_.maximumMdlBytes) {
            throw ModelError(sourcePrefix(label_) + "MDL exceeds the configured input limit");
        }
        if (mdxBytes_.size() > limits_.maximumMdxBytes) {
            throw ModelError(sourcePrefix(label_) + "MDX exceeds the configured input limit");
        }
        if (mdlBytes_.size() < 4u) throw ModelError(sourcePrefix(label_) + "MDL is truncated");

        std::size_t payloadOffset = 0u;
        if (mdlBytes_.size() >= kWrapperSize + 4u && isModelToken(whole_.u32(kWrapperSize))) {
            payloadOffset = kWrapperSize;
            const auto declaredMdl = whole_.u32(4u, "declared MDL size");
            const auto declaredMdx = whole_.u32(8u, "declared MDX size");
            if (declaredMdl != 0u && declaredMdl > mdlBytes_.size() - kWrapperSize) {
                throw ModelError(sourcePrefix(label_) + "MDL wrapper declares more payload bytes than are present");
            }
            if (declaredMdl != 0u && declaredMdl != mdlBytes_.size() - kWrapperSize) {
                model_.warnings.push_back("The MDL wrapper size differs from the supplied payload length.");
            }
            if (declaredMdx != 0u && mdxBytes_.empty()) {
                model_.warnings.push_back("The MDL declares a companion MDX payload, but no MDX resource was supplied.");
            } else if (declaredMdx != 0u && declaredMdx != mdxBytes_.size()) {
                model_.warnings.push_back("The MDL wrapper's MDX size differs from the supplied companion length.");
            }
        } else if (!isModelToken(whole_.u32(0u))) {
            throw ModelError(sourcePrefix(label_) + "Unrecognized packed MDL layout token or ASCII MDL input");
        }

        payloadOffset_=payloadOffset;
        payload_ = std::make_unique<ByteView>(mdlBytes_.data() + payloadOffset,
                                              mdlBytes_.size() - payloadOffset, label_, job_);
        parseHeader();
        loadNames();
        if (rootNodeOffset_ == 0u || rootNodeOffset_ == 0xFFFFFFFFu) {
            throw ModelError(sourcePrefix(label_) + "MDL has no valid root node offset");
        }
        job_.update(1u, 5u, "Decoding model geometry: " + label_);
        parseNode(rootNodeOffset_, std::nullopt, 0u);
        // Reset preserves the distinct model animation-root pointer, including
        // null. AnimateHierarchy enables controllers only after visiting that
        // source node; it does not substitute the geometry root during load.
        const auto animationRoot = nodeByOffset_.find(
            payload_->u32(168u, "model animation root offset"));
        if (animationRoot != nodeByOffset_.end())
            model_.animationRootNode = animationRoot->second;
        if (declaredNodeCount_ != 0u && model_.nodes.size() != declaredNodeCount_) {
            model_.warnings.push_back("The reachable node count (" + std::to_string(model_.nodes.size()) +
                                      ") differs from the model header (" +
                                      std::to_string(declaredNodeCount_) + ").");
        }
        job_.update(2u, 5u, "Decoding model animations: " + label_);
        parseAnimations();
        // Packed Input::Reset sets the geometry-type 0x80 flag. Engine
        // ProcessSkinSeams returns immediately for that flag.
        // Preserve the compiled MDX normals, including for the lazy CPU path.
        job_.update(3u, 5u, "Retaining compiled model normals: " + label_);
        job_.update(4u, 5u, "Building model bind pose: " + label_);
        calculateWorldTransformsAndBounds();
        job_.update(5u, 5u, "Decoded model: " + label_);
        return std::move(model_);
    }

private:
    void parseHeader() {
        payload_->require(0u, kModelHeaderSize, "model header");
        const auto token0 = payload_->u32(0u, "model layout token 0");
        const auto token1 = payload_->u32(4u, "model layout token 1");
        if (token0 == kK1ModelToken0 && token1 == kK1ModelToken1) {
            model_.game = GameVersion::Kotor1;
        } else if (token0 == kK2ModelToken0 && token1 == kK2ModelToken1) {
            model_.game = GameVersion::Kotor2;
        } else {
            std::ostringstream message;
            message << sourcePrefix(label_) << "Unsupported model layout tokens 0x" << std::hex
                    << token0 << "/0x" << token1;
            throw ModelError(message.str());
        }
        model_.name = payload_->fixedString(8u, 32u, "model name");
        rootNodeOffset_ = payload_->u32(40u, "root node offset");
        declaredNodeCount_ = payload_->u32(44u, "declared node count");
        model_.engineNodeCount=declaredNodeCount_;
        model_.engineMdxPoolBytes=payload_->u32(176u,"engine MDX pool size");
        model_.classification = payload_->u16(80u, "model classification");
        model_.subclassification = payload_->u8(82u, "model subclassification");
        model_.fog = payload_->u8(83u, "model fog flag") != 0u;
        model_.engineReferenceCount=payload_->u32(84u,"engine model reference count");
        animationsOffset_ = payload_->u32(88u, "animation-offset array");
        model_.declaredAnimationCount = payload_->u32(92u, "animation count");
        model_.engineBounds.minimum = payload_->vec3(104u, "engine model bounds minimum");
        model_.engineBounds.maximum = payload_->vec3(116u, "engine model bounds maximum");
        model_.engineBounds.valid = true;
        model_.engineRadius = payload_->f32(128u, "engine model radius");
        // Reset only byte-swaps this authored float; FindAnimScale multiplies
        // it verbatim. Preserve non-finite values through packed compilation
        // and reload just as setanimationscale's sscanf producer does.
        model_.animationScale = payload_->f32(132u, "animation scale");
        model_.engineSupermodelName=payload_->fixedString(136u,32u,"supermodel name");
        model_.supermodel=lowercase(model_.engineSupermodelName)=="null"
            ? std::string{} : model_.engineSupermodelName;
        namesOffset_ = payload_->u32(184u, "name-offset table");
        namesCount_ = payload_->u32(188u, "name count");
        if (declaredNodeCount_ > limits_.maximumNodes || namesCount_ > limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) + "MDL declares more nodes/names than the configured limit");
        }
        if (model_.declaredAnimationCount > limits_.maximumAnimations) {
            throw ModelError(sourcePrefix(label_) + "MDL declares more animations than the configured limit");
        }
    }

    void loadNames() {
        names_.clear();
        names_.reserve(namesCount_);
        if (namesCount_ == 0u) return;
        if (namesOffset_ == 0u || namesOffset_ == 0xFFFFFFFFu ||
            namesCount_ > (payload_->size() - std::min<std::size_t>(namesOffset_, payload_->size())) / 4u) {
            model_.warnings.push_back("The node-name offset table is unavailable; node IDs will be displayed instead.");
            return;
        }
        payload_->require(namesOffset_, static_cast<std::size_t>(namesCount_) * 4u, "name-offset table");
        for (std::uint32_t index = 0u; index < namesCount_; ++index) {
            const auto offset = payload_->u32(namesOffset_ + static_cast<std::size_t>(index) * 4u,
                                              "node-name offset");
            if (offset == 0u || offset == 0xFFFFFFFFu || offset >= payload_->size()) {
                names_.emplace_back();
                continue;
            }
            try {
                names_.push_back(payload_->cString(offset, limits_.maximumNameLength, "node name"));
            } catch (const ModelError&) {
                names_.emplace_back();
                model_.warnings.push_back("Node-name entry " + std::to_string(index) + " is malformed.");
            }
        }
        model_.engineNames=names_;
    }

    static ControllerValueKind controllerValueKind(std::uint8_t rawFlags) noexcept {
        switch (rawFlags & 0x0fu) {
        case 1u: return ControllerValueKind::Float;
        case 2u: return ControllerValueKind::PackedQuaternion;
        case 3u: return ControllerValueKind::Vector;
        case 4u: return ControllerValueKind::Quaternion;
        default: return ControllerValueKind::Unknown;
        }
    }

    static ControllerInterpolation controllerInterpolation(std::uint8_t rawFlags) noexcept {
        switch (rawFlags & 0xf0u) {
        case 0x00u: return ControllerInterpolation::Linear;
        case 0x10u: return ControllerInterpolation::Bezier;
        default: return ControllerInterpolation::Unknown;
        }
    }

    static std::size_t controllerWordsPerKey(const Controller& controller) noexcept {
        const bool bezier = controller.interpolation == ControllerInterpolation::Bezier;
        switch (controller.valueKind) {
        case ControllerValueKind::Float: return bezier ? 3u : 1u;
        case ControllerValueKind::PackedQuaternion: return 1u;
        case ControllerValueKind::Vector: return bezier ? 9u : 3u;
        case ControllerValueKind::Quaternion: return 4u;
        case ControllerValueKind::Unknown: return 0u;
        }
        return 0u;
    }

    void parseControllers(std::size_t nodeOffset, Node& node, const std::string& description) {
        const auto controllerOffset = payload_->u32(nodeOffset + 56u, "controller array offset");
        const auto controllerCount = payload_->u32(nodeOffset + 60u, "controller count");
        const auto dataOffset = payload_->u32(nodeOffset + 68u, "controller-data offset");
        const auto dataCount = payload_->u32(nodeOffset + 72u, "controller-data count");

        if (controllerCount > limits_.maximumControllers ||
            totalControllers_ > limits_.maximumControllers - controllerCount) {
            throw ModelError(sourcePrefix(label_) + "Controller count exceeds the configured limit");
        }
        if (dataCount > limits_.maximumControllerValues ||
            totalControllerValues_ > limits_.maximumControllerValues - dataCount) {
            throw ModelError(sourcePrefix(label_) + "Controller-data count exceeds the configured limit");
        }
        totalControllers_ += controllerCount;
        totalControllerValues_ += dataCount;

        if (dataCount != 0u) {
            if (dataOffset == 0u || dataOffset == 0xffffffffu ||
                !payload_->contains(dataOffset, static_cast<std::size_t>(dataCount) * 4u)) {
                model_.warnings.push_back(description +
                    " declares controller data outside the MDL payload; its controllers were ignored.");
                return;
            }
            node.controllerDataWords.reserve(dataCount);
            for (std::uint32_t index = 0u; index < dataCount; ++index) {
                node.controllerDataWords.push_back(payload_->f32Bits(
                    static_cast<std::size_t>(dataOffset) + static_cast<std::size_t>(index) * 4u,
                    "controller-data word"));
            }
        }

        if (controllerCount == 0u) return;
        if (controllerOffset == 0u || controllerOffset == 0xffffffffu ||
            !payload_->contains(controllerOffset,
                                static_cast<std::size_t>(controllerCount) * kControllerSize)) {
            model_.warnings.push_back(description +
                " declares a controller array outside the MDL payload; its controllers were ignored.");
            return;
        }

        node.controllers.reserve(controllerCount);
        for (std::uint32_t index = 0u; index < controllerCount; ++index) {
            const auto offset = static_cast<std::size_t>(controllerOffset) +
                                static_cast<std::size_t>(index) * kControllerSize;
            Controller controller;
            controller.type = payload_->u32(offset, "controller type");
            controller.sourceOffset = payload_->i16(offset + 4u, "controller source offset");
            controller.rowCount = payload_->u16(offset + 6u, "controller row count");
            controller.timeIndex = payload_->u16(offset + 8u, "controller time index");
            controller.dataIndex = payload_->u16(offset + 10u, "controller value index");
            controller.rawFlags = payload_->u8(offset + 12u, "controller encoding");
            controller.valueKind = controllerValueKind(controller.rawFlags);
            controller.interpolation = controllerInterpolation(controller.rawFlags);

            bool valid = true;
            const auto rows = static_cast<std::size_t>(controller.rowCount);
            const auto timeIndex = static_cast<std::size_t>(controller.timeIndex);
            if (rows != 0u &&
                (timeIndex > node.controllerDataWords.size() ||
                 rows > node.controllerDataWords.size() - timeIndex)) {
                valid = false;
            }
            const auto wordsPerKey = controllerWordsPerKey(controller);
            if (rows != 0u && wordsPerKey != 0u) {
                const auto dataIndex = static_cast<std::size_t>(controller.dataIndex);
                if (rows > std::numeric_limits<std::size_t>::max() / wordsPerKey ||
                    dataIndex > node.controllerDataWords.size() ||
                    rows * wordsPerKey > node.controllerDataWords.size() - dataIndex) {
                    valid = false;
                }
            }
            if (!valid) {
                model_.warnings.push_back(description + " controller " + std::to_string(index) +
                    " references values outside its bounded controller-data array and was skipped.");
                continue;
            }

            if (rows > 1u) {
                float previous = wordAsFloat(node.controllerDataWords[timeIndex]);
                bool ordered = finite(previous);
                for (std::size_t key = 1u; key < rows && ordered; ++key) {
                    const auto current = wordAsFloat(node.controllerDataWords[timeIndex + key]);
                    ordered = finite(current) && current >= previous;
                    previous = current;
                }
                if (!ordered) {
                    model_.warnings.push_back(description + " controller " + std::to_string(index) +
                        " has non-finite or unsorted key times; evaluation will clamp safely.");
                }
            }
            node.controllers.push_back(controller);
        }
    }

    std::optional<std::pair<std::size_t, std::size_t>> readArrayDefinition(
        std::size_t definition, std::size_t stride, std::size_t maximumCount,
        const std::string& description) {
        if (!payload_->contains(definition, kArrayDefinitionSize)) {
            model_.warnings.push_back(description + " has no complete array definition.");
            return std::nullopt;
        }
        const auto dataOffset = payload_->u32(definition, "array data offset");
        const auto count = payload_->u32(definition + 4u, "array count");
        const auto capacity = payload_->u32(definition + 8u, "array capacity");
        if (count > maximumCount) {
            throw ModelError(sourcePrefix(label_) + description +
                             " count exceeds the configured limit");
        }
        if (capacity != 0u && count > capacity) {
            model_.warnings.push_back(description +
                " has a count larger than its capacity; the array was ignored.");
            return std::nullopt;
        }
        if (count == 0u) return std::pair<std::size_t, std::size_t>{0u, 0u};
        if (dataOffset == 0u || dataOffset == 0xffffffffu ||
            stride > std::numeric_limits<std::size_t>::max() / count ||
            !payload_->contains(dataOffset, static_cast<std::size_t>(count) * stride)) {
            model_.warnings.push_back(description +
                " points outside the bounded MDL payload; the array was ignored.");
            return std::nullopt;
        }
        return std::pair<std::size_t, std::size_t>{
            static_cast<std::size_t>(dataOffset), static_cast<std::size_t>(count)};
    }

    void parseReferenceNode(std::size_t nodeOffset, std::size_t nodeIndex) {
        ReferenceNodeData data;
        const auto extension = nodeOffset + kNodeHeaderSize;
        if (!payload_->contains(extension, kReferenceHeaderSize)) {
            model_.warnings.push_back("Reference node " + model_.nodes[nodeIndex].name +
                                      " has a truncated 36-byte extension.");
            model_.nodes[nodeIndex].reference = std::move(data);
            return;
        }
        // CreateInstance forwards this complete string to NewCAurObject /
        // FindModel. NONE and surrounding whitespace are not null tokens.
        data.model = payload_->fixedString(extension, 32u, "reference model ResRef");
        data.reattachable = payload_->i32(extension + 32u, "reference reattachable flag");
        model_.nodes[nodeIndex].reference = std::move(data);
    }

    void parseLightNode(std::size_t nodeOffset, std::size_t nodeIndex) {
        LightNodeData data;
        // K1 Desktop MdlNodeLight constructor (0x330c18) and ResetLight
        // (0x321d5a) match the K2 authored fields and constructor defaults.
        data.dynamicType = 1;
        data.dynamic = true;
        data.affectDynamic = true;
        data.castsShadow = true;
        data.priority = 5;
        data.fading = true;
        const auto extension = nodeOffset + kNodeHeaderSize;
        if (!payload_->contains(extension, kLightHeaderSize)) {
            model_.warnings.push_back("Light node " + model_.nodes[nodeIndex].name +
                                      " has a truncated light extension.");
            model_.nodes[nodeIndex].light = std::move(data);
            return;
        }
        data.flareRadius = payload_->f32(extension, "light flare radius");
        // Engine flare consumer validates this value; do not silently repair it.

        // The first array definition at +4 is a serialized engine-side
        // texture-object handle list. The fifth array is a list of serialized
        // string offsets, not inline 32-byte names. Both engine Desktop ResetLight
        // routines relocate the list and each four-byte element: K1
        // 0x321eb4..0x321ee1, K2 0xf4615..0xf465a.
        const auto textures = readArrayDefinition(
            extension + 52u, 4u, limits_.maximumVertices,
            "Light node " + model_.nodes[nodeIndex].name + " texture-name array");
        if (textures) {
            data.textureNames.reserve(textures->second);
            for (std::size_t index = 0u; index < textures->second; ++index) {
                const auto nameOffset=payload_->u32(textures->first + index * 4u,
                                                   "light flare texture-name offset");
                // ResetLight forwards the stored name verbatim to the texture
                // lookup. Keep NULL/NONE spellings for engine ASCII export;
                // resource binding owns any interpretation of those names.
                data.textureNames.push_back(payload_->cString(
                    nameOffset, limits_.maximumNameLength, "light flare texture name"));
            }
        }
        const auto sizes = readArrayDefinition(
            extension + 16u, 4u, limits_.maximumVertices,
            "Light node " + model_.nodes[nodeIndex].name + " flare-size array");
        if (sizes) {
            data.flareSizes.reserve(sizes->second);
            for (std::size_t index = 0u; index < sizes->second; ++index) {
                auto value = payload_->f32(sizes->first + index * 4u, "light flare size");
                data.flareSizes.push_back(value);
            }
        }
        const auto positions = readArrayDefinition(
            extension + 28u, 4u, limits_.maximumVertices,
            "Light node " + model_.nodes[nodeIndex].name + " flare-position array");
        if (positions) {
            data.flarePositions.reserve(positions->second);
            for (std::size_t index = 0u; index < positions->second; ++index) {
                auto value = payload_->f32(positions->first + index * 4u,
                                           "light flare position");
                data.flarePositions.push_back(value);
            }
        }
        const auto shifts = readArrayDefinition(
            extension + 40u, 12u, limits_.maximumVertices,
            "Light node " + model_.nodes[nodeIndex].name + " flare-color array");
        if (shifts) {
            data.flareColorShifts.reserve(shifts->second);
            for (std::size_t index = 0u; index < shifts->second; ++index) {
                data.flareColorShifts.push_back(
                    payload_->vec3(shifts->first + index * 12u, "light flare color shift"));
            }
        }

        // These source offsets follow the complete MdlNodeLight32 ->
        // MdlNodeLight conversion constructor and matching ASCII fields.
        data.priority = payload_->i32(extension + 64u, "light priority");
        data.ambientOnly = payload_->i32(extension + 68u, "ambient-only flag");
        data.dynamicType = payload_->i32(extension + 72u, "dynamic-light type");
        data.dynamic = data.dynamicType != 0;
        data.affectDynamic = payload_->i32(extension + 76u, "affect-dynamic flag");
        data.castsShadow = payload_->i32(extension + 80u, "light shadow flag");
        data.generateFlare = payload_->i32(extension + 84u, "generate-flare flag");
        data.fading = payload_->i32(extension + 88u, "fading-light flag");
        model_.nodes[nodeIndex].light = std::move(data);
    }

    void parseEmitterNode(std::size_t nodeOffset, std::size_t nodeIndex) {
        EmitterNodeData data;
        const auto extension = nodeOffset + kNodeHeaderSize;
        if (!payload_->contains(extension, kEmitterHeaderSize)) {
            model_.warnings.push_back("Emitter node " + model_.nodes[nodeIndex].name +
                (model_.game == GameVersion::Kotor2 ? " has a truncated 224-byte K2 extension." :
                                                     " has a truncated 224-byte K1 extension."));
            model_.nodes[nodeIndex].emitter = std::move(data);
            return;
        }
        for (std::size_t index = 0u; index < data.rawWords.size(); ++index) {
            data.rawWords[index] = payload_->u32(extension + index * 4u,
                                                 "emitter extension word");
        }
        // ResetEmitter (K1 Desktop 0x322085; K2 Desktop 0xf4423) only byte-swaps
        // these authored floats. Preserve them for export and the engine
        // emitter consumers instead of replacing non-finite words with zero.
        data.deadSpace = payload_->f32(extension, "emitter dead space");
        data.blastRadius = payload_->f32(extension + 4u, "emitter blast radius");
        data.blastLength = payload_->f32(extension + 8u, "emitter blast length");
        data.branchCount = payload_->u32(extension + 12u, "lightning branch count");
        data.controlPointSmoothing = payload_->i32(extension + 16u, "lightning control smoothing");
        data.xGrid = payload_->u32(extension + 20u, "emitter X grid");
        data.yGrid = payload_->u32(extension + 24u, "emitter Y grid");
        data.spawnType = payload_->u32(extension + 28u, "emitter spawn type");
        data.updateMode = payload_->fixedString(extension + 32u, 32u, "emitter update mode");
        data.renderMode = payload_->fixedString(extension + 64u, 32u, "emitter render mode");
        data.blendMode = payload_->fixedString(extension + 96u, 32u, "emitter blend mode");
        // ResetEmitter retains these fixed strings. In particular, a named
        // chunk remains nonempty even when its resource cannot be resolved.
        data.texture = payload_->fixedString(extension + 128u, 32u, "emitter texture");
        data.chunkName = payload_->fixedString(extension + 160u, 16u, "emitter chunk name");
        data.twoSidedTexture = payload_->i32(extension + 176u,
                                             "emitter two-sided texture flag");
        data.loop = payload_->i32(extension + 180u, "emitter loop flag");
        data.renderOrder = payload_->u16(extension + 184u, "emitter render order");
        data.frameBlending = payload_->u8(extension + 186u,
                                          "emitter frame-blending flag") != 0u;
        data.depthTexture = payload_->fixedString(extension + 187u, 32u, "emitter depth texture");
        if (model_.game == GameVersion::Kotor1) {
            model_.warnings.push_back("Emitter node " + model_.nodes[nodeIndex].name +
                " uses the packed-verified 224-byte K1 extension; mode-specific runtime parity is reported by the renderer.");
        }
        model_.nodes[nodeIndex].emitter = std::move(data);
    }

    void parseAnimMesh(std::size_t nodeOffset, Node& node) {
        if ((node.flags & kNodeAnimation) == 0u) return;
        if (node.flags != (kNodeBase | kNodeMesh | kNodeAnimation))
            throw ModelError(sourcePrefix(label_) + "Invalid combined animmesh node flags");
        // K1 Desktop ResetAnim 0x321c20 / K2 ResetMdlNode 0xf42bb..0xf42e5
        // relocate both final pools against MDL, not MDX. Their 56-byte
        // extension is identical, after the respective common mesh header.
        const auto extension = nodeOffset + kNodeHeaderSize +
            (model_.game == GameVersion::Kotor1 ? kK1MeshHeaderSize : kK2MeshHeaderSize);
        payload_->require(extension, 56u, "animmesh extension");
        AnimMeshData data;
        for (std::size_t i=0;i<data.rawHeader.size();++i)
            data.rawHeader[i]=payload_->u8(extension+i,"animmesh raw header");
        data.samplePeriod=payload_->f32(extension,"animmesh sample period");
        data.vertexCount=payload_->u16(nodeOffset+0x180u,"animmesh vertex count");
        data.positionSampleCount=payload_->i32(extension+48u,"animmesh position sample count");
        data.texcoordSampleCount=payload_->i32(extension+52u,"animmesh UV sample count");
        if (data.vertexCount>32767u ||
            data.positionSampleCount<0 || data.texcoordSampleCount<0)
            throw ModelError(sourcePrefix(label_) + "Invalid animmesh signed pool dimensions");
        const auto pool=[&](std::int32_t frames, std::size_t field, std::size_t stride) {
            const auto count=static_cast<std::uint64_t>(data.vertexCount)*static_cast<std::uint32_t>(frames);
            if (count>std::numeric_limits<std::size_t>::max()/stride ||
                count>limits_.maximumAnimMeshSamples ||
                totalAnimMeshSamples_>limits_.maximumAnimMeshSamples-static_cast<std::size_t>(count))
                throw ModelError(sourcePrefix(label_) + "Animmesh samples exceed the configured aggregate limit");
            const auto pointer=payload_->u32(extension+field,"animmesh final sample pointer");
            if (count && (pointer==0u || pointer==0xffffffffu))
                throw ModelError(sourcePrefix(label_) + "Missing animmesh final sample pool");
            if (count) payload_->require(pointer,static_cast<std::size_t>(count)*stride,"animmesh final sample pool");
            totalAnimMeshSamples_+=static_cast<std::size_t>(count);
            return std::pair<std::size_t,std::size_t>{pointer,static_cast<std::size_t>(count)};
        };
        const auto positions=pool(data.positionSampleCount,40u,12u);
        const auto texcoords=pool(data.texcoordSampleCount,44u,8u);
        data.positionSamples.reserve(positions.second);
        for (std::size_t i=0;i<positions.second;++i) {
            data.positionSamples.push_back(payload_->vec3(
                positions.first+i*12u,"animmesh position sample"));
        }
        data.texcoordSamples.reserve(texcoords.second);
        for (std::size_t i=0;i<texcoords.second;++i) {
            data.texcoordSamples.push_back(payload_->vec2(
                texcoords.first+i*8u,"animmesh UV sample"));
        }
        node.animmesh=std::move(data);
    }

    void parseSpecializedNode(std::size_t nodeOffset, std::size_t nodeIndex) {
        const auto flags = model_.nodes[nodeIndex].flags;
        if ((flags & kNodeLight) != 0u) parseLightNode(nodeOffset, nodeIndex);
        if ((flags & kNodeEmitter) != 0u) parseEmitterNode(nodeOffset, nodeIndex);
        if ((flags & kNodeCamera) != 0u) model_.nodes[nodeIndex].camera = CameraNodeData{};
        if ((flags & kNodeTrigger) != 0u) {
            TriggerNodeData data;
            data.packedVerified = true;
            // MdlNodeTrigger has only the 0x50-byte base payload in both games.
            // K1 Desktop InternalParseField 0x3716d4 is empty and
            // InternalCreateInstance 0x3716dc returns null, matching K2 Desktop
            // 0xb303a / 0xb3040. Runtime GIT volumes are separate objects.
            model_.nodes[nodeIndex].trigger = data;
        }
        if ((flags & kNodeReference) != 0u) parseReferenceNode(nodeOffset, nodeIndex);
        if ((flags & kNodeLightsaber) != 0u)
            model_.nodes[nodeIndex].lightsaber = LightsaberNodeData{};
    }

    std::size_t parseNode(std::uint32_t offset, std::optional<std::size_t> parent, std::size_t depth) {
        if (depth > limits_.maximumRecursionDepth) {
            throw ModelError(sourcePrefix(label_) + "Node hierarchy exceeds the configured depth limit");
        }
        const auto existing = nodeByOffset_.find(offset);
        if (existing != nodeByOffset_.end()) {
            // The first traversal establishes the canonical parent. Re-parenting a
            // previously parsed node can introduce self/cyclic world transforms.
            model_.warnings.push_back("The node hierarchy reuses or cycles through offset " +
                                      std::to_string(offset) + "; it was parsed once.");
            return existing->second;
        }
        if (model_.nodes.size() >= limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) + "Node count exceeds the configured limit");
        }
        payload_->require(offset, kNodeHeaderSize, "node header");

        Node node;
        node.flags = payload_->u16(offset, "node flags");
        // The synchronized short ID and the DWORD name-table index are
        // separate fields. ParseNode writes +2/+4 independently; engine
        // SynchronizeNodes compares +2 while TranslateNodeName indexes +4.
        node.id = payload_->u16(offset + 2u, "node ID");
        const auto nameIndex = payload_->u32(offset + 4u, "node name index");
        node.engineNameIndex=nameIndex;
        node.parent = parent;
        node.name = nameIndex < names_.size() ? names_[nameIndex] : std::string{};
        if (node.name.empty()) node.name = "node_" + std::to_string(node.id);
        node.position = safeVec3(payload_->vec3(offset + 16u, "node position"), model_.warnings,
                                 "Node " + node.name);


        node.orientation =
            {payload_->f32(offset + 32u, "node quaternion x"),
             payload_->f32(offset + 36u, "node quaternion y"),
             payload_->f32(offset + 40u, "node quaternion z"),
             payload_->f32(offset + 28u, "node quaternion w")};
        parseControllers(offset, node, "Node " + node.name);
        parseAnimMesh(offset, node);
        const auto childOffset = payload_->u32(offset + 44u, "child-offset array");
        const auto childCount = payload_->u32(offset + 48u, "child count");
        if (childCount > limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) + "Node child count exceeds the configured limit");
        }

        const auto nodeIndex = model_.nodes.size();
        model_.nodes.push_back(std::move(node));
        nodeByOffset_.emplace(offset, nodeIndex);

        parseSpecializedNode(offset, nodeIndex);
        if ((model_.nodes[nodeIndex].flags & kNodeMesh) != 0u)
            parseMesh(offset + kNodeHeaderSize, nodeIndex);

        if (childCount != 0u) {
            if (childOffset == 0u || childOffset == 0xFFFFFFFFu) {
                model_.warnings.push_back("Node " + model_.nodes[nodeIndex].name +
                                          " declares children but has no child-offset array.");
            } else {
                payload_->require(childOffset, static_cast<std::size_t>(childCount) * 4u,
                                  "child-offset array");
                for (std::uint32_t index = 0u; index < childCount; ++index) {
                    const auto child = payload_->u32(childOffset + static_cast<std::size_t>(index) * 4u,
                                                     "child node offset");
                    if (child == 0u || child == 0xFFFFFFFFu) {
                        model_.warnings.push_back("Node " + model_.nodes[nodeIndex].name +
                                                  " contains an empty child offset.");
                        continue;
                    }
                    const auto childIndex = parseNode(child, nodeIndex, depth + 1u);
                    if (model_.nodes[childIndex].parent != nodeIndex) {
                        model_.warnings.push_back("Node " + model_.nodes[nodeIndex].name +
                                                  " references a node owned by another parent; "
                                                  "the non-canonical link was ignored.");
                        continue;
                    }
                    if (std::find(model_.nodes[nodeIndex].children.begin(),
                                  model_.nodes[nodeIndex].children.end(), childIndex) ==
                        model_.nodes[nodeIndex].children.end()) {
                        model_.nodes[nodeIndex].children.push_back(childIndex);
                    }
                }
            }
        }
        return nodeIndex;
    }

    std::size_t parseAnimationNode(std::uint32_t offset, Animation& animation,
                                   std::unordered_map<std::uint32_t, std::size_t>& nodeByOffset,
                                   std::optional<std::size_t> parent, std::size_t depth) {
        if (depth > limits_.maximumRecursionDepth) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation node hierarchy exceeds the configured depth limit");
        }
        const auto existing = nodeByOffset.find(offset);
        if (existing != nodeByOffset.end()) {
            model_.warnings.push_back("Animation " + animation.name +
                " reuses or cycles through node offset " + std::to_string(offset) +
                "; it was parsed once.");
            return existing->second;
        }
        if (animation.nodes.size() >= limits_.maximumNodes ||
            totalAnimationNodes_ >= limits_.maximumAnimationNodes) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation node count exceeds the configured limit");
        }
        payload_->require(offset, kNodeHeaderSize, "animation node header");

        Node node;
        node.flags = payload_->u16(offset, "animation node flags");
        node.id = payload_->u16(offset + 2u, "animation node ID");
        const auto nameIndex = payload_->u32(offset + 4u, "animation node name index");
        node.engineNameIndex=nameIndex;
        node.parent = parent;
        node.name = nameIndex < names_.size() ? names_[nameIndex] : std::string{};
        if (node.name.empty()) node.name = "node_" + std::to_string(node.id);
        node.position = safeVec3(payload_->vec3(offset + 16u, "animation node position"),
                                 model_.warnings,
                                 "Animation " + animation.name + " node " + node.name);
        node.orientation =
            {payload_->f32(offset + 32u, "animation node quaternion x"),
             payload_->f32(offset + 36u, "animation node quaternion y"),
             payload_->f32(offset + 40u, "animation node quaternion z"),
             payload_->f32(offset + 28u, "animation node quaternion w")};
        parseControllers(offset, node, "Animation " + animation.name + " node " + node.name);
        parseAnimMesh(offset, node);

        const auto childOffset = payload_->u32(offset + 44u, "animation child-offset array");
        const auto childCount = payload_->u32(offset + 48u, "animation child count");
        if (childCount > limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation node child count exceeds the configured limit");
        }

        const auto nodeIndex = animation.nodes.size();
        animation.nodes.push_back(std::move(node));
        ++totalAnimationNodes_;
        nodeByOffset.emplace(offset, nodeIndex);

        if (childCount != 0u) {
            if (childOffset == 0u || childOffset == 0xffffffffu ||
                !payload_->contains(childOffset, static_cast<std::size_t>(childCount) * 4u)) {
                model_.warnings.push_back("Animation " + animation.name + " node " +
                    animation.nodes[nodeIndex].name +
                    " declares children outside the MDL payload.");
            } else {
                for (std::uint32_t index = 0u; index < childCount; ++index) {
                    const auto child = payload_->u32(
                        static_cast<std::size_t>(childOffset) + static_cast<std::size_t>(index) * 4u,
                        "animation child node offset");
                    if (child == 0u || child == 0xffffffffu) {
                        model_.warnings.push_back("Animation " + animation.name + " node " +
                            animation.nodes[nodeIndex].name + " contains an empty child offset.");
                        continue;
                    }
                    const auto childIndex = parseAnimationNode(
                        child, animation, nodeByOffset, nodeIndex, depth + 1u);
                    if (animation.nodes[childIndex].parent != nodeIndex) {
                        model_.warnings.push_back("Animation " + animation.name + " node " +
                            animation.nodes[nodeIndex].name +
                            " references a node owned by another parent; the link was ignored.");
                        continue;
                    }
                    if (std::find(animation.nodes[nodeIndex].children.begin(),
                                  animation.nodes[nodeIndex].children.end(), childIndex) ==
                        animation.nodes[nodeIndex].children.end()) {
                        animation.nodes[nodeIndex].children.push_back(childIndex);
                    }
                }
            }
        }
        return nodeIndex;
    }

    void parseAnimation(std::uint32_t offset, std::size_t index) {
        if (offset == 0u || offset == 0xffffffffu ||
            !payload_->contains(offset, kAnimationHeaderSize)) {
            model_.warnings.push_back("Animation entry " + std::to_string(index) +
                                      " has an invalid header offset and was skipped.");
            return;
        }

        Animation animation;
        animation.name = payload_->fixedString(offset + 8u, 32u, "animation name");
        if (animation.name.empty()) animation.name = "animation_" + std::to_string(index);
        const auto rootOffset = payload_->u32(offset + 40u, "animation root-node offset");
        const auto declaredNodes = payload_->u32(offset + 44u, "animation node count");
        animation.engineNodeCount=declaredNodes;
        if (declaredNodes > limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation declares more nodes than the configured limit");
        }
        animation.length = payload_->f32(offset + 80u, "animation length");
        animation.transitionTime = payload_->f32(offset + 84u, "animation transition time");
        // This is an authored node name, not a resource-null token. Engine
        // packed loading preserves it even when the name is "null" or "none".
        animation.root = payload_->fixedString(offset + 88u, 32u, "animation root name");
        const auto eventOffset = payload_->u32(offset + 120u, "animation event array");
        const auto eventCount = payload_->u32(offset + 124u, "animation event count");
        if (eventCount > limits_.maximumAnimationEvents ||
            totalAnimationEvents_ > limits_.maximumAnimationEvents - eventCount) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation event count exceeds the configured limit");
        }
        totalAnimationEvents_ += eventCount;
        if (eventCount != 0u) {
            if (eventOffset == 0u || eventOffset == 0xffffffffu ||
                !payload_->contains(eventOffset,
                                    static_cast<std::size_t>(eventCount) * kAnimationEventSize)) {
                model_.warnings.push_back("Animation " + animation.name +
                    " declares events outside the MDL payload; they were ignored.");
            } else {
                animation.events.reserve(eventCount);
                for (std::uint32_t eventIndex = 0u; eventIndex < eventCount; ++eventIndex) {
                    const auto eventRecord = static_cast<std::size_t>(eventOffset) +
                        static_cast<std::size_t>(eventIndex) * kAnimationEventSize;
                    AnimationEvent event;
                    event.time = payload_->f32(eventRecord, "animation event time");
                    event.name = payload_->fixedString(eventRecord + 4u, 32u,
                                                       "animation event name");
                    // Engine ResetAnimation retains every record, including
                    // unordered and infinite times (K1 Desktop 0x32218f; K2 Desktop
                    // 0xf3ff6). AnimateEvents owns the range comparisons.
                    animation.events.push_back(std::move(event));
                }
            }
        }

        if (rootOffset != 0u && rootOffset != 0xffffffffu) {
            std::unordered_map<std::uint32_t, std::size_t> nodeByOffset;
            parseAnimationNode(rootOffset, animation, nodeByOffset, std::nullopt, 0u);
        } else if (declaredNodes != 0u) {
            model_.warnings.push_back("Animation " + animation.name +
                                      " declares nodes but has no valid root-node offset.");
        }
        if (declaredNodes != 0u && animation.nodes.size() != declaredNodes) {
            model_.warnings.push_back("Animation " + animation.name + " has " +
                std::to_string(animation.nodes.size()) + " reachable node(s), but its header declares " +
                std::to_string(declaredNodes) + ".");
        }
        model_.animations.push_back(std::move(animation));
    }

    void parseAnimations() {
        const auto count = static_cast<std::size_t>(model_.declaredAnimationCount);
        if (count == 0u) return;
        if (animationsOffset_ == 0u || animationsOffset_ == 0xffffffffu ||
            !payload_->contains(animationsOffset_, count * 4u)) {
            model_.warnings.push_back(
                "The model declares animations but its animation-offset array is unavailable.");
            return;
        }
        model_.animations.reserve(count);
        std::unordered_set<std::uint32_t> seen;
        for (std::size_t index = 0u; index < count; ++index) {
            const auto offset = payload_->u32(static_cast<std::size_t>(animationsOffset_) + index * 4u,
                                              "animation offset");
            if (!seen.insert(offset).second) {
                model_.warnings.push_back("Animation entry " + std::to_string(index) +
                                          " repeats an earlier animation offset and was skipped.");
                continue;
            }
            parseAnimation(offset, index);
        }
        if (model_.animations.size() != count) {
            model_.warnings.push_back("Parsed " + std::to_string(model_.animations.size()) +
                " animation(s) from " + std::to_string(count) + " declared entries.");
        }
    }

    bool validMdxChannel(std::uint32_t offset, std::uint32_t rowSize, std::size_t bytes) const noexcept {
        return offset != 0xFFFFFFFFu && offset <= rowSize && bytes <= rowSize - offset;
    }

    void parseLightsaber(std::size_t meshOffset, std::size_t nodeIndex,
                         const Mesh& mesh) {
        auto& node=model_.nodes[nodeIndex];
        if(!node.lightsaber) return;
        if(node.flags!=(kNodeBase|kNodeMesh|kNodeLightsaber))
            throw ModelError(sourcePrefix(label_)+"Invalid combined lightsaber node flags");
        auto& saber=*node.lightsaber;
        const auto extension=meshOffset+(model_.game==GameVersion::Kotor1
            ?kK1MeshHeaderSize:kK2MeshHeaderSize);
        payload_->require(extension,20u,"lightsaber extension");




        const auto count=static_cast<std::int16_t>(mesh.enginePrelightVertexCount);
        if(count<0)
            throw ModelError(sourcePrefix(label_)+"Invalid lightsaber signed vertex count");
        const auto pool=[&](std::size_t field,std::size_t stride,const char* label) {
            const auto offset=payload_->u32(extension+field,label);
            if(count!=0) {
                if(offset==0u || offset==0xffffffffu)
                    throw ModelError(sourcePrefix(label_)+"Missing "+label);
                payload_->require(offset,static_cast<std::size_t>(count)*stride,label);
            }
            return offset;
        };
        const auto positions=pool(0u,12u,"lightsaber positions");
        const auto texcoords=pool(4u,8u,"lightsaber texture coordinates");
        const auto normals=pool(8u,12u,"lightsaber normals");
        for(std::size_t i=0;i<static_cast<std::size_t>(count);++i) {
            saber.positions.push_back(payload_->vec3(positions+i*12u,"lightsaber position"));
            saber.texcoords.push_back(payload_->vec2(texcoords+i*8u,"lightsaber texture coordinate"));
            saber.normals.push_back(payload_->vec3(normals+i*12u,"lightsaber normal"));
        }
        if(mesh.cpuPositions) saber.originalPositions=*mesh.cpuPositions;
        // The scene's fixed piece/segment topology requires the full stream.
        // Retain other bounded counts for inspection without enabling it.
        saber.postprocessed=static_cast<std::size_t>(count)==LightsaberNodeData::generatedVertexCount;
    }

    void parseAabbTree(std::size_t meshOffset, std::size_t nodeIndex,
                       const Mesh& mesh) {
        if ((model_.nodes[nodeIndex].flags & kNodeAabb) == 0u) return;

        AabbNodeData data;
        data.valid = true;
        const auto nodeName = model_.nodes[nodeIndex].name;
        // K1 Desktop ResetAABB 0x321cfa relocates node+0x19c; K2's root is
        // node+0x1a4. meshOffset already skips the common 0x50-byte node.
        const auto meshHeaderSize = model_.game == GameVersion::Kotor1
            ? kK1MeshHeaderSize : kK2MeshHeaderSize;
        if (!payload_->contains(meshOffset + meshHeaderSize, sizeof(std::uint32_t))) {
            data.valid = false;
            model_.warnings.push_back("AABB node " + nodeName +
                                      " has no bounded tree-root field.");
            model_.nodes[nodeIndex].aabb = std::move(data);
            return;
        }

        const auto rootOffset = payload_->u32(meshOffset + meshHeaderSize,
                                              "AABB root offset");
        if (rootOffset == 0u || rootOffset == 0xffffffffu) {
            data.valid = false;
            model_.warnings.push_back("AABB node " + nodeName +
                                      " has no runtime tree root.");
            model_.nodes[nodeIndex].aabb = std::move(data);
            return;
        }

        struct ParseIssues {
            std::size_t truncated{};
            std::size_t cycles{};
            std::size_t shared{};
            std::size_t missingChildren{};
            std::size_t invalidFaces{};
            std::size_t reversedBounds{};
            std::size_t nonFiniteBounds{};
        } issues;
        std::unordered_map<std::uint32_t, std::size_t> byOffset;
        std::unordered_set<std::uint32_t> active;

        const auto childOffsetValid = [](std::uint32_t offset) noexcept {
            return offset != 0u && offset != 0xffffffffu;
        };

        std::function<std::optional<std::size_t>(std::uint32_t, std::size_t)> parseNode;
        parseNode = [&](std::uint32_t offset, std::size_t depth)
            -> std::optional<std::size_t> {
            if (depth > limits_.maximumRecursionDepth ||
                data.nodes.size() >= limits_.maximumAabbNodes) {
                throw ModelError(sourcePrefix(label_) + "AABB tree on " + nodeName +
                                 " exceeds the configured tree limit");
            }
            if (active.find(offset) != active.end()) {
                ++issues.cycles;
                data.valid = false;
                return std::nullopt;
            }
            const auto existing = byOffset.find(offset);
            if (existing != byOffset.end()) {
                ++issues.shared;
                data.valid = false;
                return existing->second;
            }
            if (!payload_->contains(offset, kAabbNodeSize)) {
                ++issues.truncated;
                data.valid = false;
                return std::nullopt;
            }

            AabbTreeNode node;
            node.sourceOffset = offset;
            node.minimum = payload_->vec3(offset, "AABB minimum");
            node.maximum = payload_->vec3(offset + 12u, "AABB maximum");
            for (auto* value : {&node.minimum.x, &node.minimum.y, &node.minimum.z,
                                &node.maximum.x, &node.maximum.y, &node.maximum.z}) {
                if (!finite(*value)) {
                    ++issues.nonFiniteBounds;
                    data.valid = false;
                }
            }
            if (node.minimum.x > node.maximum.x ||
                node.minimum.y > node.maximum.y ||
                node.minimum.z > node.maximum.z) {
                ++issues.reversedBounds;
                data.valid = false;
            }
            const auto firstOffset = payload_->u32(offset + 24u, "AABB first child");
            const auto secondOffset = payload_->u32(offset + 28u, "AABB second child");
            node.faceIndex = payload_->i32(offset + 32u, "AABB face index");
            node.splitMask = payload_->u32(offset + 36u, "AABB split mask");

            const auto index = data.nodes.size();
            data.nodes.push_back(node);
            byOffset.emplace(offset, index);
            active.emplace(offset);

            if (node.faceIndex == -1) {
                // Engine traversal ANDs this stored mask with the direction
                // word (K1 0x32acb7, K2 0xacd30); it need not be one-hot.
                if (!childOffsetValid(firstOffset) || !childOffsetValid(secondOffset)) {
                    ++issues.missingChildren;
                    data.valid = false;
                }
                if (childOffsetValid(firstOffset)) {
                    if (const auto child = parseNode(firstOffset, depth + 1u))
                        data.nodes[index].firstChild = static_cast<std::int32_t>(*child);
                }
                if (childOffsetValid(secondOffset)) {
                    if (const auto child = parseNode(secondOffset, depth + 1u))
                        data.nodes[index].secondChild = static_cast<std::int32_t>(*child);
                }
            } else if (node.faceIndex < 0 ||
                       static_cast<std::size_t>(node.faceIndex) >= mesh.faces.size()) {
                ++issues.invalidFaces;
                data.valid = false;
            }

            active.erase(offset);
            return index;
        };

        data.root = parseNode(rootOffset, 0u);
        if (!data.root) data.valid = false;
        model_.nodes[nodeIndex].aabb = std::move(data);

        const auto issueText = [&](std::size_t count, const char* description) {
            if (count == 0u) return;
            model_.warnings.push_back("AABB node " + nodeName + " contains " +
                std::to_string(count) + " " + description + ".");
        };
        issueText(issues.truncated, "tree record(s) outside the bounded MDL");
        issueText(issues.cycles, "cyclic child link(s)");
        issueText(issues.shared, "reused child link(s)");
        issueText(issues.missingChildren, "internal node(s) with missing children");
        issueText(issues.invalidFaces, "leaf node(s) with invalid face indexes");
        issueText(issues.reversedBounds, "node(s) with reversed bounds");
        issueText(issues.nonFiniteBounds, "non-finite bound component(s)");
    }

    void parseAnimMeshDraws(std::size_t meshOffset, Mesh& mesh) {
        // InputPacked::ResetTriMeshParts, Desktop 0xf535b..0xf53ec: +0x10c
        // contains MDL-relative SOURCE index-pool offsets. +0x118 contains
        // runtime allocation tokens, not serialized pointers to index values.
        const auto nodeOffset=meshOffset-kNodeHeaderSize;
        const auto primitive=payload_->u32(nodeOffset+0x130u,"animmesh primitive");
        if (primitive>9u) throw ModelError("Animmesh primitive exceeds the engine translation table");
        const auto counts=readArrayDefinition(nodeOffset+0x100u,4u,limits_.maximumFaces,"animmesh index counts");
        const auto sources=readArrayDefinition(nodeOffset+0x10cu,4u,limits_.maximumFaces,"animmesh index sources");
        const auto tokens=readArrayDefinition(nodeOffset+0x118u,4u,limits_.maximumFaces,"animmesh index tokens");
        if (!counts || !sources || !tokens || counts->second!=sources->second || counts->second!=tokens->second)
            throw ModelError("Animmesh engine index array dimensions disagree");
        mesh.indices.clear();
        const auto append=[&](std::uint32_t type,std::size_t source,std::uint32_t authoredCount,
                              std::size_t first=0u) {
            if (authoredCount>0x7ffffffeu) throw ModelError("Animmesh signed index count is invalid");
            // Engine loader rounds each allocation/copy count up to even.
            const std::size_t allocated=(static_cast<std::size_t>(authoredCount)+1u)&~std::size_t{1};
            // K1 RenderFlat 0x342533 passes the reset count unchanged.
            // K2 0xcc581 trims triangle batches to complete primitives.
            const std::size_t count=type==3u && model_.game==GameVersion::Kotor2
                ? (allocated/3u)*3u : allocated;
            if (allocated>limits_.maximumAnimMeshIndices ||
                totalAnimMeshIndices_>limits_.maximumAnimMeshIndices-allocated ||
                allocated>std::numeric_limits<std::size_t>::max()/2u)
                throw ModelError("Animmesh index pools exceed the configured aggregate limit");
            if (first>allocated || count>allocated-first)
                throw ModelError("Animmesh trailing draw overruns its engine allocated index pool");
            if (allocated && (source==0u || source==0xffffffffu))
                throw ModelError("Animmesh engine index source is missing");
            if (allocated) payload_->require(source,allocated*2u,"animmesh padded index pool");
            totalAnimMeshIndices_+=allocated;
            mesh.engineDraws.push_back({type,mesh.indices.size(),count});
            for (std::size_t i=0;i<count;++i) {
                const auto value=payload_->u16(source+(first+i)*2u,"animmesh draw index");
                if (value>=mesh.vertices.size()) throw ModelError("Animmesh draw index exceeds its dynamic vertex pool");
                mesh.indices.push_back(value);
            }
        };
        for (std::size_t i=0;i<counts->second;++i)
            append(primitive,payload_->u32(sources->first+i*4u,"animmesh source offset"),
                   payload_->u32(counts->first+i*4u,"animmesh draw count"));
        // RenderFlat's strip branch optionally submits a trailing triangle
        // pool. Its offset is the common MDX-offset word: K1 +0x194
        // (0x3425a1), K2 +0x19c; not either animmesh sample-pool pointer.
        const auto tailCount=payload_->u32(nodeOffset+0x12cu,"animmesh trailing count");
        if (primitive==4u && tailCount) {
            const auto token=payload_->u32(nodeOffset+0x128u,"animmesh trailing token");
            if (token==0xffffffffu) throw ModelError("Animmesh trailing draw has no engine allocation");
            // Unlike regular triangle batches, this call does not round DOWN
            // to a multiple of three. Unconsumed trailing indexes are harmless.
            const auto before=mesh.engineDraws.size();
            append(0u,payload_->u32(nodeOffset+0x124u,"animmesh trailing source"),tailCount,
                   payload_->u32(nodeOffset+(model_.game==GameVersion::Kotor1?0x194u:0x19cu),
                                 "animmesh trailing index offset"));
            mesh.engineDraws[before].primitive=3u;
        }
    }

    void parseMesh(std::size_t offset, std::size_t nodeIndex) {
        if (model_.meshes.size() >= limits_.maximumMeshes) {
            throw ModelError(sourcePrefix(label_) + "Mesh count exceeds the configured limit");
        }
        const auto headerSize = model_.game == GameVersion::Kotor1 ? kK1MeshHeaderSize : kK2MeshHeaderSize;
        payload_->require(offset, headerSize, "triangle-mesh header");

        const auto facesOffset = payload_->u32(offset + 8u, "face array offset");
        auto faceCount = payload_->u32(offset + 12u, "face count");
        const auto headerVertexCount = payload_->u16(offset + 304u, "vertex count");
        if (faceCount > limits_.maximumFaces) {
            throw ModelError(sourcePrefix(label_) + "Mesh face count exceeds the configured limit");
        }
        if (faceCount != 0u) {
            payload_->require(facesOffset, static_cast<std::size_t>(faceCount) * kFaceSize, "face array");
        }

        Mesh mesh;
        mesh.nodeIndex = nodeIndex;
        mesh.animated = model_.nodes[nodeIndex].animmesh.has_value();
        EngineMeshBounds queryBounds;
        queryBounds.box.minimum = {payload_->f32(offset + 20u, "mesh query min x"),
            payload_->f32(offset + 24u, "mesh query min y"),
            payload_->f32(offset + 28u, "mesh query min z")};
        queryBounds.box.maximum = {payload_->f32(offset + 32u, "mesh query max x"),
            payload_->f32(offset + 36u, "mesh query max y"),
            payload_->f32(offset + 40u, "mesh query max z")};
        queryBounds.box.valid = true;
        queryBounds.sphereRadius = payload_->f32(offset + 44u, "mesh query sphere radius");
        queryBounds.sphereCenter = {payload_->f32(offset + 48u, "mesh query center x"),
            payload_->f32(offset + 52u, "mesh query center y"),
            payload_->f32(offset + 56u, "mesh query center z")};
        mesh.engineBounds = queryBounds;
        mesh.rawMeshHeader.reserve(headerSize);
        for (std::size_t byte = 0u; byte < headerSize; ++byte)
            mesh.rawMeshHeader.push_back(payload_->u8(offset + byte, "mesh header byte"));
        const auto materialFloat = [&](std::size_t at, const char* name) {
            const auto value = payload_->f32(offset + at, name);
            if (finite(value)) return value;
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                " has non-finite " + name + "; zero was substituted (raw bytes retained).");
            return 0.0f;
        };
        mesh.uvAnimation.enabledRaw = payload_->u32(offset + 232u, "animate UV flag");


        mesh.uvAnimation.direction = {payload_->f32(offset + 236u, "UV direction X"),
                                      payload_->f32(offset + 240u, "UV direction Y")};
        mesh.uvAnimation.jitter = payload_->f32(offset + 244u, "UV jitter");
        mesh.uvAnimation.jitterSpeed = payload_->f32(offset + 248u, "UV jitter speed");
        mesh.textureCount = payload_->u16(offset + 306u, "texture count");
        mesh.rotateTexture = payload_->u8(offset + 309u, "rotate texture flag") != 0u;
        mesh.beaming = payload_->u8(offset + 312u, "beaming flag") != 0u;
        if (model_.game == GameVersion::Kotor2) {
            // Both 64-bit K2 conversion constructors copy these exact widths.
            // The hologram flag is a BYTE, not the surrounding padded dword.
            mesh.k2Material = K2MeshMaterialFields{
                payload_->u8(offset + 314u, "dirt enabled flag"),
                payload_->i16(offset + 316u, "dirt texture"),
                payload_->i16(offset + 318u, "dirt worldspace"),
                payload_->u8(offset + 320u, "hologram do-not-draw flag")};
        }
        mesh.totalArea = materialFloat(model_.game == GameVersion::Kotor2 ? 324u : 316u,
                                        "mesh total area");
        mesh.texture0 = normalizedResourceName(payload_->fixedString(offset + 88u, 32u, "diffuse texture"));
        mesh.texture1 = normalizedResourceName(payload_->fixedString(offset + 120u, 32u, "secondary texture"));
        mesh.diffuse = safeVec3(payload_->vec3(offset + 60u, "mesh diffuse color"), model_.warnings,
                                "Mesh on " + model_.nodes[nodeIndex].name + " diffuse color");
        mesh.ambient = safeVec3(payload_->vec3(offset + 72u, "mesh ambient color"), model_.warnings,
                                "Mesh on " + model_.nodes[nodeIndex].name + " ambient color");
        mesh.transparencyHint = payload_->u32(offset + 84u, "transparency hint");
        mesh.mdxRowSize = payload_->u32(offset + 252u, "MDX row size");
        mesh.mdxBitmap = payload_->u32(offset + 256u, "MDX channel bitmap");
        const auto mdxVertexOffset = payload_->u32(offset + 260u, "MDX vertex channel offset");
        const auto mdxNormalOffset = payload_->u32(offset + 264u, "MDX normal channel offset");
        mesh.mdxNormalOffset = mdxNormalOffset;
        const auto mdxTex0Offset = payload_->u32(offset + 272u, "MDX texture-0 channel offset");
        const auto mdxTex1Offset = payload_->u32(offset + 276u, "MDX texture-1 channel offset");
        mesh.mdxColorOffset = payload_->u32(offset + 268u, "MDX color channel offset");
        mesh.mdxTexcoordOffsets = {{mdxTex0Offset, mdxTex1Offset,
            payload_->u32(offset + 280u, "MDX texture-2 channel offset"),
            payload_->u32(offset + 284u, "MDX texture-3 channel offset")}};
        mesh.mdxTangentOffset = payload_->u32(offset + 288u, "MDX tangent-basis offset");
        mesh.lightmapped = payload_->u8(offset + 308u, "lightmap flag") != 0u;
        mesh.backgroundGeometry = payload_->u8(offset + 310u, "background flag") != 0u;
        mesh.castsShadow = payload_->u8(offset + 311u, "shadow flag") != 0u;
        mesh.render = payload_->u8(offset + 313u, "render flag") != 0u;
        mesh.skinned = (model_.nodes[nodeIndex].flags & kNodeSkin) != 0u;
        if ((model_.nodes[nodeIndex].flags & kNodeDangly) != 0u) {
            DanglyMeshData dangly;
            dangly.restPositions.emplace();
            // K1 ResetDangly 0x321ae4: the same 28-byte extension follows
            // the eight-byte-shorter mesh header (node +0x19c..+0x1b4).
            if (!payload_->contains(offset + headerSize, kDanglyHeaderSize)) {
                throw ModelError("Dangly mesh " + model_.nodes[nodeIndex].name +
                                 " has a truncated engine extension.");
            } else {
                const auto constraints = readArrayDefinition(
                    offset + headerSize, 4u, limits_.maximumVertices,
                    "Dangly mesh " + model_.nodes[nodeIndex].name + " constraint array");
                if (constraints) {
                    dangly.constraints.reserve(constraints->second);
                    for (std::size_t index = 0u; index < constraints->second; ++index) {
                        dangly.constraints.push_back(payload_->f32(
                            constraints->first + index * 4u, "dangly constraint"));
                    }
                }
                dangly.displacement = payload_->f32(
                    offset + headerSize + 12u, "dangly displacement");
                dangly.tightness = payload_->f32(
                    offset + headerSize + 16u, "dangly tightness");
                dangly.period = payload_->f32(
                    offset + headerSize + 20u, "dangly period");






                const auto restOffset = payload_->u32(
                    offset + headerSize + 24u, "dangly rest-position offset");
                if (restOffset != 0u && restOffset != 0xffffffffu &&
                    payload_->contains(restOffset, static_cast<std::size_t>(headerVertexCount)*12u)) {
                    dangly.restPositions->reserve(headerVertexCount);
                    for (std::size_t index=0; index<headerVertexCount; ++index) {
                        dangly.restPositions->push_back(payload_->vec3(
                            restOffset+index*12u, "dangly rest position"));
                    }
                } else if (headerVertexCount != 0u) {
                    throw ModelError("Dangly mesh " + model_.nodes[nodeIndex].name +
                        " has no readable engine rest-position pool.");
                }
            }
            mesh.dangly = std::move(dangly);
        }
        if (mesh.skinned) {
            const auto skinOffset = offset - (model_.game == GameVersion::Kotor1 ? 8u : 0u);
            mesh.skinBoneMapSize = model_.game == GameVersion::Kotor1 ? 17u : 16u;
            if (payload_->contains(skinOffset + kK2SkinWeightMdxOffset, 8u)) {
                mesh.mdxWeightOffset = payload_->u32(
                    skinOffset + kK2SkinWeightMdxOffset, "skin weight MDX channel offset");
                mesh.mdxBoneIndexOffset = payload_->u32(
                    skinOffset + kK2SkinBoneMdxOffset, "skin bone-index MDX channel offset");
            }

            const auto readSkinArray = [&](std::size_t relativeOffset, std::size_t stride,
                                           const char* description)
                -> std::optional<std::pair<std::size_t, std::size_t>> {
                const auto definition = skinOffset + relativeOffset;
                if (!payload_->contains(definition, kArrayDefinitionSize)) {
                    model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                        " has no complete " + description + " array definition.");
                    return std::nullopt;
                }
                const auto dataOffset = payload_->u32(definition, description);
                const auto count = payload_->u32(definition + 4u, description);
                const auto capacity = payload_->u32(definition + 8u, description);
                if (count > limits_.maximumNodes) {
                    throw ModelError(sourcePrefix(label_) + "Skin " + description +
                                     " count exceeds the configured node limit");
                }
                if (capacity != 0u && count > capacity) {
                    model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                        " has a " + description + " count larger than its capacity; the array was ignored.");
                    return std::nullopt;
                }
                if (count == 0u) return std::pair<std::size_t, std::size_t>{0u, 0u};
                if (dataOffset == 0u || dataOffset == 0xffffffffu ||
                    !payload_->contains(dataOffset, static_cast<std::size_t>(count) * stride)) {
                    model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                        " declares " + description + " data outside the MDL payload; the array was ignored.");
                    return std::nullopt;
                }
                return std::pair<std::size_t, std::size_t>{
                    static_cast<std::size_t>(dataOffset), static_cast<std::size_t>(count)};
            };

            const auto inverseQuaternions = readSkinArray(
                kK2SkinInverseQuaternionArray, 16u, "inverse-bind quaternion");
            if (inverseQuaternions) {
                mesh.inverseBindOrientations.reserve(inverseQuaternions->second);
                for (std::size_t index = 0u; index < inverseQuaternions->second; ++index) {
                    const auto record = inverseQuaternions->first + index * 16u;
                    // MdlNodeSkin32 stores Quaternion as w,x,y,z. ResetSkin
                    // preserves the words, and RenderSkinnedVP multiplies
                    // them directly (K1 0x365644, K2 0x1094f1..0x109618).
                    mesh.inverseBindOrientations.push_back(
                        {payload_->f32(record + 4u, "inverse-bind quaternion x"),
                         payload_->f32(record + 8u, "inverse-bind quaternion y"),
                         payload_->f32(record + 12u, "inverse-bind quaternion z"),
                         payload_->f32(record, "inverse-bind quaternion w")});
                }
            }

            const auto inverseTranslations = readSkinArray(
                kK2SkinInverseTranslationArray, 12u, "inverse-bind translation");
            if (inverseTranslations) {
                mesh.inverseBindTranslations.reserve(inverseTranslations->second);
                for (std::size_t index = 0u; index < inverseTranslations->second; ++index) {
                    const auto record = inverseTranslations->first + index * 12u;
                    // ResetSkin preserves these stored IEEE words, just as
                    // it preserves the quaternion pool. Substituting zeros
                    // changes initialized engine deformation inputs.
                    mesh.inverseBindTranslations.push_back(
                        payload_->vec3(record, "inverse-bind translation"));
                }
            }

            const auto boneConstants = readSkinArray(
                kK2SkinBoneConstantsArray, 4u, "bone-constant index");
            if (boneConstants) {
                mesh.boneConstantIndices.reserve(boneConstants->second);
                for (std::size_t index = 0u; index < boneConstants->second; ++index) {
                    mesh.boneConstantIndices.push_back(payload_->i32(
                        boneConstants->first + index * 4u, "bone-constant index"));
                }
            }

            if (!mesh.inverseBindOrientations.empty() ||
                !mesh.inverseBindTranslations.empty()) {
                if (mesh.inverseBindOrientations.size() ==
                        mesh.inverseBindTranslations.size() &&
                    !mesh.inverseBindOrientations.empty()) {
                    mesh.hasInverseBindPose = true;
                } else {
                    model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                        " has mismatched inverse-bind quaternion and translation counts; CPU deformation was disabled.");
                }
            }

            if (payload_->contains(skinOffset + kK2SkinBoneMapOffset,
                                   mesh.skinBoneMapSize * sizeof(std::int16_t))) {
                for (std::size_t index = 0u; index < mesh.skinBoneMapSize; ++index) {
                    mesh.skinBoneMap[index] = payload_->i16(
                        skinOffset + kK2SkinBoneMapOffset + index * sizeof(std::int16_t),
                        "skin bone map entry");
                }
                mesh.hasSkinBoneMap = true;
            }
        }
        const auto mdxDataOffset = payload_->u32(
            offset + (model_.game == GameVersion::Kotor1 ? 324u : 332u), "MDX data offset");
        const auto mdlVerticesOffset = payload_->u32(
            offset + (model_.game == GameVersion::Kotor1 ? 328u : 336u), "MDL vertex offset");

        mesh.faces.reserve(faceCount);
        std::uint32_t requiredVertices = headerVertexCount;
        for (std::uint32_t index = 0u; index < faceCount; ++index) {
            const auto faceOffset = static_cast<std::size_t>(facesOffset) + static_cast<std::size_t>(index) * kFaceSize;
            MeshFace face;
            face.normal = payload_->vec3(faceOffset, "face normal");
            face.planeDistance = payload_->f32(faceOffset + 12u, "face plane distance");
            face.surfaceMaterial = payload_->u32(faceOffset + 16u, "face surface material");
            face.adjacentFaces[0] = payload_->i16(faceOffset + 20u, "face adjacent edge 0");
            face.adjacentFaces[1] = payload_->i16(faceOffset + 22u, "face adjacent edge 1");
            face.adjacentFaces[2] = payload_->i16(faceOffset + 24u, "face adjacent edge 2");
            face.indices[0] = payload_->i16(faceOffset + 26u, "face vertex 0");
            face.indices[1] = payload_->i16(faceOffset + 28u, "face vertex 1");
            face.indices[2] = payload_->i16(faceOffset + 30u, "face vertex 2");
            for (const auto vertexIndex : face.indices) {
                if (vertexIndex >= 0) {
                    requiredVertices = std::max(requiredVertices,
                        static_cast<std::uint32_t>(vertexIndex) + 1u);
                }
            }
            mesh.faces.push_back(face);
        }
        if (requiredVertices > limits_.maximumVertices || totalVertices_ > limits_.maximumVertices - requiredVertices) {
            throw ModelError(sourcePrefix(label_) + "Model vertex count exceeds the configured limit");
        }
        totalVertices_ += requiredVertices;
        if (mesh.animated && requiredVertices != headerVertexCount)
            throw ModelError(sourcePrefix(label_) + "Animmesh face exceeds the engine sample-pool vertex count");
        mesh.vertices.resize(requiredVertices);
        if (mesh.dangly && mesh.dangly->restPositions &&
            mesh.dangly->restPositions->size() != requiredVertices)
            throw ModelError("Dangly faces exceed the engine vertex/rest-position count");

        {
            // ResetTriMeshParts (Desktop 0xf5099..0xf510d) relocates the MDL CPU
            // pool. K1 ResetTriMeshParts (0x320bdc) uses the corresponding
            // node +0x198 pool; K1 Project reads it at 0x34115e. Both project
            // authored MDL positions, not the MDX drawing stream below.
            mesh.cpuPositions.emplace();
            if (mdlVerticesOffset != 0u && mdlVerticesOffset != 0xffffffffu &&
                payload_->contains(mdlVerticesOffset, static_cast<std::size_t>(headerVertexCount)*12u)) {
                mesh.cpuPositions->reserve(headerVertexCount);
                for (std::size_t index=0; index<headerVertexCount; ++index)
                    mesh.cpuPositions->push_back(payload_->vec3(mdlVerticesOffset+index*12u,
                                                               "engine CPU vertex"));
            } else if (headerVertexCount != 0u && !mesh.skinned) {
                model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                    " has no readable engine CPU vertex pool; CPU shadow projection is unavailable.");
            }
        }

        const bool mdxPositions = !mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
            (mesh.mdxBitmap & kMdxVertex) != 0u && validMdxChannel(mdxVertexOffset, mesh.mdxRowSize, 12u);
        const bool mdlPositions = mdlVerticesOffset != 0u && mdlVerticesOffset != 0xFFFFFFFFu &&
            payload_->contains(mdlVerticesOffset, static_cast<std::size_t>(requiredVertices) * 12u);
        if (!mdxPositions && !mdlPositions && requiredVertices != 0u) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                                      " has no readable vertex-position source.");
        }

        bool mdxRowsValid = mdxPositions;
        if (mdxRowsValid && requiredVertices != 0u) {
            const std::uint64_t finalRow = static_cast<std::uint64_t>(mdxDataOffset) +
                static_cast<std::uint64_t>(requiredVertices - 1u) * mesh.mdxRowSize;
            mdxRowsValid = finalRow <= mdxBytes_.size() &&
                mdxVertexOffset <= mdxBytes_.size() - static_cast<std::size_t>(finalRow) &&
                12u <= mdxBytes_.size() - static_cast<std::size_t>(finalRow) - mdxVertexOffset;
            if (!mdxRowsValid && mdlPositions) {
                model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                                          " has truncated MDX rows; MDL positions were used.");
            }
        }
        const bool mdxNormalsReadable = !mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
            (mesh.mdxBitmap & kMdxNormal) != 0u &&
            validMdxChannel(mdxNormalOffset, mesh.mdxRowSize, 12u) &&
            (requiredVertices == 0u || [&] {
                const std::uint64_t finalRow = static_cast<std::uint64_t>(mdxDataOffset) +
                    static_cast<std::uint64_t>(requiredVertices - 1u) * mesh.mdxRowSize;
                return finalRow <= mdxBytes_.size() &&
                    mdxNormalOffset <= mdxBytes_.size() - static_cast<std::size_t>(finalRow) &&
                    12u <= mdxBytes_.size() - static_cast<std::size_t>(finalRow) - mdxNormalOffset;
            }());
        if (mesh.animated && static_cast<std::int32_t>(mdxNormalOffset)>=0 &&
            !mdxNormalsReadable && requiredVertices != 0u)
            throw ModelError(sourcePrefix(label_) + "Animmesh has no readable enabled MDX normal channel");
        if ((mesh.mdxBitmap & kMdxNormal) != 0u && !mdxNormalsReadable && requiredVertices != 0u) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                                      " has an unreadable MDX normal channel; normals were generated.");
        }

        const bool skinChannelsReadable = mesh.skinned &&
            !mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
            validMdxChannel(mesh.mdxWeightOffset, mesh.mdxRowSize, 16u) &&
            validMdxChannel(mesh.mdxBoneIndexOffset, mesh.mdxRowSize, 8u) &&
            (requiredVertices == 0u || [&] {
                const std::uint64_t finalRow = static_cast<std::uint64_t>(mdxDataOffset) +
                    static_cast<std::uint64_t>(requiredVertices - 1u) * mesh.mdxRowSize;
                if (finalRow > mdxBytes_.size()) return false;
                const auto row = static_cast<std::size_t>(finalRow);
                return mesh.mdxWeightOffset <= mdxBytes_.size() - row &&
                    16u <= mdxBytes_.size() - row - mesh.mdxWeightOffset &&
                    mesh.mdxBoneIndexOffset <= mdxBytes_.size() - row &&
                    8u <= mdxBytes_.size() - row - mesh.mdxBoneIndexOffset;
            }());
        if (skinChannelsReadable) {
            mesh.hasSkinChannels = true;
            mesh.skinInfluences.resize(requiredVertices);
        } else if (mesh.skinned && requiredVertices != 0u) {
            model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                " has no complete packed-verified MDX weight/bone-index channels.");
        }

        const auto channelReadable = [&](std::uint32_t channel, std::size_t bytes) {
            if (mdxBytes_.empty() || mesh.mdxRowSize == 0u || requiredVertices == 0u ||
                !validMdxChannel(channel, mesh.mdxRowSize, bytes)) return false;
            const std::uint64_t last = static_cast<std::uint64_t>(mdxDataOffset) +
                static_cast<std::uint64_t>(requiredVertices - 1u) * mesh.mdxRowSize;
            return last <= mdxBytes_.size() && channel <= mdxBytes_.size() - last &&
                   bytes <= mdxBytes_.size() - last - channel;
        };
        const bool tangentsReadable = channelReadable(mesh.mdxTangentOffset, 36u);
        mesh.hasVertexColors = (mesh.mdxBitmap & 0x40u) != 0u &&
            channelReadable(mesh.mdxColorOffset, 4u);
        const bool uv2Readable = (mesh.mdxBitmap & 0x08u) != 0u &&
            channelReadable(mesh.mdxTexcoordOffsets[2], 8u);
        const bool uv3Readable = (mesh.mdxBitmap & 0x10u) != 0u &&
            channelReadable(mesh.mdxTexcoordOffsets[3], 8u);
        bool invalidTangent = false;
        bool invalidUv = false;
        if (!tangentsReadable && mesh.mdxTangentOffset != 0xffffffffu &&
            (mesh.mdxBitmap & 0x80u) != 0u && requiredVertices != 0u) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                " has an invalid or truncated MDX tangent channel; geometry was retained.");
        }
        ByteView mdx(mdxBytes_.data(), mdxBytes_.size(), label_.empty() ? "MDX" : label_ + " MDX", job_);
        bool warnedNonFiniteSkinWeight = false;
        for (std::uint32_t index = 0u; index < requiredVertices; ++index) {
            auto& vertex = mesh.vertices[index];
            const auto row = static_cast<std::size_t>(mdxDataOffset) +
                             static_cast<std::size_t>(index) * mesh.mdxRowSize;
            if (mdxRowsValid) {
                vertex.position = safeVec3(mdx.vec3(row + mdxVertexOffset, "MDX position"), model_.warnings,
                                           "Mesh vertex position");
            } else if (mdlPositions) {
                vertex.position = safeVec3(payload_->vec3(static_cast<std::size_t>(mdlVerticesOffset) +
                    static_cast<std::size_t>(index) * 12u, "MDL position"), model_.warnings,
                    "Mesh vertex position");
            }
            if (mdxNormalsReadable) {
                const auto authored = mdx.vec3(row + mdxNormalOffset, "MDX normal");
                // All four SetInterleavedBuffer implementations pass the MDX
                // channel directly to glNormalPointer. K2's rigid ARB/NV
                // programs also retain its magnitude. Draw-time state and
                // programs own normalization; inspection geometry separately
                // constructs unit normals in prepareRenderGeometry.
                vertex.normal = authored;
            }
            if (!mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
                (mesh.mdxBitmap & kMdxTex0) != 0u &&
                validMdxChannel(mdxTex0Offset, mesh.mdxRowSize, 8u) &&
                row <= mdxBytes_.size() && mdxTex0Offset <= mdxBytes_.size() - row &&
                8u <= mdxBytes_.size() - row - mdxTex0Offset) {
                vertex.texcoord0 = mdx.vec2(row + mdxTex0Offset, "MDX texture coordinate 0");
            }
            if (!mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
                (mesh.mdxBitmap & kMdxTex1) != 0u &&
                validMdxChannel(mdxTex1Offset, mesh.mdxRowSize, 8u) &&
                row <= mdxBytes_.size() && mdxTex1Offset <= mdxBytes_.size() - row &&
                8u <= mdxBytes_.size() - row - mdxTex1Offset) {
                vertex.texcoord1 = mdx.vec2(row + mdxTex1Offset, "MDX texture coordinate 1");
            }
            if (mesh.hasVertexColors) {
                for (std::size_t c = 0u; c < vertex.color.size(); ++c)
                    vertex.color[c] = mdx.u8(row + mesh.mdxColorOffset + c, "MDX color");
            }
            if (uv2Readable) vertex.texcoord2 = mdx.vec2(
                row + mesh.mdxTexcoordOffsets[2], "MDX texture coordinate 2");
            if (uv3Readable) vertex.texcoord3 = mdx.vec2(
                row + mesh.mdxTexcoordOffsets[3], "MDX texture coordinate 3");
            for (auto* uv : {&vertex.texcoord0, &vertex.texcoord1,
                             &vertex.texcoord2, &vertex.texcoord3}) {
                if (!finite(uv->x) || !finite(uv->y)) {
                    *uv = {}; invalidUv = true;
                }
            }
            if (tangentsReadable) {
                const auto at = row + mesh.mdxTangentOffset;
                auto& basis = vertex.tangentBasis;
                basis.bitangent = mdx.vec3(at, "MDX bitangent");
                basis.tangent = mdx.vec3(at + 12u, "MDX tangent");
                basis.normal = mdx.vec3(at + 24u, "MDX basis normal");
                const auto goodVector = [](const Vec3& v) {
                    const double length2 = static_cast<double>(v.x) * v.x +
                        static_cast<double>(v.y) * v.y + static_cast<double>(v.z) * v.z;
                    return finite(v.x) && finite(v.y) && finite(v.z) &&
                           std::isfinite(length2) && length2 > 1.0e-20;
                };
                const auto good = goodVector(basis.bitangent) && goodVector(basis.tangent) &&
                                  goodVector(basis.normal);
                if (good) {
                    const auto b = normalize(basis.bitangent);
                    const auto t = normalize(basis.tangent);
                    const auto n = normalize(basis.normal);
                    const auto c = cross(t, b);
                    const float determinant = c.x * n.x + c.y * n.y + c.z * n.z;
                    basis.valid = finite(determinant) && std::abs(determinant) > 1.0e-5f;
                }
                if (basis.valid) ++mesh.validTangentVertices;
                else {
                    invalidTangent = true;
                    // Non-finite input must never reach a GPU upload.
                    for (auto* v : {&basis.bitangent, &basis.tangent, &basis.normal})
                        if (!finite(v->x) || !finite(v->y) || !finite(v->z)) *v = {};
                }
            }
            if (skinChannelsReadable) {
                auto& influence = mesh.skinInfluences[index];
                for (std::size_t component = 0u; component < influence.weights.size(); ++component) {
                    auto value = mdx.f32(row + mesh.mdxWeightOffset + component * 4u,
                                         "MDX skin weight");
                    if (!finite(value)) {
                        value = 0.0f;
                        warnedNonFiniteSkinWeight = true;
                    }
                    influence.weights[component] = value;
                    influence.boneIndices[component] = mdx.u16(
                        row + mesh.mdxBoneIndexOffset + component * 2u,
                        "MDX skin bone index");
                }
            }
        }
        if (invalidTangent) model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
            " contains invalid tangent bases; affected vertices need generated tangents.");
        if (invalidUv) model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
            " contains non-finite texture coordinates; zero was substituted.");
        if (warnedNonFiniteSkinWeight) {
            model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                                      " contains non-finite weights; zero was substituted.");
        }

        std::vector<Vec3> generatedNormals(requiredVertices);
        mesh.indices.reserve(mesh.faces.size() * 3u);
        std::size_t skippedFaces = 0u;
        for (const auto& face : mesh.faces) {
            job_.check();
            if (face.indices[0] < 0 || face.indices[1] < 0 || face.indices[2] < 0 ||
                static_cast<std::uint32_t>(face.indices[0]) >= requiredVertices ||
                static_cast<std::uint32_t>(face.indices[1]) >= requiredVertices ||
                static_cast<std::uint32_t>(face.indices[2]) >= requiredVertices) {
                ++skippedFaces;
                continue;
            }
            for (const auto index : face.indices)
                mesh.indices.push_back(static_cast<std::uint32_t>(index));
            const auto& a = mesh.vertices[static_cast<std::size_t>(face.indices[0])].position;
            const auto& b = mesh.vertices[static_cast<std::size_t>(face.indices[1])].position;
            const auto& c = mesh.vertices[static_cast<std::size_t>(face.indices[2])].position;
            const auto normal = cross(subtract(b, a), subtract(c, a));
            generatedNormals[static_cast<std::size_t>(face.indices[0])] = add(
                generatedNormals[static_cast<std::size_t>(face.indices[0])], normal);
            generatedNormals[static_cast<std::size_t>(face.indices[1])] = add(
                generatedNormals[static_cast<std::size_t>(face.indices[1])], normal);
            generatedNormals[static_cast<std::size_t>(face.indices[2])] = add(
                generatedNormals[static_cast<std::size_t>(face.indices[2])], normal);
        }
        if (skippedFaces != 0u) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name + " skipped " +
                                      std::to_string(skippedFaces) + " face(s) with invalid indices.");
        }
        if (!mdxNormalsReadable) {
            for (std::size_t index = 0u; index < mesh.vertices.size(); ++index) {
                mesh.vertices[index].normal = normalize(generatedNormals[index]);
            }
        }
        if (mesh.animated) parseAnimMeshDraws(offset,mesh);
        {
            // One compiled source serves engine topology, CPU/GPU skin and
            // LightPartTriMesh's independent raw position/normal/color reads.
            if(!engineMeshMdl_) {
                engineMeshMdl_=std::make_shared<const std::vector<std::uint8_t>>(
                    mdlBytes_.begin()+static_cast<std::ptrdiff_t>(payloadOffset_),mdlBytes_.end());
            }
            if(!engineSkinMdx_)
                engineSkinMdx_=std::make_shared<const std::vector<std::uint8_t>>(mdxBytes_);
            const EngineMeshSource input{engineMeshMdl_,engineSkinMdx_,
                offset-kNodeHeaderSize,mdxDataOffset,mdxVertexOffset,model_.game};
            mesh.enginePrelightInput=std::make_shared<const EngineMeshSource>(input);
            mesh.enginePrelightVertexCount=headerVertexCount;
            if(mesh.skinned)
                mesh.engineCpuSkin=std::make_shared<const EngineCpuSkinInput>(input);
            if(model_.game==GameVersion::Kotor1 && !mesh.animated) {
                auto topology=decodeEngineMeshTopology(input,false,job_);
                mesh.engineDrawIndices=std::move(topology.indices);
                mesh.engineDraws=std::move(topology.draws);
            }
        }
        if (mesh.dangly && !mesh.dangly->constraints.empty() &&
            mesh.dangly->constraints.size() != mesh.vertices.size()) {
            model_.warnings.push_back("Dangly mesh " + model_.nodes[nodeIndex].name +
                " has " + std::to_string(mesh.dangly->constraints.size()) +
                " constraints for " + std::to_string(mesh.vertices.size()) +
                " vertices; missing entries are treated as pinned and extras are ignored.");
        }
        if (mesh.skinned && model_.game == GameVersion::Kotor2 &&
            !(mesh.hasSkinChannels && mesh.hasSkinBoneMap && mesh.hasInverseBindPose)) {
            model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                " does not contain every packed-verified K2 skinning input; its stored geometry will be used as a fallback.");
        }
        if (requiredVertices != headerVertexCount) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                                      " required more vertices than its header declared; face indices were authoritative.");
        }

        parseAabbTree(offset, nodeIndex, mesh);
        parseLightsaber(offset, nodeIndex, mesh);
        model_.nodes[nodeIndex].mesh = model_.meshes.size();
        model_.meshes.push_back(std::move(mesh));
    }

    void calculateWorldTransformsAndBounds() {
        const auto pose = bindPose(model_, job_);
        model_.worldTransforms = pose.worldTransforms;
        model_.bounds = pose.bounds;
    }

    const std::vector<std::uint8_t>& mdlBytes_;
    const std::vector<std::uint8_t>& mdxBytes_;
    std::string label_;
    const ParseLimits& limits_;
    const neoshared::bif::JobControl& job_;
    ByteView whole_;
    std::unique_ptr<ByteView> payload_;
    std::size_t payloadOffset_{};
    std::shared_ptr<const std::vector<std::uint8_t>> engineMeshMdl_,engineSkinMdx_;
    Model model_;
    std::uint32_t rootNodeOffset_{};
    std::uint32_t declaredNodeCount_{};
    std::uint32_t animationsOffset_{};
    std::uint32_t namesOffset_{};
    std::uint32_t namesCount_{};
    std::vector<std::string> names_;
    std::unordered_map<std::uint32_t, std::size_t> nodeByOffset_;
    std::size_t totalVertices_{};
    std::size_t totalAnimationNodes_{};
    std::size_t totalAnimMeshSamples_{};
    std::size_t totalAnimMeshIndices_{};
    std::size_t totalAnimationEvents_{};
    std::size_t totalControllers_{};
    std::size_t totalControllerValues_{};
};

Quaternion normalizedQuaternion(Quaternion value) noexcept {
    if (!finite(value.x) || !finite(value.y) || !finite(value.z) || !finite(value.w)) return {};
    const double lengthSquared = static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z +
        static_cast<double>(value.w) * value.w;
    if (lengthSquared < 1.0e-20 || !std::isfinite(lengthSquared)) return {};
    const auto reciprocal = static_cast<float>(1.0 / std::sqrt(lengthSquared));
    value.x *= reciprocal;
    value.y *= reciprocal;
    value.z *= reciprocal;
    value.w *= reciprocal;
    return value;
}

Quaternion multiplyQuaternion(const Quaternion& left, const Quaternion& right,
                               bool engineDesktop = false) noexcept {
    // K1 operator* 0x3a4024, used by GatherBones 0x36181f; K2
    // GatherBones 0x104b22 uses the same unnormalized product/order.
    const auto value = engineQuaternionProduct(left, right);
    return engineDesktop ? value : normalizedQuaternion(value);
}

Vec3 rotateVector(const Quaternion& orientation, const Vec3& value,
                   bool engineDesktop = false) noexcept {
    if (engineDesktop) {
        // Engine default quaternionidentityoptimization fast path; otherwise
        // multiply by the raw quaternion's matrix (K1 0x3a3448, K2 0x104b37).
        return engineQuaternionRotate(orientation, value);
    }
    const auto quaternion = normalizedQuaternion(orientation);
    const Vec3 axis{quaternion.x, quaternion.y, quaternion.z};
    const auto twiceCross = cross(axis, value);
    const Vec3 twice{twiceCross.x * 2.0f, twiceCross.y * 2.0f, twiceCross.z * 2.0f};
    const auto correction = cross(axis, twice);
    return {value.x + quaternion.w * twice.x + correction.x,
            value.y + quaternion.w * twice.y + correction.y,
            value.z + quaternion.w * twice.z + correction.z};
}

SkinSeamStatistics processK2SkinSeamsImpl(Model& model, const neoshared::bif::JobControl& job) {
    job.check();
    SkinSeamStatistics result;
    if (model.game != GameVersion::Kotor2) {
        model.skinSeams = result;
        return result;
    }
    result.processed = true;

    std::vector<std::size_t> skinMeshes;
    skinMeshes.reserve(model.meshes.size());
    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        if (mesh.skinned && mesh.nodeIndex < model.nodes.size())
            skinMeshes.push_back(meshIndex);
    }
    result.skinMeshes = skinMeshes.size();
    if (skinMeshes.size() < 2u) {
        model.skinSeams = result;
        return result;
    }

    // ProcessSkinSeams uses part positions and quaternions, not uniform scale.
    // Resolve them recursively so malformed/non-topological node ordering does
    // not change the comparison space.
    std::vector<Vec3> worldPositions(model.nodes.size());
    std::vector<Quaternion> worldOrientations(model.nodes.size());
    std::vector<std::uint8_t> state(model.nodes.size(), 0u);
    std::function<void(std::size_t)> resolveNode = [&](std::size_t index) {
        job.check();
        if (index >= model.nodes.size() || state[index] == 2u) return;
        if (state[index] == 1u) {
            // A cyclic hierarchy cannot produce a valid runtime part tree.
            // Match the parser's safe fallback by treating this node as a root.
            worldPositions[index] = model.nodes[index].position;
            worldOrientations[index] = normalizedQuaternion(
                model.nodes[index].orientation);
            state[index] = 2u;
            return;
        }
        state[index] = 1u;
        const auto localOrientation = normalizedQuaternion(
            model.nodes[index].orientation);
        const auto parent = model.nodes[index].parent;
        if (parent && *parent < model.nodes.size() && *parent != index) {
            resolveNode(*parent);
            worldOrientations[index] = multiplyQuaternion(
                worldOrientations[*parent], localOrientation);
            worldPositions[index] = add(
                worldPositions[*parent],
                rotateVector(worldOrientations[*parent],
                             model.nodes[index].position));
        } else {
            worldPositions[index] = model.nodes[index].position;
            worldOrientations[index] = localOrientation;
        }
        state[index] = 2u;
    };
    for (std::size_t index = 0u; index < model.nodes.size(); ++index)
        resolveNode(index);

    const auto inverseRotate = [](const Quaternion& orientation,
                                  const Vec3& value) {
        const auto unit = normalizedQuaternion(orientation);
        return rotateVector({-unit.x, -unit.y, -unit.z, unit.w}, value);
    };
    constexpr float seamTolerance = 0.0010000000474974513f;
    struct NormalReference {
        std::size_t meshIndex{};
        std::size_t vertexIndex{};
    };

    for (std::size_t ownerPosition = 0u;
         ownerPosition + 1u < skinMeshes.size(); ++ownerPosition) {
        job.update(ownerPosition, skinMeshes.size(), "Processing model skin seams");
        const auto currentMeshIndex = skinMeshes[ownerPosition];
        auto& currentMesh = model.meshes[currentMeshIndex];
        const auto currentNode = currentMesh.nodeIndex;
        const auto& currentOrientation = worldOrientations[currentNode];
        const auto& currentPosition = worldPositions[currentNode];

        for (std::size_t currentVertex = 0u;
             currentVertex < currentMesh.vertices.size(); ++currentVertex) {
            job.check();
            const auto worldPoint = add(
                currentPosition,
                rotateVector(currentOrientation,
                             currentMesh.vertices[currentVertex].position));
            Vec3 normalSum = rotateVector(
                currentOrientation,
                currentMesh.vertices[currentVertex].normal);
            std::vector<NormalReference> matches;
            matches.push_back({currentMeshIndex, currentVertex});

            for (std::size_t candidatePosition = ownerPosition + 1u;
                 candidatePosition < skinMeshes.size(); ++candidatePosition) {
                const auto candidateMeshIndex = skinMeshes[candidatePosition];
                auto& candidateMesh = model.meshes[candidateMeshIndex];
                const auto candidateNode = candidateMesh.nodeIndex;
                const auto& candidateOrientation = worldOrientations[candidateNode];
                const auto& candidatePositionWorld = worldPositions[candidateNode];
                for (std::size_t candidateVertex = 0u;
                     candidateVertex < candidateMesh.vertices.size(); ++candidateVertex) {
                    if ((candidateVertex & 1023u) == 0u) job.check();
                    ++result.candidateComparisons;
                    const auto candidatePoint = add(
                        candidatePositionWorld,
                        rotateVector(candidateOrientation,
                                     candidateMesh.vertices[candidateVertex].position));
                    const auto delta = subtract(candidatePoint, worldPoint);
                    if (std::abs(delta.x) > seamTolerance ||
                        std::abs(delta.y) > seamTolerance ||
                        std::abs(delta.z) > seamTolerance) {
                        continue;
                    }
                    normalSum = add(normalSum, rotateVector(
                        candidateOrientation,
                        candidateMesh.vertices[candidateVertex].normal));
                    matches.push_back({candidateMeshIndex, candidateVertex});
                }
            }

            if (matches.size() <= 1u) continue;
            ++result.matchedGroups;
            const float reciprocal = 1.0f / static_cast<float>(matches.size());
            const auto averagedWorldNormal = normalize({
                normalSum.x * reciprocal,
                normalSum.y * reciprocal,
                normalSum.z * reciprocal});
            for (const auto& match : matches) {
                auto& mesh = model.meshes[match.meshIndex];
                const auto nodeIndex = mesh.nodeIndex;
                mesh.vertices[match.vertexIndex].normal = normalize(
                    inverseRotate(worldOrientations[nodeIndex],
                                  averagedWorldNormal));
                ++result.adjustedVertices;
            }
        }
    }

    model.skinSeams = result;
    return result;
}

Quaternion packedQuaternion(std::uint32_t packed) noexcept {
    // The game stores x/y in 11-bit fields and z in a 10-bit field. The
    // component ranges and positive-w reassembly below match
    // NewController::GetQuaternionFromIndexLocation in the engine.
    Quaternion value;
    value.x = static_cast<float>(packed & 0x7ffu) * (2.0f / 2046.0f) - 1.0f;
    value.y = static_cast<float>((packed >> 11u) & 0x7ffu) * (2.0f / 2046.0f) - 1.0f;
    value.z = static_cast<float>(packed >> 22u) * (2.0f / 1022.0f) - 1.0f;
    const float xyzLengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (xyzLengthSquared <= 1.0f) {
        value.w = std::sqrt(std::max(0.0f, 1.0f - xyzLengthSquared));
        return value;
    }
    const auto length = std::sqrt(xyzLengthSquared);
    if (length > 0.0f && finite(length)) {
        value.x /= length;
        value.y /= length;
        value.z /= length;
    } else {
        value = {};
    }
    value.w = 0.0f;
    return value;
}

float clampedLerp(float left, float right, float alpha) noexcept {
    return left + (right - left) * std::clamp(alpha, 0.0f, 1.0f);
}

Vec3 lerp(const Vec3& left, const Vec3& right, float alpha) noexcept {
    return {clampedLerp(left.x, right.x, alpha),
            clampedLerp(left.y, right.y, alpha),
            clampedLerp(left.z, right.z, alpha)};
}

float cubicBezier(float p0, float p1, float p2, float p3, float alpha) noexcept {
    const auto t = std::clamp(alpha, 0.0f, 1.0f);
    const auto inverse = 1.0f - t;
    return inverse * inverse * inverse * p0 + 3.0f * inverse * inverse * t * p1 +
           3.0f * inverse * t * t * p2 + t * t * t * p3;
}

Vec3 cubicBezier(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
                 float alpha) noexcept {
    return {cubicBezier(p0.x, p1.x, p2.x, p3.x, alpha),
            cubicBezier(p0.y, p1.y, p2.y, p3.y, alpha),
            cubicBezier(p0.z, p1.z, p2.z, p3.z, alpha)};
}

Quaternion slerp(Quaternion left, Quaternion right, float alpha) noexcept {
    left = normalizedQuaternion(left);
    right = normalizedQuaternion(right);
    float dot = left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
    if (dot < 0.0f) {
        dot = -dot;
        right.x = -right.x;
        right.y = -right.y;
        right.z = -right.z;
        right.w = -right.w;
    }
    const auto t = std::clamp(alpha, 0.0f, 1.0f);
    if (dot > 0.9995f) {
        return normalizedQuaternion({clampedLerp(left.x, right.x, t),
                                     clampedLerp(left.y, right.y, t),
                                     clampedLerp(left.z, right.z, t),
                                     clampedLerp(left.w, right.w, t)});
    }
    dot = std::clamp(dot, -1.0f, 1.0f);
    const auto angle = std::acos(dot);
    const auto sine = std::sin(angle);
    if (std::fabs(sine) < 1.0e-7f) return left;
    const auto leftWeight = std::sin((1.0f - t) * angle) / sine;
    const auto rightWeight = std::sin(t * angle) / sine;
    return normalizedQuaternion({left.x * leftWeight + right.x * rightWeight,
                                 left.y * leftWeight + right.y * rightWeight,
                                 left.z * leftWeight + right.z * rightWeight,
                                 left.w * leftWeight + right.w * rightWeight});
}

float engineAnimationLerp(float left, float right, float alpha) noexcept {
    // NewController::Control/GetFloatValue and Interpolate(Vector) retain
    // both products (K1 0x2dedf3/0x2decec/0x3f292a; K2 0xfc98b).
    const float leftTerm = (1.0f - alpha) * left;
    const float rightTerm = alpha * right;
    return leftTerm + rightTerm;
}

float engineAnimationBezier(float p0, float p1, float p2, float p3,
                            float alpha) noexcept {
    // K1 0x3a3236; K2 0x316a58 use de Casteljau. The engine argument
    // order is p0,p3,p1,p2; this helper keeps the existing viewer ordering.
    const float first = engineAnimationLerp(p0, p1, alpha);
    const float middle = engineAnimationLerp(p1, p2, alpha);
    const float last = engineAnimationLerp(p2, p3, alpha);
    return engineAnimationLerp(engineAnimationLerp(first, middle, alpha),
                               engineAnimationLerp(middle, last, alpha), alpha);
}

Quaternion engineAnimationSlerp(Quaternion left, Quaternion right,
                                 float alpha, GameVersion game) noexcept {
    // Engine Quaternion storage is w,x,y,z; keep that dot-product order.
    // K1 0x3a4ff8 and K2 0x3141b8 flip the left input, do not normalize,
    // and do not replace near-parallel quaternions with a linear blend.
    float dot = left.w * right.w + left.x * right.x;
    dot += left.y * right.y;
    dot += left.z * right.z;
    if (dot < 0.0f) {
        dot = -dot;
        left.x = -left.x;
        left.y = -left.y;
        left.z = -left.z;
        left.w = -left.w;
    }
    if (dot >= 1.0f) return left;
    // K1 desktop loads acosf/sinf; K2 desktop loads acos/sin and stores float.
    const bool floatAngle = game == GameVersion::Kotor1;
    const float angle = floatAngle
        ? std::acos(dot) : static_cast<float>(std::acos(static_cast<double>(dot)));
    if (angle == 0.0f) return left;
    const float scaledAngle = angle * alpha;
    const float denominator = floatAngle
        ? std::sin(angle) : static_cast<float>(std::sin(static_cast<double>(angle)));
    // Both desktop callers use double numerator sin/divide.
    const float leftWeight = static_cast<float>(
        std::sin(static_cast<double>(angle-scaledAngle))/static_cast<double>(denominator));
    const float rightWeight = static_cast<float>(
        std::sin(static_cast<double>(scaledAngle))/static_cast<double>(denominator));
    return {right.x * rightWeight + left.x * leftWeight,
            right.y * rightWeight + left.y * leftWeight,
            right.z * rightWeight + left.z * leftWeight,
            right.w * rightWeight + left.w * leftWeight};
}

Quaternion engineAnimationPackedQuaternion(std::uint32_t packed,
                                            GameVersion game) noexcept {
    // K1 0x2dccaa; K2 0xfc4ce retain add/divide before subtracting one.
    const float x = static_cast<float>(packed & 0x7ffu);
    const float y = static_cast<float>((packed >> 11u) & 0x7ffu);
    const float z = static_cast<float>(packed >> 22u);
    Quaternion value{(x + x) / 2046.0f - 1.0f,
                     (y + y) / 2046.0f - 1.0f,
                     (z + z) / 1022.0f - 1.0f, 0.0f};
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (lengthSquared <= 1.0f) {
        value.w = std::sqrt(1.0f - lengthSquared);
    } else {
        const float length = std::sqrt(lengthSquared);
        if (game == GameVersion::Kotor1) {
            value.x /= length;
            value.y /= length;
            value.z /= length;
        } else {
            const float inverseLength = 1.0f / length;
            value.x *= inverseLength;
            value.y *= inverseLength;
            value.z *= inverseLength;
        }
    }
    return value;
}

struct KeySample {
    std::size_t first{};
    std::size_t second{};
    float alpha{};
    bool valid{};
};

KeySample controllerKeySample(const Node& node, const Controller& controller, float time,
                               bool engineDesktop = false) noexcept {
    const auto rows = static_cast<std::size_t>(controller.rowCount);
    const auto start = static_cast<std::size_t>(controller.timeIndex);
    // GetKeyIndexAndAlpha initializes index 0 / alpha 1 even when the ushort
    // row count is zero. A nonempty authored list can reach this via the
    // builder's 16-bit count narrowing; the getter still reads its data pool.
    if (engineDesktop && rows == 0u) return {0u, 0u, 1.0f, true};
    if (rows == 0u || start > node.controllerDataWords.size() ||
        rows > node.controllerDataWords.size() - start) {
        return {};
    }
    if (engineDesktop) {
        // K1 0x2dcbba; K2 0xfc318: direct division for interior samples.
        std::size_t upper = 0u;
        while (upper < rows && time > wordAsFloat(node.controllerDataWords[start + upper]))
            ++upper;
        if (upper == 0u) return {0u, 0u, 1.0f, true};
        if (upper == rows) return {rows - 1u, rows - 1u, 1.0f, true};
        const float previous = wordAsFloat(node.controllerDataWords[start + upper - 1u]);
        const float current = wordAsFloat(node.controllerDataWords[start + upper]);
        return {upper - 1u, upper, (time - previous) / (current - previous), true};
    }
    if (!finite(time)) time = 0.0f;
    const auto firstTime = wordAsFloat(node.controllerDataWords[start]);
    if (rows == 1u || !finite(firstTime) || time <= firstTime) return {0u, 0u, 0.0f, true};
    float previous = firstTime;
    for (std::size_t key = 1u; key < rows; ++key) {
        const auto current = wordAsFloat(node.controllerDataWords[start + key]);
        if (!finite(current)) continue;
        if (time <= current) {
            const auto denominator = current - previous;
            const auto alpha = denominator > 0.0f && finite(previous)
                ? std::clamp((time - previous) / denominator, 0.0f, 1.0f)
                : 1.0f;
            return {key - 1u, key, alpha, true};
        }
        previous = current;
    }
    return {rows - 1u, rows - 1u, 0.0f, true};
}

std::optional<float> controllerFloat(const Node& node, const Controller& controller,
                                     float time,
                                     bool engineGame = false) noexcept {
    if (controller.valueKind != ControllerValueKind::Float) return std::nullopt;
    const auto sample = controllerKeySample(node, controller, time, engineGame);
    if (!sample.valid) return std::nullopt;
    const auto start = static_cast<std::size_t>(controller.dataIndex);
    if (engineGame && sample.first == sample.second) {
        const auto word = start + sample.first *
            (controller.interpolation == ControllerInterpolation::Bezier ? 3u : 1u);
        if (word >= node.controllerDataWords.size()) return std::nullopt;
        return wordAsFloat(node.controllerDataWords[word]);
    }
    if (controller.interpolation == ControllerInterpolation::Bezier) {
        const auto rows = static_cast<std::size_t>(controller.rowCount);
        const auto requiredWords = rows * 3u;
        if (start > node.controllerDataWords.size() ||
            requiredWords > node.controllerDataWords.size() - start) {
            return std::nullopt;
        }

        // NewController::GetFloatValue in the K2 engine uses a
        // scalar-specific word layout rather than the vector controller's
        // conventional [value, incoming, outgoing] triplets. For an interior
        // key K it reads:
        //   start       = words[3*K - 3]
        //   end         = words[3*K - 1]
        //   control one = start + words[3*K]
        //   control two = end   + words[3*K + 1]
        // Preserve that indexing exactly, including the engine's endpoint
        // behavior, instead of normalizing it into the vector layout.
        if (sample.first == sample.second) {
            const auto value = wordAsFloat(
                node.controllerDataWords[start + sample.first * 3u]);
            return engineGame || finite(value) ? std::optional<float>{value} : std::nullopt;
        }

        const auto key = sample.second;
        if (key == 0u || key >= rows) return std::nullopt;
        const auto previousBase = start + (key - 1u) * 3u;
        const auto currentBase = start + key * 3u;
        const auto p0 = wordAsFloat(node.controllerDataWords[previousBase]);
        const auto p3 = wordAsFloat(node.controllerDataWords[previousBase + 2u]);
        const auto p1 = p0 + wordAsFloat(node.controllerDataWords[currentBase]);
        const auto p2 = p3 + wordAsFloat(node.controllerDataWords[currentBase + 1u]);
        const auto result = engineGame
            ? engineAnimationBezier(p0, p1, p2, p3, sample.alpha)
            : cubicBezier(p0, p1, p2, p3, sample.alpha);
        return engineGame || finite(result) ? std::optional<float>{result} : std::nullopt;
    }
    if (controller.interpolation != ControllerInterpolation::Linear ||
        start > node.controllerDataWords.size() ||
        sample.second >= node.controllerDataWords.size() - start) {
        return std::nullopt;
    }
    const auto left = wordAsFloat(node.controllerDataWords[start + sample.first]);
    const auto right = wordAsFloat(node.controllerDataWords[start + sample.second]);
    const auto result = engineGame ? engineAnimationLerp(left, right, sample.alpha)
                                   : clampedLerp(left, right, sample.alpha);
    return engineGame || finite(result) ? std::optional<float>{result} : std::nullopt;
}

std::optional<Vec3> controllerVector(const Node& node, const Controller& controller,
                                     float time,
                                     bool engineGame = false) noexcept {
    if (controller.valueKind != ControllerValueKind::Vector) return std::nullopt;
    const auto sample = controllerKeySample(node, controller, time, engineGame);
    if (!sample.valid) return std::nullopt;
    const auto start = static_cast<std::size_t>(controller.dataIndex);
    const auto readVector = [&](std::size_t word) -> std::optional<Vec3> {
        if (word > node.controllerDataWords.size() ||
            3u > node.controllerDataWords.size() - word) return std::nullopt;
        Vec3 result{wordAsFloat(node.controllerDataWords[word]),
                    wordAsFloat(node.controllerDataWords[word + 1u]),
                    wordAsFloat(node.controllerDataWords[word + 2u])};
        if (!engineGame && (!finite(result.x) || !finite(result.y) || !finite(result.z)))
            return std::nullopt;
        return result;
    };
    if (controller.interpolation == ControllerInterpolation::Bezier) {
        const auto firstBase = start + sample.first * 9u;
        const auto secondBase = start + sample.second * 9u;
        const auto p0 = readVector(firstBase);
        if (!p0) return std::nullopt;
        if (sample.first == sample.second) return p0;
        const auto firstOut = readVector(firstBase + 6u);
        const auto p3 = readVector(secondBase);
        const auto secondIn = readVector(secondBase + 3u);
        if (!firstOut || !p3 || !secondIn) return std::nullopt;
        const auto p1 = add(*p0, *firstOut);
        const auto p2 = add(*p3, *secondIn);
        if (engineGame)
            return Vec3{engineAnimationBezier(p0->x, p1.x, p2.x, p3->x, sample.alpha),
                        engineAnimationBezier(p0->y, p1.y, p2.y, p3->y, sample.alpha),
                        engineAnimationBezier(p0->z, p1.z, p2.z, p3->z, sample.alpha)};
        return cubicBezier(*p0, p1, p2, *p3, sample.alpha);
    }
    if (controller.interpolation != ControllerInterpolation::Linear) return std::nullopt;
    const auto left = readVector(start + sample.first * 3u);
    const auto right = readVector(start + sample.second * 3u);
    if (!left || !right) return std::nullopt;
    if (engineGame) {
        if (sample.first == sample.second) return left;
        return Vec3{engineAnimationLerp(left->x, right->x, sample.alpha),
                    engineAnimationLerp(left->y, right->y, sample.alpha),
                    engineAnimationLerp(left->z, right->z, sample.alpha)};
    }
    return lerp(*left, *right, sample.alpha);
}

std::optional<Quaternion> controllerQuaternion(const Node& node, const Controller& controller,
                                               float time,
                                               std::optional<GameVersion> engineGame = std::nullopt) noexcept {
    if (controller.valueKind != ControllerValueKind::PackedQuaternion &&
        controller.valueKind != ControllerValueKind::Quaternion) return std::nullopt;
    if (controller.interpolation != ControllerInterpolation::Linear) return std::nullopt;
    const auto sample = controllerKeySample(node, controller, time, engineGame.has_value());
    if (!sample.valid) return std::nullopt;
    const auto start = static_cast<std::size_t>(controller.dataIndex);
    const auto read = [&](std::size_t key) -> std::optional<Quaternion> {
        if (controller.valueKind == ControllerValueKind::PackedQuaternion) {
            if (start > node.controllerDataWords.size() || key >= node.controllerDataWords.size() - start)
                return std::nullopt;
            return engineGame
                ? engineAnimationPackedQuaternion(node.controllerDataWords[start + key], *engineGame)
                : packedQuaternion(node.controllerDataWords[start + key]);
        }
        const auto word = start + key * 4u;
        if (word > node.controllerDataWords.size() ||
            4u > node.controllerDataWords.size() - word) return std::nullopt;
        const Quaternion value{wordAsFloat(node.controllerDataWords[word]),
                               wordAsFloat(node.controllerDataWords[word + 1u]),
                               wordAsFloat(node.controllerDataWords[word + 2u]),
                               wordAsFloat(node.controllerDataWords[word + 3u])};
        return engineGame ? value : normalizedQuaternion(value);
    };
    const auto left = read(sample.first);
    const auto right = read(sample.second);
    if (!left || !right) return std::nullopt;
    if (engineGame) {
        if (sample.first == sample.second) return left;
        return engineAnimationSlerp(*left, *right, sample.alpha, *engineGame);
    }
    return slerp(*left, *right, sample.alpha);
}

std::optional<float> bindControllerFloat(const Node& node,const Controller& controller,
                                         float time) noexcept {
    const auto first=controllerFloat(node,controller,-1.0f,true);
    const auto second=controllerFloat(node,controller,time,true);
    if(!first || !second) return std::nullopt;
    // ControlDispatch(-1,0,1) samples both endpoints: only firstTime < -95
    // uses the previous Part value. Keep the zero-weight first product so
    // authored nonfinite keys retain engine arithmetic behavior.
    return engineAnimationLerp(*first,*second,1.0f);
}

std::optional<Vec3> bindControllerVector(const Node& node,const Controller& controller,
                                       float time) noexcept {
    const auto first=controllerVector(node,controller,-1.0f,true);
    const auto second=controllerVector(node,controller,time,true);
    if(!first || !second) return std::nullopt;
    return Vec3{engineAnimationLerp(first->x,second->x,1.0f),
                engineAnimationLerp(first->y,second->y,1.0f),
                engineAnimationLerp(first->z,second->z,1.0f)};
}

void applyMaterialControllers(const Node& node, float time,
                              MaterialValueSource source,
                              NodeMaterialState& material,
                              bool engineControl = false) noexcept {
    // Bind-state callers reproduce ControlDispatch(-1,0,1), including engine
    // interpolation and both blend products. Inspection clip sampling keeps
    // its separate evaluator. Scalar/vector arithmetic is shared by all four
    // executables; quaternion sampling retains its game-specific arithmetic.
    // Callers establish the target part is a mesh. Animation-node flags are
    // not the discriminator: Control resolves offsets from the target part.
    for (const auto& controller : node.controllers) {
        if (controller.type == kControllerAlpha) {
            if (const auto value = engineControl ? bindControllerFloat(node,controller,time)
                                                : controllerFloat(node,controller,time);
                value && (engineControl || finite(*value))) {
                material.alpha = *value;
                material.alphaSource = source;
            }
        } else if (controller.type == kControllerSelfIllumColor) {
            if (engineControl && controller.valueKind == ControllerValueKind::Float) {
                if (const auto value = bindControllerFloat(node,controller,time)) {
                    material.selfIllumination[0] = *value;
                    material.selfIlluminationSource = source;
                }
                continue;
            }
            if (const auto value = engineControl ? bindControllerVector(node,controller,time)
                                                : controllerVector(node,controller,time);
                value && (engineControl || (finite(value->x) && finite(value->y) && finite(value->z)))) {
                material.selfIllumination = {{value->x,value->y,value->z}};
                material.selfIlluminationSource = source;
            }
        }
    }
}

void applyLightControllers(const Node& node,float time,MaterialValueSource source,
                           NodeLightState& light,bool engineControl = false) noexcept {
    for(const auto& controller:node.controllers) {
        if(controller.type==kControllerLightColor) {
            if(engineControl && controller.valueKind==ControllerValueKind::Float) {
                if(const auto v=bindControllerFloat(node,controller,time)) {
                    light.color[0]=*v;
                    light.sources[0]=source;
                }
                continue;
            }
            if(const auto v=engineControl ? bindControllerVector(node,controller,time)
                                         : controllerVector(node,controller,time);
               v && (engineControl || (finite(v->x) && finite(v->y) && finite(v->z)))) {
                light.color={{v->x,v->y,v->z}};
                light.sources[0]=source;
            }
            continue;
        }
        float* field=nullptr;std::size_t slot=0;
        switch(controller.type) {
        case kControllerLightRadius: field=&light.radius;slot=1;break;
        case kControllerLightShadowRadius: field=&light.shadowRadius;slot=2;break;
        case kControllerLightVerticalDisplacement: field=&light.verticalDisplacement;slot=3;break;
        case kControllerLightMultiplier: field=&light.multiplier;slot=4;break;
        default: break;
        }
        if(field) if(const auto v=engineControl ? bindControllerFloat(node,controller,time)
                                               : controllerFloat(node,controller,time);
                     v && (engineControl || finite(*v))) {
            *field=*v;
            light.sources[slot]=source;
        }
    }
}

std::optional<std::array<const EmitterScalarBinding*,3>> emitterVectorBindings(
    std::uint32_t destination) noexcept {
    std::array<const EmitterScalarBinding*,3> result{};
    // These IDs are actual engine float offsets. A vector control writes
    // three adjacent floats even when its authored field usually is scalar.
    // Only spans fully represented by the engine scalar bindings belong here.
    for(std::size_t lane=0;lane<result.size();++lane) {
        for(const auto& binding:kEmitterScalarBindings)
            if(binding.id==destination+static_cast<std::uint32_t>(lane)*4u) {
                result[lane]=&binding;
                break;
            }
        if(!result[lane]) return std::nullopt;
    }
    return result;
}

void applyEmitterControllers(const Node& node, float time, MaterialValueSource source,
                             NodeEmitterState& emitter,bool engineControl = false) noexcept {
    for (const auto& controller : node.controllers) {
        if(engineControl && controller.valueKind==ControllerValueKind::Vector) {
            if(const auto bindings=emitterVectorBindings(controller.type)) {
                if(const auto value=bindControllerVector(node,controller,time)) {
                    const std::array<float,3> values{{value->x,value->y,value->z}};
                    for(std::size_t lane=0;lane<bindings->size();++lane) {
                        const auto& binding=*(*bindings)[lane];
                        auto& field=emitter.*(binding.field);
                        field=values[lane];
                        emitter.sources[static_cast<std::size_t>(binding.property)]=source;
                    }
                }
                continue;
            }
        }
        for (const auto& binding : kEmitterScalarBindings) {
            if (controller.type != binding.id) continue;
            if (const auto value=engineControl ? bindControllerFloat(node,controller,time)
                                              : controllerFloat(node,controller,time);
                value && (engineControl || finite(*value))) {
                auto& field=emitter.*(binding.field);
                field=*value;
                emitter.sources[static_cast<std::size_t>(binding.property)]=source;
            }
            break;
        }
        std::array<float,3>* field=nullptr;
        EmitterProperty property=EmitterProperty::ColorStart;
        switch (controller.type) {
        case kControllerEmitterColorStart: field=&emitter.colorStart; break;
        case kControllerEmitterColorMid: field=&emitter.colorMid; property=EmitterProperty::ColorMid; break;
        case kControllerEmitterColorEnd: field=&emitter.colorEnd; property=EmitterProperty::ColorEnd; break;
        default: break;
        }
        if(field && engineControl && controller.valueKind==ControllerValueKind::Float) {
            if(const auto v=bindControllerFloat(node,controller,time)) {
                (*field)[0]=*v;
                emitter.sources[static_cast<std::size_t>(property)]=source;
            }
            continue;
        }
        if (field) if (const auto v=engineControl ? bindControllerVector(node,controller,time)
                                                : controllerVector(node,controller,time);
            v && (engineControl || (finite(v->x) && finite(v->y) && finite(v->z)))) {
            *field={{v->x,v->y,v->z}};
            emitter.sources[static_cast<std::size_t>(property)]=source;
        }
    }
}

void calculatePoseTransformsAndBounds(const Model& model, Pose& pose,
                                     const neoshared::bif::JobControl& job = {},
                                     bool engineDesktop = false) {
    job.check();
    const auto nodeCount = model.nodes.size();
    pose.worldTransforms.assign(nodeCount, identityMatrix());
    std::vector<Vec3> boneWorldPositions(nodeCount);
    std::vector<Quaternion> boneWorldOrientations(nodeCount);

    for (std::size_t index = 0u; index < nodeCount; ++index) {
        if ((index & 1023u) == 0u) job.check();
        const auto localPosition = index < pose.positions.size()
            ? pose.positions[index] : model.nodes[index].position;
        auto localOrientation = index < pose.orientations.size()
            ? pose.orientations[index] : model.nodes[index].orientation;
        if (!engineDesktop) localOrientation = normalizedQuaternion(localOrientation);
        const auto localScale = index < pose.scales.size() ? pose.scales[index] : 1.0f;
        auto localTransform = engineDesktop ? nodeTransform(localPosition, localOrientation)
            : nodeTransform(localPosition, localOrientation, localScale);
        if (engineDesktop) {
            for (std::size_t column = 0u; column < 3u; ++column)
                for (std::size_t row = 0u; row < 3u; ++row)
                    localTransform.values[column * 4u + row] *= localScale;
        }
        const auto parent = model.nodes[index].parent;
        if (parent && *parent < index && *parent < nodeCount) {
            pose.worldTransforms[index] = multiply(pose.worldTransforms[*parent], localTransform);
            boneWorldOrientations[index] = multiplyQuaternion(
                boneWorldOrientations[*parent], localOrientation, engineDesktop);
            boneWorldPositions[index] = add(
                boneWorldPositions[*parent],
                rotateVector(boneWorldOrientations[*parent], localPosition, engineDesktop));
        } else {
            pose.worldTransforms[index] = localTransform;
            boneWorldOrientations[index] = localOrientation;
            boneWorldPositions[index] = localPosition;
        }
    }

    // Keep PartAnimMesh's local pools across controller/recalculate passes.
    pose.deformedMeshes.resize(model.meshes.size());
    for (std::size_t i=0;i<model.meshes.size();++i)
        if (!model.meshes[i].animated) pose.deformedMeshes[i] = DeformedMesh{};
    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        job.check();
        const auto& mesh = model.meshes[meshIndex];
        auto& deformed = pose.deformedMeshes[meshIndex];
        if (!mesh.skinned ||
            !mesh.hasSkinChannels || !mesh.hasSkinBoneMap ||
            !mesh.hasInverseBindPose ||
            mesh.skinInfluences.size() != mesh.vertices.size()) {
            continue;
        }

        if (mesh.nodeIndex < nodeCount)
            deformed.skinPartFrame=nodeTransform(boneWorldPositions[mesh.nodeIndex],
                                                boneWorldOrientations[mesh.nodeIndex]);
        const auto boneCount = std::min({boneWorldPositions.size(),
                                         boneWorldOrientations.size(),
                                         mesh.inverseBindOrientations.size(),
                                         mesh.inverseBindTranslations.size()});
        if (boneCount == 0u) continue;

        std::array<Quaternion, 17> skinOrientations{};
        std::array<Vec3, 17> skinTranslations{};
        for (std::size_t slot = 0u; slot < mesh.skinBoneMapSize; ++slot) {
            const auto mappedBone = mesh.skinBoneMap[slot];
            auto& palette = deformed.skinPalette[slot];
            palette.modelNode = mappedBone;
            if (mappedBone < 0 || static_cast<std::size_t>(mappedBone) >= boneCount)
                continue;
            const auto bone = static_cast<std::size_t>(mappedBone);
            // RenderSkinned uploads a compact palette selected by the engine
            // skin map. Each slot is current world-bone * model-space inverse
            // bind, stored as three row vec4 constants at c[18 + slot*3].
            skinOrientations[slot] = multiplyQuaternion(
                boneWorldOrientations[bone], mesh.inverseBindOrientations[bone], engineDesktop);
            skinTranslations[slot] = add(
                boneWorldPositions[bone],
                rotateVector(boneWorldOrientations[bone],
                             mesh.inverseBindTranslations[bone], engineDesktop));
            const auto transform = nodeTransform(
                skinTranslations[slot], skinOrientations[slot]);
            palette.rows = {{
                transform.values[0], transform.values[4], transform.values[8],
                transform.values[12],
                transform.values[1], transform.values[5], transform.values[9],
                transform.values[13],
                transform.values[2], transform.values[6], transform.values[10],
                transform.values[14]}};
            palette.valid = true;
            deformed.skinPaletteSize = slot + 1u;
        }

        deformed.positions.resize(mesh.vertices.size());
        deformed.normals.resize(mesh.vertices.size());
        deformed.rawSkinNormals.resize(mesh.vertices.size());
        deformed.tangents.resize(mesh.vertices.size());
        deformed.bitangents.resize(mesh.vertices.size());
        deformed.basisNormals.resize(mesh.vertices.size());
        deformed.exactTangentBasis.assign(mesh.vertices.size(), 0u);
        deformed.preserveTangentBasis = true;
        for (std::size_t vertexIndex = 0u; vertexIndex < mesh.vertices.size(); ++vertexIndex) {
            if ((vertexIndex & 1023u) == 0u) job.check();
            const auto& vertex = mesh.vertices[vertexIndex];
            const auto& influence = mesh.skinInfluences[vertexIndex];
            Vec3 position{};
            Vec3 normal{};
            Vec3 tangent{};
            Vec3 bitangent{};
            Vec3 basisNormal{};
            bool contributed = false;
            const bool hasAuthoredBasis = vertex.tangentBasis.valid;
            for (std::size_t component = 0u; component < influence.weights.size(); ++component) {
                const auto weight = influence.weights[component];
                if (!finite(weight) || weight == 0.0f) continue;
                const auto localBone = static_cast<std::size_t>(influence.boneIndices[component]);
                if (localBone >= deformed.skinPalette.size() ||
                    !deformed.skinPalette[localBone].valid) {
                    ++deformed.rejectedInfluences;
                    continue;
                }
                const auto rotatedPosition = rotateVector(
                    skinOrientations[localBone], vertex.position, engineDesktop);
                const auto skinnedPosition = add(
                    rotatedPosition, skinTranslations[localBone]);
                const auto skinnedNormal = rotateVector(
                    skinOrientations[localBone], mesh.normalAt(vertexIndex), engineDesktop);
                position.x += weight * skinnedPosition.x;
                position.y += weight * skinnedPosition.y;
                position.z += weight * skinnedPosition.z;
                normal.x += weight * skinnedNormal.x;
                normal.y += weight * skinnedNormal.y;
                normal.z += weight * skinnedNormal.z;
                if (hasAuthoredBasis) {
                    const auto skinnedTangent = rotateVector(
                        skinOrientations[localBone], vertex.tangentBasis.tangent, engineDesktop);
                    const auto skinnedBitangent = rotateVector(
                        skinOrientations[localBone], vertex.tangentBasis.bitangent, engineDesktop);
                    const auto skinnedBasisNormal = rotateVector(
                        skinOrientations[localBone], vertex.tangentBasis.normal, engineDesktop);
                    tangent.x += weight * skinnedTangent.x;
                    tangent.y += weight * skinnedTangent.y;
                    tangent.z += weight * skinnedTangent.z;
                    bitangent.x += weight * skinnedBitangent.x;
                    bitangent.y += weight * skinnedBitangent.y;
                    bitangent.z += weight * skinnedBitangent.z;
                    basisNormal.x += weight * skinnedBasisNormal.x;
                    basisNormal.y += weight * skinnedBasisNormal.y;
                    basisNormal.z += weight * skinnedBasisNormal.z;
                }
                contributed = true;
            }
            if (!contributed) {
                position = vertex.position;
                normal = mesh.normalAt(vertexIndex);
            }
            deformed.positions[vertexIndex] = position;
            // cSkinBumpedOutVP uses the raw weighted ordinary normal for its
            // shell displacement. cSkinVP and cSkinMotionBlurVP normalize the
            // same value before lighting/facing calculations.
            deformed.rawSkinNormals[vertexIndex] = normal;
            deformed.normals[vertexIndex] = normalize(normal);
            if (contributed && hasAuthoredBasis) {
                deformed.tangents[vertexIndex] = tangent;
                deformed.bitangents[vertexIndex] = bitangent;
                deformed.basisNormals[vertexIndex] = basisNormal;
                deformed.exactTangentBasis[vertexIndex] = 1u;
            }
        }
        deformed.valid = true;
    }

    rebuildAnimMeshGeometry(model, pose);

    Bounds bounds;
    std::size_t boundedVertices = 0u;
    const auto extendBounds = [&bounds, &job, &boundedVertices](const Vec3& point) {
        if ((boundedVertices++ & 1023u) == 0u) job.check();
        if (!bounds.valid) {
            bounds.minimum = bounds.maximum = point;
            bounds.valid = true;
            return;
        }
        bounds.minimum.x = std::min(bounds.minimum.x, point.x);
        bounds.minimum.y = std::min(bounds.minimum.y, point.y);
        bounds.minimum.z = std::min(bounds.minimum.z, point.z);
        bounds.maximum.x = std::max(bounds.maximum.x, point.x);
        bounds.maximum.y = std::max(bounds.maximum.y, point.y);
        bounds.maximum.z = std::max(bounds.maximum.z, point.z);
    };

    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        const auto& deformed = pose.deformedMeshes[meshIndex];
        if (deformed.valid && deformed.positions.size() == mesh.vertices.size()) {
            for (const auto& point : deformed.positions) extendBounds(point);
            continue;
        }
        if (mesh.animated) continue; // undefined engine pool is not a rigid MDX bound
        if (mesh.nodeIndex >= pose.worldTransforms.size()) continue;
        const auto& transform = pose.worldTransforms[mesh.nodeIndex];
        for (const auto& vertex : mesh.vertices)
            extendBounds(transformPoint(transform, vertex.position));
    }
    pose.bounds = bounds;
}

std::optional<std::size_t> animationIndexByName(const Model& model,
                                                const std::string& name) {
    const auto wanted = lowercase(name);
    for (std::size_t index = 0u; index < model.animations.size(); ++index) {
        if (lowercase(model.animations[index].name) == wanted) return index;
    }
    return std::nullopt;
}

std::size_t contiguousHierarchySize(const std::vector<const Model*>& hierarchy) noexcept {
    std::size_t count = 0u;
    while (count < hierarchy.size() && hierarchy[count] != nullptr) ++count;
    return count;
}

float inheritedPositionScale(const std::vector<const Model*>& hierarchy,
                             std::size_t ownerIndex) noexcept {
    float result = 1.0f;
    const auto count = std::min(ownerIndex, contiguousHierarchySize(hierarchy));
    for (std::size_t index = 0u; index < count; ++index)
        result *= hierarchy[index]->animationScale;
    return result;
}

std::optional<std::size_t> animationRootNode(const Animation& animation) {
    if (animation.root.empty() || lowercase(animation.root) == "nullptr") return std::nullopt;
    for (std::size_t index = 0u; index < animation.nodes.size(); ++index) {
        // Animation::PostProcess uses strcmp for this lookup, not stricmp.
        if (animation.nodes[index].name == animation.root) return index;
    }
    return std::nullopt;
}

std::vector<std::pair<std::size_t,std::size_t>> engineAnimationNodePairs(
    const Model& targetModel,const Animation& animation) {
    std::vector<std::pair<std::size_t,std::size_t>> result;
    const auto admitted=[&](std::size_t index) {
        return targetModel.sceneNodeAdmission.empty() ||
            (index<targetModel.sceneNodeAdmission.size() && targetModel.sceneNodeAdmission[index]);
    };
    std::optional<std::size_t> targetRoot,sourceRoot;
    for(std::size_t index=0;index<targetModel.nodes.size();++index)
        if(!targetModel.nodes[index].parent && admitted(index)) {targetRoot=index;break;}
    for(std::size_t index=0;index<animation.nodes.size();++index)
        if(!animation.nodes[index].parent) {sourceRoot=index;break;}
    if(!targetRoot || !sourceRoot) return result;

    // SynchronizeTree has already assigned node IDs during model loading.
    // AnimateHierarchy walks the live Part children and selects one animation
    // child by ID, searching cyclically from the Part child's ordinal. Names
    // are not re-synchronized during playback, and duplicate IDs do not cause
    // all matching animation children to write the same Part in sequence.
    std::function<void(std::size_t,std::size_t,bool)> visit;
    visit=[&](std::size_t source,std::size_t target,bool enabled) {
        // Gob::Animate starts with false. Each Part compares its original
        // source node with Model::animationRoot, then passes the enabled value
        // to its children. Several live Parts can reference the same source
        // node; each of those occurrences must open the control gate.
        if (targetModel.animationRootNode) {
            const auto root = *targetModel.animationRootNode;
            enabled = enabled || (targetModel.partSourceNodes.empty()
                ? root == target
                : targetModel.partSourceNodes[root] == targetModel.partSourceNodes[target]);
        }
        if(enabled) result.emplace_back(source,target);
        const auto& targetChildren=targetModel.nodes[target].children;
        const auto& sourceChildren=animation.nodes[source].children;
        for(std::size_t ordinal=0;ordinal<targetChildren.size();++ordinal) {
            const auto child=targetChildren[ordinal];
            if(!admitted(child) || targetModel.nodes[child].id==0xffffu) continue;
            for(std::size_t candidate=0;candidate<sourceChildren.size();++candidate) {
                const auto sourceChild=sourceChildren[(ordinal+candidate)%sourceChildren.size()];
                if(animation.nodes[sourceChild].id==targetModel.nodes[child].id) {
                    visit(sourceChild,child,enabled);
                    break;
                }
            }
        }
    };
    visit(*sourceRoot,*targetRoot,false);
    return result;
}

void appendForwardEvents(std::vector<AnimationEventOccurrence>& result,
                         const Animation& animation, float startTime, float endTime) {
    for (std::size_t index = 0u; index < animation.events.size(); ++index) {
        const auto& event = animation.events[index];
        if (event.time > startTime && event.time <= endTime)
            result.push_back({index, event.time, event.name});
    }
}

void appendReverseEvents(std::vector<AnimationEventOccurrence>& result,
                         const Animation& animation, float startTime, float endTime) {
    for (std::size_t remaining = animation.events.size(); remaining != 0u; --remaining) {
        const auto index = remaining - 1u;
        const auto& event = animation.events[index];
        if (event.time >= endTime && event.time < startTime)
            result.push_back({index, event.time, event.name});
    }
}

} // namespace

EngineCpuSkinSource decodeEngineMeshTopology(const EngineMeshSource& input,bool includeCpuTraversal,
                                           const neoshared::bif::JobControl& job) {
    ByteView mdl(input.mdl->data(),input.mdl->size(),"engine CPU skin MDL",job);
    EngineCpuSkinSource out;
    out.unsignedStripBoneIndices=input.game==GameVersion::Kotor1;
    const auto node=input.nodeOffset;
    const auto primitive=mdl.u32(node+0x130u,"skin primitive");
    const auto counts=mdl.u32(node+0x100u,"skin counts pointer");
    const auto sources=mdl.u32(node+0x10cu,"skin index sources pointer");
    const auto drawBatches=mdl.u32(node+0x104u,"skin draw batch count");
    const auto cpuBatches=mdl.u32(node+0x11cu,"skin CPU batch count");
    if(includeCpuTraversal)for(std::size_t batch=0;batch<cpuBatches;++batch) {
        // ResetTriMeshParts 0xf536b..0xf5373 writes the even count back into
        // the runtime count list. CPU visits padding indexes as well.
        const auto authored=mdl.u32(counts+batch*4u,"skin index count");
        const std::size_t allocated=(static_cast<std::size_t>(authored)+1u)&~std::size_t{1};
        const auto source=mdl.u32(sources+batch*4u,"skin index source");
        mdl.require(source,allocated*2u,"skin padded index pool");
        for(std::size_t index=0;index<allocated;++index)
            out.stripVertices.push_back(mdl.u16(source+index*2u,"skin CPU strip index"));
    }
    // Execute uses the retained +0x11c token-array count, whereas RenderFlat
    // uses +0x104. Reset does not merge these separately authored counts.
    for(std::size_t batch=0;batch<drawBatches;++batch) {
        const auto authored=mdl.u32(counts+batch*4u,"skin draw index count");
        const std::size_t runtimeCount=batch<cpuBatches?
            (static_cast<std::size_t>(authored)+1u)&~std::size_t{1}:authored;
        const auto source=mdl.u32(sources+batch*4u,"skin draw index source");
        // K1 RenderFlat passes the full runtime count (0x342533..0x34255e).
        // K2 truncates triangle batches at 0xcc581..0xcc59d.
        const auto drawCount=primitive==3u && input.game==GameVersion::Kotor2
            ? (runtimeCount/3u)*3u : runtimeCount;
        out.draws.push_back({primitive,out.indices.size(),drawCount});
        for(std::size_t index=0;index<drawCount;++index) {
            const auto at=source+index*2u;
            // K1 ResetTriMeshParts rounds an odd count before copying the
            // serialized pool (0x321343..0x3213d6), but its writer emits the
            // original pool size. A final batch can therefore lack the
            // extra ushort. Only a non-CPU GL_TRIANGLES tail that cannot
            // form a complete primitive may use an inert placeholder;
            // preserve both readable padding and the engine draw count.
            const bool ignoredPadding=!includeCpuTraversal &&
                input.game==GameVersion::Kotor1 && primitive==3u &&
                index>=authored && index>=(drawCount/3u)*3u;
            out.indices.push_back(ignoredPadding && !mdl.contains(at,2u)
                ?0u:mdl.u16(at,"skin draw index"));
        }
    }
    // The trailing count is independently rounded and written back at
    // ResetTriMeshParts 0xf54a3..0xf54a8 before both transform and draw calls.
    const auto authoredTriangles=mdl.u32(node+0x12cu,"skin triangle count");
    const std::size_t triangles=(static_cast<std::size_t>(authoredTriangles)+1u)&~std::size_t{1};
    const auto triangleSource=mdl.u32(node+0x124u,"skin triangle source");
    if(includeCpuTraversal)for(std::size_t index=0;index<triangles;++index)
        out.triangleVertices.push_back(mdl.u16(triangleSource+index*2u,"skin CPU triangle index"));
    if(primitive==4u && triangles) {
        const auto offset=mdl.u32(node+(input.game==GameVersion::Kotor1?0x194u:0x19cu),"skin trailing draw offset");
        out.draws.push_back({3u,out.indices.size(),triangles});
        for(std::size_t index=0;index<triangles;++index)
            out.indices.push_back(mdl.u16(triangleSource+(offset+index)*2u,"skin trailing draw index"));
    }
    return out;
}

std::vector<Vec2> decodeEngineProceduralUvSource(const Mesh& mesh) {
    const auto& input=*mesh.enginePrelightInput;
    const neoshared::bif::JobControl job;
    ByteView mdx(input.mdx->data(),input.mdx->size(),"engine procedural UV MDX",job);
    std::vector<Vec2> values;
    values.reserve(mesh.vertices.size());
    // PartTriMesh and VertexPrimitiveFlat copy the offset-addressed MDX
    // values without the viewer's bitmap/finite-value filtering. Preserve a
    // referenced sentinel row too; the procedural signed-count loop excludes it.
    for(std::size_t index=0;index<mesh.vertices.size();++index)
        values.push_back(mdx.vec2(input.mdxDataOffset+index*mesh.mdxRowSize+
            mesh.mdxTexcoordOffsets[0],"procedural UV source"));
    return values;
}

EnginePrelightSource decodeEnginePrelightSource(const Mesh& mesh) {
    if(mesh.builtPrelightSource) {
        auto out=*mesh.builtPrelightSource;
        if(mesh.asciiNormalPool)
            for(std::size_t index=0;index<out.vertices.size();++index)
                out.vertices[index].normal=mesh.normalAt(index);
        return out;
    }
    const auto& input=*mesh.enginePrelightInput;
    const neoshared::bif::JobControl job;
    ByteView mdx(input.mdx->data(),input.mdx->size(),"engine static prelighting MDX",job);
    EnginePrelightSource out;
    out.authoredColors=mesh.mdxColorOffset!=0xffffffffu;
    // The engine allocation and loop both sign-extend the authored ushort.
    const auto count=static_cast<std::int16_t>(mesh.enginePrelightVertexCount);
    for(std::int32_t index=0;index<count;++index) {
        const auto row=input.mdxDataOffset+static_cast<std::size_t>(index)*mesh.mdxRowSize;
        EnginePrelightVertex vertex;
        vertex.position=mdx.vec3(row+input.mdxVertexOffset,"prelight position");
        vertex.normal=mdx.vec3(row+mesh.mdxNormalOffset,"prelight normal");
        if(out.authoredColors)for(std::size_t channel=0;channel<3;++channel)
            vertex.color[channel]=mdx.u8(row+mesh.mdxColorOffset+channel,"prelight color");
        out.vertices.push_back(vertex);
    }
    return out;
}

EngineCpuSkinSource decodeEngineCpuSkinSource(const Mesh& mesh) {
    if(mesh.builtCpuSkinSource) {
        auto out=*mesh.builtCpuSkinSource;
        if(mesh.asciiNormalPool)
            for(std::size_t index=0;index<out.normals.size();++index)
                out.normals[index]=mesh.normalAt(index);
        return out;
    }
    const auto& input=*mesh.engineCpuSkin;
    auto out=decodeEngineMeshTopology(input,true);
    const neoshared::bif::JobControl job;
    ByteView mdx(input.mdx->data(),input.mdx->size(),"engine CPU skin MDX",job);
    const auto count=mesh.vertices.size();
    out.positions.resize(count);out.normals.resize(count);out.weights.resize(count);
    out.stripBoneIndices.resize(count);out.triangleBoneIndices.resize(count);
    // Execute reads only referenced vertices, first all strip pools and then
    // the trailing triangle pool. Do not demand inactive/unreferenced rows.
    const auto sourceVertex=[&](std::uint32_t vertex,bool strip) {
        const auto row=input.mdxDataOffset+vertex*mesh.mdxRowSize;
        out.positions[vertex]=mdx.vec3(row+input.mdxVertexOffset,"skin CPU position");
        out.normals[vertex]=mdx.vec3(row+mesh.mdxNormalOffset,"skin CPU normal");
        for(unsigned k=0;k<4;++k) {
            const auto weight=mdx.f32(row+mesh.mdxWeightOffset+k*4u,"skin CPU weight");
            out.weights[vertex][k]=weight;
            if(weight!=0.0f) {
                if(strip)out.stripBoneIndices[vertex][k]=mdx.f32(row+mesh.mdxBoneIndexOffset+k*4u,"skin CPU float bone index");
                else out.triangleBoneIndices[vertex][k]=mdx.u16(row+mesh.mdxBoneIndexOffset+k*2u,"skin CPU ushort bone index");
            }
        }
    };
    for(const auto vertex:out.stripVertices)sourceVertex(vertex,true);
    for(const auto vertex:out.triangleVertices)sourceVertex(vertex,false);
    return out;
}

std::vector<EngineFloatSkinChannels> decodeEngineFloatSkinChannels(const Mesh& mesh) {
    if(mesh.builtFloatSkinChannels)return *mesh.builtFloatSkinChannels;
    const auto& input=*mesh.engineCpuSkin;
    const neoshared::bif::JobControl job;
    ByteView mdx(input.mdx->data(),input.mdx->size(),"engine skin MDX",job);
    std::vector<EngineFloatSkinChannels> out(mesh.vertices.size());
    // K1 SetInterleavedBuffer 0x58918c..0x5891dc and K2 0x7394f0..0x739538
    // submit these channels as four floats. Preserve the source indices for
    // the engine shader's multiply/clamp/int conversion, without a ushort
    // reinterpretation or CPU weight sanitization.
    for(std::size_t vertex=0;vertex<out.size();++vertex) {
        const auto row=input.mdxDataOffset+vertex*mesh.mdxRowSize;
        for(unsigned k=0;k<4;++k) {
            out[vertex].weights[k]=mdx.f32(row+mesh.mdxWeightOffset+k*4u,"skin GPU weight");
            out[vertex].indices[k]=mdx.f32(row+mesh.mdxBoneIndexOffset+k*4u,"skin GPU float bone index");
        }
    }
    return out;
}

Quaternion unpackEngineControllerQuaternion(std::uint32_t packed, GameVersion game) noexcept {
    return engineAnimationPackedQuaternion(packed,game);
}

SkinSeamStatistics processK2SkinSeams(Model& model, const neoshared::bif::JobControl& job) {
    return processK2SkinSeamsImpl(model, job);
}

bool isAsciiModelInput(const std::vector<std::uint8_t>& mdlBytes) noexcept {
    // IODispatcher selects ASCII on a nonzero first byte. Preserve this
    // viewer's existing raw packed-payload input in addition to wrapped MDL.
    std::uint32_t firstWord{};
    if(mdlBytes.size()>=4u)for(unsigned i=0;i<4;++i)
        firstWord|=std::uint32_t(mdlBytes[i])<<(i*8u);
    return !mdlBytes.empty()&&mdlBytes.front()!=0&&!isModelToken(firstWord);
}

Model decodeModelBytes(const std::vector<std::uint8_t>& mdlBytes,
                       const std::vector<std::uint8_t>& mdxBytes,
                       const std::string& sourceLabel,
                       const ParseLimits& limits,
                       const neoshared::bif::JobControl& job,
                       std::optional<GameVersion> asciiGame,
                       const AsciiResourceResolver& asciiResources) {
    if(isAsciiModelInput(mdlBytes)) {
        if(!asciiGame)throw AsciiModelGameRequired();
        return decodeAsciiModelBytes(mdlBytes,*asciiGame,sourceLabel,job,asciiResources);
    }
    auto model=Parser(mdlBytes, mdxBytes, sourceLabel, limits, job).parse();
    initializeProceduralUvPools(model);
    return model;
}

Model readModelFile(const std::filesystem::path& mdlPath,
                    const std::optional<std::filesystem::path>& mdxPath,
                    const ParseLimits& limits,
                    const neoshared::bif::JobControl& job,
                    std::optional<GameVersion> asciiGame,
                    const AsciiResourceResolver& asciiResources) {
    const auto mdl = readBoundedFile(mdlPath, limits.maximumMdlBytes, job);
    std::vector<std::uint8_t> mdx;
    if (!isAsciiModelInput(mdl)) {
        std::optional<std::filesystem::path> companion = mdxPath;
        if (!companion) companion = caseInsensitiveSibling(mdlPath, ".mdx", job);
        if (companion) mdx = readBoundedFile(*companion, limits.maximumMdxBytes, job);
    }
    const AsciiResourceResolver looseResources=[&](const std::string& name,const std::string& extension)
        ->std::optional<std::vector<std::uint8_t>> {
        const auto source=mdlPath.parent_path()/std::filesystem::u8path(name+extension);
        const auto file=caseInsensitiveSibling(source,extension,job);
        if(!file)return {};
        return readBoundedFile(*file,limits.maximumMdlBytes,job);
    };
    return decodeModelBytes(mdl, mdx, neoshared::pathToUtf8(mdlPath), limits, job,
                            asciiGame,asciiResources?asciiResources:looseResources);
}

Mat4 identityMatrix() noexcept {
    Mat4 result;
    result.values = {1.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 1.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 1.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 1.0f};
    return result;
}

Mat4 multiply(const Mat4& left, const Mat4& right) noexcept {
    Mat4 result{};
    for (std::size_t column = 0u; column < 4u; ++column) {
        for (std::size_t row = 0u; row < 4u; ++row) {
            float value = 0.0f;
            for (std::size_t inner = 0u; inner < 4u; ++inner) {
                value += left.values[inner * 4u + row] * right.values[column * 4u + inner];
            }
            result.values[column * 4u + row] = value;
        }
    }
    return result;
}

Mat4 nodeTransform(const Vec3& position, const Quaternion& orientation) noexcept {
    const float xx = orientation.x * orientation.x;
    const float yy = orientation.y * orientation.y;
    const float zz = orientation.z * orientation.z;
    const float xy = orientation.x * orientation.y;
    const float xz = orientation.x * orientation.z;
    const float yz = orientation.y * orientation.z;
    const float wx = orientation.w * orientation.x;
    const float wy = orientation.w * orientation.y;
    const float wz = orientation.w * orientation.z;
    Mat4 result = identityMatrix();
    result.values[0] = 1.0f - 2.0f * (yy + zz);
    result.values[1] = 2.0f * (xy + wz);
    result.values[2] = 2.0f * (xz - wy);
    result.values[4] = 2.0f * (xy - wz);
    result.values[5] = 1.0f - 2.0f * (xx + zz);
    result.values[6] = 2.0f * (yz + wx);
    result.values[8] = 2.0f * (xz + wy);
    result.values[9] = 2.0f * (yz - wx);
    result.values[10] = 1.0f - 2.0f * (xx + yy);
    result.values[12] = position.x;
    result.values[13] = position.y;
    result.values[14] = position.z;
    return result;
}

Mat4 nodeTransform(const Vec3& position, const Quaternion& orientation, float uniformScale) noexcept {
    auto result = nodeTransform(position, orientation);
    if (!finite(uniformScale)) uniformScale = 1.0f;
    for (std::size_t column = 0u; column < 3u; ++column) {
        for (std::size_t row = 0u; row < 3u; ++row) {
            result.values[column * 4u + row] *= uniformScale;
        }
    }
    return result;
}

Vec3 transformPoint(const Mat4& matrix, const Vec3& point) noexcept {
    return {matrix.values[0] * point.x + matrix.values[4] * point.y +
                matrix.values[8] * point.z + matrix.values[12],
            matrix.values[1] * point.x + matrix.values[5] * point.y +
                matrix.values[9] * point.z + matrix.values[13],
            matrix.values[2] * point.x + matrix.values[6] * point.y +
                matrix.values[10] * point.z + matrix.values[14]};
}

Vec3 transformDirection(const Mat4& matrix, const Vec3& direction) noexcept {
    return normalize({matrix.values[0] * direction.x + matrix.values[4] * direction.y +
                          matrix.values[8] * direction.z,
                      matrix.values[1] * direction.x + matrix.values[5] * direction.y +
                          matrix.values[9] * direction.z,
                      matrix.values[2] * direction.x + matrix.values[6] * direction.y +
                          matrix.values[10] * direction.z});
}

std::optional<AabbHit> hitTestAabb(const Model& model, std::size_t nodeIndex,
                                   const Vec3& start, const Vec3& end,
                                   const AabbHitOptions& options) {
    if (nodeIndex >= model.nodes.size() || !finite(options.scale) ||
        !finite(start.x) || !finite(start.y) || !finite(start.z) ||
        !finite(end.x) || !finite(end.y) || !finite(end.z)) {
        return std::nullopt;
    }
    const auto& modelNode = model.nodes[nodeIndex];
    if (!modelNode.aabb || !modelNode.aabb->root || !modelNode.mesh ||
        *modelNode.mesh >= model.meshes.size()) {
        return std::nullopt;
    }
    const auto& tree = *modelNode.aabb;
    const auto& mesh = model.meshes[*modelNode.mesh];
    if (*tree.root >= tree.nodes.size()) return std::nullopt;

    const auto segmentIntersectsBox = [](const Vec3& from, const Vec3& to,
                                         const Vec3& minimum,
                                         const Vec3& maximum) noexcept {
        float first = 0.0f;
        float last = 1.0f;
        const std::array<float, 3> origin{{from.x, from.y, from.z}};
        const std::array<float, 3> target{{to.x, to.y, to.z}};
        const std::array<float, 3> low{{minimum.x, minimum.y, minimum.z}};
        const std::array<float, 3> high{{maximum.x, maximum.y, maximum.z}};
        for (std::size_t axis = 0u; axis < origin.size(); ++axis) {
            if (low[axis] > high[axis]) return false;
            const auto delta = target[axis] - origin[axis];
            if (delta == 0.0f) {
                if (origin[axis] < low[axis] || origin[axis] > high[axis])
                    return false;
                continue;
            }
            auto enter = (low[axis] - origin[axis]) / delta;
            auto leave = (high[axis] - origin[axis]) / delta;
            if (enter > leave) std::swap(enter, leave);
            first = std::max(first, enter);
            last = std::min(last, leave);
            if (first > last) return false;
        }
        return true;
    };

    const auto polygonHit = [](const MeshFace& face,
                               const std::array<Vec3, 3>& vertices,
                               const Vec3& from, const Vec3& to,
                               Vec3& intersection) noexcept {
        const auto dot3 = [](const Vec3& left, const Vec3& right) noexcept {
            return left.x * right.x + left.y * right.y + left.z * right.z;
        };
        if (!finite(face.normal.x) || !finite(face.normal.y) ||
            !finite(face.normal.z) || !finite(face.planeDistance)) {
            return false;
        }
        const auto startDistance = dot3(face.normal, from) + face.planeDistance;
        const auto endDistance = dot3(face.normal, to) + face.planeDistance;
        // polyhit is directed and uses exact comparisons for this plane gate.
        if (startDistance == endDistance || startDistance < 0.0f ||
            endDistance > 0.0f) {
            return false;
        }
        const auto fraction = startDistance / (startDistance - endDistance);
        const auto inverse = 1.0f - fraction;
        intersection = {from.x * inverse + to.x * fraction,
                        from.y * inverse + to.y * fraction,
                        from.z * inverse + to.z * fraction};

        constexpr float insideTolerance = -1.0e-5f;
        for (std::size_t index = 0u; index < vertices.size(); ++index) {
            const auto& current = vertices[index];
            const auto& next = vertices[(index + 1u) % vertices.size()];
            const auto edge = subtract(next, current);
            const auto relative = subtract(intersection, current);
            if (dot3(face.normal, cross(edge, relative)) < insideTolerance)
                return false;
        }
        return true;
    };

    std::uint32_t directionMask = end.x > start.x ? kAabbPositiveX : kAabbNegativeX;
    directionMask |= end.y > start.y ? kAabbPositiveY : kAabbNegativeY;
    directionMask |= end.z > start.z ? kAabbPositiveZ : kAabbNegativeZ;

    Vec3 mutableEnd = end;
    std::optional<AabbHit> result;
    std::size_t acceptedHits = 0u;
    std::vector<bool> active(tree.nodes.size(), false);

    std::function<std::size_t(std::size_t, std::size_t)> traverse;
    traverse = [&](std::size_t treeIndex, std::size_t depth) -> std::size_t {
        if (treeIndex >= tree.nodes.size() || depth > tree.nodes.size() ||
            active[treeIndex]) {
            return 0u;
        }
        const auto& node = tree.nodes[treeIndex];
        const Vec3 minimum{node.minimum.x * options.scale,
                           node.minimum.y * options.scale,
                           node.minimum.z * options.scale};
        const Vec3 maximum{node.maximum.x * options.scale,
                           node.maximum.y * options.scale,
                           node.maximum.z * options.scale};
        if (!segmentIntersectsBox(start, mutableEnd, minimum, maximum)) return 0u;

        if (node.leaf()) {
            if (node.faceIndex < 0 ||
                static_cast<std::size_t>(node.faceIndex) >= mesh.faces.size()) {
                return 0u;
            }
            const auto faceIndex = static_cast<std::size_t>(node.faceIndex);
            const auto& face = mesh.faces[faceIndex];
            const auto surfaceBit = face.surfaceMaterial & 31u;
            if ((options.surfaceMask & (1u << surfaceBit)) == 0u) {
                return 0u;
            }
            std::array<Vec3, 3> vertices{};
            for (std::size_t index = 0u; index < vertices.size(); ++index) {
                if (face.indices[index] < 0 ||
                    static_cast<std::size_t>(face.indices[index]) >=
                        (mesh.cpuPositions ? mesh.cpuPositions->size() : mesh.vertices.size())) {
                    return 0u;
                }
                const auto vertex = static_cast<std::size_t>(face.indices[index]);
                vertices[index] = mesh.cpuPositions ? (*mesh.cpuPositions)[vertex]
                                                     : mesh.vertices[vertex].position;
            }
            Vec3 intersection;
            if (!polygonHit(face, vertices, start, mutableEnd, intersection)) return 0u;
            mutableEnd = intersection;
            ++acceptedHits;
            AabbHit hit;
            hit.position = intersection;
            hit.normal = face.normal;
            hit.surfaceMaterial = face.surfaceMaterial;
            hit.surfaceMask = 1u << surfaceBit;
            hit.faceIndex = faceIndex;
            hit.treeNodeIndex = treeIndex;
            hit.intersectionCount = acceptedHits;
            result = hit;
            return 1u;
        }

        active[treeIndex] = true;
        const bool splitMatches = (node.splitMask & directionMask) != 0u;
        const bool firstChildFirst = options.testOpposite ? !splitMatches : splitMatches;
        const auto first = firstChildFirst ? node.firstChild : node.secondChild;
        const auto second = firstChildFirst ? node.secondChild : node.firstChild;
        std::size_t count = 0u;
        if (first >= 0) count += traverse(static_cast<std::size_t>(first), depth + 1u);
        if (second >= 0) count += traverse(static_cast<std::size_t>(second), depth + 1u);
        active[treeIndex] = false;
        return count;
    };

    const auto count = traverse(*tree.root, 0u);
    if (count == 0u || !result) return std::nullopt;
    result->intersectionCount = count;
    const auto direction = subtract(end, start);
    const auto offset = subtract(result->position, start);
    const double denominator = static_cast<double>(direction.x) * direction.x +
        static_cast<double>(direction.y) * direction.y +
        static_cast<double>(direction.z) * direction.z;
    result->segmentFraction = denominator > 0.0
        ? static_cast<float>((static_cast<double>(offset.x) * direction.x +
                              static_cast<double>(offset.y) * direction.y +
                              static_cast<double>(offset.z) * direction.z) / denominator)
        : 0.0f;
    return result;
}

NodeEmitterState bindNodeEmitter(const Node& node,bool lightningBranch) noexcept {
    NodeEmitterState result;
    // InternalCreateInstance invokes LightningEmitter::Initialize before the
    // base ControlDispatch (K2 Desktop 0x111fd9, 0x120098, 0xa803d). The ownerless
    // branch uses PartEmitter::Initialize and keeps its constructor default.
    if(!lightningBranch && node.emitter) {
        const auto& update=node.emitter->updateMode;
        // The engine factory uses exact strings and selects Lightning for
        // every update name other than these three, including an empty name.
        if(update!="Fountain" && update!="Explosion" && update!="Single")
            result.lifeExpectancy=1.0f;
    }
    if ((node.flags & kNodeEmitter)!=0u)
        applyEmitterControllers(node,0.0f,MaterialValueSource::ModelController,result,true);
    return result;
}

NodeLightState bindNodeLight(const Node& node) noexcept {
    NodeLightState result;
    if((node.flags&kNodeLight)!=0u)
        applyLightControllers(node,0.0f,MaterialValueSource::ModelController,result,true);
    return result;
}

NodeMaterialState bindNodeMaterial(const Node& node) noexcept {
    NodeMaterialState result;
    if ((node.flags & kNodeMesh) != 0u)
        applyMaterialControllers(node, 0.0f, MaterialValueSource::ModelController, result,true);
    return result;
}

const char* materialValueSourceName(MaterialValueSource source) noexcept {
    switch (source) {
    case MaterialValueSource::ModelController: return "base-model controller";
    case MaterialValueSource::AnimationController: return "animation controller";
    default: return "default";
    }
}

namespace {

void controlTransform(const Node& sourceNode,const Node& targetNode,
                      const Controller& controller,float firstTime,float secondTime,
                      float blend,bool sampledPair,float positionScale,
                      Vec3& position,Quaternion& orientation,GameVersion game) noexcept {





    const bool positionTarget=controller.type==kControllerPosition;
    const std::array<float*,4> destination=positionTarget
        ? std::array<float*,4>{{&position.x,&position.y,&position.z,&orientation.w}}
        : std::array<float*,4>{{&orientation.w,&orientation.x,&orientation.y,&orientation.z}};
    const auto addScalarSource=[&](float value) {
        if(controller.sourceOffset==16) value+=targetNode.position.x;
        else if(controller.sourceOffset==28) value+=targetNode.orientation.w;
        return value;
    };
    const auto prepareVector=[&](Vec3 value) {
        if(positionTarget) {
            value.x*=positionScale;value.y*=positionScale;value.z*=positionScale;
        }
        if(controller.sourceOffset==16) value=add(value,targetNode.position);
        else if(controller.sourceOffset==28)
            value=add(value,{targetNode.orientation.w,targetNode.orientation.x,targetNode.orientation.y});
        return value;
    };
    switch(controller.valueKind) {
    case ControllerValueKind::Float: {
        const auto sampled=controllerFloat(sourceNode,controller,secondTime,true);
        if(!sampled) return;
        const float second=addScalarSource(*sampled);
        float current=*destination[0];
        if(sampledPair) {
            const auto first=controllerFloat(sourceNode,controller,firstTime,true);
            if(!first) return;
            // Engine scalar Control adds the node source only to the second
            // sample. The vector branch adds it to both sampled endpoints.
            current=*first;
        }
        *destination[0]=engineAnimationLerp(current,second,blend);
        break;
    }
    case ControllerValueKind::Vector: {
        const auto sampled=controllerVector(sourceNode,controller,secondTime,true);
        if(!sampled) return;
        const auto second=prepareVector(*sampled);
        Vec3 current{*destination[0],*destination[1],*destination[2]};
        if(sampledPair) {
            const auto first=controllerVector(sourceNode,controller,firstTime,true);
            if(!first) return;
            current=prepareVector(*first);
        }
        *destination[0]=engineAnimationLerp(current.x,second.x,blend);
        *destination[1]=engineAnimationLerp(current.y,second.y,blend);
        *destination[2]=engineAnimationLerp(current.z,second.z,blend);
        break;
    }
    case ControllerValueKind::PackedQuaternion:
    case ControllerValueKind::Quaternion: {
        const auto second=controllerQuaternion(sourceNode,controller,secondTime,game);
        if(!second) return;
        Quaternion current{*destination[1],*destination[2],*destination[3],*destination[0]};
        if(sampledPair) {
            const auto first=controllerQuaternion(sourceNode,controller,firstTime,game);
            if(!first) return;
            current=*first;
        }
        // This branch ignores sourceOffset, including keyed orientation's 28.
        const auto value=engineAnimationSlerp(current,*second,blend,game);
        *destination[0]=value.w;*destination[1]=value.x;
        *destination[2]=value.y;*destination[3]=value.z;
        break;
    }
    case ControllerValueKind::Unknown: break;
    }
}

} // namespace

void constructedNodeFrame(const Node& node,Vec3& position,
                          Quaternion& orientation,GameVersion game) noexcept {
    position={};orientation={};
    // CreateInstanceTreeR initializes each Part, then runs the same typed
    // ControlDispatch(-1,0,1) used by animation playback in all four executables.
    // Engine Control uses the existing Part only for firstTime < -95, so -1
    // samples the controller twice. Even at blend 1 this matters: slerp can
    // return its first sample for dot >= 1, and an earlier controller's NaN
    // must not enter the retained zero-weight scalar/vector product.
    for(const auto& controller:node.controllers)
        if(controller.type==kControllerPosition || controller.type==kControllerOrientation)
            controlTransform(node,node,controller,-1.0f,0.0f,1.0f,true,1.0f,
                             position,orientation,game);
}

Quaternion engineQuaternionProduct(const Quaternion& left,
                                   const Quaternion& right) noexcept {
    return {
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
}

Quaternion engineQuaternionInverse(Quaternion value) noexcept {
    value.w = -value.w;
    return value;
}

Vec3 engineQuaternionRotate(const Quaternion& orientation,
                            const Vec3& value) noexcept {
    // All four executables initialize quaternionidentityoptimization to 1.
    // This shortcut belongs to Quaternion*Vector, not xdir/ydir/zdir.
    if (orientation.w == 1.0f || orientation.w == -1.0f) return value;
    const float xx = orientation.x * orientation.x;
    const float yy = orientation.y * orientation.y;
    const float zz = orientation.z * orientation.z;
    const float xy = orientation.x * orientation.y;
    const float xz = orientation.x * orientation.z;
    const float yz = orientation.y * orientation.z;
    const float wx = orientation.w * orientation.x;
    const float wy = orientation.w * orientation.y;
    const float wz = orientation.w * orientation.z;
    return {
        ((yy + zz) * -2.0f + 1.0f) * value.x +
            2.0f * (xy - wz) * value.y + 2.0f * (xz + wy) * value.z,
        2.0f * (xy + wz) * value.x +
            ((zz + xx) * -2.0f + 1.0f) * value.y + 2.0f * (yz - wx) * value.z,
        2.0f * (xz - wy) * value.x + 2.0f * (yz + wx) * value.y +
            ((xx + yy) * -2.0f + 1.0f) * value.z};
}

EngineFrame enginePartWorld(const Model& model, const Pose& pose,
                           const EngineFrame& owner, std::size_t nodeIndex) {
    EngineFrame result;
    for (std::optional<std::size_t> current = nodeIndex; current;
         current = model.nodes[*current].parent) {
        const auto index = *current;
        const float scale = pose.scales[index];
        result.position = engineQuaternionRotate(pose.orientations[index],
            {result.position.x * scale, result.position.y * scale,
             result.position.z * scale});
        result.position = add(result.position, pose.positions[index]);
        result.orientation = engineQuaternionProduct(pose.orientations[index],
                                                     result.orientation);
    }
    // The ordered equality bypasses multiplication only for exactly 1.
    if (owner.scale != 1.0f) {
        result.position = {result.position.x * owner.scale,
                           result.position.y * owner.scale,
                           result.position.z * owner.scale};
    }
    result.position = add(engineQuaternionRotate(owner.orientation, result.position),
                          owner.position);
    result.orientation = engineQuaternionProduct(owner.orientation, result.orientation);
    return result;
}

Pose bindPose(const Model& model, const neoshared::bif::JobControl& job) {
    job.check();
    Pose pose;
    pose.positions.reserve(model.nodes.size());
    pose.orientations.reserve(model.nodes.size());
    pose.scales.assign(model.nodes.size(), 1.0f);
    pose.nodeMaterials.reserve(model.nodes.size());
    pose.nodeLights.reserve(model.nodes.size());
    pose.nodeEmitters.reserve(model.nodes.size());
    for (const auto& node : model.nodes) {
        job.check();
        pose.positions.push_back(node.position);
        pose.orientations.push_back(node.orientation);
        pose.nodeMaterials.push_back(bindNodeMaterial(node));
        pose.nodeLights.push_back(bindNodeLight(node));
        pose.nodeEmitters.push_back(std::make_shared<NodeEmitterState>(bindNodeEmitter(node)));
    }
    calculatePoseTransformsAndBounds(model, pose, job);
    return pose;
}

namespace {

constexpr float kDanglyMinimumElapsed = 0.0125f;
constexpr float kDanglyMaximumElapsed = 0.035f;
constexpr float kDanglyTeleportDistance = 5.0f;
constexpr float kDanglySpringScale = 0.5f;
constexpr float kDanglyDampingScale = 1.5f;
constexpr float kDanglyWindScale = 20.0f;
constexpr float kDanglyConstraintScale = -255.0f;
constexpr float kDanglyCoordinateLimit = 10000000000.0f;

Vec3 scaledVector(const Vec3& value, float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

float engineDanglyMovementLength(const Vec3& value, GameVersion game) noexcept {
    // PartDanglyMesh calls magnitude before the >5 teleport branch. Both Desktop
    // helpers use float32 products/sums, including overflow and unordered
    // values (K1 0x3a4668; K2 0x3133ed).
    const float xx=value.x*value.x, yy=value.y*value.y, zz=value.z*value.z;
    return std::sqrt(game==GameVersion::Kotor1 ? (xx+yy)+zz : zz+(yy+xx));
}

float engineDanglyCoordinate(float candidate, float authored, float maximum) noexcept {
    const float delta=candidate-authored;
    if(delta>maximum)return authored+maximum;
    if(delta< -maximum)return authored-maximum;
    // The two ordered range checks retain the candidate only when both
    // succeed. An unordered axis restores that axis, not all three (K1
    // 0x2ddc47..0x2ddcf2; K2 0xfe77a..0xfe805).
    return maximum>=delta && delta>=-maximum ? candidate : authored;
}

const std::vector<Vec3>* engineDanglyRestPositions(const Mesh& mesh) {
    if (!mesh.dangly || !mesh.dangly->restPositions) return nullptr;
    const auto& positions = *mesh.dangly->restPositions;
    if (positions.size() != mesh.vertices.size())
        throw ModelError("Dangly engine rest-position pool has the wrong size");
    return &positions;
}

void initializeDanglyMeshState(const Mesh& mesh, DanglyMeshState& state) {
    const auto* rest = engineDanglyRestPositions(mesh);
    std::vector<Vec3> positions;
    positions.reserve(mesh.vertices.size());
    for (std::size_t index=0; index<mesh.vertices.size(); ++index) {
        // All four PartDanglyMesh constructors copy the authored rest pool;
        // they do not reject or repair its floating-point components.
        positions.push_back(rest ? (*rest)[index] : mesh.vertices[index].position);
    }
    state.localPositions = std::move(positions);
    state.localVelocities.assign(mesh.vertices.size(), Vec3{});
    state.previousPartPosition = {};
    state.previousPartOrientation = {};
    state.initialized = true;
}

void updateDanglyPoseBounds(const Model& model, Pose& pose) {
    Bounds bounds;
    const auto extend = [&bounds](const Vec3& point) {
        if (!bounds.valid) {
            bounds.minimum = bounds.maximum = point;
            bounds.valid = true;
            return;
        }
        bounds.minimum.x = std::min(bounds.minimum.x, point.x);
        bounds.minimum.y = std::min(bounds.minimum.y, point.y);
        bounds.minimum.z = std::min(bounds.minimum.z, point.z);
        bounds.maximum.x = std::max(bounds.maximum.x, point.x);
        bounds.maximum.y = std::max(bounds.maximum.y, point.y);
        bounds.maximum.z = std::max(bounds.maximum.z, point.z);
    };
    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        const auto& deformed = pose.deformedMeshes[meshIndex];
        if (deformed.valid && deformed.positions.size() == mesh.vertices.size()) {
            for (const auto& point : deformed.positions) extend(point);
        } else if (mesh.nodeIndex < pose.worldTransforms.size()) {
            for (const auto& vertex : mesh.vertices)
                extend(transformPoint(pose.worldTransforms[mesh.nodeIndex], vertex.position));
        }
    }
    pose.bounds = bounds;
}

} // namespace

void resetDanglySimulation(const Model& model, const Pose&,
                           DanglySimulationState& state) {
    state.meshes.clear();
    state.meshes.resize(model.meshes.size());
    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        if (!mesh.dangly || mesh.skinned) continue;
        initializeDanglyMeshState(mesh, state.meshes[meshIndex]);
    }
}

DanglySimulationState makeDanglySimulationState(const Model& model, const Pose& pose) {
    DanglySimulationState result;
    resetDanglySimulation(model, pose, result);
    return result;
}

void simulateDanglyMeshes(const Model& model, Pose& pose,
                          DanglySimulationState& state, float elapsedSeconds,
                          const DanglyEnvironment& environment) {
    if (state.meshes.size() != model.meshes.size())
        resetDanglySimulation(model, pose, state);
    if (pose.deformedMeshes.size() != model.meshes.size())
        pose.deformedMeshes.resize(model.meshes.size());

    // Both Desktop clamps retain unordered elapsed values; +/-infinity reaches
    // the corresponding endpoint (K1 0x2dd292; K2 0xfd8bb).
    const float elapsed = std::clamp(elapsedSeconds,
                                     kDanglyMinimumElapsed,
                                     kDanglyMaximumElapsed);

    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        if (!mesh.dangly || mesh.skinned ||
            mesh.nodeIndex >= pose.worldTransforms.size()) {
            continue;
        }
        auto& meshState = state.meshes[meshIndex];
        const auto* restPositions = engineDanglyRestPositions(mesh);
        if (!meshState.initialized ||
            meshState.localPositions.size() != mesh.vertices.size() ||
            meshState.localVelocities.size() != mesh.vertices.size()) {
            initializeDanglyMeshState(mesh, meshState);
        }

        const auto partFrame = enginePartWorld(model, pose, environment.objectFrame,
                                               mesh.nodeIndex);
        const auto& currentPosition = partFrame.position;
        const auto& currentOrientation = partFrame.orientation;
        const auto movement = subtract(currentPosition, meshState.previousPartPosition);
        const bool discontinuity = engineDanglyMovementLength(movement,model.game) > kDanglyTeleportDistance;

        if (discontinuity) {
            // PartDanglyMesh resets the streamed positions but deliberately
            // leaves VertexPrimitiveDangly's velocity array untouched.
            for (std::size_t index = 0u; index < mesh.vertices.size(); ++index)
                meshState.localPositions[index] = restPositions ? (*restPositions)[index] : mesh.vertices[index].position;
        } else {
            // The original runtime uses a zero vector part as its uninitialized
            // sentinel. Consequently an identity previous orientation refreshes
            // the previous transform on every call; preserve that exact quirk.
            if (meshState.previousPartOrientation.x == 0.0f &&
                meshState.previousPartOrientation.y == 0.0f &&
                meshState.previousPartOrientation.z == 0.0f) {
                meshState.previousPartOrientation = currentOrientation;
                meshState.previousPartPosition = currentPosition;
            }

            const Quaternion oldToCurrent = engineQuaternionProduct(
                engineQuaternionInverse(currentOrientation),
                meshState.previousPartOrientation);
            // Both Desktop callers translate in the previous frame, then rotate
            // that sum into the current frame (K1 0x2dd78d..0x2dd970,
            // K2 0xfdf09..0xfe43b). Preserve this arithmetic order.
            const Vec3 translationInPrevious = engineQuaternionRotate(
                engineQuaternionInverse(meshState.previousPartOrientation),
                subtract(meshState.previousPartPosition, currentPosition));

            Vec3 sceneWind{};
            if (environment.windDisplacement) {
                sceneWind = environment.windDisplacement(currentPosition, elapsed);
            }
            const Vec3 localWind = scaledVector(
                engineQuaternionRotate(engineQuaternionInverse(currentOrientation), sceneWind),
                kDanglyWindScale);
            const auto& data = *mesh.dangly;
            const float springBase = data.tightness * kDanglySpringScale * elapsed;
            const float damping = data.period * kDanglyDampingScale * elapsed;

            for (std::size_t index = 0u; index < mesh.vertices.size(); ++index) {
                const float constraint = index < data.constraints.size()
                    ? data.constraints[index] : 0.0f;
                if (constraint == 0.0f) continue;

                const Vec3 authored = restPositions ? (*restPositions)[index] : mesh.vertices[index].position;
                Vec3 vertex = engineQuaternionRotate(oldToCurrent,
                    add(meshState.localPositions[index],translationInPrevious));
                const Vec3 transformedVelocity = engineQuaternionRotate(
                    oldToCurrent, meshState.localVelocities[index]);
                const float spring = constraint * springBase;
                Vec3 velocity = add(
                    transformedVelocity,
                    subtract(scaledVector(subtract(authored, vertex), spring),
                             scaledVector(transformedVelocity, damping)));
                meshState.localVelocities[index] = velocity;

                Vec3 candidate = add(vertex, scaledVector(velocity, elapsed));
                const float maximum =
                    (constraint / kDanglyConstraintScale + 1.0f) * data.displacement;
                candidate = add(candidate, scaledVector(localWind, maximum));

                const Vec3 delta = subtract(candidate, authored);
                if (delta.x < -kDanglyCoordinateLimit ||
                    delta.y < -kDanglyCoordinateLimit ||
                    delta.z < -kDanglyCoordinateLimit ||
                    delta.x > kDanglyCoordinateLimit ||
                    delta.y > kDanglyCoordinateLimit ||
                    delta.z > kDanglyCoordinateLimit) {
                    candidate = authored;
                } else {
                    candidate.x=engineDanglyCoordinate(candidate.x,authored.x,maximum);
                    candidate.y=engineDanglyCoordinate(candidate.y,authored.y,maximum);
                    candidate.z=engineDanglyCoordinate(candidate.z,authored.z,maximum);
                }
                meshState.localPositions[index] = candidate;
            }
        }

        meshState.previousPartPosition = currentPosition;
        meshState.previousPartOrientation = currentOrientation;

        auto& deformed = pose.deformedMeshes[meshIndex];
        deformed.valid = true;
        deformed.preserveTangentBasis = true;
        deformed.positions.resize(mesh.vertices.size());
        deformed.normals.resize(mesh.vertices.size());
        deformed.tangents.resize(mesh.vertices.size());
        deformed.bitangents.resize(mesh.vertices.size());
        deformed.basisNormals.resize(mesh.vertices.size());
        deformed.exactTangentBasis.assign(mesh.vertices.size(), 0u);
        const auto& modelTransform = pose.worldTransforms[mesh.nodeIndex];
        deformed.danglyPartFrame = modelTransform;
        for (std::size_t index = 0u; index < mesh.vertices.size(); ++index) {
            const auto& source = mesh.vertices[index];
            deformed.positions[index] = transformPoint(
                modelTransform, meshState.localPositions[index]);
            // Animate writes only the dynamic position pool. Normals and the
            // authored tangent frame remain static local channels and receive
            // the ordinary part transform at draw time.
            deformed.normals[index] = transformDirection(modelTransform, mesh.normalAt(index));
            if (source.tangentBasis.valid) {
                deformed.tangents[index] = transformDirection(
                    modelTransform, source.tangentBasis.tangent);
                deformed.bitangents[index] = transformDirection(
                    modelTransform, source.tangentBasis.bitangent);
                deformed.basisNormals[index] = transformDirection(
                    modelTransform, source.tangentBasis.normal);
            } else {
                deformed.tangents[index] = {};
                deformed.bitangents[index] = {};
                deformed.basisNormals[index] = {};
            }
        }
    }

    updateDanglyPoseBounds(model, pose);
}

void simulateDanglyMeshes(const Model& model, Pose& pose,
                          DanglySimulationState& state, float elapsedSeconds,
                          Vec3 windVelocity) {
    DanglyEnvironment environment;
    environment.windDisplacement = [windVelocity](Vec3, float seconds) {
        return scaledVector(windVelocity, seconds);
    };
    simulateDanglyMeshes(model, pose, state, elapsedSeconds, environment);
}

std::optional<AnimationReference> findAnimation(
    const std::vector<const Model*>& hierarchy, const std::string& name) {
    const auto count = contiguousHierarchySize(hierarchy);
    if (count == 0u) return std::nullopt;

    for (std::size_t modelIndex = 0u; modelIndex < count; ++modelIndex) {
        if (const auto animationIndex = animationIndexByName(*hierarchy[modelIndex], name)) {
            return AnimationReference{modelIndex, *animationIndex, modelIndex,
                                      inheritedPositionScale(hierarchy, modelIndex), false};
        }
    }

    // FindAnimation only performs the default fallback after reaching the end
    // of the supermodel chain. An intermediary default is not a fallback.
    const auto terminal = count - 1u;
    if (const auto animationIndex = animationIndexByName(*hierarchy[terminal], "default")) {
        return AnimationReference{terminal, *animationIndex, terminal,
                                  inheritedPositionScale(hierarchy, terminal), true};
    }
    return std::nullopt;
}

std::vector<AnimationReference> enumerateAnimations(
    const std::vector<const Model*>& hierarchy) {
    std::vector<AnimationReference> result;
    std::unordered_set<std::string> seen;
    const auto count = contiguousHierarchySize(hierarchy);
    for (std::size_t modelIndex = 0u; modelIndex < count; ++modelIndex) {
        const auto& model = *hierarchy[modelIndex];
        const auto positionScale = inheritedPositionScale(hierarchy, modelIndex);
        for (std::size_t animationIndex = 0u;
             animationIndex < model.animations.size(); ++animationIndex) {
            const auto key = lowercase(model.animations[animationIndex].name);
            if (!seen.emplace(key).second) continue;
            result.push_back(AnimationReference{modelIndex, animationIndex, modelIndex,
                                                positionScale, false});
        }
    }
    return result;
}

std::vector<AnimationEventOccurrence> animationEventsBetween(
    const Animation& animation, float startTime, float endTime, std::uint32_t flags) {
    std::vector<AnimationEventOccurrence> result;
    constexpr float minimumEventTime = -1000000000.0f;
    constexpr float maximumEventTime = 1000000000.0f;

    if ((flags & kAnimationEventFullRange) != 0u) {
        // This is the literal AnimateEvents behavior: bit 1 replaces the range
        // with finite sentinels and is then cleared. In the observed executables,
        // combining it with reverse playback produces two empty wrapped ranges.
        startTime = minimumEventTime;
        endTime = maximumEventTime;
        flags &= ~kAnimationEventFullRange;
    }

    if ((flags & kAnimationEventReverse) != 0u) {
        if (endTime > startTime) {
            appendReverseEvents(result, animation, startTime, minimumEventTime);
            appendReverseEvents(result, animation, maximumEventTime, endTime);
        } else {
            appendReverseEvents(result, animation, startTime, endTime);
        }
    } else {
        if (startTime > endTime) {
            appendForwardEvents(result, animation, startTime, maximumEventTime);
            appendForwardEvents(result, animation, minimumEventTime, endTime);
        } else {
            appendForwardEvents(result, animation, startTime, endTime);
        }
    }
    return result;
}

Pose evaluateAnimation(const Model& model, std::size_t animationIndex, float time, bool loop,
                       const neoshared::bif::JobControl& job) {
    return evaluateAnimation(model, model, animationIndex, time, 1.0f, loop, job);
}

Pose evaluateAnimation(const Model& targetModel, const Model& animationModel,
                       std::size_t animationIndex, float time,
                       float positionScale, bool loop, const neoshared::bif::JobControl& job) {
    job.check();
    if (animationIndex >= animationModel.animations.size()) {
        throw ModelError("Animation index is outside the parsed model animation array");
    }
    Pose pose = bindPose(targetModel, job);
    const auto& animation = animationModel.animations[animationIndex];
    if (!finite(time)) time = 0.0f;
    if (animation.length > 0.0f) {
        if (loop) {
            time = std::fmod(time, animation.length);
            if (time < 0.0f) time += animation.length;
        } else {
            time = std::clamp(time, 0.0f, animation.length);
        }
    } else {
        time = 0.0f;
    }
    pose.time = time;

    const auto sourceRoot = animationRootNode(animation);
    // Use the same Part-child ordinal/ID dispatch as scheduled playback.
    // A source-node -> single-target map loses repeated live occurrences.
    const auto nodePairs = engineAnimationNodePairs(targetModel, animation);
    for (const auto& [sourceIndex, target] : nodePairs) {
        job.check();
        if (sourceRoot && sourceIndex == *sourceRoot)
            pose.controlledRootNode = target;
        const auto& animationNode = animation.nodes[sourceIndex];
        controlAnimMeshNode(targetModel, animationNode, target, time, 1.0f, pose);
        if ((targetModel.nodes[target].flags & kNodeEmitter)!=0u)
            applyEmitterControllers(animationNode,time,MaterialValueSource::AnimationController,
                                    *pose.nodeEmitters[target]);
        if((targetModel.nodes[target].flags & kNodeLight)!=0u)
            applyLightControllers(animationNode,time,MaterialValueSource::AnimationController,
                                  pose.nodeLights[target]);
        if ((targetModel.nodes[target].flags & kNodeMesh) != 0u) {
            // Apply only to the synchronized, controlled target. Do not scale
            // material values with inherited animationScale and do not inherit
            // alpha down the transform hierarchy. Missing tracks keep the
            // child's base-model value, not the donor model's default.
            applyMaterialControllers(animationNode, time,
                MaterialValueSource::AnimationController, pose.nodeMaterials[target]);
        }

        for (const auto& controller : animationNode.controllers) {
            switch (controller.type) {
            case kControllerPosition:
                if (const auto sampled = controllerVector(animationNode, controller, time)) {
                    auto value = *sampled;
                    value.x *= positionScale;
                    value.y *= positionScale;
                    value.z *= positionScale;
                    pose.positions[target] = value;
                }
                break;
            case kControllerOrientation:
                if (const auto value = controllerQuaternion(animationNode, controller, time))
                    pose.orientations[target] = *value;
                break;
            case kControllerScale:
                if (const auto value = controllerFloat(animationNode, controller, time))
                    pose.scales[target] = *value;
                break;
            default:
                break;
            }
        }
    }
    calculatePoseTransformsAndBounds(targetModel, pose, job);
    return pose;
}

namespace {

float runBlendFloat(float current, float target, float blend,
                    std::optional<GameVersion> engineGame) noexcept {
    return engineGame ? engineAnimationLerp(current, target, blend)
                      : current + (target - current) * blend;
}

Vec3 runBlendVector(Vec3 current, Vec3 target, float blend,
                    std::optional<GameVersion> engineGame) noexcept {
    return {runBlendFloat(current.x, target.x, blend, engineGame),
            runBlendFloat(current.y, target.y, blend, engineGame),
            runBlendFloat(current.z, target.z, blend, engineGame)};
}

struct AnimationControllerSample {
    float firstTime{};
    float secondTime{};
    float blend{};
    bool sampledPair{};
};

std::optional<float> runFloatSample(const Node& node,
                                    const Controller& controller,
                                    const AnimationControllerSample& sample,
                                    float current,
                                    std::optional<GameVersion> engineGame) noexcept {
    const auto second = controllerFloat(node, controller, sample.secondTime, engineGame.has_value());
    if (!second) return std::nullopt;
    if (!sample.sampledPair)
        return runBlendFloat(current, *second, sample.blend, engineGame);
    const auto first = controllerFloat(node, controller, sample.firstTime, engineGame.has_value());
    return first ? runBlendFloat(*first, *second, sample.blend, engineGame) : second;
}

std::optional<Vec3> runVectorSample(const Node& node,
                                    const Controller& controller,
                                    const AnimationControllerSample& sample,
                                    Vec3 current,
                                    std::optional<GameVersion> engineGame) noexcept {
    const auto second = controllerVector(node, controller, sample.secondTime, engineGame.has_value());
    if (!second) return std::nullopt;
    if (!sample.sampledPair)
        return runBlendVector(current, *second, sample.blend, engineGame);
    const auto first = controllerVector(node, controller, sample.firstTime, engineGame.has_value());
    return first ? runBlendVector(*first, *second, sample.blend, engineGame) : second;
}

std::optional<Vec3> runColorSample(const Node& node,
                                  const Controller& controller,
                                  const AnimationControllerSample& sample,
                                  Vec3 current,
                                  std::optional<GameVersion> engineGame) noexcept {
    // AddSingle selects the value kind from the parsed component count.
    // A scalar targeting a color controls its first float only; Control leaves
    // the other two destination words untouched, including their exact bits.
    if (engineGame && controller.valueKind == ControllerValueKind::Float) {
        const auto value = runFloatSample(node,controller,sample,current.x,engineGame);
        if (!value) return std::nullopt;
        current.x = *value;
        return current;
    }
    return runVectorSample(node,controller,sample,current,engineGame);
}

void applyAnimationControllerSample(
    const Model& targetModel, const Animation& animation,
    float positionScale, const AnimationControllerSample& sample,
    Pose& pose) {
    const std::optional<GameVersion> engineGame = targetModel.game;
    // Engine AnimateHierarchy consumes the finalized node tree directly.
    // animroot pruning belongs to authoring; its name is only editor metadata
    // here and must not filter already admitted controller nodes a second time.
    const auto sourceRoot = animationRootNode(animation);
    const auto nodePairs = engineAnimationNodePairs(targetModel, animation);
    for (const auto& [sourceIndex,target] : nodePairs) {
        if (sourceRoot && sourceIndex == *sourceRoot)
            pose.controlledRootNode = target;
        if (target >= targetModel.nodes.size() ||
            target >= pose.positions.size() ||
            target >= pose.orientations.size() ||
            target >= pose.scales.size()) {
            continue;
        }
        const auto& targetNode = targetModel.nodes[target];
        const auto& animationNode = animation.nodes[sourceIndex];
        controlAnimMeshNode(targetModel, animationNode, target,
                            sample.secondTime, sample.blend, pose);

        for (const auto& controller : animationNode.controllers) {
            if(controller.type==kControllerPosition || controller.type==kControllerOrientation) {
                controlTransform(animationNode,targetNode,controller,
                    sample.firstTime,sample.secondTime,sample.blend,sample.sampledPair,
                    positionScale,pose.positions[target],pose.orientations[target],*engineGame);
            } else if(controller.type==kControllerScale) {
                if (const auto value = runFloatSample(
                        animationNode, controller, sample,
                        pose.scales[target], engineGame)) {
                    pose.scales[target] = *value;
                }
            }

            // Controller numbers are contextual in K1/K2. In particular, raw
            // controller 100 means mesh self-illumination, light vertical
            // displacement, or an emitter field depending on the node type.
            // Dispatch each namespace only after checking the target node.
            if ((targetNode.flags & kNodeMesh) != 0u &&
                target < pose.nodeMaterials.size()) {
                auto& material = pose.nodeMaterials[target];
                if (controller.type == kControllerAlpha) {
                    if (const auto value = runFloatSample(
                            animationNode, controller, sample,
                            material.alpha, engineGame)) {
                        material.alpha = *value;
                        material.alphaSource =
                            MaterialValueSource::AnimationController;
                    }
                } else if (controller.type == kControllerSelfIllumColor) {
                    const Vec3 current{material.selfIllumination[0],
                                       material.selfIllumination[1],
                                       material.selfIllumination[2]};
                    if (const auto value = runColorSample(
                            animationNode, controller, sample, current, engineGame)) {
                        material.selfIllumination =
                            {{value->x, value->y, value->z}};
                        material.selfIlluminationSource =
                            MaterialValueSource::AnimationController;
                    }
                }
            }

            if ((targetNode.flags & kNodeLight) != 0u &&
                target < pose.nodeLights.size()) {
                auto& light = pose.nodeLights[target];
                if (controller.type == kControllerLightColor) {
                    const Vec3 current{light.color[0], light.color[1],
                                       light.color[2]};
                    if (const auto value = runColorSample(
                            animationNode, controller, sample, current, engineGame)) {
                        light.color = {{value->x, value->y, value->z}};
                        light.sources[0] =
                            MaterialValueSource::AnimationController;
                    }
                } else {
                    float* field = nullptr;
                    std::size_t sourceSlot = 0u;
                    if (controller.type == kControllerLightRadius) {
                        field = &light.radius;
                        sourceSlot = 1u;
                    } else if (controller.type == kControllerLightShadowRadius) {
                        field = &light.shadowRadius;
                        sourceSlot = 2u;
                    } else if (controller.type ==
                               kControllerLightVerticalDisplacement) {
                        field = &light.verticalDisplacement;
                        sourceSlot = 3u;
                    } else if (controller.type == kControllerLightMultiplier) {
                        field = &light.multiplier;
                        sourceSlot = 4u;
                    }
                    if (field) {
                        if (const auto value = runFloatSample(
                                animationNode, controller, sample, *field, engineGame)) {
                            *field = *value;
                            light.sources[sourceSlot] =
                                MaterialValueSource::AnimationController;
                        }
                    }
                }
            }

            if ((targetNode.flags & kNodeEmitter) != 0u &&
                target < pose.nodeEmitters.size()) {
                auto& emitter = *pose.nodeEmitters[target];
                if(controller.valueKind==ControllerValueKind::Vector) {
                    if(const auto bindings=emitterVectorBindings(controller.type)) {
                        const Vec3 current{emitter.*((*bindings)[0]->field),
                                           emitter.*((*bindings)[1]->field),
                                           emitter.*((*bindings)[2]->field)};
                        if(const auto value=runVectorSample(
                                animationNode,controller,sample,current,engineGame)) {
                            const std::array<float,3> values{{value->x,value->y,value->z}};
                            for(std::size_t lane=0;lane<bindings->size();++lane) {
                                const auto& binding=*(*bindings)[lane];
                                emitter.*(binding.field)=values[lane];
                                emitter.sources[static_cast<std::size_t>(binding.property)]=
                                    MaterialValueSource::AnimationController;
                            }
                        }
                        continue;
                    }
                }
                if (controller.type == kControllerEmitterColorStart ||
                    controller.type == kControllerEmitterColorMid ||
                    controller.type == kControllerEmitterColorEnd) {
                    std::array<float, 3>* field = &emitter.colorStart;
                    EmitterProperty property = EmitterProperty::ColorStart;
                    if (controller.type == kControllerEmitterColorMid) {
                        field = &emitter.colorMid;
                        property = EmitterProperty::ColorMid;
                    } else if (controller.type == kControllerEmitterColorEnd) {
                        field = &emitter.colorEnd;
                        property = EmitterProperty::ColorEnd;
                    }
                    const Vec3 current{(*field)[0], (*field)[1], (*field)[2]};
                    if (const auto value = runColorSample(
                            animationNode, controller, sample, current, engineGame)) {
                        *field = {{value->x, value->y, value->z}};
                        emitter.sources[static_cast<std::size_t>(property)] =
                            MaterialValueSource::AnimationController;
                    }
                } else {
                    for (const auto& binding : kEmitterScalarBindings) {
                        if (controller.type != binding.id) continue;
                        float& field = emitter.*(binding.field);
                        if (const auto value = runFloatSample(
                                animationNode, controller, sample, field, engineGame)) {
                            field = *value;
                            emitter.sources[static_cast<std::size_t>(
                                binding.property)] =
                                MaterialValueSource::AnimationController;
                        }
                        break;
                    }
                }
            }
        }
    }
}

AnimationControllerSample normalRunSample(const AnimationRun& run) noexcept {
    return {-100.0f - run.previousTime, run.time, run.weight, false};
}

AnimationControllerSample outOfOrderRunSample(
    const AnimationRun& run, bool boundary) noexcept {
    AnimationControllerSample sample;
    if (run.outOfOrderAnimationTimes.empty() ||
        run.outOfOrderControlTimes.empty()) {
        sample.secondTime = run.time;
        sample.blend = run.weight;
        return sample;
    }

    const auto count = std::min(run.outOfOrderAnimationTimes.size(),
                                run.outOfOrderControlTimes.size());
    if (count == 0u) {
        sample.secondTime = run.time;
        sample.blend = run.weight;
        return sample;
    }
    const float normalizedTime = run.outOfOrderDuration != 0.0f
        ? run.time / run.outOfOrderDuration : 0.0f;
    std::size_t upper = 0u;
    if (normalizedTime > run.outOfOrderControlTimes[0]) {
        while (upper + 1u < count &&
               normalizedTime > run.outOfOrderControlTimes[upper + 1u]) {
            ++upper;
        }
        if (upper + 1u < count) ++upper;
        else upper = count;
    }

    if (boundary || upper == 0u || upper >= count) {
        const auto endpoint = upper >= count ? count - 1u : upper;
        sample.firstTime = -100.0f;
        sample.secondTime = run.outOfOrderAnimationTimes[endpoint];
        sample.blend = run.weight;
        sample.sampledPair = false;
        return sample;
    }

    const auto lower = upper - 1u;
    const float firstControl = run.outOfOrderControlTimes[lower];
    const float secondControl = run.outOfOrderControlTimes[upper];
    const float denominator = secondControl - firstControl;
    const float fraction = denominator != 0.0f
        ? (normalizedTime - firstControl) / denominator : 0.0f;
    sample.firstTime = run.outOfOrderAnimationTimes[lower];
    sample.secondTime = run.outOfOrderAnimationTimes[upper];
    sample.blend = fraction;
    sample.sampledPair = true;
    return sample;
}

bool equalsIgnoreCase(const std::string& left,
                      const std::string& right) noexcept {
    return lowercase(left) == lowercase(right);
}

Pose engineAnimationBindPose(const Model& model,
                             const neoshared::bif::JobControl& job = {}) {
    job.check();
    Pose pose;
    pose.positions.resize(model.nodes.size());
    pose.orientations.resize(model.nodes.size());
    pose.scales.assign(model.nodes.size(),1.0f);
    pose.nodeMaterials.reserve(model.nodes.size());
    pose.nodeLights.reserve(model.nodes.size());
    pose.nodeEmitters.reserve(model.nodes.size());
    for (std::size_t index = 0u; index < model.nodes.size(); ++index) {
        job.check();
        const auto& node = model.nodes[index];
        pose.nodeMaterials.push_back(bindNodeMaterial(node));
        pose.nodeLights.push_back(bindNodeLight(node));
        pose.nodeEmitters.push_back(std::make_shared<NodeEmitterState>(bindNodeEmitter(node)));




        constructedNodeFrame(node,pose.positions[index],pose.orientations[index],
                             model.game);
        for (const auto& controller : node.controllers) {
            if (controller.type != kControllerScale) continue;
            if (const auto scale = bindControllerFloat(node,controller,0.0f))
                pose.scales[index] = *scale;
        }
    }
    calculatePoseTransformsAndBounds(model,pose,job,true);
    return pose;
}

} // namespace

Pose constructedBindPose(const Model& model, const neoshared::bif::JobControl& job) {
    return engineAnimationBindPose(model,job);
}

const char* animationRunPriorityName(const AnimationRun& run) noexcept {
    if (run.outOfOrder()) return "out-of-order";
    if (run.overlay()) return "overlay";
    if (run.queued()) return "queued";
    return "base";
}

const char* animationRuntimeEventName(AnimationRuntimeEventKind kind) noexcept {
    switch (kind) {
    case AnimationRuntimeEventKind::Started: return "started";
    case AnimationRuntimeEventKind::Loop: return "loop";
    case AnimationRuntimeEventKind::Completed: return "completed";
    case AnimationRuntimeEventKind::AnimationEvent: return "animation event";
    }
    return "animation event";
}

void AnimationRuntime::setModel(const Model* targetModel,
                                std::vector<const Model*> hierarchy,
                                const neoshared::bif::JobControl& job) {
    job.check();
    targetModel_ = targetModel;
    hierarchy_ = std::move(hierarchy);
    if (targetModel_ &&
        (hierarchy_.empty() || hierarchy_.front() != targetModel_)) {
        hierarchy_.insert(hierarchy_.begin(), targetModel_);
    }
    runs_.clear();
    events_.clear();
    nextRunId_ = 1u;
    lookAtAnimation_.reset();lookAtRun_.reset();
    pose_ = targetModel_ ? engineAnimationBindPose(*targetModel_,job) : Pose{};
    proceduralUvParts_=targetModel_ ? createProceduralUvParts(*targetModel_)
                                   : std::vector<ProceduralUvPart>{};
}

void AnimationRuntime::advanceProceduralUv(ParticleRandom& random, float elapsedSeconds) {
    if (!targetModel_) return;


    if (elapsedSeconds<=0.0f && !(targetModel_->classification&0x10u)) return;
    animateProceduralUv(*targetModel_,proceduralUvParts_,random);
}

void AnimationRuntime::clear(bool resetToBindPose) {
    runs_.clear();
    events_.clear();
    lookAtAnimation_.reset();lookAtRun_.reset();
    if (resetToBindPose && targetModel_)
        pose_ = engineAnimationBindPose(*targetModel_);
}

const AnimationRun* AnimationRuntime::run(std::uint64_t id) const noexcept {
    const auto index = runIndex(id);
    return index ? &runs_[*index] : nullptr;
}

AnimationRun* AnimationRuntime::run(std::uint64_t id) noexcept {
    const auto index = runIndex(id);
    return index ? &runs_[*index] : nullptr;
}

std::optional<std::size_t> AnimationRuntime::runIndex(
    std::uint64_t id) const noexcept {
    for (std::size_t index = 0u; index < runs_.size(); ++index) {
        if (runs_[index].id == id) return index;
    }
    return std::nullopt;
}

const Animation* AnimationRuntime::animationFor(
    const AnimationReference& reference) const noexcept {
    if(reference.modelIndex==std::numeric_limits<std::size_t>::max())
        return lookAtAnimation_?&*lookAtAnimation_:nullptr;
    if (reference.modelIndex >= hierarchy_.size() ||
        hierarchy_[reference.modelIndex] == nullptr) {
        return nullptr;
    }
    const auto& animations = hierarchy_[reference.modelIndex]->animations;
    return reference.animationIndex < animations.size()
        ? &animations[reference.animationIndex] : nullptr;
}

void AnimationRuntime::setEventCallback(
    EventCallback callback, std::weak_ptr<const void> ownerLifetime) {
    eventCallback_ = std::move(callback);
    eventOwnerLifetime_ = std::move(ownerLifetime);
}

AnimationRun& AnimationRuntime::borrowedRun(std::uint64_t id) {
    auto* value = run(id);
    if (!value)
        throw std::logic_error("Engine animation callback destroyed its borrowed AnimRun");
    return *value;
}

bool AnimationRuntime::emit(AnimationRuntimeEventKind kind,
                            const AnimationRun& runValue,
                            float time, std::string name,
                            bool hierarchyUpdated) {
    AnimationRuntimeEvent event;
    event.kind = kind;
    event.runId = runValue.id;
    event.animationName = runValue.name;
    event.time = time;
    event.name = std::move(name);
    event.hierarchyUpdated = hierarchyUpdated;
    events_.push_back(event);
    // Both copies precede DoEvent: the producer may destroy this runtime,
    // replace its callback or re-enter PlayAnimation while this call is active.
    const auto callback = eventCallback_;
    const auto lifetime = eventOwnerLifetime_;
    if (!callback) return true;
    callback(event);
    return !lifetime.expired();
}

bool AnimationRuntime::emitAnimationEvents(const AnimationRun& runValue,
                                           bool hierarchyUpdated) {
    const auto* animation = animationFor(runValue.animation);
    if (!animation) return true;
    const auto eventRun = runValue;
    const auto flags = runValue.playbackRate < 0.0f
        ? kAnimationEventReverse : 0u;
    const auto occurrences = animationEventsBetween(
        *animation, runValue.previousTime, runValue.time, flags);
    for (std::size_t index = 0u; index < occurrences.size(); ++index) {
        const auto& occurrence = occurrences[index];
        if (!emit(AnimationRuntimeEventKind::AnimationEvent, eventRun,
                  occurrence.time, occurrence.name, hierarchyUpdated)) {
            // AnimateEvents has no owner SafePointer of its own. Destruction
            // at the last authored event reaches Gob::Animate's tail check;
            // another event would dereference the already-destroyed Gob.
            if (index + 1u != occurrences.size())
                throw std::logic_error("Engine AnimateEvents requires a live owner for the next event");
            return false;
        }
    }
    return true;
}

AnimationPlayResult AnimationRuntime::play(
    const std::string& name, float playbackRate,
    std::uint32_t flags, float startTime) {
    AnimationPlayResult result;
    if (!targetModel_) {
        result.diagnostic = "No target model is assigned to the animation runtime";
        return result;
    }
    if (name.empty() || equalsIgnoreCase(name, "NULL")) {
        clear(false);
        result.cleared = true;
        return result;
    }

    std::optional<std::size_t> matched;
    if ((flags & kAnimationRunAllowDuplicate) == 0u) {
        for (std::size_t index = runs_.size(); index-- > 0u;) {
            if (equalsIgnoreCase(runs_[index].name, name)) {
                matched = index;
                break;
            }
        }
    }
    if (matched && (flags & kAnimationRunForceStop) != 0u) {
        auto& existing = runs_[*matched];
        existing.flags |= kAnimationRunNonLooping | kAnimationRunForceStop;
        result.runId = existing.id;
        return result;
    }

    if (equalsIgnoreCase(name, "current")) {
        matched.reset();
        for (std::size_t index = 0u; index < runs_.size(); ++index) {
            if ((runs_[index].flags &
                 (kAnimationRunOverlay | kAnimationRunQueued)) == 0u) {
                matched = index;
                break;
            }
        }
        if (!matched) {
            result.diagnostic = "No current base animation run exists";
            return result;
        }
        if ((flags & kAnimationRunStop) != 0u) {
            const auto value = runs_[*matched];
            runs_.erase(runs_.begin() + static_cast<std::ptrdiff_t>(*matched));
            if (!emit(AnimationRuntimeEventKind::Completed, value,
                      value.time, "done" + value.name))
                throw std::logic_error("Engine PlayAnimation stop requires its owner after DoEvent");
            result.cleared = true;
            return result;
        }
        const auto currentId = runs_[*matched].id;
        return playResolved(runs_[*matched].animation, runs_[*matched].name,
                            playbackRate, flags, startTime, false,
                            currentId);
    }

    if (matched && (flags & kAnimationRunStop) != 0u) {
        const auto value = runs_[*matched];
        runs_.erase(runs_.begin() + static_cast<std::ptrdiff_t>(*matched));
        if (!emit(AnimationRuntimeEventKind::Completed, value,
                  value.time, "done" + value.name))
            throw std::logic_error("Engine PlayAnimation stop requires its owner after DoEvent");
        result.cleared = true;
        return result;
    }

    if (!matched && (flags & kAnimationRunStop) != 0u) {
        result.diagnostic =
            "No matching active animation run exists to stop: " + name;
        return result;
    }

    const auto reference = findAnimation(hierarchy_, name);
    if (!reference) {
        result.diagnostic = "Animation was not found in the model hierarchy: " + name;
        return result;
    }
    const auto reuseId = matched
        ? std::optional<std::uint64_t>{runs_[*matched].id}
        : std::nullopt;
    // The engine performs its duplicate-name lookup before FindAnimation. A
    // failed name can resolve to the terminal default clip, but that must not
    // retroactively match an already-running clip named "default".
    return playResolved(*reference, name, playbackRate, flags,
                        startTime, false, reuseId);
}

AnimationPlayResult AnimationRuntime::play(
    const AnimationReference& animation, float playbackRate,
    std::uint32_t flags, float startTime) {
    const auto* clip = animationFor(animation);
    if (!clip) {
        AnimationPlayResult result;
        result.diagnostic = "Resolved animation reference is outside the model hierarchy";
        return result;
    }
    return playResolved(animation, clip->name, playbackRate, flags,
                        startTime, true);
}

AnimationPlayResult AnimationRuntime::playResolved(
    AnimationReference animationReference, std::string name,
    float playbackRate, std::uint32_t flags, float startTime,
    bool allowLookupSemantics,
    std::optional<std::uint64_t> exactReuseRun) {
    AnimationPlayResult result;
    const auto* clip = animationFor(animationReference);
    if (!clip || !targetModel_) {
        result.diagnostic = "Resolved animation reference is unavailable";
        return result;
    }
    // AnimRun stores only the resolved Animation pointer. Callbacks and later
    // duplicate searches therefore observe the clip's own name, including a
    // terminal "default" fallback, rather than the original lookup string.
    name = clip->name;
    // PlayAnimation stores the supplied rate unchanged. The desktop MAXSS
    // lower bound preserves unordered and negative-zero inputs.
    if (startTime < 0.0f) startTime = 0.0f;
    while (startTime > clip->length) startTime -= clip->length;

    std::optional<std::size_t> reusedIndex;
    if (exactReuseRun) reusedIndex = runIndex(*exactReuseRun);
    if (!reusedIndex && allowLookupSemantics &&
        (flags & kAnimationRunAllowDuplicate) == 0u) {
        for (std::size_t index = runs_.size(); index-- > 0u;) {
            if (equalsIgnoreCase(runs_[index].name, name)) {
                reusedIndex = index;
                break;
            }
        }
    }
    if (reusedIndex && (flags & kAnimationRunForceStop) != 0u) {
        auto& existing = runs_[*reusedIndex];
        existing.flags |= kAnimationRunNonLooping | kAnimationRunForceStop;
        result.runId = existing.id;
        return result;
    }
    if (reusedIndex && (flags & kAnimationRunStop) != 0u) {
        const auto value = runs_[*reusedIndex];
        runs_.erase(runs_.begin() +
                    static_cast<std::ptrdiff_t>(*reusedIndex));
        if (!emit(AnimationRuntimeEventKind::Completed, value,
                  value.time, "done" + value.name))
            throw std::logic_error("Engine PlayAnimation stop requires its owner after DoEvent");
        result.cleared = true;
        return result;
    }
    if (!reusedIndex && (flags & kAnimationRunStop) != 0u) {
        result.diagnostic =
            "No matching active animation run exists to stop: " + name;
        return result;
    }

    AnimationRun value;
    if (reusedIndex) {
        value = std::move(runs_[*reusedIndex]);
        runs_.erase(runs_.begin() +
                    static_cast<std::ptrdiff_t>(*reusedIndex));
        value.animation = animationReference;
        value.name = std::move(name);
    } else {
        value.id = nextRunId_++;
        value.animation = animationReference;
        value.name = std::move(name);
        value.previousTime = 0.0f;
        value.time = 0.0f;
        value.weight = 1.0f;
        value.transitionTime = 0.0f;
        value.positionScale = animationReference.positionScale;
        constexpr float startEpsilon = 1.0e-7f;
        if (playbackRate < 0.0f)
            value.time = clip->length + startEpsilon;
        else if (playbackRate > 0.0f)
            value.time = -startEpsilon;
        if (startTime != 0.0f) value.time = startTime;
        if ((flags & kAnimationRunImmediate) == 0u) {
            value.weight = 0.0f;
            value.transitionTime = clip->transitionTime;
        }
    }

    value.playbackRate = playbackRate;
    value.flags = flags;
    value.positionScale = animationReference.positionScale;
    value.transitionComplete = false;
    value.outOfOrderDuration = 0.0f;
    value.outOfOrderAnimationTimes.clear();
    value.outOfOrderControlTimes.clear();

    std::size_t insertion = 0u;
    if ((flags & kAnimationRunOverlay) != 0u) {
        while (insertion < runs_.size() && runs_[insertion].outOfOrder())
            ++insertion;
    } else {
        while (insertion < runs_.size()) {
            if (runs_[insertion].overlay()) {
                ++insertion;
                continue;
            }
            if ((flags & kAnimationRunQueued) != 0u) break;
            if (runs_[insertion].queued()) {
                ++insertion;
                continue;
            }
            break;
        }
    }
    runs_.insert(runs_.begin() + static_cast<std::ptrdiff_t>(insertion),
                 std::move(value));
    result.runId = runs_[insertion].id;

    if ((flags & kAnimationRunSynchronizeTime) != 0u &&
        insertion + 1u < runs_.size()) {
        runs_[insertion].time = runs_[insertion + 1u].time;
        return result;
    }
    if ((flags & (kAnimationRunOverlay | kAnimationRunQueued)) != 0u) {
        return result;
    }

    while (runs_.size() > insertion + 1u) {
        const auto removed = runs_[insertion + 1u];
        runs_.erase(runs_.begin() +
                    static_cast<std::ptrdiff_t>(insertion + 1u));
        if (!emit(AnimationRuntimeEventKind::Completed, removed,
                  removed.time, "done" + removed.name))
            throw std::logic_error("Engine PlayAnimation replacement requires its owner after DoEvent");
    }
    const auto& inserted = borrowedRun(*result.runId);
    emit(AnimationRuntimeEventKind::Started, inserted,
         inserted.time, "start" + inserted.name);



    return result;
}

AnimationPlayResult AnimationRuntime::playOutOfOrder(
    const std::string& name,
    const std::vector<float>& normalizedAnimationTimes,
    const std::vector<float>& controlTimes,
    float duration) {
    AnimationPlayResult result;
    const auto reference = findAnimation(hierarchy_, name);
    if (!reference) {
        result.diagnostic = "Animation was not found in the model hierarchy: " + name;
        return result;
    }
    return playOutOfOrderResolved(*reference, name,
                                  normalizedAnimationTimes,
                                  controlTimes, duration);
}

AnimationPlayResult AnimationRuntime::playOutOfOrder(
    const AnimationReference& animation,
    const std::vector<float>& normalizedAnimationTimes,
    const std::vector<float>& controlTimes,
    float duration) {
    const auto* clip = animationFor(animation);
    if (!clip) {
        AnimationPlayResult result;
        result.diagnostic = "Resolved animation reference is outside the model hierarchy";
        return result;
    }
    return playOutOfOrderResolved(animation, clip->name,
                                  normalizedAnimationTimes,
                                  controlTimes, duration);
}

AnimationPlayResult AnimationRuntime::playOutOfOrderResolved(
    const AnimationReference& animationReference, std::string name,
    const std::vector<float>& normalizedAnimationTimes,
    const std::vector<float>& controlTimes, float duration) {
    AnimationPlayResult result;
    const auto* clip = animationFor(animationReference);
    if (!clip || normalizedAnimationTimes.empty() ||
        normalizedAnimationTimes.size() != controlTimes.size()) {
        result.diagnostic =
            "Out-of-order animation requires equal non-empty timelines";
        return result;
    }

    AnimationRun value;
    value.id = nextRunId_++;
    value.animation = animationReference;
    value.name = std::move(name);
    value.playbackRate = 1.0f;
    value.flags = kAnimationRunNonLooping |
                  kAnimationRunOverlay |
                  kAnimationRunOutOfOrder;
    value.previousTime = 0.0f;
    value.time = 0.0f;
    value.weight = 0.0f;
    value.transitionTime = clip->transitionTime;
    value.positionScale = 1.0f;
    value.transitionComplete = false;



    value.outOfOrderDuration = duration;
    value.outOfOrderAnimationTimes.reserve(
        normalizedAnimationTimes.size());
    for (const float time : normalizedAnimationTimes) {
        value.outOfOrderAnimationTimes.push_back(time * clip->length);
    }
    value.outOfOrderControlTimes = controlTimes;
    result.runId = value.id;
    runs_.insert(runs_.begin(), std::move(value));
    // PlayOutOfOrderAnimation also leaves controller application to Animate.
    return result;
}

bool AnimationRuntime::stop(std::uint64_t id, bool graceful) {
    const auto index = runIndex(id);
    if (!index) return false;
    if (graceful) {
        runs_[*index].flags |=
            kAnimationRunNonLooping | kAnimationRunForceStop;
        return true;
    }
    const auto value = runs_[*index];
    runs_.erase(runs_.begin() + static_cast<std::ptrdiff_t>(*index));
    if (!emit(AnimationRuntimeEventKind::Completed, value,
              value.time, "done" + value.name))
        throw std::logic_error("Engine PlayAnimation stop requires its owner after DoEvent");
    return true;
}

bool AnimationRuntime::remove(std::uint64_t id) {
    return stop(id, false);
}

bool AnimationRuntime::setRunTime(std::uint64_t id, float time,
                                  bool rebuildFromBind) {
    auto* value = run(id);
    if (!value || !finite(time)) return false;
    const auto* clip = animationFor(value->animation);
    if (!clip) return false;
    const float length = value->outOfOrder()
        ? value->outOfOrderDuration : clip->length;
    value->previousTime = value->time;
    value->time = length > 0.0f
        ? std::clamp(time, 0.0f, length) : 0.0f;
    value->transitionComplete = false;
    rebuildPose(rebuildFromBind);
    return true;
}

bool AnimationRuntime::setRunPlaybackRate(
    std::uint64_t id, float playbackRate) noexcept {
    auto* value = run(id);
    if (!value || !finite(playbackRate)) return false;
    value->playbackRate = playbackRate;
    return true;
}

bool AnimationRuntime::setRunLooping(std::uint64_t id, bool looping) noexcept {
    auto* value = run(id);
    if (!value || value->outOfOrder()) return false;
    if (looping) value->flags &= ~kAnimationRunNonLooping;
    else value->flags |= kAnimationRunNonLooping;
    value->transitionComplete = false;
    return true;
}

void AnimationRuntime::beginLookAtAnimation(std::size_t sourceNode,float transitionTime) {
    if(lookAtRun_)if(const auto index=runIndex(*lookAtRun_))
        runs_.erase(runs_.begin()+static_cast<std::ptrdiff_t>(*index));
    Animation clip;clip.name="LookAtAnimation";clip.length=transitionTime;clip.transitionTime=transitionTime;
    for(std::optional<std::size_t> node=sourceNode;node;node=targetModel_->nodes[*node].parent) {
        Node entry;entry.id=targetModel_->nodes[*node].id;entry.name=targetModel_->nodes[*node].name;
        const auto index=clip.nodes.size();
        if(index) {entry.children.push_back(index-1);clip.nodes.back().parent=index;}
        clip.nodes.push_back(std::move(entry));
    }
    Controller orientation;orientation.type=kControllerOrientation;orientation.rowCount=2;
    orientation.timeIndex=0;orientation.dataIndex=2;orientation.rawFlags=4;
    orientation.valueKind=ControllerValueKind::Quaternion;orientation.interpolation=ControllerInterpolation::Linear;
    clip.nodes[0].controllers.push_back(orientation);
    clip.nodes[0].controllerDataWords={0u,0x3e800000u,0u,0u,0u,0x3f800000u,0u,0u,0u,0x3f800000u};
    lookAtAnimation_=std::move(clip);
    AnimationRun value;value.id=nextRunId_++;value.name="LookAtAnimation";
    value.animation.modelIndex=std::numeric_limits<std::size_t>::max();
    value.flags=kAnimationRunOverlay;value.transitionTime=transitionTime;
    lookAtRun_=value.id;runs_.insert(runs_.begin(),std::move(value));
}
bool AnimationRuntime::lookAtAnimationActive() {
    if(!lookAtRun_)return false;
    const auto* value=run(*lookAtRun_);
    if(value&&!value->transitionComplete)return true;
    if(const auto index=runIndex(*lookAtRun_))runs_.erase(runs_.begin()+static_cast<std::ptrdiff_t>(*index));
    lookAtRun_.reset();lookAtAnimation_.reset();return false;
}
void AnimationRuntime::controlLookAtAnimation(Quaternion current,Quaternion desired,bool ending) {
    auto& words=lookAtAnimation_->nodes[0].controllerDataWords;
    const float keys[]{current.x,current.y,current.z,current.w,desired.x,desired.y,desired.z,desired.w};
    for(std::size_t i=0;i<8;++i)std::memcpy(&words[i+2],&keys[i],sizeof(float));
    auto& value=*run(*lookAtRun_);value.time=0.0f;value.previousTime=0.0f;value.weight=1.0f;
    value.transitionComplete=false;
    if(ending) {
        value.flags=kAnimationRunOverlay|kAnimationRunNonLooping;
        lookAtAnimation_->length=targetModel_->game==GameVersion::Kotor1?0.0f:0.25f;
    }
}

bool AnimationRuntime::advance(float elapsedSeconds, bool evaluatePose) {
    if (!targetModel_) return false;
    if (pose_.positions.size() != targetModel_->nodes.size())
        pose_ = engineAnimationBindPose(*targetModel_);
    if(evaluatePose)pose_.controlledRootNode.reset();
    bool applied=false;

    // Gob retains this numeric cursor, not a snapshot of run identities or a
    // count refreshed after DoEvent (K2 Desktop 0xfedc0, 0xfee92).
    std::ptrdiff_t index = static_cast<std::ptrdiff_t>(runs_.size()) - 1;
    if (!runs_.empty() && runs_.back().queued()) {
        runs_.back().flags &= ~kAnimationRunQueued;
        if (!emit(AnimationRuntimeEventKind::Started, runs_.back(),
                  runs_.back().time, "start" + runs_.back().name))
            return applied;
    }

    while (index >= 0) {
        if (static_cast<std::size_t>(index) >= runs_.size())
            throw std::logic_error("Engine animation callback removed a pending run-list slot");
        auto* value = &runs_[static_cast<std::size_t>(index)];
        const auto borrowedId = value->id;
        if (value->queued()) {
            --index;
            continue;
        }
        // Gob::Animate advances the clock before removing a run whose fade
        // finished on the preceding frame (K1 Desktop 0x2e08c8; K2 Desktop 0xfeea9).
        value->previousTime = value->time;
        value->time += value->playbackRate * elapsedSeconds;
        if ((value->flags & kAnimationRunNonLooping) != 0u &&
            value->transitionComplete) {
            const auto completed = *value;
            runs_.erase(runs_.begin() + index);
            if (!emit(AnimationRuntimeEventKind::Completed, completed,
                      completed.time, "done" + completed.name, applied))
                return applied;
            --index;
            continue;
        }

        const auto* clip = animationFor(value->animation);
        if (!clip) {
            runs_.erase(runs_.begin() + index);
            --index;
            continue;
        }
        bool boundary = false;

        if ((value->flags & kAnimationRunNonLooping) != 0u) {
            if (value->time < 0.0f) {
                value->time = 0.0f;
                boundary = true;
            } else if (value->outOfOrder()) {
                if (value->time > value->outOfOrderDuration) {
                    value->transitionTime = 0.5f;
                    value->time = value->outOfOrderDuration;
                    boundary = true;
                }
            } else if (value->time > clip->length) {
                value->time = clip->length;
                boundary = true;
            }

            if (boundary ||
                (value->flags & kAnimationRunForceStop) != 0u) {
                if (index > 0 &&
                    runs_[static_cast<std::size_t>(index - 1)].queued()) {
                    auto& queued = runs_[static_cast<std::size_t>(index - 1)];
                    queued.flags &= ~kAnimationRunQueued;
                    if (!emit(AnimationRuntimeEventKind::Started, queued,
                              queued.time, "start" + queued.name, applied))
                        throw std::logic_error("Engine queued-start callback requires its owner until the run tail");
                    value = &borrowedRun(borrowedId);
                }
                // Engine chooses the constant only for transition<=0;
                // an unordered transition takes the division path.
                const float decrease = value->transitionTime <= 0.0f
                    ? 2.0f
                    : (2.0f * elapsedSeconds) / value->transitionTime;
                value->weight -= decrease;
                if (value->weight <= 0.0f) {
                    value->weight = 0.0f;
                    value->transitionComplete = true;
                    if (value->transitionTime == 0.0f)
                        value->weight = 1.0f;
                }
            }
        } else {
            // Both Desktop callers perform one wrap then clamp, including a
            // zero-length looping clip (K1 0x2e0ae0; K2 0xfef57).
            if (value->time < 0.0f && value->playbackRate <= 0.0f) {
                if ((value->flags & kAnimationRunReportLoop) != 0u) {
                    if (!emit(AnimationRuntimeEventKind::Loop, *value,
                              value->time, "loop" + value->name, applied))
                        throw std::logic_error("Engine loop callback requires its owner until the run tail");
                    value = &borrowedRun(borrowedId);
                    clip = animationFor(value->animation);
                }
                value->time += clip->length;
                if (value->time < 0.0f) value->time = 0.0f;
            }
            if (value->time > clip->length && value->playbackRate >= 0.0f) {
                if ((value->flags & kAnimationRunReportLoop) != 0u) {
                    if (!emit(AnimationRuntimeEventKind::Loop, *value,
                              value->time, "loop" + value->name, applied))
                        throw std::logic_error("Engine loop callback requires its owner until the run tail");
                    value = &borrowedRun(borrowedId);
                    clip = animationFor(value->animation);
                }
                value->time -= clip->length;
                if (value->time > clip->length) value->time = clip->length;
            }
        }

        if (!value->transitionComplete && value->weight < 1.0f) {
            const float increase = value->transitionTime <= 0.0f
                ? 1.0f : elapsedSeconds / value->transitionTime;
            value->weight += increase;
            const bool capWeight = value->weight > 1.0f;
            if (capWeight) {
                value->weight = 1.0f;
                if (!value->overlay()) {
                    while (runs_.size() >
                           static_cast<std::size_t>(index + 1)) {
                        runs_.erase(runs_.begin() + index + 1);
                    }
                    value = &borrowedRun(borrowedId);
                }
            }
        }

        clip = animationFor(value->animation);
        const auto sample = value->outOfOrder()
            ? outOfOrderRunSample(*value, boundary)
            : normalRunSample(*value);
        if(evaluatePose) {
            applyAnimationControllerSample(*targetModel_, *clip,
                                           value->positionScale,
                                           sample, pose_);
            pose_.time = value->time;applied=true;
        }
        if (!emitAnimationEvents(*value, applied)) return applied;
        --index;
    }
    if(evaluatePose)calculatePoseTransformsAndBounds(*targetModel_, pose_, {}, true);
    return applied;
}

void AnimationRuntime::composeRuns() {
    if (!targetModel_) return;
    if (pose_.positions.size() != targetModel_->nodes.size())
        pose_ = engineAnimationBindPose(*targetModel_);
    pose_.controlledRootNode.reset();
    for (std::size_t position = runs_.size(); position-- > 0u;) {
        const auto& value = runs_[position];
        if (value.queued()) continue;
        const auto* clip = animationFor(value.animation);
        if (!clip) continue;
        // Recomposition is equivalent to a zero-delta Animate call. At an
        // exact OO duration endpoint the engine takes the ordinary final
        // control segment (fraction 1); only a frame that overshot the duration
        // sets the temporary boundary flag used by Gob::Animate.
        const auto sample = value.outOfOrder()
            ? outOfOrderRunSample(value, false)
            : normalRunSample(value);
        applyAnimationControllerSample(*targetModel_, *clip,
                                       value.positionScale,
                                       sample, pose_);
        pose_.time = value.time;
    }
    calculatePoseTransformsAndBounds(*targetModel_, pose_, {}, true);
}

void AnimationRuntime::rebuildPose(bool resetToBindPose) {
    if (!targetModel_) {
        pose_ = {};
        return;
    }
    if (resetToBindPose)
        pose_ = engineAnimationBindPose(*targetModel_);
    composeRuns();
}

std::vector<AnimationRuntimeEvent> AnimationRuntime::takeEvents() {
    auto result = std::move(events_);
    events_.clear();
    return result;
}

std::optional<std::uint64_t> AnimationRuntime::primaryRunId() const noexcept {
    for (const auto& value : runs_) {
        if (!value.queued()) return value.id;
    }
    return std::nullopt;
}

float AnimationRuntime::primaryTime() const noexcept {
    const auto id = primaryRunId();
    const auto* value = id ? run(*id) : nullptr;
    return value ? value->time : 0.0f;
}

std::string gameVersionName(GameVersion game) {
    return game == GameVersion::Kotor1 ? "Knights of the Old Republic" : "The Sith Lords";
}

std::string nodeFlagsText(std::uint16_t flags) {
    struct FlagName { std::uint16_t value; const char* name; };
    static constexpr FlagName names[] = {
        {kNodeBase, "base"}, {kNodeLight, "light"}, {kNodeEmitter, "emitter"},
        {kNodeCamera, "camera"}, {kNodeReference, "reference"}, {kNodeMesh, "mesh"},
        {kNodeSkin, "skin"}, {kNodeAnimation, "animmesh"}, {kNodeDangly, "dangly"},
        {kNodeAabb, "AABB"}, {kNodeTrigger, "trigger"}, {kNodeLightsaber, "lightsaber"},
    };
    std::ostringstream text;
    bool first = true;
    for (const auto& entry : names) {
        if ((flags & entry.value) == 0u) continue;
        if (!first) text << ", ";
        text << entry.name;
        first = false;
    }
    if (first) text << "none";
    return text.str();
}

std::string controllerTypeName(std::uint32_t type) {
    switch (type) {
    case kControllerPosition: return "position";
    case kControllerOrientation: return "orientation";
    case kControllerScale: return "scale";
    default: return "controller_" + std::to_string(type);
    }
}

std::string modelSummary(const Model& model) {
    std::size_t vertices = 0u;
    std::size_t triangles = 0u;
    std::size_t textured = 0u;
    std::size_t skinned = 0u;
    std::size_t deformableSkins = 0u;
    std::size_t dangly = 0u;
    std::size_t animmeshes = 0u;
    std::size_t lights = 0u;
    std::size_t emitters = 0u;
    std::size_t cameras = 0u;
    std::size_t triggers = 0u;
    std::size_t references = 0u;
    std::size_t lightsabers = 0u;
    std::size_t aabbs = 0u;
    for (const auto& mesh : model.meshes) {
        vertices += mesh.vertices.size();
        triangles += mesh.animated ? mesh.faces.size() : mesh.indices.size() / 3u;
        if (mesh.animated) ++animmeshes;
        if (!mesh.texture0.empty()) ++textured;
        if (mesh.skinned) {
            ++skinned;
            if (mesh.hasSkinChannels &&
                mesh.hasSkinBoneMap && mesh.hasInverseBindPose) {
                ++deformableSkins;
            }
        }
        if (mesh.dangly) ++dangly;
    }
    for (const auto& node : model.nodes) {
        if (node.light) ++lights;
        if (node.emitter) ++emitters;
        if (node.camera) ++cameras;
        if (node.trigger) ++triggers;
        if (node.reference) ++references;
        if (node.lightsaber) ++lightsabers;
        if (node.aabb) ++aabbs;
    }
    std::ostringstream text;
    text << (model.name.empty() ? "Unnamed model" : model.name) << " — "
         << gameVersionName(model.game) << ", " << model.nodes.size() << " node(s), "
         << model.meshes.size() << " mesh(es), " << vertices << " vertices, "
         << triangles << " triangles, " << textured << " textured mesh(es), "
         << model.animations.size() << " animation(s)";
    if (animmeshes != 0u) text << ", " << animmeshes << " animmesh(es)";
    if (skinned != 0u) {
        text << ", " << skinned << " skin mesh(es)";
        if (deformableSkins != 0u)
            text << " (" << deformableSkins << " K2 CPU-deformable)";
    }
    if (dangly != 0u) text << ", " << dangly << " dangly mesh(es)";
    if (lights != 0u) text << ", " << lights << " light(s)";
    if (emitters != 0u) text << ", " << emitters << " emitter(s)";
    if (cameras != 0u) text << ", " << cameras << " camera(s)";
    if (triggers != 0u) text << ", " << triggers << " trigger marker(s)";
    if (references != 0u) text << ", " << references << " reference(s)";
    if (lightsabers != 0u) text << ", " << lightsabers << " lightsaber node(s)";
    if (aabbs != 0u) text << ", " << aabbs << " AABB node(s)";
    return text.str();
}

} // namespace neomdl::model
