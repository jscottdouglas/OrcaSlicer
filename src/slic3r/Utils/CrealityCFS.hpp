#ifndef slic3r_CrealityCFS_hpp_
#define slic3r_CrealityCFS_hpp_

// Native Creality CFS support for the K2 family (K2 Plus, K2 Pro, K2, SPARKX i7).
//
// This is a separate print host type ("crealitycfs") that sits alongside the stock
// "crealityprint" host. Nothing here changes the behaviour of any other host type:
// a printer that is not configured as Creality CFS never reaches this code.
//
// What it adds over the stock CrealityPrint host:
//
//   * the upload goes to port 80 with the multipart field name set to the file
//     name itself, which is what the K2 firmware expects
//   * the absolute G-code path is asked for with reqGcodeFile instead of being
//     hard coded
//   * the colorMatch table is written, then read back and verified before the
//     start message is sent, in the spirit of CrealityPrint's own
//     SetColorMatchWithRetry. The read-back goes
//     to Moonraker's box.map, because K2 firmware only puts a colorMatch key in
//     the boxsInfo reply while a job is holding one
//   * the slot table merges boxsInfo with Moonraker's box object so remaining
//     length is known and a manual touchscreen slot edit is visible
//   * every mapped slot's minTemp is raised to the job's own nozzle temperature
//     with modifyMaterial before the colorMatch write, because the printer takes
//     its pre-print calibration, load, unload and purge temperatures from the
//     slot's material entry and not from the G-code
//   * the G-code tool label arithmetic is the printer's own: tool 0 is T1A,
//     tool 4 is T2A, tool 5 is T2B

#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/string.h>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;

// One physical CFS slot, merged from boxsInfo and the Moonraker box object.
struct CfsSlot
{
    int         unit    = 0;      // 1..4, or 0 for the external spool holder
    int         slot    = 0;      // 0..3 = A..D
    std::string type;             // "PLA", "PETG", ... empty when the slot is empty
    std::string colour;           // "#RRGGBB", empty when unknown
    std::string colour_raw;       // exactly what the printer reported, "#0RRGGBB"
    std::string vendor;
    std::string name;
    std::string rfid;             // Creality material id, e.g. "P1003"
    int         percent    = 0;
    double      remain_len = -1.0; // metres, Moonraker only, negative when unknown
    bool        present    = false;
    bool        loaded     = false; // currently fed to the toolhead
    bool        mapped     = false; // a G-code slot label resolves here right now
    bool        edited     = false; // type came from a manual edit, not an RFID tag

    // The rest of the printer's own material entry, carried over unchanged by a
    // modifyMaterial write. The printer heats to minTemp for everything it does
    // outside the job itself, so this is what the send-time sync raises.
    int         box_type   = -1;   // materialBoxs[].type, negative on every variant
                                   // but the CFS Mini, the only one whose slot
                                   // edit carries it
    double      min_temp   = -1.0; // C, negative when the printer did not say
    double      max_temp   = -1.0; // C, negative when the printer did not say
    double      pressure   = 0.0;

    bool        is_external() const { return unit == 0; }
    std::string label() const;      // "2B", or "Ext" for the spool holder
    std::string tnn() const;        // "T2B"
};

struct CfsSlotTable
{
    std::vector<CfsSlot>               slots;
    std::set<int>                      units_present;
    std::map<std::string, std::string> gcode_map;   // {"T1A":"T2B", ...} from Moonraker
    std::string                        model_id;    // "F008"
    std::string                        model_name;  // "K2 Plus"
    std::string                        hostname;

    const CfsSlot* get(int unit, int slot) const;
    bool           map_is_identity() const;
};

// Deliberately not derived from CrealityPrint: that class calls its own virtual
// test() from model_name(), so a subclass that overrode test() would recurse.
class CrealityCFS : public PrintHost
{
public:
    explicit CrealityCFS(DynamicPrintConfig* config);
    ~CrealityCFS() override = default;

    const char* get_name() const override;
    bool        can_test() const override { return true; }
    bool        has_auto_discovery() const override { return true; }
    std::string get_host() const override { return m_cfs_host; }

    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString& msg) const override;
    bool                       test(wxString& curl_msg) const override;
    PrintHostPostUploadActions get_post_upload_actions() const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;

    // The Device tab. With "Device UI" left empty this writes a small self
    // contained camera page into the Orca data directory and returns a file:
    // URL for it, because the K2 camera is WebRTC behind Creality's own
    // base64 signalling envelope and no third party web UI can play it.
    // Setting "Device UI" by hand, for example to http://<ip>:4408, still wins.
    static std::string get_print_host_webui(DynamicPrintConfig* config);

    // Write the camera page for one printer and return its absolute path.
    // Exposed so it can be produced and inspected without a Device tab.
    static std::string write_camera_page(const std::string& host);

    // Read boxsInfo over ws://<ip>:9999 and merge Moonraker's box object into it.
    bool query_slot_table(CfsSlotTable& table, std::string& error) const;

    // The bare IP, with any scheme and port stripped. Port 80 and port 9999 are
    // fixed by the firmware, so a print_host carrying some other port (a Moonraker
    // 7125 address, say) still works.
    std::string bare_host() const { return m_cfs_host; }

    // Tool index to the slot label the printer's box module resolves it to.
    // 0 -> "T1A", 3 -> "T1D", 4 -> "T2A", 5 -> "T2B", 15 -> "T4D".
    static std::string index_to_tnn(int index);

    // The inverse: "T1A" -> 0, "T2B" -> 5. Returns -1 for anything that is not
    // one of those labels.
    static int tnn_to_index(const std::string& tnn);

    // The per-extruder nozzle temperatures out of the config block Orca writes
    // at the end of the G-code, from "; nozzle_temperature = 230,215". Index i
    // is Orca extruder i and 0 means the file did not name one;
    // "; nozzle_temperature_initial_layer" fills in whatever the first key left
    // out. An empty result means neither key was present.
    static std::vector<int> parse_nozzle_temperatures(const std::string& gcode_tail);

    // Creality reports "#0RRGGBB" (seven hex digits). Return "#RRGGBB", or an
    // empty string when the value means "no colour".
    static std::string normalise_colour(const std::string& value);

    // Squared RGB distance between two "#RRGGBB" strings. Returns a large finite
    // value when either side is unknown, so a colourless slot loses to any slot
    // with a colour without being excluded outright.
    static double colour_distance(const std::string& a, const std::string& b);

    // The extended_info key the send dialog writes the accepted mapping into, as
    // a JSON array of {"id","type","color","boxId","materialId"} objects.
    static constexpr const char* EXTENDED_COLORMATCH = "cfs_colormatch";
    static constexpr const char* EXTENDED_SELFTEST   = "cfs_selftest";

    // "1" or "0": whether the send writes the job's nozzle temperature into the
    // mapped slots. Absent means yes, which is the dialog's default.
    static constexpr const char* EXTENDED_SYNC_SLOT_TEMP = "cfs_sync_slot_temp";

    // "F008" -> "K2 Plus". Empty id gives an empty string.
    static std::string model_name_for(const std::string& model_id);

private:
    std::string m_cfs_host;

    std::string upload_url(const std::string& filename) const;
    bool        query_gcode_root(std::string& root, std::string& error) const;
    bool        start_job(const std::string& abs_path, const std::string& colormatch_json, int self_test, wxString& msg) const;

    // The live G-code-slot to physical-slot table, {"T1A":"T2B", ...}, read from
    // Moonraker's box object. This is the same table colorMatch writes, and it
    // is readable whether or not a job is running, which boxsInfo.colorMatch
    // is not.
    bool read_gcode_map(std::map<std::string, std::string>& map_out, std::string& error) const;

    // True when the printer's live map already serves every requested entry.
    // `missing` names the entries that disagree; `read_error` is set instead
    // when the map could not be read at all, which is a different problem and
    // gets a different message.
    bool verify_color_match(const std::string& colormatch_json, std::string& missing, std::string& read_error) const;

    // boxsInfo on its own, without the Moonraker merge query_slot_table does.
    // This is what the slot temperature write reads to carry the existing entry
    // over, and what it polls to read the write back.
    bool read_slot_materials(std::vector<CfsSlot>& slots, std::string& error) const;

    // "standby", "printing", "paused", "complete", ... Moonraker's
    // print_stats.state when it answers, otherwise derived from the LAN
    // socket's own deviceState flag, and "unknown" when neither said anything.
    std::string print_state() const;

    // What sync_slot_temperatures managed to do. The caller composes the message
    // from this rather than being handed one, because whether a slot that never
    // read back is fatal depends on whether a print is about to start.
    struct SlotTempOutcome
    {
        std::string unverified; // the slots that never read back, with their values
        std::string changed;    // the slots this call did change, comma separated
        std::string transport;  // set instead when a write could not be sent at all
    };

    // Raise every mapped slot's minTemp to the job's nozzle temperature for that
    // extruder. `job_temps` is parse_nozzle_temperatures' result. A slot already
    // at or above the job temperature, one the G-code names no temperature for,
    // and the external spool holder are all left alone.
    //
    // Every slot that needs raising is written in one round, then a single
    // boxsInfo read per poll checks all of them at once and the ones that agree
    // drop out; the remainder get another round, up to the colorMatch write's
    // budget. So the cost is bounded by the budget and not by the slot count.
    // False means at least one slot never read back; `outcome` says which.
    bool sync_slot_temperatures(const std::string&      colormatch_json,
                                const std::vector<int>& job_temps,
                                const InfoFn&           info_fn,
                                SlotTempOutcome&        outcome) const;

    // Report a sync that did not fully land, and say whether the send may carry
    // on. For Upload and Print it must not: the job would start with a slot the
    // printer will heat to the wrong temperature, so this raises the error and
    // returns false. For a plain Upload it may: the file is on the printer, no
    // job is being started, and nothing is about to print, so this reports a
    // warning and returns true. Either way the message names the slots that were
    // already changed, because the printer has been left part way.
    bool handle_slot_temp_failure(const SlotTempOutcome& outcome,
                                  bool                   will_start_print,
                                  const ErrorFn&         error_fn,
                                  const InfoFn&          info_fn) const;

    mutable wxString m_test_summary;
};

} // namespace Slic3r

#endif
