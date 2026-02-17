#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

struct SourceEntry {
    std::string url;
    bool enabled = true;
    int last_latency_ms = -1;
    bool last_ok = false;
};

struct HttpResult {
    long status = 0;
    int latency_ms = -1;
    std::string body;
    std::string err;
};

struct PhaseProgress {
    std::atomic<bool> running{ false };
    std::atomic<int> done{ 0 };
    std::atomic<int> total{ 0 };
};

struct RunState {
    std::atomic<bool> cancel{ false };
    std::unordered_set<std::string> existing_files;

    PhaseProgress indexing;
    PhaseProgress downloading;
    PhaseProgress decompressing;
    PhaseProgress deleting;

    std::atomic<int> last_remote_unique{ 0 };
    std::atomic<int> last_remote_after_filters{ 0 };
    std::atomic<int> last_already_have{ 0 };
    std::atomic<int> last_to_download{ 0 };
};

struct Settings {
    fs::path hl2mp_path;
    int threads = 4;

    bool decompress = false;
    bool delete_bz2 = false;

    int index_timeout_ms = 8000;
    int head_timeout_ms = 5000;
    int dl_timeout_ms = 30000;
    int retries = 3;

    std::string include_filters;
    std::string exclude_filters;
};

struct LiveLog {
    std::mutex mtx;
    std::vector<std::string> lines;
    std::vector<std::string> failures;

    void push( std::string s );
    void fail( std::string s );
};
