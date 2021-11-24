#pragma once

#include <cstdint>
#include <string>

namespace skyline::utils::SymbolMap {

bool tryLoad();
uintptr_t getSymbolAddress(std::string name);
std::string getSymbolName(uintptr_t address);

}
