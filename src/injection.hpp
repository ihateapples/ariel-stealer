#pragma once

#include <string>

namespace ariel::injection {

bool install(const std::string& webhook, const std::string& script, std::string& error);

}  // namespace ariel::injection
