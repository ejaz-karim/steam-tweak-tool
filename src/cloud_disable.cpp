#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <vector>
#include "steam-tweak-tool/cloud_disable.hpp"
#include "steam-tweak-tool/utility.hpp"
using namespace std;
namespace
{
string trimS(const string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == string::npos)
        return "";
    return s.substr(b, e - b + 1);
}
string indentOf(const string &s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == string::npos)
        return s;
    return s.substr(0, b);
}
string readQuoted(const string &s, size_t &i, bool &terminated)
{
    size_t q1 = i;
    size_t q2 = q1 + 1;
    while (q2 < s.size())
    {
        if (s[q2] == '\\')
        {
            q2 += 2;
            continue;
        }
        if (s[q2] == '"')
            break;
        ++q2;
    }
    terminated = (q2 < s.size());
    string raw = s.substr(q1 + 1, (q2 < s.size() ? q2 : s.size()) - q1 - 1);
    i = (q2 < s.size()) ? q2 + 1 : s.size();
    string decoded;
    for (size_t k = 0; k < raw.size(); ++k)
    {
        if (raw[k] == '\\' && k + 1 < raw.size())
        {
            decoded += raw[k + 1];
            k++;
        }
        else
        {
            decoded += raw[k];
        }
    }
    return decoded;
}
string appendClosingQuote(const string &s)
{
    size_t k = s.size();
    while (k > 0 && s[k - 1] == '\\')
        --k;
    size_t nbs = s.size() - k;
    string out = s;
    if (nbs % 2 == 1)
        out += '\\';
    out += '"';
    return out;
}
bool ieq(const string &a, const string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}
bool isMangledOf(const string &name, const string &target, int maxEdits = -1)
{
    if (ieq(name, target))
        return true;
    if (name.size() < 2 || target.size() < 3)
        return false;
    int cap = (maxEdits >= 0) ? maxEdits :
               (target.size() > 14 ? 3 : (target.size() >= 8 ? 2 : 1));
    if (abs((int)name.size() - (int)target.size()) > cap)
        return false;
    string a = name, b = target;
    for (auto &c : a) c = (char)tolower((unsigned char)c);
    for (auto &c : b) c = (char)tolower((unsigned char)c);
    size_t n = a.size(), m = b.size();
    if (n + 1 > 32 || m + 1 > 32)
        return false;
    vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= n; ++i)
    {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; ++j)
        {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        swap(prev, cur);
    }
    return prev[m] <= cap;
}
bool isDeletionOf(const string &name, const string &target, size_t maxDeleted = 3)
{
    if (ieq(name, target))
        return true;
    if (name.size() < 2 || target.size() < 3 || name.size() > target.size())
        return false;
    if (target.size() - name.size() > maxDeleted)
        return false;
    size_t i = 0, j = 0;
    while (i < name.size() && j < target.size())
    {
        if (tolower((unsigned char)name[i]) == tolower((unsigned char)target[j]))
            ++i;
        ++j;
    }
    return i == name.size();
}
vector<string> splitKeepingNewlines(const string &text)
{
    vector<string> out;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find_first_of("\r\n", start);
        if (end == string::npos)
        {
            out.push_back(text.substr(start));
            break;
        }
        size_t after = end + 1;
        if (text[end] == '\r' && after < text.size() && text[after] == '\n')
            after++;
        out.push_back(text.substr(start, after - start));
        start = after;
    }
    return out;
}
string detectEol(const string &text)
{
    return text.find("\r\n") != string::npos ? "\r\n" : "\n";
}
}
namespace
{
enum TokKind
{
    TK_RAW,
    TK_OPEN,
    TK_CLOSE,
    TK_NAME,
    TK_LEAF
};
struct Tok
{
    TokKind kind;
    string raw;
    string key;
};
vector<Tok> scanLine(const string &body)
{
    vector<Tok> out;
    size_t n = body.size();
    size_t tokStart = 0;
    size_t i = 0;
    auto emit = [&](TokKind k, const string &key, size_t end) {
        Tok t;
        t.kind = k;
        t.key = key;
        t.raw = body.substr(tokStart, end - tokStart);
        out.push_back(std::move(t));
        tokStart = end;
    };
    while (i < n)
    {
        char c = body[i];
        if (c == '{' || c == '}')
        {
            emit(c == '{' ? TK_OPEN : TK_CLOSE, "", i + 1);
            i++;
        }
        else if (c == '"')
        {
            bool keyTerm = true;
            string key = readQuoted(body, i, keyTerm);
            size_t j = i;
            while (j < n && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r'))
                j++;
            if (j < n && body[j] == '"')
            {
                bool valTerm = true;
                i = j;
                readQuoted(body, i, valTerm);
                emit(TK_LEAF, key, i);
                if (!valTerm)
                    out.back().raw = appendClosingQuote(out.back().raw);
            }
            else
            {
                emit(TK_NAME, key, j);
                i = j;
                if (!keyTerm)
                    out.back().raw = appendClosingQuote(out.back().raw);
            }
        }
        else
        {
            size_t j = i;
            while (j < n && body[j] != '"' && body[j] != '{' && body[j] != '}')
                j++;
            if (body.substr(i, j - i).find_first_not_of(" \t\r") != string::npos)
                emit(TK_RAW, "", j);
            i = j;
        }
    }
    if (tokStart < n)
        emit(TK_RAW, "", n);
    return out;
}
vector<Tok> tokenize(const vector<string> &physical)
{
    vector<Tok> out;
    out.reserve(physical.size());
    for (const auto &rawLine : physical)
    {
        string body = rawLine;
        string nl;
        if (!body.empty() && body.back() == '\n')
        {
            nl = "\n";
            body.pop_back();
            if (!body.empty() && body.back() == '\r')
            {
                nl = "\r\n";
                body.pop_back();
            }
        }
        else if (!body.empty() && body.back() == '\r')
        {
            nl = "\r";
            body.pop_back();
        }
        string t = trimS(body);
        if (t.size() >= 2 && t[0] == '/' && t[1] == '/')
        {
            Tok tk;
            tk.kind = TK_RAW;
            tk.key = "";
            tk.raw = rawLine;
            out.push_back(std::move(tk));
            continue;
        }
        vector<Tok> line = scanLine(body);
        if (!line.empty())
        {
            line.back().raw += nl;
            for (auto &tk : line)
                out.push_back(std::move(tk));
        }
        else
        {
            Tok tk;
            tk.kind = TK_RAW;
            tk.key = "";
            tk.raw = rawLine;
            out.push_back(std::move(tk));
        }
    }
    return out;
}
}
struct Seg
{
    bool isBlock = false;
    string name;
    string header;
    vector<Seg> kids;
    string close;
    bool generated = false;
    string genText;
    bool drop = false;
};
static string renderSeg(const Seg &s)
{
    if (s.drop)
        return "";
    if (s.generated)
        return s.genText;
    string o = s.header;
    if (s.isBlock)
    {
        for (const auto &k : s.kids)
            o += renderSeg(k);
        if (!s.close.empty())
            o += s.close;
        else
        {
            string eol = "\n";
            if (s.header.find("\r\n") != string::npos)
                eol = "\r\n";
            o += indentOf(s.header) + "}" + eol;
        }
    }
    return o;
}
namespace
{
const int MAX_DEPTH = 2048;
const size_t MAX_INDENT = 64;
vector<Seg> parseChildren(const vector<Tok> &tokens, size_t &idx, bool isTop, int depth = 0)
{
    vector<Seg> out;
    string rawBuf;
    auto flushRaw = [&]() {
        if (!rawBuf.empty())
        {
            Seg r;
            r.header = rawBuf;
            out.push_back(std::move(r));
            rawBuf.clear();
        }
    };
    while (idx < tokens.size())
    {
        const Tok &tk = tokens[idx];
        if (tk.kind == TK_RAW)
        {
            rawBuf += tk.raw;
            idx++;
            continue;
        }
        if (tk.kind == TK_CLOSE)
        {
            if (isTop)
            {
                rawBuf += tk.raw;
                idx++;
                continue;
            }
            flushRaw();
            return out;
        }
        if (tk.kind == TK_OPEN)
        {
            rawBuf += tk.raw;
            idx++;
            continue;
        }
        if (tk.kind == TK_LEAF)
        {
            flushRaw();
            Seg leaf;
            leaf.name = tk.key;
            leaf.header = tk.raw;
            out.push_back(std::move(leaf));
            idx++;
            continue;
        }
        {
            flushRaw();
            Seg node;
            node.name = tk.key;
            node.header = tk.raw;
            idx++;
            if (idx < tokens.size() && tokens[idx].kind == TK_OPEN)
            {
                if (depth >= MAX_DEPTH)
                {
                    string flat = node.header + tokens[idx].raw;
                    idx++;
                    int bal = 1;
                    while (idx < tokens.size() && bal > 0)
                    {
                        if (tokens[idx].kind == TK_OPEN)
                            bal++;
                        else if (tokens[idx].kind == TK_CLOSE)
                            bal--;
                        flat += tokens[idx].raw;
                        idx++;
                    }
                    Seg raw;
                    raw.header = flat;
                    out.push_back(std::move(raw));
                    continue;
                }
                node.isBlock = true;
                node.header += tokens[idx].raw;
                idx++;
                node.kids = parseChildren(tokens, idx, false, depth + 1);
                if (idx < tokens.size() && tokens[idx].kind == TK_CLOSE)
                {
                    node.close = tokens[idx].raw;
                    idx++;
                }
            }
            else
            {
                bool knownBlockName = ieq(node.name, "friendsui") ||
                                      ieq(node.name, "FriendsUI") ||
                                      ieq(node.name, "apps") ||
                                      ieq(node.name, "JSClientStorage") ||
                                      ieq(node.name, "WebStorage");
                size_t k = idx;
                while (k < tokens.size() && tokens[k].kind == TK_RAW)
                    k++;
                bool implicitBlock = false;
                if (knownBlockName && k < tokens.size())
                {
                    const Tok &nxt = tokens[k];
                    if (nxt.kind == TK_LEAF || nxt.kind == TK_NAME)
                        implicitBlock =
                            indentOf(nxt.raw).size() > indentOf(node.header).size();
                }
                if (implicitBlock)
                {
                    node.isBlock = true;
                    string eigen = node.header.find("\r\n") != string::npos ? "\r\n" : "\n";
                    node.header += indentOf(node.header) + "{" + eigen;
                    node.kids = parseChildren(tokens, idx, false, depth + 1);
                    if (idx < tokens.size() && tokens[idx].kind == TK_CLOSE)
                    {
                        node.close = tokens[idx].raw;
                        idx++;
                    }
                }
            }
            out.push_back(std::move(node));
        }
    }
    flushRaw();
    return out;
}
void collectSteamRecursive(Seg &block, vector<Seg *> &out)
{
    if (block.isBlock && ieq(block.name, "Steam"))
        out.push_back(&block);
    for (auto &k : block.kids)
        collectSteamRecursive(k, out);
}
}
namespace
{
bool isSteamLevel(const Seg &s)
{
    if (s.isBlock)
        return ieq(s.name, "friendsui") || ieq(s.name, "FriendsUI") ||
               isDeletionOf(s.name, "friendsui");
    if (isDeletionOf(s.name, "DeskopShortcutCheck") || isDeletionOf(s.name, "PlaySoundOnToast") ||
        isDeletionOf(s.name, "DisableAllToasts") || isDeletionOf(s.name, "DisableToastsInGame"))
        return false;
    if (ieq(s.name, "SurveyDate") || ieq(s.name, "SurveyDateVersion") ||
        ieq(s.name, "StartMenuShortcutCheck") || ieq(s.name, "DesktopShortcutCheck") ||
        ieq(s.name, "SteamDefaultDialog") || ieq(s.name, "ShowScreenshotManager") ||
        ieq(s.name, "CloudEnabled") || ieq(s.name, "cloudenabled") ||
        ieq(s.name, "FriendsUIJSON"))
        return true;
    return isDeletionOf(s.name, "SurveyDate") || isDeletionOf(s.name, "SurveyDateVersion") ||
           isDeletionOf(s.name, "StartMenuShortcutCheck") || isDeletionOf(s.name, "SteamDefaultDialog") ||
           isDeletionOf(s.name, "ShowScreenshotManager") || isDeletionOf(s.name, "FriendsUIJSON");
}
bool isCleanKey(const string &name)
{
    if (name.empty())
        return false;
    for (char c : name)
    {
        unsigned char u = (unsigned char)c;
        if (isalnum(u) || c == ' ' || c == '_' || c == '.' || c == '-')
            continue;
        return false;
    }
    return true;
}
bool leafHasValue(const Seg &s)
{
    if (s.isBlock)
        return true;
    size_t q1 = s.header.find('"');
    if (q1 == string::npos)
        return false;
    size_t q2 = s.header.find('"', q1 + 1);
    if (q2 == string::npos)
        return false;
    return s.header.find('"', q2 + 1) != string::npos;
}
bool isStoreRootLevel(const Seg &s)
{
    if (s.isBlock)
    {
        if (isMangledOf(s.name, "JSClientStorage") || isMangledOf(s.name, "WebStorage"))
            return true;
        return ieq(s.name, "timeline_intro") || isDeletionOf(s.name, "timeline_intro");
    }
    if (isMangledOf(s.name, "PlaySoundOnToast") || isMangledOf(s.name, "DisableAllToasts") ||
        isMangledOf(s.name, "DisableToastsInGame") || isMangledOf(s.name, "DeskopShortcutCheck"))
        return true;
    return ieq(s.name, "PlaySoundOnToast") || ieq(s.name, "DisableAllToasts") ||
           ieq(s.name, "DisableToastsInGame") || ieq(s.name, "DeskopShortcutCheck");
}
bool isStoreBlock(const Seg &s)
{
    if (!s.isBlock)
        return false;
    if (ieq(s.name, "UserRoamingConfigStore") || ieq(s.name, "UserLocalConfigStore"))
        return true;
    if (isDeletionOf(s.name, "UserRoamingConfigStore", 4) ||
        isDeletionOf(s.name, "UserLocalConfigStore", 4))
        return true;
    size_t n = s.name.size();
    const char *needle = "configstore";
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; ++i)
    {
        bool match = true;
        for (size_t j = 0; j < m; ++j)
            if (tolower((unsigned char)s.name[i + j]) != needle[j])
            {
                match = false;
                break;
            }
        if (match)
            return true;
    }
    return false;
}
bool isStoreContainer(const Seg &s)
{
    if (!s.isBlock)
        return false;
    if (isStoreBlock(s) || isStoreRootLevel(s))
        return true;
    if (isMangledOf(s.name, "JSClientStorage") || isMangledOf(s.name, "WebStorage") ||
        isDeletionOf(s.name, "JSClientStorage") || isDeletionOf(s.name, "WebStorage"))
        return true;
    return false;
}
bool isCloudSwitchLeaf(const Seg &s)
{
    return !s.isBlock && !s.name.empty() && isMangledOf(s.name, "CloudEnabled");
}
bool isAppsName(const string &name)
{
    return !name.empty() &&
           (ieq(name, "apps") || isMangledOf(name, "apps", 2) ||
            isDeletionOf(name, "apps"));
}
bool isAppsBlock(const Seg &s)
{
    return s.isBlock && isAppsName(s.name);
}
bool isAppIdName(const string &name)
{
    if (name.empty())
        return false;
    for (char c : name)
        if (c < '0' || c > '9')
            return false;
    return true;
}
void addAppId(vector<string> &ids, const string &quoted)
{
    for (const auto &x : ids)
        if (x == quoted)
            return;
    ids.push_back(quoted);
}
bool isIgnorable(char c)
{
    unsigned char u = (unsigned char)c;
    return u <= 0x20 || u == 0x7f;
}
bool isBraceOnly(const string &text)
{
    bool any = false;
    for (char c : text)
    {
        if (isIgnorable(c))
            continue;
        if (c == '{' || c == '}')
        {
            any = true;
            continue;
        }
        return false;
    }
    return any;
}
bool isStrayBrace(const Seg &s)
{
    if (s.isBlock || !s.name.empty())
        return false;
    for (char c : s.header)
        if (!isIgnorable(c) && c != '{' && c != '}')
            return false;
    return true;
}
string stripBraceLines(const string &text)
{
    string out;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find_first_of("\r\n", start);
        string line = (end == string::npos) ? text.substr(start) : text.substr(start, end - start);
        size_t nextStart = (end == string::npos) ? text.size() : end + 1;
        if (end != string::npos && text[end] == '\r' && nextStart < text.size() && text[nextStart] == '\n')
            nextStart++;
        if (!isBraceOnly(line))
        {
            out += line;
            if (end != string::npos)
                out += '\n';
        }
        if (end == string::npos)
            break;
        start = nextStart;
    }
    return out;
}
string reindent(const string &text, const string &pad, const string &eol)
{
    string out;
    size_t start = 0;
    size_t base = 0;
    bool haveBase = false;
    while (start < text.size())
    {
        size_t end = text.find_first_of("\r\n", start);
        string line = (end == string::npos) ? text.substr(start) : text.substr(start, end - start);
        size_t nextStart = (end == string::npos) ? text.size() : end + 1;
        if (end != string::npos && text[end] == '\r' && nextStart < text.size() && text[nextStart] == '\n')
            nextStart++;
        size_t b = line.find_first_not_of(" \t");
        if (b == string::npos)
        {
            out += line;
            if (end != string::npos)
                out += eol;
        }
        else
        {
            size_t cur = b;
            if (!haveBase)
            {
                base = cur;
                haveBase = true;
            }
            size_t extra = (cur > base) ? cur - base : 0;
            if (extra > MAX_INDENT)
                extra = MAX_INDENT;
            out += pad + string(extra, '\t') + line.substr(b) + eol;
        }
        if (end == string::npos)
            break;
        start = nextStart;
    }
    return out;
}
bool isSafeJunk(const string &text)
{
    int depth = 0;
    bool inString = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (inString)
        {
            if (c == '\\')
            {
                if (i + 1 < text.size())
                    i++;
                continue;
            }
            if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            while (i + 1 < text.size() && text[i] != '\n' && text[i] != '\r')
                i++;
        }
        else if (c == '{')
            depth++;
        else if (c == '}')
        {
            if (--depth < 0)
                return false;
        }
    }
    return !inString && depth == 0;
}
bool isFragmentLeaf(const Seg &s)
{
    if (s.isBlock)
        return !isCleanKey(s.name);
    if (!s.name.empty())
        return !isCleanKey(s.name) || !leafHasValue(s);
    string h = stripBraceLines(s.header);
    if (trimS(h).empty())
        return true;
    string trimmed = trimS(h);
    if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/')
        return false;
    if (trimmed[0] == '"' && trimmed.size() >= 2 && trimmed[1] == '"')
        return true;
    string core;
    for (char c : trimmed)
        if (c != '"' && !isspace((unsigned char)c))
            core += c;
    return trimmed.find('"') == string::npos || core.empty();
}
string foldJunk(const Seg &top, const string &pad, const string &eol, bool strip)
{
    string rendered = renderSeg(top);
    if (strip)
        rendered = stripBraceLines(rendered);
    if (!isSafeJunk(rendered))
        return "";
    return reindent(rendered, pad, eol);
}
string alignHeader(const string &header, const string &pad)
{
    string out;
    size_t start = 0;
    while (start < header.size())
    {
        size_t end = header.find_first_of("\r\n", start);
        string line = (end == string::npos) ? header.substr(start)
                                            : header.substr(start, end - start);
        string nl;
        size_t next = (end == string::npos) ? header.size() : end + 1;
        if (end != string::npos && header[end] == '\r' && next < header.size() && header[next] == '\n')
        {
            nl = "\r\n";
            next++;
        }
        else if (end != string::npos)
            nl = (header[end] == '\n') ? "\n" : "\r";
        size_t b = line.find_first_not_of(" \t");
        if (b == string::npos)
            out += line + nl;
        else
            out += pad + line.substr(b) + nl;
        if (end == string::npos)
            break;
        start = next;
    }
    return out;
}
bool blockHasRecoverable(const Seg &node)
{
    if (node.isBlock && (isStoreBlock(node) || ieq(node.name, "Steam") ||
                         isAppsBlock(node) || isStoreRootLevel(node)))
        return true;
    if (node.isBlock && isAppIdName(node.name))
        for (const auto &k : node.kids)
            if (ieq(k.name, "CloudEnabled"))
                return true;
    for (const auto &k : node.kids)
        if (blockHasRecoverable(k))
            return true;
    return false;
}
void flattenReal(Seg &n, vector<Seg> &out)
{
    if (n.isBlock && isFragmentLeaf(n) && blockHasRecoverable(n))
    {
        for (auto &sub : n.kids)
            flattenReal(sub, out);
        return;
    }
    if (n.isBlock)
    {
        vector<Seg> kept;
        for (auto &k : n.kids)
            flattenReal(k, kept);
        n.kids = std::move(kept);
    }
    out.push_back(std::move(n));
}
void normalizeTree(Seg &node, const string &pad, const string &eol)
{
    if (node.generated || node.drop)
        return;
    if (node.isBlock)
    {
        node.header = reindent(node.header, pad, eol);
        node.header = alignHeader(node.header, pad);
        if (!node.close.empty())
            node.close = reindent(node.close, pad, eol);
        string childPad = pad;
        if (childPad.size() < MAX_INDENT)
            childPad += "\t";
        vector<Seg> kept;
        for (auto &k : node.kids)
        {
            if (isFragmentLeaf(k) && (!k.isBlock || !blockHasRecoverable(k)))
                continue;
            if (!k.isBlock && k.name.empty())
            {
                k.header = stripBraceLines(k.header);
                if (trimS(k.header).empty())
                    continue;
                if (!isSafeJunk(k.header))
                    continue;
            }
            normalizeTree(k, childPad, eol);
            kept.push_back(std::move(k));
        }
        node.kids = std::move(kept);
    }
    else
    {
        node.header = reindent(node.header, pad, eol);
    }
}
void normalizeChildIndents(Seg &block, const string &pad, const string &eol)
{
    vector<Seg> newKids;
    for (auto &k : block.kids)
    {
        if (k.generated || k.drop)
        {
            newKids.push_back(std::move(k));
            continue;
        }
        Seg raw;
        raw.header = reindent(renderSeg(k), pad, eol);
        newKids.push_back(std::move(raw));
    }
    block.kids = std::move(newKids);
}
bool hasKid(const Seg &block, const string &name)
{
    for (const auto &k : block.kids)
        if (ieq(k.name, name))
            return true;
    return false;
}
bool containsSeg(const Seg &node, const Seg *target)
{
    if (&node == target)
        return true;
    for (const auto &k : node.kids)
        if (containsSeg(k, target))
            return true;
    return false;
}
string firstQuotedKey(const string &line)
{
    size_t b = line.find('"');
    if (b == string::npos)
        return "";
    size_t e = line.find('"', b + 1);
    if (e == string::npos)
        return "";
    return line.substr(b + 1, e - b - 1);
}
bool rawContainsSteamBlock(const string &header)
{
    size_t start = 0;
    while (start < header.size())
    {
        size_t nl = header.find('\n', start);
        string line = header.substr(start, (nl == string::npos) ? string::npos : nl - start);
        size_t b = line.find('"');
        if (b != string::npos)
        {
            size_t q2 = line.find('"', b + 1);
            if (q2 != string::npos)
            {
                string key = line.substr(b + 1, q2 - b - 1);
                if (ieq(key, "Steam"))
                {
                    string after = line.substr(q2 + 1);
                    bool onlyWs = true;
                    for (char c : after)
                        if (c != ' ' && c != '\t')
                        {
                            onlyWs = false;
                            break;
                        }
                    if (!onlyWs && after.find('{') != string::npos)
                        return true;
                    if (onlyWs)
                    {
                        size_t nx = (nl == string::npos) ? header.size() : nl + 1;
                        while (nx < header.size())
                        {
                            size_t nl2 = header.find('\n', nx);
                            string nline = header.substr(nx, (nl2 == string::npos) ? string::npos : nl2 - nx);
                            string t = trimS(nline);
                            if (!t.empty())
                                return t == "{";
                            nx = (nl2 == string::npos) ? header.size() : nl2 + 1;
                        }
                    }
                    return false;
                }
            }
        }
        if (nl == string::npos)
            break;
        start = nl + 1;
    }
    return false;
}
void dedupeSteamExtras(string &extras, const Seg &canonical)
{
    if (extras.empty())
        return;
    vector<string> keys;
    for (const auto &k : canonical.kids)
    {
        string key = firstQuotedKey(k.header);
        if (key.empty())
            key = k.name;
        if (!key.empty())
            keys.push_back(key);
    }
    if (keys.empty())
        return;
    string out;
    size_t start = 0;
    while (start < extras.size())
    {
        size_t nl = extras.find('\n', start);
        size_t end = (nl == string::npos) ? extras.size() : nl + 1;
        string key = firstQuotedKey(extras.substr(start, end - start));
        bool drop = false;
        if (!key.empty())
        {
            for (const auto &t : keys)
                if (ieq(t, key))
                {
                    drop = true;
                    break;
                }
            if (!drop)
                keys.push_back(key);
        }
        if (!drop)
            out += extras.substr(start, end - start);
        start = end;
    }
    extras = out;
}
void stripUnbalancedBraceLines(string &text)
{
    if (text.empty())
        return;
    vector<string> lines;
    {
        size_t start = 0;
        while (start < text.size())
        {
            size_t nl = text.find('\n', start);
            size_t end = (nl == string::npos) ? text.size() : nl + 1;
            lines.push_back(text.substr(start, end - start));
            start = end;
        }
    }
    const size_t n = lines.size();
    vector<int> delta(n, 0);
    vector<bool> bareOnly(n, false);
    vector<bool> kept(n, true);
    for (size_t li = 0; li < n; ++li)
    {
        const string &ln = lines[li];
        int d = 0;
        bool onlyBraces = true;
        bool inQ = false;
        for (size_t i = 0; i < ln.size(); ++i)
        {
            char c = ln[i];
            if (inQ)
            {
                if (c == '\\' && i + 1 < ln.size())
                {
                    ++i;
                    continue;
                }
                if (c == '"')
                    inQ = false;
                continue;
            }
            if (c == '"')
            {
                inQ = true;
                onlyBraces = false;
                continue;
            }
            if (c == '/' && i + 1 < ln.size() && ln[i + 1] == '/')
                break;
            if (c == '{')
            {
                ++d;
                continue;
            }
            if (c == '}')
            {
                --d;
                continue;
            }
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                onlyBraces = false;
        }
        delta[li] = d;
        bareOnly[li] = onlyBraces;
    }
    int depth = 0;
    for (size_t li = 0; li < n; ++li)
    {
        if (!kept[li])
            continue;
        if (bareOnly[li] && depth + delta[li] < 0)
        {
            kept[li] = false;
            continue;
        }
        depth += delta[li];
    }
    for (size_t li = n; li-- > 0 && depth > 0;)
    {
        if (kept[li] && bareOnly[li] && delta[li] > 0)
        {
            kept[li] = false;
            depth -= delta[li];
        }
    }
    string out;
    for (size_t li = 0; li < n; ++li)
        if (kept[li])
            out += lines[li];
    text = out;
}
void hoistStoreRootKeysRec(Seg &node, int depth, const string &eol, vector<Seg> &hoisted);
bool chainDescendant(const Seg &node);
void drainChainShell(Seg &node, const string &pad, const string &steamPad,
                     const string &eol, string &rootExtra, string &steamExtra);
void drainStoreWrapper(Seg &node, const string &rootPad, const string &steamPad,
                       const string &eol, string &rootExtra, string &steamExtra);
bool isMangledChainContainer(const Seg &s);
bool containsStructuralChain(const Seg &node);
bool containsRealSteam(const Seg &node);
void drainNestedApps(Seg &node, vector<string> &ids);
void hoistStoreRootKeysText(Seg &node, const string &pad, const string &eol,
                            string &rootExtra)
{
    for (auto &k : node.kids)
    {
        bool rootContainer =
            k.isBlock && (ieq(k.name, "JSClientStorage") || ieq(k.name, "WebStorage"));
        bool healthyRootContainer = rootContainer && !chainDescendant(k);
        if (isStoreRootLevel(k))
        {
            if (chainDescendant(k) || containsRealSteam(k) || isStoreBlock(k))
            {
                hoistStoreRootKeysText(k, pad, eol, rootExtra);
                continue;
            }
            rootExtra += foldJunk(k, pad, eol, false);
            k.drop = true;
            continue;
        }
        if (k.isBlock && !healthyRootContainer)
            hoistStoreRootKeysText(k, pad, eol, rootExtra);
    }
}
void recoverScattered(Seg &node, const Seg *canonical, bool insideCanonical,
                      const string &pad, vector<string> &ids,
                      string &steamExtra, string &rootExtraOut,
                      const string &eol)
{
    if (&node == canonical)
    {
        for (auto &k : node.kids)
        {
            if (isAppsBlock(k))
            {
                for (auto &g : k.kids)
                    if (g.isBlock && isAppIdName(g.name))
                        addAppId(ids, "\"" + g.name + "\"");
                continue;
            }
            recoverScattered(k, canonical, true, pad, ids, steamExtra,
                             rootExtraOut, eol);
        }
        return;
    }
    if (node.isBlock && isAppIdName(node.name))
    {
        addAppId(ids, "\"" + node.name + "\"");
        if (!containsSeg(node, canonical))
            node.drop = true;
        return;
    }
    if (node.isBlock && !insideCanonical && isSteamLevel(node) &&
        !containsSeg(node, canonical))
    {
#ifndef SKIP_HOIST
        hoistStoreRootKeysText(node, "\t", eol, rootExtraOut);
#endif
#ifndef SKIP_DRAIN
        drainNestedApps(node, ids);
#endif
        steamExtra += foldJunk(node, pad, eol, false);
        node.drop = true;
        return;
    }
    if (node.isBlock)
    {
        for (auto &k : node.kids)
        {
            recoverScattered(k, canonical, insideCanonical, pad, ids, steamExtra,
                             rootExtraOut, eol);
            if (isMangledChainContainer(k) && !containsSeg(k, canonical))
                k.drop = true;
        }
        return;
    }
    if (!insideCanonical && !node.name.empty() && !node.isBlock &&
        isMangledOf(node.name, "CloudEnabled"))
    {
        node.drop = true;
        return;
    }
    if (!insideCanonical && !node.name.empty() && !node.isBlock &&
        isAppsName(node.name))
    {
        node.drop = true;
        return;
    }
    if (!insideCanonical && !node.name.empty() && isSteamLevel(node))
    {
        if (ieq(node.name, "CloudEnabled"))
        {
            node.drop = true;
        }
        else
        {
            steamExtra += foldJunk(node, pad, eol, false);
            node.drop = true;
        }
    }
}
void drainNestedApps(Seg &node, vector<string> &ids)
{
    if (!node.isBlock)
        return;
    if (isAppsBlock(node))
    {
        for (auto &g : node.kids)
            if (g.isBlock && isAppIdName(g.name))
            {
                addAppId(ids, "\"" + g.name + "\"");
                g.drop = true;
            }
        node.drop = true;
        return;
    }
    if (isAppIdName(node.name))
    {
        addAppId(ids, "\"" + node.name + "\"");
        node.drop = true;
        return;
    }
    for (auto &k : node.kids)
        drainNestedApps(k, ids);
}
bool chainDescendant(const Seg &node)
{
    if (node.isBlock &&
        (ieq(node.name, "Software") || ieq(node.name, "valve") ||
         isDeletionOf(node.name, "Software") || isDeletionOf(node.name, "valve")))
        return true;
    if (node.isBlock && node.name.find("Steam") != string::npos)
        return true;
    for (const auto &k : node.kids)
        if (chainDescendant(k))
            return true;
    return false;
}
void hoistStoreRootKeysRec(Seg &node, int depth, const string &eol, vector<Seg> &hoisted)
{
    vector<Seg> kept;
    for (auto &k : node.kids)
    {
        bool rootContainer =
            k.isBlock && (ieq(k.name, "JSClientStorage") || ieq(k.name, "WebStorage"));
        bool healthyRootContainer = rootContainer && !chainDescendant(k);
        if (depth >= 1 && isStoreRootLevel(k))
        {
            normalizeTree(k, "\t", eol);
            hoisted.push_back(std::move(k));
            continue;
        }
        if (k.isBlock && !healthyRootContainer)
            hoistStoreRootKeysRec(k, depth + 1, eol, hoisted);
        kept.push_back(std::move(k));
    }
    node.kids = std::move(kept);
}
void hoistStoreRootKeys(Seg &storeRoot, const string &eol)
{
    vector<Seg> hoisted;
    hoistStoreRootKeysRec(storeRoot, 0, eol, hoisted);
    for (auto &h : hoisted)
        storeRoot.kids.push_back(std::move(h));
}
void findSteamsUnder(Seg &node, const string &target, bool inside, vector<Seg *> &out)
{
    if (inside && node.isBlock && ieq(node.name, "Steam"))
        out.push_back(&node);
    bool childInside = inside || (node.isBlock && ieq(node.name, target));
    for (auto &k : node.kids)
        findSteamsUnder(k, target, childInside, out);
}
bool steamInsideSteam(const Seg *candidate, const vector<Seg *> &allSteams)
{
    for (const Seg *s : allSteams)
        if (s != candidate && containsSeg(*s, candidate))
            return true;
    return false;
}
Seg *pickCanonicalSteam(vector<Seg> &tops, const vector<Seg *> &allSteams)
{
    auto findFirst = [&](const string &target) -> Seg * {
        for (auto &top : tops)
        {
            vector<Seg *> found;
            findSteamsUnder(top, target, false, found);
            for (Seg *f : found)
                if (!steamInsideSteam(f, allSteams))
                    return f;
        }
        return nullptr;
    };
    Seg *f = findFirst("valve");
    if (f)
        return f;
    f = findFirst("Software");
    if (f)
        return f;
    f = findFirst("UserRoamingConfigStore");
    if (f)
        return f;
    for (Seg *s : allSteams)
        if (!steamInsideSteam(s, allSteams))
            return s;
    return allSteams[0];
}
void markStrayApps(Seg &node, const Seg *canonical, bool insideCanonical)
{
    if (isAppsBlock(node) && !insideCanonical
        && !containsSeg(node, canonical))
        node.drop = true;
    bool childInside = insideCanonical || (&node == canonical);
    for (auto &k : node.kids)
        markStrayApps(k, canonical, childInside);
}
bool rawHasRootKeys(const string &header)
{
    for (const auto &line : splitKeepingNewlines(header))
    {
        string s = trimS(line);
        if (s.size() >= 2 && s[0] == '"')
        {
            size_t close = s.find('"', 1);
            if (close != string::npos)
            {
                string key = s.substr(1, close - 1);
                if (ieq(key, "JSClientStorage") || ieq(key, "WebStorage") ||
                    ieq(key, "PlaySoundOnToast") || ieq(key, "DisableAllToasts") ||
                    ieq(key, "DisableToastsInGame") || ieq(key, "DeskopShortcutCheck"))
                    return true;
            }
        }
    }
    return false;
}
bool isEmptyShell(const string &header)
{
    string stripped = stripBraceLines(header);
    string trimmed = trimS(stripped);
    if (trimmed.empty())
        return true;
    for (char c : trimmed)
        if (c != '"' && !isspace((unsigned char)c))
            return false;
    return true;
}
bool markEmptyContainers(Seg &node)
{
    if (!node.isBlock)
    {
        if (!node.generated && (node.drop || isEmptyShell(node.header)))
            node.drop = true;
        return node.drop;
    }
    bool allKidsGone = true;
    for (auto &k : node.kids)
        if (!markEmptyContainers(k))
            allKidsGone = false;
    if (allKidsGone && !node.generated)
        node.drop = true;
    return node.drop;
}
string buildAppsText(const string &ci, const vector<string> &ids, const string &eol)
{
    string acc = ci + "\"apps\"" + eol;
    acc += ci + "{" + eol;
    for (const auto &id : ids)
    {
        acc += ci + "\t" + id + eol;
        acc += ci + "\t{" + eol;
        acc += ci + "\t\t\"CloudEnabled\"\t\t\"0\"" + eol;
        acc += ci + "\t}" + eol;
    }
    acc += ci + "}" + eol;
    return acc;
}
string buildSteamBlockText(const string &pid, const vector<string> &ids, const string &eol)
{
    string ci = pid + "\t";
    string s = pid + "\"Steam\"" + eol;
    s += pid + "{" + eol;
    s += ci + "\"CloudEnabled\"\t\t\"0\"" + eol;
    s += buildAppsText(ci, ids, eol);
    s += pid + "}" + eol;
    return s;
}
void transformCanonical(Seg &steam, const vector<string> &ids, const string &eol)
{
    string steamChildIndent = indentOf(steam.header) + "\t";
    vector<Seg> newKids;
    bool sawOuterCloud = false;
    bool appsDone = false;
    for (auto &k : steam.kids)
    {
        if (!k.isBlock && !k.name.empty() && !ieq(k.name, "CloudEnabled") &&
            isMangledOf(k.name, "CloudEnabled", 6))
            continue;
        if (!k.isBlock && !k.name.empty() && isAppsName(k.name))
            continue;
        if (isAppsBlock(k))
        {
            if (!appsDone)
            {
                appsDone = true;
                Seg apps;
                apps.name = "apps";
                apps.isBlock = true;
                apps.generated = true;
                apps.genText = buildAppsText(steamChildIndent, ids, eol);
                newKids.push_back(std::move(apps));
            }
            continue;
        }
        if (ieq(k.name, "CloudEnabled") && !k.isBlock)
        {
            if (!sawOuterCloud)
            {
                sawOuterCloud = true;
                Seg cloud;
                cloud.name = "CloudEnabled";
                cloud.generated = true;
                cloud.genText = steamChildIndent + "\"CloudEnabled\"\t\t\"0\"" + eol;
                newKids.push_back(std::move(cloud));
            }
            continue;
        }
        {
            string rendered = renderSeg(k);
            if (!k.isBlock && k.name.empty())
            {
                rendered = alignHeader(stripBraceLines(rendered), steamChildIndent);
            }
            if (isSafeJunk(rendered))
            {
                if (!k.isBlock && k.name.empty())
                    k.header = rendered;
                newKids.push_back(std::move(k));
            }
        }
    }
    if (!sawOuterCloud)
    {
        Seg cloud;
        cloud.name = "CloudEnabled";
        cloud.generated = true;
        cloud.genText = steamChildIndent + "\"CloudEnabled\"\t\t\"0\"" + eol;
        newKids.insert(newKids.begin(), std::move(cloud));
    }
    if (!appsDone)
    {
        Seg apps;
        apps.name = "apps";
        apps.generated = true;
        apps.genText = buildAppsText(steamChildIndent, ids, eol);
        newKids.push_back(std::move(apps));
    }
    for (size_t i = 0; i < newKids.size(); ++i)
    {
        Seg &b = newKids[i];
        if (!b.isBlock || !(ieq(b.name, "friendsui") || ieq(b.name, "FriendsUI")) ||
            !b.kids.empty())
            continue;
        for (size_t j = 0; j < newKids.size(); ++j)
        {
            if (i == j)
                continue;
            Seg &leaf = newKids[j];
            if (leaf.isBlock)
                continue;
            if (ieq(leaf.name, "FriendsUIJSON") ||
                isDeletionOf(leaf.name, "FriendsUIJSON"))
            {
                b.kids.push_back(std::move(leaf));
                newKids.erase(newKids.begin() + (long)j);
                if (j < i)
                    --i;
                break;
            }
        }
    }
    steam.kids = std::move(newKids);
    normalizeChildIndents(steam, steamChildIndent, eol);
}
string addSteamToRoam(vector<Seg> &tops, const vector<string> &ids, const string &eol)
{
    Seg *roam = nullptr;
    for (auto &top : tops)
        if (top.isBlock && ieq(top.name, "UserRoamingConfigStore"))
        {
            roam = &top;
            break;
        }
    if (!roam)
        return "";
    Seg *software = nullptr;
    for (auto &k : roam->kids)
        if (k.isBlock && ieq(k.name, "Software"))
        {
            software = &k;
            break;
        }
    if (!software)
    {
        Seg s;
        s.name = "Software";
        s.isBlock = true;
        s.header = "\t\"Software\"" + eol + "\t{" + eol;
        s.close = "\t}" + eol;
        roam->kids.insert(roam->kids.begin(), std::move(s));
        software = &roam->kids.front();
    }
    {
        vector<Seg> kept;
        for (auto &k : software->kids)
            if (!ieq(k.name, "Steam"))
                kept.push_back(std::move(k));
        software->kids = std::move(kept);
    }
    Seg *valve = nullptr;
    for (auto &k : software->kids)
        if (k.isBlock && ieq(k.name, "valve"))
        {
            valve = &k;
            break;
        }
    if (!valve)
    {
        Seg v;
        v.name = "valve";
        v.isBlock = true;
        v.header = "\t\t\"valve\"" + eol + "\t\t{" + eol;
        v.close = "\t\t}" + eol;
        software->kids.push_back(std::move(v));
        valve = &software->kids.back();
    }
    string pid = indentOf(valve->header) + "\t";
    vector<Seg> kept;
    for (auto &k : valve->kids)
        if (!ieq(k.name, "Steam"))
            kept.push_back(std::move(k));
    valve->kids = std::move(kept);
    vector<string> allIds = ids;
    string steamExtra;
    string rootExtra;
    for (auto &top : tops)
        recoverScattered(top, nullptr, false, pid + "\t", allIds, steamExtra,
                         rootExtra, eol);
    for (auto &top : tops)
        markStrayApps(top, nullptr, false);
    Seg steam;
    steam.name = "Steam";
    steam.isBlock = true;
    steam.generated = true;
    steam.genText = buildSteamBlockText(pid, allIds, eol);
    valve->kids.push_back(std::move(steam));
    Seg *steamPtr = &valve->kids.back();
    for (auto &k : roam->kids)
        if (isAppsBlock(k))
            k.drop = true;
    for (auto &top : tops)
        if (&top != roam && ieq(top.name, "Software"))
            top.drop = true;
    for (auto &top : tops)
    {
        if (&top == roam || top.drop)
            continue;
        if (isStrayBrace(top))
        {
            top.drop = true;
            continue;
        }
        if (isCloudSwitchLeaf(top))
        {
            top.drop = true;
            continue;
        }
        bool chainShell = top.isBlock &&
                          (ieq(top.name, "Software") || ieq(top.name, "valve") ||
                           isDeletionOf(top.name, "Software") ||
                           isDeletionOf(top.name, "valve") ||
                           isDeletionOf(top.name, "Steam") ||
                           isMangledOf(top.name, "Software") ||
                           isMangledOf(top.name, "valve") ||
                           isMangledOf(top.name, "Steam") ||
                           containsStructuralChain(top));
        if (chainShell)
        {
            drainChainShell(top, "\t", pid + "\t", eol, rootExtra, steamExtra);
            continue;
        }
        if (isStoreContainer(top))
        {
            drainStoreWrapper(top, "\t", pid + "\t", eol, rootExtra, steamExtra);
            continue;
        }
        if (isSteamLevel(top))
            steamExtra += foldJunk(top, pid + "\t", eol, false);
        else if (top.isBlock || !top.name.empty())
            rootExtra += foldJunk(top, "\t", eol, false);
        else
            rootExtra += foldJunk(top, "\t", eol, true);
        top.drop = true;
    }
    for (auto &k : roam->kids)
    {
        if (k.drop)
            continue;
        bool chainKid = k.isBlock &&
                        (ieq(k.name, "Software") || ieq(k.name, "valve") ||
                         isDeletionOf(k.name, "Software") ||
                         isDeletionOf(k.name, "valve") ||
                         isDeletionOf(k.name, "Steam") ||
                         isMangledOf(k.name, "Software") ||
                         isMangledOf(k.name, "valve") ||
                         isMangledOf(k.name, "Steam") ||
                         containsStructuralChain(k));
        if (chainKid && !containsRealSteam(k))
        {
            drainChainShell(k, "\t", pid + "\t", eol, rootExtra, steamExtra);
            continue;
        }
        if (isStoreContainer(k))
        {
            vector<Seg> keptStore;
            for (auto &ck : k.kids)
            {
                if (ck.drop)
                    continue;
                bool sh = ck.isBlock &&
                          (ieq(ck.name, "Software") || ieq(ck.name, "valve") ||
                           isDeletionOf(ck.name, "Software") ||
                           isDeletionOf(ck.name, "valve") ||
                           isDeletionOf(ck.name, "Steam"));
                if (sh)
                    drainChainShell(ck, "\t", pid + "\t", eol, rootExtra, steamExtra);
                else if (isAppsBlock(ck))
                    continue;
                else if (isStoreContainer(ck))
                {
                    drainStoreWrapper(ck, "\t", pid + "\t", eol, rootExtra,
                                      steamExtra);
                }
                else if (isSteamLevel(ck))
                    steamExtra += foldJunk(ck, pid + "\t", eol, false);
                else
                    keptStore.push_back(std::move(ck));
            }
            k.kids = std::move(keptStore);
            continue;
        }
        if (isCloudSwitchLeaf(k))
        {
            k.drop = true;
            continue;
        }
        if (isSteamLevel(k))
        {
            steamExtra += foldJunk(k, pid + "\t", eol, false);
            k.drop = true;
        }
    }
    if (!steamExtra.empty())
    {
        string close = pid + "}" + eol;
        string text = steamPtr->genText;
        size_t pos = text.rfind(close);
        if (pos != string::npos)
            text.insert(pos, steamExtra);
        else
            text += steamExtra;
        steamPtr->genText = text;
    }
    if (!rootExtra.empty())
    {
        Seg raw;
        raw.header = rootExtra;
        roam->kids.push_back(std::move(raw));
    }
    hoistStoreRootKeys(*roam, eol);
    for (auto &top : tops)
        markEmptyContainers(top);
    string outText;
    for (const auto &top : tops)
        if (!top.drop)
            outText += renderSeg(top);
    return outText;
}
void foldNonChainContent(Seg &node, const Seg *canonical, const string &eol,
                         string &out, string &steamExtra, const string &steamPad)
{
    if (&node == canonical)
        return;
    for (auto &k : node.kids)
    {
        if (k.drop)
            continue;
        if (!k.isBlock && k.name.empty() && rawContainsSteamBlock(k.header))
            continue;
        if (containsSeg(k, canonical))
        {
            if (k.isBlock && &k != canonical)
                foldNonChainContent(k, canonical, eol, out, steamExtra, steamPad);
            continue;
        }
        if (k.isBlock && isAppIdName(k.name))
            continue;
        if (isCloudSwitchLeaf(k))
            continue;
        bool sh = k.isBlock &&
                  (ieq(k.name, "Software") || ieq(k.name, "valve") ||
                   isDeletionOf(k.name, "Software") || isDeletionOf(k.name, "valve") ||
                   isDeletionOf(k.name, "Steam") || containsStructuralChain(k));
        if (sh)
        {
            drainChainShell(k, "\t", steamPad, eol, out, steamExtra);
            continue;
        }
        if (isStoreContainer(k))
        {
            drainStoreWrapper(k, "\t", steamPad, eol, out, steamExtra);
            continue;
        }
        if (isSteamLevel(k))
        {
            steamExtra += foldJunk(k, steamPad, eol, false);
            continue;
        }
        if (k.isBlock && containsRealSteam(k))
        {
            drainStoreWrapper(k, "\t", steamPad, eol, out, steamExtra);
            continue;
        }
        out += foldJunk(k, "\t", eol, false);
    }
}
void renameSeg(Seg &s, const string &newName)
{
    size_t q1 = s.header.find('"');
    if (q1 == string::npos)
        return;
    size_t q2 = s.header.find('"', q1 + 1);
    if (q2 == string::npos)
        return;
    s.header = s.header.substr(0, q1 + 1) + newName + s.header.substr(q2);
    s.name = newName;
}
void canonicalizeChainContainers(Seg &node, const Seg *canonical)
{
    if (&node == canonical)
        return;
    if (node.isBlock)
    {
        if (isMangledOf(node.name, "Software") && !ieq(node.name, "Software"))
            renameSeg(node, "Software");
        else if (isMangledOf(node.name, "valve") && !ieq(node.name, "valve"))
            renameSeg(node, "valve");
        else if (isMangledOf(node.name, "Steam") && !ieq(node.name, "Steam"))
            renameSeg(node, "Steam");
    }
    for (auto &k : node.kids)
        if (containsSeg(k, canonical))
            canonicalizeChainContainers(k, canonical);
}
bool storeWrapperOnChain(const Seg &roam, const Seg *canonical)
{
    for (const auto &k : roam.kids)
        if (k.isBlock && isStoreBlock(k) && containsSeg(k, canonical))
            return true;
    return false;
}
bool isMangledChainContainer(const Seg &s)
{
    if (!s.isBlock)
        return false;
    if (ieq(s.name, "Software") || ieq(s.name, "valve") || ieq(s.name, "Steam"))
        return false;
    return isDeletionOf(s.name, "Software") || isDeletionOf(s.name, "valve") ||
           isDeletionOf(s.name, "Steam") || isMangledOf(s.name, "Software") ||
           isMangledOf(s.name, "valve") || isMangledOf(s.name, "Steam");
}
bool containsStructuralChain(const Seg &node)
{
    if (!node.isBlock)
        return false;
    if (ieq(node.name, "Steam") || isAppsBlock(node))
        return true;
    for (const auto &k : node.kids)
        if (containsStructuralChain(k))
            return true;
    return false;
}
bool containsRealSteam(const Seg &node)
{
    if (node.isBlock && ieq(node.name, "Steam"))
        return true;
    for (const auto &k : node.kids)
        if (containsRealSteam(k))
            return true;
    return false;
}
void drainStoreWrapper(Seg &node, const string &rootPad, const string &steamPad,
                       const string &eol, string &rootExtra, string &steamExtra)
{
    for (auto &k : node.kids)
    {
        if (k.drop)
            continue;
        bool sh = k.isBlock &&
                  (ieq(k.name, "Software") || ieq(k.name, "valve") ||
                   isDeletionOf(k.name, "Software") || isDeletionOf(k.name, "valve") ||
                   isDeletionOf(k.name, "Steam") || containsStructuralChain(k));
        if (sh)
            drainChainShell(k, rootPad, steamPad, eol, rootExtra, steamExtra);
        else if (isAppsBlock(k))
            continue;
        else if (isStoreContainer(k))
        {
            drainStoreWrapper(k, rootPad, steamPad, eol, rootExtra, steamExtra);
        }
        else if (isSteamLevel(k))
            steamExtra += foldJunk(k, steamPad, eol, false);
        else
            rootExtra += foldJunk(k, rootPad, eol, false);
    }
    node.drop = true;
}
void drainChainShell(Seg &node, const string &pad, const string &steamPad,
                     const string &eol, string &rootExtra, string &steamExtra)
{
    for (auto &k : node.kids)
    {
        if (k.drop)
            continue;
        bool nestedShell = k.isBlock &&
                           (isDeletionOf(k.name, "Software") ||
                            isDeletionOf(k.name, "valve") ||
                            isDeletionOf(k.name, "Steam") ||
                            containsStructuralChain(k));
        if (nestedShell)
        {
            drainChainShell(k, pad, steamPad, eol, rootExtra, steamExtra);
            continue;
        }
        if (isStoreContainer(k))
        {
            drainStoreWrapper(k, pad, steamPad, eol, rootExtra, steamExtra);
            continue;
        }
        if (isStoreRootLevel(k))
            rootExtra += foldJunk(k, pad, eol, false);
        else if (isSteamLevel(k))
            steamExtra += foldJunk(k, steamPad, eol, false);
    }
    node.drop = true;
}
string buildRoamRoot(vector<Seg> &tops, Seg *canonical, vector<string> ids,
                     const string &eol, const string &rootExtraPre = "")
{
    string ci = "\t\t\t\t";
    string steamExtra;
    string rootExtra;
    string rootExtraEarly;
    if (canonical)
    {
        for (auto &k : canonical->kids)
        {
            if (k.drop)
                continue;
            if (!k.isBlock && k.name.empty() && rawContainsSteamBlock(k.header))
                continue;
            if (isAppsBlock(k))
                continue;
            if (ieq(k.name, "CloudEnabled") && !k.isBlock)
                continue;
            if (isStoreRootLevel(k) ||
                (!k.isBlock && k.name.empty() && rawHasRootKeys(k.header)))
            {
                rootExtraEarly += foldJunk(k, "\t", eol, false);
                continue;
            }
            drainNestedApps(k, ids);
            steamExtra += foldJunk(k, ci, eol, false);
        }
    }
    else
    {
        for (auto &top : tops)
            recoverScattered(top, nullptr, false, ci, ids, steamExtra,
                             rootExtra, eol);
        for (auto &top : tops)
            markStrayApps(top, nullptr, false);
    }
    for (auto &top : tops)
        markEmptyContainers(top);
    for (auto &top : tops)
    {
        if (top.drop)
            continue;
        if (canonical && containsSeg(top, canonical))
        {
            foldNonChainContent(top, canonical, eol, rootExtra, steamExtra, ci);
            continue;
        }
        if (isStoreContainer(top))
        {
            drainStoreWrapper(top, "\t", "\t\t\t\t", eol, rootExtra, steamExtra);
            continue;
        }
        if (isAppsBlock(top))
            continue;
        bool chainShell =
            ieq(top.name, "Software") || ieq(top.name, "valve") ||
            isDeletionOf(top.name, "Software") || isDeletionOf(top.name, "valve") ||
            isDeletionOf(top.name, "Steam");
        if (chainShell)
        {
            for (const auto &k : top.kids)
                if (!k.drop && isStoreRootLevel(k))
                    rootExtra += foldJunk(k, "\t", eol, false);
            continue;
        }
        if (!top.isBlock && ieq(top.name, "CloudEnabled"))
            continue;
        if (isStrayBrace(top))
            continue;
        if (isSteamLevel(top))
            steamExtra += foldJunk(top, ci, eol, false);
        else if (!top.isBlock && top.name.empty() && rawContainsSteamBlock(top.header))
            continue;
        else if (top.isBlock && containsRealSteam(top))
            drainStoreWrapper(top, "\t", ci, eol, rootExtra, steamExtra);
        else if (top.isBlock || !top.name.empty())
            rootExtra += foldJunk(top, "\t", eol, false);
        else
            rootExtra += foldJunk(top, "\t", eol, true);
    }
    stripUnbalancedBraceLines(steamExtra);
    steamExtra = alignHeader(steamExtra, ci);
    rootExtra = rootExtraPre + rootExtraEarly + rootExtra;
    stripUnbalancedBraceLines(rootExtra);
    string full;
    full += "\"UserRoamingConfigStore\"" + eol;
    full += "{" + eol;
    full += "\t\"Software\"" + eol;
    full += "\t{" + eol;
    full += "\t\t\"valve\"" + eol;
    full += "\t\t{" + eol;
    full += "\t\t\t\"Steam\"" + eol;
    full += "\t\t\t{" + eol;
    full += ci + "\"CloudEnabled\"\t\t\"0\"" + eol;
    full += buildAppsText(ci, ids, eol);
    full += steamExtra;
    full += "\t\t\t}" + eol;
    full += "\t\t}" + eol;
    full += "\t}" + eol;
    full += rootExtra;
    full += "}" + eol;
    return full;
}
string repairBracelessFriends(const string &text, const string &eol)
{
    vector<string> lines = splitKeepingNewlines(text);
    auto lineKey = [](const string &t) {
        size_t q1 = t.find('"');
        if (q1 == string::npos)
            return string();
        size_t q2 = t.find('"', q1 + 1);
        if (q2 == string::npos)
            return string();
        return t.substr(q1 + 1, q2 - q1 - 1);
    };
    vector<size_t> shells, leaves;
    for (size_t i = 0; i + 2 < lines.size(); ++i)
    {
        if (ieq(lineKey(trimS(lines[i])), "friendsui") &&
            trimS(lines[i + 1]) == "{" && trimS(lines[i + 2]) == "}")
            shells.push_back(i);
        if (i + 1 < lines.size() && trimS(lines[i + 1]) == "}")
        {
            string t = trimS(lines[i]);
            if (ieq(lineKey(t), "friendsui"))
            {
                size_t k1 = t.find('"');
                size_t k2 = t.find('"', k1 + 1);
                string after = t.substr(k2 + 1);
                bool onlyWsBrace = true;
                for (char c : after)
                    if (c != ' ' && c != '\t' && c != '{')
                    {
                        onlyWsBrace = false;
                        break;
                    }
                if (onlyWsBrace && after.find('{') != string::npos)
                    shells.push_back(i);
            }
        }
        if (ieq(lineKey(trimS(lines[i])), "FriendsUIJSON"))
            leaves.push_back(i);
    }
    if (shells.empty() || leaves.empty())
        return text;
    vector<bool> usedLeaf(leaves.size(), false);
    vector<string> out;
    bool repaired = false;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        bool isLeaf = false;
        for (size_t li = 0; li < leaves.size(); ++li)
            if (leaves[li] == i) { isLeaf = true; break; }
        if (isLeaf)
        {
            bool droppedBefore = false;
            for (size_t li = 0; li < leaves.size(); ++li)
                if (leaves[li] == i && usedLeaf[li]) { droppedBefore = true; break; }
            if (droppedBefore)
                continue;
        }
        bool atShell = false;
        size_t shellIdx = 0;
        for (size_t si = 0; si < shells.size(); ++si)
            if (shells[si] == i) { atShell = true; shellIdx = si; break; }
        if (atShell)
        {
            string pad = indentOf(lines[i]);
            bool glued = trimS(lines[i]).find('{') != string::npos;
            size_t mate = lines.size();
            size_t mateLi = 0;
            for (size_t li = 0; li < leaves.size(); ++li)
            {
                if (usedLeaf[li]) continue;
                if (indentOf(lines[leaves[li]]) != pad) continue;
                mate = leaves[li]; mateLi = li; break;
            }
            if (mate != lines.size())
            {
                repaired = true;
                usedLeaf[mateLi] = true;
                string body = lines[mate];
                string child = pad + "\t" +
                               (body.size() > pad.size() ? body.substr(pad.size())
                                                          : body);
                out.push_back(pad + "\"friendsui\"" + eol);
                out.push_back(pad + "{" + eol);
                out.push_back(child + eol);
                out.push_back(pad + "}" + eol);
                i += glued ? 1 : 2;
                continue;
            }
        }
        out.push_back(lines[i]);
    }
    if (!repaired)
        return text;
    string s;
    for (auto &line : out)
        s += line;
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r'))
        --e;
    return s.substr(0, e) + eol;
}
string stripAbsorbedStoreWrapper(const string &text, const string &eol)
{
    if (text.find("\"JSClientStorage\"") == string::npos &&
        text.find("\"WebStorage\"") == string::npos)
        return text;
    vector<string> lines = splitKeepingNewlines(text);
    size_t i = 0;
    bool changed = false;
    vector<string> out;
    while (i < lines.size())
    {
        string t = trimS(lines[i]);
        bool wrap = false;
        if (t == "\"JSClientStorage\"" || t == "\"WebStorage\"" ||
            t.rfind("\"JSClientStorage\"", 0) == 0 ||
            t.rfind("\"WebStorage\"", 0) == 0 ||
            t.rfind("\"jsclientstorage\"", 0) == 0 ||
            t.rfind("\"webstorage\"", 0) == 0)
            wrap = true;
        if (!wrap || i + 1 >= lines.size() || trimS(lines[i + 1]) != "{")
        {
            out.push_back(lines[i]);
            ++i;
            continue;
        }
        string pad = indentOf(lines[i]);
        size_t j = i + 2;
        int depth = 1;
        vector<string> kids;
        bool hasReal = false, onlyJunk = true, malformed = false;
        while (j < lines.size() && depth > 0)
        {
            string ct = trimS(lines[j]);
            int d = 0;
            if (ct == "{") ++d;
            if (ct == "}") --d;
            if (ct == "}" && indentOf(lines[j]) == pad && depth == 1)
            {
                depth += d;
                break;
            }
            if (depth == 1 && ct != "{" && ct != "}")
            {
                string low = ct;
                for (auto &c : low) c = (char)tolower((unsigned char)c);
                bool isApps = ct == "\"apps\"";
                size_t firstQ = ct.find('"');
                size_t thirdQ = (firstQ == string::npos)
                                    ? string::npos
                                    : ct.find('"', firstQ + (size_t)1);
                bool leaf = (thirdQ != string::npos) &&
                            (ct.find('"', thirdQ + (size_t)1) != string::npos);
                bool isSteamLeaf =
                    leaf &&
                    (ct.find("\"SurveyDate\"") == 0 ||
                     ct.find("\"SurveyDateVersion\"") == 0 ||
                     ct.find("\"StartMenuShortcutCheck\"") == 0 ||
                     ct.find("\"DesktopShortcutCheck\"") == 0 ||
                     ct.find("\"DeskopShortcutCheck\"") == 0 ||
                     ct.find("\"CloudEnabled\"") == 0 ||
                     ct.find("\"SteamDefaultDialog\"") == 0 ||
                     ct.find("\"ShowScreenshotManager\"") == 0 ||
                     ct.find("\"friendsui\"") == 0 || ct.find("\"FriendsUIJSON\"") == 0 ||
                     ct.find("\"Steam\"") == 0 || ct.find("\"Software\"") == 0 ||
                     ct.find("\"valve\"") == 0);
                bool isSpotlight = low.find("spotlight") != string::npos ||
                                   low.find("timeline") != string::npos ||
                                   low.find("toast") != string::npos ||
                                   isApps;
                if (isApps || isSteamLeaf)
                    kids.push_back(lines[j]);
                else
                    hasReal = true;
            }
            depth += d;
            ++j;
        }
        if ((!malformed) && !hasReal && !kids.empty() && depth == 0)
        {
            changed = true;
            i = j + 1;
            continue;
        }
        out.push_back(lines[i]);
        ++i;
    }
    if (!changed)
        return text;
    string s;
    for (auto &line : out)
        s += line;
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r'))
        --e;
    return s.substr(0, e) + eol;
}
string dropBraceFragmentLeaves(const string &text, const string &eol)
{
    vector<string> lines = splitKeepingNewlines(text);
    vector<string> out;
    bool changed = false;
    for (auto &line : lines)
    {
        size_t q1 = line.find('"');
        if (q1 != string::npos)
        {
            size_t q2 = line.find('"', q1 + (size_t)1);
            if (q2 != string::npos)
            {
                size_t q3 = line.find('"', q2 + (size_t)1);
                bool hasValue = q3 != string::npos;
                if (hasValue)
                {
                    string key = line.substr(q1 + (size_t)1, q2 - q1 - (size_t)1);
                    bool bad = key.empty();
                    for (char kc : key)
                    {
                        unsigned char u = (unsigned char)kc;
                        if (!(isalnum(u) || kc == ' ' || kc == '_' || kc == '.' ||
                              kc == '-' || kc == '\\'))
                        {
                            bad = true;
                            break;
                        }
                    }
                    if (bad || key.find("\\t") != string::npos ||
                        key.find("\\n") != string::npos)
                    {
                        changed = true;
                        continue;
                    }
                }
            }
        }
        out.push_back(line);
    }
    if (!changed)
        return text;
    string s;
    for (auto &line : out)
        s += line;
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r'))
        --e;
    return s.substr(0, e) + eol;
}
string relocateStoreRootSteamLeaves(const string &text, const string &eol)
{
    if (text.find("\"Steam\"") == string::npos)
        return text;
    vector<string> lines = splitKeepingNewlines(text);
    size_t steamHead = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
        if (trimS(lines[i]) == "\"Steam\"")
        {
            steamHead = i;
            break;
        }
    if (steamHead == lines.size())
        return text;
    string steamPad = indentOf(lines[steamHead]);
    size_t steamClose = lines.size();
    int depth = 0;
    bool opened = false;
    for (size_t i = steamHead + 1; i < lines.size(); ++i)
    {
        int d = 0;
        for (char c : lines[i])
        {
            if (c == '{')
                ++d;
            else if (c == '}')
                --d;
        }
        depth += d;
        if (depth > 0)
            opened = true;
        if (opened && depth <= 0 && d < 0 &&
            indentOf(lines[i]) == steamPad && trimS(lines[i]) == "}")
        {
            steamClose = i;
            break;
        }
        if (depth < 0)
            break;
    }
    if (steamClose == lines.size())
        return text;
    const char *steamLeafKeys[] = {
        "SurveyDate", "SurveyDateVersion", "StartMenuShortcutCheck",
        "DesktopShortcutCheck", "SteamDefaultDialog", "ShowScreenshotManager",
        "FriendsUIJSON", "CloudEnabled"};
    auto isSteamLeafKey = [&](const string &key) {
        if (key.empty())
            return false;
        for (const char *k : steamLeafKeys)
            if (ieq(key, k))
                return true;
        return false;
    };
    string childPad = steamPad + "\t";
    vector<string> relocated;
    vector<size_t> removeIdx;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i >= steamHead && i <= steamClose)
            continue;
        string t = trimS(lines[i]);
        if (t.empty() || t[0] != '"')
            continue;
        size_t q1 = t.find('"');
        size_t q2 = t.find('"', q1 + 1);
        size_t q3 = (q2 == string::npos) ? string::npos : t.find('"', q2 + 1);
        string key = (q1 != string::npos && q2 != string::npos && q2 > q1)
                         ? t.substr(q1 + 1, q2 - q1 - 1) : "";
        if (q3 != string::npos && isSteamLeafKey(key))
        {
            int lead = (int)indentOf(lines[i]).size();
            string body = lines[i].substr((size_t)lead);
            relocated.push_back(childPad + body);
            removeIdx.push_back(i);
        }
    }
    if (relocated.empty())
        return text;
    vector<string> out;
    size_t ri = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (ri < removeIdx.size() && removeIdx[ri] == i)
        {
            ++ri;
            continue;
        }
        out.push_back(lines[i]);
        if (i == steamClose - 1)
            for (const auto &r : relocated)
                out.push_back(r);
    }
    string s;
    for (auto &line : out)
        s += line;
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r'))
        --e;
    return s.substr(0, e) + eol;
}
string lineKey(const string &t)
{
    size_t q1 = t.find('"');
    if (q1 == string::npos)
        return "";
    size_t q2 = t.find('"', q1 + 1);
    if (q2 == string::npos || q2 == q1 + 1)
        return "";
    return t.substr(q1 + 1, q2 - q1 - 1);
}
size_t braceBalancedEnd(const vector<string> &lines, size_t start)
{
    int depth = 0;
    bool opened = false;
    for (size_t j = start; j < lines.size(); ++j)
    {
        bool inString = false;
        for (size_t k = 0; k < lines[j].size(); ++k)
        {
            char c = lines[j][k];
            if (inString)
            {
                if (c == '\\' && k + 1 < lines[j].size())
                    ++k;
                else if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == '{')
                ++depth;
            else if (c == '}')
                --depth;
        }
        if (depth > 0)
            opened = true;
        if (opened && depth <= 0 && j > start)
            return j;
        if (depth < 0)
            return lines.size();
    }
    return lines.size();
}
string dropStrayAppBlocks(const string &text, const string &eol,
                          const vector<string> &ids)
{
    vector<string> lines = splitKeepingNewlines(text);
    auto isAllDigits = [](const string &k) {
        if (k.empty())
            return false;
        for (char c : k)
            if (c < '0' || c > '9')
                return false;
        return true;
    };
    auto isKnownId = [&](const string &quoted) {
        for (const auto &id : ids)
            if (id == quoted)
                return true;
        return false;
    };
    auto isMangledKnownId = [&](const string &key) {
        for (const auto &id : ids)
        {
            string digits = id.substr(1, id.size() - 2);
            if (isDeletionOf(key, digits) || isMangledOf(key, digits, 2))
                return true;
        }
        return false;
    };
    size_t steamHead = lines.size(), steamEnd = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (trimS(lines[i]) == "\"Steam\"")
        {
            size_t e = braceBalancedEnd(lines, i);
            if (e < lines.size())
            {
                steamHead = i;
                steamEnd = e;
            }
            break;
        }
    }
    if (steamEnd == lines.size())
        return text;
    string steamPad = indentOf(lines[steamHead]);
    string appPad = steamPad + "\t";
    size_t appsHead = lines.size(), appsEnd = lines.size();
    for (size_t i = steamHead + 1; i < steamEnd; ++i)
    {
        if (trimS(lines[i]) == "\"apps\"" && indentOf(lines[i]) == appPad)
        {
            size_t e = braceBalancedEnd(lines, i);
            if (e < lines.size())
            {
                appsHead = i;
                appsEnd = e;
            }
            break;
        }
    }
    if (appsHead == lines.size())
        return text;
    vector<char> drop(lines.size(), 0);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i > appsHead && i < appsEnd)
            continue;
        string t = trimS(lines[i]);
        string key = lineKey(t);
        if (!isAllDigits(key))
            continue;
        string quoted = "\"" + key + "\"";
        if (!isKnownId(quoted) && !isMangledKnownId(key))
            continue;
        if (t != quoted)
            continue;
        size_t next = i + 1;
        while (next < lines.size() && trimS(lines[next]).empty())
            ++next;
        if (next >= lines.size() || trimS(lines[next]) != "{")
            continue;
        size_t j = braceBalancedEnd(lines, i);
        if (j >= lines.size())
            continue;
        if (j > appsHead && i < appsEnd)
            continue;
        for (size_t k = i; k <= j; ++k)
            drop[k] = 1;
        i = j;
    }
    vector<string> out;
    for (size_t i = 0; i < lines.size(); ++i)
        if (!drop[i])
            out.push_back(lines[i]);
    string s;
    for (auto &line : out)
        s += line;
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r'))
        --e;
    return s.substr(0, e) + eol;
}
bool writeOutput(const string &path, const string &text, size_t idCount)
{
    if (trimS(text).empty())
    {
        cerr << "Error: refusing to write empty output to " << path << endl;
        return false;
    }
    ofstream sharedConfigFile(path, ios::trunc | ios::binary);
    if (!sharedConfigFile)
    {
        cerr << "Error: Could not open file " << path << endl;
        return false;
    }
    sharedConfigFile << text;
    sharedConfigFile.close();
    cout << ">Modified: " << path << endl;
    cout << ">Disabled cloud for " << idCount << " games" << endl << endl;
    return true;
}
}
CloudDisabler::CloudDisabler() {}
bool CloudDisabler::replaceAppsBlock(const string &sharedConfigPath, const string &sharedConfigText, const string &acfIds)
{
    string eol = detectEol(sharedConfigText);
    vector<string> physical = splitKeepingNewlines(sharedConfigText);
    vector<Tok> tokens = tokenize(physical);
    size_t idx = 0;
    vector<Seg> tops = parseChildren(tokens, idx, true);
    {
        vector<Seg> flat;
        for (auto &top : tops)
            flattenReal(top, flat);
        tops = std::move(flat);
    }
    for (auto &top : tops)
        normalizeTree(top, "", eol);
    {
        vector<Seg> kept;
        for (auto &top : tops)
        {
            if (isFragmentLeaf(top) && (!top.isBlock || !blockHasRecoverable(top)))
                continue;
            kept.push_back(std::move(top));
        }
        tops = std::move(kept);
    }
    for (auto &top : tops)
        if (top.isBlock)
            hoistStoreRootKeys(top, eol);
    vector<string> ids;
    {
        stringstream ss(acfIds);
        string lid;
        while (getline(ss, lid))
        {
            string s = trimS(lid);
            if (!s.empty())
                ids.push_back(s);
        }
    }
    vector<Seg *> allSteams;
    for (auto &top : tops)
        collectSteamRecursive(top, allSteams);
    string outText;
    if (allSteams.empty())
    {
        bool hasRoam = false;
        for (const auto &top : tops)
            if (top.isBlock && ieq(top.name, "UserRoamingConfigStore"))
            {
                hasRoam = true;
                break;
            }
        if (hasRoam)
            outText = addSteamToRoam(tops, ids, eol);
        else
            outText = buildRoamRoot(tops, nullptr, ids, eol);
    }
    else
    {
        Seg *canonical = pickCanonicalSteam(tops, allSteams);
        vector<string> allIds = ids;
        string steamExtra;
        string rootExtraPre;
        for (auto &top : tops)
            recoverScattered(top, canonical, false, indentOf(canonical->header) + "\t",
                             allIds, steamExtra, rootExtraPre, eol);
        allSteams.clear();
        for (auto &top : tops)
            collectSteamRecursive(top, allSteams);
        {
            bool canonicalAlive = false;
            for (Seg *s : allSteams)
                if (s == canonical)
                {
                    canonicalAlive = true;
                    break;
                }
            if (!canonicalAlive)
                canonical = pickCanonicalSteam(tops, allSteams);
        }
        for (Seg *stray : allSteams)
        {
            if (stray == canonical)
                continue;
            if (stray->drop)
                continue;
            for (size_t sKi = 0; sKi < stray->kids.size(); ++sKi)
            {
                Seg &k = stray->kids[sKi];
                if (isAppsBlock(k))
                    continue;
                if (ieq(k.name, "CloudEnabled") && !k.isBlock)
                    continue;
                if (ieq(k.name, "Steam") && k.isBlock)
                    continue;
                if (containsSeg(k, canonical))
                    continue;
                bool sh = k.isBlock &&
                          (ieq(k.name, "Software") || ieq(k.name, "valve") ||
                           isDeletionOf(k.name, "Software") ||
                           isDeletionOf(k.name, "valve") ||
                           isDeletionOf(k.name, "Steam"));
                if (sh)
                {
                    drainChainShell(k, "\t", indentOf(canonical->header) + "\t",
                                    eol, rootExtraPre, steamExtra);
                    continue;
                }
                if (isStoreContainer(k) || isStoreRootLevel(k))
                {
                    if (chainDescendant(k) || containsRealSteam(k) || isStoreBlock(k))
                    {
                        drainStoreWrapper(k, "\t",
                                          indentOf(canonical->header) + "\t",
                                          eol, rootExtraPre, steamExtra);
                        continue;
                    }
                    rootExtraPre += foldJunk(k, "\t", eol, false);
                    continue;
                }
                if (k.name.empty() || !hasKid(*canonical, k.name))
                {
                    steamExtra += foldJunk(k, indentOf(canonical->header) + "\t",
                                           eol, false);
                    k.drop = true;
                }
            }
        }
        for (Seg *stray : allSteams)
            if (stray != canonical)
                stray->drop = true;
        for (auto &top : tops)
            markStrayApps(top, canonical, false);
        transformCanonical(*canonical, allIds, eol);
        if (!steamExtra.empty())
        {
            dedupeSteamExtras(steamExtra, *canonical);
            stripUnbalancedBraceLines(steamExtra);
            steamExtra = alignHeader(steamExtra, indentOf(canonical->header) + "\t");
            if (!steamExtra.empty())
            {
                Seg raw;
                raw.header = steamExtra;
                canonical->kids.push_back(std::move(raw));
            }
        }
        for (auto &top : tops)
            markEmptyContainers(top);
        bool canonicalUnderRoam = false;
        for (const auto &top : tops)
            if (top.isBlock && ieq(top.name, "UserRoamingConfigStore") && containsSeg(top, canonical))
            {
                canonicalUnderRoam = true;
                break;
            }
        if (canonicalUnderRoam)
        {
            for (const auto &top : tops)
                if (top.isBlock && ieq(top.name, "UserRoamingConfigStore") &&
                    containsSeg(top, canonical) && storeWrapperOnChain(top, canonical))
                {
                    canonicalUnderRoam = false;
                    break;
                }
        }
        if (canonicalUnderRoam)
        {
            Seg *canonicalRoam = nullptr;
            for (auto &top : tops)
                if (top.isBlock && ieq(top.name, "UserRoamingConfigStore") && containsSeg(top, canonical))
                {
                    canonicalRoam = &top;
                    break;
                }
            string steamExtra;
            string rootExtra;
            string steamChildIndent = indentOf(canonical->header) + "\t";
            for (auto &top : tops)
            {
                if (top.drop)
                    continue;
                if (&top == canonicalRoam)
                    continue;
                bool chainShell =
                    ieq(top.name, "Software") || ieq(top.name, "valve") ||
                    isDeletionOf(top.name, "Software") || isDeletionOf(top.name, "valve") ||
                    isDeletionOf(top.name, "Steam");
                if (chainShell)
                {
                    drainChainShell(top, "\t", steamChildIndent, eol, rootExtra, steamExtra);
                    continue;
                }
                if (!top.isBlock && ieq(top.name, "CloudEnabled"))
                {
                    top.drop = true;
                    continue;
                }
                if (isStrayBrace(top))
                {
                    top.drop = true;
                    continue;
                }
                if (isStoreContainer(top))
                {
                    drainStoreWrapper(top, "\t", steamChildIndent, eol, rootExtra,
                                      steamExtra);
                    continue;
                }
                if (isSteamLevel(top))
                    steamExtra += foldJunk(top, steamChildIndent, eol, false);
                else if (!top.isBlock && top.name.empty() && rawContainsSteamBlock(top.header))
                    top.drop = true;
                else if (top.isBlock && containsRealSteam(top))
                    drainStoreWrapper(top, "\t", steamChildIndent, eol, rootExtra, steamExtra);
                else if (top.isBlock || !top.name.empty())
                    rootExtra += foldJunk(top, "\t", eol, false);
                else
                    rootExtra += foldJunk(top, "\t", eol, true);
                top.drop = true;
            }
            for (auto &k : canonicalRoam->kids)
            {
                if (k.drop || !isStoreContainer(k))
                    continue;
                if (containsSeg(k, canonical))
                    continue;
                vector<Seg> keptStore;
                for (auto &ck : k.kids)
                {
                    if (ck.drop)
                        continue;
                    bool sh = ck.isBlock &&
                              (ieq(ck.name, "Software") || ieq(ck.name, "valve") ||
                               isDeletionOf(ck.name, "Software") ||
                               isDeletionOf(ck.name, "valve") ||
                               isDeletionOf(ck.name, "Steam") ||
                               containsStructuralChain(ck));
                    if (sh)
                    {
                        drainChainShell(ck, "\t", steamChildIndent, eol, rootExtra,
                                        steamExtra);
                        continue;
                    }
                    if (isAppsBlock(ck))
                        continue;
                    if (isStoreContainer(ck))
                    {
                        drainStoreWrapper(ck, "\t", steamChildIndent, eol, rootExtra,
                                          steamExtra);
                        continue;
                    }
                    if (isSteamLevel(ck))
                    {
                        steamExtra += foldJunk(ck, steamChildIndent, eol, false);
                        continue;
                    }
                    keptStore.push_back(std::move(ck));
                }
                k.kids = std::move(keptStore);
            }
            canonicalizeChainContainers(*canonicalRoam, canonical);
            if (!steamExtra.empty())
            {
                dedupeSteamExtras(steamExtra, *canonical);
                stripUnbalancedBraceLines(steamExtra);
                steamExtra = alignHeader(steamExtra, steamChildIndent);
                if (!steamExtra.empty())
                {
                    Seg raw;
                    raw.header = steamExtra;
                    canonical->kids.push_back(std::move(raw));
                }
            }
            if (!rootExtra.empty() || !rootExtraPre.empty())
            {
                string rootRaw = rootExtraPre + rootExtra;
                stripUnbalancedBraceLines(rootRaw);
                if (!rootRaw.empty())
                {
                    Seg raw;
                    raw.header = rootRaw;
                    canonicalRoam->kids.push_back(std::move(raw));
                }
            }
            for (const auto &top : tops)
            {
                if (top.drop)
                    continue;
                outText += renderSeg(top);
            }
        }
        else
            outText = buildRoamRoot(tops, canonical, allIds, eol, rootExtraPre);
    }
    outText = repairBracelessFriends(outText, eol);
    outText = stripAbsorbedStoreWrapper(outText, eol);
    outText = dropBraceFragmentLeaves(outText, eol);
    outText = relocateStoreRootSteamLeaves(outText, eol);
    outText = dropStrayAppBlocks(outText, eol, ids);
    outText = stripAbsorbedStoreWrapper(outText, eol);
    outText = dropBraceFragmentLeaves(outText, eol);
    outText = relocateStoreRootSteamLeaves(outText, eol);
    outText = dropStrayAppBlocks(outText, eol, ids);
    if (trimS(outText).empty())
        outText = buildRoamRoot(tops, nullptr, ids, eol);
    else
    {
        vector<string> physical2 = splitKeepingNewlines(outText);
        vector<Tok> tokens2 = tokenize(physical2);
        size_t idx2 = 0;
        vector<Seg> tops2 = parseChildren(tokens2, idx2, true);
        bool ok = tops2.size() == 1 && tops2[0].isBlock &&
                  isStoreBlock(tops2[0]);
        if (ok)
        {
            vector<Seg *> steams2;
            collectSteamRecursive(tops2[0], steams2);
            if (steams2.size() != 1)
                ok = false;
        }
        if (!ok)
            outText = buildRoamRoot(tops, nullptr, ids, eol);
    }
    return writeOutput(sharedConfigPath, outText, ids.size());
}
bool CloudDisabler::iterateSharedConfig(const string &userDataPath, const string &acfIds)
{
    FileUtility fileUtility;
    for (const auto &entry : filesystem::directory_iterator(userDataPath))
    {
        if (entry.is_directory())
        {
            string steamID = entry.path().string();
            replace(steamID.begin(), steamID.end(), '\\', '/');
            string remotePath = steamID + "/7/remote";
            if (filesystem::exists(remotePath))
            {
                string sharedConfigPath = remotePath + "/sharedconfig.vdf";
                if (filesystem::exists(sharedConfigPath))
                {
                    string sharedConfigText = fileUtility.readFileContents(sharedConfigPath);
                    replaceAppsBlock(sharedConfigPath, sharedConfigText, acfIds);
                }
            }
        }
    }
    return true;
}
