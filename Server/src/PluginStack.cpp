#include "PluginStack.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace SkyrimMP::Server
{
    namespace
    {
        PluginLoadOrderInfo g_lastLoadOrderInfo;

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

        std::vector<std::string> ReadOrderFile(const fs::path& path)
        {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("failed to open plugin order file: " + path.string());
            }

            std::vector<std::string> result;
            std::unordered_set<std::string> seen;
            std::string line;
            while (std::getline(input, line)) {
                line = Trim(line);
                if (line.empty() || line.starts_with('#') || line.starts_with(';')) {
                    continue;
                }
                if (line.front() == '*') {
                    line = Trim(line.substr(1));
                }
                if (line.empty()) {
                    continue;
                }

                const auto key = Lower(line);
                if (!seen.emplace(key).second) {
                    throw std::runtime_error("duplicate plugin in " + path.string() + ": " + line);
                }
                result.push_back(key);
            }
            return result;
        }

        std::string Sha256Text(const std::string& text)
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            DWORD objectLength = 0;
            DWORD resultLength = 0;
            DWORD hashLength = 0;

            auto cleanup = [&]() {
                if (hash) BCryptDestroyHash(hash);
                if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            };

            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
                cleanup();
                throw std::runtime_error("BCryptOpenAlgorithmProvider failed for load-order hash");
            }
            if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) < 0 ||
                BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                    reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &resultLength, 0) < 0) {
                cleanup();
                throw std::runtime_error("BCryptGetProperty failed for load-order hash");
            }

            std::vector<UCHAR> object(objectLength);
            std::vector<UCHAR> digest(hashLength);
            if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) {
                cleanup();
                throw std::runtime_error("BCryptCreateHash failed for load-order hash");
            }
            if (!text.empty() && BCryptHashData(hash,
                    reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                    static_cast<ULONG>(text.size()), 0) < 0) {
                cleanup();
                throw std::runtime_error("BCryptHashData failed for load-order hash");
            }
            if (BCryptFinishHash(hash, digest.data(), hashLength, 0) < 0) {
                cleanup();
                throw std::runtime_error("BCryptFinishHash failed for load-order hash");
            }
            cleanup();

            std::ostringstream out;
            out << std::hex << std::setfill('0');
            for (const auto byte : digest) {
                out << std::setw(2) << static_cast<unsigned int>(byte);
            }
            return out.str();
        }

        struct Candidate
        {
            fs::path path;
            PluginSource source{ PluginSource::HostedMod };
            BethesdaPluginHeader header;
        };
    }

    const PluginLoadOrderInfo& GetLastPluginLoadOrderInfo()
    {
        return g_lastLoadOrderInfo;
    }

    const char* PluginSourceName(PluginSource a_source)
    {
        switch (a_source) {
        case PluginSource::BaseGame:
            return "base";
        case PluginSource::HostedMod:
            return "hosted";
        }
        return "unknown";
    }

    std::vector<PluginStackEntry> ResolvePluginStack(
        const std::vector<fs::path>& a_hostedPlugins,
        const fs::path& a_gameDataPath)
    {
        std::unordered_map<std::string, Candidate> candidates;
        for (const auto& path : a_hostedPlugins) {
            auto header = ParseBethesdaPluginHeader(path);
            const auto key = Lower(header.filename);
            if (candidates.contains(key)) {
                throw std::runtime_error("duplicate hosted plugin filename: " + header.filename);
            }
            candidates.emplace(key, Candidate{ path, PluginSource::HostedMod, std::move(header) });
        }

        const fs::path pluginsPath = "plugins.txt";
        const fs::path loadOrderPath = "loadorder.txt";
        g_lastLoadOrderInfo = {};
        g_lastLoadOrderInfo.explicitPlugins = fs::exists(pluginsPath);
        g_lastLoadOrderInfo.explicitLoadOrder = fs::exists(loadOrderPath);

        std::vector<std::string> enabled;
        if (g_lastLoadOrderInfo.explicitPlugins) {
            enabled = ReadOrderFile(pluginsPath);
            for (const auto& name : enabled) {
                if (!candidates.contains(name)) {
                    throw std::runtime_error("plugins.txt enables plugin not present in hosted Mods: " + name);
                }
            }
        } else {
            enabled.reserve(candidates.size());
            for (const auto& [name, candidate] : candidates) {
                (void)candidate;
                enabled.push_back(name);
            }
            std::sort(enabled.begin(), enabled.end());
        }
        g_lastLoadOrderInfo.enabledHostedPlugins = enabled;

        std::unordered_set<std::string> enabledSet(enabled.begin(), enabled.end());
        std::vector<std::string> roots;
        if (g_lastLoadOrderInfo.explicitLoadOrder) {
            const auto requestedOrder = ReadOrderFile(loadOrderPath);
            std::unordered_set<std::string> ordered;
            for (const auto& name : requestedOrder) {
                if (!candidates.contains(name)) {
                    throw std::runtime_error("loadorder.txt references plugin not present in hosted Mods: " + name);
                }
                if (!enabledSet.contains(name)) {
                    throw std::runtime_error("loadorder.txt contains disabled plugin: " + name);
                }
                roots.push_back(name);
                ordered.insert(name);
            }
            for (const auto& name : enabled) {
                if (!ordered.contains(name)) {
                    throw std::runtime_error("enabled plugin missing from loadorder.txt: " + name);
                }
            }
        } else {
            roots = enabled;
        }

        std::unordered_map<std::string, std::uint8_t> state;
        std::vector<PluginStackEntry> result;

        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            const auto stateIt = state.find(name);
            if (stateIt != state.end()) {
                if (stateIt->second == 2) return;
                if (stateIt->second == 1) {
                    throw std::runtime_error("plugin master dependency cycle involving: " + name);
                }
            }

            auto candidateIt = candidates.find(name);
            if (candidateIt == candidates.end()) {
                if (a_gameDataPath.empty()) {
                    throw std::runtime_error(
                        "missing master " + name + "; configure [Game] DataPath so the server can import base-game masters");
                }

                const fs::path basePath = a_gameDataPath / name;
                if (!fs::exists(basePath) || !fs::is_regular_file(basePath)) {
                    throw std::runtime_error("missing required master plugin: " + basePath.string());
                }

                auto header = ParseBethesdaPluginHeader(basePath);
                const auto actualKey = Lower(header.filename);
                candidateIt = candidates.emplace(actualKey, Candidate{
                    basePath,
                    PluginSource::BaseGame,
                    std::move(header)
                }).first;

                if (actualKey != name) {
                    throw std::runtime_error(
                        "master filename case/identity mismatch: requested " + name + " resolved " + actualKey);
                }
            }

            state[name] = 1;
            for (const auto& master : candidateIt->second.header.masters) {
                visit(Lower(master));
            }
            state[name] = 2;

            PluginStackEntry entry;
            entry.stackIndex = static_cast<std::uint32_t>(result.size());
            entry.source = candidateIt->second.source;
            entry.path = candidateIt->second.path;
            entry.header = candidateIt->second.header;
            result.push_back(std::move(entry));
        };

        for (const auto& root : roots) {
            visit(root);
        }

        std::ostringstream canonicalOrder;
        canonicalOrder << "skyrimmp-load-order-v1\n";
        for (const auto& entry : result) {
            canonicalOrder << entry.stackIndex << '|'
                           << PluginSourceName(entry.source) << '|'
                           << Lower(entry.header.filename) << '\n';
        }
        g_lastLoadOrderInfo.revisionHash = Sha256Text(canonicalOrder.str());

        std::cout << "[LOADORDER] plugins="
                  << (g_lastLoadOrderInfo.explicitPlugins ? "explicit" : "implicit-all-hosted")
                  << " order="
                  << (g_lastLoadOrderInfo.explicitLoadOrder ? "explicit" : "deterministic-fallback")
                  << " enabledHosted=" << enabled.size() << '\n';
        std::cout << "[LOADORDER] revision=" << g_lastLoadOrderInfo.revisionHash << '\n';

        return result;
    }
}
