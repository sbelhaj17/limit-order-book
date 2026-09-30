#include "lob/itch.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace lob;

namespace {

// Builds messages byte by byte the way the spec lays them out, so the offsets
// in the parser are checked against something written independently.
class Message {
public:
    explicit Message(char type, std::uint16_t locate = 0, std::uint64_t timestamp = 0) {
        u8(static_cast<std::uint8_t>(type));
        u16(locate);
        u16(0);  // tracking number
        u48(timestamp);
    }

    Message& u8(std::uint8_t v) { bytes_.push_back(std::byte{v}); return *this; }
    Message& ch(char c) { return u8(static_cast<std::uint8_t>(c)); }
    Message& u16(std::uint16_t v) { return be(v, 2); }
    Message& u32(std::uint32_t v) { return be(v, 4); }
    Message& u48(std::uint64_t v) { return be(v, 6); }
    Message& u64(std::uint64_t v) { return be(v, 8); }
    Message& str(std::string s, std::size_t width) {
        s.resize(width, ' ');
        for (char c : s) ch(c);
        return *this;
    }

    // appends this message, length prefix included, to a stream
    void append_to(std::vector<std::byte>& out) const {
        const auto len = static_cast<std::uint16_t>(bytes_.size());
        out.push_back(std::byte(len >> 8));
        out.push_back(std::byte(len & 0xff));
        out.insert(out.end(), bytes_.begin(), bytes_.end());
    }

    std::size_t size() const { return bytes_.size(); }

private:
    Message& be(std::uint64_t v, int width) {
        for (int shift = 8 * (width - 1); shift >= 0; shift -= 8) u8(static_cast<std::uint8_t>(v >> shift));
        return *this;
    }

    std::vector<std::byte> bytes_;
};

struct Recorder : itch::Handler {
    std::vector<itch::AddOrder> adds;
    std::vector<itch::OrderExecuted> executions;
    std::vector<itch::OrderExecutedWithPrice> priced_executions;
    std::vector<itch::OrderCancel> cancels;
    std::vector<itch::OrderDelete> deletes;
    std::vector<itch::OrderReplace> replaces;
    std::vector<itch::Trade> trades;
    std::vector<itch::SystemEvent> events;
    std::vector<std::pair<std::uint16_t, std::string>> symbols;
    std::string other;

    void on_add(const itch::AddOrder& m) { adds.push_back(m); }
    void on_executed(const itch::OrderExecuted& m) { executions.push_back(m); }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) { priced_executions.push_back(m); }
    void on_cancel(const itch::OrderCancel& m) { cancels.push_back(m); }
    void on_delete(const itch::OrderDelete& m) { deletes.push_back(m); }
    void on_replace(const itch::OrderReplace& m) { replaces.push_back(m); }
    void on_trade(const itch::Trade& m) { trades.push_back(m); }
    void on_system_event(const itch::SystemEvent& m) { events.push_back(m); }
    void on_stock_directory(const itch::StockDirectory& m) { symbols.emplace_back(m.locate, std::string(m.symbol)); }
    void on_other(char type) { other.push_back(type); }
};

constexpr std::uint64_t kNineThirty = 34'200'000'000'000ull;  // 09:30:00 in ns since midnight

TEST(Itch, AddOrder) {
    Message m('A', 13, kNineThirty);
    m.u64(987654321).ch('B').u32(300).str("AAPL", 8).u32(2915200);
    ASSERT_EQ(m.size(), 36u);

    std::vector<std::byte> stream;
    m.append_to(stream);
    Recorder r;
    EXPECT_EQ(itch::parse(stream, r), stream.size());

    ASSERT_EQ(r.adds.size(), 1u);
    const auto& a = r.adds[0];
    EXPECT_EQ(a.locate, 13);
    EXPECT_EQ(a.timestamp, kNineThirty);
    EXPECT_EQ(a.ref, 987654321u);
    EXPECT_EQ(a.side, Side::Buy);
    EXPECT_EQ(a.shares, 300u);
    EXPECT_EQ(a.price, 2915200u);
}

TEST(Itch, AddOrderWithAttributionDecodesLikeAPlainAdd) {
    Message m('F', 7, 1);
    m.u64(42).ch('S').u32(100).str("MSFT", 8).u32(1570000).str("NITE", 4);
    ASSERT_EQ(m.size(), 40u);

    std::vector<std::byte> stream;
    m.append_to(stream);
    Recorder r;
    itch::parse(stream, r);

    ASSERT_EQ(r.adds.size(), 1u);
    EXPECT_EQ(r.adds[0].ref, 42u);
    EXPECT_EQ(r.adds[0].side, Side::Sell);
    EXPECT_EQ(r.adds[0].price, 1570000u);
}

TEST(Itch, Executions) {
    std::vector<std::byte> stream;
    Message e('E', 5, 100);
    e.u64(77).u32(50).u64(9001);
    ASSERT_EQ(e.size(), 31u);
    e.append_to(stream);

    Message c('C', 5, 200);
    c.u64(78).u32(25).u64(9002).ch('N').u32(1234500);
    ASSERT_EQ(c.size(), 36u);
    c.append_to(stream);

    Recorder r;
    itch::parse(stream, r);

    ASSERT_EQ(r.executions.size(), 1u);
    EXPECT_EQ(r.executions[0].ref, 77u);
    EXPECT_EQ(r.executions[0].shares, 50u);
    EXPECT_EQ(r.executions[0].match, 9001u);

    ASSERT_EQ(r.priced_executions.size(), 1u);
    EXPECT_EQ(r.priced_executions[0].ref, 78u);
    EXPECT_EQ(r.priced_executions[0].shares, 25u);
    EXPECT_FALSE(r.priced_executions[0].printable);
    EXPECT_EQ(r.priced_executions[0].price, 1234500u);
}

TEST(Itch, CancelDeleteReplace) {
    std::vector<std::byte> stream;
    Message x('X', 1, 10);
    x.u64(500).u32(30);
    ASSERT_EQ(x.size(), 23u);
    x.append_to(stream);

    Message d('D', 1, 11);
    d.u64(501);
    ASSERT_EQ(d.size(), 19u);
    d.append_to(stream);

    Message u('U', 1, 12);
    u.u64(502).u64(503).u32(400).u32(999900);
    ASSERT_EQ(u.size(), 35u);
    u.append_to(stream);

    Recorder r;
    itch::parse(stream, r);

    ASSERT_EQ(r.cancels.size(), 1u);
    EXPECT_EQ(r.cancels[0].ref, 500u);
    EXPECT_EQ(r.cancels[0].shares, 30u);

    ASSERT_EQ(r.deletes.size(), 1u);
    EXPECT_EQ(r.deletes[0].ref, 501u);

    ASSERT_EQ(r.replaces.size(), 1u);
    EXPECT_EQ(r.replaces[0].old_ref, 502u);
    EXPECT_EQ(r.replaces[0].new_ref, 503u);
    EXPECT_EQ(r.replaces[0].shares, 400u);
    EXPECT_EQ(r.replaces[0].price, 999900u);
}

TEST(Itch, HiddenTradeAndSystemEventAndDirectory) {
    std::vector<std::byte> stream;
    Message p('P', 3, 5);
    p.u64(0).ch('B').u32(10).str("TSLA", 8).u32(4140000).u64(31337);
    ASSERT_EQ(p.size(), 44u);
    p.append_to(stream);

    Message s('S', 0, 6);
    s.ch('Q');
    ASSERT_EQ(s.size(), 12u);
    s.append_to(stream);

    Message dir('R', 3, 7);
    dir.str("TSLA", 8).ch('Q').ch('N').u32(100).ch('N').ch('C').str("Z", 2).ch('P').ch('N').ch('N').ch('1').ch('N')
        .u32(0).ch('N');
    ASSERT_EQ(dir.size(), 39u);
    dir.append_to(stream);

    Recorder r;
    itch::parse(stream, r);

    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].shares, 10u);
    EXPECT_EQ(r.trades[0].price, 4140000u);
    EXPECT_EQ(r.trades[0].match, 31337u);

    ASSERT_EQ(r.events.size(), 1u);
    EXPECT_EQ(r.events[0].code, 'Q');

    ASSERT_EQ(r.symbols.size(), 1u);
    EXPECT_EQ(r.symbols[0].first, 3);
    EXPECT_EQ(r.symbols[0].second, "TSLA    ");
}

TEST(Itch, TimestampUsesAllSixBytes) {
    // 16:00:00.000000001, which needs more than 32 bits
    const std::uint64_t ts = 57'600'000'000'001ull;
    Message d('D', 1, ts);
    d.u64(1);
    std::vector<std::byte> stream;
    d.append_to(stream);
    Recorder r;
    itch::parse(stream, r);
    ASSERT_EQ(r.deletes.size(), 1u);
    EXPECT_EQ(r.deletes[0].timestamp, ts);
}

TEST(Itch, UnknownTypesAreSkippedByLength) {
    std::vector<std::byte> stream;
    Message noii('I', 1, 1);
    for (int i = 0; i < 39; ++i) noii.u8(0);  // 50 bytes total, contents irrelevant
    noii.append_to(stream);
    Message d('D', 1, 2);
    d.u64(9);
    d.append_to(stream);

    Recorder r;
    EXPECT_EQ(itch::parse(stream, r), stream.size());
    EXPECT_EQ(r.other, "I");
    ASSERT_EQ(r.deletes.size(), 1u);
    EXPECT_EQ(r.deletes[0].ref, 9u);
}

TEST(Itch, StopsAtATruncatedMessage) {
    std::vector<std::byte> stream;
    Message d('D', 1, 2);
    d.u64(9);
    d.append_to(stream);
    const std::size_t whole = stream.size();
    d.append_to(stream);
    stream.resize(stream.size() - 5);  // chop the second one

    Recorder r;
    EXPECT_EQ(itch::parse(stream, r), whole);
    EXPECT_EQ(r.deletes.size(), 1u);
}

struct Peeker : itch::Handler {
    std::vector<OrderId> upcoming;
    std::vector<OrderId> handled;
    // every id must be announced before its message is delivered
    bool announced_first = true;

    void on_upcoming(OrderId ref) { upcoming.push_back(ref); }
    void on_delete(const itch::OrderDelete& m) {
        handled.push_back(m.ref);
        if (upcoming.size() < handled.size()) announced_first = false;
    }
};

TEST(Itch, LookaheadAnnouncesEveryOrderIdOnceAndInOrder) {
    std::vector<std::byte> stream;
    std::vector<OrderId> refs;
    for (OrderId ref = 100; ref < 140; ++ref) {
        Message d('D', 1, ref);
        d.u64(ref);
        d.append_to(stream);
        refs.push_back(ref);
        if (ref % 7 == 0) {  // something without an order id in between
            Message s('S', 0, ref);
            s.ch('Q');
            s.append_to(stream);
        }
    }

    for (std::size_t lookahead : {1u, 8u, 39u, 40u, 1000u}) {
        Peeker p;
        EXPECT_EQ(itch::parse(stream, p, lookahead), stream.size());
        EXPECT_EQ(p.handled, refs) << "lookahead " << lookahead;
        EXPECT_EQ(p.upcoming, refs) << "lookahead " << lookahead;
        EXPECT_TRUE(p.announced_first) << "lookahead " << lookahead;
    }

    Peeker off;
    itch::parse(stream, off);
    EXPECT_EQ(off.handled, refs);
    EXPECT_TRUE(off.upcoming.empty());
}

TEST(Itch, ShortBodyIsNotDecoded) {
    // claims to be an add but is too short to hold one
    Message bad('A', 1, 1);
    bad.u64(5);
    std::vector<std::byte> stream;
    bad.append_to(stream);

    Recorder r;
    EXPECT_EQ(itch::parse(stream, r), stream.size());
    EXPECT_TRUE(r.adds.empty());
    EXPECT_EQ(r.other, "A");
}

}  // namespace
