#include "pch.h"
#include "../TaggedInputs.h"
#include <cstdio>
#include <stdexcept>

static void check(bool value)
{
    if (!value)
        throw std::runtime_error("tag metadata contract failed");
}
int main()
{
    using Store = GlassFg::TaggedInputs;
    Store store;
    int feature, other, depth, motion, color;
    const void* native[] = { &motion, &color, &depth };
    const void* tagged[] = { &depth, &motion, &color };
    Store::Tag result[3];
    auto observe = [&](uint64_t frame)
    {
        for (unsigned i = 0; i < 3; ++i)
            store.observe(i, { tagged[i], frame, 0, 0x400, 0, 0, 64, 32, true });
    };
    check(!store.read(&feature, 1, 3, native, result));
    observe(1);
    check(store.read(&feature, 1, 3, native, result));
    check(result[0].resource == &motion && result[1].resource == &color && result[2].resource == &depth);
    check(store.read(&feature, 2, 3, native, result) && store.read(&feature, 3, 3, native, result));
    check(!store.read(&feature, 3, 3, native, result));
    check(!store.read(&feature, 1, 3, native, result)); // Same addresses require fresh tags.
    observe(2);
    check(store.read(&feature, 1, 3, native, result));
    check(!store.read(&other, 2, 3, native, result));
    observe(3);
    store.observe(2, { &color, 2, 0, 0x400, 0, 0, 64, 32, true });
    check(!store.read(&feature, 1, 3, native, result)); // Mixed frames.
    store.observe(2, { &color, 3, 0, 0x400, 0, 0, 64, 32, true });
    check(!store.read(&feature, 1, 3, native, result)); // Failed consumption cannot reuse partial tags.
    for (unsigned bad = 0; bad < 5; ++bad)
    {
        observe(4 + bad);
        Store::Tag t { &color, 4 + bad, 0, 0x400, 0, 0, 64, 32, true };
        if (bad == 0)
            t.viewport = 1;
        if (bad == 1)
            t.left = 1;
        if (bad == 2)
            t.state = 0x80;
        if (bad == 3)
            t.valid = false;
        if (bad == 4)
            t.resource = &other;
        store.observe(2, t);
        check(!store.read(&feature, 1, 3, native, result));
    }
    observe(10);
    check(store.read(&feature, 1, 1, native, result));
    check(!store.read(&feature, 2, 1, native, result));
    observe(11);
    store.clear();
    check(!store.read(&feature, 1, 3, native, result));
    // Resource reuse during MFG must not hide a changed frame/viewport/region.
    for (unsigned changed = 0; changed < 7; ++changed)
    {
        const auto frame = uint64_t(20 + changed);
        observe(frame);
        check(store.read(&feature, 1, 3, native, result));
        Store::Tag tag { &color, frame, 0, 0x400, 0, 0, 64, 32, true };
        switch (changed)
        {
        case 0: ++tag.frame; break;
        case 1: ++tag.viewport; break;
        case 2: ++tag.left; break;
        case 3: ++tag.top; break;
        case 4: ++tag.width; break;
        case 5: ++tag.height; break;
        case 6: tag.valid = false; break;
        }
        store.observe(2, tag);
        check(!store.read(&feature, 2, 3, native, result));
        check(!store.read(&feature, 3, 3, native, result));
    }
    observe(40);
    check(store.read(&feature, 1, 3, native, result));
    observe(40); // Re-reading identical tags is allowed; values must stay frozen.
    check(store.read(&feature, 2, 3, native, result) && result[0].frame == 40);
    check(store.read(&feature, 3, 3, native, result));
    std::puts("TAGGED_INPUTS fresh_batch=pass phases=pass stale_mixed_invalid=pass lifecycle=pass");
}
