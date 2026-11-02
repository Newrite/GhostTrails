module;

#define GHOSTTRAILS_NO_WINDOWS_HELPER
#include "GhostTrailsAPI.h"
#include <array>
#include <charconv>
#include <cmath>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <type_traits>

export module GhostTrails.AnnotationCommands;

export namespace GhostTrails::Annotations
{

  // Wire-format limits bound work and allocations in an animation callback.
  inline constexpr std::size_t maxPayloadBytes = 1024;
  inline constexpr std::size_t maxChannelBytes = 64;

  struct LookPatch
  {
    std::optional<std::array<float, 4>> color;
    std::optional<float>                intensity;
    std::optional<float>                rimPower;

    GhostTrailsAPI::Appearance Apply(GhostTrailsAPI::Appearance look) const
    {
      if (color)
      {
        look.red   = (*color)[0];
        look.green = (*color)[1];
        look.blue  = (*color)[2];
        look.alpha = (*color)[3];
      }
      if (intensity) look.intensity = *intensity;
      if (rimPower) look.rimPower = *rimPower;
      return look;
    }
  };

  struct Spawn
  {
    GhostTrailsAPI::SpawnOptions options;
    LookPatch                    look;
  };

  struct Prepare
  {
    std::uint32_t slots{1};
  };

  struct Color
  {
    LookPatch look;
  };

  struct Stop
  {};

  using Operation = std::variant<Spawn, Prepare, Color, Stop>;

  struct Command
  {
    std::string channel{"default"};
    Operation   operation;
  };

  enum class ParseError
  {
    UnknownEvent,
    TooLong,
    ConflictingPayload,
    InvalidPair,
    UnknownKey,
    DuplicateKey,
    InvalidNumber,
    InvalidColor,
    InvalidChannel,
    EmptyColor
  };

  std::string_view Describe(ParseError error)
  {
    switch (error)
    {
      case ParseError::UnknownEvent:
        return "unknown GhostTrails event";
      case ParseError::TooLong:
        return "payload exceeds 1024 bytes";
      case ParseError::ConflictingPayload:
        return "payload present in both tag and payload field";
      case ParseError::InvalidPair:
        return "expected nonempty key=value separated by semicolons";
      case ParseError::UnknownKey:
        return "unknown key or key invalid for this command";
      case ParseError::DuplicateKey:
        return "duplicate key";
      case ParseError::InvalidNumber:
        return "invalid or nonfinite number";
      case ParseError::InvalidColor:
        return "color requires four comma-separated numbers";
      case ParseError::InvalidChannel:
        return "channel requires 1..64 ASCII letters, digits, underscores or hyphens";
      case ParseError::EmptyColor:
        return "GT_Color requires color, intensity or rimPower";
    }
    return "unknown parser failure";
  }

  bool Recognized(std::string_view tag)
  {
    if (!tag.starts_with("GT_")) return false;
    tag = tag.substr(0, tag.find('.'));
    return tag == "GT_Spawn" || tag == "GT_Prepare" || tag == "GT_Color" || tag == "GT_Stop";
  }

}

namespace
{

  std::string_view Trim(std::string_view text)
  {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  }

  template <class T>
  bool Number(std::string_view text, T& value)
  {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
    if constexpr (std::is_floating_point_v<T>) return std::isfinite(value);
    return true;
  }

  bool Channel(std::string_view value)
  {
    if (value.empty() || value.size() > GhostTrails::Annotations::maxChannelBytes) return false;
    for (const char c : value)
    {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
  }

  bool ColorValue(std::string_view value, std::array<float, 4>& color)
  {
    for (std::size_t i = 0; i < color.size(); ++i)
    {
      const auto comma = value.find(',');
      if ((comma == value.npos) != (i == color.size() - 1)) return false;
      if (!Number(Trim(value.substr(0, comma)), color[i])) return false;
      if (comma != value.npos) value.remove_prefix(comma + 1);
    }
    return true;
  }

}

export namespace GhostTrails::Annotations
{

  std::expected<Command, ParseError> Parse(std::string_view tag, std::string_view payload)
  {
    if (!Recognized(tag)) return std::unexpected(ParseError::UnknownEvent);
    const auto dot = tag.find('.');
    if (dot != tag.npos)
    {
      if (!payload.empty()) return std::unexpected(ParseError::ConflictingPayload);
      payload = tag.substr(dot + 1);
      tag     = tag.substr(0, dot);
    }
    if (payload.size() > maxPayloadBytes) return std::unexpected(ParseError::TooLong);
    Command command;
    if (tag == "GT_Prepare")
      command.operation = Prepare{};
    else if (tag == "GT_Color")
      command.operation = Color{};
    else if (tag == "GT_Stop")
      command.operation = Stop{};

    auto* spawn   = std::get_if<Spawn>(&command.operation);
    auto* prepare = std::get_if<Prepare>(&command.operation);
    auto* color   = std::get_if<Color>(&command.operation);
    auto* look    = spawn ? &spawn->look : (color ? &color->look : nullptr);
    using namespace std::literals;
    constexpr std::array keys{
        "channel"sv,
        "count"sv,
        "interval"sv,
        "lifetime"sv,
        "retryWindow"sv,
        "spawnWindow"sv,
        "color"sv,
        "intensity"sv,
        "rimPower"sv,
        "slots"sv
    };
    std::uint32_t seen{};
    payload = Trim(payload);
    while (!payload.empty())
    {
      const auto semicolon = payload.find(';');
      const auto pair      = Trim(payload.substr(0, semicolon));
      const auto equal     = pair.find('=');
      if (equal == pair.npos) return std::unexpected(ParseError::InvalidPair);
      const auto key   = Trim(pair.substr(0, equal));
      const auto value = Trim(pair.substr(equal + 1));
      if (key.empty() || value.empty()) return std::unexpected(ParseError::InvalidPair);
      std::size_t index{};
      while (index < keys.size() && keys[index] != key)
        ++index;
      if (index == keys.size()) return std::unexpected(ParseError::UnknownKey);
      const auto bit = 1u << index;
      if (seen & bit) return std::unexpected(ParseError::DuplicateKey);
      seen |= bit;
      bool valid{};
      if (key == "channel")
      {
        if (prepare) return std::unexpected(ParseError::UnknownKey);
        if (!Channel(value)) return std::unexpected(ParseError::InvalidChannel);
        command.channel = value;
        valid           = true;
      }
      else if (key == "slots" && prepare)
        valid = Number(value, prepare->slots);
      else if (key == "count" && spawn)
        valid = Number(value, spawn->options.count);
      else if (key == "interval" && spawn)
        valid = Number(value, spawn->options.interval);
      else if (key == "lifetime" && spawn)
        valid = Number(value, spawn->options.lifetime);
      else if (key == "retryWindow" && spawn)
        valid = Number(value, spawn->options.retryWindow);
      else if (key == "spawnWindow" && spawn)
        valid = Number(value, spawn->options.spawnWindow);
      else if (key == "color" && look)
      {
        std::array<float, 4> rgba;
        if (!ColorValue(value, rgba)) return std::unexpected(ParseError::InvalidColor);
        look->color = rgba;
        valid       = true;
      }
      else if ((key == "intensity" || key == "rimPower") && look)
      {
        float number{};
        valid = Number(value, number);
        if (key == "intensity")
          look->intensity = number;
        else
          look->rimPower = number;
      }
      else
        return std::unexpected(ParseError::UnknownKey);
      if (!valid) return std::unexpected(ParseError::InvalidNumber);
      if (semicolon == payload.npos) break;
      payload = Trim(payload.substr(semicolon + 1));
      if (payload.empty()) return std::unexpected(ParseError::InvalidPair);
    }
    if (color && !color->look.color && !color->look.intensity && !color->look.rimPower) return std::unexpected(ParseError::EmptyColor);
    return command;
  }

}
