// SSH plumbing: one multiplexed ControlMaster, many channels.
//
// A single TCP+SSH handshake is paid once; the audio channel and the ALSA
// telemetry channel then ride it as separate channels. This keeps the DAC
// connection independent of how often we poll the Pi.

#pragma once

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

extern char** environ;

namespace pib {

struct Child {
    pid_t pid = -1;
    int in = -1;    // we write here
    int out = -1;   // we read here
    int err = -1;   // we read here

    bool running() const { return pid > 0; }

    void closeIn() {
        if (in >= 0) { ::close(in); in = -1; }
    }

    void reap() {
        if (pid > 0) {
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (in >= 0) { ::close(in); in = -1; }
        if (out >= 0) { ::close(out); out = -1; }
        if (err >= 0) { ::close(err); err = -1; }
    }
};

inline std::vector<std::string> sshBase(const std::string& sock, const std::string& key,
                                        const std::string& user, const std::string& host) {
    std::vector<std::string> a;
    a.push_back("/usr/bin/ssh");
    a.push_back("-S"); a.push_back(sock);
    if (!key.empty()) { a.push_back("-i"); a.push_back(key); }
    a.push_back("-o"); a.push_back("BatchMode=yes");
    a.push_back("-o"); a.push_back("StrictHostKeyChecking=accept-new");
    a.push_back("-o"); a.push_back("ConnectTimeout=8");
    a.push_back("-o"); a.push_back("ServerAliveInterval=15");
    a.push_back("-o"); a.push_back("TCPKeepAlive=yes");
    a.push_back("-o"); a.push_back("Compression=no");
    a.push_back(user + "@" + host);
    return a;
}

// Opens the master if it is not already usable. Returns false with a reason.
inline bool ensureMaster(const std::string& sock, const std::string& key, const std::string& user,
                         const std::string& host, std::string& err) {
    auto base = sshBase(sock, key, user, host);
    // -O check exits 0 when a live master is listening on the control socket.
    {
        std::vector<char*> argv;
        std::vector<std::string> check = base;
        check.insert(check.begin() + 1, "-O");
        check.insert(check.begin() + 2, "check");
        for (auto& s : check) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        pid_t pid = ::fork();
        if (pid == 0) {
            int devnull = ::open("/dev/null", O_RDWR);
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    }

    std::vector<std::string> cmd = base;
    cmd.insert(cmd.begin() + 1, "-M");
    cmd.insert(cmd.begin() + 2, "-fN");
    std::vector<char*> argv;
    for (auto& s : cmd) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        ::dup2(devnull, 0);
        ::dup2(devnull, 1);
        ::dup2(devnull, 2);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        err = "ssh master to " + user + "@" + host + " failed (is the Pi up, and is the key installed?)";
        return false;
    }
    return true;
}

// Runs `remote` on the far side over the existing master.
inline bool spawnRemote(const std::string& sock, const std::string& key, const std::string& user,
                        const std::string& host, const std::string& remote, bool wantIn, bool wantOut,
                        bool wantErr, Child& child, std::string& err) {
    std::vector<std::string> cmd = sshBase(sock, key, user, host);
    if (!wantIn) cmd.insert(cmd.begin() + 1, "-T");
    cmd.push_back(remote);

    int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
    if (wantIn && ::pipe(inPipe) != 0) { err = "pipe()"; return false; }
    if (wantOut && ::pipe(outPipe) != 0) { err = "pipe()"; return false; }
    if (wantErr && ::pipe(errPipe) != 0) { err = "pipe()"; return false; }

    std::vector<char*> argv;
    for (auto& s : cmd) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (wantIn) {
        posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
        posix_spawn_file_actions_addclose(&fa, inPipe[1]);
    } else {
        int devnull = ::open("/dev/null", O_RDONLY);
        posix_spawn_file_actions_adddup2(&fa, devnull, 0);
        posix_spawn_file_actions_addclose(&fa, devnull);
    }
    if (wantOut) {
        posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
        posix_spawn_file_actions_addclose(&fa, outPipe[0]);
    } else {
        int devnull = ::open("/dev/null", O_WRONLY);
        posix_spawn_file_actions_adddup2(&fa, devnull, 1);
        posix_spawn_file_actions_addclose(&fa, devnull);
    }
    if (wantErr) {
        posix_spawn_file_actions_adddup2(&fa, errPipe[1], 2);
        posix_spawn_file_actions_addclose(&fa, errPipe[0]);
    } else {
        int devnull = ::open("/dev/null", O_WRONLY);
        posix_spawn_file_actions_adddup2(&fa, devnull, 2);
        posix_spawn_file_actions_addclose(&fa, devnull);
    }

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);

    if (wantIn) { ::close(inPipe[0]); child.in = inPipe[1]; }
    if (wantOut) { ::close(outPipe[1]); child.out = outPipe[0]; }
    if (wantErr) { ::close(errPipe[1]); child.err = errPipe[0]; }
    if (rc != 0) {
        err = std::string("posix_spawn: ") + std::strerror(rc);
        return false;
    }
    child.pid = pid;
    if (child.err >= 0) ::fcntl(child.err, F_SETFL, O_NONBLOCK);
    return true;
}

inline void closeMaster(const std::string& sock, const std::string& key, const std::string& user,
                        const std::string& host) {
    auto base = sshBase(sock, key, user, host);
    base.insert(base.begin() + 1, "-O");
    base.insert(base.begin() + 2, "exit");
    std::vector<char*> argv;
    for (auto& s : base) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        ::dup2(devnull, 0);
        ::dup2(devnull, 1);
        ::dup2(devnull, 2);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
}

}  // namespace pib
