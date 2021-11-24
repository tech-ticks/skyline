#include "skyline/utils/SymbolMap.hpp"

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <algorithm>

#include "nn/fs.h"
#include "skyline/logger/Logger.hpp"
#include "skyline/utils/cpputils.hpp"

namespace skyline::utils::SymbolMap {

static constexpr auto MAP_PATH = "skyline/maps/main.map";

struct Symbol {
    uintptr_t address;
    std::string name;

    Symbol(uintptr_t address, std::string name): address(address), name(name) {}
};

static std::unordered_map<std::string, uintptr_t> nameToAddr;
static std::vector<Symbol> symbols;
static bool symbolsSorted = false;

class LineWalker {
  public:
    LineWalker(char* stringStart, u64 length): stringStart(stringStart), stringEnd(stringStart + length),
        lineStart(stringStart), lineEnd(stringStart), isEof(false), isLineEmpty(false) {
    }

    char* getLine() {
        lineStart = lineEnd;

        while (*lineStart == ' ' || *lineStart == '\t') {  // Trim empty start characters
            lineStart++;
            if (lineStart >= stringEnd) {
                // We've reached the end of the file
                isEof = true;
                return lineStart;
            }
        }
        lineEnd = lineStart;

        while (*lineEnd != '\n') {
            lineEnd++;
            if (lineEnd >= stringEnd) {
                // We've reached the end of the file
                isEof = true;
                return lineStart;
            }
        }

        isLineEmpty = lineStart == lineEnd;

        *lineEnd = 0; // Replace newline with null byte
        lineEnd++;

        return lineStart;
    }

    bool eof() {
        return isEof;
    }

    bool lineEmpty() {
        return isLineEmpty;
    }

  private:
    char* stringStart;
    char* stringEnd;
    char* lineStart;
    char* lineEnd;

    bool isEof;
    bool isLineEmpty;
};

static void parse(LineWalker& walker) {
    char* line = walker.getLine();

    // Ignore empty lines and headings
    while (walker.lineEmpty() && !walker.eof()) {
        line = walker.getLine();
    }

    int textSectionId = -1;
    int bssSectionId = -1;
    int dataSectionId = -1;
    int rodataSectionId = -1;

    // Parse header to find out which identifiers map to each section
    while (!walker.eof()) {
        line = walker.getLine();
        if (strncmp(line, "Address", 7) == 0) {
            // If the line starts with "Address", we're finished with the header and the actual addresses come next
            break;
        }

        char sectionId[32] = {};
        char start[32] = {};
        char length[32] = {};
        char name[32] = {};

        // Example line: "0004:0000000000000000 00000000000000030H .text       CODE"
        int matches = sscanf(line, "%31[^:]:%31s %31s %31s", sectionId, start, length, name);
        if (matches != 4) {
            // Ignore unexpected lines
            continue;
        }

        int startInt = strtol(start, nullptr, 16);
        if (startInt != 0) {
            skyline::logger::s_Instance->LogFormat("[SymbolMap] Section %s has a non-zero start offset."
                "This is not supported.", name);
            continue;
        }

        int sectionIdInt = strtol(sectionId, nullptr, 16);
        if (strncmp(name, ".text", 5) == 0) {
            textSectionId = sectionIdInt;
        } else if (strncmp(name, ".bss", 4) == 0) {
            bssSectionId = sectionIdInt;
        } else if (strncmp(name, ".data", 5) == 0) {
            dataSectionId = sectionIdInt;
        } else if (strncmp(name, ".rodata", 7) == 0) {
            rodataSectionId = sectionIdInt;
        }
    }

    skyline::logger::s_Instance->LogFormat("[SymbolMap] Map file sections: %d=.text, %d=.bss, %d=.data, %d=.rodata",
        textSectionId, bssSectionId, dataSectionId, rodataSectionId);

    char sectionId[32] = {};
    char symAddr[32] = {};
    char symName[256] = {};

    int matchedSymbolCount = 0; // Number of symbols that could be associated with a section
    int unmatchedSymbolCount = 0; // Number of symbols for which no section could be found

    // Parse the actual symbols
    while (!walker.eof()) {
        line = walker.getLine();
        if (walker.lineEmpty()) {
            continue;
        }

        memset(sectionId, 0, 32);
        memset(symAddr, 0, 32);
        memset(symName, 0, 256);

        // Example line:
        // 00000004:000000000020A860       DungeonDatabase_GetName
        int matches = sscanf(line, "%31[^:]:%31s %255s", sectionId, symAddr, symName);
        if (matches != 3) {
            // Ignore unexpected lines
            continue;
        }

        int sectionIdInt = strtol(sectionId, nullptr, 16);
        uintptr_t offset = 0;
        if (sectionIdInt == textSectionId) {
            offset = g_MainTextAddr;
        } else if (sectionIdInt == bssSectionId) {
            offset = g_MainBssAddr;
        } else if (sectionIdInt == dataSectionId) {
            offset = g_MainDataAddr;
        } else if (sectionIdInt == rodataSectionId) {
            offset = g_MainRodataAddr;
        } else {
            // Couldn't find which section the symbol belongs to, continue with the next one
            unmatchedSymbolCount++;
            continue;
        }

        int addrInt = strtol(symAddr, nullptr, 16);
        uintptr_t absoluteAddr = addrInt + offset;

        matchedSymbolCount++;
        nameToAddr[std::string(symName)] = absoluteAddr;
        symbols.push_back(Symbol(absoluteAddr, std::string(symName)));
    }

    std::sort(symbols.begin(), symbols.end(), [](Symbol a, Symbol b) { return a.address < b.address; });

    symbolsSorted = true;
    skyline::logger::s_Instance->LogFormat("[SymbolMap] Read %d symbols from symbol map. %d symbols were skipped "
        "because their sections could not be identified.", matchedSymbolCount, unmatchedSymbolCount);
}

bool tryLoad() {
    std::string path = skyline::utils::g_RomMountStr + MAP_PATH;
    skyline::logger::Logger* logger = skyline::logger::s_Instance;
    Result rc = 0;

    nn::fs::FileHandle handle;
    rc = nn::fs::OpenFile(&handle, path.c_str(), nn::fs::OpenMode_Read);
    if (R_FAILED(rc)) {
        // Assume that the file doesn't exist and do nothing
        logger->LogFormat("[SymbolMap] Failed to open file %s. Code: %d", path.c_str(), rc);
        return false;
    }

    logger->LogFormat("[SymbolMap] Loading .map file: \"%s\"", path.c_str());

    s64 size;
    rc = nn::fs::GetFileSize(&size, handle);
    if (R_FAILED(rc)) {
        logger->LogFormat("[SymbolMap] Failed to get file size. Code: %d", rc);
        return false;
    }
    if (size == 0) {
        logger->LogFormat("[SymbolMap] Empty file!");
        return false;
    }

    char* fileBuffer = new char[size + 1];
    rc = nn::fs::ReadFile(handle, 0, fileBuffer, size);
    if (R_FAILED(rc)) {
        logger->LogFormat("[SymbolMap] Failed to read map file. Code: %d", rc);
        delete[] fileBuffer;
        return false;
    }
    fileBuffer[size] = 0; // Null-terminate

    nn::fs::CloseFile(handle);

    LineWalker walker(fileBuffer, size);
    parse(walker);

    delete[] fileBuffer;

    if (nameToAddr.size() == 0) {
        logger->LogFormat("[SymbolMap] The symbol map was parsed without errors, but no symbols were added.");
    }

    return nameToAddr.size() > 0;
}

uintptr_t getSymbolAddress(std::string name) {
    return nameToAddr[name];
}

std::string getSymbolName(uintptr_t address) {
    // Lazily sort symbols when it's first required to avoid negatively impacting startup time
    if (!symbolsSorted) {
        std::sort(symbols.begin(), symbols.end(), [](Symbol a, Symbol b) { return a.address < b.address; });
        symbolsSorted = true;
    }

    for (auto& sym: symbols) {
        if (sym.address > address) {
            return sym.name;
        }
    }

    return "";
}

}  // namespace skyline::utils::SymbolMap
