#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""崩溃一致性测试：反复"写一半就掉电"，重启后验证已确认写入的数据没丢。

这是存储系统里最不能靠"看起来对"来糊弄的一条性质，所以做法是：
  1. 起一个子进程，让它**逐条 fsync 写入**，写到第 N 条时**直接 _exit(9)**（不做任何清理）；
  2. 撕裂写模式再额外写一条"头部完整、数据不全"的记录才掉电；
  3. 另起进程重新打开日志做恢复，检查：
       - 恢复出来的记录必须是 1..M 的**连续前缀**（不能有洞、不能重复、不能乱序）；
       - **凡是已经 fsync 确认过的记录必须全部在**（N 条一条不少）；
       - 残缺尾部必须被检测到并截断；
       - 恢复后还能继续写入并再次正确读出。

N 从小到大扫一遍，覆盖"掉电正好落在不同写入阶段"的各种情况。
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile


def run(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def parse_verify(out: str) -> dict:
    vals = {}
    for key in ("VERIFY_RECORDS", "VERIFY_REPAIRS", "VERIFY_PREFIX_OK"):
        m = re.search(rf"{key}=(-?\d+)", out)
        vals[key] = int(m.group(1)) if m else None
    return vals


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./sdslite")
    ap.add_argument("--max-n", type=int, default=40)
    ap.add_argument("--keep", action="store_true", help="保留临时目录便于排查")
    args = ap.parse_args()

    binary = os.path.abspath(args.bin)
    if not os.path.isfile(binary):
        print(f"找不到可执行文件: {binary}")
        return 2

    tmpdir = tempfile.mkdtemp(prefix="sdslite_crash_")
    wal = os.path.join(tmpdir, "wal.log")

    total = 0
    failed = 0
    repaired_cases = 0
    max_recovery_ns = 0

    for torn in (False, True):
        label = "撕裂写（写到一半掉电）" if torn else "干净掉电（fsync 后立刻退出）"
        print(f"\n=== {label} ===")
        for n in range(1, args.max_n + 1):
            total += 1
            if os.path.exists(wal):
                os.unlink(wal)

            flag = "--torn-write" if torn else "--crash-after"
            rc, out = run([binary, flag, str(n), "--wal", wal])

            # 掉电进程必须以非 0 退出（_exit(9)），否则说明没真崩
            if rc == 0:
                print(f"  [FAIL] N={n}: 掉电进程竟然正常退出（rc=0），测试无效")
                failed += 1
                continue

            rc2, out2 = run([binary, "--verify", wal])
            v = parse_verify(out2)
            recs = v["VERIFY_RECORDS"]
            repairs = v["VERIFY_REPAIRS"]

            ok = True
            reason = ""
            # 1) 已确认写入的 N 条一条不能少
            if recs is None or recs < n:
                ok, reason = False, f"恢复出 {recs} 条 < 已确认 {n} 条（丢数据！）"
            # 2) 必须是连续前缀
            elif v["VERIFY_PREFIX_OK"] != 1:
                ok, reason = False, "恢复出的记录不是连续前缀（有洞/重复/乱序）"
            # 3) 撕裂写必须被发现并截断
            elif torn and (repairs is None or repairs < 1):
                ok, reason = False, "残缺尾部没有被检测到（repairs=0）"
            # 4) 干净掉电不该产生"修复"
            elif (not torn) and repairs not in (0, None):
                ok, reason = False, f"干净掉电却报告了 {repairs} 次修复"
            # 5) verify 进程本身要成功
            elif rc2 != 0:
                ok, reason = False, f"verify 退出码 {rc2}"

            if repairs:
                repaired_cases += 1
            if not ok:
                failed += 1
                print(f"  [FAIL] N={n}: {reason}")
                if not args.keep:
                    pass
            elif n in (1, args.max_n // 2, args.max_n):
                print(f"  [ OK ] N={n}: 恢复 {recs} 条（已确认 {n} 条全在），"
                      f"修复 {repairs if repairs is not None else 0} 次")

    # ---- 恢复之后必须还能继续正常工作 ----
    # 第二次运行时记录序号会从 0 重新开始（payload 是 rec-0、rec-1 …），
    # 所以这里**不检查"连续前缀"**（那是单次运行的语义），只检查：
    #   - 恢复过的日志仍能被打开、且没有损坏；
    #   - 追加的条数确实是"原来 7 条 + 再加 5 条"；
    #   - 第一次崩溃前确认的 7 条一条都没丢。
    print("\n=== 恢复后继续写入 ===")
    if os.path.exists(wal):
        os.unlink(wal)
    run([binary, "--crash-after", "7", "--wal", wal])
    rc, _ = run([binary, "--crash-after", "5", "--wal", wal])
    rc2, out2 = run([binary, "--verify", wal])
    v = parse_verify(out2)
    recs = v["VERIFY_RECORDS"]
    total += 1
    if rc2 == 0 and recs == 12:
        print(f"  [ OK ] 崩溃后重建并继续追加：恢复 {recs} 条（7 条老数据 + 5 条新数据）")
    else:
        failed += 1
        print(f"  [FAIL] 恢复后继续写入失败：rc={rc2} records={recs}（期望 12）")

    print("\n--------------------------------------------------------")
    print(f"TEST_RESULT: crash_test cases={total} failed={failed} "
          f"torn_repaired={repaired_cases}")
    if failed == 0:
        print("ALL TESTS PASSED")
    if not args.keep:
        for f in os.listdir(tmpdir):
            os.unlink(os.path.join(tmpdir, f))
        os.rmdir(tmpdir)
    else:
        print(f"临时目录保留在: {tmpdir}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
