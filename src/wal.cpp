/*==============================================================================
 * wal.cpp —— 预写日志、CRC 校验与崩溃恢复
 *
 * 存储系统里"数据不能丢"这条承诺，靠的是**顺序**：
 *   先把要做的事写进日志并 fsync，再动真正的数据。
 * 这样任何时刻断电，重启后重放日志就能把"已经向调用方回过 OK"的操作补齐。
 *
 * 记录格式（小端）：[len:4][crc32:4][payload:len]
 *   crc32 覆盖 payload；重放时逐条校验，**遇到第一条不完整或校验失败的记录就停**，
 *   并把文件截断到该位置 —— "最后一条可能只写了一半"是常态而不是异常。
 *============================================================================*/
#include "sdslite.h"
#include "posix_compat.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

namespace sds {

namespace {

constexpr size_t kHeaderSize = 8;

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        long long n = ::write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

WriteAheadLog::WriteAheadLog(std::string path) : path_(std::move(path)) {}

WriteAheadLog::~WriteAheadLog() { close(); }

Status WriteAheadLog::open(bool truncate_tail) {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ >= 0) return Status::Ok();
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_BINARY, 0644);
    if (fd_ < 0)
        return Status::Err(Code::IoError, "打开 WAL 失败: " + path_);
    if (truncate_tail) {
        // 重放一遍，顺便把残缺尾部截掉，保证后续 append 从干净位置开始
        std::vector<std::string> recs;
        Status st = replay_locked(recs);
        if (!st.ok()) return st;
    }
    return Status::Ok();
}

Status WriteAheadLog::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ >= 0) {
        ::fsync(fd_);
        ::close(fd_);
        fd_ = -1;
    }
    return Status::Ok();
}

Status WriteAheadLog::append(const std::string& payload, bool durable) {
    if (payload.size() > 64u * 1024u * 1024u)
        return Status::Err(Code::InvalidArgument, "单条记录过大");

    // 一条记录拼成一个缓冲、只发**一次** write：
    // 既避免本进程多线程交错，也减少跨进程撕裂的概率（O_APPEND 下单次写是原子的）
    std::string rec;
    rec.resize(kHeaderSize + payload.size());
    put_u32(reinterpret_cast<uint8_t*>(&rec[0]), static_cast<uint32_t>(payload.size()));
    put_u32(reinterpret_cast<uint8_t*>(&rec[0]) + 4, crc32(payload.data(), payload.size()));
    if (!payload.empty()) std::memcpy(&rec[kHeaderSize], payload.data(), payload.size());

    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ < 0) return Status::Err(Code::IoError, "WAL 未打开");
    if (!write_all(fd_, rec.data(), rec.size()))
        return Status::Err(Code::IoError, "写 WAL 失败");

    if (durable && ::fsync(fd_) != 0)
        return Status::Err(Code::IoError, "fsync WAL 失败");

    records_written_++;
    bytes_written_ += rec.size();
    return Status::Ok();
}

Status WriteAheadLog::replay(std::vector<std::string>& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    return replay_locked(out);
}

Status WriteAheadLog::replay_locked(std::vector<std::string>& out) const {
    out.clear();
    int fd = ::open(path_.c_str(), O_RDONLY | O_BINARY);
    if (fd < 0) return Status::Err(Code::IoError, "打开 WAL 读取失败");
    long long good_end = 0;
    for (;;) {
        uint8_t hdr[kHeaderSize];
        long long n = ::pread(fd, hdr, kHeaderSize, good_end);
        if (n == 0) break;                                  // 正常结束
        if (n < 0) { ::close(fd); return Status::Err(Code::IoError, "读 WAL 失败"); }
        if (n < static_cast<long long>(kHeaderSize)) break;  // 头部残缺：停在这

        uint32_t len = get_u32(hdr);
        uint32_t want_crc = get_u32(hdr + 4);
        if (len > 64u * 1024u * 1024u) break;                // 龙头数据：按损坏处理

        std::string payload(len, '\0');
        if (len > 0) {
            long long m = ::pread(fd, &payload[0], len, good_end + kHeaderSize);
            if (m < 0 || static_cast<uint32_t>(m) != len) break;   // 数据残缺：停在这
        }
        if (crc32(payload.data(), payload.size()) != want_crc) break;  // 校验失败：停在这

        out.push_back(std::move(payload));
        good_end += static_cast<long long>(kHeaderSize + len);
    }
    ::close(fd);

    // 把残缺尾部截掉（这一步是崩溃恢复的关键：下次写入不会被尾巴干扰）
    struct stat st{};
    bool need_truncate = false;
    if (::stat(path_.c_str(), &st) == 0 && st.st_size > good_end)
        need_truncate = true;
    if (need_truncate) {
        int wfd = ::open(path_.c_str(), O_RDWR | O_BINARY);
        if (wfd >= 0) {
            if (::ftruncate(wfd, good_end) == 0) {
                ::fsync(wfd);
                const_cast<WriteAheadLog*>(this)->repairs_++;
            }
            ::close(wfd);
        }
    }
    return Status::Ok();
}

Status WriteAheadLog::reset() {
    // ⚠️ 不能在这里调 open()：open() 也要拿 mu_，而 std::mutex 不可重入 → 自死锁
    //    （本项目就这么挂过一次，测试直接卡住不返回）。这里直接复用已打开的 fd。
    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_BINARY, 0644);
    if (fd < 0) return Status::Err(Code::IoError, "打开 WAL 用于截断失败");
    if (::ftruncate(fd, 0) != 0) {
        ::close(fd);
        return Status::Err(Code::IoError, "截断 WAL 失败");
    }
    fd_ = fd;
    return Status::Ok();
}

}  // namespace sds
