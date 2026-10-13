#include "wx/AsciiEditor.hpp"

#include <model/AsciiExport.hpp>
#include <neoshared/PathUtf8.hpp>
#include <neoshared/texture/FileUtil.hpp>
#include "NeoWxUi.hpp"

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/stc/stc.h>

#include <algorithm>

namespace neomdl::ui {

AsciiEditor::AsciiEditor(wxWindow* parent, std::shared_ptr<const ModelDocument> source,
    std::function<void(std::unique_ptr<ModelDocument>)> apply, std::function<void()> changed)
    : wxPanel(parent), source_(std::move(source)), apply_(std::move(apply)), changed_(std::move(changed)) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    search_ = new wxSearchCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    search_->SetDescriptiveText("Find in ASCII (Ctrl+F)");
    update_ = new wxButton(this, wxID_ANY, "Update Preview");
    update_->SetToolTip("Update now (Ctrl+Enter). Edits update automatically after a short typing pause.");
    update_->Enable(false);
    toolbar->Add(search_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    toolbar->Add(update_, 0);
    layout->Add(toolbar, 0, wxEXPAND | wxALL, FromDIP(5));
    editor_ = new wxStyledTextCtrl(this);
    editor_->SetCodePage(wxSTC_CP_UTF8);
    editor_->SetWrapMode(wxSTC_WRAP_NONE);
    editor_->SetUseTabs(false);
    editor_->SetTabWidth(2);
    editor_->SetMarginType(0, wxSTC_MARGIN_NUMBER);
    editor_->SetMarginWidth(0, FromDIP(60));
    editor_->SetMarginWidth(1, 0);
    editor_->SetReadOnly(true);
    layout->Add(editor_, 1, wxEXPAND);
    status_ = new wxTextCtrl(this, wxID_ANY, "Generating ASCII; the viewport shows the original model.",
        wxDefaultPosition, wxSize(-1, FromDIP(52)), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    layout->Add(status_, 0, wxEXPAND | wxALL, FromDIP(5));
    SetSizer(layout);
    setAppearance(wxui::currentDarkMode());

    editor_->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT);
    editor_->Bind(wxEVT_STC_MODIFIED, [this](wxStyledTextEvent&) {
        ++revision_;
        cancelled_.store(true);
        pending_ = true;
        updateAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        status_->ChangeValue("Preview out of date — waiting for typing to pause.");
        changed_();
    });
    search_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { findNext(); });
    search_->Bind(wxEVT_SEARCHCTRL_SEARCH_BTN, [this](wxCommandEvent&) { findNext(); });
    update_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { updatePreview(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.ControlDown() && event.GetKeyCode() == 'F') {
            search_->SetFocus();
            search_->SelectAll();
        } else if (event.ControlDown() && event.GetKeyCode() == WXK_RETURN && ready_) {
            updatePreview();
        } else event.Skip();
    });
    timer_.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); }, timer_.GetId());
    timer_.Start(50);

    if (source_->originalAscii()) {
        installText(*source_->originalAscii());
        savePath_ = source_->path();
        status_->ChangeValue("Preview current. Edits update after a 0.5-second typing pause.");
    } else {
        generating_ = true;
        workStarted_ = std::chrono::steady_clock::now();
        worker_ = std::async(std::launch::async, [this] {
            neoshared::bif::JobControl job;
            job.cancelled = [this] { return cancelled_.load(); };
            const ResourceLoadScope scope(job);
            return Result{model::exportAsciiModel(source_->sourceModel(), job).text, {}};
        });
    }
}

AsciiEditor::~AsciiEditor() { cancelAndWait(); }

void AsciiEditor::cancelAndWait() {
    timer_.Stop();
    cancelled_.store(true);
    if (worker_.valid()) worker_.wait();
}

bool AsciiEditor::modified() const { return editor_->GetModify(); }

std::string AsciiEditor::text() const {
    const auto bytes = editor_->GetTextRaw();
    return std::string(bytes.data(), static_cast<std::size_t>(editor_->GetTextLength()));
}

void AsciiEditor::installText(const std::string& text) {
    editor_->SetModEventMask(0);
    editor_->SetReadOnly(false);
    editor_->SetTextRaw(text.c_str());
    editor_->EmptyUndoBuffer();
    editor_->SetSavePoint();
    editor_->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT);
    ready_ = true;
    update_->Enable(true);
}

void AsciiEditor::updatePreview() {
    cancelled_.store(true);
    pending_ = true;
    updateAt_ = std::chrono::steady_clock::now();
    poll();
}

void AsciiEditor::poll() {
    // Modal dialogs retain the document they opened against until they close.
    if (!previewEnabled_ || !IsEnabled()) return;
    if (worker_.valid()) {
        if (worker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            if (generating_) {
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - workStarted_).count();
                if (seconds != displayedSeconds_) {
                    displayedSeconds_ = static_cast<long>(seconds);
                    status_->ChangeValue(wxString::Format(
                        "Generating ASCII — %ld s. You can keep using the viewport.", displayedSeconds_));
                }
            }
            return;
        }
        try {
            auto result = worker_.get();
            if (generating_) {
                installText(result.text);
                status_->ChangeValue("ASCII ready. The viewport shows the original binary until you edit or Update Preview.");
                changed_();
            } else if (workerRevision_ == revision_ && !cancelled_.load()) {
                previewRevision_ = workerRevision_;
                apply_(std::move(result.document));
                status_->ChangeValue("Preview current. Edits update after a 0.5-second typing pause.");
                changed_();
            }
        } catch (const neoshared::bif::JobCancelled&) {
            // A newer edit replaces this work; its typing deadline remains queued.
        } catch (const std::exception& error) {
            if (generating_ || workerRevision_ == revision_) {
                status_->ChangeValue(wxui::toWx(std::string(generating_ ? "ASCII generation failed: " :
                    "Preview out of date — ASCII error: ") + error.what()));
                changed_();
            }
        }
        generating_ = false;
    }
    if (pending_ && std::chrono::steady_clock::now() >= updateAt_) {
        pending_ = false;
        cancelled_.store(false);
        workerRevision_ = revision_;
        status_->ChangeValue("Updating preview from ASCII...");
        worker_ = std::async(std::launch::async, [this, buffer = text()] {
            neoshared::bif::JobControl job;
            job.cancelled = [this] { return cancelled_.load(); };
            const ResourceLoadScope scope(job);
            return Result{{}, source_->rebuildAscii(buffer, job)};
        });
    }
}

void AsciiEditor::findNext() {
    editor_->GotoPos(editor_->GetSelectionEnd());
    editor_->SearchAnchor();
    if (editor_->SearchNext(0, search_->GetValue()) == wxSTC_INVALID_POSITION) {
        editor_->GotoPos(0);
        editor_->SearchAnchor();
        editor_->SearchNext(0, search_->GetValue());
    }
    editor_->ScrollRange(editor_->GetSelectionStart(), editor_->GetSelectionEnd());
}

void AsciiEditor::replaceText(const std::string& text) {
    editor_->BeginUndoAction();
    editor_->SetTargetStart(0);
    editor_->SetTargetEnd(editor_->GetTextLength());
    editor_->ReplaceTargetRaw(text.data(), static_cast<int>(text.size()));
    editor_->EndUndoAction();
    updatePreview();
}

void AsciiEditor::setAppearance(bool dark) {
    const auto colors = wxui::themePalette(dark);
    editor_->StyleSetFont(wxSTC_STYLE_DEFAULT, wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
    editor_->StyleSetForeground(wxSTC_STYLE_DEFAULT, colors.text);
    editor_->StyleSetBackground(wxSTC_STYLE_DEFAULT, colors.field);
    editor_->StyleClearAll();
    editor_->StyleSetForeground(wxSTC_STYLE_LINENUMBER, colors.mutedText);
    editor_->StyleSetBackground(wxSTC_STYLE_LINENUMBER, colors.panel);
    editor_->SetCaretForeground(colors.text);
    editor_->SetSelBackground(true, colors.selection);
    editor_->SetSelForeground(true, colors.selectionText);
}

bool AsciiEditor::save(bool choosePath) {
    auto path = savePath_;
    if (choosePath || path.empty()) {
        const auto name = path.empty() ? source_->sourceModel().name + "-ascii.mdl"
                                       : neoshared::pathToUtf8(path.filename());
        wxFileDialog dialog(this, "Save ASCII MDL", neosettings::pathToWx(source_->path().parent_path()),
            wxui::toWx(name), "ASCII models (*.mdl)|*.mdl", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) return false;
        path = neosettings::pathFromWx(dialog.GetPath());
    }
    try {
        auto protectedInputs = source_->openPaths();
        if (source_->originalAscii()) {
            protectedInputs.erase(std::remove_if(protectedInputs.begin(), protectedInputs.end(), [&](const auto& input) {
                return neoshared::sameResourcePath(input, source_->path());
            }), protectedInputs.end());
        }
        neoshared::checkResourceOutput(path, protectedInputs);
        const auto buffer = text();
        neoshared::texture::writeFileBytes(path, std::vector<std::uint8_t>(buffer.begin(), buffer.end()));
        savePath_ = path;
        editor_->SetSavePoint();
        changed_();
        return true;
    } catch (const std::exception& error) {
        wxui::showError(this, error);
        return false;
    }
}

} // namespace neomdl::ui
