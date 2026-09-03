#include "GameplayPayloadImporter.h"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace SkyrimMP::Server
{
    namespace
    {
        constexpr std::uint32_t kCompressedRecordFlag = 0x00040000u;

        bool IsGameplayType(const std::string& type)
        {
            static const std::unordered_set<std::string> kTypes{
                "NPC_", "ACHR", "REFR", "CONT", "LVLI",
                "WEAP", "ARMO", "AMMO", "SPEL", "QUST"
            };
            return kTypes.contains(type);
        }

        std::string TrimZeroTerminated(std::string value)
        {
            const auto zero = value.find('\0');
            if (zero != std::string::npos) {
                value.resize(zero);
            }
            return value;
        }

        std::uint16_t ReadU16(const std::vector<char>& data, std::size_t offset)
        {
            if (offset + sizeof(std::uint16_t) > data.size()) {
                throw std::runtime_error("truncated gameplay subrecord size");
            }
            std::uint16_t value{};
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        std::uint32_t ReadU32(const std::vector<char>& data, std::size_t offset)
        {
            if (offset + sizeof(std::uint32_t) > data.size()) {
                throw std::runtime_error("truncated gameplay extended subrecord size");
            }
            std::uint32_t value{};
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        std::string ReadSignature(const std::vector<char>& data, std::size_t offset)
        {
            if (offset + 4 > data.size()) {
                throw std::runtime_error("truncated gameplay subrecord signature");
            }
            return std::string(data.data() + offset, 4);
        }

        struct ParsedPayload
        {
            std::uint32_t subrecordCount{};
            std::string editorId;
        };

        ParsedPayload ParsePayload(const std::vector<char>& data)
        {
            ParsedPayload result;
            std::size_t offset = 0;
            std::uint32_t extendedSize = 0;

            while (offset < data.size()) {
                if (offset + 6 > data.size()) {
                    throw std::runtime_error("gameplay payload ends with partial subrecord header");
                }

                const auto type = ReadSignature(data, offset);
                offset += 4;
                const auto shortSize = ReadU16(data, offset);
                offset += 2;

                if (type == "XXXX") {
                    if (shortSize != 4 || offset + 4 > data.size()) {
                        throw std::runtime_error("invalid XXXX gameplay subrecord");
                    }
                    extendedSize = ReadU32(data, offset);
                    offset += 4;
                    continue;
                }

                const std::uint32_t size = extendedSize != 0 ? extendedSize : shortSize;
                extendedSize = 0;
                if (offset + size > data.size()) {
                    throw std::runtime_error("gameplay subrecord exceeds record payload");
                }

                ++result.subrecordCount;
                if (type == "EDID" && result.editorId.empty()) {
                    result.editorId = TrimZeroTerminated(std::string(data.data() + offset, size));
                }
                offset += size;
            }

            if (extendedSize != 0) {
                throw std::runtime_error("dangling XXXX gameplay subrecord");
            }
            return result;
        }
    }

    GameplayPayloadSummary ImportGameplayPayloads(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack,
        std::size_t a_sampleLimit)
    {
        GameplayPayloadSummary summary;

        for (std::size_t stackIndex = 0; stackIndex < a_stack.size(); ++stackIndex) {
            std::ifstream input(a_stack[stackIndex].path, std::ios::binary);
            if (!input) {
                throw std::runtime_error("failed to open plugin for gameplay payload import: " + a_stack[stackIndex].path.string());
            }

            for (const auto& [key, record] : a_database.winners) {
                if (record.sourceStackIndex != stackIndex || !IsGameplayType(record.type)) {
                    continue;
                }

                ++summary.candidateRecords;
                ++summary.typeCounts[record.type];

                if ((record.recordFlags & kCompressedRecordFlag) != 0) {
                    ++summary.deferredCompressed;
                    continue;
                }

                std::vector<char> data(record.dataSize);
                if (!data.empty()) {
                    input.seekg(static_cast<std::streamoff>(record.dataOffset), std::ios::beg);
                    input.read(data.data(), static_cast<std::streamsize>(data.size()));
                    if (!input) {
                        throw std::runtime_error("truncated gameplay record payload in " + a_stack[stackIndex].path.string());
                    }
                }

                const auto parsed = ParsePayload(data);
                ++summary.parsedRecords;
                summary.subrecords += parsed.subrecordCount;
                if (!parsed.editorId.empty()) {
                    ++summary.editorIds;
                }

                if (summary.samples.size() < a_sampleLimit) {
                    summary.samples.push_back(GameplayPayloadSample{
                        record.type,
                        record.sourcePlugin,
                        parsed.editorId,
                        key,
                        parsed.subrecordCount
                    });
                }
            }
        }

        return summary;
    }
}
