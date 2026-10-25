#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <system_error>
#endif

#include "steam-tweak-tool/delete_cloud_data.hpp"
#include "steam-tweak-tool/utility.hpp"
#include "steam-tweak-tool/gameupdate_appmanifest_disable.hpp"
#include "steam-tweak-tool/cloud_disable.hpp"
#include "steam-tweak-tool/autoupdate_disable.hpp"

using namespace std;
namespace fs = std::filesystem;

// Steam3 IDs (userdata folder names) are SteamID64 minus this base.
static const unsigned long long STEAM_ID64_BASE = 76561197960265728ULL;

// The Steamworks flat C API is loaded at runtime from steam_api64.dll, the same
// redistributable Valve ships with every game (see vendor/). Signatures match
// Valve's auto-generated steam_api_flat.h.
// SteamAPI_InitFlat returns ESteamAPIInitResult (0 = OK) and takes a
// SteamErrMsg out-buffer (char[1024]) that must always be provided.
typedef int (*SteamAPI_InitFlat_t)(char *pOutErrMsg);
typedef void (*SteamAPI_Shutdown_t)();
typedef void (*SteamAPI_RunCallbacks_t)();
typedef void *(*SteamAPI_SteamRemoteStorage_v016_t)();
typedef int (*SteamAPI_ISteamRemoteStorage_GetFileCount_t)(void *self);
typedef const char *(*SteamAPI_ISteamRemoteStorage_GetFileNameAndSize_t)(void *self, int iFile, int *pnFileSizeInBytes);
typedef bool (*SteamAPI_ISteamRemoteStorage_FileDelete_t)(void *self, const char *pchFile);
typedef bool (*SteamAPI_ISteamRemoteStorage_FileForget_t)(void *self, const char *pchFile);
typedef bool (*SteamAPI_ISteamRemoteStorage_FileExists_t)(void *self, const char *pchFile);
typedef bool (*SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp_t)(void *self);
typedef void (*SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp_t)(void *self, bool bEnabled);

namespace
{

    struct CloudFileInfo
    {
        string name;
        unsigned long long size = 0;
        // Steam Cloud storage root (remotecache.vdf "root" field).
        // 0 = API-managed file (userdata/.../remote/, deletable via the
        // Remote Storage API); 1-18 = auto-cloud (UFS) file Steam syncs from
        // a local folder - these are invisible to the Remote Storage API, so
        // removing them from the cloud requires deleting the LOCAL file
        // (same approach as SteamCloudFileManager's delete_by_indices).
        int root = 0;
    };

    struct CloudGameInfo
    {
        int appId = 0;
        string name;
        bool installed = false;
        int fileCount = 0;
        unsigned long long totalSize = 0;
    };

    bool isAllDigits(const string &text)
    {
        if (text.empty())
        {
            return false;
        }
        for (char c : text)
        {
            if (c < '0' || c > '9')
            {
                return false;
            }
        }
        return true;
    }

    bool parsePromptNumber(const string &input, int &outNumber)
    {
        if (!isAllDigits(input))
        {
            return false;
        }
        try
        {
            outNumber = stoi(input);
        }
        catch (const exception &)
        {
            return false;
        }
        return true;
    }

    string formatSize(unsigned long long bytes)
    {
        ostringstream buffer;
        buffer.precision(1);
        if (bytes >= 1024ULL * 1024 * 1024)
        {
            buffer << fixed << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GB";
        }
        else if (bytes >= 1024ULL * 1024)
        {
            buffer << fixed << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
        }
        else if (bytes >= 1024ULL)
        {
            buffer << fixed << (static_cast<double>(bytes) / 1024.0) << " KB";
        }
        else
        {
            buffer << bytes << " B";
        }
        return buffer.str();
    }

    // Name is quoted; interior escaped quotes are rare in appmanifests and are
    // not unescaped here since only display is needed.
    string extractVdfValue(const string &line, const string &key)
    {
        string token = "\"" + key + "\"";
        size_t keyPos = line.find(token);
        if (keyPos == string::npos)
        {
            return "";
        }
        size_t firstQuote = line.find('"', keyPos + token.size());
        if (firstQuote == string::npos)
        {
            return "";
        }
        size_t secondQuote = line.find('"', firstQuote + 1);
        if (secondQuote == string::npos)
        {
            return "";
        }
        return line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    }

    string getEnvVar(const char *name)
    {
        DWORD n = GetEnvironmentVariableA(name, nullptr, 0);
        if (n == 0)
        {
            return "";
        }
        vector<char> buffer(n);
        DWORD read = GetEnvironmentVariableA(name, buffer.data(), n);
        if (read == 0 || read >= n)
        {
            return "";
        }
        return string(buffer.data(), read);
    }

    // Short label for a Steam Cloud root id (Valve's UFS roots that matter on
    // Windows; names mirror SteamCloudFileManager's RootType table).
    string rootNameForRoot(int root)
    {
        switch (root)
        {
        case 0: return "API";
        case 1: return "GameInstall";
        case 2: return "Documents";
        case 3: return "AppData/Local";
        case 4: return "AppData/Roaming";
        case 5: return "SteamUserBaseStorage";
        case 6: return "Home";
        case 7: return "AppData/Roaming";
        case 8: return "Documents";
        case 9: return "Saved Games";
        case 10: return "ProgramData";
        case 11: return "Documents/Steam Cloud";
        case 12: return "AppData/LocalLow";
        case 13: return "Cache";
        case 14: return "Home";
        case 15: return "AppData/Local";
        case 16: return "AppData/Roaming";
        case 17: return "Android";
        case 18: return "Home";
        default: return "Unknown root " + to_string(root);
        }
    }

    // Best-effort local path of an auto-cloud (UFS) file, following Valve's
    // UFS root table on Windows (subset ported from SteamCloudFileManager's
    // path_resolver.rs). Returns false when the root cannot be resolved.
    bool resolveLocalUfsPath(const CloudFileInfo &file, const string &steamRoot,
                             const string &manifestPath, const string &gameName,
                             string &outPath)
    {
        if (file.root <= 0)
        {
            return false;
        }

        if (file.root == 1)
        {
            // GameInstall: <library>/steamapps/common/<installdir>/<file>.
            // manifestPath is the appmanifest in the library owning the game.
            string installdir;
            if (!manifestPath.empty())
            {
                try
                {
                    FileUtility fileUtility;
                    installdir = extractVdfValue(fileUtility.readFileContents(manifestPath), "installdir");
                }
                catch (const exception &)
                {
                }
            }
            if (installdir.empty())
            {
                return false;
            }
            outPath = steamRoot + "/steamapps/common/" + installdir + "/" + file.name;
            return true;
        }

        if (file.root == 11)
        {
            // SteamCloudDocuments: Documents/Steam Cloud/<persona>/<game>/.
            // The persona name is not tracked here, so scan the base folder's
            // subdirectories for a <gameName> folder (SCFM's fallback too).
            string home = getEnvVar("USERPROFILE");
            if (home.empty() || gameName.empty())
            {
                return false;
            }
            fs::path baseDir = fs::path(home) / "Documents" / "Steam Cloud";
            error_code ec;
            if (!fs::exists(baseDir, ec))
            {
                return false;
            }
            for (const auto &dirEntry : fs::directory_iterator(baseDir, ec))
            {
                if (ec)
                {
                    break;
                }
                if (!dirEntry.is_directory())
                {
                    continue;
                }
                fs::path candidate = dirEntry.path() / gameName;
                if (fs::exists(candidate, ec))
                {
                    outPath = (candidate / file.name).string();
                    return true;
                }
            }
            return false;
        }

        string base;
        switch (file.root)
        {
        case 2: case 8: // WinMyDocuments / MacDocuments
            base = getEnvVar("USERPROFILE") + "/Documents";
            break;
        case 3: case 15: // WinAppDataLocal / LinuxXdgDataHome (win branch)
            base = getEnvVar("LOCALAPPDATA");
            break;
        case 4: case 7: case 16: // WinAppDataRoaming / MacAppSupport / LinuxXdgConfigHome
            base = getEnvVar("APPDATA");
            break;
        case 6: case 14: case 18: // home
            base = getEnvVar("USERPROFILE");
            break;
        case 9: // WinSavedGames
            base = getEnvVar("USERPROFILE") + "/Saved Games";
            break;
        case 10: // WinProgramData
            base = getEnvVar("PROGRAMDATA");
            break;
        case 12: // WinAppDataLocalLow
            base = getEnvVar("USERPROFILE") + "/AppData/LocalLow";
            break;
        default: // 5 (SteamUserBaseStorage), 13, 17 (Android), unknown
            return false;
        }
        if (base.empty())
        {
            return false;
        }
        outPath = base + "/" + file.name;
        return true;
    }

    // Counts entries and sums sizes from remotecache.vdf, following the same
    // line state machine SteamCloudFileManager uses in parse_remotecache():
    // a quoted non-numeric key followed by "{" opens a file entry.
    void parseRemotecache(const string &content, vector<CloudFileInfo> &files)
    {
        bool inEntry = false;
        string pendingKey;
        CloudFileInfo current;

        istringstream stream(content);
        string rawLine;
        while (getline(stream, rawLine))
        {
            string line = rawLine;
            // trim
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == string::npos)
            {
                continue;
            }
            size_t end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);

            if (!inEntry)
            {
                if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
                {
                    string key = line.substr(1, line.size() - 2);
                    if (isAllDigits(key))
                    {
                        pendingKey.clear();
                    }
                    else
                    {
                        pendingKey = key;
                    }
                }
                else if (line == "{" && !pendingKey.empty())
                {
                    inEntry = true;
                    current = CloudFileInfo();
                    current.name = pendingKey;
                    pendingKey.clear();
                }
            }
            else if (line == "}")
            {
                files.push_back(current);
                inEntry = false;
            }
            else
            {
                string value = extractVdfValue(line, "size");
                if (!value.empty())
                {
                    try
                    {
                        current.size = stoull(value);
                    }
                    catch (const exception &)
                    {
                    }
                }

                value = extractVdfValue(line, "root");
                if (!value.empty())
                {
                    try
                    {
                        current.root = stoi(value);
                    }
                    catch (const exception &)
                    {
                    }
                }
            }
        }
    }

    // Builds appId -> manifest path across all libraries by reusing the
    // existing helpers (FileUtility::getAcfID/sortAcfID via collectAppManifests).
    map<int, string> buildManifestMap(const vector<string> &libraryPaths)
    {
        GameUpdateDisabler disabler;
        map<int, string> manifests;
        for (const auto &entry : disabler.collectAppManifests(libraryPaths))
        {
            manifests[entry.first] = entry.second;
        }
        return manifests;
    }

    // Determines the currently signed-in user the way Steam itself does:
    // the "users" entry in loginusers.vdf flagged MostRecent "1", falling back
    // to the newest Timestamp (modern Steam clients often omit MostRecent).
    // Returns the steam3 ID used by userdata folders.
    bool resolveCurrentUser(const string &steamRoot, string &outUserId, string &outPersonaName)
    {
        string loginusersPath = steamRoot + "/config/loginusers.vdf";
        if (!fs::exists(loginusersPath))
        {
            return false;
        }

        FileUtility fileUtility;
        string content;
        try
        {
            content = fileUtility.readFileContents(loginusersPath);
        }
        catch (const exception &)
        {
            return false;
        }

        struct LoginUser
        {
            string id64;
            string persona;
            long long timestamp = 0;
            bool mostRecent = false;
        };
        vector<LoginUser> users;
        LoginUser current;

        istringstream stream(content);
        string rawLine;
        while (getline(stream, rawLine))
        {
            string line = rawLine;
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == string::npos)
            {
                continue;
            }
            size_t end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);

            // A top-level "users" entry is a bare 17-digit SteamID64 in quotes.
            bool looksLikeId64 = line.size() >= 2 && line.front() == '"' && line.back() == '"' &&
                                 line.size() <= 21 && isAllDigits(line.substr(1, line.size() - 2));
            if (looksLikeId64)
            {
                if (!current.id64.empty())
                {
                    users.push_back(current);
                }
                current = LoginUser();
                current.id64 = line.substr(1, line.size() - 2);
                continue;
            }

            if (current.id64.empty())
            {
                continue;
            }

            string value = extractVdfValue(line, "PersonaName");
            if (!value.empty())
            {
                current.persona = value;
            }

            value = extractVdfValue(line, "MostRecent");
            if (value == "1")
            {
                current.mostRecent = true;
            }

            value = extractVdfValue(line, "Timestamp");
            if (!value.empty())
            {
                try
                {
                    long long ts = stoll(value);
                    if (ts > current.timestamp)
                    {
                        current.timestamp = ts;
                    }
                }
                catch (const exception &)
                {
                }
            }
        }
        if (!current.id64.empty())
        {
            users.push_back(current);
        }

        const LoginUser *chosen = nullptr;
        for (const auto &user : users)
        {
            if (user.mostRecent && (chosen == nullptr || user.timestamp > chosen->timestamp))
            {
                chosen = &user;
            }
        }
        if (chosen == nullptr)
        {
            for (const auto &user : users)
            {
                if (chosen == nullptr || user.timestamp > chosen->timestamp)
                {
                    chosen = &user;
                }
            }
        }
        if (chosen == nullptr)
        {
            return false;
        }

        string chosenId64 = chosen->id64;
        outPersonaName = chosen->persona.empty() ? "(unknown)" : chosen->persona;

        unsigned long long id64 = 0;
        try
        {
            id64 = stoull(chosenId64);
        }
        catch (const exception &)
        {
            return false;
        }
        if (id64 <= STEAM_ID64_BASE)
        {
            return false;
        }
        outUserId = to_string(id64 - STEAM_ID64_BASE);
        return !outUserId.empty();
    }

    vector<CloudGameInfo> scanCloudGames(const string &userDataPath, const map<int, string> &manifests)
    {
        vector<CloudGameInfo> games;

        for (const auto &entry : fs::directory_iterator(userDataPath))
        {
            if (!entry.is_directory())
            {
                continue;
            }
            string dirName = entry.path().filename().string();
            if (!isAllDigits(dirName))
            {
                continue;
            }
            int appId = 0;
            try
            {
                appId = stoi(dirName);
            }
            catch (const exception &)
            {
                continue;
            }

            string remotecachePath = entry.path().string() + "/remotecache.vdf";
            if (!fs::exists(remotecachePath))
            {
                continue;
            }

            FileUtility fileUtility;
            string content;
            try
            {
                content = fileUtility.readFileContents(remotecachePath);
            }
            catch (const exception &)
            {
                continue;
            }

            vector<CloudFileInfo> files;
            parseRemotecache(content, files);
            if (files.empty())
            {
                continue;
            }

            CloudGameInfo info;
            info.appId = appId;
            info.fileCount = static_cast<int>(files.size());
            for (const auto &file : files)
            {
                info.totalSize += file.size;
            }

            auto manifestIt = manifests.find(appId);
            if (manifestIt != manifests.end())
            {
                info.installed = true;
                try
                {
                    string manifestContent = fileUtility.readFileContents(manifestIt->second);
                    info.name = extractVdfValue(manifestContent, "name");
                }
                catch (const exception &)
                {
                }
            }

            games.push_back(info);
        }

        sort(games.begin(), games.end(),
             [](const CloudGameInfo &a, const CloudGameInfo &b)
             { return a.appId < b.appId; });
        return games;
    }

#ifdef _WIN32
    // Defined further below; declared here so the restart helpers can use it.
    void printLastError(const string &action);

    bool isSteamRunning()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        bool running = false;
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0)
                {
                    running = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return running;
    }

    // Lists every running steam.exe process (the client plus any game session,
    // which Steam spawns as a parented "steam.exe -applaunch <id>" host).
    bool getSteamExePids(vector<DWORD> &outPids)
    {
        outPids.clear();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0)
                {
                    outPids.push_back(entry.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return true;
    }

    // A game is in session when a steam.exe process's parent is ALSO a
    // steam.exe process (the client spawning the -applaunch host).
    bool isSteamInGame()
    {
        vector<DWORD> pids;
        if (!getSteamExePids(pids) || pids.empty())
        {
            return false;
        }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        bool inGame = false;
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0 &&
                    find(pids.begin(), pids.end(), entry.th32ParentProcessID) != pids.end())
                {
                    inGame = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return inGame;
    }

    // Sends Valve's own graceful client shutdown command line.
    void startSteamShutdown(const string &steamRoot)
    {
        // Windows process creation is happiest with backslash paths.
        string rootBackslash = steamRoot;
        replace(rootBackslash.begin(), rootBackslash.end(), '/', '\\');
        // Built with char appends so there is no ambiguity in the escaping:
        // the command line must be: "<root>\steam.exe" -shutdown
        string commandLine;
        commandLine += '"';
        commandLine += rootBackslash;
        commandLine += '\\';
        commandLine += "steam.exe";
        commandLine += '"';
        commandLine += " -shutdown";
        STARTUPINFOA startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo = {};
        if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, steamRoot.c_str(), &startupInfo, &processInfo))
        {
            cout << ">Error: \"" << commandLine << "\" (workdir " << steamRoot << ")" << endl;
            printLastError("Starting steam.exe -shutdown");
        }
        if (processInfo.hProcess)
        {
            CloseHandle(processInfo.hProcess);
        }
        if (processInfo.hThread)
        {
            CloseHandle(processInfo.hThread);
        }
    }

    // Last-resort fallback: force-close the Steam client with taskkill, the
    // same approach the reference Steam Cloud File Manager uses on Windows.
    // Skips Steam's graceful config flush, so the post-exit revert check
    // still runs after the wait completes.
    void forceKillSteam()
    {
        STARTUPINFOA startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo = {};
        string commandLine = "taskkill /F /IM steam.exe";
        if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo))
        {
            printLastError("Force-closing Steam with taskkill");
        }
        if (processInfo.hProcess)
        {
            CloseHandle(processInfo.hProcess);
        }
        if (processInfo.hThread)
        {
            CloseHandle(processInfo.hThread);
        }
    }

    // Polls until no steam.exe process remains (Steam writes its config files
    // BEFORE exiting, so this is the point after which edits are safe).
    bool waitForSteamExit(int timeoutMs)
    {
        vector<DWORD> pids;
        for (int waited = 0; waited <= timeoutMs; waited += 500)
        {
            if (getSteamExePids(pids) && pids.empty())
            {
                return true;
            }
            Sleep(500);
        }
        return false;
    }

    bool startSteamClient(const string &steamRoot)
    {
        string exePath = steamRoot + "/steam.exe";
        if (!fs::exists(exePath))
        {
            return false;
        }
        // Windows process creation is happiest with backslash paths.
        string exePathBackslash = exePath;
        replace(exePathBackslash.begin(), exePathBackslash.end(), '/', '\\');
        // Built with char appends so there is no ambiguity in the escaping:
        // the command line must be: "<exePath>"
        string commandLine;
        commandLine += '"';
        commandLine += exePathBackslash;
        commandLine += '"';
        STARTUPINFOA startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo = {};
        bool started = CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0,
                                      nullptr, steamRoot.c_str(), &startupInfo, &processInfo);
        if (!started)
        {
            printLastError("Starting the Steam client");
        }
        if (processInfo.hProcess)
        {
            CloseHandle(processInfo.hProcess);
        }
        if (processInfo.hThread)
        {
            CloseHandle(processInfo.hThread);
        }
        return started;
    }

    // Reads the target account's "Timestamp" value (unix seconds of the last
    // login) from config/loginusers.vdf. Steam updates it on every sign-in,
    // which makes it a reliable "the same user is logged in again" signal.
    long long readUserTimestamp(const string &steamRoot, const string &userId)
    {
        string loginusersPath = steamRoot + "/config/loginusers.vdf";
        if (!fs::exists(loginusersPath))
        {
            return 0;
        }
        string content;
        try
        {
            content = FileUtility().readFileContents(loginusersPath);
        }
        catch (const exception &)
        {
            return 0;
        }

        // loginusers.vdf keys blocks by SteamID64, but userId here is the Steam3
        // ID used by userdata folders - convert before comparing.
        string id64ToMatch;
        try
        {
            id64ToMatch = to_string(STEAM_ID64_BASE + stoull(userId));
        }
        catch (const exception &)
        {
            return 0;
        }

        istringstream stream(content);
        string rawLine;
        bool inBlock = false;
        while (getline(stream, rawLine))
        {
            string line = rawLine;
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == string::npos)
            {
                continue;
            }
            size_t end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);

            bool looksLikeId64 = line.size() >= 2 && line.front() == '"' && line.back() == '"' &&
                                 line.size() <= 21 && isAllDigits(line.substr(1, line.size() - 2));
            if (looksLikeId64)
            {
                inBlock = line.compare(1, line.size() - 2, id64ToMatch) == 0;
                continue;
            }
            if (inBlock && line == "}")
            {
                inBlock = false;
                continue;
            }
            if (inBlock)
            {
                // Modern Steam writes the key lowercase ("timestamp"); older
                // versions used "Timestamp". Accept both spellings.
                string value = extractVdfValue(line, "Timestamp");
                if (value.empty())
                {
                    value = extractVdfValue(line, "timestamp");
                }
                if (!value.empty())
                {
                    try
                    {
                        return stoll(value);
                    }
                    catch (const exception &)
                    {
                        return 0;
                    }
                }
            }
        }
        return 0;
    }

    // Waits until Steam is running again AND the same account's login timestamp
    // advanced past the pre-restart baseline, proving that user is signed in.
    bool waitForSteamLogon(const string &steamRoot, const string &userId,
                           long long previousTimestamp, int timeoutMs)
    {
        int lastDotAt = -1;
        for (int waited = 0; waited <= timeoutMs; waited += 500)
        {
            if (isSteamRunning())
            {
                long long stamp = readUserTimestamp(steamRoot, userId);
                if (stamp > previousTimestamp)
                {
                    cout << endl;
                    return true;
                }
            }
            if (waited / 2000 != lastDotAt)
            {
                cout << "." << flush;
                lastDotAt = waited / 2000;
            }
            Sleep(500);
        }
        cout << endl;
        return false;
    }

    // Flips EVERY "CloudEnabled" leaf in the file from "0" to "1" in place:
    // the global switch inside the "Steam" block AND the per-game switch of
    // every app in its "apps" {} block. Only meaningful on a normalized
    // sharedconfig (option 1's writer emits exactly those switches as "0").
    // Returns the number of switches flipped, 0 when they were already all on,
    // or -1 when the file could not be read or written.
    int enableCloudEverywhere(const string &sharedConfigPath)
    {
        try
        {
            string content = FileUtility().readFileContents(sharedConfigPath);
            const size_t cloudKeyLen = sizeof("\"CloudEnabled\"") - 1; // 14
            int flipped = 0;
            size_t pos = 0;
            while (true)
            {
                size_t keyPos = content.find("\"CloudEnabled\"", pos);
                if (keyPos == string::npos)
                {
                    break;
                }
                size_t valQ1 = content.find('"', keyPos + cloudKeyLen);
                if (valQ1 == string::npos || valQ1 + 1 >= content.size())
                {
                    break;
                }
                if (content[valQ1 + 1] == '0')
                {
                    content[valQ1 + 1] = '1';
                    flipped++;
                }
                pos = valQ1 + 2;
            }
            if (flipped == 0)
            {
                return 0;
            }
            ofstream outFile(sharedConfigPath, ios::trunc | ios::binary);
            if (!outFile)
            {
                return -1;
            }
            outFile << content;
            return flipped;
        }
        catch (const exception &e)
        {
            cout << ">Error: could not update sharedconfig.vdf: " << e.what() << endl;
            return -1;
        }
    }

    // Full automatic Steam client restart:
    //   * refused (with a warning) while a game session is running;
    //   * normally: graceful "steam.exe -shutdown", and if the client ignores
    //     it, force-closes Steam with taskkill (the reference Steam Cloud File
    //     Manager's approach); after the exit, a normalized sharedconfig whose
    //     path is passed in fixSharedConfig gets its cloud switches (global and
    //     per-game) re-applied if Steam reverted them on exit;
    //   * with preserveDiskEdits (used after deletions + option 1/2 cleanup):
    //     skips the graceful shutdown entirely and force-closes right away,
    //     because Steam's in-memory state IS the stale state those files just
    //     corrected - a graceful exit would write it back over the fixes.
    //   * relaunches and waits for the same user to be signed back in.
    // Returns true when Steam is running and that user is signed in again.
    bool restartSteamAutomatically(const string &steamRoot, const string &userId,
                                   const string &personaName, const string &fixSharedConfig,
                                   bool preserveDiskEdits)
    {
        auto startAndAwaitLogon = [&]() -> bool
        {
            long long baseline = readUserTimestamp(steamRoot, userId);
            if (!startSteamClient(steamRoot))
            {
                cout << ">Error: could not start the Steam client. Start Steam manually," << endl;
                cout << ">then run option 4 again." << endl;
                return false;
            }
            cout << ">Waiting for Steam to start and log " << personaName
                 << " in (up to 4 minutes)" << endl;
            if (!waitForSteamLogon(steamRoot, userId, baseline, 240000))
            {
                cout << ">Could not confirm the Steam login within 4 minutes. If Steam is" << endl;
                cout << ">still waiting for a password, log in, then run option 4 again." << endl;
                return false;
            }
            cout << ">Steam is running again and signed in as " << personaName << "." << endl;
            return true;
        };

        if (!isSteamRunning())
        {
            cout << endl;
            cout << ">Steam is not running. Starting it now..." << endl;
            return startAndAwaitLogon();
        }
        if (isSteamInGame())
        {
            cout << endl;
            cout << ">WARNING: Steam was NOT restarted automatically because a game appears" << endl;
            cout << ">to be running (a parented steam.exe -applaunch host was found). Close" << endl;
            cout << ">and reopen Steam when you can: until that restart the corrected" << endl;
            cout << ">settings are not the ones Steam is actually using." << endl;
            return false;
        }

        cout << endl;
        if (preserveDiskEdits)
        {
            cout << ">Force-closing the Steam client so the corrected config files are" << endl;
            cout << ">kept (a graceful exit would let Steam rewrite them from its old" << endl;
            cout << ">in-memory state)..." << endl;
            forceKillSteam();
            if (!waitForSteamExit(60000))
            {
                cout << ">Error: Steam did not shut down within 60 seconds. Restart Steam" << endl;
                cout << ">manually, then run option 4 again." << endl;
                return false;
            }
            return startAndAwaitLogon();
        }

        cout << ">Restarting the Steam client automatically so the new setting takes effect..." << endl;
        startSteamShutdown(steamRoot);
        if (!waitForSteamExit(15000))
        {
            // steam.exe -shutdown can be ignored while a download or dialog
            // blocks the client; fall back to the force-close.
            cout << ">Steam did not respond to the graceful shutdown command;" << endl;
            cout << ">force-closing it (this skips Steam's config flush on exit)..." << endl;
            forceKillSteam();
            if (!waitForSteamExit(45000))
            {
                cout << ">Error: Steam did not shut down within 60 seconds. Restart Steam" << endl;
                cout << ">manually, then run option 4 again." << endl;
                return false;
            }
        }

        // Steam rewrites its config files on exit and can flip the cloud
        // switches back off with its old in-memory values; fix them all up.
        if (!fixSharedConfig.empty())
        {
            int reEnabled = enableCloudEverywhere(fixSharedConfig);
            if (reEnabled > 0)
            {
                cout << ">Steam re-applied its old cloud settings on exit; turning them" << endl;
                cout << ">back on globally and for every game (the file is already" << endl;
                cout << ">normalized)..." << endl;
                cout << ">Cloud switches re-applied successfully." << endl;
            }
            else if (reEnabled < 0)
            {
                cout << ">Could not re-apply the cloud switches: enable Steam Cloud" << endl;
                cout << ">manually in Steam -> Settings -> Cloud after Steam restarts." << endl;
            }
        }
        return startAndAwaitLogon();
    }

    string getExecutablePath()
    {
        char path[MAX_PATH];
        DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            return "";
        }
        return string(path);
    }

    string getExecutableDirectory()
    {
        string exePath = getExecutablePath();
        if (exePath.empty())
        {
            return ".";
        }
        size_t lastSlash = exePath.find_last_of("\\/");
        return lastSlash == string::npos ? "." : exePath.substr(0, lastSlash);
    }

    // ---- Real top-level option 1 / option 2 ----
    // Mirrors the main-menu implementations exactly (all libraries, ALL users
    // in the userdata tree), so option 4's cleanup runs the genuine,
    // fully-featured options instead of a per-user subset.
    bool runTopLevelOptionOne(const string &steamRoot, const vector<string> &libraryPaths)
    {
        string userDataPath = steamRoot + "/userdata";
        if (!fs::exists(userDataPath))
        {
            cout << ">No userdata folder found, skipping Steam Cloud settings" << endl;
            return false;
        }
        FileUtility fileUtility;
        vector<int> combinedAcfIds;
        for (const string &lib : libraryPaths)
        {
            string sAppsPath = lib + "/steamapps";
            if (fs::exists(sAppsPath))
            {
                vector<int> ids = fileUtility.getAcfID(sAppsPath);
                combinedAcfIds.insert(combinedAcfIds.end(), ids.begin(), ids.end());
            }
        }
        string acfIds = fileUtility.sortAcfID(combinedAcfIds);
        if (acfIds.empty())
        {
            cout << ">There are no games in your steamapps folders" << endl;
            return false;
        }
        CloudDisabler cloudDisabler;
        bool ok = cloudDisabler.iterateSharedConfig(userDataPath, acfIds);
        cout << ">Success" << endl;
        return ok;
    }

    bool runTopLevelOptionTwo(const vector<string> &libraryPaths)
    {
        bool foundAny = false;
        AutoUpdateDisabler autoUpdateDisabler;
        for (const string &lib : libraryPaths)
        {
            string sAppsPath = lib + "/steamapps";
            if (fs::exists(sAppsPath))
            {
                cout << ">Processing directory: " << sAppsPath << endl;
                autoUpdateDisabler.iterateSteamApps(sAppsPath);
                foundAny = true;
            }
        }
        if (!foundAny)
        {
            cout << ">There are no games in your steamapps folders" << endl;
            return false;
        }
        cout << ">Success" << endl;
        return true;
    }

    // Waits until Steam's local remotecache.vdf no longer lists the deleted
    // files (the client rewrites it once the deletion syncs). A timeout of 0
    // performs a single immediate check. Returns true when the cache caught up.
    bool waitForRemotecacheUpdate(const string &remotecachePath, const vector<string> &deletedNames, int timeoutMs)
    {
        for (int waited = 0; waited <= timeoutMs; waited += 300)
        {
            bool allGone = true;
            try
            {
                FileUtility fileUtility;
                vector<CloudFileInfo> files;
                parseRemotecache(fileUtility.readFileContents(remotecachePath), files);
                set<string> remaining;
                for (const auto &file : files)
                {
                    remaining.insert(file.name);
                }
                for (const auto &name : deletedNames)
                {
                    if (remaining.count(name) != 0)
                    {
                        allGone = false;
                        break;
                    }
                }
            }
            catch (const exception &)
            {
                allGone = false;
            }
            if (allGone)
            {
                return true;
            }
            Sleep(300);
        }
        return false;
    }
#endif

    // Prints a one-line hint (and Windows console code) for a failed API call.
    void printLastError(const string &action)
    {
#ifdef _WIN32
        DWORD errorCode = GetLastError();
        char message[256];
        if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           message, sizeof(message), nullptr) > 0)
        {
            string text(message);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            {
                text.pop_back();
            }
            cout << ">Error: " << action << " failed: " << text << endl;
            return;
        }
        cout << ">Error: " << action << " failed (code " << errorCode << ")" << endl;
#else
        (void)action;
#endif
    }

    int promptMenuChoice(const string &prompt)
    {
        cout << prompt;
        cout.flush();
        string input;
        if (!getline(cin, input))
        {
            return -1;
        }
        int number = 0;
        if (!parsePromptNumber(input, number))
        {
            return -2;
        }
        return number;
    }

#ifdef _WIN32
    // Flat C API function pointers resolved from steam_api64.dll.
    struct SteamApiFunctions
    {
        SteamAPI_InitFlat_t initFlat = nullptr;
        SteamAPI_Shutdown_t shutdown = nullptr;
        SteamAPI_RunCallbacks_t runCallbacks = nullptr;
        SteamAPI_SteamRemoteStorage_v016_t remoteStorageAccessor = nullptr;
        SteamAPI_ISteamRemoteStorage_FileDelete_t fileDelete = nullptr;
        SteamAPI_ISteamRemoteStorage_FileForget_t fileForget = nullptr;
        SteamAPI_ISteamRemoteStorage_FileExists_t fileExists = nullptr;
        SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp_t isCloudEnabledForApp = nullptr;
        SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp_t setCloudEnabledForApp = nullptr;

        bool valid() const
        {
            return initFlat && shutdown && runCallbacks && remoteStorageAccessor &&
                   fileDelete && fileForget && fileExists && isCloudEnabledForApp && setCloudEnabledForApp;
        }
    };

    SteamApiFunctions loadSteamApi(HMODULE &outModule)
    {
        SteamApiFunctions api;
        string dllPath = getExecutableDirectory() + "\\steam_api64.dll";
        outModule = LoadLibraryA(dllPath.c_str());
        if (!outModule)
        {
            printLastError("Loading steam_api64.dll");
            cout << ">Make sure steam_api64.dll is in the same folder as this program." << endl;
            return api;
        }

        api.initFlat = reinterpret_cast<SteamAPI_InitFlat_t>(GetProcAddress(outModule, "SteamAPI_InitFlat"));
        api.shutdown = reinterpret_cast<SteamAPI_Shutdown_t>(GetProcAddress(outModule, "SteamAPI_Shutdown"));
        api.runCallbacks = reinterpret_cast<SteamAPI_RunCallbacks_t>(GetProcAddress(outModule, "SteamAPI_RunCallbacks"));
        api.remoteStorageAccessor = reinterpret_cast<SteamAPI_SteamRemoteStorage_v016_t>(GetProcAddress(outModule, "SteamAPI_SteamRemoteStorage_v016"));
        api.fileDelete = reinterpret_cast<SteamAPI_ISteamRemoteStorage_FileDelete_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_FileDelete"));
        api.fileForget = reinterpret_cast<SteamAPI_ISteamRemoteStorage_FileForget_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_FileForget"));
        api.fileExists = reinterpret_cast<SteamAPI_ISteamRemoteStorage_FileExists_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_FileExists"));
        api.isCloudEnabledForApp = reinterpret_cast<SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp"));
        api.setCloudEnabledForApp = reinterpret_cast<SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp"));

        if (!api.valid())
        {
            cout << ">Error: steam_api64.dll is missing required Steamworks exports." << endl;
        }
        return api;
    }

    // Post-deletion follow-up. Steam does not always apply deletions
    // immediately, and every completed deletion re-queues the game's update
    // and re-enables its per-game cloud switch, so this prints the
    // double-check warning and offers the standard cleanup: the real
    // top-level option 1 first, then the real top-level option 2
    // (sequentially, in that order).
    //
    // Returns true when the user ran the option 1 + 2 cleanup. That cleanup
    // disables Steam Cloud GLOBALLY (option 1's writer turns the Steam-level
    // "CloudEnabled" off), so the caller must NOT loop back into the cloud
    // game list afterwards: a fresh list would let further deletions queue
    // but never upload. It returns false when the user skipped the cleanup -
    // cloud is still set up the way option 4 left it (global and every game
    // switch on), so looping straight back to the list is safe there.
    bool postDeletionFollowUp(const string &steamRoot, const string &userId,
                              const string &personaName, const vector<string> &libraryPaths)
    {
        cout << ">Warning: Steam does not always apply deletions immediately - the file" << endl;
        cout << ">may NOT actually be gone even though the deletion reported success." << endl;
        cout << ">Double-check at: https://store.steampowered.com/account/remotestorage" << endl;
        cout << ">If the file still shows there, delete it again with option 4 and keep" << endl;
        cout << ">retrying until it no longer appears - it will eventually stay deleted." << endl;
        cout << ">Also check that Steam Cloud is enabled globally in the Steam settings;" << endl;
        cout << ">If the cloud file does not delete, manually resync the cloud files by pressing play (cloud conflict will occur, choose cloud over local save file), and try deleting again." << endl;

        cout << endl;
        cout << ">Deleting cloud data queues an update for this game and re-enables its" << endl;
        cout << ">per-game Steam Cloud setting. Run option 1 (disable per-game cloud) and" << endl;
        cout << ">then option 2 (disable auto-updates) now, in one go? (Will restart Steam)" << endl;
        cout << ">Enter 1 for yes, anything else to skip: " << endl;
        cout << ">";
        string cleanupInput;
        if (!getline(cin, cleanupInput))
        {
            return false;
        }
        cout << endl;
        if (cleanupInput != "1")
        {
            return false;
        }

        cout << ">Running option 1 (disable Steam Cloud per-game)..." << endl;
        runTopLevelOptionOne(steamRoot, libraryPaths);
        cout << ">Option 1 done." << endl;

        cout << ">Running option 2 (disable auto-updates and unschedule updates)..." << endl;
        runTopLevelOptionTwo(libraryPaths);
        cout << ">Option 2 done." << endl;

        // Restart the Steam client automatically - the user no longer has to
        // do it. This deliberately force-closes: Steam's in-memory state is
        // exactly what the deletion just re-enabled, and a graceful exit
        // would write that stale state back over the files options 1 and 2
        // just fixed, undoing them before Steam ever re-reads them.
        cout << endl;
        restartSteamAutomatically(steamRoot, userId, personaName, "", /*preserveDiskEdits*/ true);
        return true;
    }

    // Explains why the loop must end after a cleanup instead of showing the
    // cloud game list again: the cleanup left Steam Cloud off GLOBALLY, so
    // the list would be a trap (deletions queue but never upload). Returning
    // to the main menu means the next option 4 entry re-runs the cloud setup,
    // which re-enables every switch and restarts Steam first.
    void returnToMainMenuAfterCleanup()
    {
        cout << endl;
        cout << ">That cleanup disabled Steam Cloud globally, so option 4 cannot keep" << endl;
        cout << ">deleting until it is re-enabled - staying in the game list here would" << endl;
        cout << ">leave further deletions queued but never uploaded." << endl;
        cout << ">Returning to the main menu. The next option 4 run re-enables Steam" << endl;
        cout << ">Cloud globally and for every game, then restarts Steam automatically" << endl;
        cout << ">before showing the list again." << endl;
    }
#endif
} // namespace

CloudDataDeleter::CloudDataDeleter() {}

// Worker mode: this same exe is spawned by run() as a short-lived child
// ("--cloud-worker <appid> <filelist>"). It connects to Steam, deletes the
// listed cloud files, and EXITS. The process exit is what makes the Steam
// client end the app session and flush the deletions to the cloud - keeping
// the session alive (e.g. a menu loop) leaves Steam "in-game" and the
// deletions never sync. SteamCloudFileManager's worker does exactly this.
int runCloudWorker(int argc, char *argv[])
{
#ifdef _WIN32
    if (argc < 4)
    {
        cout << ">Usage: --cloud-worker <appid> <filelist>" << endl;
        return 2;
    }

    string appIdString = argv[2];
    if (!isAllDigits(appIdString))
    {
        cout << ">Invalid appid: " << appIdString << endl;
        return 2;
    }        // Read the file list (one "<root>|<name>" entry per line; root 0 =
        // API-managed file, root != 0 = auto-cloud file, display only).
    vector<string> filenames;
    {
        ifstream listFile(argv[3]);
        if (!listFile)
        {
            cout << ">Error: could not read file list: " << argv[3] << endl;
            return 2;
        }
        string line;
        while (getline(listFile, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            {
                line.pop_back();
            }
            if (!line.empty())
            {
                filenames.push_back(line);
            }
        }
    }
    if (filenames.empty())
    {
        cout << ">Error: empty file list" << endl;
        return 2;
    }

    // Steam picks the app id up from these before init (same trick SCFM uses).
    SetEnvironmentVariableA("SteamAppId", appIdString.c_str());
    SetEnvironmentVariableA("SteamGameId", appIdString.c_str());

    // steam_appid.txt next to the exe is the fallback Steam uses when the env
    // vars are not picked up; written before init and removed after.
    string appidFilePath = getExecutableDirectory() + "\\steam_appid.txt";
    {
        ofstream appidFile(appidFilePath, ios::trunc);
        if (appidFile)
        {
            appidFile << appIdString;
        }
    }

    HMODULE steamApiModule = nullptr;
    SteamApiFunctions api = loadSteamApi(steamApiModule);
    if (!steamApiModule || !api.valid())
    {
        DeleteFileA(appidFilePath.c_str());
        return 1;
    }

    char initErrMsg[1024] = {0};
    int initResult = api.initFlat(initErrMsg);
    if (initResult != 0) // k_ESteamAPIInitResult_OK
    {
        cout << ">Error: could not connect to Steam for app " << appIdString
             << " (InitFlat result " << initResult
             << (initErrMsg[0] != '\0' ? ": " + string(initErrMsg) : "") << ")." << endl;
        cout << ">Make sure Steam is running and logged in, then try again. If Steam already" << endl;
        cout << ">holds an app session for this app (e.g. a running game or the client" << endl;
        cout << ">itself for appid 7), the attach attempt can fail:" << endl;
        cout << ">close that session or retry after a client restart." << endl;
        DeleteFileA(appidFilePath.c_str());
        FreeLibrary(steamApiModule);
        return 1;
    }

    void *remoteStorage = api.remoteStorageAccessor();
    if (!remoteStorage)
    {
        cout << ">Error: could not access Steam Remote Storage interface." << endl;
        api.shutdown();
        DeleteFileA(appidFilePath.c_str());
        FreeLibrary(steamApiModule);
        return 1;
    }

    // Make sure per-app cloud sync is on, otherwise deletions for
    // not-installed games are refused and local auto-cloud deletions are not
    // synced (same as SCFM's ensure_cloud_enabled).
    if (!api.isCloudEnabledForApp(remoteStorage))
    {
        api.setCloudEnabledForApp(remoteStorage, true);
    }

    // List entries are "<root>|<name>". Root 0 files are deleted through the
    // API; auto-cloud (root != 0) names are display-only - the API cannot see
    // them, the parent already deleted their local copies, and Steam uploads
    // those deletions when this session exits. They must never be reported
    // as API-deleted here.
    int successCount = 0;
    int failCount = 0;
    for (const auto &entry : filenames)
    {
        string filename = entry;
        bool isUfs = false;
        size_t sep = entry.find('|');
        if (sep != string::npos)
        {
            string prefix = entry.substr(0, sep);
            if (prefix.size() <= 3 && isAllDigits(prefix))
            {
                int root = atoi(prefix.c_str());
                filename = entry.substr(sep + 1);
                isUfs = (root != 0);
            }
        }
        if (isUfs)
        {
            cout << ">Auto-cloud file (local deleted, syncs on exit): " << filename << endl;
            continue;
        }

        bool deleted = api.fileDelete(remoteStorage, filename.c_str());
        bool forgotten = api.fileForget(remoteStorage, filename.c_str());
        // The delete calls return false for files that are already gone (e.g.
        // a deletion queued earlier that only synced now), so verify the goal
        // state via FileExists before reporting failure.
        bool stillExists = api.fileExists(remoteStorage, filename.c_str());
        if (deleted || forgotten)
        {
            successCount++;
            cout << ">Deleted: " << filename << endl;
        }
        else if (!stillExists)
        {
            successCount++;
            cout << ">Verified gone from cloud (no delete needed): " << filename << endl;
        }
        else
        {
            failCount++;
            cout << ">Failed: " << filename << " (still present after delete attempt)" << endl;
        }
    }

    // Let Steam process the deletion callbacks before the session ends.
    api.runCallbacks();
    Sleep(300);
    api.runCallbacks();

    api.shutdown();
    DeleteFileA(appidFilePath.c_str());
    FreeLibrary(steamApiModule);

    cout << ">Cloud worker done: " << successCount << " deleted"
         << (failCount > 0 ? ", " + to_string(failCount) + " failed" : "")
         << ". Exiting so Steam can sync." << endl;
    // Exiting this process is what ends the app session and triggers the
    // Steam client to upload the deletions.
    return failCount > 0 ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    cout << ">Error: Steam Cloud deletion is only supported on Windows builds." << endl;
    return 1;
#endif
}

void CloudDataDeleter::run(const string &steamRoot, const vector<string> &libraryPaths)
{
    string userId;
    string personaName;
    if (!resolveCurrentUser(steamRoot, userId, personaName))
    {
        cout << ">Error: could not determine the current Steam user from config/loginusers.vdf" << endl;
        return;
    }

    cout << ">Using Steam user: " << personaName << " (" << userId << ")" << endl;
    cout << ">(Note: only the currently signed-in user is supported. To manage a different user, switch accounts in Steam and run this again.)" << endl;

    string userDataPath = steamRoot + "/userdata/" + userId;
    if (!fs::exists(userDataPath))
    {
        cout << ">Error: userdata folder not found for user " << userId << endl;
        return;
    }

    // Steam Cloud has to be ON both globally (the "CloudEnabled" leaf inside
    // sharedconfig.vdf's "Steam" block) and per game (one "CloudEnabled" per
    // app in that block's "apps" {}). If either is off the client queues
    // deletions without ever uploading them (observed in logs/cloud_log.txt)
    // or holds a local pending state it never syncs (cloud desync).
    //
    // The whole setup therefore runs on EVERY option 4 entry, unconditionally:
    // top-level option 1 normalizes sharedconfig.vdf (its writer emits every
    // switch as "0"), all switches are then flipped back to "1" - global and
    // per game - and Steam is restarted so it re-reads them. Checking only the
    // global switch first would miss the file state this is meant to prevent:
    // global on, every per-game switch off.
    string sharedConfigPath = userDataPath + "/7/remote/sharedconfig.vdf";
    if (fs::exists(sharedConfigPath))
    {
        cout << ">Preparing Steam Cloud: deletions only sync while cloud is enabled" << endl;
        cout << ">globally and for the game, so both are set up now, every run." << endl;
        cout << endl;
        cout << ">Running the real top-level option 1 first (all libraries, all users):" << endl;
        cout << ">this normalizes sharedconfig.vdf and writes every cloud switch as off." << endl;
        cout << endl;
        runTopLevelOptionOne(steamRoot, libraryPaths);

        // The file is now canonical: the first "CloudEnabled" is the global
        // switch and every app entry holds a per-game one, all written as "0".
        cout << ">Turning Steam Cloud back on globally and for every game..." << endl;
        if (enableCloudEverywhere(sharedConfigPath) < 0)
        {
            cout << ">Could not enable Steam Cloud automatically. Please turn it on" << endl;
            cout << ">manually in Steam -> Settings -> Cloud, restart Steam, and run" << endl;
            cout << ">option 4 again." << endl;
            return;
        }
        cout << ">Steam Cloud is enabled globally and for every game." << endl;

        // Steam only re-reads these settings at startup, so bounce the client
        // right here instead of making the user press option 4 twice.
#ifdef _WIN32
        if (!restartSteamAutomatically(steamRoot, userId, personaName, sharedConfigPath,
                                       /*preserveDiskEdits*/ false))
        {
            return;
        }
#else
        cout << endl;
        cout << ">Restart Steam manually for the new settings to take effect, then run" << endl;
        cout << ">option 4 again." << endl;
        return;
#endif
        // Steam is back up with the global and per-game switches in effect -
        // fall through to the cloud game list instead of the main menu.
    }

    map<int, string> manifests = buildManifestMap(libraryPaths);

    while (true)
    {
        vector<CloudGameInfo> games = scanCloudGames(userDataPath, manifests);
        if (games.empty())
        {
            cout << ">No games with Steam Cloud data found for this user" << endl;
            return;
        }

        cout << endl;
        cout << ">Cloud game library (games with Steam Cloud data):" << endl;
        int number = 1;
        for (const auto &game : games)
        {
            cout << ">" << number << ". " << game.appId << " - "
                 << (game.name.empty() ? "(unknown name)" : game.name)
                 << " [" << (game.installed ? "installed" : "not installed") << "] - "
                 << game.fileCount << (game.fileCount == 1 ? " file, " : " files, ")
                 << formatSize(game.totalSize) << endl;
            number++;
        }
        cout << ">0. Back to main menu" << endl;
        cout << ">Select the number of the game: " << endl;

        string input;
        cout << ">";
        if (!getline(cin, input))
        {
            return;
        }
        cout << endl;

        int choice = 0;
        if (!parsePromptNumber(input, choice))
        {
            cout << ">Invalid input" << endl;
            continue;
        }
        if (choice == 0)
        {
            return;
        }
        if (choice < 1 || choice > static_cast<int>(games.size()))
        {
            cout << ">Invalid input" << endl;
            continue;
        }

        CloudGameInfo game = games[choice - 1];

        // Manifest path for this game (used for the GameInstall UFS root).
        string manifestPath;
        {
            auto manifestIt = manifests.find(game.appId);
            if (manifestIt != manifests.end())
            {
                manifestPath = manifestIt->second;
            }
        }

        // Re-read the file list fresh for the selected game.
        string remotecachePath = userDataPath + "/" + to_string(game.appId) + "/remotecache.vdf";
        FileUtility fileUtility;
        vector<CloudFileInfo> files;
        try
        {
            parseRemotecache(fileUtility.readFileContents(remotecachePath), files);
        }
        catch (const exception &)
        {
            cout << ">Error: could not read remotecache.vdf for app " << game.appId << endl;
            continue;
        }
        if (files.empty())
        {
            cout << ">No cloud files listed for app " << game.appId << endl;
            continue;
        }

        cout << ">Cloud files for " << game.appId
             << (game.name.empty() ? "" : " - " + game.name) << ":" << endl;
        cout << "(no root note = API-managed cloud file; a root note marks an auto-cloud" << endl;
        cout << "> file synced from that local folder - deleting one removes the local copy" << endl;
        cout << "> and Steam syncs the deletion to the cloud)" << endl;
        int fileNumber = 1;
        bool anyGhostStub = false;
        for (const auto &file : files)
        {
            cout << ">" << fileNumber << ". " << file.name << " (" << formatSize(file.size);
            if (file.root != 0)
            {
                cout << ", root: " << rootNameForRoot(file.root);
            }
            if (file.root == 0 && file.size == 0)
            {
                cout << ", 0-byte local record";
                anyGhostStub = true;
            }
            cout << ")" << endl;
            fileNumber++;
        }
        if (anyGhostStub)
        {
            // App-222900-style entry (live-verified): the cloud holds a REAL
            // file (2.67 KB per the web console) while the local record reads
            // 0 B - the client never synced it, so the API view no longer even
            // knows the file exists and a FileDelete can report success
            // without touching the real object. Set expectations here.
            cout << ">Note: entries marked \"0-byte local record\" have an out-of-sync local" << endl;
            cout << ">sync record: either an empty tombstone, or a REAL cloud file this" << endl;
            cout << ">client never synced. Either way a delete attempt here can report" << endl;
            cout << ">success without touching the real cloud object. Offline fix first:" << endl;
            cout << ">launch the game (click Play); Steam then shows the cloud conflict" << endl;
            cout << ">dialog - choose  STEAM CLOUD  (not local) so the client pulls the" << endl;
            cout << ">real file into the local record, then re-run the deletion here." << endl;
            cout << ">If the game cannot be launched or no conflict dialog appears, use" << endl;
            cout << ">the web console instead (its Delete button acts on the real file," << endl;
            cout << ">bypassing the local record); verify any change there as well." << endl;
        }
        cout << ">" << (files.size() + 1) << ". Delete ALL files listed above" << endl;
        cout << ">0. Back to game list" << endl;

        int fileChoice = promptMenuChoice(">Select your option: " + string("\n>"));
        if (fileChoice == -1)
        {
            return;
        }
        if (fileChoice == -2)
        {
            cout << ">Invalid input" << endl;
            continue;
        }
        if (fileChoice == 0)
        {
            continue;
        }
        if (fileChoice < 1 || fileChoice > static_cast<int>(files.size()) + 1)
        {
            cout << ">Invalid input" << endl;
            continue;
        }

        bool deleteAll = (fileChoice == static_cast<int>(files.size()) + 1);
        vector<CloudFileInfo> selected;
        if (deleteAll)
        {
            selected = files;
        }
        else
        {
            selected.push_back(files[fileChoice - 1]);
        }

        // Split by cloud root. Root 0 files live in userdata/.../remote and
        // are deleted through the Steam API by the worker. Root != 0 files are
        // auto-cloud (UFS) files the Steam client syncs from local folders
        // (Documents, AppData, ...); the Remote Storage API cannot see them
        // (FileDelete/FileForget return false for them), so - exactly like
        // SteamCloudFileManager - the LOCAL file is deleted and Steam uploads
        // that deletion on the next app-session sync.
        vector<CloudFileInfo> apiFiles;
        vector<CloudFileInfo> ufsFiles;
        for (const auto &file : selected)
        {
            if (file.root == 0)
            {
                apiFiles.push_back(file);
            }
            else
            {
                ufsFiles.push_back(file);
            }
        }

        cout << endl;
        if (!game.installed && !ufsFiles.empty())
        {
            cout << ">Warning: this game is not installed and the selection includes" << endl;
            cout << ">auto-cloud (UFS) files. If it has also been removed from your account" << endl;
                        cout << ">(e.g. delisted from the Steam store, like Dead Island: Epidemic), there" << endl;
                        cout << ">is no local copy to delete, and Steam Support (help.steampowered.com ->" << endl;
            cout << ">search the game -> \"restore to library\") would need to restore it first." << endl;
        }
        else if (!game.installed)
        {
            // API-managed (root 0) cloud deletions do NOT require the game to
            // be installed: a live probe against uninstalled appid 222900 still
            // connected SteamAPI InitFlat and read the cloud. Only auto-cloud
            // files need a local copy, which is what the other branch is for.
            cout << ">Note: this game is not installed, but that does NOT block these" << endl;
            cout << ">deletions: API-managed cloud files are handled by a Steam remote-storage" << endl;
            cout << ">session, which still connects for uninstalled games (verified live for" << endl;
            cout << ">appid 222900)." << endl;
        }
        cout << ">About to PERMANENTLY delete " << selected.size()
             << (selected.size() == 1 ? " cloud file" : " cloud files") << " from app " << game.appId << ":" << endl;
        if (!ufsFiles.empty())
        {
            cout << ">(" << ufsFiles.size() << " auto-cloud file(s) will be deleted from their local folder;" << endl;
            cout << "> Steam then syncs those deletions to the cloud)" << endl;
        }
        for (const auto &file : selected)
        {
            cout << ">  - " << file.name << endl;
        }
        cout << ">Steam must be running; API deletions sync when the worker exits, and" << endl;
        cout << ">local auto-cloud deletions sync on Steam's next app-session sync." << endl;
        cout << ">Enter 1 to confirm deletion, anything else to cancel: " << endl;

        string confirmInput;
        cout << ">";
        if (!getline(cin, confirmInput))
        {
            continue;
        }
        cout << endl;
        if (confirmInput != "1")
        {
            cout << ">Cancelled" << endl;
            continue;
        }

        // Delete local auto-cloud (UFS) files now (platform-neutral). Steam
        // uploads these deletions when it next syncs the app session.
        int ufsDeleted = 0;
        int ufsAlreadyAbsent = 0;
        vector<string> ufsFailed;
        for (const auto &file : ufsFiles)
        {
            string localPath;
            if (!resolveLocalUfsPath(file, steamRoot, manifestPath, game.name, localPath))
            {
                ufsFailed.push_back(file.name + " (unsupported cloud root " + to_string(file.root) + ")");
                continue;
            }
            error_code ec;
            fs::path localFile(localPath);
            if (!fs::exists(localFile, ec))
            {
                // No local copy. Steam's client only propagates LOCAL deletions
                // for auto-cloud files, so if the cloud copy still exists it
                // will simply re-download - nothing we do here can delete it.
                // Not a success; tell the user how to finish the job (SCFM's
                // guidance: run the game once so Steam restores the file,
                // then delete it here again).
                cout << ">Not deleted (no local copy): " << file.name << endl;
                cout << ">  If the cloud copy still exists, Steam will re-download it on the" << endl;
                cout << ">  next sync; run the game once, then delete it here again. If it is" << endl;
                cout << ">  still listed after a refresh, the list was stale and the cloud" << endl;
                cout << ">  copy was already gone." << endl;
                ufsAlreadyAbsent++;
                continue;
            }
            fs::remove(localFile, ec);
            if (ec)
            {
                ufsFailed.push_back(file.name + " (could not delete " + localPath + ")");
            }
            else
            {
                cout << ">Deleted local auto-cloud file: " << localPath << endl;
                ufsDeleted++;
            }
        }
        if (!ufsFailed.empty())
        {
            cout << ">Warning: " << ufsFailed.size() << " auto-cloud file(s) could not be deleted locally:" << endl;
            for (const auto &f : ufsFailed)
            {
                cout << ">  - " << f << endl;
            }
        }

#ifdef _WIN32
        if (apiFiles.empty())
        {
            // Only local auto-cloud deletions were requested; the Steam API
            // worker is not needed. The Steam client syncs these deletions on
            // its next app-session sync (per-app Cloud sync must be enabled).
            cout << ">Done: " << ufsDeleted << " local auto-cloud file(s) deleted from app " << game.appId << "." << endl;
            if (ufsAlreadyAbsent > 0)
            {
                cout << ">Note: " << ufsAlreadyAbsent << " file(s) had no local copy and were NOT deleted (see above)." << endl;
            }
            if (postDeletionFollowUp(steamRoot, userId, personaName, libraryPaths))
            {
                returnToMainMenuAfterCleanup();
                return;
            }
            continue;
        }

        if (!isSteamRunning())
        {
            cout << ">Error: Steam is not running. Start Steam, log in, and try again." << endl;
            continue;
        }

        // Write the selected cloud file names to a temp list for the worker.
        string listPath;
        {
            string tempDir = fs::temp_directory_path().string();
            listPath = tempDir + "\\steam-tweak-tool-cloud-" + to_string(GetCurrentProcessId()) + ".txt";
        }
        {
            ofstream listFile(listPath, ios::trunc);
            if (!listFile)
            {
                cout << ">Error: could not create temporary file list: " << listPath << endl;
                continue;
            }
            for (const auto &file : apiFiles)
            {
                // "<root>|<name>" lets the worker tell API files (root 0)
                // apart from auto-cloud files (display-only for the latter).
                listFile << file.root << "|" << file.name << "\n";
            }
        }

        // Spawn this exe as a short-lived worker. It prints its per-file
        // results into this console and exits, which ends the Steam app
        // session and lets the client sync the deletions.
        string exePath = getExecutablePath();
        string commandLine = "\"" + exePath + "\" --cloud-worker " + to_string(game.appId) + " \"" + listPath + "\"";

        STARTUPINFOA startupInfo;
        ZeroMemory(&startupInfo, sizeof(startupInfo));
        startupInfo.cb = sizeof(startupInfo);
        // Route the child's stderr to NUL: steam_api64.dll prints un-prefixed
        // diagnostics ("Setting breakpad minidump AppID = ...") to stderr.
        SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
        HANDLE nullStderr = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        if (nullStderr != INVALID_HANDLE_VALUE)
        {
            startupInfo.dwFlags = STARTF_USESTDHANDLES;
            startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            startupInfo.hStdError = nullStderr;
        }
        PROCESS_INFORMATION processInfo;
        ZeroMemory(&processInfo, sizeof(processInfo));

        // Start the child in the exe's own directory: Steam looks for the
        // steam_appid.txt fallback in the process CWD, which only works when
        // CWD == exe dir (the SteamAppId env vars remain the primary way).
        string workingDir = getExecutableDirectory();
        cout << ">Connecting to Steam and deleting cloud files..." << endl;
        if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, workingDir.data(), &startupInfo, &processInfo))
        {
            printLastError("Launching cloud worker process");
            if (nullStderr != INVALID_HANDLE_VALUE)
            {
                CloseHandle(nullStderr);
            }
            fs::remove(listPath);
            continue;
        }

        WaitForSingleObject(processInfo.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        if (nullStderr != INVALID_HANDLE_VALUE)
        {
            CloseHandle(nullStderr);
        }
        fs::remove(listPath);

        if (exitCode == 0)
        {
            bool allApiGhosts = true;
            for (const auto &file : apiFiles)
            {
                if (file.size != 0)
                {
                    allApiGhosts = false;
                    break;
                }
            }
            cout << ">Done: " << apiFiles.size() << " cloud file(s) deleted via Steam API from app " << game.appId;
            if (ufsDeleted > 0)
            {
                cout << ", plus " << ufsDeleted << " local auto-cloud file(s) queued for sync";
            }
            cout << "." << endl;
            if (allApiGhosts)
            {
                // Live-verified failure mode (0-byte local records, e.g. appid
                // 222900's public_steamprefs.dat - the cloud side is a real
                // 2.67 KB file the API view never sees): API reports success,
                // the real cloud object survives untouched.
                cout << ">Heads-up: every file above had a 0-byte local record. The delete call" << endl;
                cout << ">reported success, but that record may not map to the real cloud object" << endl;
                cout << ">at all (a real file can exist that this client never synced). The" << endl;
                cout << ">offline fix: launch the game, accept the cloud conflict dialog, and" << endl;
                cout << ">choose STEAM CLOUD so the local record gets the real file - then" << endl;
                cout << ">re-run the deletion here. If that is not possible (delisted game, or" << endl;
                cout << ">no conflict dialog ever appears), the web console's own Delete button" << endl;
                cout << ">acts on the real object directly, and the web-console check below is" << endl;
                cout << ">the only reliable confirmation." << endl;
            }
            if (ufsAlreadyAbsent > 0)
            {
                cout << ">Note: " << ufsAlreadyAbsent << " auto-cloud file(s) had no local copy and were NOT deleted." << endl;
            }

            // Steam rewrites remotecache.vdf after the deletion syncs; wait
            // briefly so the refreshed list below is accurate.
            vector<string> deletedNames;
            for (const auto &file : apiFiles)
            {
                deletedNames.push_back(file.name);
            }
            if (!waitForRemotecacheUpdate(remotecachePath, deletedNames, 0))
            {
                cout << ">Waiting for Steam to update its local cache..." << endl;
                if (!waitForRemotecacheUpdate(remotecachePath, deletedNames, 10000))
                {
                    cout << ">Note: Steam's local cache has not caught up yet; the deleted" << endl;
                    cout << ">file(s) may still appear below for a moment." << endl;
                }
            }
            if (postDeletionFollowUp(steamRoot, userId, personaName, libraryPaths))
            {
                returnToMainMenuAfterCleanup();
                return;
            }
        }
        else if (exitCode == 1)
        {
            cout << ">Finished with failures (see above). If every file failed and this game was removed" << endl;
            cout << ">from your account (e.g. delisted), restore it via Steam Support -> \"restore to library\"." << endl;
        }
        // exitCode 2 = usage/internal error, messages already printed.
#else
        cout << ">Error: Steam Cloud deletion is only supported on Windows builds." << endl;
#endif
        // Loop back to a fresh cloud game library list.
    }
}
