#pragma once
#include <set>
#include <string>
#include <vector>

struct CdpCloudGame
{
    int appId = 0;
    std::string name;
    int fileCount = 0;
    unsigned long long totalSize = 0;
    std::string totalSizeText;
};

struct CdpCloudFile
{
    std::string name;
    std::string rootLabel;
    int root = 0;
    unsigned long long size = 0;
    std::string sizeText;

    std::string downloadUrl;
};

bool cdpIsAvailable();

bool cdpFetchCloudGames(std::vector<CdpCloudGame> &outGames, std::string &outError);

bool cdpFetchCloudFiles(int appId, std::vector<CdpCloudFile> &outFiles, std::string &outError,
                        bool allowEmpty = false, int expectedCount = 0);

bool cdpHasCloudFiles(int appId, bool &outHasFiles, std::string &outError);

int cdpRootFromLabel(const std::string &label);

unsigned long long cdpParseSize(const std::string &text);

bool cdpRetryAppSync(int appId, std::string &outError);

bool cdpRefreshCloudPage(std::string &outError);

bool cdpPreferLocalAppSync(int appId, std::string &outError);

bool cdpFetchConflictingApps(const std::vector<int> &appIds,
                             std::set<int> &outConflicted, std::string &outError);
