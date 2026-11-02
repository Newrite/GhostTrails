module;

#include "PCH.h"
#include <charconv>
#include <fstream>

export module GhostTrails.Settings;
import GhostTrails.Requests;

export namespace GhostTrails
{

  struct Settings
  {
    Limits limits;
    bool   verbose{};
  };

  Settings ReadSettings()
  {
    Settings        settings;
    const auto      path = std::filesystem::path{"Data/SKSE/Plugins/GhostTrails.ini"};
    std::error_code error;
    const bool      exists = std::filesystem::exists(path, error);
    if (error)
    {
      logger::warn("INI existence check failed: {}", error.message());
      return settings;
    }
    if (!exists) return settings;

    std::ifstream input(path);
    if (!input)
    {
      logger::warn("Cannot read GhostTrails.ini; using defaults");
      return settings;
    }

    std::string line;
    while (std::getline(input, line))
    {
      auto trim = [](std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r");
        if (first == text.npos) return std::string_view{};
        return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
      };
      const auto text = trim(std::string_view(line).substr(0, line.find_first_of(";#")));
      if (text.empty() || text.front() == '[') continue;
      const auto equal = text.find('=');
      if (equal == text.npos)
      {
        logger::warn("INI line lacks '=': {}", text);
        continue;
      }

      const auto    key   = trim(text.substr(0, equal));
      const auto    value = trim(text.substr(equal + 1));
      std::uint32_t number{};
      const auto    parsed = std::from_chars(value.data(), value.data() + value.size(), number);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
      {
        logger::warn("INI invalid integer for {}: {}", key, value);
        continue;
      }
      if (key == "VerboseLog" && number <= 1)
        settings.verbose = number == 1;
      else if (key == "MaxRequests" && number > 0 && number <= 65536)
        settings.limits.requests = number;
      else if (key == "MaxActors" && number > 0 && number <= 1024)
        settings.limits.actors = number;
      else if (key == "SlotsPerActor" && number > 0 && number <= 1024)
        settings.limits.slotsPerActor = number;
      else if (key == "TotalSlots" && number > 0 && number <= 4096)
        settings.limits.totalSlots = number;
      else if (key == "ClonesPerFrame" && number > 0 && number <= 64)
        settings.limits.clonesPerFrame = number;
      else if (key == "CloneBudgetMs" && number > 0 && number <= 250)
        settings.limits.cloneBudgetMilliseconds = static_cast<float>(number);
      else if (key == "IdleSeconds" && number > 0)
        settings.limits.idleSeconds = static_cast<float>(number);
      else
        logger::warn("INI unknown key or out-of-range value {}={}", key, value);
    }
    if (input.bad()) logger::warn("I/O error while reading GhostTrails.ini");
    return settings;
  }

}
