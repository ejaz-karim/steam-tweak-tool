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

namespace
{
    bool isVdfSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    string readManifestName(const string &path)
    {
        ifstream file(path, ios::binary);
        if (!file)
        {
            return "";
        }
        string text((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
        size_t pos = 0;
        int depth = 0;
        string pendingKey;
        while (pos < text.size())
        {
            char current = text[pos];
            if (isVdfSpace(current))
            {
                pos++;
                continue;
            }
            if (current == '{')
            {
                depth++;
                pendingKey.clear();
                pos++;
                continue;
            }
            if (current == '}')
            {
                depth--;
                pendingKey.clear();
                pos++;
                continue;
            }
            if (current == '[')
            {
                size_t close = text.find(']', pos);
                pos = (close == string::npos) ? text.size() : close + 1;
                continue;
            }
            if (current != '"')
            {
                pos++;
                continue;
            }
            size_t close = text.find('"', pos + 1);
            if (close == string::npos)
            {
                break;
            }
            string token = text.substr(pos + 1, close - pos - 1);
            pos = close + 1;
            if (pendingKey.empty())
            {
                pendingKey = token;
                continue;
            }
            if (pendingKey == "name" && depth == 1)
            {
                return token;
            }
            pendingKey.clear();
        }
        return "";
    }
}

GameUpdateDisabler::GameUpdateDisabler() {}

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

            continue;
        }

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
        string gameName = readManifestName(manifest.second);
        cout << ">" << number << ". ["
             << (readOnly ? "DISABLED/READ-ONLY" : "ENABLED/WRITABLE")
             << "] " << path.filename().string();
        if (!gameName.empty())
        {
            cout << " (" << gameName << ")";
        }
        cout << endl;
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
        if (input.empty() || input.find_first_not_of("0123456789") != string::npos)
        {
            cout << ">Invalid input" << endl;
            continue;
        }
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

    }
}
