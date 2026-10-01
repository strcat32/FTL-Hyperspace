#include "Global.h"
#include "Duels.h"
#include "DuelsHttp.h"

#include <cctype>
#include <cstdio>
#include <deque>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace Duels
{
    namespace Http
    {
        // ---------------------------------------------------------------------------------------------------------
        // Requests: a worker thread each (Windows' own threads and lock: the build has no std::thread)
        // ---------------------------------------------------------------------------------------------------------

        struct Job
        {
            std::string method, url, body, bearer;
            Callback done;
            Response response;
        };

        static int g_pending = 0;   // the game's thread only
#ifdef _WIN32
        static CRITICAL_SECTION g_lock;
        static bool g_lockMade = false;
        static std::deque<Job*> g_done;

        static void MakeLock()
        {
            if (g_lockMade) return;
            InitializeCriticalSection(&g_lock);
            g_lockMade = true;
        }

        static std::wstring Wide(const std::string &text)
        {
            if (text.empty()) return std::wstring();
            int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
            std::wstring out((size_t)std::max(0, size), L'\0');
            if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
            return out;
        }

        static void Fetch(Job &job)
        {
            Response &r = job.response;
            std::wstring url = Wide(job.url);
            URL_COMPONENTS parts;
            ZeroMemory(&parts, sizeof(parts));
            parts.dwStructSize = sizeof(parts);
            parts.dwHostNameLength = (DWORD)-1;
            parts.dwUrlPathLength = (DWORD)-1;
            parts.dwExtraInfoLength = (DWORD)-1;
            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || !parts.lpszHostName)
            {
                r.error = "not an address: " + job.url;
                return;
            }
            std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
            std::wstring path = parts.lpszUrlPath ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : std::wstring(L"/");
            if (parts.lpszExtraInfo) path += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
            if (path.empty()) path = L"/";

            HINTERNET session = WinHttpOpen(L"FTL-Duels", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            HINTERNET connect = session ? WinHttpConnect(session, host.c_str(), parts.nPort, 0) : nullptr;
            HINTERNET request = connect ? WinHttpOpenRequest(connect, Wide(job.method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                        : nullptr;
            if (session) WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
            std::wstring headers = L"Accept: application/json\r\n";
            if (job.method == "POST") headers += L"Content-Type: application/json\r\n";
            if (!job.bearer.empty()) headers += L"Authorization: Bearer " + Wide(job.bearer) + L"\r\n";
            BOOL sent = request && WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L,
                                                      job.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)job.body.data(),
                                                      (DWORD)job.body.size(), (DWORD)job.body.size(), 0) &&
                        WinHttpReceiveResponse(request, nullptr);
            if (sent)
            {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                    WINHTTP_NO_HEADER_INDEX);
                r.status = (int)status;
                for (;;)
                {
                    DWORD available = 0, read = 0;
                    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                    std::string chunk(available, '\0');
                    if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0) break;
                    chunk.resize(read);
                    r.body += chunk;
                    if (r.body.size() > (1u << 20)) break;   // the master's answers are small
                }
            }
            else
            {
                char text[64];
                std::snprintf(text, sizeof(text), "no answer (WinHTTP error %lu)", (unsigned long)GetLastError());
                r.error = text;
            }
            if (request) WinHttpCloseHandle(request);
            if (connect) WinHttpCloseHandle(connect);
            if (session) WinHttpCloseHandle(session);
        }

        static DWORD WINAPI Worker(LPVOID data)
        {
            Job *job = (Job*)data;
            Fetch(*job);
            EnterCriticalSection(&g_lock);
            g_done.push_back(job);
            LeaveCriticalSection(&g_lock);
            return 0;
        }

        void Send(const std::string &method, const std::string &url, const std::string &body, const std::string &bearer, Callback done)
        {
            MakeLock();
            Job *job = new Job();
            job->method = method;
            job->url = url;
            job->body = body;
            job->bearer = bearer;
            job->done = done;
            HANDLE thread = CreateThread(nullptr, 0, Worker, job, 0, nullptr);
            if (!thread)
            {
                job->response.error = "no thread for the request";
                EnterCriticalSection(&g_lock);
                g_done.push_back(job);
                LeaveCriticalSection(&g_lock);
            }
            else CloseHandle(thread);
            ++g_pending;
        }

        void Frame()
        {
            if (!g_lockMade) return;
            std::deque<Job*> ready;
            EnterCriticalSection(&g_lock);
            ready.swap(g_done);
            LeaveCriticalSection(&g_lock);
            for (Job *job : ready)
            {
                --g_pending;
                if (job->done) job->done(job->response);
                delete job;
            }
        }
#else
        void Send(const std::string &, const std::string &, const std::string &, const std::string &, Callback done)
        {
            Response response;
            response.error = "the master is reached on Windows only";
            if (done) done(response);
        }

        void Frame()
        {
        }
#endif

        int Pending()
        {
            return g_pending;
        }

        // ---------------------------------------------------------------------------------------------------------
        // JSON: objects of strings, numbers, true, false and null, and lists of such objects
        // ---------------------------------------------------------------------------------------------------------

        struct Parser
        {
            const std::string &text;
            size_t pos = 0;

            explicit Parser(const std::string &t) : text(t) {}

            void Space()
            {
                while (pos < text.size() && std::isspace((unsigned char)text[pos])) ++pos;
            }

            bool Take(char c)
            {
                Space();
                if (pos < text.size() && text[pos] == c)
                {
                    ++pos;
                    return true;
                }
                return false;
            }

            bool String(std::string &out)
            {
                if (!Take('"')) return false;
                out.clear();
                while (pos < text.size() && text[pos] != '"')
                {
                    char c = text[pos++];
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (pos >= text.size()) return false;
                    char e = text[pos++];
                    switch (e)
                    {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u':
                    {
                        if (pos + 4 > text.size()) return false;
                        unsigned code = (unsigned)std::strtoul(text.substr(pos, 4).c_str(), nullptr, 16);
                        pos += 4;
                        // UTF-8 for the basic plane (a name's letters); surrogate pairs become a '?'.
                        if (code < 0x80) out += (char)code;
                        else if (code < 0x800)
                        {
                            out += (char)(0xC0 | (code >> 6));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        else if (code >= 0xD800 && code <= 0xDFFF) out += '?';
                        else
                        {
                            out += (char)(0xE0 | (code >> 12));
                            out += (char)(0x80 | ((code >> 6) & 0x3F));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: out += e; break;
                    }
                }
                return pos++ < text.size();
            }

            // A value as text: a string's contents, or a number, true, false, null as written.
            bool Value(std::string &out)
            {
                Space();
                if (pos < text.size() && text[pos] == '"') return String(out);
                size_t start = pos;
                while (pos < text.size() && (std::isalnum((unsigned char)text[pos]) || text[pos] == '-' || text[pos] == '+' || text[pos] == '.'))
                    ++pos;
                out = text.substr(start, pos - start);
                return !out.empty();
            }

            bool ObjectInto(Object &out)
            {
                out.clear();
                if (!Take('{')) return false;
                if (Take('}')) return true;
                do
                {
                    std::string key, value;
                    if (!String(key) || !Take(':') || !Value(value)) return false;
                    out[key] = value;
                } while (Take(','));
                return Take('}');
            }
        };

        bool ReadObject(const std::string &json, Object &out)
        {
            Parser p(json);
            if (!p.ObjectInto(out)) return false;
            p.Space();
            return p.pos == json.size();
        }

        bool ReadList(const std::string &json, std::vector<Object> &out)
        {
            Parser p(json);
            out.clear();
            if (!p.Take('[')) return false;
            if (p.Take(']')) return true;
            do
            {
                Object object;
                if (!p.ObjectInto(object)) return false;
                out.push_back(object);
            } while (p.Take(','));
            if (!p.Take(']')) return false;
            p.Space();
            return p.pos == json.size();
        }

        std::string Quote(const std::string &text)
        {
            std::string out = "\"";
            for (char c : text)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char)c < 0x20)
                    {
                        char code[8];
                        std::snprintf(code, sizeof(code), "\\u%04x", (unsigned)(unsigned char)c);
                        out += code;
                    }
                    else out += c;
                }
            }
            return out + "\"";
        }
    }
}
