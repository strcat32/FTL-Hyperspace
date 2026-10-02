#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

// HTTPS for the master server (roadmap BG, 4.3; docs/design/ranked-play.md in the FTL: Duels repository): Windows' own
// WinHTTP in a worker thread for each request (no TLS code of our own), the answer handed to the game's thread in
// Http::Frame. And a small reader for the master's JSON answers (flat objects and lists of them).
namespace Duels
{
    namespace Http
    {
        struct Response
        {
            int status = 0;             // the HTTP status; 0 when no answer came (error says why)
            std::string body, error;
        };
        using Callback = std::function<void(const Response &)>;

        // A request (GET without a body, POST with one as JSON, or of the type given: a demo's bytes); bearer: an
        // Authorization: Bearer header, if not empty. The answer comes to done in a later Frame, on the game's thread.
        void Send(const std::string &method, const std::string &url, const std::string &body, const std::string &bearer, Callback done,
                  const std::string &contentType = "application/json");
        void Frame();
        int Pending();   // requests not answered yet

        // The JSON of an answer, read: an object's members as text (strings without quotes, numbers as written, true or
        // false), a list's objects in order. False when it isn't JSON of that shape.
        using Object = std::map<std::string, std::string>;
        bool ReadObject(const std::string &json, Object &out);
        bool ReadList(const std::string &json, std::vector<Object> &out);
        // A string for JSON (quotes and the characters JSON escapes).
        std::string Quote(const std::string &text);
    }
}
