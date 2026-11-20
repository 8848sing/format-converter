#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace fc {

struct ProcessResult {
    bool launched = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string out;
    std::string err;
};

/*
 * 以参数数组运行外部程序，不经过 shell。
 * timeout_ms <= 0 表示不限时；超时会强制结束进程。
 */
ProcessResult run_process(const std::vector<std::string>& args,
                          int timeout_ms = 0,
                          const fs::path& cwd = fs::path());

#ifdef _WIN32
std::string windows_error_message(unsigned long code);
#endif

}  // namespace fc