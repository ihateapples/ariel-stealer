#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ariel::screenshot {

struct capture_result {
    bool ok;
    std::vector<uint8_t> data;
    std::string error;
};

capture_result capture_desktop();

}  // namespace ariel::screenshot
