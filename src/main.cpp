#include <iostream>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif
#include "steam-tweak-tool/cloud_disable.hpp"
#include "steam-tweak-tool/autoupdate_disable.hpp"
#include "steam-tweak-tool/gameupdate_appmanifest_disable.hpp"
#include "steam-tweak-tool/delete_cloud_data.hpp"
#include "steam-tweak-tool/top_level_actions.hpp"
#include "steam-tweak-tool/utility.hpp"

using namespace std;

int main(int argc, char *argv[])
{

    if (argc >= 2 && string(argv[1]) == "--cloud-worker")
    {
        return runCloudWorker(argc, argv);
    }

    if (argc >= 2 && string(argv[1]) == "--cloud-sync")
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
                disableSteamCloudForAllGames(userDataPath, libraryPaths);
            }
            else if (input == "2")
            {
                disableAutoUpdatesForAllGames(libraryPaths);
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
