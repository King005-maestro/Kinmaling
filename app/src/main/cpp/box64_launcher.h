#pragma once
#include <string>
int launchBox64(const std::string& box64Path, const std::string& wineBinary,
                const std::string& exePath, const std::string& workDir,
                const std::string& logPath);
void stopBox64();
