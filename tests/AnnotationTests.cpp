#define GHOSTTRAILS_NO_WINDOWS_HELPER
#include "GhostTrailsAPI.h"
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <variant>
#include <cstdlib>

import GhostTrails.Requests;
import GhostTrails.AnnotationCommands;
import GhostTrails.AnnotationRequests;

namespace A   = GhostTrails::Annotations;
namespace API = GhostTrailsAPI;

namespace
{

  int failures{};

  void Check(bool condition, const char* contract)
  {
    if (!condition)
    {
      ++failures;
      std::cerr << "FAIL: " << contract << '\n';
    }
  }

  A::Command Command(std::string_view tag, std::string_view payload = {})
  {
    auto parsed = A::Parse(tag, payload);
    if (!parsed)
    {
      std::cerr << "Invalid test fixture: " << tag << ' ' << payload << '\n';
      std::exit(2);
    }
    return std::move(*parsed);
  }

  void Ready(GhostTrails::Requests& requests)
  {
    requests.ChangeSession(true, false);
    requests.Open(requests.Session().epoch);
  }

  void Parsing()
  {
    Check(!A::Recognized("weaponSwing"), "ordinary animation event ignored");
    Check(!A::Recognized("GT_SpawnOther"), "prefix alone is not a command");
    auto parsed = A::Parse("GT_Spawn.channel=dodge;count=4;interval=0.05;lifetime=0.6;color=0.3,0.8,1,0.6", {});
    Check(parsed.has_value(), "whole-tag payload supported");
    if (parsed)
    {
      const auto& spawn = std::get<A::Spawn>(parsed->operation);
      Check(
        parsed->channel == "dodge" && spawn.options.count == 4 && spawn.options.interval == 0.05f && spawn.look.color->at(3) == 0.6f,
        "spawn parameters preserved");
    }
    Check(A::Parse("GT_Spawn", {}).has_value(), "spawn with defaults");
    Check(A::Parse("GT_Prepare", " slots = 3 ").has_value(), "trimmed prepare");
    for (
      const auto payload :
      {"count=1;count=2",
       "unknown=2",
       "count=-1",
       "count=1junk",
       "lifetime=nan",
       "interval=inf",
       "color=1,2,3",
       "color=1,2,3,4,5",
       "color=1,,1,1",
       "channel=bad/name",
       "channel=",
       "count=2;",
       "count=99999999999999999999"})
    {
      Check(!A::Parse("GT_Spawn", payload), "malformed annotation rejected");
    }
    Check(!A::Parse("GT_Stop", "count=1"), "command-specific keys enforced");
    Check(!A::Parse("GT_Prepare", "channel=x"), "prepare has no channel ownership");
    Check(!A::Parse("GT_Color", {}), "empty color patch rejected");
    Check(!A::Parse("GT_Spawn.count=2", "count=3"), "conflicting payload rejected");
    Check(!A::Parse("GT_Spawn", std::string(1025, 'a')), "payload work bounded");
    Check(!A::Parse("GT_Stop", "channel=" + std::string(65, 'a')), "channel length bounded");
  }

  void ChannelsAndLifetime()
  {
    GhostTrails::Requests requests({});
    Ready(requests);
    A::RequestOwner owner(requests, 16);
    Check(
      owner.Enqueue(1, Command("GT_Spawn", "channel=trail;count=3;lifetime=0.6")) == API::Result::Accepted,
      "event admitted to mailbox");
    Check(requests.Snapshot().empty(), "callback does not spawn before frame");
    auto       outcomes = owner.Tick(0);
    const auto first    = outcomes.at(0).id;
    Check(outcomes[0].result == API::Result::Accepted, "frame submits spawn");
    owner.Enqueue(1, Command("GT_Color", "channel=trail;color=1,0,0,0.7;intensity=4"));
    Check(owner.Tick(0).at(0).result == API::Result::Accepted, "color updates pending emissions");
    auto snapshot = requests.Snapshot();
    Check(
      snapshot[0].options.appearance.red == 1 && snapshot[0].options.appearance.green == 0 && snapshot[0].options.appearance.intensity == 4,
      "appearance reaches API request");

    owner.Enqueue(2, Command("GT_Spawn", "channel=trail"));
    const auto otherActor = owner.Tick(0).at(0).id;
    owner.Enqueue(1, Command("GT_Spawn", "channel=other"));
    const auto otherChannel = owner.Tick(0).at(0).id;
    owner.Enqueue(1, Command("GT_Spawn", "channel=trail;lifetime=-1"));
    Check(owner.Tick(0).at(0).result == API::Result::InvalidArgument, "API owns numeric range validation");
    API::Status status;
    Check(requests.GetStatus(first, &status) == API::Result::Accepted, "failed replacement preserves old channel");
    owner.Enqueue(1, Command("GT_Spawn", "channel=trail;lifetime=0.6"));
    const auto replacement = owner.Tick(0).at(0).id;
    Check(requests.GetStatus(first, &status) == API::Result::NotFound, "successful replacement releases old ID");
    Check(
      requests.GetStatus(otherActor, &status) == API::Result::Accepted &&
        requests.GetStatus(otherChannel, &status) == API::Result::Accepted,
      "channels isolated by actor and name");
    requests.Publish(replacement, {API::State::Completed, API::Failure::None, 1, 0, 1, 1});
    owner.Tick(10);  // First observed completion starts a conservative lifetime countdown.
    owner.Tick(0);   // Pause must preserve an already visible phantom.
    Check(requests.GetStatus(replacement, &status) == API::Result::Accepted, "completion does not remove visible ghost");
    owner.Tick(0.3f);
    Check(requests.GetStatus(replacement, &status) == API::Result::Accepted, "ghost kept through its lifetime");
    owner.Tick(0.4f);
    Check(requests.GetStatus(replacement, &status) == API::Result::NotFound, "finished ID automatically released after fade");
    owner.Enqueue(2, Command("GT_Stop", "channel=trail"));
    Check(
      owner.Tick(0).at(0).result == API::Result::Accepted && requests.GetStatus(otherActor, &status) == API::Result::NotFound,
      "stop removes only selected actor channel");
    API::RequestID    external{};
    API::SpawnOptions externalOptions;
    requests.Spawn(3, &externalOptions, &external);
    owner.Reset();
    Check(requests.GetStatus(external, &status) == API::Result::Accepted, "annotation reset preserves native consumer ownership");
    Check(requests.GetStatus(otherChannel, &status) == API::Result::NotFound, "reset releases all annotation-owned IDs");
  }

  void PreparationAndSessions()
  {
    GhostTrails::Requests requests({});
    A::RequestOwner       owner(requests, 2);
    Check(owner.Enqueue(1, Command("GT_Spawn")) == API::Result::NotReady, "events during loading rejected");
    Ready(requests);
    owner.Enqueue(1, Command("GT_Prepare", "slots=2"));
    const auto id = owner.Tick(0).at(0).id;
    requests.Publish(id, {API::State::Completed, API::Failure::None, 0, 0, 1, 2});
    owner.Tick(0);
    API::Status status;
    Check(requests.GetStatus(id, &status) == API::Result::NotFound, "prepare result retired without lingering");
    requests.AcknowledgeStop(id);
    owner.Enqueue(1, Command("GT_Spawn"));
    const auto failed = owner.Tick(0).at(0).id;
    requests.Publish(failed, {API::State::Failed, API::Failure::No3D, 0, 1, 1, 1});
    owner.Tick(0);
    Check(requests.GetStatus(failed, &status) == API::Result::NotFound, "failed spawn retired without waiting for lifetime");
    requests.AcknowledgeStop(failed);
    owner.Enqueue(1, Command("GT_Spawn"));
    Ready(requests);  // A completion from the old session must never execute.
    Check(owner.Tick(0).empty() && requests.Snapshot().empty(), "old queued event discarded across epoch change");
    owner.Enqueue(1, Command("GT_Spawn"));
    owner.Enqueue(2, Command("GT_Spawn"));
    Check(owner.Enqueue(3, Command("GT_Spawn")) == API::Result::Capacity, "mailbox backpressure explicit");
    owner.Reset();
    Check(owner.Tick(0).empty(), "reset cancels pending event values");
  }

  void ConcurrentAdmission()
  {
    GhostTrails::Limits limits;
    limits.requests = 128;
    GhostTrails::Requests requests(limits);
    Ready(requests);
    A::RequestOwner            owner(requests, 128);
    std::array<std::thread, 4> workers;
    for (std::size_t i = 0; i < workers.size(); ++i)
    {
      workers[i] = std::thread([&, i] {
        for (unsigned j = 0; j < 16; ++j)
          owner.Enqueue(static_cast<unsigned>(i + 1), Command("GT_Spawn", "channel=c" + std::to_string(j)));
      });
    }
    for (auto& worker : workers)
      worker.join();
    const auto outcomes = owner.Tick(0);
    Check(outcomes.size() == 64, "concurrent actors deliver all commands");
    for (const auto& outcome : outcomes)
      Check(outcome.result == API::Result::Accepted, "concurrent command accepted by API");
    owner.Reset();
  }

}

int main()
{
  Parsing();
  ChannelsAndLifetime();
  PreparationAndSessions();
  ConcurrentAdmission();
  if (failures) return 1;
  std::cout << "Annotation parsing, channels, lifetime, sessions and concurrency contracts passed\n";
}
