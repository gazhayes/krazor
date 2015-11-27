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

#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>

#include <boost/utility/value_init.hpp>
#include "include_base_utils.h"
using namespace epee;

#include "cryptonote_config.h"
#include "wallet2.h"
#include "cryptonote_core/cryptonote_format_utils.h"
#include "rpc/core_rpc_server_commands_defs.h"
#include "misc_language.h"
#include "cryptonote_core/cryptonote_basic_impl.h"
#include "common/boost_serialization_helper.h"
#include "profile_tools.h"
#include "crypto/crypto.h"
#include "serialization/binary_utils.h"
#include "cryptonote_protocol/blobdatatype.h"
#include "mnemonics/electrum-words.h"
#include "common/dns_utils.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

extern "C"
{
#include "crypto/keccak.h"
#include "crypto/crypto-ops.h"
}
using namespace cryptonote;

// used to choose when to stop adding outputs to a tx
#define APPROXIMATE_INPUT_BYTES 80

// used to target a given block size (additional outputs may be added on top to build fee)
#define TX_SIZE_TARGET(bytes) (bytes*2/3)

// arbitrary, used to generate different hashes from the same input
#define CHACHA8_KEY_TAIL 0x8c

#define KILL_IOSERVICE()  \
    do { \
      work.reset(); \
      threadpool.join_all(); \
      ioservice.stop(); \
    } while(0)

namespace
{
void do_prepare_file_names(const std::string& file_path, std::string& keys_file, std::string& wallet_file)
{
  keys_file = file_path;
  wallet_file = file_path;
  boost::system::error_code e;
  if(string_tools::get_extension(keys_file) == "keys")
  {//provided keys file name
    wallet_file = string_tools::cut_off_extension(wallet_file);
  }else
  {//provided wallet file name
    keys_file += ".keys";
  }
}

} //namespace

namespace tools
{
// for now, limit to 30 attempts.  TODO: discuss a good number to limit to.
const size_t MAX_SPLIT_ATTEMPTS = 30;

//----------------------------------------------------------------------------------------------------
void wallet2::init(const std::string& daemon_address, uint64_t upper_transaction_size_limit)
{
  m_upper_transaction_size_limit = upper_transaction_size_limit;
  m_daemon_address = daemon_address;
}
//----------------------------------------------------------------------------------------------------
bool wallet2::is_deterministic() const
{
  crypto::secret_key second;
  keccak((uint8_t *)&get_account().get_keys().m_spend_secret_key, sizeof(crypto::secret_key), (uint8_t *)&second, sizeof(crypto::secret_key));
  sc_reduce32((uint8_t *)&second);
  bool keys_deterministic = memcmp(second.data,get_account().get_keys().m_view_secret_key.data, sizeof(crypto::secret_key)) == 0;
  return keys_deterministic;
}
//----------------------------------------------------------------------------------------------------
bool wallet2::get_seed(std::string& electrum_words) const
{
  bool keys_deterministic = is_deterministic();
  if (!keys_deterministic)
  {
    std::cout << "This is not a deterministic wallet" << std::endl;
    return false;
  }
  if (seed_language.empty())
  {
    std::cout << "seed_language not set" << std::endl;
    return false;
  }

  crypto::ElectrumWords::bytes_to_words(get_account().get_keys().m_spend_secret_key, electrum_words, seed_language);

  return true;
}
/*!
 * \brief Gets the seed language
 */
const std::string &wallet2::get_seed_language() const
{
  return seed_language;
}
/*!
 * \brief Sets the seed language
 * \param language  Seed language to set to
 */
void wallet2::set_seed_language(const std::string &language)
{
  seed_language = language;
}
/*!
 * \brief Tells if the wallet file is deprecated.
 */
bool wallet2::is_deprecated() const
{
  return is_old_file_format;
}
//----------------------------------------------------------------------------------------------------
void wallet2::check_acc_out(const account_keys &acc, const tx_out &o, const crypto::public_key &tx_pub_key, size_t i, uint64_t &money_transfered, bool &error) const
{
  if (o.target.type() !=  typeid(txout_to_key))
  {
     error = true;
     LOG_ERROR("wrong type id in transaction out");
     return;
  }
  if(is_out_to_acc(acc, boost::get<txout_to_key>(o.target), tx_pub_key, i))
  {
    money_transfered = o.amount;
  }
  else
  {
    money_transfered = 0;
  }
  error = false;
}
//----------------------------------------------------------------------------------------------------
void wallet2::process_new_transaction(const cryptonote::transaction& tx, uint64_t height, bool miner_tx)
{
  if (!miner_tx)
    process_unconfirmed(tx, height);
  std::vector<size_t> outs;
  uint64_t tx_money_got_in_outs = 0;
  crypto::public_key tx_pub_key = null_pkey;

  std::vector<tx_extra_field> tx_extra_fields;
  if(!parse_tx_extra(tx.extra, tx_extra_fields))
  {
    // Extra may only be partially parsed, it's OK if tx_extra_fields contains public key
    LOG_PRINT_L0("Transaction extra has unsupported format: " << get_transaction_hash(tx));
  }

  // Don't try to extract tx public key if tx has no ouputs
  if (!tx.vout.empty()) 
  {
    tx_extra_pub_key pub_key_field;
    if(!find_tx_extra_field_by_type(tx_extra_fields, pub_key_field))
    {
      LOG_PRINT_L0("Public key wasn't found in the transaction extra. Skipping transaction " << get_transaction_hash(tx));
      if(0 != m_callback)
	m_callback->on_skip_transaction(height, tx);
      return;
    }

    tx_pub_key = pub_key_field.pub_key;
    bool r = true;
    int threads;
    if (miner_tx && m_refresh_type == RefreshNoCoinbase)
    {
      // assume coinbase isn't for us
    }
    else if (miner_tx && m_refresh_type == RefreshOptimizeCoinbase)
    {
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        uint64_t money_transfered = 0;
        bool error = false;
        check_acc_out(m_account.get_keys(), tx.vout[i], tx_pub_key, i, money_transfered, error);
        if (error)
        {
          r = false;
          break;
        }
        // this assumes that the miner tx pays a single address
        if (money_transfered == 0)
          break;
        outs.push_back(i);
        tx_money_got_in_outs += money_transfered;
      }
    }
    else if (tx.vout.size() > 1 && (threads = std::thread::hardware_concurrency()) > 1)
    {
      boost::asio::io_service ioservice;
      boost::thread_group threadpool;
      std::auto_ptr < boost::asio::io_service::work > work(new boost::asio::io_service::work(ioservice));
      for (int i = 0; i < threads; i++)
      {
        threadpool.create_thread(boost::bind(&boost::asio::io_service::run, &ioservice));
      }

      const account_keys &keys = m_account.get_keys();
      std::vector<uint64_t> money_transfered(tx.vout.size());
      std::deque<bool> error(tx.vout.size());
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        ioservice.dispatch(boost::bind(&wallet2::check_acc_out, this, std::cref(keys), std::cref(tx.vout[i]), std::cref(tx_pub_key), i,
          std::ref(money_transfered[i]), std::ref(error[i])));
      }
      KILL_IOSERVICE();
      tx_money_got_in_outs = 0;
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        if (error[i])
        {
          r = false;
          break;
        }
        if (money_transfered[i])
        {
          outs.push_back(i);
          tx_money_got_in_outs += money_transfered[i];
        }
      }
    }
    else
    {
      r = lookup_acc_outs(m_account.get_keys(), tx, tx_pub_key, outs, tx_money_got_in_outs);
    }
    THROW_WALLET_EXCEPTION_IF(!r, error::acc_outs_lookup_error, tx, tx_pub_key, m_account.get_keys());

    if(!outs.empty() && tx_money_got_in_outs)
    {
      //good news - got money! take care about it
      //usually we have only one transfer for user in transaction
      cryptonote::COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::request req = AUTO_VAL_INIT(req);
      cryptonote::COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::response res = AUTO_VAL_INIT(res);
      req.txid = get_transaction_hash(tx);
      m_daemon_rpc_mutex.lock();
      bool r = net_utils::invoke_http_bin_remote_command2(m_daemon_address + "/get_o_indexes.bin", req, res, m_http_client, WALLET_RCP_CONNECTION_TIMEOUT);
      m_daemon_rpc_mutex.unlock();
      THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_o_indexes.bin");
      THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_o_indexes.bin");
      THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::get_out_indices_error, res.status);
      THROW_WALLET_EXCEPTION_IF(res.o_indexes.size() != tx.vout.size(), error::wallet_internal_error,
				"transactions outputs size=" + std::to_string(tx.vout.size()) +
				" not match with COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES response size=" + std::to_string(res.o_indexes.size()));

      BOOST_FOREACH(size_t o, outs)
      {
	THROW_WALLET_EXCEPTION_IF(tx.vout.size() <= o, error::wallet_internal_error, "wrong out in transaction: internal index=" +
				  std::to_string(o) + ", total_outs=" + std::to_string(tx.vout.size()));

	m_transfers.push_back(boost::value_initialized<transfer_details>());
	transfer_details& td = m_transfers.back();
	td.m_block_height = height;
	td.m_internal_output_index = o;
	td.m_global_output_index = res.o_indexes[o];
	td.m_tx = tx;
	td.m_spent = false;
	cryptonote::keypair in_ephemeral;
	cryptonote::generate_key_image_helper(m_account.get_keys(), tx_pub_key, o, in_ephemeral, td.m_key_image);
	THROW_WALLET_EXCEPTION_IF(in_ephemeral.pub != boost::get<cryptonote::txout_to_key>(tx.vout[o].target).key,
				  error::wallet_internal_error, "key_image generated ephemeral public key not matched with output_key");

	m_key_images[td.m_key_image] = m_transfers.size()-1;
	LOG_PRINT_L0("Received money: " << print_money(td.amount()) << ", with tx: " << get_transaction_hash(tx));
	if (0 != m_callback)
	  m_callback->on_money_received(height, td.m_tx, td.m_internal_output_index);
      }
    }
  }

  uint64_t tx_money_spent_in_ins = 0;
  // check all outputs for spending (compare key images)
  BOOST_FOREACH(auto& in, tx.vin)
  {
    if(in.type() != typeid(cryptonote::txin_to_key))
      continue;
    auto it = m_key_images.find(boost::get<cryptonote::txin_to_key>(in).k_image);
    if(it != m_key_images.end())
    {
      LOG_PRINT_L0("Spent money: " << print_money(boost::get<cryptonote::txin_to_key>(in).amount) << ", with tx: " << get_transaction_hash(tx));
      tx_money_spent_in_ins += boost::get<cryptonote::txin_to_key>(in).amount;
      transfer_details& td = m_transfers[it->second];
      td.m_spent = true;
      if (0 != m_callback)
        m_callback->on_money_spent(height, td.m_tx, td.m_internal_output_index, tx);
    }
  }

  if (tx_money_spent_in_ins > 0)
  {
    process_outgoing(tx, height, tx_money_spent_in_ins, tx_money_got_in_outs);
  }

  uint64_t received = (tx_money_spent_in_ins < tx_money_got_in_outs) ? tx_money_got_in_outs - tx_money_spent_in_ins : 0;
  if (0 < received)
  {
    tx_extra_nonce extra_nonce;
    crypto::hash payment_id = null_hash;
    if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
    {
      crypto::hash8 payment_id8 = null_hash8;
      if(get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
      {
        // We got a payment ID to go with this tx
        LOG_PRINT_L2("Found encrypted payment ID: " << payment_id8);
        if (tx_pub_key != null_pkey)
        {
          if (!decrypt_payment_id(payment_id8, tx_pub_key, m_account.get_keys().m_view_secret_key))
          {
            LOG_PRINT_L0("Failed to decrypt payment ID: " << payment_id8);
          }
          else
          {
            LOG_PRINT_L2("Decrypted payment ID: " << payment_id8);
            // put the 64 bit decrypted payment id in the first 8 bytes
            memcpy(payment_id.data, payment_id8.data, 8);
            // rest is already 0, but guard against code changes above
            memset(payment_id.data + 8, 0, 24);
          }
        }
        else
        {
          LOG_PRINT_L1("No public key found in tx, unable to decrypt payment id");
        }
      }
      else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
      {
        LOG_PRINT_L2("Found unencrypted payment ID: " << payment_id);
      }
    }
    else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
    {
      LOG_PRINT_L2("Found unencrypted payment ID: " << payment_id);
    }

    payment_details payment;
    payment.m_tx_hash      = cryptonote::get_transaction_hash(tx);
    payment.m_amount       = received;
    payment.m_block_height = height;
    payment.m_unlock_time  = tx.unlock_time;
    m_payments.emplace(payment_id, payment);
    LOG_PRINT_L2("Payment found: " << payment_id << " / " << payment.m_tx_hash << " / " << payment.m_amount);
  }
}
//----------------------------------------------------------------------------------------------------
void wallet2::process_unconfirmed(const cryptonote::transaction& tx, uint64_t height)
{
  crypto::hash txid = get_transaction_hash(tx);
  auto unconf_it = m_unconfirmed_txs.find(txid);
  if(unconf_it != m_unconfirmed_txs.end()) {
    if (store_tx_info()) {
      try {
        m_confirmed_txs.insert(std::make_pair(txid, confirmed_transfer_details(unconf_it->second, height)));
      }
      catch (...) {
        // can fail if the tx has unexpected input types
        LOG_PRINT_L0("Failed to add outgoing transaction to confirmed transaction map");
      }
    }
    m_unconfirmed_txs.erase(unconf_it);
  }
}
//----------------------------------------------------------------------------------------------------
void wallet2::process_outgoing(const cryptonote::transaction &tx, uint64_t height, uint64_t spent, uint64_t received)
{
  crypto::hash txid = get_transaction_hash(tx);
  confirmed_transfer_details &ctd = m_confirmed_txs[txid];
  // operator[] creates if not found
  // fill with the info we know, some info might already be there
  ctd.m_amount_in = spent;
  ctd.m_amount_out = get_outs_money_amount(tx);
  ctd.m_change = received;
  ctd.m_block_height = height;
}
//----------------------------------------------------------------------------------------------------
void wallet2::process_new_blockchain_entry(const cryptonote::block& b, const cryptonote::block_complete_entry& bche, const crypto::hash& bl_id, uint64_t height)
{
  //handle transactions from new block
    
  //optimization: seeking only for blocks that are not older then the wallet creation time plus 1 day. 1 day is for possible user incorrect time setup
  if(b.timestamp + 60*60*24 > m_account.get_createtime())
  {
    TIME_MEASURE_START(miner_tx_handle_time);
    process_new_transaction(b.miner_tx, height, true);
    TIME_MEASURE_FINISH(miner_tx_handle_time);

    TIME_MEASURE_START(txs_handle_time);
    BOOST_FOREACH(auto& txblob, bche.txs)
    {
      cryptonote::transaction tx;
      bool r = parse_and_validate_tx_from_blob(txblob, tx);
      THROW_WALLET_EXCEPTION_IF(!r, error::tx_parse_error, txblob);
      process_new_transaction(tx, height, false);
    }
    TIME_MEASURE_FINISH(txs_handle_time);
    LOG_PRINT_L2("Processed block: " << bl_id << ", height " << height << ", " <<  miner_tx_handle_time + txs_handle_time << "(" << miner_tx_handle_time << "/" << txs_handle_time <<")ms");
  }else
  {
    LOG_PRINT_L2( "Skipped block by timestamp, height: " << height << ", block time " << b.timestamp << ", account time " << m_account.get_createtime());
  }
  m_blockchain.push_back(bl_id);
  ++m_local_bc_height;

  if (0 != m_callback)
    m_callback->on_new_block(height, b);
}
//----------------------------------------------------------------------------------------------------
void wallet2::get_short_chain_history(std::list<crypto::hash>& ids) const
{
  size_t i = 0;
  size_t current_multiplier = 1;
  size_t sz = m_blockchain.size();
  if(!sz)
    return;
  size_t current_back_offset = 1;
  bool genesis_included = false;
  while(current_back_offset < sz)
  {
    ids.push_back(m_blockchain[sz-current_back_offset]);
    if(sz-current_back_offset == 0)
      genesis_included = true;
    if(i < 10)
    {
      ++current_back_offset;
    }else
    {
      current_back_offset += current_multiplier *= 2;
    }
    ++i;
  }
  if(!genesis_included)
    ids.push_back(m_blockchain[0]);
}
//----------------------------------------------------------------------------------------------------
void wallet2::parse_block_round(const cryptonote::blobdata &blob, cryptonote::block &bl, crypto::hash &bl_id, bool &error) const
{
  error = !cryptonote::parse_and_validate_block_from_blob(blob, bl);
  if (!error)
    bl_id = get_block_hash(bl);
}
//----------------------------------------------------------------------------------------------------
void wallet2::pull_blocks(uint64_t start_height, uint64_t &blocks_start_height, const std::list<crypto::hash> &short_chain_history, std::list<cryptonote::block_complete_entry> &blocks)
{
  cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::request req = AUTO_VAL_INIT(req);
  cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::response res = AUTO_VAL_INIT(res);
  req.block_ids = short_chain_history;

  req.start_height = start_height;
  m_daemon_rpc_mutex.lock();
  bool r = net_utils::invoke_http_bin_remote_command2(m_daemon_address + "/getblocks.bin", req, res, m_http_client, WALLET_RCP_CONNECTION_TIMEOUT);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "getblocks.bin");
  THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "getblocks.bin");
  THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::get_blocks_error, res.status);

  blocks_start_height = res.start_height;
  blocks = res.blocks;
}
//----------------------------------------------------------------------------------------------------
void wallet2::process_blocks(uint64_t start_height, const std::list<cryptonote::block_complete_entry> &blocks, uint64_t& blocks_added)
{
  size_t current_index = start_height;
  blocks_added = 0;

  int threads = std::thread::hardware_concurrency();
  if (threads > 1)
  {
    std::vector<crypto::hash> round_block_hashes(threads);
    std::vector<cryptonote::block> round_blocks(threads);
    std::deque<bool> error(threads);
    size_t blocks_size = blocks.size();
    std::list<block_complete_entry>::const_iterator blocki = blocks.begin();
    for (size_t b = 0; b < blocks_size; b += threads)
    {
      size_t round_size = std::min((size_t)threads, blocks_size - b);

      boost::asio::io_service ioservice;
      boost::thread_group threadpool;
      std::unique_ptr < boost::asio::io_service::work > work(new boost::asio::io_service::work(ioservice));
      for (size_t i = 0; i < round_size; i++)
      {
        threadpool.create_thread(boost::bind(&boost::asio::io_service::run, &ioservice));
      }

      std::list<block_complete_entry>::const_iterator tmpblocki = blocki;
      for (size_t i = 0; i < round_size; ++i)
      {
        ioservice.dispatch(boost::bind(&wallet2::parse_block_round, this, std::cref(tmpblocki->block),
          std::ref(round_blocks[i]), std::ref(round_block_hashes[i]), std::ref(error[i])));
        ++tmpblocki;
      }
      KILL_IOSERVICE();
      tmpblocki = blocki;
      for (size_t i = 0; i < round_size; ++i)
      {
        THROW_WALLET_EXCEPTION_IF(error[i], error::block_parse_error, tmpblocki->block);
        ++tmpblocki;
      }
      for (size_t i = 0; i < round_size; ++i)
      {
        const crypto::hash &bl_id = round_block_hashes[i];
        cryptonote::block &bl = round_blocks[i];

        if(current_index >= m_blockchain.size())
        {
          process_new_blockchain_entry(bl, *blocki, bl_id, current_index);
          ++blocks_added;
        }
        else if(bl_id != m_blockchain[current_index])
        {
          //split detected here !!!
          THROW_WALLET_EXCEPTION_IF(current_index == start_height, error::wallet_internal_error,
            "wrong daemon response: split starts from the first block in response " + string_tools::pod_to_hex(bl_id) +
            " (height " + std::to_string(start_height) + "), local block id at this height: " +
            string_tools::pod_to_hex(m_blockchain[current_index]));

          detach_blockchain(current_index);
          process_new_blockchain_entry(bl, *blocki, bl_id, current_index);
        }
        else
        {
          LOG_PRINT_L2("Block is already in blockchain: " << string_tools::pod_to_hex(bl_id));
        }
        ++current_index;
        ++blocki;
      }
    }
  }
  else
  {
  BOOST_FOREACH(auto& bl_entry, blocks)
  {
    cryptonote::block bl;
    bool r = cryptonote::parse_and_validate_block_from_blob(bl_entry.block, bl);
    THROW_WALLET_EXCEPTION_IF(!r, error::block_parse_error, bl_entry.block);

    crypto::hash bl_id = get_block_hash(bl);
    if(current_index >= m_blockchain.size())
    {
      process_new_blockchain_entry(bl, bl_entry, bl_id, current_index);
      ++blocks_added;
    }
    else if(bl_id != m_blockchain[current_index])
    {
      //split detected here !!!
      THROW_WALLET_EXCEPTION_IF(current_index == start_height, error::wallet_internal_error,
        "wrong daemon response: split starts from the first block in response " + string_tools::pod_to_hex(bl_id) +
        " (height " + std::to_string(start_height) + "), local block id at this height: " +
        string_tools::pod_to_hex(m_blockchain[current_index]));

      detach_blockchain(current_index);
      process_new_blockchain_entry(bl, bl_entry, bl_id, current_index);
    }
    else
    {
      LOG_PRINT_L2("Block is already in blockchain: " << string_tools::pod_to_hex(bl_id));
    }

    ++current_index;
  }
  }
}
//----------------------------------------------------------------------------------------------------
void wallet2::refresh()
{
  uint64_t blocks_fetched = 0;
  refresh(0, blocks_fetched);
}
//----------------------------------------------------------------------------------------------------
void wallet2::refresh(uint64_t start_height, uint64_t & blocks_fetched)
{
  bool received_money = false;
  refresh(start_height, blocks_fetched, received_money);
}
//----------------------------------------------------------------------------------------------------
void wallet2::pull_next_blocks(uint64_t start_height, uint64_t &blocks_start_height, std::list<crypto::hash> &short_chain_history, const std::list<cryptonote::block_complete_entry> &prev_blocks, std::list<cryptonote::block_complete_entry> &blocks)
{
  // prepend the last 3 blocks, should be enough to guard against a block or two's reorg
  cryptonote::block bl;
  std::list<cryptonote::block_complete_entry>::const_reverse_iterator i = prev_blocks.rbegin();
  for (size_t n = 0; n < std::min((size_t)3, prev_blocks.size()); ++n)
  {
    bool ok = cryptonote::parse_and_validate_block_from_blob(i->block, bl);
    THROW_WALLET_EXCEPTION_IF(!ok, error::block_parse_error, i->block);
    short_chain_history.push_front(cryptonote::get_block_hash(bl));
    ++i;
  }

  // pull the new blocks
  pull_blocks(start_height, blocks_start_height, short_chain_history, blocks);
}
//----------------------------------------------------------------------------------------------------
void wallet2::refresh(uint64_t start_height, uint64_t & blocks_fetched, bool& received_money)
{
  received_money = false;
  blocks_fetched = 0;
  uint64_t added_blocks = 0;
  size_t try_count = 0;
  crypto::hash last_tx_hash_id = m_transfers.size() ? get_transaction_hash(m_transfers.back().m_tx) : null_hash;
  std::list<crypto::hash> short_chain_history;
  std::thread pull_thread;
  uint64_t blocks_start_height;
  std::list<cryptonote::block_complete_entry> blocks;

  // pull the first set of blocks
  get_short_chain_history(short_chain_history);
  pull_blocks(start_height, blocks_start_height, short_chain_history, blocks);

  while(m_run.load(std::memory_order_relaxed))
  {
    try
    {
      // pull the next set of blocks while we're processing the current one
      uint64_t next_blocks_start_height;
      std::list<cryptonote::block_complete_entry> next_blocks;
      pull_thread = std::thread([&]{pull_next_blocks(start_height, next_blocks_start_height, short_chain_history, blocks, next_blocks);});

      process_blocks(blocks_start_height, blocks, added_blocks);
      blocks_fetched += added_blocks;
      pull_thread.join();
      if(!added_blocks)
        break;

      // switch to the new blocks from the daemon
      blocks_start_height = next_blocks_start_height;
      blocks = next_blocks;
    }
    catch (const std::exception&)
    {
      blocks_fetched += added_blocks;
      if(try_count < 3)
      {
        LOG_PRINT_L1("Another try pull_blocks (try_count=" << try_count << ")...");
        ++try_count;
      }
      else
      {
        LOG_ERROR("pull_blocks failed, try_count=" << try_count);
        throw;
      }
    }
  }
  if(last_tx_hash_id != (m_transfers.size() ? get_transaction_hash(m_transfers.back().m_tx) : null_hash))
    received_money = true;

  LOG_PRINT_L1("Refresh done, blocks received: " << blocks_fetched << ", balance: " << print_money(balance()) << ", unlocked: " << print_money(unlocked_balance()));
}
//----------------------------------------------------------------------------------------------------
bool wallet2::refresh(uint64_t & blocks_fetched, bool& received_money, bool& ok)
{
  try
  {
    refresh(0, blocks_fetched, received_money);
    ok = true;
  }
  catch (...)
  {
    ok = false;
  }
  return ok;
}
//----------------------------------------------------------------------------------------------------
void wallet2::detach_blockchain(uint64_t height)
{
  LOG_PRINT_L0("Detaching blockchain on height " << height);
  size_t transfers_detached = 0;

  auto it = std::find_if(m_transfers.begin(), m_transfers.end(), [&](const transfer_details& td){return td.m_block_height >= height;});
  size_t i_start = it - m_transfers.begin();

  for(size_t i = i_start; i!= m_transfers.size();i++)
  {
    auto it_ki = m_key_images.find(m_transfers[i].m_key_image);
    THROW_WALLET_EXCEPTION_IF(it_ki == m_key_images.end(), error::wallet_internal_error, "key image not found");
    m_key_images.erase(it_ki);
    ++transfers_detached;
  }
  m_transfers.erase(it, m_transfers.end());

  size_t blocks_detached = m_blockchain.end() - (m_blockchain.begin()+height);
  m_blockchain.erase(m_blockchain.begin()+height, m_blockchain.end());
  m_local_bc_height -= blocks_detached;

  for (auto it = m_payments.begin(); it != m_payments.end(); )
  {
    if(height <= it->second.m_block_height)
      it = m_payments.erase(it);
    else
      ++it;
  }

  LOG_PRINT_L0("Detached blockchain on height " << height << ", transfers detached " << transfers_detached << ", blocks detached " << blocks_detached);
}
//----------------------------------------------------------------------------------------------------
bool wallet2::deinit()
{
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet2::clear()
{
  m_blockchain.clear();
  m_transfers.clear();
  m_local_bc_height = 1;
  return true;
}

/*!
 * \brief Stores wallet information to wallet file.
 * \param  keys_file_name Name of wallet file
 * \param  password       Password of wallet file
 * \param  watch_only     true to save only view key, false to save both spend and view keys
 * \return                Whether it was successful.
 */
bool wallet2::store_keys(const std::string& keys_file_name, const std::string& password, bool watch_only)
{
  std::string account_data;
  cryptonote::account_base account = m_account;

  if (watch_only)
    account.forget_spend_key();
  bool r = epee::serialization::store_t_to_binary(account, account_data);
  CHECK_AND_ASSERT_MES(r, false, "failed to serialize wallet keys");
  wallet2::keys_file_data keys_file_data = boost::value_initialized<wallet2::keys_file_data>();

  // Create a JSON object with "key_data" and "seed_language" as keys.
  rapidjson::Document json;
  json.SetObject();
  rapidjson::Value value(rapidjson::kStringType);
  value.SetString(account_data.c_str(), account_data.length());
  json.AddMember("key_data", value, json.GetAllocator());
  if (!seed_language.empty())
  {
    value.SetString(seed_language.c_str(), seed_language.length());
    json.AddMember("seed_language", value, json.GetAllocator());
  }

  rapidjson::Value value2(rapidjson::kNumberType);
  value2.SetInt(watch_only ? 1 :0); // WTF ? JSON has different true and false types, and not boolean ??
  json.AddMember("watch_only", value2, json.GetAllocator());

  value2.SetInt(m_always_confirm_transfers ? 1 :0);
  json.AddMember("always_confirm_transfers", value2, json.GetAllocator());

  value2.SetInt(m_store_tx_info ? 1 :0);
  json.AddMember("store_tx_info", value2, json.GetAllocator());

  value2.SetUint(m_default_mixin);
  json.AddMember("default_mixin", value2, json.GetAllocator());

  // Serialize the JSON object
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  json.Accept(writer);
  account_data = buffer.GetString();

  // Encrypt the entire JSON object.
  crypto::chacha8_key key;
  crypto::generate_chacha8_key(password, key);
  std::string cipher;
  cipher.resize(account_data.size());
  keys_file_data.iv = crypto::rand<crypto::chacha8_iv>();
  crypto::chacha8(account_data.data(), account_data.size(), key, keys_file_data.iv, &cipher[0]);
  keys_file_data.account_data = cipher;

  std::string buf;
  r = ::serialization::dump_binary(keys_file_data, buf);
  r = r && epee::file_io_utils::save_string_to_file(keys_file_name, buf); //and never touch wallet_keys_file again, only read
  CHECK_AND_ASSERT_MES(r, false, "failed to generate wallet keys file " << keys_file_name);

  return true;
}
//----------------------------------------------------------------------------------------------------
namespace
{
  bool verify_keys(const crypto::secret_key& sec, const crypto::public_key& expected_pub)
  {
    crypto::public_key pub;
    bool r = crypto::secret_key_to_public_key(sec, pub);
    return r && expected_pub == pub;
  }
}

/*!
 * \brief Load wallet information from wallet file.
 * \param keys_file_name Name of wallet file
 * \param password       Password of wallet file
 */
void wallet2::load_keys(const std::string& keys_file_name, const std::string& password)
{
  wallet2::keys_file_data keys_file_data;
  std::string buf;
  bool r = epee::file_io_utils::load_file_to_string(keys_file_name, buf);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_read_error, keys_file_name);

  // Decrypt the contents
  r = ::serialization::parse_binary(buf, keys_file_data);
  THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "internal error: failed to deserialize \"" + keys_file_name + '\"');
  crypto::chacha8_key key;
  crypto::generate_chacha8_key(password, key);
  std::string account_data;
  account_data.resize(keys_file_data.account_data.size());
  crypto::chacha8(keys_file_data.account_data.data(), keys_file_data.account_data.size(), key, keys_file_data.iv, &account_data[0]);

  // The contents should be JSON if the wallet follows the new format.
  rapidjson::Document json;
  if (json.Parse(account_data.c_str(), keys_file_data.account_data.size()).HasParseError())
  {
    is_old_file_format = true;
    m_watch_only = false;
    m_always_confirm_transfers = false;
    m_default_mixin = 0;
  }
  else
  {
    account_data = std::string(json["key_data"].GetString(), json["key_data"].GetString() +
      json["key_data"].GetStringLength());
    if (json.HasMember("seed_language"))
    {
      set_seed_language(std::string(json["seed_language"].GetString(), json["seed_language"].GetString() +
        json["seed_language"].GetStringLength()));
    }
    if (json.HasMember("watch_only"))
    {
      m_watch_only = json["watch_only"].GetInt() != 0;
    }
    else
    {
      m_watch_only = false;
    }
    m_always_confirm_transfers = json.HasMember("always_confirm_transfers") && (json["always_confirm_transfers"].GetInt() != 0);
    m_store_tx_info = (json.HasMember("store_tx_keys") && (json["store_tx_keys"].GetInt() != 0))
                   || (json.HasMember("store_tx_info") && (json["store_tx_info"].GetInt() != 0));
    m_default_mixin = json.HasMember("default_mixin") ? json["default_mixin"].GetUint() : 0;
  }

  const cryptonote::account_keys& keys = m_account.get_keys();
  r = epee::serialization::load_t_from_binary(m_account, account_data);
  r = r && verify_keys(keys.m_view_secret_key,  keys.m_account_address.m_view_public_key);
  if(!m_watch_only)
    r = r && verify_keys(keys.m_spend_secret_key, keys.m_account_address.m_spend_public_key);
  THROW_WALLET_EXCEPTION_IF(!r, error::invalid_password);
}

/*!
 * \brief verify password for default wallet keys file.
 * \param password       Password to verify
 *
 * for verification only
 * should not mutate state, unlike load_keys()
 * can be used prior to rewriting wallet keys file, to ensure user has entered the correct password
 *
 */
bool wallet2::verify_password(const std::string& password) const
{
  const std::string keys_file_name = m_keys_file;
  wallet2::keys_file_data keys_file_data;
  std::string buf;
  bool r = epee::file_io_utils::load_file_to_string(keys_file_name, buf);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_read_error, keys_file_name);

  // Decrypt the contents
  r = ::serialization::parse_binary(buf, keys_file_data);
  THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "internal error: failed to deserialize \"" + keys_file_name + '\"');
  crypto::chacha8_key key;
  crypto::generate_chacha8_key(password, key);
  std::string account_data;
  account_data.resize(keys_file_data.account_data.size());
  crypto::chacha8(keys_file_data.account_data.data(), keys_file_data.account_data.size(), key, keys_file_data.iv, &account_data[0]);

  // The contents should be JSON if the wallet follows the new format.
  rapidjson::Document json;
  if (json.Parse(account_data.c_str(), keys_file_data.account_data.size()).HasParseError())
  {
    // old format before JSON wallet key file format
  }
  else
  {
    account_data = std::string(json["key_data"].GetString(), json["key_data"].GetString() +
      json["key_data"].GetStringLength());
  }

  cryptonote::account_base account_data_check;

  r = epee::serialization::load_t_from_binary(account_data_check, account_data);
  const cryptonote::account_keys& keys = account_data_check.get_keys();

  r = r && verify_keys(keys.m_view_secret_key,  keys.m_account_address.m_view_public_key);
  r = r && verify_keys(keys.m_spend_secret_key, keys.m_account_address.m_spend_public_key);
  return r;
}

/*!
 * \brief  Generates a wallet or restores one.
 * \param  wallet_        Name of wallet file
 * \param  password       Password of wallet file
 * \param  recovery_param If it is a restore, the recovery key
 * \param  recover        Whether it is a restore
 * \param  two_random     Whether it is a non-deterministic wallet
 * \return                The secret key of the generated wallet
 */
crypto::secret_key wallet2::generate(const std::string& wallet_, const std::string& password,
  const crypto::secret_key& recovery_param, bool recover, bool two_random)
{
  clear();
  prepare_file_names(wallet_);

  boost::system::error_code ignored_ec;
  THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_wallet_file, ignored_ec), error::file_exists, m_wallet_file);
  THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_keys_file,   ignored_ec), error::file_exists, m_keys_file);

  crypto::secret_key retval = m_account.generate(recovery_param, recover, two_random);

  m_account_public_address = m_account.get_keys().m_account_address;
  m_watch_only = false;

  bool r = store_keys(m_keys_file, password, false);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);

  r = file_io_utils::save_string_to_file(m_wallet_file + ".address.txt", m_account.get_public_address_str(m_testnet));
  if(!r) LOG_PRINT_RED_L0("String with address text not saved");

  cryptonote::block b;
  generate_genesis(b);
  m_blockchain.push_back(get_block_hash(b));

  store();
  return retval;
}

/*!
* \brief Creates a watch only wallet from a public address and a view secret key.
* \param  wallet_        Name of wallet file
* \param  password       Password of wallet file
* \param  viewkey        view secret key
*/
void wallet2::generate(const std::string& wallet_, const std::string& password,
  const cryptonote::account_public_address &account_public_address,
  const crypto::secret_key& viewkey)
{
  clear();
  prepare_file_names(wallet_);

  boost::system::error_code ignored_ec;
  THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_wallet_file, ignored_ec), error::file_exists, m_wallet_file);
  THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_keys_file,   ignored_ec), error::file_exists, m_keys_file);

  m_account.create_from_viewkey(account_public_address, viewkey);
  m_account_public_address = account_public_address;
  m_watch_only = true;

  bool r = store_keys(m_keys_file, password, true);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);

  r = file_io_utils::save_string_to_file(m_wallet_file + ".address.txt", m_account.get_public_address_str(m_testnet));
  if(!r) LOG_PRINT_RED_L0("String with address text not saved");

  cryptonote::block b;
  generate_genesis(b);
  m_blockchain.push_back(get_block_hash(b));

  store();
}

/*!
 * \brief Rewrites to the wallet file for wallet upgrade (doesn't generate key, assumes it's already there)
 * \param wallet_name Name of wallet file (should exist)
 * \param password    Password for wallet file
 */
void wallet2::rewrite(const std::string& wallet_name, const std::string& password)
{
  prepare_file_names(wallet_name);
  boost::system::error_code ignored_ec;
  THROW_WALLET_EXCEPTION_IF(!boost::filesystem::exists(m_keys_file, ignored_ec), error::file_not_found, m_keys_file);
  bool r = store_keys(m_keys_file, password, false);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);
}
/*!
 * \brief Writes to a file named based on the normal wallet (doesn't generate key, assumes it's already there)
 * \param wallet_name Base name of wallet file
 * \param password    Password for wallet file
 */
void wallet2::write_watch_only_wallet(const std::string& wallet_name, const std::string& password)
{
  prepare_file_names(wallet_name);
  boost::system::error_code ignored_ec;
  std::string filename = m_keys_file + "-watchonly";
  bool watch_only_keys_file_exists = boost::filesystem::exists(filename, ignored_ec);
  THROW_WALLET_EXCEPTION_IF(watch_only_keys_file_exists, error::file_save_error, filename);
  bool r = store_keys(filename, password, true);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, filename);
}
//----------------------------------------------------------------------------------------------------
void wallet2::wallet_exists(const std::string& file_path, bool& keys_file_exists, bool& wallet_file_exists)
{
  std::string keys_file, wallet_file;
  do_prepare_file_names(file_path, keys_file, wallet_file);

  boost::system::error_code ignore;
  keys_file_exists = boost::filesystem::exists(keys_file, ignore);
  wallet_file_exists = boost::filesystem::exists(wallet_file, ignore);
}
//----------------------------------------------------------------------------------------------------
bool wallet2::wallet_valid_path_format(const std::string& file_path)
{
  return !file_path.empty();
}
//----------------------------------------------------------------------------------------------------
bool wallet2::parse_long_payment_id(const std::string& payment_id_str, crypto::hash& payment_id)
{
  cryptonote::blobdata payment_id_data;
  if(!epee::string_tools::parse_hexstr_to_binbuff(payment_id_str, payment_id_data))
    return false;

  if(sizeof(crypto::hash) != payment_id_data.size())
    return false;

  payment_id = *reinterpret_cast<const crypto::hash*>(payment_id_data.data());
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet2::parse_short_payment_id(const std::string& payment_id_str, crypto::hash8& payment_id)
{
  cryptonote::blobdata payment_id_data;
  if(!epee::string_tools::parse_hexstr_to_binbuff(payment_id_str, payment_id_data))
    return false;

  if(sizeof(crypto::hash8) != payment_id_data.size())
    return false;

  payment_id = *reinterpret_cast<const crypto::hash8*>(payment_id_data.data());
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet2::parse_payment_id(const std::string& payment_id_str, crypto::hash& payment_id)
{
  if (parse_long_payment_id(payment_id_str, payment_id))
    return true;
  crypto::hash8 payment_id8;
  if (parse_short_payment_id(payment_id_str, payment_id8))
  {
    memcpy(payment_id.data, payment_id8.data, 8);
    memset(payment_id.data + 8, 0, 24);
    return true;
  }
  return false;
}
//----------------------------------------------------------------------------------------------------
bool wallet2::prepare_file_names(const std::string& file_path)
{
  do_prepare_file_names(file_path, m_keys_file, m_wallet_file);
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet2::check_connection()
{
  std::lock_guard<std::mutex> lock(m_daemon_rpc_mutex);

  if(m_http_client.is_connected())
    return true;

  net_utils::http::url_content u;
  net_utils::parse_url(m_daemon_address, u);

  if(!u.port)
  {
    u.port = m_testnet ? config::testnet::RPC_DEFAULT_PORT : config::RPC_DEFAULT_PORT;
  }

  return m_http_client.connect(u.host, std::to_string(u.port), WALLET_RCP_CONNECTION_TIMEOUT);
}
//----------------------------------------------------------------------------------------------------
bool wallet2::generate_chacha8_key_from_secret_keys(crypto::chacha8_key &key) const
{
  const account_keys &keys = m_account.get_keys();
  const crypto::secret_key &view_key = keys.m_view_secret_key;
  const crypto::secret_key &spend_key = keys.m_spend_secret_key;
  char data[sizeof(view_key) + sizeof(spend_key) + 1];
  memcpy(data, &view_key, sizeof(view_key));
  memcpy(data + sizeof(view_key), &spend_key, sizeof(spend_key));
  data[sizeof(data) - 1] = CHACHA8_KEY_TAIL;
  crypto::generate_chacha8_key(data, sizeof(data), key);
  memset(data, 0, sizeof(data));
  return true;
}
//----------------------------------------------------------------------------------------------------
void wallet2::load(const std::string& wallet_, const std::string& password)
{
  clear();
  prepare_file_names(wallet_);

  boost::system::error_code e;
  bool exists = boost::filesystem::exists(m_keys_file, e);
  THROW_WALLET_EXCEPTION_IF(e || !exists, error::file_not_found, m_keys_file);

  load_keys(m_keys_file, password);
  LOG_PRINT_L0("Loaded wallet keys file, with public address: " << m_account.get_public_address_str(m_testnet));

  //keys loaded ok!
  //try to load wallet file. but even if we failed, it is not big problem
  if(!boost::filesystem::exists(m_wallet_file, e) || e)
  {
    LOG_PRINT_L0("file not found: " << m_wallet_file << ", starting with empty blockchain");
    m_account_public_address = m_account.get_keys().m_account_address;
  }
  else
  {
    wallet2::cache_file_data cache_file_data;
    std::string buf;
    bool r = epee::file_io_utils::load_file_to_string(m_wallet_file, buf);
    THROW_WALLET_EXCEPTION_IF(!r, error::file_read_error, m_wallet_file);

    // try to read it as an encrypted cache
    try
    {
      LOG_PRINT_L1("Trying to decrypt cache data");

      r = ::serialization::parse_binary(buf, cache_file_data);
      THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "internal error: failed to deserialize \"" + m_wallet_file + '\"');
      crypto::chacha8_key key;
      generate_chacha8_key_from_secret_keys(key);
      std::string cache_data;
      cache_data.resize(cache_file_data.cache_data.size());
      crypto::chacha8(cache_file_data.cache_data.data(), cache_file_data.cache_data.size(), key, cache_file_data.iv, &cache_data[0]);

      std::stringstream iss;
      iss << cache_data;
      boost::archive::binary_iarchive ar(iss);
      ar >> *this;
    }
    catch (...)
    {
      LOG_PRINT_L1("Failed to load encrypted cache, trying unencrypted");
      std::stringstream iss;
      iss << buf;
      boost::archive::binary_iarchive ar(iss);
      ar >> *this;
    }
    THROW_WALLET_EXCEPTION_IF(
      m_account_public_address.m_spend_public_key != m_account.get_keys().m_account_address.m_spend_public_key ||
      m_account_public_address.m_view_public_key  != m_account.get_keys().m_account_address.m_view_public_key,
      error::wallet_files_doesnt_correspond, m_keys_file, m_wallet_file);
  }

  cryptonote::block genesis;
  generate_genesis(genesis);
  crypto::hash genesis_hash = get_block_hash(genesis);

  if (m_blockchain.empty())
  {
    m_blockchain.push_back(genesis_hash);
  }
  else
  {
    check_genesis(genesis_hash);
  }

  m_local_bc_height = m_blockchain.size();
}
//----------------------------------------------------------------------------------------------------
void wallet2::check_genesis(const crypto::hash& genesis_hash) const {
  std::string what("Genesis block missmatch. You probably use wallet without testnet flag with blockchain from test network or vice versa");

  THROW_WALLET_EXCEPTION_IF(genesis_hash != m_blockchain[0], error::wallet_internal_error, what);
}
//----------------------------------------------------------------------------------------------------
void wallet2::store()
{
  std::stringstream oss;
  boost::archive::binary_oarchive ar(oss);
  ar << *this;

  wallet2::cache_file_data cache_file_data = boost::value_initialized<wallet2::cache_file_data>();
  cache_file_data.cache_data = oss.str();
  crypto::chacha8_key key;
  generate_chacha8_key_from_secret_keys(key);
  std::string cipher;
  cipher.resize(cache_file_data.cache_data.size());
  cache_file_data.iv = crypto::rand<crypto::chacha8_iv>();
  crypto::chacha8(cache_file_data.cache_data.data(), cache_file_data.cache_data.size(), key, cache_file_data.iv, &cipher[0]);
  cache_file_data.cache_data = cipher;

  std::string buf;
  bool r = ::serialization::dump_binary(cache_file_data, buf);
  r = r && epee::file_io_utils::save_string_to_file(m_wallet_file, buf);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_wallet_file);
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet2::unlocked_balance() const
{
  uint64_t amount = 0;
  BOOST_FOREACH(const transfer_details& td, m_transfers)
    if(!td.m_spent && is_transfer_unlocked(td))
      amount += td.amount();

  return amount;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet2::balance() const
{
  uint64_t amount = 0;
  BOOST_FOREACH(auto& td, m_transfers)
    if(!td.m_spent)
      amount += td.amount();


  BOOST_FOREACH(auto& utx, m_unconfirmed_txs)
    amount+= utx.second.m_change;

  return amount;
}
//----------------------------------------------------------------------------------------------------
void wallet2::get_transfers(wallet2::transfer_container& incoming_transfers) const
{
  incoming_transfers = m_transfers;
}
//----------------------------------------------------------------------------------------------------
void wallet2::get_payments(const crypto::hash& payment_id, std::list<wallet2::payment_details>& payments, uint64_t min_height) const
{
  auto range = m_payments.equal_range(payment_id);
  std::for_each(range.first, range.second, [&payments, &min_height](const payment_container::value_type& x) {
    if (min_height < x.second.m_block_height)
    {
      payments.push_back(x.second);
    }
  });
}
//----------------------------------------------------------------------------------------------------
void wallet2::get_payments(std::list<std::pair<crypto::hash,wallet2::payment_details>>& payments, uint64_t min_height, uint64_t max_height) const
{
  auto range = std::make_pair(m_payments.begin(), m_payments.end());
  std::for_each(range.first, range.second, [&payments, &min_height, &max_height](const payment_container::value_type& x) {
    if (min_height < x.second.m_block_height && max_height >= x.second.m_block_height)
    {
      payments.push_back(x);
    }
  });
}
//----------------------------------------------------------------------------------------------------
void wallet2::get_payments_out(std::list<std::pair<crypto::hash,wallet2::confirmed_transfer_details>>& confirmed_payments,
    uint64_t min_height, uint64_t max_height) const
{
  for (auto i = m_confirmed_txs.begin(); i != m_confirmed_txs.end(); ++i) {
    if (i->second.m_block_height > min_height && i->second.m_block_height <= max_height) {
      confirmed_payments.push_back(*i);
    }
  }
}
//----------------------------------------------------------------------------------------------------
void wallet2::get_unconfirmed_payments_out(std::list<std::pair<crypto::hash,wallet2::unconfirmed_transfer_details>>& unconfirmed_payments) const
{
  for (auto i = m_unconfirmed_txs.begin(); i != m_unconfirmed_txs.end(); ++i) {
    unconfirmed_payments.push_back(*i);
  }
}
//----------------------------------------------------------------------------------------------------
void wallet2::rescan_spent()
{
  std::vector<std::string> key_images;

  // make a list of key images for all our outputs
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    const transfer_details& td = m_transfers[i];
    key_images.push_back(string_tools::pod_to_hex(td.m_key_image));
  }

  COMMAND_RPC_IS_KEY_IMAGE_SPENT::request req = AUTO_VAL_INIT(req);
  COMMAND_RPC_IS_KEY_IMAGE_SPENT::response daemon_resp = AUTO_VAL_INIT(daemon_resp);
  req.key_images = key_images;
  m_daemon_rpc_mutex.lock();
  bool r = epee::net_utils::invoke_http_json_remote_command2(m_daemon_address + "/is_key_image_spent", req, daemon_resp, m_http_client, 200000);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "is_key_image_spent");
  THROW_WALLET_EXCEPTION_IF(daemon_resp.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "is_key_image_spent");
  THROW_WALLET_EXCEPTION_IF(daemon_resp.status != CORE_RPC_STATUS_OK, error::is_key_image_spent_error, daemon_resp.status);
  THROW_WALLET_EXCEPTION_IF(daemon_resp.spent_status.size() != key_images.size(), error::wallet_internal_error,
    "daemon returned wrong response for is_key_image_spent, wrong amounts count = " +
    std::to_string(daemon_resp.spent_status.size()) + ", expected " +  std::to_string(key_images.size()));

  // update spent status
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    transfer_details& td = m_transfers[i];
    if (td.m_spent != daemon_resp.spent_status[i])
    {
      if (td.m_spent)
      {
        LOG_PRINT_L1("Marking output " << i << " as unspent, it was marked as spent");
      }
      else
      {
        LOG_PRINT_L1("Marking output " << i << " as spent, it was marked as unspent");
      }
      td.m_spent = daemon_resp.spent_status[i];
    }
  }
}
//----------------------------------------------------------------------------------------------------
bool wallet2::is_transfer_unlocked(const transfer_details& td) const
{
  if(!is_tx_spendtime_unlocked(td.m_tx.unlock_time))
    return false;

  if(td.m_block_height + CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE > m_blockchain.size())
    return false;

  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet2::is_tx_spendtime_unlocked(uint64_t unlock_time) const
{
  if(unlock_time < CRYPTONOTE_MAX_BLOCK_NUMBER)
  {
    //interpret as block index
    if(m_blockchain.size()-1 + CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_BLOCKS >= unlock_time)
      return true;
    else
      return false;
  }else
  {
    //interpret as time
    uint64_t current_time = static_cast<uint64_t>(time(NULL));
    if(current_time + CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_SECONDS >= unlock_time)
      return true;
    else
      return false;
  }
  return false;
}
//----------------------------------------------------------------------------------------------------
namespace
{
  template<typename T>
  T pop_index(std::vector<T>& vec, size_t idx)
  {
    CHECK_AND_ASSERT_MES(!vec.empty(), T(), "Vector must be non-empty");

    T res = vec[idx];
    if (idx + 1 != vec.size())
    {
      vec[idx] = vec.back();
    }
    vec.resize(vec.size() - 1);

    return res;
  }

  template<typename T>
  T pop_random_value(std::vector<T>& vec)
  {
    CHECK_AND_ASSERT_MES(!vec.empty(), T(), "Vector must be non-empty");

    size_t idx = crypto::rand<size_t>() % vec.size();
    return pop_index (vec, idx);
  }
}
//----------------------------------------------------------------------------------------------------
// Select random input sources for transaction.
// returns:
//    direct return: amount of money found
//    modified reference: selected_transfers, a list of iterators/indices of input sources
uint64_t wallet2::select_transfers(uint64_t needed_money, bool add_dust, uint64_t dust, std::list<transfer_container::iterator>& selected_transfers)
{
  std::vector<size_t> unused_transfers_indices;
  std::vector<size_t> unused_dust_indices;

  // aggregate sources available for transfers
  // if dust needed, take dust from only one source (so require source has at least dust amount)
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    const transfer_details& td = m_transfers[i];
    if (!td.m_spent && is_transfer_unlocked(td))
    {
      if (dust < td.amount() && is_valid_decomposed_amount(td.amount()))
        unused_transfers_indices.push_back(i);
      else
        unused_dust_indices.push_back(i);
    }
  }

  bool select_one_dust = add_dust && !unused_dust_indices.empty();
  uint64_t found_money = 0;
  while (found_money < needed_money && (!unused_transfers_indices.empty() || !unused_dust_indices.empty()))
  {
    size_t idx;
    if (select_one_dust)
    {
      idx = pop_random_value(unused_dust_indices);
      select_one_dust = false;
    }
    else
    {
      idx = !unused_transfers_indices.empty() ? pop_random_value(unused_transfers_indices) : pop_random_value(unused_dust_indices);
    }

    transfer_container::iterator it = m_transfers.begin() + idx;
    selected_transfers.push_back(it);
    found_money += it->amount();
  }

  return found_money;
}
//----------------------------------------------------------------------------------------------------
void wallet2::add_unconfirmed_tx(const cryptonote::transaction& tx, const std::vector<cryptonote::tx_destination_entry> &dests, const crypto::hash &payment_id, uint64_t change_amount)
{
  unconfirmed_transfer_details& utd = m_unconfirmed_txs[cryptonote::get_transaction_hash(tx)];
  utd.m_change = change_amount;
  utd.m_sent_time = time(NULL);
  utd.m_tx = tx;
  utd.m_dests = dests;
  utd.m_payment_id = payment_id;
}

//----------------------------------------------------------------------------------------------------
void wallet2::transfer(const std::vector<cryptonote::tx_destination_entry>& dsts, size_t fake_outputs_count,
                       uint64_t unlock_time, uint64_t fee, const std::vector<uint8_t>& extra, cryptonote::transaction& tx, pending_tx& ptx)
{
  transfer(dsts, fake_outputs_count, unlock_time, fee, extra, detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD), tx, ptx);
}
//----------------------------------------------------------------------------------------------------
void wallet2::transfer(const std::vector<cryptonote::tx_destination_entry>& dsts, size_t fake_outputs_count,
                       uint64_t unlock_time, uint64_t fee, const std::vector<uint8_t>& extra)
{
  cryptonote::transaction tx;
  pending_tx ptx;
  transfer(dsts, fake_outputs_count, unlock_time, fee, extra, tx, ptx);
}

namespace {
// split_amounts(vector<cryptonote::tx_destination_entry> dsts, size_t num_splits)
//
// split amount for each dst in dsts into num_splits parts
// and make num_splits new vector<crypt...> instances to hold these new amounts
std::vector<std::vector<cryptonote::tx_destination_entry>> split_amounts(
    std::vector<cryptonote::tx_destination_entry> dsts, size_t num_splits)
{
  std::vector<std::vector<cryptonote::tx_destination_entry>> retVal;

  if (num_splits <= 1)
  {
    retVal.push_back(dsts);
    return retVal;
  }

  // for each split required
  for (size_t i=0; i < num_splits; i++)
  {
    std::vector<cryptonote::tx_destination_entry> new_dsts;

    // for each destination
    for (size_t j=0; j < dsts.size(); j++)
    {
      cryptonote::tx_destination_entry de;
      uint64_t amount;

      amount = dsts[j].amount;
      amount = amount / num_splits;

      // if last split, add remainder
      if (i + 1 == num_splits)
      {
        amount += dsts[j].amount % num_splits;
      }
      
      de.addr = dsts[j].addr;
      de.amount = amount;

      new_dsts.push_back(de);
    }

    retVal.push_back(new_dsts);
  }

  return retVal;
}
} // anonymous namespace

/**
 * @brief gets a monero address from the TXT record of a DNS entry
 *
 * gets the monero address from the TXT record of the DNS entry associated
 * with <url>.  If this lookup fails, or the TXT record does not contain an
 * XMR address in the correct format, returns an empty string.  <dnssec_valid>
 * will be set true or false according to whether or not the DNS query passes
 * DNSSEC validation.
 *
 * @param url the url to look up
 * @param dnssec_valid return-by-reference for DNSSEC status of query
 *
 * @return a monero address (as a string) or an empty string
 */
std::vector<std::string> wallet2::addresses_from_url(const std::string& url, bool& dnssec_valid)
{
  std::vector<std::string> addresses;
  // get txt records
  bool dnssec_available, dnssec_isvalid;
  std::string oa_addr = tools::DNSResolver::instance().get_dns_format_from_oa_address(url);
  auto records = tools::DNSResolver::instance().get_txt_record(oa_addr, dnssec_available, dnssec_isvalid);

  // TODO: update this to allow for conveying that dnssec was not available
  if (dnssec_available && dnssec_isvalid)
  {
    dnssec_valid = true;
  }
  else dnssec_valid = false;

  // for each txt record, try to find a monero address in it.
  for (auto& rec : records)
  {
    std::string addr = address_from_txt_record(rec);
    if (addr.size())
    {
      addresses.push_back(addr);
    }
  }

  return addresses;
}

//----------------------------------------------------------------------------------------------------
// TODO: parse the string in a less stupid way, probably with regex
std::string wallet2::address_from_txt_record(const std::string& s)
{
  // make sure the txt record has "oa1:xmr" and find it
  auto pos = s.find("oa1:xmr");

  // search from there to find "recipient_address="
  pos = s.find("recipient_address=", pos);

  pos += 18; // move past "recipient_address="

  // find the next semicolon
  auto pos2 = s.find(";", pos);
  if (pos2 != std::string::npos)
  {
    // length of address == 95, we can at least validate that much here
    if (pos2 - pos == 95)
    {
      return s.substr(pos, 95);
    }
  }
  return std::string();
}

crypto::hash wallet2::get_payment_id(const pending_tx &ptx) const
{
  std::vector<tx_extra_field> tx_extra_fields;
  if(!parse_tx_extra(ptx.tx.extra, tx_extra_fields))
    return cryptonote::null_hash;
  tx_extra_nonce extra_nonce;
  crypto::hash payment_id = null_hash;
  if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
  {
    crypto::hash8 payment_id8 = null_hash8;
    if(get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
    {
      if (decrypt_payment_id(payment_id8, ptx.dests[0].addr.m_view_public_key, ptx.tx_key))
      {
        memcpy(payment_id.data, payment_id8.data, 8);
      }
    }
    else if (!get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
    {
      payment_id = cryptonote::null_hash;
    }
  }
  return payment_id;
}
//----------------------------------------------------------------------------------------------------
// take a pending tx and actually send it to the daemon
void wallet2::commit_tx(pending_tx& ptx)
{
  using namespace cryptonote;
  crypto::hash txid;

  COMMAND_RPC_SEND_RAW_TX::request req;
  req.tx_as_hex = epee::string_tools::buff_to_hex_nodelimer(tx_to_blob(ptx.tx));
  COMMAND_RPC_SEND_RAW_TX::response daemon_send_resp;
  m_daemon_rpc_mutex.lock();
  bool r = epee::net_utils::invoke_http_json_remote_command2(m_daemon_address + "/sendrawtransaction", req, daemon_send_resp, m_http_client, 200000);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "sendrawtransaction");
  THROW_WALLET_EXCEPTION_IF(daemon_send_resp.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "sendrawtransaction");
  THROW_WALLET_EXCEPTION_IF(daemon_send_resp.status != CORE_RPC_STATUS_OK, error::tx_rejected, ptx.tx, daemon_send_resp.status);

  txid = get_transaction_hash(ptx.tx);
  crypto::hash payment_id = cryptonote::null_hash;
  std::vector<cryptonote::tx_destination_entry> dests;
  if (store_tx_info())
  {
    payment_id = get_payment_id(ptx);
    dests = ptx.dests;
  }
  add_unconfirmed_tx(ptx.tx, dests, payment_id, ptx.change_dts.amount);
  if (store_tx_info())
    m_tx_keys.insert(std::make_pair(txid, ptx.tx_key));

  LOG_PRINT_L2("transaction " << txid << " generated ok and sent to daemon, key_images: [" << ptx.key_images << "]");

  BOOST_FOREACH(transfer_container::iterator it, ptx.selected_transfers)
    it->m_spent = true;

  LOG_PRINT_L0("Transaction successfully sent. <" << txid << ">" << ENDL
            << "Commission: " << print_money(ptx.fee+ptx.dust) << " (dust: " << print_money(ptx.dust) << ")" << ENDL
            << "Balance: " << print_money(balance()) << ENDL
            << "Unlocked: " << print_money(unlocked_balance()) << ENDL
            << "Please, wait for confirmation for your balance to be unlocked.");
}

void wallet2::commit_tx(std::vector<pending_tx>& ptx_vector)
{
  for (auto & ptx : ptx_vector)
  {
    commit_tx(ptx);
  }
}

//----------------------------------------------------------------------------------------------------
// separated the call(s) to wallet2::transfer into their own function
//
// this function will make multiple calls to wallet2::transfer if multiple
// transactions will be required
std::vector<wallet2::pending_tx> wallet2::create_transactions(std::vector<cryptonote::tx_destination_entry> dsts, const size_t fake_outs_count, const uint64_t unlock_time, const uint64_t fee_UNUSED, const std::vector<uint8_t> extra)
{

  // failsafe split attempt counter
  size_t attempt_count = 0;

  for(attempt_count = 1; ;attempt_count++)
  {
    size_t num_tx = 0.5 + pow(1.7,attempt_count-1);

    auto split_values = split_amounts(dsts, num_tx);

    // Throw if split_amounts comes back with a vector of size different than it should
    if (split_values.size() != num_tx)
    {
      throw std::runtime_error("Splitting transactions returned a number of potential tx not equal to what was requested");
    }

    std::vector<pending_tx> ptx_vector;
    try
    {
      // for each new destination vector (i.e. for each new tx)
      for (auto & dst_vector : split_values)
      {
        cryptonote::transaction tx;
        pending_tx ptx;

	// loop until fee is met without increasing tx size to next KB boundary.
	uint64_t needed_fee = 0;
	do
	{
	  transfer(dst_vector, fake_outs_count, unlock_time, needed_fee, extra, tx, ptx);
	  auto txBlob = t_serializable_object_to_blob(ptx.tx);
	  uint64_t txSize = txBlob.size();
	  uint64_t numKB = txSize / 1024;
	  if (txSize % 1024)
	  {
	    numKB++;
	  }
	  needed_fee = numKB * FEE_PER_KB;
	} while (ptx.fee < needed_fee);

        ptx_vector.push_back(ptx);

        // mark transfers to be used as "spent"
        BOOST_FOREACH(transfer_container::iterator it, ptx.selected_transfers)
          it->m_spent = true;
      }

      // if we made it this far, we've selected our transactions.  committing them will mark them spent,
      // so this is a failsafe in case they don't go through
      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        BOOST_FOREACH(transfer_container::iterator it2, ptx.selected_transfers)
          it2->m_spent = false;

      }

      // if we made it this far, we're OK to actually send the transactions
      return ptx_vector;

    }
    // only catch this here, other exceptions need to pass through to the calling function
    catch (const tools::error::tx_too_big& e)
    {

      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        BOOST_FOREACH(transfer_container::iterator it2, ptx.selected_transfers)
          it2->m_spent = false;

      }

      if (attempt_count >= MAX_SPLIT_ATTEMPTS)
      {
        throw;
      }
    }
    catch (...)
    {
      // in case of some other exception, make sure any tx in queue are marked unspent again

      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        BOOST_FOREACH(transfer_container::iterator it2, ptx.selected_transfers)
          it2->m_spent = false;

      }

      throw;
    }
  }
}

template<typename T>
void wallet2::transfer_selected(const std::vector<cryptonote::tx_destination_entry>& dsts, const std::list<transfer_container::iterator> selected_transfers, size_t fake_outputs_count,
  uint64_t unlock_time, uint64_t fee, const std::vector<uint8_t>& extra, T destination_split_strategy, const tx_dust_policy& dust_policy, cryptonote::transaction& tx, pending_tx &ptx)
{
  using namespace cryptonote;
  // throw if attempting a transaction with no destinations
  THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);

  uint64_t needed_money = fee;
  LOG_PRINT_L2("transfer: starting with fee " << print_money (needed_money));

  // calculate total amount being sent to all destinations
  // throw if total amount overflows uint64_t
  BOOST_FOREACH(auto& dt, dsts)
  {
    THROW_WALLET_EXCEPTION_IF(0 == dt.amount, error::zero_destination);
    needed_money += dt.amount;
    LOG_PRINT_L2("transfer: adding " << print_money(dt.amount) << ", for a total of " << print_money (needed_money));
    THROW_WALLET_EXCEPTION_IF(needed_money < dt.amount, error::tx_sum_overflow, dsts, fee, m_testnet);
  }

  uint64_t found_money = 0;
  BOOST_FOREACH(auto it, selected_transfers)
  {
    found_money += it->amount();
  }

  LOG_PRINT_L2("wanted " << print_money(needed_money) << ", found " << print_money(found_money) << ", fee " << print_money(fee));
  THROW_WALLET_EXCEPTION_IF(found_money < needed_money, error::not_enough_money, found_money, needed_money - fee, fee);

  typedef COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry out_entry;
  typedef cryptonote::tx_source_entry::output_entry tx_output_entry;

  COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response daemon_resp = AUTO_VAL_INIT(daemon_resp);
  if(fake_outputs_count)
  {
    COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request req = AUTO_VAL_INIT(req);
    req.outs_count = fake_outputs_count + 1;// add one to make possible (if need) to skip real output key
    BOOST_FOREACH(transfer_container::iterator it, selected_transfers)
    {
      THROW_WALLET_EXCEPTION_IF(it->m_tx.vout.size() <= it->m_internal_output_index, error::wallet_internal_error,
        "m_internal_output_index = " + std::to_string(it->m_internal_output_index) +
        " is greater or equal to outputs count = " + std::to_string(it->m_tx.vout.size()));
      req.amounts.push_back(it->amount());
    }

    m_daemon_rpc_mutex.lock();
    bool r = epee::net_utils::invoke_http_bin_remote_command2(m_daemon_address + "/getrandom_outs.bin", req, daemon_resp, m_http_client, 200000);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "getrandom_outs.bin");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "getrandom_outs.bin");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status != CORE_RPC_STATUS_OK, error::get_random_outs_error, daemon_resp.status);
    THROW_WALLET_EXCEPTION_IF(daemon_resp.outs.size() != selected_transfers.size(), error::wallet_internal_error,
      "daemon returned wrong response for getrandom_outs.bin, wrong amounts count = " +
      std::to_string(daemon_resp.outs.size()) + ", expected " +  std::to_string(selected_transfers.size()));

    std::vector<COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> scanty_outs;
    BOOST_FOREACH(COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount& amount_outs, daemon_resp.outs)
    {
      if (amount_outs.outs.size() < fake_outputs_count)
      {
        scanty_outs.push_back(amount_outs);
      }
    }
    THROW_WALLET_EXCEPTION_IF(!scanty_outs.empty(), error::not_enough_outs_to_mix, scanty_outs, fake_outputs_count);
  }

  //prepare inputs
  size_t i = 0;
  std::vector<cryptonote::tx_source_entry> sources;
  BOOST_FOREACH(transfer_container::iterator it, selected_transfers)
  {
    sources.resize(sources.size()+1);
    cryptonote::tx_source_entry& src = sources.back();
    transfer_details& td = *it;
    src.amount = td.amount();
    //paste mixin transaction
    if(daemon_resp.outs.size())
    {
      daemon_resp.outs[i].outs.sort([](const out_entry& a, const out_entry& b){return a.global_amount_index < b.global_amount_index;});
      BOOST_FOREACH(out_entry& daemon_oe, daemon_resp.outs[i].outs)
      {
        if(td.m_global_output_index == daemon_oe.global_amount_index)
          continue;
        tx_output_entry oe;
        oe.first = daemon_oe.global_amount_index;
        oe.second = daemon_oe.out_key;
        src.outputs.push_back(oe);
        if(src.outputs.size() >= fake_outputs_count)
          break;
      }
    }

    //paste real transaction to the random index
    auto it_to_insert = std::find_if(src.outputs.begin(), src.outputs.end(), [&](const tx_output_entry& a)
    {
      return a.first >= td.m_global_output_index;
    });
    //size_t real_index = src.outputs.size() ? (rand() % src.outputs.size() ):0;
    tx_output_entry real_oe;
    real_oe.first = td.m_global_output_index;
    real_oe.second = boost::get<txout_to_key>(td.m_tx.vout[td.m_internal_output_index].target).key;
    auto interted_it = src.outputs.insert(it_to_insert, real_oe);
    src.real_out_tx_key = get_tx_pub_key_from_extra(td.m_tx);
    src.real_output = interted_it - src.outputs.begin();
    src.real_output_in_tx_index = td.m_internal_output_index;
    detail::print_source_entry(src);
    ++i;
  }

  cryptonote::tx_destination_entry change_dts = AUTO_VAL_INIT(change_dts);
  if (needed_money < found_money)
  {
    change_dts.addr = m_account.get_keys().m_account_address;
    change_dts.amount = found_money - needed_money;
  }

  std::vector<cryptonote::tx_destination_entry> splitted_dsts, dust_dsts;
  uint64_t dust = 0;
  destination_split_strategy(dsts, change_dts, dust_policy.dust_threshold, splitted_dsts, dust_dsts);
  BOOST_FOREACH(auto& d, dust_dsts) {
    THROW_WALLET_EXCEPTION_IF(dust_policy.dust_threshold < d.amount, error::wallet_internal_error, "invalid dust value: dust = " +
      std::to_string(d.amount) + ", dust_threshold = " + std::to_string(dust_policy.dust_threshold));
  }
  BOOST_FOREACH(auto& d, dust_dsts) {
    if (!dust_policy.add_to_fee)
      splitted_dsts.push_back(cryptonote::tx_destination_entry(d.amount, dust_policy.addr_for_dust));
    dust += d.amount;
  }

  crypto::secret_key tx_key;
  bool r = cryptonote::construct_tx_and_get_tx_key(m_account.get_keys(), sources, splitted_dsts, extra, tx, unlock_time, tx_key);
  THROW_WALLET_EXCEPTION_IF(!r, error::tx_not_constructed, sources, splitted_dsts, unlock_time, m_testnet);
  THROW_WALLET_EXCEPTION_IF(m_upper_transaction_size_limit <= get_object_blobsize(tx), error::tx_too_big, tx, m_upper_transaction_size_limit);

  std::string key_images;
  bool all_are_txin_to_key = std::all_of(tx.vin.begin(), tx.vin.end(), [&](const txin_v& s_e) -> bool
  {
    CHECKED_GET_SPECIFIC_VARIANT(s_e, const txin_to_key, in, false);
    key_images += boost::to_string(in.k_image) + " ";
    return true;
  });
  THROW_WALLET_EXCEPTION_IF(!all_are_txin_to_key, error::unexpected_txin_type, tx);

  ptx.key_images = key_images;
  ptx.fee = fee;
  ptx.dust = dust;
  ptx.tx = tx;
  ptx.change_dts = change_dts;
  ptx.selected_transfers = selected_transfers;
  ptx.tx_key = tx_key;
  ptx.dests = dsts;
}

// Another implementation of transaction creation that is hopefully better
// While there is anything left to pay, it goes through random outputs and tries
// to fill the next destination/amount. If it fully fills it, it will use the
// remainder to try to fill the next one as well.
// The tx size if roughly estimated as a linear function of only inputs, and a
// new tx will be created when that size goes above a given fraction of the
// max tx size. At that point, more outputs may be added if the fee cannot be
// satisfied.
// If the next output in the next tx would go to the same destination (ie, we
// cut off at a tx boundary in the middle of paying a given destination), the
// fee will be carved out of the current input if possible, to avoid having to
// add another output just for the fee and getting change.
// This system allows for sending (almost) the entire balance, since it does
// not generate spurious change in all txes, thus decreasing the instantaneous
// usable balance.
std::vector<wallet2::pending_tx> wallet2::create_transactions_2(std::vector<cryptonote::tx_destination_entry> dsts, const size_t fake_outs_count, const uint64_t unlock_time, const uint64_t fee_UNUSED, const std::vector<uint8_t> extra)
{
  std::vector<size_t> unused_transfers_indices;
  std::vector<size_t> unused_dust_indices;
  uint64_t needed_money;
  uint64_t accumulated_fee, accumulated_outputs, accumulated_change;
  struct TX {
    std::list<transfer_container::iterator> selected_transfers;
    std::vector<cryptonote::tx_destination_entry> dsts;
    cryptonote::transaction tx;
    pending_tx ptx;
    size_t bytes;

    void add(const account_public_address &addr, uint64_t amount) {
      std::vector<cryptonote::tx_destination_entry>::iterator i;
      i = std::find_if(dsts.begin(), dsts.end(), [&](const cryptonote::tx_destination_entry &d) { return !memcmp (&d.addr, &addr, sizeof(addr)); });
      if (i == dsts.end())
        dsts.push_back(tx_destination_entry(amount,addr));
      else
        i->amount += amount;
    }
  };
  std::vector<TX> txes;
  bool adding_fee; // true if new outputs go towards fee, rather than destinations
  uint64_t needed_fee, available_for_fee = 0;

  // throw if attempting a transaction with no destinations
  THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);

  // calculate total amount being sent to all destinations
  // throw if total amount overflows uint64_t
  needed_money = 0;
  BOOST_FOREACH(auto& dt, dsts)
  {
    THROW_WALLET_EXCEPTION_IF(0 == dt.amount, error::zero_destination);
    needed_money += dt.amount;
    LOG_PRINT_L2("transfer: adding " << print_money(dt.amount) << ", for a total of " << print_money (needed_money));
    THROW_WALLET_EXCEPTION_IF(needed_money < dt.amount, error::tx_sum_overflow, dsts, 0, m_testnet);
  }

  // throw if attempting a transaction with no money
  THROW_WALLET_EXCEPTION_IF(needed_money == 0, error::zero_destination);

  // gather all our dust and non dust outputs
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    const transfer_details& td = m_transfers[i];
    if (!td.m_spent && is_transfer_unlocked(td))
    {
      if (is_valid_decomposed_amount(td.amount()))
        unused_transfers_indices.push_back(i);
      else
        unused_dust_indices.push_back(i);
    }
  }
  LOG_PRINT_L2("Starting with " << unused_transfers_indices.size() << " non-dust outputs and " << unused_dust_indices.size() << " dust outputs");

  // start with an empty tx
  txes.push_back(TX());
  accumulated_fee = 0;
  accumulated_outputs = 0;
  accumulated_change = 0;
  adding_fee = false;
  needed_fee = 0;

  // while we have something to send
  while ((!dsts.empty() && dsts[0].amount > 0) || adding_fee) {
    TX &tx = txes.back();

    // if we need to spend money and don't have any left, we fail
    if (unused_dust_indices.empty() && unused_transfers_indices.empty()) {
      LOG_PRINT_L2("No more outputs to choose from");
      THROW_WALLET_EXCEPTION_IF(1, error::not_enough_money, unlocked_balance(), needed_money, accumulated_fee + needed_fee);
    }

    // get a random unspent output and use it to pay part (or all) of the current destination (and maybe next one, etc)
    // This could be more clever, but maybe at the cost of making probabilistic inferences easier
    size_t idx = !unused_transfers_indices.empty() ? pop_random_value(unused_transfers_indices) : pop_random_value(unused_dust_indices);

    const transfer_details &td = m_transfers[idx];
    LOG_PRINT_L2("Picking output " << idx << ", amount " << print_money(td.amount()));

    // add this output to the list to spend
    tx.selected_transfers.push_back(m_transfers.begin() + idx);
    uint64_t available_amount = td.amount();
    accumulated_outputs += available_amount;

    if (adding_fee)
    {
      LOG_PRINT_L2("We need more fee, adding it to fee");
      available_for_fee += available_amount;
    }
    else
    {
      while (!dsts.empty() && dsts[0].amount <= available_amount)
      {
        // we can fully pay that destination
        LOG_PRINT_L2("We can fully pay " << get_account_address_as_str(m_testnet, dsts[0].addr) <<
          " for " << print_money(dsts[0].amount));
        tx.add(dsts[0].addr, dsts[0].amount);
        available_amount -= dsts[0].amount;
        dsts[0].amount = 0;
        pop_index(dsts, 0);
      }

      if (available_amount > 0 && !dsts.empty()) {
        // we can partially fill that destination
        LOG_PRINT_L2("We can partially pay " << get_account_address_as_str(m_testnet, dsts[0].addr) <<
          " for " << print_money(available_amount) << "/" << print_money(dsts[0].amount));
        tx.add(dsts[0].addr, available_amount);
        dsts[0].amount -= available_amount;
        available_amount = 0;
      }
    }

    // here, check if we need to sent tx and start a new one
    LOG_PRINT_L2("Considering whether to create a tx now, " << tx.selected_transfers.size() << " inputs, tx limit "
      << m_upper_transaction_size_limit);
    bool try_tx;
    if (adding_fee)
    {
      /* might not actually be enough if adding this output bumps size to next kB, but we need to try */
      try_tx = available_for_fee >= needed_fee;
    }
    else
    {
      try_tx = dsts.empty() || (tx.selected_transfers.size() * (fake_outs_count+1) * APPROXIMATE_INPUT_BYTES >= TX_SIZE_TARGET(m_upper_transaction_size_limit));
    }

    if (try_tx) {
      cryptonote::transaction test_tx;
      pending_tx test_ptx;

      needed_fee = 0;

      LOG_PRINT_L2("Trying to create a tx now, with " << tx.dsts.size() << " destinations and " <<
        tx.selected_transfers.size() << " outputs");
      transfer_selected(tx.dsts, tx.selected_transfers, fake_outs_count, unlock_time, needed_fee, extra,
        detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD), test_tx, test_ptx);
      auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
      uint64_t txSize = txBlob.size();
      uint64_t numKB = txSize / 1024;
      if (txSize % 1024)
      {
        numKB++;
      }
      needed_fee = numKB * FEE_PER_KB;
      available_for_fee = test_ptx.fee + test_ptx.change_dts.amount;
      LOG_PRINT_L2("Made a " << numKB << " kB tx, with " << print_money(available_for_fee) << " available for fee (" <<
        print_money(needed_fee) << " needed)");

      if (needed_fee > available_for_fee && dsts[0].amount > 0)
      {
        // we don't have enough for the fee, but we've only partially paid the current address,
        // so we can take the fee from the paid amount, since we'll have to make another tx anyway
        std::vector<cryptonote::tx_destination_entry>::iterator i;
        i = std::find_if(tx.dsts.begin(), tx.dsts.end(),
          [&](const cryptonote::tx_destination_entry &d) { return !memcmp (&d.addr, &dsts[0].addr, sizeof(dsts[0].addr)); });
        THROW_WALLET_EXCEPTION_IF(i == tx.dsts.end(), error::wallet_internal_error, "paid address not found in outputs");
        if (i->amount > needed_fee)
        {
          uint64_t new_paid_amount = i->amount /*+ test_ptx.fee*/ - needed_fee;
          LOG_PRINT_L2("Adjusting amount paid to " << get_account_address_as_str(m_testnet, i->addr) << " from " <<
            print_money(i->amount) << " to " << print_money(new_paid_amount) << " to accomodate " <<
            print_money(needed_fee) << " fee");
          dsts[0].amount += i->amount - new_paid_amount;
          i->amount = new_paid_amount;
          test_ptx.fee = needed_fee;
          available_for_fee = needed_fee;
        }
      }

      if (needed_fee > available_for_fee)
      {
        LOG_PRINT_L2("We could not make a tx, switching to fee accumulation");

        adding_fee = true;
      }
      else
      {
        LOG_PRINT_L2("We made a tx, adjusting fee and saving it");
        transfer_selected(tx.dsts, tx.selected_transfers, fake_outs_count, unlock_time, needed_fee, extra,
          detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD), test_tx, test_ptx);
        txBlob = t_serializable_object_to_blob(test_ptx.tx);
      LOG_PRINT_L2("Made a final " << ((txBlob.size() + 1023)/1024) << " kB tx, with " << print_money(test_ptx.fee) <<
        " fee  and " << print_money(test_ptx.change_dts.amount) << " change");

        tx.tx = test_tx;
        tx.ptx = test_ptx;
        tx.bytes = txBlob.size();
        accumulated_fee += test_ptx.fee;
        accumulated_change += test_ptx.change_dts.amount;
        adding_fee = false;
        if (!dsts.empty())
        {
          LOG_PRINT_L2("We have more to pay, starting another tx");
          txes.push_back(TX());
        }
      }
    }
  }

  if (adding_fee)
  {
    LOG_PRINT_L1("We ran out of outputs while trying to gather final fee");
    THROW_WALLET_EXCEPTION_IF(1, error::not_enough_money, unlocked_balance(), needed_money, accumulated_fee + needed_fee);
  }

  LOG_PRINT_L1("Done creating " << txes.size() << " transactions, " << print_money(accumulated_fee) <<
    " total fee, " << print_money(accumulated_change) << " total change");

  std::vector<wallet2::pending_tx> ptx_vector;
  for (std::vector<TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TX &tx = *i;
    uint64_t tx_money = 0;
    for (std::list<transfer_container::iterator>::const_iterator mi = tx.selected_transfers.begin(); mi != tx.selected_transfers.end(); ++mi)
      tx_money += (*mi)->amount();
    LOG_PRINT_L1("  Transaction " << (1+std::distance(txes.begin(), i)) << "/" << txes.size() <<
      ": " << (tx.bytes+1023)/1024 << " kB, sending " << print_money(tx_money) << " in " << tx.selected_transfers.size() <<
      " outputs to " << tx.dsts.size() << " destination(s), including " <<
      print_money(tx.ptx.fee) << " fee, " << print_money(tx.ptx.change_dts.amount) << " change");
    ptx_vector.push_back(tx.ptx);
  }

  // if we made it this far, we're OK to actually send the transactions
  return ptx_vector;
}

uint64_t wallet2::unlocked_dust_balance(const tx_dust_policy &dust_policy) const
{
  uint64_t money = 0;
  std::list<transfer_container::iterator> selected_transfers;
  for (transfer_container::const_iterator i = m_transfers.begin(); i != m_transfers.end(); ++i)
  {
    const transfer_details& td = *i;
    if (!td.m_spent && td.amount() < dust_policy.dust_threshold && is_transfer_unlocked(td))
    {
      money += td.amount();
    }
  }
  return money;
}

template<typename T>
void wallet2::transfer_dust(size_t num_outputs, uint64_t unlock_time, uint64_t needed_fee, T destination_split_strategy, const tx_dust_policy& dust_policy, const std::vector<uint8_t> &extra, cryptonote::transaction& tx, pending_tx &ptx)
{
  using namespace cryptonote;

  // select all dust inputs for transaction
  // throw if there are none
  uint64_t money = 0;
  std::list<transfer_container::iterator> selected_transfers;
  for (transfer_container::iterator i = m_transfers.begin(); i != m_transfers.end(); ++i)
  {
    const transfer_details& td = *i;
    if (!td.m_spent && (td.amount() < dust_policy.dust_threshold || !is_valid_decomposed_amount(td.amount())) && is_transfer_unlocked(td))
    {
      selected_transfers.push_back (i);
      money += td.amount();
      if (selected_transfers.size() >= num_outputs)
        break;
    }
  }

  // we don't allow no output to self, easier, but one may want to burn the dust if = fee
  THROW_WALLET_EXCEPTION_IF(money <= needed_fee, error::not_enough_money, money, needed_fee, needed_fee);

  typedef cryptonote::tx_source_entry::output_entry tx_output_entry;

  //prepare inputs
  size_t i = 0;
  std::vector<cryptonote::tx_source_entry> sources;
  BOOST_FOREACH(transfer_container::iterator it, selected_transfers)
  {
    sources.resize(sources.size()+1);
    cryptonote::tx_source_entry& src = sources.back();
    transfer_details& td = *it;
    src.amount = td.amount();

    //paste real transaction to the random index
    auto it_to_insert = std::find_if(src.outputs.begin(), src.outputs.end(), [&](const tx_output_entry& a)
    {
      return a.first >= td.m_global_output_index;
    });
    tx_output_entry real_oe;
    real_oe.first = td.m_global_output_index;
    real_oe.second = boost::get<txout_to_key>(td.m_tx.vout[td.m_internal_output_index].target).key;
    auto interted_it = src.outputs.insert(it_to_insert, real_oe);
    src.real_out_tx_key = get_tx_pub_key_from_extra(td.m_tx);
    src.real_output = interted_it - src.outputs.begin();
    src.real_output_in_tx_index = td.m_internal_output_index;
    detail::print_source_entry(src);
    ++i;
  }

  cryptonote::tx_destination_entry change_dts = AUTO_VAL_INIT(change_dts);

  std::vector<cryptonote::tx_destination_entry> dsts;
  uint64_t money_back = money - needed_fee;
  if (dust_policy.dust_threshold > 0)
    money_back = money_back - money_back % dust_policy.dust_threshold;
  dsts.push_back(cryptonote::tx_destination_entry(money_back, m_account_public_address));
  std::vector<cryptonote::tx_destination_entry> splitted_dsts, dust;
  destination_split_strategy(dsts, change_dts, dust_policy.dust_threshold, splitted_dsts, dust);
  BOOST_FOREACH(auto& d, dust) {
    THROW_WALLET_EXCEPTION_IF(dust_policy.dust_threshold < d.amount, error::wallet_internal_error, "invalid dust value: dust = " +
      std::to_string(d.amount) + ", dust_threshold = " + std::to_string(dust_policy.dust_threshold));
  }

  crypto::secret_key tx_key;
  bool r = cryptonote::construct_tx_and_get_tx_key(m_account.get_keys(), sources, splitted_dsts, extra, tx, unlock_time, tx_key);
  THROW_WALLET_EXCEPTION_IF(!r, error::tx_not_constructed, sources, splitted_dsts, unlock_time, m_testnet);
  THROW_WALLET_EXCEPTION_IF(m_upper_transaction_size_limit <= get_object_blobsize(tx), error::tx_too_big, tx, m_upper_transaction_size_limit);

  std::string key_images;
  bool all_are_txin_to_key = std::all_of(tx.vin.begin(), tx.vin.end(), [&](const txin_v& s_e) -> bool
  {
    CHECKED_GET_SPECIFIC_VARIANT(s_e, const txin_to_key, in, false);
    key_images += boost::to_string(in.k_image) + " ";
    return true;
  });
  THROW_WALLET_EXCEPTION_IF(!all_are_txin_to_key, error::unexpected_txin_type, tx);

  ptx.key_images = key_images;
  ptx.fee = money - money_back;
  ptx.dust = 0;
  ptx.tx = tx;
  ptx.change_dts = change_dts;
  ptx.selected_transfers = selected_transfers;
  ptx.tx_key = tx_key;
  ptx.dests = dsts;
}

//----------------------------------------------------------------------------------------------------
std::vector<wallet2::pending_tx> wallet2::create_dust_sweep_transactions()
{
  tx_dust_policy dust_policy(::config::DEFAULT_DUST_THRESHOLD);

  size_t num_dust_outputs = 0;
  for (transfer_container::const_iterator i = m_transfers.begin(); i != m_transfers.end(); ++i)
  {
    const transfer_details& td = *i;
    if (!td.m_spent && (td.amount() < dust_policy.dust_threshold || !is_valid_decomposed_amount(td.amount())) && is_transfer_unlocked(td))
    {
      num_dust_outputs++;
    }
  }

  // failsafe split attempt counter
  size_t attempt_count = 0;

  for(attempt_count = 1; ;attempt_count++)
  {
    size_t num_tx = 0.5 + pow(1.7,attempt_count-1);
    size_t num_outputs_per_tx = (num_dust_outputs + num_tx - 1) / num_tx;

    std::vector<pending_tx> ptx_vector;
    try
    {
      // for each new tx
      for (size_t i=0; i<num_tx;++i)
      {
        cryptonote::transaction tx;
        pending_tx ptx;
        std::vector<uint8_t> extra;

	// loop until fee is met without increasing tx size to next KB boundary.
	uint64_t needed_fee = 0;
	if (1)
	{
	  transfer_dust(num_outputs_per_tx, (uint64_t)0 /* unlock_time */, 0, detail::digit_split_strategy, dust_policy, extra, tx, ptx);
	  auto txBlob = t_serializable_object_to_blob(ptx.tx);
	  uint64_t txSize = txBlob.size();
	  uint64_t numKB = txSize / 1024;
	  if (txSize % 1024)
	  {
	    numKB++;
	  }
	  needed_fee = numKB * FEE_PER_KB;

          // reroll the tx with the actual amount minus the fee
          // if there's not enough for the fee, it'll throw
	  transfer_dust(num_outputs_per_tx, (uint64_t)0 /* unlock_time */, needed_fee, detail::digit_split_strategy, dust_policy, extra, tx, ptx);
	  txBlob = t_serializable_object_to_blob(ptx.tx);
	}

        ptx_vector.push_back(ptx);

        // mark transfers to be used as "spent"
        BOOST_FOREACH(transfer_container::iterator it, ptx.selected_transfers)
          it->m_spent = true;
      }

      // if we made it this far, we've selected our transactions.  committing them will mark them spent,
      // so this is a failsafe in case they don't go through
      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        BOOST_FOREACH(transfer_container::iterator it2, ptx.selected_transfers)
          it2->m_spent = false;

      }

      // if we made it this far, we're OK to actually send the transactions
      return ptx_vector;

    }
    // only catch this here, other exceptions need to pass through to the calling function
    catch (const tools::error::tx_too_big& e)
    {

      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        BOOST_FOREACH(transfer_container::iterator it2, ptx.selected_transfers)
          it2->m_spent = false;

      }

      if (attempt_count >= MAX_SPLIT_ATTEMPTS)
      {
        throw;
      }
    }
    catch (...)
    {
      // in case of some other exception, make sure any tx in queue are marked unspent again

      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        BOOST_FOREACH(transfer_container::iterator it2, ptx.selected_transfers)
          it2->m_spent = false;

      }

      throw;
    }
  }
}

bool wallet2::get_tx_key(const crypto::hash &txid, crypto::secret_key &tx_key) const
{
  const std::unordered_map<crypto::hash, crypto::secret_key>::const_iterator i = m_tx_keys.find(txid);
  if (i == m_tx_keys.end())
    return false;
  tx_key = i->second;
  return true;
}

//----------------------------------------------------------------------------------------------------
void wallet2::generate_genesis(cryptonote::block& b) {
  if (m_testnet)
  {
    cryptonote::generate_genesis_block(b, config::testnet::GENESIS_TX, config::testnet::GENESIS_NONCE);
  }
  else
  {
    cryptonote::generate_genesis_block(b, config::GENESIS_TX, config::GENESIS_NONCE);
  }
}
}
