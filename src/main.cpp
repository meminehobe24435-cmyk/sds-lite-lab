/*==============================================================================
 * main.cpp —— 基准测试 + 崩溃注入
 *
 * 三种运行模式：
 *   --bench                 跑各项微基准（EC / 哈希环 / WAL / 缓存 / 并发 IO）
 *   --crash-after N         追加 N 条并 fsync 后**立即 _exit(9)**（模拟掉电）
 *   --torn-write N          追加 N 条后再**故意写半条**就掉电（撕裂写）
 *   --verify                重放 WAL 并打印恢复结果（给崩溃测试脚本判定用）
 *
 * `_exit` 而不是 `exit`：前者不做任何清理（不 flush、不析构），
 * 这才是"掉电"的语义；用 exit 会把缓冲区刷下去，测试就白做了。
 *============================================================================*/
#include "sdslite.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace sds;

static int mode_bench() {
    std::printf("# 软件定义存储核心机制 —— 基准测试\n");
    std::printf("# 全部数字为现场实测，环境为单机内存/本地文件系统\n\n");

    /* ---------------- 纠删码 ---------------- */
    for (auto km : {std::pair<int,int>{4,2}, {8,3}, {10,4}}) {
        ReedSolomon rs(km.first, km.second);
        const size_t chunk = 64 * 1024;
        std::vector<std::vector<uint8_t>> data(km.first, std::vector<uint8_t>(chunk, 0x5A));
        std::vector<std::vector<uint8_t>> coded;
        const int reps = 200;
        uint64_t t0 = now_us();
        for (int i = 0; i < reps; i++) rs.encode(data, coded);
        double enc_s = (now_us() - t0) / 1e6;

        std::vector<bool> present(km.first + km.second, true);
        present[0] = present[km.first] = false;      // 丢一个数据片 + 一个校验片
        std::vector<std::vector<uint8_t>> out;
        t0 = now_us();
        for (int i = 0; i < reps; i++) rs.decode(coded, present, out);
        double dec_s = (now_us() - t0) / 1e6;

        double bytes = static_cast<double>(chunk) * km.first * reps;
        std::printf("[EC %d+%d] 条带 %s/片\n", km.first, km.second,
                    human_bytes(chunk).c_str());
        std::printf("  编码吞吐 %.2f GB/s（%.3f ms/次）\n",
                    bytes / enc_s / 1e9, enc_s / reps * 1000);
        std::printf("  解码吞吐 %.2f GB/s（%.3f ms/次，恢复 %d 片）\n",
                    bytes / dec_s / 1e9, dec_s / reps * 1000, 2);
        std::printf("  空间开销 %.2fx（三副本为 3.00x）\n\n",
                    static_cast<double>(km.first + km.second) / km.first);
    }

    /* ---------------- 一致性哈希 ---------------- */
    {
        std::vector<std::string> keys;
        for (int i = 0; i < 200000; i++) keys.push_back("obj-" + std::to_string(i));
        HashRing r(128);
        for (int i = 0; i < 10; i++) r.add_node("node" + std::to_string(i));
        uint64_t t0 = now_us();
        volatile size_t sink = 0;
        for (const auto& k : keys) sink += r.locate(k).size();
        double s = (now_us() - t0) / 1e6;
        std::printf("[哈希环] 10 节点 × 128 虚拟节点\n");
        std::printf("  定位吞吐 %.2f 万次/秒（%.0f ns/次）\n", keys.size() / s / 1e4,
                    s / keys.size() * 1e9);
        HashRing r2 = r;
        r2.add_node("node10");
        double mig = HashRing::migration_ratio(r, r2, keys);
        std::printf("  10 → 11 节点迁移比例 %.4f（理想 %.4f）\n\n", mig, 1.0 / 11.0);
    }

    /* ---------------- WAL 写入 ---------------- */
    {
        std::string p = "./sdslite_bench_wal";
        ::unlink(p.c_str());
        const int N = 20000;
        std::string payload(256, 'x');
        WriteAheadLog wal(p);
        wal.open();
        uint64_t t0 = now_us();
        for (int i = 0; i < N; i++) wal.append(payload, /*durable=*/false);
        double s = (now_us() - t0) / 1e6;
        std::printf("[WAL] %d 条 × 256 B（不 fsync）\n", N);
        std::printf("  写入 %.0f 条/秒，%.2f MB/s\n", N / s, N * 264.0 / s / (1024 * 1024));

        t0 = now_us();
        for (int i = 0; i < 200; i++) wal.append(payload, /*durable=*/true);
        s = (now_us() - t0) / 1e6;
        std::printf("  带 fsync：%.0f 条/秒（%.0f us/次）—— 这才是「确认写入」的真实成本\n\n",
                    200 / s, s / 200 * 1e6);
        ::unlink(p.c_str());
    }

    /* ---------------- 缓存 ---------------- */
    {
        LruCache c(1024);
        std::string v;
        std::mt19937 rng(42);
        uint64_t t0 = now_us();
        for (int i = 0; i < 1000000; i++) {
            int key = (rng() % 100 < 90) ? static_cast<int>(rng() % 200)
                                         : static_cast<int>(rng() % 5000);
            std::string k = "k" + std::to_string(key);
            if (!c.get(k, v)) c.put(k, "value-" + std::to_string(key));
        }
        double s = (now_us() - t0) / 1e6;
        std::printf("[缓存] 容量 1024，100 万次访问（90%% 落在 200 个热点键）\n");
        std::printf("  命中率 %.2f%%，吞吐 %.2f 万次/秒\n\n", c.hit_rate() * 100.0,
                    100.0 / s);
    }

    /* ---------------- 并发写 ---------------- */
    {
        CowBlockStore st("./sdslite_bench_cow", 4096);
        st.open();
        for (int threads : {1, 2, 4, 8}) {
            ThreadPool pool(threads);
            const int per = 20000;
            std::string data(4096, 'Z');
            uint64_t t0 = now_us();
            for (int t = 0; t < threads; t++)
                pool.submit([&st, t, per, &data] {
                    for (int i = 0; i < per; i++)
                        st.write(static_cast<uint64_t>(t) * 1000000 + i, data);
                });
            pool.wait_idle();
            double s = (now_us() - t0) / 1e6;
            uint64_t ops = static_cast<uint64_t>(threads) * per;
            std::printf("[并发块写] %d 线程：%.2f 万 IOPS，%.2f GB/s\n", threads,
                        ops / s / 1e4, ops * 4096.0 / s / 1e9);
        }
        std::printf("\n");
    }

    return 0;
}

static int mode_verify(const std::string& path) {
    WriteAheadLog wal(path);
    std::vector<std::string> recs;
    Status st = wal.replay(recs);
    if (!st.ok()) {
        std::printf("VERIFY_FAIL reason=%s\n", st.str().c_str());
        return 1;
    }
    // 记录必须是 1..N 的连续前缀：序号不能有洞、不能重复、不能乱序
    bool prefix_ok = true;
    for (size_t i = 0; i < recs.size(); i++) {
        std::string expect = "rec-" + std::to_string(i);
        if (recs[i] != expect) { prefix_ok = false; break; }
    }
    std::printf("VERIFY_RECORDS=%zu\n", recs.size());
    std::printf("VERIFY_REPAIRS=%llu\n", (unsigned long long)wal.repairs());
    std::printf("VERIFY_PREFIX_OK=%d\n", prefix_ok ? 1 : 0);
    // 退出码只表示"结构完整、能正常重放"；「是否构成 1..N 连续前缀」由调用方按场景判断
    // （多次运行追加到同一个日志时，序号会重新开始，本就不该是全局前缀）
    return 0;
}

int main(int argc, char** argv) {
    std::string path = "./sdslite_crash_wal";

    if (argc >= 2 && std::strcmp(argv[1], "--bench") == 0) return mode_bench();
    if (argc >= 3 && std::strcmp(argv[1], "--verify") == 0) return mode_verify(argv[2]);

    bool torn = false;
    int crash_after = -1;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--crash-after") == 0) crash_after = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--torn-write") == 0) {
            crash_after = std::atoi(argv[i + 1]);
            torn = true;
        }
        if (std::strcmp(argv[i], "--wal") == 0) path = argv[i + 1];
    }

    if (crash_after > 0) {
        WriteAheadLog wal(path);
        if (!wal.open().ok()) { std::printf("OPEN_FAIL\n"); return 2; }
        for (int i = 0; i < crash_after; i++) {
            std::string payload = "rec-" + std::to_string(i);
            Status s = wal.append(payload, /*durable=*/true);   // 每条都 fsync：确认即持久
            if (!s.ok()) { std::printf("APPEND_FAIL %d\n", i); return 3; }
        }
        std::fflush(nullptr);
        if (torn) {
            // 故意写一条"头部完整、数据不全"的记录，然后掉电 —— 模拟写放大/掉电撕裂
            int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_BINARY);
            if (fd >= 0) {
                const unsigned char half[12] = {0x40, 0x00, 0x00, 0x00,   // 声称 64 字节
                                                0xDE, 0xAD, 0xBE, 0xEF,   // 随便一个 CRC
                                                'h', 'a', 'l', 'f'};      // 只有 4 字节数据
                ssize_t w = ::write(fd, half, sizeof(half));
                (void)w;
                ::close(fd);
            }
        }
        // 关键：用 _exit 直接终止，不走任何析构 / 不 flush stdio 缓冲
        _exit(9);
    }

    std::printf("用法: %s --bench | --crash-after N [--wal P] | --torn-write N [--wal P] | --verify P\n",
                argv[0]);
    return 0;
}
