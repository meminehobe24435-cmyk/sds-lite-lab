/*==============================================================================
 * ec_rs.cpp —— Reed-Solomon 纠删码（k + m）
 *
 * 为什么存储系统用 EC 而不是三副本：三副本要 3 倍空间，而 EC(4+2) 只要 1.5 倍，
 * 同样能容忍任意 2 块盘失效。代价是编码/解码要算，以及**重建时要读多个盘**。
 *
 * 实现要点（经典范德蒙德矩阵法）：
 *   编码矩阵是 n×k：
 *       前 k 行 = 单位阵        （数据分片原样落盘，不浪费算力）
 *       后 m 行 = 范德蒙德行    （A[i][j] = (i)^j，i 从 0 计数）
 *   校验分片 = 矩阵行 · 数据分片（GF(256) 上的乘加，加就是异或）
 *
 *   解码：任意 k 个存活分片组成的 k×k 子矩阵一定可逆（范德蒙德矩阵的任意
 *   k 行线性无关），求逆后乘回存活的 k 个分片，就还原出原始 k 个数据分片。
 *============================================================================*/
#include "sdslite.h"

#include <cstring>

namespace sds {

ReedSolomon::ReedSolomon(int k, int m) : k_(k), m_(m) {
    if (k <= 0 || m <= 0 || k + m > 255) {
        k_ = 1; m_ = 1;                   // 非法参数退化成 1+1，调用方应自己校验
    }
    const GF256& gf = GF256::instance();
    matrix_.assign(k_ + m_, std::vector<uint8_t>(k_, 0));
    for (int i = 0; i < k_; i++) matrix_[i][i] = 1;            // 单位阵
    for (int i = 0; i < m_; i++) {
        for (int j = 0; j < k_; j++) {
            // 范德蒙德：第 (k+i) 行第 j 列取 (k+i)^j；用 gf.pow 保证在域内
            matrix_[k_ + i][j] = gf.pow(static_cast<uint8_t>(k_ + i), j);
        }
    }
}

void ReedSolomon::matmul(const std::vector<std::vector<uint8_t>>& a,
                         const std::vector<std::vector<uint8_t>>& b,
                         std::vector<std::vector<uint8_t>>& out) const {
    const GF256& gf = GF256::instance();
    size_t rows = a.size(), inner = b.size(), cols = b.empty() ? 0 : b[0].size();
    out.assign(rows, std::vector<uint8_t>(cols, 0));
    for (size_t i = 0; i < rows; i++)
        for (size_t j = 0; j < cols; j++) {
            uint8_t acc = 0;
            for (size_t t = 0; t < inner; t++) {
                if (a[i][t] == 0 || b[t][j] == 0) continue;    // 跳过 0，省一次查表
                acc = static_cast<uint8_t>(acc ^ gf.mul(a[i][t], b[t][j]));
            }
            out[i][j] = acc;
        }
}

Status ReedSolomon::invert(const std::vector<std::vector<uint8_t>>& in,
                           std::vector<std::vector<uint8_t>>& out) const {
    size_t n = in.size();
    if (n == 0 || in[0].size() != n)
        return Status::Err(Code::InvalidArgument, "invert 需要方阵");
    const GF256& gf = GF256::instance();

    // 增广矩阵 [in | I]，做高斯-约当消元
    std::vector<std::vector<uint8_t>> aug(n, std::vector<uint8_t>(2 * n, 0));
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) aug[i][j] = in[i][j];
        aug[i][n + i] = 1;
    }
    for (size_t col = 0; col < n; col++) {
        size_t piv = col;
        while (piv < n && aug[piv][col] == 0) piv++;
        if (piv == n)
            return Status::Err(Code::InvalidArgument, "矩阵不可逆（分片组合非法）");
        if (piv != col) std::swap(aug[piv], aug[col]);
        uint8_t d = gf.inv(aug[col][col]);
        for (size_t j = 0; j < 2 * n; j++)
            aug[col][j] = gf.mul(aug[col][j], d);
        for (size_t r = 0; r < n; r++) {
            if (r == col) continue;
            uint8_t f = aug[r][col];
            if (f == 0) continue;
            for (size_t j = 0; j < 2 * n; j++)
                aug[r][j] = static_cast<uint8_t>(aug[r][j] ^ gf.mul(f, aug[col][j]));
        }
    }
    out.assign(n, std::vector<uint8_t>(n, 0));
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++) out[i][j] = aug[i][n + j];
    return Status::Ok();
}

Status ReedSolomon::encode(const std::vector<std::vector<uint8_t>>& data,
                           std::vector<std::vector<uint8_t>>& out) const {
    if (static_cast<int>(data.size()) != k_)
        return Status::Err(Code::InvalidArgument, "数据分片数必须等于 k");
    size_t len = data.empty() ? 0 : data[0].size();
    for (const auto& d : data)
        if (d.size() != len)
            return Status::Err(Code::InvalidArgument, "所有数据分片长度必须一致");

    out.assign(n(), std::vector<uint8_t>(len, 0));
    for (int i = 0; i < k_; i++) out[i] = data[i];       // 前 k 个就是数据本身

    const GF256& gf = GF256::instance();
    for (int i = 0; i < m_; i++) {
        const std::vector<uint8_t>& row = matrix_[k_ + i];
        std::vector<uint8_t>& dst = out[k_ + i];
        for (size_t b = 0; b < len; b++) {
            uint8_t acc = 0;
            for (int j = 0; j < k_; j++) {
                if (row[j] == 0) continue;
                acc = static_cast<uint8_t>(acc ^ gf.mul(row[j], data[j][b]));
            }
            dst[b] = acc;
        }
    }
    return Status::Ok();
}

Status ReedSolomon::decode(std::vector<std::vector<uint8_t>>& shards,
                           const std::vector<bool>& present,
                           std::vector<std::vector<uint8_t>>& out) const {
    if (static_cast<int>(shards.size()) != n() ||
        static_cast<int>(present.size()) != n())
        return Status::Err(Code::InvalidArgument, "分片数量与 n 不符");

    std::vector<int> alive;
    for (int i = 0; i < n(); i++)
        if (present[i]) alive.push_back(i);
    if (static_cast<int>(alive.size()) < k_)
        return Status::Err(Code::NotEnoughShards, "存活分片不足 k 个");

    size_t len = shards[alive[0]].size();

    // 全是数据分片：直接返回
    bool all_data = true;
    for (int i : alive) if (i >= k_) { all_data = false; break; }
    if (all_data && static_cast<int>(alive.size()) == k_) {
        out.assign(k_, {});
        for (int j = 0; j < k_; j++) out[j] = shards[alive[j]];
        return Status::Ok();
    }

    // 取前 k 个存活分片对应的矩阵行，组成 k×k 方阵并求逆
    std::vector<std::vector<uint8_t>> sub(k_, std::vector<uint8_t>(k_, 0));
    for (int r = 0; r < k_; r++) sub[r] = matrix_[alive[r]];
    std::vector<std::vector<uint8_t>> inv;
    Status st = invert(sub, inv);
    if (!st.ok()) return st;

    // 把存活的 k 个分片当作一个 k×len 的矩阵，乘上逆矩阵即得原始数据
    std::vector<std::vector<uint8_t>> alive_data(k_, std::vector<uint8_t>(len, 0));
    for (int r = 0; r < k_; r++) alive_data[r] = shards[alive[r]];
    matmul(inv, alive_data, out);
    return Status::Ok();
}

}  // namespace sds
