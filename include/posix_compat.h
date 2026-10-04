/*==============================================================================
 * posix_compat.h —— 让 POSIX 存储代码在 Windows 上也能编译跑测试
 *
 * ⚠️ **目标平台是 Linux**：预写日志用的 fsync / pread / ftruncate 都是 POSIX 接口，
 * 生产实现也只会跑在 Linux 服务器上。
 *
 * 这里加一层极小的垫片，只是为了让开发者在 Windows 上也能 `make test`，
 * 而不是为了宣称"跨平台存储"：
 *   - fsync    → _commit
 *   - ftruncate→ _chsize_s
 *   - pread    → _lseeki64 + _read（**会改动文件偏移，多线程下等价实现并不成立**，
 *                所以 Windows 上只保证单线程测试可用；Linux 上是真正的 pread）
 *============================================================================*/
#ifndef SDSLITE_POSIX_COMPAT_H
#define SDSLITE_POSIX_COMPAT_H

#ifdef _WIN32

#include <io.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#ifndef O_APPEND
#define O_APPEND _O_APPEND
#endif
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif

/* ⚠️ 必须显式用二进制模式：Windows 的 CRT 默认按**文本模式**打开文件，
 *    遇到字节 0x1A（Ctrl-Z）会当成 EOF、还会做 CRLF 转换。
 *    存储代码写的是任意二进制，一旦踩上这条，WAL 重放会在第一个含 0x1A 的
 *    记录处提前结束（本项目实测只恢复出 100 条里的 18 条）。
 *    Linux 上 O_BINARY 不存在，定义为 0 即可。 */
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif

#ifndef _SSIZE_T_DEFINED
typedef long long ssize_t_;
#endif

using ssize_t_compat = long long;

/** Windows 没有 fsync，用 _commit（刷新到磁盘）等价替代。 */
inline int fsync(int fd) { return _commit(fd); }

/** Windows 没有 ftruncate。 */
inline int ftruncate(int fd, long long len) { return _chsize_s(fd, len); }

/** Windows 没有 pread：用 seek + read 模拟（非原子，单线程可用）。 */
inline long long pread(int fd, void* buf, size_t count, long long offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return static_cast<long long>(_read(fd, buf, static_cast<unsigned>(count)));
}

#else

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

/* POSIX 下没有 O_BINARY（本来就都是二进制），补一个 0 让调用处写法统一 */
#ifndef O_BINARY
#define O_BINARY 0
#endif

#endif  // _WIN32

#endif  // SDSLITE_POSIX_COMPAT_H
