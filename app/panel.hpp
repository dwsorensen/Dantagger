#pragma once

// A measurement panel: one Dantagger measurement (the same C++ classes the
// Python library exposes) in its own dockable window.  Any number can run at
// once on the live stream; each can instead read a saved tag stream.
//
// The base class owns what every panel has: the source (live or a saved
// file), Start / Stop / Continue / Clear, an optional run time (startFor),
// "save raw tags" (the unprocessed record stream of the live run into the
// data folder, user 2026-10-22), the shared channel table, the status line
// and the two-column layout.  A panel type adds its settings, how it reads
// the measurement's data, and its plots.
//
// Data flow: the measurement lives in the tagger's engine (processing
// thread); the panel copies its data under the engine mutex at most 10 times
// a second while it runs (once after any change) and draws from the copy.

#include "config.hpp"
#include "device.hpp"
#include "file_backend.hpp"
#include "measurements.hpp"
#include "recorder.hpp"
#include "ui_common.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace app {

enum class Kind { countrate, counter, histogram, correlation, histogram2d, stream };
constexpr int kKinds = 6;
const char* kind_title(Kind kind);      // "Countrate"
const char* kind_key(Kind kind);        // "countrate" (session file)
const char* kind_summary(Kind kind);    // one line for menus
bool kind_from_key(const std::string& key, Kind& out);

struct AppContext {
    Device& dev;
    const Device::View& v;
    std::string data_dir;
};

class Panel {
public:
    Panel(Kind kind, int id);
    virtual ~Panel();
    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    Kind kind() const { return kind_; }
    int id() const { return id_; }
    std::string window_name() const;    // "<title>###panel<id>" (stable id for docking)
    char title[64]{};
    bool open{true};

    // Every frame, also while the window is hidden: follows the run, the
    // file replay and the recording, and copies new data.
    void update(AppContext& ctx);
    // The window's contents.
    void draw(AppContext& ctx);

    bool running() const { return running_; }
    bool startable(const AppContext& ctx, std::string* why = nullptr) const;
    void start(AppContext& ctx, bool clear);
    void stop(AppContext& ctx);
    void clear();
    std::string state_text() const;     // short, for the sidebar
    ImVec4 state_colour() const;

    // Session file (the panels and their settings survive a restart).
    void save(KeyValues& kv, const std::string& prefix) const;
    void load(const KeyValues& kv, const std::string& prefix);

    // ---- common settings (also used by the self-test) ----
    bool use_file{false};
    std::string file_path;
    double run_s{0};                    // 0: until Stop
    bool save_raw{false};

    const std::vector<std::string>& recordings() const { return recordings_; }
    const std::string& error() const { return error_; }
    std::uint64_t data_version() const { return data_version_; }
    double capture_s() const { return static_cast<double>(capture_ps_) * 1e-12; }
    std::shared_ptr<dt::StreamRecorder> recorder() const { return recorder_; }
    // Key numbers of the shown data (self-test and the sidebar).
    virtual std::map<std::string, double> metrics() const { return {}; }
    // Key of the panel's CSV button (self-test: ui::g_force_csv).
    virtual std::string csv_key() const { return {}; }

protected:
    // ---- per panel type ----
    // A new measurement from the edited settings (throws std::invalid_argument).
    virtual std::shared_ptr<dt::Measurement> make() = 0;
    virtual bool edited_differs() const = 0;     // the edited settings differ from the measurement's
    virtual void adopt_edited() = 0;             // the measurement now has the edited settings
    virtual void settings_ui(AppContext& ctx) = 0;
    virtual void pull(dt::Measurement& m) = 0;   // copies the data (engine mutex held)
    virtual void view_ui(AppContext& ctx) = 0;
    virtual unsigned channel_mask() const = 0;   // channels the settings use (bit c-1 = channel c)
    virtual void cleared() {}                    // the data was cleared (drop local history)
    virtual void save_settings(KeyValues& kv, const std::string& prefix) const = 0;
    virtual void load_settings(const KeyValues& kv, const std::string& prefix) = 0;
    // TimeTagStream: Start always records (the panel is "stream to disk").
    virtual bool records_always() const { return false; }
    virtual unsigned record_mask() const { return 0xF; }
    virtual bool channels_open_by_default() const { return false; }
    virtual float settings_width() const { return 400.0f; }

    // ---- for panel types ----
    bool has_data() const { return m_ != nullptr; }
    bool is_file_run() const { return file_backend_ != nullptr; }
    const dt::FileBackend* file_backend() const { return file_backend_; }
    // "# ..." lines describing the source, the settings shared by all panels and the capture (CSV headers).
    std::string csv_preamble(const AppContext& ctx) const;
    std::string default_csv(const AppContext& ctx, const std::string& what) const;
    void log(AppContext& ctx, Level level, const std::string& text) const;
    std::uint64_t data_version_{0};
    dt::timestamp_t capture_ps_{0};

private:
    void header_ui(AppContext& ctx);
    void finish_recording(AppContext* ctx);
    void detach();                      // removes the measurement from its tagger (and closes a file replay)
    void replace_measurement(AppContext& ctx);
    void do_pull();

    Kind kind_;
    int id_;
    std::shared_ptr<dt::Tagger> tagger_;         // the tagger m_ is registered with (live or a file replay)
    dt::FileBackend* file_backend_{nullptr};     // of tagger_ when it replays a file
    std::shared_ptr<dt::Measurement> m_;
    std::shared_ptr<dt::StreamRecorder> recorder_;
    dt::TdcBackend* rec_backend_{nullptr};
    std::shared_ptr<dt::Tagger> rec_tagger_;     // keeps rec_backend_ alive while recording
    std::uint64_t generation_{0};
    bool running_{false};
    std::chrono::steady_clock::time_point last_pull_{};
    std::chrono::steady_clock::time_point started_at_{};
    bool dirty_{true};
    std::string error_;
    std::string note_;                           // last outcome ("file read", "window finished", ...)
    std::vector<std::string> recordings_;
    bool compact_header_{false};       // short window: source and run time in a popup
};

std::unique_ptr<Panel> make_panel(Kind kind, int id);
// Set by the main window: opens a new panel of `kind` reading `file` (the
// TimeTagStream panel's "open in a new panel").
extern std::function<void(Kind kind, const std::string& file)> g_open_panel_with_file;

// The channel settings shared by every measurement (trigger, hysteresis,
// input delay, dead time), as drawn in each panel and on the Comparator
// page.  Rows in `highlight` (bit c-1) are the channels the caller uses.
void channel_table(AppContext& ctx, unsigned highlight, bool comparator_only = false);

// A START/STOP channel picker (1..4).
bool channel_combo(const char* id, int* channel, float width = 90.0f);
// Channel checkboxes (bit c-1); keeps at least one set.
bool channel_checks(const char* id, unsigned* mask);

}  // namespace app
