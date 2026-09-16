#include "CrealityCFS.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/Utils.hpp"
#include "Http.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

namespace beast     = boost::beast;
namespace bhttp     = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;
using json          = nlohmann::json;

namespace Slic3r {

// Fixed by the firmware.
static const char* CFS_LAN_PORT      = "9999";
static const int   CFS_UPLOAD_PORT   = 80;
static const int   CFS_MOONRAKER_PORT = 7125;
static const int   CFS_FLUIDD_PORT   = 4408;

static const int   SLOTS_PER_UNIT = 4;
static const int   MAX_UNITS      = 4;

// How hard to try to get the colorMatch table installed. The write lands in the
// printer's map table in well under a second when it is going to land at all
// (measured against 192.168.1.50: box.map changed 0.4 s after the write), so
// the read-back polls quickly and only re-writes if a whole round of polls came
// back wrong. Worst case 3 * 6 * 400 ms = 7.2 s.
static const int COLORMATCH_WRITE_ATTEMPTS = 3;
static const int COLORMATCH_READBACK_POLLS = 6;
static const int COLORMATCH_POLL_MS        = 400;

// -- CfsSlot / CfsSlotTable ----------------------------------------------

std::string CfsSlot::label() const
{
    if (is_external())
        return "Ext";
    return std::to_string(unit) + std::string(1, char('A' + slot));
}

std::string CfsSlot::tnn() const { return "T" + label(); }

const CfsSlot* CfsSlotTable::get(int unit_id, int slot_id) const
{
    for (const auto& s : slots)
        if (s.unit == unit_id && s.slot == slot_id)
            return &s;
    return nullptr;
}

bool CfsSlotTable::map_is_identity() const
{
    for (const auto& kv : gcode_map)
        if (kv.first != kv.second)
            return false;
    return true;
}

// -- the LAN socket ------------------------------------------------------

namespace {

// A single short lived connection to ws://<ip>:9999. The printer never
// correlates a reply to a request, so a read waits for the first frame that
// carries the key it is after.
class LanSocket
{
public:
    LanSocket(const std::string& host, int connect_timeout_s = 5, int read_timeout_ms = 3000)
        : m_ws(m_ioc)
    {
        tcp::resolver resolver{m_ioc};
        beast::get_lowest_layer(m_ws).expires_after(std::chrono::seconds(connect_timeout_s));
        auto const results = resolver.resolve(host, CFS_LAN_PORT);
        beast::get_lowest_layer(m_ws).connect(results);

        std::string host_header = host + ':' +
            std::to_string(beast::get_lowest_layer(m_ws).socket().remote_endpoint().port());

        m_ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(bhttp::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " OrcaSlicer-CFS");
        }));
        m_ws.handshake(host_header, "/");

        // The connect deadline must not stay armed over the reads that follow.
        beast::get_lowest_layer(m_ws).expires_never();

#ifdef _WIN32
        DWORD recv_timeout = static_cast<DWORD>(read_timeout_ms);
#else
        struct timeval recv_timeout = {read_timeout_ms / 1000, (read_timeout_ms % 1000) * 1000};
#endif
        setsockopt(beast::get_lowest_layer(m_ws).socket().native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));
    }

    ~LanSocket()
    {
        beast::error_code ec;
        m_ws.close(websocket::close_code::normal, ec);
    }

    void send(const json& cmd) { m_ws.write(net::buffer(cmd.dump())); }

    // Read frames until one carries `key`, merging every frame into m_state so
    // the opening full state dump is not thrown away.
    bool read_until(const std::string& key, json& out, int max_reads = 40)
    {
        for (int i = 0; i < max_reads; i++) {
            beast::flat_buffer buf;
            beast::error_code  ec;
            m_ws.read(buf, ec);
            if (ec)
                break;
            std::string raw = beast::buffers_to_string(buf.data());
            if (raw == "ok")
                continue;
            json frame;
            try {
                frame = json::parse(raw);
            } catch (const std::exception&) {
                continue;
            }
            if (!frame.is_object())
                continue;
            for (auto it = frame.begin(); it != frame.end(); ++it)
                m_state[it.key()] = it.value();
            if (frame.contains(key)) {
                out = frame[key];
                return true;
            }
        }
        return false;
    }

    // Drain whatever the printer has already pushed, without asking for anything.
    void drain(int max_reads = 8)
    {
        json ignored;
        read_until("\x01never\x01", ignored, max_reads);
    }

    const json& state() const { return m_state; }

private:
    net::io_context                      m_ioc;
    websocket::stream<beast::tcp_stream> m_ws;
    json                                 m_state = json::object();
};

std::string json_str(const json& obj, const char* key, const std::string& fallback = std::string())
{
    if (!obj.is_object() || !obj.contains(key))
        return fallback;
    const json& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    return fallback;
}

// Fetch Moonraker's `box` object, unwrapped down to the object itself.
// This is the only place the live G-code-slot map can be read from on an idle
// printer: see also the note in CrealityCFS::verify_color_match.
bool fetch_box_object(const std::string& host, json& box, std::string& error)
{
    std::string body;
    bool        got = false;
    std::string transport_error;

    auto http = Http::get("http://" + host + ":" + std::to_string(CFS_MOONRAKER_PORT) + "/printer/objects/query?box");
    http.timeout_max(5)
        .on_complete([&](std::string b, unsigned) {
            body = std::move(b);
            got  = true;
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            transport_error = err + " (HTTP " + std::to_string(status) + ")";
        })
        .perform_sync();

    if (!got) {
        error = "Moonraker on port 7125 did not answer: " + transport_error;
        return false;
    }
    try {
        json parsed = json::parse(body);
        box         = parsed;
        if (box.is_object() && box.contains("result"))
            box = box["result"];
        if (box.is_object() && box.contains("status"))
            box = box["status"];
        if (box.is_object() && box.contains("box"))
            box = box["box"];
    } catch (const std::exception& e) {
        error = std::string("the Moonraker box object could not be parsed: ") + e.what();
        return false;
    }
    if (!box.is_object()) {
        error = "the printer has no Moonraker box object";
        return false;
    }
    return true;
}

int json_int(const json& obj, const char* key, int fallback = 0)
{
    if (!obj.is_object() || !obj.contains(key))
        return fallback;
    const json& v = obj[key];
    if (v.is_number())
        return static_cast<int>(v.get<double>());
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

} // namespace

// -- helpers -------------------------------------------------------------

std::string CrealityCFS::index_to_tnn(int index)
{
    if (index < 0)
        index = 0;
    if (index >= MAX_UNITS * SLOTS_PER_UNIT)
        index = MAX_UNITS * SLOTS_PER_UNIT - 1;
    const int unit = index / SLOTS_PER_UNIT + 1;
    const int slot = index % SLOTS_PER_UNIT;
    return "T" + std::to_string(unit) + std::string(1, char('A' + slot));
}

std::string CrealityCFS::normalise_colour(const std::string& value)
{
    std::string text = value;
    text.erase(0, text.find_first_not_of(" \t\r\n"));
    text.erase(text.find_last_not_of(" \t\r\n") + 1);
    if (text.empty())
        return {};
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
    if (lowered == "unknown" || lowered == "-1" || lowered == "none")
        return {};
    if (!text.empty() && text[0] == '#')
        text = text.substr(1);
    // The client inserts an extra leading 0 when it writes a colour, so the
    // printer reports seven digits. Drop it.
    if (text.size() == 7)
        text = text.substr(1);
    if (text.size() == 8)
        text = text.substr(0, 6); // RRGGBBAA
    if (text.size() != 6)
        return {};
    for (char c : text)
        if (!isxdigit(static_cast<unsigned char>(c)))
            return {};
    std::transform(text.begin(), text.end(), text.begin(), ::toupper);
    return "#" + text;
}

double CrealityCFS::colour_distance(const std::string& a, const std::string& b)
{
    const std::string ca = normalise_colour(a);
    const std::string cb = normalise_colour(b);
    if (ca.empty() || cb.empty())
        return 1e6;
    double sum = 0.0;
    for (int i = 1; i < 7; i += 2) {
        const int pa = std::stoi(ca.substr(i, 2), nullptr, 16);
        const int pb = std::stoi(cb.substr(i, 2), nullptr, 16);
        sum += double(pa - pb) * double(pa - pb);
    }
    return sum;
}

// -- construction --------------------------------------------------------

std::string CrealityCFS::model_name_for(const std::string& model_id)
{
    // The K2 platform boards, which are the ones that carry a CFS.
    static const std::map<std::string, std::string> names = {
        {"F008", "K2 Plus"},
        {"F012", "K2 Pro"},
        {"F021", "K2"},
        {"F022", "SPARKX i7"},
    };
    auto it = names.find(model_id);
    if (it != names.end())
        return it->second;
    return model_id;
}

CrealityCFS::CrealityCFS(DynamicPrintConfig* config)
{
    // Ports 80, 9999, 7125 and 4408 are all fixed by the firmware, so only the
    // bare address matters. Strip any scheme and port the user typed, which also
    // means an address copied from a Moonraker or Fluidd setup still works.
    m_cfs_host = Http::get_host_from_url(config->opt_string("print_host"));
}

const char* CrealityCFS::get_name() const { return "Creality CFS"; }

PrintHostPostUploadActions CrealityCFS::get_post_upload_actions() const
{
    return PrintHostPostUploadAction::StartPrint;
}

// -- the Device tab camera page ------------------------------------------
//
// The K2's camera is WebRTC only, and the signalling is Creality's own: a POST
// to http://<ip>:8000/call/webrtc_local whose body is base64 of
// {"type":"offer","sdp":...} with Content-Type "plain/text", answered with
// base64 of {"type":"answer","sdp":...}.
//
// Fluidd's webcam card cannot drive that. Its "webrtc-camerastreamer" player is
// ayufan's flavour, which POSTs raw JSON, and the K2 answers a raw-JSON or a
// raw-SDP POST with the two byte body "{}" and no answer SDP. Verified live
// against 192.168.1.50 on 2026-09-13, all three body shapes against the same
// endpoint. So the Device tab gets its own page.

namespace {

void replace_all_in(std::string& text, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}

// The address this machine reaches the printer from. Chromium hands out mDNS
// ".local" host candidates, which the printer's libpeer stack cannot resolve,
// so the page rewrites them to this. CrealityPrint does the same thing in
// DeviceMgrRoutes.cpp:149-210, for the same reason.
std::string local_address_towards(const std::string& host)
{
    try {
        net::io_context             ioc;
        net::ip::udp::resolver      resolver(ioc);
        net::ip::udp::resolver::results_type endpoints = resolver.resolve(net::ip::udp::v4(), host, CFS_LAN_PORT);
        if (endpoints.empty())
            return {};
        net::ip::udp::socket sock(ioc);
        sock.connect(*endpoints.begin());
        return sock.local_endpoint().address().to_string();
    } catch (const std::exception&) {
        return {};
    }
}

// Everything that is not a letter or a digit becomes an underscore, so a host
// name or an IP turns into a usable file name.
std::string host_to_filename(const std::string& host)
{
    std::string out;
    for (char c : host)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out;
}

const char* CFS_CAMERA_PAGE = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__MODEL__ camera</title>
<style>
  :root { color-scheme: dark; }
  html, body { margin: 0; height: 100%; background: #141414; color: #e8e8e8;
               font: 13px/1.45 "Segoe UI", system-ui, sans-serif; }
  body { display: flex; flex-direction: column; }
  #bar { display: flex; align-items: center; gap: 10px; padding: 6px 10px;
         background: #1e1e1e; border-bottom: 1px solid #2d2d2d; flex: 0 0 auto; }
  #status { flex: 1 1 auto; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  #status.live { color: #6ec96e; }
  #status.bad  { color: #e0a24a; }
  button { background: #2d2d2d; color: #e8e8e8; border: 1px solid #3d3d3d;
           border-radius: 3px; padding: 4px 10px; cursor: pointer; }
  button:hover { background: #383838; }
  #stage { flex: 0 0 auto; background: #000; display: flex; justify-content: center; }
  video { width: 100%; max-height: 62vh; display: block; background: #000; }
  #frame { flex: 1 1 auto; border: 0; width: 100%; background: #141414; }
  #frame.hidden { display: none; }
</style>
</head>
<body>
<div id="bar">
  <span id="status">Starting.</span>
  <button id="retry" type="button">Reconnect camera</button>
  <button id="toggle" type="button">Hide Fluidd</button>
</div>
<div id="stage"><video id="video" autoplay muted playsinline></video></div>
<iframe id="frame" src="__FLUIDD__" referrerpolicy="no-referrer"></iframe>
<script>
(function () {
  "use strict";
  var IP       = "__IP__";
  var LOCAL_IP = "__LOCAL_IP__";
  var SIGNAL   = "http://" + IP + ":8000/call/webrtc_local";

  var video  = document.getElementById("video");
  var status = document.getElementById("status");
  var pc = null, retryTimer = null, frameTimer = null, generation = 0;

  function say(text, kind) {
    status.textContent = text;
    status.className = kind || "";
  }

  // Hand the device a single H264 codec, the way CrealityPrint's fr() does.
  // The device echoes the offered payload type straight back into its answer,
  // and an offer with no valid H264 payload type in 96-127 makes it emit
  // garbage, so the offer must carry exactly one H264 payload type in that
  // range and nothing else.
  function singleH264(sdp) {
    var lines = sdp.split(/\r\n|\n/), keep = -1, i, m;
    for (i = 0; i < lines.length; i++) {
      m = /^a=rtpmap:(\d+) H264\/90000/i.exec(lines[i]);
      if (m) {
        var pt = parseInt(m[1], 10);
        if (pt >= 96 && pt <= 127 && (keep < 0 || pt < keep)) keep = pt;
      }
    }
    if (keep < 0) return sdp;
    var out = [], inVideo = false;
    for (i = 0; i < lines.length; i++) {
      var line = lines[i];
      if (line.indexOf("m=") === 0) {
        inVideo = line.indexOf("m=video") === 0;
        if (inVideo) {
          var parts = line.split(" ");
          out.push(parts.slice(0, 3).join(" ") + " " + keep);
          continue;
        }
      }
      if (inVideo) {
        m = /^a=(?:rtpmap|fmtp|rtcp-fb):(\d+)/.exec(line);
        if (m && parseInt(m[1], 10) !== keep) continue;
      }
      out.push(line);
    }
    return out.join("\r\n");
  }

  // Chromium hides the real LAN address behind an mDNS ".local" candidate and
  // the printer cannot resolve it, so put the real address back.
  function realCandidates(sdp) {
    if (!LOCAL_IP) return sdp;
    return sdp.split(/\r\n|\n/).map(function (line) {
      if (line.indexOf("a=candidate:") !== 0) return line;
      var p = line.split(" ");
      if (p.length > 4 && /\.local$/i.test(p[4])) p[4] = LOCAL_IP;
      return p.join(" ");
    }).join("\r\n");
  }

  // Non-trickle: the offer is only sent once gathering is done, which is what
  // the device expects; it ignores candidates that arrive after the offer.
  function iceComplete(peer) {
    if (peer.iceGatheringState === "complete") return Promise.resolve();
    return new Promise(function (resolve) {
      var timer = setTimeout(finish, 3000);
      function finish() {
        clearTimeout(timer);
        peer.removeEventListener("icegatheringstatechange", onChange);
        resolve();
      }
      function onChange() { if (peer.iceGatheringState === "complete") finish(); }
      peer.addEventListener("icegatheringstatechange", onChange);
    });
  }

  function teardown() {
    generation++;
    if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
    if (frameTimer) { clearTimeout(frameTimer); frameTimer = null; }
    if (pc) { try { pc.close(); } catch (e) {} pc = null; }
    // Deliberately not clearing video.srcObject: setting it to null restarts the
    // media element's load algorithm against the document URL, which on a file:
    // page logs "unsafe attempt to load URL". The last frame simply stays on
    // screen until the next stream arrives, which also looks better.
  }

  function later(seconds) {
    if (retryTimer) clearTimeout(retryTimer);
    retryTimer = setTimeout(start, seconds * 1000);
  }

  function start() {
    teardown();
    var mine = generation;
    say("Connecting to the camera at " + IP + ".");
    pc = new RTCPeerConnection({ iceServers: [] });
    // sendrecv, not recvonly: with recvonly this hardware completes signalling
    // and then never sends a frame.
    pc.addTransceiver("video", { direction: "sendrecv" });
    pc.ontrack = function (event) {
      video.srcObject = event.streams[0];
      video.play().catch(function () {});
    };
    pc.oniceconnectionstatechange = function () {
      if (!pc || mine !== generation) return;
      var state = pc.iceConnectionState;
      if (state === "connected" || state === "completed") say("Live.", "live");
      else if (state === "failed" || state === "disconnected" || state === "closed") {
        say("The camera stream dropped. Reconnecting.", "bad");
        later(5);
      }
    };

    pc.createOffer().then(function (offer) {
      return pc.setLocalDescription(offer);
    }).then(function () {
      return iceComplete(pc);
    }).then(function () {
      var sdp = realCandidates(singleH264(pc.localDescription.sdp));
      return fetch(SIGNAL, {
        method: "POST",
        headers: { "Content-Type": "plain/text" },
        body: btoa(JSON.stringify({ type: "offer", sdp: sdp }))
      });
    }).then(function (response) {
      return response.text();
    }).then(function (text) {
      if (mine !== generation) return;
      var answer;
      try {
        answer = JSON.parse(atob(text.trim()));
      } catch (e) {
        say("The camera did not return an answer (" + text.slice(0, 40) + "). Retrying.", "bad");
        later(5);
        return;
      }
      if (!answer || !answer.sdp) {
        say("The camera returned an empty answer. Retrying.", "bad");
        later(5);
        return;
      }
      return pc.setRemoteDescription(new RTCSessionDescription({ type: "answer", sdp: answer.sdp }))
        .then(function () {
          say("Negotiated. Waiting for the first frame.");
          frameTimer = setTimeout(function () {
            if (mine === generation && !video.videoWidth)
              say("Signalling worked but no video arrived. Reconnecting.", "bad");
            if (mine === generation && !video.videoWidth) later(2);
          }, 12000);
        });
    }).catch(function (error) {
      if (mine !== generation) return;
      say("Could not reach the camera: " + error + ". Retrying.", "bad");
      later(5);
    });
  }

  video.addEventListener("resize", function () {
    if (video.videoWidth) say("Live, " + video.videoWidth + " x " + video.videoHeight + ".", "live");
  });
  document.getElementById("retry").addEventListener("click", start);
  document.getElementById("toggle").addEventListener("click", function () {
    var frame = document.getElementById("frame");
    var hidden = frame.classList.toggle("hidden");
    this.textContent = hidden ? "Show Fluidd" : "Hide Fluidd";
  });
  window.addEventListener("pagehide", teardown);
  start();
})();
</script>
</body>
</html>
)PAGE";

} // namespace

std::string CrealityCFS::write_camera_page(const std::string& host)
{
    if (host.empty())
        return {};

    std::string page = CFS_CAMERA_PAGE;
    replace_all_in(page, "__IP__", host);
    replace_all_in(page, "__LOCAL_IP__", local_address_towards(host));
    replace_all_in(page, "__MODEL__", "Creality CFS");
    replace_all_in(page, "__FLUIDD__", "http://" + host + ":" + std::to_string(CFS_FLUIDD_PORT));

    try {
        boost::filesystem::path path = boost::filesystem::path(data_dir()) / ("cfs_camera_" + host_to_filename(host) + ".html");
        boost::nowide::ofstream out(path.string().c_str(), std::ios::binary | std::ios::trunc);
        if (!out.good())
            return {};
        out << page;
        out.close();
        return path.string();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityCFS: could not write the camera page: " << e.what();
        return {};
    }
}

std::string CrealityCFS::get_print_host_webui(DynamicPrintConfig* config)
{
    if (config == nullptr)
        return {};
    const std::string explicit_webui = config->opt_string("print_host_webui");
    if (!explicit_webui.empty())
        return explicit_webui;
    const std::string host = Http::get_host_from_url(config->opt_string("print_host"));
    if (host.empty())
        return {};

    // With Device UI left empty, the Device tab gets the camera page, which
    // carries Fluidd in an iframe underneath the video. A user who wants only
    // Fluidd types http://<ip>:4408 into Device UI and that wins.
    const std::string path = write_camera_page(host);
    if (!path.empty()) {
        std::string url = path;
        std::replace(url.begin(), url.end(), '\\', '/');
        if (!url.empty() && url[0] != '/')
            url = "/" + url; // C:/Users/... -> /C:/Users/...
        replace_all_in(url, " ", "%20");
        return "file://" + url;
    }

    // Fluidd, which the K2 serves itself.
    return "http://" + host + ":" + std::to_string(CFS_FLUIDD_PORT);
}

// -- test ----------------------------------------------------------------

wxString CrealityCFS::get_test_ok_msg() const
{
    if (!m_test_summary.empty())
        return m_test_summary;
    return _(L("Connected to the Creality printer."));
}

wxString CrealityCFS::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to the Creality printer"), msg.Truncate(256));
}

bool CrealityCFS::test(wxString& curl_msg) const
{
    m_test_summary.clear();

    if (m_cfs_host.empty()) {
        curl_msg = _L("No printer address. Enter the printer's IP address, for example 192.168.1.50.");
        return false;
    }

    CfsSlotTable table;
    std::string  error;
    if (!query_slot_table(table, error)) {
        curl_msg = wxString::FromUTF8(error.c_str());
        return false;
    }

    int filled = 0;
    for (const auto& s : table.slots)
        if (!s.is_external() && s.present && !s.type.empty())
            ++filled;

    wxString name = table.model_name.empty() ? wxString("Creality printer") : wxString::FromUTF8(table.model_name.c_str());
    wxString host = table.hostname.empty() ? wxString::FromUTF8(m_cfs_host.c_str()) : wxString::FromUTF8(table.hostname.c_str());

    m_test_summary = wxString::Format(_L("Connected to %s (%s).\n%d CFS unit(s) on the bus, %d slot(s) loaded."), name,
                                      host, int(table.units_present.size()), filled);
    return true;
}

// -- reading the slot table ----------------------------------------------

bool CrealityCFS::query_slot_table(CfsSlotTable& table, std::string& error) const
{
    table = CfsSlotTable();

    json boxs_info;
    json opening_state = json::object();
    try {
        LanSocket sock(m_cfs_host);
        sock.send(json{{"method", "get"}, {"params", {{"boxsInfo", 1}}}});
        if (!sock.read_until("boxsInfo", boxs_info)) {
            error = "The printer did not answer a boxsInfo request on port 9999.";
            return false;
        }
        opening_state = sock.state();
    } catch (const std::exception& e) {
        error = std::string("Could not reach the printer on port 9999: ") + e.what();
        return false;
    }

    table.model_id   = json_str(opening_state, "model");
    table.hostname   = json_str(opening_state, "hostname");

    if (table.model_id.empty()) {
        // The opening state dump does not always carry the model. Port 80 answers
        // GET /info with {"mac","model","sn","version",...} and is cheap to ask.
        auto http = Http::get("http://" + m_cfs_host + "/info");
        http.timeout_max(5)
            .on_complete([&](std::string body, unsigned) {
                try {
                    table.model_id = json_str(json::parse(body), "model");
                } catch (const std::exception&) {}
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();
    }
    table.model_name = model_name_for(table.model_id);

    if (boxs_info.contains("materialBoxs") && boxs_info["materialBoxs"].is_array()) {
        for (const auto& box : boxs_info["materialBoxs"]) {
            const int box_id   = json_int(box, "id", -1);
            const int box_type = json_int(box, "type", 0);
            if (box_id < 0 || box_id > MAX_UNITS)
                continue;
            // id 0 is the external spool holder. It is kept in the table because the
            // user may legitimately print from it, but it is not a CFS unit.
            if (box_id >= 1 && json_int(box, "state", 0) == 1)
                table.units_present.insert(box_id);
            if (!box.contains("materials") || !box["materials"].is_array())
                continue;
            for (const auto& mat : box["materials"]) {
                const int mid = json_int(mat, "id", -1);
                if (mid < 0 || mid >= SLOTS_PER_UNIT)
                    continue;
                CfsSlot s;
                s.unit    = box_id;
                s.slot    = mid;
                s.type    = json_str(mat, "type");
                s.colour  = normalise_colour(json_str(mat, "color"));
                s.vendor  = json_str(mat, "vendor");
                s.name    = json_str(mat, "name");
                s.rfid    = json_str(mat, "rfid");
                s.percent = json_int(mat, "percent", 0);
                s.loaded  = json_int(mat, "selected", 0) != 0;
                s.present = json_int(mat, "state", 0) != 0;
                (void) box_type;
                table.slots.push_back(std::move(s));
            }
        }
    }

    // colorMatch names the physical slots serving the current job.
    if (boxs_info.contains("colorMatch") && boxs_info["colorMatch"].is_array()) {
        for (const auto& entry : boxs_info["colorMatch"]) {
            const int b = json_int(entry, "boxId", -1);
            const int m = json_int(entry, "materialId", -1);
            for (auto& s : table.slots)
                if (s.unit == b && s.slot == m)
                    s.mapped = true;
        }
    }

    // Moonraker is the only source of remaining length, and its material_type is
    // the raw RFID layer, so a disagreement marks a manual touchscreen edit.
    {
        json        box;
        std::string box_error;
        if (!fetch_box_object(m_cfs_host, box, box_error)) {
            BOOST_LOG_TRIVIAL(info) << "CrealityCFS: " << box_error << "; remaining length will not be shown";
        } else {
            try {
                if (box.contains("map") && box["map"].is_object())
                    for (auto it = box["map"].begin(); it != box["map"].end(); ++it)
                        if (it.value().is_string())
                            table.gcode_map[it.key()] = it.value().get<std::string>();

                for (int unit = 1; unit <= MAX_UNITS; ++unit) {
                    const std::string key = "T" + std::to_string(unit);
                    if (!box.contains(key) || !box[key].is_object())
                        continue;
                    const json& u       = box[key];
                    const json  remains = u.contains("remain_len") ? u["remain_len"] : json::array();
                    const json  types   = u.contains("material_type") ? u["material_type"] : json::array();
                    for (int mid = 0; mid < SLOTS_PER_UNIT; ++mid) {
                        CfsSlot* slot = nullptr;
                        for (auto& s : table.slots)
                            if (s.unit == unit && s.slot == mid)
                                slot = &s;
                        if (slot == nullptr)
                            continue;
                        if (remains.is_array() && mid < int(remains.size())) {
                            double value = -1.0;
                            const json& r = remains[mid];
                            if (r.is_number())
                                value = r.get<double>();
                            else if (r.is_string()) {
                                try {
                                    value = std::stod(r.get<std::string>());
                                } catch (...) {
                                    value = -1.0;
                                }
                            }
                            slot->remain_len = value >= 0.0 ? value : -1.0;
                        }
                        if (types.is_array() && mid < int(types.size())) {
                            std::string raw;
                            const json& t = types[mid];
                            if (t.is_string())
                                raw = t.get<std::string>();
                            else if (t.is_number())
                                raw = std::to_string(t.get<long long>());
                            if (raw == "unknown" && !slot->type.empty())
                                slot->edited = true;
                        }
                    }
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(info) << "CrealityCFS: could not parse the Moonraker box object: " << e.what();
            }
        }
    }

    std::sort(table.slots.begin(), table.slots.end(), [](const CfsSlot& a, const CfsSlot& b) {
        return a.unit != b.unit ? a.unit < b.unit : a.slot < b.slot;
    });
    return true;
}

// -- the G-code root -----------------------------------------------------

bool CrealityCFS::query_gcode_root(std::string& root, std::string& error) const
{
    // Ask, do not assume. The K2 family and the K1 family use different roots,
    // and the client itself derives this.
    root = "/mnt/UDISK/printer_data/gcodes";
    try {
        LanSocket sock(m_cfs_host);
        sock.send(json{{"method", "get"}, {"params", {{"reqGcodeFile", 1}}}});
        json files;
        if (sock.read_until("retGcodeFileInfo2", files) && files.is_array()) {
            for (const auto& entry : files) {
                const std::string path = json_str(entry, "path");
                const auto        cut  = path.rfind('/');
                if (cut != std::string::npos && cut > 0) {
                    root = path.substr(0, cut);
                    return true;
                }
            }
        }
    } catch (const std::exception& e) {
        error = std::string("could not ask the printer where G-code is stored: ") + e.what();
        // Not fatal: fall back to the documented K2 default.
    }
    return true;
}

// -- colorMatch verification ---------------------------------------------

bool CrealityCFS::read_gcode_map(std::map<std::string, std::string>& map_out, std::string& error) const
{
    map_out.clear();
    json box;
    if (!fetch_box_object(m_cfs_host, box, error))
        return false;
    if (!box.contains("map") || !box["map"].is_object()) {
        error = "the printer's box object carries no map table";
        return false;
    }
    for (auto it = box["map"].begin(); it != box["map"].end(); ++it)
        if (it.value().is_string())
            map_out[it.key()] = it.value().get<std::string>();
    if (map_out.empty()) {
        error = "the printer's slot map table is empty";
        return false;
    }
    return true;
}

bool CrealityCFS::verify_color_match(const std::string& colormatch_json, std::string& missing, std::string& read_error) const
{
    missing.clear();
    read_error.clear();

    json wanted;
    try {
        wanted = json::parse(colormatch_json);
    } catch (const std::exception&) {
        read_error = "the mapping could not be checked because it is not valid JSON";
        return false;
    }
    if (!wanted.is_array() || wanted.empty()) {
        read_error = "there was no mapping to check";
        return false;
    }

    // What the map should say afterwards: the G-code slot label on the left, the
    // physical slot label on the right, which is exactly Moonraker's box.map.
    std::vector<std::pair<std::string, std::string>> expected;
    for (const auto& w : wanted) {
        const std::string id  = json_str(w, "id");
        const int         box = json_int(w, "boxId", -1);
        const int         mat = json_int(w, "materialId", -1);
        if (id.empty() || box < 1 || box > MAX_UNITS || mat < 0 || mat >= SLOTS_PER_UNIT)
            continue; // the external spool holder has no map entry
        expected.emplace_back(id, "T" + std::to_string(box) + std::string(1, char('A' + mat)));
    }
    if (expected.empty()) {
        read_error = "the mapping named no CFS slot to check";
        return false;
    }

    // Read the live table back from Moonraker. This is where the read-back has to
    // go: on K2 firmware the reply to {"method":"get","params":{"boxsInfo":1}}
    // carries only same_material, materialBoxs and enable, with NO colorMatch
    // key at all, unless a job is currently holding a map. Verified live against
    // 192.168.1.50 on 2026-09-13: immediately after a colorMatch write that the
    // printer accepted, boxsInfo had no colorMatch while box.map read
    // {"T1A":"T2B",...}. Reading boxsInfo alone is what made the first mapped
    // print fail with "(no reply)".
    std::map<std::string, std::string> live;
    std::string                        map_error;
    if (read_gcode_map(live, map_error)) {
        std::string bad;
        for (const auto& e : expected) {
            auto it = live.find(e.first);
            if (it != live.end() && it->second == e.second)
                continue;
            if (!bad.empty())
                bad += ", ";
            bad += e.first + " should serve from " + e.second + " but reads " +
                   (it == live.end() ? std::string("nothing") : it->second);
        }
        missing = bad;
        return bad.empty();
    }

    // Fall back to boxsInfo.colorMatch, which is what CrealityPrint's own client
    // reads and what a printer with a job loaded does report. Only reached when
    // Moonraker is not answering on port 7125.
    BOOST_LOG_TRIVIAL(info) << "CrealityCFS: " << map_error << "; falling back to boxsInfo.colorMatch";
    json boxs_info;
    try {
        LanSocket sock(m_cfs_host);
        sock.send(json{{"method", "get"}, {"params", {{"boxsInfo", 1}}}});
        if (!sock.read_until("boxsInfo", boxs_info)) {
            read_error = "the printer did not answer a boxsInfo request on port 9999, and " + map_error;
            return false;
        }
    } catch (const std::exception& e) {
        read_error = std::string("the mapping could not be read back: ") + e.what() + ", and " + map_error;
        return false;
    }

    if (!boxs_info.contains("colorMatch") || !boxs_info["colorMatch"].is_array()) {
        read_error = "the printer does not report its slot mapping on either port (" + map_error + ")";
        return false;
    }
    const json& have = boxs_info["colorMatch"];

    for (const auto& w : wanted) {
        const std::string id  = json_str(w, "id");
        const int         box = json_int(w, "boxId", -1);
        const int         mat = json_int(w, "materialId", -1);
        if (id.empty() || box < 1)
            continue;
        bool found = false;
        for (const auto& h : have) {
            if (json_str(h, "id") == id && json_int(h, "boxId", -2) == box && json_int(h, "materialId", -2) == mat) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (!missing.empty())
                missing += ", ";
            missing += id + " should serve from " + std::to_string(box) + std::string(1, char('A' + std::max(0, mat)));
        }
    }
    return missing.empty();
}

// -- start ---------------------------------------------------------------

bool CrealityCFS::start_job(const std::string& abs_path, const std::string& colormatch_json, int self_test, wxString& msg) const
{
    json list = json::array();
    bool use_spool_holder = false;
    if (!colormatch_json.empty()) {
        try {
            json parsed = json::parse(colormatch_json);
            if (parsed.is_array()) {
                list = parsed;
                for (const auto& e : list)
                    if (json_int(e, "boxId", -1) == 0)
                        use_spool_holder = true;
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "CrealityCFS: bad colorMatch payload: " << e.what();
        }
    }

    const bool use_color_match = !list.empty() && !use_spool_holder;

    if (use_color_match) {
        // Write the map, then read it back and only start once the printer agrees.
        // Starting on an unconfirmed map is how a job ends up unloading the wrong
        // spool.
        std::string missing;
        std::string read_error;
        bool        accepted = false;
        for (int attempt = 1; attempt <= COLORMATCH_WRITE_ATTEMPTS && !accepted; ++attempt) {
            try {
                LanSocket sock(m_cfs_host);
                sock.send(json{{"method", "set"},
                               {"params", {{"colorMatch", {{"path", abs_path}, {"list", list}}}}}});
            } catch (const std::exception& e) {
                msg = wxString::Format(_L("Could not send the filament mapping to the printer: %s"),
                                       wxString::FromUTF8(e.what()));
                return false;
            }
            for (int poll = 1; poll <= COLORMATCH_READBACK_POLLS && !accepted; ++poll) {
                std::this_thread::sleep_for(std::chrono::milliseconds(COLORMATCH_POLL_MS));
                accepted = verify_color_match(colormatch_json, missing, read_error);
                BOOST_LOG_TRIVIAL(info) << "CrealityCFS: colorMatch write " << attempt << ", read-back " << poll << ": "
                                        << (accepted        ? std::string("accepted") :
                                            read_error.empty() ? ("not yet, " + missing) :
                                                                 ("could not read back, " + read_error));
            }
        }
        if (!accepted) {
            const std::string detail = read_error.empty() ? missing : read_error;
            msg = wxString::Format(_L("The printer did not accept the filament mapping (%s). "
                                      "The print was not started, so nothing has moved. "
                                      "Check that every mapped slot is loaded and that the CFS unit is connected."),
                                   wxString::FromUTF8(detail.c_str()));
            return false;
        }
    }

    try {
        LanSocket sock(m_cfs_host);
        if (use_color_match) {
            sock.send(json{{"method", "set"},
                           {"params", {{"multiColorPrint", {{"gcode", abs_path}, {"enableSelfTest", self_test}}}}}});
        } else {
            sock.send(json{{"method", "set"},
                           {"params", {{"opGcodeFile", "printprt:" + abs_path}, {"enableSelfTest", self_test}}}});
        }
        // The firmware may close the socket straight after accepting the start
        // command, so a read error here is not a failure: the command was written.
        sock.drain(2);
    } catch (const std::exception& e) {
        msg = wxString::Format(_L("Could not start the print: %s"), wxString::FromUTF8(e.what()));
        return false;
    }
    return true;
}

// -- upload --------------------------------------------------------------

std::string CrealityCFS::upload_url(const std::string& filename) const
{
    return (boost::format("http://%1%:%2%/upload/%3%") % m_cfs_host % CFS_UPLOAD_PORT % Http::url_encode(filename)).str();
}

bool CrealityCFS::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char* name = get_name();

    if (m_cfs_host.empty()) {
        error_fn(_L("No printer address."));
        return false;
    }

    // Spaces in a name confuse the start message, which is a bare path in a JSON
    // string handed to the shell side of the firmware. Keep it simple.
    std::string filename = upload_data.upload_path.filename().string();
    std::replace(filename.begin(), filename.end(), ' ', '_');

    const std::string url = upload_url(filename);
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: uploading %2% to %3%") % name % upload_data.source_path.string() % url;

    bool res         = true;
    bool body_says_ok = false;

    auto http = Http::post(url);
    // No API key, no CA file, no Content-MD5: Klipper4408Interface.cpp:52 clears
    // every header first, and the K2 upload endpoint has no authentication.
    //
    // Content-Type is deliberately NOT set by hand. CrealityPrint sets it, but
    // curl generates "multipart/form-data; boundary=..." from the form itself, and
    // anything we put in CURLOPT_HTTPHEADER replaces that wholesale, boundary and
    // all, which would make the body unparseable.
    http.headers_reset();
    // The multipart field name is the file name itself, not the literal "file".
    // Http::priv::form_add_file maps these onto CURLFORM_COPYNAME and
    // CURLFORM_FILENAME, so passing the file name for both is what the K2
    // firmware expects to find in the multipart body.
    http.form_add_file(filename, upload_data.source_path.string(), filename)
        .timeout_connect(5)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: upload finished, HTTP %2%: %3%") % name % status % body;
            // Success is a case-insensitive substring test for OK in the body, not
            // the HTTP status. Klipper4408Interface.cpp:68.
            body_says_ok = boost::icontains(body, "OK");
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: upload error: %2%, HTTP %3%, body: `%4%`") % name % error %
                                            status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": upload cancelled";
                res = false;
            }
        })
        .perform_sync();

    if (!res)
        return false;

    if (!body_says_ok) {
        error_fn(_L("The printer did not confirm the upload. Nothing was started."));
        return false;
    }

    if (upload_data.post_action != PrintHostPostUploadAction::StartPrint)
        return true;

    std::string root;
    std::string root_error;
    query_gcode_root(root, root_error);
    const std::string abs_path = root + "/" + filename;
    BOOST_LOG_TRIVIAL(info) << name << ": starting " << abs_path;

    std::string colormatch_json;
    {
        auto it = upload_data.extended_info.find(EXTENDED_COLORMATCH);
        if (it != upload_data.extended_info.end())
            colormatch_json = it->second;
    }
    int self_test = 0;
    {
        auto it = upload_data.extended_info.find(EXTENDED_SELFTEST);
        if (it != upload_data.extended_info.end()) {
            try {
                self_test = std::stoi(it->second);
            } catch (...) {
                self_test = 0;
            }
        }
    }

    wxString msg;
    if (!start_job(abs_path, colormatch_json, self_test, msg)) {
        error_fn(std::move(msg));
        return false;
    }

    if (info_fn)
        info_fn(L"complete", wxString::FromUTF8(abs_path.c_str()));
    return true;
}

} // namespace Slic3r
