module;

#include "GhostTrailsAPI.h"

export module GhostTrails.ConsumerExample;

export namespace GhostTrailsExample
{
    struct Admission
    {
        GhostTrailsAPI::Result result;
        GhostTrailsAPI::RequestID id;
    };

    // In a game plugin call after kPostLoad and supply actor->GetHandle().native_handle().
    // Store an accepted ID, poll status from your update, Release after its fade.
    Admission Spawn(GhostTrailsAPI::ActorHandle actor)
    {
        auto* api = GhostTrailsAPI::RequestPluginAPI();
        if (!api) return {GhostTrailsAPI::Result::NotReady, 0};

        GhostTrailsAPI::SpawnOptions options;
        options.appearance = api->GetDefaultAppearance();
        options.count = 4;
        options.interval = 0.05f;
        options.lifetime = 0.4f;
        options.spawnWindow = 0.3f;
        GhostTrailsAPI::RequestID id{};
        return {api->Spawn(actor, &options, &id), id};
    }
}
