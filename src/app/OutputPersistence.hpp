#pragma once

#include "output/OutputWindow.hpp"
#include "project/Project.hpp"

namespace quadmapper {

// Save writes an output's DESIRED monitor (QM-090), never the monitor it resolves
// to right now: a fallback taken while the projector was unplugged must not
// replace the projector's identity in the .qmap.
inline Project::OutputData outputToData(const OutputWindow& output) {
    Project::OutputData data;
    data.id = output.getId();
    data.name = output.getName();
    const MonitorIdentity& desired = output.getDesiredMonitor();
    data.monitor = desired.savedIndex;
    data.monitorName = desired.name;
    data.monitorPosition = desired.position;
    data.resolution = output.getResolution();
    data.rotationDegrees = output.getRotationDegrees();
    data.enabled = output.isEnabled();
    data.projector = output.getProjector();
    data.calibrationPoints = output.getCalibrationPoints();
    return data;
}

// Load sets the desired monitor only; OutputManager::resolveOutputs picks the
// connected display.
inline void applyOutputData(const Project::OutputData& data, OutputWindow& output) {
    output.setName(data.name);
    output.setDesiredMonitor({data.monitorName, data.monitorPosition, data.monitor});
    output.setResolution(data.resolution);
    output.setRotationDegrees(data.rotationDegrees);
    output.setEnabled(data.enabled);
    output.getProjector() = data.projector;
    output.getCalibrationPoints() = data.calibrationPoints;
}

}  // namespace quadmapper
