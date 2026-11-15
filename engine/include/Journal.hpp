#pragma once
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "Messages.hpp"
#include "Types.hpp"

namespace exch {

inline constexpr std::size_t kJournalHeaderSize  = 8;
inline constexpr std::size_t kJournalPayloadSize = 42;
inline constexpr std::size_t kJournalRecordSize  = 4 + kJournalPayloadSize;   // crc32 + payload
inline constexpr char        kJournalMagic[kJournalHeaderSize + 1] = "EXJRNL01";

// CRC-32 (the common IEEE one: crc32("123456789") == 0xCBF43926).
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

enum class RecordKind : std::uint8_t { CREATE_ACCOUNT = 1, GRANT_SHARES = 2, COMMAND = 3, REFERENCE_PRICE = 4, JOURNAL_ID = 5 };

// One journal entry. All three kinds share one flat layout; unused fields stay 0.
//   CREATE_ACCOUNT: user = the id the account must get, price = initial cash
//   GRANT_SHARES  : user, symbol, qty, price = cost per share
//   COMMAND       : the command's own fields
// REFERENCE_PRICE: symbol, price= that symbol's reference price (first records of a new journal)
struct JournalRecord {
    RecordKind    kind      = RecordKind::COMMAND;
    CommandType   cmdType   = CommandType::NEW_ORDER;
    Side          side      = Side::BUY;
    OrderType     orderType = OrderType::LIMIT;
    SymbolId      symbol    = 0;
    UserId        user      = 0;
    Price         price     = 0;
    Qty           qty       = 0;
    OrderId       orderId   = 0;
    std::uint64_t clientRef = 0;

    static JournalRecord forCommand(const Command& c) {
        JournalRecord r;
        r.kind      = RecordKind::COMMAND;
        r.cmdType   = c.type;
        r.side      = c.side;
        r.orderType = c.orderType;
        r.symbol    = c.symbol;
        r.user      = c.user;
        r.price     = c.price;
        r.qty       = c.qty;
        r.orderId   = c.orderId;
        r.clientRef = c.clientRef;
        return r;
    }
    static JournalRecord forCreateAccount(UserId expectedUser, Price cash) {
        JournalRecord r;
        r.kind  = RecordKind::CREATE_ACCOUNT;
        r.user  = expectedUser;
        r.price = cash;
        return r;
    }
        // The identity of a journal (see JournaledCore). Kept in orderId: 52 bits, never 0.
    static JournalRecord forJournalId(std::uint64_t id) {
        JournalRecord r;
        r.kind    = RecordKind::JOURNAL_ID;
        r.orderId = id;
        return r;
    }

    static JournalRecord forReferencePrice(SymbolId symbol, Price referencePrice) {
        JournalRecord r;
        r.kind   = RecordKind::REFERENCE_PRICE;
        r.symbol = symbol;
        r.price  = referencePrice;
        return r;
    }
    static JournalRecord forGrantShares(UserId user, SymbolId symbol, Qty qty, Price costPerShare) {
        JournalRecord r;
        r.kind   = RecordKind::GRANT_SHARES;
        r.user   = user;
        r.symbol = symbol;
        r.qty    = qty;
        r.price  = costPerShare;
        return r;
    }
    Command toCommand() const {
        Command c{};
        c.type      = cmdType;
        c.side      = side;
        c.orderType = orderType;
        c.symbol    = symbol;
        c.user      = user;
        c.price     = price;
        c.qty       = qty;
        c.orderId   = orderId;
        c.clientRef = clientRef;
        return c;
    }
    bool operator==(const JournalRecord& o) const {
        return kind == o.kind && cmdType == o.cmdType && side == o.side &&
               orderType == o.orderType && symbol == o.symbol && user == o.user &&
               price == o.price && qty == o.qty && orderId == o.orderId &&
               clientRef == o.clientRef;
    }
};

namespace detail {

template <typename T>
inline void put(std::uint8_t*& p, T v) { std::memcpy(p, &v, sizeof(T)); p += sizeof(T); }

template <typename T>
inline T get(const std::uint8_t*& p) { T v; std::memcpy(&v, p, sizeof(T)); p += sizeof(T); return v; }

// Owns a file descriptor and closes it, even when a constructor throws halfway.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&)            = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    int get() const { return fd_; }
private:
    int fd_ = -1;
};

inline int openJournalFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        throw std::runtime_error("cannot open journal " + path + ": " + std::strerror(errno));
    return fd;
}

// Missing file -> empty vector (treated as an empty journal).
inline std::vector<std::uint8_t> readWholeFile(const std::string& path) {
    Fd fd(::open(path.c_str(), O_RDONLY));
    if (fd.get() < 0) {
        if (errno == ENOENT) return {};
        throw std::runtime_error("cannot open journal " + path + ": " + std::strerror(errno));
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0)
        throw std::runtime_error("cannot stat journal " + path + ": " + std::strerror(errno));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < data.size()) {
        const ssize_t r = ::read(fd.get(), data.data() + got, data.size() - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot read journal " + path + ": " + std::strerror(errno));
        }
        if (r == 0) break;                               // the file shrank while we read it
        got += static_cast<std::size_t>(r);
    }
    data.resize(got);
    return data;
}

}  // namespace detail

inline void encodePayload(const JournalRecord& r, std::uint8_t* out) {
    std::uint8_t* p = out;
    detail::put<std::uint8_t>(p, static_cast<std::uint8_t>(r.kind));
    detail::put<std::uint8_t>(p, static_cast<std::uint8_t>(r.cmdType));
    detail::put<std::uint8_t>(p, static_cast<std::uint8_t>(r.side));
    detail::put<std::uint8_t>(p, static_cast<std::uint8_t>(r.orderType));
    detail::put<SymbolId>(p, r.symbol);
    detail::put<UserId>(p, r.user);
    detail::put<Price>(p, r.price);
    detail::put<Qty>(p, r.qty);
    detail::put<OrderId>(p, r.orderId);
    detail::put<std::uint64_t>(p, r.clientRef);
}

// false if a field holds a value no valid record can contain.
inline bool decodePayload(const std::uint8_t* in, JournalRecord& r) {
    const std::uint8_t* p = in;
    const auto kind = detail::get<std::uint8_t>(p);
    const auto cmd  = detail::get<std::uint8_t>(p);
    const auto side = detail::get<std::uint8_t>(p);
    const auto ot   = detail::get<std::uint8_t>(p);
    if (kind < 1 || kind > 5 || cmd > 1 || side > 1 || ot > 1) return false;
    r.kind      = static_cast<RecordKind>(kind);
    r.cmdType   = static_cast<CommandType>(cmd);
    r.side      = static_cast<Side>(side);
    r.orderType = static_cast<OrderType>(ot);
    r.symbol    = detail::get<SymbolId>(p);
    r.user      = detail::get<UserId>(p);
    r.price     = detail::get<Price>(p);
    r.qty       = detail::get<Qty>(p);
    r.orderId   = detail::get<OrderId>(p);
    r.clientRef = detail::get<std::uint64_t>(p);
    return true;
}

// Appends records to the journal file. Throws std::runtime_error on any I/O failure.
// One writer per file at a time. Not thread-safe: the engine thread will own it.
class JournalWriter {
public:
    explicit JournalWriter(const std::string& path)
        : path_(path), fd_(detail::openJournalFile(path)) {
        struct stat st{};
        if (::fstat(fd_.get(), &st) != 0)
            throw std::runtime_error("cannot stat journal " + path_ + ": " + std::strerror(errno));
        if (st.st_size == 0) {
            writeAll(kJournalMagic, kJournalHeaderSize);                 // brand-new file
        } else {
            char hdr[kJournalHeaderSize] = {};
            const bool read =
                static_cast<std::size_t>(st.st_size) >= kJournalHeaderSize &&
                ::pread(fd_.get(), hdr, kJournalHeaderSize, 0) ==
                    static_cast<ssize_t>(kJournalHeaderSize);
            if (!read || std::memcmp(hdr, kJournalMagic, kJournalHeaderSize) != 0)
                throw std::runtime_error("not an exchange journal: " + path_);
        }
        buf_.reserve(kBufferLimit + kJournalRecordSize);
    }

    ~JournalWriter() {
        try { flush(); } catch (...) {}                                  // never throw from a destructor
    }

    JournalWriter(const JournalWriter&)            = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    // Buffered. Not on disk, and not even in the OS, until flush().
    void append(const JournalRecord& r) {
        std::uint8_t payload[kJournalPayloadSize];
        encodePayload(r, payload);
        const std::uint32_t crc = crc32(payload, kJournalPayloadSize);
        const std::size_t at = buf_.size();
        buf_.resize(at + kJournalRecordSize);
        std::memcpy(buf_.data() + at, &crc, sizeof crc);
        std::memcpy(buf_.data() + at + sizeof crc, payload, kJournalPayloadSize);
        ++appended_;
        if (buf_.size() >= kBufferLimit) flush();
    }

    // Hands everything buffered to the OS. Survives a crash of this process.
    void flush() {
        if (buf_.empty()) return;
        writeAll(buf_.data(), buf_.size());
        buf_.clear();
    }

    // flush() plus a disk sync. Survives power loss. Much slower: use it in batches.
    void sync() {
        flush();
        if (::fdatasync(fd_.get()) != 0)
            throw std::runtime_error("fdatasync failed on " + path_ + ": " + std::strerror(errno));
    }

    std::uint64_t recordsAppended() const { return appended_; }
    const std::string& path() const { return path_; }

private:
    static constexpr std::size_t kBufferLimit = 64 * 1024;

    void writeAll(const void* data, std::size_t n) {
        const char* p = static_cast<const char*>(data);
        while (n > 0) {
            const ssize_t w = ::write(fd_.get(), p, n);
            if (w < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("write to journal failed: " + path_ + ": " + std::strerror(errno));
            }
            p += w;
            n -= static_cast<std::size_t>(w);
        }
    }

    std::string               path_;
    detail::Fd                fd_;
    std::vector<std::uint8_t> buf_;
    std::uint64_t             appended_ = 0;
};

enum class JournalStatus { CLEAN, TORN_TAIL, CORRUPT, BAD_HEADER };

struct JournalReadResult {
    JournalStatus status     = JournalStatus::CLEAN;
    std::uint64_t records    = 0;    // valid records delivered to the visitor
    std::uint64_t validBytes = 0;    // length of the valid prefix: cut the file here to repair a torn tail
};

// Calls visit(const JournalRecord&) for every valid record, in order.
// A missing or empty file is an empty, clean journal.
template <typename Visitor>
JournalReadResult readJournal(const std::string& path, Visitor&& visit) {
    const std::vector<std::uint8_t> data = detail::readWholeFile(path);
    JournalReadResult res;
    if (data.empty()) return res;

    if (data.size() < kJournalHeaderSize) {                  // a crash while the header was being written?
        res.status = (std::memcmp(data.data(), kJournalMagic, data.size()) == 0)
                         ? JournalStatus::TORN_TAIL : JournalStatus::BAD_HEADER;
        return res;
    }
    if (std::memcmp(data.data(), kJournalMagic, kJournalHeaderSize) != 0) {
        res.status = JournalStatus::BAD_HEADER;
        return res;
    }

    std::size_t pos = kJournalHeaderSize;
    res.validBytes  = kJournalHeaderSize;
    while (pos < data.size()) {
        if (data.size() - pos < kJournalRecordSize) {        // partial last record
            res.status = JournalStatus::TORN_TAIL;
            return res;
        }
        std::uint32_t stored = 0;
        std::memcpy(&stored, &data[pos], sizeof stored);
        const std::uint8_t* payload = &data[pos + sizeof stored];

        JournalRecord rec;
        const bool good = crc32(payload, kJournalPayloadSize) == stored && decodePayload(payload, rec);
        if (!good) {
            // Damage in the LAST record is what a crash mid-write leaves behind.
            // Damage with more data after it is real corruption.
            res.status = (pos + kJournalRecordSize < data.size()) ? JournalStatus::CORRUPT
                                                                  : JournalStatus::TORN_TAIL;
            return res;
        }
        visit(rec);
        ++res.records;
        pos += kJournalRecordSize;
        res.validBytes = pos;
    }
    return res;
}

}  // namespace exch