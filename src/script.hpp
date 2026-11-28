#pragma once

#include <string>

namespace ariel::script {

struct result {
    bool ok;
    std::string detail;
};

result run_powershell(const std::string& command,
                      const bool every_run,
                      const std::string& appdata_dir,
                      std::string& error);

}  // namespace ariel::script
