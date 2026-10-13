#pragma once

#include "core/ModelDocument.hpp"

#include <wx/panel.h>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>

class wxButton;
class wxSearchCtrl;
class wxStyledTextCtrl;
class wxTextCtrl;

namespace neomdl::ui {

class AsciiEditor final : public wxPanel {
public:
    AsciiEditor(wxWindow* parent, std::shared_ptr<const ModelDocument> source,
                std::function<void(std::unique_ptr<ModelDocument>)> apply,
                std::function<void()> changed);
    ~AsciiEditor() override;

    bool ready() const noexcept { return ready_; }
    bool previewCurrent() const noexcept { return ready_ && previewRevision_ == revision_; }
    bool modified() const;
    std::string text() const;
    const std::shared_ptr<const ModelDocument>& source() const { return source_; }
    void updatePreview();
    void setPreviewEnabled(bool enabled) { previewEnabled_ = enabled; }
    void setAppearance(bool dark);
    void cancelAndWait();
    bool save(bool choosePath = false);
    void editNode(const model::Model& model, std::size_t nodeIndex);
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

    std::shared_ptr<const ModelDocument> source_;
    std::function<void(std::unique_ptr<ModelDocument>)> apply_;
    std::function<void()> changed_;
    wxStyledTextCtrl* editor_{};
    wxSearchCtrl* search_{};
    wxTextCtrl* status_{};
    wxButton* update_{};
    wxTimer timer_;
    std::future<Result> worker_;
    std::atomic<bool> cancelled_{false};
    std::size_t revision_{}, workerRevision_{}, previewRevision_{};
    std::chrono::steady_clock::time_point updateAt_{};
    std::chrono::steady_clock::time_point workStarted_{};
    long displayedSeconds_{-1};
    bool generating_{}, ready_{}, pending_{}, previewEnabled_{true};
    std::filesystem::path savePath_;
};

} // namespace neomdl::ui
