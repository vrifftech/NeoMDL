#pragma once

#include "core/ModelDocument.hpp"

#include <wx/panel.h>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>

class wxButton;
class wxChoice;
class wxSearchCtrl;
class wxStaticText;
class wxStyledTextCtrl;
class wxTextCtrl;

namespace neomdl::ui {

bool confirmCompanionOverwrite(wxWindow* parent,const std::vector<std::filesystem::path>& paths);
using AuthoringJob = std::function<bool(const wxString&,
    const std::function<void(const neoshared::bif::JobControl&)>&)>;

class AsciiEditor final : public wxPanel {
public:
    AsciiEditor(wxWindow* parent, std::shared_ptr<const ModelDocument> source,
                std::function<void(std::unique_ptr<ModelDocument>)> apply,
                std::function<void()> changed);
    ~AsciiEditor() override;

    bool ready() const noexcept { return ready_ && !generating_ && !requestedFormat_; }
    bool previewCurrent() const noexcept { return ready() && previewRevision_ == revision_; }
    bool advanced() const noexcept { return advanced_; }
    bool edited() const noexcept { return revision_ != 0; }
    bool modified() const;
    const std::filesystem::path& savePath() const noexcept { return savePath_; }
    std::string text() const;
    const std::shared_ptr<const ModelDocument>& source() const { return source_; }
    void updatePreview();
    void setPreviewEnabled(bool enabled) { previewEnabled_ = enabled; }
    void setAppearance(bool dark);
    void cancelAndWait();
    bool save(bool choosePath = false);
    const CollisionSet& collisions() const noexcept { return collisions_; }
    void editCollisions(const model::Model& model);
    void useRoomCollision(const model::Model& model,std::size_t node,
                         const CollisionMesh& collision,model::Vec3 placement);
    void editNode(const model::Model& model, std::size_t nodeIndex);
    void editVertices(const model::Model& model, std::size_t nodeIndex);
    void convertMesh(const model::Model& model, std::size_t nodeIndex, const AuthoringJob& runJob);
    void recalculateNormals(const model::Model& model, std::size_t nodeIndex, const AuthoringJob& runJob);
    void editMeshReplacements(const model::Model& model, std::size_t nodeIndex, const AuthoringJob& runJob);
    const std::vector<std::filesystem::path>& replacementInputs() const noexcept { return replacementInputs_; }
    void editControllers(const model::Model& model, std::size_t nodeIndex);
    void editModel(const model::Model& model);
    void renameTexture();
    void exportTextureList();

private:
    struct Result {
        std::string text;
        std::unique_ptr<ModelDocument> document;
    };
    void poll();
    void installText(const std::string& text);
    void findNext();
    void replaceText(const std::string& text);
    void generateAscii(bool advanced);
    void setSavePath(const std::filesystem::path& path);

    std::shared_ptr<const ModelDocument> source_;
    std::function<void(std::unique_ptr<ModelDocument>)> apply_;
    std::function<void()> changed_;
    wxStyledTextCtrl* editor_{};
    wxSearchCtrl* search_{};
    wxChoice* format_{};
    wxTextCtrl* status_{};
    wxStaticText* saveLocation_{};
    wxButton* update_{};
    wxTimer timer_;
    std::future<Result> worker_;
    std::atomic<bool> cancelled_{false};
    std::size_t revision_{}, workerRevision_{}, previewRevision_{};
    std::chrono::steady_clock::time_point updateAt_{};
    std::chrono::steady_clock::time_point workStarted_{};
    long displayedSeconds_{-1};
    bool generating_{}, ready_{}, pending_{}, previewEnabled_{true};
    bool advanced_{}, generationAdvanced_{};
    std::optional<bool> requestedFormat_;
    std::filesystem::path savePath_;
    CollisionSet collisions_;
    bool collisionsModified_{};
    std::vector<std::filesystem::path> replacementInputs_;
};

} // namespace neomdl::ui
