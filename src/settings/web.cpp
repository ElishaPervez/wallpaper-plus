#include "web.h"

#include <winhttp.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace web {

namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// One session for the whole process; WinHTTP sessions are safe to share between threads.
HINTERNET Session() {
    static HINTERNET session = [] {
        HINTERNET s = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) WallpaperPlus/1.0",
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);  // resolve, connect, send, each read
        return s;
    }();
    return session;
}

std::wstring QueryHeader(HINTERNET request, DWORD which) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};
    value.resize(bytes / sizeof(wchar_t));
    return value;
}

// attachment; filename="galaxy-eyes.1920x1080.mp4"  ->  galaxy-eyes.1920x1080.mp4
std::wstring FilenameFrom(const std::wstring& disposition) {
    size_t at = disposition.find(L"filename=");
    if (at == std::wstring::npos) return {};
    std::wstring name = disposition.substr(at + 9);
    if (!name.empty() && name[0] == L'"') name = name.substr(1, name.find(L'"', 1) - 1);
    return name.substr(0, name.find(L';'));
}

}  // namespace

Response Get(const std::wstring& path, const std::function<bool(const char*, size_t)>& sink,
             const std::function<void(const Response&)>& onResponse) {
    HINTERNET session = Session();
    if (!session) throw std::runtime_error("Couldn't start networking");
    Handle connect(WinHttpConnect(session, L"motionbgs.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    Handle request(connect.h ? WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                             : nullptr);
    if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        throw std::runtime_error("Couldn't connect. Check your internet connection.");

    Response res;
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    res.status = (int)status;
    res.length = _wcstoui64(QueryHeader(request.h, WINHTTP_QUERY_CONTENT_LENGTH).c_str(), nullptr, 10);
    res.filename = FilenameFrom(QueryHeader(request.h, WINHTTP_QUERY_CONTENT_DISPOSITION));
    DWORD urlBytes = 0;
    WinHttpQueryOption(request.h, WINHTTP_OPTION_URL, nullptr, &urlBytes);
    if (urlBytes) {
        res.finalUrl.resize(urlBytes / sizeof(wchar_t));
        if (WinHttpQueryOption(request.h, WINHTTP_OPTION_URL, res.finalUrl.data(), &urlBytes))
            res.finalUrl.resize(wcslen(res.finalUrl.c_str()));
        else
            res.finalUrl.clear();
    }

    if (onResponse) onResponse(res);

    std::vector<char> buffer(64 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.h, buffer.data(), (DWORD)buffer.size(), &read))
            throw std::runtime_error("The download was interrupted");
        if (!read) break;
        if (!sink(buffer.data(), read)) break;
    }
    return res;
}

// ---------- work queue ----------

struct WorkQueue::State {
    struct Job {
        int priority;
        std::string group;
        std::function<void(bool)> run;
    };
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Job> jobs;
};

WorkQueue::WorkQueue(int threads) : state_(std::make_shared<State>()) {
    for (int i = 0; i < threads; ++i) {
        std::thread([s = state_] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;) {
                State::Job job;
                {
                    std::unique_lock<std::mutex> hold(s->lock);
                    s->wake.wait(hold, [&] { return !s->jobs.empty(); });
                    auto best = s->jobs.begin();
                    for (auto it = s->jobs.begin(); it != s->jobs.end(); ++it)
                        if (it->priority < best->priority) best = it;
                    job = std::move(*best);
                    s->jobs.erase(best);
                }
                job.run(false);
            }
        }).detach();  // they live as long as the process; exiting ends them mid-wait
    }
}

void WorkQueue::Push(int priority, std::string group, std::function<void(bool)> job) {
    {
        std::lock_guard<std::mutex> hold(state_->lock);
        state_->jobs.push_back({priority, std::move(group), std::move(job)});
    }
    state_->wake.notify_one();
}

void WorkQueue::CancelGroup(const std::string& group) {
    std::vector<State::Job> dropped;
    {
        std::lock_guard<std::mutex> hold(state_->lock);
        for (auto it = state_->jobs.begin(); it != state_->jobs.end();) {
            if (it->group == group || it->group.rfind(group + ":", 0) == 0) {
                dropped.push_back(std::move(*it));
                it = state_->jobs.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& job : dropped) job.run(true);
}

}  // namespace web
