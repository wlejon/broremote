// Windows process spawning: piped children (ssh, the proxy) and plain processes.
#include "broremote/stream.h"
#include "win_util.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace broremote {

namespace {

// Quote one argument the way the MSVC runtime's argv parser undoes it.
std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string out = "\"";
    for (auto it = a.begin();; ++it) {
        size_t backslashes = 0;
        while (it != a.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }
        if (it == a.end()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(*it);
        }
    }
    out.push_back('"');
    return out;
}

std::wstring command_line(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const std::string& a : argv) {
        if (!cmd.empty()) cmd.push_back(' ');
        cmd += quote_arg(a);
    }
    return win::to_wide(cmd);
}

// CreateProcess inheriting exactly `inherit` (nothing when empty).
bool create_process(const std::vector<std::string>& argv, DWORD flags, std::vector<HANDLE> inherit, HANDLE in,
                    HANDLE out, HANDLE errh, PROCESS_INFORMATION& pi, std::string* err) {
    if (argv.empty()) {
        if (err) *err = "empty command";
        return false;
    }
    std::wstring cmd = command_line(argv);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    std::vector<char> attr_buf;
    const bool use_list = !inherit.empty();
    if (use_list) {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = in;
        si.StartupInfo.hStdOutput = out;
        si.StartupInfo.hStdError = errh;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attr_buf.resize(size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) ||
            !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                       inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
            if (err) *err = "cannot set up handle inheritance: " + win::error_text(GetLastError());
            return false;
        }
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, use_list ? TRUE : FALSE, flags, nullptr, nullptr,
                             &si.StartupInfo, &pi);
    DWORD e = GetLastError();
    if (use_list) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) {
        if (err) *err = "cannot start " + argv[0] + ": " + win::error_text(e);
        return false;
    }
    return true;
}

// A child's stdin/stdout as a stream; stderr collected for diagnostics.
class ChildStream final : public Stream {
public:
    ChildStream(PROCESS_INFORMATION pi, HANDLE in_w, HANDLE out_r, HANDLE err_r)
        : proc_(pi.hProcess), in_w_(in_w), out_r_(out_r), err_r_(err_r) {
        CloseHandle(pi.hThread);
        err_thread_ = std::thread([this] {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(err_r_, buf, sizeof buf, &n, nullptr) && n > 0) {
                std::lock_guard<std::mutex> lk(mu_);
                stderr_.append(buf, n);
                if (stderr_.size() > 8192) stderr_.erase(0, stderr_.size() - 8192);
            }
        });
    }
    ~ChildStream() override {
        // Closing its stdin lets a well-behaved child (the proxy) exit on its
        // own; one that does not is terminated.
        close_input();
        if (WaitForSingleObject(proc_, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(proc_, 1);
            WaitForSingleObject(proc_, 5000);
        }
        if (err_thread_.joinable()) err_thread_.join();
        CloseHandle(out_r_);
        CloseHandle(err_r_);
        CloseHandle(proc_);
    }
    size_t read(char* buf, size_t n) override {
        DWORD got = 0;
        if (!ReadFile(out_r_, buf, DWORD(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), &got, nullptr)) return 0;
        return got;
    }
    bool write(std::string_view data) override {
        std::lock_guard<std::mutex> lk(wmu_);
        while (!data.empty()) {
            if (stopped_ || !in_w_) return false;
            DWORD put = 0;
            const DWORD chunk = DWORD(data.size() > (1u << 20) ? (1u << 20) : data.size());
            if (!WriteFile(in_w_, data.data(), chunk, &put, nullptr) || put == 0) return false;
            data.remove_prefix(put);
        }
        return true;
    }
    // Ending the child closes its ends of the pipes, which fails any blocked
    // read or write here.
    void shutdown() override {
        if (stopped_.exchange(true)) return;
        if (WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT) TerminateProcess(proc_, 1);
    }
    std::string diagnostics() const override {
        std::lock_guard<std::mutex> lk(mu_);
        std::string s = stderr_;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }

private:
    void close_input() {
        std::lock_guard<std::mutex> lk(wmu_);
        if (in_w_) CloseHandle(in_w_);
        in_w_ = nullptr;
    }

    HANDLE proc_;
    HANDLE in_w_;
    HANDLE out_r_;
    HANDLE err_r_;
    std::thread err_thread_;
    mutable std::mutex mu_;
    std::mutex wmu_;
    std::string stderr_;
    std::atomic<bool> stopped_{false};
};

class WinProcess final : public Process {
public:
    explicit WinProcess(PROCESS_INFORMATION pi) : pi_(pi) { CloseHandle(pi_.hThread); }
    ~WinProcess() override {
        if (WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(pi_.hProcess, 137);
            WaitForSingleObject(pi_.hProcess, 2000);
        }
        CloseHandle(pi_.hProcess);
    }
    int64_t pid() const override { return int64_t(pi_.dwProcessId); }
    void kill() override { TerminateProcess(pi_.hProcess, 137); }
    bool wait_for(std::chrono::milliseconds timeout, int* exit_code) override {
        if (WaitForSingleObject(pi_.hProcess, DWORD(timeout.count())) != WAIT_OBJECT_0) return false;
        DWORD code = 0;
        GetExitCodeProcess(pi_.hProcess, &code);
        if (exit_code) *exit_code = int(code);
        return true;
    }

private:
    PROCESS_INFORMATION pi_;
};

bool make_pipe(HANDLE& read_end, HANDLE& write_end, bool child_reads) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    if (!CreatePipe(&read_end, &write_end, &sa, 1u << 16)) return false;
    // Only the child's end is inheritable.
    SetHandleInformation(child_reads ? write_end : read_end, HANDLE_FLAG_INHERIT, 0);
    return true;
}

}  // namespace

std::unique_ptr<Stream> spawn_stream(const std::vector<std::string>& argv, std::string* err) {
    HANDLE in_r, in_w, out_r, out_w, err_r, err_w;
    if (!make_pipe(in_r, in_w, true)) {
        if (err) *err = "CreatePipe: " + win::error_text(GetLastError());
        return nullptr;
    }
    if (!make_pipe(out_r, out_w, false)) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        if (err) *err = "CreatePipe: " + win::error_text(GetLastError());
        return nullptr;
    }
    if (!make_pipe(err_r, err_w, false)) {
        for (HANDLE h : {in_r, in_w, out_r, out_w}) CloseHandle(h);
        if (err) *err = "CreatePipe: " + win::error_text(GetLastError());
        return nullptr;
    }
    PROCESS_INFORMATION pi{};
    const bool ok = create_process(argv, CREATE_NO_WINDOW, {in_r, out_w, err_w}, in_r, out_w, err_w, pi, err);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return nullptr;
    }
    return std::make_unique<ChildStream>(pi, in_w, out_r, err_r);
}

std::unique_ptr<Process> Process::spawn(const std::vector<std::string>& argv, std::string* err) {
    std::vector<HANDLE> inherit;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE errh = GetStdHandle(STD_ERROR_HANDLE);
    // Inherit our std handles when they are real (and inheritable) handles.
    for (HANDLE* h : {&in, &out, &errh}) {
        if (*h && *h != INVALID_HANDLE_VALUE && SetHandleInformation(*h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            bool dup = false;
            for (HANDLE x : inherit) dup = dup || x == *h;
            if (!dup) inherit.push_back(*h);
        } else {
            *h = nullptr;
        }
    }
    PROCESS_INFORMATION pi{};
    if (inherit.empty()) {
        if (!create_process(argv, CREATE_NO_WINDOW, {}, nullptr, nullptr, nullptr, pi, err)) return nullptr;
    } else if (!create_process(argv, 0, inherit, in, out, errh, pi, err)) {
        return nullptr;
    }
    return std::make_unique<WinProcess>(pi);
}

}  // namespace broremote
