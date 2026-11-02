module;

#include "PCH.h"
#include "GhostTrailsAPI.h"

export module GhostTrails.AnimationEvents;
import GhostTrails.Requests;
import GhostTrails.AnnotationCommands;
import GhostTrails.AnnotationRequests;

namespace
{

  // Borrowed from the plugin composition root for the DLL lifetime. The
  // hook never owns an actor, graph, event source, or event payload.
  GhostTrails::Annotations::RequestOwner* owner{};
  using Sink   = RE::BSTEventSink<RE::BSAnimationGraphEvent>;
  using Source = RE::BSTEventSource<RE::BSAnimationGraphEvent>;

  void Capture(Sink* receiver, const RE::BSAnimationGraphEvent* event)
  {
    if (!owner || !event || !event->holder) return;
    const std::string_view tag = event->tag.c_str();
    if (!GhostTrails::Annotations::Recognized(tag)) return;
    const auto eventName = tag.substr(0, tag.find('.'));
    auto*      reference = static_cast<RE::TESObjectREFR*>(receiver);
    if (event->holder != reference) return;
    const auto actor = reference->As<RE::Actor>();
    if (!actor) return;
    auto command = GhostTrails::Annotations::Parse(tag, event->payload.c_str());
    if (!command)
    {
      logger::warn(
        "Animation annotation '{}' rejected for actor={:08X}: {}",
        eventName,
        actor->GetFormID(),
        GhostTrails::Annotations::Describe(command.error()));
      return;
    }
    const auto result = owner->Enqueue(actor->GetHandle().native_handle(), std::move(*command));
    if (result != GhostTrails::API::Result::Accepted)
    {
      logger::warn(
        "Animation annotation '{}' not queued for actor={:08X}: result={}",
        eventName,
        actor->GetFormID(),
        static_cast<std::uint32_t>(result));
    }
  }

  template <bool Player>
  struct EventHook
  {
    static RE::BSEventNotifyControl Thunk(Sink* receiver, const RE::BSAnimationGraphEvent* event, Source* source)
    {
      Capture(receiver, event);
      return previous(receiver, event, source);
    }

    static inline REL::Relocation<decltype(Thunk)> previous;
  };

}

export namespace GhostTrails::AnimationEvents
{

  void Install(Annotations::RequestOwner& requests)
  {
    owner = &requests;
    // TESObjectREFR's animation sink is the third vtable, slot 1.
    // Character and PlayerCharacter can have different installed chains.
    REL::Relocation<std::uintptr_t> character{RE::VTABLE_Character[2]};
    REL::Relocation<std::uintptr_t> player{RE::VTABLE_PlayerCharacter[2]};
    EventHook<false>::previous = character.write_vfunc(1, EventHook<false>::Thunk);
    EventHook<true>::previous  = player.write_vfunc(1, EventHook<true>::Thunk);
    logger::info("Animation annotations enabled: GT_Prepare, GT_Spawn, GT_Color, GT_Stop");
  }

  void Log(const std::vector<Annotations::Outcome>& outcomes)
  {
    for (const auto& outcome : outcomes)
    {
      if (outcome.result != API::Result::Accepted)
      {
        logger::warn(
          "Animation command failed: actorHandle={:08X} channel={} action={} result={} request={}",
          outcome.actor,
          outcome.channel,
          Annotations::Describe(outcome.action),
          static_cast<std::uint32_t>(outcome.result),
          outcome.id);
      }
#ifdef GHOSTTRAILS_DIAGNOSTICS
      else
      {
        logger::debug(
          "Animation command accepted: actorHandle={:08X} channel={} action={} request={}",
          outcome.actor,
          outcome.channel,
          Annotations::Describe(outcome.action),
          outcome.id);
      }
#endif
    }
  }

}
