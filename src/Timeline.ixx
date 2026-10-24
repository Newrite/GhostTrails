module;

#define GHOSTTRAILS_NO_WINDOWS_HELPER
#include "GhostTrailsAPI.h"
#include <limits>

export module GhostTrails.Timeline;

export namespace GhostTrails
{
    // One finite sequence of emissions. A failed emission has its own retry
    // deadline; a hitch never emits the missed historical poses in a burst.
    class Timeline
    {
    public:
        explicit Timeline(GhostTrailsAPI::SpawnOptions options, double start = 0) : options_(options), start_(start)
        {
            status_.requested = options.count;
            status_.state = GhostTrailsAPI::State::Running;
        }

        [[nodiscard]] bool Due(double now)
        {
            if (!finished_ && options_.spawnWindow > 0 && now - start_ > options_.spawnWindow)
                Fail(GhostTrailsAPI::Failure::WindowExpired);
            return !finished_ && now >= next_;
        }
        [[nodiscard]] GhostTrailsAPI::Status Status() const { return status_; }
        [[nodiscard]] bool Finished() const { return finished_; }

        void Success(double now)
        {
            Attempt();
            ++status_.spawned;
            Advance(now);
        }

        // Waiting for this frame's construction budget is not a failed attempt.
        // Resource pressure / no 3D / actual clone failures do enter this path.
        bool Retry(GhostTrailsAPI::Failure failure, double now)
        {
            Attempt();
            status_.lastFailure = failure;
            if (!retrying_) {
                retrying_ = true;
                deadline_ = now + options_.retryWindow;
            }
            if (now >= deadline_) {
                ++status_.missed;
                Advance(now);
                return false;
            }
            // At most 20 retries/second, with no busy-loop on unloaded 3D.
            next_ = now + 0.05;
            return true;
        }

        void Fail(GhostTrailsAPI::Failure failure)
        {
            status_.lastFailure = failure;
            status_.missed = status_.requested - status_.spawned;
            Finish();
        }

    private:
        void Attempt()
        {
            if (status_.attempts != std::numeric_limits<std::uint32_t>::max()) ++status_.attempts;
        }

        void Advance(double now)
        {
            retrying_ = false;
            next_ = now + options_.interval;
            if (status_.spawned == status_.requested - status_.missed) Finish();
        }

        void Finish()
        {
            finished_ = true;
            status_.state = status_.missed == 0 ? GhostTrailsAPI::State::Completed :
                            status_.spawned != 0 ? GhostTrailsAPI::State::Partial : GhostTrailsAPI::State::Failed;
        }

        GhostTrailsAPI::SpawnOptions options_;
        GhostTrailsAPI::Status status_;
        double next_{};
        double deadline_{};
        double start_{};
        bool retrying_{};
        bool finished_{};
    };
}
