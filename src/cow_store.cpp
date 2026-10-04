/*==============================================================================
 * cow_store.cpp / cache.cpp —— 写时复制块存储、快照与 LRU 缓存
 *
 * 快照为什么能做成 O(1)：因为**不在原地改数据**。
 *   写一个新块 → 改映射表；快照 = 把映射表冻结一份。
 *   于是快照与活动卷天然隔离，空间开销只发生在"快照之后又被改写"的块上。
 *
 * 这也是存储系统里"写时复制 / 重定向写（ROW）"这条主线的核心 ——
 * 它同样支撑了精简置备、克隆、增量备份、以及分布式复制。
 *============================================================================*/
#include "sdslite.h"

#include <algorithm>
#include <cstring>

namespace sds {

/*------------------------------------------------------- 块存储 + 快照 ---*/

CowBlockStore::CowBlockStore(std::string path, uint32_t block_size)
    : path_(std::move(path)), block_size_(block_size ? block_size : 4096) {}

Status CowBlockStore::open() {
    if (block_size_ == 0 || block_size_ > (1u << 20))
        return Status::Err(Code::InvalidArgument, "block_size 非法");
    return Status::Ok();   // 本原型纯内存映射，path_ 仅用于标识与后续持久化扩展
}

Status CowBlockStore::close() { return Status::Ok(); }

Status CowBlockStore::write(uint64_t lba, const std::string& data) {
    if (data.size() != block_size_)
        return Status::Err(Code::InvalidArgument,
                           "写长度必须等于块大小（这就是块存储的语义）");
    std::lock_guard<std::mutex> lk(mu_);

    // 写时复制：永远写新块，绝不覆盖老块（老块可能被快照引用）
    uint32_t id = next_block_++;
    Block b;
    b.data.assign(data.begin(), data.end());
    blocks_[id] = std::move(b);

    auto it = map_.find(lba);
    if (it != map_.end()) {
        auto bit = blocks_.find(it->second);
        if (bit != blocks_.end() && --bit->second.refcnt == 0)
            blocks_.erase(bit);                 // 没人引用了才回收
    }
    map_[lba] = id;
    logical_writes_++;
    return Status::Ok();
}

Status CowBlockStore::read(uint64_t lba, std::string& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(lba);
    if (it == map_.end()) return Status::Err(Code::NotFound, "逻辑块未分配");
    auto bit = blocks_.find(it->second);
    if (bit == blocks_.end()) return Status::Err(Code::Corrupted, "映射指向的物理块不存在");
    out.assign(bit->second.data.begin(), bit->second.data.end());
    return Status::Ok();
}

Status CowBlockStore::snapshot(const std::string& name, uint64_t& snap_id) {
    (void)name;
    std::lock_guard<std::mutex> lk(mu_);
    snap_id = next_snap_++;
    snaps_[snap_id] = map_;                     // O(1)：只冻结映射表，不复制数据
    for (const auto& kv : map_) {
        auto bit = blocks_.find(kv.second);
        if (bit != blocks_.end()) bit->second.refcnt++;
    }
    return Status::Ok();
}

Status CowBlockStore::read_snapshot(uint64_t snap_id, uint64_t lba, std::string& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto sit = snaps_.find(snap_id);
    if (sit == snaps_.end()) return Status::Err(Code::NotFound, "快照不存在");
    auto it = sit->second.find(lba);
    if (it == sit->second.end()) return Status::Err(Code::NotFound, "快照中没有该逻辑块");
    auto bit = blocks_.find(it->second);
    if (bit == blocks_.end()) return Status::Err(Code::Corrupted, "快照引用的物理块已丢失");
    out.assign(bit->second.data.begin(), bit->second.data.end());
    return Status::Ok();
}

Status CowBlockStore::drop_snapshot(uint64_t snap_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto sit = snaps_.find(snap_id);
    if (sit == snaps_.end()) return Status::Err(Code::NotFound, "快照不存在");
    for (const auto& kv : sit->second) {
        auto bit = blocks_.find(kv.second);
        if (bit != blocks_.end() && --bit->second.refcnt == 0)
            blocks_.erase(bit);
    }
    snaps_.erase(sit);
    return Status::Ok();
}

/*------------------------------------------------------------ LRU 缓存 ---*/

LruCache::LruCache(size_t capacity) : cap_(capacity ? capacity : 1) {}

void LruCache::unlink(Node* n) {
    if (n->prev) n->prev->next = n->next; else head_ = n->next;
    if (n->next) n->next->prev = n->prev; else tail_ = n->prev;
    n->prev = n->next = nullptr;
}

void LruCache::push_front(Node* n) {
    n->prev = nullptr;
    n->next = head_;
    if (head_) head_->prev = n;
    head_ = n;
    if (!tail_) tail_ = n;
}

bool LruCache::get(const std::string& key, std::string& out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(key);
    if (it == index_.end()) { misses_++; return false; }
    Node* n = it->second;
    unlink(n);
    push_front(n);                 // 命中即提到队首
    out = n->value;
    hits_++;
    return true;
}

void LruCache::put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        Node* n = it->second;
        n->value = value;
        unlink(n);
        push_front(n);
        return;
    }
    Node* n = new Node{key, value, nullptr, nullptr};
    index_[key] = n;
    push_front(n);
    if (index_.size() > cap_) {     // 淘汰队尾（最久未使用）
        Node* victim = tail_;
        if (victim) {
            unlink(victim);
            index_.erase(victim->key);
            delete victim;
        }
    }
}

void LruCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    Node* cur = head_;
    while (cur) { Node* nx = cur->next; delete cur; cur = nx; }
    head_ = tail_ = nullptr;
    index_.clear();
    hits_ = misses_ = 0;
}

double LruCache::hit_rate() const {
    uint64_t h = hits_, m = misses_;
    uint64_t total = h + m;
    return total ? static_cast<double>(h) / static_cast<double>(total) : 0.0;
}

size_t LruCache::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return index_.size();
}

}  // namespace sds
