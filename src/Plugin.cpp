#include "PCH.h"
#include <spdlog/sinks/basic_file_sink.h>

import GhostTrails.Requests;
import GhostTrails.Settings;
import GhostTrails.Runtime;
import GhostTrails.Geometry;
import GhostTrails.Bindings;

namespace
{
    // Single composition root. Domain code never resolves global dependencies.
    struct Plugin
    {
        explicit Plugin(GhostTrails::Settings value) : settings(value), requests(settings.limits), runtime(requests, settings.limits) {}

        GhostTrails::Settings settings;
        GhostTrails::Requests requests;
        GhostTrails::Runtime runtime;
        std::uint64_t appliedGeneration{};
        bool geometryReady{};
        bool initialized{};
        bool operational{};
        bool enteredWorld{};
    };

    std::unique_ptr<Plugin> plugin;

    bool SetupLog()
    {
        const auto directory = SKSE::log::log_directory();
        if (!directory) return false;
        try {
            const auto path = *directory / "GhostTrails.log";
#ifdef SPDLOG_WCHAR_FILENAMES
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.native(), true);
#else
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
#endif
            auto log = std::make_shared<spdlog::logger>("GhostTrails", std::move(sink));
            log->set_pattern("[%Y-%m-%d %T.%e] [%l] %v");
            log->set_level(spdlog::level::info);
            log->flush_on(spdlog::level::warn);
            spdlog::set_default_logger(std::move(log));
            return true;
        } catch (const spdlog::spdlog_ex&) {
            // spdlog's file-opening adapter reports ordinary I/O failure by exception.
            return false;
        }
    }

    void Transition(bool playable, bool reset)
    {
        plugin->requests.ChangeSession(playable, reset);
    }

    struct MainUpdate
    {
        static void Thunk(RE::Main* main)
        {
            if (!plugin->operational) {
                original(main);
                return;
            }
            if (!plugin->initialized) {
                plugin->geometryReady = GhostTrails::Geometry::Install();
                plugin->initialized = true;
            }

            // Release scene resources before the engine advances a load/reset/quit.
            auto session = plugin->requests.Session();
            const auto& mainState = main->GetRuntimeData();
            const auto ui = RE::UI::GetSingleton();
            const bool menu = ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
            const bool leaving = mainState.quitGame || mainState.resetGame ||
                (menu && plugin->enteredWorld);
            if (leaving && session.playable) {
                Transition(false, true);
                session = plugin->requests.Session();
            }
            if (session.epoch != plugin->appliedGeneration) {
                plugin->runtime.Reset();
                plugin->appliedGeneration = session.epoch;
                plugin->enteredWorld = false;
                logger::info("Transient state cleared for session epoch={}", session.epoch);
            }

            original(main);
            // Main::Update may request termination itself; WinMain need not
            // enter this hook again before destroying the engine's allocators.
            if (mainState.quitGame || mainState.resetGame) {
                Transition(false, true);
                plugin->runtime.Reset();
                plugin->appliedGeneration = plugin->requests.Session().epoch;
                plugin->enteredWorld = false;
                return;
            }
            auto* currentUI = RE::UI::GetSingleton();
            const bool loading = !currentUI || currentUI->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
                currentUI->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
            if (!loading) plugin->enteredWorld = true;
            const bool ready = plugin->requests.Open(plugin->appliedGeneration,
                plugin->geometryReady && plugin->enteredWorld && !loading);
            if (ready) {
                const bool paused = currentUI && currentUI->GameIsPaused();
                plugin->runtime.Tick(paused ? 0.0f : RE::Main::QFrameAnimTime());
            }
        }

        static inline REL::Relocation<decltype(Thunk)> original;
    };

    bool InstallMainHook()
    {
        const auto version = REL::Module::get().version();
        const bool known = (REL::Module::IsSE() && version == REL::Version{1, 5, 97, 0}) ||
                           (REL::Module::IsAE() && version == REL::Version{1, 6, 1170, 0}) ||
                           (REL::Module::IsVR() && version == REL::Version{1, 4, 15, 0});
        if (!known) {
            logger::error("Unaudited runtime {}; no patches installed", version.string());
            return false;
        }
        const REL::Relocation<std::uintptr_t> loop{REL::RelocationID{35551, 36544}};
        const auto call = loop.address() + REL::Relocate(0x11F, 0x160, 0x11F);
        if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8) {
            logger::error("Main::Update call site changed at {:X}; plugin disabled", call);
            return false;
        }
        SKSE::AllocTrampoline(128);
        MainUpdate::original = SKSE::GetTrampoline().write_call<5>(call, MainUpdate::Thunk);
        logger::info("Main::Update hook installed; runtime {}", version.string());
        return true;
    }

    void Message(SKSE::MessagingInterface::Message* message)
    {
        if (!message) return;
        switch (message->type) {
        case SKSE::MessagingInterface::kPreLoadGame:
            Transition(false, true);
            break;
        case SKSE::MessagingInterface::kNewGame:
            Transition(true, true);
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            // A failed load may return to the previous playable session. The
            // old transient effects have already been discarded either way.
            Transition(true, false);
            break;
        default:
            break;
        }
    }

    void Revert(SKSE::SerializationInterface*) { Transition(false, true); }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    if (!SetupLog()) return false;
    SKSE::Init(skse, false);
    logger::info("GhostTrails 1.0.4 loading");
    plugin = std::make_unique<Plugin>(GhostTrails::ReadSettings());
    GhostTrails::Bindings::Bind(plugin->requests);
#ifdef GHOSTTRAILS_DIAGNOSTICS
    if (plugin->settings.verbose) spdlog::set_level(spdlog::level::debug);
#else
    if (plugin->settings.verbose) logger::warn("VerboseLog requires a diagnostics-enabled build");
#endif
    const auto messaging = SKSE::GetMessagingInterface();
    const auto papyrus = SKSE::GetPapyrusInterface();
    const auto serialization = SKSE::GetSerializationInterface();
    if (!messaging || !papyrus || !serialization) {
        logger::error("Required SKSE messaging/Papyrus/serialization interface unavailable");
        return false;
    }
    if (!InstallMainHook()) return false;

    if (!messaging->RegisterListener(Message) || !papyrus->Register(GhostTrails::Bindings::Register)) {
        // SKSE FreeLibrary()s a DLL whose Load returns false. Once a hook or
        // callback is published, retain the DLL and keep admission disabled.
        logger::error("SKSE callback registration failed; DLL retained disabled for callback safety");
        return true;
    }
    serialization->SetUniqueID(0x4754524C); // GTRL; transient geometry is never saved.
    serialization->SetRevertCallback(Revert);
    plugin->operational = true;
    return true;
}
