#include "d3xx_transport.hpp"

#include "d3xx_common.hpp"

#include "ftd3xx.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>

namespace au2ft601 {
namespace {

constexpr UCHAR kFifoIndex = 0;       // channel 1 for FT_*PipeEx and FT_SetTransferParams
constexpr UCHAR kInEndpoint = 0x82;   // channel 1 IN, validated from descriptors only
constexpr UCHAR kOutEndpoint = 0x02;  // channel 1 OUT, validated from descriptors only

static_assert(sizeof(FT_60XCONFIGURATION) == kFt601ConfigSize, "FT_60XCONFIGURATION size");
static_assert(sizeof(ULONG) == 4, "D3XX ULONG is 32-bit on Linux");

IoStatus map_status(FT_STATUS status) {
    if (status == FT_OK) {
        return IoStatus::ok;
    }
    return status == FT_TIMEOUT ? IoStatus::timeout : IoStatus::error;
}

}  // namespace

// Overlapped reads kept in flight in submission order.  `head` is the oldest;
// D3XX fills them in that order.  `carry` holds bytes already reaped but not
// yet returned by read().
struct D3xxTransport::Pipeline {
    std::vector<OVERLAPPED> overlapped;
    std::vector<std::vector<std::uint8_t>> buffers;
    std::vector<bool> pending;
    std::size_t head{0};
    std::vector<std::uint8_t> carry;
    std::size_t carry_at{0};
    bool failed{false};
};

D3xxTransport::D3xxTransport(D3xxTransportOptions options) : options_(options) {}

D3xxTransport::~D3xxTransport() { close(); }

std::vector<DeviceInfo> D3xxTransport::list() {
    std::vector<DeviceInfo> devices;
    for (const auto& device : d3xx::enumerate_devices()) {
        devices.push_back(DeviceInfo{device.serial, device.description, 0, 0});
    }
    return devices;
}

void D3xxTransport::open_by_serial(const std::string& serial) {
    if (handle_ != nullptr) {
        throw DeviceError("transport is already open");
    }
    const auto listed = d3xx::require_unique(d3xx::enumerate_devices(), serial);
    const auto speed = d3xx::usb_speed_for_serial(serial);
    std::uint32_t urb = options_.in_urb_buffer_size;
    if (urb == 0) {
        if (speed && *speed == "480") {
            urb = 512;
        } else if (speed && (*speed == "5000" || *speed == "10000" || *speed == "20000")) {
            urb = 1024;
        } else {
            throw DeviceError("cannot select the bring-up URB size: USB speed for serial " + serial + " is " +
                              (speed ? *speed : std::string("unknown")) + "; pass an explicit size");
        }
    }

    // Must precede FT_Create; it applies to this process until the last FT_Close.
    FT_TRANSFER_CONF conf;
    std::memset(&conf, 0, sizeof(conf));
    conf.wStructSize = sizeof(FT_TRANSFER_CONF);
    conf.pipe[FT_PIPE_DIR_IN].fPipeNotUsed = FALSE;
    conf.pipe[FT_PIPE_DIR_OUT].fPipeNotUsed = FALSE;
    conf.pipe[FT_PIPE_DIR_IN].dwURBBufferSize = urb;
    conf.pipe[FT_PIPE_DIR_IN].bURBCount = options_.in_urb_count;  // < 2 selects the library default
    conf.pipe[FT_PIPE_DIR_IN].wURBBufferCount = options_.in_urb_buffer_count;  // < 2: default
    conf.pipe[FT_PIPE_DIR_IN].fNonThreadSafeTransfer = options_.non_thread_safe ? TRUE : FALSE;
    conf.pipe[FT_PIPE_DIR_OUT].fNonThreadSafeTransfer = options_.non_thread_safe ? TRUE : FALSE;
    FT_STATUS status = FT_SetTransferParams(&conf, kFifoIndex);
    if (status != FT_OK) {
        throw DeviceError("FT_SetTransferParams failed: " + d3xx::status_name(status));
    }

    FT_HANDLE handle = nullptr;
    status = FT_Create(const_cast<char*>(serial.c_str()), FT_OPEN_BY_SERIAL_NUMBER, &handle);
    if (status != FT_OK) {
        throw DeviceError("FT_Create(" + serial + ") failed: " + d3xx::status_name(status));
    }
    handle_ = handle;
    try {
        FT_TRANSFER_CONF effective;
        std::memset(&effective, 0, sizeof(effective));
        effective.wStructSize = sizeof(FT_TRANSFER_CONF);
        status = FT_GetTransferParams(&effective, kFifoIndex);
        if (status != FT_OK) {
            throw DeviceError("FT_GetTransferParams failed: " + d3xx::status_name(status));
        }
        const auto& in = effective.pipe[FT_PIPE_DIR_IN];
        if (in.dwURBBufferSize != urb) {
            throw DeviceError("effective IN URB buffer size " + std::to_string(in.dwURBBufferSize) +
                              " differs from requested " + std::to_string(urb));
        }
        in_urb_count_ = in.bURBCount;
        if (options_.in_urb_count >= 2 && in.bURBCount != options_.in_urb_count) {
            throw DeviceError("effective IN URB count " + std::to_string(static_cast<unsigned>(in.bURBCount)) +
                              " differs from requested " + std::to_string(static_cast<unsigned>(options_.in_urb_count)));
        }

        FT_60XCONFIGURATION config{};
        status = FT_GetChipConfiguration(handle, &config);
        if (status != FT_OK) {
            throw DeviceError("FT_GetChipConfiguration failed: " + d3xx::status_name(status));
        }
        std::memcpy(config_.data(), &config, config_.size());
        const auto decoded = decode_config(config_);
        if (decoded.fifo_mode != kFifoMode245 || decoded.channel_config != kChannelConfig1Bidirectional) {
            throw DeviceError("device is not configured for 245 mode with one bidirectional channel");
        }
        if (options_.expected_fifo_clock &&
            decoded.fifo_clock != static_cast<std::uint8_t>(*options_.expected_fifo_clock)) {
            throw DeviceError("device FIFOClock is " + fifo_clock_name(decoded.fifo_clock) + ", expected " +
                              fifo_clock_name(static_cast<std::uint8_t>(*options_.expected_fifo_clock)));
        }

        bool have_in = false;
        bool have_out = false;
        FT_CONFIGURATION_DESCRIPTOR usb_config{};
        status = FT_GetConfigurationDescriptor(handle, &usb_config);
        if (status != FT_OK) {
            throw DeviceError("FT_GetConfigurationDescriptor failed: " + d3xx::status_name(status));
        }
        for (UCHAR interface_index = 0; interface_index < usb_config.bNumInterfaces; ++interface_index) {
            FT_INTERFACE_DESCRIPTOR descriptor{};
            if (FT_GetInterfaceDescriptor(handle, interface_index, &descriptor) != FT_OK) {
                continue;
            }
            for (UCHAR pipe_index = 0; pipe_index < descriptor.bNumEndpoints; ++pipe_index) {
                FT_PIPE_INFORMATION pipe{};
                if (FT_GetPipeInformation(handle, interface_index, pipe_index, &pipe) == FT_OK &&
                    FT_IS_BULK_PIPE(pipe.PipeType)) {
                    have_in = have_in || pipe.PipeId == kInEndpoint;
                    have_out = have_out || pipe.PipeId == kOutEndpoint;
                }
            }
        }
        if (!have_in || !have_out) {
            throw DeviceError("channel-1 bulk endpoints 0x82/0x02 were not both found");
        }

        std::ostringstream info;
        info << "serial: " << serial << "\ndescription: " << listed.description
             << "\nUSB speed: " << (speed ? *speed + " Mb/s" : std::string("unknown"))
             << "\nFIFOClock: " << fifo_clock_name(decoded.fifo_clock)
             << "\noptional features: 0x" << std::hex << decoded.optional_features << std::dec
             << "\nconfiguration sha256: " << config_sha256(config_)
             << "\ntransfer params (FIFO 0 IN): URB buffer " << in.dwURBBufferSize << " B, URB count "
             << static_cast<unsigned>(in.bURBCount) << ", URB buffer count " << in.wURBBufferCount
             << ", non-thread-safe " << in.fNonThreadSafeTransfer
             << ", stop-on-underrun " << effective.fStopReadingOnURBUnderrun
             << "\nread API: FT_ReadPipeEx(FIFO index 0); endpoints 0x82 IN / 0x02 OUT present\n";
        session_info_ = info.str();
    } catch (...) {
        close();
        throw;
    }
}

void D3xxTransport::close() noexcept {
    if (pipeline_) {
        try {
            (void)end_pipelined_reads();
        } catch (...) {
            pipeline_.reset();
        }
    }
    if (handle_ != nullptr) {
        (void)FT_Close(static_cast<FT_HANDLE>(handle_));
        handle_ = nullptr;
    }
    stash_.clear();
}

IoResult D3xxTransport::read(std::uint8_t* destination, std::size_t capacity, std::uint32_t timeout_ms) {
    if (handle_ == nullptr || capacity > std::numeric_limits<ULONG>::max()) {
        return IoResult{IoStatus::error, 0, 0, handle_ == nullptr ? "not open" : "request too large"};
    }
    if (pipeline_) {
        return read_pipelined(destination, capacity, timeout_ms);
    }
    if (!stash_.empty()) {
        const std::size_t count = std::min(capacity, stash_.size());
        std::memcpy(destination, stash_.data(), count);
        stash_.erase(stash_.begin(), stash_.begin() + static_cast<std::ptrdiff_t>(count));
        return IoResult{IoStatus::ok, count, FT_OK, ""};
    }
    ULONG transferred = 0;
    const FT_STATUS status = FT_ReadPipeEx(static_cast<FT_HANDLE>(handle_), kFifoIndex, destination,
                                           static_cast<ULONG>(capacity), &transferred, timeout_ms);
    return IoResult{map_status(status), transferred, status, status == FT_OK ? "" : d3xx::status_name(status)};
}

IoResult D3xxTransport::write(const std::uint8_t* source, std::size_t size, std::uint32_t timeout_ms) {
    if (handle_ == nullptr || size > std::numeric_limits<ULONG>::max()) {
        return IoResult{IoStatus::error, 0, 0, handle_ == nullptr ? "not open" : "request too large"};
    }
    ULONG transferred = 0;
    const FT_STATUS status = FT_WritePipeEx(static_cast<FT_HANDLE>(handle_), kFifoIndex,
                                            const_cast<std::uint8_t*>(source), static_cast<ULONG>(size),
                                            &transferred, timeout_ms);
    return IoResult{map_status(status), transferred, status, status == FT_OK ? "" : d3xx::status_name(status)};
}

unsigned D3xxTransport::begin_pipelined_reads(std::size_t read_bytes, unsigned depth) {
    depth = std::min(depth, in_urb_count_);
    if (handle_ == nullptr || pipeline_ || depth == 0 || read_bytes == 0 ||
        read_bytes > std::numeric_limits<ULONG>::max()) {
        return 0;
    }
    const auto handle = static_cast<FT_HANDLE>(handle_);
    auto pipeline = std::make_unique<Pipeline>();
    pipeline->overlapped.resize(depth);
    pipeline->buffers.assign(depth, std::vector<std::uint8_t>(read_bytes));
    pipeline->pending.assign(depth, false);
    for (std::size_t slot = 0; slot < depth; ++slot) {
        std::memset(&pipeline->overlapped[slot], 0, sizeof(OVERLAPPED));
        if (FT_InitializeOverlapped(handle, &pipeline->overlapped[slot]) != FT_OK) {
            for (std::size_t done = 0; done < slot; ++done) {
                (void)FT_ReleaseOverlapped(handle, &pipeline->overlapped[done]);
            }
            return 0;
        }
    }
    bool ok = true;
    for (std::size_t slot = 0; ok && slot < depth; ++slot) {
        ULONG transferred = 0;
        const FT_STATUS status =
            FT_ReadPipeAsync(handle, kFifoIndex, pipeline->buffers[slot].data(),
                             static_cast<ULONG>(read_bytes), &transferred, &pipeline->overlapped[slot]);
        ok = status == FT_IO_PENDING || status == FT_OK;
        pipeline->pending[slot] = ok;
    }
    pipeline_ = std::move(pipeline);
    if (!ok) {
        // Reap what was submitted; synchronous reads return its bytes first.
        stash_ = end_pipelined_reads();
        return 0;
    }
    return depth;
}

IoResult D3xxTransport::read_pipelined(std::uint8_t* destination, std::size_t capacity, std::uint32_t timeout_ms) {
    auto& p = *pipeline_;
    const auto take_carry = [&]() {
        const std::size_t count = std::min(capacity, p.carry.size() - p.carry_at);
        std::memcpy(destination, p.carry.data() + p.carry_at, count);
        p.carry_at += count;
        if (p.carry_at == p.carry.size()) {
            p.carry.clear();
            p.carry_at = 0;
        }
        return IoResult{IoStatus::ok, count, FT_OK, ""};
    };
    if (capacity == 0) {
        return IoResult{IoStatus::ok, 0, FT_OK, ""};
    }
    if (p.carry_at < p.carry.size()) {
        return take_carry();
    }
    if (p.failed) {
        return IoResult{IoStatus::error, 0, 0, "pipelined reads have failed"};
    }
    const auto handle = static_cast<FT_HANDLE>(handle_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const std::size_t slot = p.head;
        ULONG transferred = 0;
        // Returns when the read fills or D3XX ends it (about 100 ms).
        FT_STATUS status = FT_GetOverlappedResult(handle, &p.overlapped[slot], &transferred, TRUE);
        if (status == FT_IO_INCOMPLETE) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return IoResult{IoStatus::timeout, 0, status, d3xx::status_name(status)};
            }
            continue;
        }
        p.pending[slot] = false;
        if (status != FT_OK && status != FT_TIMEOUT) {
            p.failed = true;
            return IoResult{IoStatus::error, 0, status, d3xx::status_name(status)};
        }
        const auto& buffer = p.buffers[slot];
        const std::size_t got = std::min<std::size_t>(transferred, buffer.size());
        p.carry.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
        p.carry_at = 0;
        // Resubmit before handing the bytes over, so the link never idles.
        ULONG ignored = 0;
        status = FT_ReadPipeAsync(handle, kFifoIndex, p.buffers[slot].data(), static_cast<ULONG>(buffer.size()),
                                  &ignored, &p.overlapped[slot]);
        if (status != FT_IO_PENDING && status != FT_OK) {
            p.failed = true;
        } else {
            p.pending[slot] = true;
            p.head = (slot + 1) % p.overlapped.size();
        }
        if (got != 0) {
            return take_carry();
        }
        if (p.failed) {
            return IoResult{IoStatus::error, 0, status, "FT_ReadPipeAsync: " + d3xx::status_name(status)};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return IoResult{IoStatus::timeout, 0, FT_TIMEOUT, "FT_TIMEOUT"};
        }
    }
}

std::vector<std::uint8_t> D3xxTransport::end_pipelined_reads() {
    if (!pipeline_) {
        return {};
    }
    auto pipeline = std::move(pipeline_);
    auto& p = *pipeline;
    std::vector<std::uint8_t> leftover(p.carry.begin() + static_cast<std::ptrdiff_t>(p.carry_at), p.carry.end());
    const auto handle = static_cast<FT_HANDLE>(handle_);
    const std::size_t depth = p.overlapped.size();
    for (std::size_t i = 0; i < depth; ++i) {
        const std::size_t slot = (p.head + i) % depth;
        if (!p.pending[slot]) {
            continue;
        }
        ULONG transferred = 0;
        FT_STATUS status = FT_IO_INCOMPLETE;
        for (int attempts = 0; status == FT_IO_INCOMPLETE && attempts < 100; ++attempts) {
            status = FT_GetOverlappedResult(handle, &p.overlapped[slot], &transferred, TRUE);
        }
        if (status == FT_IO_INCOMPLETE) {
            // Never observed: cancel rather than release a read D3XX still owns.
            (void)FT_AbortPipe(handle, kInEndpoint);
            (void)FT_GetOverlappedResult(handle, &p.overlapped[slot], &transferred, TRUE);
        }
        const std::size_t got = std::min<std::size_t>(transferred, p.buffers[slot].size());
        leftover.insert(leftover.end(), p.buffers[slot].begin(), p.buffers[slot].begin() + static_cast<std::ptrdiff_t>(got));
    }
    for (auto& overlapped : p.overlapped) {
        (void)FT_ReleaseOverlapped(handle, &overlapped);
    }
    return leftover;
}

}  // namespace au2ft601
