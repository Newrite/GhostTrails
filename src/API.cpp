#include "PCH.h"
#include "GhostTrailsAPI.h"
import GhostTrails.Bindings;

extern "C" __declspec(dllexport) void* RequestPluginAPI(GhostTrailsAPI::InterfaceVersion version)
{
#ifdef GHOSTTRAILS_DIAGNOSTICS
    logger::debug("Native API requested: version={} bound={}", static_cast<std::uint32_t>(version),
        GhostTrails::Bindings::NativeAPI() != nullptr);
#endif
    return version == GhostTrailsAPI::InterfaceVersion::V1 ? GhostTrails::Bindings::NativeAPI() : nullptr;
}
