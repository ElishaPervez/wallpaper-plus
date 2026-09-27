#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Network access for the Browse tab. Everything is fetched from https://motionbgs.com only; the
// web page itself is never allowed onto the internet (see Bridge::Attach), so these calls are the
// single way anything gets in.
namespace web {

struct Response {
    int status = 0;
    std::wstring finalUrl;  // after redirects (search can land on a topic page)
    std::wstring filename;  // from Content-Disposition, if the server named the file
    uint64_t length = 0;    // 0 when the server didn't say
};

// Blocking GET of https://motionbgs.com + path. `onResponse` sees the status and headers before
// the body arrives; the body is then handed to `sink` in chunks, and returning false from it
// aborts the transfer. Throws std::runtime_error when the site can't be reached.
Response Get(const std::wstring& path, const std::function<bool(const char*, size_t)>& sink,
             const std::function<void(const Response&)>& onResponse = nullptr);

// A fixed set of background threads. Jobs with a lower priority number run first; equal
// priorities run in the order they were added. A job receives true instead of running when it
// was cancelled while still waiting, so it can still answer whoever asked for it.
class WorkQueue {
public:
    explicit WorkQueue(int threads);
    void Push(int priority, std::string group, std::function<void(bool cancelled)> job);
    // Cancels every waiting job of `group`, and of its subgroups "group:…" (jobs already running
    // finish normally).
    void CancelGroup(const std::string& group);

private:
    struct State;
    std::shared_ptr<State> state_;  // shared with the threads, which are never joined
};

}  // namespace web
