#include "WorldSpatialContext.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace SkyrimMP::Server
{
    namespace
    {
        struct ScanContext
        {
            CanonicalFormReference cell;
            CanonicalFormReference worldspace;
            bool hasCell{};
            bool hasWorldspace{};
        };

        template <class T>
        T ReadAt(std::ifstream& input, std::uint64_t offset)
        {
            T value{};
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            input.read(reinterpret_cast<char*>(&value), sizeof(T));
            if (!input) throw std::runtime_error("unexpected end of plugin while scanning spatial context");
            return value;
        }

        std::string ReadSignatureAt(std::ifstream& input, std::uint64_t offset)
        {
            std::array<char, 4> value{};
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            input.read(value.data(), static_cast<std::streamsize>(value.size()));
            if (!input) throw std::runtime_error("unexpected end of plugin while reading spatial signature");
            return std::string(value.data(), value.size());
        }

        CanonicalRecordKey ToKey(const CanonicalFormReference& reference)
        {
            return CanonicalRecordKey{ reference.kind, reference.namespaceIndex, reference.localId };
        }

        CanonicalFormReference ResolveGroupLabel(
            std::uint32_t rawFormId,
            const PluginStackEntry& sourcePlugin,
            const std::vector<PluginStackEntry>& stack,
            const std::vector<PluginNamespace>& namespaces,
            const char* labelKind)
        {
            auto resolved = ResolvePluginFormReference(rawFormId, sourcePlugin, stack, namespaces);
            if (!resolved.resolved || resolved.isNull) {
                std::ostringstream message;
                message << "failed to resolve " << labelKind << " GRUP label in " << sourcePlugin.header.filename
                        << " raw=0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << rawFormId;
                throw std::runtime_error(message.str());
            }
            return resolved;
        }

        void ValidateContextTarget(
            const CanonicalFormReference& reference,
            const CanonicalRecordDatabase& database,
            const char* expectedType,
            std::uint64_t& validated,
            std::uint64_t& missing,
            std::uint64_t& mismatched)
        {
            const auto key = ToKey(reference);
            const auto it = database.winners.find(key);
            if (it == database.winners.end()) {
                ++missing;
                throw std::runtime_error(std::string("spatial context target missing for expected type ") + expectedType);
            }
            ++validated;
            if (it->second.type != expectedType) {
                ++mismatched;
                std::ostringstream message;
                message << "spatial context target type mismatch expected=" << expectedType
                        << " actual=" << it->second.type
                        << " target=" << FormNamespaceKindName(reference.kind)
                        << ':' << reference.namespaceIndex << ":0x" << std::hex << std::uppercase << reference.localId;
                throw std::runtime_error(message.str());
            }
        }

        void ScanRange(
            std::ifstream& input,
            std::uint64_t begin,
            std::uint64_t end,
            const PluginStackEntry& plugin,
            const std::vector<PluginStackEntry>& stack,
            const std::vector<PluginNamespace>& namespaces,
            const CanonicalRecordDatabase& database,
            const ScanContext& parentContext,
            WorldSpatialContextDatabase& result)
        {
            std::uint64_t cursor = begin;
            while (cursor + 4 <= end) {
                const auto signature = ReadSignatureAt(input, cursor);
                if (signature == "GRUP") {
                    if (cursor + 24 > end) throw std::runtime_error("truncated GRUP while scanning spatial context");
                    const auto groupSize = ReadAt<std::uint32_t>(input, cursor + 4);
                    const auto label = ReadAt<std::uint32_t>(input, cursor + 8);
                    const auto groupType = ReadAt<std::int32_t>(input, cursor + 12);
                    if (groupSize < 24 || cursor + groupSize > end) throw std::runtime_error("invalid GRUP size while scanning spatial context");

                    auto child = parentContext;
                    if (groupType == 1) {
                        child.worldspace = ResolveGroupLabel(label, plugin, stack, namespaces, "worldspace");
                        child.hasWorldspace = true;
                        child.hasCell = false;
                    } else if (groupType == 6 || groupType == 8 || groupType == 9 || groupType == 10) {
                        child.cell = ResolveGroupLabel(label, plugin, stack, namespaces, "cell");
                        child.hasCell = true;
                    }

                    ScanRange(input, cursor + 24, cursor + groupSize, plugin, stack, namespaces, database, child, result);
                    cursor += groupSize;
                    continue;
                }

                if (cursor + 24 > end) throw std::runtime_error("truncated record while scanning spatial context");
                const auto dataSize = ReadAt<std::uint32_t>(input, cursor + 4);
                const auto rawFormId = ReadAt<std::uint32_t>(input, cursor + 12);
                const std::uint64_t totalSize = 24ull + dataSize;
                if (cursor + totalSize > end) throw std::runtime_error("record exceeds plugin bounds while scanning spatial context");

                if (signature == "REFR" || signature == "ACHR") {
                    const auto reference = ResolvePluginFormReference(rawFormId, plugin, stack, namespaces);
                    if (!reference.resolved || reference.isNull) {
                        throw std::runtime_error("failed to resolve REFR/ACHR record identity while scanning spatial context");
                    }
                    const auto key = ToKey(reference);
                    const auto winner = database.winners.find(key);
                    if (winner != database.winners.end() && winner->second.sourceStackIndex == plugin.stackIndex) {
                        ++result.referenceRecords;
                        WorldSpatialContext context;
                        context.referenceKey = key;

                        if (parentContext.hasCell) {
                            context.cell = parentContext.cell;
                            context.hasCell = true;
                            ++result.withCell;
                            const auto cellKey = ToKey(context.cell);
                            result.uniqueCells.insert(cellKey);
                            ValidateContextTarget(context.cell, database, "CELL", result.cellTargetsValidated, result.missingTargets, result.cellTypeMismatches);
                        } else {
                            ++result.withoutCell;
                        }

                        if (parentContext.hasWorldspace) {
                            context.worldspace = parentContext.worldspace;
                            context.hasWorldspace = true;
                            ++result.withWorldspace;
                            ++result.exteriorReferences;
                            const auto worldKey = ToKey(context.worldspace);
                            result.uniqueWorldspaces.insert(worldKey);
                            ValidateContextTarget(context.worldspace, database, "WRLD", result.worldspaceTargetsValidated, result.missingTargets, result.worldspaceTypeMismatches);
                        } else {
                            ++result.interiorReferences;
                        }

                        const auto [it, inserted] = result.contexts.emplace(key, std::move(context));
                        if (!inserted) throw std::runtime_error("duplicate winning world spatial context encountered");
                    }
                }

                cursor += totalSize;
            }
            if (cursor != end) throw std::runtime_error("spatial context scan ended on a partial structure");
        }
    }

    WorldSpatialContextDatabase BuildWorldSpatialContextDatabase(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack)
    {
        const auto namespaces = BuildFormNamespaces(a_stack);
        WorldSpatialContextDatabase result;

        for (const auto& plugin : a_stack) {
            std::ifstream input(plugin.path, std::ios::binary | std::ios::ate);
            if (!input) throw std::runtime_error("failed to open plugin for spatial context import: " + plugin.path.string());
            const auto fileSize = static_cast<std::uint64_t>(input.tellg());
            ScanRange(input, 0, fileSize, plugin, a_stack, namespaces, a_database, ScanContext{}, result);
        }

        if (result.referenceRecords != result.contexts.size()) throw std::runtime_error("spatial context record-count invariant failed");
        if (result.missingTargets != 0 || result.cellTypeMismatches != 0 || result.worldspaceTypeMismatches != 0) {
            throw std::runtime_error("spatial context canonical target invariant failed");
        }
        if (result.withCell != result.cellTargetsValidated) throw std::runtime_error("spatial CELL validation count invariant failed");
        if (result.withWorldspace != result.worldspaceTargetsValidated) throw std::runtime_error("spatial WRLD validation count invariant failed");
        if (result.interiorReferences + result.exteriorReferences != result.referenceRecords) {
            throw std::runtime_error("spatial interior/exterior partition invariant failed");
        }

        std::cout << "[SPATIAL] references=" << result.referenceRecords
                  << " withCell=" << result.withCell
                  << " withoutCell=" << result.withoutCell
                  << " interior=" << result.interiorReferences
                  << " exterior=" << result.exteriorReferences
                  << " withWorld=" << result.withWorldspace
                  << " uniqueCells=" << result.uniqueCells.size()
                  << " uniqueWorlds=" << result.uniqueWorldspaces.size()
                  << " cellTargetsValidated=" << result.cellTargetsValidated
                  << " worldTargetsValidated=" << result.worldspaceTargetsValidated
                  << " missingTargets=" << result.missingTargets
                  << " cellTypeMismatches=" << result.cellTypeMismatches
                  << " worldTypeMismatches=" << result.worldspaceTypeMismatches << '\n';

        for (const auto& [key, context] : result.contexts) {
            if (context.hasCell) {
                std::cout << "[SPATIAL-SAMPLE] ref=" << FormNamespaceKindName(key.kind) << ':' << key.namespaceIndex
                          << ":0x" << std::hex << std::uppercase << key.localId << std::dec << std::nouppercase
                          << " cell=" << FormNamespaceKindName(context.cell.kind) << ':' << context.cell.namespaceIndex
                          << ":0x" << std::hex << std::uppercase << context.cell.localId << std::dec << std::nouppercase;
                if (context.hasWorldspace) {
                    std::cout << " world=" << FormNamespaceKindName(context.worldspace.kind) << ':' << context.worldspace.namespaceIndex
                              << ":0x" << std::hex << std::uppercase << context.worldspace.localId << std::dec << std::nouppercase
                              << " exterior=true";
                } else {
                    std::cout << " interior=true";
                }
                std::cout << '\n';
                break;
            }
        }

        return result;
    }
}
