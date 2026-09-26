#pragma once

#include "AudioAttachment.hpp"
#include "core/UUID.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quadmapper {

class AudioDevice;
class AudioMixer;

enum class AudioOutputState {
    Closed,  // not opened yet, or shut down
    Open,
    Failed,  // the device would not open: running silent
    Lost,    // the device went away while open (unplugged, driver reset)
};

// What the UI shows for one source's audio.
enum class SourceAudioStatus {
    None,     // no attachment (not a video source, or not synced yet)
    Probing,  // opening the file
    NoAudio,  // the file has no audio stream
    Ready,
};

/**
 * @brief The app's audio: one output device, one mixer, and the audio of every video
 *
 * ADR-050 §2. Opens the preferred device (empty = the system default). A device that
 * will not open is logged and the app runs silent; one that goes away mid-run
 * (unplugged) is detected by poll() and can be reopened with selectDevice().
 *
 * Attachments are keyed by source id. The app calls sync() for every video source each
 * frame (creating the attachment on first sight) and retainOnly() with the live ids, so
 * a released source's audio stops and is freed, and a source restored by undo (or
 * re-created) gets its audio back.
 *
 * Control thread only.
 */
class AudioEngine {
public:
    struct Options {
        bool nullBackend = false;  // a silent device that still runs callbacks (tests, stress)
    };

    AudioEngine();
    explicit AudioEngine(Options options);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // --- Output device ---

    /** Open (or reopen) the device named `deviceName` ("" = system default) and start it. */
    bool open(const std::string& deviceName);

    /** Reopen on another device (or the same one, to recover after it was lost). */
    bool selectDevice(const std::string& deviceName) { return open(deviceName); }

    /** Names of the playback devices present now (refreshes the list). */
    std::vector<std::string> listDevices();

    /** The device asked for ("" = system default), whether or not it is open. */
    const std::string& preferredDevice() const { return preferredDevice_; }
    /** Name of the device actually playing, or empty. */
    std::string activeDeviceName() const;
    AudioOutputState outputState() const { return outputState_; }

    /** Once per frame: notices a lost device. */
    void poll();

    // --- Master ---

    float masterVolume() const;
    void setMasterVolume(float volume);
    bool masterMuted() const;
    void setMasterMuted(bool muted);

    // --- Per-source audio ---

    /** Keep the audio of video source `id` (file `path`) on `transport`. */
    void sync(const UUID& id, const std::filesystem::path& path, const AudioTransport& transport);

    /** Release every attachment whose id is not in `live`. */
    void retainOnly(const std::unordered_set<UUID>& live);

    SourceAudioStatus status(const UUID& id) const;
    const AudioAttachment* attachment(const UUID& id) const;
    size_t attachmentCount() const { return attachments_.size(); }

    AudioMixer& mixer() { return *mixer_; }

private:
    Options options_;
    std::unique_ptr<AudioDevice> device_;
    std::unique_ptr<AudioMixer> mixer_;
    std::unordered_map<UUID, std::unique_ptr<AudioAttachment>> attachments_;
    std::string preferredDevice_;
    AudioOutputState outputState_ = AudioOutputState::Closed;
};

}  // namespace quadmapper
