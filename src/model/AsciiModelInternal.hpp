#pragma once

#include <model/AsciiModel.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <utility>

namespace neomdl::model::ascii {

struct Line {
    std::size_t number{};
    std::size_t bufferOffset{};
    std::vector<std::string> tokens;
    std::string text;
};

// Engine AurResGetNextLine whitespace; command-level comments are ignored by
// the owning parser. Blank lines are retained for counted array semantics.
// A Reader belongs to one load; nested model/node/array readers share it.
class Reader {
public:
    Reader(const std::vector<std::uint8_t>& bytes, GameVersion game, const std::string& sourceLabel,
           const neoshared::bif::JobControl& job,
           const AsciiResourceResolver& resources = {});
    bool next(Line& line);
    void trimCommandLine(Line& line);
    Line fieldLine(std::size_t offset) const;
    void finishPackedArray(const Line& header);
    std::vector<std::uint8_t> takeBytes(std::size_t count, const Line& line);
    [[noreturn]] void fail(const Line& line, const std::string& message) const;
    GameVersion game() const noexcept { return game_; }
    const neoshared::bif::JobControl& job() const noexcept { return job_; }
    const AsciiResourceResolver& resources() const noexcept { return resources_; }
private:
    const std::vector<std::uint8_t>& bytes_;
    GameVersion game_;
    std::string sourceLabel_;
    const neoshared::bif::JobControl& job_;
    AsciiResourceResolver resources_;
    std::vector<std::uint8_t> buffer_;
    std::size_t cursor_{}, lineNumber_{};
};

std::string lower(std::string value);
bool equal(std::string_view left, std::string_view right) noexcept;
float parseFloat(const std::string& token);
std::int32_t parseInt(const std::string& token);
std::uint32_t parseUInt(const std::string& token);
bool scanInteger(const char* text, std::int32_t& value);
void scanBoolean(const char* text, bool& value);
void scanString(const char* text, std::string& value);
void requireTokens(const Line& line, std::size_t count, const Reader& reader);
Quaternion engineAxisAngle(Vec3 axis, float angle);
Quaternion engineModelQuaternionProduct(Quaternion left,Quaternion right) noexcept;
Vec3 engineModelRotateVector(Quaternion orientation,Vec3 value) noexcept;

// Parse<T> in both engine loaders: a count consumes physical lines; an
// additional header character selects packed bytes; no count overwrites a
// growing list until the case-sensitive endlist prefix. Existing tail
// elements survive the uncounted form, and a growing list copies its last
// element before Parse writes the next row, matching List<T>::Append.
struct RejectIncompleteArray {
    void operator()(const Line& header, const Reader& reader) const {
        reader.fail(header,"Unfinished counted array");
    }
};

template<class T, class Text, class Bytes, class End = RejectIncompleteArray>
void readArray(const Line& header, Reader& reader, std::vector<T>& values,
               std::size_t stride, Text text, Bytes bytes, End end = {}) {
    std::string arguments;
    for (std::size_t i=1;i<header.tokens.size();++i) {
        if(i!=1u) arguments+=' ';
        arguments+=header.tokens[i];
    }
    int count{};
    char marker{};
    const auto fields=std::sscanf(arguments.c_str(),"%d %c",&count,&marker);
    if(fields>0) {
        values.resize(static_cast<std::size_t>(count));
        if(stride && fields==2) {
            const auto block=reader.takeBytes(static_cast<std::size_t>(count)*stride,header);
            for(std::size_t i=0;i<values.size();++i) values[i]=bytes(block.data()+i*stride);
            reader.finishPackedArray(header);
            return;
        }
        for(auto& value:values) {
            Line row;
            if(!reader.next(row)) {
                end(header,reader);
                return;
            }
            text(row,reader,value);
        }
        return;
    }
    std::size_t index{};
    Line row;
    while(reader.next(row)) {
        if(row.tokens.front().compare(0u,7u,"endlist")==0) return;
        if(index==values.size()) values.push_back(index?values.back():T{});
        text(row,reader,values[index]);
        ++index;
    }
}

// Scalar List allocations do not initialize new slots. Keep the loader's
// existing unsupported-input error when a failed first scan would otherwise
// expose allocator contents; retained, copied and packed values are defined.
template<class T, class Text, class Bytes>
void readScalarArray(const Line& header, Reader& reader, std::vector<T>& values,
                     std::size_t stride, Text text, Bytes bytes,
                     bool retainInitializedAtEof = false) {
    struct Scalar { T value{}; bool initialized{}; };
    std::vector<Scalar> parsed;
    for(const auto value:values) parsed.push_back({value,true});
    readArray(header,reader,parsed,stride,
        [&](const Line& row,const Reader& input,Scalar& scalar) {
            if(text(row,input,scalar.value)) scalar.initialized=true;
            else if(!scalar.initialized)
                input.fail(row,"ASCII scalar has no initialized engine value");
        },[&](const std::uint8_t* data) { return Scalar{bytes(data),true}; },
        [&](const Line& line,const Reader& input) {
            // Parse<float> returns at physical EOF after SetSize copies the
            // old rows. Preserve that defined tail without supplying values
            // for newly allocated engine scalar slots.
            if(!retainInitializedAtEof) RejectIncompleteArray{}(line,input);
            for(const auto& scalar:parsed)
                if(!scalar.initialized) RejectIncompleteArray{}(line,input);
        });
    values.resize(parsed.size());
    for(std::size_t index=0;index<values.size();++index) values[index]=parsed[index].value;
}

struct RawFace {
    std::array<std::uint32_t,3> vertices{};
    std::uint32_t smoothing{};
    std::array<std::array<std::uint32_t,3>,4> texcoords{};
    std::uint32_t material{};
};

struct RawNode {
    bool advancedAscii{};
    bool retainedLightsaber{};
    bool retainedTotalArea{};
    Node node;
    std::optional<Mesh> mesh;
    std::string parentName;
    std::vector<Vec3> vertices;
    std::vector<std::array<double,3>> normalPositions64;
    std::uint32_t normalPositionPrecision{32u};
    std::vector<Vec3> normals;
    std::vector<std::array<std::uint32_t,3>> normalIndices;
    std::vector<std::array<std::size_t,3>> processedSkinNormals;
    std::array<std::vector<Vec3>,4> texcoords;
    std::vector<std::array<double,2>> tangentTexcoords64;
    std::vector<std::size_t> tangentFaceSources;
    std::uint32_t tangentUvPrecision{32u};
    std::array<std::vector<Vec3>,3> textureIndices;
    std::vector<Vec3> colors;
    std::vector<std::array<std::uint8_t,4>> packedColors;
    std::vector<std::array<std::uint32_t,3>> colorIndices;
    std::vector<TangentBasis> tangentBases;
    std::vector<std::array<std::uint32_t,3>> tangentIndices;
    std::vector<Vec3> cpuPositions;
    std::vector<std::array<std::uint32_t,3>> cpuIndices;
    std::vector<std::array<float,4>> facePlanes;
    std::optional<Vec3> sphereCenter;
    std::optional<float> sphereRadius;
    std::vector<RawFace> faces;
    std::vector<std::int32_t> vertexIndicesCount;
    std::vector<std::vector<std::uint32_t>> vertexIndices;
    std::vector<std::uint32_t> leftoverFaces;
    std::vector<std::vector<std::pair<std::string,float>>> weights;
    std::vector<std::array<std::int32_t,4>> skinWeightIndices;
    std::optional<AsciiSkinSource> skinData;
    std::vector<EngineFloatSkinChannels> skinChannels;
    std::vector<float> constraints;
    std::vector<Vec3> animatedVertices;
    std::vector<Vec3> animatedTexcoords;
    std::vector<std::array<float,4>> animTangents;
    std::vector<std::int32_t> animMirrorList;
    std::vector<std::size_t> generatedVertexSources;
    std::vector<Vec3> generatedVertexColors;
    float samplePeriod{};
    bool animation{};
    std::uint16_t modelClassification{};
    std::optional<bool> compressQuaternions;
    std::size_t controllerDataBase{};
};

// Each field handler returns false only when its engine parser does not own
// the keyword. Node dispatch chains base, derived and controller handlers.
void initializeNode(RawNode& raw, GameVersion game);
bool parseNodeField(const Line& line, Reader& reader, RawNode& raw, GameVersion game);
bool parseMeshField(const Line& line, Reader& reader, RawNode& raw, GameVersion game);
bool parseSkinField(const Line& line, Reader& reader, RawNode& raw, GameVersion game);
bool parseControllerExtension(const Line& line, Reader& reader, RawNode& raw);
void parseBaseTransformFields(const Line& line, Reader& reader, RawNode& raw);
bool parseControllerField(const Line& line, Reader& reader, RawNode& raw, GameVersion game,
                          std::string_view group = {}, bool updateBaseTransform = true);
void applyAsciiLightmap(RawNode& raw, Reader& reader);
void postProcessSkinNode(RawNode& raw,const neoshared::bif::JobControl& job);
void buildMesh(Model& model, RawNode& raw, std::size_t nodeIndex,
               const neoshared::bif::JobControl& job);
Vec3 tangentBasisNormal(Vec3 bitangent,Vec3 tangent,Vec3 faceArea);
struct NormalFaceCross {
    float x,y;
    double z;
    Vec3 accumulate(Vec3 sum) const {
        return {sum.x+x,sum.y+y,static_cast<float>(z+sum.z)};
    }
};
inline NormalFaceCross normalFaceCross(Vec3 a,Vec3 b,Vec3 c) {
    // The first edge's XY are stored; its Z and the second edge stay precise.
    // Cross XY are stored before accumulation; Z stays precise through the sum.
    const float px=b.x-a.x,py=b.y-a.y;
    const double pz=double(b.z)-a.z;
    const double x=double(c.x)-b.x,y=double(c.y)-b.y,z=double(c.z)-b.z;
    return {static_cast<float>(py*z-pz*y),
        static_cast<float>(pz*x-px*z),px*y-py*x};
}
inline Vec3 normalizeVertexNormal(Vec3 value) {
    // ProcessVerts preserves exact zero. Keep normalization intermediates
    // at expression precision until the normalized components are stored.
    const double length=std::sqrt((double(value.x)*value.x+double(value.y)*value.y)+double(value.z)*value.z);
    if(length!=0.0) {
        if(length<1e-9) return {1.0f,0.0f,0.0f};
        const double scale=1.0/length;
        return {static_cast<float>(value.x*scale),static_cast<float>(value.y*scale),
            static_cast<float>(value.z*scale)};
    }
    return value;
}
std::array<Vec3,2> normalPositionFaceProducts(const std::array<Vec3,3>& positions,
    const std::array<std::array<double,3>,3>& precise);
Vec3 normalPositionFaceCross(const std::array<Vec3,3>& positions,
    const std::array<std::array<double,3>,3>& precise);
Vec3 tangentUvDerivative(Vec3 p,Vec3 q,const std::array<Vec2,3>& uv,
    const std::array<std::array<double,2>,3>& precise,std::size_t component);
struct TangentFace {
    std::array<Vec3,3> positions;
    std::array<Vec2,3> texcoords;
};
void orientSingularTangentComponents(const std::vector<TangentFace>& faces,
    std::vector<TangentBasis>& bases,const neoshared::bif::JobControl& job);
// Reuse the mesh producers for exporter seam evaluation. Returns
// authored face-corner rows before duplicate-index faces are removed.
std::vector<std::array<std::int16_t,3>> generateAsciiMeshGeometry(
    Model& model, RawNode& raw, Mesh& mesh, const neoshared::bif::JobControl& job);
void finalizeNodeTree(Model& model, std::vector<RawNode>& raw, Animation* animation);
void buildSkin(Model& model, RawNode& raw, Mesh& mesh,
               const std::vector<std::size_t>& sources,
               const std::vector<std::size_t>& partPath,
               const neoshared::bif::JobControl& job);

} // namespace neomdl::model::ascii

namespace neomdl::model {

struct AsciiSkinSeamCache {
    struct Reference { std::size_t mesh,vertex; };
    std::vector<std::vector<Vec3>> positions;
    std::vector<std::vector<std::vector<Reference>>> matches;
};
void processAsciiSkinSeams(const Model& model,const neoshared::bif::JobControl& job,
                          AsciiSkinSeamCache& cache);

} // namespace neomdl::model
