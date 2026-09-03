#include "CanonicalRecordDatabase.h"

#include <stdexcept>

namespace SkyrimMP::Server
{
    std::size_t CanonicalRecordKeyHash::operator()(const CanonicalRecordKey& a_key) const noexcept
    {
        const auto kind = static_cast<std::uint64_t>(a_key.kind == FormNamespaceKind::Light ? 1u : 0u);
        const std::uint64_t packed = (kind << 63) |
            (static_cast<std::uint64_t>(a_key.namespaceIndex) << 32) |
            static_cast<std::uint64_t>(a_key.localId);
        return std::hash<std::uint64_t>{}(packed);
    }

    CanonicalRecordDatabase BuildCanonicalRecordDatabase(
        const std::vector<PluginStackEntry>& a_stack,
        const std::vector<PluginNamespace>& a_namespaces)
    {
        if (a_stack.size() != a_namespaces.size()) {
            throw std::runtime_error("plugin stack and namespace table size mismatch while building record database");
        }

        CanonicalRecordDatabase database;

        for (const auto& plugin : a_stack) {
            auto summary = ScanBethesdaRecords(plugin, a_stack, a_namespaces, 0);
            if (summary.canonicalUnresolved != 0) {
                throw std::runtime_error(
                    "cannot build canonical record database with unresolved FormIDs in " + plugin.header.filename);
            }

            database.scannedRecords += summary.records.size();

            for (const auto& record : summary.records) {
                CanonicalRecordKey key{
                    record.canonical.kind,
                    record.canonical.namespaceIndex,
                    record.canonical.localId
                };

                auto [it, inserted] = database.winners.try_emplace(key, WinningRecord{
                    key,
                    record.type,
                    record.recordFlags,
                    plugin.stackIndex,
                    plugin.header.filename
                });

                if (!inserted) {
                    ++database.overrideCount;
                    if (it->second.type != record.type) {
                        ++database.typeMismatchOverrides;
                    }
                    it->second = WinningRecord{
                        key,
                        record.type,
                        record.recordFlags,
                        plugin.stackIndex,
                        plugin.header.filename
                    };
                }
            }
        }

        database.winningRecords = database.winners.size();
        return database;
    }
}
