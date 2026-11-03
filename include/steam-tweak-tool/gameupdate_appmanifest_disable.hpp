#pragma once
#include <string>
#include <utility>
#include <vector>

class GameUpdateDisabler
{
public:
    GameUpdateDisabler();
    std::vector<std::pair<int, std::string>> collectAppManifests(const std::vector<std::string> &libraryPaths);
    void printManifestList(const std::vector<std::pair<int, std::string>> &manifests);
    void run(const std::vector<std::string> &libraryPaths);
};
