#pragma once

#include <nlohmann/json.hpp>

#include <wx/panel.h>
#include <wx/webview.h>

#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

// Name of a plugin's pane in the Plater's dock manager. It stays the same across sessions, so the
// saved window layout can put the pane back, and it never contains a wxAuiManager layout delimiter.
std::string plugin_pane_name(const std::string& plugin_key, const std::string& title);

// The pane part saved for `pane_name` in a wxAuiManager layout string, in the form
// wxAuiManager::LoadPaneInfo() takes, or empty when the layout has no such pane.
std::string plugin_pane_layout_entry(const std::string& layout, const std::string& pane_name);

// Plugin-supplied HTML hosted in a pane of the Plater's dock manager, bridged to the plugin through
// the same window.orca API as PluginWebDialog. Python-agnostic for the same reason: it can be
// destroyed on the main thread without the GIL, so its hooks must not capture pybind11 objects.
class PluginDockPanel : public wxPanel
{
public:
    using MessageHandler = std::function<void(const nlohmann::json& data)>;
    using CloseHandler   = std::function<void()>;

    // on_close fires once, on a user or page initiated close. on_destroyed runs from the destructor
    // on every path and must touch host-side state only.
    PluginDockPanel(wxWindow*          parent,
                    const std::string& html,
                    MessageHandler     on_message,
                    CloseHandler       on_close,
                    CloseHandler       on_destroyed);
    ~PluginDockPanel() override;

    // Main thread only.
    void push_message(const nlohmann::json& data);
    // Fires on_close, then removes the pane.
    void request_close();
    // Removes the pane without firing on_close, for plugin unload.
    void destroy_for_plugin();
    // Fires on_close at most once. The Plater calls it when the pane's own close button is used.
    void fire_close();

private:
    void on_bootstrap_event(wxWebViewEvent& event);
    void on_script_message(wxWebViewEvent& event);
    void on_webview_recreated(wxCommandEvent& event);
    void remove_pane();

    wxWebView*     m_browser{nullptr};
    std::string    m_html;
    bool           m_content_loaded{false};
    bool           m_closing{false};
    MessageHandler m_on_message;
    CloseHandler   m_on_close;
    CloseHandler   m_on_destroyed;
};

}} // namespace Slic3r::GUI
