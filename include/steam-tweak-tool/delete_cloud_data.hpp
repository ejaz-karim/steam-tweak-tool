#pragma once
#include <string>
#include <vector>

class CloudDataDeleter
{
public:
    CloudDataDeleter();

    void run(const std::string &steamRoot, const std::vector<std::string> &libraryPaths);
};

int runCloudWorker(int argc, char *argv[]);

int runCloudSyncWorker(int argc, char *argv[]);
