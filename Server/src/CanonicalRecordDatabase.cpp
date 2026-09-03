#include "CanonicalRecordDatabase.h"

#include <iomanip>
#include <sstream>
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
                std::ostringstream message;
                message << "cannot build canonical record database with unresolved FormIDs in "
                        << plugin.header.filename
                        << ": unresolved=" << summary.canonicalUnresolved;
                for (const auto& record : summary.unresolvedSamples) {
                    message << " [type=" << record.type
                            << " raw=0x" << std::hex << std::uppercase
                            << std::setw(8) << std::setfill('0') << record.rawFormId
                            << std::dec << std::nouppercase << std::setfill(' ')
                            << " flags=0x" << std::hex << std::uppercase << record.recordFlags
                            << std::dec << std::nouppercase << ']';
                }
                throw std::runtime_error(message.str());
            }

            database.scannedRecords += summary.records.size();

            for (const auto& record : summary.records) {
                CanonicalRecordKey key{
                    record.canonical.kind,
                    record.canonical.namespaceIndex,
                    record.canonical.localId
                };

                auto makeWinner = [&]() {
                    return WinningRecord{
                        key,
                        record.type,
                        record.recordFlags,
                        plugin.stackIndex,
                        plugin.header.filename,
                        record.dataOffset,
                        record.dataSize
                    };
                };

                auto [it, inserted] = database.winners.try_emplace(key, makeWinner());

                if (!inserted) {
                    ++database.overrideCount;
                    if (it->second.type != record.type) {
                        ++database.typeMismatchOverrides;
                    }
                    it->second = makeWinner();
                }
            }
        }

        database.winningRecords = database.winners.size();
        return database;
    }
}
