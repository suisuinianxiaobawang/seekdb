/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */

#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "data_plane/blocksstable/ob_storage_datum.h"
#include "sql/engine/basic/chunk_store/ob_compact_store.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::sql;

// Standalone round-trip test: link against the same libraries as seekdb.
// Keep checks active in release builds.
#define REQUIRE(x) do { if (!(x)) { \
  std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #x); \
  std::abort(); } } while (false)

static int64_t checks = 0;
static constexpr int64_t COLS = 8;

static void check_row(const ObChunkDatumStore::StoredRow &row,
                      const std::array<std::vector<ObDatum>, COLS> &values,
                      const int64_t index, const bool constants)
{
  REQUIRE(row.cnt_ == COLS);
  for (int64_t col = 0; col < COLS; ++col) {
    const int64_t source = constants && (col == 3 || col == 5) ? 0 : index;
    const ObDatum &expected = values[col][source];
    const ObDatum &actual = row.cells()[col];
    REQUIRE(actual.is_null() == expected.is_null());
    if (!expected.is_null()) {
      REQUIRE(actual.len_ == expected.len_);
      REQUIRE(0 == std::memcmp(actual.ptr_, expected.ptr_, expected.len_));
    }
    ++checks;
  }
}

static void run_case(const int64_t batch_size, const bool long_rows, const bool constants)
{
  ObArenaAllocator alloc("CompactBatch");
  ObExpr expressions[COLS];
  ExprFixedArray exprs(alloc);
  REQUIRE(OB_SUCCESS == exprs.init(COLS));
  const ObObjType types[COLS] = {ObIntType, ObVarcharType, ObVarcharType,
      ObUInt64Type, ObDoubleType, ObVarcharType, ObVarcharType, ObVarcharType};
  for (int64_t col = 0; col < COLS; ++col) {
    expressions[col].datum_meta_.type_ = types[col];
    expressions[col].batch_result_ = !(constants && (col == 3 || col == 5));
    REQUIRE(OB_SUCCESS == exprs.push_back(&expressions[col]));
  }
  ObCompactStore batch(&alloc), scalar(&alloc);
  REQUIRE(OB_SUCCESS == batch.init(0, ObCtxIds::DEFAULT_CTX_ID, "BatchRows", false,
                                 0, false, NONE_COMPRESSOR, &exprs));
  REQUIRE(OB_SUCCESS == scalar.init(0, ObCtxIds::DEFAULT_CTX_ID, "ScalarRows", false,
                                  0, false, NONE_COMPRESSOR, &exprs));
  // A null expression has the same layout as the nullable metadata column.
  exprs.at(6) = nullptr;
  const int64_t count = long_rows ? 97 : 1031;
  std::array<std::vector<std::string>, COLS> storage;
  std::array<std::vector<ObDatum>, COLS> values;
  const int64_t long_lengths[] = {65499, 65508, 65509, 65510, 65511, 65512,
      65513, 65514, 65515, 65516, 65517, 65518, 65519, 65520, 65521, 65522,
      65523, 65524, 65525, 65526, 65527, 65528, 65529, 65530, 65531, 65532,
      65533, 65534, 65535, 65536, 65537, 70000, 131071};
  for (int64_t col = 0; col < COLS; ++col) {
    storage[col].resize(count);
    values[col].resize(count);
    for (int64_t i = 0; i < count; ++i) {
      const bool fixed = col == 0 || col == 3 || col == 4;
      const int64_t len = fixed ? 8 : col == 1 && long_rows
          ? long_lengths[i % (sizeof(long_lengths) / sizeof(long_lengths[0]))]
          : (i * 17 + col * 3) % 73;
      storage[col][i] = std::string(len, static_cast<char>((i + col) % 256));
      if (!storage[col][i].empty()) {
        storage[col][i][0] = '\0';
      }
      values[col][i].set_string(storage[col][i].data(), storage[col][i].size());
      if (col == 6 || ((col == 4 || col == 7) && i % 5 == 0)) {
        values[col][i].set_null();
      }
    }
  }
  std::vector<int64_t> selected;
  for (int64_t i = count - 1; i >= 0; --i) {
    if (i % 7 != 0) {
      selected.push_back(i);
    }
  }
  const ObDatum *datums[COLS];
  for (int64_t col = 0; col < COLS; ++col) {
    datums[col] = col == 6 ? nullptr : values[col].data();
  }
  std::vector<uint16_t> selector(batch_size);
  std::vector<uint32_t> sizes(batch_size);
  std::vector<ObChunkDatumStore::StoredRow *> pointers(batch_size);
  BatchCtx ctx;
  ctx.row_size_array_ = sizes.data();
  ctx.stored_rows_ = pointers.data();
  REQUIRE(OB_SUCCESS == batch.get_writer()->add_batch(nullptr, exprs, nullptr, 0, nullptr, nullptr));
  REQUIRE(OB_INVALID_ARGUMENT == batch.get_writer()->add_batch(datums, exprs, selector.data(), -1, nullptr, &ctx));
  for (int64_t begin = 0; begin < static_cast<int64_t>(selected.size()); begin += batch_size) {
    const int64_t n = std::min(batch_size, static_cast<int64_t>(selected.size()) - begin);
    for (int64_t i = 0; i < n; ++i) {
      selector[i] = selected[begin + i];
    }
    // Exercise output/scratch aliasing as used by ObCompactStore::add_batch.
    auto **out = begin % 2 == 0 ? pointers.data() : nullptr;
    REQUIRE(OB_SUCCESS == batch.get_writer()->add_batch(datums, exprs, selector.data(), n, out, &ctx));
    if (out != nullptr) {
      for (int64_t i = 0; i < n; ++i) {
        REQUIRE(out[i] == nullptr);
      }
    }
    for (int64_t i = 0; i < n; ++i) {
      blocksstable::ObStorageDatum row[COLS];
      for (int64_t col = 0; col < COLS; ++col) {
        const int64_t source = constants && (col == 3 || col == 5) ? 0 : selector[i];
        row[col].shallow_copy_from_datum(values[col][source]);
      }
      REQUIRE(OB_SUCCESS == scalar.get_writer()->add_row(row, COLS, 0, nullptr));
    }
  }
  REQUIRE(batch.get_block_id_cnt() == static_cast<int64_t>(selected.size()));
  const ObChunkDatumStore::StoredRow *last = nullptr;
  REQUIRE(OB_SUCCESS == batch.get_last_stored_row(last));
  REQUIRE(last != nullptr);
  check_row(*last, values, selected.back(), constants);
  REQUIRE(OB_SUCCESS == batch.finish_add_row(false));
  REQUIRE(OB_SUCCESS == scalar.finish_add_row(false));
  for (ObCompactStore *store : {&batch, &scalar}) {
    for (int pass = 0; pass < 2; ++pass) {
      store->rescan();
      for (const int64_t index : selected) {
        const ObChunkDatumStore::StoredRow *row = nullptr;
        REQUIRE(OB_SUCCESS == store->get_next_row(row));
        REQUIRE(row != nullptr);
        check_row(*row, values, index, constants);
      }
      const ObChunkDatumStore::StoredRow *row = nullptr;
      REQUIRE(OB_ITER_END == store->get_next_row(row));
    }
  }
}

int main()
{
  OB_LOGGER.set_log_level("ERROR");
  for (int64_t batch_size : {1, 2, 17, 255, 256, 257, 511, 512, 513}) {
    for (bool long_rows : {false, true}) {
      for (bool constants : {false, true}) {
        run_case(batch_size, long_rows, constants);
      }
    }
  }
  std::printf("36 compact batch/scalar round-trip cases passed; %ld cell checks\n", checks);
  return 0;
}
