#pragma once

#include <string_view>

namespace corps {

// M0 smoke test: proves sim -> simcli / SimBridge linkage.
std::string_view simHello() noexcept;

}  // namespace corps
