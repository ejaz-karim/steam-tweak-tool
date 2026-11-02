#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ctime>
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
#include <urlmon.h>
#include <system_error>
#endif

#include "steam-tweak-tool/delete_cloud_data.hpp"
#include "steam-tweak-tool/utility.hpp"
#include "steam-tweak-tool/gameupdate_appmanifest_disable.hpp"
#include "steam-tweak-tool/cloud_disable.hpp"
#include "steam-tweak-tool/autoupdate_disable.hpp"
#include "steam-tweak-tool/top_level_actions.hpp"
#include "steam-tweak-tool/steam_chromedevtoolsprotocol.hpp"

using namespace std;
namespace fs = std::filesystem;

static const unsigned long long STEAM_ID64_BASE = 76561197960265728ULL;

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
        string sizeText;

        string downloadUrl;

        int root = 0;
    };

    struct CloudGameInfo
    {
        int appId = 0;
        string name;
        bool installed = false;
        int fileCount = 0;
        unsigned long long totalSize = 0;
        string totalSizeText;

        bool hasLocalRecord = false;
        bool cloudListed = false;
    };

#ifdef _WIN32

    class QuickEditGuard
    {
    public:
        QuickEditGuard()
        {
            handle = GetStdHandle(STD_INPUT_HANDLE);
            if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &originalMode))
            {
                DWORD mode = (originalMode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE;
                disabled = SetConsoleMode(handle, mode) != 0;
            }
        }

        ~QuickEditGuard()
        {
            if (disabled)
            {
                SetConsoleMode(handle, originalMode);
            }
        }

        QuickEditGuard(const QuickEditGuard &) = delete;
        QuickEditGuard &operator=(const QuickEditGuard &) = delete;

    private:
        HANDLE handle = INVALID_HANDLE_VALUE;
        DWORD originalMode = 0;
        bool disabled = false;
    };

    class ScopedHandle
    {
    public:
        explicit ScopedHandle(HANDLE value) : handle(value) {}

        ~ScopedHandle()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
        }

        ScopedHandle(const ScopedHandle &) = delete;
        ScopedHandle &operator=(const ScopedHandle &) = delete;

        HANDLE get() const
        {
            return handle;
        }

    private:
        HANDLE handle = INVALID_HANDLE_VALUE;
    };
#endif

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

    string formatGameLabel(int appId, const string &name)
    {
        if (name.empty())
        {
            return "App ID " + to_string(appId);
        }
        return name + " (App ID " + to_string(appId) + ")";
    }

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

    fs::path pathFromUtf8(const string &text)
    {
#ifdef _WIN32
        if (text.empty())
        {
            return fs::path();
        }
        int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0)
        {
            return fs::path(text);
        }
        wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            &wide[0], length);
        return fs::path(wide);
#else
        return fs::path(text);
#endif
    }

    string pathToUtf8(const fs::path &path)
    {
#ifdef _WIN32
        const wstring wide = path.wstring();
        if (wide.empty())
        {
            return "";
        }
        int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return "";
        }
        string text(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                            &text[0], length, nullptr, nullptr);
        return text;
#else
        return path.native();
#endif
    }

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

    class AppInfoCache
    {
    public:
        bool installDir(const string &steamRoot, int appId, string &out)
        {
            out.clear();
            if (appId <= 0 || !ensureLoaded(steamRoot))
            {
                return false;
            }
            auto cached = cache.find(appId);
            if (cached != cache.end())
            {
                out = cached->second;
                return !out.empty();
            }

            string installdir;
            auto record = records.find(static_cast<unsigned int>(appId));
            if (record != records.end())
            {
                size_t position = record->second.first;
                parseEntries(position, record->second.second, 0, false, installdir);
            }
            cache[appId] = installdir;
            out = installdir;
            return !out.empty();
        }

    private:
        bool ensureLoaded(const string &steamRoot)
        {
            if (root == steamRoot)
            {
                return loaded;
            }
            root = steamRoot;
            records.clear();
            strings.clear();
            bytes.clear();
            cache.clear();
            loaded = load(steamRoot);
            return loaded;
        }

        bool readFile(const string &path)
        {
            ifstream file(path, ios::binary);
            if (!file)
            {
                return false;
            }
            file.seekg(0, ios::end);
            streamoff size = file.tellg();
            if (size <= 0 || size > 64LL * 1024 * 1024)
            {
                return false;
            }
            file.seekg(0, ios::beg);
            bytes.resize(static_cast<size_t>(size));
            file.read(reinterpret_cast<char *>(bytes.data()), size);
            return file.good() || file.eof();
        }

        bool u32(size_t at, unsigned int &value) const
        {
            if (at + 4 > bytes.size())
            {
                return false;
            }
            value = static_cast<unsigned int>(bytes[at]) |
                    (static_cast<unsigned int>(bytes[at + 1]) << 8) |
                    (static_cast<unsigned int>(bytes[at + 2]) << 16) |
                    (static_cast<unsigned int>(bytes[at + 3]) << 24);
            return true;
        }

        bool u64(size_t at, unsigned long long &value) const
        {
            unsigned int low = 0;
            unsigned int high = 0;
            if (!u32(at, low) || !u32(at + 4, high))
            {
                return false;
            }
            value = (static_cast<unsigned long long>(high) << 32) | low;
            return true;
        }

        bool cstring(size_t &position, size_t end, string &out) const
        {
            out.clear();
            while (position < end && bytes[position] != 0)
            {
                out.push_back(static_cast<char>(bytes[position]));
                position++;
            }
            if (position >= end)
            {
                return false;
            }
            position++;
            return true;
        }

        bool entryKey(size_t &position, size_t end, string &out) const
        {
            out.clear();
            if (version >= 41)
            {
                if (position + 4 > end)
                {
                    return false;
                }
                unsigned int index = 0;
                if (!u32(position, index))
                {
                    return false;
                }
                position += 4;
                if (index >= strings.size())
                {
                    return false;
                }
                out = strings[index];
                return true;
            }
            return cstring(position, end, out);
        }

        bool parseEntries(size_t &position, size_t end, int depth, bool inConfig,
                          string &installdir) const
        {
            if (depth > 32)
            {
                return false;
            }
            while (position < end)
            {
                unsigned char type = bytes[position++];
                if (type == 0x08)
                {
                    return true;
                }
                string name;
                if (!entryKey(position, end, name))
                {
                    return false;
                }

                if (type == 0x00)
                {
                    string found;
                    bool childInConfig = inConfig || name == "config";
                    if (!parseEntries(position, end, depth + 1, childInConfig, found))
                    {
                        return false;
                    }
                    if (!found.empty())
                    {
                        installdir = found;
                        return true;
                    }
                }
                else if (type == 0x01)
                {
                    string value;
                    if (!cstring(position, end, value))
                    {
                        return false;
                    }
                    if (inConfig && name == "installdir" && !value.empty())
                    {
                        installdir = value;
                        return true;
                    }
                }
                else if (type == 0x02 || type == 0x03 || type == 0x04 || type == 0x06)
                {
                    if (position + 4 > end)
                    {
                        return false;
                    }
                    position += 4;
                }
                else if (type == 0x07 || type == 0x0A)
                {
                    if (position + 8 > end)
                    {
                        return false;
                    }
                    position += 8;
                }
                else
                {

                    return false;
                }
            }
            return true;
        }

        bool load(const string &steamRoot)
        {
            if (!readFile(steamRoot + "/appcache/appinfo.vdf"))
            {
                return false;
            }
            unsigned int magic = 0;
            if (!u32(0, magic))
            {
                return false;
            }
            version = static_cast<int>(magic & 0xFF);
            if ((magic >> 8) != 0x075644 || version < 39 || version > 41)
            {
                return false;
            }

            size_t position = 8;
            if (version >= 41)
            {
                unsigned long long tableOffset = 0;
                if (!u64(8, tableOffset) || tableOffset + 4 > bytes.size())
                {
                    return false;
                }
                size_t table = static_cast<size_t>(tableOffset);
                unsigned int count = 0;
                if (!u32(table, count))
                {
                    return false;
                }
                size_t stringPosition = table + 4;
                for (unsigned int i = 0; i < count; i++)
                {
                    string text;
                    if (!cstring(stringPosition, bytes.size(), text))
                    {
                        break;
                    }
                    strings.push_back(text);
                }
                position = 16;
            }

            while (position + 8 <= bytes.size())
            {
                unsigned int appId = 0;
                unsigned int size = 0;
                if (!u32(position, appId) || appId == 0 || !u32(position + 4, size))
                {
                    break;
                }
                size_t recordEnd = position + 8 + size;
                if (recordEnd > bytes.size())
                {
                    break;
                }
                size_t header = 40 + (version >= 40 ? 20 : 0);
                size_t keyValuesStart = position + 8 + header;
                if (keyValuesStart < recordEnd)
                {
                    records[appId] = make_pair(keyValuesStart, recordEnd);
                }
                position = recordEnd;
            }
            return !records.empty();
        }

        vector<unsigned char> bytes;
        vector<string> strings;
        map<unsigned int, pair<size_t, size_t>> records;
        map<int, string> cache;
        string root;
        int version = 0;
        bool loaded = false;
    };

    bool resolveGameInstallBase(int appId, const string &steamRoot,
                                const vector<string> &libraryPaths, const string &manifestPath,
                                string &outBase)
    {
        string installdir;
        string libraryRoot;
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
            if (!installdir.empty())
            {

                libraryRoot = pathToUtf8(pathFromUtf8(manifestPath).parent_path().parent_path());
            }
        }
        if (installdir.empty())
        {
            static AppInfoCache appInfo;
            appInfo.installDir(steamRoot, appId, installdir);
        }
        if (installdir.empty())
        {
            return false;
        }

        fs::path base;
        if (!libraryRoot.empty())
        {
            base = pathFromUtf8(libraryRoot);
        }
        else
        {

            for (const auto &library : libraryPaths)
            {
                if (library.empty())
                {
                    continue;
                }
                fs::path candidate = pathFromUtf8(library) / "steamapps" / "common" /
                                     pathFromUtf8(installdir);
                error_code ec;
                if (fs::exists(candidate, ec))
                {
                    base = pathFromUtf8(library);
                    break;
                }
            }
            if (base.empty())
            {
                base = pathFromUtf8(steamRoot);
            }
        }

        outBase = pathToUtf8(base / "steamapps" / "common" / pathFromUtf8(installdir));
        return true;
    }

    bool resolveLocalUfsPath(const CloudFileInfo &file, int appId, const string &steamRoot,
                             const vector<string> &libraryPaths, const string &manifestPath,
                             const string &gameName, string &outPath)
    {
        if (file.root <= 0)
        {
            return false;
        }

        if (file.root == 1)
        {

            string base;
            if (!resolveGameInstallBase(appId, steamRoot, libraryPaths, manifestPath, base))
            {
                return false;
            }
            outPath = pathToUtf8(pathFromUtf8(base) / pathFromUtf8(file.name));
            return true;
        }

        if (file.root == 11)
        {

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
                fs::path candidate = dirEntry.path() / pathFromUtf8(gameName);
                if (fs::exists(candidate, ec))
                {
                    outPath = pathToUtf8(candidate / pathFromUtf8(file.name));
                    return true;
                }
            }
            return false;
        }

        string home = getEnvVar("USERPROFILE");
        fs::path base;
        switch (file.root)
        {
        case 2: case 8:
            if (home.empty())
            {
                return false;
            }
            base = fs::path(home) / "Documents";
            break;
        case 3: case 13: case 15:
            base = fs::path(getEnvVar("LOCALAPPDATA"));
            break;
        case 4: case 7: case 16:
            base = fs::path(getEnvVar("APPDATA"));
            break;
        case 6: case 14: case 18:
            if (home.empty())
            {
                return false;
            }
            base = fs::path(home);
            break;
        case 9:
            if (home.empty())
            {
                return false;
            }
            base = fs::path(home) / "Saved Games";
            break;
        case 10:
            base = fs::path(getEnvVar("PROGRAMDATA"));
            break;
        case 12:
            if (home.empty())
            {
                return false;
            }
            base = fs::path(home) / "AppData" / "LocalLow";
            break;
        default:
            return false;
        }
        if (base.empty())
        {
            return false;
        }
        outPath = pathToUtf8(base / pathFromUtf8(file.name));
        return true;
    }

    bool isSafeCloudFileName(const string &name)
    {
        if (name.empty())
        {
            return false;
        }
        fs::path relative = pathFromUtf8(name);
        if (relative.is_absolute() || relative.has_root_name() ||
            relative.has_root_directory())
        {
            return false;
        }
        for (const auto &part : relative)
        {
#ifdef _WIN32
            wstring text = part.native();
            while (!text.empty() && (text.back() == L' ' || text.back() == L'.'))
            {
                text.pop_back();
            }
            if (text.empty() || text == L"." || text == L"..")
            {
                return false;
            }
#else
            string text = part.native();
            while (!text.empty() && (text.back() == ' ' || text.back() == '.'))
            {
                text.pop_back();
            }
            if (text.empty() || text == "." || text == "..")
            {
                return false;
            }
#endif
        }
        return true;
    }

    void dropUnsafeCloudNames(vector<CloudFileInfo> &files, bool removeUnpreparable)
    {
        vector<CloudFileInfo> kept;
        for (const auto &file : files)
        {
            if (file.root != 0 && !isSafeCloudFileName(file.name))
            {
                cout << ">Skipping " << file.name << ": unsafe cloud path." << endl;
                if (!removeUnpreparable)
                {
                    kept.push_back(file);
                }
                continue;
            }
            kept.push_back(file);
        }
        if (removeUnpreparable)
        {
            files = kept;
        }
    }

    void parseRemotecache(const string &content, vector<CloudFileInfo> &files)
    {
        bool inEntry = false;
        int blockDepth = 0;
        string pendingKey;
        CloudFileInfo current;

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

            if (!inEntry)
            {
                if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
                {
                    string key = line.substr(1, line.size() - 2);
                    if (blockDepth == 0 && isAllDigits(key))
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
                else if (line == "{")
                {
                    blockDepth++;
                }
                else if (line == "}")
                {
                    if (blockDepth > 0)
                    {
                        blockDepth--;
                    }
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

        error_code dirError;
        fs::directory_iterator userEntries(userDataPath, dirError);
        if (dirError)
        {
            cerr << "Error: could not read " << userDataPath << ": " << dirError.message() << endl;
            return games;
        }
        for (const auto &entry : userEntries)
        {
            error_code entryError;
            if (!entry.is_directory(entryError))
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
            info.hasLocalRecord = true;
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

    void mergeCloudPageGames(vector<CloudGameInfo> &games,
                             const vector<CdpCloudGame> &cloudGames,
                             const map<int, string> &manifests,
                             const set<int> &justEmptiedApps)
    {
        map<int, size_t> indexById;
        for (size_t i = 0; i < games.size(); i++)
        {
            indexById[games[i].appId] = i;
        }

        for (const auto &cloudGame : cloudGames)
        {
            auto it = indexById.find(cloudGame.appId);
            if (it != indexById.end())
            {
                CloudGameInfo &game = games[it->second];
                game.cloudListed = true;
                if (!cloudGame.name.empty())
                {
                    game.name = cloudGame.name;
                }

                game.fileCount = cloudGame.fileCount;
                game.totalSize = cloudGame.totalSize;
                game.totalSizeText = cloudGame.totalSizeText;
                continue;
            }

            CloudGameInfo game;
            game.appId = cloudGame.appId;
            game.name = cloudGame.name;
            game.fileCount = cloudGame.fileCount;
            game.totalSize = cloudGame.totalSize;
            game.totalSizeText = cloudGame.totalSizeText;
            game.cloudListed = true;
            game.hasLocalRecord = false;
            game.installed = manifests.find(cloudGame.appId) != manifests.end();
            games.push_back(game);
        }

        set<int> pageIds;
        for (const auto &cloudGame : cloudGames)
        {
            pageIds.insert(cloudGame.appId);
        }

        for (int appId : justEmptiedApps)
        {
            pageIds.erase(appId);
        }
        vector<CloudGameInfo> kept;
        kept.reserve(games.size());
        for (const auto &game : games)
        {
            if (pageIds.count(game.appId) != 0)
            {
                kept.push_back(game);
                continue;
            }
            if (justEmptiedApps.count(game.appId) != 0)
            {
                continue;
            }

            bool hasCloudFiles = false;
            string pageError;
            if (cdpHasCloudFiles(game.appId, hasCloudFiles, pageError) &&
                !hasCloudFiles)
            {
                continue;
            }

            kept.push_back(game);
        }
        games.swap(kept);

        sort(games.begin(), games.end(),
             [](const CloudGameInfo &a, const CloudGameInfo &b)
             { return a.appId < b.appId; });
    }

#ifdef _WIN32

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

    void startSteamShutdown(const string &steamRoot)
    {
        string rootBackslash = steamRoot;
        replace(rootBackslash.begin(), rootBackslash.end(), '/', '\\');

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

    bool startSteamClient(const string &steamRoot, bool enableCdp)
    {
        string exePath = steamRoot + "/steam.exe";
        if (!fs::exists(exePath))
        {
            return false;
        }

        string exePathBackslash = exePath;
        replace(exePathBackslash.begin(), exePathBackslash.end(), '/', '\\');

        string commandLine;
        commandLine += '"';
        commandLine += exePathBackslash;
        commandLine += '"';
        if (enableCdp)
        {
            commandLine += " -cef-enable-debugging";
        }
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

    int enableCloudEverywhere(const string &sharedConfigPath)
    {
        try
        {
            string content = FileUtility().readFileContents(sharedConfigPath);
            const size_t cloudKeyLen = sizeof("\"CloudEnabled\"") - 1;
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
            if (!outFile)
            {
                return -1;
            }
            outFile.close();
            if (!outFile)
            {
                return -1;
            }
            return flipped;
        }
        catch (const exception &e)
        {
            cout << ">Error: could not update sharedconfig.vdf: " << e.what() << endl;
            return -1;
        }
    }

    int enableCloudEverywhereForAllUsers(const string &steamRoot)
    {
        string userDataRoot = steamRoot + "/userdata";
        int failures = 0;
        error_code ec;
        if (!fs::exists(userDataRoot, ec))
        {
            return ec ? 1 : 0;
        }
        fs::directory_iterator it(userDataRoot, ec);
        if (ec)
        {
            return 1;
        }
        for (const auto &entry : it)
        {
            bool isDirectory = entry.is_directory(ec);
            if (ec)
            {
                ec.clear();
                ++failures;
                continue;
            }
            if (!isDirectory)
            {
                continue;
            }
            string sharedConfigPath = pathToUtf8(entry.path() / "7" / "remote" / "sharedconfig.vdf");
            bool exists = fs::exists(pathFromUtf8(sharedConfigPath), ec);
            if (ec)
            {
                ec.clear();
                ++failures;
                continue;
            }
            if (!exists)
            {
                continue;
            }
            if (enableCloudEverywhere(sharedConfigPath) < 0)
            {
                ++failures;
            }
        }
        return failures;
    }

    bool allCloudSwitchesOn(const string &sharedConfigPath)
    {
        try
        {
            string content = FileUtility().readFileContents(sharedConfigPath);
            const size_t cloudKeyLen = sizeof("\"CloudEnabled\"") - 1;
            bool sawSwitch = false;
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
                    return false;
                }
                sawSwitch = true;
                if (content[valQ1 + 1] != '1')
                {
                    return false;
                }
                pos = valQ1 + 2;
            }
            return sawSwitch;
        }
        catch (const exception &)
        {
            return false;
        }
    }

    bool cloudSwitchesPresent(const string &sharedConfigPath)
    {
        try
        {
            return FileUtility().readFileContents(sharedConfigPath).find("\"CloudEnabled\"") != string::npos;
        }
        catch (const exception &)
        {
            return false;
        }
    }

    bool restartSteamAutomatically(const string &steamRoot, const string &userId,
                                   const string &personaName, const string &fixSharedConfig,
                                   bool preserveDiskEdits, bool enableCdp)
    {
        auto startAndAwaitLogon = [&]() -> bool
        {
            long long baseline = readUserTimestamp(steamRoot, userId);
            if (!startSteamClient(steamRoot, enableCdp))
            {
                cout << ">Error: could not start Steam; start it, then run option 4 again." << endl;
                return false;
            }
            cout << ">Waiting for Steam to sign in as " << personaName << endl;
            if (!waitForSteamLogon(steamRoot, userId, baseline, 240000))
            {
                cout << ">Steam did not sign in; log in, then run option 4 again." << endl;
                return false;
            }
            cout << ">Signed in as " << personaName << "." << endl;
            return true;
        };

        if (!isSteamRunning())
        {
            cout << ">Starting Steam" << endl;
            return startAndAwaitLogon();
        }
        if (isSteamInGame())
        {
            cout << ">A game is running; close it and restart Steam to apply the new settings." << endl;
            return false;
        }

        if (preserveDiskEdits)
        {
            cout << ">Force-closing Steam to keep the corrected config" << endl;
            forceKillSteam();
            if (!waitForSteamExit(60000))
            {
                cout << ">Error: Steam did not close; close it, then run option 4 again." << endl;
                return false;
            }
            return startAndAwaitLogon();
        }

        cout << ">Restarting Steam" << endl;
        startSteamShutdown(steamRoot);
        if (!waitForSteamExit(15000))
        {

            cout << ">Steam did not close; force-closing it" << endl;
            forceKillSteam();
            if (!waitForSteamExit(45000))
            {
                cout << ">Error: Steam did not close; close it, then run option 4 again." << endl;
                return false;
            }
        }

        if (!fixSharedConfig.empty())
        {
            int reEnabled = enableCloudEverywhere(fixSharedConfig);
            if (reEnabled > 0)
            {
                cout << ">Steam reset the cloud switches; enabling them again" << endl;
            }
            else if (reEnabled < 0)
            {
                cout << ">Could not re-enable the cloud switches; use Steam -> Settings -> Cloud." << endl;
            }
        }
        if (!startAndAwaitLogon())
        {
            return false;
        }
        if (!fixSharedConfig.empty() && !allCloudSwitchesOn(fixSharedConfig))
        {
            enableCloudEverywhere(fixSharedConfig);
        }
        return true;
    }

    bool waitForCdpAvailable(int timeoutMs)
    {
        for (int waited = 0; waited <= timeoutMs; waited += 500)
        {
            if (cdpIsAvailable())
            {
                return true;
            }
            if (waited < timeoutMs)
            {
                Sleep(500);
            }
        }
        return false;
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

    struct PendingUpload
    {
        string remotecachePath;
        vector<string> fileNames;
        long long queuedAt = 0;
    };

    bool readRemainingCloudNames(const string &remotecachePath, set<string> &remaining)
    {
        remaining.clear();
        error_code ec;
        bool exists = fs::exists(remotecachePath, ec);
        if (ec || !exists)
        {
            return false;
        }
        try
        {
            FileUtility fileUtility;
            string content = fileUtility.readFileContents(remotecachePath);
            if (content.find_first_not_of(" \t\r\n") == string::npos)
            {
                return false;
            }
            vector<CloudFileInfo> files;
            parseRemotecache(content, files);
            for (const auto &file : files)
            {
                remaining.insert(file.name);
            }
            return true;
        }
        catch (const exception &)
        {
            return false;
        }
    }

    bool waitForUploadToFinish(int appId, const string &remotecachePath,
                               const vector<string> &names, int timeoutMs)
    {
        unsigned long long started = GetTickCount64();
        unsigned long long pageEmptySince = 0;
        int waited = 0;
        while (true)
        {
            set<string> remaining;
            bool recordReadable = readRemainingCloudNames(remotecachePath, remaining);
            if (recordReadable)
            {
                bool listed = false;
                for (const auto &name : names)
                {
                    if (remaining.count(name) != 0)
                    {
                        listed = true;
                        break;
                    }
                }
                if (!listed)
                {
                    return true;
                }
            }
            else if (waited % 3000 == 0)
            {
                bool hasFiles = false;
                string pageError;
                if (cdpHasCloudFiles(appId, hasFiles, pageError))
                {
                    unsigned long long now = GetTickCount64();
                    if (hasFiles)
                    {
                        pageEmptySince = 0;
                    }
                    else if (pageEmptySince != 0)
                    {
                        if (now - pageEmptySince >= 2000)
                        {
                            return true;
                        }
                    }
                    else
                    {
                        pageEmptySince = now;
                    }
                }
            }

            if (GetTickCount64() - started >= static_cast<unsigned long long>(timeoutMs))
            {
                return false;
            }
            if (waited % 1000 == 0)
            {
                cout << "." << flush;
            }
            Sleep(500);
            waited += 500;
        }
    }

    bool isConflictQueryEcho(const string &line)
    {
        return line.find("GetConflictingFileTimestamps") != string::npos;
    }

    bool containsNoCase(const string &haystack, const string &needle)
    {
        if (needle.empty())
        {
            return true;
        }
        auto it = search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                         [](char left, char right)
                         {
                             return tolower(static_cast<unsigned char>(left)) ==
                                    tolower(static_cast<unsigned char>(right));
                         });
        return it != haystack.end();
    }

    string readCloudLogHistory(const string &steamRoot)
    {
        string combined;
        const string paths[] = {steamRoot + "/logs/cloud_log.previous.txt",
                                steamRoot + "/logs/cloud_log.txt"};
        for (const auto &logPath : paths)
        {
            error_code ec;
            if (!fs::exists(pathFromUtf8(logPath), ec))
            {
                continue;
            }
            try
            {
                FileUtility fileUtility;
                combined += fileUtility.readFileContents(logPath);
                combined += '\n';
            }
            catch (const exception &)
            {
                continue;
            }
        }
        return combined;
    }

    set<int> readConflictedApps(const string &steamRoot)
    {
        set<int> conflicted;
        string content = readCloudLogHistory(steamRoot);
        if (content.empty())
        {
            return conflicted;
        }
        const string marker = "[AppID ";
        istringstream stream(content);
        string line;
        while (getline(stream, line))
        {
            if (isConflictQueryEcho(line))
            {
                continue;
            }
            size_t at = line.find(marker);
            if (at == string::npos)
            {
                continue;
            }
            at += marker.size();
            size_t end = line.find(']', at);
            if (end == string::npos)
            {
                continue;
            }
            string digits = line.substr(at, end - at);
            if (digits.empty() ||
                digits.find_first_not_of("0123456789") != string::npos)
            {
                continue;
            }
            int appId = 0;
            try
            {
                appId = stoi(digits);
            }
            catch (const exception &)
            {
                continue;
            }
            if (appId <= 0)
            {
                continue;
            }
            if (line.find("Upload complete, result OK") != string::npos)
            {
                conflicted.erase(appId);
                continue;
            }
            if (containsNoCase(line, "conflict"))
            {
                conflicted.insert(appId);
            }
        }
        return conflicted;
    }

    struct CloudBatchStatus
    {
        bool batchSeen = false;
        bool inFlight = false;
        bool conflicted = false;
        int deletedOurs = 0;
    };

    CloudBatchStatus readCloudBatchStatus(const string &steamRoot, int appId,
                                          const set<string> &queuedNames)
    {
        CloudBatchStatus status;
        string logPath = steamRoot + "/logs/cloud_log.txt";
        error_code ec;
        if (!fs::exists(pathFromUtf8(logPath), ec))
        {
            return status;
        }
        string content;
        try
        {
            FileUtility fileUtility;
            content = fileUtility.readFileContents(logPath);
        }
        catch (const exception &)
        {
            return status;
        }
        const string marker = "[AppID " + to_string(appId) + "]";
        const string deletePrefix = "Delete OK for file ";
        istringstream stream(content);
        string line;
        bool openBatch = false;
        while (getline(stream, line))
        {
            if (line.find(marker) == string::npos)
            {
                continue;
            }
            if (isConflictQueryEcho(line))
            {
                continue;
            }
            if (line.find("Upload batch initiated") != string::npos)
            {
                status.batchSeen = true;
                openBatch = true;
                continue;
            }
            if (line.find("Upload complete, result ") != string::npos)
            {
                openBatch = false;
                if (line.find("result OK") != string::npos)
                {
                    status.conflicted = false;
                }
                continue;
            }
            if (containsNoCase(line, "conflict"))
            {
                status.conflicted = true;
            }
            size_t deletePos = line.find(deletePrefix);
            if (deletePos == string::npos)
            {
                continue;
            }
            string name = line.substr(deletePos + deletePrefix.size());
            while (!name.empty() &&
                   (name.back() == 13 || name.back() == 10 || name.back() == ' '))
            {
                name.pop_back();
            }
            if (queuedNames.count(name) != 0)
            {
                status.deletedOurs++;
            }
        }
        status.inFlight = openBatch;
        return status;
    }

    struct CloudLogDigest
    {
        bool logReadable = false;
        bool inFlight = false;
        bool conflicted = false;
        int failures = 0;
        int lastBatchDeletes = 0;
        int lastBatchForgets = 0;
        bool lastBatchOk = false;
        vector<string> evidence;
    };

    CloudLogDigest readCloudLogDigest(const string &steamRoot, int appId)
    {
        CloudLogDigest digest;
        string content = readCloudLogHistory(steamRoot);
        if (content.empty())
        {
            return digest;
        }
        digest.logReadable = true;
        const string marker = "[AppID " + to_string(appId) + "]";
        istringstream stream(content);
        string line;
        bool openBatch = false;
        while (getline(stream, line))
        {
            if (line.find(marker) == string::npos)
            {
                continue;
            }
            if (isConflictQueryEcho(line))
            {
                continue;
            }
            if (line.find("Upload batch initiated") != string::npos)
            {
                openBatch = true;
                digest.lastBatchDeletes = 0;
                digest.lastBatchForgets = 0;
                digest.lastBatchOk = false;
                continue;
            }
            if (line.find("Upload complete, result OK") != string::npos)
            {
                openBatch = false;
                digest.lastBatchOk = true;
                digest.conflicted = false;
                digest.evidence.clear();
                continue;
            }
            if (line.find("Upload complete, result Failure") != string::npos)
            {
                openBatch = false;
                digest.lastBatchOk = false;
                digest.failures++;
                continue;
            }
            if (containsNoCase(line, "conflict"))
            {
                digest.conflicted = true;
                if (digest.evidence.size() < 3)
                {
                    digest.evidence.push_back(line);
                }
                continue;
            }
            if (line.find("Delete OK for file") != string::npos)
            {
                digest.lastBatchDeletes++;
            }
            else if (line.find("Forget OK for file") != string::npos)
            {
                digest.lastBatchForgets++;
            }
            else if (line.find("Upload Failure for file") != string::npos)
            {
                digest.failures++;
                if (digest.evidence.size() < 3)
                {
                    digest.evidence.push_back(line);
                }
            }
        }
        digest.inFlight = openBatch;
        return digest;
    }

    bool offerConflictRestart(int appId, const string &steamRoot, const string &userId,
                              const string &personaName, bool liveConflicted)
    {
        CloudLogDigest digest = readCloudLogDigest(steamRoot, appId);
        cout << ">Cloud sync is conflicted; changes may be ignored." << endl;
        if (liveConflicted)
        {
            cout << ">Steam reports the conflict right now." << endl;
        }
        if (digest.inFlight)
        {
            cout << ">Steam is still uploading; try again shortly." << endl;
            return false;
        }
        bool doRestart = false;
        while (true)
        {
            cout << ">Restart Steam to clear it?" << endl;
            cout << ">Enter 1 to restart" << endl;
            cout << ">Enter 0 to continue" << endl;
            cout << ">";
            string input;
            if (!getline(cin, input))
            {
                return false;
            }
            if (input == "1")
            {
                doRestart = true;
                cout << endl;
                break;
            }
            if (input == "0")
            {
                break;
            }
            cout << endl;
            cout << ">Invalid input" << endl;
        }
        if (!doRestart)
        {
            return false;
        }
        if (!restartSteamAutomatically(steamRoot, userId, personaName, "", false, true))
        {
            return false;
        }
        waitForCdpAvailable(20000);
        cout << endl;
        cout << ">Steam restarted; select this game again." << endl;
        return true;
    }

    struct PendingBatchOutcome
    {
        bool safeToContinue = false;
        bool conflicted = false;
        bool finishedClean = false;
        size_t leftoverCount = 0;
    };

    PendingBatchOutcome waitForPendingBatchToSettle(int appId, const string &steamRoot,
                                                    const string &remotecachePath,
                                                    const vector<string> &names)
    {
        PendingBatchOutcome outcome;
        set<string> queuedNames(names.begin(), names.end());
        unsigned long long started = GetTickCount64();
        unsigned long long lastChange = started;
        long long lastRemaining = -1;
        bool lastInFlight = false;
        bool batchDeletedOurs = false;
        bool headerPrinted = false;
        int waited = 0;
        while (true)
        {
            size_t remaining = names.size();
            set<string> listed;
            if (readRemainingCloudNames(remotecachePath, listed))
            {
                remaining = 0;
                for (const auto &name : names)
                {
                    if (listed.count(name) != 0)
                    {
                        remaining++;
                    }
                }
            }
            else
            {
                bool hasFiles = true;
                string pageError;
                if (cdpHasCloudFiles(appId, hasFiles, pageError) && !hasFiles)
                {
                    remaining = 0;
                }
            }
            CloudBatchStatus status = readCloudBatchStatus(steamRoot, appId, queuedNames);
            if (status.conflicted)
            {
                outcome.conflicted = true;
            }
            if (status.deletedOurs > 0)
            {
                batchDeletedOurs = true;
            }
            if (remaining == 0)
            {
                outcome.safeToContinue = true;
                outcome.finishedClean = true;
                if (headerPrinted)
                {
                    cout << endl;
                }
                return outcome;
            }
            if (!headerPrinted)
            {
                cout << ">Waiting for the previous deletion to finish" << flush;
                headerPrinted = true;
            }
            if (static_cast<long long>(remaining) != lastRemaining ||
                status.inFlight != lastInFlight)
            {
                lastRemaining = static_cast<long long>(remaining);
                lastInFlight = status.inFlight;
                lastChange = GetTickCount64();
            }
            unsigned long long idleFor = GetTickCount64() - lastChange;
            unsigned long long sinceStart = GetTickCount64() - started;
            if (status.conflicted && idleFor >= 20000)
            {
                cout << endl;
                return outcome;
            }
            if (!status.inFlight && batchDeletedOurs && idleFor >= 8000)
            {
                outcome.safeToContinue = true;
                outcome.leftoverCount = remaining;
                cout << endl;
                return outcome;
            }
            if (status.inFlight && sinceStart >= 240000)
            {
                cout << endl;
                return outcome;
            }
            if (!status.inFlight && idleFor >= 60000)
            {
                cout << endl;
                return outcome;
            }
            if (sinceStart >= 300000)
            {
                cout << endl;
                return outcome;
            }
            if (waited % 1000 == 0)
            {
                cout << "." << flush;
            }
            Sleep(500);
            waited += 500;
        }
    }

    string pendingStateFilePath()
    {
        try
        {
            return (fs::temp_directory_path() / "steam-tweak-tool-pending-deletions.txt").string();
        }
        catch (const exception &)
        {
            return string();
        }
    }

    void savePendingUploads(const map<int, PendingUpload> &pendingUploads)
    {
        string statePath = pendingStateFilePath();
        if (statePath.empty())
        {
            return;
        }
        if (pendingUploads.empty())
        {
            error_code ec;
            fs::remove(pathFromUtf8(statePath), ec);
            return;
        }
        ofstream out(statePath, ios::trunc);
        if (!out)
        {
            return;
        }
        for (const auto &entry : pendingUploads)
        {
            long long stamp = entry.second.queuedAt != 0
                                  ? entry.second.queuedAt
                                  : static_cast<long long>(time(nullptr));
            out << "# " << entry.first << " " << stamp << endl;
            for (const auto &name : entry.second.fileNames)
            {
                out << name << endl;
            }
        }
    }

    void loadPendingUploads(map<int, PendingUpload> &pendingUploads, const string &userDataPath)
    {
        string statePath = pendingStateFilePath();
        if (statePath.empty())
        {
            return;
        }
        ifstream in(statePath);
        if (!in)
        {
            return;
        }
        const long long maxAge = 30 * 60;
        long long now = static_cast<long long>(time(nullptr));
        int currentApp = 0;
        long long currentStamp = 0;
        string line;
        while (getline(in, line))
        {
            if (!line.empty() && line.back() == 13)
            {
                line.pop_back();
            }
            if (line.compare(0, 2, "# ") == 0)
            {
                istringstream header(line.substr(2));
                if (!(header >> currentApp >> currentStamp))
                {
                    currentApp = 0;
                }
                continue;
            }
            if (currentApp <= 0 || line.empty())
            {
                continue;
            }
            if (now - currentStamp > maxAge || currentStamp - now > 300)
            {
                continue;
            }
            PendingUpload &entry = pendingUploads[currentApp];
            entry.remotecachePath = userDataPath + "/" + to_string(currentApp) + "/remotecache.vdf";
            entry.queuedAt = currentStamp;
            entry.fileNames.push_back(line);
        }
    }

#endif

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

    struct SteamApiFunctions
    {
        SteamAPI_InitFlat_t initFlat = nullptr;
        SteamAPI_Shutdown_t shutdown = nullptr;
        SteamAPI_RunCallbacks_t runCallbacks = nullptr;
        SteamAPI_SteamRemoteStorage_v016_t remoteStorageAccessor = nullptr;
        SteamAPI_ISteamRemoteStorage_FileDelete_t fileDelete = nullptr;
        SteamAPI_ISteamRemoteStorage_FileForget_t fileForget = nullptr;
        SteamAPI_ISteamRemoteStorage_FileExists_t fileExists = nullptr;
        SteamAPI_ISteamRemoteStorage_GetFileCount_t getFileCount = nullptr;
        SteamAPI_ISteamRemoteStorage_GetFileNameAndSize_t getFileNameAndSize = nullptr;
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
            cout << ">steam_api64.dll must be in the same folder as this program." << endl;
            return api;
        }

        api.initFlat = reinterpret_cast<SteamAPI_InitFlat_t>(GetProcAddress(outModule, "SteamAPI_InitFlat"));
        api.shutdown = reinterpret_cast<SteamAPI_Shutdown_t>(GetProcAddress(outModule, "SteamAPI_Shutdown"));
        api.runCallbacks = reinterpret_cast<SteamAPI_RunCallbacks_t>(GetProcAddress(outModule, "SteamAPI_RunCallbacks"));
        api.remoteStorageAccessor = reinterpret_cast<SteamAPI_SteamRemoteStorage_v016_t>(GetProcAddress(outModule, "SteamAPI_SteamRemoteStorage_v016"));
        api.fileDelete = reinterpret_cast<SteamAPI_ISteamRemoteStorage_FileDelete_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_FileDelete"));
        api.fileForget = reinterpret_cast<SteamAPI_ISteamRemoteStorage_FileForget_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_FileForget"));
        api.fileExists = reinterpret_cast<SteamAPI_ISteamRemoteStorage_FileExists_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_FileExists"));
        api.getFileCount = reinterpret_cast<SteamAPI_ISteamRemoteStorage_GetFileCount_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_GetFileCount"));
        api.getFileNameAndSize = reinterpret_cast<SteamAPI_ISteamRemoteStorage_GetFileNameAndSize_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_GetFileNameAndSize"));
        api.isCloudEnabledForApp = reinterpret_cast<SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp"));
        api.setCloudEnabledForApp = reinterpret_cast<SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp_t>(GetProcAddress(outModule, "SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp"));

        if (!api.valid())
        {
            cout << ">Error: steam_api64.dll is missing required exports." << endl;
        }
        return api;
    }

    bool postDeletionFollowUp(const string &steamRoot, const string &userId,
                              const string &personaName, const vector<string> &libraryPaths,
                              bool refreshCloudPage, int appId,
                              const string &remotecachePath,
                              const vector<string> &pendingNames)
    {
        if (refreshCloudPage)
        {
            string refreshError;
            cdpRefreshCloudPage(refreshError);
        }

        cout << endl;
        bool runCleanup = false;
        while (true)
        {
            cout << ">Disable per-game cloud and auto-updates? (restarts Steam)" << endl;
            cout << ">Enter 1 to disable them" << endl;
            cout << ">Enter 0 to skip" << endl;
            cout << ">";
            string cleanupInput;
            if (!getline(cin, cleanupInput))
            {
                return false;
            }
            if (cleanupInput == "1")
            {
                runCleanup = true;
                cout << endl;
                break;
            }
            if (cleanupInput == "0")
            {
                break;
            }
            cout << endl;
            cout << ">Invalid input" << endl;
        }
        if (!runCleanup)
        {
            return false;
        }

        if (!pendingNames.empty())
        {
            cout << ">Waiting for the upload to finish." << flush;
            bool uploaded = waitForUploadToFinish(appId, remotecachePath, pendingNames, 240000);
            cout << endl;
            if (!uploaded)
            {
                cout << ">Still uploading; cleanup skipped." << endl;
                return false;
            }
            cout << endl;
        }

        cout << ">Running option 1" << endl;
        disableSteamCloudForAllGames(steamRoot + "/userdata", libraryPaths);

        cout << ">Running option 2" << endl;
        disableAutoUpdatesForAllGames(libraryPaths);

        restartSteamAutomatically(steamRoot, userId, personaName, "",
                                   true,  true);
        return true;
    }

    void returnToMainMenuAfterCleanup()
    {
        cout << ">Cleanup done." << endl;
    }

    bool runCloudSyncWorkerProcess(const string &appId, const string &remotecachePath,
                                   const string &gameName, int &exitCode)
    {
        string exePath = getExecutablePath();
        string commandLine = "\"" + exePath + "\" --cloud-sync " + appId + " \"" + remotecachePath +
                             "\" \"" + gameName + "\"";
        STARTUPINFOA startupInfo;
        ZeroMemory(&startupInfo, sizeof(startupInfo));
        startupInfo.cb = sizeof(startupInfo);

        SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
        ScopedHandle nullOut(CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
        ScopedHandle nullErr(CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
        if (nullOut.get() != INVALID_HANDLE_VALUE && nullErr.get() != INVALID_HANDLE_VALUE)
        {
            startupInfo.dwFlags = STARTF_USESTDHANDLES;
            startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startupInfo.hStdOutput = nullOut.get();
            startupInfo.hStdError = nullErr.get();
        }
        PROCESS_INFORMATION processInfo;
        ZeroMemory(&processInfo, sizeof(processInfo));
        string workingDir = getExecutableDirectory();
        if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
                            workingDir.data(), &startupInfo, &processInfo))
        {
            printLastError("Launching the cloud sync worker");
            return false;
        }

        DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 60000);
        if (waitResult == WAIT_TIMEOUT)
        {
            cout << ">Worker timed out; stopping it." << endl;
            TerminateProcess(processInfo.hProcess, 1);
            WaitForSingleObject(processInfo.hProcess, 5000);
        }
        DWORD workerExitCode = 1;
        GetExitCodeProcess(processInfo.hProcess, &workerExitCode);
        exitCode = static_cast<int>(workerExitCode);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return true;
    }
#endif
}

CloudDataDeleter::CloudDataDeleter() {}

int runCloudWorker(int argc, char *argv[])
{
#ifdef _WIN32
    if (argc < 5)
    {
        cout << ">Usage: --cloud-worker <appid> <filelist> <name>" << endl;
        return 2;
    }

    string appIdString = argv[2];
    if (!isAllDigits(appIdString))
    {
        cout << ">Invalid appid: " << appIdString << endl;
        return 2;
    }
    string gameName = (argc > 4) ? argv[4] : "";

    struct WorkerFile
    {
        int root = 0;
        string localPath;
        string name;
    };
    vector<WorkerFile> files;
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
            if (line.empty())
            {
                continue;
            }
            size_t firstTab = line.find('\t');
            size_t secondTab = (firstTab == string::npos) ? string::npos
                                                          : line.find('\t', firstTab + 1);
            if (firstTab == string::npos || secondTab == string::npos)
            {
                continue;
            }
            WorkerFile entry;
            entry.root = atoi(line.substr(0, firstTab).c_str());
            entry.localPath = line.substr(firstTab + 1, secondTab - firstTab - 1);
            entry.name = line.substr(secondTab + 1);
            if (entry.name.empty())
            {
                continue;
            }
            files.push_back(entry);
        }
    }
    if (files.empty())
    {
        cout << ">Error: empty file list" << endl;
        return 2;
    }

    SetEnvironmentVariableA("SteamAppId", appIdString.c_str());
    SetEnvironmentVariableA("SteamGameId", appIdString.c_str());

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
    if (initResult != 0)
    {
        cout << ">Error: could not connect to " << formatGameLabel(atoi(appIdString.c_str()), gameName)
             << (initErrMsg[0] != '\0' ? ": " + string(initErrMsg)
                                        : " (code " + to_string(initResult) + ")")
             << endl;
        DeleteFileA(appidFilePath.c_str());
        FreeLibrary(steamApiModule);
        return 1;
    }

    void *remoteStorage = api.remoteStorageAccessor();
    if (!remoteStorage)
    {
        cout << ">Error: Steam Remote Storage is unavailable." << endl;
        api.shutdown();
        DeleteFileA(appidFilePath.c_str());
        FreeLibrary(steamApiModule);
        return 1;
    }

    if (!api.isCloudEnabledForApp(remoteStorage))
    {
        api.setCloudEnabledForApp(remoteStorage, true);
    }

    int successCount = 0;
    int failCount = 0;
    bool anyUfsFailed = false;

    vector<const WorkerFile *> apiFiles;

    bool anyLocalUfsCopy = false;
    for (const auto &file : files)
    {
        if (file.root == 0 || file.localPath.empty())
        {
            continue;
        }
        error_code ec;
        if (fs::exists(pathFromUtf8(file.localPath), ec))
        {
            anyLocalUfsCopy = true;
            break;
        }
    }
    if (anyLocalUfsCopy)
    {
        cout << ">Waiting for Steam" << flush;
        for (int waited = 0; waited < 1000; waited += 500)
        {
            cout << "." << flush;
            api.runCallbacks();
            Sleep(500);
        }
        cout << endl;
    }

    vector<string> ufsLocalPathsToClean;
    for (const auto &file : files)
    {
        if (file.root == 0 || file.localPath.empty())
        {
            apiFiles.push_back(&file);
            continue;
        }

        error_code ec;
        fs::path localFile = pathFromUtf8(file.localPath);
        if (!fs::exists(localFile, ec))
        {
            apiFiles.push_back(&file);
            continue;
        }

        ufsLocalPathsToClean.push_back(file.localPath);
        fs::remove(localFile, ec);
        if (ec)
        {
            failCount++;
            anyUfsFailed = true;
            cout << ">Failed: " << file.name << " (could not delete its local copy)" << endl;
        }
        else
        {
            successCount++;
            cout << ">Deleted local copy: " << file.localPath << endl;
        }
    }

    if (!ufsLocalPathsToClean.empty())
    {
        cout << ">Waiting for Steam to register the deletions" << flush;
        for (int waited = 0; waited < 500; waited += 250)
        {
            cout << "." << flush;
            api.runCallbacks();
            Sleep(500);
            for (const auto &localPath : ufsLocalPathsToClean)
            {
                error_code ec;
                if (fs::exists(pathFromUtf8(localPath), ec))
                {
                    fs::remove(pathFromUtf8(localPath), ec);
                }
            }
        }
        cout << endl;
    }

    for (const WorkerFile *file : apiFiles)
    {
        api.fileDelete(remoteStorage, file->name.c_str());
        api.fileForget(remoteStorage, file->name.c_str());
    }
    if (!apiFiles.empty())
    {
        for (int waited = 0; waited < 500; waited += 250)
        {
            api.runCallbacks();
            Sleep(250);
        }

        bool secondPassUsed = false;
        for (const WorkerFile *file : apiFiles)
        {
            if (!api.fileExists(remoteStorage, file->name.c_str()))
            {
                successCount++;
                cout << ">Deleted: " << file->name << endl;
                continue;
            }

            secondPassUsed = true;
            api.fileDelete(remoteStorage, file->name.c_str());
            api.fileForget(remoteStorage, file->name.c_str());
            if (!api.fileExists(remoteStorage, file->name.c_str()))
            {
                successCount++;
                cout << ">Deleted: " << file->name << " (second pass)" << endl;
            }
            else
            {
                failCount++;
                anyUfsFailed = true;
                cout << ">Failed: " << file->name << " (still present)" << endl;
            }
        }

        if (secondPassUsed)
        {
            for (int waited = 0; waited < 1000; waited += 250)
            {
                api.runCallbacks();
                Sleep(250);
            }
        }
    }

    api.runCallbacks();

    api.shutdown();
    DeleteFileA(appidFilePath.c_str());
    FreeLibrary(steamApiModule);

    if (failCount > 0)
    {
        cout << ">Worker: " << successCount << " deleted, " << failCount << " failed." << endl;
    }
    if (anyUfsFailed)
    {
        cout << ">Some auto-cloud files could not be deleted." << endl;
    }

    return failCount > 0 ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    cout << ">Error: Steam Cloud deletion is only supported on Windows builds." << endl;
    return 1;
#endif
}

int runCloudSyncWorker(int argc, char *argv[])
{
#ifdef _WIN32
    if (argc < 5)
    {
        cout << ">Usage: --cloud-sync <appid> <remotecache.vdf> <name>" << endl;
        return 2;
    }
    string appIdString = argv[2];
    string remotecachePath = argv[3];
    string gameName = (argc > 4) ? argv[4] : "";
    if (!isAllDigits(appIdString))
    {
        cout << ">Invalid appid: " << appIdString << endl;
        return 2;
    }

    SetEnvironmentVariableA("SteamAppId", appIdString.c_str());
    SetEnvironmentVariableA("SteamGameId", appIdString.c_str());
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
    if (api.initFlat(initErrMsg) != 0)
    {
        cout << ">Error: could not connect to " << formatGameLabel(atoi(appIdString.c_str()), gameName)
             << (initErrMsg[0] != '\0' ? ": " + string(initErrMsg) : "") << endl;
        DeleteFileA(appidFilePath.c_str());
        FreeLibrary(steamApiModule);
        return 1;
    }

    void *remoteStorage = api.remoteStorageAccessor();
    if (!remoteStorage)
    {
        cout << ">Error: Steam Remote Storage is unavailable." << endl;
        api.shutdown();
        DeleteFileA(appidFilePath.c_str());
        FreeLibrary(steamApiModule);
        return 1;
    }
    if (!api.isCloudEnabledForApp(remoteStorage))
    {
        api.setCloudEnabledForApp(remoteStorage, true);
    }

    cout << ">Waiting for Steam to sync the cloud record" << flush;
    bool appeared = false;
    int lastDotSecond = 0;
    int reportedCount = -1;
    for (int waited = 0; waited <= 12000; waited += 200)
    {
        api.runCallbacks();

        if (api.getFileCount)
        {
            int cloudCount = api.getFileCount(remoteStorage);
            if (cloudCount != reportedCount)
            {
                reportedCount = cloudCount;
                if (cloudCount > 0)
                {
                    cout << endl << ">Steam lists " << cloudCount << " cloud file(s)." << endl;
                }
            }
        }
        error_code ec;
        if (fs::exists(pathFromUtf8(remotecachePath), ec))
        {
            appeared = true;
            break;
        }
        if (waited / 1000 > lastDotSecond)
        {
            lastDotSecond = waited / 1000;
            cout << "." << flush;
        }
        Sleep(200);
    }
    cout << endl;

    if (appeared)
    {
        for (int waited = 0; waited < 1500; waited += 250)
        {
            api.runCallbacks();
            Sleep(250);
        }
    }

    api.shutdown();
    DeleteFileA(appidFilePath.c_str());
    FreeLibrary(steamApiModule);

    if (!appeared)
    {
        for (int waited = 0; waited < 12000; waited += 250)
        {
            error_code ec;
            if (fs::exists(pathFromUtf8(remotecachePath), ec))
            {
                appeared = true;
                break;
            }
            Sleep(250);
        }
    }

    if (appeared)
    {
        cout << ">Steam created the cloud record." << endl;
        return 0;
    }
    cout << ">Steam did not create a cloud record." << endl;
    return 3;
#else
    (void)argc;
    (void)argv;
    cout << ">Error: Steam Cloud lookup is only supported on Windows builds." << endl;
    return 1;
#endif
}

#ifdef _WIN32

    bool emptyCloudFilesToZero(int appId,
                               const vector<CloudFileInfo> &files,
                               const string &userDataPath,
                               const string &steamRoot,
                               const vector<string> &libraryPaths,
                               const string &manifestPath,
                               const string &gameName,
                               set<string> &outEmptied)
    {
        outEmptied.clear();

        struct Entry
        {
            string name;
            int root = 0;
            fs::path target;
            fs::path backup;
            bool existed = false;
        };

        error_code ec;
        fs::path backupDir = fs::temp_directory_path() /
                             ("steam-tweak-empty-" + to_string(GetCurrentProcessId()) + "-" +
                              to_string(appId));
        fs::create_directories(backupDir, ec);

        vector<Entry> entries;
        for (const auto &file : files)
        {
            fs::path target;
            if (file.root == 0)
            {
                if (!isSafeCloudFileName(file.name))
                {
                    continue;
                }
                target = pathFromUtf8(userDataPath + "/" + to_string(appId) + "/remote/" + file.name);
            }
            else
            {
                string resolved;
                if (!resolveLocalUfsPath(file, appId, steamRoot, libraryPaths, manifestPath,
                                         gameName, resolved))
                {
                    continue;
                }
                target = pathFromUtf8(resolved);
            }

            Entry entry;
            entry.name = file.name;
            entry.root = file.root;
            entry.target = target;
            if (fs::exists(target, ec))
            {
                entry.backup = backupDir / (to_string(entries.size()) + ".bak");
                fs::copy_file(target, entry.backup, fs::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    cout << ">Could not back up " << file.name << "; skipping it." << endl;
                    continue;
                }
                entry.existed = true;
            }
            entries.push_back(entry);
        }

        vector<Entry> ready;
        for (const auto &entry : entries)
        {
            fs::create_directories(entry.target.parent_path(), ec);
            ofstream output(entry.target, ios::binary | ios::trunc);
            if (!output)
            {
                cout << ">Could not empty " << entry.name << "; skipping it." << endl;
                continue;
            }
            ready.push_back(entry);
        }
        if (ready.empty())
        {
            fs::remove_all(backupDir, ec);
            return false;
        }

        string syncError;
        bool syncRequested = cdpPreferLocalAppSync(appId, syncError);
        if (!syncRequested)
        {
            syncRequested = cdpRetryAppSync(appId, syncError);
        }
        if (!syncRequested)
        {
            cout << ">Steam did not sync " << formatGameLabel(appId, gameName);
            if (!syncError.empty())
            {
                cout << " (" << syncError << ")";
            }
            cout << "; restoring files." << endl;
            for (const auto &entry : ready)
            {
                if (entry.existed)
                {
                    fs::copy_file(entry.backup, entry.target,
                                  fs::copy_options::overwrite_existing, ec);
                }
                else
                {
                    fs::remove(entry.target, ec);
                }
            }
            fs::remove_all(backupDir, ec);
            return false;
        }

        cout << ">Waiting for Steam to upload" << flush;
        vector<CdpCloudFile> pageFiles;
        string pageError;
        bool pageReadable = false;
        for (int attempt = 0; attempt < 12; attempt++)
        {
            Sleep(5000);
            pageFiles.clear();
            pageReadable = cdpFetchCloudFiles(appId, pageFiles, pageError, true);
            if (!pageReadable)
            {
                continue;
            }
            bool allEmpty = true;
            bool emptiedAgain = false;
            for (const auto &entry : ready)
            {
                bool found = false;
                unsigned long long size = 0;
                for (const auto &pageFile : pageFiles)
                {
                    if (pageFile.root == entry.root && pageFile.name == entry.name)
                    {
                        found = true;
                        size = pageFile.size;
                        break;
                    }
                }
                if (!found || size == 0)
                {
                    continue;
                }
                allEmpty = false;
                ofstream output(entry.target, ios::binary | ios::trunc);
                if (output)
                {
                    emptiedAgain = true;
                }
            }
            if (allEmpty)
            {
                break;
            }
            if (emptiedAgain)
            {
                string retryError;
                cdpPreferLocalAppSync(appId, retryError);
            }
            cout << "." << flush;
        }
        cout << endl;

        int emptiedCount = 0;
        vector<string> stillListed;
        for (const auto &entry : ready)
        {
            bool found = false;
            unsigned long long size = 0;
            for (const auto &pageFile : pageFiles)
            {
                if (pageFile.root == entry.root && pageFile.name == entry.name)
                {
                    found = true;
                    size = pageFile.size;
                    break;
                }
            }
            if (pageReadable && (!found || size == 0))
            {
                outEmptied.insert(entry.name);
                emptiedCount++;
                continue;
            }
            stillListed.push_back(found ? entry.name + " (" + to_string(size) + " B)"
                                        : entry.name);
            if (entry.existed)
            {
                fs::copy_file(entry.backup, entry.target,
                              fs::copy_options::overwrite_existing, ec);
            }
            else
            {
                fs::remove(entry.target, ec);
            }
        }
        cout << ">Emptied " << emptiedCount << " of " << ready.size() << " file(s) to 0 B" << endl;
        for (const auto &name : stillListed)
        {
            cout << ">[" << name << "] (still listed)" << endl;
        }

        fs::remove_all(backupDir, ec);
        return emptiedCount > 0;
    }

#endif

void CloudDataDeleter::run(const string &steamRoot, const vector<string> &libraryPaths)
{
#ifdef _WIN32

    QuickEditGuard quickEditGuard;
#endif
    string userId;
    string personaName;
    if (!resolveCurrentUser(steamRoot, userId, personaName))
    {
        cout << ">Error: could not find the current Steam user." << endl;
        return;
    }

    cout << ">Steam user: " << personaName << " (" << userId << ")" << endl;

    string userDataPath = steamRoot + "/userdata/" + userId;
    if (!fs::exists(userDataPath))
    {
        cout << ">Error: userdata folder not found for user " << userId << endl;
        return;
    }

    string sharedConfigPath = userDataPath + "/7/remote/sharedconfig.vdf";
    bool steamJustRestarted = false;
    if (fs::exists(sharedConfigPath) && cloudSwitchesPresent(sharedConfigPath))
    {
        if (!allCloudSwitchesOn(sharedConfigPath))
        {
            cout << ">Enabling Steam Cloud" << endl;
            if (enableCloudEverywhereForAllUsers(steamRoot) > 0)
            {
                cout << ">Error: could not enable Steam Cloud for every user; retry, or turn Cloud on in Steam -> Settings -> Cloud." << endl;
                return;
            }

#ifdef _WIN32
            if (!restartSteamAutomatically(steamRoot, userId, personaName, sharedConfigPath,
                                            false,  true))
            {
                return;
            }
            steamJustRestarted = true;
#else
            cout << ">Restart Steam to apply the new settings, then run option 4 again." << endl;
            return;
#endif

        }
    }

    bool cdpRestartAttempted = steamJustRestarted;
    if (steamJustRestarted)
    {
        waitForCdpAvailable(20000);
    }
    if (!cdpIsAvailable())
    {
        if (isSteamInGame())
        {
            cout << ">A game is running; only local records are listed." << endl;
        }
        else if (!cdpRestartAttempted)
        {
            cdpRestartAttempted = true;
            cout << ">Steam's cloud page is unavailable; restarting Steam in debug mode" << endl;
            if (restartSteamAutomatically(steamRoot, userId, personaName, "",
                                           false,  true))
            {
                waitForCdpAvailable(20000);
            }
        }
        else
        {
            cout << ">Could not open Steam's cloud page; using the local list." << endl;
        }
    }

    map<int, string> manifests = buildManifestMap(libraryPaths);
    bool cloudPageWarningShown = false;
    bool cloudPageAvailable = false;

    set<int> justEmptiedApps;
    map<int, PendingUpload> pendingUploads;
    loadPendingUploads(pendingUploads, userDataPath);

    while (true)
    {
        vector<CloudGameInfo> games = scanCloudGames(userDataPath, manifests);

        vector<CdpCloudGame> cloudGames;
        string cloudPageError;
        if (cdpFetchCloudGames(cloudGames, cloudPageError))
        {
            cloudPageAvailable = true;
            mergeCloudPageGames(games, cloudGames, manifests, justEmptiedApps);
            justEmptiedApps.clear();
            cloudPageWarningShown = false;
        }
        else if (!cloudPageWarningShown)
        {
            cout << ">Could not read Steam's cloud page (" << cloudPageError << "); using the local list." << endl;
            cloudPageWarningShown = true;
        }

        for (auto &game : games)
        {
            if (game.totalSize != 0 || game.fileCount <= 0)
            {
                continue;
            }
            vector<CdpCloudFile> pageFiles;
            string pageError;
            if (!cdpFetchCloudFiles(game.appId, pageFiles, pageError, true, game.fileCount))
            {
                continue;
            }
            unsigned long long totalSize = 0;
            int fileCount = 0;
            for (const auto &file : pageFiles)
            {
                if (file.root < 0)
                {
                    continue;
                }
                totalSize += file.size;
                fileCount++;
            }
            if (fileCount > 0)
            {
                game.fileCount = fileCount;
                game.totalSize = totalSize;
            }
        }

        cout << endl;
        for (auto it = pendingUploads.begin(); it != pendingUploads.end();)
        {
            bool recordLists = false;
            set<string> remaining;
            if (readRemainingCloudNames(it->second.remotecachePath, remaining))
            {
                for (const auto &name : it->second.fileNames)
                {
                    if (remaining.count(name) != 0)
                    {
                        recordLists = true;
                        break;
                    }
                }
            }

            size_t listedIndex = games.size();
            for (size_t index = 0; index < games.size(); index++)
            {
                if (games[index].appId == it->first)
                {
                    listedIndex = index;
                    break;
                }
            }
            bool stillHasFiles = listedIndex < games.size() && games[listedIndex].fileCount > 0;
            if (!recordLists && !stillHasFiles)
            {
                justEmptiedApps.insert(it->first);
                if (listedIndex < games.size())
                {
                    games.erase(games.begin() + static_cast<ptrdiff_t>(listedIndex));
                }
                it = pendingUploads.erase(it);
                continue;
            }
            ++it;
        }
        savePendingUploads(pendingUploads);

        cout << ">Games with Steam Cloud data:" << endl;
        if (games.empty())
        {
            cout << ">No games with Steam Cloud data." << endl;
        }
        const set<int> conflictedApps = readConflictedApps(steamRoot);
        int number = 1;
        for (const auto &game : games)
        {
            cout << ">" << number << ". " << formatGameLabel(game.appId, game.name)
                 << " [" << (game.installed ? "installed" : "not installed") << "] - "
                 << game.fileCount << (game.fileCount == 1 ? " file, " : " files, ")
                 << (game.totalSizeText.empty() ? formatSize(game.totalSize)
                                                : game.totalSizeText);
            if (!game.hasLocalRecord)
            {
                cout << " [cloud only]";
            }
            if (conflictedApps.count(game.appId) != 0)
            {
                cout << " [Conflicted]";
            }
            else if (pendingUploads.count(game.appId) != 0)
            {
                cout << " [Currently Deleting]";
            }
            cout << endl;
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

        int choice = 0;
        if (!parsePromptNumber(input, choice))
        {
            cout << endl;
            cout << ">Invalid input" << endl;
            continue;
        }
        if (choice == 0)
        {
            return;
        }
        cout << endl;

        if (choice < 1 || choice > static_cast<int>(games.size()))
        {
            cout << ">Invalid input" << endl;
            continue;
        }

        CloudGameInfo game = games[choice - 1];

        bool liveConflicted = false;
        if (conflictedApps.count(game.appId) == 0)
        {
            vector<int> single(1, game.appId);
            set<int> liveConflicts;
            string conflictStateError;
            if (cdpFetchConflictingApps(single, liveConflicts, conflictStateError))
            {
                liveConflicted = liveConflicts.count(game.appId) != 0;
            }
        }
        bool gameConflicted = conflictedApps.count(game.appId) != 0 || liveConflicted;
        if (gameConflicted)
        {
            if (offerConflictRestart(game.appId, steamRoot, userId, personaName, liveConflicted))
            {
                continue;
            }
            cout << endl;
        }

        {
            auto pendingIt = pendingUploads.find(game.appId);
            if (pendingIt != pendingUploads.end())
            {
                PendingBatchOutcome outcome =
                    waitForPendingBatchToSettle(game.appId, steamRoot,
                                                pendingIt->second.remotecachePath,
                                                pendingIt->second.fileNames);
                if (!outcome.safeToContinue)
                {
                    cout << ">Previous deletion is still uploading." << endl;
                    continue;
                }
                pendingUploads.erase(pendingIt);
                savePendingUploads(pendingUploads);
                if (outcome.finishedClean)
                {
                    cout << ">Previous deletion finished; nothing to re-queue." << endl;
                    continue;
                }
                cout << ">Previous deletion finished; " << outcome.leftoverCount
                     << " file(s) still listed." << endl;
            }
        }

        string manifestPath;
        {
            auto manifestIt = manifests.find(game.appId);
            if (manifestIt != manifests.end())
            {
                manifestPath = manifestIt->second;
            }
        }

        string remotecachePath = userDataPath + "/" + to_string(game.appId) + "/remotecache.vdf";
        FileUtility fileUtility;
        vector<CloudFileInfo> files;
        error_code cacheEc;
        bool hasLocalRecord = fs::exists(pathFromUtf8(remotecachePath), cacheEc);
        if (hasLocalRecord)
        {
            try
            {
                parseRemotecache(fileUtility.readFileContents(remotecachePath), files);
            }
            catch (const exception &)
            {
                files.clear();
            }
        }

        bool filesFromCloudPage = false;
        vector<bool> hasLocalCopy;
        files.erase(remove_if(files.begin(), files.end(),
                              [](const CloudFileInfo &file) { return file.root < 0; }),
                    files.end());
        bool recordMatchesList = hasLocalRecord && !files.empty() &&
                                 game.fileCount > 0 &&
                                 files.size() == static_cast<size_t>(game.fileCount);
        if (recordMatchesList)
        {
            for (const auto &file : files)
            {
                if (file.root == 0)
                {
                    continue;
                }
                string localPath;
                if (!resolveLocalUfsPath(file, game.appId, steamRoot, libraryPaths,
                                         manifestPath, game.name, localPath))
                {
                    recordMatchesList = false;
                    break;
                }
                error_code localEc;
                fs::path localFile = pathFromUtf8(localPath);
                if (!fs::exists(localFile, localEc))
                {
                    recordMatchesList = false;
                    break;
                }
                if (file.size != 0 && fs::file_size(localFile, localEc) == 0)
                {
                    recordMatchesList = false;
                    break;
                }
            }
        }
        if (!recordMatchesList)
        {
            vector<CloudFileInfo> localFiles = files;
            vector<bool> localUsed(localFiles.size(), false);
            vector<CdpCloudFile> cloudFiles;
            string cloudFileError;
            bool pageReadable =
                cdpFetchCloudFiles(game.appId, cloudFiles, cloudFileError, true, game.fileCount);
            if (pageReadable)
            {
                cloudPageAvailable = true;
            }
            if (!pageReadable)
            {
                if (files.empty())
                {
                    cout << ">Could not get a file list for " << formatGameLabel(game.appId, game.name)
                         << ": " << cloudFileError << endl;
                    if (hasLocalRecord)
                    {
                        cout << ">Local record is empty; nothing to delete." << endl;
                    }
                    else
                    {
                        cout << ">Steam lists no files; if the game was delisted, Steam Support must restore it." << endl;
                    }
                    continue;
                }
                cout << ">Could not read Steam's cloud page; using the local record." << endl;
            }
            else
            {
                files.clear();
                auto sizesMatch = [](unsigned long long left, unsigned long long right)
                {
                    if (left == right)
                    {
                        return true;
                    }
                    if (left == 0 || right == 0)
                    {
                        return false;
                    }
                    unsigned long long larger = left > right ? left : right;
                    unsigned long long difference = left > right ? left - right : right - left;
                    return difference <= 1024ULL || difference * 100ULL <= larger * 5ULL;
                };
                for (const auto &cloudFile : cloudFiles)
                {
                    if (cloudFile.root < 0)
                    {
                        cout << ">Skipping " << cloudFile.name
                             << ": unrecognized cloud root \"" << cloudFile.rootLabel << "\"." << endl;
                        continue;
                    }
                    CloudFileInfo file;
                    file.name = cloudFile.name;
                    file.root = cloudFile.root;
                    file.size = cloudFile.size;
                    file.sizeText = cloudFile.sizeText;
                    file.downloadUrl = cloudFile.downloadUrl;

                    size_t bestMatch = localFiles.size();
                    int bestScore = -1;
                    for (size_t i = 0; i < localFiles.size(); i++)
                    {
                        if (localUsed[i] || localFiles[i].root != cloudFile.root)
                        {
                            continue;
                        }
                        int score = -1;
                        if (localFiles[i].name == cloudFile.name)
                        {
                            score = 3;
                        }
                        else if (localFiles[i].name.size() > cloudFile.name.size() &&
                                 localFiles[i].name.compare(0, cloudFile.name.size(),
                                                            cloudFile.name) == 0 &&
                                 sizesMatch(localFiles[i].size, cloudFile.size))
                        {
                            score = 2;
                        }
                        else if (cloudFile.name.size() > 3 &&
                                 cloudFile.name.compare(cloudFile.name.size() - 3, 3, "...") == 0 &&
                                 localFiles[i].name.size() > cloudFile.name.size() - 3 &&
                                 localFiles[i].name.compare(0, cloudFile.name.size() - 3,
                                                            cloudFile.name.substr(0, cloudFile.name.size() - 3)) == 0 &&
                                 sizesMatch(localFiles[i].size, cloudFile.size))
                        {
                            score = 1;
                        }
                        if (score > bestScore ||
                            (score == bestScore && score >= 0 &&
                             localFiles[i].size == cloudFile.size &&
                             localFiles[bestMatch].size != cloudFile.size))
                        {
                            bestScore = score;
                            bestMatch = i;
                        }
                    }
                    if (bestScore >= 0)
                    {
                        file.name = localFiles[bestMatch].name;
                        localUsed[bestMatch] = true;
                    }

                    files.push_back(file);
                }
                filesFromCloudPage = true;

                if (files.empty())
                {
                    if (cloudFiles.empty())
                    {
                        cout << ">Steam lists no files; the local record is stale." << endl;
                    }
                    else
                    {
                        cout << ">No files use a supported root; nothing changed." << endl;
                    }
                    continue;
                }

                hasLocalCopy.assign(files.size(), false);
                for (size_t i = 0; i < files.size(); i++)
                {
                    if (files[i].root == 0)
                    {
                        continue;
                    }
                    string localPath;
                    if (resolveLocalUfsPath(files[i], game.appId, steamRoot, libraryPaths,
                                            manifestPath, game.name, localPath))
                    {
                        error_code localEc;
                        fs::path localFile = pathFromUtf8(localPath);
                        bool exists = fs::exists(localFile, localEc);
                        unsigned long long localSize = 0;
                        if (exists)
                        {
                            localSize = fs::file_size(localFile, localEc);
                        }

                        hasLocalCopy[i] = exists && (localSize > 0 || files[i].size == 0);
                    }
                }
            }
        }

        set<size_t> queued;
        vector<CloudFileInfo> selected;
        bool deletionRequested = false;
        bool firstFileList = true;
        auto formatFileLine = [&](size_t index)
        {
            const CloudFileInfo &file = files[index];
            string line = ">" + to_string(index + 1) + ".";
            if (queued.count(index) != 0)
            {
                line += " [QUEUED]";
            }
            line += " " + file.name + " (" +
                    (file.sizeText.empty() ? formatSize(file.size) : file.sizeText);
            if (file.root != 0)
            {
                line += ", root: " + rootNameForRoot(file.root);
                if (filesFromCloudPage && !hasLocalCopy[index])
                {
                    line += ", no local copy";
                }
            }
            if (file.size == 0)
            {
                line += ", empty file";
            }
            return line + ")";
        };
        while (true)
        {
            if (firstFileList)
            {
                cout << ">Cloud files for " << formatGameLabel(game.appId, game.name) << ":" << endl;
                bool anyRoot = false;
                for (const auto &file : files)
                {
                    anyRoot = anyRoot || file.root != 0;
                }
                if (anyRoot)
                {
                    cout << ">(auto-cloud files also delete the local copy)" << endl;
                }
                for (size_t index = 0; index < files.size(); index++)
                {
                    cout << formatFileLine(index) << endl;
                }
                firstFileList = false;
            }

            bool allQueued = (queued.size() == files.size());
            cout << ">" << (files.size() + 1) << ". "
                 << (allQueued ? "Unqueue ALL files" : "Queue ALL files") << endl;
            if (!queued.empty())
            {
                cout << ">" << (files.size() + 2) << ". Delete the " << queued.size()
                     << (queued.size() == 1 ? " queued file" : " queued files") << endl;
            }
            cout << ">0. Back to game list" << endl;

            int fileChoice = promptMenuChoice(">Select your option: " + string("\n>"));
            if (fileChoice == -1)
            {
                return;
            }
            if (fileChoice == 0)
            {
                break;
            }
            if (fileChoice == -2)
            {
                cout << endl;
                cout << ">Invalid input" << endl;
                continue;
            }
            if (fileChoice >= 1 && fileChoice <= static_cast<int>(files.size()))
            {
                cout << endl;
                size_t index = static_cast<size_t>(fileChoice - 1);
                if (!queued.insert(index).second)
                {
                    queued.erase(index);
                }
                firstFileList = true;
                continue;
            }
            if (fileChoice == static_cast<int>(files.size()) + 1)
            {
                cout << endl;
                if (allQueued)
                {
                    queued.clear();
                }
                else
                {
                    for (size_t index = 0; index < files.size(); index++)
                    {
                        queued.insert(index);
                    }
                }
                firstFileList = true;
                continue;
            }
            if (!queued.empty() && fileChoice == static_cast<int>(files.size()) + 2)
            {
                for (size_t index : queued)
                {
                    selected.push_back(files[index]);
                }
                deletionRequested = true;
                break;
            }
            cout << endl;
            cout << ">Invalid input" << endl;
        }
        if (!deletionRequested)
        {
            continue;
        }

        vector<CloudFileInfo> ufsFiles;
        for (const auto &file : selected)
        {
            if (file.root != 0)
            {
                ufsFiles.push_back(file);
            }
        }

        cout << endl;
        string gameLabel = formatGameLabel(game.appId, game.name);
        cout << ">Delete " << selected.size()
             << (selected.size() == 1 ? " cloud file" : " cloud files") << " from " << gameLabel
             << "?" << endl;
        for (const auto &file : selected)
        {
            cout << ">[" << file.name << "]" << endl;
        }
        if (!ufsFiles.empty())
        {
            cout << ">(" << ufsFiles.size() << " auto-cloud files delete the local copy too)" << endl;
        }
        bool confirmed = false;
        while (true)
        {
            cout << ">Enter 1 to confirm" << endl;
            cout << ">Enter 0 to cancel" << endl;
            cout << ">";
            string confirmInput;
            if (!getline(cin, confirmInput))
            {
                return;
            }
            if (confirmInput == "1")
            {
                confirmed = true;
                cout << endl;
                break;
            }
            if (confirmInput == "0")
            {
                break;
            }
            cout << endl;
            cout << ">Invalid input" << endl;
        }
        if (!confirmed)
        {
            cout << endl;
            cout << ">Cancelled" << endl;
            continue;
        }

#ifdef _WIN32
        if (!isSteamRunning())
        {
            cout << ">Error: Steam is not running." << endl;
            continue;
        }

        size_t requestedCount = selected.size();
        dropUnsafeCloudNames(selected, true);
        if (selected.empty())
        {
            cout << ">Error: no file could be prepared; nothing deleted." << endl;
            continue;
        }
        if (selected.size() < requestedCount)
        {
            cout << ">Skipping " << (requestedCount - selected.size())
                 << " file(s) that could not be prepared." << endl;
        }

        ufsFiles.clear();
        for (const auto &file : selected)
        {
            if (file.root != 0)
            {
                ufsFiles.push_back(file);
            }
        }

        const vector<CloudFileInfo> allUfsFiles = ufsFiles;

        string listPath;
        {
            string tempDir = fs::temp_directory_path().string();
            listPath = tempDir + "\\steam-tweak-tool-cloud-" + to_string(GetCurrentProcessId()) + ".txt";
        }
        auto writeWorkerList = [&](const vector<CloudFileInfo> &files) -> bool
        {
            ofstream listFile(listPath, ios::trunc);
            if (!listFile)
            {
                return false;
            }
            for (const auto &file : files)
            {

                string localPath;
                if (file.root != 0 &&
                    !resolveLocalUfsPath(file, game.appId, steamRoot, libraryPaths,
                                        manifestPath, game.name, localPath))
                {
                    localPath.clear();
                }
                listFile << file.root << "\t" << localPath << "\t" << file.name << "\n";
            }
            return true;
        };
        if (!writeWorkerList(selected))
        {
            cout << ">Error: could not create temporary file list: " << listPath << endl;
            continue;
        }
        const size_t attemptedCount = selected.size();

        vector<string> deletedNames;
        for (const auto &file : selected)
        {
            deletedNames.push_back(file.name);
        }

        vector<string> ufsLocalPaths;
        for (const auto &file : ufsFiles)
        {
            string localPath;
            if (resolveLocalUfsPath(file, game.appId, steamRoot, libraryPaths, manifestPath,
                                    game.name, localPath))
            {
                ufsLocalPaths.push_back(localPath);
            }
        }

        bool deletionQueued = false;
        bool workerLaunchFailed = false;
        int exitCode = 1;
        set<string> emptiedNames;

        {
            bool needsInitialSync = (!game.hasLocalRecord && !ufsFiles.empty());
            if (needsInitialSync)
            {
                cout << ">Registering the cloud record in Steam" << endl;
                int syncExitCode = 1;
                runCloudSyncWorkerProcess(to_string(game.appId), remotecachePath, game.name, syncExitCode);

                Sleep(1000);
            }

            string exePath = getExecutablePath();
            string commandLine = "\"" + exePath + "\" --cloud-worker " + to_string(game.appId) +
                                 " \"" + listPath + "\" \"" + game.name + "\"";

            STARTUPINFOA startupInfo;
            ZeroMemory(&startupInfo, sizeof(startupInfo));
            startupInfo.cb = sizeof(startupInfo);

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

            string workingDir = getExecutableDirectory();
            cout << ">Deleting " << attemptedCount << (attemptedCount == 1 ? " file" : " files") << endl;
            if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, workingDir.data(), &startupInfo, &processInfo))
            {
                printLastError("Launching cloud worker process");
                if (nullStderr != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(nullStderr);
                }
                workerLaunchFailed = true;
                break;
            }

            DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 60000);
            if (waitResult == WAIT_TIMEOUT)
            {
                cout << ">Worker timed out; stopping it." << endl;
                TerminateProcess(processInfo.hProcess, 1);
                WaitForSingleObject(processInfo.hProcess, 5000);
            }
            DWORD workerExitCode = 1;
            GetExitCodeProcess(processInfo.hProcess, &workerExitCode);
            exitCode = static_cast<int>(workerExitCode);
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            if (nullStderr != INVALID_HANDLE_VALUE)
            {
                CloseHandle(nullStderr);
            }

            if (exitCode == 0)
            {
                string syncError;
                set<string> selectedNames(deletedNames.begin(), deletedNames.end());
                CloudBatchStatus afterBatch =
                    readCloudBatchStatus(steamRoot, game.appId, selectedNames);
                if (gameConflicted || afterBatch.conflicted)
                {
                    if (cdpPreferLocalAppSync(game.appId, syncError))
                    {
                        cout << ">Conflict resolved with this PC's copy." << endl;
                        gameConflicted = false;
                    }
                    else
                    {
                        cdpRetryAppSync(game.appId, syncError);
                        cout << ">Cloud sync is conflicted; deletions stay local. Restart Steam and retry." << endl;
                    }
                }
                else
                {
                    cdpRetryAppSync(game.appId, syncError);
                }

                deletionQueued = true;
            }
        }

        fs::remove(listPath);

        if (workerLaunchFailed)
        {
            continue;
        }

        if (!deletionQueued)
        {
            set<string> remaining;
            bool recordReadable = readRemainingCloudNames(remotecachePath, remaining);
            bool pageChecked = false;
            vector<CloudFileInfo> stillListedFiles;
            if (recordReadable)
            {
                for (const auto &file : selected)
                {
                    if (remaining.count(file.name) != 0)
                    {
                        stillListedFiles.push_back(file);
                    }
                }
            }
            else
            {
                vector<CdpCloudFile> pageFiles;
                string pageError;
                if (cdpFetchCloudFiles(game.appId, pageFiles, pageError, true))
                {
                    pageChecked = true;
                    for (const auto &file : selected)
                    {
                        for (const auto &pageFile : pageFiles)
                        {
                            if (pageFile.root != file.root)
                            {
                                continue;
                            }
                            if (pageFile.name == file.name ||
                                (file.name.size() > pageFile.name.size() &&
                                 file.name.compare(0, pageFile.name.size(),
                                                   pageFile.name) == 0))
                            {
                                stillListedFiles.push_back(file);
                                break;
                            }
                        }
                    }
                }
            }

            if (stillListedFiles.empty())
            {
                if (!recordReadable && !pageChecked)
                {
                    cout << ">Could not read Steam's file list." << endl;
                }
                else
                {
                    cout << ">Steam no longer lists these files." << endl;
                }
            }

            if (!stillListedFiles.empty())
            {
                cout << ">Steam refused the deletion; emptying to 0 B." << endl;
                set<string> emptied;
                emptyCloudFilesToZero(game.appId, stillListedFiles, userDataPath, steamRoot,
                                      libraryPaths, manifestPath, game.name, emptied);
                emptiedNames.insert(emptied.begin(), emptied.end());
                if (stillListedFiles.size() > emptiedNames.size())
                {
                    cout << ">Could not delete or empty " << (stillListedFiles.size() - emptiedNames.size()) << " file(s); retry." << endl;
                }
            }
            else if (exitCode == 1)
            {
                cout << ">Finished with failures; retry." << endl;
            }
            else
            {
                cout << ">Worker failed (exit " << exitCode << "); retry." << endl;
            }

            if (!stillListedFiles.empty() && emptiedNames.size() < stillListedFiles.size())
            {
                cout << endl;
                offerConflictRestart(game.appId, steamRoot, userId, personaName, false);
            }
            continue;
        }

        cout << ">Success" << endl;

        if (!allUfsFiles.empty())
        {
            for (const auto &file : allUfsFiles)
            {
                string localPath;
                if (!resolveLocalUfsPath(file, game.appId, steamRoot, libraryPaths,
                                        manifestPath, game.name, localPath))
                {
                    continue;
                }
                error_code ec;
                if (fs::exists(pathFromUtf8(localPath), ec))
                {
                    fs::remove(pathFromUtf8(localPath), ec);
                }
            }
        }
        PendingUpload upload;
        upload.remotecachePath = remotecachePath;
        upload.fileNames = deletedNames;
        upload.queuedAt = static_cast<long long>(time(nullptr));
        pendingUploads[game.appId] = upload;
        savePendingUploads(pendingUploads);
        if (postDeletionFollowUp(steamRoot, userId, personaName, libraryPaths,
                                 cloudPageAvailable, game.appId, remotecachePath, deletedNames))
        {
            returnToMainMenuAfterCleanup();
            return;
        }
#else
        cout << ">Error: Steam Cloud deletion is only supported on Windows builds." << endl;
#endif

    }
}
