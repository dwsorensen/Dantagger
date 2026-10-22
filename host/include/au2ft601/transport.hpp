#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace au2ft601 {

struct DeviceInfo {
    std::string serial;
    std::string description;
    std::uint16_t vendor_id{0};
    std::uint16_t product_id{0};
};

enum class IoStatus {
    ok,       // call completed; `bytes` may be fewer than requested (short read)
    timeout,  // timeout expired; `bytes` may still be nonzero and is valid data
    error,    // any other failure; `bytes` is still reported and must be accounted for
};

struct IoResult {
    IoStatus status{IoStatus::error};
    std::size_t bytes{0};
    std::uint32_t native_status{0};  // SDK status code, for logs only
    std::string detail;
};

// SDK-neutral boundary. The D3XX implementation owns all FTDI types; protocol,
// parser and block-reader code remain testable without the proprietary SDK.
// read/write address the single FT601 channel (FIFO index 0, endpoints 0x82/0x02).
class Transport {
public:
    virtual ~Transport() = default;
    virtual std::vector<DeviceInfo> list() = 0;
    virtual void open_by_serial(const std::string& serial) = 0;
    virtual void close() noexcept = 0;
    virtual IoResult read(std::uint8_t* destination, std::size_t capacity, std::uint32_t timeout_ms) = 0;
    virtual IoResult write(const std::uint8_t* source, std::size_t size, std::uint32_t timeout_ms) = 0;

    // Optional pipelined reading, for streaming at the link rate.  Between
    // begin_pipelined_reads and end_pipelined_reads the transport keeps up to
    // `depth` reads of `read_bytes` each in flight (it may use fewer and
    // returns how many; 0 means the mode is unavailable and nothing changed).
    // read() then returns the next bytes of the stream in order as soon as any
    // have arrived (at most `capacity`); asking for more than will arrive is
    // harmless.  end_pipelined_reads waits for the reads still in flight and
    // returns every byte received but not yet returned by read().  A transport
    // without the mode is read as before.
    virtual unsigned begin_pipelined_reads(std::size_t /*read_bytes*/, unsigned /*depth*/) { return 0; }
    virtual std::vector<std::uint8_t> end_pipelined_reads() { return {}; }
};

}  // namespace au2ft601
