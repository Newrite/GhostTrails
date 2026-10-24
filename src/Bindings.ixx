module;

#include "PCH.h"
#include "GhostTrailsAPI.h"

export module GhostTrails.Bindings;
import GhostTrails.Requests;

namespace GhostTrails::Bindings
{
    // Borrowed only by the foreign-function adapters, bound once by composition.
    // The plugin and its API remain loaded for the process lifetime.
    GhostTrails::Requests* requests{};

    std::int32_t Spawn(RE::StaticFunctionTag*, RE::Actor* actor, std::int32_t count,
        float interval, float lifetime, float red, float green, float blue,
        float alpha, float intensity, float rimPower, float retryWindow, float spawnWindow)
    {
        if (!requests || !actor || count <= 0) return -static_cast<std::int32_t>(GhostTrailsAPI::Result::InvalidArgument);
        GhostTrailsAPI::SpawnOptions options;
        options.count = static_cast<std::uint32_t>(count);
        options.interval = interval;
        options.lifetime = lifetime;
        options.retryWindow = retryWindow;
        options.spawnWindow = spawnWindow;
        if (red == -1 && green == -1 && blue == -1) {
            options.appearance = requests->GetDefaultAppearance();
        } else {
            options.appearance = {red, green, blue, alpha, intensity, rimPower};
        }
        GhostTrailsAPI::RequestID id{};
        const auto result = requests->Spawn(actor->GetHandle().native_handle(), &options, &id);
        return result == GhostTrailsAPI::Result::Accepted ? static_cast<std::int32_t>(id) : -static_cast<std::int32_t>(result);
    }

    std::int32_t Prepare(RE::StaticFunctionTag*, RE::Actor* actor, std::int32_t slots)
    {
        if (!requests || !actor || slots <= 0) return -static_cast<std::int32_t>(GhostTrailsAPI::Result::InvalidArgument);
        GhostTrailsAPI::RequestID id{};
        const auto result = requests->Prepare(actor->GetHandle().native_handle(), static_cast<std::uint32_t>(slots), &id);
        return result == GhostTrailsAPI::Result::Accepted ? static_cast<std::int32_t>(id) : -static_cast<std::int32_t>(result);
    }

    std::int32_t GetRequestState(RE::StaticFunctionTag*, std::int32_t id)
    {
        GhostTrailsAPI::Status status;
        const auto result = requests->GetStatus(static_cast<std::uint32_t>(id), &status);
        return result == GhostTrailsAPI::Result::Accepted ? static_cast<std::int32_t>(status.state) : -static_cast<std::int32_t>(result);
    }

    std::vector<std::int32_t> GetStatus(RE::StaticFunctionTag*, std::int32_t id)
    {
        GhostTrailsAPI::Status status;
        const auto result = requests->GetStatus(static_cast<std::uint32_t>(id), &status);
        // Error has its own first field; it is never disguised as an empty array.
        if (result != GhostTrailsAPI::Result::Accepted) return {-static_cast<std::int32_t>(result)};
        return {static_cast<std::int32_t>(status.state), static_cast<std::int32_t>(status.lastFailure),
            static_cast<std::int32_t>(status.spawned), static_cast<std::int32_t>(status.missed),
            static_cast<std::int32_t>(std::min(status.attempts, static_cast<std::uint32_t>(INT32_MAX))),
            static_cast<std::int32_t>(status.requested)};
    }

    std::int32_t SetAppearance(RE::StaticFunctionTag*, std::int32_t id,
        float red, float green, float blue, float alpha, float intensity, float rimPower)
    {
        const GhostTrailsAPI::Appearance look{red, green, blue, alpha, intensity, rimPower};
        return static_cast<std::int32_t>(requests->SetAppearance(static_cast<std::uint32_t>(id), &look));
    }

    std::int32_t SetDefaultAppearance(RE::StaticFunctionTag*, float red, float green, float blue,
        float alpha, float intensity, float rimPower)
    {
        const GhostTrailsAPI::Appearance look{red, green, blue, alpha, intensity, rimPower};
        return static_cast<std::int32_t>(requests->SetDefaultAppearance(&look));
    }

    std::int32_t Cancel(RE::StaticFunctionTag*, std::int32_t id)
    {
        return static_cast<std::int32_t>(requests->Cancel(static_cast<std::uint32_t>(id)));
    }

    std::int32_t Release(RE::StaticFunctionTag*, std::int32_t id)
    {
        return static_cast<std::int32_t>(requests->Release(static_cast<std::uint32_t>(id)));
    }
}

export namespace GhostTrails::Bindings
{
    void Bind(GhostTrails::Requests& owner) { requests = &owner; }
    GhostTrailsAPI::IVGhostTrails1* NativeAPI() { return requests; }

    bool Register(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm || !requests) return false;
        vm->RegisterFunction("Spawn", "GhostTrails", Spawn);
        vm->RegisterFunction("Prepare", "GhostTrails", Prepare);
        vm->RegisterFunction("GetRequestState", "GhostTrails", GetRequestState);
        vm->RegisterFunction("GetStatus", "GhostTrails", GetStatus);
        vm->RegisterFunction("SetAppearance", "GhostTrails", SetAppearance);
        vm->RegisterFunction("SetDefaultAppearance", "GhostTrails", SetDefaultAppearance);
        vm->RegisterFunction("Cancel", "GhostTrails", Cancel);
        vm->RegisterFunction("Release", "GhostTrails", Release);
        logger::info("Registered GhostTrails Papyrus API");
        return true;
    }
}
