#pragma once
#include <string>
#include <vector>

class CloudDataDeleter
{
public:
    CloudDataDeleter();
    // steamRoot and libraryPaths come from FileUtility in main.cpp so the
    // Steam install is not detected a second time here.
    void run(const std::string &steamRoot, const std::vector<std::string> &libraryPaths);
};

// Internal worker entry point. The tool spawns itself with
// "--cloud-worker <appid> <filelist>" (list lines are "<root>|<name>"); that
// child connects to Steam, deletes the root-0 (API-managed) cloud files, and
// exits so the Steam client ends the app session and syncs the deletions -
// including the local auto-cloud (root != 0) deletions done by the parent.
int runCloudWorker(int argc, char *argv[]);
