#pragma once

#include "core/ModelDocument.hpp"
#include "core/AreaVisualRecords.hpp"
#include "core/ReferenceScene.hpp"
#include "core/SceneQueries.hpp"
#include "core/DialogueSequence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

// The K1/K2 animated-camera path owns one external CAurCamera and attaches it to a
// model part named "camerahook". The camera part itself has no private payload
// or field-of-view controller; it is a generic animated PartCamera.
inline constexpr const char* kK2AnimatedCameraHookName = "camerahook";
inline constexpr float kK2CameraDefaultViewAngle = 45.0f;
inline constexpr float kK2AnimatedCameraNearClip = 0.1f;
inline constexpr float kK2AnimatedCameraFarClip = 10000.0f;

// Camera::MouseDir / HitCheck inputs, verified for K1/K2 Desktop. Coordinates
// have the viewport's bottom-left origin. Supply the stored viewport when
// width and height are nonzero; engine otherwise reads GL_VIEWPORT.
struct CameraHitViewport {int x{},y{},width{},height{};};
struct CameraHitState {
    model::Vec3 position{};
    model::Quaternion orientation{}; // raw camera orientation, no reassembly
    float viewAngle{45.0f},orthographicHeight{1.0f};
    float nearClip{0.1f},farClip{100.0f}; // Camera constructor, not external clip setup
    CameraHitViewport viewport;
};
struct CameraHitSegment {model::Vec3 start{},end{};};
model::Vec3 engineCameraMouseDirection(const CameraHitState&,int x,int y,
                                      model::GameVersion game) noexcept;
// Perspective accepts pixels outside the viewport. The non-perspective
// branch rejects them and uses its engine world-axis-aligned segment.
std::optional<CameraHitSegment> engineCameraHitSegment(const CameraHitState&,
    int x,int y,model::GameVersion game) noexcept;

struct CameraAttachment {
    // A null reference identifies the main camera-model object. A populated
    // value identifies one of ReferenceScene's separately owned objects.
    std::optional<std::size_t> reference;
    std::size_t nodeIndex{};

    friend bool operator==(const CameraAttachment& left,
                           const CameraAttachment& right) noexcept {
        return left.reference == right.reference &&
               left.nodeIndex == right.nodeIndex;
    }
    friend bool operator!=(const CameraAttachment& left,
                           const CameraAttachment& right) noexcept {
        return !(left == right);
    }
};

struct RuntimeAreaCamera {
    AreaCameraRecord authored;
    neomdl::model::Mat4 world = neomdl::model::identityMatrix();
    neomdl::model::Mat4 view = neomdl::model::identityMatrix();
    neomdl::model::Vec3 position{};
    neomdl::model::Quaternion orientation{};
    neomdl::model::Vec3 right{1.0f, 0.0f, 0.0f};
    neomdl::model::Vec3 up{0.0f, 1.0f, 0.0f};
    neomdl::model::Vec3 forward{0.0f, 0.0f, -1.0f};
};

struct RuntimeCameraAttachment {
    CameraAttachment attachment;
    std::string nodeName;
    neomdl::model::Mat4 world = neomdl::model::identityMatrix();
    neomdl::model::Mat4 view = neomdl::model::identityMatrix();
    neomdl::model::Vec3 position{};
    neomdl::model::Quaternion orientation{};
    neomdl::model::Vec3 right{1.0f, 0.0f, 0.0f};
    neomdl::model::Vec3 up{0.0f, 1.0f, 0.0f};
    neomdl::model::Vec3 forward{0.0f, 0.0f, -1.0f};
    bool cameraNode{};
    bool engineHook{};
};

// Actual participant inputs to CSWBehaviorCameraDialog. Camera height is the
// game object's engine height getter; cameraHook is an animated CAMERAHOOK.
// Neither is inferred from model drawing bounds.
struct DialogueCameraActor {
    HitOwnerId object{};
    model::Vec3 position{};
    float cameraHeight{};
    std::optional<model::Vec3> cameraHook;
    std::vector<HitOwnerId> ignoredOwners; // engine body and optional head owners
};
// Owner frames retain engine quaternion words; drawing matrices are outputs.
float dialogueCreatureHeadHeight(const model::Model* body,const model::Pose& pose,
    const model::EngineFrame& owner,float creaturePositionZ,float appearanceHeight);
std::optional<model::Vec3> dialogueCreatureCameraHook(const model::Model& body,
    const model::Pose& pose,const model::EngineFrame& owner);
struct DialogueCameraOptions {
    bool oldHitCheck{};
    float heightOffset{};
    float targetHeightOffset{};
    float pullback{}; // engine UpdateShotCamera close-up pullback state
    // Engine global shot overrides. Zero selects the function's shot constant.
    float targetFraction{}, distanceFraction{}, yawDegrees{};
    model::GameVersion game{model::GameVersion::Kotor2};
};
enum class DialogueActorShot : std::uint32_t {Closeup=1u,OverShoulder=2u,TwoShot=3u,Angle4=4u};

// CSWSDialog chooses CameraAngle=0 while sending entries/reply lists, before
// the client sees a shot. The chosen angle remains on that node across loops.
class DialogueCameraSequence final {
public:
    void initialize(const std::string& engineResref,std::vector<std::uint32_t> authoredAngles,
        model::GameVersion game=model::GameVersion::Kotor2);
    void beginEntry(std::size_t lineIndex);
    void prepareReplies(const std::vector<std::size_t>& lineIndices);
    std::uint32_t angle(std::size_t lineIndex) const {return angles_.at(lineIndex);}
private:
    std::uint32_t next();
    std::vector<std::uint32_t> angles_;
    DialogueSequenceCursor cursor_;
    bool firstEntry_{true},firstReplies_{true};
};

// The engine actor-camera behavior, separate from the external animated camera
// model. SetShot establishes the pair/obstruction state; Control updates from
// current actor hooks without selecting another side of the line of action.
class DialogueCameraScene final {
public:
    void reset() noexcept;
    void setPlaceableCamera(const RuntimeAreaCamera& camera);
    // Nullopt denotes a false engine dialogue-animation predicate. A present
    // value is dialoganimations.CU_PB_Range, including its missing-cell zero.
    float selectPullback(DialogueActorShot shot,std::optional<float> tableValue,
                         bool speakerAppearance565,
                         model::GameVersion game=model::GameVersion::Kotor2);
    bool setShot(DialogueActorShot shot,const DialogueCameraActor& speaker,
                 const DialogueCameraActor& listener,const DialogueCameraOptions&,
                 const SceneQueries& queries);
    void update(const DialogueCameraActor& speaker,const DialogueCameraActor& listener,
                const SceneQueries& queries);
    const std::optional<RuntimeAreaCamera>& frame() const noexcept {return frame_;}
private:
    struct LineOfAction {HitOwnerId speaker{},listener{};std::uint32_t side{};};
    struct Parameters {model::Vec3 position{},target{},direction{};model::Quaternion orientation{};};
    Parameters parameters(std::uint32_t shot,std::uint32_t side,
        model::Vec3 speaker,model::Vec3 listener,
        const DialogueCameraActor&,const DialogueCameraActor&,const SceneQueries&,
        model::Vec3 retainedPosition={});
    std::uint32_t sideFor(const DialogueCameraActor&,const DialogueCameraActor&,
                         const SceneQueries&);
    std::uint32_t savedSide(HitOwnerId speaker,HitOwnerId listener) const noexcept;
    std::array<LineOfAction,4> lines_{};
    DialogueCameraOptions options_;
    std::uint32_t shot_{};
    bool obstructed_{};
    float speakerHeight_{},listenerHeight_{};
    float retainedPullback_{};
    model::Vec3 fixedPosition_{};
    std::optional<RuntimeAreaCamera> frame_;
};

// Runtime counterpart of the packed's external Camera-to-Part attachment.
// K1 and K2 share the attachment behavior.
class CameraScene final {
public:
    void setDocument(const ModelDocument* document);
    void setReferenceScene(const ReferenceScene* references) noexcept {
        references_ = references;
    }
    void reset();
    void synchronize(const neomdl::model::Pose& mainPose,
                     bool includeReferences = true,
                     bool updateAttachment = true);

    bool activate(const CameraAttachment& attachment);
    bool activateMainCameraNode(std::size_t nodeIndex);
    bool activateEngineHook();
    bool setAreaCameras(const std::vector<AreaCameraRecord>& records,
                        std::string& error);
    void clearAreaCameras();
    bool activateAreaCamera(std::size_t index);
    bool activateAreaCameraId(std::int32_t cameraId);
    // CAurBehaviorAttach loses its target by clearing the behavior; the
    // independent Camera keeps its last frame (K2 Desktop 0x80d56..0x80d66).
    void detachAttachment() noexcept { activeAttachment_.reset(); ++attachmentRevision_; }
    void deactivate() noexcept {
        activeAttachment_.reset(); activeFrame_.reset(); activeAreaCamera_.reset();
        ++attachmentRevision_;
    }

    bool engineRuntime() const noexcept { return engineRuntime_; }
    bool kotor2Runtime() const noexcept {
        return engineRuntime_ && document_->model().game==model::GameVersion::Kotor2;
    }
    const std::vector<RuntimeCameraAttachment>& attachments() const noexcept {
        return attachments_;
    }
    const RuntimeCameraAttachment* active() const noexcept;
    const RuntimeAreaCamera* activeAreaCamera() const noexcept;
    const std::vector<RuntimeAreaCamera>& areaCameras() const noexcept {return areaCameras_;}
    const std::optional<std::size_t>& activeAreaCameraIndex() const noexcept {return activeAreaCamera_;}
    bool anyActive() const noexcept {return active()!=nullptr||activeAreaCamera()!=nullptr;}
    const RuntimeCameraAttachment* mainNode(std::size_t nodeIndex) const noexcept;
    const RuntimeCameraAttachment* engineHook() const noexcept;
    const std::optional<CameraAttachment>& activeAttachment() const noexcept {
        return activeAttachment_;
    }
    bool activeUsesEngineHook() const noexcept;
    std::uint64_t revision() const noexcept { return revision_; }
    std::uint64_t attachmentRevision() const noexcept { return attachmentRevision_; }

private:
    void appendObject(const neomdl::model::Model& model,
                      const neomdl::model::Pose& pose,
                      const neomdl::model::EngineFrame& owner,
                      std::optional<std::size_t> reference);
    const RuntimeCameraAttachment* find(
        const CameraAttachment& attachment) const noexcept;

    const ModelDocument* document_{};
    const ReferenceScene* references_{};
    bool engineRuntime_{};
    std::vector<RuntimeCameraAttachment> attachments_;
    std::vector<RuntimeAreaCamera> areaCameras_;
    std::optional<CameraAttachment> activeAttachment_;
    std::optional<RuntimeCameraAttachment> activeFrame_;
    std::optional<std::size_t> activeAreaCamera_;
    std::uint64_t revision_{};
    std::uint64_t attachmentRevision_{};
};

} // namespace neomdl
