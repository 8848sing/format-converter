#include "process_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fc {

namespace {

constexpr size_t kMaxCapture = 4 * 1024 * 1024;

void append_capped(std::string& target, const char* data, size_t size) {
    if (target.size() >= kMaxCapture) {
        return;
    }
    size_t room = kMaxCapture - target.size();
    target.append(data, std::min(size, room));
}

#ifdef _WIN32

std::wstring quote_windows_arg(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    bool needs_quotes = false;
    for (wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return arg;
    }
    std::wstring out;
    out.push_back(L'"');
    int backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(static_cast<size_t>(backslashes * 2 + 1), L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(static_cast<size_t>(backslashes), L'\\');
            out.push_back(ch);
            backslashes = 0;
        }
    }
    out.append(static_cast<size_t>(backslashes * 2), L'\\');
    out.push_back(L'"');
    return out;
}

// 用 PeekNamedPipe 轮询而不是阻塞 ReadFile：LibreOffice 会拉起 soffice.bin，
// 孙进程继承了写端句柄，即使父进程已结束，阻塞的 ReadFile 也等不到 EOF。
// 轮询 + stop 标志保证超时/结束时读取线程一定能退出。
void read_pipe(HANDLE pipe, std::string& output, std::atomic<bool>* stop) {
    char buffer[65536];
    DWORD read = 0;
    while (true) {
        if (stop && stop->load()) {
            break;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            break;
        }
        if (available == 0) {
            if (stop && stop->load()) {
                break;
            }
            Sleep(10);
            continue;
        }
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        append_capped(output, buffer, read);
    }
}

#else

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

#endif

}  // namespace

#ifdef _WIN32
std::string windows_error_message(unsigned long code) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string message;
    if (length && buffer) {
        message = to_utf8(std::wstring(buffer, length));
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
            message.pop_back();
        }
    }
    if (buffer) {
        LocalFree(buffer);
    }
    if (message.empty()) {
        message = "Windows error " + std::to_string(code);
    }
    return message;
}
#endif

ProcessResult run_process(const std::vector<std::string>& args,
                          int timeout_ms,
                          const fs::path& cwd) {
    if (args.empty()) {
        throw Error("run_process: 参数为空");
    }
    ProcessResult result;

#ifdef _WIN32
    std::wstring command_line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) {
            command_line.push_back(L' ');
        }
        command_line += quote_windows_arg(to_wide(args[i]));
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &security, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &security, 0)) {
        throw Error("创建进程管道失败: " + windows_error_message(GetLastError()));
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    // 子进程的标准输入一律接 NUL：桌面端是通过管道把任务交给 fccli 的，管道一直
    // 开着不关，若被 ffmpeg 继承，它会一直等交互命令，转视频就整个卡死（CPU 0%、
    // 输出文件 0 字节）。接上 NUL 后子进程立刻读到 EOF，永不阻塞。
    HANDLE nul_input = CreateFileW(L"NUL", GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                   OPEN_EXISTING, 0, nullptr);
    startup.hStdInput =
        nul_input == INVALID_HANDLE_VALUE ? GetStdHandle(STD_INPUT_HANDLE) : nul_input;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;

    // 用作业对象托管子进程树：LibreOffice 的 soffice.exe 会再拉起 soffice.bin，
    // 只 TerminateProcess 直接子进程会留下孙进程。残留的 soffice.bin 会占住
    // 全局单实例锁，让之后所有 Office 转换一直挂到超时。
    // KILL_ON_JOB_CLOSE 保证本进程退出或超时时整棵树一起结束。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    std::wstring cwd_wide = cwd.empty() ? std::wstring() : cwd.wstring();

    DWORD create_flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (job) {
        create_flags |= CREATE_SUSPENDED;
    }
    BOOL created = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        create_flags,
        nullptr, cwd.empty() ? nullptr : cwd_wide.c_str(),
        &startup, &process);

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (nul_input != INVALID_HANDLE_VALUE) {
        CloseHandle(nul_input);
    }

    if (!created) {
        DWORD code = GetLastError();
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        if (job) {
            CloseHandle(job);
        }
        throw Error("无法启动进程 " + args[0] + ": " + windows_error_message(code));
    }

    if (job) {
        if (AssignProcessToJobObject(job, process.hProcess)) {
            ResumeThread(process.hThread);
        } else {
            // 极少数环境不允许嵌套作业对象，退化为只管理直接子进程。
            ResumeThread(process.hThread);
            CloseHandle(job);
            job = nullptr;
        }
    }

    result.launched = true;
    std::atomic<bool> stop_reading{false};
    std::thread out_thread(read_pipe, stdout_read, std::ref(result.out), &stop_reading);
    std::thread err_thread(read_pipe, stderr_read, std::ref(result.err), &stop_reading);

    DWORD wait_result = WaitForSingleObject(process.hProcess,
                                            timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE);
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        if (job) {
            TerminateJobObject(job, 1);
        } else {
            TerminateProcess(process.hProcess, 1);
        }
        WaitForSingleObject(process.hProcess, 5000);
        // 孙进程可能还占着管道写端，通知读取线程退出，避免卡在等待 EOF。
        stop_reading.store(true);
    }

    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        exit_code = 1;
    }
    result.exit_code = static_cast<int>(exit_code);

    out_thread.join();
    err_thread.join();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    if (job) {
        // 子进程已经结束；关闭作业对象会顺带清掉任何残留的孙进程。
        CloseHandle(job);
    }

#else
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        throw Error("创建进程管道失败");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        throw Error("fork 失败");
    }
    if (pid == 0) {
        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) {
                _exit(126);
            }
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        dup2(out_pipe[1], STDIN_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);
    result.launched = true;

    auto start = std::chrono::steady_clock::now();
    bool child_exited = false;
    int status = 0;
    bool out_open = true;
    bool err_open = true;
    char buffer[65536];

    while (out_open || err_open || !child_exited) {
        struct pollfd fds[2];
        int count = 0;
        if (out_open) {
            fds[count++] = {out_pipe[0], POLLIN, 0};
        }
        if (err_open) {
            fds[count++] = {err_pipe[0], POLLIN, 0};
        }
        if (count > 0) {
            poll(fds, static_cast<nfds_t>(count), 100);
            for (int i = 0; i < count; ++i) {
                if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    int fd = fds[i].fd;
                    ssize_t read = ::read(fd, buffer, sizeof(buffer));
                    if (read > 0) {
                        if (fd == out_pipe[0]) {
                            append_capped(result.out, buffer, static_cast<size_t>(read));
                        } else {
                            append_capped(result.err, buffer, static_cast<size_t>(read));
                        }
                    } else if (read == 0 || (read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        if (fd == out_pipe[0]) {
                            close(out_pipe[0]);
                            out_open = false;
                        } else {
                            close(err_pipe[0]);
                            err_open = false;
                        }
                    }
                }
            }
        }
        if (!child_exited) {
            pid_t wait = waitpid(pid, &status, WNOHANG);
            if (wait == pid) {
                child_exited = true;
            } else if (timeout_ms > 0 &&
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count() > timeout_ms) {
                result.timed_out = true;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                child_exited = true;
            }
        }
        if (count == 0 && child_exited) {
            break;
        }
    }

    if (!child_exited) {
        waitpid(pid, &status, 0);
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = 1;
    }
#endif

    return result;
}

}  // namespace fc