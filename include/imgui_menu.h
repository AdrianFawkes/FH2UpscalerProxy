#pragma once
// ============================================================================
// imgui_menu.h — In-game Dear ImGui overlay menu for DLSS / DLAA control
// ============================================================================
#include "dlss_manager.h"

class ImGuiMenu {
public:
    static ImGuiMenu& Get();

    void Draw();
    void ToggleVisibility();
    bool IsVisible() const { return m_visible; }
    void SetVisible(bool v) { m_visible = v; }

private:
    ImGuiMenu();
    bool m_visible;
};
