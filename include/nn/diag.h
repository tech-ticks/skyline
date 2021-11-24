/**
 * @file diag.h
 * @brief Module, logging, and symbol operations.
 */

#pragma once

#include "types.h"
#include "os.hpp"

namespace nn {
namespace diag {
    struct LogMetaData;

    struct ModuleInfo {
        char* mPath;
        u64 mBaseAddr;
        u64 mSize;
    };

    namespace detail {
        // LOG
        void LogImpl(nn::diag::LogMetaData const&, char const*, ...);
        void AbortImpl(char const*, char const*, char const*, s32);
        void VAbortImpl(char const*, char const*, char const*, int, Result const*, ::nn::os::UserExceptionInfo*, char const* fmt, va_list args);
        Result GetNearestExportedSymbol(char* buffer, u64* size, uintptr_t symbolAddr);
    };  // namespace detail

    // MODULE / SYMBOL
    u32* GetSymbolName(char* name, u64 nameSize, u64 addr);
    u64 GetRequiredBufferSizeForGetAllModuleInfo();
    s32 GetAllModuleInfo(nn::diag::ModuleInfo** out, void* buffer, u64 bufferSize);
    u64 GetSymbolSize(u64 addr);

    int GetBacktrace(uintptr_t* pOutArray, int arrayCountMax);
    int GetBacktrace(uintptr_t* pOutArray, int arrayCountMax, u64 fp, u64 sp, u64 pc);
};  // namespace diag
};  // namespace nn
