
#include "steam-tweak-tool/steam_chromedevtoolsprotocol.hpp"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

namespace
{
    const int kFirstPort = 8080;
    const int kLastPort = 8090;
    const char *kCloudListUrl = "https://store.steampowered.com/account/remotestorage";
    const char *kCloudAppUrlBase = "https://store.steampowered.com/account/remotestorageapp/?appid=";

    SOCKET gSocket = INVALID_SOCKET;
    int gPort = 0;
    string gTargetId;
    unsigned long long gNextId = 0;

    unsigned long long gNavigationToken = 0;
    bool gWsaStarted = false;

    unsigned long long nowMs()
    {
        return static_cast<unsigned long long>(GetTickCount64());
    }

    unsigned long long nextNavigationToken()
    {
        if (gNavigationToken == 0)
        {
#ifdef _WIN32
            gNavigationToken = (static_cast<unsigned long long>(GetCurrentProcessId()) << 32) ^ nowMs();
#else
            gNavigationToken = nowMs();
#endif
        }
        return ++gNavigationToken;
    }

    bool wsaEnsure()
    {
        if (!gWsaStarted)
        {
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                return false;
            }
            gWsaStarted = true;
        }
        return true;
    }

    string trimText(const string &text)
    {
        size_t start = text.find_first_not_of(" \t\r\n");
        if (start == string::npos)
        {
            return string();
        }
        size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(start, end - start + 1);
    }

    string lowerText(string text)
    {
        transform(text.begin(), text.end(), text.begin(),
                  [](unsigned char c)
                  { return static_cast<char>(tolower(c)); });
        return text;
    }

    bool containsText(const string &text, const string &needle)
    {
        return text.find(needle) != string::npos;
    }

    string jsonEscape(const string &text)
    {
        string out;
        out.reserve(text.size() + 16);
        char buffer[8];
        for (unsigned char c : text)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else
                {
                    out += static_cast<char>(c);
                }
                break;
            }
        }
        return out;
    }

    void appendUtf8(string &out, unsigned int codePoint)
    {
        if (codePoint <= 0x7F)
        {
            out += static_cast<char>(codePoint);
        }
        else if (codePoint <= 0x7FF)
        {
            out += static_cast<char>(0xC0 | (codePoint >> 6));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
        else if (codePoint <= 0xFFFF)
        {
            out += static_cast<char>(0xE0 | (codePoint >> 12));
            out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (codePoint >> 18));
            out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
    }

    bool parseJsonStringAt(const string &text, size_t pos, string &out, size_t &nextPos)
    {
        if (pos >= text.size() || text[pos] != '"')
        {
            return false;
        }
        out.clear();
        size_t i = pos + 1;
        while (i < text.size())
        {
            char c = text[i];
            if (c == '"')
            {
                nextPos = i + 1;
                return true;
            }
            if (c != '\\')
            {
                out += c;
                i++;
                continue;
            }
            if (i + 1 >= text.size())
            {
                return false;
            }
            char esc = text[i + 1];
            switch (esc)
            {
            case '"': out += '"'; i += 2; break;
            case '\\': out += '\\'; i += 2; break;
            case '/': out += '/'; i += 2; break;
            case 'b': out += '\b'; i += 2; break;
            case 'f': out += '\f'; i += 2; break;
            case 'n': out += '\n'; i += 2; break;
            case 'r': out += '\r'; i += 2; break;
            case 't': out += '\t'; i += 2; break;
            case 'u':
            {
                if (i + 5 >= text.size())
                {
                    return false;
                }
                unsigned int code = 0;
                for (int digit = 0; digit < 4; digit++)
                {
                    char hex = text[i + 2 + digit];
                    code <<= 4;
                    if (hex >= '0' && hex <= '9')
                        code |= static_cast<unsigned int>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f')
                        code |= static_cast<unsigned int>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F')
                        code |= static_cast<unsigned int>(hex - 'A' + 10);
                    else
                        return false;
                }
                i += 6;

                if (code >= 0xD800 && code <= 0xDBFF && i + 5 < text.size() &&
                    text[i] == '\\' && text[i + 1] == 'u')
                {
                    unsigned int low = 0;
                    bool lowOk = true;
                    for (int digit = 0; digit < 4; digit++)
                    {
                        char hex = text[i + 2 + digit];
                        low <<= 4;
                        if (hex >= '0' && hex <= '9')
                            low |= static_cast<unsigned int>(hex - '0');
                        else if (hex >= 'a' && hex <= 'f')
                            low |= static_cast<unsigned int>(hex - 'a' + 10);
                        else if (hex >= 'A' && hex <= 'F')
                            low |= static_cast<unsigned int>(hex - 'A' + 10);
                        else
                        {
                            lowOk = false;
                            break;
                        }
                    }
                    if (lowOk && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                appendUtf8(out, code);
                break;
            }
            default:
                out += esc;
                i += 2;
                break;
            }
        }
        return false;
    }

    void splitJsonObjects(const string &text, vector<string> &out)
    {
        out.clear();
        bool inString = false;
        bool escaped = false;
        int depth = 0;
        size_t start = 0;
        for (size_t i = 0; i < text.size(); i++)
        {
            char c = text[i];
            if (inString)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (c == '\\')
                {
                    escaped = true;
                }
                else if (c == '"')
                {
                    inString = false;
                }
                continue;
            }
            if (c == '"')
            {
                inString = true;
            }
            else if (c == '{')
            {
                if (depth == 0)
                {
                    start = i;
                }
                depth++;
            }
            else if (c == '}')
            {
                if (depth > 0)
                {
                    depth--;
                    if (depth == 0)
                    {
                        out.push_back(text.substr(start, i - start + 1));
                    }
                }
            }
        }
    }

    bool jsonFindString(const string &object, const string &key, string &out)
    {
        string token = "\"" + key + "\"";
        size_t pos = 0;
        while ((pos = object.find(token, pos)) != string::npos)
        {
            size_t i = pos + token.size();
            while (i < object.size() && isspace(static_cast<unsigned char>(object[i])))
            {
                i++;
            }
            if (i >= object.size() || object[i] != ':')
            {
                pos += token.size();
                continue;
            }
            i++;
            while (i < object.size() && isspace(static_cast<unsigned char>(object[i])))
            {
                i++;
            }
            if (i >= object.size() || object[i] != '"')
            {
                return false;
            }
            size_t next = 0;
            return parseJsonStringAt(object, i, out, next);
        }
        return false;
    }

    bool jsonFindValuePos(const string &object, const string &key, size_t &valuePos)
    {
        string token = "\"" + key + "\"";
        size_t pos = 0;
        while ((pos = object.find(token, pos)) != string::npos)
        {
            size_t i = pos + token.size();
            while (i < object.size() && isspace(static_cast<unsigned char>(object[i])))
            {
                i++;
            }
            if (i >= object.size() || object[i] != ':')
            {
                pos += token.size();
                continue;
            }
            i++;
            while (i < object.size() && isspace(static_cast<unsigned char>(object[i])))
            {
                i++;
            }
            valuePos = i;
            return i < object.size();
        }
        return false;
    }

    bool sendAllSocket(SOCKET socket, const char *data, size_t length)
    {
        size_t sent = 0;
        while (sent < length)
        {
            int chunk = send(socket, data + sent, static_cast<int>(length - sent), 0);
            if (chunk <= 0)
            {
                return false;
            }
            sent += static_cast<size_t>(chunk);
        }
        return true;
    }

    bool connectWithTimeout(SOCKET socket, const sockaddr_in &address, int timeoutMs)
    {
        u_long nonBlocking = 1;
        ioctlsocket(socket, FIONBIO, &nonBlocking);
        int result = ::connect(socket, reinterpret_cast<const sockaddr *>(&address),
                               sizeof(address));
        if (result == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL)
            {
                return false;
            }
            fd_set writeSet;
            fd_set errorSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&errorSet);
            FD_SET(socket, &writeSet);
            FD_SET(socket, &errorSet);
            timeval wait = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            int ready = select(0, nullptr, &writeSet, &errorSet, &wait);
            if (ready <= 0 || FD_ISSET(socket, &errorSet))
            {
                return false;
            }
            int socketError = 0;
            int length = sizeof(socketError);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char *>(&socketError), &length) != 0 ||
                socketError != 0)
            {
                return false;
            }
        }
        nonBlocking = 0;
        ioctlsocket(socket, FIONBIO, &nonBlocking);
        return true;
    }

    bool decodeChunked(const string &body, string &out)
    {
        out.clear();
        size_t pos = 0;
        while (pos < body.size())
        {
            size_t lineEnd = body.find("\r\n", pos);
            if (lineEnd == string::npos)
            {
                break;
            }
            string sizeLine = body.substr(pos, lineEnd - pos);
            size_t semicolon = sizeLine.find(';');
            if (semicolon != string::npos)
            {
                sizeLine = sizeLine.substr(0, semicolon);
            }
            unsigned long size = 0;
            try
            {
                size = stoul(trimText(sizeLine), nullptr, 16);
            }
            catch (const exception &)
            {
                return false;
            }
            pos = lineEnd + 2;
            if (size == 0)
            {
                return true;
            }
            if (pos + size > body.size())
            {
                return false;
            }
            out.append(body, pos, size);
            pos += size;
            if (body.compare(pos, 2, "\r\n") == 0)
            {
                pos += 2;
            }
        }
        return !out.empty();
    }

    bool httpFetch(int port, const string &method, const string &path, int timeoutMs, string &body)
    {
        body.clear();
        if (!wsaEnsure())
        {
            return false;
        }
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET)
        {
            return false;
        }

        sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!connectWithTimeout(socket, address, 250))
        {
            closesocket(socket);
            return false;
        }

        string request = method + " " + path + " HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:" + to_string(port) + "\r\n";
        request += "Accept: */*\r\n";
        request += "Connection: close\r\n\r\n";
        if (!sendAllSocket(socket, request.data(), request.size()))
        {
            closesocket(socket);
            return false;
        }

        string raw;
        char buffer[8192];
        unsigned long long deadline = nowMs() + static_cast<unsigned long long>(timeoutMs);
        while (nowMs() < deadline && raw.size() < 8 * 1024 * 1024)
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket, &readSet);
            timeval wait = {0, 150000};
            int ready = select(0, &readSet, nullptr, nullptr, &wait);
            if (ready <= 0)
            {
                continue;
            }
            int received = recv(socket, buffer, sizeof(buffer), 0);
            if (received <= 0)
            {
                break;
            }
            raw.append(buffer, static_cast<size_t>(received));

            size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != string::npos)
            {
                string headerText = lowerText(raw.substr(0, headerEnd));
                size_t lengthPos = headerText.find("content-length:");
                if (lengthPos != string::npos)
                {
                    long long expected = atoll(headerText.c_str() + lengthPos + 15);
                    if (expected >= 0 &&
                        raw.size() >= headerEnd + 4 + static_cast<size_t>(expected))
                    {
                        break;
                    }
                }
                else if (containsText(headerText, "transfer-encoding: chunked") &&
                         raw.size() >= headerEnd + 9 &&
                         raw.compare(raw.size() - 5, 5, "\r\n0\r\n\r\n") == 0)
                {
                    break;
                }
            }
        }
        closesocket(socket);

        int status = 0;
        if (sscanf(raw.c_str(), "HTTP/%*d.%*d %d", &status) != 1 || status != 200)
        {
            return false;
        }
        size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == string::npos)
        {
            return false;
        }
        string headers = lowerText(raw.substr(0, headerEnd));
        string payload = raw.substr(headerEnd + 4);
        if (containsText(headers, "transfer-encoding: chunked"))
        {
            string decoded;
            if (!decodeChunked(payload, decoded))
            {
                return false;
            }
            body = decoded;
        }
        else
        {
            body = payload;
        }
        return true;
    }

    string base64Encode(const unsigned char *data, size_t length)
    {
        static const char *alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        string out;
        out.reserve((length + 2) / 3 * 4);
        for (size_t i = 0; i < length; i += 3)
        {
            unsigned int value = static_cast<unsigned int>(data[i]) << 16;
            int remaining = static_cast<int>(length - i);
            if (remaining > 1)
            {
                value |= static_cast<unsigned int>(data[i + 1]) << 8;
            }
            if (remaining > 2)
            {
                value |= static_cast<unsigned int>(data[i + 2]);
            }
            out += alphabet[(value >> 18) & 0x3F];
            out += alphabet[(value >> 12) & 0x3F];
            out += remaining > 1 ? alphabet[(value >> 6) & 0x3F] : '=';
            out += remaining > 2 ? alphabet[value & 0x3F] : '=';
        }
        return out;
    }

    bool recvExact(SOCKET socket, char *data, size_t length, unsigned long long deadline)
    {
        size_t receivedTotal = 0;
        while (receivedTotal < length)
        {
            unsigned long long remaining = deadline > nowMs() ? deadline - nowMs() : 0;
            if (remaining == 0)
            {
                return false;
            }
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket, &readSet);
            timeval wait = {0, 150000};
            int ready = select(0, &readSet, nullptr, nullptr, &wait);
            if (ready <= 0)
            {
                continue;
            }
            int received = recv(socket, data + receivedTotal,
                                static_cast<int>(length - receivedTotal), 0);
            if (received <= 0)
            {
                return false;
            }
            receivedTotal += static_cast<size_t>(received);
        }
        return true;
    }

    bool wsSendFrame(unsigned char opcode, const string &payload)
    {
        if (gSocket == INVALID_SOCKET)
        {
            return false;
        }
        vector<unsigned char> frame;
        frame.push_back(static_cast<unsigned char>(0x80 | opcode));
        size_t length = payload.size();
        if (length < 126)
        {
            frame.push_back(static_cast<unsigned char>(0x80 | length));
        }
        else if (length <= 0xFFFF)
        {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<unsigned char>((length >> 8) & 0xFF));
            frame.push_back(static_cast<unsigned char>(length & 0xFF));
        }
        else
        {
            frame.push_back(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                frame.push_back(static_cast<unsigned char>((length >> shift) & 0xFF));
            }
        }

        unsigned char mask[4];
        unsigned int seed = static_cast<unsigned int>(GetTickCount()) ^
                            static_cast<unsigned int>(GetCurrentProcessId()) ^
                            static_cast<unsigned int>(length * 2654435761u);
        for (int i = 0; i < 4; i++)
        {
            seed = seed * 1103515245u + 12345u;
            mask[i] = static_cast<unsigned char>((seed >> 16) & 0xFF);
        }
        for (unsigned char byte : mask)
        {
            frame.push_back(byte);
        }
        for (size_t i = 0; i < payload.size(); i++)
        {
            frame.push_back(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
        }
        return sendAllSocket(gSocket, reinterpret_cast<const char *>(frame.data()), frame.size());
    }

    bool wsReadFrame(bool &fin, unsigned char &opcode, string &payload, int timeoutMs)
    {
        unsigned long long deadline = nowMs() + static_cast<unsigned long long>(timeoutMs);
        unsigned char header[2];
        if (!recvExact(gSocket, reinterpret_cast<char *>(header), 2, deadline))
        {
            return false;
        }
        fin = (header[0] & 0x80) != 0;
        opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        unsigned long long length = header[1] & 0x7F;
        if (length == 126)
        {
            unsigned char extended[2];
            if (!recvExact(gSocket, reinterpret_cast<char *>(extended), 2, deadline))
            {
                return false;
            }
            length = (static_cast<unsigned long long>(extended[0]) << 8) | extended[1];
        }
        else if (length == 127)
        {
            unsigned char extended[8];
            if (!recvExact(gSocket, reinterpret_cast<char *>(extended), 8, deadline))
            {
                return false;
            }
            length = 0;
            for (int i = 0; i < 8; i++)
            {
                length = (length << 8) | extended[i];
            }
        }
        if (length > 16ULL * 1024 * 1024)
        {
            return false;
        }
        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked && !recvExact(gSocket, reinterpret_cast<char *>(mask), 4, deadline))
        {
            return false;
        }
        payload.assign(static_cast<size_t>(length), '\0');
        if (length > 0 && !recvExact(gSocket, &payload[0], static_cast<size_t>(length), deadline))
        {
            return false;
        }
        if (masked)
        {
            for (size_t i = 0; i < payload.size(); i++)
            {
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            }
        }
        return true;
    }

    bool wsReadMessage(string &message, int timeoutMs)
    {
        unsigned long long deadline = nowMs() + static_cast<unsigned long long>(timeoutMs);
        message.clear();
        while (nowMs() < deadline)
        {
            bool fin = false;
            unsigned char opcode = 0;
            string payload;
            int remaining = static_cast<int>(deadline - nowMs());
            if (remaining <= 0)
            {
                return false;
            }
            if (!wsReadFrame(fin, opcode, payload, remaining))
            {
                return false;
            }
            if (opcode == 0x9)
            {
                wsSendFrame(0xA, payload);
                continue;
            }
            if (opcode == 0x8)
            {
                return false;
            }
            if (opcode == 0xA)
            {
                continue;
            }
            if (opcode == 0x1 || opcode == 0x0)
            {
                message += payload;
                if (fin)
                {
                    return true;
                }
            }
        }
        return false;
    }

    void closeSocket()
    {
        if (gSocket != INVALID_SOCKET)
        {
            closesocket(gSocket);
            gSocket = INVALID_SOCKET;
        }
    }

    bool wsConnect(int port, const string &websocketUrl, string &error)
    {
        closeSocket();

        string path = "/";
        size_t scheme = websocketUrl.find("://");
        size_t pathStart = websocketUrl.find('/', scheme == string::npos ? 0 : scheme + 3);
        if (pathStart != string::npos)
        {
            path = websocketUrl.substr(pathStart);
        }

        if (!wsaEnsure())
        {
            error = "Windows sockets could not be initialised";
            return false;
        }
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET)
        {
            error = "could not create a socket";
            return false;
        }
        sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!connectWithTimeout(socket, address, 2000))
        {
            closesocket(socket);
            error = "could not connect to Steam's DevTools port";
            return false;
        }

        unsigned char keyBytes[16];
        unsigned int seed = static_cast<unsigned int>(GetTickCount()) ^
                            static_cast<unsigned int>(GetCurrentProcessId());
        for (int i = 0; i < 16; i++)
        {
            seed = seed * 1664525u + 1013904223u;
            keyBytes[i] = static_cast<unsigned char>((seed >> 24) & 0xFF);
        }
        string key = base64Encode(keyBytes, sizeof(keyBytes));

        string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:" + to_string(port) + "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAllSocket(socket, request.data(), request.size()))
        {
            closesocket(socket);
            error = "could not send the DevTools handshake";
            return false;
        }

        string response;
        char c = 0;
        unsigned long long deadline = nowMs() + 5000;
        while (nowMs() < deadline && response.size() < 16384)
        {
            if (!recvExact(socket, &c, 1, deadline))
            {
                break;
            }
            response += c;
            if (response.size() >= 4 &&
                response.compare(response.size() - 4, 4, "\r\n\r\n") == 0)
            {
                break;
            }
        }
        if (!containsText(response, " 101 ") && !containsText(response, " 101\r"))
        {
            closesocket(socket);
            error = "Steam refused the DevTools WebSocket connection";
            return false;
        }

        gSocket = socket;
        gPort = port;
        return true;
    }

    bool wsCommand(const string &method, const string &params, string &response, string &error)
    {
        if (gSocket == INVALID_SOCKET)
        {
            error = "not connected to Steam's DevTools endpoint";
            return false;
        }
        unsigned long long id = ++gNextId;
        string message = "{\"id\":" + to_string(id) + ",\"method\":\"" + method +
                         "\",\"params\":" + (params.empty() ? "{}" : params) + "}";
        if (!wsSendFrame(0x1, message))
        {
            closeSocket();
            error = "could not send a command to Steam's DevTools endpoint";
            return false;
        }
        unsigned long long deadline = nowMs() + 30000;
        while (nowMs() < deadline)
        {
            string incoming;
            int remaining = static_cast<int>(deadline - nowMs());
            if (remaining <= 0 || !wsReadMessage(incoming, remaining))
            {
                break;
            }
            size_t idPos = incoming.find("\"id\":");
            if (idPos == string::npos)
            {
                continue;
            }
            unsigned long long responseId = 0;
            try
            {
                responseId = stoull(incoming.substr(idPos + 5));
            }
            catch (const exception &)
            {
                continue;
            }
            if (responseId != id)
            {
                continue;
            }
            if (containsText(incoming, "\"error\""))
            {
                string message;
                if (jsonFindString(incoming, "message", message) && !message.empty())
                {
                    error = "Steam DevTools error: " + message;
                }
                else
                {
                    error = "Steam DevTools returned an error";
                }
                return false;
            }
            response = incoming;
            return true;
        }
        closeSocket();
        error = "Steam's DevTools endpoint did not answer";
        return false;
    }

    bool evaluate(const string &expression, string &value, string &error)
    {
        string params = "{\"expression\":\"" + jsonEscape(expression) +
                        "\",\"returnByValue\":true,\"awaitPromise\":true}";
        string response;
        if (!wsCommand("Runtime.evaluate", params, response, error))
        {
            return false;
        }
        if (containsText(response, "\"exceptionDetails\""))
        {
            error = "the cloud page script failed";
            return false;
        }
        size_t resultPos = response.find("\"result\"");
        if (resultPos == string::npos)
        {
            value.clear();
            return true;
        }
        size_t valuePos = 0;
        if (!jsonFindValuePos(response.substr(resultPos), "value", valuePos))
        {
            value.clear();
            return true;
        }
        valuePos += resultPos;
        if (response[valuePos] == '"')
        {
            size_t next = 0;
            if (!parseJsonStringAt(response, valuePos, value, next))
            {
                error = "could not decode Steam's DevTools response";
                return false;
            }
            return true;
        }
        size_t end = valuePos;
        while (end < response.size() && response[end] != ',' && response[end] != '}')
        {
            end++;
        }
        value = trimText(response.substr(valuePos, end - valuePos));
        return true;
    }

    struct TargetInfo
    {
        string type;
        string url;
        string websocketUrl;
        string id;
    };

    bool fetchTargets(int port, vector<TargetInfo> &targets)
    {
        targets.clear();
        string body;
        if (!httpFetch(port, "GET", "/json", 400, body))
        {
            return false;
        }
        vector<string> objects;
        splitJsonObjects(body, objects);
        for (const auto &object : objects)
        {
            TargetInfo target;
            if (!jsonFindString(object, "type", target.type))
            {
                continue;
            }
            jsonFindString(object, "url", target.url);
            jsonFindString(object, "webSocketDebuggerUrl", target.websocketUrl);
            jsonFindString(object, "id", target.id);
            if (target.type == "page" && !target.websocketUrl.empty())
            {
                targets.push_back(target);
            }
        }
        return true;
    }

    bool targetLooksLikeCloudPage(const TargetInfo &target)
    {
        return containsText(target.url, "remotestorage");
    }

    bool findSteamPort(int &port)
    {
        for (int candidate = kFirstPort; candidate <= kLastPort; candidate++)
        {
            string body;
            if (!httpFetch(candidate, "GET", "/json/version", 500, body))
            {
                continue;
            }
            if (containsText(body, "Valve Steam Client") ||
                containsText(body, "steamwebhelper"))
            {
                port = candidate;
                return true;
            }
        }
        return false;
    }

    bool findCloudTarget(int &port, TargetInfo &target, string &error)
    {
        int steamPort = 0;
        if (!findSteamPort(steamPort))
        {
            error = "Steam is not running with its DevTools port enabled";
            return false;
        }

        vector<TargetInfo> targets;
        if (!fetchTargets(steamPort, targets))
        {
            error = "Steam's DevTools endpoint did not return its page list";
            return false;
        }

        if (!gTargetId.empty())
        {
            for (const auto &entry : targets)
            {
                if (entry.id == gTargetId)
                {
                    port = steamPort;
                    target = entry;
                    return true;
                }
            }
        }
        for (const auto &entry : targets)
        {
            if (targetLooksLikeCloudPage(entry))
            {
                port = steamPort;
                target = entry;
                return true;
            }
        }
        error = "Steam's built-in browser has no cloud page open";
        return false;
    }

    bool openCloudPageInSteam()
    {
        HINSTANCE result = ShellExecuteA(nullptr, "open",
                                         "steam://openurl/https://store.steampowered.com/account/remotestorage",
                                         nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    }

    bool waitForCloudTarget(int &port, TargetInfo &target, int timeoutMs, string &error)
    {
        unsigned long long deadline = nowMs() + static_cast<unsigned long long>(timeoutMs);
        while (nowMs() < deadline)
        {
            if (findCloudTarget(port, target, error))
            {

                error.clear();
                return true;
            }
            Sleep(500);
        }
        error = "timed out waiting for Steam's built-in browser to open the cloud page";
        return false;
    }

    bool ensureConnected(string &error)
    {
        if (gSocket != INVALID_SOCKET)
        {

            string probe;
            string probeError;
            if (evaluate("1", probe, probeError))
            {
                return true;
            }
            closeSocket();
        }

        int port = 0;
        TargetInfo target;
        if (!findCloudTarget(port, target, error))
        {
            if (!openCloudPageInSteam())
            {
                error = "could not open Steam's cloud page in the Steam client";
                return false;
            }
            if (!waitForCloudTarget(port, target, 25000, error))
            {
                return false;
            }
        }
        gTargetId = target.id;
        if (!wsConnect(port, target.websocketUrl, error))
        {
            return false;
        }
        error.clear();
        return true;
    }

    bool navigate(const string &url, string &error)
    {

        string marker;
        string expectedBase;
        size_t markerPosition = url.rfind("_=");
        if (markerPosition != string::npos)
        {
            marker = url.substr(markerPosition);
            expectedBase = url.substr(0, markerPosition);
        }

        string params = "{\"url\":\"" + jsonEscape(url) + "\"}";
        string response;
        if (!wsCommand("Page.navigate", params, response, error))
        {
            return false;
        }
        Sleep(400);

        unsigned long long deadline = nowMs() + 25000;
        while (nowMs() < deadline)
        {
            string state;
            string probeError;
            bool onTarget = true;
            if (!marker.empty())
            {
                string location;
                onTarget = evaluate("window.location.href", location, probeError) &&
                           location.find(marker) != string::npos &&
                           (expectedBase.empty() || location.find(expectedBase) != string::npos);
            }
            if (onTarget &&
                evaluate("document.readyState", state, probeError) && state == "complete")
            {
                return true;
            }
            Sleep(150);
        }

        error = "Steam's cloud page did not finish navigating";
        return false;
    }

    bool waitForTable(int timeoutMs, string &error)
    {
        unsigned long long deadline = nowMs() + static_cast<unsigned long long>(timeoutMs);
        unsigned long long emptyTableSince = 0;
        while (nowMs() < deadline)
        {
            string rows;
            string probeError;
            if (evaluate("(function(){var t=document.querySelector('.accountTable');"
                         "return t?String(t.querySelectorAll('tr').length):'0';})()",
                         rows, probeError))
            {

                int count = atoi(rows.c_str());
                if (count >= 1)
                {
                    string dataRows;
                    if (evaluate("(function(){var rows=document.querySelectorAll('.accountTable tr');"
                                 "var n=0;for(var i=0;i<rows.length;i++){"
                                 "if(rows[i].querySelectorAll('td').length>=2)n++;}"
                                 "return String(n);})()",
                                 dataRows, probeError))
                    {
                        if (atoi(dataRows.c_str()) > 0)
                        {
                            return true;
                        }
                        string state;
                        if (evaluate("document.readyState", state, probeError) &&
                            state == "complete")
                        {
                            if (emptyTableSince == 0)
                            {
                                emptyTableSince = nowMs();
                            }
                            if (nowMs() - emptyTableSince >= 1500)
                            {
                                return true;
                            }
                        }
                    }
                }
            }
            Sleep(250);
        }

        string location;
        if (evaluate("window.location.href", location, error) &&
            containsText(location, "login"))
        {
            error = "Steam's built-in browser is not signed in";
        }
        else if (error.empty())
        {
            error = "Steam's cloud page did not finish loading";
        }
        return false;
    }

    string urlWithToken(const string &base)
    {
        string url = base;
        url += (url.find('?') == string::npos ? "?" : "&");
        url += "_=" + to_string(nextNavigationToken());
        return url;
    }

    string sanitizeField(const string &text)
    {
        string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++)
        {
            char c = text[i];

            if (c == '\xC2' && i + 1 < text.size() &&
                static_cast<unsigned char>(text[i + 1]) == 0xA0)
            {
                out += ' ';
                i++;
            }
            else if (c == '\t' || c == '\n' || c == '\r')
            {
                out += ' ';
            }
            else
            {
                out += c;
            }
        }
        return trimText(out);
    }

    bool evaluateRows(const string &script, string &rows, string &error)
    {
        if (!evaluate(script, rows, error))
        {
            return false;
        }
        if (!rows.empty())
        {
            return true;
        }
        Sleep(2500);
        return evaluate(script, rows, error);
    }

    bool evaluateOnSteamUi(const string &expression, const string &rejection,
                           string &outError)
    {
        outError.clear();
        int port = 0;
        if (!findSteamPort(port))
        {
            outError = "Steam is not running with its DevTools port enabled";
            return false;
        }
        vector<TargetInfo> targets;
        if (!fetchTargets(port, targets))
        {
            outError = "Steam's DevTools endpoint did not return its page list";
            return false;
        }
        const TargetInfo *ui = nullptr;
        for (const auto &target : targets)
        {
            if (target.url.find("steamloopback.host/index.html") != string::npos)
            {
                ui = &target;
                break;
            }
        }
        if (!ui)
        {
            outError = "Steam's main UI target was not found";
            return false;
        }
        closeSocket();
        if (!wsConnect(port, ui->websocketUrl, outError))
        {
            return false;
        }
        string value;
        bool ok = evaluate(expression, value, outError);
        closeSocket();
        if (!ok)
        {
            return false;
        }
        if (value != "true")
        {
            outError = rejection;
            return false;
        }
        return true;
    }

    bool evaluateOnMainUi(const string &expression, string &outValue, string &outError)
    {
        outValue.clear();
        outError.clear();
        int port = 0;
        if (!findSteamPort(port))
        {
            outError = "Steam is not running with its DevTools port enabled";
            return false;
        }
        vector<TargetInfo> targets;
        if (!fetchTargets(port, targets))
        {
            outError = "Steam's DevTools endpoint did not return its page list";
            return false;
        }
        const TargetInfo *ui = nullptr;
        for (const auto &target : targets)
        {
            if (target.url.find("steamloopback.host/index.html") != string::npos)
            {
                ui = &target;
                break;
            }
        }
        if (!ui)
        {
            outError = "Steam's main UI target was not found";
            return false;
        }
        closeSocket();
        if (!wsConnect(port, ui->websocketUrl, outError))
        {
            return false;
        }
        bool ok = evaluate(expression, outValue, outError);
        closeSocket();
        return ok;
    }
}

bool cdpIsAvailable()
{
    int port = 0;
    return findSteamPort(port);
}

bool cdpFetchCloudGames(vector<CdpCloudGame> &outGames, string &outError)
{
    outGames.clear();
    outError.clear();
    if (!ensureConnected(outError))
    {
        return false;
    }

    const char *script =
        "(function(){var out=[];var rows=document.querySelectorAll('.accountTable tr');"
        "for(var i=0;i<rows.length;i++){var tds=rows[i].querySelectorAll('td');"
        "if(tds.length<3)continue;var a=rows[i].querySelector('a[href*=\"appid=\"]');"
        "if(!a)continue;var m=a.href.match(/appid=(\\d+)/);if(!m)continue;"
        "var name=(tds[0].textContent||'').replace(/[\\t\\r\\n]+/g,' ').trim();"
        "var count=(tds[1].textContent||'').trim();var size=(tds[2].textContent||'').trim();"
        "out.push(m[1]+'\\t'+name+'\\t'+count+'\\t'+size);}"
        "return out.join('\\n');})()";

    auto parseGames = [&](const string &rows)
    {
        size_t pos = 0;
        while (pos <= rows.size())
        {
            size_t lineEnd = rows.find('\n', pos);
            string line = rows.substr(pos, lineEnd == string::npos ? string::npos : lineEnd - pos);
            pos = (lineEnd == string::npos) ? rows.size() + 1 : lineEnd + 1;
            line = trimText(line);
            if (line.empty())
            {
                continue;
            }
            size_t firstTab = line.find('\t');
            size_t secondTab = (firstTab == string::npos) ? string::npos : line.find('\t', firstTab + 1);
            size_t thirdTab = (secondTab == string::npos) ? string::npos : line.find('\t', secondTab + 1);
            if (firstTab == string::npos || secondTab == string::npos || thirdTab == string::npos)
            {
                continue;
            }
            CdpCloudGame game;
            try
            {
                game.appId = stoi(line.substr(0, firstTab));
            }
            catch (const exception &)
            {
                continue;
            }
            if (game.appId <= 0)
            {
                continue;
            }
            game.name = sanitizeField(line.substr(firstTab + 1, secondTab - firstTab - 1));
            string countText = sanitizeField(line.substr(secondTab + 1, thirdTab - secondTab - 1));
            game.fileCount = atoi(countText.c_str());
            game.totalSize = cdpParseSize(line.substr(thirdTab + 1));
            game.totalSizeText = trimText(line.substr(thirdTab + 1));
            outGames.push_back(game);
        }
    };

    string fetchScript =
        "(async function(){const r=await fetch('" + string(kCloudListUrl) + "?_=" +
        to_string(nowMs()) +
        "',{credentials:'include',cache:'no-store'});if(!r.ok)return 'ERROR status '+r.status;"
        "const t=await r.text();const d=new DOMParser().parseFromString(t,'text/html');"
        "const rows=d.querySelectorAll('.accountTable tr');const out=[];"
        "for(let i=0;i<rows.length;i++){const tds=rows[i].querySelectorAll('td');"
        "if(tds.length<3)continue;const a=rows[i].querySelector('a[href*=\"appid=\"]');"
        "if(!a)continue;const m=a.href.match(/appid=(\\d+)/);if(!m)continue;"
        "const name=(tds[0].textContent||'').replace(/[\\t\\r\\n]+/g,' ').trim();"
        "out.push(m[1]+'\\t'+name+'\\t'+(tds[1].textContent||'').trim()+'\\t'"
        "+(tds[2].textContent||'').trim());}"
        "return out.length?out.join('\\n'):'ERROR empty';})()";

    string fetched;
    string fetchError;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (evaluate(fetchScript, fetched, fetchError) && fetched.compare(0, 6, "ERROR ") != 0)
        {
            parseGames(fetched);
            if (!outGames.empty())
            {
                return true;
            }
        }
        Sleep(250);
    }

    if (!navigate(urlWithToken(kCloudListUrl), outError))
    {
        return false;
    }
    if (!waitForTable(20000, outError))
    {
        return false;
    }

    string rows;
    if (!evaluateRows(script, rows, outError))
    {
        return false;
    }
    parseGames(rows);
    return true;
}

bool cdpHasCloudFiles(int appId, bool &outHasFiles, string &outError)
{
    outHasFiles = false;
    outError.clear();
    if (appId <= 0)
    {
        outError = "invalid app id";
        return false;
    }
    if (!ensureConnected(outError))
    {
        return false;
    }

    string script =
        "(async function(){const r=await fetch('/account/remotestorageapp/?appid=" +
        to_string(appId) + "&_=" + to_string(nowMs()) +
        "',{credentials:'include',cache:'no-store'});if(!r.ok)return 'ERROR status '+r.status;"
        "const t=await r.text();const d=new DOMParser().parseFromString(t,'text/html');"
        "const table=d.querySelector('.accountTable');if(!table)return 'ERROR no table';"
        "const rows=table.querySelectorAll('tr');let count=0;"
        "for(let i=0;i<rows.length;i++){const cells=rows[i].querySelectorAll('td');"
        "if(cells.length>=2&&cells[1].textContent.trim())count++;}"
        "return 'FILES '+count;})()";

    string value;
    if (!evaluate(script, value, outError))
    {
        return false;
    }
    if (value.compare(0, 6, "ERROR ") == 0)
    {
        outError = "Steam's cloud page could not list app " + to_string(appId) +
                   " (" + value.substr(6) + ")";
        return false;
    }
    if (value.compare(0, 6, "FILES ") != 0)
    {
        outError = "unexpected answer from Steam's cloud page for app " + to_string(appId);
        return false;
    }
    if (atoi(value.c_str() + 6) == 0)
    {
        Sleep(80);
        string second;
        if (!evaluate(script, second, outError))
        {
            return false;
        }
        if (second.compare(0, 6, "ERROR ") == 0)
        {
            outError = "Steam's cloud page could not list app " + to_string(appId) +
                       " (" + second.substr(6) + ")";
            return false;
        }
        if (second.compare(0, 6, "FILES ") != 0)
        {
            outError = "unexpected answer from Steam's cloud page for app " +
                       to_string(appId);
            return false;
        }
        value = second;
    }
    outHasFiles = atoi(value.c_str() + 6) > 0;
    return true;
}

bool fetchCloudPageRows(const string &url, int nextIndex, string &rows, bool &hasNext,
                        string &outError)
{
    string script =
        "(async function(){try{const r=await fetch('" + jsonEscape(url) + "',"
        "{credentials:'include',cache:'no-store'});"
        "if(!r.ok)return 'ERROR status '+r.status;"
        "const t=await r.text();const d=new DOMParser().parseFromString(t,'text/html');"
        "const table=d.querySelector('.accountTable');if(!table)return 'ERROR no table';"
        "const out=[];const trs=table.querySelectorAll('tr');"
        "for(let i=0;i<trs.length;i++){const tds=trs[i].querySelectorAll('td');"
        "if(tds.length<4)continue;"
        "const root=(tds[0].textContent||'').replace(/[\\t\\r\\n]+/g,' ').trim();"
        "const name=(tds[1].textContent||'').trim();if(!name)continue;"
        "const size=(tds[2].textContent||'').trim();"
        "const link=trs[i].querySelector('a[href*=\"filedownload\"],a[href*=\"ugc\"],"
        "a[href*=\"steamusercontent\"]');"
        "out.push(root+'\\t'+name+'\\t'+size+'\\t'+(link?link.href:''));}"
        "let next='0';const as=d.querySelectorAll('a[href*=\"index=\"]');"
        "for(let i=0;i<as.length;i++){const m=(as[i].getAttribute('href')||'')"
        ".match(/[?&]index=(\\d+)/);"
        "if(m&&parseInt(m[1],10)===" + to_string(nextIndex) + "){next='1';break;}}"
        "return 'OK\\n'+next+'\\n'+out.join('\\n');}catch(e){return 'ERROR '+e;}})()";

    string value;
    if (!evaluate(script, value, outError))
    {
        return false;
    }

    size_t firstBreak = value.find('\n');
    if (firstBreak == string::npos)
    {
        outError = "unexpected cloud page response";
        return false;
    }
    string status = value.substr(0, firstBreak);
    if (status != "OK")
    {
        outError = status.empty() ? "empty cloud page response" : status;
        return false;
    }

    size_t secondBreak = value.find('\n', firstBreak + 1);
    if (secondBreak == string::npos)
    {
        outError = "unexpected cloud page response";
        return false;
    }

    hasNext = trimText(value.substr(firstBreak + 1, secondBreak - firstBreak - 1)) == "1";
    rows = value.substr(secondBreak + 1);
    outError.clear();
    return true;
}

bool cdpFetchCloudFiles(int appId, vector<CdpCloudFile> &outFiles, string &outError,
                        bool allowEmpty, int expectedCount)
{
    outFiles.clear();
    outError.clear();
    if (appId <= 0)
    {
        outError = "invalid app id";
        return false;
    }
    if (!ensureConnected(outError))
    {
        return false;
    }

    const char *script =
        "(function(){var out=[];var rows=document.querySelectorAll('.accountTable tr');"
        "for(var i=0;i<rows.length;i++){var tds=rows[i].querySelectorAll('td');"
        "if(tds.length<4)continue;var root=(tds[0].textContent||'').replace(/[\\t\\r\\n]+/g,' ').trim();"
        "var name=(tds[1].textContent||'').trim();if(!name)continue;"
        "var size=(tds[2].textContent||'').trim();"
        "var link=rows[i].querySelector('a[href*=\"filedownload\"],a[href*=\"ugc\"],a[href*=\"steamusercontent\"]');"
        "var url=link?link.href:'';"
        "out.push(root+'\\t'+name+'\\t'+size+'\\t'+url);}"
        "return out.join('\\n');})()";

    auto parseRows = [&](const string &rows) -> size_t
    {
        size_t pos = 0;
        size_t rowCount = 0;
        while (pos <= rows.size())
        {
            size_t lineEnd = rows.find('\n', pos);
            string line = rows.substr(pos, lineEnd == string::npos ? string::npos : lineEnd - pos);
            pos = (lineEnd == string::npos) ? rows.size() + 1 : lineEnd + 1;

            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            rowCount++;
            size_t firstTab = line.find('\t');
            size_t secondTab = (firstTab == string::npos) ? string::npos : line.find('\t', firstTab + 1);
            size_t thirdTab = (secondTab == string::npos) ? string::npos : line.find('\t', secondTab + 1);
            if (firstTab == string::npos || secondTab == string::npos)
            {
                continue;
            }
            CdpCloudFile file;
            file.rootLabel = sanitizeField(line.substr(0, firstTab));
            file.name = sanitizeField(line.substr(firstTab + 1, secondTab - firstTab - 1));
            file.size = cdpParseSize(line.substr(secondTab + 1,
                                                thirdTab == string::npos
                                                    ? string::npos
                                                    : thirdTab - secondTab - 1));
            file.sizeText = sanitizeField(line.substr(secondTab + 1,
                                                      thirdTab == string::npos
                                                          ? string::npos
                                                          : thirdTab - secondTab - 1));
            if (thirdTab != string::npos)
            {
                file.downloadUrl = trimText(line.substr(thirdTab + 1));
            }
            if (file.name.empty())
            {
                continue;
            }
            file.root = cdpRootFromLabel(file.rootLabel);
            outFiles.push_back(file);
        }
        return rowCount;
    };

    auto readPage = [&](const string &url, int nextIndex, string &rows, bool &hasNext) -> bool
    {
        string fetchError;
        for (int attempt = 0; attempt < 4; attempt++)
        {
            hasNext = false;
            if (fetchCloudPageRows(url, nextIndex, rows, hasNext, fetchError) && !rows.empty())
            {
                return true;
            }
            Sleep(250);
        }
        if (!navigate(url, outError))
        {
            return false;
        }
        if (!waitForTable(20000, outError))
        {
            return false;
        }
        if (!evaluateRows(script, rows, outError))
        {
            return false;
        }
        outError.clear();
        return true;
    };

    int index = 0;
    for (int page = 0; page < 60; page++)
    {
        string url = string(kCloudAppUrlBase) + to_string(appId);
        if (index > 0)
        {
            url += "&index=" + to_string(index);
        }
        url += "&_=" + to_string(nextNavigationToken());

        string rows;
        bool hasNext = false;
        if (!readPage(url, index + 50, rows, hasNext))
        {
            return false;
        }

        size_t rowCount = parseRows(rows);
        if (rowCount == 0)
        {
            Sleep(1500);
            if (!evaluateRows(script, rows, outError))
            {
                return false;
            }
            rowCount = parseRows(rows);
        }

        size_t known = outFiles.size();
        if (rowCount == 0)
        {
            bool expectedMore = expectedCount > 0 && known < static_cast<size_t>(expectedCount);
            if (expectedMore || (expectedCount <= 0 && page > 0))
            {
                outFiles.clear();
                outError = "Steam's cloud page stopped listing app " + to_string(appId) +
                           " part-way through; try again";
                return false;
            }
            break;
        }

        bool shortOfCount = expectedCount > 0 && known < static_cast<size_t>(expectedCount);
        if (!shortOfCount && (rowCount < 50 || !hasNext))
        {
            break;
        }
        index += 50;
    }

    if (expectedCount > 0 && outFiles.size() < static_cast<size_t>(expectedCount))
    {
        outFiles.clear();
        outError = "Steam's cloud page listed " + to_string(expectedCount) + " file(s) for app " +
                   to_string(appId) + " but only part of the list could be read; try again";
        return false;
    }
    if (outFiles.empty() && !allowEmpty)
    {
        outError = "Steam's cloud page listed no files for app " + to_string(appId);
        return false;
    }
    return true;
}

bool cdpRefreshCloudPage(string &outError)
{
    outError.clear();
    if (!ensureConnected(outError))
    {
        return false;
    }
    if (!navigate(urlWithToken(kCloudListUrl), outError))
    {
        return false;
    }
    if (!waitForTable(20000, outError))
    {
        return false;
    }
    outError.clear();
    return true;
}

bool cdpRetryAppSync(int appId, string &outError)
{
    return evaluateOnSteamUi("SteamClient.Cloud.RetryAppSync(" + to_string(appId) + "), true",
                            "Steam did not accept the sync retry", outError);
}

bool cdpPreferLocalAppSync(int appId, string &outError)
{
    return evaluateOnSteamUi("SteamClient.Cloud.ResolveAppSyncConflict(" +
                                 to_string(appId) + ",true), true",
                             "Steam did not accept the local sync preference", outError);
}

bool cdpFetchConflictingApps(const vector<int> &appIds, set<int> &outConflicted,
                             string &outError)
{
    outConflicted.clear();
    outError.clear();
    if (appIds.empty())
    {
        return true;
    }
    string ids;
    for (int appId : appIds)
    {
        if (appId <= 0)
        {
            continue;
        }
        if (!ids.empty())
        {
            ids += ',';
        }
        ids += to_string(appId);
    }
    if (ids.empty())
    {
        return true;
    }
    string expression =
        "Promise.all([" + ids +
        "].map(function(id){return SteamClient.Apps.GetConflictingFileTimestamps(id)"
        ".then(function(v){return (v && (v.rtLocalTime || v.rtRemoteTime)) ? id : 0;})}))"
        ".then(function(r){return r.filter(function(x){return x > 0;}).join(',');})";
    string value;
    if (!evaluateOnMainUi(expression, value, outError))
    {
        return false;
    }
    istringstream stream(trimText(value));
    string token;
    while (getline(stream, token, ','))
    {
        token = trimText(token);
        if (token.empty() || token.find_first_not_of("0123456789") != string::npos)
        {
            continue;
        }
        int appId = 0;
        try
        {
            appId = stoi(token);
        }
        catch (const exception &)
        {
            continue;
        }
        if (appId > 0)
        {
            outConflicted.insert(appId);
        }
    }
    outError.clear();
    return true;
}


int cdpRootFromLabel(const string &label)
{
    string text = lowerText(trimText(label));
    if (text.empty() || containsText(text, "default") || containsText(text, "api"))
    {
        return 0;
    }
    if (containsText(text, "gameinstall"))
    {
        return 1;
    }
    if (containsText(text, "winmydocuments") || containsText(text, "macdocuments"))
    {
        return 2;
    }
    if (containsText(text, "winappdatalocallow"))
    {
        return 12;
    }
    if (containsText(text, "winappdatalocal"))
    {
        return 3;
    }
    if (containsText(text, "winappdataroaming"))
    {
        return 4;
    }
    if (containsText(text, "steamuserbasestorage"))
    {
        return 5;
    }
    if (containsText(text, "winsavedgames"))
    {
        return 9;
    }
    if (containsText(text, "winprogramdata"))
    {
        return 10;
    }
    if (containsText(text, "steamclouddocuments"))
    {
        return 11;
    }
    if (containsText(text, "macappsupport"))
    {
        return 7;
    }
    if (containsText(text, "machome"))
    {
        return 6;
    }
    if (containsText(text, "maccaches"))
    {
        return 13;
    }
    if (containsText(text, "linuxhome"))
    {
        return 14;
    }
    if (containsText(text, "windowshome"))
    {
        return 18;
    }
    if (containsText(text, "linuxxdgdatahome"))
    {
        return 15;
    }
    if (containsText(text, "linuxxdgconfighome"))
    {
        return 16;
    }
    if (containsText(text, "android"))
    {
        return 17;
    }
    if (text == "home")
    {
        return 6;
    }
    return -1;
}

unsigned long long cdpParseSize(const string &text)
{
    string value = trimText(text);
    for (char &c : value)
    {
        if (static_cast<unsigned char>(c) == 0xA0)
        {
            c = ' ';
        }
    }
    if (value.empty())
    {
        return 0;
    }
    char *end = nullptr;
    double number = strtod(value.c_str(), &end);
    if (end == value.c_str())
    {
        return 0;
    }
    string unit = lowerText(trimText(value.substr(static_cast<size_t>(end - value.c_str()))));
    double multiplier = 1.0;
    if (unit == "kb" || unit == "kib")
    {
        multiplier = 1024.0;
    }
    else if (unit == "mb" || unit == "mib")
    {
        multiplier = 1024.0 * 1024.0;
    }
    else if (unit == "gb" || unit == "gib")
    {
        multiplier = 1024.0 * 1024.0 * 1024.0;
    }
    else if (unit == "tb" || unit == "tib")
    {
        multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    }
    return static_cast<unsigned long long>(number * multiplier + 0.5);
}

#else

bool cdpIsAvailable() { return false; }
bool cdpFetchCloudGames(vector<CdpCloudGame> &, string &outError)
{
    outError = "Steam cloud scraping is only supported on Windows builds";
    return false;
}
bool cdpFetchCloudFiles(int, vector<CdpCloudFile> &, string &outError, bool, int)
{
    outError = "Steam cloud scraping is only supported on Windows builds";
    return false;
}
bool cdpHasCloudFiles(int, bool &, string &outError)
{
    outError = "Steam cloud scraping is only supported on Windows builds";
    return false;
}
bool cdpRefreshCloudPage(string &outError)
{
    outError = "Steam cloud scraping is only supported on Windows builds";
    return false;
}
bool cdpRetryAppSync(int, string &outError)
{
    outError = "Steam cloud scraping is only supported on Windows builds";
    return false;
}
bool cdpPreferLocalAppSync(int, string &outError)
{
    outError = "Steam cloud scraping is only supported on Windows builds";
    return false;
}
bool cdpFetchConflictingApps(const vector<int> &, set<int> &outConflicted, string &outError)
{
    outConflicted.clear();
    outError = "Steam cloud scraping is only supported on Windows builds";
    return false;
}
int cdpRootFromLabel(const string &) { return -1; }
unsigned long long cdpParseSize(const string &) { return 0; }

#endif
