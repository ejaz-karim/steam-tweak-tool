#pragma once
#include <string>
#include <vector>

bool disableSteamCloudForAllGames(const std::string &userDataPath,
                                  const std::vector<std::string> &libraryPaths);
bool disableAutoUpdatesForAllGames(const std::vector<std::string> &libraryPaths);
