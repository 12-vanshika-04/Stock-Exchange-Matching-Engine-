#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#include "EnginePipeline.hpp"
#include "Protocol.hpp"
#include "TcpServer.hpp"

using namespace exch;
using Bytes  = std::vector<std::uint8_t>;
using Server = TcpServer<EnginePipeline>;

namespace {

// A blocking test client with a 10 second receive timeout.
class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) throw std::runtime_error("socket failed");
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            ::close(fd_);
            throw std::runtime_error("connect failed");
        }
        const int yes = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        timeval tv{};
        tv.tv_sec = 10;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~Client() { if (fd_ >= 0) ::close(fd_); }
    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    bool sendAll(const Bytes& b) {
        std::size_t off = 0;
        while (off < b.size()) {
            const ssize_t n = ::send(fd_, b.data() + off, b.size() - off, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    // One byte per send, with tiny pauses: the server must reassemble the frame.
    void sendSlowly(const Bytes& b) {
        for (std::uint8_t byte : b) {
            ::send(fd_, &byte, 1, MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    }

    // Reads until `count` events are in `out`. false on EOF, a protocol problem or the timeout.
    bool readEvents(std::size_t count, std::vector<Event>& out) {
        std::uint8_t buf[65536];
        while (out.size() < count) {
            std::uint8_t        type = 0;
            const std::uint8_t* body = nullptr;
            std::size_t         len  = 0;
            const auto res = reader_.next(type, body, len);
            if (res == wire::FrameReader::Result::FRAME) {
                Event e;
                if (type != static_cast<std::uint8_t>(wire::MsgType::EVENT) ||
                    !wire::decodeEvent(body, len, e))
                    return false;
                out.push_back(e);
                continue;
            }
            if (res == wire::FrameReader::Result::BAD_FRAME) return false;
            const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n <= 0) return false;
            reader_.feed(buf, static_cast<std::size_t>(n));
        }
        return true;
    }

    // true once the server has closed the connection (whatever arrives before that is ignored).
    bool waitForClose() {
        std::uint8_t buf[4096];
        for (;;) {
            const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n == 0) return true;
            if (n < 0) return false;
        }
    }

    void abort() { ::shutdown(fd_, SHUT_RDWR); }     // unblocks a sender that is stuck

private:
    int               fd_ = -1;
    wire::FrameReader reader_;
};

Command limitOrder(UserId u, Side s, Price px, Qty q, std::uint64_t ref) {
    Command c{};
    c.type      = CommandType::NEW_ORDER;
    c.side      = s;
    c.orderType = OrderType::LIMIT;
    c.symbol    = 0;
    c.user      = u;
    c.price     = px;
    c.qty       = q;
    c.clientRef = ref;
    return c;
}

Bytes frameOf(const Command& c) {
    Bytes b;
    wire::encodeCommand(c, b);
    return b;
}

void seedUsers(EngineCore& core, unsigned users) {
    for (unsigned i = 0; i < users; ++i) {
        const UserId u = core.exchange().accounts().createAccount(toPaise(1'000'000));
        core.exchange().accounts().grantShares(u, 0, 5000, toPaise(500));
    }
}

std::vector<Command> randomCommands(unsigned seed, int n) {
    std::mt19937 rng(seed);
    std::vector<Command> cmds;
    for (int i = 0; i < n; ++i) {
        Command c{};
        c.user      = rng() % 3;
        c.symbol    = 0;
        c.clientRef = static_cast<std::uint64_t>(i);
        const unsigned roll = rng() % 100;
        if (roll < 15) {
            c.type    = CommandType::CANCEL_ORDER;
            c.orderId = makeOrderId(0, 1 + rng() % 400);
        } else {
            c.type = CommandType::NEW_ORDER;
            c.side = (rng() & 1) ? Side::BUY : Side::SELL;
            c.qty  = 1 + rng() % 100;
            if (roll < 80) {
                c.orderType = OrderType::LIMIT;
                c.price     = toPaise(490 + rng() % 21);
            } else {
                c.orderType = OrderType::MARKET;
            }
        }
        cmds.push_back(c);
    }
    return cmds;
}

bool sameEvent(const Event& a, const Event& b) {
    return a.type == b.type && a.reason == b.reason && a.status == b.status &&
           a.side == b.side && a.symbol == b.symbol && a.user == b.user &&
           a.orderId == b.orderId && a.price == b.price && a.qty == b.qty &&
           a.remaining == b.remaining && a.clientRef == b.clientRef &&
           a.trade.tradeId == b.trade.tradeId && a.trade.symbolId == b.trade.symbolId &&
           a.trade.price == b.trade.price && a.trade.qty == b.trade.qty &&
           a.trade.buyOrderId == b.trade.buyOrderId &&
           a.trade.sellOrderId == b.trade.sellOrderId &&
           a.trade.buyerId == b.trade.buyerId && a.trade.sellerId == b.trade.sellerId &&
           a.trade.seq == b.trade.seq;
}

}  // namespace

// alice: Rs 1,00,000 cash. bob: Rs 1,00,000 cash plus 1000 shares.
class TcpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        alice = pipe.core().exchange().accounts().createAccount(toPaise(100000));
        bob   = pipe.core().exchange().accounts().createAccount(toPaise(100000));
        pipe.core().exchange().accounts().grantShares(bob, 0, 1000, toPaise(400));
        pipe.start();
        server = std::make_unique<Server>(pipe, 0);
        server->start();
    }
    void TearDown() override {
        server->stop();
        pipe.stop();
    }
    EnginePipeline          pipe{1};
    std::unique_ptr<Server> server;
    UserId                  alice = 0;
    UserId                  bob   = 0;
};

TEST_F(TcpServerTest, AnOrderAndItsTradeComeBackOverARealSocket) {
    Client c(server->port());
    Bytes out = frameOf(limitOrder(bob, Side::SELL, toPaise(500), 100, 1));
    const Bytes buy = frameOf(limitOrder(alice, Side::BUY, toPaise(500), 100, 2));
    out.insert(out.end(), buy.begin(), buy.end());
    ASSERT_TRUE(c.sendAll(out));

    std::vector<Event> ev;
    ASSERT_TRUE(c.readEvents(3, ev));
    EXPECT_EQ(ev[0].type, EventType::ORDER_ACCEPTED);        // bob's sell rests
    EXPECT_EQ(ev[0].clientRef, 1u);
    EXPECT_EQ(ev[0].status, OrderStatus::NEW);
    EXPECT_EQ(ev[1].type, EventType::ORDER_ACCEPTED);        // alice's buy fills at once
    EXPECT_EQ(ev[1].clientRef, 2u);
    EXPECT_EQ(ev[1].status, OrderStatus::FILLED);
    EXPECT_EQ(ev[2].type, EventType::TRADE);
    EXPECT_EQ(ev[2].trade.qty, 100);
    EXPECT_EQ(ev[2].trade.price, toPaise(500));
    EXPECT_EQ(ev[2].trade.buyerId, alice);
    EXPECT_EQ(ev[2].trade.sellerId, bob);
    EXPECT_EQ(ev[2].clientRef, 2u);
}

TEST_F(TcpServerTest, AFrameDeliveredOneByteAtATimeIsStillUnderstood) {
    Client c(server->port());
    c.sendSlowly(frameOf(limitOrder(alice, Side::BUY, toPaise(500), 10, 5)));
    std::vector<Event> ev;
    ASSERT_TRUE(c.readEvents(1, ev));
    EXPECT_EQ(ev[0].type, EventType::ORDER_ACCEPTED);
    EXPECT_EQ(ev[0].clientRef, 5u);
}

TEST_F(TcpServerTest, BrokenFramesCloseOnlyThatConnectionAndTheServerKeepsWorking) {
    std::vector<Bytes> bad;
    bad.push_back(Bytes{0, 0, 0, 0});                        // length 0
    bad.push_back(Bytes{0x01, 0x04, 0, 0});                  // length 1025: larger than any legal frame
    bad.push_back(Bytes{1, 0, 0, 0, 99});                    // unknown message type
    Bytes badSide = frameOf(limitOrder(alice, Side::BUY, toPaise(500), 1, 1));
    badSide[5] = 2;                                          // an invalid side byte
    bad.push_back(badSide);

    for (const Bytes& frame : bad) {
        Client c(server->port());
        ASSERT_TRUE(c.sendAll(frame));
        EXPECT_TRUE(c.waitForClose());
    }
    EXPECT_EQ(server->protocolErrors(), 4u);

    Client ok(server->port());                               // the server is still alive
    ASSERT_TRUE(ok.sendAll(frameOf(limitOrder(alice, Side::BUY, toPaise(500), 1, 9))));
    std::vector<Event> ev;
    ASSERT_TRUE(ok.readEvents(1, ev));
    EXPECT_EQ(ev[0].clientRef, 9u);
}

TEST_F(TcpServerTest, ServerAcceptsANewClientAfterTheFirstOneLeaves) {
    {
        Client a(server->port());
        ASSERT_TRUE(a.sendAll(frameOf(limitOrder(alice, Side::BUY, toPaise(500), 1, 1))));
        std::vector<Event> ev;
        ASSERT_TRUE(a.readEvents(1, ev));
    }                                                        // client A hangs up
    Client b(server->port());
    ASSERT_TRUE(b.sendAll(frameOf(limitOrder(alice, Side::BUY, toPaise(499), 1, 2))));
    std::vector<Event> ev;
    ASSERT_TRUE(b.readEvents(1, ev));
    EXPECT_EQ(ev[0].clientRef, 2u);
    EXPECT_EQ(server->connections(), 2u);
}

TEST_F(TcpServerTest, StopDisconnectsAnIdleClientAndReturns) {
    Client c(server->port());
    ASSERT_TRUE(c.sendAll(frameOf(limitOrder(alice, Side::BUY, toPaise(500), 1, 1))));
    std::vector<Event> ev;
    ASSERT_TRUE(c.readEvents(1, ev));                        // the server has certainly accepted us now
    server->stop();
    EXPECT_TRUE(c.waitForClose());
}

// Thousands of commands through the socket: the events must equal a direct engine run.
TEST(TcpServerStream, ThousandsOfCommandsMatchTheDirectEngineEventForEvent) {
    const std::vector<Command> cmds = randomCommands(7, 20000);

    std::vector<Event> expected;
    {
        EngineCore ref(1);
        seedUsers(ref, 3);
        for (const Command& c : cmds) ref.process(c, [&](const Event& e) { expected.push_back(e); });
    }
    std::size_t trades = 0;
    for (const Event& e : expected) trades += (e.type == EventType::TRADE);
    ASSERT_GT(trades, 0u);

    EnginePipeline pipe(1);
    seedUsers(pipe.core(), 3);
    pipe.start();
    Server server(pipe, 0);
    server.start();

    Client client(server.port());
    Bytes all;
    for (const Command& c : cmds) wire::encodeCommand(c, all);

    std::atomic<bool> sent{false};
    std::thread sender([&] { sent.store(client.sendAll(all)); });
    std::vector<Event> got;
    got.reserve(expected.size());
    const bool received = client.readEvents(expected.size(), got);
    if (!received) client.abort();                           // never leave the sender stuck
    sender.join();
    server.stop();
    pipe.stop();

    ASSERT_TRUE(received) << "got " << got.size() << " of " << expected.size() << " events";
    EXPECT_TRUE(sent.load());
    for (std::size_t i = 0; i < got.size(); ++i)
        ASSERT_TRUE(sameEvent(got[i], expected[i])) << "streams diverge at event " << i;
    EXPECT_EQ(server.commandsReceived(), cmds.size());
    EXPECT_EQ(server.protocolErrors(), 0u);
}
TEST_F(TcpServerTest, AccountAndBookQueriesAreAnsweredOverTheSocket) {
    Client c(server->port());
    Bytes out = frameOf(limitOrder(bob, Side::SELL, toPaise(501), 10, 1));

    Command qa{};
    qa.type      = CommandType::QUERY_ACCOUNT;
    qa.user      = bob;
    qa.clientRef = 2;
    Command qb{};
    qb.type      = CommandType::QUERY_BOOK;
    qb.symbol    = 0;
    qb.qty       = 5;
    qb.clientRef = 3;
    for (const Command& q : {qa, qb}) {
        const Bytes f = frameOf(q);
        out.insert(out.end(), f.begin(), f.end());
    }
    ASSERT_TRUE(c.sendAll(out));

    // the order's acknowledgement (1) + account answer (balance, position, done = 3)
    // + book answer (one ask level, done = 2)
    std::vector<Event> ev;
    ASSERT_TRUE(c.readEvents(6, ev));
    EXPECT_EQ(ev[0].type, EventType::ORDER_ACCEPTED);
    EXPECT_EQ(ev[1].type, EventType::ACCOUNT_BALANCE);
    EXPECT_EQ(ev[1].clientRef, 2u);
    EXPECT_EQ(ev[2].type, EventType::ACCOUNT_POSITION);
    EXPECT_EQ(ev[2].remaining, 10);                            // bob's 10 shares are reserved by the open sell
    EXPECT_EQ(ev[3].type, EventType::QUERY_DONE);
    EXPECT_EQ(ev[4].type, EventType::BOOK_LEVEL);
    EXPECT_EQ(ev[4].side, Side::SELL);
    EXPECT_EQ(ev[4].price, toPaise(501));
    EXPECT_EQ(ev[4].qty, 10);
    EXPECT_EQ(ev[4].clientRef, 3u);
    EXPECT_EQ(ev[5].type, EventType::QUERY_DONE);
}

TEST_F(TcpServerTest, EngineInfoIsAnsweredOverTheSocket) {
    Client c(server->port());
    Command q{};
    q.type      = CommandType::QUERY_INFO;
    q.clientRef = 11;
    ASSERT_TRUE(c.sendAll(frameOf(q)));
    std::vector<Event> ev;
    ASSERT_TRUE(c.readEvents(2, ev));
    EXPECT_EQ(ev[0].type, EventType::ENGINE_INFO);
    EXPECT_EQ(ev[0].qty, 0);                             // this pipeline has no journal
    EXPECT_EQ(ev[0].remaining, 1);                       // one symbol
    EXPECT_EQ(ev[0].clientRef, 11u);
    EXPECT_EQ(ev[1].type, EventType::QUERY_DONE);
}
