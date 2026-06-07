#pragma once

#include "viper/cceh.hpp"
#include "common_fixture.hpp"
#include "../benchmark.hpp"
#include <libpmemobj++/allocator.hpp>
#include <libpmemobj++/transaction.hpp>
#include <libpmemobj++/make_persistent.hpp>
#include <libpmemobj++/container/vector.hpp>

namespace viper::kv_bm {

template <typename KeyT = KeyType8, typename ValueT = ValueType8>
class CcehFixture : public BaseFixture {
    using Entry = std::pair<KeyT, ValueT>;
    using EntryVector = pmem::obj::vector<pmem::obj::persistent_ptr<Entry>>;

    struct CcehPool {
        pmem::obj::persistent_ptr<EntryVector> ptrs;
    };

  public:
    void InitMap(const uint64_t num_prefill_inserts, const bool re_init) final;
    void DeInitMap() final;
    uint64_t setup_and_insert(uint64_t start_idx, uint64_t end_idx) final;
    uint64_t setup_and_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates);
    uint64_t setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds);
    uint64_t setup_and_delete(uint64_t start_idx, uint64_t end_idx, uint64_t num_deletes);
    uint64_t run_ycsb(uint64_t start_idx, uint64_t end_idx, const std::vector<ycsb::Record>& data,
                      LatencyHistograms hdrs) final;
    uint64_t insert(uint64_t start_idx, uint64_t end_idx) final;
    void prefill_ycsb(const std::vector<ycsb::Record>& data) override;

    // White-box index DRAM for E1: the DRAM-resident CCEH directory + unique
    // segments (the value store lives in PM, ~0 DRAM). Mirrors ViperFixture's
    // cceh_dram_bytes() so DRAM-CCEH appears in the E1 DRAM comparison instead
    // of reporting 0.
    size_t fixture_dram_bytes() override {
        return dram_map_ ? dram_map_->dram_bytes() : 0;
    }

  protected:
    std::unique_ptr<cceh::CCEH<KeyT>> dram_map_;
    pmem::obj::pool<CcehPool> pmem_pool_;
    EntryVector* ptrs_;
    std::atomic<size_t> pool_vector_pos_;
    std::string pmem_pool_name_;
    bool map_initialized_ = false;

    bool insert_internal(const KeyT& key, const ValueT& value);
};

template <typename KeyT, typename ValueT>
void CcehFixture<KeyT, ValueT>::InitMap(const uint64_t num_prefill_inserts, const bool re_init) {
    if (map_initialized_ && !re_init) {
        return;
    }

    pmem_pool_name_ = random_file(DB_PMEM_DIR);
    int sds_write_value = 0;
    pmemobj_ctl_set(NULL, "sds.at_create", &sds_write_value);
    // Value store (Entry=(key,value) in a PM vector; DRAM CCEH holds offsets).
    // 24 GiB ~= tens of millions K8/V200; was 80 GiB (100M-paper over-provision,
    // unsafe on shared /pmem0 which FS-DAX fully allocates on create).
    pmem_pool_ = pmem::obj::pool<CcehPool>::create(pmem_pool_name_, "", 24ul * ONE_GB, S_IRWXU);
    if (pmem_pool_.handle() == nullptr) {
        throw std::runtime_error("Could not create pool");
    }
    pmem::obj::transaction::run(pmem_pool_, [&] {
        pmem_pool_.root()->ptrs = pmem::obj::make_persistent<EntryVector>();
        ptrs_ = pmem_pool_.root()->ptrs.get();
        ptrs_->resize(num_prefill_inserts * 2);
    });

    pool_vector_pos_ = 0;
    // Match Viper's CCEH init capacity (ViperConfig::cceh_init_cap = 131072 =
    // 2^17 segments x 16 KiB ~= 2 GiB) so DRAM-CCEH is a fair DRAM-index peer to
    // Viper, not an over-provisioned 2^19 (~8 GiB) outlier. The ctor arg is
    // initCap with directory depth = log2(initCap), so 1000000 silently became
    // depth 19 (524288 segments); 131072 gives depth 17, identical to Viper.
    dram_map_ = std::make_unique<cceh::CCEH<KeyT>>(131072);
    prefill(num_prefill_inserts);
    map_initialized_ = true;
}

template <typename KeyT, typename ValueT>
void CcehFixture<KeyT, ValueT>::DeInitMap() {
    pmem_pool_.close();
    pmempool_rm(pmem_pool_name_.c_str(), 0);
    dram_map_ = nullptr;
    map_initialized_ = false;
}

template <typename KeyT, typename ValueT>
inline bool CcehFixture<KeyT, ValueT>::insert_internal(const KeyT& key, const ValueT& value) {
    block_size_t ptrs_pos;
    pmem::obj::persistent_ptr<Entry> offset_ptr;
    pmem::obj::transaction::run(pmem_pool_, [&] {
        offset_ptr = pmem::obj::make_persistent<Entry>(key, value);
        ptrs_pos = pool_vector_pos_.fetch_add(1);
        (*ptrs_)[ptrs_pos] = offset_ptr;
    });
    KeyValueOffset offset{ptrs_pos, 0, 0};
    KeyValueOffset old_offset = dram_map_->Insert(key, offset);
    return old_offset.is_tombstone();
}

template <typename KeyT, typename ValueT>
uint64_t CcehFixture<KeyT, ValueT>::insert(uint64_t start_idx, uint64_t end_idx) {
    uint64_t insert_counter = 0;
    for (uint64_t pos = start_idx; pos < end_idx; ++pos) {
        const KeyT db_key{pos};
        const ValueT value{pos};
        insert_counter += insert_internal(db_key, value);
    }
    return insert_counter;
}

template <>
uint64_t CcehFixture<std::string, std::string>::insert(uint64_t start_idx, uint64_t end_idx) {
    uint64_t insert_counter = 0;
    const std::vector<std::string>& keys = std::get<0>(var_size_kvs_);
    const std::vector<std::string>& values = std::get<1>(var_size_kvs_);
    for (uint64_t pos = start_idx; pos < end_idx; ++pos) {
        const std::string& db_key = keys[pos];
        const std::string& value = values[pos];
        insert_counter += insert_internal(db_key, value);
    }
    return insert_counter;
}

template <typename KeyT, typename ValueT>
uint64_t CcehFixture<KeyT, ValueT>::setup_and_insert(uint64_t start_idx, uint64_t end_idx) {
    return insert(start_idx, end_idx);
}

template <typename KeyT, typename ValueT>
uint64_t CcehFixture<KeyT, ValueT>::setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto key_check_fn = [this](const KeyT& key, IndexV offset) {
        block_size_t entry_ptr_pos = offset.block_number;
        const pmem::obj::persistent_ptr<Entry>& entry_ptr = (*ptrs_)[entry_ptr_pos];
        return key == entry_ptr->first;
    };

    uint64_t found_counter = 0;
    for (uint64_t i = 0; i < num_finds; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyValueOffset offset = dram_map_->Get(key, key_check_fn);
        if (!offset.is_tombstone()) {
            block_size_t entry_ptr_pos = offset.block_number;
            pmem::obj::persistent_ptr<Entry> entry_ptr = (*ptrs_)[entry_ptr_pos];
            ValueT found_val = entry_ptr->second;
            found_counter += (found_val.data[0] == key);
        }
    }
    return found_counter;
}

template <>
uint64_t CcehFixture<std::string, std::string>::setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto key_check_fn = [this](const std::string& key, IndexV offset) {
        block_size_t entry_ptr_pos = offset.block_number;
        pmem::obj::persistent_ptr<Entry> entry_ptr = (*ptrs_)[entry_ptr_pos];
        return key == entry_ptr->first;
    };

    const std::vector<std::string>& keys = std::get<0>(var_size_kvs_);
    const std::vector<std::string>& values = std::get<1>(var_size_kvs_);

    uint64_t found_counter = 0;
    for (uint64_t i = 0; i < num_finds; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const std::string& db_key = keys[key];
        const std::string& value = values[key];
        const KeyValueOffset offset = dram_map_->Get(db_key, key_check_fn);
        if (!offset.is_tombstone()) {
            block_size_t entry_ptr_pos = offset.block_number;
            pmem::obj::persistent_ptr<Entry> entry_ptr = (*ptrs_)[entry_ptr_pos];
            found_counter += (entry_ptr->second == value);
        }
    }
    return found_counter;
}

template <typename KeyT, typename ValueT>
uint64_t CcehFixture<KeyT, ValueT>::setup_and_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto key_check_fn = [this](const KeyT& key, IndexV offset) {
        block_size_t entry_ptr_pos = offset.block_number;
        pmem::obj::persistent_ptr<Entry> entry_ptr = (*ptrs_)[entry_ptr_pos];
        return key == entry_ptr->first;
    };

    uint64_t update_counter = 0;
    for (uint64_t i = 0; i < num_updates; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyT db_key{key};
        const KeyValueOffset offset = dram_map_->Get(db_key, key_check_fn);
        if (!offset.is_tombstone()) {
            block_size_t entry_ptr_pos = offset.block_number;
            pmem::obj::persistent_ptr<Entry> entry_ptr = (*ptrs_)[entry_ptr_pos];
            ValueT& value = entry_ptr->second;
            value.update_value();
            pmem_persist(&value, sizeof(uint64_t));
            update_counter++;
        }
    }
    return update_counter;
}

template <>
uint64_t CcehFixture<std::string, std::string>::setup_and_update(uint64_t, uint64_t, uint64_t) { return 0; }

template <typename KeyT, typename ValueT>
uint64_t CcehFixture<KeyT, ValueT>::setup_and_delete(uint64_t start_idx, uint64_t end_idx, uint64_t num_deletes) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto key_check_fn = [this](const KeyT& key, IndexV offset) {
        block_size_t entry_ptr_pos = offset.block_number;
        const pmem::obj::persistent_ptr<Entry>& entry_ptr = (*ptrs_)[entry_ptr_pos];
        return !!entry_ptr && key == entry_ptr->first;
    };

    uint64_t delete_counter = 0;
    for (uint64_t i = 0; i < num_deletes; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyT db_key{key};
        const KeyValueOffset offset = dram_map_->Get(db_key, key_check_fn);
        if (!offset.is_tombstone()) {
            block_size_t entry_ptr_pos = offset.block_number;
            pmem::obj::persistent_ptr<Entry> entry_ptr = (*ptrs_)[entry_ptr_pos];
            pmem::obj::transaction::run(pmem_pool_, [&] {
                pmem::obj::delete_persistent<Entry>(entry_ptr);
            });
            (*ptrs_)[entry_ptr_pos] = pmem::obj::persistent_ptr<Entry>();
            dram_map_->Insert(key, IndexV::NONE(), key_check_fn);
            delete_counter++;
        }
    }
    return delete_counter;
}

template <>
uint64_t CcehFixture<std::string, std::string>::setup_and_delete(uint64_t, uint64_t, uint64_t) { return 0; }

template <typename KeyT, typename ValueT>
uint64_t CcehFixture<KeyT, ValueT>::run_ycsb(uint64_t, uint64_t, const std::vector<ycsb::Record>&, LatencyHistograms) {
    throw std::runtime_error{"YCSB not implemented for non-ycsb key/value types."};
}

template <>
uint64_t CcehFixture<KeyType8, ValueType200>::run_ycsb(uint64_t start_idx,
    uint64_t end_idx, const std::vector<ycsb::Record>& data, LatencyHistograms hdrs) {
    // Single-HDR fallback for non-active fixtures (Viper/HiOM do per-op split).
    hdr_histogram* hdr = hdrs.write ? hdrs.write : hdrs.read;

    const ValueType200 null_value{0ul};
    uint64_t op_count = 0;
    for (int op_num = start_idx; op_num < end_idx; ++op_num) {
        const ycsb::Record& record = data[op_num];

        const auto start = std::chrono::high_resolution_clock::now();

        switch (record.op) {
            case ycsb::Record::Op::INSERT: {
                insert_internal(record.key, record.value);
                op_count++;
                break;
            }
            case ycsb::Record::Op::GET: {
                const KeyValueOffset offset = dram_map_->Get(record.key);
                if (!offset.is_tombstone()) {
                    block_size_t entry_ptr_pos = offset.block_number;
                    pmem::obj::persistent_ptr<Entry> entry_ptr = (*ptrs_)[entry_ptr_pos];
                    // YCSB READ records carry no value, so the previous
                    // `== record.value` check was always false → found=0.
                    // Match Viper/Dash run_ycsb: a hit is a non-tombstone
                    // offset whose stored value is non-null.
                    op_count += (entry_ptr->second != null_value);
                }
                break;
            }
            case ycsb::Record::Op::UPDATE: {
                insert_internal(record.key, record.value);
                op_count++;
                break;
            }
            default: {
                throw std::runtime_error("Unknown operation: " + std::to_string(record.op));
            }
        }

        if (hdr == nullptr) {
            continue;
        }

        const auto end = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        hdr_record_value(hdr, duration.count());
    }

    return op_count;
}

template <typename KeyT, typename ValueT>
void CcehFixture<KeyT, ValueT>::prefill_ycsb(const std::vector<ycsb::Record>& data) {
    ptrs_->resize(data.size() * 2);
    BaseFixture::prefill_ycsb(data);
}

}  // namespace
