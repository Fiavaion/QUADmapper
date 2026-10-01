#pragma once

#include "EditorState.hpp"
#include "PerformLock.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "scene/ProjectorCamera.hpp"
#include "scene/Scene.hpp"

namespace quadmapper {

// Keep each output's projector camera matched to what it shows now (ADR-052): its
// current rotation-aware size and, while uncalibrated, framed on its scene's objects.
// Every render and calibration path then reads a camera that is already right. Run at
// the frame boundary and after a scene take.
//
// Under the perform lock (ADR-051 §4, TD-36) an output keeps the framing of the scene
// it already shows: an OBJ import landing after locking changes that scene's bounds,
// and re-framing on them would move every 3D object on air. A take onto another scene
// still frames the scene taken, and after the unlock the framing follows the bounds
// again.
inline void fitProjectorsToOutputs(EditorState& es, OutputManager& om) {
    om.followOutputSizes(es.scenes);
    for (const auto& output : om.getOutputs()) {
        if (!output) continue;
        const IVec2 size = output->getEffectiveResolution();
        const Scene* scene = OutputManager::sceneForOutput(es.scenes, *output);
        const UUID sceneId = scene ? scene->getId() : UUID();
        UUID& framedScene = es.projectorFramedScenes[output->getId()];
        const bool keepFraming = es.performLock.isOn() && framedScene == sceneId;
        framedScene = sceneId;
        Vec3 lo, hi;
        const bool framed = !keepFraming && scene && scene->visibleObjectBounds(lo, hi);
        output->getProjector() = ProjectorCamera::fitToOutput(
            output->getProjector(), size.x, size.y, framed ? &lo : nullptr, framed ? &hi : nullptr);
    }
}

}  // namespace quadmapper
