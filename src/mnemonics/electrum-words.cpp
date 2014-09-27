// Copyright (c) 2014, The Monero Project
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

/*!
 * \file electrum-words.cpp
 * 
 * \brief Mnemonic seed generation and wallet restoration from them.
 * 
 * This file and its header file are for translating Electrum-style word lists
 * into their equivalent byte representations for cross-compatibility with
 * that method of "backing up" one's wallet keys.
 */

#include <string>
#include <cassert>
#include <map>
#include <cstdint>
#include <vector>
#include <boost/algorithm/string.hpp>
#include "crypto/crypto.h"  // for declaration of crypto::secret_key
#include <fstream>
#include "mnemonics/electrum-words.h"
#include <stdexcept>
#include <boost/filesystem.hpp>

namespace
{
  int num_words = 0; 
  std::map<std::string,uint32_t> words_map;
  std::vector<std::string> words_array;

  bool is_old_style_mnemonics = false;

  const std::string WORD_LISTS_DIRECTORY = "wordlists";
  const std::string LANGUAGES_DIRECTORY = "languages";
  const std::string OLD_WORD_FILE = "old-word-list";

  /*!
   * Tells if the module hasn't been initialized with a word list file.
   * \return Whether the module hasn't been initialized with a word list file.
   */
  bool is_uninitialized()
  {
    return num_words == 0 ? true : false;
  }

  /*!
   * Create word list map and array data structres for use during inter-conversion between
   * words and secret key.
   * \param word_file Path to the word list file from pwd.
   */
  void create_data_structures(const std::string &word_file)
  {
    words_array.clear();
    words_map.clear();
    num_words = 0;
    std::ifstream input_stream;
    input_stream.open(word_file.c_str(), std::ifstream::in);

    if (!input_stream)
      throw std::runtime_error(std::string("Word list file couldn't be opened."));

    std::string word;
    while (input_stream >> word)
    {
      words_array.push_back(word);
      words_map[word] = num_words;
      num_words++;
    }
    input_stream.close();
  }

  /*!
   * Tells if all the words passed in wlist was present in current word list file.
   * \param  wlist List of words to match.
   * \return       Whether they were all present or not.
   */
  bool word_list_file_match(const std::vector<std::string> &wlist)
  {
    for (std::vector<std::string>::const_iterator it = wlist.begin(); it != wlist.end(); it++)
    {
      if (words_map.count(*it) == 0)
      {
        return false;
      }
    }
    return true;
  }
}

/*!
 * \namespace crypto
 */
namespace crypto
{
  /*!
   * \namespace ElectrumWords
   * 
   * \brief Mnemonic seed word generation and wallet restoration.
   */
  namespace ElectrumWords
  {
    /*!
     * \brief Called to initialize it work with a word list file.
     * \param language      Language of the word list file.
     * \param old_word_list Whether it is to use the old style word list file.
     */
    void init(const std::string &language, bool old_word_list)
    {
      if (old_word_list)
      {
        // Use the old word list file if told to.
        create_data_structures(WORD_LISTS_DIRECTORY + '/' + OLD_WORD_FILE);
        is_old_style_mnemonics = true;
      }
      else
      {
        create_data_structures(WORD_LISTS_DIRECTORY + '/' + LANGUAGES_DIRECTORY + '/' + language);
        is_old_style_mnemonics = false;
      }
      if (num_words == 0)
      {
        throw std::runtime_error(std::string("Word list file is empty: ") +
          (old_word_list ? OLD_WORD_FILE : (LANGUAGES_DIRECTORY + '/' + language)));
      }
    }

    /*!
     * \brief If the module is currenly using an old style word list.
     * \return Whether it is currenly using an old style word list.
     */
    bool get_is_old_style_mnemonics()
    {
      if (is_uninitialized())
      {
        throw std::runtime_error("ElectrumWords hasn't been initialized with a word list yet.");
      }
      return is_old_style_mnemonics;
    }

    /*!
     * \brief Converts seed words to bytes (secret key).
     * \param  words String containing the words separated by spaces.
     * \param  dst   To put the secret key restored from the words.
     * \return       false if not a multiple of 3 words, or if word is not in the words list
     */
    bool words_to_bytes(const std::string& words, crypto::secret_key& dst)
    {
      std::vector<std::string> wlist;

      boost::split(wlist, words, boost::is_any_of(" "));

      std::vector<std::string> languages;
      get_language_list(languages);

      // Try to find a word list file that contains all the words in the word list.
      std::vector<std::string>::iterator it;
      for (it = languages.begin(); it != languages.end(); it++)
      {
        init(*it);
        if (word_list_file_match(wlist))
        {
          break;
        }
      }
      // If no such file was found, see if the old style word list file has them all.
      if (it == languages.end())
      {
        init("", true);
        if (!word_list_file_match(wlist))
        {
          return false;
        }
      }
      int n = num_words;

      // error on non-compliant word list
      if (wlist.size() != 12 && wlist.size() != 24) return false;

      for (unsigned int i=0; i < wlist.size() / 3; i++)
      {
        uint32_t val;
        uint32_t w1, w2, w3;

        w1 = words_map.at(wlist[i*3]);
        w2 = words_map.at(wlist[i*3 + 1]);
        w3 = words_map.at(wlist[i*3 + 2]);

        val = w1 + n * (((n - w1) + w2) % n) + n * n * (((n - w2) + w3) % n);

        if (!(val % n == w1)) return false;

        memcpy(dst.data + i * 4, &val, 4);  // copy 4 bytes to position
      }

      std::string wlist_copy = words;
      if (wlist.size() == 12)
      {
        memcpy(dst.data, dst.data + 16, 16);  // if electrum 12-word seed, duplicate
        wlist_copy += ' ';
        wlist_copy += words;
      }

      return true;
    }

    /*!
     * \brief Converts bytes (secret key) to seed words.
     * \param  src   Secret key
     * \param  words Space separated words get copied here.
     * \return       Whether it was successful or not. Unsuccessful if wrong key size.
     */
    bool bytes_to_words(const crypto::secret_key& src, std::string& words)
    {
      if (is_uninitialized())
      {
        init("", true);
      }
      int n = num_words;

      if (sizeof(src.data) % 4 != 0) return false;

      // 8 bytes -> 3 words.  8 digits base 16 -> 3 digits base 1626
      for (unsigned int i=0; i < sizeof(src.data)/4; i++, words += ' ')
      {
        uint32_t w1, w2, w3;
        
        uint32_t val;

        memcpy(&val, (src.data) + (i * 4), 4);

        w1 = val % n;
        w2 = ((val / n) + w1) % n;
        w3 = (((val / n) / n) + w2) % n;

        words += words_array[w1];
        words += ' ';
        words += words_array[w2];
        words += ' ';
        words += words_array[w3];
      }
      return false;
    }

    /*!
     * \brief Gets a list of seed languages that are supported.
     * \param languages The list gets added to this.
     */
    void get_language_list(std::vector<std::string> &languages)
    {
      languages.clear();
      boost::filesystem::path languages_directory("wordlists/languages");
      if (!boost::filesystem::exists(languages_directory) ||
        !boost::filesystem::is_directory(languages_directory))
      {
        throw std::runtime_error("Word list languages directory is missing.");
      }
      boost::filesystem::directory_iterator end;
      for (boost::filesystem::directory_iterator it(languages_directory); it != end; it++)
      {
        languages.push_back(it->path().filename().string());
      }
    }

  }

}
