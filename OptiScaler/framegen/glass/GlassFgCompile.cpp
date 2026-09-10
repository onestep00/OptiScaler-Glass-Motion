#include "pch.h"
#include "GlassFgPass.h"
#include "CyberpunkSurfacePass.h"
#include "SurfaceQueueLink.h"
#include "SurfaceSnapshotPool.h"
#include <nvsdk_ngx_params.h>

// Compile the NGX adapter against the same MSVC ABI and SDK as the native host.
// Runtime registration follows after surface snapshot ownership is connected.
template class GlassFg::ScopedInputs<NVSDK_NGX_Parameter>;
template bool GlassFg::Inputs::read<NVSDK_NGX_Parameter>(NVSDK_NGX_Parameter*, GlassFg::Inputs&);
