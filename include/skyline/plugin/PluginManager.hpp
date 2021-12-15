#pragma once

#include <vector>
#include <set>
#include <memory>
#include <string>

#include "nn/ro.h"
#include "skyline/utils/cpputils.hpp"

namespace skyline {
namespace plugin {

    static constexpr auto PLUGIN_PATH = "skyline/plugins";

    struct PluginInfo {
        std::string Path;
        std::unique_ptr<u8> Data;
        size_t Size;
        utils::Sha256Hash Hash;
        nn::ro::Module Module;
        std::unique_ptr<u8> BssData;
        size_t BssSize;
    };

    class Manager {
       private:
        std::vector<PluginInfo> m_pluginInfos;
        std::unique_ptr<u8> m_nrrBuffer;
        size_t m_nrrSize;
        nn::ro::RegistrationInfo m_registrationInfo;
        std::set<utils::Sha256Hash> m_sortedHashes;
        bool m_nrrRegistered = false;
        int m_loadedPluginCount = 0;

        static inline auto& GetInstance() {
            static Manager s_instance;
            return s_instance;
        }

        bool AddPluginImpl(std::string path);
        void LoadPluginsImpl();
        bool LoadPluginModulesImpl();
        const PluginInfo* GetContainingPluginImpl(const void* addr);

        bool OpenPlugin(PluginInfo& plugin);
        bool RegisterNrr();
        bool LoadPluginModule(PluginInfo& plugin);

       public:
        static inline bool AddPlugin(std::string path) { return GetInstance().AddPluginImpl(path); }
        static inline void LoadPlugins() { GetInstance().LoadPluginsImpl(); }
        static inline bool LoadPluginModules() { return GetInstance().LoadPluginModulesImpl(); }
        static inline const PluginInfo* GetContainingPlugin(const void* addr) { return GetInstance().GetContainingPluginImpl(addr); }
    };

};  // namespace plugin
};  // namespace skyline

#ifdef __cplusplus
extern "C" {
#endif
void get_plugin_addresses(const void* internal_addr, void** start, void** end);

/** Add a plugin that should be loaded */
bool add_plugin(const char* path);

/** Load plugin modules added with add_plugin() */
bool load_plugin_modules();

#ifdef __cplusplus
}
#endif
