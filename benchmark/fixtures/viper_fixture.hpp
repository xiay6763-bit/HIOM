#pragma once

#include "../benchmark.hpp"
#include "common_fixture.hpp"
#include "viper/viper.hpp"


namespace viper {
namespace kv_bm {

template <typename KeyT = KeyType16, typename ValueT = ValueType200>
class ViperFixture : public BaseFixture {
  public:
    typedef KeyT KeyType;
    using ViperT = Viper<KeyT, ValueT>;

    void InitMap(const uint64_t num_prefill_inserts = 0, const bool re_init = true) override;
    void InitMap(const uint64_t num_prefill_inserts, ViperConfig v_config);

    void DeInitMap() override;

    uint64_t insert(uint64_t start_idx, uint64_t end_idx) final;

    uint64_t setup_and_insert(uint64_t start_idx, uint64_t end_idx) final;
    uint64_t setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds) final;
    uint64_t setup_and_delete(uint64_t start_idx, uint64_t end_idx, uint64_t num_deletes) final;
    uint64_t setup_and_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates) final;

    uint64_t setup_and_get_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates);

    uint64_t run_ycsb(uint64_t start_idx, uint64_t end_idx,
        const std::vector<ycsb::Record>& data, LatencyHistograms hdrs) final;

    ViperT* getViper() {
        return viper_.get();
    }

    // White-box index DRAM = the CCEH offset map's directory + segments.
    size_t fixture_dram_bytes() override {
        return viper_ ? viper_->cceh_dram_bytes() : 0;
    }

  protected:
    std::unique_ptr<ViperT> viper_;
    bool viper_initialized_ = false;
    std::string pool_file_;
};

template <typename KeyT, typename ValueT>
void ViperFixture<KeyT, ValueT>::InitMap(uint64_t num_prefill_inserts, const bool re_init) {
    if (viper_initialized_ && !re_init) {
        return;
    }

    return InitMap(num_prefill_inserts, ViperConfig{});
}

template <typename KeyT, typename ValueT>
void ViperFixture<KeyT, ValueT>::InitMap(uint64_t num_prefill_inserts, ViperConfig v_config) {
#ifdef CCEH_PERSISTENT
    PMemAllocator::get().initialize();
#endif

    pool_file_ = VIPER_POOL_FILE;
//    pool_file_ = random_file(DB_PMEM_DIR);
//    pool_file_ = DB_PMEM_DIR + std::string("/viper");

    // Defensive cleanup: if a previous benchmark crashed or was terminated, the
    // pool dir may still exist and Viper::create refuses non-empty dirs. Only
    // touch our own pool path (never blanket-rm under /pmem0; that mount is
    // shared with other users — see CLAUDE.md).
    if (pool_file_.find("/dev/dax") == std::string::npos) {
        std::filesystem::remove_all(pool_file_);
    }

//    viper_ = ViperT::open(pool_file_, v_config);
    viper_ = ViperT::create(pool_file_, BM_POOL_SIZE, v_config);
    this->prefill(num_prefill_inserts);
    viper_initialized_ = true;
}

template <typename KeyT, typename ValueT>
void ViperFixture<KeyT, ValueT>::DeInitMap() {
    BaseFixture::DeInitMap();
    viper_ = nullptr;
    viper_initialized_ = false;
    if (pool_file_.find("/dev/dax") == std::string::npos) {
        std::filesystem::remove_all(pool_file_);
    }
}

template <typename KeyT, typename ValueT>
uint64_t ViperFixture<KeyT, ValueT>::insert(uint64_t start_idx, uint64_t end_idx) {
    uint64_t insert_counter = 0;
    auto v_client = viper_->get_client();
    for (uint64_t key = start_idx; key < end_idx; ++key) {
        const KeyT db_key{key};
        const ValueT value{key};
        insert_counter += v_client.put(db_key, value);
    }
    return insert_counter;
}

template <>
uint64_t ViperFixture<std::string, std::string>::insert(uint64_t start_idx, uint64_t end_idx) {
    uint64_t insert_counter = 0;
    auto v_client = viper_->get_client();
    const std::vector<std::string>& keys = std::get<0>(var_size_kvs_);
    const std::vector<std::string>& values = std::get<1>(var_size_kvs_);
    for (uint64_t key = start_idx; key < end_idx; ++key) {
        const std::string& db_key = keys[key];
        const std::string& value = values[key];
        insert_counter += v_client.put(db_key, value);
    }
    return insert_counter;
}

template <typename KeyT, typename ValueT>
uint64_t ViperFixture<KeyT, ValueT>::setup_and_insert(uint64_t start_idx, uint64_t end_idx) {
    return insert(start_idx, end_idx);
}

template <typename KeyT, typename ValueT>
uint64_t ViperFixture<KeyT, ValueT>::setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    const auto v_client = viper_->get_read_only_client();
    uint64_t found_counter = 0;
    ValueT value;
    for (uint64_t i = 0; i < num_finds; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyT db_key{key};
        const bool found = v_client.get(db_key, &value);
        found_counter += found && (value == ValueT{key});
    }
    return found_counter;
}

template <>
uint64_t ViperFixture<std::string, std::string>::setup_and_find(uint64_t start_idx, uint64_t end_idx, uint64_t num_finds) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    const std::vector<std::string>& keys = std::get<0>(var_size_kvs_);
    const std::vector<std::string>& values = std::get<1>(var_size_kvs_);

    auto v_client = viper_->get_read_only_client();
    uint64_t found_counter = 0;
    std::string result;
    for (uint64_t i = 0; i < num_finds; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const std::string& db_key = keys[key];
        const std::string& value = values[key];
        const bool found = v_client.get(db_key, &result);
        found_counter += found && (result == value);
    }
    return found_counter;
}

template <typename KeyT, typename ValueT>
uint64_t ViperFixture<KeyT, ValueT>::setup_and_delete(uint64_t start_idx, uint64_t end_idx, uint64_t num_deletes) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto v_client = viper_->get_client();
    uint64_t delete_counter = 0;
    for (uint64_t i = 0; i < num_deletes; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyT db_key{key};
        delete_counter += v_client.remove(db_key);
    }
    return delete_counter;
}

template <>
uint64_t ViperFixture<std::string, std::string>::setup_and_delete(uint64_t start_idx, uint64_t end_idx, uint64_t num_deletes) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);
    const std::vector<std::string>& keys = std::get<0>(var_size_kvs_);

    auto v_client = viper_->get_client();
    uint64_t delete_counter = 0;
    for (uint64_t i = 0; i < num_deletes; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const std::string& db_key = keys[key];
        delete_counter += v_client.remove(db_key);
    }
    return delete_counter;
}

template <typename KeyT, typename ValueT>
uint64_t ViperFixture<KeyT, ValueT>::setup_and_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto v_client = viper_->get_client();
    uint64_t update_counter = 0;

    auto update_fn = [](ValueT* value) {
        value->update_value();
        internal::pmem_persist(value, sizeof(uint64_t));
    };

    for (uint64_t i = 0; i < num_updates; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyT db_key{key};
        update_counter += v_client.update(db_key, update_fn);
    }
    return update_counter;
}

template <>
uint64_t ViperFixture<std::string, std::string>::setup_and_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates) {
    throw std::runtime_error("Not supported");
}

template <typename KeyT, typename ValueT>
uint64_t ViperFixture<KeyT, ValueT>::setup_and_get_update(uint64_t start_idx, uint64_t end_idx, uint64_t num_updates) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto v_client = viper_->get_client();

    ValueT new_v{};
    for (uint64_t i = 0; i < num_updates; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyT db_key{key};
        v_client.get(db_key, &new_v);
        new_v.update_value();
        v_client.put(db_key, new_v);
    }

    return num_updates;
}

template <>
uint64_t ViperFixture<std::string, std::string>::setup_and_get_update(uint64_t, uint64_t, uint64_t) {
    throw std::runtime_error("Not supported");
}

template <typename KeyT, typename ValueT>
uint64_t ViperFixture<KeyT, ValueT>::run_ycsb(uint64_t, uint64_t, const std::vector<ycsb::Record>&, LatencyHistograms) {
    throw std::runtime_error{"YCSB not implemented for non-ycsb key/value types."};
}

template <>
uint64_t ViperFixture<KeyType8, ValueType200>::run_ycsb(
    uint64_t start_idx, uint64_t end_idx, const std::vector<ycsb::Record>& data, LatencyHistograms hdrs) {
    uint64_t op_count = 0;
    auto v_client = viper_->get_client();
    ValueType200 value;
    const ValueType200 null_value{0ul};

    const bool log_latency = (hdrs.read != nullptr || hdrs.write != nullptr);
    std::chrono::high_resolution_clock::time_point start;
    for (int op_num = start_idx; op_num < end_idx; ++op_num) {
        const ycsb::Record& record = data[op_num];

        if (log_latency) {
            start = std::chrono::high_resolution_clock::now();
        }

        // Route latency sample by op semantics: GET → read tail,
        // INSERT/UPDATE → write tail. Mixed workloads need this
        // separation because write tail is dominated by CCEH segment
        // splits / PMem flush whereas read tail is index lookup; a
        // merged CDF blurs both. 100% read or 100% write workloads
        // are unaffected (one histogram stays empty and is dropped
        // by the JSON emitter).
        hdr_histogram* op_hdr = nullptr;
        switch (record.op) {
            case ycsb::Record::Op::INSERT: {
                v_client.put(record.key, record.value);
                op_count++;
                op_hdr = hdrs.write;
                break;
            }
            case ycsb::Record::Op::GET: {
                const bool found = v_client.get(record.key, &value);
                op_count += found && (value != null_value);
                op_hdr = hdrs.read;
                break;
            }
            case ycsb::Record::Op::UPDATE: {
                auto update_fn = [&](ValueType200* value) {
                    value->data[0] = record.value.data[0];
                    internal::pmem_persist(value->data.data(), sizeof(uint64_t));
                };
                op_count += v_client.update(record.key, update_fn);
                op_hdr = hdrs.write;
                break;
            }
            default: {
                throw std::runtime_error("Unknown operation: " + std::to_string(record.op));
            }
        }

        if (!log_latency || op_hdr == nullptr) {
            continue;
        }

        const auto end = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        hdr_record_value(op_hdr, duration.count());
    }

    return op_count;
}

}  // namespace kv_bm
}  // namespace viper
