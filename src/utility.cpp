#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <vector>
#include "steam-tweak-tool/utility.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace std;

FileUtility::FileUtility() {}

string FileUtility::readFileContents(const string &filePath)
{
    ifstream file(filePath);
    if (!file)
    {
        throw runtime_error("Failed to open file: " + filePath);
    }
    stringstream buffer;
    buffer << file.rdbuf();
    file.close();
    return buffer.str();
}

vector<int> FileUtility::getAcfID(const string &path)
{
    vector<int> intVector;
    for (const auto &entry : filesystem::directory_iterator(path))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        if (entry.path().extension() == ".acf")
        {
            string filename = entry.path().filename().string();

            if (filename.rfind("appmanifest_", 0) == 0)
            {
                size_t start = 12;
                size_t end = filename.size() - 4;
                if (end <= start)
                {
                    continue;
                }
                string appIdText = filename.substr(start, end - start);
                if (appIdText.find_first_not_of("0123456789") != string::npos)
                {
                    continue;
                }
                try
                {
                    intVector.push_back(stoi(appIdText));
                }
                catch (const exception &)
                {
                }
            }
        }
    }

    return intVector;
}

string FileUtility::sortAcfID(vector<int> &intVector)
{
    sort(intVector.begin(), intVector.end());
    intVector.erase(unique(intVector.begin(), intVector.end()), intVector.end());
    stringstream buffer;

    for (auto &entry : intVector)
    {
        string id = to_string(entry);
        id = "\"" + id + "\"";
        buffer << id << endl;
    }

    return buffer.str();
}

string FileUtility::loadSteamRoot()
{
    ifstream file("steam_path.cfg");
    if (file)
    {
        string path;
        getline(file, path);
        file.close();
        if (!path.empty() && filesystem::exists(path))
        {
            return path;
        }
    }
    return "";
}

void FileUtility::saveSteamRoot(const string &path)
{
    ofstream file("steam_path.cfg", ios::trunc);
    if (file)
    {
        file << path;
        file.close();
    }
}

string FileUtility::promptSteamRoot()
{
    while (true)
    {
        string root;
        cout << ">Enter the path to your Steam root folder: " << endl;
        cout << ">Example: C:/Program Files (x86)/Steam" << endl;
        cout << ">";
        if (!getline(cin, root))
        {
            return "";
        }
        cout << endl;

        replace(root.begin(), root.end(), '\\', '/');

        if (filesystem::is_regular_file(root + "/steam.exe"))
        {
            saveSteamRoot(root);
            cout << ">Steam path saved to steam_path.cfg" << endl;
            return root;
        }
        else if (filesystem::is_regular_file(root + "/steam.dll") || filesystem::is_directory(root + "/steamapps"))
        {
            string resolvedRoot = resolveMainRootFromLibraryDirectory(root);
            if (!resolvedRoot.empty() && filesystem::is_regular_file(resolvedRoot + "/steam.exe"))
            {
                saveSteamRoot(resolvedRoot);
                cout << ">Steam path saved to steam_path.cfg" << endl;
                return resolvedRoot;
            }
            else
            {
                cout << ">Could not resolve main Steam root from library directory. Make sure Steam is installed." << endl;
            }
        }
        else
        {
            cout << ">Invalid Steam directory. Could not find steam.exe, steam.dll, or steamapps/." << endl;
        }
    }
}

string FileUtility::resolveSteamRoot()
{
    string root;

    string saved = loadSteamRoot();
    if (!saved.empty() && filesystem::is_regular_file(saved + "/steam.exe"))
    {
        cout << ">Using saved Steam path: " << saved << endl;
        root = saved;
    }
    else
    {

        string defaultPath = "C:/Program Files (x86)/Steam";
        if (filesystem::is_regular_file(defaultPath + "/steam.exe"))
        {
            cout << ">Found Steam at: " << defaultPath << endl;
            root = defaultPath;
        }
        else
        {

            cout << ">Could not find Steam root folder" << endl;
            root = promptSteamRoot();
            if (root.empty())
            {
                throw runtime_error("Steam path not provided");
            }
        }
    }

    vector<string> libraries = parseLibraryFolders(root);
    cout << ">Detected Library Folders:" << endl;
    for (const string& lib : libraries) {
        cout << ">" << lib << endl;
    }

    return root;
}

vector<string> FileUtility::parseLibraryFolders(const string &steamRoot)
{
    vector<string> paths;
    paths.push_back(steamRoot);

    string vdfPath = steamRoot + "/steamapps/libraryfolders.vdf";
    if (filesystem::exists(vdfPath))
    {
        ifstream file(vdfPath);
        if (file)
        {
            string line;
            while (getline(file, line))
            {
                if (line.find("\"path\"") != string::npos)
                {
                    size_t firstQuote = line.find('"', line.find("\"path\"") + 6);
                    if (firstQuote != string::npos)
                    {
                        size_t secondQuote = line.find('"', firstQuote + 1);
                        if (secondQuote != string::npos)
                        {
                            string path = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                            replace(path.begin(), path.end(), '\\', '/');
                            size_t pos;
                            while ((pos = path.find("//")) != string::npos) {
                                path.replace(pos, 2, "/");
                            }
                            if (path != steamRoot && find(paths.begin(), paths.end(), path) == paths.end()) {
                                paths.push_back(path);
                            }
                        }
                    }
                }
            }
        }
    }
    return paths;
}

string FileUtility::resolveMainRootFromLibraryDirectory(const string &path)
{
    string vdfPaths[] = {
        path + "/libraryfolder.vdf",
        path + "/libraryfolders.vdf",
        path + "/steamapps/libraryfolder.vdf",
        path + "/steamapps/libraryfolders.vdf"
    };

    string foundVdf = "";
    for (const string& p : vdfPaths) {
        if (filesystem::exists(p)) {
            foundVdf = p;
            break;
        }
    }

    if (!foundVdf.empty())
    {
        ifstream file(foundVdf);
        if (file)
        {
            string line;
            while (getline(file, line))
            {
                if (line.find("\"launcher\"") != string::npos)
                {
                    size_t firstQuote = line.find('"', line.find("\"launcher\"") + 10);
                    if (firstQuote != string::npos)
                    {
                        size_t secondQuote = line.find('"', firstQuote + 1);
                        if (secondQuote != string::npos)
                        {
                            string launcherPath = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                            replace(launcherPath.begin(), launcherPath.end(), '\\', '/');
                            size_t pos;
                            while ((pos = launcherPath.find("//")) != string::npos) {
                                launcherPath.replace(pos, 2, "/");
                            }
                            
                            size_t lastSlash = launcherPath.find_last_of('/');
                            if (lastSlash != string::npos)
                            {
                                return launcherPath.substr(0, lastSlash);
                            }
                        }
                    }
                }
            }
        }
    }
    return "";
}

bool FileUtility::isFileReadOnly(const string &filePath)
{
#ifdef _WIN32
    filesystem::path path(filePath);
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
    error_code ec;
    filesystem::file_status status = filesystem::status(filePath, ec);
    if (ec)
    {
        return false;
    }
    return (status.permissions() & filesystem::perms::owner_write) == filesystem::perms::none;
#endif
}

bool FileUtility::setFileReadOnly(const string &filePath, bool readOnly)
{
#ifdef _WIN32
    filesystem::path path(filePath);
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    DWORD newAttributes = readOnly ? (attributes | FILE_ATTRIBUTE_READONLY)
                                   : (attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));

    return SetFileAttributesW(path.c_str(), newAttributes) != 0;
#else
    error_code ec;
    filesystem::perm_options options = readOnly ? filesystem::perm_options::remove
                                                : filesystem::perm_options::add;
    filesystem::permissions(filePath, filesystem::perms::owner_write, options, ec);
    return !ec;
#endif
}
