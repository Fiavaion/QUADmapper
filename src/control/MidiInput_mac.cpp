// MIDI input through CoreMIDI: the macOS half of MidiInput (MidiInput_win.cpp is WinMM).

#include "MidiInput.hpp"

#include "MidiDecode.hpp"
#include "core/Logger.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace quadmapper::control {

struct MidiInput::Port {
    MIDIPortRef port = 0;
    MIDIEndpointRef source = 0;
    std::string name;
    DeviceName fixedName{};           // copied into every event without allocating
    ControlEventQueue* queue = nullptr;
    std::atomic<uint64_t>* received = nullptr;
    std::atomic<bool> open{false};
    std::atomic<int> inCallback{0};
};

namespace {

// One CoreMIDI client for the process, made on first use and never disposed. Made on
// the main thread in practice, whose run loop GLFW pumps: that is what keeps
// CoreMIDI's list of sources current when a device is plugged in or out.
MIDIClientRef midiClient() {
    static MIDIClientRef client = [] {
        MIDIClientRef created = 0;
        const OSStatus status = MIDIClientCreate(CFSTR("QUADmapper"), nullptr, nullptr, &created);
        if (status != noErr) Logger::warning("MIDI: CoreMIDI is not available (error {})", status);
        return created;
    }();
    return client;
}

std::string sourceName(MIDIEndpointRef source) {
    CFStringRef text = nullptr;
    if (MIDIObjectGetStringProperty(source, kMIDIPropertyDisplayName, &text) != noErr || !text) {
        return std::string();
    }
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(capacity), '\0');
    const bool converted = CFStringGetCString(text, out.data(), capacity, kCFStringEncodingUTF8);
    CFRelease(text);
    if (!converted) return std::string();
    out.resize(std::strlen(out.c_str()));
    return out;
}

// How many 32-bit words a Universal MIDI Packet has, by its message type (top 4 bits).
constexpr int UMP_WORDS[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
constexpr uint32_t UMP_MIDI1_CHANNEL_VOICE = 0x2;

// Runs on a CoreMIDI thread, so it does the minimum, as the WinMM callback does:
// decode, push a ControlEvent that holds no heap memory, count.
void receive(MidiInput::Port& port, const MIDIEventList* list) {
    port.inCallback.fetch_add(1, std::memory_order_acq_rel);
    if (port.open.load(std::memory_order_acquire)) {
        const MIDIEventPacket* packet = &list->packet[0];
        for (UInt32 p = 0; p < list->numPackets; ++p) {
            for (UInt32 w = 0; w < packet->wordCount;) {
                const uint32_t word = packet->words[w];
                const uint32_t type = word >> 28;
                w += static_cast<UInt32>(UMP_WORDS[type]);
                if (type != UMP_MIDI1_CHANNEL_VOICE) continue;  // SysEx, clock and the rest are ignored
                // The packet holds status, data1, data2 from the high end; the decoder
                // takes the WinMM layout, status in the low byte.
                const uint32_t status = (word >> 16) & 0xFF;
                const uint32_t data1 = (word >> 8) & 0x7F;
                const uint32_t data2 = word & 0x7F;
                if (auto decoded = decodeMidiShortMessage(status | (data1 << 8) | (data2 << 16))) {
                    ControlEvent event;
                    event.source = ControlEvent::Source::Midi;
                    event.midi = *decoded;
                    event.device = port.fixedName;
                    port.queue->push(std::move(event));
                    port.received->fetch_add(1, std::memory_order_relaxed);
                }
            }
            packet = MIDIEventPacketNext(packet);
        }
    }
    port.inCallback.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace

MidiInput::MidiInput(ControlEventQueue& queue) : queue_(queue) {}

MidiInput::~MidiInput() {
    closeAll();
}

std::vector<std::string> MidiInput::listDevices() {
    std::vector<std::string> names;
    if (midiClient() == 0) return names;
    const ItemCount count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i) {
        names.push_back(sourceName(MIDIGetSource(i)));
    }
    return names;
}

Result<void, Error> MidiInput::open(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t alreadyOpen = 0;  // same-named devices already open (two identical controllers)
    for (const auto& port : ports_) {
        if (port->open && port->name == name) ++alreadyOpen;
    }

    const MIDIClientRef client = midiClient();
    const ItemCount count = client != 0 ? MIDIGetNumberOfSources() : 0;
    size_t seen = 0;
    for (ItemCount i = 0; i < count; ++i) {
        const MIDIEndpointRef source = MIDIGetSource(i);
        if (sourceName(source) != name) continue;
        if (seen++ < alreadyOpen) continue;

        auto port = std::make_unique<Port>();
        port->name = name;
        const size_t n = std::min(name.size(), port->fixedName.size() - 1);
        std::memcpy(port->fixedName.data(), name.data(), n);
        port->queue = &queue_;
        port->received = &received_;
        port->source = source;
        port->open = true;  // set before the connection so the first message is not lost

        auto fail = [&name](const char* what, OSStatus status) {
            const std::string message = std::string("cannot ") + what + " MIDI device '" + name +
                                        "': CoreMIDI error " + std::to_string(status);
            Logger::warning("MIDI: {}", message);
            return Result<void, Error>::error(Error(message));
        };

        // The context outlives every callback: a Port is freed only with this object.
        Port* context = port.get();
        OSStatus status = MIDIInputPortCreateWithProtocol(
            client, CFSTR("QUADmapper input"), kMIDIProtocol_1_0, &port->port,
            ^(const MIDIEventList* list, void* /*connection*/) { receive(*context, list); });
        if (status != noErr) return fail("open", status);
        status = MIDIPortConnectSource(port->port, source, nullptr);
        if (status != noErr) {
            ports_.push_back(std::move(port));
            closePort(*ports_.back());
            return fail("start", status);
        }
        Logger::info("MIDI: opened '{}'", name);
        ports_.push_back(std::move(port));
        return Result<void, Error>::success();
    }
    const std::string message = alreadyOpen > 0 ? "MIDI device '" + name + "' is already open"
                                                : "MIDI device '" + name + "' is not connected";
    return Result<void, Error>::error(Error(message));
}

void MidiInput::closePort(Port& port) {
    port.open = false;
    if (port.port != 0) {
        // Errors are expected for an unplugged device and change nothing here.
        MIDIPortDisconnectSource(port.port, port.source);
        MIDIPortDispose(port.port);
        port.port = 0;
    }
    // Let a callback that was already running finish before the caller moves on.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (port.inCallback.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

void MidiInput::close(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& port : ports_) {
        if (port->open && port->name == name) {
            closePort(*port);
            Logger::info("MIDI: closed '{}'", name);
        }
    }
}

void MidiInput::closeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& port : ports_) {
        if (port->open || port->port != 0) closePort(*port);
    }
}

bool MidiInput::isOpen(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(ports_.begin(), ports_.end(),
                       [&](const auto& port) { return port->open && port->name == name; });
}

std::vector<std::string> MidiInput::openDeviceNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& port : ports_) {
        if (port->open) names.push_back(port->name);
    }
    return names;
}

std::vector<std::string> MidiInput::closeMissing(const std::vector<std::string>& present) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> closed;
    for (auto& port : ports_) {
        if (!port->open) continue;
        const size_t openWithName = static_cast<size_t>(std::count_if(
            ports_.begin(), ports_.end(),
            [&](const auto& p) { return p->open && p->name == port->name; }));
        const size_t presentWithName =
            static_cast<size_t>(std::count(present.begin(), present.end(), port->name));
        if (openWithName > presentWithName) {
            closePort(*port);
            closed.push_back(port->name);
            Logger::warning("MIDI: '{}' was disconnected", port->name);
        }
    }
    return closed;
}

}  // namespace quadmapper::control
