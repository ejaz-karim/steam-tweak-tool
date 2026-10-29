#include <iostream>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif
#include "steam-tweak-tool/cloud_disable.hpp"
#include "steam-tweak-tool/autoupdate_disable.hpp"
#include "steam-tweak-tool/gameupdate_appmanifest_disable.hpp"
#include "steam-tweak-tool/delete_cloud_data.hpp"
#include "steam-tweak-tool/utility.hpp"

using namespace std;

int main(int argc, char *argv[])
{

    if (argc >= 3 && string(argv[1]) == "--cloud-worker")
    {
        return runCloudWorker(argc, argv);
    }

    if (argc >= 4 && string(argv[1]) == "--cloud-sync")
    {
        return runCloudSyncWorker(argc, argv);
    }
#ifdef _WIN32
    SetConsoleTitleW(L"Steam Tweak Tool");
    SetConsoleOutputCP(CP_UTF8);
#endif

    try
    {
        FileUtility fileUtility;
        string steamRoot = fileUtility.resolveSteamRoot();
        string userDataPath = steamRoot + "/userdata";
        vector<string> libraryPaths = fileUtility.parseLibraryFolders(steamRoot);


        while (true)
        {
            cout << endl;
            cout << ">Enter 0 to change Steam directory" << endl;
            cout << ">Enter 1 to disable Steam Cloud for all games (per-game settings)" << endl;
            cout << ">Enter 2 to disable Auto-updates and Unschedule all game and Workshop updates" << endl;
            cout << ">Enter 3 to disable or enable Game updates" << endl;
            cout << ">Enter 4 to delete Steam Cloud data (per-user) [Restarts Steam in debug mode + Runs option 1]" << endl;
            cout << ">Enter 5 to exit" << endl;
            cout << ">Select your option: " << endl;
            string input;
            cout << ">";
            if (!getline(cin, input))
            {
                break;
            }
            cout << endl;

            if (input == "0")
            {
                steamRoot = fileUtility.promptSteamRoot();
                userDataPath = steamRoot + "/userdata";
                libraryPaths = fileUtility.parseLibraryFolders(steamRoot);
                cout << ">Steam directory updated to: " << steamRoot << endl;
            }
            else if (input == "1")
            {
                if (!filesystem::exists(userDataPath))
                {
                    cout << ">No userdata folder found, skipping Steam Cloud settings" << endl;
                    continue;
                }
                vector<int> combinedAcfIds;
                bool libraryReadFailed = false;
                for (const string& lib : libraryPaths) {
                    string sAppsPath = lib + "/steamapps";
                    if (filesystem::exists(sAppsPath)) {
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
                    continue;
                }
                
                CloudDisabler cloudDisabler;
                if (cloudDisabler.iterateSharedConfig(userDataPath, acfIds) && !libraryReadFailed)
                {
                    cout << ">Success" << endl;
                }
                else
                {
                    cout << ">Could not update every user's sharedconfig.vdf (or none was found)." << endl;
                }
            }
            else if (input == "2")
            {
                bool foundAny = false;
                bool anyGames = false;
                bool allOk = true;
                AutoUpdateDisabler autoUpdateDisabler;
                
                for (const string& lib : libraryPaths) {
                    string sAppsPath = lib + "/steamapps";
                    if (filesystem::exists(sAppsPath)) {
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
                }
                
                if (!foundAny || !anyGames)
                {
                    cout << ">There are no games in your steamapps folders" << endl;
                    continue;
                }
                if (allOk)
                {
                    cout << ">Success" << endl;
                }
                else
                {
                    cout << ">Could not update every appmanifest file." << endl;
                }
            }
            else if (input == "3")
            {
                GameUpdateDisabler gameUpdateDisabler;
                gameUpdateDisabler.run(libraryPaths);
            }
            else if (input == "4")
            {
                CloudDataDeleter cloudDataDeleter;
                cloudDataDeleter.run(steamRoot, libraryPaths);
            }
            else if (input == "5")
            {
                break;
            }
            else
            {
                cout << ">Invalid input" << endl;
            }
        }
    }
    catch (const exception &e)
    {
        cerr << ">Error: " << e.what() << endl;
        return 1;
    }
    return 0;
}
