#pragma once
#include <windows.h>
#include <cstdint>

namespace GlassFg
{
// Bounded synchronous diagnostics only. No production draw/history caller.
// Only nonvolatile registers, RIP and RSP describe the recovered caller.
// Volatile registers cannot be reconstructed by Windows unwind metadata.
__declspec(noinline) inline bool DiagnosticCallerContext(
    std::uint64_t returnAddress, CONTEXT& result, unsigned& steps) noexcept
{
    result = {}; steps = 0;
    if (!returnAddress) return false;
    CONTEXT context {};
    RtlCaptureContext(&context);
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    __try
    {
        for (; steps < 8; ++steps)
        {
            if (context.Rsp < low || context.Rsp > high - sizeof(std::uint64_t)) return false;
            if (context.Rip == returnAddress) { result = context; return true; }
            if (!context.Rip) return false;
            const auto oldStack = context.Rsp;
            DWORD64 base = 0, establisher = 0;
            auto* function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
            if (function)
            {
                void* handlerData = nullptr;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function,
                                 &context, &handlerData, &establisher, nullptr);
            }
            else
            {
                context.Rip = *reinterpret_cast<const std::uint64_t*>(context.Rsp);
                context.Rsp += sizeof(std::uint64_t);
            }
            if (context.Rsp <= oldStack || context.Rsp > high) return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}
} // namespace GlassFg
