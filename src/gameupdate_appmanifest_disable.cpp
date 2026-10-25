#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "steam-tweak-tool/gameupdate_appmanifest_disable.hpp"
#include "steam-tweak-tool/utility.hpp"

using namespace std;
namespace fs = std::filesystem;

GameUpdateDisabler::GameUpdateDisabler() {}

// Collect every appmanifest_<id>.acf per library folder, in library order,
// sorted by app id within each library. Reuses FileUtility::getAcfID() and
// FileUtility::sortAcfID() from utility.cpp.
vector<pair<int, string>> GameUpdateDisabler::collectAppManifests(const vector<string> &libraryPaths)
{
    FileUtility fileUtility;
    vector<pair<int, string>> manifests;

    for (const string &lib : libraryPaths)
    {
        string sAppsPath = lib + "/steamapps";
        if (!fs::exists(sAppsPath))
        {
            continue;
        }

        vector<int> acfIds;
        try
        {
            acfIds = fileUtility.getAcfID(sAppsPath);
        }
        catch (const exception &)
        {
            // Skip libraries that cannot be read
            continue;
        }

        // sortAcfID sorts numerically and removes duplicates in place
        fileUtility.sortAcfID(acfIds);

        for (const int &id : acfIds)
        {
            string candidate = sAppsPath + "/appmanifest_" + to_string(id) + ".acf";
            if (fs::exists(candidate))
            {
                manifests.push_back(make_pair(id, candidate));
            }
        }
    }

    return manifests;
}

void GameUpdateDisabler::printManifestList(const vector<pair<int, string>> &manifests)
{
    FileUtility fileUtility;
    string lastDirectory;
    int number = 1;
    for (const auto &manifest : manifests)
    {
        fs::path path(manifest.second);
        string directory = path.parent_path().string();
        if (directory != lastDirectory)
        {
            if (!lastDirectory.empty())
            {
                cout << endl;
            }
            cout << ">Processing directory: " << directory << endl;
            lastDirectory = directory;
        }
        bool readOnly = fileUtility.isFileReadOnly(manifest.second);
        cout << ">" << number << ". ["
             << (readOnly ? "DISABLED/READ-ONLY" : "ENABLED/WRITABLE")
             << "] " << path.filename().string() << endl;
        number++;
    }
    cout << endl << ">0. Back to main menu" << endl;
}

void GameUpdateDisabler::run(const vector<string> &libraryPaths)
{
    FileUtility fileUtility;

    while (true)
    {
        vector<pair<int, string>> manifests = collectAppManifests(libraryPaths);
        if (manifests.empty())
        {
            cout << ">There are no games in your steamapps folders" << endl;
            return;
        }

        printManifestList(manifests);
        cout << ">Select the number of the appmanifest file: " << endl;
        cout << ">";
        string input;
        if (!getline(cin, input))
        {
            return;
        }

        if (input == "0")
        {
            return;
        }
        cout << endl;

        int choice = 0;
        try
        {
            choice = stoi(input);
        }
        catch (const exception &)
        {
            cout << ">Invalid input" << endl;
            continue;
        }

        if (choice < 1 || choice > static_cast<int>(manifests.size()))
        {
            cout << ">Invalid input" << endl;
            continue;
        }

        string filePath = manifests[choice - 1].second;
        string filename = fs::path(filePath).filename().string();

        bool currentlyReadOnly = fileUtility.isFileReadOnly(filePath);
        cout << ">" << filename << " is currently: "
             << (currentlyReadOnly ? "READ-ONLY" : "WRITABLE") << endl;
        cout << ">Enter 1 to Disable game updates (set read-only)" << endl;
        cout << ">Enter 2 to Enable game updates (set writable)" << endl;
        cout << ">Enter 0 to cancel" << endl;
        cout << ">";
        string action;
        if (!getline(cin, action))
        {
            continue;
        }
        cout << endl;

        if (action == "0")
        {
            cout << ">Cancelled" << endl;
        }
        else if (action == "1" || action == "2")
        {
            bool wantReadOnly = (action == "1");
            // Always re-apply the operation, even if the file already appears to
            // be in the requested state.
            if (fileUtility.setFileReadOnly(filePath, wantReadOnly))
            {
                bool nowReadOnly = fileUtility.isFileReadOnly(filePath);
                cout << ">" << filename << " is now "
                     << (nowReadOnly ? "READ-ONLY (game updates disabled)"
                                     : "WRITABLE (game updates enabled)") << endl << endl;
            }
            else
            {
                cout << ">Error: could not change attributes for " << filename << endl;
                cout << ">Try running this tool as administrator" << endl;
            }
        }
        else
        {
            cout << ">Invalid input" << endl;
        }
        // Loop back: the list is shown again with fresh states.
    }
}
