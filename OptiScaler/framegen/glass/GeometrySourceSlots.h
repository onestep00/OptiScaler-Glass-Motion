#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace GlassFg
{
// CPU-only handoff for one externally ordered producer/consumer domain.
// The caller must prove view/submission scope and owner/array generations.
// This class neither discovers those generations nor makes indices persistent.
// No concurrent access: publish finishes before consumers start resolving.
template <std::size_t Capacity> class GeometrySourceSlots
{
  public:
    struct Source
    {
        std::uint64_t proxy = 0, mesh = 0;
        std::uint32_t ownerGeneration = 0, arrayGeneration = 0, index = 0;
        bool operator==(const Source&) const = default;
        explicit operator bool() const { return proxy && mesh && ownerGeneration && arrayGeneration; }
    };
    struct Ticket
    {
        std::uint64_t epoch = 0, view = 0;
        std::uint32_t frame = 0;
        bool operator==(const Ticket&) const = default;
    };

  private:
    struct Entry { Source source {}; std::uint64_t epoch = 0; bool conflict = false; };
    std::array<Entry, Capacity> entries {};
    Ticket current {};
    bool sealed = false;

  public:
    // A fresh epoch also distinguishes two submissions of the same view/frame.
    Ticket begin(std::uint64_t view, std::uint32_t frame)
    {
        if (++current.epoch == 0)
        {
            for (auto& entry : entries) entry = {};
            current.epoch = 1;
        }
        current.view = view; current.frame = frame; sealed = false;
        return current;
    }
    bool publish(Ticket ticket, std::uint32_t slot, Source source)
    {
        if (sealed || ticket != current || !ticket.view || !ticket.frame || slot >= Capacity) return false;
        auto& entry = entries[slot];
        if (entry.epoch != current.epoch) entry = {source, current.epoch, !source};
        else if (!source || entry.source != source) entry.conflict = true;
        return !entry.conflict;
    }
    bool seal(Ticket ticket)
    {
        if (ticket != current || !ticket.view || !ticket.frame) return false;
        sealed = true; return true;
    }
    Source resolve(Ticket ticket, std::uint32_t slot) const
    {
        if (!sealed || ticket != current || !ticket.view || !ticket.frame || slot >= Capacity) return {};
        const auto& entry = entries[slot];
        return entry.epoch == current.epoch && !entry.conflict ? entry.source : Source {};
    }
};
} // namespace GlassFg
