#include "PluginStack.h"

#include <algorithm>
#include <cctype>
#include <functional>
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

        struct Candidate
        {
            fs::path path;
            PluginSource source{ PluginSource::HostedMod };
            BethesdaPluginHeader header;
            bool parsed{};
        };
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

            candidates.emplace(key, Candidate{
                path,
                PluginSource::HostedMod,
                std::move(header),
                true
            });
        }

        std::vector<std::string> roots;
        roots.reserve(a_hostedPlugins.size());
        for (const auto& [name, candidate] : candidates) {
            if (candidate.source == PluginSource::HostedMod) {
                roots.push_back(name);
            }
        }
        std::sort(roots.begin(), roots.end());

        std::unordered_map<std::string, std::uint8_t> state;
        std::vector<PluginStackEntry> result;

        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            const auto stateIt = state.find(name);
            if (stateIt != state.end()) {
                if (stateIt->second == 2) {
                    return;
                }
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
                    std::move(header),
                    true
                }).first;

                if (actualKey != name) {
                    throw std::runtime_error("master filename case/identity mismatch: requested " + name + " resolved " + actualKey);
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

        return result;
    }
}
