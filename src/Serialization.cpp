#include "GameBridge.h"

namespace kr {
namespace {
constexpr std::uint32_t kSerializationID = 0x4B525247;
constexpr std::uint32_t kStateRecord = 0x52414745;
constexpr std::uint32_t kVersion = 1;

void Save(SKSE::SerializationInterface* serialization)
{
    const auto state = GameBridge::Get().Save();
    const std::array<std::uint32_t, 3> data{
        static_cast<std::uint32_t>(state.current), state.pending ? 1u : 0u, state.raging ? 1u : 0u};
    if (!serialization->WriteRecord(kStateRecord, kVersion, data.data(), sizeof(data))) {
        spdlog::error("Failed to save rage state");
    }
}

void Load(SKSE::SerializationInterface* serialization)
{
    GameBridge::Get().Revert();
    std::uint32_t type{}, version{}, length{};
    while (serialization->GetNextRecordInfo(type, version, length)) {
        if (type != kStateRecord) {
            continue;
        }
        std::array<std::uint32_t, 3> data{};
        if (version != kVersion || length != sizeof(data)
            || serialization->ReadRecordData(data.data(), sizeof(data)) != sizeof(data)) {
            spdlog::error("Unsupported or truncated rage save record");
            continue;
        }
        if (data[0] > 10000 || data[1] > 1 || data[2] > 1) {
            spdlog::error("Invalid rage save data");
            continue;
        }
        GameBridge::Get().Restore({kVersion, static_cast<int>(data[0]), data[1] != 0, data[2] != 0});
    }
}

void Revert(SKSE::SerializationInterface*) { GameBridge::Get().Revert(); }
}

void RegisterSerializationCallbacks()
{
    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(kSerializationID);
    serialization->SetSaveCallback(Save);
    serialization->SetLoadCallback(Load);
    serialization->SetRevertCallback(Revert);
}

}
