#pragma once
#include "ExperimentDrawAbi.h"

enum GlassExperimentCaptureStage
{
    GlassCapturePrepare = 1,
    GlassCaptureRecorded = 2,
    GlassCaptureRetired = 3
};
struct GlassExperimentPreparedCapture
{
    uint32_t size, reserved;
    void* pipeline; // module-owned, matches supplied extended root
    uint32_t history[8];
    uint64_t previous, current, material, capture, mapping;
};
struct GlassExperimentCaptureInput
{
    uint32_t size, stage;
    uint64_t job, recording;
    void* command;
    const GlassExperimentDrawInput* draw; // Prepare only, callback-scoped
    GlassExperimentPreparedCapture* output; // Prepare only
    uint32_t recorded, reserved;
};
// Prepare: no GPU commands, waits or resource mutation; 1 reserves output,
// 0 declines. Recorded: callback on the original command after host restoration,
// permits module-owned capture transitions/copies only when recorded=1.
// Retired: control-thread notification after completion AND recording discard;
// command/draw/output are null. May release/reuse this job's module resources.
// Capture callbacks may run on different threads but the owner serializes them.
