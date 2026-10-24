module;

#define GHOSTTRAILS_NO_WINDOWS_HELPER
#include "GhostTrailsAPI.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

export module GhostTrails.Requests;

export namespace GhostTrails
{
    namespace API = GhostTrailsAPI;

    struct Limits
    {
        std::uint32_t requests{256};
        std::uint32_t actors{16};
        std::uint32_t slotsPerActor{16};
        std::uint32_t totalSlots{64};
        std::uint32_t clonesPerFrame{1};
        float idleSeconds{10.0f};
    };

    [[nodiscard]] bool Valid(const API::Appearance& look)
    {
        const auto unit = [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; };
        const auto positive = [](float value) { return std::isfinite(value) && value >= 0; };
        return unit(look.red) && unit(look.green) && unit(look.blue) && unit(look.alpha) &&
               positive(look.intensity) && positive(look.rimPower);
    }

    [[nodiscard]] bool Terminal(API::State state)
    {
        return state != API::State::Queued && state != API::State::Running;
    }

    struct Request
    {
        API::RequestID id{};
        API::ActorHandle actor{};
        API::SpawnOptions options{};
        API::Status status{};
        bool prepare{};
        bool canceled{};
        bool released{};
    };

    struct SessionSnapshot
    {
        std::uint64_t epoch;
        bool playable;
    };

    // The bounded admission mailbox owns statuses and cross-thread intent. It never
    // resolves an actor or calls the engine. Runtime owns the resulting side effects.
    class Requests final : public API::IVGhostTrails1
    {
    public:
        explicit Requests(Limits limits) : limits_(limits) { records_.reserve(limits.requests); }

        API::Result Spawn(API::ActorHandle actor, const API::SpawnOptions* options, API::RequestID* id) override
        {
            if (!options || !id || !actor || !Valid(options->appearance) || !options->count ||
                !std::isfinite(options->interval) || options->interval < 0 ||
                !std::isfinite(options->lifetime) || options->lifetime <= 0 ||
                !std::isfinite(options->retryWindow) || options->retryWindow < 0 ||
                !std::isfinite(options->spawnWindow) || options->spawnWindow < 0 ||
                options->count > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
                if (id) *id = 0;
                return API::Result::InvalidArgument;
            }
            return Admit(actor, *options, false, id);
        }

        API::Result Prepare(API::ActorHandle actor, std::uint32_t slots, API::RequestID* id) override
        {
            if (!id || !actor || !slots || slots > limits_.slotsPerActor) {
                if (id) *id = 0;
                return API::Result::InvalidArgument;
            }
            API::SpawnOptions options;
            options.count = slots;
            return Admit(actor, options, true, id);
        }

        API::Result GetStatus(API::RequestID id, API::Status* status) const override
        {
            if (!status) return API::Result::InvalidArgument;
            std::scoped_lock lock(mutex_);
            const auto it = Find(id);
            if (it == records_.end() || it->released) return API::Result::NotFound;
            *status = it->status;
            return API::Result::Accepted;
        }

        API::Result SetAppearance(API::RequestID id, const API::Appearance* look) override
        {
            if (!look || !Valid(*look)) return API::Result::InvalidArgument;
            std::scoped_lock lock(mutex_);
            const auto it = Find(id);
            if (it == records_.end() || it->released) return API::Result::NotFound;
            if (Terminal(it->status.state) || it->canceled) return API::Result::Finished;
            it->options.appearance = *look;
            return API::Result::Accepted;
        }

        API::Result Cancel(API::RequestID id) override { return Stop(id, false); }
        API::Result Release(API::RequestID id) override { return Stop(id, true); }

        API::Result SetDefaultAppearance(const API::Appearance* look) override
        {
            if (!look || !Valid(*look)) return API::Result::InvalidArgument;
            std::scoped_lock lock(mutex_);
            defaults_ = *look;
            return API::Result::Accepted;
        }

        API::Appearance GetDefaultAppearance() const override
        {
            std::scoped_lock lock(mutex_);
            return defaults_;
        }

        // Bounded value copies cross the ownership boundary, no aliases escape.
        std::vector<Request> Snapshot() const
        {
            std::scoped_lock lock(mutex_);
            return records_;
        }

        void Publish(API::RequestID id, API::Status status)
        {
            std::scoped_lock lock(mutex_);
            const auto it = Find(id);
            if (it != records_.end() && !it->canceled && !it->released) it->status = status;
        }

        void AcknowledgeStop(API::RequestID id)
        {
            std::scoped_lock lock(mutex_);
            const auto it = Find(id);
            if (it == records_.end()) return;
            if (it->released) records_.erase(it);
            else {
                it->status.state = API::State::Canceled;
                it->canceled = false;
            }
        }

        // Lifecycle messages and admission use the same synchronization owner.
        // An old frame may never reopen admission after a concurrent load/reset.
        void ChangeSession(bool playable, bool reset)
        {
            std::scoped_lock lock(mutex_);
            ready_ = false;
            playable_ = playable;
            ++epoch_;
            // Session IDs are never recycled, including across loads.
            if (reset) records_.clear();
        }

        SessionSnapshot Session() const
        {
            std::scoped_lock lock(mutex_);
            return {epoch_, playable_};
        }

        bool Open(std::uint64_t expectedEpoch, bool available = true)
        {
            std::scoped_lock lock(mutex_);
            if (epoch_ != expectedEpoch || !playable_) return false;
            ready_ = available;
            return ready_;
        }

    private:
        std::vector<Request>::iterator Find(API::RequestID id) { return std::ranges::find(records_, id, &Request::id); }
        std::vector<Request>::const_iterator Find(API::RequestID id) const { return std::ranges::find(records_, id, &Request::id); }

        API::Result Admit(API::ActorHandle actor, API::SpawnOptions options, bool prepare, API::RequestID* id)
        {
            *id = 0;
            std::scoped_lock lock(mutex_);
            if (!ready_) return API::Result::NotReady;
            if (records_.size() == limits_.requests || nextID_ > std::numeric_limits<std::int32_t>::max())
                return API::Result::Capacity;

            Request request;
            request.id = static_cast<API::RequestID>(nextID_++);
            request.actor = actor;
            request.options = options;
            request.prepare = prepare;
            request.status.requested = options.count;
            records_.push_back(request);
            *id = request.id;
            return API::Result::Accepted;
        }

        API::Result Stop(API::RequestID id, bool release)
        {
            std::scoped_lock lock(mutex_);
            const auto it = Find(id);
            if (it == records_.end() || it->released) return API::Result::NotFound;
            it->canceled = true;
            it->released = release;
            return API::Result::Accepted;
        }

        Limits limits_;
        mutable std::mutex mutex_;
        std::vector<Request> records_;
        API::Appearance defaults_;
        std::uint64_t nextID_{1};
        bool ready_{};
        bool playable_{};
        std::uint64_t epoch_{1};
    };
}
