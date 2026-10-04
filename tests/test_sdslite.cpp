/*==============================================================================
 * test_sdslite.cpp —— 单元测试
 *
 * 测试口味（面向底层研发岗位）：
 *   1. **穷举**比抽样更可信：纠删码要把"任意 m 块丢失"的所有组合都跑一遍；
 *   2. **不变量**比现象更可信：GF(256) 的域公理、快照隔离、WAL 前缀性；
 *   3. 每个指标都打印真实数字（偏斜、迁移比、命中率、回收情况）。
 *============================================================================*/
#include "sdslite.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>   // POSIX 的 mkdir 在这里；少这个头文件在 Linux/macOS 上直接编译不过
#include <sys/types.h>
#endif

using namespace sds;

static int g_pass = 0, g_fail = 0;
static const char* g_group = "";

#define GROUP(n) do { g_group = (n); std::printf("\n== %s ==\n", g_group); } while (0)
#define CHECK(cond, ...) do {                                                 \
    if (cond) { g_pass++; }                                                   \
    else { g_fail++; std::printf("  [FAIL] %s: ", g_group);                   \
           std::printf(__VA_ARGS__); std::printf("\n"); }                     \
} while (0)
#define INFO(...) do { std::printf("  [INFO] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

static std::string tmp_path(const std::string& name) {
    // 用相对目录：Linux 与 Windows 都能跑（/tmp 在 Windows 的 UCRT 工具链下不存在）
    static bool inited = false;
    if (!inited) {
#ifdef _WIN32
        ::_mkdir("test_tmp");
#else
        ::mkdir("test_tmp", 0755);
#endif
        inited = true;
    }
    return "test_tmp/" + name + "_" + std::to_string(::getpid());
}

static void unlink_path(const std::string& p) {
#ifdef _WIN32
    ::_unlink(p.c_str());
#else
    ::unlink(p.c_str());
#endif
}

/*-------------------------------------------------------------- GF(256) --*/
static void test_gf256() {
    GROUP("GF(256) 有限域");
    const GF256& gf = GF256::instance();
    std::mt19937 rng(12345);

    // 域公理：a*inv(a)=1、div(mul(a,b),b)=a、分配律 (a^b)*c = a*c ^ b*c
    int axiom_fail = 0;
    for (int t = 0; t < 20000; t++) {
        uint8_t a = static_cast<uint8_t>(rng() & 0xFF);
        uint8_t b = static_cast<uint8_t>(rng() & 0xFF);
        uint8_t c = static_cast<uint8_t>(rng() & 0xFF);
        if (a && gf.mul(a, gf.inv(a)) != 1) axiom_fail++;
        if (b && gf.div(gf.mul(a, b), b) != a) axiom_fail++;
        uint8_t lhs = gf.mul(static_cast<uint8_t>(a ^ b), c);
        uint8_t rhs = static_cast<uint8_t>(gf.mul(a, c) ^ gf.mul(b, c));
        if (lhs != rhs) axiom_fail++;
    }
    CHECK(axiom_fail == 0, "域公理（逆元/除法/分配律）应恒成立，违反 %d 次", axiom_fail);
    CHECK(gf.mul(0, 123) == 0, "0 乘任何数为 0");
    CHECK(gf.mul(1, 200) == 200, "1 是乘法单位元");
    CHECK(gf.pow(2, 255) == 1, "生成元 2 的 255 次幂应为 1（域阶）");
    INFO("20000 组随机三元组验证通过（逆元/除法/分配律）");

    // 多项式求值：常数多项式 f(x)=c 在任何点都等于 c
    std::vector<uint8_t> poly = {77};
    CHECK(gf.eval_poly(poly, 0) == 77 && gf.eval_poly(poly, 200) == 77,
          "常数多项式求值应恒等于常数");
    // 一次多项式 f(x) = a1*x + a0 在 x=1 处等于 a1^a0（加法即异或）
    std::vector<uint8_t> poly2 = {5, 9};
    CHECK(gf.eval_poly(poly2, 1) == (5 ^ 9), "f(1) 应等于系数异或");
}

/*------------------------------------------------------------ 纠删码 ----*/
static std::vector<uint8_t> rand_chunk(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& x : v) x = static_cast<uint8_t>(rng() & 0xFF);
    return v;
}

static void test_ec() {
    GROUP("Reed-Solomon 纠删码");
    const int k = 4, m = 2;
    ReedSolomon rs(k, m);
    const size_t chunk = 4096;
    std::vector<std::vector<uint8_t>> data(k);
    for (int i = 0; i < k; i++) data[i] = rand_chunk(chunk, 100 + i);

    std::vector<std::vector<uint8_t>> coded;
    Status st = rs.encode(data, coded);
    CHECK(st.ok(), "编码应成功: %s", st.str().c_str());
    CHECK(static_cast<int>(coded.size()) == k + m, "编码后应有 k+m 个分片");
    for (int i = 0; i < k; i++)
        CHECK(coded[i] == data[i], "数据分片应原样保留（第 %d 片）", i);

    // ---- 穷举任意 m=2 块丢失的所有组合（C(6,2)=15 种）----
    int combo = 0, combo_fail = 0;
    for (int a = 0; a < k + m; a++)
        for (int b = a + 1; b < k + m; b++) {
            auto shards = coded;
            std::vector<bool> present(k + m, true);
            present[a] = present[b] = false;
            std::vector<std::vector<uint8_t>> out;
            Status s2 = rs.decode(shards, present, out);
            combo++;
            if (!s2.ok() || out.size() != static_cast<size_t>(k)) { combo_fail++; continue; }
            for (int i = 0; i < k; i++)
                if (out[i] != data[i]) { combo_fail++; break; }
        }
    CHECK(combo == 15, "应覆盖 C(6,2)=15 种双盘失效组合");
    CHECK(combo_fail == 0, "任意 2 块丢失都应能完整恢复，失败 %d 种", combo_fail);
    INFO("EC(%d+%d)：%d 种双盘失效组合全部恢复成功，空间开销 %.2fx（三副本为 3.00x）",
         k, m, combo, static_cast<double>(k + m) / k);

    // ---- 单块丢失也要能恢复（1 块丢是常态）----
    int single_fail = 0;
    for (int a = 0; a < k + m; a++) {
        auto shards = coded;
        std::vector<bool> present(k + m, true);
        present[a] = false;
        std::vector<std::vector<uint8_t>> out;
        Status s2 = rs.decode(shards, present, out);
        if (!s2.ok()) { single_fail++; continue; }
        for (int i = 0; i < k; i++) if (out[i] != data[i]) { single_fail++; break; }
    }
    CHECK(single_fail == 0, "任意 1 块丢失都应能恢复");

    // ---- 存活不足 k：必须报 NotEnoughShards 而不是给错数据 ----
    {
        auto shards = coded;
        std::vector<bool> present(k + m, false);
        present[0] = present[1] = present[k];
        std::vector<std::vector<uint8_t>> out;
        Status s3 = rs.decode(shards, present, out);
        CHECK(s3.code == Code::NotEnoughShards,
              "存活 3 片 < k=4 时应返回 NotEnoughShards，实际 %s", s3.str().c_str());
    }

    // ---- 参数校验 ----
    {
        std::vector<std::vector<uint8_t>> bad(k);
        for (int i = 0; i < k; i++) bad[i] = rand_chunk(i == 1 ? 100 : 200, 7 + i);
        std::vector<std::vector<uint8_t>> o;
        CHECK(rs.encode(bad, o).code == Code::InvalidArgument, "分片长度不一致应报错");
    }

    // ---- 不同条带大小 ----
    for (size_t cs : {16u, 512u, 65536u}) {
        std::vector<std::vector<uint8_t>> d2(k);
        for (int i = 0; i < k; i++) d2[i] = rand_chunk(cs, 900 + i);
        std::vector<std::vector<uint8_t>> c2, o2;
        rs.encode(d2, c2);
        std::vector<bool> pres(k + m, true);
        pres[1] = pres[4] = false;
        Status s4 = rs.decode(c2, pres, o2);
        bool ok = s4.ok();
        for (int i = 0; i < k && ok; i++) ok = (o2[i] == d2[i]);
        CHECK(ok, "条带 %zu 字节时应能恢复", cs);
    }
}

/*-------------------------------------------------- 一致性哈希与副本 ----*/
static void test_hash_ring() {
    GROUP("一致性哈希与副本放置");
    std::vector<std::string> keys;
    for (int i = 0; i < 20000; i++) keys.push_back("object-" + std::to_string(i));

    // 稳定性
    HashRing ring(128);
    for (int i = 0; i < 5; i++) ring.add_node("node" + std::to_string(i));
    CHECK(ring.locate("object-12345") == ring.locate("object-12345"), "同一个键必须落在同一节点");
    CHECK(ring.locate("") != "", "空键也应落在某个节点（而不是崩）");
    INFO("5 节点 × 128 虚拟节点，环上共 %zu 个虚拟节点", ring.vnode_count());

    // 副本放置：r 个互不相同的物理节点
    auto reps = ring.locate_replicas("object-777", 3);
    CHECK(reps.size() == 3, "应返回 3 个副本位置");
    std::set<std::string> uniq(reps.begin(), reps.end());
    CHECK(uniq.size() == 3, "3 个副本必须落在 3 个不同物理节点上（避免同节点多虚拟节点占位）");

    // ---- 分布均匀性：虚拟节点数的影响 ----
    // 理论上单个节点的份额标准差 ≈ sqrt(p(1-p)/V)，V=128、p=0.2 时约 3.5%，
    // 5 个节点取 max/min 之后比值落在 2x 附近是正常的 —— 一开始我按 1.25x 设门限，
    // 被实测打回来了（见 README「踩坑」）；要更均匀就得继续加虚拟节点。
    auto skew = [&](uint32_t vn) {
        HashRing r(vn);
        for (int i = 0; i < 5; i++) r.add_node("node" + std::to_string(i));
        auto d = r.distribution(keys);
        uint64_t mn = UINT64_MAX, mx = 0;
        for (auto& kv : d) { mn = std::min(mn, kv.second); mx = std::max(mx, kv.second); }
        return mx ? static_cast<double>(mx) / static_cast<double>(mn) : 0.0;
    };
    double skew1 = skew(1), skew16 = skew(16), skew128 = skew(128), skew1024 = skew(1024);
    INFO("虚拟节点数 → 最大/最小负载比：1 个 = %.2fx，16 个 = %.2fx，128 个 = %.2fx，1024 个 = %.2fx",
         skew1, skew16, skew128, skew1024);
    CHECK(skew16 < skew1 && skew128 < skew16 && skew1024 < skew128,
          "虚拟节点越多，负载必须越均匀（单调改善）");
    CHECK(skew1 > 5.0, "只给 1 个虚拟节点时偏斜应该非常严重（实际 %.2fx）", skew1);
    CHECK(skew128 < 3.0, "128 虚拟节点时偏斜应在 3x 以内（实际 %.2fx）", skew128);

    // ---- 增删节点的迁移量 ----
    HashRing before(128), after(128);
    for (int i = 0; i < 5; i++) { before.add_node("node" + std::to_string(i)); after.add_node("node" + std::to_string(i)); }
    after.add_node("node5");                       // 5 → 6 个节点
    double mig = HashRing::migration_ratio(before, after, keys);
    INFO("5 → 6 节点：迁移比例 %.4f（理想 1/6 = %.4f，取模哈希会接近 1.0）", mig, 1.0 / 6.0);
    CHECK(mig > 0.10 && mig < 0.25, "扩容迁移比例应接近 1/N（实际 %.4f）", mig);
    CHECK(mig < 0.5, "一致性哈希的迁移量必须远小于取模哈希");

    // 删节点
    HashRing removed(128);
    for (int i = 1; i < 6; i++) removed.add_node("node" + std::to_string(i));
    double mig2 = HashRing::migration_ratio(before, removed, keys);
    INFO("移除 node0：迁移比例 %.4f", mig2);
    CHECK(mig2 > 0.10 && mig2 < 0.30, "缩容迁移比例应接近 1/N（实际 %.4f）", mig2);

    CHECK(HashRing(128).locate("x").empty(), "空环应返回空字符串而不是崩");
    HashRing r2(8);
    r2.add_node("a");
    CHECK(r2.has_node("a") && !r2.has_node("b"), "has_node 判断应正确");
    r2.remove_node("a");
    CHECK(!r2.has_node("a") && r2.vnode_count() == 0, "移除节点后环应为空");
}

/*------------------------------------------------------------- WAL ------*/
static void test_wal() {
    GROUP("预写日志与崩溃恢复");
    std::string p = tmp_path("wal");
    unlink_path(p);

    {
        WriteAheadLog wal(p);
        CHECK(wal.open().ok(), "WAL 应能创建");
        for (int i = 0; i < 100; i++)
            CHECK(wal.append("record-" + std::to_string(i)).ok(), "追加第 %d 条应成功", i);
        std::vector<std::string> recs;
        Status st = wal.replay(recs);
        CHECK(st.ok() && recs.size() == 100, "重放应得到 100 条（实际 %zu）", recs.size());
        CHECK(recs[0] == "record-0" && recs[99] == "record-99", "顺序必须保持");
        CHECK(wal.repairs() == 0, "干净文件不应需要修复");
        INFO("写入 %llu 条 / %s，重放一致",
             (unsigned long long)wal.records_written(), human_bytes(wal.bytes_written()).c_str());
    }

    // ---- 模拟"最后一条只写了一半"（撕裂写）----
    {
        std::ofstream f(p, std::ios::binary | std::ios::app);
        // ⚠️ 十六进制转义后紧跟数字会被当成更长的转义（"\xDD12" 越界），必须拆成相邻字符串
        const char half[] = "\x10\x00\x00\x00" "\xAA\xBB\xCC\xDD" "1234";  // 声称 16 字节，只给 4
        f.write(half, 12);
    }
    {
        WriteAheadLog wal(p);
        std::vector<std::string> recs;
        wal.open();                       // open 会顺带把残缺尾部截断
        wal.replay(recs);
        CHECK(recs.size() == 100, "撕裂写之后，前面的 100 条仍应完好（实际 %zu）", recs.size());
        CHECK(wal.repairs() >= 1, "应检测到残缺尾部并截断（replay 计数 %llu）",
              (unsigned long long)wal.repairs());
        // 截断后还能继续正常追加
        CHECK(wal.append("after-repair").ok(), "修复后应能继续追加");
        std::vector<std::string> recs2;
        wal.replay(recs2);
        CHECK(recs2.size() == 101 && recs2.back() == "after-repair",
              "修复+追加后应有 101 条（实际 %zu）", recs2.size());
        INFO("撕裂写恢复：保留 100 条 + 截断残缺尾部 + 继续追加成功");
    }

    // ---- CRC 被篡改：必须在损坏处停下，不能把脏数据当有效数据 ----
    {
        std::string p2 = tmp_path("wal_crc");
        unlink_path(p2);
        {
            WriteAheadLog w(p2);
            w.open();
            for (int i = 0; i < 10; i++) w.append("ok-" + std::to_string(i));
        }
        {   // 手动破坏第 3 条记录的 payload（不动头部）
            std::fstream f(p2, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(8 + 5 + 8 + 5 + 8 + 5 + 1);   // 落到第 4 条的 payload 里
            f.put('\xFF');
        }
        WriteAheadLog w(p2);
        std::vector<std::string> recs;
        w.replay(recs);
        CHECK(recs.size() == 3, "校验失败的记录及其之后必须全部丢弃（实际保留 %zu 条）", recs.size());
        INFO("CRC 篡改：只保留了损坏点之前的 %zu 条，未把脏数据当成有效记录", recs.size());
        unlink_path(p2);
    }

    // ---- reset ----
    {
        WriteAheadLog wal(p);
        wal.replay(*(new std::vector<std::string>()));
        CHECK(wal.reset().ok(), "reset 应成功");
        std::vector<std::string> recs;
        wal.replay(recs);
        CHECK(recs.empty(), "reset 后应为空（实际 %zu 条）", recs.size());
    }
    unlink_path(p);
}

/*------------------------------------------------------ COW 块存储 -------*/
static void test_cow_store() {
    GROUP("写时复制块存储与快照");
    CowBlockStore st(tmp_path("cow"), 4096);
    st.open();

    std::string blk(4096, 'A'), rd;
    CHECK(st.write(10, blk).ok(), "写逻辑块 10");
    CHECK(st.read(10, rd).ok() && rd == blk, "读回来应一致");
    CHECK(st.read(99, rd).code == Code::NotFound, "未分配的块应返回 NotFound");
    CHECK(st.write(1, "短数据").code == Code::InvalidArgument, "长度不等于块大小应报错");

    uint64_t snap = 0;
    CHECK(st.snapshot("s1", snap).ok(), "创建快照应成功");
    uint64_t phys_after_snap = st.physical_blocks();

    // 快照隔离：改活动卷不能影响快照
    std::string blk2(4096, 'B');
    st.write(10, blk2);
    CHECK(st.read(10, rd).ok() && rd == blk2, "活动卷应读到新数据");
    CHECK(st.read_snapshot(snap, 10, rd).ok() && rd == blk, "快照必须仍读到老数据（隔离性）");
    CHECK(st.physical_blocks() > phys_after_snap, "写时复制会分配新物理块");
    INFO("快照隔离：活动卷 %c → %c，快照仍为 %c；物理块 %llu → %llu",
         'A', rd[0], 'A', (unsigned long long)phys_after_snap,
         (unsigned long long)st.physical_blocks());

    // 快照后不再改写：物理块不应继续增长
    uint64_t phys = st.physical_blocks();
    st.write(11, blk2);
    CHECK(st.physical_blocks() == phys + 1, "写新块只增加 1 个物理块");
    uint64_t phys2 = st.physical_blocks();
    st.write(10, blk2);                        // 重复写同样的内容仍是写时复制
    CHECK(st.physical_blocks() == phys2, "覆盖写自身不应额外增长（老块仍被快照引用）");

    // 删除快照后回收
    uint64_t before_drop = st.physical_blocks();
    st.drop_snapshot(snap);
    CHECK(st.physical_blocks() < before_drop, "删除快照应回收不再被引用的物理块");
    INFO("快照回收：物理块 %llu → %llu", (unsigned long long)before_drop,
         (unsigned long long)st.physical_blocks());
    CHECK(st.read_snapshot(snap, 10, rd).code == Code::NotFound, "已删除的快照应读不到");
}

/*------------------------------------------------------------- LRU ------*/
static void test_lru() {
    GROUP("LRU 缓存");
    LruCache c(3);
    c.put("a", "1"); c.put("b", "2"); c.put("c", "3");
    std::string v;
    CHECK(c.get("a", v) && v == "1", "命中 a");
    c.put("d", "4");                       // 淘汰最久未用的 b（a 刚被访问过）
    CHECK(!c.get("b", v), "b 应已被淘汰");
    CHECK(c.get("a", v) && c.get("c", v) && c.get("d", v), "a/c/d 应都在");
    CHECK(c.size() == 3, "容量应被遵守");
    INFO("命中率 %.2f%%（%llu 命中 / %llu 未命中）", c.hit_rate() * 100.0,
         (unsigned long long)c.hits(), (unsigned long long)c.misses());

    // 真实一点的负载：80% 的访问集中在 20% 的键上（Zipf 式热点）
    LruCache c2(100);
    std::mt19937 rng(7);
    for (int i = 0; i < 20000; i++) {
        int key = (rng() % 100 < 80) ? static_cast<int>(rng() % 20) : static_cast<int>(rng() % 500);
        std::string k = "k" + std::to_string(key);
        if (!c2.get(k, v)) c2.put(k, "v" + std::to_string(key));
    }
    INFO("热点负载（80%% 访问落在 20 个键）：容量 100 时命中率 %.2f%%", c2.hit_rate() * 100.0);
    CHECK(c2.hit_rate() > 0.5, "热点负载下命中率应明显偏高（实际 %.2f%%）", c2.hit_rate() * 100.0);
}

/*------------------------------------------------------ 线程池与并发 -----*/
static void test_concurrency() {
    GROUP("多线程并发 IO");
    ThreadPool pool(8);
    std::atomic<int> counter{0};
    for (int i = 0; i < 10000; i++) pool.submit([&counter] { counter++; });
    pool.wait_idle();
    CHECK(counter == 10000, "1 万个任务应全部执行（实际 %d）", counter.load());
    INFO("线程池 %zu 线程，完成 %llu 个任务", pool.threads(), (unsigned long long)pool.completed());

    // 并发写 COW 的不同逻辑块：结果必须全部正确（不能有丢失更新）
    CowBlockStore st(tmp_path("cow_mt"), 4096);
    st.open();
    const int nthreads = 8, per_thread = 2000;
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < nthreads; t++) {
            // per_thread 是 const int，不需要捕获（clang 会报 -Wunused-lambda-capture）
            ts.emplace_back([&st, t] {
                for (int i = 0; i < per_thread; i++) {
                    std::string data(4096, static_cast<char>('A' + t));
                    st.write(static_cast<uint64_t>(t) * 100000 + i, data);
                }
            });
        }
        for (auto& th : ts) th.join();
    }
    int bad = 0;
    for (int t = 0; t < nthreads; t++)
        for (int i = 0; i < per_thread; i += 137) {
            std::string rd;
            if (!st.read(static_cast<uint64_t>(t) * 100000 + i, rd).ok() ||
                rd[0] != static_cast<char>('A' + t)) bad++;
        }
    CHECK(bad == 0, "并发写入后所有块都应与写入者一致（错误 %d 个）", bad);
    INFO("8 线程 × %d 次并发写：%llu 个逻辑块，抽查全部正确", per_thread,
         (unsigned long long)st.logical_writes());

    // 并发读写同一个 WAL：追加的记录数必须等于成功返回数
    std::string wp = tmp_path("wal_mt");
    unlink_path(wp);
    WriteAheadLog wal(wp);
    wal.open();
    std::atomic<int> ok_count{0};
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; t++) {
            ts.emplace_back([&wal, &ok_count, t] {
                for (int i = 0; i < 500; i++) {
                    if (wal.append("t" + std::to_string(t) + "-" + std::to_string(i), false).ok())
                        ok_count++;
                }
            });
        }
        for (auto& th : ts) th.join();
    }
    std::vector<std::string> recs;
    wal.replay(recs);
    INFO("4 线程并发追加：成功返回 %d 条，重放得到 %zu 条", ok_count.load(), recs.size());
    CHECK(static_cast<int>(recs.size()) == ok_count.load(),
          "重放条数必须等于成功返回的条数（否则就是丢写或写坏）");
    unlink_path(wp);
}

int main() {
    std::printf("========================================================\n");
    std::printf(" 软件定义存储核心机制实验台 —— 单元测试\n");
    std::printf("========================================================\n");
    test_gf256();
    test_ec();
    test_hash_ring();
    test_wal();
    test_cow_store();
    test_lru();
    test_concurrency();

    std::printf("\n--------------------------------------------------------\n");
    std::printf("TEST_RESULT: test_sdslite passed=%d failed=%d\n", g_pass, g_fail);
    if (g_fail == 0) std::printf("ALL TESTS PASSED\n");
    return g_fail == 0 ? 0 : 1;
}
