#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static std::string readFileText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("Failed to open: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void writeFileText(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Failed to write: " + p.string());
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
    if (!out) throw std::runtime_error("Failed while writing: " + p.string());
}

static std::optional<fs::file_time_type> mtimeIfExists(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) return std::nullopt;
    auto t = fs::last_write_time(p, ec);
    if (ec) return std::nullopt;
    return t;
}

static void safeRemove(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

static void safeRename(const fs::path& from, const fs::path& to) {
    // Cross-platform atomic-ish rename semantics:
    // - POSIX: rename over existing is atomic.
    // - Windows: rename fails if destination exists.
    std::error_code ec;

    // Remove destination first (Windows friendliness)
    fs::remove(to, ec); // ignore errors

    fs::rename(from, to, ec);
    if (ec) {
        // If rename failed, try a copy+remove fallback (less ideal, but robust).
        ec.clear();
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("Failed to move " + from.string() + " -> " + to.string() +
                                         " (rename/copy failed)");
        fs::remove(from, ec); // ignore
    }
}

struct MailboxPaths {
    fs::path dir;
    fs::path cmd_new, cmd_txt;
    fs::path out_txt, rc_txt;
};

static MailboxPaths pathsFromDir(const fs::path& dir) {
    MailboxPaths m;
    m.dir = dir;
    m.cmd_new = dir / "CMD.NEW";
    m.cmd_txt = dir / "CMD.TXT";
    m.out_txt = dir / "OUT.TXT";
    m.rc_txt  = dir / "RC.TXT";
    return m;
}

struct Reply {
    std::string out;
    std::optional<int> rc;
};

static std::optional<int> parseReturnCode(const std::string& s) {
    // RC.TXT is expected to contain a number, possibly with whitespace/newlines.
    std::string t;
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') t.push_back(' ');
        else t.push_back(c);
    }
    std::istringstream iss(t);
    int v;
    if (iss >> v) return v;
    return std::nullopt;
}

static Reply sendCommandAndWait(const MailboxPaths& m,
                                const std::string& command,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                                std::chrono::milliseconds poll = std::chrono::milliseconds(50)) {
    // Snapshot mtimes so we can detect "new" output.
    auto out_before = mtimeIfExists(m.out_txt);
    auto rc_before  = mtimeIfExists(m.rc_txt);

    // Clean stale CMD files (host-side). Be conservative: remove only CMD.NEW.
    safeRemove(m.cmd_new);

    // Write CMD.NEW then rename to CMD.TXT
    writeFileText(m.cmd_new, command + "\r\n");
    safeRename(m.cmd_new, m.cmd_txt);

    const auto start = std::chrono::steady_clock::now();

    // Wait for OUT.TXT (and optionally RC.TXT) to update
    while (true) {
        auto out_now = mtimeIfExists(m.out_txt);
        auto rc_now  = mtimeIfExists(m.rc_txt);

        bool out_updated = false;
        if (out_now) {
            if (!out_before) out_updated = true;
            else out_updated = (*out_now != *out_before);
        }

        bool rc_updated = false;
        if (rc_now) {
            if (!rc_before) rc_updated = true;
            else rc_updated = (*rc_now != *rc_before);
        }

        // Treat OUT update as the primary signal; RC is a nice-to-have.
        if (out_updated) {
            Reply r;
            r.out = readFileText(m.out_txt);

            if (rc_updated) {
                auto rc_text = readFileText(m.rc_txt);
                r.rc = parseReturnCode(rc_text);
            } else {
                // If RC wasn't updated yet, give it a brief grace period.
                // This helps when filesystem timestamps are coarse.
                auto grace_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                while (std::chrono::steady_clock::now() < grace_end) {
                    auto rc2 = mtimeIfExists(m.rc_txt);
                    bool rc2_updated = false;
                    if (rc2) {
                        if (!rc_before) rc2_updated = true;
                        else rc2_updated = (*rc2 != *rc_before);
                    }
                    if (rc2_updated) {
                        auto rc_text = readFileText(m.rc_txt);
                        r.rc = parseReturnCode(rc_text);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }

            return r;
        }

        if (std::chrono::steady_clock::now() - start > timeout) {
            throw std::runtime_error("Timeout waiting for OUT.TXT. Is MBXSRV running in the shared folder?");
        }

        std::this_thread::sleep_for(poll);
    }
}

static void usage() {
    std::cerr <<
        "Usage:\n"
        "  mbxhost <shared_folder_path>            # REPL mode\n"
        "  mbxhost <shared_folder_path> --cmd \"dir\" [--timeout ms]\n"
        "\n"
        "Examples:\n"
        "  mbxhost ./shared\n"
        "  mbxhost ./shared --cmd \"ver\" --timeout 8000\n";
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage();
            return 2;
        }

        fs::path dir = fs::path(argv[1]);
        if (!fs::exists(dir)) {
            std::cerr << "Shared folder does not exist: " << dir.string() << "\n";
            return 2;
        }

        auto m = pathsFromDir(dir);

        std::optional<std::string> oneShotCmd;
        std::chrono::milliseconds timeout(5000);

        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--cmd" && i + 1 < argc) {
                oneShotCmd = argv[++i];
            } else if (a == "--timeout" && i + 1 < argc) {
                timeout = std::chrono::milliseconds(std::stoi(argv[++i]));
            } else if (a == "--help" || a == "-h") {
                usage();
                return 0;
            } else {
                std::cerr << "Unknown arg: " << a << "\n";
                usage();
                return 2;
            }
        }

        if (oneShotCmd) {
            auto r = sendCommandAndWait(m, *oneShotCmd, timeout);
            std::cout << r.out;
            if (r.rc) std::cout << "\n[RC] " << *r.rc << "\n";
            return r.rc.value_or(0);
        }

        // REPL mode
        std::cout << "mbxhost REPL. Shared folder: " << dir.string() << "\n"
                  << "Type DOS commands. Use 'exit' to quit. (Sends EXIT to guest with 'quit-guest')\n";

        std::string line;
        while (true) {
            std::cout << "dos> " << std::flush;
            if (!std::getline(std::cin, line)) break;

            // local exit
            if (line == "exit") break;

            // send EXIT to guest and quit
            if (line == "quit-guest") {
                auto r = sendCommandAndWait(m, "EXIT", timeout);
                std::cout << r.out;
                break;
            }

            if (line.empty()) continue;

            auto r = sendCommandAndWait(m, line, timeout);
            std::cout << r.out;
            if (r.rc) std::cout << "[RC] " << *r.rc << "\n";
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "mbxhost error: " << e.what() << "\n";
        return 1;
    }
}
