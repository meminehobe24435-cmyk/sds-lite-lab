/*==============================================================================
 * gf256.cpp —— GF(2^8) 有限域与公共工具
 *
 * 纠删码的全部运算都发生在有限域里：加法就是异或，乘法靠对数表。
 * 本原多项式取 0x11D（x^8 + x^4 + x^3 + x^2 + 1），生成元 2 —— 这是
 * Reed-Solomon 实现里最常用的一组参数（与 Linux RAID6、Jerasure 一致）。
 *============================================================================*/
#include "sdslite.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace sds {

/*-------------------------------------------------------------- 工具 -----*/

std::string Status::str() const {
    static const char* names[] = {"Ok", "NotFound", "Corrupted", "IoError",
                                  "InvalidArgument", "OutOfRange", "AlreadyExists",
                                  "CrcMismatch", "NotEnoughShards"};
    int idx = static_cast<int>(code);
    const char* n = (idx >= 0 && idx < 9) ? names[idx] : "Unknown";
    return msg.empty() ? std::string(n) : std::string(n) + ": " + msg;
}

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {                        // 首次调用建表（不是线程安全的热点，够用）
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string human_bytes(uint64_t n) {
    char buf[64];
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    double v = static_cast<double>(n);
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, u[i]);
    return buf;
}

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

/*------------------------------------------------------------ GF(256) ----*/

GF256::GF256() {
    // 用生成元 2 建指数/对数表；exp_ 长度取 512 以免乘法时越界回绕
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        exp_[i] = static_cast<uint8_t>(x);
        log_[x] = static_cast<uint8_t>(i);
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;      // 越过 8 位就模本原多项式
    }
    for (int i = 255; i < 512; i++) exp_[i] = exp_[i - 255];
    log_[0] = 0;                         // 0 没有对数，占位
}

const GF256& GF256::instance() {
    static GF256 inst;                   // C++11 起局部静态变量初始化线程安全
    return inst;
}

uint8_t GF256::mul(uint8_t a, uint8_t b) const {
    if (a == 0 || b == 0) return 0;
    return exp_[log_[a] + log_[b]];
}

uint8_t GF256::div(uint8_t a, uint8_t b) const {
    if (b == 0) return 0;
    if (a == 0) return 0;
    int d = static_cast<int>(log_[a]) - static_cast<int>(log_[b]);
    if (d < 0) d += 255;
    return exp_[d];
}

uint8_t GF256::pow(uint8_t a, int n) const {
    if (a == 0) return 0;
    if (n == 0) return 1;
    int l = (static_cast<int>(log_[a]) * (n % 255)) % 255;
    if (l < 0) l += 255;
    return exp_[l];
}

uint8_t GF256::inv(uint8_t a) const {
    if (a == 0) return 0;
    return exp_[255 - log_[a]];
}

uint8_t GF256::eval_poly(const std::vector<uint8_t>& coef, uint8_t x) const {
    // 霍纳法则
    uint8_t acc = 0;
    for (size_t i = coef.size(); i-- > 0;)
        acc = static_cast<uint8_t>(mul(acc, x) ^ coef[i]);
    return acc;
}

}  // namespace sds
