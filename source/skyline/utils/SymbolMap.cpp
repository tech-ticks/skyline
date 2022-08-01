#include "skyline/utils/SymbolMap.hpp"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "nn/fs.h"
#include "skyline/logger/Logger.hpp"
#include "skyline/utils/cpputils.hpp"

namespace skyline::utils::SymbolMap {

static constexpr auto MAP_DIR_PATH = "skyline/maps/3AB632DEE82D59448599B2291F30994A";
static constexpr auto MAP_PATH = "skyline/maps/3AB632DEE82D59448599B2291F30994A/unity_syms.bin";

static std::unordered_map<std::string, uintptr_t> nameToAddr;

static void parse(char* buffer) {
    s64 pos = 0;
    s32 symCount = 0;
    memcpy(&symCount, buffer, 4);
    pos += 4;

    for (int i = 0; i < symCount; i++) {
        s32 offset = 0;
        memcpy(&offset, buffer + pos, 4);
        pos += 4;
        auto str = std::string(buffer + pos);
        pos += str.length() + 1;

        uintptr_t absoluteAddr = static_cast<uintptr_t>(offset) + g_MainTextAddr;
        nameToAddr[str] = absoluteAddr;
    }

    skyline::logger::s_Instance->LogFormat("[SymbolMap] Read %d symbols from symbol map.", symCount);
}

bool hasMapFileExtension(char* fileName) {
    char* extension = strrchr(fileName, '.');
    if (extension == nullptr) {
        return false;
    }

    return strcmp(extension, ".bin") == 0;
}

bool tryLoad() {
    std::string path = skyline::utils::g_RomMountStr + MAP_PATH;
    std::string dirPath = skyline::utils::g_RomMountStr + MAP_DIR_PATH;
    skyline::logger::Logger* logger = skyline::logger::s_Instance;
    Result rc = 0;

    nn::fs::DirectoryHandle dirHandle;
    rc = nn::fs::OpenDirectory(&dirHandle, dirPath.c_str(), nn::fs::OpenDirectoryMode_File);
    if (R_FAILED(rc)) {
        logger->LogFormat("[SymbolMap] Failed to open symbol map %s. Code: %d", dirPath.c_str(), rc);
        return false;
    }
    s64 entryCount;
    rc = nn::fs::GetDirectoryEntryCount(&entryCount, dirHandle);
    if (R_FAILED(rc)) {
        logger->LogFormat("[SymbolMap] Failed to get directory entry count. Code: %d", rc);
        return false;
    }

    auto entryBuffer = new nn::fs::DirectoryEntry[entryCount];
    s64 outEntryCount;
    rc = nn::fs::ReadDirectory(&outEntryCount, entryBuffer, dirHandle, entryCount);
    if (R_FAILED(rc)) {
        logger->LogFormat("[SymbolMap] Failed to get directory entries. Code: %d", rc);
        return false;
    }

    for (int i = 0; i < outEntryCount; i++) {
        nn::fs::DirectoryEntry entry = entryBuffer[i];
        if (!hasMapFileExtension(entry.name) || entry.type != nn::fs::DirectoryEntryType_File) {
            continue;
        }

        if (entry.fileSize < 4) {
            logger->LogFormat("[SymbolMap] File size of '%s' empty or too small!", entry.name);
            return false;
        }

        std::string entryPath = dirPath + "/" + std::string(entry.name);

        nn::fs::FileHandle handle;
        rc = nn::fs::OpenFile(&handle, entryPath.c_str(), nn::fs::OpenMode_Read);
        if (R_FAILED(rc)) {
            // Assume that the file doesn't exist and do nothing
            logger->LogFormat("[SymbolMap] Failed to open file %s. Code: %d", entryPath.c_str(), rc);
            return false;
        }

        logger->LogFormat("[SymbolMap] Loading symbol map file: \"%s\"", entryPath.c_str());    

        char* fileBuffer = new char[entry.fileSize + 1];
        rc = nn::fs::ReadFile(handle, 0, fileBuffer, entry.fileSize);
        if (R_FAILED(rc)) {
            logger->LogFormat("[SymbolMap] Failed to read symbol map file. Code: %d", rc);
            delete[] fileBuffer;
            return false;
        }
        fileBuffer[entry.fileSize] = 0; // Null-terminate

        nn::fs::CloseFile(handle);

        parse(fileBuffer);

        delete[] fileBuffer;
    }

    nn::fs::CloseDirectory(dirHandle);
    delete[] entryBuffer;

    if (nameToAddr.size() == 0) {
        logger->LogFormat("[SymbolMap] The symbol map was parsed without errors, but no symbols were added.");
    }

    return nameToAddr.size() > 0;
}

uintptr_t getSymbolAddress(std::string name) {
    return nameToAddr[name];
}

}  // namespace skyline::utils::SymbolMap
