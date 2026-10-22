#pragma once

#include "au2ft601/ft601_config.hpp"
#include "au2ft601/transport.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace au2ft601 {

struct D3xxTransportOptions {
    // open_by_serial refuses a device whose FIFOClock differs from this.
    std::optional<FifoClock> expected_fifo_clock;
    // Channel-1 IN URB buffer size set before FT_Create. 0 selects the
    // bring-up profile from the negotiated USB speed: 512 at 480 Mb/s,
    // 1024 at SuperSpeed; an unknown speed is refused.
    std::uint32_t in_urb_buffer_size{0};
    // Channel-1 IN URBs the library keeps queued; 0 keeps the library default (8).
    std::uint8_t in_urb_count{0};
    // Channel-1 IN library queue depth in URB buffers; 0 keeps the library
    // default (256).
    std::uint16_t in_urb_buffer_count{0};
    // D3XX fNonThreadSafeTransfer for both pipes: faster, but only if at most
    // one thread uses each pipe at a time.
    bool non_thread_safe{false};
};

// Thin D3XX 1.1.8 adapter for channel 1 (FIFO index 0) using the synchronous
// FT_ReadPipeEx/FT_WritePipeEx calls, plus pipelined FT_ReadPipeAsync reads
// for streaming. It never writes chip configuration.
class D3xxTransport final : public Transport {
public:
    explicit D3xxTransport(D3xxTransportOptions options);
    ~D3xxTransport() override;
    D3xxTransport(const D3xxTransport&) = delete;
    D3xxTransport& operator=(const D3xxTransport&) = delete;

    std::vector<DeviceInfo> list() override;
    // Throws DeviceError. Validates 245 mode, one channel, expected clock and
    // bulk endpoints 0x02/0x82 before returning.
    void open_by_serial(const std::string& serial) override;
    void close() noexcept override;
    IoResult read(std::uint8_t* destination, std::size_t capacity, std::uint32_t timeout_ms) override;
    IoResult write(const std::uint8_t* source, std::size_t size, std::uint32_t timeout_ms) override;
    // Overlapped reads (hardware findings, 2026-09-23): D3XX 1.1.8 ends each
    // one after about 100 ms whatever the pipe timeout, returning FT_OK with
    // the bytes that arrived or FT_TIMEOUT with none; later bytes wait in the
    // library for the next read, so nothing is lost and order is kept.  At
    // most bURBCount of them can be in flight: submitting one more blocks for
    // about 100 ms, so the depth is capped at the effective URB count.
    unsigned begin_pipelined_reads(std::size_t read_bytes, unsigned depth) override;
    std::vector<std::uint8_t> end_pipelined_reads() override;

    // Multi-line description of the opened session for evidence logs.
    const std::string& session_info() const { return session_info_; }
    const ConfigImage& opened_config() const { return config_; }
    // The FT_HANDLE, for measurement tools that call D3XX directly.
    void* native_handle() const { return handle_; }

private:
    struct Pipeline;
    IoResult read_pipelined(std::uint8_t* destination, std::size_t capacity, std::uint32_t timeout_ms);

    D3xxTransportOptions options_;
    std::unique_ptr<Pipeline> pipeline_;
    std::vector<std::uint8_t> stash_;  // reaped when pipelining failed to start
    unsigned in_urb_count_{0};         // effective IN URB count after open
    void* handle_{nullptr};
    std::string session_info_;
    ConfigImage config_{};
};

}  // namespace au2ft601
