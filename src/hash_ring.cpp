/*==============================================================================
 * hash_ring.cpp —— 一致性哈希环与副本放置
 *
 * 存储集群里"这个键该放哪个节点"决定了三件事：
 *   1. **负载是否均匀**；2. **加/减节点时要搬多少数据**；3. 副本能不能分散到不同故障域。
 *
 * 直接用 hash(key) % N 的致命问题：N 一变，几乎所有键都要搬家。
 * 一致性哈希把它变成"只有 1/N 的键需要搬"，代价是要处理**虚拟节点**带来的偏斜。
 *============================================================================*/
#include "sdslite.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace sds {

uint32_t HashRing::hash32(const std::string& s) {
    // FNV-1a 32 位：实现简单、分布够用
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

HashRing::HashRing(uint32_t vnodes_per_node) : vnodes_per_node_(vnodes_per_node ? vnodes_per_node : 1) {}

void HashRing::add_node(const std::string& node) {
    if (node.empty()) return;
    for (uint32_t i = 0; i < vnodes_per_node_; i++) {
        // 同一个物理节点的多个虚拟节点用不同后缀散开
        std::string vn = node + "#" + std::to_string(i);
        ring_[hash32(vn)] = node;
    }
}

void HashRing::remove_node(const std::string& node) {
    for (auto it = ring_.begin(); it != ring_.end();) {
        if (it->second == node) it = ring_.erase(it);
        else ++it;
    }
}

bool HashRing::has_node(const std::string& node) const {
    for (const auto& kv : ring_)
        if (kv.second == node) return true;
    return false;
}

std::vector<std::string> HashRing::nodes() const {
    std::set<std::string> s;
    for (const auto& kv : ring_) s.insert(kv.second);
    return std::vector<std::string>(s.begin(), s.end());
}

std::string HashRing::locate(const std::string& key) const {
    if (ring_.empty()) return "";
    uint32_t h = hash32(key);
    auto it = ring_.lower_bound(h);           // 顺时针第一个虚拟节点
    if (it == ring_.end()) it = ring_.begin(); // 越过环尾则回到环首
    return it->second;
}

std::vector<std::string> HashRing::locate_replicas(const std::string& key, int r) const {
    std::vector<std::string> out;
    if (ring_.empty() || r <= 0) return out;
    uint32_t h = hash32(key);
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) it = ring_.begin();

    // 从落点开始顺时针走，遇到新物理节点就选它（避免同一节点的多个虚拟节点占满副本位）
    auto cur = it;
    size_t guard = 0;
    while (static_cast<int>(out.size()) < r && guard++ < ring_.size()) {
        const std::string& node = cur->second;
        if (std::find(out.begin(), out.end(), node) == out.end()) out.push_back(node);
        ++cur;
        if (cur == ring_.end()) cur = ring_.begin();
    }
    // 节点数少于副本数：允许重复填满（真实系统此时会报"故障域不足"）
    while (static_cast<int>(out.size()) < r && !ring_.empty())
        out.push_back(it->second);
    return out;
}

std::map<std::string, uint64_t> HashRing::distribution(
    const std::vector<std::string>& keys) const {
    std::map<std::string, uint64_t> dist;
    for (const auto& k : keys) {
        std::string n = locate(k);
        if (!n.empty()) dist[n]++;
    }
    return dist;
}

double HashRing::migration_ratio(const HashRing& a, const HashRing& b,
                                 const std::vector<std::string>& keys) {
    if (keys.empty()) return 0.0;
    uint64_t moved = 0;
    for (const auto& k : keys)
        if (a.locate(k) != b.locate(k)) moved++;
    return static_cast<double>(moved) / static_cast<double>(keys.size());
}

/*---------------------------------------------------------- 线程池 -------*/

ThreadPool::ThreadPool(size_t threads) {
    if (threads == 0) threads = 1;
    for (size_t i = 0; i < threads; i++) {
        workers_.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_job_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
                    if (stop_ && jobs_.empty()) return;
                    job = std::move(jobs_.front());
                    jobs_.pop();
                    active_++;
                }
                job();
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    active_--;
                    completed_++;
                    if (jobs_.empty() && active_ == 0) cv_done_.notify_all();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_job_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
}

void ThreadPool::submit(std::function<void()> job) {
    {
        std::unique_lock<std::mutex> lk(mu_);
        jobs_.push(std::move(job));
    }
    cv_job_.notify_one();
}

void ThreadPool::wait_idle() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_done_.wait(lk, [this] { return jobs_.empty() && active_ == 0; });
}

}  // namespace sds
