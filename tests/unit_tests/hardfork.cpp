// Copyright (c) 2014-2015, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <algorithm>
#include "gtest/gtest.h"

#include "blockchain_db/lmdb/db_lmdb.h"
#include "cryptonote_core/hardfork.h"

using namespace cryptonote;

#define BLOCKS_PER_YEAR 525960
#define SECONDS_PER_YEAR 31557600

static cryptonote::block mkblock(uint8_t version)
{
  cryptonote::block b;
  b.major_version = version;
  return b;
}

TEST(empty_hardforks, Success)
{
  HardFork hf;

  ASSERT_TRUE(hf.get_state(time(NULL)) == HardFork::Ready);
  ASSERT_TRUE(hf.get_state(time(NULL) + 3600*24*400) == HardFork::Ready);

  ASSERT_EQ(hf.get(0), 1);
  ASSERT_EQ(hf.get(1), 1);
  ASSERT_EQ(hf.get(100000000), 1);
}

TEST(ordering, Success)
{
  HardFork hf;

  ASSERT_TRUE(hf.add(2, 2, 1));
  ASSERT_FALSE(hf.add(3, 3, 1));
  ASSERT_FALSE(hf.add(3, 2, 2));
  ASSERT_FALSE(hf.add(2, 3, 2));
  ASSERT_TRUE(hf.add(3, 10, 2));
  ASSERT_TRUE(hf.add(4, 20, 3));
  ASSERT_FALSE(hf.add(5, 5, 4));
}

TEST(states, Success)
{
  HardFork hf;

  ASSERT_TRUE(hf.add(2, BLOCKS_PER_YEAR, SECONDS_PER_YEAR));

  ASSERT_TRUE(hf.get_state(0) == HardFork::Ready);
  ASSERT_TRUE(hf.get_state(SECONDS_PER_YEAR / 2) == HardFork::Ready);
  ASSERT_TRUE(hf.get_state(SECONDS_PER_YEAR + HardFork::DEFAULT_UPDATE_TIME / 2) == HardFork::Ready);
  ASSERT_TRUE(hf.get_state(SECONDS_PER_YEAR + (HardFork::DEFAULT_UPDATE_TIME + HardFork::DEFAULT_FORKED_TIME) / 2) == HardFork::UpdateNeeded);
  ASSERT_TRUE(hf.get_state(SECONDS_PER_YEAR + HardFork::DEFAULT_FORKED_TIME * 2) == HardFork::LikelyForked);

  ASSERT_TRUE(hf.add(3, BLOCKS_PER_YEAR * 5, SECONDS_PER_YEAR * 5));

  ASSERT_TRUE(hf.get_state(0) == HardFork::Ready);
  ASSERT_TRUE(hf.get_state(SECONDS_PER_YEAR / 2) == HardFork::Ready);
  ASSERT_TRUE(hf.get_state(SECONDS_PER_YEAR + HardFork::DEFAULT_UPDATE_TIME / 2) == HardFork::Ready);
  ASSERT_TRUE(hf.get_state(SECONDS_PER_YEAR + (HardFork::DEFAULT_UPDATE_TIME + HardFork::DEFAULT_FORKED_TIME) / 2) == HardFork::Ready);
  ASSERT_TRUE(hf.get_state(SECONDS_PER_YEAR + HardFork::DEFAULT_FORKED_TIME * 2) == HardFork::Ready);
}

TEST(steps_asap, Success)
{
  HardFork hf(1,1,1,1);

  //                 v  h  t
  ASSERT_TRUE(hf.add(4, 2, 1));
  ASSERT_TRUE(hf.add(7, 4, 2));
  ASSERT_TRUE(hf.add(9, 6, 3));

  for (uint64_t h = 0; h < 10; ++h)
    hf.add(mkblock(10), h);

  ASSERT_EQ(hf.get(0), 1);
  ASSERT_EQ(hf.get(1), 1);
  ASSERT_EQ(hf.get(2), 4);
  ASSERT_EQ(hf.get(3), 4);
  ASSERT_EQ(hf.get(4), 7);
  ASSERT_EQ(hf.get(5), 7);
  ASSERT_EQ(hf.get(6), 9);
  ASSERT_EQ(hf.get(7), 9);
  ASSERT_EQ(hf.get(8), 9);
  ASSERT_EQ(hf.get(9), 9);
  ASSERT_EQ(hf.get(100000), 9);
}

TEST(steps_1, Success)
{
  HardFork hf(1,1,1,1);

  //                 v  h  t
  for (int n = 1 ; n < 10; ++n)
    ASSERT_TRUE(hf.add(n+1, n, n));

  for (uint64_t h = 0; h < 10; ++h) {
    hf.add(mkblock(h+1), h);
    ASSERT_EQ(hf.get(h), h+1);
  }
}

class TestDB: public BlockchainDB {
public:
  virtual void open(const std::string& filename, const int db_flags = 0) {}
  virtual void close() {}
  virtual void sync() {}
  virtual void reset() {}
  virtual std::vector<std::string> get_filenames() const { return std::vector<std::string>(); }
  virtual std::string get_db_name() const { return std::string(); }
  virtual bool lock() { return true; }
  virtual void unlock() { }
  virtual void batch_start(uint64_t batch_num_blocks=0) {}
  virtual void batch_stop() {}
  virtual void set_batch_transactions(bool) {}
  virtual bool block_exists(const crypto::hash& h) const { return false; }
  virtual block get_block(const crypto::hash& h) const { return block(); }
  virtual uint64_t get_block_height(const crypto::hash& h) const { return 0; }
  virtual block_header get_block_header(const crypto::hash& h) const { return block_header(); }
  virtual uint64_t get_block_timestamp(const uint64_t& height) const { return 0; }
  virtual uint64_t get_top_block_timestamp() const { return 0; }
  virtual size_t get_block_size(const uint64_t& height) const { return 128; }
  virtual difficulty_type get_block_cumulative_difficulty(const uint64_t& height) const { return 10; }
  virtual difficulty_type get_block_difficulty(const uint64_t& height) const { return 0; }
  virtual uint64_t get_block_already_generated_coins(const uint64_t& height) const { return 10000000000; }
  virtual crypto::hash get_block_hash_from_height(const uint64_t& height) const { return crypto::hash(); }
  virtual std::vector<block> get_blocks_range(const uint64_t& h1, const uint64_t& h2) const { return std::vector<block>(); }
  virtual std::vector<crypto::hash> get_hashes_range(const uint64_t& h1, const uint64_t& h2) const { return std::vector<crypto::hash>(); }
  virtual crypto::hash top_block_hash() const { return crypto::hash(); }
  virtual block get_top_block() const { return block(); }
  virtual uint64_t height() const { return blocks.size(); }
  virtual bool tx_exists(const crypto::hash& h) const { return false; }
  virtual uint64_t get_tx_unlock_time(const crypto::hash& h) const { return 0; }
  virtual transaction get_tx(const crypto::hash& h) const { return transaction(); }
  virtual uint64_t get_tx_count() const { return 0; }
  virtual std::vector<transaction> get_tx_list(const std::vector<crypto::hash>& hlist) const { return std::vector<transaction>(); }
  virtual uint64_t get_tx_block_height(const crypto::hash& h) const { return 0; }
  virtual uint64_t get_num_outputs(const uint64_t& amount) const { return 1; }
  virtual output_data_t get_output_key(const uint64_t& amount, const uint64_t& index) { return output_data_t(); }
  virtual output_data_t get_output_key(const uint64_t& global_index) const { return output_data_t(); }
  virtual tx_out get_output(const crypto::hash& h, const uint64_t& index) const { return tx_out(); }
  virtual tx_out_index get_output_tx_and_index_from_global(const uint64_t& index) const { return tx_out_index(); }
  virtual tx_out_index get_output_tx_and_index(const uint64_t& amount, const uint64_t& index) { return tx_out_index(); }
  virtual void get_output_tx_and_index(const uint64_t& amount, const std::vector<uint64_t> &offsets, std::vector<tx_out_index> &indices) {}
  virtual void get_output_key(const uint64_t &amount, const std::vector<uint64_t> &offsets, std::vector<output_data_t> &outputs) {}
  virtual bool can_thread_bulk_indices() const { return false; }
  virtual std::vector<uint64_t> get_tx_output_indices(const crypto::hash& h) const { return std::vector<uint64_t>(); }
  virtual std::vector<uint64_t> get_tx_amount_output_indices(const crypto::hash& h) const { return std::vector<uint64_t>(); }
  virtual bool has_key_image(const crypto::key_image& img) const { return false; }
  virtual void remove_block() { blocks.pop_back(); }
  virtual void add_transaction_data(const crypto::hash& blk_hash, const transaction& tx, const crypto::hash& tx_hash) {}
  virtual void remove_transaction_data(const crypto::hash& tx_hash, const transaction& tx) {}
  virtual void add_output(const crypto::hash& tx_hash, const tx_out& tx_output, const uint64_t& local_index, const uint64_t unlock_time) {}
  virtual void remove_output(const tx_out& tx_output) {}
  virtual void add_spent_key(const crypto::key_image& k_image) {}
  virtual void remove_spent_key(const crypto::key_image& k_image) {}

  virtual void add_block( const block& blk
                        , const size_t& block_size
                        , const difficulty_type& cumulative_difficulty
                        , const uint64_t& coins_generated
                        , const crypto::hash& blk_hash
                        ) {
    blocks.push_back(blk);
  }
  virtual block get_block_from_height(const uint64_t& height) const {
    return blocks[height];
  }
private:
  std::vector<block> blocks;
};

TEST(reorganize, Same)
{
  for (int history = 1; history <= 12; ++history) {
    for (uint64_t checkpoint_period = 1; checkpoint_period <= 16; checkpoint_period++) {
      HardFork hf(1, 1, 1, history, 100, checkpoint_period);
      TestDB db;

      //                 v  h  t
      ASSERT_TRUE(hf.add(4, 2, 1));
      ASSERT_TRUE(hf.add(7, 4, 2));
      ASSERT_TRUE(hf.add(9, 6, 3));

      //                                 index  0  1  2  3  4  5  6  7  8  9
      static const uint8_t block_versions[] = { 1, 1, 4, 4, 7, 7, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
      for (uint64_t h = 0; h < 20; ++h) {
        db.add_block(mkblock(block_versions[h]), 0, 0, 0, crypto::hash());
        hf.add(db.get_block_from_height(h), h);
      }

      for (uint64_t rh = 0; rh < 20; ++rh) {
        hf.reorganize_from_block_height(&db, rh);
        for (int hh = 0; hh < 20; ++hh) {
          uint8_t version = hh >= (history-1) ? block_versions[hh - (history-1)] : 1;
          ASSERT_EQ(hf.get(hh), version);
        }
        ASSERT_EQ(hf.get(100000), 9);
      }
    }
  }
}

TEST(reorganize, Changed)
{
  int history = 4;
  for (uint64_t checkpoint_period = 1; checkpoint_period <= 16; checkpoint_period++) {
    HardFork hf(1, 1, 1, 4, 100, checkpoint_period);
    TestDB db;

    //                 v  h  t
    ASSERT_TRUE(hf.add(4, 2, 1));
    ASSERT_TRUE(hf.add(7, 4, 2));
    ASSERT_TRUE(hf.add(9, 6, 3));

    //                                 index  0  1  2  3  4  5  6  7  8  9
    static const uint8_t block_versions[] = { 1, 1, 4, 4, 7, 7, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
    for (uint64_t h = 0; h < 20; ++h) {
      db.add_block(mkblock(block_versions[h]), 0, 0, 0, crypto::hash());
      hf.add(db.get_block_from_height(h), h);
    }

    for (uint64_t rh = 0; rh < 20; ++rh) {
      hf.reorganize_from_block_height(&db, rh);
      for (int hh = 0; hh < 20; ++hh) {
        uint8_t version = hh >= (history-1) ? block_versions[hh - (history-1)] : 1;
        ASSERT_EQ(hf.get(hh), version);
      }
      ASSERT_EQ(hf.get(100000), 9);
    }

    // delay a bit for 9, and go back to 1 to check it stays at 9
    static const uint8_t block_versions_new[] =    { 1, 1, 4, 4, 7, 7, 4, 7, 7, 7, 9, 9, 9, 9, 9, 1, 1, 1, 1, 1 };
    static const uint8_t expected_versions_new[] = { 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 7, 7, 7, 9, 9, 9, 9, 9, 9, 9 };
    for (uint64_t h = 3; h < 20; ++h) {
      db.remove_block();
    }
    ASSERT_EQ(db.height(), 3);
    hf.reorganize_from_block_height(&db, 2);
    for (uint64_t h = 3; h < 20; ++h) {
      db.add_block(mkblock(block_versions_new[h]), 0, 0, 0, crypto::hash());
      hf.add(db.get_block_from_height(h), h);
    }
    for (int hh = 0; hh < 20; ++hh) {
      ASSERT_EQ(hf.get(hh), expected_versions_new[hh]);
    }
    ASSERT_EQ(hf.get(100000), 9);
  }
}

TEST(voting, threshold)
{
  for (int threshold = 87; threshold <= 88; ++threshold) {
    HardFork hf(1, 1, 1, 8, threshold, 10);
    TestDB db;

    //                 v  h  t
    ASSERT_TRUE(hf.add(2, 2, 1));

    for (uint64_t h = 0; h < 10; ++h) {
      uint8_t v = 1 + !!(h % 8);
      hf.add(mkblock(v), h);
      uint8_t expected = threshold == 88 ? 1 : h < 7 ? 1 : 2;
      ASSERT_EQ(hf.get(h), expected);
    }
  }
}

TEST(new_blocks, denied)
{
    HardFork hf(1, 1, 1, 4, 50, 10);

    //                 v  h  t
    ASSERT_TRUE(hf.add(2, 2, 1));

    ASSERT_FALSE(hf.add(mkblock(0), 0));
    ASSERT_TRUE(hf.add(mkblock(1), 0));
    ASSERT_TRUE(hf.add(mkblock(1), 1));
    ASSERT_TRUE(hf.add(mkblock(1), 2));
    ASSERT_TRUE(hf.add(mkblock(2), 3));
    ASSERT_TRUE(hf.add(mkblock(1), 4));
    ASSERT_TRUE(hf.add(mkblock(1), 5));
    ASSERT_TRUE(hf.add(mkblock(1), 6));
    ASSERT_TRUE(hf.add(mkblock(2), 7));
    ASSERT_TRUE(hf.add(mkblock(2), 8)); // we reach 50% of the last 4
    ASSERT_FALSE(hf.add(mkblock(1), 9)); // so this one can't get added
    ASSERT_TRUE(hf.add(mkblock(2), 10));

    ASSERT_EQ(hf.get_start_height(2), 8);
}

TEST(new_version, early)
{
    HardFork hf(1, 1, 1, 4, 50, 10);

    //                 v  h  t
    ASSERT_TRUE(hf.add(2, 4, 1));

    ASSERT_FALSE(hf.add(mkblock(0), 0));
    ASSERT_TRUE(hf.add(mkblock(2), 0));
    ASSERT_TRUE(hf.add(mkblock(2), 1)); // we have enough votes already
    ASSERT_TRUE(hf.add(mkblock(2), 2));
    ASSERT_TRUE(hf.add(mkblock(1), 3)); // we accept a previous version because we did not switch, even with all the votes
    ASSERT_TRUE(hf.add(mkblock(2), 4)); // but have to wait for the declared height anyway
    ASSERT_TRUE(hf.add(mkblock(2), 5));
    ASSERT_FALSE(hf.add(mkblock(1), 6)); // we don't accept 1 anymore
    ASSERT_TRUE(hf.add(mkblock(2), 7)); // but we do accept 2

    ASSERT_EQ(hf.get_start_height(2), 4);
}

TEST(reorganize, changed)
{
    HardFork hf(1, 1, 1, 4, 50, 10);
    TestDB db;

    //                 v  h  t
    ASSERT_TRUE(hf.add(2, 2, 1));
    ASSERT_TRUE(hf.add(3, 5, 2));

#define ADD(v, h, a) \
  do { \
    cryptonote::block b = mkblock(v); \
    db.add_block(b, 0, 0, 0, crypto::hash()); \
    ASSERT_##a(hf.add(b, h)); \
  } while(0)
#define ADD_TRUE(v, h) ADD(v, h, TRUE)
#define ADD_FALSE(v, h) ADD(v, h, FALSE)

    ADD_FALSE(0, 0);
    ADD_TRUE(1, 0);
    ADD_TRUE(1, 1);
    ADD_TRUE(2, 2);
    ADD_TRUE(2, 3); // switch to 2 here
    ADD_TRUE(2, 4);
    ADD_TRUE(2, 5);
    ADD_TRUE(2, 6);
    ASSERT_EQ(hf.get_current_version(), 2);
    ADD_TRUE(3, 7);
    ADD_TRUE(4, 8);
    ADD_TRUE(4, 9);
    ASSERT_EQ(hf.get_start_height(2), 3);
    ASSERT_EQ(hf.get_start_height(3), 8);
    ASSERT_EQ(hf.get_current_version(), 3);

    // pop a few blocks and check current version goes back down
    db.remove_block();
    hf.reorganize_from_block_height(&db, 8);
    ASSERT_EQ(hf.get_current_version(), 3);
    db.remove_block();
    hf.reorganize_from_block_height(&db, 7);
    ASSERT_EQ(hf.get_current_version(), 2);
    db.remove_block();
    ASSERT_EQ(hf.get_current_version(), 2);

    // add blocks again, but remaining at 2
    ADD_TRUE(2, 7);
    ADD_TRUE(2, 8);
    ADD_TRUE(2, 9);
    ASSERT_EQ(hf.get_start_height(2), 3); // unchanged
    ASSERT_EQ(hf.get_current_version(), 2); // we did not bump to 3 this time
    ASSERT_EQ(hf.get_start_height(3), std::numeric_limits<uint64_t>::max()); // not yet
}
