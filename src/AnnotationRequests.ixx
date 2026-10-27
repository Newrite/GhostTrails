module;

#define GHOSTTRAILS_NO_WINDOWS_HELPER
#include "GhostTrailsAPI.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module GhostTrails.AnnotationRequests;
import GhostTrails.Requests;
import GhostTrails.AnnotationCommands;

export namespace GhostTrails::Annotations
{
    enum class Action { Spawn, Prepare, Color, Stop };

    std::string_view Describe(Action action)
    {
        switch (action) {
        case Action::Spawn: return "Spawn";
        case Action::Prepare: return "Prepare";
        case Action::Color: return "Color";
        case Action::Stop: return "Stop";
        }
        return "unknown action";
    }

    struct Outcome
    {
        API::ActorHandle actor;
        std::string channel;
        Action action;
        API::Result result;
        API::RequestID id;
    };

    // Animation threads enqueue owned values. Only the frame owner touches
    // channels, retained request IDs, and their lifetime countdowns.
    class RequestOwner
    {
    public:
        explicit RequestOwner(Requests& requests, std::uint32_t capacity)
            : requests_(requests), capacity_(capacity)
        {
            pending_.reserve(capacity);
            draining_.reserve(capacity);
            owned_.reserve(capacity);
        }

        API::Result Enqueue(API::ActorHandle actor, Command command)
        {
            if (!actor) return API::Result::InvalidArgument;
            const auto session = requests_.Session();
            if (!session.playable) return API::Result::NotReady;
            std::scoped_lock lock(mutex_);
            if (pending_.size() == capacity_) return API::Result::Capacity;
            pending_.push_back({actor, session.epoch, std::move(command)});
            return API::Result::Accepted;
        }

        void Reset()
        {
            {
                std::scoped_lock lock(mutex_);
                pending_.clear();
            }
            for (const auto& record : owned_) requests_.Release(record.id);
            owned_.clear();
            draining_.clear();
        }

        std::vector<Outcome> Tick(float delta)
        {
            Retire(std::isfinite(delta) && delta > 0 ? delta : 0);
            draining_.clear();
            {
                std::scoped_lock lock(mutex_);
                draining_.swap(pending_);
            }
            const auto session = requests_.Session();
            std::vector<Outcome> outcomes;
            outcomes.reserve(draining_.size());
            for (auto& pending : draining_) {
                if (pending.epoch != session.epoch || !session.playable) continue;
                outcomes.push_back(Apply(pending.actor, pending.command));
            }
            return outcomes;
        }

    private:
        struct Pending
        {
            API::ActorHandle actor;
            std::uint64_t epoch;
            Command command;
        };

        struct Owned
        {
            API::ActorHandle actor;
            std::string channel;
            API::RequestID id;
            API::Appearance look;
            float lifetime;
            // Absent while emissions are pending. Present once terminal;
            // retain successful spawn requests until their last ghost fades.
            std::optional<float> remaining;
            Action action;
        };

        void Retire(float delta)
        {
            std::erase_if(owned_, [&](Owned& record) {
                API::Status status;
                if (requests_.GetStatus(record.id, &status) != API::Result::Accepted) return true;
                if (!Terminal(status.state)) return false;
                if (record.action == Action::Prepare || status.spawned == 0) {
                    requests_.Release(record.id);
                    return true;
                }
                if (!record.remaining) record.remaining = record.lifetime;
                else *record.remaining -= delta;
                if (*record.remaining > 0) return false;
                requests_.Release(record.id);
                return true;
            });
        }

        auto Find(API::ActorHandle actor, const std::string& channel)
        {
            return std::ranges::find_if(owned_, [&](const Owned& record) {
                return record.action == Action::Spawn && record.actor == actor && record.channel == channel;
            });
        }

        Outcome Apply(API::ActorHandle actor, const Command& command)
        {
            return std::visit([&](const auto& operation) {
                return Apply(actor, command.channel, operation);
            }, command.operation);
        }

        Outcome Apply(API::ActorHandle actor, const std::string& channel, const Stop&)
        {
            const auto record = Find(actor, channel);
            if (record == owned_.end()) return {actor, channel, Action::Stop, API::Result::NotFound, 0};
            const auto id = record->id;
            const auto result = requests_.Release(id);
            owned_.erase(record);
            return {actor, channel, Action::Stop, result, id};
        }

        Outcome Apply(API::ActorHandle actor, const std::string& channel, const Color& color)
        {
            const auto record = Find(actor, channel);
            if (record == owned_.end()) return {actor, channel, Action::Color, API::Result::NotFound, 0};
            const auto look = color.look.Apply(record->look);
            const auto result = requests_.SetAppearance(record->id, &look);
            if (result == API::Result::Accepted) record->look = look;
            return {actor, channel, Action::Color, result, record->id};
        }

        Outcome Apply(API::ActorHandle actor, const std::string& channel, const Prepare& prepare)
        {
            if (owned_.size() == capacity_) return {actor, channel, Action::Prepare, API::Result::Capacity, 0};
            API::RequestID id{};
            const auto result = requests_.Prepare(actor, prepare.slots, &id);
            if (result == API::Result::Accepted)
                owned_.push_back({actor, channel, id, {}, 0, std::nullopt, Action::Prepare});
            return {actor, channel, Action::Prepare, result, id};
        }

        Outcome Apply(API::ActorHandle actor, const std::string& channel, const Spawn& spawn)
        {
            const auto previous = Find(actor, channel);
            if (owned_.size() == capacity_ && previous == owned_.end())
                return {actor, channel, Action::Spawn, API::Result::Capacity, 0};

            auto options = spawn.options;
            options.appearance = spawn.look.Apply(requests_.GetDefaultAppearance());
            API::RequestID id{};
            const auto result = requests_.Spawn(actor, &options, &id);
            if (result == API::Result::Accepted) {
                // Keep the old channel intact if new admission failed.
                if (previous != owned_.end()) {
                    requests_.Release(previous->id);
                    owned_.erase(previous);
                }
                owned_.push_back({actor, channel, id, options.appearance, options.lifetime,
                    std::nullopt, Action::Spawn});
            }
            return {actor, channel, Action::Spawn, result, id};
        }

        Requests& requests_;
        std::uint32_t capacity_;
        std::mutex mutex_;
        std::vector<Pending> pending_;
        std::vector<Pending> draining_;
        std::vector<Owned> owned_;
    };
}
