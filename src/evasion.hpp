#pragma once

#include <string>

namespace ariel::evasion {

bool anti_debug();
bool anti_vm(std::string* reason = nullptr);
void show_vm_message();

}  // namespace ariel::evasion
