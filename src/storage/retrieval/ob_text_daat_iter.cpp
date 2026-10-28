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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_project_relevance()
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else if (OB_FAIL(union_id_cmp_.init(param.base_param_->id_proj_expr_->obj_meta_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  reset_union();
  union_id_cmp_.reset();
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  reset_union();
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

void ObTextDaaTIter::reset_union()
{
  for (int64_t i = 0; i < MAX_UNION_DIMS; ++i) {
    union_heads_[i] = nullptr;
  }
  union_pending_ = 0;
  union_started_ = false;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  if (is_inited_ && sql::NATURAL_LANGUAGE_MODE == mode_flag_ && !function_lookup_mode_
      && !iter_param_->need_project_relevance() && !iter_param_->need_filter()
      && nullptr == iter_param_->dim_weights_
      && dim_iters_->count() > 0 && dim_iters_->count() <= MAX_UNION_DIMS) {
    return get_union_rows(capacity, count);
  }
  return ObSRDaaTIterImpl::get_next_rows(capacity, count);
}

int ObTextDaaTIter::compare_union_ids(const ObDatum &lhs, const ObDatum &rhs, int &cmp) const
{
  const sql::ObDatumMeta &meta = iter_param_->id_proj_expr_->datum_meta_;
  if (!lhs.is_null() && !rhs.is_null() && !lhs.is_ext() && !rhs.is_ext()) {
    if (meta.type_ == ObUInt64Type) {
      cmp = (lhs.get_uint() > rhs.get_uint()) - (lhs.get_uint() < rhs.get_uint());
      return OB_SUCCESS;
    } else if (meta.type_ == ObVarcharType && meta.cs_type_ == CS_TYPE_BINARY) {
      const int64_t len = MIN(lhs.len_, rhs.len_);
      cmp = len == 0 ? 0 : MEMCMP(lhs.ptr_, rhs.ptr_, len);
      if (cmp == 0) {
        cmp = (lhs.len_ > rhs.len_) - (lhs.len_ < rhs.len_);
      }
      return OB_SUCCESS;
    }
  }
  return union_id_cmp_.compare(lhs, rhs, cmp);
}

int ObTextDaaTIter::get_union_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  const int64_t dims = dim_iters_->count();
  const int64_t batch_capacity = MIN(capacity, iter_param_->max_batch_size_);
  if (iter_param_->limit_param_->is_valid()
      && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    return OB_ITER_END;
  }
  if (!union_started_) {
    union_pending_ = (UINT64_C(1) << dims) - 1;
    union_started_ = true;
  }
  // A small natural-language membership query only needs the sorted union of
  // its posting lists. Preserve each head across batches and advance every
  // list sharing the emitted ID, without a merge heap or relevance collector.
  while (OB_SUCC(ret) && count < batch_capacity) {
    for (int64_t i = 0; OB_SUCC(ret) && i < dims; ++i) {
      if (union_pending_ & (UINT64_C(1) << i)) {
        auto *iter = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
        if (OB_FAIL(iter->get_next_row())) {
          if (OB_ITER_END == ret) {
            union_heads_[i] = nullptr;
            ret = OB_SUCCESS;
          }
        } else if (OB_FAIL(iter->get_curr_id(union_heads_[i]))) {
        }
      }
    }
    union_pending_ = 0;
    const ObDatum *minimum = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < dims; ++i) {
      if (nullptr != union_heads_[i]) {
        int cmp = -1;
        if (nullptr != minimum && OB_FAIL(compare_union_ids(*union_heads_[i], *minimum, cmp))) {
        } else if (cmp < 0) {
          minimum = union_heads_[i];
          union_pending_ = UINT64_C(1) << i;
        } else if (cmp == 0) {
          union_pending_ |= UINT64_C(1) << i;
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (nullptr == minimum) {
      ret = OB_ITER_END;
    } else if (OB_FAIL(cache_result(count, *minimum, 0.0))) {
    }
  }
  if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0) {
    int project_ret = project_results(count);
    if (OB_SUCCESS != project_ret) {
      ret = project_ret;
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  const int64_t call_start = ObTimeUtility::current_monotonic_time();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  const int64_t estimate_us = ObTimeUtility::current_monotonic_time() - call_start;
  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  const int64_t call_total_us = ObTimeUtility::current_monotonic_time() - call_start;
  LOG_TRACE("FTS_TOPK_EXEC_STATS", K(ret), K(capacity), K(estimate_us), K(call_total_us));
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
