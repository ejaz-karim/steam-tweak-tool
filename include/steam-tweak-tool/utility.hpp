#pragma once
#include <string>
#include <vector>

class FileUtility
{
public:
    FileUtility();
    std::string readFileContents(const std::string &filePath);
    std::vector<int> getAcfID(const std::string &path);
    std::string sortAcfID(std::vector<int> &intVector);
    bool isFileReadOnly(const std::string &filePath);
    bool setFileReadOnly(const std::string &filePath, bool readOnly);
    std::string resolveSteamRoot();
    std::string promptSteamRoot();
    void saveSteamRoot(const std::string &path);
    std::string loadSteamRoot();
    std::vector<std::string> parseLibraryFolders(const std::string &steamRoot);
    std::string resolveMainRootFromLibraryDirectory(const std::string &path);
};
