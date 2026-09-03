#pragma once

#include "BethesdaRecordScanner.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimMP::Server
{
    struct CanonicalRecordKey
    {
        FormNamespaceKind kind{ FormNamespaceKind::Full };
        std::uint32_t namespaceIndex{};
        std::uint32_t localId{};

        bool operator==(const CanonicalRecordKey&) const = default;
    };

    struct CanonicalRecordKeyHash
    {
        std::size_t operator()(const CanonicalRecordKey& a_key) const noexcept;
    };

    struct WinningRecord
    {
        CanonicalRecordKey key;
        std::string type;
        std::uint32_t recordFlags{};
        std::uint32_t sourceStackIndex{};
        std::string sourcePlugin;
        std::uint64_t dataOffset{};
        std::uint32_t dataSize{};
    };

    struct CanonicalRecordDatabase
    {
        std::uint64_t scannedRecords{};
        std::uint64_t winningRecords{};
        std::uint64_t overrideCount{};
        std::uint64_t typeMismatchOverrides{};
        std::unordered_map<CanonicalRecordKey, WinningRecord, CanonicalRecordKeyHash> winners;
    };

    CanonicalRecordDatabase BuildCanonicalRecordDatabase(
        const std::vector<PluginStackEntry>& a_stack,
        const std::vector<PluginNamespace>& a_namespaces);
}
