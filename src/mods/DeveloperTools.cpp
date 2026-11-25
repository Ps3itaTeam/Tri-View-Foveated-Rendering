#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the developer tools: debug build only (Mods.cpp makes them only there)
#include <imgui.h>

#include "tools/GameObjectsDisplay.hpp"
#include "tools/ChainViewer.hpp"
#include "tools/ObjectExplorer.hpp"

#include "DeveloperTools.hpp"

DeveloperTools::DeveloperTools() {
    m_tools.emplace_back(std::make_shared<ChainViewer>());
    m_tools.emplace_back(std::make_shared<GameObjectsDisplay>());
    #ifndef _DEBUG
    // std::structs are not same as Release, this made crash
    m_tools.emplace_back(ObjectExplorer::get());
    #endif
}

void DeveloperTools::on_draw_ui() {
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    for (auto& tool : m_tools) {
        ImGui::PushID(tool->get_name().data());
        tool->on_draw_dev_ui();
        ImGui::PopID();
    }
}

#endif // !REVR_RELEASE
