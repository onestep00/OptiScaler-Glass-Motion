#pragma once
#include "ExperimentDrawAbi.h"
inline constexpr uint32_t GLASS_EXPERIMENT_CAPTURE_VERSION = 3; // Nested draw input version 4.
inline constexpr uint32_t GLASS_EXPERIMENT_SUBMISSION_VERSION = 1;

// Optional GlassExperimentSubmission capability, separate from the unchanged
// capture payload. Delivered for each captured recording before its actual
// ExecuteCommandLists occurrence, in command-list order. Scalar observation
// only: never mutate uploads, issue commands or wait. Scalar identity logging
// is allowed; borrowed COM pointers must not be called after this callback.
// This notification alone does not authorize cross-frame resource reuse.
struct GlassExperimentSubmissionInput
{
    uint32_t size, reserved;
    uint64_t job, recording, submission;
    void* command;
    void* queue;
    uint32_t listIndex, listCount;
};

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
// Prepare: no GPU commands, waits or shared-input mutation. May fill exclusively
// reserved upload ranges that have no pending GPU/recording use. 1 reserves output,
// 0 declines. Recorded: callback on the original command after host restoration,
// permits module-owned capture transitions/copies only when recorded=1.
// Retired: control-thread notification after completion AND recording discard;
// command/draw/output are null. May release/reuse this job's module resources.
// Capture callbacks may run on different threads but the owner serializes them.
