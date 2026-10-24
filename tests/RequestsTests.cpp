#define GHOSTTRAILS_NO_WINDOWS_HELPER
#include "GhostTrailsAPI.h"
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

import GhostTrails.Requests;
import GhostTrails.Timeline;

namespace API = GhostTrailsAPI;

namespace
{
    int failures{};
    void MakeReady(GhostTrails::Requests& requests)
    {
        requests.ChangeSession(true, false);
        requests.Open(requests.Session().epoch);
    }

    void Check(bool condition, const char* contract)
    {
        if (!condition) {
            std::cerr << "FAIL: " << contract << '\n';
            ++failures;
        }
    }

    void AdmissionAndOwnership()
    {
        GhostTrails::Limits limits;
        limits.requests = 2;
        GhostTrails::Requests requests(limits);
        API::RequestID id{99};
        API::SpawnOptions options;
        Check(requests.Spawn(42, &options, &id) == API::Result::NotReady && id == 0, "not-ready admission has no ID");
        MakeReady(requests);
        Check(requests.Spawn(42, &options, &id) == API::Result::Accepted && id > 0, "ready admission yields an ID");
        const auto first = id;
        options.appearance.red = 0.9f;
        Check(requests.Snapshot().front().options.appearance.red == 0.2f, "admission copies caller-owned settings");
        Check(requests.Prepare(42, 2, &id) == API::Result::Accepted, "prepare is admitted");
        Check(requests.Spawn(42, &options, &id) == API::Result::Capacity && id == 0, "retained results are bounded");

        auto status = API::Status{};
        status.state = API::State::Completed;
        requests.Publish(first, status);
        Check(requests.SetAppearance(first, &options.appearance) == API::Result::Finished, "completed requests reject look changes");
        Check(requests.Release(first) == API::Result::Accepted, "release accepts completed request for ghost removal");
        requests.AcknowledgeStop(first);
        Check(requests.GetStatus(first, &status) == API::Result::NotFound, "released status is unavailable");
        Check(requests.Spawn(42, &options, &id) == API::Result::Accepted && id > first, "release restores capacity without recycling ID");
        const auto beforeReset = id;
        requests.ChangeSession(true, true);
        requests.Open(requests.Session().epoch);
        Check(requests.Spawn(42, &options, &id) == API::Result::Accepted && id > beforeReset, "load reset cannot alias stale IDs");

        const auto oldEpoch = requests.Session().epoch;
        requests.ChangeSession(false, true);
        Check(!requests.Open(oldEpoch), "stale main frame cannot reopen admission after concurrent load");
        requests.ChangeSession(true, false);
        Check(!requests.Open(oldEpoch), "stale frame cannot open a newer playable session either");
        Check(requests.Open(requests.Session().epoch), "current main frame may open the playable session");
        Check(!requests.Open(requests.Session().epoch, false), "loading UI may suspend admission without ending session");
        Check(requests.Spawn(42, &options, &id) == API::Result::NotReady, "UI suspension is visible to producers");
        Check(requests.Open(requests.Session().epoch), "closed loading UI permits reopening current session");
    }

    void InvalidInputs()
    {
        GhostTrails::Requests requests({});
        MakeReady(requests);
        API::SpawnOptions options;
        API::RequestID id{};
        Check(requests.Spawn(0, &options, &id) == API::Result::InvalidArgument, "null handle rejected");
        Check(requests.Spawn(42, nullptr, &id) == API::Result::InvalidArgument, "missing options rejected");
        options.appearance.red = std::numeric_limits<float>::quiet_NaN();
        Check(requests.Spawn(42, &options, &id) == API::Result::InvalidArgument, "NaN color rejected");
        options = {};
        options.lifetime = 0;
        Check(requests.Spawn(42, &options, &id) == API::Result::InvalidArgument, "zero lifetime rejected");
        options = {};
        options.interval = std::numeric_limits<float>::infinity();
        Check(requests.Spawn(42, &options, &id) == API::Result::InvalidArgument, "infinite interval rejected");
        options = {};
        options.retryWindow = -1;
        Check(requests.Spawn(42, &options, &id) == API::Result::InvalidArgument, "negative retry window rejected");
        Check(requests.Prepare(42, 17, &id) == API::Result::InvalidArgument, "warmup respects per-actor capacity");
    }

    void ConcurrentIntent()
    {
        GhostTrails::Requests requests({});
        MakeReady(requests);
        std::vector<std::thread> threads;
        std::atomic_uint32_t admitted{};
        for (int worker = 0; worker < 4; ++worker) {
            threads.emplace_back([&] {
                for (int i = 0; i < 64; ++i) {
                    API::SpawnOptions options;
                    API::RequestID id{};
                    if (requests.Spawn(42, &options, &id) == API::Result::Accepted) ++admitted;
                }
            });
        }
        for (auto& thread : threads) thread.join();
        const auto snapshot = requests.Snapshot();
        Check(admitted == 256 && snapshot.size() == 256, "concurrent producers fill bounded admission exactly");
        for (std::size_t i = 1; i < snapshot.size(); ++i)
            Check(snapshot[i - 1].id < snapshot[i].id, "IDs are unique and ordered under concurrency");

        const auto id = snapshot.front().id;
        requests.Cancel(id);
        API::Status late;
        late.state = API::State::Completed;
        requests.Publish(id, late);
        Check(requests.Snapshot().front().canceled, "late completion cannot clear cancellation intent");
        requests.AcknowledgeStop(id);
        requests.GetStatus(id, &late);
        Check(late.state == API::State::Canceled, "cancel acknowledged after runtime removes ghosts");
    }

    void SchedulingAndFailures()
    {
        API::SpawnOptions options;
        options.count = 3;
        options.interval = 0.25f;
        options.retryWindow = 0.1f;
        GhostTrails::Timeline trail(options);
        Check(trail.Due(0), "first pose requested immediately");
        Check(trail.Retry(API::Failure::No3D, 0), "missing 3D is retryable");
        Check(!trail.Due(0.01), "failed attempts have a retry cadence");
        trail.Success(0.05);
        Check(!trail.Due(0.1), "spacing starts at actual successful capture");
        Check(trail.Due(10), "hitch yields a due pose");
        trail.Success(10);
        Check(!trail.Due(10) && !trail.Due(10.1), "hitch never catches up historical poses in same frame");
        trail.Retry(API::Failure::CloneFailed, 10.25);
        Check(!trail.Retry(API::Failure::CloneFailed, 10.5), "per-emission retry deadline is finite");
        const auto status = trail.Status();
        Check(trail.Finished() && status.state == API::State::Partial && status.spawned == 2 && status.missed == 1,
            "failure after successes is reported as Partial");
        Check(status.lastFailure == API::Failure::CloneFailed && status.attempts == 5, "status preserves diagnostic cause and actual attempts");

        options.count = 2;
        options.retryWindow = 0;
        GhostTrails::Timeline resilient(options);
        resilient.Retry(API::Failure::InvalidSkin, 0);
        Check(!resilient.Finished(), "one failed emission does not abort the whole sequence");
        resilient.Success(1);
        Check(resilient.Status().state == API::State::Partial, "later emission may succeed after first failed");

        GhostTrails::Timeline gone(options);
        gone.Fail(API::Failure::ActorGone);
        Check(gone.Finished() && gone.Status().missed == 2 && gone.Status().state == API::State::Failed,
            "permanently unresolved handle terminates without futile retries");

        options.spawnWindow = 0.2f;
        GhostTrails::Timeline windowed(options, 100);
        windowed.Success(100);
        Check(!windowed.Due(101) && windowed.Finished() && windowed.Status().lastFailure == API::Failure::WindowExpired,
            "dodge spawn window stops future emissions even after a hitch");
    }
}

int main()
{
    AdmissionAndOwnership();
    InvalidInputs();
    ConcurrentIntent();
    SchedulingAndFailures();
    if (failures) return 1;
    std::cout << "All admission, ownership, concurrency and timeline contracts passed\n";
    return 0;
}
