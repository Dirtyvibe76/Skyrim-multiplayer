#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    struct ServerConfig
    {
        std::string name = "SkyrimMP Server";
        std::uint16_t port = 10578;
        std::uint32_t maxPlayers = 64;
        fs::path modsPath = "Mods";
        fs::path manifestPath = "modmanifest.json";
    };

    struct ManifestFile
    {
        std::string relativePath;
        std::uintmax_t size{};
        std::string sha256;
        bool plugin{};
    };

    std::string Trim(std::string value)
    {
        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    std::string JsonEscape(const std::string& value)
    {
        std::ostringstream out;
        for (const unsigned char c : value) {
            switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                } else {
                    out << static_cast<char>(c);
                }
            }
        }
        return out.str();
    }

    ServerConfig LoadConfig(const fs::path& path)
    {
        ServerConfig config;
        std::ifstream input(path);
        if (!input) {
            std::cout << "[CONFIG] " << path.string() << " not found; using defaults\n";
            return config;
        }

        std::string section;
        std::string line;
        while (std::getline(input, line)) {
            line = Trim(line);
            if (line.empty() || line.starts_with(';') || line.starts_with('#')) {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                section = Lower(Trim(line.substr(1, line.size() - 2)));
                continue;
            }

            const auto equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }

            const auto key = Lower(Trim(line.substr(0, equals)));
            const auto value = Trim(line.substr(equals + 1));

            try {
                if (section == "server" && key == "name") {
                    config.name = value;
                } else if (section == "server" && key == "port") {
                    const auto parsed = std::stoul(value);
                    if (parsed == 0 || parsed > 65535) {
                        throw std::out_of_range("port");
                    }
                    config.port = static_cast<std::uint16_t>(parsed);
                } else if (section == "server" && key == "maxplayers") {
                    config.maxPlayers = std::stoul(value);
                } else if (section == "mods" && key == "path") {
                    config.modsPath = value;
                } else if (section == "mods" && key == "manifest") {
                    config.manifestPath = value;
                }
            } catch (...) {
                throw std::runtime_error("invalid server.ini value for [" + section + "] " + key + "=" + value);
            }
        }
        return config;
    }

    std::string Sha256File(const fs::path& path)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD objectLength = 0;
        DWORD resultLength = 0;
        DWORD hashLength = 0;

        auto fail = [&](const char* message) -> void {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error(message);
        };

        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            fail("BCryptOpenAlgorithmProvider failed");
        }
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &resultLength, 0) < 0) {
            fail("BCryptGetProperty failed");
        }

        std::vector<UCHAR> object(objectLength);
        std::vector<UCHAR> digest(hashLength);
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) {
            fail("BCryptCreateHash failed");
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            fail("failed to open file for hashing");
        }

        std::vector<char> buffer(1024 * 1024);
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) {
                fail("BCryptHashData failed");
            }
        }

        if (BCryptFinishHash(hash, digest.data(), hashLength, 0) < 0) {
            fail("BCryptFinishHash failed");
        }

        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (const auto byte : digest) {
            out << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return out.str();
    }

    bool IsPlugin(const fs::path& path)
    {
        const auto ext = Lower(path.extension().string());
        return ext == ".esm" || ext == ".esp" || ext == ".esl";
    }

    std::vector<ManifestFile> BuildManifest(const fs::path& modsRoot)
    {
        std::vector<ManifestFile> files;
        if (!fs::exists(modsRoot)) {
            fs::create_directories(modsRoot);
            return files;
        }

        for (const auto& entry : fs::recursive_directory_iterator(modsRoot)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            ManifestFile file;
            file.relativePath = fs::relative(entry.path(), modsRoot).generic_string();
            file.size = entry.file_size();
            file.sha256 = Sha256File(entry.path());
            file.plugin = IsPlugin(entry.path());
            files.push_back(std::move(file));
        }

        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return Lower(a.relativePath) < Lower(b.relativePath);
        });
        return files;
    }

    void WriteManifest(const ServerConfig& config, const std::vector<ManifestFile>& files)
    {
        const auto parent = config.manifestPath.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent);
        }

        std::ofstream out(config.manifestPath, std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to write mod manifest");
        }

        std::size_t pluginCount = 0;
        std::uintmax_t totalBytes = 0;
        for (const auto& file : files) {
            pluginCount += file.plugin ? 1u : 0u;
            totalBytes += file.size;
        }

        out << "{\n";
        out << "  \"schema\": 1,\n";
        out << "  \"serverName\": \"" << JsonEscape(config.name) << "\",\n";
        out << "  \"fileCount\": " << files.size() << ",\n";
        out << "  \"pluginCount\": " << pluginCount << ",\n";
        out << "  \"totalBytes\": " << totalBytes << ",\n";
        out << "  \"files\": [\n";
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& file = files[i];
            out << "    {\"path\":\"" << JsonEscape(file.relativePath)
                << "\",\"size\":" << file.size
                << ",\"sha256\":\"" << file.sha256
                << "\",\"plugin\":" << (file.plugin ? "true" : "false") << "}";
            if (i + 1 != files.size()) out << ',';
            out << '\n';
        }
        out << "  ]\n";
        out << "}\n";
    }
}

int main(int argc, char** argv)
{
    try {
        const fs::path configPath = argc > 1 ? fs::path(argv[1]) : fs::path("server.ini");
        const auto config = LoadConfig(configPath);

        std::cout << "SkyrimMP Dedicated Server bootstrap\n";
        std::cout << "[SERVER] name=\"" << config.name << "\" port=" << config.port
                  << " maxPlayers=" << config.maxPlayers << '\n';
        std::cout << "[MODS] scanning " << fs::absolute(config.modsPath).string() << '\n';

        const auto files = BuildManifest(config.modsPath);
        WriteManifest(config, files);

        const auto pluginCount = std::count_if(files.begin(), files.end(), [](const auto& file) { return file.plugin; });
        std::cout << "[MODS] files=" << files.size() << " plugins=" << pluginCount << '\n';
        std::cout << "[MODS] manifest=" << fs::absolute(config.manifestPath).string() << '\n';
        std::cout << "[PASS] server mod-host manifest bootstrap complete\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FATAL] " << ex.what() << '\n';
        return 1;
    }
}
