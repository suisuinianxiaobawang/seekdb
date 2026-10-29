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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/chunk_store/ob_compact_block_writer.h"
#include "data_plane/blocksstable/ob_storage_datum.h"

namespace oceanbase
{
using namespace common;

namespace sql
{


int ObCompactBlockWriter::CurRowInfo::init(const ChunkRowMeta *row_meta, const uint8_t offset_width, char* buf)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(row_meta) || OB_ISNULL(buf)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // Private callers instantiate the row writer with uint16_t or uint32_t.
    OB_ASSERT(offset_width == BASE_OFFSET_SIZE || offset_width == EXTENDED_OFFSET_SIZE);
    buf_ = buf;
    cur_var_offset_pos_ = 0;
    var_column_cnt_ = row_meta->col_cnt_ - row_meta->fixed_cnt_;
    bitmap_size_ = sql::ObBitVector::memory_size(row_meta->col_cnt_);
    bit_vec_ = sql::to_bit_vector(buf_ + HEAD_SIZE);
    bit_vec_->reset(row_meta->col_cnt_);
    data_offset_ = HEAD_SIZE + bitmap_size_ + (var_column_cnt_ + 1) * offset_width; // the start of fixed data buffer
    var_offset_ = data_offset_ + row_meta->var_data_off_;
  }
  return ret;
}

void ObCompactBlockWriter::CurRowInfo::reset()
{
  buf_ = nullptr;
  bit_vec_ = nullptr;
  var_column_cnt_ = 0;
  cur_var_offset_pos_ = 0;
  bitmap_size_ = 0;
  data_offset_ = 0;
  var_offset_ = 0;
}

int ObCompactBlockWriter::add_row(const common::ObIArray<ObExpr*> &exprs, ObEvalCtx &ctx, ObChunkDatumStore::StoredRow **stored_row)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(ensure_init())) {
  } else if (OB_FAIL(ensure_write(exprs, ctx))) {
  } else {
    if ((cur_row_offset_width_ == BASE_OFFSET_SIZE) &&
        OB_FAIL(inner_add_row<uint16_t>(exprs, ctx))) {
    } else if ((cur_row_offset_width_ == EXTENDED_OFFSET_SIZE) &&
                OB_FAIL(inner_add_row<uint32_t>(exprs, ctx))) {
    }
  }
  return ret;
}

int ObCompactBlockWriter::add_row(const ObChunkDatumStore::StoredRow &src_sr, ObChunkDatumStore::StoredRow **dst_sr)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ensure_init())) {
  } else if (OB_FAIL(ensure_write(src_sr))) {
  } else{
    if ((cur_row_offset_width_ == BASE_OFFSET_SIZE) &&
        OB_FAIL(inner_build_from_stored_row<uint16_t>(src_sr))) {
    } else if ((cur_row_offset_width_ == EXTENDED_OFFSET_SIZE) &&
                OB_FAIL(inner_build_from_stored_row<uint32_t>(src_sr))) {
    }
  }

  return ret;
}

int ObCompactBlockWriter::add_batch(const common::ObDatum **datums,
                                   const common::ObIArray<ObExpr *> &exprs,
                                   const uint16_t selector[], const int64_t size,
                                   ObChunkDatumStore::StoredRow **stored_rows, BatchCtx *batch_ctx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ensure_init())) {
  } else if (OB_UNLIKELY(size < 0 || exprs.count() != row_meta_->col_cnt_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == size) {
  } else if (OB_ISNULL(datums) || OB_ISNULL(selector) || OB_ISNULL(batch_ctx)
             || OB_ISNULL(batch_ctx->row_size_array_) || OB_ISNULL(batch_ctx->stored_rows_)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    uint32_t *sizes = batch_ctx->row_size_array_;
    for (int64_t i = 0; i < size; ++i) {
      sizes[i] = row_meta_->var_data_off_;
    }
    // Size variable columns once per batch, without reevaluating expressions.
    for (int64_t col = 0; OB_SUCC(ret) && col < exprs.count(); ++col) {
      if (0 == row_meta_->column_length_[col] && OB_NOT_NULL(datums[col])) {
        const bool is_batch = exprs.at(col)->is_batch_result();
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          const ObDatum &datum = datums[col][is_batch ? selector[i] : 0];
          if (!datum.is_null()) {
            if (OB_UNLIKELY(datum.len_ > UINT32_MAX - sizes[i])) {
              ret = OB_SIZE_OVERFLOW;
            } else {
              sizes[i] += datum.len_;
            }
          }
        }
      }
    }
    const int64_t bitmap_size = sql::ObBitVector::memory_size(row_meta_->col_cnt_);
    const int64_t var_count = row_meta_->col_cnt_ - row_meta_->fixed_cnt_;
    int64_t row_idx = 0;
    while (OB_SUCC(ret) && row_idx < size) {
      cur_row_offset_width_ = get_offset_width(sizes[row_idx]);
      const int64_t row_size = sizes[row_idx] + HEAD_SIZE + bitmap_size
          + (var_count + 1) * cur_row_offset_width_;
      if (OB_UNLIKELY(row_size > INT32_MAX)) {
        ret = OB_SIZE_OVERFLOW;
      } else if (OB_FAIL(ensure_write(row_size))) {
      } else if (BASE_OFFSET_SIZE == cur_row_offset_width_) {
        ret = inner_add_batch<uint16_t>(datums, exprs, selector, size, row_idx, *batch_ctx);
      } else {
        ret = inner_add_batch<uint32_t>(datums, exprs, selector, size, row_idx, *batch_ctx);
      }
    }
    // Compact rows have no StoredRow view in the block, as in add_row().
    if (OB_NOT_NULL(stored_rows)) {
      for (int64_t i = 0; i < size; ++i) {
        stored_rows[i] = nullptr;
      }
    }
  }
  return ret;
}

template <typename T>
int ObCompactBlockWriter::inner_add_batch(const common::ObDatum **datums,
                                         const common::ObIArray<ObExpr *> &exprs,
                                         const uint16_t selector[], const int64_t size,
                                         int64_t &row_idx, BatchCtx &batch_ctx)
{
  int ret = OB_SUCCESS;
  const ChunkRowMeta &meta = *row_meta_;
  const int64_t bitmap_size = sql::ObBitVector::memory_size(meta.col_cnt_);
  const int64_t var_count = meta.col_cnt_ - meta.fixed_cnt_;
  const int64_t offsets_pos = HEAD_SIZE + bitmap_size;
  const int64_t data_pos = offsets_pos + (var_count + 1) * sizeof(T);
  const int64_t begin = row_idx;
  const int64_t available = get_remain();
  char *buf = get_cur_buf();
  uint32_t *offsets = batch_ctx.row_size_array_;
  // Reuse the private pointer scratch space for compact-row addresses. No
  // StoredRow objects are constructed or exposed through these pointers.
  ObChunkDatumStore::StoredRow **rows = batch_ctx.stored_rows_;
  int64_t bytes = 0;
  for (; row_idx < size; ++row_idx) {
    const uint32_t data_size = offsets[row_idx];
    const int64_t row_size = data_size + data_pos;
    if (get_offset_width(data_size) != sizeof(T) || row_size > available - bytes) {
      break;
    }
    char *row = buf + bytes;
    rows[row_idx] = reinterpret_cast<ObChunkDatumStore::StoredRow *>(row);
    const uint32_t stored_size = row_size;
    MEMCPY(row, &stored_size, sizeof(stored_size));
    const uint8_t width = sizeof(T);
    MEMCPY(row + sizeof(stored_size), &width, sizeof(width));
    sql::to_bit_vector(row + HEAD_SIZE)->reset(meta.col_cnt_);
    const T final_offset = data_size;
    MEMCPY(row + offsets_pos + var_count * sizeof(T), &final_offset, sizeof(T));
    offsets[row_idx] = meta.var_data_off_;
    bytes += row_size;
  }
  if (OB_UNLIKELY(begin == row_idx)) {
    ret = OB_ERR_UNEXPECTED;
  }
  int64_t var_idx = 0;
  for (int64_t col = 0; OB_SUCC(ret) && col < meta.col_cnt_; ++col) {
    const int64_t fixed_len = meta.column_length_[col];
    const int64_t fixed_pos = data_pos + meta.column_offset_[col];
    const ObDatum *values = datums[col];
    const bool is_batch = OB_NOT_NULL(values) && exprs.at(col)->is_batch_result();
    for (int64_t i = begin; OB_SUCC(ret) && i < row_idx; ++i) {
      char *row = reinterpret_cast<char *>(rows[i]);
      const ObDatum *datum = OB_ISNULL(values) ? nullptr : &values[is_batch ? selector[i] : 0];
      if (0 == fixed_len) {
        const T offset = offsets[i];
        MEMCPY(row + offsets_pos + var_idx * sizeof(T), &offset, sizeof(T));
      }
      if (OB_ISNULL(datum) || datum->is_null()) {
        sql::to_bit_vector(row + HEAD_SIZE)->set(col);
      } else if (0 == fixed_len) {
        MEMCPY(row + data_pos + offsets[i], datum->ptr_, datum->len_);
        offsets[i] += datum->len_;
      } else if (OB_UNLIKELY(datum->len_ != fixed_len)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        MEMCPY(row + fixed_pos, datum->ptr_, fixed_len);
      }
    }
    var_idx += 0 == fixed_len;
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(advance(bytes))) {
    } else {
      const int64_t additional_rows = row_idx - begin - 1;
      cur_blk_->cnt_ += additional_rows;
      store_->inc_block_id_cnt(additional_rows);
      last_row_pos_ = reinterpret_cast<char *>(rows[row_idx - 1]);
    }
  }
  return ret;
}


int ObCompactBlockWriter::add_row(const blocksstable::ObStorageDatum *storage_datums,
                                  const int64_t column_count,
                                  const int64_t extra_size, ObChunkDatumStore::StoredRow **stored_row)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ensure_init())) {
  } else if (OB_FAIL(ensure_write(storage_datums, column_count, extra_size))) {
  } else {
    if ((cur_row_offset_width_ == BASE_OFFSET_SIZE) &&
         OB_FAIL(inner_add_row<uint16_t>(storage_datums, column_count, extra_size, stored_row))) {
    } else if (cur_row_offset_width_ == EXTENDED_OFFSET_SIZE &&
         OB_FAIL(inner_add_row<uint32_t>(storage_datums, column_count, extra_size, stored_row))) {
    }
  }
  return ret;
}

template <typename T>
int ObCompactBlockWriter::inner_add_row(const blocksstable::ObStorageDatum *storage_datums,
                                        const int64_t column_count,
                                        const int64_t extra_size, ObChunkDatumStore::StoredRow **stored_row)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(row_meta_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(row_info_.init(row_meta_, sizeof(T), get_cur_buf()))) {
  } else if (OB_ISNULL(row_info_.buf_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    T *var_offset_array = reinterpret_cast<T*>(row_info_.buf_ + HEAD_SIZE + row_info_.bitmap_size_);
    for (int64_t i = 0; OB_SUCC(ret) && i < column_count; i++) {
      if (OB_FAIL(inner_process_datum<T>(storage_datums[i], i, *row_meta_, row_info_))) {
      }
    }
    if (OB_SUCC(ret)) {
      // head
      uint32_t row_size = row_info_.var_offset_;
      MEMCPY(row_info_.buf_, &row_size, sizeof(row_size)); // row_size_
      MEMCPY(row_info_.buf_ + sizeof(row_size), &cur_row_offset_width_, sizeof(cur_row_offset_width_)); // the offset width
      // last offset;
      T tmp_offset = row_info_.var_offset_ - row_info_.data_offset_;
      MEMCPY(var_offset_array + row_info_.cur_var_offset_pos_, &tmp_offset, sizeof(T));
      // payload
      if (OB_FAIL(advance(row_size))) {
      }
    }
  }

  return ret;
}

template <typename T>
int ObCompactBlockWriter::inner_build_from_stored_row(const ObChunkDatumStore::StoredRow &sr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(row_meta_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(row_info_.init(row_meta_, sizeof(T), get_cur_buf()))) {
  } else if (OB_ISNULL(row_info_.buf_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    T *var_offset_array = reinterpret_cast<T*>(row_info_.buf_ + HEAD_SIZE + row_info_.bitmap_size_);
    for (int64_t i = 0 ; OB_SUCC(ret) && i < sr.cnt_; i++) {
      if (OB_FAIL(inner_process_datum<T>(sr.cells()[i], i, *row_meta_, row_info_))) {
      }
    }

    if (OB_SUCC(ret)) {
      // head
      uint32_t row_size = row_info_.var_offset_;
      MEMCPY(row_info_.buf_, &row_size, sizeof(row_size)); // row_size_
      MEMCPY(row_info_.buf_ + sizeof(row_size), &cur_row_offset_width_, sizeof(cur_row_offset_width_)); // the offset width
      // last offset
      T tmp_offset = row_info_.var_offset_ - row_info_.data_offset_;
      MEMCPY(var_offset_array + row_info_.cur_var_offset_pos_, &tmp_offset, sizeof(T));
      // payload
      if (OB_FAIL(advance(row_size))) {
      }
    }
  }

  return ret;
}

// before call this function -- we need to ensure the size if enough.
template <typename T>
int ObCompactBlockWriter::inner_add_row(const common::ObIArray<ObExpr*> &exprs, ObEvalCtx &ctx)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(row_meta_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(row_info_.init(row_meta_, sizeof(T), get_cur_buf()))) {
  } else if (OB_ISNULL(row_info_.buf_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    T *var_offset_array = reinterpret_cast<T*>(row_info_.buf_ + HEAD_SIZE + row_info_.bitmap_size_);
    for (int64_t i = 0; OB_SUCC(ret) && i < exprs.count(); i++) {
      ObExpr *expr = exprs.at(i);
      ObDatum *in_datum = NULL;
      if (OB_UNLIKELY(NULL == expr)) {
        // Set datum to NULL for NULL expr
        row_info_.bit_vec_->set(i);
        if (row_meta_->column_length_[i] == 0)  {
          T tmp_offset = row_info_.var_offset_ - row_info_.data_offset_;
          MEMCPY(var_offset_array + row_info_.cur_var_offset_pos_, &tmp_offset, sizeof(T));
          row_info_.cur_var_offset_pos_ ++;
        }
      } else if (OB_FAIL(expr->eval(ctx, in_datum))) {
      } else if (OB_NOT_NULL(in_datum)) {
        if (OB_FAIL(inner_process_datum<T>(*in_datum, i, *row_meta_, row_info_))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      // head
      uint32_t row_size = row_info_.var_offset_;
      MEMCPY(row_info_.buf_, &row_size, sizeof(row_size)); // row_size_
      MEMCPY(row_info_.buf_ + sizeof(row_size), &cur_row_offset_width_, sizeof(cur_row_offset_width_)); // the offset width
      // last offset
      T tmp_offset = row_info_.var_offset_ - row_info_.data_offset_;
      MEMCPY(var_offset_array + row_info_.cur_var_offset_pos_, &tmp_offset, sizeof(T));

      // payload
      if (OB_FAIL(advance(row_size))) {
      }
    }
  }

  return ret;
}

template <typename T>
int ObCompactBlockWriter::inner_process_datum(const ObDatum &src_datum, const int64_t cur_pos,
                                              const ChunkRowMeta &row_meta, CurRowInfo &row_info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(row_info.buf_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    T *var_offset_array = reinterpret_cast<T*>(row_info.buf_ + HEAD_SIZE + row_info.bitmap_size_);
    if (OB_ISNULL(row_info.bit_vec_) || cur_pos < 0 || cur_pos >= row_meta.col_cnt_) {
      ret = OB_INVALID_ARGUMENT;
    } else if (src_datum.is_null()) {
      row_info.bit_vec_->set(cur_pos);
      if (row_meta.column_length_[cur_pos] == 0)  {
        T tmp_offset = row_info.var_offset_ - row_info.data_offset_;
        MEMCPY(var_offset_array + row_info.cur_var_offset_pos_, &tmp_offset, sizeof(T));
        row_info.cur_var_offset_pos_ ++;
      }
    } else if (row_meta.column_length_[cur_pos] == 0) { // the column is variable size;
      if (row_info.cur_var_offset_pos_ >= row_info.var_column_cnt_) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        T tmp_offset = row_info.var_offset_ - row_info.data_offset_;
        MEMCPY(var_offset_array + row_info.cur_var_offset_pos_, &tmp_offset, sizeof(T));
        MEMCPY(row_info.buf_ + row_info.var_offset_, src_datum.ptr_, src_datum.len_);
        row_info.var_offset_ += src_datum.len_;
        row_info.cur_var_offset_pos_ ++;
      }
    } else { // the column is fixed size;
      if (src_datum.len_ != row_meta.column_length_[cur_pos]) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        MEMCPY(row_info.buf_ + row_info.data_offset_ + row_meta.column_offset_[cur_pos], src_datum.ptr_,
             row_meta.column_length_[cur_pos]);
      }
    }
  }
  return ret;
}

int ObCompactBlockWriter::get_row_stored_size(const common::ObIArray<ObExpr*> &exprs, ObEvalCtx &ctx, uint64_t &size)
{
  int ret = OB_SUCCESS;
  const ChunkRowMeta &row_meta = *get_meta();
  int64_t bit_map_size = sql::ObBitVector::memory_size(row_meta.col_cnt_);
  int64_t data_size = 0;
  int64_t offset_size = 0;
  int8_t tmp_offset_width = 0; // byte size of each offset in a row;
  size = 0;

  ObExpr *expr = nullptr;
  common::ObDatum *datum = nullptr;
  for (int64_t i = 0; OB_SUCC(ret) && i < exprs.count(); ++i) {
    if (row_meta.column_length_[i] != 0) {
      data_size += row_meta.column_length_[i];
    } else {
      expr = exprs.at(i);
      if (OB_ISNULL(expr)) {
      } else if (OB_FAIL(expr->eval(ctx, datum))) {
      } else if (OB_ISNULL(datum)) {
        ret = OB_ERR_UNEXPECTED;
        SQL_ENG_LOG(WARN, "the datum is null", K(ret), KP(datum));
      } else if (!datum->is_null()) {
        data_size += datum->len_;
      }
    }
  }

  if (OB_SUCC(ret)) {
    cur_row_offset_width_ = get_offset_width(data_size);
    offset_size = (row_meta.col_cnt_ - row_meta.fixed_cnt_ + 1) * cur_row_offset_width_;
    size = data_size + offset_size + bit_map_size + HEAD_SIZE;

    // offset_width_ is used in add_row
    cur_row_size_ = size;
  }

  return ret;
}

int ObCompactBlockWriter::get_row_stored_size(const ObChunkDatumStore::StoredRow &sr, uint64_t &size)
{
  int ret = OB_SUCCESS;
  const ChunkRowMeta &row_meta = *get_meta();
  int64_t bit_map_size = sql::ObBitVector::memory_size(row_meta.col_cnt_);
  int64_t data_size = 0;
  int64_t offset_size = 0;
  size = 0;

  ObExpr *expr = nullptr;
  common::ObDatum *datum = nullptr;
  for (int64_t i = 0; OB_SUCC(ret) && i < sr.cnt_; ++i) {
    if (row_meta.column_length_[i] != 0) {
      data_size +=  row_meta.column_length_[i];
    } else if (!sr.cells()[i].is_null()) {
      data_size += sr.cells()[i].len_;
    }
  }

  if (OB_SUCC(ret)) {
    cur_row_offset_width_ = get_offset_width(data_size);
    offset_size = (row_meta.col_cnt_ - row_meta.fixed_cnt_ + 1) * cur_row_offset_width_;
    size = data_size + offset_size + bit_map_size + HEAD_SIZE;

    // offset_width_ is used in add_row
    cur_row_size_ = size;
  }
  return ret;
}


int ObCompactBlockWriter::get_row_stored_size(const blocksstable::ObStorageDatum *storage_datums,
                                              const int64_t column_count,
                                              const int64_t extra_size, uint64_t &size)
{
  int ret = OB_SUCCESS;
  int64_t head_size = sizeof(ObChunkDatumStore::StoredRow);
  const ChunkRowMeta &row_meta = *get_meta();
  int64_t bit_map_size = sql::ObBitVector::memory_size(row_meta.col_cnt_);
  int64_t data_size = 0;
  int64_t offset_size = 0;
  for (int64_t i = 0; i < column_count; ++i) {
    if (row_meta.column_length_[i] != 0) {
      data_size += row_meta.column_length_[i];
    } else {
      if (!storage_datums[i].is_null()) {
        data_size += storage_datums[i].len_;
      }
    }
  }
  cur_row_offset_width_ = get_offset_width(data_size);
  offset_size = (row_meta.col_cnt_ - row_meta.fixed_cnt_ + 1) * cur_row_offset_width_;
  size = HEAD_SIZE + bit_map_size + offset_size + extra_size + data_size;
  return ret;
}


int ObCompactBlockWriter::close()
{
  int ret = OB_SUCCESS;
  inited_ = false;
  return ret;
}

int ObCompactBlockWriter::ensure_write(const common::ObIArray<ObExpr*> &exprs, ObEvalCtx &ctx)
{
  int ret = OB_SUCCESS;
  uint64_t row_size = 0;
  if (OB_FAIL(get_row_stored_size(exprs, ctx, row_size))) {
  } else if (OB_FAIL(ensure_write(row_size))) {
  }
  return ret;
}

int ObCompactBlockWriter::ensure_write(const blocksstable::ObStorageDatum *storage_datums,
                                       const int64_t column_count,
                                       const int64_t extra_size)
{
  int ret = OB_SUCCESS;
  uint64_t row_size = 0;
  if (OB_FAIL(get_row_stored_size(storage_datums, column_count, extra_size, row_size))) {
  } else if (OB_FAIL(ensure_write(row_size))) {
  }

  return ret;
}

int ObCompactBlockWriter::ensure_write(const ObChunkDatumStore::StoredRow &sr)
{
  int ret = OB_SUCCESS;
  uint64_t row_size = 0;
  if (OB_FAIL(get_row_stored_size(sr, row_size))) {
  } else if (OB_FAIL(ensure_write(row_size))) {
  }
  return ret;
}

int ObCompactBlockWriter::ensure_write(const int64_t size)
{
  int ret = OB_SUCCESS;
  if (is_overflow(size)) {
    int64_t new_blk_size = size < DEFAULT_BUF_SIZE ? DEFAULT_BUF_SIZE : size;
    ObTempBlockStore::Block *tmp_blk = nullptr;
    if (OB_FAIL(store_->new_block(new_blk_size, tmp_blk, true))) {
    } else if (OB_ISNULL(tmp_blk)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      cur_blk_ = tmp_blk;
    }
  } else {
    // do nothing, directly add row to buffer.
  }

  return ret;
}
int ObCompactBlockWriter::get_last_stored_row(const ObChunkDatumStore::StoredRow *&sr)
{
  int ret = OB_SUCCESS;
  const char *compact_row = get_last_row();
  int64_t size = 0;
  ObChunkDatumStore::StoredRow *tmp_sr = nullptr;
  sr = nullptr;
  // convert from compact format to storedrow;
  if (OB_ISNULL(compact_row) || OB_ISNULL(row_meta_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_get_stored_row_size(compact_row, size))) {
  } else if (size <= 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(tmp_sr = reinterpret_cast<ObChunkDatumStore::StoredRow*>(store_->alloc(size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    const int8_t offset_width = *reinterpret_cast<const int8_t*>(compact_row + sizeof(int32_t));
    if (offset_width == BASE_OFFSET_SIZE) {
      if (OB_FAIL(convert_to_stored_row<uint16_t>(compact_row, tmp_sr))) {
      }
    } else if (offset_width == EXTENDED_OFFSET_SIZE) {
      if (OB_FAIL(convert_to_stored_row<uint32_t>(compact_row, tmp_sr))) {
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
    if (OB_FAIL(ret)) {
      store_->free(tmp_sr, size);
    }
  }
  if (OB_SUCC(ret)) {
    sr = tmp_sr;
    last_stored_row_ = tmp_sr;
    last_sr_size_ = size;
  }
  return ret;
}

int ObCompactBlockWriter::inner_get_stored_row_size(const char *compact_row, int64_t &size)
{
  int ret = OB_SUCCESS;
  size = 0;
  if (OB_ISNULL(row_meta_) || OB_ISNULL(compact_row)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObCompactBlockReader::calc_stored_row_size(compact_row, row_meta_, size))){
  }

  return ret;
}

template <typename T>
int ObCompactBlockWriter::convert_to_stored_row(const char *compact_row, ObChunkDatumStore::StoredRow *sr)
{
  //TODO DAISI: this function is similar to ObCompactBlockReader::get_stored_row
  int ret = OB_SUCCESS;
  if (OB_ISNULL(row_meta_) || OB_ISNULL((compact_row))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int64_t offset_width = sizeof(T);
    const int32_t row_size = *reinterpret_cast<const int32_t*>(compact_row);
    int64_t bitmap_size = sql::ObBitVector::memory_size(row_meta_->col_cnt_);
    const sql::ObBitVector *bit_vec = sql::to_bit_vector(compact_row + HEAD_SIZE);
    int64_t cur_var_offset_pos = 0;
    const int64_t var_column_cnt = row_meta_->col_cnt_ - row_meta_->fixed_cnt_;
    const int64_t data_offset = HEAD_SIZE + bitmap_size + (var_column_cnt + 1) * offset_width; // the start of fixed data buffer
    int64_t cur_data_ptr = sizeof(ObDatum) * row_meta_->col_cnt_ + sizeof(ObChunkDatumStore::StoredRow);
    const T *offset_array = reinterpret_cast<const T *>(compact_row + HEAD_SIZE + bitmap_size);
    for (int64_t i = 0; OB_SUCC(ret) && i < row_meta_->col_cnt_; i++) {
      ObDatum *cur_datum = reinterpret_cast<ObDatum *>(sr->payload_ + i * sizeof(ObDatum));
      uint32_t len = 0;
      char *tmp_ptr = reinterpret_cast<char *>(sr) + cur_data_ptr;
      if (bit_vec->at(i)) {
        cur_datum->set_null();
        if (row_meta_->column_length_[i] == 0) {
          cur_var_offset_pos ++;
        }
      } else if (row_meta_->column_length_[i] == 0) {
        const T offset = offset_array[cur_var_offset_pos];
        if (cur_var_offset_pos < var_column_cnt) {
          len = offset_array[cur_var_offset_pos +  1] - offset;
        } else {
          ret = OB_INDEX_OUT_OF_RANGE;
        }
        if (OB_SUCC(ret)) {
          // set datum->len_, use the pack_ to conver the NULL_FLAG
          cur_datum->pack_ = len;
          // set data
          MEMCPY(tmp_ptr, compact_row + data_offset + offset, len);
          // set datum->ptr_
          cur_datum->ptr_ = tmp_ptr;
          cur_var_offset_pos ++;
        }
      } else {
        const int32_t offset = row_meta_->column_offset_[i];
        len = row_meta_->column_length_[i];
        cur_datum->pack_ = len;
        MEMCPY(tmp_ptr, compact_row + data_offset + offset, len);
        cur_datum->ptr_ = tmp_ptr;
      }
      cur_data_ptr += len;
    }
    if (OB_SUCC(ret)) {
      sr->cnt_ = row_meta_->col_cnt_;
      sr->row_size_ =  cur_data_ptr;
    }
  }

  return ret;
}

}
}
