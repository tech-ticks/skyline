#include "skyline/plugin/PluginManager.hpp"

#include "nn/crypto.h"
#include "skyline/logger/TcpLogger.hpp"
#include "skyline/utils/utils.h"

namespace skyline {
namespace plugin {
    void Manager::LoadPluginsImpl() {
        Result rc;

        skyline::logger::s_Instance->LogFormat("[PluginManager] Initializing plugins...");

        // walk through romfs:/skyline/plugins recursively to find any files and push them into map
        skyline::utils::walkDirectory(utils::g_RomMountStr + PLUGIN_PATH,
                                      [this](nn::fs::DirectoryEntry const& entry, std::shared_ptr<std::string> path) {
                                          if (entry.type == nn::fs::DirectoryEntryType_File)  // ignore directories
                                              m_pluginInfos.push_back(PluginInfo{.Path = *path});
                                      });

        if (m_pluginInfos.empty()) {
            skyline::logger::s_Instance->LogFormat("[PluginManager] No plugin to load.");
            return;
        }

        // open plugins
        skyline::logger::s_Instance->LogFormat("[PluginManager] Opening plugins...");

        m_sortedHashes.clear();  // ro requires hashes to be sorted

        auto pluginInfoIter = m_pluginInfos.begin();
        while (pluginInfoIter != m_pluginInfos.end()) {
            auto& plugin = *pluginInfoIter;

            if (!OpenPlugin(plugin)) {
                pluginInfoIter = m_pluginInfos.erase(pluginInfoIter);
                continue;
            }

            pluginInfoIter++;
        }

        // manually init nn::ro ourselves, then stub it so the game doesn't try again
        nn::ro::Initialize();

        LoadPluginModulesImpl();
    }

    bool Manager::AddPluginImpl(std::string path) {
        m_pluginInfos.push_back(PluginInfo{.Path = path});
        if (!OpenPlugin(m_pluginInfos.back())) {
            m_pluginInfos.pop_back();
            return false;
        }

        return true;
    }

    bool Manager::LoadPluginModulesImpl() {
        if (!RegisterNrr()) {
            // free all loaded plugins
            m_pluginInfos.clear();
            return false;
        }

        bool success = true;
        skyline::logger::s_Instance->Log("[PluginManager] Loading plugins...\n");
        auto pluginInfoIter = m_pluginInfos.begin() + m_loadedPluginCount;
        while (pluginInfoIter != m_pluginInfos.end()) {
            auto& plugin = *pluginInfoIter;
            if (!LoadPluginModule(plugin)) {
                // stop tracking
                pluginInfoIter = m_pluginInfos.erase(pluginInfoIter);
                success = false;
                continue;
            }

            pluginInfoIter++;
        }

        // execute plugin entrypoints

        pluginInfoIter = m_pluginInfos.begin() + m_loadedPluginCount;
        while (pluginInfoIter != m_pluginInfos.end()) {
            auto& plugin = *pluginInfoIter;

            skyline::logger::s_Instance->LogFormat("[PluginManager] Running `main` for %s", plugin.Path.c_str(),
                                                   &plugin.Module.Name);

            // try to find entrypoint
            void (*pluginEntrypoint)() = NULL;
            Result rc = nn::ro::LookupModuleSymbol(reinterpret_cast<uintptr_t*>(&pluginEntrypoint), &plugin.Module, "main");

            if (pluginEntrypoint != NULL && R_SUCCEEDED(rc)) {
                pluginEntrypoint();
                skyline::logger::s_Instance->LogFormat("[PluginManager] Finished running `main` for '%s' (0x%x)",
                                                       plugin.Path.c_str(), rc);
            } else {
                success = false;
                skyline::logger::s_Instance->LogFormat("[PluginManager] Failed to lookup symbol for '%s' (0x%x)",
                                                       plugin.Path.c_str(), rc);
            }

            pluginInfoIter++;
        }
        m_loadedPluginCount = m_pluginInfos.size();

        return success;
    }

    bool Manager::OpenPlugin(PluginInfo& plugin) {
        // open file
        nn::fs::FileHandle handle;
        Result rc = nn::fs::OpenFile(&handle, plugin.Path.c_str(), nn::fs::OpenMode_Read);

        // file couldn't be opened, bail
        if (R_FAILED(rc)) {
            skyline::logger::s_Instance->LogFormat("[PluginManager] Failed to open '%s' (0x%x). Skipping.",
                                                    plugin.Path.c_str(), rc);
            // stop tracking
            return false;
        }

        s64 fileSize;
        rc = nn::fs::GetFileSize(&fileSize, handle);
        nn::fs::CloseFile(handle);  // file should be closed regardless of anything failing

        // getting file size failed, bail
        if (R_FAILED(rc)) {
            skyline::logger::s_Instance->LogFormat("[PluginManager] Failed to get '%s' size. (0x%x). Skipping.",
                                                    plugin.Path.c_str(), rc);
            // stop tracking
            return false;
        }

        plugin.Size = fileSize;
        plugin.Data = std::unique_ptr<u8>((u8*)memalign(0x1000, plugin.Size));

        rc = skyline::utils::readFile(plugin.Path, 0, plugin.Data.get(), plugin.Size);
        if (R_SUCCEEDED(rc))
            skyline::logger::s_Instance->LogFormat("[PluginManager] Read %s", plugin.Path.c_str());
        else {
            skyline::logger::s_Instance->LogFormat("[PluginManager] Failed to read '%s'. (0x%x). Skipping.",
                                                    plugin.Path.c_str(), rc);
            // stop tracking
            return false;
        }

        // get the required size for the bss
        rc = nn::ro::GetBufferSize(&plugin.BssSize, plugin.Data.get());
        if (R_FAILED(rc)) {
            // ro rejected file, bail
            // (the original input is not validated to be an actual NRO, so this isn't unusual)
            skyline::logger::s_Instance->LogFormat(
                "[PluginManager] Failed to get NRO buffer size for '%s' (0x%x), not an nro? Skipping.",
                plugin.Path.c_str(), rc);

            // stop tracking
            return false;
        }

        // calculate plugin hashes
        nn::ro::NroHeader* nroHeader = (nn::ro::NroHeader*)plugin.Data.get();
        nn::crypto::GenerateSha256Hash(&plugin.Hash, sizeof(utils::Sha256Hash), nroHeader, nroHeader->size);

        if (m_sortedHashes.find(plugin.Hash) != m_sortedHashes.end()) {
            skyline::logger::s_Instance->LogFormat("[PluginManager] '%s' is detected duplicate, Skipping.",
                                                    plugin.Path.c_str());
            // stop tracking
            return false;
        }

        m_sortedHashes.insert(plugin.Hash);
        return true;
    }

    bool Manager::RegisterNrr() {
        if (m_nrrRegistered) {
            // An NRR was already registered, unregister it first
            Result rc = nn::ro::UnregisterModuleInfo(&m_registrationInfo);

            if (R_FAILED(rc)) {
                skyline::logger::s_Instance->LogFormat("[PluginManager] Failed to unregister NRR (0x%x).", rc);
                return false;
            }

            m_nrrBuffer = nullptr;
        }

        // build nrr and register plugins

        // (sizeof(nrr header) + sizeof(sha256) * plugin count) aligned by 0x1000, as required by ro
        m_nrrSize = ALIGN_UP(sizeof(nn::ro::NrrHeader) + (m_pluginInfos.size() * sizeof(utils::Sha256Hash)), 0x1000);
        m_nrrBuffer = std::unique_ptr<u8>((u8*)memalign(0x1000, m_nrrSize));  // must be page aligned
        memset(m_nrrBuffer.get(), 0, m_nrrSize);

        // get our own program ID
        // TODO: dedicated util for this
        u64 program_id = get_program_id();

        // initialize nrr header
        auto nrrHeader = reinterpret_cast<nn::ro::NrrHeader*>(m_nrrBuffer.get());
        *nrrHeader = nn::ro::NrrHeader{
            .magic = 0x3052524E,  // NRR0
            .program_id = {program_id},
            .size = (u32)m_nrrSize,
            .type = 0,  // ForSelf
            .hashes_offset = sizeof(nn::ro::NrrHeader),
            .num_hashes = (u32)m_pluginInfos.size(),
        };

        // copy hashes into nrr
        utils::Sha256Hash* hashes =
            reinterpret_cast<utils::Sha256Hash*>((size_t)m_nrrBuffer.get() + nrrHeader->hashes_offset);
        auto curHashIdx = 0;
        for (auto hash : m_sortedHashes) {
            hashes[curHashIdx++] = hash;
        }

        // register plugins
        Result rc = nn::ro::RegisterModuleInfo(&m_registrationInfo, m_nrrBuffer.get());

        if (R_FAILED(rc)) {
            // ro rejected, free and bail
            m_nrrBuffer = nullptr;

            skyline::logger::s_Instance->LogFormat("[PluginManager] Failed to register NRR (0x%x).", rc);
            return false;
        }

        m_nrrRegistered = true;

        return true;
    }

    bool Manager::LoadPluginModule(PluginInfo& plugin) {
        plugin.BssData = std::unique_ptr<u8>((u8*)memalign(0x1000, plugin.BssSize));  // must be page aligned

        Result rc = nn::ro::LoadModule(
            &plugin.Module, plugin.Data.get(), plugin.BssData.get(), plugin.BssSize,
            nn::ro::BindFlag_Now);  // bind immediately, so all symbols are immediately available

        if (R_SUCCEEDED(rc)) {
            skyline::logger::s_Instance->LogFormat("[PluginManager] Loaded '%s'", plugin.Path.c_str(),
                                                    &plugin.Module.Name);
        } else {
            skyline::logger::s_Instance->LogFormat("[PluginManager] Failed to load '%s' (0x%x). Skipping.",
                                                    plugin.Path.c_str(), rc);
            // stop tracking
            return false;
        }
        return true;
    }

    const PluginInfo* Manager::GetContainingPluginImpl(const void* addr) {
        const PluginInfo* ret = nullptr;
        for (auto& plugin : m_pluginInfos) {
            void* module_start = (void*)plugin.Module.ModuleObject->module_base;
            void* module_end = module_start + plugin.Size;
            if (module_start < addr && addr < module_end) {
                ret = &plugin;
                break;
            }
        }
        return ret;
    }

};  // namespace plugin
};  // namespace skyline

void get_plugin_addresses(const void* internal_addr, void** start, void** end) {
    auto info = skyline::plugin::Manager::GetContainingPlugin(internal_addr);
    if (info == nullptr)
        *start = *end = nullptr;
    else {
        *start = (void*)info->Module.ModuleObject->module_base;
        *end = *start + info->Size;
    }
}

bool add_plugin(const char* path) {
    return skyline::plugin::Manager::AddPlugin(std::string(path));
}

bool load_plugin_modules() {
    return skyline::plugin::Manager::LoadPluginModules();
}
