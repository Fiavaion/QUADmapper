// The app's actions (ADR-051 §3): the ones that need the application's services, put
// in the registry with the edit.* actions. Their ids, labels and chords come from
// makeAppAction (editor/EditActions), so every chord is defined in one place.

#include "Application.hpp"
#include "ProjectSession.hpp"
#include "../editor/ActionRegistry.hpp"
#include "../editor/EditActions.hpp"
#include "../editor/EditorState.hpp"
#include "../editor/PerformLock.hpp"
#include "../editor/SceneTakes.hpp"
#include "../output/OutputManager.hpp"
#include "../ui/dialogs/PreferencesDialog.hpp"

#include <cassert>
#include <string>
#include <utility>

namespace quadmapper {

void Application::registerActions() {
    m_actions = std::make_unique<ActionRegistry>();
    ActionRegistry& registry = *m_actions;

    // Every id is an app action with a distinct default chord: a failure is a bug.
    const auto put = [&registry](const std::string& id, std::function<void(EditorContext&)> run,
                                 std::function<bool(const EditorContext&)> enabled = {}) {
        std::optional<Action> action = makeAppAction(id, std::move(run));
        assert(action);
        if (!action) return;
        action->enabled = std::move(enabled);
        const AddResult added = registry.add(std::move(*action));
        assert(added == AddResult::Added);
        (void)added;
    };
    const auto hasOutputs = [](const EditorContext& ctx) { return ctx.outputManager != nullptr; };

    // In the order the old shortcut block ran them, since several chords can fire in
    // one frame: Save, the edit keys, Preferences, Blackout, F11, Esc.
    put(action_id::SAVE, [this](EditorContext&) { saveProject(); });
    registerFileActions(registry);  // New, Open, Save As

    registerEditActions(registry, [this]() { m_session->invalidateUnsavedLabel(); });

    put(action_id::PREFERENCES,
        [](EditorContext& ctx) { ctx.es.requestPanelOpen(PreferencesDialog::ID); });

    // Blackout (ADR-048 §3), the panic key.
    put(action_id::BLACKOUT, [](EditorContext& ctx) { ctx.outputManager->toggleBlackout(); },
        hasOutputs);

    // F11 toggles perform mode and Esc leaves it, even with a popup open: the way out of
    // fullscreen must never take two presses (the Show guard). Esc on a session modal
    // answers only the modal (editor/PerformLock).
    put(action_id::PERFORM_TOGGLE, performToggle, hasOutputs);
    put(action_id::PERFORM_ESCAPE, performEscape);

    // The perform lock (ADR-051 §4): the toggle, and the policy that refuses the Edit
    // actions while it is on. Show-class actions (takes, blackout, F11, Esc) and Save
    // stay allowed.
    put(action_id::PERFORM_LOCK, togglePerformLock);
    installPerformLockPolicy(registry, *m_editorState);

    // Scene takes 1-9 (US-008): no chord. runSceneTakes() invokes them for the queued
    // physical key presses (Source::Key) and OSC/MIDI takes (Source::Control, with the
    // controller's fade in SceneTakeQueue::controlFade).
    for (int n = 1; n <= 9; ++n) {
        put(sceneTakeId(n), [this, n](EditorContext&) { takeSceneByNumber(n, m_sceneTakes->controlFade); },
            hasOutputs);
    }

    // The user's rebound keys (Edit > Preferences > Keyboard), over the defaults above.
    applyKeyBindings(registry, m_preferences->getPreferences().keyBindings);
}

}  // namespace quadmapper
