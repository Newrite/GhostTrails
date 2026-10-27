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
#ifdef GHOSTTRAILS_DIAGNOSTICS
        bool updateEntered{};
        bool updateReturned{};
        std::optional<std::uint32_t> readiness;
#endif
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
#ifdef GHOSTTRAILS_DIAGNOSTICS
            log->set_level(spdlog::level::debug);
            log->flush_on(spdlog::level::debug);
#else
            log->set_level(spdlog::level::info);
            log->flush_on(spdlog::level::info);
#endif
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
#ifdef GHOSTTRAILS_DIAGNOSTICS
        const auto session = plugin->requests.Session();
        logger::debug("Session transition: playable={} reset={} epoch={}", playable, reset, session.epoch);
#endif
    }

    struct MainUpdate
    {
        static void Thunk(RE::Main* main)
        {
#ifdef GHOSTTRAILS_DIAGNOSTICS
            if (!plugin->updateEntered) {
                plugin->updateEntered = true;
                logger::debug("Main::Update first entry: operational={} initialized={}", plugin->operational, plugin->initialized);
            }
#endif
            if (!plugin->operational) {
                original(main);
                return;
            }
            if (!plugin->initialized) {
                plugin->geometryReady = GhostTrails::Geometry::Install();
                plugin->initialized = true;
#ifdef GHOSTTRAILS_DIAGNOSTICS
                logger::debug("Geometry initialization completed: available={}", plugin->geometryReady);
#endif
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
#ifdef GHOSTTRAILS_DIAGNOSTICS
            if (!plugin->updateReturned) {
                plugin->updateReturned = true;
                logger::debug("Main::Update chained call first return");
            }
#endif
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
#ifdef GHOSTTRAILS_DIAGNOSTICS
            const auto currentSession = plugin->requests.Session();
            const bool mainMenu = currentUI && currentUI->IsMenuOpen(RE::MainMenu::MENU_NAME);
            const bool loadingMenu = currentUI && currentUI->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
            const bool epochMatches = currentSession.epoch == plugin->appliedGeneration;
            const std::uint32_t state = (ready ? 1u : 0u) | (plugin->geometryReady ? 2u : 0u) |
                (plugin->enteredWorld ? 4u : 0u) | (currentUI ? 8u : 0u) | (mainMenu ? 16u : 0u) |
                (loadingMenu ? 32u : 0u) | (currentSession.playable ? 64u : 0u) | (epochMatches ? 128u : 0u);
            if (plugin->readiness != state) {
                plugin->readiness = state;
                logger::debug("API readiness changed: ready={} geometry={} enteredWorld={} ui={} mainMenu={} loadingMenu={} playable={} epoch={}/{}",
                    ready, plugin->geometryReady, plugin->enteredWorld, currentUI != nullptr, mainMenu,
                    loadingMenu, currentSession.playable, currentSession.epoch, plugin->appliedGeneration);
            }
#endif
            if (ready) {
                const bool paused = currentUI && currentUI->GameIsPaused();
                plugin->runtime.Tick(paused ? 0.0f : RE::Main::QFrameAnimTime());
            }
        }

#ifdef GHOSTTRAILS_DIAGNOSTICS
        // Read only at SKSE lifecycle boundaries. A changed call site can still
        // chain correctly through another plugin; it is evidence, not rejection.
        static void LogHookSite()
        {
            std::array<std::uint8_t, 5> bytes{};
            std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
            logger::debug("Main::Update hook check: site={:X} bytes={:02X} {:02X} {:02X} {:02X} {:02X} changed={} entered={} returned={}",
                site, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes != installedBytes,
                plugin->updateEntered, plugin->updateReturned);
        }

        static inline std::uintptr_t site{};
        static inline std::array<std::uint8_t, 5> installedBytes{};
#endif
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
#ifdef GHOSTTRAILS_DIAGNOSTICS
        MainUpdate::site = call;
        std::memcpy(MainUpdate::installedBytes.data(), reinterpret_cast<const void*>(call), MainUpdate::installedBytes.size());
        logger::debug("Main::Update hook addresses: site={:X} previous={:X} thunk={:X}",
            call, MainUpdate::original.address(), reinterpret_cast<std::uintptr_t>(&MainUpdate::Thunk));
        MainUpdate::LogHookSite();
#endif
        return true;
    }

    void Message(SKSE::MessagingInterface::Message* message)
    {
        if (!message) return;
#ifdef GHOSTTRAILS_DIAGNOSTICS
        logger::debug("SKSE message: type={} operational={} updateEntered={} geometry={} epoch={}",
            message->type, plugin->operational, plugin->updateEntered, plugin->geometryReady,
            plugin->requests.Session().epoch);
        MainUpdate::LogHookSite();
#endif
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
    logger::info("GhostTrails 1.0.5 load entry; logger ready");
    SKSE::Init(skse, false);
#ifdef GHOSTTRAILS_DIAGNOSTICS
    logger::debug("SKSE initialization complete; diagnostics enabled unconditionally");
#endif
    plugin = std::make_unique<Plugin>(GhostTrails::ReadSettings());
    GhostTrails::Bindings::Bind(plugin->requests);
#ifndef GHOSTTRAILS_DIAGNOSTICS
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
    logger::info("GhostTrails initialization complete; awaiting playable session");
    return true;
}
