// Replays a NASDAQ ITCH 5.0 file through the book and reports how long it took.
//
//   itch_replay data/12302019.NASDAQ_ITCH50
//   itch_replay data/12302019.NASDAQ_ITCH50 --parse-only
//   itch_replay data/12302019.NASDAQ_ITCH50 --symbol AAPL --at 10:30:00

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lob/itch.hpp"
#include "lob/market.hpp"

using namespace lob;

namespace {

std::string commas(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

std::string dollars(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
    return buf;
}

struct Counts {
    std::uint64_t adds = 0, executions = 0, priced_executions = 0, cancels = 0, deletes = 0, replaces = 0;
    std::uint64_t hidden_trades = 0, other = 0;

    std::uint64_t total() const {
        return adds + executions + priced_executions + cancels + deletes + replaces + hidden_trades + other;
    }
};

// Counts messages and does nothing else. Timing this on its own shows how much
// of the replay is parsing and how much is the book.
struct Counter : itch::Handler {
    Counts n;
    void on_add(const itch::AddOrder&) { ++n.adds; }
    void on_executed(const itch::OrderExecuted&) { ++n.executions; }
    void on_executed_with_price(const itch::OrderExecutedWithPrice&) { ++n.priced_executions; }
    void on_cancel(const itch::OrderCancel&) { ++n.cancels; }
    void on_delete(const itch::OrderDelete&) { ++n.deletes; }
    void on_replace(const itch::OrderReplace&) { ++n.replaces; }
    void on_trade(const itch::Trade&) { ++n.hidden_trades; }
    void on_system_event(const itch::SystemEvent&) { ++n.other; }
    void on_stock_directory(const itch::StockDirectory&) { ++n.other; }
    void on_other(char) { ++n.other; }
};

struct Snapshot {
    std::string symbol;
    std::uint64_t at = 0;  // ns since midnight
    bool taken = false;
};

class BookBuilder : public itch::Handler {
public:
    explicit BookBuilder(Snapshot snap) : market_(1 << 16), snap_(std::move(snap)) {}

    Counts n;
    std::uint64_t instruments = 0;
    std::uint64_t unknown_refs = 0;
    std::uint64_t peak_live = 0;
    // Of the executions at an order's own price, how many hit the order that
    // was first in line at the best price. The exchange matches in price-time
    // priority, so anything short of all of them means a queue is wrong.
    std::uint64_t in_priority = 0;

    void on_stock_directory(const itch::StockDirectory& m) {
        ++n.other;
        ++instruments;
        std::string_view s = m.symbol;
        if (s.substr(0, s.find(' ')) == snap_.symbol) snap_locate_ = m.locate;
    }

    void on_add(const itch::AddOrder& m) {
        ++n.adds;
        maybe_snapshot(m.timestamp);
        if (!market_.add(m.locate, m.ref, m.side, m.price, m.shares)) ++unknown_refs;
        peak_live = std::max<std::uint64_t>(peak_live, market_.live_orders());
    }

    void on_executed(const itch::OrderExecuted& m) {
        ++n.executions;
        maybe_snapshot(m.timestamp);
        if (market_.is_next_to_trade(m.ref)) ++in_priority;
        if (!market_.reduce(m.ref, m.shares)) ++unknown_refs;
    }

    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        ++n.priced_executions;
        if (!market_.reduce(m.ref, m.shares)) ++unknown_refs;
    }

    void on_cancel(const itch::OrderCancel& m) {
        ++n.cancels;
        if (!market_.reduce(m.ref, m.shares)) ++unknown_refs;
    }

    void on_delete(const itch::OrderDelete& m) {
        ++n.deletes;
        if (!market_.remove(m.ref)) ++unknown_refs;
    }

    void on_replace(const itch::OrderReplace& m) {
        ++n.replaces;
        maybe_snapshot(m.timestamp);
        if (!market_.replace(m.old_ref, m.new_ref, m.price, m.shares)) ++unknown_refs;
    }

    void on_trade(const itch::Trade&) { ++n.hidden_trades; }
    void on_system_event(const itch::SystemEvent&) { ++n.other; }
    void on_other(char) { ++n.other; }

    void on_upcoming(OrderId ref) { market_.prefetch(ref); }

    std::size_t live_orders() const { return market_.live_orders(); }

private:
    void maybe_snapshot(std::uint64_t now) {
        if (snap_.taken || snap_locate_ < 0 || now < snap_.at) return;
        snap_.taken = true;
        const auto book = static_cast<std::uint16_t>(snap_locate_);
        const auto bids = market_.top(book, Side::Buy, 5);
        const auto asks = market_.top(book, Side::Sell, 5);
        std::printf("\n%s, first update after the requested time\n", snap_.symbol.c_str());
        std::printf("  %10s %12s   %-12s %-10s\n", "bid size", "bid", "ask", "ask size");
        for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
            const std::string bq = i < bids.size() ? commas(bids[i].qty) : "";
            const std::string bp = i < bids.size() ? dollars(bids[i].price) : "";
            const std::string ap = i < asks.size() ? dollars(asks[i].price) : "";
            const std::string aq = i < asks.size() ? commas(asks[i].qty) : "";
            std::printf("  %10s %12s   %-12s %-10s\n", bq.c_str(), bp.c_str(), ap.c_str(), aq.c_str());
        }
        std::printf("\n");
    }

    Market market_;
    Snapshot snap_;
    int snap_locate_ = -1;
};

// "10:30:00" -> nanoseconds since midnight
std::uint64_t parse_time(const char* s) {
    unsigned h = 0, m = 0, sec = 0;
    if (std::sscanf(s, "%u:%u:%u", &h, &m, &sec) < 2) {
        std::fprintf(stderr, "bad time '%s', expected HH:MM[:SS]\n", s);
        std::exit(2);
    }
    return (std::uint64_t{h} * 3600 + m * 60 + sec) * 1'000'000'000ull;
}

void print_counts(const Counts& n) {
    std::printf("  %-22s %15s\n", "add", commas(n.adds).c_str());
    std::printf("  %-22s %15s\n", "executed", commas(n.executions).c_str());
    std::printf("  %-22s %15s\n", "executed with price", commas(n.priced_executions).c_str());
    std::printf("  %-22s %15s\n", "cancel", commas(n.cancels).c_str());
    std::printf("  %-22s %15s\n", "delete", commas(n.deletes).c_str());
    std::printf("  %-22s %15s\n", "replace", commas(n.replaces).c_str());
    std::printf("  %-22s %15s\n", "hidden trade", commas(n.hidden_trades).c_str());
    std::printf("  %-22s %15s\n", "everything else", commas(n.other).c_str());
}

// How many messages ahead to prefetch order ids. Anything from 2 to 32 measured
// the same within noise on my machine, so the exact value is not important.
constexpr std::size_t kLookahead = 8;

// User-mode CPU time this process has used. The file is memory-mapped, so the
// wall clock includes however long the disk takes to page 8 GB in, and on a
// machine that cannot keep the whole file cached that is most of it. CPU time
// is what the parser and the book actually cost.
double cpu_seconds() {
    rusage usage {};
    ::getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec) / 1e6;
}

struct Elapsed {
    double cpu;
    double wall;
};

template <class H>
Elapsed timed_parse(std::span<const std::byte> data, H& handler) {
    const auto wall_start = std::chrono::steady_clock::now();
    const double cpu_start = cpu_seconds();
    const std::size_t used = itch::parse(data, handler, kLookahead);
    const double cpu = cpu_seconds() - cpu_start;
    const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - wall_start;
    if (used != data.size()) {
        std::fprintf(stderr, "warning: stopped %zu bytes before the end (truncated file?)\n", data.size() - used);
    }
    return {cpu, wall.count()};
}

void print_speed(std::uint64_t messages, Elapsed t) {
    std::printf("%s messages: %.2f s cpu (%.2f s wall), %.1f M msg/s, %.0f ns/msg\n", commas(messages).c_str(),
                t.cpu, t.wall, static_cast<double>(messages) / t.cpu / 1e6,
                t.cpu * 1e9 / static_cast<double>(messages));
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    bool parse_only = false;
    Snapshot snap;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--parse-only") {
            parse_only = true;
        } else if (arg == "--symbol" && i + 1 < argc) {
            snap.symbol = argv[++i];
        } else if (arg == "--at" && i + 1 < argc) {
            snap.at = parse_time(argv[++i]);
        } else if (!path) {
            path = argv[i];
        } else {
            path = nullptr;
            break;
        }
    }
    if (!path) {
        std::fprintf(stderr, "usage: %s <itch file> [--parse-only] [--symbol SYM --at HH:MM:SS]\n", argv[0]);
        return 2;
    }

    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        std::perror(path);
        return 1;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        std::perror("fstat");
        return 1;
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    const std::span<const std::byte> data(static_cast<const std::byte*>(map), size);

    std::printf("%s (%.2f GB)\n", path, static_cast<double>(size) / 1e9);

    if (parse_only) {
        Counter counter;
        const Elapsed elapsed = timed_parse(data, counter);
        print_counts(counter.n);
        print_speed(counter.n.total(), elapsed);
    } else {
        BookBuilder builder(snap);
        const Elapsed elapsed = timed_parse(data, builder);
        print_counts(builder.n);
        print_speed(builder.n.total(), elapsed);
        std::printf("instruments: %s\n", commas(builder.instruments).c_str());
        std::printf("orders resting at the end: %s (peak %s)\n", commas(builder.live_orders()).c_str(),
                    commas(builder.peak_live).c_str());
        std::printf("messages the book could not apply: %s\n", commas(builder.unknown_refs).c_str());
        if (builder.n.executions > 0) {
            std::printf("executions that hit the front of the best level: %s of %s (%.3f%%)\n",
                        commas(builder.in_priority).c_str(), commas(builder.n.executions).c_str(),
                        100.0 * static_cast<double>(builder.in_priority) / static_cast<double>(builder.n.executions));
        }
    }

    ::munmap(map, size);
    ::close(fd);
    return 0;
}
