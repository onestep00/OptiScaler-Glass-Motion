#pragma once
#include <cstdint>

namespace GlassFg
{
// One instance per retained COMPUTE command-list identity. The host serializes
// callbacks and observes every listed method on the actual interface. No binding
// state is inferred from FG parameters, a null root signature, or a GPU fence.
class ComputeRecording
{
  public:
    enum Method : uint32_t
    {
        Reset = 1u << 0,
        Close = 1u << 1,
        ClearState = 1u << 2,
        Pipeline = 1u << 3,
        Heaps = 1u << 4,
        RootSignature = 1u << 5,
        RootTable = 1u << 6,
        RootConstant = 1u << 7,
        RootConstants = 1u << 8,
        RootCbv = 1u << 9,
        RootSrv = 1u << 10,
        RootUav = 1u << 11,
        Predication = 1u << 12,
        Indirect = 1u << 13,
        StateObject = 1u << 14,
        Program = 1u << 15
    };
    static constexpr uint32_t BaseMethods = (1u << 14) - 1;
    static constexpr uint32_t AllMethods = (1u << 16) - 1;
    struct Ticket
    {
        uint64_t epoch = 0;
        explicit operator bool() const { return epoch != 0; }
    };

  private:
    const void* command = nullptr;
    uint64_t epoch = 0;
    bool covered = false, fresh = false, reserved = false;

    void invalidate()
    {
        fresh = reserved = false;
        if (++epoch == 0)
            covered = false; // A wrapped epoch must not validate an old ticket.
    }

  public:
    // Include StateObject/Program in supported when the respective extended
    // interface is available. Failed/unknown QueryInterface is not absence.
    // Rebinding always requires a newly observed successful Reset.
    void bind(const void* identity, bool compute, uint32_t supported, uint32_t observed)
    {
        invalidate();
        command = identity;
        covered = command && compute && (supported & BaseMethods) == BaseMethods && (supported & ~AllMethods) == 0 &&
                  (observed & supported) == supported;
    }

    void onReset(const void* identity, bool succeeded, const void* initialPipeline)
    {
        if (identity != command)
            return;
        invalidate();
        fresh = covered && succeeded && !initialPipeline;
    }

    // Invoke for every state setter (even a null/no-op value), ClearState,
    // Close, ExecuteIndirect and state-object/program change. Correction's own
    // commands must be excluded by a strictly scoped host reentrancy guard.
    void onMutation(const void* identity)
    {
        if (identity == command)
            invalidate();
    }

    Ticket begin(const void* identity)
    {
        if (!covered || !fresh || reserved || identity != command)
            return {};
        fresh = false;
        reserved = true;
        return { epoch };
    }

    // True permits exactly one ClearState(nullptr), immediately after our
    // compute commands and before native FG. No caller commands may interleave.
    // This is command binding restoration, not resource-state restoration.
    bool finish(const void* identity, Ticket ticket)
    {
        if (!covered || !reserved || identity != command || !ticket || ticket.epoch != epoch)
            return false;
        reserved = false;
        return true;
    }
};
} // namespace GlassFg
