#include "BethesdaPlugin.h"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace SkyrimMP::Server
{
    namespace
    {
        template <class T>
        T ReadValue(std::istream& a_stream)
        {
            T value{};
            a_stream.read(reinterpret_cast<char*>(&value), sizeof(T));
            if (!a_stream) {
                throw std::runtime_error("unexpected end of plugin file");
            }
            return value;
        }

        std::string ReadSignature(std::istream& a_stream)
        {
            std::array<char, 4> signature{};
            a_stream.read(signature.data(), static_cast<std::streamsize>(signature.size()));
            if (!a_stream) {
                throw std::runtime_error("unexpected end of plugin file while reading signature");
            }
            return std::string(signature.data(), signature.size());
        }

        std::string TrimZeroTerminated(std::string value)
        {
            const auto zero = value.find('\0');
            if (zero != std::string::npos) {
                value.resize(zero);
            }
            return value;
        }
    }

    BethesdaPluginHeader ParseBethesdaPluginHeader(const std::filesystem::path& a_path)
    {
        std::ifstream input(a_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("failed to open plugin: " + a_path.string());
        }

        BethesdaPluginHeader result;
        result.filename = a_path.filename().string();

        const auto recordType = ReadSignature(input);
        if (recordType != "TES4") {
            throw std::runtime_error("plugin does not begin with TES4 header: " + a_path.string());
        }

        const auto dataSize = ReadValue<std::uint32_t>(input);
        result.recordFlags = ReadValue<std::uint32_t>(input);

        (void)ReadValue<std::uint32_t>(input); // FormID; TES4 header is normally zero.
        (void)ReadValue<std::uint32_t>(input); // Revision/version-control metadata.
        (void)ReadValue<std::uint16_t>(input); // Form version.
        (void)ReadValue<std::uint16_t>(input); // Unknown/reserved.

        std::vector<char> data(dataSize);
        if (dataSize > 0) {
            input.read(data.data(), static_cast<std::streamsize>(data.size()));
            if (!input) {
                throw std::runtime_error("truncated TES4 header payload: " + a_path.string());
            }
        }

        std::size_t offset = 0;
        std::uint32_t extendedSize = 0;
        while (offset + 6 <= data.size()) {
            std::string type(data.data() + offset, 4);
            offset += 4;

            std::uint16_t shortSize = 0;
            std::memcpy(&shortSize, data.data() + offset, sizeof(shortSize));
            offset += sizeof(shortSize);

            std::uint32_t subrecordSize = shortSize;
            if (type == "XXXX") {
                if (shortSize != 4 || offset + 4 > data.size()) {
                    throw std::runtime_error("invalid XXXX extended subrecord in " + a_path.string());
                }
                std::memcpy(&extendedSize, data.data() + offset, sizeof(extendedSize));
                offset += 4;
                continue;
            }

            if (extendedSize != 0) {
                subrecordSize = extendedSize;
                extendedSize = 0;
            }

            if (offset + subrecordSize > data.size()) {
                throw std::runtime_error("subrecord exceeds TES4 payload in " + a_path.string());
            }

            if (type == "HEDR" && subrecordSize >= 12) {
                std::memcpy(&result.headerVersion, data.data() + offset, sizeof(float));
                std::memcpy(&result.recordCount, data.data() + offset + 4, sizeof(std::uint32_t));
                std::memcpy(&result.nextObjectId, data.data() + offset + 8, sizeof(std::uint32_t));
            } else if (type == "MAST") {
                result.masters.push_back(TrimZeroTerminated(std::string(data.data() + offset, subrecordSize)));
            }

            offset += subrecordSize;
        }

        return result;
    }
}
