#pragma once

#include "NeoModulePanel.hpp"
#include "core/ModelDocument.hpp"

#include <neoshared/ResourceDocument.hpp>

namespace neomdl::ui {

inline constexpr unsigned kEditorApiVersion = 1;

class ModelEditorPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual bool openFile(const std::filesystem::path& path) = 0;
    virtual bool openResource(neoshared::ResourceDocument resource) = 0;
    virtual bool activateResource(const std::string& identity) = 0;
    virtual std::size_t documentCount() const = 0;
};

ModelEditorPanel* createModelEditorPanel(wxWindow* parent, neomodules::Context context = {});

} // namespace neomdl::ui
