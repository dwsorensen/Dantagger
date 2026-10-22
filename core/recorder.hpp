#pragma once

// Writes the unprocessed record stream of the TDC board to FILE (raw 8-byte
// little-endian FPGA records, markers included, PROTOCOL.md 3.3) and
// FILE.json (what is needed to decode it, readable by tdcctl and by
// FileBackend): the app's "save raw tags" and "stream to disk" (user,
// 2026-10-22).  Never used by the Python library.
//
// Attached to TdcBackend as a RecordTap.  The reader thread only appends to a
// buffer; a writer thread writes it, so a slow disk never stalls the USB
// reads (beyond kMaxBuffered the records are counted as lost instead).
// A recording that joins a run late starts with a synthetic START marker (the
// backend supplies it); one that ends before its run (stop, time limit) ends
// with a synthetic END marker, so the file always decodes on its own.
// Events of channels outside `channel_mask` are not written (markers always).

#include "record_tap.hpp"

#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dt {

class StreamRecorder final : public RecordTap {
public:
    struct Options {
        std::string path;
        double max_seconds{0};          // FPGA time to record (0: until finish())
        unsigned channel_mask{0xF};     // bit c-1: channel c
        std::string note;               // written to the sidecar (e.g. which measurement)
    };
    struct Progress {
        std::string path;
        std::uint64_t records{0};       // written to the buffer
        std::array<std::uint64_t, kNumChannels> events{};
        std::uint64_t bytes_written{0};
        std::uint64_t lost{0};          // records not written (buffer full: disk too slow)
        std::uint64_t fpga_dropped{0};  // dropped by the FPGA (from the runs' final responses)
        double seconds{0};              // FPGA time covered
        int runs{0};
        bool limit_reached{false};
        bool finished{false};
        std::string error;
    };
    static constexpr std::size_t kMaxBuffered = 32u << 20;   // records (256 MB)

    explicit StreamRecorder(Options options);   // throws std::runtime_error if FILE cannot be created
    ~StreamRecorder() override;
    StreamRecorder(const StreamRecorder&) = delete;
    StreamRecorder& operator=(const StreamRecorder&) = delete;

    // Stops recording: ends an open run with an END marker, writes everything,
    // closes FILE and writes FILE.json.  Idempotent.
    void finish();
    Progress progress() const;
    bool limit_reached() const;

    void run_started(const RunInfo& info) override;
    void records(const std::uint64_t* raw, std::size_t n) override;
    void run_ended(const RunEnd& end) override;

private:
    void writer();
    void write_sidecar();
    void append_locked(std::uint64_t raw);

    Options opt_;
    std::ofstream file_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::uint64_t> pending_;
    bool quit_{false};
    bool accepting_{true};
    Progress p_;
    // run state (mu_)
    bool in_run_{false};
    bool end_written_{false};
    std::uint64_t coarse_{0};
    std::uint64_t run_start_coarse_{0};
    double seconds_before_{0};
    double tick_ns_{10.0};
    std::uint64_t dropped_marked_{0};
    std::vector<RunInfo> runs_;
    std::string started_at_;
    std::thread thread_;
};

}  // namespace dt
