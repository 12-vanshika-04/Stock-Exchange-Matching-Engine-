#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "Messages.hpp"
#include "Protocol.hpp"

namespace exch {

// Serves ONE gateway connection at a time on 127.0.0.1.
//
// While the server runs, its I/O thread is the ONLY thread allowed to call
// pipeline.trySubmit() and pipeline.tryPollEvent() (the SPSC queues have one producer
// and one consumer). Start the pipeline first; stop the server before the pipeline.
//
// `Pipeline` needs: bool trySubmit(const Command&) and bool tryPollEvent(Event&).
template <typename Pipeline>
class TcpServer {
public:
    // port 0 lets the OS pick a free port: read it back with port().
    TcpServer(Pipeline& pipeline, std::uint16_t port) : pipe_(pipeline) {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listenFd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
        const int yes = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listenFd_, 1) != 0) {
            const int err = errno;
            ::close(listenFd_);
            throw std::runtime_error("cannot listen on 127.0.0.1:" + std::to_string(port) + ": " +
                                     std::strerror(err));
        }
        socklen_t len = sizeof addr;
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }

    ~TcpServer() {
        stop();
        if (listenFd_ >= 0) ::close(listenFd_);
    }
    TcpServer(const TcpServer&)            = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void start() { thread_ = std::thread([this] { run(); }); }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    std::uint16_t port() const { return port_; }

    // Monitoring counters.
    std::uint64_t connections()      const { return connections_.load(); }
    std::uint64_t disconnects()      const { return disconnects_.load(); }
    std::uint64_t commandsReceived() const { return commandsReceived_.load(); }
    std::uint64_t eventsSent()       const { return eventsSent_.load(); }
    std::uint64_t protocolErrors()   const { return protocolErrors_.load(); }

private:
    static constexpr std::size_t kBatch = 256;   // events encoded per send

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            pollfd fd{};
            if (clientFd_ < 0) {
                fd.fd     = listenFd_;
                fd.events = POLLIN;
            } else {
                fd.fd = clientFd_;
                if (!havePendingCmd_)        fd.events |= POLLIN;    // else: engine is behind, stop reading
                if (outPos_ < outBuf_.size()) fd.events |= POLLOUT;
            }
            const int r = ::poll(&fd, 1, worked_ ? 0 : 1);
            worked_ = false;
            const short re = (r > 0) ? fd.revents : 0;

            if (clientFd_ < 0) {
                if (re & POLLIN) acceptClient();
                continue;
            }
            if (re & (POLLERR | POLLNVAL)) { disconnect(); continue; }
            if ((re & (POLLIN | POLLHUP)) && !havePendingCmd_ && !readInput()) { disconnect(); continue; }
            if (!processFrames() || !pumpOutput()) { disconnect(); continue; }
            if (havePendingCmd_) std::this_thread::yield();
        }
        if (clientFd_ >= 0) {
            ::close(clientFd_);
            clientFd_ = -1;
        }
    }

    void acceptClient() {
        const int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;                                  // nobody there after all
        const int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        clientFd_ = fd;
        ++connections_;
        worked_ = true;
    }

    void disconnect() {
        if (clientFd_ >= 0) {
            ::close(clientFd_);
            clientFd_ = -1;
        }
        reader_          = wire::FrameReader{};
        havePendingCmd_  = false;                            // never acknowledged, so the client may retry it
        if (outPos_ < outBuf_.size()) outPos_ = 0;           // a half-sent batch is resent in full to the next client
        ++disconnects_;
    }

    // false = the peer closed the connection or the socket broke.
    bool readInput() {
        std::uint8_t buf[64 * 1024];
        const ssize_t n = ::recv(clientFd_, buf, sizeof buf, 0);
        if (n > 0) {
            reader_.feed(buf, static_cast<std::size_t>(n));
            worked_ = true;
            return true;
        }
        if (n == 0) return false;
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }

    // Decode complete frames and submit them. false = the peer broke the protocol.
    bool processFrames() {
        for (;;) {
            if (havePendingCmd_) {
                if (!pipe_.trySubmit(pendingCmd_)) return true;      // engine queue full: retry next loop
                havePendingCmd_ = false;
                ++commandsReceived_;
                worked_ = true;
            }
            std::uint8_t        type = 0;
            const std::uint8_t* body = nullptr;
            std::size_t         len  = 0;
            const auto res = reader_.next(type, body, len);
            if (res == wire::FrameReader::Result::NEED_MORE) return true;
            if (res == wire::FrameReader::Result::BAD_FRAME) { ++protocolErrors_; return false; }
            Command c{};
            if (!wire::decodeCommandFrame(type, body, len, c)) { ++protocolErrors_; return false; }
            pendingCmd_     = c;
            havePendingCmd_ = true;
        }
    }

    // Send events to the client. false = the connection is broken.
    bool pumpOutput() {
        for (;;) {
            if (outPos_ == outBuf_.size()) {                         // nothing in flight: build the next batch
                batch_.clear();
                outBuf_.clear();
                outPos_ = 0;
                Event e;
                while (batch_.size() < kBatch && pipe_.tryPollEvent(e)) batch_.push_back(e);
                if (batch_.empty()) return true;
                for (const Event& ev : batch_) wire::encodeEvent(ev, outBuf_);
                worked_ = true;
            }
            const ssize_t n = ::send(clientFd_, outBuf_.data() + outPos_,
                                     outBuf_.size() - outPos_, MSG_NOSIGNAL);
            if (n > 0) {
                outPos_ += static_cast<std::size_t>(n);
                worked_ = true;
                if (outPos_ < outBuf_.size()) return true;           // the socket is full: continue later
                eventsSent_ += batch_.size();
                continue;                                            // batch done: look for more events
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
    }

    Pipeline&          pipe_;
    int                listenFd_ = -1;
    int                clientFd_ = -1;
    std::uint16_t      port_     = 0;
    std::atomic<bool>  stop_{false};
    std::thread        thread_;

    // I/O thread only:
    wire::FrameReader          reader_;
    Command                    pendingCmd_{};
    bool                       havePendingCmd_ = false;
    std::vector<Event>         batch_;             // events whose bytes are in outBuf_
    std::vector<std::uint8_t>  outBuf_;
    std::size_t                outPos_ = 0;        // how much of outBuf_ has been sent
    bool                       worked_ = false;    // did the last loop make progress?

    std::atomic<std::uint64_t> connections_{0}, disconnects_{0}, commandsReceived_{0},
                               eventsSent_{0}, protocolErrors_{0};
};

}  // namespace exch