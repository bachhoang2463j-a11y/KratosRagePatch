#include "GameBridge.h"
#include <spdlog/sinks/basic_file_sink.h>

extern "C" __declspec(dllexport) constinit SKSE::PluginVersionData SKSEPlugin_Version = [] {
    SKSE::PluginVersionData version;
    version.PluginName("KratosRage");
    version.PluginVersion({0, 3, 0, 0});
    version.AuthorName("KratosRagePatch");
    version.UsesAddressLibrary();
    version.UsesStructsPost629();
    version.CompatibleVersions({REL::Version{1, 6, 640, 0}});
    version.MinimumRequiredXSEVersion({2, 2, 3, 0});
    return version;
}();

namespace {
void OnSKSEMessage(SKSE::MessagingInterface::Message* message)
{
    if (!message) {
        return;
    }
    switch (message->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        kr::GameBridge::Get().Initialize();
        break;
    case SKSE::MessagingInterface::kNewGame:
        kr::GameBridge::Get().Revert();
        kr::GameBridge::Get().ReconcileAfterLoad();
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
        kr::GameBridge::Get().ReconcileAfterLoad();
        break;
    default:
        break;
    }
}
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
    if (!skse || skse->RuntimeVersion() != REL::Version{1, 6, 640, 0}) {
        return false;
    }
    auto logDirectory = SKSE::log::log_directory();
    if (!logDirectory) {
        return false;
    }
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        (*logDirectory / "KratosRage.log").string(), false);
    auto logger = std::make_shared<spdlog::logger>("KratosRage", std::move(sink));
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
    SKSE::Init(skse);
    if (!SKSE::GetPapyrusInterface()->Register(kr::RegisterPapyrusFunctions)
        || !SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage)) {
        spdlog::error("Failed to register SKSE interfaces");
        return false;
    }
    kr::RegisterSerializationCallbacks();
    spdlog::info("KratosRage 0.3.0 loaded for Skyrim 1.6.640; combat hits +2/event, no rate cap, clear on combat end; passive gain and damage hooks disabled");
    return true;
}
