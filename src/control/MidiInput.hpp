#pragma once

#include "ControlEventQueue.hpp"
#include "core/Result.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace quadmapper::control {

// MIDI input through WinMM (ADR-048 §1): any number of input devices opened by
// name, each with a CALLBACK_FUNCTION callback.
//
// The callback runs on a WinMM thread and may only call a few system functions, so
// it does the minimum: decode the short message (a few shifts and masks) and push a
// ControlEvent, which holds no heap memory, into the fixed-slot queue under its
// mutex. Nothing allocates and no WinMM function is called from it.
//
// Hot-unplug: WinMM does not reliably tell us a device vanished, so refresh()
// compares the open devices with the current device list and closes the missing
// ones. A port's callback context is never freed until this object is destroyed, so
// a late callback from a device whose close failed (because it was unplugged) still
// finds valid memory; and close waits for a callback in flight to leave first.
class MidiInput {
public:
    explicit MidiInput(ControlEventQueue& queue);
    ~MidiInput();

    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // Names of the MIDI input devices present now (UTF-8), in WinMM order.
    static std::vector<std::string> listDevices();

    // Open the first present, not yet open device with this name and start it.
    Result<void, Error> open(const std::string& name);
    void close(const std::string& name);
    void closeAll();

    bool isOpen(const std::string& name) const;
    std::vector<std::string> openDeviceNames() const;

    // Close open devices that are no longer in `present` (unplugged). Returns the
    // names that were closed.
    std::vector<std::string> closeMissing(const std::vector<std::string>& present);

    uint64_t messagesReceived() const { return received_.load(std::memory_order_relaxed); }

    struct Port;  // WinMM callback context (defined in the .cpp)

private:
    void closePort(Port& port);

    ControlEventQueue& queue_;
    mutable std::mutex mutex_;  // guards ports_ (main thread only in practice)
    std::vector<std::unique_ptr<Port>> ports_;  // open and closed; freed in the destructor
    std::atomic<uint64_t> received_{0};
};

}  // namespace quadmapper::control
