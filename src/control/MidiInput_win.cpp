#include "MidiInput.hpp"

#include "MidiDecode.hpp"
#include "core/Logger.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace quadmapper::control {

struct MidiInput::Port {
    HMIDIIN handle = nullptr;
    std::string name;
    DeviceName fixedName{};           // copied into every event without allocating
    ControlEventQueue* queue = nullptr;
    std::atomic<uint64_t>* received = nullptr;
    std::atomic<bool> open{false};
    std::atomic<int> inCallback{0};
};

namespace {

std::string toUtf8(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return std::string();
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string mmErrorText(MMRESULT result) {
    char text[MAXERRORLENGTH] = {};
    if (midiInGetErrorTextA(result, text, MAXERRORLENGTH) == MMSYSERR_NOERROR && text[0]) {
        return text;
    }
    return "WinMM error " + std::to_string(result);
}

void CALLBACK midiInCallback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    auto* port = reinterpret_cast<MidiInput::Port*>(instance);
    if (!port) return;
    port->inCallback.fetch_add(1, std::memory_order_acq_rel);
    if (message == MIM_DATA && port->open.load(std::memory_order_acquire)) {
        if (auto decoded = decodeMidiShortMessage(static_cast<uint32_t>(param1))) {
            ControlEvent event;
            event.source = ControlEvent::Source::Midi;
            event.midi = *decoded;
            event.device = port->fixedName;
            port->queue->push(std::move(event));
            port->received->fetch_add(1, std::memory_order_relaxed);
        }
    }
    // MIM_OPEN / MIM_CLOSE / MIM_ERROR / MIM_LONGDATA (SysEx) are ignored.
    port->inCallback.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace

MidiInput::MidiInput(ControlEventQueue& queue) : queue_(queue) {}

MidiInput::~MidiInput() {
    closeAll();
}

std::vector<std::string> MidiInput::listDevices() {
    std::vector<std::string> names;
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            names.push_back(toUtf8(caps.szPname));
        }
    }
    return names;
}

Result<void, Error> MidiInput::open(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t alreadyOpen = 0;  // same-named devices already open (two identical controllers)
    for (const auto& port : ports_) {
        if (port->open && port->name == name) ++alreadyOpen;
    }

    const UINT count = midiInGetNumDevs();
    size_t seen = 0;
    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        if (toUtf8(caps.szPname) != name) continue;
        if (seen++ < alreadyOpen) continue;

        auto port = std::make_unique<Port>();
        port->name = name;
        const size_t n = std::min(name.size(), port->fixedName.size() - 1);
        std::memcpy(port->fixedName.data(), name.data(), n);
        port->queue = &queue_;
        port->received = &received_;
        port->open = true;  // set before midiInStart so the first message is not lost

        HMIDIIN handle = nullptr;
        MMRESULT result = midiInOpen(&handle, i, reinterpret_cast<DWORD_PTR>(&midiInCallback),
                                     reinterpret_cast<DWORD_PTR>(port.get()), CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) {
            const std::string message = "cannot open MIDI device '" + name + "': " + mmErrorText(result);
            Logger::warning("MIDI: {}", message);
            return Result<void, Error>::error(Error(message));
        }
        port->handle = handle;
        result = midiInStart(handle);
        if (result != MMSYSERR_NOERROR) {
            const std::string message = "cannot start MIDI device '" + name + "': " + mmErrorText(result);
            ports_.push_back(std::move(port));
            closePort(*ports_.back());
            Logger::warning("MIDI: {}", message);
            return Result<void, Error>::error(Error(message));
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
    if (port.handle) {
        // Errors are expected for an unplugged device and change nothing here.
        midiInStop(port.handle);
        midiInReset(port.handle);
        midiInClose(port.handle);
        port.handle = nullptr;
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
        if (port->open || port->handle) closePort(*port);
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
