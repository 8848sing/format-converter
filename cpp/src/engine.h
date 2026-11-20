#pragma once

#include "common.h"
#include "registry.h"

#include <atomic>

namespace fc {

struct RunResult {
    int ok = 0;
    int failed = 0;
    bool cancelled = false;
};

class Engine {
public:
    Engine(fs::path base_dir, json settings);

    RunResult run(const Job& job, EventSink& sink, std::atomic<bool>* cancel = nullptr) const;

    const fs::path& base_dir() const { return base_dir_; }
    const json& settings() const { return settings_; }

private:
    fs::path base_dir_;
    json settings_;
};

}  // namespace fc