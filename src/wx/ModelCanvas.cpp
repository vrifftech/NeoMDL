#include "model/Shadow.hpp"
#include "wx/ModelCanvas.hpp"
#include <gff/GFFFile.hpp>
#include "render/LightSubmission.hpp"
#include <model/SceneOrdering.hpp>
#include <model/AsciiModel.hpp>
#include <model/EngineCpuSkin.hpp>
#include <model/LightningControl.hpp>
#include "core/EmitterBucket.hpp"
#include "core/CreatureLipSync.hpp"
#include <neoshared/texture/FileUtil.hpp>
#include <neoshared/PathUtf8.hpp>
#include <neoshared/glu/Perspective.hpp>

#include <wx/dcclient.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>

#if defined(__WXMAC__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <cstring>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <limits>
#include <iterator>
#include <utility>
#include <vector>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace neomdl::ui {

wxDEFINE_EVENT(EVT_MODEL_NODE_PICKED, wxCommandEvent);
wxDEFINE_EVENT(EVT_AREA_OWNER_PICKED, wxCommandEvent);

namespace {
struct DetectionCaptureExhausted : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr material::EngineMaterialGame engineMaterialGame(model::GameVersion game) noexcept {
    return game==model::GameVersion::Kotor1
        ? material::EngineMaterialGame::Kotor1 : material::EngineMaterialGame::Kotor2;
}

bool validCameraPreviewSettings(const CameraPreviewSettings& settings) noexcept {
    return std::isfinite(settings.viewAngle) &&
           std::isfinite(settings.nearClip) &&
           std::isfinite(settings.farClip) &&
           settings.viewAngle > 0.0f && settings.viewAngle < 180.0f &&
           settings.nearClip > 0.0f && settings.farClip > settings.nearClip;
}

bool validK2SkinPreviewSettings(const K2SkinPreviewSettings& settings) noexcept {
    return std::isfinite(settings.objectMotion.x) &&
           std::isfinite(settings.objectMotion.y) &&
           std::isfinite(settings.objectMotion.z) &&
           std::isfinite(settings.maximumBlurLength) &&
           settings.maximumBlurLength > 0.0f &&
           std::isfinite(settings.sideScale) && settings.sideScale >= 0.0f;
}

struct SkinEffectSpace {
    neomdl::model::Vec3 cameraPosition{};
    neomdl::model::Vec3 sideDirection{1.0f,0.0f,0.0f};
};

SkinEffectSpace skinEffectSpace(
    const neomdl::model::Mat4& modelView) {
    const auto inverseTranspose=neomdl::model::makeNormalMatrix(modelView);
    const auto inverseVector=[&](neomdl::model::Vec3 value) {
        // makeNormalMatrix returns A^-T. Its stored columns are therefore the
        // rows needed for A^-1 * value.
        return neomdl::model::Vec3{
            inverseTranspose[0]*value.x + inverseTranspose[1]*value.y +
                inverseTranspose[2]*value.z,
            inverseTranspose[3]*value.x + inverseTranspose[4]*value.y +
                inverseTranspose[5]*value.z,
            inverseTranspose[6]*value.x + inverseTranspose[7]*value.y +
                inverseTranspose[8]*value.z};
    };
    const neomdl::model::Vec3 translation{
        modelView.values[12],modelView.values[13],modelView.values[14]};
    SkinEffectSpace result;
    result.cameraPosition=inverseVector({-translation.x,-translation.y,
                                         -translation.z});
    result.sideDirection=neomdl::model::normalize(
        inverseVector({1.0f,0.0f,0.0f}));
    if (result.sideDirection.x==0.0f && result.sideDirection.y==0.0f &&
        result.sideDirection.z==0.0f) {
        result.sideDirection={1.0f,0.0f,0.0f};
    }
    return result;
}

// Resolve against the CURRENT canvas context. WGL extension entry points are
// context-specific and opengl32.dll exports only the legacy subset.
neomdl::render::gl::Proc resolveGlProcedure(const char* name) {
    using Proc = neomdl::render::gl::Proc;
#if defined(_WIN32)
    const auto procedure = wglGetProcAddress(name);
    const auto address = reinterpret_cast<std::uintptr_t>(procedure);
    if (procedure && address != 1u && address != 2u && address != 3u &&
        address != std::numeric_limits<std::uintptr_t>::max())
        return reinterpret_cast<Proc>(procedure);
    const auto module = GetModuleHandleW(L"opengl32.dll");
    return module ? reinterpret_cast<Proc>(GetProcAddress(module, name)) : nullptr;
#else
    void* address = dlsym(RTLD_DEFAULT, name);
#if defined(__APPLE__)
    static void* module = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
#else
    static void* module = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
#endif
    if (!address && module) address = dlsym(module, name);
    Proc result{};
    static_assert(sizeof(result) == sizeof(address), "Unsupported procedure pointer ABI");
    std::memcpy(&result, &address, sizeof(result));
#if !defined(__APPLE__)
    if (!result && module) {
        using GlxResolver = Proc (*)(const unsigned char*);
        GlxResolver resolver{};
        auto* resolverAddress = dlsym(module, "glXGetProcAddressARB");
        std::memcpy(&resolver, &resolverAddress, sizeof(resolver));
        if (resolver) result = resolver(reinterpret_cast<const unsigned char*>(name));
    }
#endif
    return result;
#endif
}

int* canvasAttributes() {
    static int withStencil[] = {WX_GL_RGBA, WX_GL_DOUBLEBUFFER,
                                WX_GL_DEPTH_SIZE, 24,
                                WX_GL_STENCIL_SIZE, 8, 0};
    static int withoutStencil[] = {WX_GL_RGBA, WX_GL_DOUBLEBUFFER,
                                   WX_GL_DEPTH_SIZE, 24, 0};
    // GLX configurations with stencil buffers are normal on Linux, but older
    // remote/indirect displays may not expose one. Keep the application usable
    // there; the packed itself also skips the highlight pass when no stencil
    // bits are available.
    return wxGLCanvas::IsDisplaySupported(withStencil)
        ? withStencil : withoutStencil;
}

float maximumExtent(const neomdl::model::Bounds& bounds) {
    if (!bounds.valid) return 1.0f;
    return std::max({bounds.maximum.x - bounds.minimum.x,
                     bounds.maximum.y - bounds.minimum.y,
                     bounds.maximum.z - bounds.minimum.z, 0.01f});
}

void extendBounds(neomdl::model::Bounds& bounds,
                  const neomdl::model::Vec3& point) {
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
}

void extendTransformedBounds(neomdl::model::Bounds& destination,
                             const neomdl::model::Bounds& source,
                             const neomdl::model::Mat4& transform) {
    if (!source.valid) return;
    for (unsigned mask = 0u; mask < 8u; ++mask) {
        const neomdl::model::Vec3 corner{
            (mask & 1u) != 0u ? source.maximum.x : source.minimum.x,
            (mask & 2u) != 0u ? source.maximum.y : source.minimum.y,
            (mask & 4u) != 0u ? source.maximum.z : source.minimum.z,
        };
        extendBounds(destination, neomdl::model::transformPoint(transform, corner));
    }
}

float clampColor(float value) {
    return std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
}

float ownerSphereCameraDistance(const neomdl::HitSphere& sphere,
                               const neomdl::model::EngineFrame& frame,
                               const neomdl::model::Vec3& cameraPosition) {
    return neomdl::engineOwnerCameraDistance(sphere,frame.position,
        frame.orientation,cameraPosition);
}

neomdl::render::MeshSubmission ownerOrderingSubmission(std::uint64_t owner,
    const neomdl::model::Model& asset,bool transparency,float distance,float alpha,
    const neomdl::model::Mat4& modelView) {
    neomdl::render::MeshSubmission submission;
    submission.ownerOnly=true;
    submission.drawPrimary=false;
    submission.sceneOwner=owner;
    submission.ownerModelClassification=asset.classification;
    submission.ownerHasTransparency=transparency;
    submission.ownerCameraDistance=distance;
    submission.ownerAlpha=alpha;
    submission.lightOwner=owner-1u;
    submission.parameters.modelView=modelView;
    submission.parameters.material.engineGame=neomdl::material::EngineMaterialGame::Kotor1;
    submission.parameters.material.k2PartTriMeshState=true;
    return submission;
}


void drawCircle(unsigned segments, float radius, int plane) {
    constexpr float tau = 6.28318530717958647692f;
    glBegin(GL_LINE_LOOP);
    for (unsigned index = 0u; index < segments; ++index) {
        const auto angle = tau * static_cast<float>(index) / static_cast<float>(segments);
        const auto first = std::cos(angle) * radius;
        const auto second = std::sin(angle) * radius;
        if (plane == 0) glVertex3f(first, second, 0.0f);
        else if (plane == 1) glVertex3f(first, 0.0f, second);
        else glVertex3f(0.0f, first, second);
    }
    glEnd();
}

using AabbDebugColor = std::array<GLubyte, 3>;

// PartAABB::Draw builds these two 32-entry palettes once from the shared Desktop
// float table. Conversion is truncating float*255, including the complementary
// back-face palette rather than 255 minus the already-truncated byte.
constexpr std::array<AabbDebugColor, 32> kAabbFrontColors{{
    {{204u, 204u, 204u}}, {{147u, 124u, 80u}}, {{204u, 204u, 0u}},
    {{61u, 149u, 31u}}, {{102u, 102u, 102u}}, {{151u, 89u, 47u}},
    {{0u, 189u, 177u}}, {{204u, 0u, 0u}}, {{253u, 204u, 189u}},
    {{130u, 0u, 128u}}, {{158u, 184u, 184u}}, {{128u, 253u, 204u}},
    {{137u, 156u, 0u}}, {{73u, 28u, 0u}}, {{70u, 91u, 35u}},
    {{204u, 98u, 0u}}, {{0u, 0u, 0u}}, {{0u, 24u, 204u}},
    {{0u, 0u, 0u}}, {{0u, 0u, 0u}}, {{0u, 0u, 0u}},
    {{0u, 0u, 0u}}, {{0u, 0u, 0u}}, {{0u, 0u, 0u}},
    {{0u, 0u, 0u}}, {{0u, 0u, 0u}}, {{0u, 0u, 0u}},
    {{0u, 0u, 0u}}, {{0u, 0u, 0u}}, {{0u, 0u, 0u}},
    {{0u, 0u, 0u}}, {{0u, 0u, 0u}},
}};

constexpr std::array<AabbDebugColor, 32> kAabbBackColors{{
    {{50u, 50u, 50u}}, {{107u, 130u, 174u}}, {{50u, 50u, 255u}},
    {{193u, 105u, 223u}}, {{153u, 153u, 153u}}, {{103u, 165u, 207u}},
    {{255u, 65u, 77u}}, {{50u, 255u, 255u}}, {{1u, 50u, 65u}},
    {{124u, 255u, 126u}}, {{96u, 70u, 70u}}, {{126u, 1u, 50u}},
    {{117u, 98u, 255u}}, {{181u, 226u, 255u}}, {{184u, 163u, 219u}},
    {{50u, 156u, 255u}}, {{255u, 255u, 255u}}, {{255u, 230u, 50u}},
    {{255u, 255u, 255u}}, {{255u, 255u, 255u}}, {{255u, 255u, 255u}},
    {{255u, 255u, 255u}}, {{255u, 255u, 255u}}, {{255u, 255u, 255u}},
    {{255u, 255u, 255u}}, {{255u, 255u, 255u}}, {{255u, 255u, 255u}},
    {{255u, 255u, 255u}}, {{255u, 255u, 255u}}, {{255u, 255u, 255u}},
    {{255u, 255u, 255u}}, {{255u, 255u, 255u}},
}};

void drawAabbSurfacePass(const neomdl::model::Mesh& mesh,
                         const std::array<AabbDebugColor, 32>& colors) {
    // PartAABB::Draw reads the authored CPU pool: K1 +0x198 (0x34b60a),
    // K2 +0x1a0. Its collision geometry does not use the MDX drawing stream.
    glBegin(GL_TRIANGLES);
    for (const auto& face : mesh.faces) {
        const auto& positions=mesh.cpuPositions.value();
        if (face.indices[0] < 0 || face.indices[1] < 0 ||
            face.indices[2] < 0 ||
            static_cast<std::size_t>(face.indices[0]) >= positions.size() ||
            static_cast<std::size_t>(face.indices[1]) >= positions.size() ||
            static_cast<std::size_t>(face.indices[2]) >= positions.size()) {
            continue;
        }
        // GLRender::DrawTriangles indexes its RenderVertexC table directly.
        // The PartAABB palette has 32 entries; malformed larger
        // material identifiers would read beyond it in the original routine.
        if (face.surfaceMaterial >= colors.size()) continue;
        const auto& color = colors[face.surfaceMaterial];
        glColor4ub(color[0], color[1], color[2], 255u);
        const auto& first = positions[static_cast<std::size_t>(face.indices[0])];
        const auto& second = positions[static_cast<std::size_t>(face.indices[1])];
        const auto& third = positions[static_cast<std::size_t>(face.indices[2])];
        const neomdl::model::Vec3 firstEdge{
            second.x - first.x, second.y - first.y, second.z - first.z};
        const neomdl::model::Vec3 secondEdge{
            third.x - second.x, third.y - second.y, third.z - second.z};
        neomdl::model::Vec3 normal{
            firstEdge.y * secondEdge.z - firstEdge.z * secondEdge.y,
            firstEdge.z * secondEdge.x - firstEdge.x * secondEdge.z,
            firstEdge.x * secondEdge.y - firstEdge.y * secondEdge.x};
        // K1 NormalOf 0x3a5ab6 / K2 0x31330b: second edge is
        // third-second; all arithmetic is float, with an exact-zero branch.
        const float lengthSquared=(normal.x*normal.x+normal.y*normal.y)+normal.z*normal.z;
        const float length=std::sqrt(lengthSquared);
        if(length==0.0f)normal={1.0f,0.0f,0.0f};
        else {
            const float reciprocal=1.0f/length;
            normal.x*=reciprocal;normal.y*=reciprocal;normal.z*=reciprocal;
        }
        glNormal3f(normal.x, normal.y, normal.z);
        for (const auto vertexIndex : face.indices) {
            const auto& position = positions[static_cast<std::size_t>(vertexIndex)];
            glVertex3f(position.x, position.y, position.z);
        }
    }
    glEnd();
}

void drawAabbBounds(const neomdl::model::AabbTreeNode& node) {
    const auto& low = node.minimum;
    const auto& high = node.maximum;
    const std::array<neomdl::model::Vec3, 8> corners{{
        {low.x, low.y, low.z}, {high.x, low.y, low.z},
        {high.x, high.y, low.z}, {low.x, high.y, low.z},
        {low.x, low.y, high.z}, {high.x, low.y, high.z},
        {high.x, high.y, high.z}, {low.x, high.y, high.z},
    }};
    static constexpr std::array<std::array<unsigned, 2>, 12> edges{{
        {{0u, 1u}}, {{1u, 2u}}, {{2u, 3u}}, {{3u, 0u}},
        {{4u, 5u}}, {{5u, 6u}}, {{6u, 7u}}, {{7u, 4u}},
        {{0u, 4u}}, {{1u, 5u}}, {{2u, 6u}}, {{3u, 7u}},
    }};
    for (const auto& edge : edges) {
        const auto& first = corners[edge[0]];
        const auto& second = corners[edge[1]];
        glVertex3f(first.x, first.y, first.z);
        glVertex3f(second.x, second.y, second.z);
    }
}

neomdl::model::Mat4 multiplyColumnMajor(const GLfloat* left,
                                            const GLfloat* right) noexcept {
    neomdl::model::Mat4 result;
    for (std::size_t column = 0u; column < 4u; ++column) {
        for (std::size_t row = 0u; row < 4u; ++row) {
            float value = 0.0f;
            for (std::size_t index = 0u; index < 4u; ++index) {
                value += left[index * 4u + row] *
                         right[column * 4u + index];
            }
            result.values[column * 4u + row] = value;
        }
    }
    return result;
}

neomdl::model::TriggerPlane normalizedOutsidePlane(
    float x, float y, float z, float distance) noexcept {
    // OpenGL frustum extraction produces planes whose non-negative side is
    // inside. PartOutside/BoxAbovePlane uses the opposite convention: a
    // positive result means the complete box is outside.
    x = -x;
    y = -y;
    z = -z;
    distance = -distance;
    const auto magnitude = std::sqrt(x * x + y * y + z * z);
    if (!(magnitude > 1.0e-20f) || !std::isfinite(magnitude)) return {};
    const auto reciprocal = 1.0f / magnitude;
    return {{x * reciprocal, y * reciprocal, z * reciprocal},
            distance * reciprocal};
}

std::vector<neomdl::model::TriggerPlane> currentTriggerFrustum() {
    GLfloat projection[16]{};
    GLfloat modelView[16]{};
    glGetFloatv(GL_PROJECTION_MATRIX, projection);
    glGetFloatv(GL_MODELVIEW_MATRIX, modelView);
    const auto clip = multiplyColumnMajor(projection, modelView);
    const auto& matrix = clip.values;
    std::vector<neomdl::model::TriggerPlane> result;
    result.reserve(6u);
    result.push_back(normalizedOutsidePlane(
        matrix[3] + matrix[0], matrix[7] + matrix[4],
        matrix[11] + matrix[8], matrix[15] + matrix[12]));
    result.push_back(normalizedOutsidePlane(
        matrix[3] - matrix[0], matrix[7] - matrix[4],
        matrix[11] - matrix[8], matrix[15] - matrix[12]));
    result.push_back(normalizedOutsidePlane(
        matrix[3] + matrix[1], matrix[7] + matrix[5],
        matrix[11] + matrix[9], matrix[15] + matrix[13]));
    result.push_back(normalizedOutsidePlane(
        matrix[3] - matrix[1], matrix[7] - matrix[5],
        matrix[11] - matrix[9], matrix[15] - matrix[13]));
    result.push_back(normalizedOutsidePlane(
        matrix[3] + matrix[2], matrix[7] + matrix[6],
        matrix[11] + matrix[10], matrix[15] + matrix[14]));
    result.push_back(normalizedOutsidePlane(
        matrix[3] - matrix[2], matrix[7] - matrix[6],
        matrix[11] - matrix[10], matrix[15] - matrix[14]));
    return result;
}

void drawTriggerVertices(GLenum primitive,
                         const std::vector<neomdl::model::Vec3>& vertices) {
    if (vertices.empty()) return;
    glBegin(primitive);
    for (const auto& vertex : vertices)
        glVertex3f(vertex.x, vertex.y, vertex.z);
    glEnd();
}

void drawTriggerVolumeGeometry(
    const neomdl::model::TriggerVolume& volume) {
    drawTriggerVertices(GL_TRIANGLE_STRIP, volume.sideStrip);
    drawTriggerVertices(GL_POLYGON, volume.top);
    drawTriggerVertices(GL_POLYGON, volume.bottom);
}

void drawTriggerScreenOverlay(
    const neomdl::model::TriggerVolume& volume, float alpha) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glColor4f(volume.highlight.color.x, volume.highlight.color.y,
              volume.highlight.color.z, alpha);
    glBegin(GL_QUADS);
    glVertex3f(-1.0f, -1.0f, 0.0f);
    glVertex3f(1.0f, -1.0f, 0.0f);
    glVertex3f(1.0f, 1.0f, 0.0f);
    glVertex3f(-1.0f, 1.0f, 0.0f);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

} // namespace

ModelCanvas::ModelCanvas(wxWindow* parent)
    : wxGLCanvas(parent, wxID_ANY, canvasAttributes(), wxDefaultPosition, wxDefaultSize,
                 wxFULL_REPAINT_ON_RESIZE | wxBORDER_NONE),
      context_(std::make_unique<wxGLContext>(this)) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &ModelCanvas::onPaint, this);
    Bind(wxEVT_SIZE, &ModelCanvas::onSize, this);
    Bind(wxEVT_ERASE_BACKGROUND, &ModelCanvas::onErase, this);
    Bind(wxEVT_LEFT_DOWN, &ModelCanvas::onMouse, this);
    Bind(wxEVT_LEFT_UP, &ModelCanvas::onMouse, this);
    Bind(wxEVT_RIGHT_DOWN, &ModelCanvas::onMouse, this);
    Bind(wxEVT_RIGHT_UP, &ModelCanvas::onMouse, this);
    Bind(wxEVT_MIDDLE_DOWN, &ModelCanvas::onMouse, this);
    Bind(wxEVT_MIDDLE_UP, &ModelCanvas::onMouse, this);
    Bind(wxEVT_MOTION, &ModelCanvas::onMouse, this);
    Bind(wxEVT_MOUSEWHEEL, &ModelCanvas::onMouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &ModelCanvas::onCaptureLost, this);
    Bind(wxEVT_KEY_DOWN, &ModelCanvas::onKey, this);
    Bind(wxEVT_KEY_UP, &ModelCanvas::onKey, this);
    Bind(wxEVT_KILL_FOCUS, &ModelCanvas::onFocusLost, this);
    Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
        if (freeFlyCameraActive() && std::any_of(flyMovement_.begin(), flyMovement_.end(),
                                               [](bool down) { return down; })) {
            Refresh(false);
            event.RequestMore();
        }
    });
}

ModelCanvas::~ModelCanvas() {
    releaseChunkCallbacks();
    if (context_ && IsShownOnScreen() && SetCurrent(*context_)) {
        clearGpuTextures();
        meshRenderer_.shutdown();
    }
    // When hidden/detached, destruction of our owned GL context releases its
    // objects. The renderer destructor never calls GL on another context.
}

void ModelCanvas::setLoading(bool loading) {
    if(loading_==loading)return;
    loading_=loading;
    selectionStart_.reset();
    meshPick_.reset();
    if(sceneAudioProvider_)try {sceneAudioProvider_->setSuspended(loading);}
    catch(const std::exception& error) {
        // This is the editor's modal-job boundary. A failed external device
        // must not unwind the UI restoration destructor or retain stale voice
        // handles. No other provider is selected on its behalf.
        sceneAudioVoices_.reset({}, {}, {});
        sceneAudioRuntime_.clear(sceneAudioBackend());sceneAudioScheduled_=false;
        sceneAudioProvider_.reset();sceneAudioBuffers_.clear();
        sceneAudioStatus_=error.what();dialogueMessages_.push_back(sceneAudioStatus_);
    }
    if(loading) {
        resetCameraInput();
        resumeDialogueAudio_=dialogueAudio_.playing();
        if(resumeDialogueAudio_)dialogueAudio_.pause();
    } else {
        if(resumeDialogueAudio_) {
            try {dialogueAudio_.play();}
            catch(const std::exception& e){dialogueMessages_.push_back(e.what());dialogueAudioFailed_=true;}
        }
        resumeDialogueAudio_=false;Refresh(false);
    }
}

AreaScene ModelCanvas::prepareAreaLoad() {
    if (!document_ || !engineAreaRuntimeSupported(document_->model().game))
        throw std::runtime_error("Area context requires an implemented engine scene");
    AreaScene pending;
    pending.prepareRuntime({&particleScene_.windManager(),&particleScene_.runtimeRandom(),&sceneQueries_,document_->model().game});
    return pending;
}
void ModelCanvas::installArea(AreaScene pending,PreparedSceneAudioPlayback sounds,bool preserveCamera) {
    // Validate external visual registries before releasing the current area.
    // Both installations are deterministic and repeated after the area swap.
    {
        TriggerScene validation;validation.setDocument(document_);
        std::string error;
        if(!validation.replaceAreaTriggers(pending.triggers(),error))
            throw std::runtime_error("Area trigger load: "+error);
        CameraScene cameras;cameras.setDocument(document_);
        if(!cameras.setAreaCameras(pending.cameras(),error))
            throw std::runtime_error("Area camera load: "+error);
    }
    for(const auto& object:pending.objects()) {
        object->particles().setSettings(options_.particleSettings);
        object->particles().setChunkLightsaberSettings({options_.lightsaberSettings.renderPieces,
                                                       options_.lightsaberSettings.renderFrontHalf});
        object->lightsabers().setSettings({options_.lightsaberSettings.renderPieces,
                                         options_.lightsaberSettings.renderFrontHalf});
    }
    clearArea();
    pending.commitRuntime({&particleScene_.windManager(),&particleScene_.runtimeRandom(),&sceneQueries_,document_->model().game,{},particleScene_.sceneAnimation()});
    areaScene_ = std::move(pending);explicitDirtyArea_.reset();explicitDirtyIdentity_.clear();++dirtyAreaEpoch_;
    resetWorldClock(false);
    sceneAudioAssets_=std::move(sounds);
    std::string visualRecordError;
    if(!triggerScene_.replaceAreaTriggers(areaScene_.triggers(),visualRecordError))
        throw std::runtime_error("Area trigger load changed during installation: "+visualRecordError);
    if(!cameraScene_.setAreaCameras(areaScene_.cameras(),visualRecordError))
        throw std::runtime_error("Area camera load changed during installation: "+visualRecordError);
    if(areaScene_.visualSettings().wind)
        particleScene_.windManager().applyK2AreaWind(*areaScene_.visualSettings().wind);
    synchronizeEngineQueries();
    pendingGpuReset_ = true;
    updateSceneBounds();
    if (!preserveCamera) frameModel();
    Refresh(false);
}
void ModelCanvas::clearArea() {
    selectedAreaOwners_.clear();
    stopDialogue();
    clearWorldClock();
    for(auto it=dialogueParticipantStates_.begin();it!=dialogueParticipantStates_.end();)
        if(it->first)it=dialogueParticipantStates_.erase(it);else ++it;
    clearSceneAudio();
    triggerMineInputs_.clear();trapDoorAnimations_.clear();
    pendingDoorElapsed_=0;
    if(worldPlaybackSettings_.playerOwner&&*worldPlaybackSettings_.playerOwner!=0u)
        worldPlaybackSettings_.playerOwner.reset();
    releaseChunkCallbacks();
    weatherInput_.enabled=false;
    disableDirtyMaterial();
    triggerScene_.clearAreaTriggers();cameraScene_.clearAreaCameras();
    shadowScene_.clear(lightScene_);motionBlurScene_.clear();motionBlurQueue_.clear();
    if(worldPlaybackSettings_.engineRoomSelection)worldPlaybackSettings_.sceneFocus={};
    sceneQueries_.setRooms({});particleScene_.setSceneQueries(nullptr);
    pendingFlareElapsed_=0;pendingSceneElapsed_=0;pendingEmitterSceneAnimating_=false;
    emitterPrewarmTrace_={};emitterFrameTrace_={};particleError_.clear();grassError_.clear();
    areaScene_.clear(lightScene_);
    particleScene_.sceneAnimation()->setInterleaved(false);
    for (const auto id : areaOwnerQueries_) {
        auto query = lightScene_.ownerQuery(id).value_or(LightOwnerQuery{});
        query.source = LightQuerySource::Global;
        lightScene_.setOwnerQuery(id,query);
    }
    areaOwnerQueries_.clear();
    particleScene_.setEmitterRooms({});
    resetWorldClock(false);
    synchronizeEngineQueries();
    pendingGpuReset_ = true;
    updateSceneBounds();
    Refresh(false);
}
SceneAudioBackend ModelCanvas::sceneAudioBackend() {
    return {
        [this](const SceneAudioPlay& p){return playSceneAudio(p);},
        [this](std::uint64_t id){sceneAudioVoices_.stop(id,sceneAudioVoiceBackend());},
        [this](std::uint64_t id){return sceneAudioVoices_.playing(id,sceneAudioVoiceBackend());}
    };
}
SceneAudioVoiceBackend ModelCanvas::sceneAudioVoiceBackend() {
    SceneAudioVoiceBackend backend;
    backend.resource=[this](const SceneAudioPlay& p)->std::optional<SceneAudioVoiceResource> {
        const SceneAudioResourceKey key{engineSceneAudioResRef(p.resref),p.resourceType,p.streaming};
        if(!sceneAudioAssets_.sounds.count(key))return {}; // engine resource-open failure
        const auto& sound=sceneAudioBuffers_.at(sceneAudioBufferKey(p));
        return SceneAudioVoiceResource{sound.durationMilliseconds,p.streaming?0u:sound.sampleRate};
    };
    backend.start=[this](std::uint64_t provider,const SceneAudioPlay& p,std::uint32_t frequency) {
        admitSceneAudio(provider,p,frequency);
    };
    backend.prepareStream=[this](std::uint64_t provider,const SceneAudioPlay& p,bool retained) {
        if(!retained) {
            const auto& sound=sceneAudioBuffers_.at(sceneAudioBufferKey(p));
            sceneAudioProvider_->preparePausedStream(provider,sound);
        }
        const auto gain=engineSceneAudioGain(p,sceneAudioAssets_.priorities.at(p.priorityGroup));
        sceneAudioProvider_->setGain(provider,gain);
        updateSceneAudioStream(provider,p);
        updateSceneAudioStream(provider,p);
        sceneAudioProvider_->setLooping(provider,p.looping);
    };
    backend.resumeStream=[this](std::uint64_t provider){sceneAudioProvider_->resume(provider);};
    backend.pauseStream=[this](std::uint64_t provider){sceneAudioProvider_->pause(provider);};
    backend.restart=[this](std::uint64_t provider,bool looping) {sceneAudioProvider_->restart(provider,looping);};
    backend.stop=[this](std::uint64_t provider){sceneAudioProvider_->erase(provider);};
    backend.playing=[this](std::uint64_t provider){return sceneAudioProvider_&&sceneAudioProvider_->playing(provider);};
    backend.updateStream=[this](std::uint64_t provider,const SceneAudioPlay& p){updateSceneAudioStream(provider,p);};
    return backend;
}
void ModelCanvas::clearSceneAudio() {
    sceneAudioRuntime_.clear(sceneAudioBackend());sceneAudioScheduled_=false;
    sceneAudioVoices_.clear(sceneAudioVoiceBackend());sceneAudioProvider_.reset();sceneAudioBuffers_.clear();
    sceneAudioAssets_={};sceneAudioExo_.reset();sceneAudioStatus_.clear();
    sceneAudioOneShot_=std::uint64_t{1}<<63;
}
void ModelCanvas::stopSceneAudio() {
    sceneAudioRuntime_.clear(sceneAudioBackend());sceneAudioScheduled_=false;
    sceneAudioVoices_.clear(sceneAudioVoiceBackend());sceneAudioStatus_="Scene sound playback stopped";
}
void ModelCanvas::ensureSceneAudioProvider() {
    if(sceneAudioProvider_)return;
    auto device=std::make_unique<SceneAudioDevice>();
    auto buffers=prepareSceneAudioBuffers(sceneAudioAssets_,{},{});
    sceneAudioProvider_=std::move(device);sceneAudioBuffers_=std::move(buffers);
    sceneAudioVoices_.reset(sceneAudioAssets_.priorities,
        engineSceneAudioDefaultVoiceLimits(document_->model().game),
        [this]{return particleScene_.runtimeRandom().next();},document_->model().game);
    sceneAudioProvider_->setSuspended(loading_);
    sceneAudioStatus_="Scene audio ready";
}
void ModelCanvas::configureSceneAudio(std::optional<ExoRandom::State> capturedState) {
    ensureSceneAudioProvider();
    stopSceneAudio();
    if(capturedState)sceneAudioExo_.emplace(*capturedState);
    if(!areaScene_.sounds().empty()) {
        const auto clock=sceneAudioClock(areaScene_.environment());
        if(!clock)throw std::runtime_error("GIT sound scheduling requires the module world clock");
        if(!sceneAudioExo_&&std::any_of(areaScene_.sounds().begin(),areaScene_.sounds().end(),
            [](const auto& sound){return sound.interval&&sound.intervalVariation;}))
            throw std::runtime_error("These sounds use interval variation. Supply the captured random state in Developer > Scene Audio Inputs.");
        sceneAudioRuntime_.load(areaScene_.sounds(),*clock,{
            [this]{return particleScene_.runtimeRandom().next();},
            [this]{return sceneAudioExo_.value().next();}
        },document_->model().game);
        sceneAudioScheduled_=true;
    }
    sceneAudioProvider_->setSuspended(loading_);
    sceneAudioStatus_="Scene sound playback active";
}
void ModelCanvas::mergeSceneAudioAssets(PreparedSceneAudioPlayback prepared) {
    if(!prepared.priorities.empty())sceneAudioAssets_.priorities=std::move(prepared.priorities);
    if(sceneAudioProvider_) {
        auto buffers=prepareSceneAudioBuffers(prepared,sceneAudioBuffers_,{});
        sceneAudioBuffers_.insert(buffers.begin(),buffers.end());
    }
    for(auto& entry:prepared.sounds)sceneAudioAssets_.sounds.emplace(entry.first,std::move(entry.second));
}
void ModelCanvas::admitSceneAudio(std::uint64_t provider,const SceneAudioPlay& p,std::uint32_t frequency) {
    const auto& sound=sceneAudioBuffers_.at(sceneAudioBufferKey(p));
    const auto& priority=sceneAudioAssets_.priorities.at(p.priorityGroup);
    SceneAudioSourceParameters parameters;
    parameters.gain=engineSceneAudioGain(p,priority);
    parameters.pitch=float(frequency)/float(sound.sampleRate);
    parameters.positional=p.positional;parameters.looping=p.looping;
    const auto position=engineAudioProviderPosition(p.position);
    parameters.position={position.x,position.y,position.z};
    parameters.minimumDistance=p.minimumDistance;parameters.maximumDistance=p.maximumDistance;
    sceneAudioProvider_->play(provider,sound,parameters);
}
void ModelCanvas::updateSceneAudioStream(std::uint64_t provider,const SceneAudioPlay& p) {
    // Authored source position already feeds the engine's stream mix. The
    // common backend receives that mix as a listener-relative voice, avoiding
    // a second backend-specific distance/spatialization pass.
    const auto gain=engineSceneAudioGain(p,sceneAudioAssets_.priorities.at(p.priorityGroup));
    if(const auto mix=engineStreamingAudioMix(p,sceneAudioListener_,gain,0,document_->model().game,0.0f)) {
        sceneAudioProvider_->setGain(provider,mix->gain);
        if(mix->writePan)
            sceneAudioProvider_->setPan(provider,mix->pan*2.0f-1.0f);
    }
}
std::uint32_t ModelCanvas::playSceneAudio(const SceneAudioPlay& request) {
    auto p=request;
    // PlayOneShot's C_/P_/N_ path replaces its one streaming-source slot.
    if(p.oneShot&&p.streaming)p.source=std::numeric_limits<std::uint64_t>::max();
    return sceneAudioVoices_.play(std::move(p),sceneAudioVoiceBackend(),sceneAudioListener_.position);
}
void ModelCanvas::updateSceneAudioListener() {
    // UpdateSoundEngine retains Camera's position and raw zdir/ydir;
    // transposing a nonunit quaternion's view matrix cannot recover position.
    const auto frame=sceneCameraFrame();
    const auto forward=model::engineQuaternionAxis(frame.orientation,2u);
    sceneAudioListener_={frame.position,{-forward.x,-forward.y,-forward.z},
        model::engineQuaternionAxis(frame.orientation,1u)};
    if(!dialogueCameraActive_&&!dialogueActorCameraActive_&&!cameraScene_.active())
        if(const auto* camera=cameraScene_.activeAreaCamera())
            sceneAudioListener_=placeableCameraAudioListener(camera->position,camera->orientation,camera->authored.microphoneRange);
    const auto listener=engineAudioProviderListener(sceneAudioListener_);
    sceneAudioProvider_->setListener({listener.position.x,listener.position.y,listener.position.z},
        {listener.forward.x,listener.forward.y,listener.forward.z},{listener.up.x,listener.up.y,listener.up.z});
}
void ModelCanvas::advanceSceneAudio() {
    if(!sceneAudioProvider_)return;
    try {
        updateSceneAudioListener();
        if(sceneAudioScheduled_)
            if(const auto clock=sceneAudioClock(areaScene_.environment()))sceneAudioRuntime_.update(*clock,sceneAudioBackend());
        sceneAudioVoices_.update(sceneAudioVoiceBackend(),sceneAudioListener_.position);
    }catch(const std::exception& e) {
        stopSceneAudio();sceneAudioStatus_=e.what();
        dialogueMessages_.push_back(sceneAudioStatus_);
    }
}
void ModelCanvas::setSceneOwnerWorld(LightOwnerId id,const model::Mat4& world) {
    (void)engineRigidFrame(world);
    if(!document_||!document_->isOpen())throw std::runtime_error("No open scene");
    bool applied=false;
    if(id==mainLightOwner&&document_->hasModelOwner()) {referenceScene_.setOwnerWorld(world,false);applied=true;}
    else for(const auto& object:areaScene_.objects())if(!object->staticPrimary()&&object->ownerId()==id) {
        object->setWorld(world);applied=true;break;
    }
    if(!applied)throw std::runtime_error("Move the containing Gob: references retain their engine attachment");
    synchronizeEngineQueries();updateSceneBounds();shaderErrorReported_=false;Refresh(false);
}
DirtyAreaState ModelCanvas::dirtyAreaState() const {
    return explicitDirtyArea_?*explicitDirtyArea_:areaScene_.visualSettings().dirty;
}
void ModelCanvas::loadDirtyArea(const std::filesystem::path& path) {
    neogff::GffFile gff;gff.LoadFile(path);
    if(!gff.root()||gff.filetype()!="ARE "||gff.version()!="V3.2")throw std::runtime_error("Expected ARE V3.2");
    auto tuple=readDirtyArea(*gff.root());
    explicitDirtyArea_=std::move(tuple);explicitDirtyIdentity_=path.generic_u8string();++dirtyAreaEpoch_;
    shaderErrorReported_=false;Refresh(false);
}
void ModelCanvas::useLoadedDirtyArea() {
    explicitDirtyArea_.reset();explicitDirtyIdentity_.clear();++dirtyAreaEpoch_;shaderErrorReported_=false;Refresh(false);
}
void ModelCanvas::disableDirtyMaterial() {
    dirtySettings_.enabled=false;dirtyMesh_=nullptr;shaderErrorReported_=false;
}
void ModelCanvas::setDirtyMaterial(LightOwnerId owner,std::size_t mesh,material::DirtyMaterialSettings settings) {
    if(!settings.enabled){disableDirtyMaterial();Refresh(false);return;}
    if(!options_.textures||!options_.lighting)throw std::runtime_error("Enable textures and lighting before the Dirty diagnostic");
    const auto& owners=motionBlurScene_.owners();
    const auto it=std::find_if(owners.begin(),owners.end(),[&](const auto& x){return x.id==owner;});
    if(it==owners.end()||mesh>=it->asset->meshes.size())throw std::runtime_error("Dirty target is no longer registered");
    const auto& target=it->asset->meshes[mesh];
    if(target.skinned||target.animated)throw std::runtime_error("Dirty caller requires a rigid PartTriMesh");
    if(settings.slot>=3||!std::isfinite(settings.desaturation)||!std::isfinite(settings.forceAlpha))
        throw std::runtime_error("Invalid Dirty diagnostic inputs");
    const auto* image=document_->texture(material::dirtyTextureNames[settings.slot]);
    if(!image)throw std::runtime_error("Load the real dirtytex/dirtytex2/dirtytex3 resource before enabling its caller");
    const auto meta=document_->textureMetadata(material::dirtyTextureNames[settings.slot]);
    if(!meta||!material::procedureDefinition(meta->txi,engineMaterialGame(it->asset->game)).volumeTexture)
        throw std::runtime_error("Selected resource is not a engine Dirty volume controller");
    dirtySettings_=settings;dirtyOwner_=owner;dirtyMesh_=&target;shaderErrorReported_=false;Refresh(false);
}

void ModelCanvas::setSceneShadowInputs(SceneShadowInputs inputs) {
    shadowScene_.setInputs(std::move(inputs));
    lightScene_.removeOwner(ShadowScene::independentContainer);
    shaderErrorReported_=false;Refresh(false);
}
void ModelCanvas::setSceneCasterEnabled(LightOwnerId owner,bool enabled) {
    shadowScene_.setCasterEnabled(owner,enabled);shaderErrorReported_=false;Refresh(false);
}
const AreaReplayCamera* ModelCanvas::activeAreaReplayCamera() const noexcept {
    return options_.useAreaReplayCamera&&!areaScene_.empty()?areaScene_.activeReplayCamera():nullptr;
}
model::FogSettings ModelCanvas::effectiveFog() const noexcept {
    const auto areaFog=[this] {
        if(!options_.useLoadedAreaEnvironment||areaScene_.empty())return options_.fog;
        const auto& sample=areaScene_.environment().sample();
        if(!sample.hasFogState)return options_.fog;
        model::FogSettings fog=options_.fog;
        if(!sample.fogEnabled){fog.mode=model::FogMode::Disabled;return fog;}
        if(!sample.fogColor||!sample.fogNear||!sample.fogFar)return options_.fog;
        fog.mode=model::FogMode::Linear;fog.color=*sample.fogColor;
        fog.start=*sample.fogNear;fog.end=*sample.fogFar;return fog;
    };
    auto fog=areaFog();
    fog.mode=options_.fogEnabled?fog.mode:model::FogMode::Disabled;
    const auto force=effectiveForceSight();
    if(force.enabled) {
        // Scene::SetFogColor 0xbfc02 and DoFog select the retained gray color.
        const float gray=(0.30f*fog.color.x+0.59f*fog.color.y+0.11f*fog.color.z)*force.desaturation;
        fog.color={gray,gray,gray};
    }
    return fog;
}

std::array<float,3> ModelCanvas::effectiveSceneAmbient() const noexcept {
    if(options_.useLoadedAreaEnvironment&&!areaScene_.empty()) {
        const auto& ambient=areaScene_.environment().sample().sceneAmbient;
        if(ambient)return {{ambient->x,ambient->y,ambient->z}};
    }
    return options_.sceneAmbient;
}
bool ModelCanvas::effectiveAreaShadows() const noexcept {
    if(!options_.useLoadedAreaEnvironment||areaScene_.empty())return true;
    const auto& sample=areaScene_.environment().sample();
    return !sample.hasShadowState||sample.shadowsEnabled;
}
void ModelCanvas::setAreaRoom(std::optional<LightRoomId> id) {
    areaScene_.setCurrentRoom(id);Refresh(false);
}
void ModelCanvas::useReplayedAreaRoom() {areaScene_.useReplayedCurrentRoom();Refresh(false);}
void ModelCanvas::updateWorldRoomSelection() {
    if(worldPlaybackSettings_.engineRoomSelection) {
        WorldRoomInputs input;input.cameraMode=worldPlaybackSettings_.cameraMode;
        WorldSceneFocusInputs focus;
        if(worldPlaybackSettings_.playerOwner) {
            const auto selected=*worldPlaybackSettings_.playerOwner;
            if(const auto* owner=sceneQueries_.owner(selected)) {
                input.playerRenderPosition=owner->position;
                WorldSceneFocusPlayer player{owner->position,owner->position};
                std::optional<std::size_t> actorOwner;
                bool primaryActor=false;
                if(selected==mainLightOwner) {
                    primaryActor=true;
                    player.position=referenceScene_.ownerFrame().position;
                } else for(const auto& areaObject:areaScene_.objects()) {
                    const auto& object=*areaObject;
                    if(selected==object.ownerId()) {
                        primaryActor=true;actorOwner=object.instanceIndex();
                        player.position=object.references().ownerFrame().position;
                        break;
                    }
                }
                if(primaryActor)for(const auto& active:dialogueActorAnimations_)
                    if(active.source->owner==actorOwner&&active.logicalFrame)
                        player.position=active.logicalFrame->position;
                // Reference/chunk selections are explicit host captures: they
                // have no separate CSWCObject logical-position field. Their
                // selected world origin supplies both player input positions.
                focus.player=player;
            }
        }
        if(worldPlaybackSettings_.moduleCameraPresent) {
            const auto capture=[&](const auto& camera) {
                input.moduleCameraPosition=camera.position;
                focus.moduleCamera=WorldSceneFocusCamera{camera.position,camera.orientation};
            };
            // MainLoop reads the current module camera before/after AI;
            // authored cameras must not wait for the next picking capture.
            if(dialogueCameraActive_&&dialogueCameraScene_.active())
                capture(*dialogueCameraScene_.active());
            else if(dialogueActorCameraActive_&&dialogueActorCamera_.frame())
                capture(*dialogueActorCamera_.frame());
            else if(cameraScene_.active())capture(*cameraScene_.active());
            else if(cameraScene_.activeAreaCamera())capture(*cameraScene_.activeAreaCamera());
            else if(const auto* replay=activeAreaReplayCamera())
                capture(engineRigidFrame(replay->world));
            else if(pickingCamera_)capture(*pickingCamera_);
        }
        input.dialogueActive=focus.dialogueActive=dialogueRuntime_.running();
        input.shotCameraActive=focus.shotCameraActive=dialogueActorCameraActive_;
        input.animatedCameraActive=focus.animatedCameraActive=dialogueCameraActive_;
        // SetSceneFocus depends on the Scene and player, not WOK availability.
        // A missing selected player preserves the retained focus position.
        if(const auto position=engineWorldSceneFocus(focus))
            worldPlaybackSettings_.sceneFocus=*position;
        if(!areaScene_.empty()&&areaScene_.roomPositionQueriesAvailable())
            areaScene_.updateCurrentRoomFromWorld(input);
    } else {
        const auto position=sceneCameraFrame().position;
        if(worldPlaybackSettings_.cameraSceneFocus)worldPlaybackSettings_.sceneFocus=position;
        if(!areaScene_.empty()&&areaScene_.roomPositionQueriesAvailable())
            areaScene_.updateCurrentRoomFromPosition(position);
    }
}

void ModelCanvas::synchronizeWorldPause() {
    // Client SetPauseState invokes PauseScene/PlayScene as well as changing
    // timer admission; the clock retains those engine transition results.
    particleScene_.sceneAnimation()->setAnimating(
        !worldPlaybackSettings_.paused&&!worldClock_.worldPaused());
}
void ModelCanvas::synchronizeWorldClockCreatures() {
    std::set<HitOwnerId> current;
    if(document_&&document_->hasModelOwner())current.insert(mainLightOwner);
    for(const auto& object:areaScene_.objects())
        if(object->kind()==AreaObjectKind::Creature)current.insert(object->ownerId());
    for(auto entry=worldClockOwnerKeys_.begin();entry!=worldClockOwnerKeys_.end();) {
        if(current.count(entry->first)) {++entry;continue;}
        worldClock_.unregisterCreature(entry->second);
        entry=worldClockOwnerKeys_.erase(entry);
    }
    for(const auto owner:current) {
        const auto entry=worldClockOwnerKeys_.emplace(owner,owner).first;
        const auto key=entry->second;
        worldClock_.registerCreature(key,[this,key](bool enabled) {
            const auto live=std::find_if(worldClockOwnerKeys_.begin(),worldClockOwnerKeys_.end(),
                [key](const auto& entry){return entry.second==key;});
            // GetModel(255,1) selects only the creature's primary Gob; its
            // head and equipment do not receive this ForceAnimations write.
            animationOwnerForId(live->first)->setForceAnimated(enabled);
        });
    }
}
std::vector<HitOwnerId> ModelCanvas::worldPauseExclusions() const {
    std::vector<HitOwnerId> result;
    for(const auto key:worldClock_.exclusions()) {
        const auto live=std::find_if(worldClockOwnerKeys_.begin(),worldClockOwnerKeys_.end(),
            [key](const auto& entry){return entry.second==key;});
        if(live!=worldClockOwnerKeys_.end())result.push_back(live->first);
    }
    return result;
}
void ModelCanvas::setWorldPauseState(std::uint8_t mask,int enable,
        const std::vector<HitOwnerId>& exclusions) {
    synchronizeWorldClockCreatures();
    std::vector<std::uint64_t> keys;keys.reserve(exclusions.size());
    for(const auto owner:exclusions) {
        // AddToPauseExclusionList skips IDs absent from the engine object array.
        const auto entry=worldClockOwnerKeys_.find(owner);
        if(entry!=worldClockOwnerKeys_.end())keys.push_back(entry->second);
    }
    worldClock_.setPauseState(mask,enable,keys);
    // Engine commands may snapshot an already-unpaused timer. Publish its
    // changed epoch immediately without rebasing module/area prior samples.
    dialogueRuntime_.setWorldTime(worldClock_.worldTime(),worldClock_.dayMilliseconds());
    synchronizeWorldPause();Refresh(false);
}
void ModelCanvas::clearWorldClock() {
    // Release exclusion flags while their old primary Gobs are still alive.
    // CWorldTimer's constructor epoch and day period (five minutes/hour).
    worldClock_.reset(450380u,10496000u,7200000u);
    areaScene_.resetWeatherWorldTime(worldClock_.worldTime());
    dialogueRuntime_.setWorldTime(worldClock_.worldTime(),worldClock_.dayMilliseconds());
    worldClockOwnerKeys_.clear();worldMicrosecondsRemainder_=0;
}
void ModelCanvas::resetWorldClock(bool preservePause) {
    const auto mask=preservePause?worldClock_.pauseMask():std::uint8_t{};
    const auto exclusions=preservePause?worldPauseExclusions():std::vector<HitOwnerId>{};
    const auto& environment=areaScene_.environment();
    const auto& time=environment.worldTime();
    if(time)worldClock_.reset(time->calendarDay,time->timeOfDayMilliseconds,
        environment.worldDayMilliseconds().value());
    else worldClock_.reset(450380u,10496000u,7200000u);
    // Engine area construction samples the current world epoch; explicit
    // inspector resets rebase that same owned sample to the loaded clock.
    areaScene_.resetWeatherWorldTime(worldClock_.worldTime());
    dialogueRuntime_.setWorldTime(worldClock_.worldTime(),worldClock_.dayMilliseconds());
    worldClockOwnerKeys_.clear();worldMicrosecondsRemainder_=0;
    synchronizeWorldClockCreatures();
    // An explicit inspector time reset reconstructs its captured pause state
    // after establishing the loaded world epoch and current creature IDs.
    if(mask&1u)setWorldPauseState(1u,1,exclusions);
    if(mask&2u)setWorldPauseState(2u,1,exclusions);
    synchronizeWorldPause();
}
void ModelCanvas::setWorldPlaybackSettings(WorldPlaybackSettings settings) {
    worldPlaybackSettings_=std::move(settings);
    synchronizeWorldPause();Refresh(false);
}
void ModelCanvas::resetWorldTime() {
    stopSceneAudio();
    areaScene_.resetWorldTime();resetWorldClock(true);Refresh(false);
}
bool ModelCanvas::applyAreaEnvironmentReplay(AreaEnvironmentReplayState state,std::string& error) {
    if(!areaScene_.applyEnvironmentReplay(std::move(state),error))return false;
    shaderErrorReported_=false;Refresh(false);return true;
}
void ModelCanvas::clearAreaEnvironmentReplay() {
    areaScene_.clearEnvironmentReplay();shaderErrorReported_=false;Refresh(false);
}

void ModelCanvas::loadGrassArea(const std::filesystem::path& path) {
    areaScene_.loadGrassArea(path);grassError_.clear();pendingGpuReset_=true;Refresh(false);
}
void ModelCanvas::configureGrass(const GrassSettings& settings) {
    areaScene_.configureGrass(settings);grassError_.clear();pendingGpuReset_=true;Refresh(false);
}
bool ModelCanvas::playAreaAnimation(std::size_t instance,const std::string& clip,float rate,std::string& error) {
    auto* object=areaScene_.object(instance);
    if(!object){error="Unknown area object";return false;}
    std::optional<std::uint16_t> roomAnimation;
    if(object->kind()==AreaObjectKind::AnimatedRoom && clip.rfind("scriptloop",0)==0 && rate==1.0f) {
        std::uint16_t number{};
        const auto end=clip.data()+clip.size();
        const auto parsed=std::from_chars(clip.data()+10,end,number);
        if(parsed.ec==std::errc{}&&parsed.ptr==end)roomAnimation=number;
    }
    try {
        const auto result=roomAnimation?object->playRoomAnimation(*roomAnimation):object->play(clip,rate);
        if(!result){error=result.diagnostic;return false;}
        object->setPaused(false);synchronizeEngineQueries();updateSceneBounds();Refresh(false);error.clear();return true;
    } catch(const std::exception& failure) {
        error=failure.what();return false;
    }
}
void ModelCanvas::pauseAreaObject(std::size_t instance,bool paused) {
    if(auto* object=areaScene_.object(instance))object->setPaused(paused);
    Refresh(false);
}
void ModelCanvas::playRoomAnimation(const std::string& room,std::int32_t number) {
    areaScene_.playRoomAnimation(room,number);synchronizeEngineQueries();Refresh(false);
}
void ModelCanvas::captureTrackAnimation(std::size_t track,TrackAnimationState state) {
    areaScene_.captureTrackAnimation(track,std::move(state));Refresh(false);
}
void ModelCanvas::startTrackAnimation(std::size_t track) {
    areaScene_.startTrackAnimation(track);synchronizeEngineQueries();Refresh(false);
}
void ModelCanvas::stopTrackAnimation(std::size_t track) {
    areaScene_.stopTrackAnimation(track);synchronizeEngineQueries();Refresh(false);
}
void ModelCanvas::setTrackSpeed(std::size_t track,float speed) {
    areaScene_.setTrackSpeed(track,speed);synchronizeEngineQueries();Refresh(false);
}
void ModelCanvas::playTrackFollowerAnimation(std::size_t track,const std::string& name,int looping,int queued,int overlay) {
    areaScene_.playTrackFollowerAnimation(track,name,looping,queued,overlay);
    synchronizeEngineQueries();Refresh(false);
}
void ModelCanvas::removeTrackFollowerAnimation(std::size_t track,const std::string& name) {
    areaScene_.removeTrackFollowerAnimation(track,name);synchronizeEngineQueries();Refresh(false);
}
void ModelCanvas::resetAreaObject(std::size_t instance) {
    if(auto* object=areaScene_.object(instance))object->reset();
    if(const auto* captured=areaScene_.trackAnimation(instance)) {
        auto state=*captured;state.running=false;
        areaScene_.captureTrackAnimation(instance,std::move(state));
    }
    synchronizeEngineQueries();updateSceneBounds();Refresh(false);
}

bool ModelCanvas::setDoorOpenState(std::size_t instance,std::uint8_t state,std::int32_t selector,std::string& error) {
    auto* object=areaScene_.object(instance);
    auto visual=object->visualState().live();
    visual.animation=doorOpenStateAnimation(state,selector);
    if(!areaScene_.applyObjectVisualState(instance,std::move(visual),error))return false;
    synchronizeEngineQueries();updateSceneBounds();Refresh(false);return true;
}

void ModelCanvas::startDialogue(PreparedDialoguePlayback prepared,std::size_t entry) {
    stopDialogue();
    dialoguePreviousCamera_=cameraScene_.activeAttachment();
    dialoguePreviousAreaCamera_=cameraScene_.activeAreaCameraIndex();dialogueSavedCamera_=true;
    dialogueCameraFov_=kK2CameraDefaultViewAngle;
    dialogueRuntime_.setDialogue(std::move(prepared.dialogue),std::move(prepared.timing),document_->model().game);
    dialogueAssets_=std::move(prepared);
    std::vector<std::uint32_t> angles;
    for(const auto& line:dialogueRuntime_.dialogue().lines)angles.push_back(line.camera.angle.value_or(0));
    dialogueCameraSequence_.initialize(resourceKey(neoshared::pathToUtf8(dialogueRuntime_.dialogue().source.stem())),std::move(angles),document_->model().game);
    if(dialogueAssets_.cameraModel) {
        auto* document=dialogueAssets_.cameraModel.get();
        // LoadAnimatedCamera constructs a new Gob (K1 Desktop 0xb7cb9).
        model::processAsciiSkinSeams(document->model(),resourceLoadJob());
        std::vector<const model::Model*> hierarchy;
        for(std::size_t index=0;index<document->modelHierarchySize();++index)
            hierarchy.push_back(&document->modelAt(index));
        dialogueCameraAnimation_.setModel(&document->model(),std::move(hierarchy));
        dialogueCameraAnimationOwner_=SceneAnimationOwner{};
        dialogueCameraAnimationOwner_.configure(document->model().classification,[this](float seconds,bool evaluate) {
            const auto lifetime=*dialogueCameraAnimationOwner_.lifetime();
            const bool applied=dialogueCameraAnimation_.advance(seconds,evaluate);
            if(lifetime.expired())return applied;
            dialogueCameraReferences_.synchronize(dialogueCameraAnimation_.pose(),false);
            dialogueCameraScene_.synchronize(dialogueCameraAnimation_.pose(),true,false);
            return applied;
        });
        dialogueCameraAnimationOwner_.configureSphere(document->model(),[this] {
            const auto& asset=dialogueAssets_.cameraModel->model();
            return engineMinimumHitSphere(asset,engineHitPose(asset,&dialogueCameraAnimation_.pose()),
                dialogueCameraReferences_.mainFrame().scale);
        });
        dialogueCameraAnimationOwner_.setHasAnimationRuns([this]{return !dialogueCameraAnimation_.empty();});
        dialogueCameraAnimationOwner_.setProcedural([this](float seconds) {
            dialogueCameraAnimation_.advanceProceduralUv(particleScene_.runtimeRandom(),seconds);
        });
        dialogueCameraAnimationOwner_.setDynamicLights(std::any_of(document->model().nodes.begin(),document->model().nodes.end(),
            [](const auto& node){return node.light&&(node.light->dynamicType||node.light->affectDynamic);}));
        dialogueCameraReferences_.setSceneRandom(&particleScene_.runtimeRandom());
        dialogueCameraReferences_.setSceneAnimation(particleScene_.sceneAnimation());
        dialogueCameraReferences_.setParentAnimationOwner(&dialogueCameraAnimationOwner_,&dialogueCameraAnimation_);
        dialogueCameraReferences_.setOwnerSceneMember(false);
        dialogueCameraReferences_.setDocument(document);
        dialogueCameraScene_.setReferenceScene(&dialogueCameraReferences_);
        dialogueCameraScene_.setDocument(document);
        dialogueCameraAnimation_.advanceProceduralUv(particleScene_.runtimeRandom(),0.0f);
        dialogueCameraAnimationOwner_.setScene(particleScene_.sceneAnimation());
        dialogueCameraReferences_.setOwnerSceneMember(true);
    }
    dialogueRuntime_.setWorldTime(worldClock_.worldTime(),worldClock_.dayMilliseconds());
    dialogueRuntime_.begin(entry);dispatchDialogueEvents();advanceDialogue();Refresh(false);
}
void ModelCanvas::stopDialogue() {
    if(dialogueRuntime_.running())applyDialogueVideoEffect(DialogueVideoCaller::CloseDialog,-2);
    dialogueFade_.reset();dialogueDrawElapsed_=0;
    if(dialogueLipSpeakerBound_)stopLipSync(dialogueLipSpeaker_);
    dialogueLipSpeakerBound_=false;dialogueMainLip_=false;
    clearDialogueParticipants();finishDialogueActors();restoreDialogueCamera();
    dialogueAudio_.stop();resumeDialogueAudio_=false;
    dialogueRuntime_.stop();dialogueRuntime_.takeEvents();
    dialogueCameraScene_.setDocument(nullptr);
    dialogueCameraReferences_.setDocument(nullptr);
    dialogueCameraAnimationOwner_.setScene(nullptr);
    dialogueCameraAnimation_.setModel(nullptr,{});
    dialogueActorCamera_.reset();dialogueActorCameraActive_=false;
    dialogueCameraActive_=false;dialogueAssets_={};
    ++sceneEffects_.historyEpoch;Refresh(false);
}
void ModelCanvas::chooseDialogueReply(std::size_t reply) {
    dialogueRuntime_.setWorldTime(worldClock_.worldTime(),worldClock_.dayMilliseconds());
    dialogueRuntime_.chooseReply(reply);dispatchDialogueEvents();advanceDialogue();
}
std::vector<std::string> ModelCanvas::takeDialogueMessages() {
    auto messages=std::move(dialogueMessages_);dialogueMessages_.clear();return messages;
}
const DialogueVoiceAsset& ModelCanvas::selectedDialogueVoice(std::size_t lineIndex) const {
    const auto& asset=dialogueAssets_.lines.at(lineIndex);
    return asset.voices.at(asset.selectedVoice);
}
void ModelCanvas::applyDialogueVideoEffect(DialogueVideoCaller caller,int row) {
    const auto action=dialogueVideoAction(document_->model().game,caller,row,scriptVideoEffect_);
    if(action.operation==DialogueVideoOperation::Retain)return;
    const auto hardware=material::selectK2HardwareCapabilities(options_.engineHardware);
    auto activation=activateVideoEffect(dialogueAssets_.videoEffects.get(),action.row,
        sceneEffects_,document_->model().game,
        hardware.frameBufferModifications||hardware.atiFrameBufferModifications);
    std::string error;
    if(!setSceneEffects(std::move(activation.settings),error,false))dialogueMessages_.push_back(error);
}
void ModelCanvas::dispatchDialogueEvents() {
    for(const auto& event:dialogueRuntime_.takeEvents()) {
        if(event.kind==DialoguePlaybackEvent::Kind::Finished) {
            applyDialogueVideoEffect(DialogueVideoCaller::CloseDialog,-2);
            dialogueFade_.reset();dialogueDrawElapsed_=0;
            clearDialogueParticipants();finishDialogueActors();restoreDialogueCamera();
            ++sceneEffects_.historyEpoch;
            dialogueCameraScene_.deactivate();dialogueCameraActive_=false;
            dialogueActorCamera_.reset();dialogueActorCameraActive_=false;
            dialogueMessages_.push_back("Dialogue finished.");continue;
        }
        if(event.kind==DialoguePlaybackEvent::Kind::PrepareReplies) {
            dialogueCameraSequence_.prepareReplies(event.lineIndices);continue;
        }
        if(event.kind==DialoguePlaybackEvent::Kind::Replies) {
            dialogueMessages_.push_back("Choose a player reply through Dialogue Playback > Choose a reply.");continue;
        }
        if(event.kind==DialoguePlaybackEvent::Kind::NeedsCondition) {
            dialogueMessages_.push_back("Dialogue is waiting for a script condition. Script execution is not available in this viewer.");continue;
        }
        if(event.kind==DialoguePlaybackEvent::Kind::NeedsAction) {
            dialogueMessages_.push_back("Dialogue is waiting for its authored script actions. Script execution is not available in this viewer.");continue;
        }
        const auto& line=dialogueRuntime_.dialogue().lines.at(event.lineIndex);
        auto& asset=dialogueAssets_.lines.at(event.lineIndex);
        if(event.kind==DialoguePlaybackEvent::Kind::StartLip) {
            const auto& voice=selectedDialogueVoice(event.lineIndex);
            if(!asset.speakerBound||!asset.speakerCreature||!voice.visemes)continue;
            if(asset.speaker)areaScene_.object(*asset.speaker)->setVisemes(*voice.visemes);
            else {
                stopCreatureVisemes(animationRuntime_);referenceScene_.stopCreatureHeadVisemes();
                playCreatureVisemes(animationRuntime_,*voice.visemes);
                referenceScene_.setCreatureHeadVisemes(*voice.visemes);
                pose_=animationRuntime_.pose();referenceScene_.synchronize(*pose_,false);
                dialogueMainLip_=true;
            }
            dialogueLipSpeaker_=asset.speaker;dialogueLipSpeakerBound_=true;
            continue;
        }
        dialogueMessages_.push_back(line.label()+": "+line.text);
        if(line.kind==neoshared::dlg::NodeKind::Entry) {
            dialogueCameraSequence_.beginEntry(event.lineIndex);
            dialogueAssets_.speech.beginEntry(event.lineIndex);
        }
        const auto& voiceName=dialogueAssets_.speech.voice(event.lineIndex).resref;
        asset.selectedVoice=static_cast<std::size_t>(std::find_if(asset.voices.begin(),asset.voices.end(),
            [&](const auto& voice){return voice.resref==voiceName;})-asset.voices.begin());
        const auto& voice=selectedDialogueVoice(event.lineIndex);
        dialogueRuntime_.setActiveNodeTiming(voice.timing);
        const auto cameraAngle=dialogueCameraSequence_.angle(event.lineIndex);
        if(line.kind==neoshared::dlg::NodeKind::Reply)dialogueFade_.apply(line.fade);
        setDialogueAnimations(asset);
        if(line.kind==neoshared::dlg::NodeKind::Entry)dialogueFade_.apply(line.fade);
        dialogueAudio_.stop();dialogueAudioFailed_=!voice.audio;
        if(voice.audio) {
            try {dialogueAudio_.load(voice.audio);dialogueAudio_.play();}
            catch(const std::exception& e){dialogueAudioFailed_=true;dialogueMessages_.push_back(e.what());}
        }
        bool animatedCamera=false;
        if(dialogueRuntime_.dialogue().conversationType.value_or(0)!=1&&
           dialogueAssets_.cameraModel&&line.camera.animation&&
           static_cast<std::uint16_t>(*line.camera.animation-1000u)<=727u) {
            const auto name=dialogueCameraAnimationName(*line.camera.animation);
            if(!name.empty()) {
                dialogueCameraClip_=name;
                dialogueCameraAnimation_.play(name,1.0f,
                    dialogueCameraAnimationLoops(*line.camera.animation)?2u:3u,0.0f);
                dialogueCameraReferences_.synchronize(dialogueCameraAnimation_.pose(),false);
                dialogueCameraScene_.synchronize(dialogueCameraAnimation_.pose());
                animatedCamera=dialogueCameraScene_.activateEngineHook();
                if(line.camera.fieldOfView&&*line.camera.fieldOfView>0)dialogueCameraFov_=*line.camera.fieldOfView;
            }
        }
        if(!animatedCamera&&dialogueRuntime_.dialogue().conversationType.value_or(0)!=1)
            prepareDialogueParticipants(asset,cameraAngle);
        if(animatedCamera) {
            dialogueCameraActive_=true;dialogueActorCameraActive_=false;
            bindExternalCameraControl();
        }
        else if(cameraAngle==6u&&line.camera.id) {
            if(cameraScene_.activateAreaCameraId(*line.camera.id)) {
                applyDialogueVideoEffect(dialogueRuntime_.dialogue().conversationType.value_or(0)==1
                    ?DialogueVideoCaller::ComputerShot:DialogueVideoCaller::PlaceableShot,
                    line.camera.videoEffect.value_or(-1));
                dialogueActorCamera_.setPlaceableCamera(*cameraScene_.activeAreaCamera());
                dialogueCameraActive_=false;dialogueActorCameraActive_=false;
            }
        } else if(dialogueRuntime_.dialogue().conversationType.value_or(0)!=1&&
                  cameraAngle>=1u&&cameraAngle<=4u&&asset.speakerBound&&asset.listenerBound) {
            applyDialogueVideoEffect(DialogueVideoCaller::ActorShot,line.camera.videoEffect.value_or(-1));
            DialogueCameraOptions options;options.game=document_->model().game;
            options.oldHitCheck=dialogueRuntime_.dialogue().oldHitCheck.value_or(0)!=0;
            options.heightOffset=line.camera.heightOffset.value_or(0.0f);
            options.targetHeightOffset=line.camera.targetHeightOffset.value_or(0.0f);
            options.pullback=dialogueActorCamera_.selectPullback(static_cast<DialogueActorShot>(cameraAngle),asset.cameraPullback,asset.speakerAppearance565,document_->model().game);
            if(dialogueActorCamera_.setShot(static_cast<DialogueActorShot>(cameraAngle),
                dialogueCameraActor(asset.speaker,asset.speakerHeight),dialogueCameraActor(asset.listener,asset.listenerHeight),options,sceneQueries_))dialogueShotActors_=&asset;
            dialogueActorCameraActive_=true;dialogueCameraActive_=false;
        }
        ++sceneEffects_.historyEpoch;
    }
}

model::EngineFrame ModelCanvas::dialogueLogicalFrame(std::optional<std::size_t> owner) const {
    for(const auto& active:dialogueActorAnimations_)if(active.source->owner==owner&&active.logicalFrame)
        return *active.logicalFrame;
    return owner?areaScene_.object(*owner)->references().ownerFrame():referenceScene_.ownerFrame();
}
bool ModelCanvas::lookAtDialogueParticipant(std::optional<std::size_t> source,
        std::optional<std::optional<std::size_t>> target,float distance) {
    const auto& actor=dialogueActor(source);
    if(!actor.creature)return false;
    auto* object=source?areaScene_.object(*source):nullptr;
    if(source&&!object)return false;
    auto* targetObject=target&&*target?areaScene_.object(**target):nullptr;
    const auto targetId=target?std::optional<HitOwnerId>{targetObject?targetObject->ownerId():0u}:std::nullopt;
    const bool targetPresent=target&&(target->has_value()?targetObject!=nullptr:document_->hasModelOwner());
    const auto outcome=dialogueLookAt(dialogueParticipantStates_[source],actor.participant,actor.headTracking,
        object!=nullptr||(!source&&document_->hasModelOwner()),targetId,targetPresent,
        dialogueLogicalFrame(source).position,
        targetPresent?dialogueLogicalFrame(*target).position:model::Vec3{},distance,
        document_->model().game);
    if(outcome.operation==DialogueLookAtOperation::End) {
        if(object)object->endCreatureLookAt();
        else {referenceScene_.endCreatureHeadLookAt();dialogueMainLookAt_.end();}
    } else if(outcome.operation==DialogueLookAtOperation::Begin) {
        const auto targetOwner=*target;
        const auto* targetIdentity=targetObject?targetObject->document().get():document_;
        const auto name=resourceKey(targetObject&&targetObject->kind()==AreaObjectKind::Door
            ?std::string("CAMERAHOOK"):targetIdentity->model().name);
        const auto& targetNodes=targetIdentity->model().nodes;
        const auto hook=std::find_if(targetNodes.begin(),targetNodes.end(),[&](const auto& n){return resourceKey(n.name)==name;});
        const auto node=hook==targetNodes.end()?std::optional<std::size_t>{}
            :std::optional<std::size_t>{std::size_t(hook-targetNodes.begin())};
        LookAtTargetResolver resolve=[this,targetOwner,targetIdentity,node]() -> std::optional<LookAtTargetFrame> {
            const auto* targetObject=targetOwner?areaScene_.object(*targetOwner):nullptr;
            // Engine SafePointer nulls after target destruction.
            if(targetOwner&&!targetObject)return {};
            if(!targetOwner&&!document_->hasModelOwner())return {};
            const auto& targetDocument=targetObject?*targetObject->document():*document_;
            if(&targetDocument!=targetIdentity||!node)return {};
            const auto& targetPose=targetObject?targetObject->pose():*pose_;
            const auto& targetFrame=targetObject?targetObject->frame():referenceScene_.mainFrame();
            const auto part=model::enginePartWorld(targetDocument.model(),targetPose,targetFrame,*node);
            // Gob vtable +0x10 is GetModelName, not the unique instance name.
            return LookAtTargetFrame{part,targetFrame.position};
        };
        const auto& head=actor.headTracking;
        if(object)object->beginCreatureLookAt(head.bone,std::move(resolve),head.horizontal,head.vertical,.25f,distance);
        else {
            referenceScene_.beginCreatureHeadLookAt(head.bone,resolve,head.horizontal,head.vertical,.25f,distance);
            dialogueMainLookAt_.begin(animationRuntime_,document_->model(),head.bone,std::move(resolve),
                head.horizontal,head.vertical,.25f,distance);
        }
    }
    return outcome.success;
}
void ModelCanvas::orientDialogueParticipant(std::optional<std::size_t> source,std::optional<std::size_t> target) {
    const auto& actor=dialogueActor(source);
    if(!actor.creature||actor.participant.preventOrientation||!(actor.participant.walkRate>0.0f))return;
    if((source&&!areaScene_.object(*source))||(target&&!areaScene_.object(*target)))return;
    auto& state=dialogueParticipantStates_[source];
    const auto frame=dialogueLogicalFrame(source);
    const auto current=state.direction.value_or(engineRotate(frame.orientation,{0,1,0}));
    const bool followed=!actor.participant.preventHeadFollow&&
        lookAtDialogueParticipant(source,std::optional<std::optional<std::size_t>>{std::in_place,target},10.0f);
    const auto facing=dialogueParticipantFacing(current,frame.position,
        dialogueLogicalFrame(target).position,actor.headTracking.horizontal,followed,
        document_->model().game);
    state.direction=facing.direction;
    const model::EngineFrame turned{frame.position,facing.orientation,frame.scale};
    for(auto& active:dialogueActorAnimations_)if(active.source->owner==source&&active.logicalFrame) {
        // FireForget's out-of-place rendering suppresses the engine Gob update.
        active.logicalFrame=turned;return;
    }
    if(source)areaScene_.object(*source)->setFrame(turned);else referenceScene_.setOwnerFrame(turned,false);
}
void ModelCanvas::clearDialogueParticipants() {
    if(dialogueShotParticipants_) {
        if(dialogueShotParticipants_->speakerBound)lookAtDialogueParticipant(dialogueShotParticipants_->speaker,{},0.0f);
        if(dialogueShotParticipants_->listenerBound)lookAtDialogueParticipant(dialogueShotParticipants_->listener,{},0.0f);
    }
    dialogueShotParticipants_=nullptr;
}
void ModelCanvas::prepareDialogueParticipants(const DialogueLineAsset& next,std::uint32_t angle) {
    const auto* selected=angle==5u&&dialogueShotParticipants_&&dialogueShotParticipants_->speakerBound
        ?dialogueShotParticipants_:&next;
    clearDialogueParticipants();
    dialogueShotParticipants_=selected;
    if(selected->speakerBound&&selected->listenerBound) {
        orientDialogueParticipant(selected->listener,selected->speaker);
        orientDialogueParticipant(selected->speaker,selected->listener);
    }
    synchronizeEngineQueries();
}

DialogueCameraActor ModelCanvas::dialogueCameraActor(std::optional<std::size_t> owner,float height) const {
    const auto* area=owner?areaScene_.object(*owner):nullptr;
    const auto& document=area?*area->document():*document_;
    const auto& pose=area?area->pose():*pose_;
    const auto& frame=area?area->frame():referenceScene_.mainFrame();
    DialogueCameraActor actor;actor.object=area?area->ownerId():0u;
    actor.position=frame.position;
    float logicalZ=actor.position.z;
    for(const auto& active:dialogueActorAnimations_)if(active.source->owner==owner&&active.logicalFrame)
        logicalZ=active.logicalFrame->position.z;
    actor.cameraHeight=area&&area->kind()!=AreaObjectKind::Creature?1.5f:
        dialogueCreatureHeadHeight(&document.model(),pose,frame,logicalZ,height);
    actor.cameraHook=dialogueCreatureCameraHook(document.model(),pose,frame);
    actor.ignoredOwners.push_back(actor.object);
    for(std::size_t i=0;i<document.references().size();++i)if(document.references()[i].attachmentKind==VisualAttachmentKind::CreatureHead)
        actor.ignoredOwners.push_back(area?area->ownerId(i):i+1u);
    return actor;
}
void ModelCanvas::updateDialogueActorCamera() {
    if(!dialogueActorCameraActive_||!dialogueShotActors_)return;
    const auto& asset=*dialogueShotActors_;
    dialogueActorCamera_.update(dialogueCameraActor(asset.speaker,asset.speakerHeight),
        dialogueCameraActor(asset.listener,asset.listenerHeight),sceneQueries_);
}
void ModelCanvas::advanceDialogue() {
    dialogueRuntime_.setWorldTime(worldClock_.worldTime(),worldClock_.dayMilliseconds());
    if(!dialogueRuntime_.running())return;
    if(const auto error=dialogueAudio_.pollError();!error.empty()) {
        dialogueAudioFailed_=true;dialogueMessages_.push_back(error);
    }
    DialoguePlaybackFeedback feedback;
    feedback.audioPlaying=dialogueAudio_.playing();
    feedback.audioStarted=dialogueAudio_.hasStartedStreaming();
    feedback.audioFailed=dialogueAudioFailed_;
    feedback.cameraPlaying=dialogueCameraActive_&&std::any_of(dialogueCameraAnimation_.runs().begin(),
        dialogueCameraAnimation_.runs().end(),[&](const auto& run){return run.name==dialogueCameraClip_;});
    feedback.fading=dialogueFade_.fading();
    // K1 returns true on the first creature playing a dialog animation;
    // K2 returns false on the first creature that is not. Noncreatures do
    // not participate, including in each game's empty-list result.
    const bool k1=document_->model().game==model::GameVersion::Kotor1;
    feedback.animationPlaying=!k1;
    const auto& lineAsset=dialogueAssets_.lines.at(*dialogueRuntime_.activeLineIndex());
    for(const auto& participant:lineAsset.animations) {
        if(participant.command.family!=DialogueAnimationFamily::Creature)continue;
        const auto found=std::find_if(dialogueActorAnimations_.begin(),dialogueActorAnimations_.end(),
            [&](const auto& active){return active.source->owner==participant.owner;});
        const bool playing=found!=dialogueActorAnimations_.end()&&
            dialogueAnimationIsPlaying(found->source->command,found->currentDialog);
        if(k1&&playing){feedback.animationPlaying=true;break;}
        if(!k1&&!playing){feedback.animationPlaying=false;break;}
    }
    dialogueRuntime_.advance(feedback);dispatchDialogueEvents();
}

void ModelCanvas::restoreDialogueCamera() {
    if(!dialogueSavedCamera_)return;
    cameraScene_.deactivate();
    if(dialoguePreviousCamera_)cameraScene_.activate(*dialoguePreviousCamera_);
    else if(dialoguePreviousAreaCamera_)cameraScene_.activateAreaCamera(*dialoguePreviousAreaCamera_);
    if(cameraScene_.activeAttachment())bindExternalCameraControl();
    dialogueSavedCamera_=false;dialoguePreviousCamera_.reset();dialoguePreviousAreaCamera_.reset();
}

const PreparedDialogueActor& ModelCanvas::dialogueActor(std::optional<std::size_t> owner) const {
    return *std::find_if(dialogueAssets_.actors.begin(),dialogueAssets_.actors.end(),
        [&](const auto& actor){return actor.owner==owner;});
}

void ModelCanvas::setDialogueInCutscene(std::optional<std::size_t> owner,bool enabled) {
    auto* object=owner?areaScene_.object(*owner):nullptr;
    auto& primary=object?object->animationOwner():mainAnimationOwner_;
    auto& references=object?object->references():referenceScene_;
    const auto& document=object?*object->document():*document_;
    primary.setInCutscene(enabled);
    // CSWCAnimBaseHeadWield::SetInCutscene forwards to body and head. Equipped
    // items and ordinary model references do not inherit this flag (K2 Desktop
    // 0x149774, K1 Desktop 0x5d852; the base wrapper addresses only its main Gob).
    for(std::size_t index=0;index<document.references().size();++index)
        if(document.references()[index].attachmentKind==VisualAttachmentKind::CreatureHead)
            references.animationOwner(index).setInCutscene(enabled);
}

void ModelCanvas::setDialogueAlwaysRendering(std::optional<std::size_t> owner,bool enabled) {
    auto* object=owner?areaScene_.object(*owner):nullptr;
    auto& primary=object?object->animationOwner():mainAnimationOwner_;
    auto& references=object?object->references():referenceScene_;
    const auto& document=object?*object->document():*document_;
    primary.setAlwaysRendering(enabled);
    // The HeadWield override also forwards AlwaysRender to its head Gob
    // (K2 Desktop 0x1496e2, K1 Desktop 0x5d7cc), independently of equipment Gobs.
    for(std::size_t index=0;index<document.references().size();++index)
        if(document.references()[index].attachmentKind==VisualAttachmentKind::CreatureHead)
            references.animationOwner(index).setAlwaysRendering(enabled);
}

void ModelCanvas::playDialogueAnimation(const DialogueActorAnimation& animation,bool saveLoop,bool talkReset) {
    auto active=std::find_if(dialogueActorAnimations_.begin(),dialogueActorAnimations_.end(),
        [&](const auto& value){return value.source->owner==animation.owner;});
    if(active==dialogueActorAnimations_.end()) {
        dialogueActorAnimations_.push_back({});active=std::prev(dialogueActorAnimations_.end());
    }
    const auto& command=animation.command;
    auto* object=animation.owner?areaScene_.object(*animation.owner):nullptr;
    auto flags=command.runFlags;
    if(active->logicalFrame) {
        if(object)object->setFrame(*active->logicalFrame);else referenceScene_.setOwnerFrame(*active->logicalFrame,false);
        active->logicalFrame.reset();flags|=model::kAnimationRunImmediate;
        setDialogueAlwaysRendering(animation.owner,false);
        setDialogueInCutscene(animation.owner,false);
    }
    if(command.playOutOfPlace) {
        active->logicalFrame=object?object->references().ownerFrame():referenceScene_.ownerFrame();
        const model::EngineFrame identity{{},{},active->logicalFrame->scale};
        if(object)object->setFrame(identity);else referenceScene_.setOwnerFrame(identity,false);
        // The out-of-place producer changes all three inputs together:
        // identity Gob frame, AlwaysRender, InCutscene (K2 Desktop 0x27cfa9..d028).
        setDialogueAlwaysRendering(animation.owner,true);
        setDialogueInCutscene(animation.owner,true);
    }
    bool stoppedTalk=false;
    if(saveLoop&&active->loopingSource) {
        const auto& old=active->loopingSource->command;
        // CSWCObject::SetAnimation stops these retained talk overlays. Its
        // reset argument1 then changes numeric identity without playing pause.
        const auto id=old.serverAnimation;
        if(id==10030u||(id>=10038u&&id<=10042u)||id==10154u||id==10155u) {
            if(!old.suppressed) {
                if(object)object->playOverlay(old.name,1.0f,model::kAnimationRunForceStop);
                else {
                    const auto stopped=animationRuntime_.play(old.name,1.0f,model::kAnimationRunForceStop);
                    if(stopped)referenceScene_.playCreatureHeadAnimation(old.name,1.0f,model::kAnimationRunForceStop,0.0f);
                }
            }
            stoppedTalk=true;
        }
    }
    active->run.reset();
    if(!command.suppressed&&!(talkReset&&stoppedTalk)) {
        if(!object&&!(flags&model::kAnimationRunOverlay))
            referenceScene_.playCreatureEquippedAnimation(command.name,command.playbackRate,flags,0.0f);
        const auto played=object?object->play(command.name,command.playbackRate,flags):
            animationRuntime_.play(command.name,command.playbackRate,flags);
        if(played&&!object)referenceScene_.playCreatureHeadAnimation(command.name,command.playbackRate,flags,0.0f);
        active->run=played.runId;
        if(!played&&!played.diagnostic.empty())dialogueMessages_.push_back(played.diagnostic);
    }
    active->source=&animation;active->currentDialog=command.dialog;
    active->remainingMilliseconds=command.fireForgetMilliseconds;
    if(saveLoop)active->loopingSource=&animation;
    if(!object) {pose_=animationRuntime_.pose();referenceScene_.synchronize(*pose_,false);}
}

void ModelCanvas::resetDialogueActor(std::optional<std::size_t> owner,bool talkReset) {
    const auto& actor=dialogueActor(owner);const auto family=actor.pause.command.family;
    if(family==DialogueAnimationFamily::Creature&&!actor.canPlay)return;
    if(!talkReset&&family!=DialogueAnimationFamily::Creature&&family!=DialogueAnimationFamily::Placeable)return;
    bool play=!(talkReset&&family==DialogueAnimationFamily::Door&&document_->model().game==model::GameVersion::Kotor2);
    if(talkReset||family==DialogueAnimationFamily::Creature) {
        const auto active=std::find_if(dialogueActorAnimations_.begin(),dialogueActorAnimations_.end(),
            [&](const auto& value){return value.source->owner==owner;});
        if(active!=dialogueActorAnimations_.end()&&active->source->command.serverAnimation==10000u)play=false;
    }
    if(play)playDialogueAnimation(talkReset?actor.pause:actor.listReset,true,
        talkReset||family==DialogueAnimationFamily::Creature);
    // ResetTalk/ResetDialog clear this independently of the pause command,
    // including an already-paused actor and K2's door branch.
    setDialogueInCutscene(owner,false);
}

void ModelCanvas::setDialogueAnimations(const DialogueLineAsset& next) {
    const auto contains=[](const DialogueLineAsset& line,std::optional<std::size_t> owner) {
        return std::any_of(line.animations.begin(),line.animations.end(),
            [&](const auto& row){return row.owner==owner;});
    };
    const auto abort=[&](ActiveDialogueAnimation& active) {
        // AbortFireForgetAnimation(0): zero the object counter, then replay
        // its retained loop. It does not stop the old overlay directly.
        active.remainingMilliseconds=0;
        playDialogueAnimation(*active.loopingSource,true);
    };
    const auto pause=[&](const PreparedDialogueActor& actor,bool abortFirst) {
        const auto active=std::find_if(dialogueActorAnimations_.begin(),dialogueActorAnimations_.end(),
            [&](const auto& value){return value.source->owner==actor.owner;});
        if(active!=dialogueActorAnimations_.end()&&active->source->command.serverAnimation==10000u)return;
        if(abortFirst&&active!=dialogueActorAnimations_.end())abort(*active);
        // Test currentID before Abort: replaying the loop does not suppress
        // the caller's subsequent pause reset.
        playDialogueAnimation(actor.pause,true,actor.pause.command.family==DialogueAnimationFamily::Creature);
    };
    if(dialoguePreviousActors_) {
        const auto& previous=*dialoguePreviousActors_;
        // Previous AnimList order first. Any current row for that owner
        // suppresses removal, regardless of its type, predicate or ID.
        for(const auto& row:previous.animations)if(!contains(next,row.owner))resetDialogueActor(row.owner,false);
        if(previous.listenerBound&&!contains(previous,previous.listener))resetDialogueActor(previous.listener,true);
        if(previous.speakerBound&&!contains(previous,previous.speaker))resetDialogueActor(previous.speaker,true);
    }
    for(const auto& row:next.animations) {
        if(dialoguePreviousActors_) {
            const auto& previous=dialoguePreviousActors_->animations;
            const auto old=std::find_if(previous.begin(),previous.end(),[&](const auto& value){return value.owner==row.owner;});
            if(old!=previous.end()&&old->command.serverAnimation==row.command.serverAnimation)continue;
        }
        const auto& actor=dialogueActor(row.owner);const auto family=row.command.family;
        if(family!=DialogueAnimationFamily::Creature&&family!=DialogueAnimationFamily::Placeable)continue;
        if(family==DialogueAnimationFamily::Creature&&!actor.canPlay)continue;
        if(!row.command.dialog)continue;
        // Engine checks Looping and FireForget independently, in that order.
        if(row.command.looping) {
            setDialogueInCutscene(row.owner,true);
            pause(actor,family==DialogueAnimationFamily::Creature&&row.command.dialogOverlay);
            playDialogueAnimation(row,true);
        }
        if(row.command.fireForget) {
            setDialogueInCutscene(row.owner,true);
            pause(actor,false);
            if(static_cast<std::uint16_t>(row.command.serverAnimation-1000u)<328u) {
                const auto active=std::find_if(dialogueActorAnimations_.begin(),dialogueActorAnimations_.end(),
                    [&](const auto& value){return value.source->owner==row.owner;});
                abort(*active);
            }
            playDialogueAnimation(row,false);
        }
    }
    const auto automatic=[&](bool bound,std::optional<std::size_t> owner,bool speaker) {
        if(!bound||contains(next,owner))return;
        const auto& actor=dialogueActor(owner);
        if(actor.pause.command.family!=DialogueAnimationFamily::Creature||!actor.canPlay)return;
        setDialogueInCutscene(owner,true);
        pause(actor,true);
        playDialogueAnimation(speaker?actor.talk:actor.listen,true);
    };
    automatic(next.listenerBound,next.listener,false);
    automatic(next.speakerBound,next.speaker,true);
    dialoguePreviousActors_=&next;
}

void ModelCanvas::finishDialogueActors() {
    if(dialoguePreviousActors_) {
        // CloseDialog: ResetDialogAnimations, then ResetTalkAnimations.
        // Talk reset order differs from the per-node previous-actor order.
        for(const auto& row:dialoguePreviousActors_->animations)resetDialogueActor(row.owner,false);
        if(dialoguePreviousActors_->speakerBound)resetDialogueActor(dialoguePreviousActors_->speaker,true);
        if(dialoguePreviousActors_->listenerBound)resetDialogueActor(dialoguePreviousActors_->listener,true);
    }
    dialogueActorAnimations_.clear();dialoguePreviousActors_=nullptr;dialogueShotActors_=nullptr;
}

void ModelCanvas::restoreCompletedDialogueAnimations(std::uint32_t elapsedMilliseconds) {
    // The loop pointer is a prepared command, independent of render-run
    // success or lifetime. Engine expiration restores this numeric identity.
    for(std::size_t i=0;i<dialogueActorAnimations_.size();++i) {
        auto& active=dialogueActorAnimations_[i];
        if(!active.currentDialog||!active.source->command.fireForget)continue;
        const auto owner=active.source->owner?static_cast<HitOwnerId>(*active.source->owner):mainLightOwner;
        const auto delta=active.source->command.family==DialogueAnimationFamily::Creature
            ?worldClock_.creatureDeltaMilliseconds(worldClockOwnerKeys_.at(owner)):elapsedMilliseconds;
        if(!advanceDialogueAnimationCounter(active.remainingMilliseconds,delta))continue;
        const auto& command=active.source->command;
        if(command.family==DialogueAnimationFamily::Creature&&command.engineOverlay&&!command.suppressed) {
            const auto owner=active.source->owner;const float rate=command.serverAnimation==373u?-1.0f:1.0f;
            if(owner)areaScene_.object(*owner)->playOverlay(command.name,rate,command.overlayStopFlags);
            else {
                const auto stopped=animationRuntime_.play(command.name,rate,command.overlayStopFlags);
                if(stopped)referenceScene_.playCreatureHeadAnimation(command.name,rate,command.overlayStopFlags,0.0f);
            }
        }
        playDialogueAnimation(*active.loopingSource,true);
    }
}

void ModelCanvas::initializeAreaObject(AreaObject& object) {
    object.particles().setSettings(options_.particleSettings);
    object.particles().setChunkLightsaberSettings({options_.lightsaberSettings.renderPieces,
                                                 options_.lightsaberSettings.renderFrontHalf});
    if(emitterPrewarmTrace_.completed)object.particles().finishPrewarm();
    object.lightsabers().setSettings({options_.lightsaberSettings.renderPieces,options_.lightsaberSettings.renderFrontHalf});
}
void ModelCanvas::configureWeather(PreparedWeatherPlayback prepared,WeatherFrameInput input,bool viewerCamera,
                                  std::optional<std::uint8_t> command,bool viewerPosition) {
    if(input.enabled&&areaScene_.game()==model::GameVersion::Kotor2) {
        mergeSceneAudioAssets(std::move(prepared.thunder));
        if(input.thunderEnabled)ensureSceneAudioProvider();
    }
    areaScene_.installWeatherResources(std::move(prepared.resources));
    weatherInput_=input;
    weatherShadowAlpha_=input.shadowAlpha;weatherUseViewerCamera_=viewerCamera;
    weatherUseViewerPosition_=viewerPosition;
    if(!input.enabled) {
        areaScene_.setWeatherImmediate(0,0,[this](AreaObject& object){initializeAreaObject(object);});
    } else if(command) {
        if(*command==0)areaScene_.stopWeather();else areaScene_.startWeather(*command);
    }
    synchronizeEngineQueries();Refresh(false);
}
void ModelCanvas::advanceWeather() {
    if(!weatherInput_.enabled) {
        // Keep the area sample current while inspection weather is disabled;
        // a model-only view has no weather objects or area runtime services.
        areaScene_.resetWeatherWorldTime(worldClock_.worldTime());
        return;
    }
    auto input=weatherInput_;
    input.worldTime=worldClock_.worldTime();
    input.dayMilliseconds=worldClock_.dayMilliseconds();
    if(weatherUseViewerPosition_) {
        input.cameraFrame=sceneCameraFrame();
        input.playerPosition=input.cameraFrame->position;
    } else if(weatherUseViewerCamera_)input.cameraFrame=sceneCameraFrame();
    input.shadowAlpha=weatherShadowAlpha_;
    const auto output=areaScene_.advanceWeather(input,[this](AreaObject& object){initializeAreaObject(object);});
    if(output.shadowAlphaWrite)weatherShadowAlpha_=*output.shadowAlphaWrite;
    for(const auto& name:output.thunderResources) {
        // Thunder calls PlayOneShotSound(group 21, zero overrides), sharing
        // ordinary voice admission with scene audio (K2 Desktop 0x22ae98).
        SceneAudioPlay request;request.source=sceneAudioOneShot_++;request.resref=name;
        request.oneShot=true;request.priorityGroup=21;request.fixedVariance=0.0f;
        try {playSceneAudio(request);}
        catch(const std::exception& e){dialogueMessages_.push_back(e.what());}
    }
}

std::optional<std::size_t> ModelCanvas::applyCreatureAppearance(std::size_t instance,
        const CreatureAppearanceState& state,std::string& error) {
    stopDialogue();releaseChunkCallbacks();
    synchronizeWorldClockCreatures();
    const auto changed=areaScene_.applyCreatureAppearance(instance,state,error);
    particleScenes();
    if(!changed)return {};
    if(*changed!=instance) {
        const auto clockKey=worldClockOwnerKeys_.at(instance);
        worldClockOwnerKeys_.erase(instance);worldClockOwnerKeys_.emplace(*changed,clockKey);
        auto& object=*areaScene_.object(*changed);
        object.particles().setSettings(options_.particleSettings);
        object.particles().setChunkLightsaberSettings({options_.lightsaberSettings.renderPieces,
                                                     options_.lightsaberSettings.renderFrontHalf});
        object.lightsabers().setSettings({options_.lightsaberSettings.renderPieces,
                                        options_.lightsaberSettings.renderFrontHalf});
        // Reuse the existing deferred context-resource reset when model
        // ownership changes; material-only appearance updates retain clocks.
        pendingGpuReset_=true;
    }
    synchronizeEngineQueries();synchronizeShadowCasters();updateSceneBounds();
    shaderErrorReported_=false;Refresh(false);return changed;
}

bool ModelCanvas::loadLipSync(std::optional<std::size_t> instance,
        const std::filesystem::path& path,float lipDelay,std::string& error) {
    try {
        const auto bytes=neoshared::texture::readFileBytes(path);
        const auto lip=neoshared::lip::readLip(bytes,path.string());
        const auto game=instance?areaScene_.object(*instance)->document()->model().game:document_->model().game;
        const auto visemes=creatureVisemes(lip,lipDelay,game);
        if(!visemes) {
            error="The LIP does not satisfy the engine first/final key conditions.";
            return false;
        }
        if(instance) areaScene_.object(*instance)->setVisemes(*visemes);
        else {
            stopCreatureVisemes(animationRuntime_);
            referenceScene_.stopCreatureHeadVisemes();
            playCreatureVisemes(animationRuntime_,*visemes);
            referenceScene_.setCreatureHeadVisemes(*visemes);
            pose_=animationRuntime_.pose();referenceScene_.synchronize(*pose_,false);
        }
        synchronizeEngineQueries();Refresh(false);error.clear();return true;
    } catch(const std::exception& failure) {error=failure.what();return false;}
}

void ModelCanvas::stopLipSync(std::optional<std::size_t> instance) {
    if(instance) areaScene_.object(*instance)->stopVisemes();
    else {
        stopCreatureVisemes(animationRuntime_);referenceScene_.stopCreatureHeadVisemes();
        pose_=animationRuntime_.pose();referenceScene_.synchronize(*pose_,false);
    }
    Refresh(false);
}

const OwnerVisualEffects& ModelCanvas::ownerVisualEffects(std::size_t owner) const {
    if(owner>=ownerEffects_.size()) throw std::out_of_range("Unknown visual-effect owner");
    return ownerEffects_[owner];
}
bool ModelCanvas::setOwnerVisualEffects(std::size_t owner,const OwnerVisualEffects& effects,
                                      std::string& error) {
    if(!document_ || owner>=ownerEffects_.size()) {error="Unknown visual-effect owner";return false;}
    const auto& model=owner==0 ? document_->model() : document_->references()[owner-1].model;
    if(model::engineSceneStageOrder(model.game).empty()) {
        error="Owner effects require an implemented engine scene";return false;
    }
    if(!render::validOwnerVisualEffects(effects,error)) return false;
    const auto validTexture=[&](const std::string& key) {
        const auto* data=document_->texture(key);
        const auto meta=document_->textureMetadata(key);
        if(!data || !data->hasPixels() || !meta || meta->facts.cubeMap)return false;
        try {
            const auto def=material::procedureDefinition(meta->txi,engineMaterialGame(model.game));
            if(!def.type)return !meta->facts.animated;
            using P=neoshared::material::ProcedureType;
            return *def.type!=P::Dirty && *def.type!=P::Dirty2 && *def.type!=P::Dirty3 &&
                *def.type!=P::Life && (*def.type!=P::Arturo || def.distort);
        } catch(const std::exception&) {return false;}
    };
    if(model.game==model::GameVersion::Kotor2 && effects.hologram && !validTexture("Holotex")) {
        error="Holograms require a resolved 2D Holotex with a defined engine texture/controller path";return false;
    }
    if(effects.shellAmount!=0.0f && (!validTexture(effects.shellTexture) ||
       !document_->runtimeTextureTxiAvailable(effects.shellTexture))) {
        error="Shells require a resolved 2D texture, TXI and a defined engine texture/controller path";return false;
    }
    if(engineOwnerBucket(model,effects)==EngineOwnerBucket::Distortion &&
       (model.game==model::GameVersion::Kotor2 ||
        sceneEffects_.enabled)) {
        const auto key=neoshared::material::textureResourceKey(sceneEffects_.forceDistortionTexture);
        if(!sceneEffects_.enabled || !validTexture(key)) {
            error="Distortion owners require Engine Scene Effects and a resolved 2D force-distortion texture";return false;
        }
    }
    ownerEffects_[owner]=effects;
    synchronizeEngineQueries();
    // A runtime TGA may have acquired its explicit TXI after an earlier upload.
    // Recreate samplers/textures with the current metadata in the owning context.
    pendingGpuReset_=true;
    shaderErrorReported_=false;
    Refresh(false);
    return true;
}

material::ForceSightSettings ModelCanvas::effectiveForceSight() const noexcept {
    material::ForceSightSettings state;
    state.desaturation=sceneEffects_.forceSightDesaturation;
    state.elapsedMilliseconds=forceSightMilliseconds_;
    if(document_&&document_->model().game==model::GameVersion::Kotor2&&sceneEffects_.forceSight) {
        const auto hardware=material::selectK2HardwareCapabilities(options_.engineHardware);
        state.enabled=hardware.frameBufferModifications||hardware.atiFrameBufferModifications;
    }
    return state;
}

bool ModelCanvas::setSceneEffects(SceneEffectsSettings value,std::string& error,bool resetHistory) {
    error.clear();
    if(!document_||model::engineSceneStageOrder(document_->model().game).empty()) {
        error="Scene effects require an implemented engine scene";return false;
    }
    const auto game=document_->model().game;
    value.desktopArbShaders=material::selectK2HardwareCapabilities(options_.engineHardware).arbFragmentPrograms;
    if(!validateSceneEffects(value,error,game))return false;
    bool producers=false;
    for(std::size_t owner=0;owner<ownerEffects_.size();++owner) {
        const auto& asset=owner ? document_->references()[owner-1u].model : document_->model();
        producers=producers || engineOwnerBucket(asset,ownerEffects_[owner])==EngineOwnerBucket::Distortion;
    }
    if(game==model::GameVersion::Kotor2 && producers&&!value.enabled) {error="Disable owner distortion before disabling the scene-effect graph";return false;}
    for(const auto role:requiredSceneEffectTextures(value,producers,game)) {
        const auto key=neoshared::material::textureResourceKey(sceneEffectTextureName(value,role));
        const auto* image=document_->texture(key);const auto meta=document_->textureMetadata(key);
        if(!image||!image->hasPixels()||!meta||meta->facts.cubeMap) {
            error=std::string("Scene effects require a resolved 2D ")+sceneEffectTextureLabel(role)+" texture";return false;
        }
    }
    // Manual graph inspection is an explicit cut. AurEnable/AurDisable and
    // VideoEffects row activation retain framebuffer and procedural lifetimes.
    value.historyEpoch=sceneEffects_.historyEpoch+(resetHistory?1u:0u);
    sceneEffects_=std::move(value);
    if(resetHistory)pendingGpuReset_=true;
    shaderErrorReported_=false;Refresh(false);return true;
}

float ModelCanvas::textureAngle(std::size_t owner,std::size_t node) const noexcept {
    const auto found=textureAngles_.find({owner,node});
    return found==textureAngles_.end() ? 0.0f : found->second;
}
bool ModelCanvas::setTextureAngle(std::size_t owner,std::size_t node,float degrees,
                                 std::string& error) {
    error.clear();
    if(!document_ || owner>document_->references().size() || !std::isfinite(degrees)) {
        error="Invalid runtime texture-rotation owner or angle";return false;
    }
    const auto& asset=owner ? document_->references()[owner-1].model : document_->model();
    if(model::engineSceneStageOrder(asset.game).empty() ||
       std::none_of(asset.meshes.begin(),asset.meshes.end(),
           [node](const auto& mesh){return mesh.nodeIndex==node;})) {
        error="Runtime texture rotation requires a mesh part with a supported engine renderer";return false;
    }
    if(degrees==0)textureAngles_.erase({owner,node});
    else textureAngles_[{owner,node}]=degrees;
    shaderErrorReported_=false;Refresh(false);return true;
}

void ModelCanvas::refreshMaterials() {
    pendingGpuReset_ = true;
    shaderErrorReported_ = false;
    Refresh(false);
}

void ModelCanvas::setDocument(const ModelDocument* document, bool preserveCamera) {
    selectedAreaOwners_.clear();
    selectionStart_.reset();
    meshPick_.reset();
    if (preserveCamera && authoredCameraActive()) useViewerCamera(false);
    stopDialogue();
    clearWorldClock();
    dialogueMainLookAt_.clear();dialogueParticipantStates_.clear();
    clearSceneAudio();
    scriptVideoEffect_=false;
    pickingCamera_.reset();
    worldPlaybackSettings_={};
    pendingDoorElapsed_=0;
    releaseChunkCallbacks();
    weatherInput_.enabled=false;
    explicitDirtyArea_.reset();explicitDirtyIdentity_.clear();++dirtyAreaEpoch_;
    disableDirtyMaterial();
    shadowScene_.clear(lightScene_);motionBlurScene_.clear();motionBlurQueue_.clear();
    particleScene_.setSceneQueries(nullptr);areaScene_.setQueries(nullptr);sceneQueries_.clear();sceneQueryError_.clear();
    pendingFlareElapsed_=0;pendingSceneElapsed_=0;pendingEmitterSceneAnimating_=false;
    emitterPrewarmTrace_={};emitterFrameTrace_={};particleError_.clear();grassError_.clear();
    areaScene_.clear(lightScene_);
    particleScene_.sceneAnimation()->setInterleaved(false);
    areaOwnerQueries_.clear();
    particleScene_.setEmitterRooms({});
    // Defer deletion until the next paint makes the owning context current.
    // Keeping the IDs until then avoids losing hidden-tab texture/buffer IDs.
    pendingGpuReset_ = true;
    document_ = document;
    ownerOrderingTransparency_.clear();
    if(document_ && document_->isOpen()) {
        ownerOrderingTransparency_.resize(document_->references().size()+1u);
        const auto constructOrdering=[this](std::size_t owner,const model::Model& asset) {
            if(asset.game==model::GameVersion::Kotor1) {
                ownerOrderingTransparency_[owner]=model::ownerTreeHasTransparency(asset,nullptr);
            }
        };
        constructOrdering(0,document_->model());
        for(std::size_t i=0;i<document_->references().size();++i)
            constructOrdering(i+1u,document_->references()[i].model);
    }
    ownerEffects_.assign(document_ ? document_->references().size()+1u : 0u,{});
    textureAngles_.clear();
    // Scene constructs its wind field before NewCAurObject constructs the
    // reference tree. Keep reference random draws after the shared reset.
    particleScene_.setReferenceScene(nullptr);
    referenceScene_.setDocument(nullptr);
    mainAnimationOwner_=SceneAnimationOwner{};
    externalCameraAnimationOwner_=SceneAnimationOwner{};
    externalCameraAnimationOwner_.configure(0,{},true);
    externalCameraAnimationOwner_.setHasModel(false);
    bindExternalCameraControl();
    particleScene_.setChunkLightsaberSettings({options_.lightsaberSettings.renderPieces,
                                             options_.lightsaberSettings.renderFrontHalf});
    particleScene_.setDanglyEnabled(options_.danglySimulation);
    particleScene_.setSettings(options_.particleSettings);
    particleScene_.setDocument(document_);
    if(document_&&document_->isOpen())externalCameraAnimationOwner_.setScene(particleScene_.sceneAnimation());
    std::vector<const neomdl::model::Model*> animationHierarchy;
    if (document_ && document_->isOpen()) {
        if(document_->hasModelOwner())
            model::processAsciiSkinSeams(document_->model(),resourceLoadJob());
        animationHierarchy.reserve(document_->modelHierarchySize());
        for (std::size_t index = 0u;
             index < document_->modelHierarchySize(); ++index) {
            animationHierarchy.push_back(&document_->modelAt(index));
        }
        animationRuntime_.setModel(&document_->model(),
                                   std::move(animationHierarchy));
    } else {
        animationRuntime_.setModel(nullptr, {});
    }
    animationRuntime_.setEventCallback([this](const model::AnimationRuntimeEvent& event) {
        if(event.hierarchyUpdated)mainAnimationOwner_.markHierarchyUpdated();
        pose_=animationRuntime_.pose();
        if(event.kind==model::AnimationRuntimeEventKind::AnimationEvent)
            particleScene_.dispatchEvent(event.name);
    },*mainAnimationOwner_.lifetime());
    referenceScene_.setSceneRandom(&particleScene_.runtimeRandom());
    referenceScene_.setSceneAnimation(particleScene_.sceneAnimation());
    referenceScene_.setParentAnimationOwner(&mainAnimationOwner_,&animationRuntime_);
    referenceScene_.setOwnerSceneMember(false);
    referenceScene_.setDocument(document_);
    cameraScene_.setReferenceScene(&referenceScene_);
    cameraScene_.setDocument(document_);
    particleScene_.setReferenceScene(&referenceScene_);
    lightsaberScene_.setReferenceScene(&referenceScene_);
    lightsaberScene_.setSettings({options_.lightsaberSettings.renderPieces,
                                  options_.lightsaberSettings.renderFrontHalf});
    lightsaberScene_.setDocument(document_);
    lightScene_.setReferenceScene(&referenceScene_);
    lightScene_.setDocument(document_);
    triggerScene_.setDocument(document_);
    triggerCullPlane_ = 0u;
    pendingLightElapsed_ = 0.0f;
    selectedNodes_.clear();hiddenNodes_.clear();isolatedNodes_.reset();
    animationRuntime_.advanceProceduralUv(particleScene_.runtimeRandom(),0.0f);
    if(document_&&document_->hasModelOwner())
        lightsaberScene_.sampleConstructedOwners(animationRuntime_.pose());
    if(document_&&document_->hasModelOwner()) {
        mainAnimationOwner_.configure(document_->model().classification,[this](float seconds,bool evaluate) {
            return advanceMainOwner(seconds,evaluate);
        });
        mainAnimationOwner_.configureSphere(document_->model(),[this] {
            const auto& asset=document_->model();
            return engineMinimumHitSphere(asset,engineHitPose(asset,&animationRuntime_.pose()),
                referenceScene_.mainFrame().scale);
        });
        mainAnimationOwner_.bindWind(&particleScene_.windManager(),[this]{return referenceScene_.mainWorld();},
            document_->model().game);
        mainAnimationOwner_.setProcedural([this](float seconds) {
            animationRuntime_.advanceProceduralUv(particleScene_.runtimeRandom(),seconds);
        });
        mainAnimationOwner_.setLookAt([this] {
            dialogueMainLookAt_.control(animationRuntime_,document_->model(),referenceScene_.mainFrame());
        });
        mainAnimationOwner_.setDynamicLights(std::any_of(document_->model().nodes.begin(),document_->model().nodes.end(),
            [](const auto& node){return node.light&&(node.light->dynamicType||node.light->affectDynamic);}));
        mainAnimationOwner_.setHasAnimationRuns([this]{return !animationRuntime_.empty();});
        mainAnimationOwner_.setPartAnimation([this](float seconds) {
            if(options_.danglySimulation) {
                model::simulateDanglyMeshes(document_->model(),*pose_,danglyState_,seconds,
                    danglyEnvironment(referenceScene_.mainFrame()));
                ++poseRevision_;
            }
            lightsaberScene_.advancePrimary(*pose_,seconds);
            lightsaberScene_.refreshTransforms(*pose_,options_.showReferences);
        });
        mainAnimationOwner_.setScene(particleScene_.sceneAnimation());
    }
    referenceScene_.setPartsAnimated([this](std::size_t index,float seconds) {
        if(options_.danglySimulation)referenceScene_.simulateDanglyInstance(index,seconds,danglyEnvironment());
        lightsaberScene_.advanceReference(*pose_,index,seconds);lightsaberScene_.refreshTransforms(*pose_);
    });
    referenceScene_.setAnimationEventCallback([this](const ReferenceAnimationEvent& event) {
        if(event.event.kind==model::AnimationRuntimeEventKind::AnimationEvent)
            particleScene_.dispatchEvent(event.event.name,event.reference);
    });
    referenceScene_.setOwnerAnimated([this](std::size_t,float) {
        referenceScene_.takeAnimationEvents();
    });
    referenceScene_.setOwnerSceneMember(document_&&document_->hasModelOwner());
    pendingAnimationRuntimeEvents_.clear();
    replayAnimationIndex_.reset();
    replayAnimationTime_ = 0.0f;
    replayAnimationLoop_ = false;
    compatibilityRunId_.reset();
    materialTime_ = 0.0;
    pose_.reset();
    danglyState_ = {};
    resetWorldClock(false);
    rebuildPose(true);
    if (!preserveCamera) frameModel();
    Refresh(false);
}

std::string ModelCanvas::engineExecutionReport() const {
    std::string text;
    if(sceneEffects_.engineVideoEffect)text="Engine VideoEffects row/state: "+std::to_string(*sceneEffects_.engineVideoEffect)+"\n";
    text+="Force Sight requested="+std::to_string(sceneEffects_.forceSight)+
        " effective="+std::to_string(effectiveForceSight().enabled)+
        " render clock="+std::to_string(forceSightMilliseconds_)+"\n";
    text+="K2 uses an embedded 15-bit RNG. K1 Desktop rand/srand provider remains unidentified.\n"
        "The default scene uses the K2 stream; original process seed/call history and platform floating-point precision remain unverified.\n";
    text+=options_.procedureMipPath==render::ProcedureMipPath::SgiAutomatic
        ?"2D mip capability: SGI automatic (driver results are not engine captures).\n"
        :"2D mip capability: GLU/Aur scaling and byte mip kernels.\n";
    text+="Portable GLU: bundled byte mipmaps and perspective matrix.\n"
        "Apple framework arithmetic has not been identified as equivalent.\n\n";
    auto hardware=options_.engineHardware;
    const auto capabilities=material::selectK2HardwareCapabilities(hardware);
    std::ostringstream graphics;
    graphics<<"Engine capability snapshot: extension bits 0x"<<std::hex<<hardware.existingExtensions<<std::dec
        <<", useatifragmentshaders="<<hardware.useAtiFragmentShaders
        <<", usearbvertexprograms="<<hardware.useArbVertexPrograms
        <<", usevertexprograms="<<options_.engineUseVertexPrograms
        <<", renderlevel="<<hardware.renderLevel<<"\n"
        <<"ATI validation="<<capabilities.atiValidation
        <<", NV1x="<<capabilities.nv1xLevelShaders<<", NV2x="<<capabilities.nv2xLevelShaders
        <<", bump-card validation="<<capabilities.bumpMapCardValidation<<"\n";
    text+=graphics.str();
    if(!areaScene_.empty()) {
        const auto& environment=areaScene_.environment();
        const auto& sample=environment.sample();
        const auto vector=[](const std::optional<model::Vec3>& value) {
            if(!value)return std::string("unset");
            std::ostringstream out;out<<value->x<<','<<value->y<<','<<value->z;return out.str();
        };
        text+="\nArea environment / camera replay:\n";
        text+="  loaded inputs: "+std::string(options_.useLoadedAreaEnvironment?"enabled":"manual viewer overrides only")+"\n";
        text+="  phase: "+std::string(areaTimeOfDayName(sample.phase))+" transition="+
            std::to_string(sample.transition)+"\n";
        text+="  scene ambient: "+vector(sample.sceneAmbient)+"\n";
        text+="  gidy_sun ambient: "+vector(sample.globalAmbient)+" diffuse: "+vector(sample.diffuse)+"\n";
        text+="  fog: "+std::string(sample.hasFogState?(sample.fogEnabled?"enabled":"disabled"):"not authored")+
            " color="+vector(sample.fogColor)+" near="+
            (sample.fogNear?std::to_string(*sample.fogNear):std::string("unset"))+" far="+
            (sample.fogFar?std::to_string(*sample.fogFar):std::string("unset"))+"\n";
        text+="  shadows: "+std::string(sample.hasShadowState?(sample.shadowsEnabled?"enabled":"disabled"):"not authored")+
            " opacity="+std::to_string(sample.shadowOpacity)+"\n";
        text+="  ForceBrightSunAndMoon: "+std::string(sample.forceBrightSunAndMoon?"active":"inactive")+"\n";
        text+="  current room: "+(areaScene_.currentRoom()?std::to_string(*areaScene_.currentRoom()):std::string("all rooms"))+
            (areaScene_.currentRoomOverridden()?" (manual override)\n":" (replay or camera WOK query)\n");
        if(const auto* camera=environment.activeCamera()) {
            text+="  replay camera: "+std::string(options_.useAreaReplayCamera?"active":"present but disabled by viewer override")+
                " position="+std::to_string(camera->world.values[12])+','+
                std::to_string(camera->world.values[13])+','+std::to_string(camera->world.values[14])+
                " fov="+std::to_string(camera->viewAngle)+" clip="+
                std::to_string(camera->nearClip)+".."+std::to_string(camera->farClip)+"\n";
        } else text+="  replay camera: inactive\n";
        text+="  camera history epoch: "+std::to_string(environment.cameraHistoryEpoch())+"\n";
        for(const auto& entry:environment.trace())
            text+="  trace "+std::to_string(entry.sequence)+": "+entry.operation+" | "+entry.detail+
                " | "+(entry.applied?"applied":"rejected")+"\n";

        text+="\nLoaded GIT visual records:\n";
        text+="  TriggerList records: "+std::to_string(areaScene_.triggers().size())+"\n";
        for(std::size_t index=0;index<triggerScene_.records().size();++index) {
            const auto& record=triggerScene_.records()[index];
            if(record.source!=RuntimeTriggerSource::AreaGit)continue;
            text+="  trigger ["+std::to_string(index)+"] source="+
                (record.sourceIndex?std::to_string(*record.sourceIndex):std::string("unknown"))+
                " object="+std::to_string(record.objectId)+" type="+
                std::to_string(record.type)+" height="+
                std::to_string(record.authoredHighlightHeight)+" suppress="+
                std::to_string(record.clientVisualState.updateSuppressed)+
                " transition="+std::to_string(record.clientVisualState.areaTransitionMode)+
                " display="+std::to_string(record.clientVisualState.displayEnabled)+"\n";
        }
        text+="  CameraList records: "+std::to_string(cameraScene_.areaCameras().size())+"\n";
        for(std::size_t index=0;index<cameraScene_.areaCameras().size();++index) {
            const auto& camera=cameraScene_.areaCameras()[index].authored;
            text+="  camera ["+std::to_string(index)+"] id="+
                std::to_string(camera.cameraId)+" position="+
                std::to_string(camera.position.x)+','+
                std::to_string(camera.position.y)+','+
                std::to_string(camera.position.z)+" fov="+
                std::to_string(camera.fieldOfView)+" pitch="+
                std::to_string(camera.pitch)+" height="+
                std::to_string(camera.height)+" mic-range="+
                std::to_string(camera.microphoneRange);
            if(cameraScene_.activeAreaCameraIndex()&&
               *cameraScene_.activeAreaCameraIndex()==index)text+=" active";
            text+='\n';
        }
    }
    text+="Emitter UpdateEmitters prewarm: completed="+std::to_string(emitterPrewarmTrace_.completed)+
        " particles="+std::to_string(emitterPrewarmTrace_.particlesBefore)+"->"+
        std::to_string(emitterPrewarmTrace_.particlesAfter)+" RNG="+
        std::to_string(emitterPrewarmTrace_.randomBefore)+"->"+
        std::to_string(emitterPrewarmTrace_.randomAfter)+"\n";
    for(const auto& step:emitterPrewarmTrace_.order)
        text+="prewarm: scene="+std::to_string(step.sceneIndex)+" emitter="+
            std::to_string(step.emitterIndex)+" particles="+
            std::to_string(step.particlesBefore)+"->"+std::to_string(step.particlesAfter)+
            " RNG="+std::to_string(step.randomBefore)+"->"+
            std::to_string(step.randomAfter)+" completed="+
            std::to_string(step.completed)+"\n";
    text+="Emitter DoEmitterBucket: elapsed="+std::to_string(emitterFrameTrace_.requestedElapsed)+
        " Scene::IsAnimating="+std::to_string(emitterFrameTrace_.sceneAnimating)+
        " completed="+std::to_string(emitterFrameTrace_.completed)+"\n";
    for(const auto& step:emitterFrameTrace_.steps)
        text+="emitter frame: scene="+std::to_string(step.sceneIndex)+" emitter="+
            std::to_string(step.emitterIndex)+" owner-query="+
            std::to_string(step.ownerGuardQueried)+" owner-animating="+
            std::to_string(step.ownerGuard)+" AnimateParticles="+
            std::to_string(step.animateCompleted)+" dt="+
            std::to_string(step.appliedElapsed)+" particles="+
            std::to_string(step.particlesBefore)+"->"+std::to_string(step.particlesAfter)+
            " RNG="+std::to_string(step.randomBefore)+"/"+
            std::to_string(step.randomAfterAnimate)+"/"+
            std::to_string(step.randomAfterRenderFunc)+"/"+
            std::to_string(step.randomAfterRender)+" RenderFunc="+
            std::to_string(step.renderFuncCompleted)+" draw="+
            std::to_string(step.drawCallbackCompleted)+"\n";
    if(!particleError_.empty())text+="Emitter scheduler error: "+particleError_+"\n";
    for(const auto& b:areaScene_.grass().bins())if(b.built) {
        text+="Bin "+std::to_string(b.identity)+" room "+std::to_string(b.room)+"\n";
        for(const auto& group:b.groups)text+="group: first vertex="+std::to_string(group.firstVertex)+" blades="+
            std::to_string(group.bladeCount)+" Part="+std::to_string(group.part)+" original lightmap Part="+
            std::to_string(group.lightmapPart)+" texture="+group.lightmap+"\n";
        for(const auto& face:b.faceBuilds) {
            text+="face: Part="+std::to_string(face.part)+" index="+std::to_string(face.face)+" first blade="+
                std::to_string(face.firstBlade)+" count="+std::to_string(face.bladeCount)+" seed="+std::to_string(face.seed)+
                " final RNG="+std::to_string(face.randomStateAfter)+"\n";
        }
    }
    return text;
}

void ModelCanvas::setOptions(RenderOptions options) {
    if (!std::isfinite(options.objectLightRadius) || options.objectLightRadius < 0.0f)
        throw std::invalid_argument("Gob light-query radius must be finite and nonnegative");
    const bool areaCameraModeChanged=options_.useAreaReplayCamera!=options.useAreaReplayCamera;
    const bool mipChanged = options_.procedureMipPath != options.procedureMipPath;
    const bool hardwareChanged=options_.engineUseVertexPrograms!=options.engineUseVertexPrograms ||
        options_.engineHardware.existingExtensions!=options.engineHardware.existingExtensions ||
        options_.engineHardware.useAtiFragmentShaders!=options.engineHardware.useAtiFragmentShaders ||
        options_.engineHardware.useArbVertexPrograms!=options.engineHardware.useArbVertexPrograms ||
        options_.engineHardware.renderLevel!=options.engineHardware.renderLevel ||
        options_.engineHardware.unlockAtiSoftShadows!=options.engineHardware.unlockAtiSoftShadows;
    meshRenderer_.setGobNewSorting(options.gobNewSorting);
    const bool resetDangly = options_.danglySimulation != options.danglySimulation;
    const bool lightsaberGeometryChanged =
        options_.lightsaberSettings.renderPieces != options.lightsaberSettings.renderPieces ||
        options_.lightsaberSettings.renderFrontHalf != options.lightsaberSettings.renderFrontHalf;
    const bool referencesChanged = options_.showReferences != options.showReferences;
    const bool emitterPreviewChanged=options_.previewEmitters!=options.previewEmitters;
    if(emitterPreviewChanged) {
        particleScene_.resetSimulation();
        for(const auto& object:areaScene_.objects())object->particles().resetSimulation();
        pendingEmitterSceneAnimating_=false;
        emitterPrewarmTrace_={};emitterFrameTrace_={};particleError_.clear();
    }
    bool particleSettingsChanged=!(options_.particleSettings==options.particleSettings);
    if(!particleScene_.setSettings(options.particleSettings)) {
        options.particleSettings=options_.particleSettings;particleSettingsChanged=false;
    }
    if (!validCameraPreviewSettings(options.cameraSettings))
        options.cameraSettings = options_.cameraSettings;
    if (!validK2SkinPreviewSettings(options.skinSettings))
        options.skinSettings = options_.skinSettings;
    options_ = options;
    sceneEffects_.desktopArbShaders=material::selectK2HardwareCapabilities(options_.engineHardware).arbFragmentPrograms;
    if(areaCameraModeChanged)++sceneEffects_.historyEpoch;
    if (mipChanged || hardwareChanged) pendingGpuReset_=true;
    for(const auto& object:areaScene_.objects()) {
        object->particles().setSettings(options_.particleSettings);
        object->particles().setChunkLightsaberSettings({options_.lightsaberSettings.renderPieces,
                                                       options_.lightsaberSettings.renderFrontHalf});
        object->particles().setDanglyEnabled(options_.danglySimulation);
        object->lightsabers().setSettings({options_.lightsaberSettings.renderPieces,
                                         options_.lightsaberSettings.renderFrontHalf});
    }
    if(document_&&document_->hasModelOwner())
        mainAnimationOwner_.bindWind(&particleScene_.windManager(),[this]{return referenceScene_.mainWorld();},
            document_->model().game);
    particleScene_.setChunkLightsaberSettings({options_.lightsaberSettings.renderPieces,
                                             options_.lightsaberSettings.renderFrontHalf});
    particleScene_.setDanglyEnabled(options_.danglySimulation);
    if (lightsaberGeometryChanged) {
        lightsaberScene_.setSettings({options_.lightsaberSettings.renderPieces,
                                      options_.lightsaberSettings.renderFrontHalf});
    }
    if (particleSettingsChanged && options_.previewEmitters && pose_)
        resetParticleHistoryForAnimationSeek();
    if(pose_) {
        synchronizeEngineQueries();
        particleScene_.prepareFrame(*pose_,0.0,options_.showReferences);
        AreaFrameSettings emitterFrame;
        emitterFrame.animate=true;emitterFrame.dangly=options_.danglySimulation;
        emitterFrame.emitters=options_.previewEmitters;emitterFrame.references=options_.showReferences;
        areaScene_.advanceEffects(0.0f,emitterFrame);
        if(!options_.previewEmitters)discardSceneEmitterFrame(particleScenes());
    }
    if (resetDangly) {
        referenceScene_.resetDangly();
        rebuildPose(true);
    } else {
        if (pose_ && referencesChanged) {
            referenceScene_.synchronize(*pose_);
            cameraScene_.synchronize(*pose_, options_.showReferences);
            synchronizeEngineQueries();
            particleScene_.prepareFrame(*pose_,0.0,options_.showReferences);
            AreaFrameSettings emitterFrame;
            emitterFrame.animate=true;emitterFrame.dangly=options_.danglySimulation;
            emitterFrame.emitters=options_.previewEmitters;emitterFrame.references=options_.showReferences;
            areaScene_.advanceEffects(0.0f,emitterFrame);
            lightsaberScene_.synchronize(*pose_, options_.showReferences);
            lightScene_.synchronize(*pose_, options_.showReferences);
        }
        updateSceneBounds();
    }
    if (!options_.previewLights) pendingLightElapsed_ = 0.0f;
    if(!options_.engineLensFlares.value_or(!areaScene_.empty()))pendingFlareElapsed_=0;
    shaderErrorReported_=false;
    Refresh(false);
}

void ModelCanvas::setSelectedNodes(std::set<std::size_t> nodes) {
    selectedNodes_=std::move(nodes);
    Refresh(false);
}

void ModelCanvas::hideSelectedMeshes() {
    hiddenNodes_.insert(selectedNodes_.begin(),selectedNodes_.end());
    Refresh(false);
}

void ModelCanvas::isolateSelectedMeshes(bool enabled) {
    if (enabled) isolatedNodes_=selectedNodes_;
    else isolatedNodes_.reset();
    Refresh(false);
}

void ModelCanvas::showAllMeshes() {
    hiddenNodes_.clear();isolatedNodes_.reset();
    Refresh(false);
}

void ModelCanvas::setMeshVisibility(std::set<std::size_t> hidden,std::optional<std::set<std::size_t>> isolated) {
    hiddenNodes_=std::move(hidden);isolatedNodes_=std::move(isolated);
    Refresh(false);
}

void ModelCanvas::copyCameraFrom(const ModelCanvas& source) {
    yaw_=source.yaw_;pitch_=source.pitch_;distance_=source.distance_;center_=source.center_;
    freeFly_=source.freeFly_;flyPosition_=source.flyPosition_;flySpeed_=source.flySpeed_;
    cameraScene_.deactivate();resetCameraInput();
    if (const auto& attachment=source.cameraScene_.activeAttachment()) {
        synchronizeCameraScene();cameraScene_.activate(*attachment);bindExternalCameraControl();
    }
    ++sceneEffects_.historyEpoch;Refresh(false);
}

bool ModelCanvas::meshVisible(std::size_t node) const {
    return !hiddenNodes_.count(node) && (!isolatedNodes_ || isolatedNodes_->count(node));
}

std::size_t ModelCanvas::referenceSelectionNode(std::size_t reference) const {
    const auto& references=document_->references();
    while (references[reference].parentInstance) reference=*references[reference].parentInstance;
    return references[reference].parentNodeIndex;
}

bool ModelCanvas::activateCameraNode(std::size_t nodeIndex) {
    synchronizeCameraScene();
    const bool activated = cameraScene_.activateMainCameraNode(nodeIndex);
    if (activated) {bindExternalCameraControl();++sceneEffects_.historyEpoch;Refresh(false);}
    return activated;
}

bool ModelCanvas::activateCameraHook() {
    synchronizeCameraScene();
    const bool activated = cameraScene_.activateEngineHook();
    if (activated) {bindExternalCameraControl();++sceneEffects_.historyEpoch;Refresh(false);}
    return activated;
}

bool ModelCanvas::activateAreaCamera(std::size_t index) {
    const bool activated=cameraScene_.activateAreaCamera(index);
    if(activated){++sceneEffects_.historyEpoch;Refresh(false);}return activated;
}
bool ModelCanvas::activateAreaCameraId(std::int32_t cameraId) {
    const bool activated=cameraScene_.activateAreaCameraId(cameraId);
    if(activated){++sceneEffects_.historyEpoch;Refresh(false);}return activated;
}

void ModelCanvas::useViewerCamera(bool freeFly) {
    auto position = viewerCameraPosition();
    if (authoredCameraActive()) {
        const auto world = inverseAreaReplayCamera(cameraView_);
        position = {world.values[12], world.values[13], world.values[14]};
        constexpr float degrees = 180.0f / 3.14159265358979323846f;
        yaw_ = std::atan2(-world.values[8], -world.values[9]) * degrees;
        pitch_ = std::atan2(world.values[10], std::hypot(world.values[8], world.values[9])) * degrees;
    }
    freeFly_ = freeFly;
    flyPosition_ = position;
    const auto offset = cameraOffset({0.0f, 0.0f, distance_});
    center_ = {position.x - offset.x, position.y - offset.y, position.z - offset.z};
    ++sceneEffects_.historyEpoch;
    cameraScene_.deactivate();
    dialogueCameraActive_ = false;
    dialogueActorCameraActive_ = false;
    if (activeAreaReplayCamera()) options_.useAreaReplayCamera = false;
    resetCameraInput();
    Refresh(false);
}

std::size_t ModelCanvas::createTriggerVolume(
    std::vector<neomdl::model::Vec3> footprint,
    std::array<float, 4> color, std::uintptr_t owner) {
    const auto index = triggerScene_.createVolume(
        std::move(footprint), color, owner);
    updateSceneBounds();
    Refresh(false);
    return index;
}

bool ModelCanvas::removeTriggerVolume(std::size_t index) {
    if (!triggerScene_.removeVolume(index)) return false;
    updateSceneBounds();
    Refresh(false);
    return true;
}

void ModelCanvas::clearTriggerVolumes() {
    triggerScene_.clearVolumes();
    updateSceneBounds();
    Refresh(false);
}

bool ModelCanvas::setTriggerHighlighted(std::size_t index, bool highlighted,
                                        float height, bool group) {
    if (!triggerScene_.setHighlighted(index, highlighted, height, group))
        return false;
    updateSceneBounds();
    Refresh(false);
    return true;
}

bool ModelCanvas::setTriggerHighlightParameters(
    std::size_t index,
    const neomdl::model::TriggerHighlightParameters& parameters) {
    if (!triggerScene_.setHighlightParameters(index, parameters)) return false;
    Refresh(false);
    return true;
}

bool ModelCanvas::setTriggerClientVisualState(
    std::size_t index,const TriggerClientVisualState& state) {
    if(!triggerScene_.applyClientVisualState(index,state))return false;
    updateSceneBounds();Refresh(false);return true;
}

std::size_t ModelCanvas::addTriggerMine(PreparedTriggerMine prepared,PreparedSceneAudioPlayback sounds) {
    mergeSceneAudioAssets(std::move(sounds));
    const auto source=areaScene_.addTriggerMine(std::move(prepared));
    const auto& record=areaScene_.triggers().at(source);
    initializeAreaObject(*areaScene_.object(*record.runtimeInstance));
    // Type 2 mines own Gobs, not PartTrigger volumes. Do not rebuild existing
    // GIT volumes or reset their retained visual state when admitting one.
    particleScenes();synchronizeEngineQueries();synchronizeShadowCasters();
    pendingGpuReset_=true;updateSceneBounds();Refresh(false);return source;
}
TriggerMinePreviewInputs ModelCanvas::triggerMineInputs(std::size_t source) const {
    const auto found=triggerMineInputs_.find(source);
    return found==triggerMineInputs_.end()?TriggerMinePreviewInputs{}:found->second;
}
void ModelCanvas::refreshTriggerObservers() {
    for(const auto& mine:areaScene_.triggerRuntime().mines())if(!mine.serverRemoved) {
        const auto input=triggerMineInputs(mine.sourceIndex);
        areaScene_.updateTriggerMineObserver(mine.sourceIndex,input.observer,input.reputation);
    }
}
bool ModelCanvas::setTriggerMineInputs(std::size_t source,bool flagged,TriggerMinePreviewInputs input) {
    if(!areaScene_.flagTriggerMine(source,flagged))return false;
    triggerMineInputs_[source]=std::move(input);refreshTriggerObservers();consumeTriggerEvents();
    Refresh(false);return true;
}
bool ModelCanvas::receiveTrapRequest(const TriggerMineRequest& request) {
    const std::string prefix=std::string(request.targetKind==TrapObjectKind::Mine?"Mine[":
        request.targetKind==TrapObjectKind::Door?"Door[":"Placeable[")+std::to_string(request.sourceIndex)+"] ";
    switch(request.kind) {
    case TriggerMineRequest::Kind::RunScript:
        dialogueMessages_.push_back(prefix+"paused for engine script '"+request.script+
            "'. Load captured effects, then explicitly resume through Trap Runtime.");return false;
    case TriggerMineRequest::Kind::SetLastEntered:
        dialogueMessages_.push_back(prefix+"LastEntered = "+std::to_string(request.actor));break;
    case TriggerMineRequest::Kind::SetLastDisarmed:
        dialogueMessages_.push_back(prefix+"LastDisarmed = "+std::to_string(request.actor));break;
    case TriggerMineRequest::Kind::RemoveKey:
        dialogueMessages_.push_back(prefix+"removed captured key item "+std::to_string(request.item));break;
    case TriggerMineRequest::Kind::Feedback82:
        dialogueMessages_.push_back(prefix+"engine trap feedback 82 for actor "+std::to_string(request.actor));break;
    case TriggerMineRequest::Kind::ScheduleDestroy:
        dialogueMessages_.push_back(prefix+"queued engine server destruction");break;
    case TriggerMineRequest::Kind::SetDeleteReason:
        dialogueMessages_.push_back(prefix+"delete reason = "+std::to_string(request.deleteReason));break;
    case TriggerMineRequest::Kind::RecoverInventoryItem:
        dialogueMessages_.push_back(prefix+"paused for engine recovered item '"+request.resource+
            "'. Load the captured inventory after engine allocation/admission, then explicitly resume through Trap Runtime.");return false;
    case TriggerMineRequest::Kind::UnequipInventoryItem:
        dialogueMessages_.push_back("Paused for engine unequip of item "+std::to_string(request.item)+
            ". Load the resulting captured inventory and explicitly resume through Trap Runtime.");return false;
    case TriggerMineRequest::Kind::DoorOpenState:
        dialogueMessages_.push_back(prefix+"engine door open state "+std::to_string(request.event));break;
    case TriggerMineRequest::Kind::SignalEvent:
        dialogueMessages_.push_back(prefix+"engine event "+std::to_string(request.event));break;
    case TriggerMineRequest::Kind::SoundSet:
        dialogueMessages_.push_back(prefix+"engine actor sound-set request "+std::to_string(request.event));break;
    case TriggerMineRequest::Kind::SetTrapFlag:
        dialogueMessages_.push_back(prefix+"engine trap flag changed");break;
    case TriggerMineRequest::Kind::RemoveFromDetectionArea:
        dialogueMessages_.push_back(prefix+"removed from engine detection area");break;
    case TriggerMineRequest::Kind::RemoveInventoryItem:
        dialogueMessages_.push_back("Removed captured mine item "+std::to_string(request.item));break;
    case TriggerMineRequest::Kind::DecrementItemStack:
        dialogueMessages_.push_back("Decremented captured mine stack "+std::to_string(request.item));break;
    case TriggerMineRequest::Kind::AddToDetectionArea:
        dialogueMessages_.push_back(prefix+"registered in engine detection area");break;
    }
    return true;
}
bool ModelCanvas::enterTriggerMine(std::size_t source,const TriggerMineEnterInput& input) {
    const bool complete=areaScene_.enterTriggerMineFromInventory(source,input,
        [this](const auto& request){return receiveTrapRequest(request);});
    refreshTriggerObservers();consumeTriggerEvents();Refresh(false);return complete;
}
bool ModelCanvas::resumeTriggerMineEntry() {
    const auto sink=[this](const auto& request){return receiveTrapRequest(request);};
    const bool complete=areaScene_.triggerRuntime().trapActionPending()
        ?areaScene_.resumeTrapAction(sink):areaScene_.resumeTriggerMineEntry(sink);
    refreshTriggerObservers();consumeTriggerEvents();Refresh(false);return complete;
}
TrapActionResult ModelCanvas::completeTrapAction(TrapObjectTarget target,TrapAction action,const TrapActionInput& input) {
    auto result=areaScene_.completeTrapAction(target,action,input,
        [this](const auto& request){return receiveTrapRequest(request);});
    refreshTriggerObservers();consumeTriggerEvents();Refresh(false);return result;
}
TrapSetMineResult ModelCanvas::completeSetMine(const TrapSetMinePlan& plan,
    std::optional<PreparedTriggerMine> prepared,PreparedSceneAudioPlayback sounds,PreparedTrapDoorAnimations doors) {
    for(auto& door:doors)trapDoorAnimations_[door.sourceIndex]=std::move(door.animations);
    const auto result=areaScene_.completeSetMine(plan,
        [this,&prepared,&sounds](const AreaTriggerRecord&,const TriggerMineState&) {
            return addTriggerMine(std::move(prepared.value()),std::move(sounds));
        },[this](const auto& request){return receiveTrapRequest(request);});
    refreshTriggerObservers();consumeTriggerEvents();Refresh(false);return result;
}
TriggerDetectionResult ModelCanvas::checkTriggerDetection(const TriggerDetectionInput& input) {
    auto result=areaScene_.checkTriggerDetection(input);
    refreshTriggerObservers();consumeTriggerEvents();Refresh(false);return result;
}
std::string triggerDetectionReport(const TriggerDetectionResult& result) {
    std::string message=result.checked?"Engine detection check executed.":"Engine actor timer has not reached the next check.";
    for(const auto& found:result.discoveries)message+="\n"+
        std::string(found.kind==TrapObjectKind::Mine?"Mine":found.kind==TrapObjectKind::Door?"Door":"Placeable")+
        "["+std::to_string(found.sourceIndex)+"] detected by "+std::to_string(found.actor)+
        ": roll "+std::to_string(found.roll)+", awareness "+std::to_string(found.awareness)+", DC "+std::to_string(found.difficultyClass);
    return message;
}
void ModelCanvas::captureTriggerDetection(TriggerDetectionInput input) {
    // These wrappers mark only an unavailable captured input. They do not
    // replace a libc random result or intercept unrelated world exceptions.
    input.engineRand=[source=std::move(input.engineRand)]() mutable {
        try {return source();}
        catch(const std::exception& e) {
            throw DetectionCaptureExhausted(std::string("Captured libc rand stream unavailable: ")+e.what());
        }
    };
    input.reputation=[source=std::move(input.reputation)](std::uint64_t from,std::uint64_t toward) {
        try {return source(from,toward);}
        catch(const std::exception& e) {throw DetectionCaptureExhausted(e.what());}
    };
    const auto actor=input.objectId;
    areaScene_.captureTriggerDetection(std::move(input));
    dialogueMessages_.push_back("Retained trap detection started for actor "+std::to_string(actor)+
        "; captured actor state is fixed until recaptured.");
    Refresh(false);
}
void ModelCanvas::clearTriggerDetectionCaptures() {
    areaScene_.clearTriggerDetectionCaptures();
    consumeTriggerDetectionResults();
    dialogueMessages_.push_back("Retained trap detection stopped.");
}
void ModelCanvas::consumeTriggerDetectionResults() {
    auto results=areaScene_.takeTriggerDetectionResults();
    for(const auto& result:results)dialogueMessages_.push_back(triggerDetectionReport(result));
    if(!results.empty())refreshTriggerObservers();
}
void ModelCanvas::beforeTriggerDestroyed(AreaObject& object) {
    const auto instance=object.instanceIndex();
    const auto bound=[&](std::optional<std::size_t> owner){return owner&&*owner==instance;};
    const bool dialogueBound=std::any_of(dialogueAssets_.actors.begin(),dialogueAssets_.actors.end(),
        [&](const auto& actor){return bound(actor.owner);}) ||
        std::any_of(dialogueAssets_.lines.begin(),dialogueAssets_.lines.end(),[&](const auto& line) {
            return (line.speakerBound&&bound(line.speaker))||(line.listenerBound&&bound(line.listener))||
                std::any_of(line.animations.begin(),line.animations.end(),[&](const auto& command){return bound(command.owner);});
        });
    if(dialogueBound)stopDialogue();
    releaseChunkCallbacks();motionBlurQueue_.clear();
    // Query owners borrow model/pose pointers. Drop them before the owner and
    // independently registered particle descendants release those allocations.
    const auto remove=[&](LightOwnerId owner) {
        sceneQueries_.removeOwner(owner);
        lightScene_.removeOwner(owner);ownerLightSpheres_.erase(owner);
        areaOwnerQueries_.erase(std::remove(areaOwnerQueries_.begin(),areaOwnerQueries_.end(),owner),areaOwnerQueries_.end());
    };
    remove(object.ownerId());
    for(std::size_t i=0;i<object.document()->references().size();++i)remove(object.ownerId(i));
    std::vector<const ParticleScene*> descendants;
    object.particles().appendAllocatedChunkScenes(descendants);
    for(const auto* scene:descendants) {
        const auto& chunk=*scene->chunkOwner();remove(chunk.ownerIdentity);
        for(std::size_t i=0;i<chunk.asset->references().size();++i)remove(chunk.ownerIdentity+i+1u);
    }
    if(worldPlaybackSettings_.playerOwner&&*worldPlaybackSettings_.playerOwner==object.ownerId())
        worldPlaybackSettings_.playerOwner.reset();
    pendingGpuReset_=true;
}
void ModelCanvas::consumeTriggerEvents() {
    for(const auto& request:areaScene_.takeTrapServerRequests())receiveTrapRequest(request);
    bool changed=false,destroyed=false;
    for(const auto& event:areaScene_.takeTriggerEvents()) {
        changed=true;
        if(event.kind==TriggerRuntimeEvent::Kind::DoorOpenState) {
            for(const auto& placement:areaScene_.placements())
                if(placement.kind==AreaObjectKind::Door&&placement.sourceIndex==event.sourceIndex&&placement.runtimeInstance) {
                    auto* owner=areaScene_.object(*placement.runtimeInstance);
                    auto state=owner->visualState().live();
                    const auto& current=owner->effectiveVisualState().animation;
                    const auto selector=current?current->doorAnimationSelector:1;
                    const auto& prepared=trapDoorAnimations_.at(event.sourceIndex);
                    state.animation=*std::find_if(prepared.begin(),prepared.end(),[&](const auto& command) {
                        return command.serializedState==event.animation&&
                            (event.animation!=3u||(command.doorAnimationSelector==0)==(selector==0));
                    });
                    state.animation->doorAnimationSelector=selector;
                    std::string error;
                    if(!areaScene_.applyPreparedObjectVisualState(*owner,state,error))
                        throw std::runtime_error(error);
                }
        } else if(event.kind==TriggerRuntimeEvent::Kind::UnregisterFootprint) {
            const auto& records=triggerScene_.records();
            for(std::size_t i=records.size();i>0;--i)
                if(records[i-1].source==RuntimeTriggerSource::AreaGit&&records[i-1].sourceIndex==event.sourceIndex)
                    triggerScene_.removeVolume(i-1);
        } else if(event.kind==TriggerRuntimeEvent::Kind::Destroy) {
            triggerMineInputs_.erase(event.sourceIndex);destroyed=true;
        } else if(event.kind==TriggerRuntimeEvent::Kind::Sound) {
            try {
                if(!sceneAudioProvider_)ensureSceneAudioProvider();
                playSceneAudio(mineExplosionAudio(sceneAudioOneShot_++,event.resource,event.position,
                    sceneAudioAssets_.priorities.at(event.soundChannel)));
            }catch(const std::exception& e) {
                sceneAudioStatus_=e.what();dialogueMessages_.push_back("Mine sound: "+sceneAudioStatus_);
            }
        }
    }
    if(destroyed) {
        particleScenes();lightScene_.synchronizeChunks(&particleScene_,areaScene_,options_.showReferences);
        if(!areaScene_.empty())areaScene_.synchronize(lightScene_,lastScenePlanes_);
        synchronizeShadowCasters();
    }
    if(changed) {synchronizeEngineQueries();updateSceneBounds();}
}
bool ModelCanvas::deleteTriggerMine(std::size_t source,std::uint8_t reason) {
    const bool accepted=areaScene_.deleteTrigger(source,reason,[this](AreaObject& owner){beforeTriggerDestroyed(owner);});
    consumeTriggerEvents();Refresh(false);return accepted;
}
void ModelCanvas::setTriggerGlobalAlpha(float alpha) {
    triggerScene_.setGlobalAlpha(alpha);
    Refresh(false);
}

std::optional<RuntimeTriggerHit> ModelCanvas::hitTestTriggers(
    const neomdl::model::Vec3& start,
    const neomdl::model::Vec3& end,
    const neomdl::model::TriggerHitOptions& options) const {
    return triggerScene_.hitTest(start, end, options);
}

void ModelCanvas::bindExternalCameraControl() {
    // Camera is an independent Gob. AttachToObject's behavior evaluates its
    // cameramodel dependency before reading the camerahook transform.
    externalCameraAnimationOwner_.setControl([this](float seconds) {
        const auto lifetime=*externalCameraAnimationOwner_.lifetime();
        const auto behavior=externalCameraAnimationOwner_.controlRevision();
        if(dialogueCameraActive_&&dialogueAssets_.cameraModel) {
            const auto attachment=dialogueCameraScene_.attachmentRevision();
            const auto targetLifetime=*dialogueCameraAnimationOwner_.lifetime();
            if(SceneAnimation::dependenciesAnimating())dialogueCameraAnimationOwner_.animate(seconds);
            if(lifetime.expired())return;
            if(externalCameraAnimationOwner_.controlRevision()!=behavior)return;
            if(!dialogueCameraActive_||dialogueCameraScene_.attachmentRevision()!=attachment)return;
            if(targetLifetime.expired()) {
                externalCameraAnimationOwner_.setControl({});
                dialogueCameraScene_.detachAttachment();return;
            }
            dialogueCameraScene_.synchronize(dialogueCameraAnimation_.pose());
        } else if(const auto attachment=cameraScene_.activeAttachment()) {
            const auto attachmentRevision=cameraScene_.attachmentRevision();
            auto& owner=attachment->reference?referenceScene_.animationOwner(*attachment->reference):mainAnimationOwner_;
            const auto targetLifetime=*owner.lifetime();
            if(SceneAnimation::dependenciesAnimating()) {
                owner.animate(seconds);
            }
            if(lifetime.expired())return;
            if(externalCameraAnimationOwner_.controlRevision()!=behavior)return;
            if(cameraScene_.attachmentRevision()!=attachmentRevision)return;
            if(targetLifetime.expired()) {
                externalCameraAnimationOwner_.setControl({});
                cameraScene_.detachAttachment();return;
            }
            cameraScene_.synchronize(*pose_,options_.showReferences);
        }
    });
}

void ModelCanvas::synchronizeCameraScene() {
    if (pose_) cameraScene_.synchronize(*pose_, options_.showReferences);
}

void ModelCanvas::collectAnimationRuntimeEvents() {
    auto events = animationRuntime_.takeEvents();
    for (const auto& event : events) {
        // Runtime effects were dispatched synchronously by Gob::DoEvent's
        // counterpart. This queue is only the UI's observational event log.
        pendingAnimationRuntimeEvents_.push_back(event);
    }
}

void ModelCanvas::resetParticleHistoryForAnimationSeek() {
    ++sceneEffects_.historyEpoch; // a seek is a scene-history cut, even with particles disabled
    pendingEmitterSceneAnimating_=false;emitterPrewarmTrace_={};emitterFrameTrace_={};
    if (!options_.previewEmitters) {
        particleScene_.resetSimulation();
        return;
    }
    if (replayAnimationIndex_) {
        ParticleReplayContext context;context.view=cameraView_;
        context.cameraFrame=sceneCameraFrame();
        for(const auto& plane:lastScenePlanes_)context.planes.push_back({plane.normal,plane.distance});
        struct ReplayPoseScope {const model::Pose*& pose;~ReplayPoseScope(){pose=nullptr;}} replayScope{inspectionReplayPose_};
        context.prepareReferences=[this](const model::Pose& pose,float) {
            inspectionReplayPose_=&pose;
            // Main-model inspection seek retains independent reference/area
            // clocks while replaying the selected owner's emitter tree.
            referenceScene_.synchronize(pose);
            sceneQueries_.synchronize(*document_,pose,referenceScene_,areaScene_,triggerScene_,mainAnimationOwner_,&particleScene_);
        };
        context.commitEmitterState=[this](const model::Pose& replayPose) {
            const auto& live=animationRuntime_.pose().nodeEmitters;
            for(std::size_t node=0;node<live.size();++node)
                *live[node]=*replayPose.nodeEmitters[node];
        };
        particleScene_.replay(replayAnimationIndex_, replayAnimationTime_,
                              replayAnimationLoop_, options_.showReferences,context);
    } else {
        // A stack can combine independent times and transition weights, so it
        // has no single deterministic replay clock. Seeking resets historical
        // particles and resumes from the newly composed emitter-controller
        // state instead of inventing an ordering that K2 never used.
        particleScene_.resetSimulation();
    }
    if(pose_) {synchronizeEngineQueries();particleScene_.prepareFrame(*pose_,0.0,options_.showReferences);}
}

void ModelCanvas::setAnimation(std::optional<std::size_t> index, float time,
                               bool loop, bool continuous) {
    const bool sameCompatibilityRun =
        index == replayAnimationIndex_ && compatibilityRunId_ &&
        animationRuntime_.run(*compatibilityRunId_) != nullptr;
    const bool seek = !continuous || !sameCompatibilityRun;
    replayAnimationIndex_ = index;
    replayAnimationTime_ = std::isfinite(time) ? time : 0.0f;
    replayAnimationLoop_ = loop;

    if (!document_ || !document_->isOpen() || !index ||
        *index >= document_->animations().size()) {
        replayAnimationIndex_.reset();
        compatibilityRunId_.reset();
        animationRuntime_.clear(true);
    } else if (sameCompatibilityRun) {
        animationRuntime_.setRunLooping(*compatibilityRunId_, loop);
        animationRuntime_.setRunTime(*compatibilityRunId_,
                                     replayAnimationTime_, true);
    } else {
        animationRuntime_.clear(true);
        std::uint32_t flags = neomdl::model::kAnimationRunImmediate;
        if (!loop) flags |= neomdl::model::kAnimationRunNonLooping;
        const auto result = animationRuntime_.play(
            document_->animationReference(*index), 1.0f, flags,
            replayAnimationTime_);
        compatibilityRunId_ = result.runId;
        if (compatibilityRunId_) {
            animationRuntime_.setRunTime(*compatibilityRunId_,
                                         replayAnimationTime_, true);
        }
    }
    collectAnimationRuntimeEvents();
    materialTime_ = animationRuntime_.empty()
        ? 0.0 : animationRuntime_.primaryTime();
    rebuildPose(seek);
    if (seek) {
        resetParticleHistoryForAnimationSeek();
        updateSceneBounds();
    }
    Refresh(false);
}

neomdl::model::AnimationPlayResult ModelCanvas::playAnimationRun(
    std::size_t clipIndex, float playbackRate, std::uint32_t flags,
    float startTime) {
    neomdl::model::AnimationPlayResult result;
    if (!document_ || !document_->isOpen() ||
        clipIndex >= document_->animations().size()) {
        result.diagnostic = "Animation selection is outside the open model";
        return result;
    }
    replayAnimationIndex_.reset();
    compatibilityRunId_.reset();
    result = animationRuntime_.play(document_->animationReference(clipIndex),
                                    playbackRate, flags, startTime);
    collectAnimationRuntimeEvents();
    if (result) {
        materialTime_ = animationRuntime_.primaryTime();
        rebuildPose(false);
        Refresh(false);
    }
    return result;
}

neomdl::model::AnimationPlayResult
ModelCanvas::playOutOfOrderAnimationRun(
    std::size_t clipIndex,
    const std::vector<float>& normalizedAnimationTimes,
    const std::vector<float>& controlTimes, float duration) {
    neomdl::model::AnimationPlayResult result;
    if (!document_ || !document_->isOpen() ||
        clipIndex >= document_->animations().size()) {
        result.diagnostic = "Animation selection is outside the open model";
        return result;
    }
    replayAnimationIndex_.reset();
    compatibilityRunId_.reset();
    result = animationRuntime_.playOutOfOrder(
        document_->animationReference(clipIndex), normalizedAnimationTimes,
        controlTimes, duration);
    collectAnimationRuntimeEvents();
    if (result) {
        materialTime_ = animationRuntime_.primaryTime();
        rebuildPose(false);
        Refresh(false);
    }
    return result;
}

bool ModelCanvas::stopAnimationRun(std::uint64_t id, bool graceful) {
    if (!animationRuntime_.stop(id, graceful)) return false;
    if (!graceful) animationRuntime_.rebuildPose(false);
    if (compatibilityRunId_ && *compatibilityRunId_ == id)
        compatibilityRunId_.reset();
    replayAnimationIndex_.reset();
    collectAnimationRuntimeEvents();
    rebuildPose(false);
    Refresh(false);
    return true;
}

bool ModelCanvas::removeAnimationRun(std::uint64_t id) {
    return stopAnimationRun(id, false);
}

bool ModelCanvas::setAnimationRunTime(std::uint64_t id, float time,
                                      bool rebuildFromBind) {
    if (!animationRuntime_.setRunTime(id, time, rebuildFromBind)) return false;
    if (compatibilityRunId_ && *compatibilityRunId_ == id) {
        replayAnimationTime_ = time;
    } else {
        replayAnimationIndex_.reset();
    }
    materialTime_ = animationRuntime_.primaryTime();
    rebuildPose(true);
    resetParticleHistoryForAnimationSeek();
    updateSceneBounds();
    Refresh(false);
    return true;
}

bool ModelCanvas::setAnimationRunPlaybackRate(
    std::uint64_t id, float playbackRate) {
    return animationRuntime_.setRunPlaybackRate(id, playbackRate);
}

bool ModelCanvas::setAnimationRunLooping(std::uint64_t id, bool looping) {
    if (!animationRuntime_.setRunLooping(id, looping)) return false;
    if (compatibilityRunId_ && *compatibilityRunId_ == id)
        replayAnimationLoop_ = looping;
    return true;
}

void ModelCanvas::clearAnimationRuns() {
    animationRuntime_.clear(true);
    pendingAnimationRuntimeEvents_.clear();
    replayAnimationIndex_.reset();
    replayAnimationTime_ = 0.0f;
    replayAnimationLoop_ = false;
    compatibilityRunId_.reset();
    materialTime_ = 0.0;
    rebuildPose(true);
    resetParticleHistoryForAnimationSeek();
    updateSceneBounds();
    Refresh(false);
}

std::vector<neomdl::model::AnimationRuntimeEvent>
ModelCanvas::takeAnimationRuntimeEvents() {
    auto result = std::move(pendingAnimationRuntimeEvents_);
    pendingAnimationRuntimeEvents_.clear();
    return result;
}

void ModelCanvas::resetDynamics() {
    // This inspection action starts fresh dynamics at the current authored
    // pose. Construct the scene wind before reference default commands, then
    // let future births consume the resulting shared random stream.
    ++sceneEffects_.historyEpoch;
    pendingEmitterSceneAnimating_=false;emitterPrewarmTrace_={};emitterFrameTrace_={};
    particleScene_.resetSimulation();
    referenceScene_.reset();
    materialTime_ = animationRuntime_.empty()
        ? 0.0 : animationRuntime_.primaryTime();
    rebuildPose(true);
    updateSceneBounds();
    Refresh(false);
}

void ModelCanvas::dispatchParticleEvent(const std::string& name) {
    particleScene_.dispatchEvent(name);
}

void ModelCanvas::detonateEmitters() {
    if(!pose_ || !options_.previewEmitters) return;
    particleScene_.detonateAll(options_.showReferences);
    // An explicit inspection event must still receive one engine bucket-time
    // step while the scene is paused. Use owner guards rather than the global
    // Scene::IsAnimating guard so unrelated area emitters do not advance.
    synchronizeEngineQueries();
    std::vector<bool> referenceAnimating(referenceScene_.instances().size(),true);
    particleScene_.prepareFrame(*pose_,1.0/60.0,options_.showReferences,true,
                                referenceAnimating);
    pendingSceneElapsed_+=1.0f/60.0f;
    updateSceneBounds();Refresh(false);
}

neomdl::model::DanglyEnvironment ModelCanvas::danglyEnvironment(
    neomdl::model::EngineFrame objectFrame) const {
    neomdl::model::DanglyEnvironment environment;
    environment.objectFrame=objectFrame;
    environment.windDisplacement=[this](neomdl::model::Vec3 position,float seconds) {
        return particleScene_.windDisplacement(position,seconds);
    };
    return environment;
}

ReferenceReattachResult ModelCanvas::reattachReferences(
    std::optional<std::size_t> ownerReference,
    std::optional<ReferenceObjectTarget> target) {
    if (!pose_) return {};
    const auto result = referenceScene_.reattachOwnedReferences(
        ownerReference, std::move(target));
    cameraScene_.synchronize(*pose_, options_.showReferences);
    synchronizeEngineQueries();
    particleScene_.prepareFrame(*pose_,0.0,options_.showReferences);
    lightsaberScene_.synchronize(*pose_, options_.showReferences);
    lightScene_.synchronize(*pose_, options_.showReferences);
    updateSceneBounds();
    Refresh(false);
    return result;
}

ReferenceReattachResult ModelCanvas::restoreReferenceAttachments(
    std::optional<std::size_t> ownerReference) {
    return reattachReferences(ownerReference, std::nullopt);
}

bool ModelCanvas::advanceMainOwner(float seconds,bool evaluatePose) {
    const bool dialogueActorAnimating=std::any_of(dialogueActorAnimations_.begin(),dialogueActorAnimations_.end(),
        [](const auto& active){return !active.source->owner;});
    const bool tick=inspectionAnimationsAdvancing_||dialogueMainLip_||dialogueActorAnimating||animationRuntime_.lookAtAnimationActive();
    mainAnimationAdvanced_=tick&&!animationRuntime_.empty()&&seconds>0.0f;
    const auto lifetime=*mainAnimationOwner_.lifetime();
    const bool applied=animationRuntime_.advance(tick?seconds:0.0f,evaluatePose);
    if(lifetime.expired())return applied;
    collectAnimationRuntimeEvents();
    pose_=animationRuntime_.pose();++poseRevision_;
    if(animationRuntime_.empty())materialTime_+=seconds;
    else materialTime_=animationRuntime_.primaryRunId()?animationRuntime_.primaryTime():pose_->time;
    if(dialogueMainLip_)dialogueMainLip_=std::any_of(animationRuntime_.runs().begin(),animationRuntime_.runs().end(),
        [](const auto& run){return (run.flags&0x200u)!=0;});
    referenceScene_.synchronize(*pose_,false);
    cameraScene_.synchronize(*pose_,options_.showReferences,false);
    return applied;
}

SceneAnimationOwner* ModelCanvas::animationOwnerForId(LightOwnerId id) {
    if(id==mainLightOwner)return document_&&document_->hasModelOwner()?&mainAnimationOwner_:nullptr;
    for(std::size_t i=0;i<referenceScene_.instances().size();++i)
        if(id==i+1u)return &referenceScene_.animationOwner(i);
    for(const auto& object:areaScene_.objects()) {
        if(!object->staticPrimary()&&id==object->ownerId())return &object->animationOwner();
        for(std::size_t i=0;i<object->references().instances().size();++i)
            if(id==object->ownerId(i))return &object->references().animationOwner(i);
    }
    std::vector<const ParticleScene*> scenes;
    particleScene_.appendAllocatedChunkScenes(scenes);
    for(const auto* root:areaScene_.particleSceneRoots())root->appendAllocatedChunkScenes(scenes);
    for(const auto* scene:scenes) {
        const auto* chunk=scene->chunkOwner();
        if(id==chunk->ownerIdentity)return scene->chunkAnimationOwner();
        for(std::size_t i=0;i<chunk->references->instances().size();++i)
            if(id==chunk->ownerIdentity+i+1u)return &chunk->references->animationOwner(i);
    }
    return nullptr;
}

bool ModelCanvas::engineOwnerAdmitted(LightOwnerId id,std::uint16_t classification) {
    if(areaScene_.rooms().empty()||(classification&0x40u))return true;
    const auto& owner=*animationOwnerForId(id);
    return owner.inViewVolume()||owner.alwaysRendering();
}

void ModelCanvas::animateRenderOwner(LightOwnerId id) {
    const auto scene=particleScene_.sceneAnimation();
    const auto found=renderAnimationOwners_.find(id);
    if(found!=renderAnimationOwners_.end()) {
        auto* owner=found->second;
        if(!scene->animateForRender(*owner))return;
        // Engine Parts expose their updated transforms immediately to Mat,
        // light selection and hit queries in this owner's render callback.
        synchronizeEngineQueries();
        lightScene_.synchronize(*pose_,options_.showReferences);
        if(!areaScene_.empty())areaScene_.synchronize(lightScene_,lastScenePlanes_);
        lightScene_.synchronizeChunks(&particleScene_,areaScene_,options_.showReferences);
    }
}

void ModelCanvas::refreshAnimatedSubmission(render::MeshSubmission& submission,
        const model::Model& asset,const model::Pose& pose,const model::Mat4& world,
        const model::EngineFrame& frame,float time,std::uint64_t revision,const neoshared::material::Material& material,
        const ModelDocument* document,const OwnerVisualEffects* effects,
        std::optional<float> textureAngle) {
    const auto& mesh=*submission.mesh;
    const auto meshIndex=*asset.nodes[mesh.nodeIndex].mesh;
    const model::DeformedMesh* deformed=nullptr;
    if((options_.danglySimulation||!mesh.dangly)&&meshIndex<pose.deformedMeshes.size()&&pose.deformedMeshes[meshIndex].valid)
        deformed=&pose.deformedMeshes[meshIndex];
    auto scaled=world;
    auto scaling=model::identityMatrix();scaling.values[0]=scaling.values[5]=scaling.values[10]=frame.scale;
    scaled=model::multiply(scaled,scaling);
    const auto drawWorld=deformed?scaled:model::multiply(scaled,pose.worldTransforms[mesh.nodeIndex]);
    const auto engineParts=asset.game==model::GameVersion::Kotor1
        ?engineCollapsedHitPose(asset,pose,frame):std::vector<HitPartTransform>{};
    std::vector<render::MeshSubmission> refreshed;std::string failure;
    glPushMatrix();const auto modelView=model::multiply(cameraView_,drawWorld);glLoadMatrixf(modelView.values.data());
    drawMesh(mesh,deformed,submission.parameters.selected,material,pose.nodeMaterials[mesh.nodeIndex],
        asset.game,asset.fog,submission.lightOwner.value_or(0),document,time,&refreshed,
        submission.sceneOwner,submission.ownerAlpha,asset.classification,submission.ownerHasTransparency,
        submission.ownerCameraDistance,false,effects,&world,textureAngle,
        engineParts.empty()?nullptr:&engineParts[mesh.nodeIndex],&failure,submission.eyeDepth);
    glPopMatrix();
    if(!failure.empty())throw std::runtime_error(failure);
    // Draw reassembly keeps the already-sorted owner key and borrowed
    // lifetime. Only live Part/material/pose state replaces the queued values.
    auto& current=refreshed.at(0);
    submission.mesh=current.mesh;submission.deformed=current.deformed;submission.poseRevision=revision;
    submission.parameters=std::move(current.parameters);
    submission.drawPrimary=current.drawPrimary;submission.materialOnly=current.materialOnly;
    submission.trailingShell=std::move(current.trailingShell);
}

void ModelCanvas::refreshOwnerSubmission(render::MeshSubmission& submission,LightOwnerId id,
        const AreaObject* retainedObject,std::optional<std::size_t> retainedReference) {
    const auto refresh=[&](const model::Model& asset,const model::Pose& pose,const model::Mat4& world,
            const model::EngineFrame& frame,const SceneAnimationOwner& animationOwner,
            float time,std::uint64_t revision,const ModelDocument& document,
            std::optional<std::size_t> reference,const OwnerVisualEffects* effects,
            std::optional<float> textureAngle={}) {
        ownerLightSpheres_[id]={model::transformPoint(world,{}),animationOwner.sphere().radius};
        if(!submission.mesh) {
            submission.parameters.modelView=model::multiply(cameraView_,world);return;
        }
        const auto meshIndex=*asset.nodes[submission.mesh->nodeIndex].mesh;
        const auto& material=reference?document.referenceMaterial(*reference,meshIndex):document.material(meshIndex);
        refreshAnimatedSubmission(submission,asset,pose,world,frame,time,revision,material,&document,effects,textureAngle);
    };
    if(id==mainLightOwner) {
        refresh(document_->model(),*pose_,referenceScene_.mainWorld(),referenceScene_.mainFrame(),mainAnimationOwner_,
            materialTime_,poseRevision_,*document_,{},&ownerEffects_[0]);return;
    }
    for(std::size_t i=0;i<referenceScene_.instances().size();++i)if(id==i+1u) {
        const auto& runtime=*referenceScene_.instance(i);
        refresh(document_->references()[i].model,runtime.pose,runtime.world,runtime.frame,runtime.animationOwner,
            runtime.animationTime,referenceScene_.revision(),*document_,i,&ownerEffects_[i+1u]);return;
    }
    const auto* object=retainedObject;
    const auto reference=retainedReference;
    const auto* runtime=reference?object->references().instance(*reference):nullptr;
    const auto& asset=reference?object->document()->references()[*reference].model:object->document()->model();
    refresh(asset,runtime?runtime->pose:object->pose(),runtime?runtime->world:object->world(),
        runtime?runtime->frame:object->frame(),runtime?runtime->animationOwner:object->animationOwner(),
        runtime?runtime->animationTime:static_cast<float>(object->animation().empty()?object->elapsed():object->animation().primaryTime()),
        (id<<32u)^object->revision(),*object->document(),reference,object->ownerVisualEffects(reference),
        submission.mesh?std::optional<float>{object->textureRotationDegrees(*submission.mesh)}:std::nullopt);
}

void ModelCanvas::updateAnimationVisibility() {
    // ManageSceneBSP returns before resetting flags if it has no room/BSP
    // representation. A standalone inspection owner keeps constructor true.
    if(areaScene_.rooms().empty()||!document_||!pose_)return;
    const auto owners=collectSceneOwners(*document_,*pose_,referenceScene_,areaScene_,true,
        options_.objectLightRadius,&danglyState_,&particleScene_,&mainAnimationOwner_);
    for(const auto& owner:owners)
        if(auto* animation=owner.animationOwner)animation->setInViewVolume(false);
    for(const auto& owner:owners) {
        if(!owner.renderVisible)continue;
        const auto& sphere=owner.animationOwner->sphere();
        const auto rotated=engineRotate(owner.frame.orientation,sphere.center);
        const model::Vec3 center{owner.frame.position.x+rotated.x,
            owner.frame.position.y+rotated.y,owner.frame.position.z+rotated.z};
        const bool outside=std::any_of(lastScenePlanes_.begin(),lastScenePlanes_.end(),[&](const auto& plane) {
            return ((plane.normal.x*center.x+plane.distance)+plane.normal.y*center.y)+plane.normal.z*center.z-sphere.radius>0.0f;
        });
        if(!outside)if(auto* animation=owner.animationOwner)animation->setInViewVolume(true);
    }
    const auto propagate=[](ReferenceScene& references,SceneAnimationOwner* primary) {
        const auto visit=[&](const auto& self,std::size_t index)->bool {
            auto& owner=references.animationOwner(index);
            const auto& reference=references.instances()[index];
            const bool parent=reference.targetReference?self(self,*reference.targetReference):
                primary&&primary->inViewVolume();
            if(parent)owner.setInViewVolume(true);
            return owner.inViewVolume();
        };
        for(std::size_t index=0;index<references.instances().size();++index)visit(visit,index);
    };
    propagate(referenceScene_,&mainAnimationOwner_);
    for(const auto& object:areaScene_.objects())
        propagate(object->references(),object->staticPrimary()?nullptr:&object->animationOwner());
    std::vector<const ParticleScene*> scenes;
    particleScene_.appendActiveChunkScenes(scenes);
    for(const auto* root:areaScene_.particleSceneRoots())root->appendActiveChunkScenes(scenes);
    for(const auto* scene:scenes)propagate(*scene->chunkOwner()->references,scene->chunkAnimationOwner());
}

void ModelCanvas::advance(float elapsedSeconds, bool advanceParticles,
                          bool advanceAnimations) {
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0f) elapsedSeconds = 0.0f;
    synchronizeWorldClockCreatures();
    std::uint64_t snapshotMicroseconds=0u;
    // All four engine timers sample the same microsecond host snapshot. Only
    // the explicit inspector pause stops that snapshot; engine pause selects
    // which world/alternate timer receives it in the client MainLoop.
    if(!worldPlaybackSettings_.paused) {
        worldMicrosecondsRemainder_+=static_cast<double>(elapsedSeconds)*1000000.0;
        snapshotMicroseconds=static_cast<std::uint64_t>(worldMicrosecondsRemainder_);
        worldMicrosecondsRemainder_-=snapshotMicroseconds;
    }
    worldClock_.advance(snapshotMicroseconds);
    const auto worldMilliseconds=worldClock_.worldDeltaMilliseconds();
    dialogueRuntime_.setWorldTime(worldClock_.worldTime(),worldClock_.dayMilliseconds());
    if(!worldPlaybackSettings_.paused&&!worldClock_.worldPaused()) {
        areaScene_.advanceWorldTime(worldClock_.worldTime(),elapsedSeconds);
        refreshTriggerObservers();
        try {areaScene_.advanceTriggers(worldMilliseconds,[this](AreaObject& owner){beforeTriggerDestroyed(owner);});}
        catch(const DetectionCaptureExhausted& e) {
            areaScene_.clearTriggerDetectionCaptures();
            dialogueMessages_.push_back(std::string("Retained trap detection stopped: ")+e.what());
        }
        consumeTriggerDetectionResults();
        consumeTriggerEvents();
    }
    // CSWCArea::Update uses the paused world timer (K1 Desktop 0x64652 /
    // K2 Desktop 0x26df0c), while zero-delta weather polling still runs.
    advanceWeather();
    if(dialogueRuntime_.running()) {
        // Animation identity expires on a DWORD timer, independently of the
        // dialogue node's absolute world deadline and the render run.
        restoreCompletedDialogueAnimations(worldMilliseconds);
    }
    // IsDialogDelay compares the paused server world clock (K2 Desktop
    // 0x3e434e); its live audio/LIP predicates still poll with zero delta.
    advanceDialogue();
    if(dialogueRuntime_.running()&&dialogueRuntime_.dialogue().conversationType.value_or(0)!=1)
        dialogueDrawElapsed_+=elapsedSeconds;
    // Scene texture controllers consume the frame clock, not animation seek time.
    pendingProcedureElapsed_ += elapsedSeconds;
    pendingDoorElapsed_ += elapsedSeconds;
    if(options_.engineLensFlares.value_or(!areaScene_.empty()))pendingFlareElapsed_+=elapsedSeconds;
    else pendingFlareElapsed_=0;
    // Scene rendering and WindManager receive the full frame clock. The
    // emitter bucket applies its own engine 0.1-second update clamp.
    pendingSceneElapsed_+=elapsedSeconds;
    const float sceneAnimationSeconds=elapsedSeconds;
    pendingEmitterSceneAnimating_=pendingEmitterSceneAnimating_ ||
        (options_.previewEmitters && advanceParticles && elapsedSeconds>0.0f);
    if (options_.previewLights)
        pendingLightElapsed_ += elapsedSeconds;
    inspectionAnimationsAdvancing_=advanceAnimations;
    mainAnimationAdvanced_=false;
    AreaFrameSettings areaFrame;
    areaFrame.animate=true;
    areaFrame.dangly=options_.danglySimulation;areaFrame.emitters=options_.previewEmitters;
    areaFrame.references=options_.showReferences;
    areaScene_.prepareAnimationFrame(areaFrame);
    areaScene_.setWorldAnimationFrameInput({sceneCameraFrame().position,false,true});
    // World playback pause supplies the engine Scene::IsAnimating input.
    // The inspection clip toggle only controls its own clip clock above.
    synchronizeWorldPause();
    const auto updateCreatureWind=[&](LightOwnerId id,SceneAnimationOwner& owner) {
        const auto fade=worldPlaybackSettings_.creatureFadeStates.find(id);
        owner.updateCreatureWind(worldPlaybackSettings_.creatureWindMode,
            worldPlaybackSettings_.playerOwner==id,
            fade==worldPlaybackSettings_.creatureFadeStates.end()?std::uint8_t{}:fade->second,
            worldClock_.activePauseState(),worldPlaybackSettings_.creatureWindIntensity);
    };
    if(document_&&document_->hasModelOwner()) {
        if(worldPlaybackSettings_.playerOwner==mainLightOwner||worldPlaybackSettings_.creatureFadeStates.count(mainLightOwner))
            updateCreatureWind(mainLightOwner,mainAnimationOwner_);
        else mainAnimationOwner_.setWind(false,0.0f,0.0f,0.0f);
    }
    for(const auto& object:areaScene_.objects())if(object->kind()==AreaObjectKind::Creature)
        updateCreatureWind(object->ownerId(),object->animationOwner());
    particleScene_.sceneAnimation()->animate(sceneAnimationSeconds);
    const bool animationAdvanced=mainAnimationAdvanced_;
    if(pose_) {
        referenceScene_.synchronize(*pose_,false);
        cameraScene_.synchronize(*pose_, options_.showReferences,false);
        for(auto& event:areaScene_.takeWorldAnimationEvents())
            dialogueMessages_.push_back("Track "+std::to_string(event.track)+" requests script slot "+
                std::to_string(event.scriptSlot)+" from owner "+std::to_string(event.source)+": "+event.argument);
        synchronizeEngineQueries();
        updateDialogueActorCamera();
        std::vector<bool> referenceAnimating(referenceScene_.instances().size());
        if(options_.previewEmitters) {
            for(std::size_t index=0;index<referenceScene_.instances().size();++index) {
                const auto& runtime=referenceScene_.instances()[index];
                referenceAnimating[index]=runtime.active && runtime.sceneMember && runtime.isAnimating();
            }
        }
        particleScene_.prepareFrame(*pose_,elapsedSeconds,options_.showReferences,
            options_.previewEmitters && mainAnimationOwner_.isAnimating(),referenceAnimating);
        areaScene_.advanceEffects(elapsedSeconds,areaFrame);
        if(!options_.previewEmitters)discardSceneEmitterFrame(particleScenes());
        // Scene::Render performs UpdateEmitters prewarm at paint time, advances
        // the shared wind timer, and only later executes sorted PartEmitter
        // animation in DoEmitterBucket. Pose preparation above does no particle
        // simulation.
        // lightsaberrender gates Draw only; PartLightsaber::Animate keeps
        // collecting transforms while rendering is disabled.
        lightsaberScene_.refreshTransforms(*pose_,options_.showReferences);
        lightScene_.synchronize(*pose_, options_.showReferences);
    }
    updateSceneBounds();
    if (elapsedSeconds > 0.0f &&
        (options_.danglySimulation || options_.previewEmitters ||
         options_.previewLightsabers || options_.previewLights ||
         options_.previewTriggers ||
         !areaScene_.empty() || cameraScene_.anyActive() || animationAdvanced || textureCache_.hasProcedures() ||
         animationRuntime_.empty())) {
        Refresh(false);
    }
}

void ModelCanvas::rebuildPose(bool resetDynamics) {
    ++poseRevision_;
    pose_.reset();
    if (!document_ || !document_->isOpen()) {
        sceneBounds_ = {};
        danglyState_ = {};
        referenceScene_.reset();
        cameraScene_.reset();
        lightsaberScene_.reset();
        lightScene_.reset();
        triggerScene_.reset();
        return;
    }
    if (animationRuntime_.valid() &&
        animationRuntime_.pose().positions.size() ==
            document_->model().nodes.size()) {
        pose_ = animationRuntime_.pose();
    } else {
        pose_ = neomdl::model::bindPose(document_->model());
    }
    if (resetDynamics || danglyState_.meshes.size() != document_->model().meshes.size()) {
        neomdl::model::resetDanglySimulation(
            document_->model(), *pose_, danglyState_);
    }
    referenceScene_.synchronize(*pose_);
    cameraScene_.synchronize(*pose_, options_.showReferences);
    synchronizeEngineQueries();
    particleScene_.prepareFrame(*pose_,0.0,options_.showReferences);
    if (resetDynamics) {
        lightsaberScene_.reset();
        lightScene_.reset();
        pendingLightElapsed_ = 0.0f;
    }
    lightsaberScene_.synchronize(*pose_, options_.showReferences);
    lightScene_.synchronize(*pose_, options_.showReferences);
    updateSceneBounds();
}

void ModelCanvas::updateSceneBounds() {
    sceneBounds_ = {};
    if (!document_ || !document_->isOpen()) return;
    const auto sourceBounds = pose_ && pose_->bounds.valid
        ? pose_->bounds : document_->model().bounds;
    extendTransformedBounds(sceneBounds_, sourceBounds, referenceScene_.mainWorld());
    const auto areaBounds = areaScene_.bounds();
    if (areaBounds.valid) {
        extendBounds(sceneBounds_,areaBounds.minimum);
        extendBounds(sceneBounds_,areaBounds.maximum);
    }
    if(options_.previewEmitters) {
        const auto particleBounds=particleScene_.bounds();
        if(particleBounds.valid) {extendBounds(sceneBounds_,particleBounds.minimum);extendBounds(sceneBounds_,particleBounds.maximum);}
    }
    if (options_.previewLightsabers) {
        const auto lightsaberBounds = lightsaberScene_.bounds();
        if (lightsaberBounds.valid) {
            extendBounds(sceneBounds_, lightsaberBounds.minimum);
            extendBounds(sceneBounds_, lightsaberBounds.maximum);
        }
    }
    if (options_.previewTriggers) {
        const auto triggerBounds = triggerScene_.bounds();
        if (triggerBounds.valid) {
            extendBounds(sceneBounds_, triggerBounds.minimum);
            extendBounds(sceneBounds_, triggerBounds.maximum);
        }
    }
    if (!options_.showReferences || document_->references().empty()) return;
    const auto referenceBounds = referenceScene_.bounds();
    if (referenceBounds.valid) {
        extendBounds(sceneBounds_, referenceBounds.minimum);
        extendBounds(sceneBounds_, referenceBounds.maximum);
    }
}

const neomdl::model::Bounds& ModelCanvas::activeBounds() const noexcept {
    if (sceneBounds_.valid) return sceneBounds_;
    if (pose_ && pose_->bounds.valid) return pose_->bounds;
    if (document_ && document_->isOpen()) return document_->model().bounds;
    static const neomdl::model::Bounds empty;
    return empty;
}

void ModelCanvas::frameModel() {
    useViewerCamera(false);
    center_ = {};
    float extent = 1.0f;
    const auto& bounds = activeBounds();
    if (bounds.valid) {
        center_ = {(bounds.minimum.x + bounds.maximum.x) * 0.5f,
                   (bounds.minimum.y + bounds.maximum.y) * 0.5f,
                   (bounds.minimum.z + bounds.maximum.z) * 0.5f};
        extent = maximumExtent(bounds);
    }
    distance_ = std::max(2.0f, extent * 2.1f);
    flySpeed_ = extent * 0.5f;
    Refresh(false);
}

void ModelCanvas::framePosition(model::Vec3 position, float extent) {
    useViewerCamera(false);
    center_=position;
    distance_=extent*2.1f;
    flySpeed_=extent*0.5f;
    Refresh(false);
}

void ModelCanvas::setSelectedAreaOwners(std::set<LightOwnerId> owners) {
    selectedAreaOwners_=std::move(owners);
    Refresh(false);
}

model::Vec3 ModelCanvas::cameraOffset(model::Vec3 local) const {
    constexpr float radians = 3.14159265358979323846f / 180.0f;
    const auto sy = std::sin(yaw_ * radians), cy = std::cos(yaw_ * radians);
    const auto sp = std::sin(pitch_ * radians), cp = std::cos(pitch_ * radians);
    // Inverse of the viewer's pitch, yaw and Z-up to Y-up rotation.
    return {cy * local.x + sy * sp * local.y - sy * cp * local.z,
            -sy * local.x + cy * sp * local.y - cy * cp * local.z,
            cp * local.y + sp * local.z};
}

model::Vec3 ModelCanvas::viewerCameraPosition() const {
    if (freeFly_) return flyPosition_;
    const auto offset = cameraOffset({0.0f, 0.0f, distance_});
    return {center_.x + offset.x, center_.y + offset.y, center_.z + offset.z};
}

void ModelCanvas::advanceCamera() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedSeconds = std::chrono::duration<float>(now - cameraTick_).count();
    cameraTick_ = now;
    if (freeFlyCameraActive()) {
        auto direction = cameraOffset({float(flyMovement_[0]) - float(flyMovement_[1]),
                                      0.0f,
                                      float(flyMovement_[4]) - float(flyMovement_[5])});
        direction.z += float(flyMovement_[2]) - float(flyMovement_[3]);
        const auto length = std::hypot(direction.x, direction.y, direction.z);
        if (length > 0.0f) {
            const auto step = flySpeed_ * elapsedSeconds * (wxGetKeyState(WXK_SHIFT) ? 4.0f : 1.0f) / length;
            flyPosition_.x += direction.x * step;
            flyPosition_.y += direction.y * step;
            flyPosition_.z += direction.z * step;
        }
    }
}

void ModelCanvas::onPaint(wxPaintEvent&) {
    wxPaintDC dc(this);
    (void)dc; // A paint DC must exist for the duration of wxEVT_PAINT.
    // A load snapshots the engine RNG/wind continuation. Paint must not run
    // procedural scene work until that snapshot is committed or discarded.
    if (loading_) return;
    if (!context_ || !SetCurrent(*context_)) return;
    advanceCamera();
    initializeGl();
    textureCache_.flushRetiredTextures();
    meshRenderer_.flushRetiredGeometry();
    if (pendingGpuReset_) {
        clearGpuTextures();
        meshRenderer_.clearGeometry();
        meshRenderer_.invalidateSceneHistory();pendingMotionHistoryReset_=true;
        pendingGpuReset_ = false;
    }
    const auto size = GetClientSize();
    const auto scale = GetContentScaleFactor();
    const int width = std::max(1, static_cast<int>(std::lround(size.x * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(size.y * scale)));
    glViewport(0, 0, width, height);
    const auto sceneFog=effectiveFog();
    if(sceneFog.mode!=neomdl::model::FogMode::Disabled && neomdl::model::validFog(sceneFog))
        glClearColor(clampColor(sceneFog.color.x),clampColor(sceneFog.color.y),clampColor(sceneFog.color.z),1.0f);
    else glClearColor(0.075f,0.085f,0.10f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    applyProjection(width, height);
    if (meshPick_) meshPick_->depth = sceneFarClip_;
    drawScene();
    advanceSceneAudio();
    if(dialogueRuntime_.running()&&dialogueRuntime_.dialogue().conversationType.value_or(0)!=1) {
        dialogueFade_.advanceDraw(dialogueDrawElapsed_);dialogueDrawElapsed_=0;
        if(const auto color=dialogueFade_.packedColor(document_->model().game)) {
            std::string error;
            if(!meshRenderer_.drawDialogueFade(*color,error)&&!shaderErrorReported_) {
                wxLogError("NeoMDL dialogue fade: %s",wxString::FromUTF8(error.c_str()));
                shaderErrorReported_=true;
            }
        }
    }
    textureCache_.flushRetiredTextures();
    meshRenderer_.flushRetiredGeometry();
    glFlush();
    SwapBuffers();
    if (meshPick_) {
        const auto node = meshPick_->node;
        const bool additive=meshPick_->additive;
        const auto owner=meshPick_->areaOwner;
        meshPick_.reset();
        wxCommandEvent selection(EVT_MODEL_NODE_PICKED, GetId());
        selection.SetEventObject(this);
        selection.SetInt(node ? static_cast<int>(*node) : -1);
        selection.SetExtraLong(additive ? 1 : 0);
        ProcessWindowEvent(selection);
        if(!areaScene_.empty()) {
            wxCommandEvent areaSelection(EVT_AREA_OWNER_PICKED,GetId());
            areaSelection.SetEventObject(this);
            areaSelection.SetString(owner?wxString::Format("%llu",static_cast<unsigned long long>(*owner)):wxString{});
            ProcessWindowEvent(areaSelection);
        }
    }
}

void ModelCanvas::onSize(wxSizeEvent& event) {
    Refresh(false);
    event.Skip();
}

void ModelCanvas::onErase(wxEraseEvent&) {}

void ModelCanvas::onMouse(wxMouseEvent& event) {
    advanceCamera();
    if (event.ButtonDown()) SetFocus();
    if(event.LeftDown()&&event.ControlDown()&&pickingCamera_&&document_&&document_->isOpen()) {
        const auto scale=GetContentScaleFactor();
        const auto x=static_cast<int>(std::lround(event.GetX()*scale));
        const auto y=pickingCamera_->viewport.height-static_cast<int>(std::lround(event.GetY()*scale));
        const auto ray=engineCameraHitSegment(*pickingCamera_,x,y,document_->model().game);
        SceneHitInfo hit;hit.surfaceMask=0xbfffffffu;hit.includeClassificationOne=true;
        try {
            if(!sceneQueryError_.empty())throw std::runtime_error(sceneQueryError_);
            std::string message="No visual hit.";
            if(ray&&sceneQueries_.hit(ray->start,ray->end,hit)) {
                message=hit.owner?"Owner "+std::to_string(*hit.owner):"Static geometry";
                if(!hit.part.empty())message+="\nPart: "+hit.part;
                if(hit.room)message+="\nRoom: "+std::to_string(*hit.room);
                message+="\nPosition: "+std::to_string(hit.position.x)+", "+
                    std::to_string(hit.position.y)+", "+std::to_string(hit.position.z);
            }
            wxMessageBox(wxString::FromUTF8(message.c_str()),"Engine visual pick",wxOK|wxICON_INFORMATION,this);
        } catch(const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()),"Engine visual pick",wxOK|wxICON_ERROR,this);
        }
        return;
    }
    if (event.LeftDown() && !event.RightIsDown() && !event.MiddleIsDown())
        selectionStart_ = event.GetPosition();
    if (selectionStart_ && (event.Dragging() || event.LeftUp())) {
        const auto delta = event.GetPosition() - *selectionStart_;
        if (std::abs(delta.x) > FromDIP(4) || std::abs(delta.y) > FromDIP(4))
            selectionStart_.reset();
    }
    if (event.RightDown() || event.MiddleDown() || event.GetWheelRotation() != 0)
        selectionStart_.reset();
    if (event.LeftUp()) {
        if (selectionStart_ && !event.ControlDown()) {
            meshPick_ = MeshPick{event.GetPosition(), 0.0f, {}, event.ShiftDown(), {}};
            Refresh(false);
        }
        selectionStart_.reset();
    }
    if (authoredCameraActive()) {
        resetCameraInput();
        event.Skip();
        return;
    }
    if (event.LeftDown() || (freeFly_ && event.RightDown())) {
        draggingRotate_ = true;
        lastMouse_ = event.GetPosition();
        if (!HasCapture()) CaptureMouse();
    } else if (event.RightDown() || event.MiddleDown()) {
        draggingPan_ = true;
        lastMouse_ = event.GetPosition();
        if (!HasCapture()) CaptureMouse();
    } else if (event.LeftUp() || (freeFly_ && event.RightUp())) {
        draggingRotate_ = event.LeftIsDown() || (freeFly_ && event.RightIsDown());
        if (!draggingRotate_ && !draggingPan_ && HasCapture()) ReleaseMouse();
    } else if (event.RightUp() || event.MiddleUp()) {
        draggingPan_ = event.MiddleIsDown() || (!freeFly_ && event.RightIsDown());
        if (!draggingRotate_ && !draggingPan_ && HasCapture()) ReleaseMouse();
    } else if (event.Dragging()) {
        const auto current = event.GetPosition();
        const auto delta = current - lastMouse_;
        lastMouse_ = current;
        if (draggingRotate_ && !selectionStart_) {
            yaw_ += static_cast<float>(delta.x) * 0.5f;
            pitch_ = std::clamp(pitch_ + static_cast<float>(delta.y) * 0.5f, -89.0f, 89.0f);
        }
        if (draggingPan_) {
            const auto scale = freeFly_ ? flySpeed_ * 0.003f : std::max(0.001f, distance_ * 0.0015f);
            const auto offset = cameraOffset({-static_cast<float>(delta.x) * scale,
                                              static_cast<float>(delta.y) * scale, 0.0f});
            auto& position = freeFly_ ? flyPosition_ : center_;
            position.x += offset.x;
            position.y += offset.y;
            position.z += offset.z;
        }
        Refresh(false);
    }
    if (event.GetWheelRotation() != 0) {
        const auto steps = static_cast<float>(event.GetWheelRotation()) /
                           static_cast<float>(std::max(1, event.GetWheelDelta()));
        if (freeFly_) {
            flySpeed_ *= std::pow(1.25f, steps);
        } else {
            const auto travel = std::max(0.1f, distance_) * (1.0f - std::pow(0.86f, steps));
            const auto previous = distance_;
            distance_ = std::max(0.01f, distance_ - travel);
            // Continue dollying through the pivot once the orbit radius reaches its minimum.
            const auto offset = cameraOffset({0.0f, 0.0f, previous - distance_ - travel});
            center_.x += offset.x;
            center_.y += offset.y;
            center_.z += offset.z;
        }
        Refresh(false);
    }
}

void ModelCanvas::considerMeshPick(const render::MeshSubmission& submission) {
    const auto& mesh = *submission.mesh;
    const auto& parameters = submission.parameters;
    const auto& projection = parameters.projection.values;
    const auto& viewport = pickingCamera_->viewport;
    const auto scale = GetContentScaleFactor();
    const model::Vec3 direction{
        static_cast<float>((2.0 * meshPick_->position.x * scale / viewport.width - 1.0) / projection[0]),
        static_cast<float>((1.0 - 2.0 * meshPick_->position.y * scale / viewport.height) / projection[5]),
        -1.0f};
    const auto nearPlane = pickingCamera_->nearClip;
    const model::Vec3 start{direction.x * nearPlane, direction.y * nearPlane, -nearPlane};
    model::Vec3 end{direction.x * meshPick_->depth, direction.y * meshPick_->depth, -meshPick_->depth};

    // Consume the posed drawing stream, including the separate CPU skin pools.
    // This runs only for a click, after the render owner has updated its pose.
    auto modelView = parameters.modelView;
    std::optional<model::EngineCpuSkinSource> cpuSource;
    std::vector<model::RenderVertex> cpuVertices;
    if (parameters.engineCpuSkin && parameters.material.k2PartTriMeshState) {
        cpuSource = model::decodeEngineCpuSkinSource(mesh);
        model::EngineCpuSkinPools pools;
        cpuVertices = pools.execute(mesh, *submission.deformed, *cpuSource, false, 0.0f);
        modelView = model::multiply(modelView, submission.deformed->skinPartFrame);
    }
    const bool k1Draw = !mesh.animated && parameters.material.k2PartTriMeshState &&
        parameters.material.engineGame == material::EngineMaterialGame::Kotor1;
    const auto& indices = cpuSource ? cpuSource->indices : k1Draw ? mesh.engineDrawIndices : mesh.indices;
    std::vector<model::Vec3> positions;
    positions.reserve(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& position = cpuSource ? cpuVertices[i].position :
            submission.deformed ? submission.deformed->positions[i] : mesh.vertices[i].position;
        positions.push_back(model::transformPoint(modelView, position));
    }
    bool hit = false;
    const bool clockwise = model::transformHandedness(modelView) < 0.0f;
    const auto triangleAt = [&](std::size_t a, std::size_t b, std::size_t c) {
        std::array<model::Vec3, 3> triangle{{positions[indices[a]], positions[indices[b]], positions[indices[c]]}};
        if (clockwise) std::swap(triangle[1], triangle[2]);
        if (engineHitTriangle(triangle, start, end)) hit = true;
        else if (!parameters.cullBackfaces) {
            std::swap(triangle[1], triangle[2]);
            if (engineHitTriangle(triangle, start, end)) hit = true;
        }
    };
    if (mesh.animated || cpuSource || k1Draw) {
        const auto& batches = cpuSource ? cpuSource->draws : mesh.engineDraws;
        for (const auto& batch : batches) {
            const auto first = batch.firstIndex;
            const auto limit = first + batch.indexCount;
            switch (batch.primitive) {
                case 3: // triangles
                    for (auto i = first; i + 2 < limit; i += 3) triangleAt(i, i + 1, i + 2);
                    break;
                case 4: // triangle strip
                    for (auto i = first; i + 2 < limit; ++i)
                        if ((i - first) % 2) triangleAt(i + 1, i, i + 2);
                        else triangleAt(i, i + 1, i + 2);
                    break;
                case 5: // triangle fan
                case 8: // polygon
                    for (auto i = first + 1; i + 1 < limit; ++i) triangleAt(first, i, i + 1);
                    break;
                case 6: // quads
                    for (auto i = first; i + 3 < limit; i += 4) {
                        triangleAt(i, i + 1, i + 2); triangleAt(i, i + 2, i + 3);
                    }
                    break;
                case 7: // quad strip
                    for (auto i = first; i + 3 < limit; i += 2) {
                        triangleAt(i, i + 1, i + 3); triangleAt(i, i + 3, i + 2);
                    }
                    break;
            }
        }
    } else {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) triangleAt(i, i + 1, i + 2);
    }
    if (hit && -end.z < meshPick_->depth) {
        meshPick_->depth = -end.z;
        meshPick_->areaOwner=submission.lightOwner;
        meshPick_->node.reset();
        const auto contains = [&](const model::Model& asset) {
            return std::any_of(asset.meshes.begin(), asset.meshes.end(),
                [&](const auto& candidate) { return &candidate == &mesh; });
        };
        if (contains(document_->model())) meshPick_->node = mesh.nodeIndex;
        else {
            const auto& references = document_->references();
            for (std::size_t i = 0; i < references.size(); ++i) {
                if (!contains(references[i].model)) continue;
                meshPick_->node = referenceSelectionNode(i);
                break;
            }
        }
    }
}

void ModelCanvas::onKey(wxKeyEvent& event) {
    advanceCamera();
    constexpr std::array<int, 6> keys{{'D', 'A', 'E', 'Q', 'S', 'W'}};
    const auto key = std::find(keys.begin(), keys.end(), event.GetKeyCode());
    if (event.ControlDown() || event.AltDown()) {
        flyMovement_.fill(false);
        event.Skip();
    } else if (event.GetKeyCode() == 'F') {
        if (event.GetEventType() == wxEVT_KEY_DOWN) {
            if (event.ShiftDown()) useViewerCamera(!freeFlyCameraActive());
            else frameModel();
        }
    } else if (freeFlyCameraActive() && key != keys.end()) {
        flyMovement_[key - keys.begin()] = event.GetEventType() == wxEVT_KEY_DOWN;
        Refresh(false);
    } else
        event.Skip();
}

void ModelCanvas::onFocusLost(wxFocusEvent& event) {
    selectionStart_.reset();
    resetCameraInput();
    event.Skip();
}

void ModelCanvas::onCaptureLost(wxMouseCaptureLostEvent&) {
    selectionStart_.reset();
    resetCameraInput();
}

void ModelCanvas::resetCameraInput() {
    cameraTick_ = std::chrono::steady_clock::now();
    flyMovement_.fill(false);
    draggingRotate_ = false;
    draggingPan_ = false;
    if (HasCapture()) ReleaseMouse();
}

void ModelCanvas::initializeGl() {
    if (glInitialized_) return;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_NORMALIZE);
    glShadeModel(GL_SMOOTH);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    std::string error;
    if (!meshRenderer_.initialize(resolveGlProcedure, error)) {
        wxLogError("NeoMDL mesh renderer: %s", wxString::FromUTF8(error.c_str()));
        shaderErrorReported_ = true;
    }
    textureCache_.setTextureDeletionCallback([this](render::gl::UInt texture) {
        meshRenderer_.forgetTexture(texture);
    });
    if(!textureCache_.initialize(resolveGlProcedure,error)) {
        wxLogError("NeoMDL texture upload: %s",wxString::FromUTF8(error.c_str()));
        shaderErrorReported_=true;
    }
    glInitialized_ = true;
}

void ModelCanvas::clearGpuTextures() {
    textureCache_.clear();
    std::string capabilityError;
    if (!textureCache_.setProcedureMipPath(options_.procedureMipPath,capabilityError))
        wxLogError("NeoMDL procedural capability: %s",wxString::FromUTF8(capabilityError.c_str()));
    pendingProcedureElapsed_=0;
    proceduralTextureError_.clear();
    renderQueue_.clear();
    staticMeshQueue_.clear();
    dynamicGobQueue_.clear();renderAnimationOwners_.clear();
    hologramQueue_.clear();
    distortionQueue_.clear();distortionProducerCount_=0;
    lightsaberQueue_.clear();
    diagnosticSkinBlurQueue_.clear();
    aabbDebugQueue_.clear();
}

neomdl::render::GpuTexture ModelCanvas::textureFor(
    const neoshared::material::TextureBinding& binding,std::size_t layer,const ModelDocument* resources) {
    const auto* source=resources?resources:document_;
    if(!source || binding.key.empty()) return {};
    const auto* texture=source->texture(binding.key);
    if(!texture || !texture->hasPixels()) return {};
    std::string error;
    const auto result=textureCache_.get(*texture,binding.metadata.get(),layer,error);
    if(result.volume) error="Engine Dirty volume has no 2D material consumer in this draw";
    if(!error.empty() && binding.metadata) {
        try { if(material::procedureDefinition(binding.metadata->txi,engineMaterialGame(source->model().game)).type)
            proceduralTextureError_=binding.key+": "+error; }
        catch(const std::exception&) {proceduralTextureError_=binding.key+": "+error;}
    }
    if(!error.empty() && !shaderErrorReported_) {
        wxLogWarning("NeoMDL texture %s: %s",wxString::FromUTF8(binding.key.c_str()),wxString::FromUTF8(error.c_str()));
        shaderErrorReported_=true;
    }
    return result.volume ? render::GpuTexture{} : result;
}

neomdl::render::GpuTexture ModelCanvas::textureForResref(
    const std::string& resref,const ModelDocument* resources) {
    const auto* source=resources?resources:document_;
    if (!source) return {};
    neoshared::material::TextureBinding binding;
    binding.resref = resref;
    binding.key = neoshared::material::textureResourceKey(resref);
    binding.metadata = source->textureMetadata(resref);
    return textureFor(binding,0,source);
}

void ModelCanvas::applyProjection(int width, int height) {
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    float viewAngle = 45.0f;
    const auto position = viewerCameraPosition();
    const auto& bounds = activeBounds();
    // Clipping follows the camera's distance to the scene, independently of the orbit pivot.
    const model::Vec3 gap{
        std::max({bounds.minimum.x - position.x, 0.0f, position.x - bounds.maximum.x}),
        std::max({bounds.minimum.y - position.y, 0.0f, position.y - bounds.maximum.y}),
        std::max({bounds.minimum.z - position.z, 0.0f, position.z - bounds.maximum.z})};
    const model::Vec3 reach{
        std::max(std::abs(position.x - bounds.minimum.x), std::abs(position.x - bounds.maximum.x)),
        std::max(std::abs(position.y - bounds.minimum.y), std::abs(position.y - bounds.maximum.y)),
        std::max(std::abs(position.z - bounds.minimum.z), std::abs(position.z - bounds.maximum.z))};
    float nearPlane = std::max(0.005f, std::hypot(gap.x, gap.y, gap.z) * 0.005f);
    float farPlane = std::max(1000.0f, std::hypot(reach.x, reach.y, reach.z) * 1.1f);
    if (dialogueCameraActive_&&dialogueCameraScene_.active()) {
        viewAngle=dialogueCameraFov_;
        nearPlane=kK2AnimatedCameraNearClip;farPlane=kK2AnimatedCameraFarClip;
    } else if(dialogueActorCameraActive_&&dialogueActorCamera_.frame()) {
        viewAngle=dialogueActorCamera_.frame()->authored.fieldOfView;
        nearPlane=options_.cameraSettings.nearClip;farPlane=options_.cameraSettings.farClip;
    } else if (cameraScene_.active()) {
        // Camera::RenderScene passes this angle directly to gluPerspective, so
        // it is a vertical angle in degrees. SetAnimatedCamera supplies the
        // external Camera with 0.1 / 10000 clipping distances.
        viewAngle = options_.cameraSettings.viewAngle;
        nearPlane = options_.cameraSettings.nearClip;
        farPlane = options_.cameraSettings.farClip;
    } else if(const auto* camera=cameraScene_.activeAreaCamera()) {
        // GIT placeable cameras store an authored vertical FOV but no clipping
        // tuple. Retain the viewer's explicit clip controls.
        viewAngle=camera->authored.fieldOfView;
        nearPlane=options_.cameraSettings.nearClip;
        farPlane=options_.cameraSettings.farClip;
    } else if(const auto* camera=activeAreaReplayCamera()) {
        viewAngle=camera->viewAngle;nearPlane=camera->nearClip;farPlane=camera->farClip;
    }
    sceneFarClip_ = farPlane;
    CameraHitState picking;picking.viewAngle=viewAngle;picking.nearClip=nearPlane;picking.farClip=farPlane;
    picking.viewport={0,0,width,height};pickingCamera_=picking;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // Camera::RenderScene 0x006e933d..0x006e9379 promotes its float inputs
    // to doubles. The embedded GLU call multiplies the float result, or leaves
    // the freshly loaded identity unchanged on its engine zero checks.
    std::array<float, 16> projection;
    if (neoshared::glu::perspective(viewAngle, aspect, nearPlane, farPlane,
                                   projection))
        glMultMatrixf(projection.data());
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

model::EngineFrame ModelCanvas::sceneCameraFrame() const {
    if(dialogueCameraActive_&&dialogueCameraScene_.active()) {
        const auto& camera=*dialogueCameraScene_.active();return {camera.position,camera.orientation};
    }
    if(dialogueActorCameraActive_&&dialogueActorCamera_.frame()) {
        const auto& camera=*dialogueActorCamera_.frame();return {camera.position,camera.orientation};
    }
    if(const auto* camera=cameraScene_.active())return {camera->position,camera->orientation};
    if(const auto* camera=cameraScene_.activeAreaCamera())return {camera->position,camera->orientation};
    // Orbit and explicitly captured replay cameras are matrix-authored host
    // inputs. Engine attached/placed cameras retain their quaternion above.
    if(const auto* camera=activeAreaReplayCamera())return engineRigidFrame(camera->world);
    return engineRigidFrame(inverseAreaReplayCamera(cameraView_));
}

void ModelCanvas::drawScene() {
    // Scene::Render snapshots timeGetTime for Force Sight independently of
    // animation pause or seeking. The viewer supplies its monotonic host clock.
    forceSightMilliseconds_=static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if(dialogueCameraActive_&&dialogueCameraScene_.active()) {
        glLoadMatrixf(dialogueCameraScene_.active()->view.values.data());
    } else if(dialogueActorCameraActive_&&dialogueActorCamera_.frame()) {
        glLoadMatrixf(dialogueActorCamera_.frame()->view.values.data());
    } else if (const auto* camera = cameraScene_.active()) {
        glLoadMatrixf(camera->view.values.data());
    } else if(const auto* camera=cameraScene_.activeAreaCamera()) {
        glLoadMatrixf(camera->view.values.data());
    } else if(const auto* camera=activeAreaReplayCamera()) {
        const auto view=inverseAreaReplayCamera(camera->world);glLoadMatrixf(view.values.data());
    } else {
        glRotatef(pitch_, 1.0f, 0.0f, 0.0f);
        glRotatef(yaw_, 0.0f, 1.0f, 0.0f);
        // Odyssey models are Z-up; orient them into the conventional viewer
        // Y-up frame only for the viewer's orbit/free-fly camera. Authored cameras use
        // their complete packed-driven position/orientation directly.
        glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
        const auto position = viewerCameraPosition();
        glTranslatef(-position.x, -position.y, -position.z);
    }

    drawGridAndAxes();
    if (!document_ || !document_->isOpen()) return;

    if (options_.cullBackfaces) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glDisable(GL_LIGHTING); // Mesh lighting is explicit shader state.

    renderQueue_.clear();
    staticMeshQueue_.clear();
    dynamicGobQueue_.clear();renderAnimationOwners_.clear();
    hologramQueue_.clear();
    distortionQueue_.clear();distortionProducerCount_=0;
    diagnosticSkinBlurQueue_.clear();
    lightsaberQueue_.clear();
    aabbDebugQueue_.clear();
    GLfloat view[16]{};
    glGetFloatv(GL_MODELVIEW_MATRIX,view);
    std::copy_n(view,16,cameraView_.values.begin());
    // Keep raw engine quaternions for authored/attached cameras. Only the
    // viewer's own orbit/replay matrix needs an explicit host conversion.
    const auto cameraFrame=sceneCameraFrame();
    pickingCamera_->position=cameraFrame.position;pickingCamera_->orientation=cameraFrame.orientation;
    updateWorldRoomSelection();
    lastScenePlanes_=currentTriggerFrustum();
    // Camera transform is orthonormal; inverse rotation = transpose.
    eyeToWorld_={{view[0],view[4],view[8],view[1],view[5],view[9],view[2],view[6],view[10]}};
    const auto& model = document_->model();
    const bool exactK2Scene = model.game == neomdl::model::GameVersion::Kotor2;
    const bool engineScene = !model::engineSceneStageOrder(model.game).empty();
    meshRenderer_.setForceSight(effectiveForceSight());
    // Scene::Render invokes its one-shot UpdateEmitters before
    // AurTextureMakeCurrent/ForceControllerUpdate, wind advancement, BSP and
    // RenderSinglePass. This ordering is observable because particles and
    // procedural texture controllers consume the same process RNG.
    grassError_.clear();particleError_.clear();
    const auto frameElapsed=pendingSceneElapsed_;pendingSceneElapsed_=0;
    auto sceneAnimation=particleScene_.sceneAnimation();
    sceneAnimation->beginRender(frameElapsed);
    struct RenderScope {
        SceneAnimation* scene;
        ~RenderScope(){if(scene)scene->endRender();}
        void finish(){scene->endRender();scene=nullptr;}
    } renderScope{sceneAnimation.get()};
    const auto doorElapsed=pendingDoorElapsed_;pendingDoorElapsed_=0;
    doorFrameElapsed_=doorElapsed;
    triggerFrameElapsed_=frameElapsed;
    const bool frameEmitterSceneAnimating=pendingEmitterSceneAnimating_&&sceneAnimation->isAnimating();
    pendingEmitterSceneAnimating_=false;
    auto emitterScenes=particleScenes();
    if(particleScene_.engineRuntime(model.game) && options_.previewEmitters) {
        SceneEmitterPrewarmTrace currentPrewarm;
        try {
            prewarmSceneEmitterBucket(emitterScenes,&currentPrewarm);
            // Preserve the last actual one-shot traversal for reports. A later
            // no-op frame must not erase the observable registration/RNG order.
            if(!currentPrewarm.order.empty() || !emitterPrewarmTrace_.completed)
                emitterPrewarmTrace_=std::move(currentPrewarm);
        } catch(const std::exception& failure) {
            emitterPrewarmTrace_=std::move(currentPrewarm);
            particleError_=failure.what();
        }
    }
    proceduralTextureError_.clear();
    textureCache_.setEngineRuntime(engineMaterialGame(model.game));
    textureCache_.context().random=[this] {return particleScene_.nextRuntimeRandom();};
    textureCache_.setDirtyAreaInputs(dirtyAreaState().controllerInputs(),
        (explicitDirtyArea_?explicitDirtyIdentity_:areaScene_.identity())+"#"+std::to_string(dirtyAreaEpoch_));
    std::vector<render::ProcedureRegistration> pendingProcedures;
    std::unordered_set<const neoshared::texture::TextureData*> registeredTextures;
    const auto registerDocument=[&](const ModelDocument& resource) {
        const auto registerTexture=[&](const std::string& name) {
            const auto* image=resource.texture(name);const auto metadata=resource.textureMetadata(name);
            if(!image || !metadata || !registeredTextures.insert(image).second)return;
            textureCache_.orderFor(*image);
            try {
                // Nonprocedural images keep their existing lazy GPU upload.
                const auto& definition=textureCache_.definitionFor(*image,metadata,engineMaterialGame(resource.model().game));
                if(!definition.type && !definition.volumeTexture)return;
                // Dormant Dirty images are not ordinary material inputs. Only
                // an explicit Desktop diagnostic constructs their 3D storage.
                if(definition.volumeTexture&&!dirtySettings_.enabled)return;
                pendingProcedures.push_back({image,metadata});
            } catch(const std::exception& error) {
                if(proceduralTextureError_.empty())proceduralTextureError_=name+": "+error.what();
            }
        };
        // Material::InitializeTextures (Desktop 0xf306a): authored texture slots,
        // then environment, bump and bumpy-shiny references. Registration must
        // not depend on alphabetically sorted resource diagnostics or visibility.
        const auto registerMaterial=[&](const neoshared::material::Material& m) {
            for(const auto* binding:{&m.diffuse,&m.lightmap,&m.environmentMap,&m.bumpMap,&m.bumpyShiny})
                registerTexture(binding->key);
        };
        for(std::size_t i=0;i<resource.model().meshes.size();++i)registerMaterial(resource.material(i));
        for(std::size_t j=0;j<resource.references().size();++j)
            for(std::size_t i=0;i<resource.references()[j].model.meshes.size();++i)
                registerMaterial(resource.referenceMaterial(j,i));
        // Standalone host registration for nonmaterial and explicitly requested
        // runtime resources. This is not a reassembly of a game's global
        // texture history or gameplay RNG seed.
        for(const auto& name:resource.textureLoadOrder())registerTexture(name);
    };
    registerDocument(*document_);
    for(const auto* scene:emitterScenes) {
        if(scene->document()&&scene!=&particleScene_)registerDocument(*scene->document());
        std::vector<const ParticleScene*> chunks;
        scene->appendAllocatedChunkScenes(chunks);
        // Pooled chunks retain their engine Parts/materials. A decoded chunk
        // asset alone owns no live texture/controller registration.
        for(const auto* child:chunks)registerDocument(*child->document());
    }
    std::string constructionError;
    if(!textureCache_.makeCurrent(pendingProcedures,constructionError) && proceduralTextureError_.empty())
        proceduralTextureError_=constructionError;
    std::string procedureFrameError;
    const auto procedureElapsed=pendingProcedureElapsed_;pendingProcedureElapsed_=0;
    if(!textureCache_.beginFrame(procedureElapsed,procedureFrameError) && proceduralTextureError_.empty())
        proceduralTextureError_=procedureFrameError;
    const auto& transforms = pose_ && pose_->worldTransforms.size() == model.nodes.size()
        ? pose_->worldTransforms : model.worldTransforms;
    const auto& mainBase = referenceScene_.mainWorld();
    const auto mainEngineParts = !exactK2Scene && pose_
        ? engineCollapsedHitPose(model,*pose_,referenceScene_.mainFrame()) : std::vector<HitPartTransform>{};
    const auto mainOwnerDistance=ownerSphereCameraDistance(mainAnimationOwner_.sphere(),
        referenceScene_.mainFrame(),sceneCameraFrame().position);
    const bool mainOwnerHasTransparency = exactK2Scene
        ? neomdl::model::ownerTreeHasTransparency(model, pose_ ? &*pose_ : nullptr)
        : ownerOrderingTransparency_.front();
    auto* mainQueue = &renderQueue_;
    std::uint64_t mainOwner = 0u;
    {
        if (exactK2Scene && (model.classification & neomdl::model::kK2ModelTile) != 0u &&
            !(exactK2Scene && options_.nearestLightsOnlyGobs && areaScene_.empty())) {
            mainQueue = &staticMeshQueue_;
        } else {
            mainQueue = &dynamicGobQueue_;
            mainOwner = 1u;
        }

        if(engineScene && !(exactK2Scene && options_.nearestLightsOnlyGobs && areaScene_.empty())) {
            const auto bucket=engineOwnerBucket(model,ownerEffects_[0]);
            if(bucket==EngineOwnerBucket::Hologram) {mainQueue=&hologramQueue_;mainOwner=1u;}
            else if(bucket==EngineOwnerBucket::Distortion) {mainQueue=&distortionQueue_;mainOwner=1u;}
            else if(bucket==EngineOwnerBucket::Lightsaber) {mainQueue=&lightsaberQueue_;mainOwner=1u;}
        }

        // Material::InitializeTextures runs while parts are created, before
        // visibility decides which parts enter a scene bucket. Register every
        // decoded diffuse texture in model order so a hidden first use cannot
        // move a later visible texture behind another static bucket.
        for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
            const auto& diffuse = document_->material(meshIndex).diffuse;
            if (diffuse.key.empty()) continue;
            const auto* texture = document_->texture(diffuse.key);
            if (texture && texture->hasPixels()) textureCache_.orderFor(*texture);
        }
    }

    configurePreviewLights(transforms);
    try {
        areaScene_.collectGrass(pickingCamera_->position,currentTriggerFrustum());
    } catch(const std::exception& error) {grassError_=error.what();}
    particleScene_.finishWindFrame(frameElapsed);
    try {
        const auto hardware=material::selectK2HardwareCapabilities(options_.engineHardware);
        // K1 RenderGrassPolys returns before UpdateGrassForWind when both
        // NV1x-level shaders and multitexture are unavailable (0x316d6e).
        if(grassError_.empty()&&(model.game!=model::GameVersion::Kotor1||
            hardware.nv1xLevelShaders||hardware.multitexture))
            areaScene_.advanceGrassWind(frameElapsed);
    }
    catch(const std::exception& error){grassError_=error.what();}
    updateAnimationVisibility();
    if(!grassError_.empty() && !shaderErrorReported_) {
        wxLogWarning("NeoMDL grass: %s",wxString::FromUTF8(grassError_.c_str()));shaderErrorReported_=true;
    }
    if(engineScene && !(exactK2Scene&&options_.nearestLightsOnlyGobs&&areaScene_.empty()) &&
       shadowScene_.focusVisible(areaScene_,currentTriggerFrustum())) {
        render::MeshSubmission focus;focus.ownerOnly=true;focus.drawPrimary=false;
        focus.sceneOwner=ShadowScene::focusContainer+1u;focus.lightOwner=ShadowScene::focusContainer;
        const auto p=shadowScene_.sceneFocus();
        focus.ownerCameraDistance=engineOwnerCameraDistance(HitSphere{{},1.0f},p,
            model::Quaternion{},pickingCamera_->position);
        dynamicGobQueue_.push_back(std::move(focus));
    }
    queueAreaRooms();
    const bool mainVisible = document_->hasModelOwner()&&engineOwnerAdmitted(mainLightOwner,model.classification);
    if(!exactK2Scene && mainVisible)
        mainQueue->push_back(ownerOrderingSubmission(mainOwner,model,
            mainOwnerHasTransparency,mainOwnerDistance,ownerEffects_[0].colorShift[3],
            model::multiply(cameraView_,mainBase)));
    if(mainVisible && mainQueue==&distortionQueue_)++distortionProducerCount_;
    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        if (!mainVisible) break;
        const auto& mesh = model.meshes[meshIndex];
        if (mesh.nodeIndex >= transforms.size() ||
            mesh.nodeIndex >= model.nodes.size()) {
            continue;
        }
        const auto& meshNode = model.nodes[mesh.nodeIndex];
        if (!meshVisible(mesh.nodeIndex)) continue;
        const bool selected = selectedNodes_.count(mesh.nodeIndex)!=0;
        // K1 Desktop PartAABB::Draw 0x34b4dc shares the specialized four-pass
        // debug consumer; it never submits ordinary textured mesh geometry.
        if (engineScene &&
            (meshNode.flags & neomdl::model::kNodeAabb) != 0u) {
            queueAabbDebug(mesh, meshNode,
                           neomdl::model::multiply(mainBase, transforms[mesh.nodeIndex]),
                           selected, true);
            continue;
        }
        // K2 MdlNodeTrigger::InternalCreateInstance returns null. Even when a
        // malformed/combined node also carries mesh data, the engine does not
        // create an ordinary drawable part for the trigger node.
        if (model.game == neomdl::model::GameVersion::Kotor2 &&
            meshNode.trigger && meshNode.trigger->packedVerified) {
            continue;
        }
        if (!mesh.render && !options_.showHidden) continue;
        // A postprocessed lightsaber is a specialized PartLightsaber,
        // not an ordinary TriMesh fallback. lightsaberrender=0 hides the part.
        if (meshNode.lightsaber && meshNode.lightsaber->postprocessed) continue;
        const neomdl::model::DeformedMesh* deformed = nullptr;
        if ((options_.danglySimulation || !mesh.dangly) && pose_ &&
            meshIndex < pose_->deformedMeshes.size()) {
            const auto& candidate = pose_->deformedMeshes[meshIndex];
            if (candidate.valid && candidate.positions.size() == mesh.vertices.size() &&
                candidate.normals.size() == mesh.vertices.size()) {
                deformed = &candidate;
            }
        }
        glPushMatrix();
        glMultMatrixf(mainBase.values.data());
        // ExecuteSkinTransforms produces model-space positions. Applying the
        // mesh-node transform here would double-transform a successful skin.
        if (!deformed) glMultMatrixf(transforms[mesh.nodeIndex].values.data());
        const auto materialState = pose_ && mesh.nodeIndex < pose_->nodeMaterials.size()
            ? pose_->nodeMaterials[mesh.nodeIndex]
            : neomdl::model::bindNodeMaterial(model.nodes[mesh.nodeIndex]);
        drawMesh(mesh, deformed, selected, document_->material(meshIndex), materialState,
            model.game, model.fog,
            mainQueue == &staticMeshQueue_ ? AreaScene::sceneDynamicLightOwner : mainLightOwner,
            nullptr, -1.0f, mainQueue, mainOwner, 1.0f,
            model.classification, mainOwnerHasTransparency,
            mainOwnerDistance, true, engineScene ? &ownerEffects_[0] : nullptr, &mainBase,
            {}, mainEngineParts.empty()?nullptr:&mainEngineParts[mesh.nodeIndex]);
        glPopMatrix();
    }
    drawReferenceModels();
    queueChunkModels();
    queueLightsabers();

    if(exactK2Scene) {
        // Engine Gob::Render still runs for a visible owner containing only
        // dummy/specialized parts, or no rendered TriMeshes. Keep that event.
        std::set<std::uint64_t> represented;
        for(const auto* queue:{&dynamicGobQueue_,&hologramQueue_,&distortionQueue_})
            for(const auto& submission:*queue)represented.insert(submission.sceneOwner);
        for(const auto& owner:motionBlurScene_.owners()) {
            if(!engineOwnerAdmitted(owner.id,owner.asset->classification) ||
               (owner.id==mainLightOwner && (!mainOwner||!mainVisible)) || represented.count(owner.id+1u))continue;
            render::MeshSubmission empty;empty.ownerOnly=true;empty.drawPrimary=false;
            empty.sceneOwner=owner.id+1u;empty.lightOwner=owner.id;
            empty.ownerLifetime=owner.ownerLifetime;
            if(owner.resolveWorld)empty.resolveOwner=[this,world=owner.resolveWorld,id=owner.id,animation=owner.animationOwner](auto& live) {
                const auto frame=world();live.parameters.modelView=model::multiply(cameraView_,frame);
                ownerLightSpheres_[id]={model::transformPoint(frame,{}),animation->sphere().radius};
            };
            empty.ownerModelClassification=owner.asset->classification;
            empty.ownerAlpha=1;
            empty.parameters.material.k2PartTriMeshState=true;
            empty.parameters.modelView=model::multiply(cameraView_,owner.world);
            empty.ownerCameraDistance=ownerSphereCameraDistance(owner.animationOwner->sphere(),
                owner.frame,sceneCameraFrame().position);
            auto* queue=&dynamicGobQueue_;
            if(owner.id<ownerEffects_.size()) {
                const auto& fx=ownerEffects_[static_cast<std::size_t>(owner.id)];
                if(fx.hologram)queue=&hologramQueue_;
                else if(fx.distortion)queue=&distortionQueue_;
            }
            queue->push_back(std::move(empty));
        }
    }
    // A engine Gob remains in its sorted bucket while Animate updates its
    // Parts. Borrow its lifetime, then resolve the current Part at draw time.
    for(auto* queue:{&dynamicGobQueue_,&hologramQueue_,&distortionQueue_,&lightsaberQueue_})
        for(auto& submission:*queue)if(submission.sceneOwner) {
            const auto id=submission.sceneOwner-1u;
            if(auto* owner=animationOwnerForId(id)) {
                renderAnimationOwners_[id]=owner;
                if(!submission.ownerLifetime)submission.ownerLifetime=owner->lifetime();
                if(!submission.resolveOwner) {
                    const AreaObject* retainedObject=nullptr;std::optional<std::size_t> retainedReference;
                    for(const auto& object:areaScene_.objects()) {
                        if(id==object->ownerId()){retainedObject=object.get();break;}
                        for(std::size_t i=0;i<object->references().instances().size();++i)
                            if(id==object->ownerId(i)){retainedObject=object.get();retainedReference=i;break;}
                        if(retainedObject)break;
                    }
                    submission.resolveOwner=[this,id,retainedObject,retainedReference](auto& live) {
                        refreshOwnerSubmission(live,id,retainedObject,retainedReference);
                    };
                }
            }
        }
    const auto synchronizeAnimatedOutputs=[&] {
        synchronizeEngineQueries();
        lightScene_.synchronize(*pose_,options_.showReferences);
        if(!areaScene_.empty())areaScene_.synchronize(lightScene_,lastScenePlanes_);
        lightScene_.synchronizeChunks(&particleScene_,areaScene_,options_.showReferences);
        std::vector<bool> referencesAnimating(referenceScene_.instances().size());
        for(std::size_t i=0;i<referenceScene_.instances().size();++i) {
            const auto& reference=referenceScene_.instances()[i];
            referencesAnimating[i]=options_.previewEmitters&&reference.active&&reference.sceneMember&&reference.isAnimating();
        }
        particleScene_.prepareFrame(*pose_,frameElapsed,options_.showReferences,
            options_.previewEmitters&&mainAnimationOwner_.isAnimating(),referencesAnimating);
        AreaFrameSettings settings;
        settings.animate=true;settings.dangly=options_.danglySimulation;
        settings.emitters=options_.previewEmitters;settings.references=options_.showReferences;
        areaScene_.advanceEffects(frameElapsed,settings);
        lightsaberScene_.refreshTransforms(*pose_,options_.showReferences);
        for(auto& event:areaScene_.takeWorldAnimationEvents())
            dialogueMessages_.push_back("Track "+std::to_string(event.track)+" requests script slot "+
                std::to_string(event.scriptSlot)+" from owner "+std::to_string(event.source)+": "+event.argument);
    };
    std::string queueError;
    bool bucketOkay=true;
    const auto drawQueued = [&](std::vector<render::MeshSubmission>& queue,
                                render::QueueOrdering ordering,
                                const char* label) {
        if (!meshRenderer_.ready() || queue.empty()) return;
        if(!proceduralTextureError_.empty()) {
            if(!shaderErrorReported_)wxLogError("NeoMDL procedural texture: %s",wxString::FromUTF8(proceduralTextureError_.c_str()));
            shaderErrorReported_=true;queueError=proceduralTextureError_;bucketOkay=false;return;
        }
        for(auto& submission:queue)submission.prepareTextures=[this](auto& parameters,auto& error) {
            return textureCache_.bind(parameters,error);
        };
        queueError.clear();
        const bool ok=meshRenderer_.drawQueue(queue, queueError, ordering,
                [this](const auto& submission,auto& parameters,auto& error) {
                    return prepareSubmission(submission,parameters,error);
                }, engineScene && (&queue==&dynamicGobQueue_ || &queue==&hologramQueue_ || &queue==&distortionQueue_)
                    ? render::OwnerDrawCompletion{[this](const auto& submissions,const auto& indices,auto& error) {
                        if(submissions[indices.front()].sceneOwner==ShadowScene::focusContainer+1u)return true;
                        return motionBlurQueue_.record(motionBlurScene_,submissions,indices,error);
                    }} : render::OwnerDrawCompletion{},
                engineScene && &queue!=&diagnosticSkinBlurQueue_
                    ? render::OwnerDrawBegin{[this,doorElapsed](const auto& submission,auto& error) {
                        if(!sceneQueryError_.empty()){error=sceneQueryError_;return false;}
                        if(submission.ownerModelClassification&8u)engineDoorRenderAlpha(sceneQueries_,submission.sceneOwner-1u,
                            pickingCamera_->position,worldPlaybackSettings_.sceneFocus,
                            doorElapsed,worldPlaybackSettings_.doorFade);
                        return true;
                    }} : render::OwnerDrawBegin{},
                ordering==render::QueueOrdering::K1DynamicGobs||
                ordering==render::QueueOrdering::K2DynamicGobs||
                ordering==render::QueueOrdering::K2HologramGobs
                    ? render::OwnerDrawBegin{[this](const auto& submission,auto&) {
                        animateRenderOwner(submission.sceneOwner-1u);return true;
                    }} : render::OwnerDrawBegin{});
        bucketOkay=bucketOkay && ok;
        if(!ok && !shaderErrorReported_) {
            wxLogError("NeoMDL %s: %s", wxString::FromUTF8(label),
                       wxString::FromUTF8(queueError.c_str()));
            shaderErrorReported_ = true;
        }
    };

    if (exactK2Scene && (options_.nearestLightsOnlyGobs && areaScene_.empty())) {
        // A skipped scene pass cannot bank elapsed time for a future flare.
        pendingFlareElapsed_=0;meshRenderer_.invalidateSceneHistory();
        // RenderOnlyGobs traverses registered Gobs, not the material/alpha
        // buckets. Room meshes, emitters and the full-scene effect stages do
        // not participate in this alternate path.
        drawQueued(dynamicGobQueue_, render::QueueOrdering::Submission,
                   "K2 nearest-light Gob list");
        discardSceneEmitterFrame(emitterScenes);
    } else if (engineScene) {
        // State belongs to the scene, not individual sorted queues. Per-draw
        // guards restore this engine state; the outer executor restores the
        // embedding application's state before the inspection overlays below.
        render::EngineSceneSettings sceneSettings;
        sceneSettings.game=model.game;
        sceneSettings.emittersEnabled=options_.previewEmitters;
        sceneSettings.sceneFog=effectiveFog();
        sceneSettings.effects=sceneEffects_;
        const auto engineHardware=material::selectK2HardwareCapabilities(options_.engineHardware);
        sceneSettings.effects.desktopArbShaders=engineHardware.arbFragmentPrograms;
        sceneSettings.effects.forceSight=effectiveForceSight().enabled;
        sceneSettings.effects.enabled=sceneSettings.effects.enabled &&
            (engineHardware.atiFrameBufferModifications ||
             (exactK2Scene&&engineHardware.frameBufferModifications));
        if(options_.useAreaReplayCamera&&!areaScene_.empty()) {
            const auto epoch=areaScene_.environment().cameraHistoryEpoch();
            sceneSettings.effects.historyEpoch^=epoch+0x9e3779b97f4a7c15ull+
                (sceneSettings.effects.historyEpoch<<6u)+(sceneSettings.effects.historyEpoch>>2u);
        }
        sceneSettings.distortionProducerCount=distortionProducerCount_;
        sceneSettings.bindEffectTexture=[this](SceneEffectTexture role,std::string& error)->render::gl::UInt {
            const auto& name=sceneEffectTextureName(sceneEffects_,role);
            const auto key=neoshared::material::textureResourceKey(name);
            const auto* image=document_->texture(key);const auto meta=document_->textureMetadata(key);
            if(!image||!image->hasPixels()||!meta) {error="Unresolved scene-effect texture: "+name;return 0;}
            const auto registered=textureCache_.get(*image,meta.get(),0,error);
            if(!error.empty())return 0;
            if(!registered.id||registered.cube||registered.volume) {error="Engine scene effects need a 2D texture: "+name;return 0;}
            // Each engine helper/overlay invokes virtual Bind, even when its
            // selected shader does not read that unit. Do not cache the result.
            return textureCache_.bind(registered.id,error).id;
        };
        sceneSettings.shadowsEnabled=options_.engineShadows.value_or(areaScene_.currentRoom().has_value())&&effectiveAreaShadows();
        sceneSettings.sceneAmbient=effectiveSceneAmbient();
        sceneSettings.triggersEnabled=options_.previewTriggers;
        sceneSettings.grassEnabled=areaScene_.grass().settings().enabled;
        sceneSettings.fogEnabled=effectiveFog().mode!=model::FogMode::Disabled;
        std::string sceneError;
        const bool sceneOkay=meshRenderer_.drawEngineScene(sceneSettings,
            [&](model::K2SceneStage stage,render::EngineSceneStageResult& result,std::string& error) {
            bucketOkay=true;queueError.clear();
            switch (stage) {
            case neomdl::model::K2SceneStage::FogSetup:
                break;
            case neomdl::model::K2SceneStage::StaticMeshGeometry:
                if(k2Lights_&&options_.previewLights)selectSceneDynamicLights();
                drawQueued(staticMeshQueue_,
                           render::QueueOrdering::StaticMeshes,
                           "engine static mesh buckets");
                break;
            case neomdl::model::K2SceneStage::StaticTriangleBins: {
                if(!grassError_.empty()){error=grassError_;return false;}
                if(!areaScene_.grass().settings().enabled)break;
                render::EngineGrassRenderSettings cfg;
                cfg.game=areaScene_.game();cfg.hardware=material::selectK2HardwareCapabilities(options_.engineHardware);
                cfg.forceSight=effectiveForceSight().enabled;cfg.desaturation=effectiveForceSight().desaturation;
                if(k2Lights_&&options_.previewLights) {
                    render::MeshDrawParameters fixed;
                    fixed.material.engineGame=cfg.game==model::GameVersion::Kotor1
                        ? material::EngineMaterialGame::Kotor1 : material::EngineMaterialGame::Kotor2;
                    render::appendK2Lights(lightScene_.illuminationLights(AreaScene::sceneDynamicLightOwner,
                        LightColorSource::FixedFunction),cameraView_,fixed,LightColorSource::FixedFunction);
                    cfg.fixedLights.assign(fixed.lights.begin(),fixed.lights.begin()+fixed.lightCount);
                }
                cfg.view=cameraView_;glGetFloatv(GL_PROJECTION_MATRIX,cfg.projection.values.data());cfg.fog=effectiveFog();
                cfg.restoredAlphaCutoff=0;
                if(!meshRenderer_.drawEngineGrass(areaScene_.grass(),cfg,
                    [this](const GrassBin& bin,const std::string& name,unsigned,std::string& message)->render::gl::UInt {
                        const auto* image=bin.document->texture(name);const auto meta=bin.document->textureMetadata(name);
                        if(!image||!meta){message="Unresolved grass texture: "+name;return 0;}
                        const auto texture=textureCache_.get(*image,meta.get(),0,message);
                        if(!message.empty())return 0;
                        if(!texture.id||texture.cube||texture.volume){message="Grass requires a engine 2D texture: "+name;return 0;}
                        return textureCache_.bind(texture.id,message).id;
                    },error))return false;
                break;
            }
            case neomdl::model::K2SceneStage::TriggerGeometry:
                drawTriggerBucket();
                break;
            case neomdl::model::K2SceneStage::TriggerHighlightBeforeDynamic:
                result.highlightVolumeDrawn=drawTriggerHighlights(false);
                break;
            case neomdl::model::K2SceneStage::Shadows:
                drawEngineShadows();
                break;
            case neomdl::model::K2SceneStage::DynamicGobs:
                drawQueued(dynamicGobQueue_,
                           exactK2Scene ? render::QueueOrdering::K2DynamicGobs : render::QueueOrdering::K1DynamicGobs,
                           "engine dynamic Gob bucket");
                // Defensive mixed-game fallback; normal K2 sessions leave the
                // legacy queue empty.
                drawQueued(renderQueue_, render::QueueOrdering::Material,
                           "legacy mesh queue");
                sceneAnimation->animateRemainingForRender();
                synchronizeAnimatedOutputs();
                break;
            case neomdl::model::K2SceneStage::HologramGobs:
                drawQueued(hologramQueue_,render::QueueOrdering::K2HologramGobs,
                           "K2 hologram Gob bucket");
                break;
            case neomdl::model::K2SceneStage::PersonalSpace:
            case neomdl::model::K2SceneStage::Paths:
                break;
            case neomdl::model::K2SceneStage::TriggerHighlightAfterDynamic:
                result.highlightVolumeDrawn=drawTriggerHighlights(true);
                break;
            case neomdl::model::K2SceneStage::Emitters:
                if(!queueParticles(frameElapsed,frameEmitterSceneAnimating,error))
                    return false;
                break;
            case neomdl::model::K2SceneStage::Lightsabers:
                drawLightsaberBucket();
                break;
            case neomdl::model::K2SceneStage::DrawableSquares:
                break;
            case neomdl::model::K2SceneStage::MotionBlur:
                if(!render::drawMotionBlurOwners(meshRenderer_,motionBlurScene_,motionBlurQueue_,
                    [this](const auto& submission,auto& parameters,auto&) {
                        parameters.material.blending=true;
                        if(submission.sceneOwner&&(submission.ownerModelClassification&8u))
                            render::applyEngineDoorAlpha(parameters,sceneQueries_.owner(submission.sceneOwner-1u)->doorAlpha);
                        if(k2Lights_&&options_.previewLights&&parameters.lighting) {
                            parameters.lightCount=submission.inspectionLightCount;
                            render::appendK2OwnerLights(lightScene_,AreaScene::sceneDynamicLightOwner,cameraView_,parameters);
                        }
                        return true;
                    },[this](const auto& draw,auto& message) {
                        if(!k2Lights_||!options_.previewLights)return true;
                        const auto sphere=ownerLightSpheres_.find(draw.owner);
                        if(sphere==ownerLightSpheres_.end()) {message="Missing blur-owner light sphere";return false;}
                        LightOwnerQuery query;query.source=areaScene_.empty()?LightQuerySource::Global:LightQuerySource::Room;
                        query.submission=LightSubmissionMode::Scene;query.sphere=sphere->second;
                        lightScene_.selectSceneContainer(AreaScene::sceneDynamicLightOwner,query);
                        return true;
                    },error,frameElapsed))return false;
                // Optional manually supplied vector remains an explicitly
                // separate inspection diagnostic, not the engine owner list.
                drawQueued(diagnosticSkinBlurQueue_, render::QueueOrdering::Submission,
                           "manual skin blur diagnostic");
                break;
            case neomdl::model::K2SceneStage::Distortion:
                drawQueued(distortionQueue_,render::QueueOrdering::Submission,"engine distortion Gob bucket");
                break;
            case neomdl::model::K2SceneStage::FramebufferEffects:
                // The renderer executes the engine capture/pass graph.
                break;
            case neomdl::model::K2SceneStage::LensFlares:
                result.lensFlareCallerRan=drawEngineLensFlares();
                break;
            case neomdl::model::K2SceneStage::FramebufferEffects2:
                break;
            }
            if(!bucketOkay)error=queueError;
            return bucketOkay;
        },sceneError);
        if(!emitterFrameTrace_.completed)discardSceneEmitterFrame(emitterScenes);
        if(!sceneOkay && !shaderErrorReported_) {
            wxLogError("NeoMDL engine scene: %s",wxString::FromUTF8(sceneError.c_str()));
            shaderErrorReported_=true;
        }
    } else {
        // The K1 mesh buckets share static texture ordering, but its owning
        // Gob comparator has a different classification sequence and one
        // reverse traversal. The full K2 scene-effects executor stays separate.
        drawQueued(renderQueue_,
                   render::QueueOrdering::Material, "scene queue");
        drawQueued(staticMeshQueue_, render::QueueOrdering::StaticMeshes,
                   "K1 static mesh queue");
        drawTriggerBucket();
        drawTriggerHighlights(false);
        drawQueued(dynamicGobQueue_, render::QueueOrdering::K1DynamicGobs,
                   "K1 dynamic Gob queue");
        sceneAnimation->animateRemainingForRender();
        synchronizeAnimatedOutputs();
        drawTriggerHighlights(true);
        std::string emitterError;
        if(!queueParticles(frameElapsed,frameEmitterSceneAnimating,emitterError) &&
           !shaderErrorReported_) {
            wxLogError("NeoMDL emitter queue: %s",
                       wxString::FromUTF8(emitterError.c_str()));
            shaderErrorReported_=true;
        }
        drawLightsaberBucket();
    }

    renderScope.finish();
    // These inspection aids are NeoMDL overlays rather than K2 scene buckets.
    drawMeshProjectionInspection();
    drawAabbDebugQueue();
    drawNodeHelpers(transforms);
    drawAreaCameraGlyphs();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    lightScene_.endSelectionFrame();
}

void ModelCanvas::drawGridAndAxes() {
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    const auto& bounds = activeBounds();
    if (options_.grid.value_or(areaScene_.empty())) {
        const float extent = bounds.valid
            ? std::max(10.0f, maximumExtent(bounds) * 1.5f) : 10.0f;
        const float step = std::pow(10.0f, std::floor(std::log10(std::max(0.1f, extent / 10.0f))));
        glColor3f(0.28f, 0.30f, 0.34f);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        for (float value = -extent; value <= extent + step * 0.5f; value += step) {
            glVertex3f(value, -extent, 0.0f);
            glVertex3f(value, extent, 0.0f);
            glVertex3f(-extent, value, 0.0f);
            glVertex3f(extent, value, 0.0f);
        }
        glEnd();
    }
    if (options_.axes.value_or(areaScene_.empty())) {
        const float length = bounds.valid
            ? std::max(1.0f, maximumExtent(bounds) * 0.35f) : 2.0f;
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glColor3f(0.9f, 0.2f, 0.2f); glVertex3f(0.0f, 0.0f, 0.0f); glVertex3f(length, 0.0f, 0.0f);
        glColor3f(0.2f, 0.9f, 0.2f); glVertex3f(0.0f, 0.0f, 0.0f); glVertex3f(0.0f, length, 0.0f);
        glColor3f(0.2f, 0.45f, 1.0f); glVertex3f(0.0f, 0.0f, 0.0f); glVertex3f(0.0f, 0.0f, length);
        glEnd();
        glLineWidth(1.0f);
    }
}

void ModelCanvas::setMotionBlurOwner(LightOwnerId owner,MotionBlurOwnerSettings settings) {
    motionBlurScene_.setSettings(owner,settings);shaderErrorReported_=false;Refresh(false);
}
void ModelCanvas::resetMotionBlurHistory() {pendingMotionHistoryReset_=true;Refresh(false);}
void ModelCanvas::synchronizeShadowCasters() {
    motionBlurQueue_.clear();
    if(!document_||!pose_||
       model::engineSceneStageOrder(document_->model().game).empty()) {
        shadowScene_.replaceCasters({});motionBlurScene_.clear();return;
    }
    auto owners=collectSceneOwners(*document_,*pose_,referenceScene_,areaScene_,
        options_.showReferences,options_.objectLightRadius,&danglyState_,&particleScene_,&mainAnimationOwner_);
    motionBlurScene_.replaceOwners(owners);
    for(const auto& object:areaScene_.objects()) if(!object->staticPrimary())
        motionBlurScene_.setSettings(object->ownerId(),object->motionBlurSettings());
    if(pendingMotionHistoryReset_) {motionBlurScene_.resetHistory();pendingMotionHistoryReset_=false;}
    shadowScene_.replaceCasters(std::move(owners));
}

void ModelCanvas::configurePreviewLights(const std::vector<neomdl::model::Mat4>&) {
    sceneLightsSelectedThisFrame_ = false;
    k2Lights_ = document_ && document_->isOpen() && pose_ &&
        !model::engineSceneStageOrder(document_->model().game).empty();
    const auto previousOwners = std::move(ownerLightSpheres_);
    ownerLightSpheres_.clear();
    std::vector<LightOwnerId> retainedReferenceOwners;
    if (k2Lights_) {
        // SetScene(nullptr) unlists a reference Gob without destroying its
        // light container. Preserve it until the owning reference is released.
        for(std::size_t i=0;i<referenceScene_.instances().size();++i)
            retainedReferenceOwners.push_back(i+1u);
        for(const auto& object:areaScene_.objects())
            for(std::size_t i=0;i<object->references().instances().size();++i)
                retainedReferenceOwners.push_back(object->ownerId(i));
        lightScene_.synchronize(*pose_, options_.showReferences);
        lightScene_.synchronizeChunks(&particleScene_,areaScene_,options_.showReferences);
        if (!areaScene_.empty()) {
            const auto planes = currentTriggerFrustum();
            areaScene_.synchronize(lightScene_,planes);
            const auto rooms=areaScene_.emitterRooms(planes);
            particleScene_.setEmitterRooms(rooms);
            for(auto* scene:areaScene_.particleSceneRoots())scene->setEmitterRooms(rooms);
        }
        const auto registerOwner = [this](LightOwnerId id,const model::Mat4& world,
                const SceneAnimationOwner& animationOwner) {
            // DoGobBuckets queries the Gob origin and its runtime radius
            // (K1 +0x138, K2 +0x164), independently of the rendering AABB.
            ownerLightSpheres_[id] = {model::transformPoint(world,{}),animationOwner.sphere().radius};
            if (!areaScene_.empty()) {
                auto query = lightScene_.ownerQuery(id).value_or(LightOwnerQuery{});
                query.source = LightQuerySource::Gob;
                lightScene_.setOwnerQuery(id,query);
                if (std::find(areaOwnerQueries_.begin(),areaOwnerQueries_.end(),id)==areaOwnerQueries_.end())
                    areaOwnerQueries_.push_back(id);
            }
        };
        for(const auto& object:areaScene_.objects()) {
            if(!object->staticPrimary()) registerOwner(object->ownerId(),object->world(),object->animationOwner());
            if(options_.showReferences) for(std::size_t i=0;i<object->references().instances().size();++i) {
                const auto& ref=object->references().instances()[i];
                if(ref.active&&ref.sceneMember)registerOwner(object->ownerId(i),ref.world,ref.animationOwner);
            }
        }
        if(document_->hasModelOwner())registerOwner(mainLightOwner,referenceScene_.mainWorld(),mainAnimationOwner_);
        if (options_.showReferences) for (std::size_t i=0;i<document_->references().size();++i) {
            const auto* reference = referenceScene_.instance(i);
            if (reference && reference->active && reference->sceneMember) registerOwner(i+1u,reference->world,reference->animationOwner);
        }
        for(const auto& owner:collectParticleChunkOwners(&particleScene_,areaScene_,options_.showReferences)) {
            ownerLightSpheres_[owner.id]=owner.lightSphere;
            auto query=lightScene_.ownerQuery(owner.id).value_or(LightOwnerQuery{});
            query.source=LightQuerySource::Gob;lightScene_.setOwnerQuery(owner.id,query);
        }
    }
    for (const auto& previous : previousOwners)
        if (ownerLightSpheres_.find(previous.first) == ownerLightSpheres_.end()&&
            !lightScene_.chunkOwnerAllocated(previous.first)&&
            std::find(retainedReferenceOwners.begin(),retainedReferenceOwners.end(),previous.first)==retainedReferenceOwners.end())
            lightScene_.removeOwner(previous.first);
    synchronizeShadowCasters();
    lightScene_.beginSelectionFrame(pendingLightElapsed_);
    shadowScene_.setSceneFocus(worldPlaybackSettings_.sceneFocus);
    if(k2Lights_)shadowScene_.retainFocusContainer(lightScene_);
    if(options_.engineShadows.value_or(areaScene_.currentRoom().has_value()))shadowScene_.retainContainer(lightScene_);
    for (const auto& owner : ownerLightSpheres_)
        lightScene_.retainOwnerForFrame(owner.first);
    for(const auto owner:retainedReferenceOwners)
        lightScene_.retainOwnerForFrame(owner);
    if (!areaScene_.empty())
        lightScene_.retainOwnerForFrame(AreaScene::sceneDynamicLightOwner);
    pendingLightElapsed_ = 0.0f;
}

bool ModelCanvas::synchronizeEngineQueries() {
    if(!document_||!document_->isOpen()||!pose_) {
        particleScene_.setSceneQueries(nullptr);areaScene_.setQueries(nullptr);sceneQueries_.clear();sceneQueryError_.clear();return false;
    }
    try {
        sceneQueries_.synchronize(*document_,inspectionReplayPose_?*inspectionReplayPose_:*pose_,referenceScene_,areaScene_,triggerScene_,mainAnimationOwner_,&particleScene_);
        // Engine SetColorShifting and EnableDistortion write the same Gob
        // fields consumed by HitCheck, independently of the render toggles.
        for(std::size_t i=0;i<ownerEffects_.size();++i)if(auto* owner=sceneQueries_.owner(i)) {
            owner->alpha=ownerEffects_[i].colorShift[3];
            owner->ignoreHitCheck=ownerEffects_[i].distortion;
        }
        sceneQueryError_.clear();particleScene_.setSceneQueries(&sceneQueries_);areaScene_.setQueries(&sceneQueries_);return true;
    } catch(const std::exception& e) {
        // Stale geometry must not be mistaken for a valid occlusion world.
        sceneQueryError_=e.what();particleScene_.setSceneQueries(nullptr);areaScene_.setQueries(nullptr);sceneQueries_.clear();return false;
    }
}

bool ModelCanvas::drawEngineLensFlares() {
    const float elapsed=pendingFlareElapsed_;pendingFlareElapsed_=0;
    if(!options_.engineLensFlares.value_or(!areaScene_.empty())||!options_.previewLights||!document_||!pose_)return false;
    const auto diagnostic=[this](const std::string& text) {
        if(!shaderErrorReported_) {
            wxLogWarning("NeoMDL engine lens flares: %s",wxString::FromUTF8(text.c_str()));
            shaderErrorReported_=true;
        }
    };
    if(areaScene_.empty()) {diagnostic("Load area context for the engine room flare collection");return false;}
    if(!synchronizeEngineQueries()) {diagnostic(sceneQueryError_);return false;}
    try {
        const auto cameraFrame=sceneCameraFrame();
        LensFlareCamera camera;camera.orientation=cameraFrame.orientation;
        camera.position=cameraFrame.position;
        const auto frame=updateEngineLensFlares(lightScene_,sceneQueries_,camera,elapsed);
        for(const auto& error:frame.diagnostics)diagnostic(error);
        model::Mat4 projection;glGetFloatv(GL_PROJECTION_MATRIX,projection.values.data());
        const auto bind=[this](const LensFlareQuad& quad,std::string& error)->render::gl::UInt {
            const ModelDocument* source=document_;
            constexpr std::size_t roomBase=0x80000000u;
            if(const auto* chunk=lightScene_.chunkLightDocument(quad.instanceIndex))source=chunk;
            else if(quad.instanceIndex>=roomBase) {
                source=areaScene_.lightDocument(quad.instanceIndex);
                if(!source) {error="Stale flare area-owner identity";return 0;}
            } else if(quad.instanceIndex>document_->references().size()) {
                error="Stale flare owner identity";return 0;
            }
            neoshared::material::TextureBinding binding;binding.resref=quad.texture;
            binding.key=neoshared::material::textureResourceKey(quad.texture);
            binding.metadata=source->textureMetadata(binding.key);
            const auto* image=source->texture(binding.key);
            if(!image||!image->hasPixels()) {error="Missing authored flare texture: "+quad.texture;return 0;}
            const auto texture=textureCache_.get(*image,binding.metadata.get(),0,error);
            if(!error.empty())return 0;
            if(!texture.id||texture.cube||texture.volume) {error="Engine flare consumer needs its 2D texture: "+quad.texture;return 0;}
            // Scene::RenderLensFlares binds EACH element. MakeCurrent/get
            // alone does not run Control or preserve the pre-Control TID.
            return textureCache_.bind(texture.id,error).id;
        };
        render::EngineFlareRenderSettings settings;settings.cullBackfaces=options_.cullBackfaces;
        settings.game=document_->model().game;
        settings.hardware=material::selectK2HardwareCapabilities(options_.engineHardware);
        settings.desaturation=effectiveForceSight().desaturation;
        settings.program=effectiveForceSight().enabled?render::EngineFlareProgram::DesktopArb:render::EngineFlareProgram::FixedFunction;
        std::string error;
        if(!meshRenderer_.drawEngineLensFlares(frame.quads,projection,bind,error,settings)) {
            diagnostic(error);return false;
        }
        return true;
    } catch(const std::exception& e) {diagnostic(e.what());return false;}
}

void ModelCanvas::drawEngineShadows() {
    if (!options_.engineShadows.value_or(areaScene_.currentRoom().has_value()) || !options_.previewLights || !document_ || !effectiveAreaShadows()) return;
    const auto diagnostic=[this](const std::string& message) {
        if (!shaderErrorReported_) {
            wxLogWarning("NeoMDL engine shadows: %s",wxString::FromUTF8(message.c_str()));
            shaderErrorReported_=true;
        }
    };
    if (model::engineSceneStageOrder(document_->model().game).empty()) {
        diagnostic("The selected runtime has no verified engine scene shadow stage.");return;
    }
    if (areaScene_.empty() || !areaScene_.currentRoom()) {
        diagnostic("Select an area current room before enabling the engine RenderSinglePass shadow stage.");return;
    }
    try {
        auto frameInputs=shadowScene_.inputs();bool overrideInputs=false;
        const auto cameraPosition=sceneCameraFrame().position;
        if(frameInputs.mode==SceneShadowInputMode::Explicit&&options_.shadowScenePositionFromAreaCamera&&activeAreaReplayCamera()) {
            frameInputs.scenePosition=cameraPosition;overrideInputs=true;
        }
        if(frameInputs.mode==SceneShadowInputMode::Explicit&&options_.shadowCasterRegionFromAreaCamera&&activeAreaReplayCamera()) {
            frameInputs.casterRegion=cameraPosition;overrideInputs=true;
        }
        std::vector<SceneShadowCaster> bucket;
        for(const auto& submission:dynamicGobQueue_) {
            if(!submission.sceneOwner||submission.sceneOwner==ShadowScene::focusContainer+1u)continue;
            const auto id=submission.sceneOwner-1u;
            if(std::any_of(bucket.begin(),bucket.end(),[id](const auto& owner){return owner.id==id;}))continue;
            const auto& owners=motionBlurScene_.owners();
            const auto found=std::find_if(owners.begin(),owners.end(),[id](const auto& owner){return owner.id==id;});
            bucket.push_back(*found);
        }
        SceneShadowOwnerAdmission admission;
        admission.count=[&] {
            bucket.erase(std::remove_if(bucket.begin(),bucket.end(),[](const auto& owner) {
                return owner.ownerLifetime&&owner.ownerLifetime->expired();
            }),bucket.end());
            return bucket.size();
        };
        admission.load=[&](std::size_t slot) {
            // SetScene(null) retains the borrowed Gob in this bucket. Only
            // destruction removes it; read its current Parts even when pooled.
            auto caster=bucket.at(slot);
            if(caster.resolveWorld)caster.world=caster.resolveWorld();
            if(caster.resolveFrame)caster.frame=caster.resolveFrame();
            caster.visible=true;return caster;
        };
        admission.animate=[this](LightOwnerId id) {
            if(renderAnimationOwners_.at(id)->hasBehavior())animateRenderOwner(id);
        };
        auto frame=shadowScene_.buildFrame(lightScene_,cameraView_,sceneFarClip_,
                                           overrideInputs?&frameInputs:nullptr,&admission);
        if(!frame.diagnostic.empty()) {diagnostic(frame.diagnostic);return;}
        if(frame.fadeFan.vertices.empty())return;
        std::vector<render::EngineShadowVolumeDraw> volumes;volumes.reserve(frame.volumes.size());
        for(auto& volume:frame.volumes)volumes.push_back({std::move(volume.geometry),
            model::multiply(cameraView_,volume.world),volume.beginsBeaming});
        model::Mat4 projection;glGetFloatv(GL_PROJECTION_MATRIX,projection.values.data());
        std::string error;
        const auto mode=options_.engineTwoSidedShadows
            ? render::EngineShadowStencilMode::TwoSidedDepthPass
            : render::EngineShadowStencilMode::TwoPassDepthFail;
        render::EngineShadowEffects effects;
        effects.fog=effectiveFog();
        effects.softMode=options_.engineSoftShadows ? options_.engineSoftShadowMode
                                                   : render::EngineSoftShadowMode::Disabled;
        if (!meshRenderer_.drawEngineShadows(volumes,frame.fadeFan,cameraView_,projection,error,mode,effects)) diagnostic(error);
    } catch (const std::exception& error) {
        // Build all casters before touching stencil. Engine skin/direct no-draw
        // branches are handled by the dispatcher; truly unsupported inputs do
        // not leave a partially submitted stencil stage.
        diagnostic(error.what());
    }
}

void ModelCanvas::drawMeshProjectionInspection() {
    if (!options_.inspectMeshProjections || !document_ || !options_.previewLights) return;
    const auto diagnostic=[this](const std::string& message) {
        if (!shaderErrorReported_) {
            wxLogWarning("NeoMDL mesh-projection inspection: %s",wxString::FromUTF8(message.c_str()));
            shaderErrorReported_=true;
        }
    };
    if (document_->model().game!=model::GameVersion::Kotor2 || areaScene_.empty()) {
        diagnostic("The explicit projection inspection requires K2 area context.");return;
    }
    try {
        // This list is an explicit INSPECTOR input: visible static room meshes,
        // in room/node order. It is not presented as meshshadowbucket
        // construction. The inspected RenderSinglePass never invokes this
        // independent method, so it remains outside all engine scene buckets.
        std::vector<model::MeshProjectionSource> sources;
        for (const auto id:areaScene_.activeRooms()) {
            const auto found=std::find_if(areaScene_.rooms().begin(),areaScene_.rooms().end(),
                [id](const auto& room){return room.id==id;});
            if (found==areaScene_.rooms().end()) continue;
            const auto& room=*found;const auto& asset=room.document->model();
            for (std::size_t n=0;n<asset.nodes.size();++n) {
                const auto& node=asset.nodes[n];
                if (!node.mesh) continue;
                if (*node.mesh>=asset.meshes.size() || n>=room.pose().worldTransforms.size())
                    throw model::ModelError("Incomplete room mesh-projection source");
                const auto& mesh=asset.meshes[*node.mesh];
                if (!mesh.castsShadow && !mesh.beaming) continue;
                sources.push_back({&node,&mesh,
                    model::multiply(room.world,room.pose().worldTransforms[n]),nullptr});
            }
        }
        // Reuse this frame's actual room query. Selecting another Scene owner
        // here would mutate the shared illumination scales and retained VP list
        // merely to draw an inspection overlay.
        const auto owner=AreaScene::sceneDynamicLightOwner;
        if (!sceneLightsSelectedThisFrame_ || !lightScene_.ownerSelection(owner))
            throw model::ModelError("No current room-light query is available for projection inspection");
        const auto selected=lightScene_.projectionShadowLights(owner,ShadowLightQuery::All);
        if (!selected.diagnostic.empty()) throw model::ModelError(selected.diagnostic);
        std::vector<model::MeshProjectionLight> lights;
        for (const auto& slot:selected.lights) {
            const auto* light=lightScene_.find(slot.id);
            if (!light) throw model::ModelError("Stale engine projection-light selection");
            // DoDynamicMeshProjections uses the raw light position, without
            // ProjectDynamicShadow's vertical displacement or radius>1000 plane.
            lights.push_back({light->position,light->radius,light->shadowRadius});
        }
        const auto& v=cameraView_.values;
        model::MeshProjectionInput input;
        input.cameraFarPlane={{-v[2],-v[6],-v[10]},-v[14]-sceneFarClip_};
        auto projected=model::buildDynamicMeshProjections(sources,lights,input);
        std::vector<render::EngineShadowVolumeDraw> volumes;
        volumes.reserve(projected.size());
        for (auto& volume:projected)
            volumes.push_back({std::move(volume.geometry),model::multiply(cameraView_,volume.world)});
        model::Mat4 projection;glGetFloatv(GL_PROJECTION_MATRIX,projection.values.data());
        std::string error;
        if (!meshRenderer_.drawShadowVolumeInspection(volumes,projection,error)) diagnostic(error);
    } catch (const std::exception& error) { diagnostic(error.what()); }
}

void ModelCanvas::selectSceneDynamicLights() {
    LightOwnerQuery query;
    query.source=areaScene_.empty()?LightQuerySource::Global:LightQuerySource::Room;
    query.submission=LightSubmissionMode::SceneDynamicOnly;
    query.includeOutOfRange=true;
    // ManageSceneBSP uses Scene::sceneFocus for both light collection and
    // ranking (K1 +0xa8, K2 +0xb4), separately from Camera.
    query.sphere=LightQuerySphere{worldPlaybackSettings_.sceneFocus,sceneFarClip_};
    lightScene_.setOwnerQuery(AreaScene::sceneDynamicLightOwner,query);
    lightScene_.selectOwner(AreaScene::sceneDynamicLightOwner,worldPlaybackSettings_.sceneFocus,sceneFarClip_);
    sceneLightsSelectedThisFrame_=true;
}

bool ModelCanvas::prepareSubmission(const render::MeshSubmission& submission,
                                    render::MeshDrawParameters& parameters,
                                    std::string& error) {
    if(submission.sceneOwner==ShadowScene::focusContainer+1u) {
        shadowScene_.updateFocusContainer(lightScene_);
        return true;
    }
    if (meshPick_ && submission.drawPrimary && !submission.ownerOnly &&
        parameters.primitive == render::PrimitiveTopology::Triangles &&
        !parameters.particlePass && !parameters.lightsaberPass)
        considerMeshPick(submission);
    if(submission.sceneOwner&&(submission.ownerModelClassification&8u)) {
        if(!sceneQueryError_.empty()){error=sceneQueryError_;return false;}
        render::applyEngineDoorAlpha(parameters,sceneQueries_.owner(submission.sceneOwner-1u)->doorAlpha);
    }
    // Scene::RenderSinglePass enables GL_BLEND at Desktop 0xc61f9 before the
    // material/Gob buckets. Material opacity classification does not disable
    // it. In particular bumpy-shiny PopAttrib restores this enabled state
    // before its ONE/ONE_MINUS_SRC_ALPHA base pass, even for an opaque owner.
    if(parameters.material.k2PartTriMeshState && !parameters.inspectMaterial &&
       !(options_.nearestLightsOnlyGobs && areaScene_.empty()))
        parameters.material.blending=true;
    if (!k2Lights_ || !options_.previewLights || !parameters.lighting || !submission.lightOwner) return true;
    parameters.lightCount = submission.inspectionLightCount;
    const auto id = *submission.lightOwner;
    if (options_.nearestLightsOnlyGobs && areaScene_.empty()) {
        render::setEngineBumpLight(lightScene_.retainedBumpLight(),parameters);
        const auto family = render::k2LightColorSource(parameters);
        if (family == LightColorSource::VertexProgram) {
            if(parameters.engineCpuSkin) {
                const auto sphere=ownerLightSpheres_.find(id);
                if(sphere==ownerLightSpheres_.end()) {error="Missing Gob origin for fixed-light query";return false;}
                auto fixed=parameters;
                render::appendK2Lights(lightScene_.nearestLights(sphere->second.position,2.0f),cameraView_,fixed,LightColorSource::FixedFunction);
                parameters.fixedFunctionLights=std::vector<render::PreviewLight>(fixed.lights.begin(),
                    fixed.lights.begin()+std::min(fixed.lightCount,fixed.lights.size()));
            }
            // GetNearestLights only calls SetLightGL. It does NOT populate the
            // custom vertex-program list or advance an object fade container.
            render::appendK2Lights(lightScene_.retainedVertexLights(),cameraView_,parameters,family);
        } else {
            const auto sphere = ownerLightSpheres_.find(id);
            if (sphere == ownerLightSpheres_.end()) { error="Missing Gob origin for nearest-light query"; return false; }
            render::appendK2Lights(lightScene_.nearestLights(sphere->second.position,2.0f),cameraView_,parameters,family);
            if(parameters.engineCpuSkin)
                parameters.fixedFunctionLights=std::vector<render::PreviewLight>(parameters.lights.begin(),
                    parameters.lights.begin()+std::min(parameters.lightCount,parameters.lights.size()));
        }
        return true;
    }
    if (id == AreaScene::sceneDynamicLightOwner) {
        // Scene::RenderStaticGeometry calls EnableLightsDynamicOnly (Desktop
        // 0xc49d3..0xc49de); owner-null vertex programs read the +0x74 list.
        if(!sceneLightsSelectedThisFrame_)selectSceneDynamicLights();
    } else {
        const auto sphere = ownerLightSpheres_.find(id);
        if (sphere == ownerLightSpheres_.end()) { error="Missing engine owner light query input"; return false; }
        auto position=sphere->second.position;
        const auto& owners=motionBlurScene_.owners();
        const auto owner=std::find_if(owners.begin(),owners.end(),[id](const auto& value){return value.id==id;});
        if(owner!=owners.end()&&owner->resolveRangePosition)
            if(const auto range=owner->resolveRangePosition())position=*range;
        lightScene_.selectOwner(id,position,sphere->second.radius);
    }
    render::appendK2OwnerLights(lightScene_,id,cameraView_,parameters);
    if (const auto* selection = lightScene_.ownerSelection(id); selection && !selection->diagnostic.empty()) {
        // The geometry remains inspectable, but no stale or invented light
        // ordering is submitted. Keep a visible diagnostic for the host.
        if (!shaderErrorReported_) {
            wxLogWarning("NeoMDL lighting: %s",wxString::FromUTF8(selection->diagnostic));
            shaderErrorReported_=true;
        }
    }
    return true;
}

void ModelCanvas::queueAreaRooms() {
    if(areaScene_.empty())return;
    std::set<std::uint64_t> distortionOwners;
    const OwnerVisualEffects ordinaryEffects;
    struct EngineAreaOwner {
        std::vector<render::MeshSubmission>* queue{};
        float distance{};
    };
    std::map<std::uint64_t,EngineAreaOwner> engineOwners;
    std::map<const model::Pose*,std::vector<HitPartTransform>> engineFrames;
    const auto rememberK1Owner=[&](std::uint64_t owner,const model::Model& asset,
        const model::Pose& pose,const model::Mat4& world,const model::EngineFrame& frame,
        const SceneAnimationOwner& animationOwner,const OwnerVisualEffects* visual) {
        if(asset.game!=model::GameVersion::Kotor1||!engineOwnerAdmitted(owner,asset.classification))return;
        const auto& effects=visual?*visual:ordinaryEffects;
        auto* queue=&dynamicGobQueue_;
        const auto bucket=engineOwnerBucket(asset,effects);
        if(bucket==EngineOwnerBucket::Distortion) {
            queue=&distortionQueue_;
            if(distortionOwners.insert(owner).second)++distortionProducerCount_;
        } else if(bucket==EngineOwnerBucket::Lightsaber)queue=&lightsaberQueue_;
        const auto distance=ownerSphereCameraDistance(animationOwner.sphere(),frame,sceneCameraFrame().position);
        const bool transparent=model::ownerTreeHasTransparency(asset,nullptr);
        // Preserve a complete Gob event even if it only owns special parts.
        queue->push_back(ownerOrderingSubmission(owner+1u,asset,transparent,distance,
            effects.colorShift[3],model::multiply(cameraView_,world)));
        engineOwners.emplace(owner,EngineAreaOwner{queue,distance});
        engineFrames.emplace(&pose,engineCollapsedHitPose(asset,pose,frame));
    };
    for(const auto& object:areaScene_.objects()) {
        if(!object->staticPrimary())rememberK1Owner(object->ownerId(),object->document()->model(),
            object->pose(),object->world(),object->frame(),object->animationOwner(),object->ownerVisualEffects());
        if(options_.showReferences)for(std::size_t i=0;i<object->references().instances().size();++i) {
            const auto& reference=object->references().instances()[i];
            if(reference.active&&reference.sceneMember)
                rememberK1Owner(object->ownerId(i),object->document()->references()[i].model,
                    reference.pose,reference.world,reference.frame,reference.animationOwner,object->ownerVisualEffects(i));
        }
    }
    for(const auto& draw:areaScene_.meshInstances(options_.showReferences,options_.danglySimulation,lastScenePlanes_)) {
        if(!draw.staticPart&&!engineOwnerAdmitted(draw.owner,draw.asset->classification))continue;
        const auto& mesh=*draw.mesh;
        const auto& node=draw.asset->nodes[mesh.nodeIndex];
        if(node.aabb) {queueAabbDebug(mesh,node,draw.world,false,true);continue;}
        if(node.trigger||(node.lightsaber&&node.lightsaber->postprocessed))continue;
        if(!mesh.render&&!options_.showHidden)continue;
        auto* queue=draw.staticPart?&staticMeshQueue_:&dynamicGobQueue_;
        const bool k1=draw.asset->game==model::GameVersion::Kotor1;
        float distance=0.0f;
        if(k1&&!draw.staticPart) {
            const auto& owner=engineOwners.at(draw.owner);
            queue=owner.queue;distance=owner.distance;
        } else if(!draw.staticPart) {
            const auto bucket=engineOwnerBucket(*draw.asset,draw.ownerEffects?*draw.ownerEffects:ordinaryEffects);
            if(bucket==EngineOwnerBucket::Hologram)queue=&hologramQueue_;
            else if(bucket==EngineOwnerBucket::Distortion) {
                queue=&distortionQueue_;
                if(distortionOwners.insert(draw.owner).second)++distortionProducerCount_;
            }
            distance=ownerSphereCameraDistance(draw.animationOwner->sphere(),
                draw.ownerFrame,sceneCameraFrame().position);
        }
        const HitPartTransform* partFrame=nullptr;
        if(k1) {
            auto found=engineFrames.find(draw.pose);
            if(found==engineFrames.end())
                found=engineFrames.emplace(draw.pose,
                    engineCollapsedHitPose(*draw.asset,*draw.pose,draw.ownerFrame)).first;
            partFrame=&found->second[mesh.nodeIndex];
        }
        const auto before=queue->size();
        glPushMatrix();glMultMatrixf(draw.world.values.data());
        drawMesh(mesh,draw.deformed,selectedAreaOwners_.count(draw.owner)!=0,*draw.material,draw.state,draw.asset->game,draw.asset->fog,
            draw.owner,draw.document,draw.time,queue,draw.staticPart?0:draw.owner+1u,
            draw.ownerEffects?draw.ownerEffects->colorShift[3]:1.0f,
            draw.asset->classification,draw.hasTransparency,
            distance,true,
            draw.ownerEffects,&draw.ownerWorld,draw.textureAngleDegrees,partFrame);
        glPopMatrix();
        for(auto i=before;i<queue->size();++i)(*queue)[i].poseRevision=draw.revision;
    }
}


void ModelCanvas::drawReferenceModels() {
    if (!options_.showReferences || !document_ || document_->references().empty()) return;
    for (std::size_t instanceIndex=0;instanceIndex<document_->references().size();++instanceIndex) {
        const auto* runtime = referenceScene_.instance(instanceIndex);
        if (!runtime || !runtime->active || !runtime->sceneMember) continue;
        const auto& reference=document_->references()[instanceIndex];
        const auto selectionNode=referenceSelectionNode(instanceIndex);
        if (!meshVisible(selectionNode)) continue;
        const auto& sampled=runtime->pose;
        const auto engineParts=reference.model.game==model::GameVersion::Kotor1
            ? engineCollapsedHitPose(reference.model,sampled,runtime->frame)
            : std::vector<HitPartTransform>{};
        if(!engineOwnerAdmitted(instanceIndex+1u,reference.model.classification))continue;
        auto* targetQueue=&renderQueue_;
        std::uint64_t sceneOwner=0u;
        {
            // ReferenceScene mirrors separately owned CAurObject/Gob
            // instances; they are not folded into the main owner's part order.
            targetQueue=&dynamicGobQueue_;
            sceneOwner=static_cast<std::uint64_t>(instanceIndex)+2u;
            if(!model::engineSceneStageOrder(reference.model.game).empty() &&
               !(reference.model.game==model::GameVersion::Kotor2 && options_.nearestLightsOnlyGobs && areaScene_.empty())) {
                const auto bucket=engineOwnerBucket(reference.model,ownerEffects_[instanceIndex+1u]);
                if(bucket==EngineOwnerBucket::Hologram)targetQueue=&hologramQueue_;
                else if(bucket==EngineOwnerBucket::Distortion){targetQueue=&distortionQueue_;++distortionProducerCount_;}
                else if(bucket==EngineOwnerBucket::Lightsaber)targetQueue=&lightsaberQueue_;
            }
        }
        const auto cameraDistance=ownerSphereCameraDistance(runtime->animationOwner.sphere(),
            runtime->frame,sceneCameraFrame().position);
        const bool ownerHasTransparency = reference.model.game==model::GameVersion::Kotor1
            ? ownerOrderingTransparency_.at(instanceIndex+1u)
            : neomdl::model::ownerTreeHasTransparency(reference.model, &sampled);
        if(reference.model.game==model::GameVersion::Kotor1)
            targetQueue->push_back(ownerOrderingSubmission(sceneOwner,reference.model,
                ownerHasTransparency,cameraDistance,ownerEffects_[instanceIndex+1u].colorShift[3],
                model::multiply(cameraView_,runtime->world)));
        {
            for (std::size_t meshIndex = 0u;
                 meshIndex < reference.model.meshes.size(); ++meshIndex) {
                const auto& diffuse =
                    document_->referenceMaterial(instanceIndex, meshIndex).diffuse;
                if (diffuse.key.empty()) continue;
                const auto* texture = document_->texture(diffuse.key);
                if (texture && texture->hasPixels()) textureCache_.orderFor(*texture);
            }
        }

        for (std::size_t meshIndex = 0u; meshIndex < reference.model.meshes.size(); ++meshIndex) {
            const auto& mesh = reference.model.meshes[meshIndex];
            if (mesh.nodeIndex >= sampled.worldTransforms.size() ||
                mesh.nodeIndex >= reference.model.nodes.size()) {
                continue;
            }
            const auto& meshNode = reference.model.nodes[mesh.nodeIndex];
            const bool selected=selectedNodes_.count(selectionNode)!=0;
            if (!model::engineSceneStageOrder(reference.model.game).empty() &&
                (meshNode.flags & neomdl::model::kNodeAabb) != 0u) {
                auto scale=model::identityMatrix();
                scale.values[0]=scale.values[5]=scale.values[10]=runtime->scale;
                const auto world = neomdl::model::multiply(
                    model::multiply(runtime->world,scale), sampled.worldTransforms[mesh.nodeIndex]);
                queueAabbDebug(mesh, meshNode, world, selected, true);
                continue;
            }
            if (reference.model.game == neomdl::model::GameVersion::Kotor2 &&
                meshNode.trigger && meshNode.trigger->packedVerified) {
                continue;
            }
            if (!mesh.render && !options_.showHidden) continue;
            if (meshNode.lightsaber && meshNode.lightsaber->postprocessed) continue;
            const neomdl::model::DeformedMesh* deformed = nullptr;
            if ((options_.danglySimulation || !mesh.dangly) &&
                meshIndex < sampled.deformedMeshes.size()) {
                const auto& candidate = sampled.deformedMeshes[meshIndex];
                if (candidate.valid && candidate.positions.size() == mesh.vertices.size() &&
                    candidate.normals.size() == mesh.vertices.size()) {
                    deformed = &candidate;
                }
            }
            glPushMatrix();
            glMultMatrixf(runtime->world.values.data());
            glScalef(runtime->scale,runtime->scale,runtime->scale);
            if (!deformed)
                glMultMatrixf(sampled.worldTransforms[mesh.nodeIndex].values.data());
            const auto materialState = mesh.nodeIndex < sampled.nodeMaterials.size()
                ? sampled.nodeMaterials[mesh.nodeIndex]
                : neomdl::model::bindNodeMaterial(reference.model.nodes[mesh.nodeIndex]);
            drawMesh(mesh, deformed, selected,
                document_->referenceMaterial(instanceIndex,meshIndex), materialState,
                reference.model.game, reference.model.fog,
                static_cast<LightOwnerId>(instanceIndex) + 1u,
                nullptr, runtime->animationTime, targetQueue, sceneOwner, 1.0f,
                reference.model.classification, ownerHasTransparency,
                cameraDistance, true, !model::engineSceneStageOrder(reference.model.game).empty()
                    ? &ownerEffects_[instanceIndex+1u] : nullptr, &runtime->world,
                {}, engineParts.empty()?nullptr:&engineParts[mesh.nodeIndex]);
            glPopMatrix();
        }
    }
}

void ModelCanvas::queueAabbDebug(
    const neomdl::model::Mesh& mesh,
    const neomdl::model::Node& node,
    const neomdl::model::Mat4& world, bool selected, bool kotor2) {
    if (!kotor2 || !options_.previewAabbs) return;
    if (mesh.faces.empty() && (!node.aabb || node.aabb->nodes.empty())) return;
    aabbDebugQueue_.push_back({&mesh, node.aabb ? &*node.aabb : nullptr,
                               world, selected});
}

void ModelCanvas::drawAabbDebugQueue() {
    if (aabbDebugQueue_.empty()) return;

    const auto lighting = glIsEnabled(GL_LIGHTING);
    const auto texturing = glIsEnabled(GL_TEXTURE_2D);
    const auto culling = glIsEnabled(GL_CULL_FACE);
    const auto blending = glIsEnabled(GL_BLEND);
    const auto depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean depthWrite = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
    GLint frontFace = GL_CCW;
    GLint cullFace = GL_BACK;
    GLint polygonMode[2]{GL_FILL, GL_FILL};
    GLfloat lineWidth = 1.0f;
    glGetIntegerv(GL_FRONT_FACE, &frontFace);
    glGetIntegerv(GL_CULL_FACE_MODE, &cullFace);
    glGetIntegerv(GL_POLYGON_MODE, polygonMode);
    glGetFloatv(GL_LINE_WIDTH, &lineWidth);

    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    for (const auto& submission : aabbDebugQueue_) {
        if (!submission.mesh) continue;
        glPushMatrix();
        glMultMatrixf(submission.world.values.data());

        // PartAABB::Draw submits four passes: front fill, reversed-front fill,
        // normal-front wireframe, and reversed-front wireframe. The palettes
        // are deliberately swapped for the two wireframe passes.
        glFrontFace(static_cast<GLenum>(frontFace));
        drawAabbSurfacePass(*submission.mesh, kAabbFrontColors);
        const auto reversedFront = frontFace == GL_CW ? GL_CCW : GL_CW;
        glFrontFace(static_cast<GLenum>(reversedFront));
        drawAabbSurfacePass(*submission.mesh, kAabbBackColors);
        glFrontFace(static_cast<GLenum>(frontFace));
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        drawAabbSurfacePass(*submission.mesh, kAabbBackColors);
        glFrontFace(static_cast<GLenum>(reversedFront));
        drawAabbSurfacePass(*submission.mesh, kAabbFrontColors);
        glFrontFace(static_cast<GLenum>(frontFace));
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // PartAABB only draws the authored collision faces. NeoMDL adds this
        // tree overlay so the hierarchy and split traversal can be
        // inspected without changing the packed-derived surface pass above.
        if (submission.tree && submission.tree->root &&
            *submission.tree->root < submission.tree->nodes.size()) {
            glDisable(GL_CULL_FACE);
            glLineWidth(submission.selected ? 2.5f : 1.25f);
            std::vector<bool> visited(submission.tree->nodes.size(), false);
            std::vector<std::pair<std::size_t, std::size_t>> pending;
            pending.emplace_back(*submission.tree->root, 0u);
            glBegin(GL_LINES);
            while (!pending.empty()) {
                const auto [index, depth] = pending.back();
                pending.pop_back();
                if (index >= submission.tree->nodes.size() || visited[index])
                    continue;
                visited[index] = true;
                const auto& treeNode = submission.tree->nodes[index];
                if (treeNode.leaf() && treeNode.faceIndex >= 0 &&
                    static_cast<std::size_t>(treeNode.faceIndex) <
                        submission.mesh->faces.size()) {
                    const auto material = submission.mesh->faces[
                        static_cast<std::size_t>(treeNode.faceIndex)]
                                              .surfaceMaterial;
                    if (material < kAabbFrontColors.size()) {
                        const auto& color = kAabbFrontColors[material];
                        glColor4ub(color[0], color[1], color[2], 255u);
                    } else {
                        glColor4ub(255u, 0u, 255u, 255u);
                    }
                } else if (index == *submission.tree->root) {
                    glColor4ub(submission.selected ? 255u : 190u,
                               submission.selected ? 190u : 230u,
                               submission.selected ? 40u : 255u, 255u);
                } else {
                    const auto shade = static_cast<GLubyte>(
                        std::max(54, 185 - static_cast<int>(depth) * 14));
                    glColor4ub(shade, shade, shade, 255u);
                }
                drawAabbBounds(treeNode);
                // Push the second child first so the first-child branch is
                // visited first, matching the stored hierarchy order.
                if (treeNode.secondChild >= 0)
                    pending.emplace_back(
                        static_cast<std::size_t>(treeNode.secondChild), depth + 1u);
                if (treeNode.firstChild >= 0)
                    pending.emplace_back(
                        static_cast<std::size_t>(treeNode.firstChild), depth + 1u);
            }
            glEnd();
            glEnable(GL_CULL_FACE);
        }
        glPopMatrix();
    }

    glFrontFace(static_cast<GLenum>(frontFace));
    glCullFace(static_cast<GLenum>(cullFace));
    glPolygonMode(GL_FRONT, static_cast<GLenum>(polygonMode[0]));
    glPolygonMode(GL_BACK, static_cast<GLenum>(polygonMode[1]));
    glLineWidth(lineWidth);
    glDepthMask(depthWrite);
    if (lighting) glEnable(GL_LIGHTING); else glDisable(GL_LIGHTING);
    if (texturing) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    if (culling) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (blending) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

void ModelCanvas::drawTriggerBucket() {
    if (!options_.previewTriggers || !triggerScene_.runtimeSupported() ||
        triggerScene_.volumes().empty()) {
        return;
    }

    const auto planes = currentTriggerFrustum();
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glLineWidth(1.0f);

    // Scene::DoTriggerBucket preserves insertion order and omits highlighted
    // parts; PartTrigger::Draw draws the footprint with blending disabled and
    // then the decomposition as a blended GL_TRIANGLES stream.
    for (const auto& volume : triggerScene_.volumes()) {
        if (!volume.valid || volume.highlighted) continue;
        if (neomdl::model::triggerOutsideFrustum(
                volume, planes, triggerCullPlane_)) {
            continue;
        }
        const auto alpha = neomdl::model::triggerEffectiveBaseAlpha(
            volume, triggerScene_.globalAlpha());
        if (alpha == 0.0f) continue;

        glColor4f(volume.color[0], volume.color[1], volume.color[2], alpha);
        glDisable(GL_BLEND);
        drawTriggerVertices(GL_LINE_LOOP, volume.footprint);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,volume.highlight.additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        drawTriggerVertices(GL_TRIANGLES, volume.triangles);
    }
    glPopAttrib();
}

bool ModelCanvas::drawTriggerHighlights(bool group) {
    if (!options_.previewTriggers || !triggerScene_.runtimeSupported()) {
        return false;
    }

    bool drewVolume=false;
    const auto planes = currentTriggerFrustum();
    GLint stencilBits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    if(stencilBits==0)return false;
    triggerScene_.advance(triggerFrameElapsed_);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glLineWidth(1.0f);

    // Scene::DoHighlightTriggers is called before and after dynamic Gobs for
    // two distinct groups. Do not collapse them into one viewer overlay pass.
    for (const auto& volume : triggerScene_.volumes()) {
        if (!volume.valid || !volume.highlighted ||
            volume.highlightGroup != group || volume.sideStrip.empty()) {
            continue;
        }
        if (neomdl::model::triggerOutsideFrustum(
                volume, planes, triggerCullPlane_)) {
            continue;
        }

        const auto alpha = neomdl::model::triggerHighlightAlpha(
            volume, triggerScene_.scenePhase());

        drewVolume=true;
        glStencilMask(0xffu);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 0xffu);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);

        // Back faces increment on depth failure; front faces decrement. The
        // packed renders DrawVolume twice with opposite winding and distinct
        // depth-fail operations.
        glCullFace(GL_FRONT);
        glStencilOp(GL_KEEP, GL_INCR, GL_KEEP);
        drawTriggerVolumeGeometry(volume);
        glCullFace(GL_BACK);
        glStencilOp(GL_KEEP, GL_DECR, GL_KEEP);
        drawTriggerVolumeGeometry(volume);

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glStencilFunc(GL_NOTEQUAL, 0, 0xffu);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        drawTriggerScreenOverlay(volume, alpha);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_DEPTH_TEST);
    }
    glPopAttrib();
    return drewVolume;
}
std::vector<ParticleScene*> ModelCanvas::particleScenes() {
    std::vector<ParticleScene*> scenes;
    scenes.reserve(areaScene_.objects().size()+1u);
    scenes.push_back(&particleScene_);
    const auto roots=areaScene_.particleSceneRoots();scenes.insert(scenes.end(),roots.begin(),roots.end());
    for(auto* scene:scenes) {
        scene->setChunkMembershipChanged([this]{refreshChunkMembership();});
        scene->setResourcesDestroyed([this](const ParticleScene& chunk){releaseParticleResources(chunk);});
        retainParticleTextures(*scene);
        std::vector<const ParticleScene*> chunks;
        scene->appendAllocatedChunkScenes(chunks);
        for(const auto* child:chunks)retainParticleTextures(*child);
    }
    return scenes;
}

void ModelCanvas::retainParticleTextures(const ParticleScene& scene) {
    const auto* resource=scene.document();
    if(!resource)return;
    const auto retain=[&](const std::string& name) {
        if(const auto* image=resource->texture(name))textureCache_.retain(&scene,*image);
    };
    const auto material=[&](const neoshared::material::Material& value) {
        for(const auto* binding:{&value.diffuse,&value.lightmap,&value.environmentMap,
                                 &value.bumpMap,&value.bumpyShiny})retain(binding->key);
    };
    for(std::size_t i=0;i<resource->model().meshes.size();++i)material(resource->material(i));
    for(std::size_t r=0;r<resource->references().size();++r)
        for(std::size_t i=0;i<resource->references()[r].model.meshes.size();++i)
            material(resource->referenceMaterial(r,i));
    for(const auto& name:resource->textureLoadOrder())retain(name);
    for(const auto& emitter:scene.emitters()) {
        retain(emitter->texture.key);
        if(emitter->atlas)textureCache_.retain(&scene,*emitter->atlas);
    }
}
void ModelCanvas::releaseParticleResources(const ParticleScene& scene) {
    // ChunkyParticle destroys this Gob's Parts and releases their texture
    // borrows. Shared model meshes, live owners and scene histories survive.
    // Detach CPU cache keys now; only numeric GL names await a current context.
    for(const auto& emitter:scene.emitters()) {
        meshRenderer_.retireGeometry(&emitter->mesh);
        for(const auto& mesh:emitter->frameBlendMeshes)meshRenderer_.retireGeometry(&mesh);
    }
    if(const auto* chunk=scene.chunkOwner())for(const auto& saber:chunk->lightsabers.instances()) {
        meshRenderer_.retireGeometry(&saber->staticMesh);
        meshRenderer_.retireGeometry(&saber->movingMesh);
    }
    textureCache_.release(&scene);
}

void ModelCanvas::releaseChunkCallbacks() {
    particleScene_.setChunkMembershipChanged({});
    particleScene_.setResourcesDestroyed({});
    for(auto* scene:areaScene_.particleSceneRoots()) {
        scene->setChunkMembershipChanged({});scene->setResourcesDestroyed({});
    }
}
void ModelCanvas::refreshChunkMembership() {
    // Constructor/recycle/expiry callbacks execute on the owning UI scene.
    // This path does no GL work and is also valid during a simulation reset.
    // Construction acquires borrows; recycle/expiry leave the allocation alive.
    particleScenes();
    synchronizeEngineQueries();
    lightScene_.synchronizeChunks(&particleScene_,areaScene_,options_.showReferences);
    if(!areaScene_.empty())areaScene_.synchronize(lightScene_,lastScenePlanes_);
    for(const auto& owner:collectParticleChunkOwners(&particleScene_,areaScene_,options_.showReferences)) {
        ownerLightSpheres_[owner.id]=owner.lightSphere;
        auto query=lightScene_.ownerQuery(owner.id).value_or(LightOwnerQuery{});
        query.source=LightQuerySource::Gob;lightScene_.setOwnerQuery(owner.id,query);
        lightScene_.retainOwnerForFrame(owner.id);
    }
}

void ModelCanvas::queueChunkModels() {
    if(!document_ || !meshRenderer_.ready()) return;
    const auto owners=collectParticleChunkOwners(&particleScene_,areaScene_,options_.showReferences);
    std::vector<const ParticleScene*> scenes;
    for(const auto* root:particleScenes()) root->appendActiveChunkScenes(scenes);
    std::stable_sort(scenes.begin(),scenes.end(),[](const auto* left,const auto* right) {
        return left->chunkOwner()->sceneRegistration<right->chunkOwner()->sceneRegistration;
    });
    std::uint64_t revision=(++particleRevision_)<<32u;
    const OwnerVisualEffects effects;
    for(const auto* scene:scenes) {
        const auto& chunk=*scene->chunkOwner();
        if(!scene->engineRuntime(chunk.asset->model().game)) continue;
        const auto draws=chunkParticleMeshes(chunk);
        for(const auto& owner:owners) {
            if(owner.id<chunk.ownerIdentity ||
               owner.id>chunk.ownerIdentity+chunk.asset->references().size() ||
               !engineOwnerAdmitted(owner.id,owner.asset->classification))continue;
            const auto& asset=*owner.asset;
            const auto& pose=*owner.pose;
            auto* queue=&dynamicGobQueue_;
            const auto bucket=engineOwnerBucket(asset,effects);
            if(bucket==EngineOwnerBucket::Hologram) queue=&hologramQueue_;
            else if(bucket==EngineOwnerBucket::Distortion) {
                queue=&distortionQueue_;++distortionProducerCount_;
            } else if(bucket==EngineOwnerBucket::Lightsaber) queue=&lightsaberQueue_;
            const auto distance=ownerSphereCameraDistance(owner.animationOwner->sphere(),
                owner.frame,sceneCameraFrame().position);
            const bool transparent=model::ownerTreeHasTransparency(asset,
                asset.game==model::GameVersion::Kotor1 ? nullptr : &pose);
            const auto sceneOwner=owner.id+1u;
            if(asset.game==model::GameVersion::Kotor1) {
                auto marker=ownerOrderingSubmission(sceneOwner,asset,transparent,distance,1.0f,
                    model::multiply(cameraView_,owner.world));
                marker.ownerLifetime=owner.ownerLifetime;
                marker.resolveOwner=[this,world=owner.resolveWorld,id=owner.id,animation=owner.animationOwner](auto& live) {
                    const auto frame=world();
                    live.parameters.modelView=model::multiply(cameraView_,frame);
                    ownerLightSpheres_[id]={model::transformPoint(frame,{}),animation->sphere().radius};
                };
                queue->push_back(std::move(marker));
            }
            const auto engineParts=asset.game==model::GameVersion::Kotor1
                ? engineCollapsedHitPose(asset,pose,owner.frame) : std::vector<HitPartTransform>{};
            for(const auto& draw:draws) {
                if(draw.ownerIdentity!=owner.id) continue;
                const auto& node=asset.nodes[draw.mesh->nodeIndex];
                if((node.flags&model::kNodeAabb)!=0u) {
                    queueAabbDebug(*draw.mesh,node,draw.world,false,true);continue;
                }
                if(asset.game==model::GameVersion::Kotor2 &&
                   node.trigger&&node.trigger->packedVerified) continue;
                if(node.lightsaber&&node.lightsaber->postprocessed) continue;
                if(!draw.mesh->render&&!options_.showHidden) continue;
                const auto before=queue->size();
                const auto modelView=model::multiply(cameraView_,draw.world);
                glPushMatrix();glLoadMatrixf(modelView.values.data());
                drawMesh(*draw.mesh,draw.deformed,false,*draw.material,draw.materialState,draw.game,draw.fog,
                    owner.id,draw.resources,draw.time,queue,sceneOwner,1.0f,asset.classification,
                    transparent,distance,false,&effects,&draw.ownerFrame,{},
                    engineParts.empty()?nullptr:&engineParts[draw.mesh->nodeIndex]);
                glPopMatrix();
                const auto reference=owner.id==chunk.ownerIdentity ? std::optional<std::size_t>{}
                    : std::optional<std::size_t>{std::size_t(owner.id-chunk.ownerIdentity-1u)};
                for(auto index=before;index<queue->size();++index) {
                    auto& submission=(*queue)[index];submission.poseRevision=++revision;
                    submission.ownerLifetime=owner.ownerLifetime;
                    submission.resolveOwner=[this,source=&chunk,reference,meshIndex=*node.mesh,
                        animation=owner.animationOwner](auto& live) {
                        // The admitted bucket retains a Gob, including one detached
                        // into the particle pool. Resolve its current Parts at Draw.
                        const auto current=chunkParticleMesh(*source,reference,meshIndex);
                        const auto& asset=reference?source->asset->references()[*reference].model:source->asset->model();
                        const auto* runtime=reference?source->references->instance(*reference):nullptr;
                        const auto& pose=runtime?runtime->pose:source->pose;
                        const OwnerVisualEffects effects;
                        refreshAnimatedSubmission(live,asset,pose,current.ownerFrame,
                            runtime?runtime->frame:source->references->mainFrame(),current.time,(++particleRevision_)<<32u,
                            *current.material,current.resources,&effects);
                        ownerLightSpheres_[current.ownerIdentity]={model::transformPoint(current.ownerFrame,{}),animation->sphere().radius};
                    };
                }
            }
        }
    }
}

bool ModelCanvas::queueParticles(float elapsedSeconds,bool sceneAnimating,
                                 std::string& error) {
    emitterFrameTrace_={};error.clear();
    auto scenes=particleScenes();
    if(!options_.previewEmitters || !document_ || !meshRenderer_.ready()) {
        discardSceneEmitterFrame(scenes);return true;
    }
    if(!particleError_.empty()) {
        discardSceneEmitterFrame(scenes);error=particleError_;return false;
    }
    if(!proceduralTextureError_.empty()) {
        discardSceneEmitterFrame(scenes);error=proceduralTextureError_;return false;
    }
    const auto triggerPlanes=currentTriggerFrustum();
    std::vector<neomdl::model::EmitterScenePlane> emitterPlanes;
    emitterPlanes.reserve(triggerPlanes.size());
    for(const auto& plane:triggerPlanes)
        emitterPlanes.push_back({plane.normal,plane.distance});
    ++particleRevision_;
    model::Mat4 projection;glGetFloatv(GL_PROJECTION_MATRIX,projection.values.data());
    std::uint64_t chunkRevision=particleRevision_<<32;

    const auto renderEmitter=[&](const SceneEmitterDraw& scheduled) {
        const auto* emitter=scheduled.emitter;
        std::vector<render::MeshSubmission> current;
        if(!emitter->active || !emitter->visible)return;
        const bool engineK1Owner=emitter->game==model::GameVersion::Kotor1 && emitter->ownerIdentity!=0u;
        if(engineK1Owner) {
            const auto owner=emitter->ownerIdentity-1u;
            const auto* source=scheduled.scene->document();
            const auto& asset=emitter->reference ? source->references()[*emitter->reference].model : source->model();
            const bool distortion=(asset.classification&0x02u)!=0u ||
                (scheduled.scene==&particleScene_ && owner<ownerEffects_.size() && ownerEffects_[owner].distortion);
            meshRenderer_.beginK1DesktopEmitter(distortion);
        }
        struct EmitterFinish {
            render::ShaderMeshRenderer& renderer;bool active;
            ~EmitterFinish(){if(active)renderer.finishK1DesktopEmitter();}
        } finish{meshRenderer_,engineK1Owner};
        // A engine ChunkyParticle owns an ordinary registered Gob, queued
        // before scene execution. RenderNothing only writes its next frame;
        // it does not draw model geometry from inside the emitter bucket.
        if(emitter->chunkAsset && scheduled.scene->engineRuntime(emitter->game)) return;
        if(emitter->chunkAsset) {
            for(const auto& chunk:emitter->chunks) if(chunk->active) {
                for(const auto& chunkDraw:chunkParticleMeshes(*chunk)) {
                    if(!chunkDraw.mesh->render && !options_.showHidden)continue;
                    const auto modelView=neomdl::model::multiply(cameraView_,chunkDraw.world);
                    const auto queuedBefore=current.size();
                    glPushMatrix();glLoadMatrixf(modelView.values.data());
                    drawMesh(*chunkDraw.mesh,chunkDraw.deformed,false,*chunkDraw.material,
                             chunkDraw.materialState,chunkDraw.game,chunkDraw.fog,
                             chunkDraw.ownerIdentity,
                             chunkDraw.resources,chunkDraw.time,&current,0u,1.0f,0u,
                             false,0.0f,false,nullptr,&chunkDraw.ownerFrame);
                    glPopMatrix();
                    // One asset can have several simultaneous skin poses. A
                    // distinct revision is required even when buffer sizes match.
                    if(current.size()!=queuedBefore)
                        current.back().poseRevision=++chunkRevision;
                }
            }
        } else if(!emitter->mesh.indices.empty()) {
            neomdl::render::MeshDrawParameters base;
            base.material.engineGame=emitter->game==model::GameVersion::Kotor1
                ? material::EngineMaterialGame::Kotor1 : material::EngineMaterialGame::Kotor2;
            base.forceSight=effectiveForceSight();
            base.modelView=neomdl::model::identityMatrix();base.projection=projection;
            base.eyeToWorld=eyeToWorld_;base.cullBackfaces=!emitter->data->twoSidedTexture;
            base.wireframe=options_.wireframe;
            base.fog=neomdl::model::fogForModel(effectiveFog(),emitter->modelFog);
            if(options_.textures) {
                if(emitter->atlas) {
                    std::string textureError;
                    base.diffuseTexture=textureCache_.get(*emitter->atlas,
                        emitter->texture.metadata.get(),0,textureError).id;
                    if(!textureError.empty() && !shaderErrorReported_) {
                        wxLogWarning("NeoMDL particle atlas: %s",
                                     wxString::FromUTF8(textureError.c_str()));
                        shaderErrorReported_=true;
                    }
                } else {
                    const auto texture=textureFor(emitter->texture,0,scheduled.scene->document());
                    base.diffuseTexture=texture.cube?0:texture.id;
                }
            }
            // Particle quads are already in eye coordinates. Keep every
            // neighboring-frame pass adjacent inside this emitter's RenderFunc.
            const auto ownerEye=neomdl::model::transformPoint(
                cameraView_,emitter->ownerPosition);
            const float ownerDepth=-ownerEye.z;
            const auto ownerGroup=emitter->ownerIdentity;
            const auto authoredOrder=static_cast<std::int16_t>(emitter->data->renderOrder);
            for(unsigned pass=0;pass<5;++pass) {
                const auto& mesh=pass?emitter->frameBlendMeshes[pass-1]:emitter->mesh;
                if(mesh.indices.empty())continue;
                render::MeshSubmission submission;
                submission.parameters=base;
                render::configureParticlePass(emitter->support.blend,pass,submission.parameters);
                submission.mesh=&mesh;submission.poseRevision=particleRevision_;
                submission.eyeDepth=ownerDepth;
                submission.authoredOrderGroup=ownerGroup;
                submission.authoredOrder=authoredOrder;
                current.push_back(std::move(submission));
            }
        }
        if(current.empty())return;
        for(auto& submission:current)
            submission.prepareTextures=[this](auto& parameters,auto& message) {
                return textureCache_.bind(parameters,message);
            };
        std::string drawError;
        if(!meshRenderer_.drawQueue(current,drawError,render::QueueOrdering::Submission,
            [this](const auto& submission,auto& parameters,auto& message) {
                return prepareSubmission(submission,parameters,message);
            })) {
            throw std::runtime_error(drawError.empty()
                ?"Engine emitter RenderFunc failed":drawError);
        }
    };

    try {
        SceneEmitterFrameSettings frameSettings{elapsedSeconds,sceneAnimating};
        frameSettings.cameraFrame=sceneCameraFrame();
        executeSceneEmitterBucket(scenes,cameraView_,emitterPlanes,
            frameSettings,&emitterFrameTrace_,renderEmitter);
        return true;
    } catch(const std::exception& failure) {
        error=failure.what();particleError_=error;return false;
    }
}

void ModelCanvas::queueLightsabers() {
    if (!options_.previewLightsabers || !document_ || !meshRenderer_.ready()) return;
    if(model::engineSceneStageOrder(document_->model().game).empty())return;

    struct Source {
        const LightsaberScene* scene;
        const ModelDocument* document;
        const model::Pose* pose;
        const ReferenceScene* references;
        const AreaObject* object{};
        const ChunkParticleInstance* chunk{};
        HitOwnerId ownerId(std::optional<std::size_t> reference) const {
            if(object)return object->ownerId(reference);
            const auto base=chunk?chunk->ownerIdentity:HitOwnerId{0};
            return base+(reference?*reference+1u:0u);
        }
    };
    std::vector<Source> sources{{&lightsaberScene_,document_,&*pose_,&referenceScene_}};
    for(const auto& object:areaScene_.objects())
        sources.push_back({&object->lightsabers(),object->document().get(),&object->pose(),
            &object->references(),object.get()});
    const auto chunkOwners=collectParticleChunkOwners(&particleScene_,areaScene_,options_.showReferences);
    std::vector<const ParticleScene*> chunkScenes;
    for(const auto* root:particleScenes())root->appendActiveChunkScenes(chunkScenes);
    std::stable_sort(chunkScenes.begin(),chunkScenes.end(),[](const auto* left,const auto* right) {
        return left->chunkOwner()->sceneRegistration<right->chunkOwner()->sceneRegistration;
    });
    for(const auto* scene:chunkScenes) {
        const auto& chunk=*scene->chunkOwner();
        sources.push_back({&chunk.lightsabers,chunk.asset,&chunk.pose,chunk.references.get(),nullptr,&chunk});
    }
    model::Mat4 projection;glGetFloatv(GL_PROJECTION_MATRIX,projection.values.data());
    for(const auto& source:sources) {
      for (const auto& instance : source.scene->instances()) {
        if (source.scene==&lightsaberScene_ &&
            !meshVisible(instance->reference ? referenceSelectionNode(*instance->reference) : instance->nodeIndex)) continue;
        const auto id=source.ownerId(instance->reference);
        {
            // ManageSceneBSP admits owning Gobs. Collapsed room Parts do not
            // acquire a fabricated owner to enter the lightsaber bucket.
            if(source.object && source.object->staticPrimary() && !instance->reference)continue;
            if(instance->game==model::GameVersion::Kotor2 && !sceneQueries_.owner(id))
                continue; // failed registration already reports its diagnostic
            // Desktop ManageSceneBSP appends visible saber owners directly;
            // no separate sphere/distance test is needed here.
        }
        const auto& ownerAsset=instance->reference?source.document->references()[*instance->reference].model:source.document->model();
        if(!engineOwnerAdmitted(id,ownerAsset.classification))continue;
        const auto& mesh = instance->renderMesh();
        if (!instance->active || mesh.indices.empty()) continue;
        render::MeshSubmission submission;
        auto& parameters=submission.parameters;
        parameters.material.engineGame=instance->game==model::GameVersion::Kotor1
            ? material::EngineMaterialGame::Kotor1 : material::EngineMaterialGame::Kotor2;
        parameters.modelView = neomdl::model::multiply(cameraView_, instance->world);
        parameters.projection = projection;
        parameters.eyeToWorld = eyeToWorld_;
        parameters.material.pass = neomdl::material::RenderPass::Opaque;
        parameters.material.opacity = 1.0f;
        parameters.material.textureAlphaIsOpacity = false;
        parameters.lightsaberPass = true;
        parameters.primitive = render::PrimitiveTopology::QuadStrip;
        parameters.oneOneBlend = options_.lightsaberSettings.additiveBlur;
        parameters.lighting = options_.lightsaberSettings.lighting && options_.lighting;
        parameters.lightCount = 0u;
        parameters.sceneAmbient = effectiveSceneAmbient();
        parameters.fog = neomdl::model::fogForModel(effectiveFog(), instance->modelFog);
        // PartLightsaber::Draw unconditionally disables culling and uses its
        // dedicated wireframe global, independent of the ordinary mesh switch.
        parameters.cullBackfaces = false;
        parameters.wireframe = options_.lightsaberSettings.wireframe;
        parameters.streamedGeometry = instance->streamsMovingGeometry();

        const auto prepareSaberMaterial=[this,source,saber=instance.get()](auto& parameters) {
          const auto* instance=saber;
          if(instance->game==model::GameVersion::Kotor1) {
            // DoLightSaberBucket -> Gob::Render -> PartDraw calls Mat before
            // the specialized Draw. Preserve slot zero and Mat's raster inputs.
            const auto* sourceDocument=source.document;
            const auto& asset=instance->reference ? sourceDocument->references()[*instance->reference].model : sourceDocument->model();
            const auto& sourcePose=instance->reference ? source.references->instance(*instance->reference)->pose
                : *source.pose;
            const auto meshIndex=*asset.nodes.at(instance->nodeIndex).mesh;
            const auto& sourceMesh=asset.meshes.at(meshIndex);
            const auto& authored=instance->reference ? sourceDocument->referenceMaterial(*instance->reference,meshIndex)
                : sourceDocument->material(meshIndex);
            const auto& state=sourcePose.nodeMaterials.at(instance->nodeIndex);
            const auto* effects=source.object ? source.object->ownerVisualEffects(instance->reference)
                : source.chunk ? nullptr : &ownerEffects_.at(instance->reference ? *instance->reference+1u : 0u);
            material::SurfaceInputs surface;
            surface.opacity=state.alpha;surface.selfIllumination=state.selfIllumination;
            surface.instanceOpacity=effects ? effects->instanceOpacity : 1.0f;
            material::EnginePartTriMeshInputs inputs;
            inputs.engineHardware=options_.engineHardware;
            parameters.material=material::evaluateEnginePartTriMeshMaterial(authored,surface,inputs,
                material::EngineMaterialGame::Kotor1,source.chunk ? (instance->reference
                    ?source.references->instance(*instance->reference)->animationTime:source.chunk->time) : materialTime_);
            auto& mat=parameters.material;
            mat.k2PartTriMeshState=false;mat.useLightmap=mat.useBumpMap=mat.useEnvironmentMap=false;
            mat.renderPath=material::TriMeshRenderPath::Flat;
            mat.pass=material::RenderPass::Opaque;mat.blending=true;
            const auto blend=authored.diffuse.metadata ? authored.diffuse.metadata->txi.properties.blending
                : std::optional<neoshared::material::BlendDirective>{};
            mat.blendFunction=blend==neoshared::material::BlendDirective::Additive ? material::BlendFunction::Additive
                : blend==neoshared::material::BlendDirective::PunchThrough ? material::BlendFunction::Replace : material::BlendFunction::Alpha;
            mat.depthWrite=blend!=neoshared::material::BlendDirective::Additive;
            mat.alphaCutoff=blend==neoshared::material::BlendDirective::PunchThrough ? 0.35f : 0.0f;
            mat.textureAlphaIsOpacity=true;
            parameters.engineLightsaberMaterial=true;
            parameters.hasColorOverride=true;
            parameters.colorOverride={{1,1,1,1}};
            if(parameters.lighting) {
                parameters.colorOverride={{sourceMesh.diffuse.x,sourceMesh.diffuse.y,sourceMesh.diffuse.z,mat.opacity}};
                if(effects && effects->colorShiftEnabled)
                    for(std::size_t c=0;c<4;++c)parameters.colorOverride[c]*=effects->colorShift[c];
                if(effects && effects->illuminationEnabled)
                    for(std::size_t c=0;c<3;++c)mat.selfIllumination[c]+=effects->illumination[c];
            } else {
                mat.opacity=1.0f;mat.selfIllumination={{0,0,0}};
            }
            mat.vertexOpacity=parameters.colorOverride[3];
            const auto texture=textureFor(authored.diffuse,mat.diffuseUv.layer,sourceDocument);
            parameters.diffuseTexture=texture.id;
        }
        };
        prepareSaberMaterial(parameters);

        float minimumZ{},maximumZ{};
        const auto& matrix=parameters.modelView.values;
        for(std::size_t n=0;n<mesh.vertices.size();++n) {
            const auto& point=mesh.vertices[n].position;
            const float z=matrix[2]*point.x+matrix[6]*point.y+
                matrix[10]*point.z+matrix[14];
            if(n==0)minimumZ=maximumZ=z;
            else {minimumZ=std::min(minimumZ,z);maximumZ=std::max(maximumZ,z);}
        }
        const float eyeDepth=mesh.vertices.empty()?-0.0f:-((minimumZ+maximumZ)*0.5f);
        if (parameters.lighting) {
            if (options_.headlight.value_or(areaScene_.empty())) {
                auto& light = parameters.lights[parameters.lightCount++];
                light.id = std::numeric_limits<std::uint64_t>::max();
                light.priority = std::numeric_limits<std::int32_t>::min();
                const auto origin = neomdl::model::transformPoint(cameraView_, {});
                const auto direction = neomdl::model::transformPoint(
                    cameraView_, {0.3f, 0.5f, 1.0f});
                light.position = {{direction.x - origin.x,
                                   direction.y - origin.y,
                                   direction.z - origin.z, 0.0f}};
                light.diffuse = {{0.92f, 0.92f, 0.92f}};
                light.ambient = {{0.0f, 0.0f, 0.0f}};
            }
        }

        // DoLightSaberBucket iterates its append-only Gob list directly. The
        // renderer receives this same instance order and bypasses depth sorting.
        submission.mesh=&mesh;
        submission.poseRevision=source.scene->revision();
        if(source.chunk)submission.ownerLifetime=instance->reference
            ?source.references->animationOwner(*instance->reference).lifetime():SceneOwnerLifetime{source.chunk->lifetime};
        else if(auto* owner=animationOwnerForId(id))submission.ownerLifetime=owner->lifetime();
        submission.resolveOwner=[this,source,saber=instance.get(),id,
            animation=animationOwnerForId(id),prepareSaberMaterial](auto& live) {
            live.mesh=&saber->renderMesh();live.poseRevision=source.scene->revision();
            live.parameters.modelView=model::multiply(cameraView_,saber->world);
            live.parameters.streamedGeometry=saber->streamsMovingGeometry();
            const auto* reference=saber->reference ? source.references->instance(*saber->reference) : nullptr;
            const auto& world=reference ? reference->world : source.chunk ? source.chunk->world
                : source.object ? source.object->world() : source.references->mainWorld();
            ownerLightSpheres_[id]={model::transformPoint(world,{}),animation->sphere().radius};
            prepareSaberMaterial(live.parameters);
        };
        if(parameters.engineLightsaberMaterial)
            submission.prepareTextures=[this](auto& p,auto& error){return textureCache_.bind(p,error);};
        if(k2Lights_) submission.lightOwner=source.object&&source.object->staticPrimary()&&!instance->reference
            ?AreaScene::sceneDynamicLightOwner:id;
        submission.inspectionLightCount = parameters.lightCount;
        submission.eyeDepth=eyeDepth;
        if(parameters.engineLightsaberMaterial) {
            // The K1 bucket contains complete owners. Insert this specialized
            // Part amongst its ordinary Parts in the loader's DFS node order.
            submission.sceneOwner=id+1u;
            const auto* sourceDocument=source.document;
            const auto& asset=instance->reference ? sourceDocument->references()[*instance->reference].model : sourceDocument->model();
            const auto* effects=source.object ? source.object->ownerVisualEffects(instance->reference)
                : source.chunk ? nullptr : &ownerEffects_.at(static_cast<std::size_t>(id));
            submission.ownerModelClassification=asset.classification;
            const auto bucket=engineOwnerBucket(asset,effects?*effects:OwnerVisualEffects{});
            auto& queue=bucket==EngineOwnerBucket::Distortion ? distortionQueue_
                : bucket==EngineOwnerBucket::Lightsaber ? lightsaberQueue_ : dynamicGobQueue_;
            auto insertion=queue.end();
            bool ownerAdmitted=false;
            for(auto it=queue.begin();it!=queue.end();++it) {
                if(it->sceneOwner!=submission.sceneOwner)continue;
                ownerAdmitted=true;
                insertion=std::next(it);
                if(it->mesh && it->mesh->nodeIndex>instance->nodeIndex) {insertion=it;break;}
            }
            // ManageSceneBSP admits the owner before any Part traversal.
            if(!ownerAdmitted)continue;
            queue.insert(insertion,std::move(submission));
        } else lightsaberQueue_.push_back(std::move(submission));
      }
    }
}

void ModelCanvas::drawLightsaberBucket() {
    if (lightsaberQueue_.empty()) return;

    // Scene::DoLightSaberBucket enables blending once, then each
    // PartLightsaber establishes its native state. Chunk entries
    // retain engine admission and resolve their current live Part at draw.
    glEnable(GL_BLEND);
    const bool engineK1=document_ && document_->model().game==model::GameVersion::Kotor1;
    if(!engineK1) {
        glBlendFunc(options_.lightsaberSettings.additiveBlur
                        ? GL_ONE : GL_SRC_ALPHA,
                    options_.lightsaberSettings.additiveBlur
                        ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(options_.lightsaberSettings.additiveBlur ? GL_FALSE : GL_TRUE);
        glStencilMask(options_.lightsaberSettings.additiveBlur ? 0u : 0xffu);
        if (options_.lightsaberSettings.lighting) glEnable(GL_LIGHTING);
        else glDisable(GL_LIGHTING);
        glDisable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK,
                      options_.lightsaberSettings.wireframe ? GL_LINE : GL_FILL);
    }

    std::string error;
    if (meshRenderer_.ready() && !meshRenderer_.drawQueue(
            lightsaberQueue_, error, render::QueueOrdering::Submission,
            [this](const auto& submission,auto& parameters,auto& message) {
                return prepareSubmission(submission,parameters,message);
            },engineK1 ? render::OwnerDrawCompletion{[this](const auto& submissions,const auto& indices,auto& message) {
                return motionBlurQueue_.record(motionBlurScene_,submissions,indices,message);
            }} : render::OwnerDrawCompletion{},
            engineK1 ? render::OwnerDrawBegin{[this](const auto& submission,auto& message) {
                if(!sceneQueryError_.empty()){message=sceneQueryError_;return false;}
                if(submission.ownerModelClassification&8u)engineDoorRenderAlpha(sceneQueries_,submission.sceneOwner-1u,
                    pickingCamera_->position,worldPlaybackSettings_.sceneFocus,
                    doorFrameElapsed_,worldPlaybackSettings_.doorFade);
                return true;
            }} : render::OwnerDrawBegin{}) &&
        !shaderErrorReported_) {
        wxLogError("NeoMDL lightsaber bucket: %s",
                   wxString::FromUTF8(error.c_str()));
        shaderErrorReported_ = true;
    }

    // PartLightsaber::Draw does not preserve arbitrary incoming state. It
    // restores these fixed engine defaults before returning.
    if(!engineK1) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LIGHTING);
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xffu);
    }
}

void ModelCanvas::drawNodeHelpers(
    const std::vector<neomdl::model::Mat4>& transforms) {
    if (!options_.nodeHelpers || !document_) return;
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDepthMask(GL_TRUE);
    const auto scale = std::max(0.05f, maximumExtent(activeBounds()) * 0.035f);
    const auto& nodes = document_->model().nodes;
    for (std::size_t nodeIndex = 0u; nodeIndex < nodes.size(); ++nodeIndex) {
        if (nodeIndex >= transforms.size()) continue;
        if (!meshVisible(nodeIndex)) continue;
        const auto& node = nodes[nodeIndex];
        if (!node.light && !node.emitter && !node.camera && !node.reference &&
            !node.lightsaber && !node.trigger) continue;
        glPushMatrix();
        const auto world = neomdl::model::multiply(
            referenceScene_.mainWorld(), transforms[nodeIndex]);
        glMultMatrixf(world.values.data());
        const bool selected=selectedNodes_.count(nodeIndex)!=0;
        glLineWidth(selected ? 3.0f : 1.5f);
        if (node.light && options_.previewLights) drawLightGlyph(*node.light, scale);
        if (node.emitter && options_.previewEmitters)
            drawEmitterGlyph(*node.emitter, nodeIndex, scale);
        const auto* activeCamera = cameraScene_.active();
        const bool viewingThroughNode = activeCamera &&
            !activeCamera->attachment.reference &&
            activeCamera->attachment.nodeIndex == nodeIndex;
        if (node.camera && options_.showCameras && !viewingThroughNode)
            drawCameraGlyph(scale);
        if (node.reference && options_.showReferences) {
            const bool loaded = std::any_of(
                document_->references().begin(), document_->references().end(),
                [nodeIndex](const ReferenceInstance& reference) {
                    return !reference.parentInstance &&
                           reference.parentNodeIndex == nodeIndex;
                });
            drawReferenceGlyph(loaded, scale);
        }
        if (node.trigger)
            drawTriggerGlyph(node.trigger->packedVerified, scale);
        if (selected && !viewingThroughNode) {
            glColor3f(1.0f, 0.72f, 0.15f);
            drawCircle(24u, scale * 1.35f, 0);
            drawCircle(24u, scale * 1.35f, 1);
            drawCircle(24u, scale * 1.35f, 2);
        }
        glPopMatrix();
    }
    glLineWidth(1.0f);
    glPointSize(1.0f);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

void ModelCanvas::drawAreaCameraGlyphs() {
    if(!options_.showCameras||cameraScene_.areaCameras().empty())return;
    glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);glDepthMask(GL_TRUE);
    const auto scale=std::max(0.05f,maximumExtent(activeBounds())*0.035f);
    const auto active=cameraScene_.activeAreaCameraIndex();
    for(std::size_t index=0;index<cameraScene_.areaCameras().size();++index) {
        if(active&&*active==index)continue;
        glPushMatrix();glMultMatrixf(cameraScene_.areaCameras()[index].world.values.data());
        drawCameraGlyph(scale);glPopMatrix();
    }
    glLineWidth(1.0f);glPointSize(1.0f);glDisable(GL_BLEND);glDepthMask(GL_TRUE);
}

void ModelCanvas::drawLightGlyph(const neomdl::model::LightNodeData& light,
                                 float scale) {
    const auto authored = std::fabs(light.flareRadius);
    const auto radius = std::clamp(authored > 0.001f ? authored : scale,
                                   scale * 0.6f, scale * 4.0f);
    glColor3f(light.ambientOnly ? 0.55f : 1.0f,
              light.ambientOnly ? 0.72f : 0.84f, 0.25f);
    drawCircle(24u, radius, 0);
    drawCircle(24u, radius, 1);
    drawCircle(24u, radius, 2);
    glBegin(GL_LINES);
    for (int axis = 0; axis < 3; ++axis) {
        for (int sign = -1; sign <= 1; sign += 2) {
            const auto length = radius * 1.35f * static_cast<float>(sign);
            glVertex3f(0.0f, 0.0f, 0.0f);
            glVertex3f(axis == 0 ? length : 0.0f,
                       axis == 1 ? length : 0.0f,
                       axis == 2 ? length : 0.0f);
        }
    }
    glEnd();
    if (light.generateFlare) {
        glPointSize(7.0f);
        glBegin(GL_POINTS);
        glVertex3f(0.0f, 0.0f, 0.0f);
        glEnd();
    }
}

void ModelCanvas::drawEmitterGlyph(
    const neomdl::model::EmitterNodeData& emitter,
    std::size_t nodeIndex, float scale) {
    const auto radius = std::clamp(std::fabs(emitter.blastRadius),
                                   scale * 0.5f, scale * 4.0f);
    const auto length = std::clamp(std::fabs(emitter.blastLength),
                                   scale, scale * 8.0f);
    glColor3f(0.96f, 0.42f, 0.16f);
    glBegin(GL_LINES);
    for (unsigned index = 0u; index < 8u; ++index) {
        constexpr float tau = 6.28318530717958647692f;
        const auto angle = tau * static_cast<float>(index) / 8.0f;
        glVertex3f(0.0f, 0.0f, 0.0f);
        glVertex3f(std::cos(angle) * radius, std::sin(angle) * radius, length);
    }
    glEnd();
    drawCircle(16u, radius, 0);

    // Selection helper only; live particles are indexed shader submissions.
    (void)nodeIndex;

}

void ModelCanvas::drawCameraGlyph(float scale) {
    const auto depth = scale * 3.0f;
    const auto halfHeight = depth * std::tan(
        options_.cameraSettings.viewAngle *
        3.14159265358979323846f / 360.0f);
    const auto halfWidth = halfHeight * (4.0f / 3.0f);
    glColor3f(0.25f, 0.78f, 1.0f);
    glBegin(GL_LINES);
    // The external Camera uses OpenGL's local -Z forward convention.
    const std::array<neomdl::model::Vec3, 4> corners{{
        {-halfWidth, -halfHeight, -depth}, {halfWidth, -halfHeight, -depth},
        {halfWidth, halfHeight, -depth}, {-halfWidth, halfHeight, -depth},
    }};
    for (const auto& corner : corners) {
        glVertex3f(0.0f, 0.0f, 0.0f);
        glVertex3f(corner.x, corner.y, corner.z);
    }
    for (std::size_t index = 0u; index < corners.size(); ++index) {
        const auto& a = corners[index];
        const auto& b = corners[(index + 1u) % corners.size()];
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(b.x, b.y, b.z);
    }
    glEnd();
}

void ModelCanvas::drawReferenceGlyph(bool loaded, float scale) {
    glColor3f(loaded ? 0.25f : 0.95f, loaded ? 0.9f : 0.25f,
              loaded ? 0.88f : 0.25f);
    const auto radius = scale * 0.9f;
    const std::array<neomdl::model::Vec3, 6> points{{
        {radius, 0.0f, 0.0f}, {-radius, 0.0f, 0.0f},
        {0.0f, radius, 0.0f}, {0.0f, -radius, 0.0f},
        {0.0f, 0.0f, radius}, {0.0f, 0.0f, -radius},
    }};
    static constexpr std::array<std::array<unsigned, 2>, 12> edges{{
        {{0u, 2u}}, {{0u, 3u}}, {{0u, 4u}}, {{0u, 5u}},
        {{1u, 2u}}, {{1u, 3u}}, {{1u, 4u}}, {{1u, 5u}},
        {{2u, 4u}}, {{2u, 5u}}, {{3u, 4u}}, {{3u, 5u}},
    }};
    glBegin(GL_LINES);
    for (const auto& edge : edges) {
        const auto& a = points[edge[0]];
        const auto& b = points[edge[1]];
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(b.x, b.y, b.z);
    }
    glEnd();
}

void ModelCanvas::drawTriggerGlyph(bool packedVerified, float scale) {
    // MdlNodeTrigger has no PartTrigger instance in K2. This glyph is an
    // inspection marker only and deliberately does not imply a stored volume.
    glColor3f(packedVerified ? 0.95f : 0.65f,
              packedVerified ? 0.25f : 0.45f,
              packedVerified ? 0.75f : 0.65f);
    const auto radius = scale * 0.9f;
    glBegin(GL_LINE_LOOP);
    glVertex3f(radius, 0.0f, 0.0f);
    glVertex3f(0.0f, radius, 0.0f);
    glVertex3f(-radius, 0.0f, 0.0f);
    glVertex3f(0.0f, -radius, 0.0f);
    glEnd();
    glBegin(GL_LINES);
    glVertex3f(0.0f, 0.0f, -radius);
    glVertex3f(0.0f, 0.0f, radius);
    glVertex3f(-radius * 0.55f, 0.0f, radius * 0.55f);
    glVertex3f(radius * 0.55f, 0.0f, radius * 0.55f);
    glEnd();
}

void ModelCanvas::drawMesh(const neomdl::model::Mesh& mesh,
                           const neomdl::model::DeformedMesh* deformed,
                           bool selected, const neoshared::material::Material& material,
                           const neomdl::model::NodeMaterialState& nodeMaterial,
                           neomdl::model::GameVersion game, bool modelFog, LightOwnerId lightOwner,
                           const ModelDocument* resources, float localTime,
                           std::vector<neomdl::render::MeshSubmission>* targetQueue,
                           std::uint64_t sceneOwner, float ownerAlpha,
                           std::uint16_t ownerModelClassification,
                           bool ownerHasTransparency, float ownerDistance,
                           bool allowK2SkinEffects, const OwnerVisualEffects* ownerEffects,
                           const model::Mat4* engineOwnerFrame,
                           std::optional<float> ownerTextureAngleDegrees,
                           const HitPartTransform* enginePartFrame,std::string* failure,
                           std::optional<float> retainedEyeDepth) {
    if (!meshRenderer_.ready()) return; // Explicit shader failure; no silent immediate-mode fallback.
    render::MeshSubmission submission;
    auto& parameters=submission.parameters;
    // Camera/helper transforms remain compatibility state in this stage; the
    // mesh program receives explicit matrix uniforms and generic attributes.
    glGetFloatv(GL_MODELVIEW_MATRIX, parameters.modelView.values.data());
    glGetFloatv(GL_PROJECTION_MATRIX, parameters.projection.values.data());
    neomdl::material::SurfaceInputs surface;
    surface.opacity=nodeMaterial.alpha;
    surface.instanceOpacity=ownerEffects?ownerEffects->instanceOpacity:1.0f;
    surface.skinned=mesh.skinned;
    surface.selfIllumination=nodeMaterial.selfIllumination;
    surface.vertexAlpha=mesh.hasVertexColors && std::any_of(mesh.vertices.begin(),mesh.vertices.end(),
        [](const auto& vertex) {return vertex.color[3]!=255;});
    const auto sampledTime=localTime>=0?localTime:materialTime_;
    // PartDraw executes Mat before PartTriMesh::Draw rejects its Part alpha.
    // Preserve the material event even when no geometry is drawn. Skin has
    // no TriMesh alpha early-out; its shell still executes (0x10fdc1).
    const bool materialOnly=!mesh.skinned && !mesh.animated && surface.opacity<=0.001f &&
        (game==model::GameVersion::Kotor1 || std::isfinite(surface.opacity));
    const auto* textureSource=resources ? resources : document_;
    neoshared::material::TextureBinding ownerEnvironmentBinding;
    if(ownerEffects&&!ownerEffects->environmentMap.empty()) {
        ownerEnvironmentBinding.resref=ownerEffects->environmentMap;
        ownerEnvironmentBinding.key=neoshared::material::textureResourceKey(ownerEnvironmentBinding.resref);
        if(textureSource)ownerEnvironmentBinding.metadata=textureSource->textureMetadata(ownerEnvironmentBinding.key);
    }
    neomdl::material::EnginePartTriMeshInputs inputs;
    inputs.engineHardware=options_.engineHardware;
    inputs.lightmapped=mesh.lightmapped;
    inputs.enableLightmap=options_.lightmaps;
    // MdlNodeTriMesh::HasTangentBases checks the stored offset only. The
    // geometry path can still generate a safe basis for malformed rows.
    inputs.tangentBases=mesh.mdxTangentOffset!=0xffffffffu;
    inputs.ownerEnvironmentMap=!ownerEnvironmentBinding.key.empty() &&
        ownerEnvironmentBinding.metadata && ownerEnvironmentBinding.metadata->facts.hasPixels;
    parameters.material=neomdl::material::evaluateEnginePartTriMeshMaterial(
        material,surface,inputs,
        game==model::GameVersion::Kotor1 ? material::EngineMaterialGame::Kotor1
                                        : material::EngineMaterialGame::Kotor2,
        sampledTime);
    if(mesh.animated) {
        parameters.engineAnimMesh=true;
        parameters.material.renderPath=material::TriMeshRenderPath::Flat;
        parameters.material.useLightmap=false;
        parameters.material.useBumpMap=false;
        parameters.material.useEnvironmentMap=false;
    }
    parameters.eyeToWorld=eyeToWorld_;
    // Scene::RenderSinglePass uploads the rigid camera inverse as env92..89.
    for(std::size_t c=0;c<3;++c) for(std::size_t row=0;row<3;++row)
        parameters.worldFromEye.values[c*4+row]=cameraView_.values[row*4+c];
    for(std::size_t row=0;row<3;++row)
        parameters.worldFromEye.values[12+row]=-(
            parameters.worldFromEye.values[row]*cameraView_.values[12]+
            parameters.worldFromEye.values[4+row]*cameraView_.values[13]+
            parameters.worldFromEye.values[8+row]*cameraView_.values[14]);
    // Material::InitializeTextures registers texture slot zero independently of
    // whether the renderer later displays textures. Preserve that order so the
    // K2 static mesh buckets do not change when the viewer toggle is disabled.
    if(textureSource && !material.diffuse.key.empty()) {
        const auto* texture=textureSource->texture(material.diffuse.key);
        if(texture && texture->hasPixels())
            parameters.diffuseTextureOrder=textureCache_.orderFor(
                *texture, parameters.material.diffuseUv.layer);
    }
    if(options_.textures) {
        const auto diffuse=textureFor(material.diffuse,parameters.material.diffuseUv.layer,resources);
        // A cubemap must never be rebound as a 2D diffuse texture.
        parameters.diffuseTexture=diffuse.cube ? 0u : diffuse.id;
        if(diffuse.order!=0u) parameters.diffuseTextureOrder=diffuse.order;
        if(parameters.material.useLightmap) {
            const auto lightmap=textureFor(material.lightmap,0,resources);
            parameters.lightmapTexture=lightmap.id;
            parameters.lightmapCube=lightmap.cube;
        }
        if(parameters.material.useBumpMap) {
            const auto bump=textureFor(
                material.bumpMap,parameters.material.bumpUv.layer,resources);
            parameters.bumpTexture=bump.cube ? 0u : bump.id;
        }
        if(parameters.material.useEnvironmentMap) {
            // K2 Material has one shared runtime slot. envmaptexture is loaded
            // after bumpyshinytexture and therefore has precedence.
            const auto& envBinding=!material.environmentMap.key.empty()
                ? material.environmentMap : material.bumpyShiny;
            const auto environment=textureFor(envBinding,0,resources);
            parameters.environmentTexture=environment.id;
            parameters.environmentCube=environment.cube;
        }
        if(inputs.ownerEnvironmentMap) {
            const auto environment=textureFor(ownerEnvironmentBinding,0,resources);
            parameters.environmentTexture=environment.id;
            parameters.environmentCube=environment.cube;
            if(environment.id&&!environment.cube && !shaderErrorReported_) {
                wxLogWarning("NeoMDL owner environment map %s is not a cubemap",
                    wxString::FromUTF8(ownerEnvironmentBinding.key.c_str()));
                shaderErrorReported_=true;
            }
        }
    }
    if(dirtySettings_.enabled && dirtyMesh_==&mesh &&
       (lightOwner==dirtyOwner_ || (dirtyOwner_==mainLightOwner&&!resources))) {
        if(game!=model::GameVersion::Kotor2||mesh.skinned||mesh.animated||!options_.textures||!options_.lighting) {
            if(proceduralTextureError_.empty())proceduralTextureError_="Dirty diagnostic requires a rigid K2 mesh with textures and lighting enabled";
            if(failure)*failure=proceduralTextureError_;
            return;
        }
        const auto key=material::dirtyTextureNames[dirtySettings_.slot];
        const auto* image=document_->texture(key);const auto meta=document_->textureMetadata(key);
        std::string error;
        const auto volume=image&&meta?textureCache_.get(*image,meta.get(),0,error):render::GpuTexture{};
        if(!error.empty()||!volume.volume||!volume.id||!volume.dirtyInputs) {
            if(proceduralTextureError_.empty())proceduralTextureError_=error.empty()?std::string("Unresolved engine Dirty volume: ")+key:error;
            if(failure)*failure=proceduralTextureError_;
            return;
        }
        parameters.dirty={dirtySettings_,volume.id,volume.dirtyInputs->parameter};
        parameters.material.useBumpMap=false;
    }
    parameters.lighting = options_.lighting;
    parameters.inspectMaterial=!options_.textures || !options_.lighting;
    parameters.partHasOwner=lightOwner!=AreaScene::sceneDynamicLightOwner;
    parameters.forceSight=effectiveForceSight();
    if(ownerEffects)parameters.forceSightOwner=ownerEffects->forceSight;
    parameters.cullBackfaces = options_.cullBackfaces;
    parameters.wireframe = options_.wireframe;
    parameters.selected = selected;
    parameters.basisDebug = options_.tangentBasisDebug;
    parameters.sceneAmbient=effectiveSceneAmbient();
    parameters.fog=neomdl::model::fogForModel(effectiveFog(),modelFog);
    // The viewer owns a current Scene in both isolated-model and area modes.
    // This is PartSkin::Draw's outer gate, separate from ARB VP availability.
    parameters.engineSkinShellVertexPrograms=parameters.material.engineHardware.nv2xLevelShaders &&
        parameters.material.engineHardware.nonRadeon8500Validation;
    parameters.engineSkinVertexPrograms=options_.engineUseVertexPrograms && parameters.engineSkinShellVertexPrograms;
    parameters.engineCpuSkin=mesh.skinned &&
        !parameters.engineSkinVertexPrograms;
    const bool engineSkinVertexProgram =
        mesh.skinned &&
        deformed && deformed->valid && parameters.engineSkinVertexPrograms;
    parameters.exactK2SkinVertexProgram=engineSkinVertexProgram;
    parameters.skinOwnerAlpha=ownerAlpha;
    // Runtime PartTriMesh +0x94, not authored rotateTexture or a UV velocity.
    // Particle chunks/area rooms have independent hosts and do not inherit it.
    if(ownerTextureAngleDegrees) parameters.textureAngleDegrees=*ownerTextureAngleDegrees;
    else if(ownerEffects && !ownerEffects_.empty()) {
        for(std::size_t owner=0;owner<ownerEffects_.size();++owner)
            if(ownerEffects==&ownerEffects_[owner]) {
                parameters.textureAngleDegrees=textureAngle(owner,mesh.nodeIndex);break;
            }
    }
    // GetPosAndOri is an explicit owner position/quaternion, not an inverse
    // scaled model-view matrix. The pose carries the separate part frame.
    if(engineOwnerFrame) parameters.skinOwnerFrame=*engineOwnerFrame;
    parameters.hasSkinOwnerFrame=engineOwnerFrame!=nullptr;
    if(enginePartFrame) {
        parameters.enginePartPosition=enginePartFrame->position;
        parameters.enginePartOrientation=enginePartFrame->orientation;
    }
    parameters.lightCount = 0u;
    float eyeDepth;
    if(retainedEyeDepth)eyeDepth=*retainedEyeDepth;
    else {
        // Posed mesh bounds supply the initial queue depth; late draws retain it.
        float minimumZ{},maximumZ{};
        const auto& matrix=parameters.modelView.values;
        for(std::size_t n=0;n<mesh.vertices.size();++n) {
            const auto& point=deformed ? deformed->positions[n] : mesh.vertices[n].position;
            const float z=matrix[2]*point.x+matrix[6]*point.y+
                matrix[10]*point.z+matrix[14];
            if(n==0)minimumZ=maximumZ=z;
            else {minimumZ=std::min(minimumZ,z);maximumZ=std::max(maximumZ,z);}
        }
        eyeDepth=mesh.vertices.empty()?-0.0f:-((minimumZ+maximumZ)*0.5f);
    }
    if(options_.lighting) {
        if(options_.headlight.value_or(areaScene_.empty())) {
            auto& light=parameters.lights[parameters.lightCount++];
            light.id=std::numeric_limits<std::uint64_t>::max();light.priority=std::numeric_limits<std::int32_t>::min();
            // Scene-oriented preview key light; not an authored MDL light.
            const auto origin=neomdl::model::transformPoint(cameraView_,{});
            const auto direction=neomdl::model::transformPoint(cameraView_,{0.3f,0.5f,1.0f});
            light.position={{direction.x-origin.x,direction.y-origin.y,direction.z-origin.z,0}};
            light.diffuse={{0.92f,0.92f,0.92f}};light.ambient={{0,0,0}};
        }
    }
    auto& queue = targetQueue ? *targetQueue : renderQueue_;
    submission.mesh=&mesh;
    submission.deformed=deformed;
    submission.poseRevision=poseRevision_;
    submission.drawPrimary=!materialOnly;submission.materialOnly=materialOnly;
    submission.prepareTextures=[this](auto& p,auto& error) {return textureCache_.bind(p,error);};
    if (k2Lights_ && !model::engineSceneStageOrder(game).empty())
        submission.lightOwner=lightOwner;
    submission.inspectionLightCount=parameters.lightCount;
    submission.eyeDepth=eyeDepth;
    submission.sceneOwner=sceneOwner;
    submission.ownerAlpha=ownerAlpha;
    submission.ownerModelClassification=ownerModelClassification;
    submission.ownerHasTransparency=ownerHasTransparency;
    submission.ownerCameraDistance=ownerDistance;
    if(ownerEffects) {
        render::OwnerEffectTextures effects;
        const bool engineHologram=ownerEffects->hologram &&
            (!mesh.skinned || parameters.engineSkinVertexPrograms);
        if(engineHologram) {
            const auto filter=textureForResref("Holotex",resources);
            effects.hologramFilter=filter.cube ? 0u : filter.id;
        }
        // Engine effect programs sample their textures even when the ordinary
        // material preview toggle is off; there is no untextured replacement.
        if((engineHologram || ownerEffects->perObjectIlluminationEnabled) &&
           !submission.parameters.diffuseTexture) {
            const auto diffuse=textureFor(material.diffuse,parameters.material.diffuseUv.layer,resources);
            submission.parameters.diffuseTexture=diffuse.cube ? 0u : diffuse.id;
        }
        if(ownerEffects->perObjectIlluminationEnabled && parameters.material.useEnvironmentMap &&
           !submission.parameters.environmentTexture) {
            const auto& binding=!material.environmentMap.key.empty() ? material.environmentMap : material.bumpyShiny;
            const auto env=textureFor(binding,0,resources);
            submission.parameters.environmentTexture=env.id;
            submission.parameters.environmentCube=env.cube;
        }
        if(ownerEffects->shellAmount!=0.0f) {
            const auto shell=textureForResref(ownerEffects->shellTexture,resources);
            effects.shell=shell.cube ? 0u : shell.id;
            const auto* effectSource=resources?resources:document_;
            effects.shellMetadata=effectSource?effectSource->textureMetadata(ownerEffects->shellTexture):nullptr;
            effects.shellTxiAvailable=effectSource&&effectSource->runtimeTextureTxiAvailable(ownerEffects->shellTexture);
        }
        std::string error;
        if(!render::configureOwnerVisualEffects(*ownerEffects,effects,material,nodeMaterial,submission,error)) {
            if(failure)*failure=error;
            if(!shaderErrorReported_) {
                wxLogError("NeoMDL owner effect: %s",wxString::FromUTF8(error.c_str()));
                shaderErrorReported_=true;
            }
            return;
        }
    }
    queue.push_back(std::move(submission));

    const bool engineSkinEffects = allowK2SkinEffects &&
        !model::engineSceneStageOrder(game).empty() && engineSkinVertexProgram;
    if (!engineSkinEffects) return;

    const auto& queued=queue.back();
    const auto& skin=options_.skinSettings;
    const float motionLengthSquared=skin.objectMotion.x*skin.objectMotion.x+
        skin.objectMotion.y*skin.objectMotion.y+
        skin.objectMotion.z*skin.objectMotion.z;
    if (skin.motionBlur && motionLengthSquared > 0.0f) {
        const auto effectSpace=skinEffectSpace(queued.parameters.modelView);
        auto blur=queued;
        // RenderSkinnedVPBlur is a separate PartSkin::Draw(true) branch; it
        // neither samples HoloFP nor consults the hologram exclusion byte.
        // PartSkin still appends its shell after either primary branch.
        blur.drawPrimary=true;
        blur.parameters.ownerFragment=render::OwnerFragmentEffect::None;
        blur.parameters.skinEffect=render::SkinRenderEffect::MotionBlur;
        blur.parameters.skinMotion=skin.objectMotion;
        blur.parameters.skinMaximumBlurLength=skin.maximumBlurLength;
        blur.parameters.skinBlurSideScale=skin.sideExtrusion
            ? skin.sideScale : 0.0f;
        blur.parameters.skinCameraPosition=effectSpace.cameraPosition;
        blur.parameters.skinBlurSideDirection=effectSpace.sideDirection;
        blur.parameters.selected=false;
        blur.parameters.outlineOnly=false;
        blur.parameters.lighting=false;
        blur.parameters.lightCount=0u;
        blur.parameters.hasColorOverride=true;
        blur.parameters.colorOverride={{1.0f,1.0f,1.0f,1.0f}};
        blur.parameters.basisDebug=0;
        blur.parameters.material.pass=neomdl::material::RenderPass::Translucent;
        blur.parameters.material.blending=true;
        blur.parameters.material.depthWrite=false;
        blur.parameters.material.alphaTest=false;
        blur.parameters.material.blendFunction=
            neomdl::material::BlendFunction::Alpha;
        blur.parameters.material.useLightmap=false;
        blur.parameters.material.useBumpMap=false;
        blur.parameters.material.useEnvironmentMap=false;
        blur.parameters.material.selfIllumination={{0.0f,0.0f,0.0f}};
        // Scene::DoBlurBuckets iterates the owning-Gob append list directly.
        diagnosticSkinBlurQueue_.push_back(std::move(blur));
    }
}

} // namespace neomdl::ui
