/*==============================================================================
 * sdslite.h —— 软件定义存储（SDS）核心机制原型
 *
 * 面向"分布式存储系统底层研发"方向，把存储系统里最关键的几件事各实现一遍：
 *
 *   1. GF(256) 有限域运算          —— 纠删码的数学底座
 *   2. Reed-Solomon 纠删码 (k+m)   —— 编码 / 解码 / 任意 m 块丢失重建
 *   3. 一致性哈希环 + 副本放置      —— 数据分布、增删节点时的最小迁移
 *   4. WAL + CRC32 + 崩溃恢复       —— 断电后"已确认写入"的数据不能丢
 *   5. 写时复制块存储 + 快照        —— 快照隔离与空间开销
 *   6. LRU 读缓存                  —— 命中率
 *   7. 多线程并发 IO               —— 线程池 + 并发读写正确性
 *   8. 基准测试                    —— IOPS / 吞吐 / 延迟分位
 *
 * 设计取舍（面向底层研发岗位的真实口味）：
 *   - **C++17、零第三方依赖**，只用标准库 + POSIX 文件接口；
 *   - 所有会出事的地方都**显式检查返回值**，错误用 `sds::Status` 返回而不是抛异常；
 *   - 崩溃一致性不靠"看起来对"，而是用**真实的非正常退出注入**反复验证；
 *   - 每个模块都能单独测，指标全部打印真实数字。
 *============================================================================*/
#ifndef SDSLITE_H
#define SDSLITE_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <queue>

namespace sds {

/*--------------------------------------------------------------- 通用 -----*/

/** 统一错误码：底层代码不抛异常，避免异常跨越线程/信号边界时的未定义行为。 */
enum class Code {
    Ok = 0,
    NotFound,
    Corrupted,
    IoError,
    InvalidArgument,
    OutOfRange,
    AlreadyExists,
    CrcMismatch,
    NotEnoughShards,
};

struct Status {
    Code code = Code::Ok;
    std::string msg;

    bool ok() const { return code == Code::Ok; }
    static Status Ok() { return {}; }
    static Status Err(Code c, const std::string& m) { return Status{c, m}; }
    std::string str() const;
};

/** CRC32（IEEE 802.3 多项式，查表法）—— WAL 与数据块都用它做完整性校验。 */
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
inline uint32_t crc32(const std::string& s, uint32_t seed = 0) {
    return crc32(s.data(), s.size(), seed);
}

/** 把字节数格式化成人类可读字符串（仅用于输出）。 */
std::string human_bytes(uint64_t n);

/*------------------------------------------------------------ GF(256) -----*/

/**
 * GF(2^8) 有限域，本原多项式 0x11D（x^8+x^4+x^3+x^2+1），生成元 2。
 * Reed-Solomon 的加/减都是异或，乘除靠对数表。
 */
class GF256 {
public:
    static const GF256& instance();
    uint8_t mul(uint8_t a, uint8_t b) const;
    uint8_t div(uint8_t a, uint8_t b) const;   // b != 0
    uint8_t pow(uint8_t a, int n) const;
    uint8_t inv(uint8_t a) const;              // a != 0
    /** 用拉格朗日插值求 x 处的值：用于解码时重建缺失分片。 */
    uint8_t eval_poly(const std::vector<uint8_t>& coef, uint8_t x) const;

private:
    GF256();
    uint8_t exp_[512];
    uint8_t log_[256];
};

/*-------------------------------------------------- Reed-Solomon EC -------*/

/**
 * 纠删码：把 k 个数据分片编码成 m 个校验分片，总共 n = k + m 个。
 * 任意 m 个分片丢失都能原样恢复（这是存储系统比三副本更省空间的原因）。
 *
 * 实现用**范德蒙德矩阵 + 高斯消元**这一套经典做法：
 *   编码矩阵前 k 行是单位阵（数据直接落盘），后 m 行是校验行。
 *   解码时取仍然存活的 k 行组成方阵，求逆后乘回去即得原始数据。
 */
class ReedSolomon {
public:
    ReedSolomon(int k, int m);
    int k() const { return k_; }
    int m() const { return m_; }
    int n() const { return k_ + m_; }

    /** 每个分片长度必须相同且为 chunk_size；编码结果追加到 out（out 先 resize(n)）。 */
    Status encode(const std::vector<std::vector<uint8_t>>& data,
                  std::vector<std::vector<uint8_t>>& out) const;

    /**
     * 解码：shards 长度 n，缺失/损坏的位置用 present[i]=false 标记（对应内容被忽略）。
     * 至少要有 k 个存活分片，否则返回 NotEnoughShards。
     */
    Status decode(std::vector<std::vector<uint8_t>>& shards,
                  const std::vector<bool>& present,
                  std::vector<std::vector<uint8_t>>& out) const;

    /** 校验矩阵（n×k，GF(256)），主要给测试与调试用。 */
    const std::vector<std::vector<uint8_t>>& matrix() const { return matrix_; }

private:
    int k_, m_;
    std::vector<std::vector<uint8_t>> matrix_;   // n × k
    Status invert(const std::vector<std::vector<uint8_t>>& in,
                  std::vector<std::vector<uint8_t>>& out) const;
    void matmul(const std::vector<std::vector<uint8_t>>& a,
                const std::vector<std::vector<uint8_t>>& b,
                std::vector<std::vector<uint8_t>>& out) const;
};

/*------------------------------------------------ 一致性哈希与副本放置 ----*/

/**
 * 一致性哈希环：把物理节点按**虚拟节点**数量撒到 [0, 2^32) 的环上，
 * 键落在顺时针第一个虚拟节点所属的物理节点上。
 *
 * 两个要盯住的指标：
 *   - **分布均匀性**：虚拟节点太少时负载会严重偏斜（实测 1 个虚拟节点最大偏斜可达数倍）；
 *   - **增删节点的迁移量**：加一个节点理想情况只搬 1/N 的数据，这是它比取模哈希强的地方。
 */
class HashRing {
public:
    explicit HashRing(uint32_t vnodes_per_node = 128);
    void add_node(const std::string& node);
    void remove_node(const std::string& node);
    bool has_node(const std::string& node) const;
    std::vector<std::string> nodes() const;
    size_t vnode_count() const { return ring_.size(); }

    /** 键 → 节点 */
    std::string locate(const std::string& key) const;
    /** 键 → 按环顺序取的 r 个不同物理节点（副本放置） */
    std::vector<std::string> locate_replicas(const std::string& key, int r) const;

    /** 统计：返回每个物理节点负责的键数量（用于算偏斜与均匀性） */
    std::map<std::string, uint64_t> distribution(
        const std::vector<std::string>& keys) const;

    /** 计算"把 keys 从 ring_a 重新分布到 ring_b"时的迁移比例 */
    static double migration_ratio(const HashRing& a, const HashRing& b,
                                  const std::vector<std::string>& keys);

private:
    uint32_t vnodes_per_node_;
    std::map<uint32_t, std::string> ring_;      // vnode hash → node
    static uint32_t hash32(const std::string& s);
};

/*------------------------------------------------------- WAL 与恢复 ------*/

/**
 * 预写日志（Write-Ahead Log）：
 *   写入顺序永远是「先写日志（含 CRC）→ fsync → 再改数据」，
 *   这样任何时刻断电，重启后重放日志就能把"已经向调用方确认过"的写入补齐。
 *
 * 记录格式：[len(4) | crc32(4) | payload(len)]
 *   - 重放时逐条校验 CRC，遇到第一条不完整/校验失败的记录就**停在那里**，
 *     并把文件截断到该位置 —— 这是"最后一条可能写了一半"的标准处理方式。
 */
class WriteAheadLog {
public:
    explicit WriteAheadLog(std::string path);
    ~WriteAheadLog();

    /** 打开（不存在则创建）；truncate_tail=true 时会把残缺尾部截断 */
    Status open(bool truncate_tail = true);
    Status close();

    /** 追加一条记录并 fsync（durable=true） */
    Status append(const std::string& payload, bool durable = true);

    /** 重放：按顺序返回所有通过 CRC 校验的记录 */
    Status replay(std::vector<std::string>& out) const;

    /** 重置（清空日志，通常在一次 checkpoint 之后调用） */
    Status reset();

    uint64_t records_written() const { return records_written_; }
    uint64_t bytes_written() const { return bytes_written_; }
    uint64_t repairs() const { return repairs_; }

private:
    /* 不带锁的重放：open() 已经持有 mu_，不能再去调带锁的 replay()，
     * 否则就是自己等自己（std::mutex 不可重入）—— 这个坑在加锁时踩到过一次。 */
    Status replay_locked(std::vector<std::string>& out) const;

    std::string path_;
    int fd_ = -1;
    uint64_t records_written_ = 0;
    uint64_t bytes_written_ = 0;
    uint64_t repairs_ = 0;
    /* ⚠️ 必须加锁：一条记录如果分两次 write（头 + 数据），多线程并发追加时
     *    两次写会交错，落盘的就是"头是甲的、数据是乙的"，CRC 全部对不上 ——
     *    本项目就是被"4 线程并发追加"这条用例抓出来的（重放得到 0 条）。
     *    现在的做法是：**把一条记录拼成一个缓冲、一次 write 下去**，再加锁串行化。
     *    mutable 是必须的：replay() 是 const 方法，但也要持锁。 */
    mutable std::mutex mu_;
};

/*----------------------------------------------- 写时复制块存储 + 快照 ---*/

/**
 * 写时复制（COW）块存储：
 *   逻辑块 → 物理块 的映射表；写新数据时不覆盖老物理块，而是写新块再改映射。
 *   快照就是"把当前映射表整个冻结一份"，所以：
 *     - 快照创建是 O(1)（不复制数据）；
 *     - 快照与活动卷互不影响（隔离性）；
 *     - 空间开销只在"快照之后又改了块"时才产生。
 */
class CowBlockStore {
public:
    CowBlockStore(std::string path, uint32_t block_size = 4096);

    Status open();
    Status close();
    uint32_t block_size() const { return block_size_; }

    Status write(uint64_t lba, const std::string& data);
    Status read(uint64_t lba, std::string& out) const;

    /** 创建快照，返回快照 id */
    Status snapshot(const std::string& name, uint64_t& snap_id);
    Status read_snapshot(uint64_t snap_id, uint64_t lba, std::string& out) const;
    Status drop_snapshot(uint64_t snap_id);

    /** 统计 */
    uint64_t physical_blocks() const { return blocks_.size(); }
    uint64_t logical_writes() const { return logical_writes_; }
    uint64_t allocated_bytes() const { return blocks_.size() * (uint64_t)block_size_; }

private:
    struct Block { std::vector<uint8_t> data; uint32_t refcnt = 1; };

    std::string path_;
    uint32_t block_size_;
    std::map<uint64_t, uint32_t> map_;                  // lba → block id
    std::map<uint32_t, Block> blocks_;                  // block id → data
    std::map<uint64_t, std::map<uint64_t, uint32_t>> snaps_;  // snap → (lba → block id)
    uint32_t next_block_ = 1;
    uint64_t next_snap_ = 1;
    uint64_t logical_writes_ = 0;
    mutable std::mutex mu_;
};

/*------------------------------------------------------------- LRU 缓存 ---*/

/** 线程安全的 LRU 读缓存（哈希表 + 双向链表）。 */
class LruCache {
public:
    explicit LruCache(size_t capacity);
    /* ⚠️ 必须有析构：缓存用裸指针维护双向链表，不写析构就会把整张链表漏掉。
     *    这正是 AddressSanitizer 抓出来的第一个真问题（单元测试完全看不出来）。 */
    ~LruCache();
    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;

    bool get(const std::string& key, std::string& out);
    void put(const std::string& key, const std::string& value);
    void clear();

    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    double hit_rate() const;
    size_t size() const;

private:
    struct Node { std::string key, value; Node* prev = nullptr; Node* next = nullptr; };
    size_t cap_;
    std::map<std::string, Node*> index_;
    Node* head_ = nullptr;   // 最近使用
    Node* tail_ = nullptr;   // 最久未使用
    uint64_t hits_ = 0, misses_ = 0;
    mutable std::mutex mu_;
    void unlink(Node* n);
    void push_front(Node* n);
};

/*---------------------------------------------------------- 线程池 -------*/

/** 固定线程数线程池：存储的 IO 路径上不允许为每个请求起线程。 */
class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();
    void submit(std::function<void()> job);
    void wait_idle();
    size_t threads() const { return workers_.size(); }
    uint64_t completed() const { return completed_; }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mu_;
    std::condition_variable cv_job_, cv_done_;
    bool stop_ = false;
    size_t active_ = 0;
    std::atomic<uint64_t> completed_{0};
};

/*----------------------------------------------------------- 基准测试 ----*/

struct BenchResult {
    std::string name;
    uint64_t ops = 0;
    double seconds = 0.0;
    double iops() const { return seconds > 0 ? ops / seconds : 0.0; }
    double mbps(double bytes_per_op) const {
        return seconds > 0 ? ops * bytes_per_op / seconds / (1024.0 * 1024.0) : 0.0;
    }
    double lat_p50_us = 0.0, lat_p99_us = 0.0, lat_avg_us = 0.0;
};

/** 微秒级时钟 */
uint64_t now_us();

}  // namespace sds

#endif  // SDSLITE_H
