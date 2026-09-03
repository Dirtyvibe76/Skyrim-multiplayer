#include "BethesdaRecordScanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

namespace SkyrimMP::Server
{
    namespace
    {
        std::string Lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        template <class T>
        T ReadAt(std::ifstream& input, std::uint64_t offset)
        {
            T value{};
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            input.read(reinterpret_cast<char*>(&value), sizeof(T));
            if (!input) {
                throw std::runtime_error("unexpected end of plugin while scanning records");
            }
            return value;
        }

        std::string ReadSignatureAt(std::ifstream& input, std::uint64_t offset)
        {
            std::array<char, 4> value{};
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            input.read(value.data(), static_cast<std::streamsize>(value.size()));
            if (!input) {
                throw std::runtime_error("unexpected end of plugin while reading record signature");
            }
            return std::string(value.data(), value.size());
        }

        struct NamespaceLookup
        {
            FormNamespaceKind kind{ FormNamespaceKind::Full };
            std::uint32_t namespaceIndex{};
            std::uint32_t localIdBits{};
            std::string pluginName;
        };

        CanonicalFormId ResolveRawFormId(
            std::uint32_t rawFormId,
            const PluginStackEntry& plugin,
            const std::unordered_map<std::string, NamespaceLookup>& namespaces)
        {
            CanonicalFormId result;

            const std::uint32_t localOwnerIndex = rawFormId >> 24;
            std::string ownerName;
            if (localOwnerIndex < plugin.header.masters.size()) {
                ownerName = plugin.header.masters[localOwnerIndex];
            } else if (localOwnerIndex == plugin.header.masters.size()) {
                ownerName = plugin.header.filename;
            } else {
                return result;
            }

            const auto it = namespaces.find(Lower(ownerName));
            if (it == namespaces.end()) {
                return result;
            }

            result.kind = it->second.kind;
            result.namespaceIndex = it->second.namespaceIndex;
            result.pluginName = it->second.pluginName;

            const std::uint32_t mask = it->second.localIdBits == 12 ? 0x00000FFFu : 0x00FFFFFFu;
            result.localId = rawFormId & mask;
            result.resolved = true;
            return result;
        }

        void ScanRange(
            std::ifstream& input,
            std::uint64_t begin,
            std::uint64_t end,
            const PluginStackEntry& plugin,
            const std::unordered_map<std::string, NamespaceLookup>& namespaces,
            BethesdaRecordSummary& summary,
            std::size_t sampleLimit)
        {
            std::uint64_t cursor = begin;
            while (cursor + 4 <= end) {
                const auto signature = ReadSignatureAt(input, cursor);

                if (signature == "GRUP") {
                    if (cursor + 24 > end) {
                        throw std::runtime_error("truncated GRUP header in " + plugin.path.string());
                    }
                    const auto groupSize = ReadAt<std::uint32_t>(input, cursor + 4);
                    if (groupSize < 24 || cursor + groupSize > end) {
                        throw std::runtime_error("invalid GRUP size in " + plugin.path.string());
                    }
                    ScanRange(input, cursor + 24, cursor + groupSize, plugin, namespaces, summary, sampleLimit);
                    cursor += groupSize;
                    continue;
                }

                if (cursor + 24 > end) {
                    throw std::runtime_error("truncated record header in " + plugin.path.string());
                }

                const auto dataSize = ReadAt<std::uint32_t>(input, cursor + 4);
                const auto rawFormId = ReadAt<std::uint32_t>(input, cursor + 12);
                const std::uint64_t totalSize = 24ull + dataSize;
                if (cursor + totalSize > end) {
                    throw std::runtime_error("record exceeds plugin bounds in " + plugin.path.string());
                }

                if (signature != "TES4") {
                    ++summary.recordCount;
                    ++summary.typeCounts[signature];
                    const auto canonical = ResolveRawFormId(rawFormId, plugin, namespaces);
                    if (canonical.resolved) {
                        ++summary.canonicalResolved;
                        if (summary.samples.size() < sampleLimit) {
                            summary.samples.emplace_back(rawFormId, canonical);
                        }
                    } else {
                        ++summary.canonicalUnresolved;
                    }
                }

                cursor += totalSize;
            }

            if (cursor != end) {
                throw std::runtime_error("record scan ended on a partial structure in " + plugin.path.string());
            }
        }
    }

    BethesdaRecordSummary ScanBethesdaRecords(
        const PluginStackEntry& a_plugin,
        const std::vector<PluginStackEntry>& a_stack,
        const std::vector<PluginNamespace>& a_namespaces,
        std::size_t a_sampleLimit)
    {
        if (a_stack.size() != a_namespaces.size()) {
            throw std::runtime_error("plugin stack and namespace table size mismatch");
        }

        std::unordered_map<std::string, NamespaceLookup> namespaces;
        for (std::size_t i = 0; i < a_stack.size(); ++i) {
            namespaces.emplace(Lower(a_stack[i].header.filename), NamespaceLookup{
                a_namespaces[i].kind,
                a_namespaces[i].namespaceIndex,
                a_namespaces[i].localIdBits,
                a_stack[i].header.filename
            });
        }

        std::ifstream input(a_plugin.path, std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error("failed to open plugin for record scan: " + a_plugin.path.string());
        }
        const auto fileSize = static_cast<std::uint64_t>(input.tellg());
        if (fileSize < 24) {
            throw std::runtime_error("plugin too small for record scan: " + a_plugin.path.string());
        }

        BethesdaRecordSummary summary;
        summary.pluginName = a_plugin.header.filename;
        ScanRange(input, 0, fileSize, a_plugin, namespaces, summary, a_sampleLimit);
        return summary;
    }
}
