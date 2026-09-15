#include "PluginDockPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "PluginWebDialog.hpp"
#include "Widgets/WebView.hpp"
#include "Widgets/WebViewHostDialog.hpp"

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>

#include <algorithm>
#include <utility>

namespace Slic3r { namespace GUI {

std::string plugin_pane_name(const std::string& plugin_key, const std::string& title)
{
    std::string name = "plugin:" + plugin_key + ":" + title;
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '|' || c == ';' || c == '=' || c == '\\'; }, '_');
    return name;
}

std::string plugin_pane_layout_entry(const std::string& layout, const std::string& pane_name)
{
    // Panes are separated by '|'; SavePerspective() escapes a '|' inside a caption as "\|".
    const std::string prefix = "name=" + pane_name + ";";
    size_t            begin  = 0;
    for (size_t i = 0; i <= layout.size(); ++i) {
        if (i < layout.size() && (layout[i] != '|' || (i > 0 && layout[i - 1] == '\\')))
            continue;
        if (layout.compare(begin, prefix.size(), prefix) == 0)
            return layout.substr(begin, i - begin);
        begin = i + 1;
    }
    return {};
}

PluginDockPanel::PluginDockPanel(wxWindow*          parent,
                                 const std::string& html,
                                 MessageHandler     on_message,
                                 CloseHandler       on_close,
                                 CloseHandler       on_destroyed)
    : wxPanel(parent, wxID_ANY)
    , m_html(html)
    , m_on_message(std::move(on_message))
    , m_on_close(std::move(on_close))
    , m_on_destroyed(std::move(on_destroyed))
{
    SetBackgroundColour(wxGetApp().get_window_default_clr());
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(sizer);

    const std::string bootstrap = (boost::filesystem::path(resources_dir()) / PluginWebDialog::BOOTSTRAP_PAGE).make_preferred().string();
    m_browser = WebView::CreateWebView(this, wxString("file://") + from_u8(bootstrap));
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Could not create the web view for a plugin dock panel";
        return;
    }

    m_browser->SetBackgroundColour(GetBackgroundColour());
    m_browser->AddUserScript(wxString::FromUTF8(WebViewHostDialog::theme_user_script()));
    m_browser->AddUserScript(wxString::FromUTF8(WebViewHostDialog::plugin_defaults_user_script()));
    m_browser->AddUserScript(PluginWebDialog::bridge_user_script());
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PluginDockPanel::on_bootstrap_event, this);
    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PluginDockPanel::on_bootstrap_event, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& event) { event.Veto(); });
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PluginDockPanel::on_script_message, this);
    m_browser->Bind(EVT_WEBVIEW_RECREATED, &PluginDockPanel::on_webview_recreated, this);
    sizer->Add(m_browser, 1, wxEXPAND);
}

PluginDockPanel::~PluginDockPanel()
{
    if (m_on_destroyed)
        m_on_destroyed();
}

void PluginDockPanel::on_bootstrap_event(wxWebViewEvent& event)
{
    if (!m_content_loaded) {
        m_content_loaded = true;
        m_browser->SetPage(wxString::FromUTF8(m_html), PluginWebDialog::content_base_url());
    }
    event.Skip();
}

void PluginDockPanel::on_script_message(wxWebViewEvent& event)
{
    const nlohmann::json payload = nlohmann::json::parse(event.GetString().utf8_string(), nullptr, false);
    if (!payload.is_object() || payload.value("channel", std::string()) != "orca")
        return;

    const std::string kind = payload.value("kind", std::string());
    if (kind == "message") {
        if (m_on_message)
            m_on_message(payload.contains("data") ? payload["data"] : nlohmann::json());
    } else if (kind == "close") {
        request_close();
    }
}

void PluginDockPanel::on_webview_recreated(wxCommandEvent&)
{
    SetBackgroundColour(wxGetApp().get_window_default_clr());
    m_browser->SetBackgroundColour(GetBackgroundColour());
    Refresh();
    // Handled without Skip(), so WebView::RecreateAll() does not reload the plugin page.
    WebView::RunScript(m_browser, wxString::FromUTF8(WebViewHostDialog::theme_apply_script()));
}

void PluginDockPanel::push_message(const nlohmann::json& data)
{
    if (m_browser == nullptr || m_closing)
        return;

    nlohmann::json envelope;
    envelope["data"] = data;
    // The page may still be loading, so wait briefly for the bridge to define __orcaDispatch.
    WebView::RunScript(m_browser, wxString::Format(
        "(function dispatch(payload, attempts) {\n"
        "  if (typeof window.__orcaDispatch === 'function') { window.__orcaDispatch(payload); return; }\n"
        "  if (attempts < 100) window.setTimeout(function() { dispatch(payload, attempts + 1); }, 25);\n"
        "})(%s, 0);",
        wxString::FromUTF8(envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))));
}

void PluginDockPanel::fire_close()
{
    if (m_closing)
        return;
    m_closing = true;
    if (m_on_close) {
        CloseHandler on_close = std::move(m_on_close);
        m_on_close            = nullptr;
        on_close();
    }
}

void PluginDockPanel::request_close()
{
    if (m_closing)
        return;
    fire_close();
    // A close requested by the page arrives inside the web view's own script-message callback,
    // which must return before the web view is destroyed.
    CallAfter([this]() { remove_pane(); });
}

void PluginDockPanel::destroy_for_plugin()
{
    m_closing  = true;
    m_on_close = nullptr;
    remove_pane();
}

void PluginDockPanel::remove_pane()
{
    if (Plater* plater = wxGetApp().plater())
        plater->remove_plugin_pane(this);
    else
        Destroy();
}

}} // namespace Slic3r::GUI
