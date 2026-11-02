#include <iostream>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "steam-tweak-tool/top_level_actions.hpp"
#include "steam-tweak-tool/cloud_disable.hpp"
#include "steam-tweak-tool/autoupdate_disable.hpp"
#include "steam-tweak-tool/utility.hpp"

using namespace std;

bool disableSteamCloudForAllGames(const string &userDataPath,
                                  const vector<string> &libraryPaths)
{
    if (!filesystem::exists(userDataPath))
    {
        cout << ">No userdata folder found, skipping Steam Cloud settings" << endl;
        return false;
    }

    FileUtility fileUtility;
    vector<int> combinedAcfIds;
    bool libraryReadFailed = false;
    for (const string &lib : libraryPaths)
    {
        string sAppsPath = lib + "/steamapps";
        if (filesystem::exists(sAppsPath))
        {
            try
            {
                vector<int> ids = fileUtility.getAcfID(sAppsPath);
                combinedAcfIds.insert(combinedAcfIds.end(), ids.begin(), ids.end());
            }
            catch (const exception &e)
            {
                cerr << ">Error: could not read " << sAppsPath << ": " << e.what() << endl;
                libraryReadFailed = true;
            }
        }
    }

    string acfIds = fileUtility.sortAcfID(combinedAcfIds);
    if (acfIds.empty())
    {
        cout << ">There are no games in your steamapps folders" << endl;
        return false;
    }

    CloudDisabler cloudDisabler;
    if (cloudDisabler.iterateSharedConfig(userDataPath, acfIds) && !libraryReadFailed)
    {
        cout << ">Success" << endl;
        return true;
    }
    cout << ">Could not update every user's sharedconfig.vdf (or none was found)." << endl;
    return false;
}

bool disableAutoUpdatesForAllGames(const vector<string> &libraryPaths)
{
    bool foundAny = false;
    bool anyGames = false;
    bool allOk = true;
    AutoUpdateDisabler autoUpdateDisabler;
    FileUtility fileUtility;

    for (const string &lib : libraryPaths)
    {
        string sAppsPath = lib + "/steamapps";
        if (!filesystem::exists(sAppsPath))
        {
            continue;
        }
        cout << ">Processing directory: " << sAppsPath << endl;
        vector<int> appIds;
        try
        {
            appIds = fileUtility.getAcfID(sAppsPath);
        }
        catch (const exception &e)
        {
            cerr << ">Error: could not read " << sAppsPath << ": " << e.what() << endl;
            allOk = false;
            foundAny = true;
            continue;
        }
        if (!appIds.empty())
        {
            anyGames = true;
            if (!autoUpdateDisabler.iterateSteamApps(sAppsPath))
            {
                allOk = false;
            }
        }
        foundAny = true;
    }

    if (!foundAny || !anyGames)
    {
        cout << ">There are no games in your steamapps folders" << endl;
        return false;
    }
    if (allOk)
    {
        cout << ">Success" << endl;
        return true;
    }
    cout << ">Could not update every appmanifest file." << endl;
    return false;
}
