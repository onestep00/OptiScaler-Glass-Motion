# nvngx.dll_dlssnr.dll

Neural Rendering's snippet resolves the module that owns its caller's return address and refuses
anything whose path does not contain `nvngx.dll` -- the driver core being `_nvngx.dll`. It returns
`FAIL_PlatformError` before it inspects a single argument. OptiScaler installs as `dxgi.dll` or
`winmm.dll`, so it fails that test like anything else would.

This library exists to be named correctly. It does nothing else: it forwards create, evaluate and
release, so the calls into the snippet originate from a module the snippet accepts.

Evaluate has two exports. `dlssnr_call_evaluate_v2` takes the depth and motion-vector subrects
separately, each with its own size and origin. `dlssnr_call_evaluate` is the original call with one
guide extent at the origin; it is a thin wrapper over v2, kept so an OptiScaler build from before v2
-- and the native Vulkan pass, through `dlssnr_vk_evaluate` -- still runs against this DLL.
`dlssnr_call_error` returns the model's last error text on the calling thread.

The prebuilt DLL is committed because it is 12 KB, changes almost never, and has to be in the package
for a drop-in build to work at all. To rebuild it:

    cmake -S . -B build -A x64
    cmake --build build --config Release

One detail is load-bearing and not obvious. The forwarder must not `return snippetFn(...)` -- that is a
tail call, the compiler emits a `jmp`, this module's frame disappears, and the snippet then resolves its
caller to whoever called the forwarder. The result goes into a `volatile` local first.
