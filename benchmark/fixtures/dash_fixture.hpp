#pragma once

#include "ex_finger.h"
#include "common_fixture.hpp"
#include "pmem_value_slab.hpp"
#include "../benchmark.hpp"
#include <libpmemobj++/allocator.hpp>
#include <libpmemobj++/transaction.hpp>
#include <libpmemobj++/make_persistent.hpp>

namespace viper::kv_bm {

struct DashVarKeyData {
    std::array<char, 32> data;
};

struct DashVarValue {
    size_t length;
    std::array<char, 400> data;

    inline void from_string(const std::string& value) {
        length = value.length();
        memcpy(data.data(), value.data(), value.length());
    }
};

template <typename KeyT>
struct DashVarKey {
    string_key str_k;
    KeyT key;

    inline static DashVarKey<KeyT> from_string(const std::string& key) {
        if constexpr (std::is_same_v<KeyT, DashVarKeyData>) {
            DashVarKey<KeyT> var_key;
            var_key.str_k.length = key.length();
            memcpy(&var_key.key.data, key.data(), key.length());
            return var_key;
        } else {
            throw std::runtime_error("not supported");
        }
    }
};

template <typename KeyT = KeyType8, typename ValueT = ValueType8>
class DashFixture : public BaseFixture {
    static constexpr size_t KeyTSize = sizeof(KeyT);
    using DashVarKeyT = typename std::conditional<std::is_same_v<KeyT, std::string>,
                                                    DashVarKey<DashVarKeyData>, DashVarKey<KeyT>>::type;
    using DashValueT = typename std::conditional<std::is_same_v<ValueT, std::string>, DashVarValue, ValueT>::type;
    using EntryKeyT = typename std::conditional<KeyTSize == 8, KeyT, DashVarKeyT>::type;
    using DashKeyT = typename std::conditional<KeyTSize == 8, KeyT, string_key*>::type;
    using Entry = std::pair<EntryKeyT, DashValueT>;

    struct DashPool {};

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

  protected:
    Hash<DashKeyT>* dash_;
    PmemValueSlab<Entry> slab_;
    pmem::obj::pool<DashPool> pmem_pool_;
    std::string dash_pool_name_;
    std::string pmem_pool_name_;
    bool map_initialized_ = false;

    bool insert_internal(const KeyT& key, const ValueT& value);
};

template <typename KeyT, typename ValueT>
void DashFixture<KeyT, ValueT>::InitMap(const uint64_t num_prefill_inserts, const bool re_init) {
    if (map_initialized_ && !re_init) {
        return;
    }

    pmem_pool_name_ = random_file(DB_PMEM_DIR);
    int sds_write_value = 0;
    pmemobj_ctl_set(NULL, "sds.at_create", &sds_write_value);
    // Value store: a pre-allocated PM slab written with clwb+sfence (no per-op
    // PMDK transaction) — the SAME value-store discipline as Viper's VPage, so
    // E2/E4 measure the index, not the value allocator (see pmem_value_slab.hpp).
    // Dash's index holds a stable pointer into this slab. 24 GiB ~= tens of
    // millions K8/V200; FS-DAX fully allocates on create — bump per-cell for a
    // 100M run, coordinating with other /pmem0 tenants.
    slab_.create(pmem_pool_name_, 24ul * ONE_GB);

    size_t segment_number = 64;
    dash_pool_name_ = random_file(DB_PMEM_DIR);
    // Dash's own index pool (segment table). 6 GiB is generous for tens of
    // millions of keys; was 10 GiB (100M-paper over-provision).
    Allocator::Initialize(dash_pool_name_.c_str(), 6 * ONE_GB);
    dash_ = reinterpret_cast<Hash<DashKeyT> *>(Allocator::GetRoot(sizeof(extendible::Finger_EH<DashKeyT>)));
    new (dash_) extendible::Finger_EH<DashKeyT>(segment_number, Allocator::Get()->pm_pool_);

    prefill(num_prefill_inserts);
    map_initialized_ = true;
}

template <typename KeyT, typename ValueT>
void DashFixture<KeyT, ValueT>::DeInitMap() {
    slab_.destroy();
    pmemobj_close(Allocator::Get()->pm_pool_);
    std::filesystem::remove(dash_pool_name_);
    dash_ = nullptr;
    map_initialized_ = false;
}

template <typename KeyT, typename ValueT>
inline bool DashFixture<KeyT, ValueT>::insert_internal(const KeyT& key, const ValueT& value) {
    // Value-store artifact fix: append into the pre-allocated PM slab with a
    // single clwb+sfence (no PMDK transaction), exactly as Viper writes a VPage
    // entry. The index stores a stable pointer into the slab.
    if constexpr (sizeof(KeyT) == 8) {
        const std::uint64_t slot = slab_.put(Entry{key, value});
        return dash_->Insert(key, (char*)slab_.at(slot)) == 0;
    } else {
        string_key str_k{};
        str_k.length = sizeof(KeyT);
        DashVarKeyT var_key{ .str_k = str_k, .key = key };
        const std::uint64_t slot = slab_.put(Entry{var_key, value});
        return dash_->Insert(&(slab_.at(slot)->first.str_k), (char*)slab_.at(slot)) == 0;
    }
}

template <>
inline bool DashFixture<KeyType8, ValueType8>::insert_internal(const KeyType8& key, const ValueType8& value) {
    return dash_->Insert(key, (char*)value.data.data()) == 0;
}

template <>
inline bool DashFixture<std::string, std::string>::insert_internal(const std::string& key, const std::string& value) {
    DashVarValue p_val;
    p_val.from_string(value);
    const std::uint64_t slot = slab_.put(Entry{DashVarKeyT::from_string(key), p_val});
    return dash_->Insert(&(slab_.at(slot)->first.str_k), (char *)slab_.at(slot)) == 0;
}


template <typename KeyT, typename ValueT>
uint64_t DashFixture<KeyT, ValueT>::insert(uint64_t start_idx, uint64_t end_idx) {
    uint64_t insert_counter = 0;
    for (uint64_t pos = start_idx; pos < end_idx; ++pos) {
        const KeyT db_key{pos};
        const ValueT value{pos};
        insert_counter += insert_internal(db_key, value);
    }
    return insert_counter;
}

template <>
uint64_t DashFixture<std::string, std::string>::insert(uint64_t start_idx, uint64_t end_idx) {
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
uint64_t DashFixture<KeyT, ValueT>::setup_and_insert(uint64_t start_idx, uint64_t end_idx) {
    return insert(start_idx, end_idx);
}

template <typename KeyT, typename ValueT>
uint64_t DashFixture<KeyT, ValueT>::setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    uint64_t found_counter = 0;
    for (uint64_t i = 0; i < num_finds; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const char* val;
        if constexpr (sizeof(KeyT) == 8) {
            val = dash_->Get(key);
        } else {
            DashVarKeyT var_key{.str_k = string_key{.length = sizeof(KeyT)}, .key = key};
            val = dash_->Get(&var_key.str_k);
        }

        const bool found = val != NONE;
        if (found) {
            if constexpr (sizeof(ValueT) == 8) {
                ValueType8 found_val = (uint64_t)val;
                found_counter += (found_val.data[0] == key);
            } else {
                const Entry *entry_ptr = (Entry *) val;
                ValueT found_val = entry_ptr->second;
                found_counter += (found_val == ValueT{key});
            }
        }
    }
    return found_counter;
}

template <>
uint64_t DashFixture<std::string, std::string>::setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    const std::vector<std::string>& keys = std::get<0>(var_size_kvs_);
    const std::vector<std::string>& values = std::get<1>(var_size_kvs_);

    uint64_t found_counter = 0;
    for (uint64_t i = 0; i < num_finds; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const std::string& db_key = keys[key];
        DashVarKeyT var_key = DashVarKeyT::from_string(db_key);
        const char* val = dash_->Get(&var_key.str_k);

        const bool found = val != NONE;
        if (found) {
            const std::string& value = values[key];
            const Entry* entry_ptr = (Entry*) val;
            const DashVarValue found_value = entry_ptr->second;
            const bool cmp_len = found_value.length == value.length();
            found_counter += cmp_len && memcmp(found_value.data.data(), value.data(), found_value.length) == 0;
        }
    }
    return found_counter;
}

template <typename KeyT, typename ValueT>
uint64_t DashFixture<KeyT, ValueT>::setup_and_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    uint64_t update_counter = 0;
    for (uint64_t i = 0; i < num_updates; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const char* val;
        if constexpr (sizeof(KeyT) == 8) {
            val = dash_->Get(key);
        } else {
            DashVarKeyT var_key{.str_k = string_key{.length = sizeof(KeyT)}, .key = key};
            val = dash_->Get(&var_key.str_k);
        }

        const bool found = val != NONE;
        if (found) {
            // In-place update (matches Viper): mutate the value in its slab slot
            // and persist 8 B — no re-alloc, no re-index, no PMDK transaction.
            Entry* entry = (Entry*) val;
            entry->second.update_value();
            viper::internal::pmem_persist(&entry->second, sizeof(uint64_t));
            update_counter++;
        }
    }
    return update_counter;
}

template <>
uint64_t DashFixture<std::string, std::string>::setup_and_update(uint64_t, uint64_t, uint64_t) { return 0; }

template <typename KeyT, typename ValueT>
uint64_t DashFixture<KeyT, ValueT>::setup_and_delete(uint64_t start_idx, uint64_t end_idx, uint64_t num_deletes) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    uint64_t delete_counter = 0;
    for (uint64_t i = 0; i < num_deletes; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const char* val;
        if constexpr (sizeof(KeyT) == 8) {
            val = dash_->Get(key);
        } else {
            DashVarKeyT var_key{.str_k = string_key{.length = sizeof(KeyT)}, .key = key};
            val = dash_->Get(&var_key.str_k);
        }

        const bool found = val != NONE;
        if (!found) {
            continue;
        }

        // Value-store artifact fix: no per-op transaction. The slab slot is
        // left in place (Viper likewise does not reclaim on delete by default);
        // only the index entry is removed.
        (void) val;
        if constexpr (sizeof(KeyT) == 8) {
            dash_->Delete(key);
        } else {
            DashVarKeyT var_key{.str_k = string_key{.length = sizeof(KeyT)}, .key = key};
            dash_->Delete(&var_key.str_k);
        }
        delete_counter++;
    }
    return delete_counter;
}

template <>
uint64_t DashFixture<std::string, std::string>::setup_and_delete(uint64_t, uint64_t, uint64_t) { return 0; }

template <typename KeyT, typename ValueT>
uint64_t DashFixture<KeyT, ValueT>::run_ycsb(uint64_t, uint64_t, const std::vector<ycsb::Record>&, LatencyHistograms) {
    throw std::runtime_error{"YCSB not implemented for non-ycsb key/value types."};
}

template <>
uint64_t DashFixture<KeyType8, ValueType200>::run_ycsb(uint64_t start_idx,
    uint64_t end_idx, const std::vector<ycsb::Record>& data, LatencyHistograms hdrs) {
    // Single-HDR fallback for non-active fixtures (Viper/HiOM do per-op split).
    hdr_histogram* hdr = hdrs.write ? hdrs.write : hdrs.read;
    uint64_t op_count = 0;
    const ValueType200 null_value{0ul};
    std::chrono::high_resolution_clock::time_point start;

    for (int op_num = start_idx; op_num < end_idx; ++op_num) {
        const ycsb::Record& record = data[op_num];

        if (hdr != nullptr) {
            start = std::chrono::high_resolution_clock::now();
        }

        switch (record.op) {
            case ycsb::Record::Op::INSERT: {
                insert_internal(record.key, record.value);
                op_count++;
                break;
            }
            case ycsb::Record::Op::GET: {
                const char* val = dash_->Get(record.key);
                const bool found = val != NONE;
                if (found) {
                    Entry* entry = (Entry*) val;
                    ValueType200 value = entry->second;
                    op_count += (value != null_value);
                }
                break;
            }
            case ycsb::Record::Op::UPDATE: {
                const char* val = dash_->Get(record.key);
                const bool found = val != NONE;
                if (found) {
                    // In-place update (matches Viper): mutate the value in its
                    // slab slot and persist 8 B — no re-alloc/re-index/txn.
                    Entry* entry = (Entry*) val;
                    entry->second.data[0] = record.value.data[0];
                    viper::internal::pmem_persist(&entry->second, sizeof(uint64_t));
                    op_count++;
                }
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
void DashFixture<KeyT, ValueT>::prefill_ycsb(const std::vector<ycsb::Record>& data) {
    BaseFixture::prefill_ycsb(data);
}

}  // namespace
