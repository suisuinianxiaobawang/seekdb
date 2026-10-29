/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "sql/engine/sort/ob_sort_op_impl.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::sql;

#define REQUIRE(x) do { if (!(x)) { \
  std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #x); \
  std::abort(); } } while (false)

static int64_t checks = 0;
static bool byte_less(const std::string &a, const std::string &b)
{
  const int cmp = std::memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
  return cmp < 0 || (cmp == 0 && a.size() < b.size());
}

static void run_case(int64_t size, int mode, uint64_t seed)
{
  std::mt19937_64 rng(seed);
  std::vector<std::string> keys(size);
  for (int64_t i = 0; i < size; ++i) {
    const int64_t length = rng() % 120;
    switch (mode) {
      case 0: keys[i] = std::string(length, '\0'); break;
      case 1: keys[i] = std::string(200, 'p') + std::to_string(rng() % 257); break;
      case 2: keys[i] = std::string(i % 257, static_cast<char>(i % 256)); break;
      case 3: keys[i] = i % 7 == 0 ? "" : "identical"; break;
      case 4: keys[i] = std::string(1, static_cast<char>(i % 256)); break;
      case 6: keys[i] = std::string(i % 2048, 'q'); break;
      default: keys[i] = std::string(length, '\0');
    }
    if (mode == 0 || mode == 5) {
      for (char &ch : keys[i]) {
        ch = static_cast<char>(rng() % 256);
      }
    }
  }
  if (mode == 5) {
    std::sort(keys.begin(), keys.end(), byte_less);
    if (seed & 1) {
      std::reverse(keys.begin(), keys.end());
    }
  }
  auto expected = keys;
  std::sort(expected.begin(), expected.end(), byte_less);
  ObArenaAllocator allocator("RadixTest");
  ObSEArray<ObChunkDatumStore::StoredRow *, 16> rows;
  // Exercise sorting a subrange: guard rows at either side must stay untouched.
  auto *guard = reinterpret_cast<ObChunkDatumStore::StoredRow *>(UINTPTR_MAX);
  REQUIRE(OB_SUCCESS == rows.push_back(guard));
  for (int64_t i = 0; i < size; ++i) {
    const int64_t bytes = sizeof(ObChunkDatumStore::StoredRow) + sizeof(ObDatum);
    auto *row = new (allocator.alloc(bytes)) ObChunkDatumStore::StoredRow();
    REQUIRE(row != nullptr);
    row->cnt_ = 1;
    row->row_size_ = bytes;
    new (&row->cells()[0]) ObDatum();
    row->cells()[0].set_string(keys[i].data(), keys[i].size());
    REQUIRE(OB_SUCCESS == rows.push_back(row));
  }
  REQUIRE(OB_SUCCESS == rows.push_back(guard));
  ObSortOpImpl::ObAdaptiveQS sort(rows, allocator);
  bool can_encode = false;
  REQUIRE(OB_SUCCESS == sort.init(rows, allocator, 1, size + 1, can_encode));
  REQUIRE(can_encode);
  sort.sort(1, size + 1);
  REQUIRE(rows.at(0) == guard && rows.at(size + 1) == guard);
  std::vector<bool> seen(size, false);
  for (int64_t i = 0; i < size; ++i) {
    const ObDatum &value = rows.at(i + 1)->cells()[0];
    const std::string actual(value.ptr_, value.len_);
    REQUIRE(actual == expected[i]);
    ++checks;
  }
}

int main()
{
  int cases = 0;
  for (int64_t size : {0, 1, 2, 15, 16, 17, 255, 256, 257, 1027, 8193}) {
    for (int mode = 0; mode < 7; ++mode) {
      for (uint64_t seed = 0; seed < 3; ++seed) {
        run_case(size, mode, seed + 20261029);
        ++cases;
      }
    }
  }
  std::printf("%d radix/reference cases passed; %ld key checks\n", cases, checks);
  return 0;
}
