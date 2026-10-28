/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_struct.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

namespace
{
bool is_ascii_english_document(const ObFTParserParam &param)
{
  bool is_ascii = param.ft_length_ <= UINT32_MAX
      && ObCharset::charset_type_by_coll(static_cast<ObCollationType>(param.cs_->number)) == CHARSET_UTF8MB4;
  for (int64_t i = 0; is_ascii && i < param.ft_length_; ++i) {
    is_ascii = static_cast<unsigned char>(param.fulltext_[i]) < 0x80;
  }
  return is_ascii;
}

// The generic English pipeline splits, trims and lowercases each token. For
// ASCII UTF-8 input the same operations need no multibyte decoding or stream.
class BasicEnglishAsciiTokenIter final : public ObITokenIterator
{
public:
  explicit BasicEnglishAsciiTokenIter(const ObFTParserParam &param)
    : allocator_(*param.allocator_), cs_(param.cs_), text_(param.fulltext_),
      length_(param.ft_length_), pos_(0)
  {}

  virtual int get_next_token(const char *&word, int64_t &word_len,
                            int64_t &char_len, int64_t &word_freq) override
  {
    int ret = OB_SUCCESS;
    word = nullptr;
    word_len = char_len = word_freq = 0;
    bool found = false;
    while (OB_SUCC(ret) && !found) {
      while (pos_ < length_ && is_delimiter(text_[pos_])) {
        ++pos_;
      }
      int64_t begin = pos_;
      while (pos_ < length_ && !is_delimiter(text_[pos_])) {
        ++pos_;
      }
      int64_t end = pos_;
      while (begin < end && !ob_isalnum(cs_, text_[begin])) {
        ++begin;
      }
      while (begin < end && !ob_isalnum(cs_, text_[end - 1])) {
        --end;
      }
      if (begin == end) {
        if (pos_ == length_) {
          ret = OB_ITER_END;
        }
      } else {
        char *buf = static_cast<char *>(allocator_.alloc(end - begin));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (int64_t i = begin; i < end; ++i) {
            const char ch = text_[i];
            buf[i - begin] = ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
          }
          word = buf;
          word_len = char_len = end - begin;
          word_freq = 1;
          found = true;
        }
      }
    }
    return ret;
  }

  VIRTUAL_TO_STRING_KV(K_(length), K_(pos));

private:
  bool is_delimiter(const char ch) const
  {
    return ob_isspace(cs_, ch) || ob_iscntrl(cs_, ch) || ob_ispunct(cs_, ch);
  }

  ObIAllocator &allocator_;
  const ObCharsetInfo *cs_;
  const char *text_;
  int64_t length_;
  int64_t pos_;
};
} // namespace

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(normalizer_.get_next(token, token_freq, allocator_))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    word = token.ptr_;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    doc_.set_string(param->fulltext_, param->ft_length_);
    if (OB_FAIL(normalizer_.init(param->cs_, tokenizer_))) {
    } else if (OB_FAIL(tokenizer_.open(doc_, param->cs_))) {
    } else {
      is_inited_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  normalizer_.reset();
  tokenizer_.reset();
  doc_.reset();
  is_inited_ = false;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_ascii_english_document(*param)) {
    if (OB_ISNULL(iter = OB_NEWx(BasicEnglishAsciiTokenIter, param->allocator_, *param))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  } else if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, param->allocator_, *(param->allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser->init(param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret) && OB_NOT_NULL(parser)) {
    OB_DELETEx(ObBEngFTParser, param->allocator_, parser);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter)) {
    abort_unless(nullptr != param);
    abort_unless(nullptr != param->allocator_);
    iter->~ObITokenIterator();
    param->allocator_->free(iter);
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
