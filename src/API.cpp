#include "PCH.h"
#include "GhostTrailsAPI.h"
import GhostTrails.Bindings;

extern "C" __declspec(dllexport) void* RequestPluginAPI(GhostTrailsAPI::InterfaceVersion version)
{
    return version == GhostTrailsAPI::InterfaceVersion::V1 ? GhostTrails::Bindings::NativeAPI() : nullptr;
}
