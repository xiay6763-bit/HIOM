#pragma once

#include <iostream>
#include <string>
#include <filesystem>
#include <random>
#include <mutex>

#include <benchmark/benchmark.h>
#include <libpmempool.h>
#include <libpmemobj++/pool.hpp>
#include <hdr_histogram.h>
#include <thread>

#include "../benchmark.hpp"
#include "ycsb_common.hpp"

namespace viper::kv_bm {

// 针对r750-withpm 服务器优化的 CPU 列表
static constexpr std::array CPUS {
    // 1. 最优先：Node 0 的物理核心 (0-25) -> 性能最强，访问 PMem 最快
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 
    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,

    // 2. 次优先：Node 0 的逻辑核心/超线程 (52-77) -> 还在本地，但共享物理核
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77,

    // 3. 迫不得已：Node 1 的物理核心 (26-51) -> 跨 NUMA，慢
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
    39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51

    // (不需要填满所有，只要覆盖你的测试最大线程数即可)
};

bool is_init_thread(const benchmark::State& state);

void set_cpu_affinity();
void set_cpu_affinity(uint16_t thread_idx);
void set_cpu_affinity(uint16_t from, uint16_t to);

std::string random_file(const std::filesystem::path& base_dir);

using VarSizeKVs = std::pair<std::vector<std::string>, std::vector<std::string>>;

void zero_block_device(const std::string& block_dev, size_t length);

class BaseFixture : public benchmark::Fixture {
  public:
    void SetUp(benchmark::State& state) override {}
    void TearDown(benchmark::State& state) override {}

    virtual void InitMap(const uint64_t num_prefill_inserts = 0, const bool re_init = true) {};
    virtual void DeInitMap() {};

    template <typename PrefillFn>
    void prefill_internal(size_t num_prefills, PrefillFn prefill_fn);

    void prefill(size_t num_prefills);
    virtual void prefill_ycsb(const std::vector<ycsb::Record>& data);

    void generate_strings(size_t num_strings, size_t key_size, size_t value_size);

    // Benchmark methods. All pure virtual.
    virtual uint64_t setup_and_insert(uint64_t start_idx, uint64_t end_idx) = 0;
    virtual uint64_t setup_and_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates) = 0;
    virtual uint64_t setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds) = 0;
    virtual uint64_t setup_and_delete(uint64_t start_idx, uint64_t end_idx, uint64_t num_deletes) = 0;

    virtual uint64_t run_ycsb(uint64_t start_idx, uint64_t end_idx,
                              const std::vector<ycsb::Record>& data, hdr_histogram* hdr) {
        throw std::runtime_error("YCSB not implemented");
    }

    void merge_hdr(hdr_histogram* other) {
        std::lock_guard lock{hdr_lock_};
        hdr_add(hdr_, other);
    }

    hdr_histogram* get_hdr() { return hdr_; }
    hdr_histogram* hdr_ = nullptr;

    static void log_find_count(benchmark::State& state, const uint64_t num_found, const uint64_t num_expected);

  protected:
    virtual uint64_t insert(uint64_t start_idx, uint64_t end_idx) = 0;

    std::mutex hdr_lock_;
    size_t num_util_threads_ = NUM_UTIL_THREADS;

    static VarSizeKVs var_size_kvs_;
};

template <typename RootType>
class BasePmemFixture : public BaseFixture {
  public:
    void SetUp(benchmark::State& state) override {
        BaseFixture::SetUp(state);
        int sds_write_value = 0;
        pmemobj_ctl_set(NULL, "sds.at_create", &sds_write_value);

        {
            std::scoped_lock lock(pool_mutex_);
            if (pool_file_.empty()) {
                pool_file_ = random_file(DB_PMEM_DIR);
                // std::cout << "Working on NVM file " << pool_file_ << std::endl;
                pmem_pool_ = pmem::obj::pool<RootType>::create(pool_file_, "", BM_POOL_SIZE, S_IRWXU);
            }
        }
    }

//    this is called and pool is closed but viper still points to something
    void TearDown(benchmark::State& state) override {
        {
            std::scoped_lock lock(pool_mutex_);
            if (!pool_file_.empty() && std::filesystem::exists(pool_file_)) {
                pmem_pool_.close();
                if (pmempool_rm(pool_file_.c_str(), PMEMPOOL_RM_FORCE | PMEMPOOL_RM_POOLSET_LOCAL) == -1) {
                    std::cout << pmempool_errormsg() << std::endl;
                }
                pool_file_.clear();
            }
        }
        BaseFixture::TearDown(state);
    }

  protected:
    pmem::obj::pool<RootType> pmem_pool_;
    std::filesystem::path pool_file_;
    std::mutex pool_mutex_;
};

}  // namespace viper::kv_bm
