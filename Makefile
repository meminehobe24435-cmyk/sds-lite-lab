# 软件定义存储核心机制实验台（sds-lite-lab）
# C++17、零第三方依赖：一条 make 命令构建 + 跑全部测试 + 基准 + 崩溃注入

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS  ?= -pthread
INC       = -Iinclude

SRC = src/gf256.cpp src/ec_rs.cpp src/hash_ring.cpp src/wal.cpp src/cow_store.cpp

.PHONY: all test bench crash clean asan tsan

all: sdslite_test sdslite

sdslite_test: $(SRC) tests/test_sdslite.cpp include/sdslite.h
	$(CXX) $(CXXFLAGS) $(INC) $(SRC) tests/test_sdslite.cpp $(LDFLAGS) -o $@

sdslite: $(SRC) src/main.cpp include/sdslite.h
	$(CXX) $(CXXFLAGS) $(INC) $(SRC) src/main.cpp $(LDFLAGS) -o $@

test: sdslite_test
	@./sdslite_test

bench: sdslite
	@./sdslite --bench

# 崩溃一致性：反复"写一半就掉电"，再重启验证已确认写入的数据没丢
crash: sdslite
	@python3 tools/crash_test.py --bin ./sdslite || python tools/crash_test.py --bin ./sdslite

# 用 AddressSanitizer / UBSan 重跑：存储代码最容易出的就是越界与未定义行为
asan: CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
asan: clean
	$(CXX) $(CXXFLAGS) $(INC) $(SRC) tests/test_sdslite.cpp $(LDFLAGS) -o sdslite_test_asan
	@./sdslite_test_asan

# 用 ThreadSanitizer 重跑并发用例：查数据竞争
tsan: CXXFLAGS += -fsanitize=thread -O1
tsan: clean
	$(CXX) $(CXXFLAGS) $(INC) $(SRC) tests/test_sdslite.cpp $(LDFLAGS) -o sdslite_test_tsan
	@./sdslite_test_tsan

clean:
	@rm -f sdslite sdslite_test sdslite_test_asan sdslite_test_tsan \
	       sdslite.exe sdslite_test.exe
