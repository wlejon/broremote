// POSIX process spawning: piped children (ssh, the proxy) and plain processes.
#include "broremote/stream.h"
#include "posix_util.h"

#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

extern char** environ;

namespace broremote {

namespace {

bool make_pipe(int fds[2]) {
    if (::pipe(fds) != 0) return false;
    posix::set_cloexec(fds[0]);
    posix::set_cloexec(fds[1]);
    return true;
}

// posix_spawnp argv with fds 0/1/2 as given (-1: inherit). Returns the pid or -1.
pid_t spawn_impl(const std::vector<std::string>& argv, int in, int out, int errfd, std::string* err) {
    if (argv.empty()) {
        if (err) *err = "empty command";
        return -1;
    }
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in >= 0) posix_spawn_file_actions_adddup2(&fa, in, 0);
    if (out >= 0) posix_spawn_file_actions_adddup2(&fa, out, 1);
    if (errfd >= 0) posix_spawn_file_actions_adddup2(&fa, errfd, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int s : {SIGPIPE, SIGCHLD, SIGINT, SIGTERM, SIGHUP}) sigaddset(&defaults, s);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = -1;
    int r = posix_spawnp(&pid, args[0], &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (r != 0) {
        if (err) *err = "cannot start " + argv[0] + ": " + posix::errno_text(r);
        return -1;
    }
    return pid;
}

bool wait_pid(pid_t pid, std::chrono::milliseconds timeout, int* code) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    auto delay = std::chrono::milliseconds(1);
    for (;;) {
        int status = 0;
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (code) *code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            return true;
        }
        if (r < 0 && errno != EINTR) return true;  // already reaped elsewhere
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds(20));
    }
}

class ChildStream final : public Stream {
public:
    ChildStream(pid_t pid, int in_w, int out_r, int err_r) : pid_(pid), in_w_(in_w), out_r_(out_r), err_r_(err_r) {
        err_thread_ = std::thread([this] {
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(err_r_, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                std::lock_guard<std::mutex> lk(mu_);
                stderr_.append(buf, size_t(n));
                if (stderr_.size() > 8192) stderr_.erase(0, stderr_.size() - 8192);
            }
        });
    }
    ~ChildStream() override {
        // Closing its stdin lets a well-behaved child (the proxy) exit on its
        // own; one that does not is terminated.
        close_input();
        if (!wait_pid(pid_, std::chrono::milliseconds(2000), nullptr)) {
            ::kill(pid_, SIGKILL);
            wait_pid(pid_, std::chrono::milliseconds(2000), nullptr);
        }
        if (err_thread_.joinable()) err_thread_.join();
        ::close(out_r_);
        ::close(err_r_);
    }
    size_t read(char* buf, size_t n) override {
        for (;;) {
            ssize_t r = ::read(out_r_, buf, n);
            if (r > 0) return size_t(r);
            if (r < 0 && errno == EINTR) continue;
            return 0;
        }
    }
    bool write(std::string_view data) override {
        std::lock_guard<std::mutex> lk(wmu_);
        while (!data.empty()) {
            if (in_w_ < 0) return false;
            ssize_t r = posix::write_pipe(in_w_, data.data(), data.size());
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            data.remove_prefix(size_t(r));
        }
        return true;
    }
    // Ending the child closes its pipe ends, which unblocks our read.
    void shutdown() override {
        if (stopped_.exchange(true)) return;
        ::kill(pid_, SIGTERM);
        close_input();
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
        if (in_w_ >= 0) ::close(in_w_);
        in_w_ = -1;
    }

    pid_t pid_;
    int in_w_;
    int out_r_;
    int err_r_;
    std::thread err_thread_;
    mutable std::mutex mu_;
    std::mutex wmu_;
    std::string stderr_;
    std::atomic<bool> stopped_{false};
};

class PosixProcess final : public Process {
public:
    explicit PosixProcess(pid_t pid) : pid_(pid) {}
    ~PosixProcess() override {
        if (!reaped_) {
            ::kill(pid_, SIGKILL);
            wait_pid(pid_, std::chrono::milliseconds(2000), nullptr);
        }
    }
    int64_t pid() const override { return pid_; }
    void kill() override {
        if (!reaped_) ::kill(pid_, SIGKILL);
    }
    bool wait_for(std::chrono::milliseconds timeout, int* exit_code) override {
        if (!reaped_) {
            if (!wait_pid(pid_, timeout, &code_)) return false;
            reaped_ = true;
        }
        if (exit_code) *exit_code = code_;
        return true;
    }

private:
    pid_t pid_;
    bool reaped_{false};
    int code_{-1};
};

}  // namespace

std::unique_ptr<Stream> spawn_stream(const std::vector<std::string>& argv, std::string* err) {
    int in[2], out[2], er[2];
    if (!make_pipe(in)) {
        if (err) *err = "pipe: " + posix::errno_text(errno);
        return nullptr;
    }
    if (!make_pipe(out)) {
        ::close(in[0]);
        ::close(in[1]);
        if (err) *err = "pipe: " + posix::errno_text(errno);
        return nullptr;
    }
    if (!make_pipe(er)) {
        for (int fd : {in[0], in[1], out[0], out[1]}) ::close(fd);
        if (err) *err = "pipe: " + posix::errno_text(errno);
        return nullptr;
    }
    pid_t pid = spawn_impl(argv, in[0], out[1], er[1], err);
    ::close(in[0]);
    ::close(out[1]);
    ::close(er[1]);
    if (pid < 0) {
        ::close(in[1]);
        ::close(out[0]);
        ::close(er[0]);
        return nullptr;
    }
    return std::make_unique<ChildStream>(pid, in[1], out[0], er[0]);
}

std::unique_ptr<Process> Process::spawn(const std::vector<std::string>& argv, std::string* err) {
    pid_t pid = spawn_impl(argv, -1, -1, -1, err);
    if (pid < 0) return nullptr;
    return std::make_unique<PosixProcess>(pid);
}

}  // namespace broremote
