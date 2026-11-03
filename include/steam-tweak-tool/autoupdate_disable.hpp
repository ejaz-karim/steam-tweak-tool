#pragma once
#include <string>

class AutoUpdateDisabler
{
public:
    AutoUpdateDisabler();
    bool checkUpdateBehaviour(const std::string &buffer);
    std::string replaceUpdateBehaviour();
    bool checkStateFlags(const std::string &buffer);
    std::string replaceStateFlags();
    bool iterateSteamApps(const std::string &steamAppsDirectory);
};
