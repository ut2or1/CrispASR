#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace crispasr_cli_process {

struct RunResult {
    std::string output;
    int exit_code = -1;
    bool timed_out = false;
    bool spawn_failed = false;
};

#ifdef _WIN32
inline std::string quote_arg(const std::string& arg) {
    if (arg.empty())
        return "\"\"";
    if (arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;

    std::string out = "\"";
    size_t bs = 0;
    for (char c : arg) {
        if (c == '\\') {
            bs++;
        } else if (c == '"') {
            out.append(bs * 2 + 1, '\\');
            out.push_back('"');
            bs = 0;
        } else {
            out.append(bs, '\\');
            bs = 0;
            out.push_back(c);
        }
    }
    out.append(bs * 2, '\\');
    out.push_back('"');
    return out;
}
#else
inline std::string quote_arg(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}
#endif

inline std::string join_cmdline(const std::vector<std::string>& args) {
    std::string cmd;
    for (const auto& arg : args) {
        if (!cmd.empty())
            cmd.push_back(' ');
        cmd += quote_arg(arg);
    }
    return cmd;
}

inline int timeout_from_audio_samples(const char* env_name, int n_samples, int min_sec = 120, int per_audio_sec = 5) {
    const char* env = std::getenv(env_name);
    if (env && *env) {
        const int v = std::atoi(env);
        if (v > 0)
            return v;
    }
    const int audio_s = std::max(1, (int)std::ceil((double)n_samples / 16000.0));
    return std::max(min_sec, audio_s * per_audio_sec);
}

#ifdef _WIN32
inline RunResult run_capture_stdout(const std::vector<std::string>& args, int timeout_sec,
                                    bool capture_stderr = false) {
    RunResult result;
    if (args.empty()) {
        result.spawn_failed = true;
        return result;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_rd = NULL;
    HANDLE out_wr = NULL;
    if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
        result.spawn_failed = true;
        return result;
    }
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_wr;
    si.hStdError = capture_stderr ? out_wr : ((nul != INVALID_HANDLE_VALUE) ? nul : out_wr);

    PROCESS_INFORMATION pi{};
    std::string cmdline = join_cmdline(args);
    std::vector<char> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back('\0');

    BOOL ok = CreateProcessA(NULL, mutable_cmd.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(out_wr);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);

    if (!ok) {
        CloseHandle(out_rd);
        result.spawn_failed = true;
        return result;
    }

    std::thread reader([&]() {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(out_rd, buf, sizeof(buf), &n, NULL) && n > 0)
            result.output.append(buf, buf + n);
    });

    const DWORD wait_ms = timeout_sec > 0 ? (DWORD)timeout_sec * 1000u : INFINITE;
    DWORD wait_rc = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code))
        result.exit_code = (int)exit_code;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (reader.joinable())
        reader.join();
    CloseHandle(out_rd);
    return result;
}
#else
inline RunResult run_capture_stdout(const std::vector<std::string>& args, int /*timeout_sec*/,
                                    bool capture_stderr = false) {
    RunResult result;
    if (args.empty()) {
        result.spawn_failed = true;
        return result;
    }

    std::string cmd = join_cmdline(args) + (capture_stderr ? " 2>&1" : " 2>/dev/null");
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.spawn_failed = true;
        return result;
    }

    char linebuf[1024];
    while (fgets(linebuf, sizeof(linebuf), pipe))
        result.output += linebuf;
    result.exit_code = pclose(pipe);
    return result;
}
#endif

} // namespace crispasr_cli_process
