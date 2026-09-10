#pragma once
#include <array>
#include <cstdint>

namespace GlassFg
{
// Metadata only. The host owns the resources and synchronization objects.
// Call under the host's queue-tracking lock and reset on feature/queue teardown.
// An observed CPU recording does not become an FG input until its producer
// submission and the matching native GPU Signal/Wait have both been observed.
class SurfaceQueueLink
{
    struct Recording
    {
        const void* command = nullptr;
        uint64_t generation = 0;
    };
    struct FgCommand
    {
        const void* command = nullptr;
        const void* queue = nullptr;
    };
    struct Signal
    {
        const void* fence = nullptr;
        uint64_t value = 0, generation = 0;
    };
    struct Binding
    {
        const void* queue = nullptr;
        uint64_t generation = 0;
    };
    std::array<Recording, 64> recordings {};
    std::array<FgCommand, 16> fgCommands {};
    std::array<Signal, 256> signals {};
    std::array<Binding, 16> bindings {};
    const void* producerQueue = nullptr;
    uint64_t latestRecorded = 0, latestSubmitted = 0;
    unsigned nextSignal = 0;
    bool invalid = false;

    void invalidate() { invalid = true; }

  public:
    bool recordSurface(const void* command, uint64_t generation)
    {
        if (invalid || !command || !generation || generation <= latestRecorded)
            return false;
        for (auto& entry : recordings)
            if (!entry.command || entry.command == command)
            {
                // Re-recording before submission makes the older copy unusable.
                entry = { command, generation };
                latestRecorded = generation;
                return true;
            }
        invalidate();
        return false;
    }

    bool registerFgCommand(const void* command)
    {
        if (invalid || !command)
            return false;
        for (auto& entry : fgCommands)
            if (entry.command == command)
                return true;
        for (auto& entry : fgCommands)
            if (!entry.command)
            {
                entry.command = command;
                return true;
            }
        invalidate();
        return false;
    }

    // A successful Reset discards an unsubmitted recording. Keep the known
    // FG queue binding, but never later submit an abandoned surface generation.
    void resetCommand(const void* command)
    {
        for (auto& entry : recordings)
            if (entry.command == command)
                entry = {};
    }

    void submit(const void* queue, const void* command)
    {
        if (invalid || !queue || !command)
            return;
        for (auto& entry : recordings)
            if (entry.command == command)
            {
                if ((producerQueue && producerQueue != queue) || entry.generation <= latestSubmitted)
                {
                    invalidate();
                    return;
                }
                producerQueue = queue;
                latestSubmitted = entry.generation;
                entry = {};
                break;
            }
        for (auto& entry : fgCommands)
            if (entry.command == command)
            {
                if (entry.queue && entry.queue != queue)
                    invalidate();
                entry.queue = queue;
                break;
            }
    }

    // Call only for successful native synchronization calls. This does not
    // insert a signal or a GPU wait, and it never waits on the CPU.
    void signal(const void* queue, const void* fence, uint64_t value)
    {
        if (invalid || !producerQueue || queue != producerQueue || !fence || !latestSubmitted)
            return;
        signals[nextSignal] = { fence, value, latestSubmitted };
        nextSignal = (nextSignal + 1) % signals.size();
    }

    void wait(const void* queue, const void* fence, uint64_t value)
    {
        if (invalid || !queue || !fence)
            return;
        bool related = false;
        uint64_t generation = 0;
        for (const auto& entry : signals)
            if (entry.fence == fence)
            {
                related = true;
                if (entry.value == value)
                {
                    if (generation && generation != entry.generation)
                    {
                        invalidate(); // Reused fence value has conflicting provenance.
                        return;
                    }
                    generation = entry.generation;
                }
            }
        if (!related)
            return;
        for (auto& entry : bindings)
            if (entry.queue == queue)
            {
                entry.generation = generation;
                return;
            }
        for (auto& entry : bindings)
            if (!entry.queue)
            {
                entry = { queue, generation };
                return;
            }
        invalidate();
    }

    uint64_t generationForFgCommand(const void* command) const
    {
        if (invalid || !command)
            return 0;
        for (const auto& fg : fgCommands)
            if (fg.command == command && fg.queue)
                for (const auto& binding : bindings)
                    if (binding.queue == fg.queue && binding.generation == latestSubmitted &&
                        binding.generation == latestRecorded)
                        return binding.generation;
        // Older or ahead-of-consumer recordings must not substitute another
        // rendered frame's glass. The host invalidates correction history here.
        return 0;
    }

    bool healthy() const { return !invalid; }
    bool isProducerQueue(const void* queue) const { return !invalid && queue && queue == producerQueue; }
};
} // namespace GlassFg
