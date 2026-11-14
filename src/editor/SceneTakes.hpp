#pragma once

// Live-show scene takes (US-008, ADR-048), out of Application so the perform lock sweep
// runs the code the application runs (TD-36): the queue the key callbacks and OSC/MIDI
// fill, the take itself, and the drain of the OSC/MIDI takes.

#include "ActionRegistry.hpp"
#include "EditActions.hpp"
#include "EditorContext.hpp"
#include "EditorState.hpp"
#include "ProjectorFit.hpp"
#include "StatusLine.hpp"
#include "core/Logger.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "scene/Scene.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace quadmapper {

// The takes asked for since the last frame. Key presses (the editor window, a focused
// output window) only queue the scene number, since GLFW callbacks run inside
// glfwPollEvents; OSC/MIDI takes are kept apart because they skip the typing/popup guard.
struct SceneTakeQueue {
    struct External {
        int number = 0;
        std::optional<float> fadeSeconds;  // nullopt = the Preferences default
    };
    std::vector<int> keys;           // scene numbers pressed since the last frame
    std::vector<External> external;  // OSC/MIDI takes since the last frame
    // The fade of the OSC/MIDI take being invoked; nullopt for a key press.
    std::optional<float> controlFade;

    // New, Load and Restore drop takes still queued: they were pressed for the show that
    // was just replaced and must not put its scene N of the new one on air.
    void clear() {
        keys.clear();
        external.clear();
    }
};

// Put scene `number` on air, fading over `fadeSeconds` (ADR-047 §5); refused while
// calibrating, since calibration edits the scene routed to the calibrated output (the
// status line says so).
inline void takeSceneByNumber(EditorState& es, OutputManager& om, int number, float fadeSeconds) {
    if (!es.calibratingOutputId.isNull()) {
        postStatus(es, "Scene " + std::to_string(number) + " not taken: leave calibration first");
        return;
    }
    const SceneTake take = es.takeScene(number, om, fadeSeconds);
    if (take.kind == SceneTake::Kind::None) return;
    // Takes run inside the editor frame, after the frame-boundary fit and before this
    // frame's projector frames are drawn: re-fit now so those frames are framed for the
    // scene just put on air.
    fitProjectorsToOutputs(es, om);

    const Scene& scene = es.activeScene();
    if (take.kind == SceneTake::Kind::Route) {
        const OutputWindow* output = om.getOutput(take.outputIds.front());
        const std::string where = take.outputIds.size() > 1
                                      ? std::to_string(take.outputIds.size()) + " outputs"
                                      : (output ? output->getName() : std::string("output"));
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Scene %d: %s on %s", number,
                 scene.getName().c_str(), where.c_str());
    } else {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Scene %d: %s", number,
                 scene.getName().c_str());
    }
    es.statusTimer = 3.0f;
}

// The OSC/MIDI takes queued since the last frame, through the scene.take.N actions
// (Source::Control), with each controller's fade in queue.controlFade while it runs.
// They skip the key guard (runSceneTakeKeys): it exists so keys typed into a field or
// behind a modal do not act on the show, and an operator's controller is not typing.
// Calibration still holds them off (takeSceneByNumber), as it does the keys.
inline void runExternalSceneTakes(ActionRegistry& registry, EditorContext& ctx, SceneTakeQueue& queue) {
    for (const SceneTakeQueue::External& t : queue.external) {
        queue.controlFade = t.fadeSeconds;
        registry.invoke(sceneTakeId(t.number), ctx, Source::Control);
        queue.controlFade.reset();
        Logger::info("Control: scene take {} requested; active scene is now '{}'", t.number,
                     ctx.es.activeScene().getName());
    }
    queue.external.clear();
}

}  // namespace quadmapper
