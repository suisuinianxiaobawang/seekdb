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

#include "ob_sparse_bmw_iter.h"
#include "ob_block_max_iter.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase
{
namespace storage
{

ObSRBMWIterImpl::ObSRBMWIterImpl()
  : ObSRDaaTIterImpl(),
    allocator_("BMWAlloc"),
    less_score_cmp_(),
    top_k_heap_(less_score_cmp_, &allocator_),
    top_k_count_(0),
    domain_id_cmp_(),
    id_cache_(),
    status_(BMWStatus::MAX_STATUS),
    candidate_range_mask_(0),
    candidate_range_score_(0.0),
    candidate_range_valid_(false)
{}

int ObSRBMWIterImpl::init(
    ObSparseRetrievalMergeParam &iter_param,
    ObIArray<ObISRDaaTDimIter *> &dim_iters,
    ObIAllocator &iter_allocator,
    ObSRDaaTRelevanceCollector &relevance_collector)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObSRDaaTIterImpl::init(iter_param, dim_iters, iter_allocator, relevance_collector))) {
  } else if (OB_UNLIKELY(iter_param.topk_limit_ < 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (iter_param.topk_limit_ == 0) {
    top_k_count_ = 0;
    status_ = BMWStatus::FINISHED;
    is_inited_ = true;
  } else {
    top_k_count_ = iter_param.topk_limit_;
    status_ = BMWStatus::MAX_STATUS;
    id_cache_.set_allocator(&allocator_);
    if (OB_FAIL(domain_id_cmp_.init(iter_param.id_proj_expr_->obj_meta_))) {
    } else if (OB_FAIL(id_cache_.init(top_k_count_))) {
    } else if (OB_FAIL(id_cache_.prepare_allocate(top_k_count_))) {
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

void ObSRBMWIterImpl::reuse(const bool switch_tablet)
{
  search_stats_ = SearchStats();
  candidate_range_valid_ = false;
  while (!top_k_heap_.empty()) {
    top_k_heap_.pop();
  }
  status_ = BMWStatus::MAX_STATUS;
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

void ObSRBMWIterImpl::reset()
{
  search_stats_ = SearchStats();
  candidate_range_valid_ = false;
  top_k_heap_.reset();
  top_k_count_ = 0;
  status_ = BMWStatus::MAX_STATUS;
  domain_id_cmp_.reset();
  id_cache_.reset();
  allocator_.reset();
}

int ObSRBMWIterImpl::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(capacity <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (BMWStatus::FINISHED == status_) {
    // skip
  } else if (OB_FAIL(top_k_search())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else {
      ret = OB_SUCCESS;
    }
  }

  if (FAILEDx(project_rows_from_top_k_heap(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObSRBMWIterImpl::top_k_search()
{
  int ret = OB_SUCCESS;
  search_stats_ = SearchStats();
  candidate_range_valid_ = false;
  const int64_t search_start = ObTimeUtility::current_monotonic_time();
  int64_t build_heap_us = 0;
  int64_t prepare_wand_us = 0;
  if (OB_UNLIKELY(BMWStatus::MAX_STATUS != status_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(build_top_k_heap())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else {
      status_ = BMWStatus::FINISHED;
    }
  } else {
    const int64_t prepare_start = ObTimeUtility::current_monotonic_time();
    if (OB_FAIL(init_before_wand_process())) {
    } else {
      status_ = BMWStatus::FIND_NEXT_PIVOT;
    }
    prepare_wand_us = ObTimeUtility::current_monotonic_time() - prepare_start;
  }

  build_heap_us = ObTimeUtility::current_monotonic_time() - search_start - prepare_wand_us;

  // stats for debug & optimization
  int64_t pivot_cnt = 0;
  int64_t eval_pivot_cnt = 0;
  int64_t eval_pivot_range_cnt = 0;
  int64_t skip_range_cnt = 0;

  int64_t pivot_iter_idx = 0;
  while (OB_SUCC(ret) && BMWStatus::FINISHED != status_) {
    switch (status_) {
    case BMWStatus::FIND_NEXT_PIVOT: {
      if (OB_FAIL(next_pivot(pivot_iter_idx))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        } else {
          status_ = BMWStatus::FINISHED;
          ret = OB_SUCCESS;
        }
      } else {
        ++pivot_cnt;
        status_ = BMWStatus::EVALUATE_PIVOT_RANGE;
      }
      break;
    }
    case BMWStatus::EVALUATE_PIVOT_RANGE: {
      bool is_candidate = false;
      if (OB_FAIL(evaluate_pivot_range(pivot_iter_idx, is_candidate))) {
      } else if (is_candidate) {
        ++eval_pivot_cnt;
        status_ = BMWStatus::EVALUATE_PIVOT;
      } else {
        status_ = BMWStatus::FIND_NEXT_PIVOT_RANGE;
      }
      break;
    }
    case BMWStatus::EVALUATE_PIVOT: {
      if (OB_FAIL(evaluate_pivot(pivot_iter_idx))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        } else {
          status_ = BMWStatus::FINISHED;
          ret = OB_SUCCESS;
        }
      } else {
        status_ = BMWStatus::FIND_NEXT_PIVOT;
      }
      break;
    }
    case BMWStatus::FIND_NEXT_PIVOT_RANGE: {
      if (OB_FAIL(next_pivot_range(skip_range_cnt))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        } else {
          status_ = BMWStatus::FINISHED;
          ret = OB_SUCCESS;
        }
      } else {
        // For pivots found next_pivot_range, maybe we can save one evaluate_pivot_range call
        ++eval_pivot_range_cnt;
        status_ = BMWStatus::FIND_NEXT_PIVOT;
      }
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
    }
    }
  }

  const int64_t search_total_us = ObTimeUtility::current_monotonic_time() - search_start;
  LOG_TRACE("FTS_TOPK_SEARCH_STATS", K(ret), K_(top_k_count),
      "dim_count", dim_iters_->count(), K_(search_stats),
      K(pivot_cnt), K(eval_pivot_cnt), K(eval_pivot_range_cnt), K(skip_range_cnt),
      K(build_heap_us), K(prepare_wand_us), K(search_total_us),
      "heap_count", top_k_heap_.count(), "threshold", get_top_k_threshold());
  return ret;
}

int ObSRBMWIterImpl::build_top_k_heap()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!top_k_heap_.empty())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    bool need_project = true;
    double relevance = 0.0;
    const ObDatum *id_datum = nullptr;
    while (OB_SUCC(ret) && top_k_heap_.count() < top_k_count_) {
      if (OB_FAIL(fill_merge_heap())) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        }
      } else if (OB_FAIL(collect_dims_by_id(id_datum, relevance, need_project))) {
      }
    }

    if (FAILEDx(fill_merge_heap())) {
    }
  }
  return ret;
}


int ObSRBMWIterImpl::process_collected_row(const ObDatum &id_datum, const double relevance)
{
  int ret = OB_SUCCESS;
  ++search_stats_.merged_doc_cnt_;
  if (OB_UNLIKELY(top_k_heap_.count() > top_k_count_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (top_k_heap_.count() < top_k_count_) {
    if (OB_FAIL(id_cache_.at(top_k_heap_.count()).from_datum(id_datum))) {
    } else if (OB_FAIL(top_k_heap_.push(TopKItem(relevance, top_k_heap_.count())))) {
    } else {
      ++search_stats_.heap_insert_cnt_;
    }
  } else {
    const int64_t cache_idx = top_k_heap_.top().cache_idx_;
    const double top_k_threshold = get_top_k_threshold();
    if (relevance <= top_k_threshold) {
      // not a new top k candidate
      ++search_stats_.heap_reject_cnt_;
    } else if (OB_FAIL(id_cache_.at(cache_idx).from_datum(id_datum))) {
    } else if (OB_FAIL(top_k_heap_.pop())) {
    } else if (OB_FAIL(top_k_heap_.push(TopKItem(relevance, cache_idx)))) {
    } else {
      ++search_stats_.heap_replace_cnt_;
    }
  }

  return ret;
}

int ObSRBMWIterImpl::next_pivot(int64_t &pivot_iter_idx)
{
  int ret = OB_SUCCESS;

  const double top_k_threshold = get_top_k_threshold();
  int64_t iter_idx = 0;
  double max_score = 0.0;
  bool curr_id_end = false;
  bool found_pivot = false;
  while (OB_SUCC(ret) && !merge_heap_->empty() && !found_pivot) {
    const ObSRMergeItem *top_item = nullptr;
    double iter_score = 0.0;
    // make sure all dimensions containing pivot candidate are included in this iteration round
    curr_id_end = merge_heap_->is_unique_champion();
    if (OB_FAIL(merge_heap_->top(top_item))) {
    } else if (FALSE_IT(iter_idx = top_item->iter_idx_)) {
    } else if (OB_FAIL(get_iter(iter_idx)->get_dim_max_score(iter_score))) {
    } else if (FALSE_IT(max_score += iter_score)) {
    } else if (OB_FAIL(merge_heap_->pop())) {
    } else {
      next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
      found_pivot = max_score > top_k_threshold && curr_id_end;
    }
  }

  if (OB_FAIL(ret)) {
  } else if (max_score <= top_k_threshold) {
    ret = OB_ITER_END;
  } else {
    pivot_iter_idx = iter_idx;
  }
  return ret;
}

// this function should be called right after evaluate a false positive pivot range
int ObSRBMWIterImpl::next_pivot_range(int64_t &skip_range_cnt)
{
  int ret = OB_SUCCESS;
  /*
    Try to prune as much domain id ranges as possible.
    We will generate minimum prunable ranges to evaluate based on max score tuple and current ids.
    And then try to prune as much ranges as possible.

    When an unprunable range is found, it implies that there could be candidate pivot ids in range.
  */

  const ObDatum *max_evaluated_id = nullptr;
  const ObDatum *min_unevaluated_id = nullptr;
  bool last_border_inclusive = false;
  // find minimum max domain id
  const ObDatum *minimum_max_domain_id = nullptr;
  int64_t next_round_iter_end_cnt = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    const ObMaxScoreTuple *max_score_tuple = nullptr;
    int cmp_ret = 0;
    ObISRDimBlockMaxIter *iter = nullptr;
    if (is_next_round_iter_end(i)) {
      ++next_round_iter_end_cnt;
    } else if (OB_ISNULL(iter = get_iter(iter_idx)) || OB_UNLIKELY(!iter->in_shallow_status())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(iter->get_curr_block_max_info(max_score_tuple))) {
    } else if (OB_ISNULL(max_score_tuple)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (nullptr == minimum_max_domain_id) {
      minimum_max_domain_id = max_score_tuple->max_domain_id_;
    } else if (OB_FAIL(domain_id_cmp_.compare(*max_score_tuple->max_domain_id_, *minimum_max_domain_id, cmp_ret))) {
    } else if (0 < cmp_ret) {
      minimum_max_domain_id = max_score_tuple->max_domain_id_;
    }
  }

  // check if minimum max domain id might be included in other non-pivot dimensions
  if (OB_FAIL(ret)) {
  } else if (next_round_cnt_ == next_round_iter_end_cnt && merge_heap_->empty()) {
    ret = OB_ITER_END;
    next_round_cnt_ = 0;
  } else if (!merge_heap_->empty()) {
    const ObSRMergeItem *top_item = nullptr;
    int64_t iter_idx = 0;
    const ObDatum *next_domain_id = nullptr;
    int cmp_ret = 0;
    if (OB_FAIL(merge_heap_->top(top_item))) {
    } else if (FALSE_IT(iter_idx = top_item->iter_idx_)) {
    } else if (OB_FAIL(get_iter(iter_idx)->get_curr_id(next_domain_id))) {
    } else if (nullptr == minimum_max_domain_id) {
      max_evaluated_id = next_domain_id;
      last_border_inclusive = true;
    } else if (OB_FAIL(domain_id_cmp_.compare(*next_domain_id, *minimum_max_domain_id, cmp_ret))) {
    } else if (cmp_ret <= 0) {
      max_evaluated_id = next_domain_id;
      last_border_inclusive = true;
    } else {
      max_evaluated_id = minimum_max_domain_id;
      last_border_inclusive = false;
    }
  } else {
    max_evaluated_id = minimum_max_domain_id;
    last_border_inclusive = false;
  }

  // try to generate next candidate range
  bool is_candidate_range = false;
  while (OB_SUCC(ret) && !is_candidate_range) {
    if (OB_FAIL(fill_merge_heap_with_shallow_dims(max_evaluated_id, last_border_inclusive))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
      }
    } else if (OB_FAIL(try_generate_next_range_from_merge_heap(
        is_candidate_range, min_unevaluated_id, max_evaluated_id))) {
    } else {
      last_border_inclusive = false;
    }
    ++skip_range_cnt;
  }

  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  } else if (OB_FAIL(ret)) {
  } else if (OB_UNLIKELY(!is_candidate_range)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // found a new top k candidate range, fill heap with non-shallow dim iters for next pivot searching
    if (OB_ISNULL(min_unevaluated_id)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(advance_dim_iters_for_next_round(*min_unevaluated_id, false))) {
    }
  }

  return ret;
}

int ObSRBMWIterImpl::evaluate_pivot(const int64_t pivot_iter_idx)
{
  int ret = OB_SUCCESS;
  ObISRDimBlockMaxIter *iter = get_iter(pivot_iter_idx);
  const ObDatum *pivot_id = nullptr;
  const ObDatum *collected_id = nullptr;
  double pivot_relevance = 0.0;
  bool need_project = false;
  if (OB_ISNULL(iter)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(iter->get_curr_id(pivot_id))) {
  } else if (OB_FAIL(advance_dim_iters_for_next_round(*pivot_id, true))) {
  } else if (OB_FAIL(collect_dims_by_id(collected_id, pivot_relevance, need_project))) {
  } else if (OB_FAIL(unify_dim_iters_for_next_round())) {
  } else if (OB_FAIL(fill_merge_heap())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }

  return ret;
}

int ObSRBMWIterImpl::evaluate_pivot_range(const int64_t pivot_iter_idx, bool &is_candidate)
{
  int ret = OB_SUCCESS;
  ObISRDimBlockMaxIter *iter = get_iter(pivot_iter_idx);
  const ObDatum *pivot_id = nullptr;
  if (OB_ISNULL(iter)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(iter->get_curr_id(pivot_id))) {
  }

  // Cache only positive candidate checks. The common block interval and the
  // participating dimensions must remain unchanged, and the saved upper bound
  // must still exceed the (possibly increased) heap threshold. Other cases
  // use the existing shallow-advance path, including queries with >64 terms.
  uint64_t dimension_mask = 0;
  bool cacheable = next_round_cnt_ > 0 && dim_iters_->count() <= 64;
  for (int64_t i = 0; cacheable && i < next_round_cnt_; ++i) {
    const int64_t idx = next_round_iter_idxes_[i];
    if (idx < 0 || idx >= 64) {
      cacheable = false;
    } else {
      dimension_mask |= UINT64_C(1) << idx;
    }
  }
  if (OB_SUCC(ret) && cacheable && candidate_range_valid_
      && dimension_mask == candidate_range_mask_
      && candidate_range_score_ > get_top_k_threshold()) {
    int begin_cmp = 0;
    int end_cmp = 0;
    if (OB_FAIL(domain_id_cmp_.compare(*pivot_id, candidate_range_begin_.get_datum(), begin_cmp))) {
    } else if (OB_FAIL(domain_id_cmp_.compare(*pivot_id, candidate_range_end_.get_datum(), end_cmp))) {
    } else if (begin_cmp >= 0 && end_cmp <= 0) {
      is_candidate = true;
      ++search_stats_.range_cache_hit_cnt_;
      return ret;
    }
  }
  candidate_range_valid_ = false;
  double block_max_score = 0.0;
  const ObMaxScoreTuple *max_score_tuple = nullptr;
  const ObDatum *range_end = nullptr;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDimBlockMaxIter *iter = get_iter(iter_idx);
    if (OB_ISNULL(iter) || OB_UNLIKELY(is_next_round_iter_end(i))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(iter->advance_shallow(*pivot_id, true))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
      } else {
        set_next_round_iter_end(i);
        cacheable = false;
        ret = OB_SUCCESS;
      }
    } else if (OB_FAIL(iter->get_curr_block_max_info(max_score_tuple))) {
    } else {
      block_max_score += max_score_tuple->max_score_;
      if (cacheable) {
        int cmp = 0;
        if (OB_UNLIKELY(max_score_tuple->max_domain_id_->is_ext())) {
          cacheable = false;
        } else if (nullptr == range_end) {
          range_end = max_score_tuple->max_domain_id_;
        } else if (OB_FAIL(domain_id_cmp_.compare(*max_score_tuple->max_domain_id_, *range_end, cmp))) {
        } else if (cmp < 0) {
          range_end = max_score_tuple->max_domain_id_;
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    is_candidate = block_max_score > get_top_k_threshold();
    if (is_candidate && cacheable && nullptr != range_end && !pivot_id->is_ext()) {
      if (OB_FAIL(candidate_range_begin_.from_datum(*pivot_id))) {
      } else if (OB_FAIL(candidate_range_end_.from_datum(*range_end))) {
      } else {
        candidate_range_mask_ = dimension_mask;
        candidate_range_score_ = block_max_score;
        candidate_range_valid_ = true;
      }
    }
  }

  LOG_DEBUG("[Sparse Retrieval] BMW evaluate pivot range", K(ret),
      K(pivot_iter_idx), KPC(pivot_id), K(block_max_score), K(is_candidate), K(get_top_k_threshold()));

  return ret;
}


int ObSRBMWIterImpl::fill_merge_heap_with_shallow_dims(const ObDatum *last_range_border_id, const bool inclusive)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(last_range_border_id)) {
    ret = OB_ERR_UNEXPECTED;
  }

  ObSRMergeItem item;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDimBlockMaxIter *iter = nullptr;
    if (is_next_round_iter_end(i)) {
      // skip
    } else if (OB_ISNULL(iter = get_iter(iter_idx))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(iter->advance_shallow(*last_range_border_id, inclusive))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
      } else {
        ret = OB_SUCCESS;
      }
    } else if (OB_FAIL(iter->get_curr_id(iter_domain_ids_[iter_idx]))) {
    } else {
      item.iter_idx_ = iter_idx;
      item.relevance_ = 0.0;
      if (OB_FAIL(merge_heap_->push(item))) {
      }
    }
  }

  // rebuild merge heap
  if (OB_FAIL(ret)) {
  } else if (merge_heap_->empty()) {
    ret = OB_ITER_END;
  } else if (0 != next_round_cnt_ && OB_FAIL(merge_heap_->rebuild())) {
  } else {
    next_round_cnt_ = 0;
  }
  return ret;
}

int ObSRBMWIterImpl::try_generate_next_range_from_merge_heap(
    bool &is_candidate_range,
    const ObDatum *&min_domain_id_with_pivot,
    const ObDatum *&max_domain_id_without_pivot)
{
  int ret = OB_SUCCESS;

  is_candidate_range = false;
  min_domain_id_with_pivot = nullptr;
  max_domain_id_without_pivot = nullptr;

  // validate next candidate range
  const ObDatum *minimum_max_domain_id = nullptr;
  bool curr_range_reach_end = false;
  const double top_k_threshold = get_top_k_threshold();
  int64_t iter_idx = 0;
  double max_score = 0.0;
  while (OB_SUCC(ret) && max_score <= top_k_threshold && !curr_range_reach_end && !merge_heap_->empty()) {
    const ObSRMergeItem *top_item = nullptr;
    ObISRDimBlockMaxIter *iter = nullptr;
    const ObDatum *curr_domain_id = nullptr;
    if (OB_FAIL(merge_heap_->top(top_item))) {
    } else if (FALSE_IT(iter_idx = top_item->iter_idx_)) {
    } else if (OB_ISNULL(iter = get_iter(iter_idx))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (iter->in_shallow_status()) {
      // already in shallow status, no need to advance shallow
    } else if (OB_FAIL(iter->get_curr_id(curr_domain_id))) {
    } else if (OB_FAIL(iter->advance_shallow(*curr_domain_id, true))) {
    }

    const ObMaxScoreTuple *max_score_tuple = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(iter->get_curr_block_max_info(max_score_tuple))) {
    } else if (OB_ISNULL(max_score_tuple)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int cmp_ret = 0;
      if (nullptr == minimum_max_domain_id) {
        minimum_max_domain_id = max_score_tuple->max_domain_id_;
      } else if (OB_FAIL(domain_id_cmp_.compare(*max_score_tuple->min_domain_id_, *minimum_max_domain_id, cmp_ret))) {
      } else if (cmp_ret > 0) {
        // no intersected range with this block max iter
        curr_range_reach_end = true;
      } else if (OB_FAIL(domain_id_cmp_.compare(*max_score_tuple->max_domain_id_, *minimum_max_domain_id, cmp_ret))) {
      } else if (0 < cmp_ret) {
        minimum_max_domain_id = max_score_tuple->max_domain_id_;
      }

      if (OB_FAIL(ret) || curr_range_reach_end) {
      } else if (OB_FAIL(merge_heap_->pop())) {
      } else {
        next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
        max_score += max_score_tuple->max_score_;
        if (max_score > top_k_threshold) {
          // found a new top k candidate range
          is_candidate_range = true;
          min_domain_id_with_pivot = max_score_tuple->min_domain_id_;
        }
      }
    }
  }

  if (OB_SUCC(ret) && !is_candidate_range) {
    max_domain_id_without_pivot = minimum_max_domain_id;
  }

  return ret;
}

int ObSRBMWIterImpl::project_rows_from_top_k_heap(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(BMWStatus::FINISHED != status_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(top_k_heap_.empty())) {
    ret = OB_ITER_END;
  } else if (OB_UNLIKELY(capacity < 0 || capacity > buffered_domain_ids_.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
    ObExpr *id_proj_expr = iter_param_->id_proj_expr_;
    ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
    sql::ObBitVector &id_evaluated_flags = id_proj_expr->get_evaluated_flags(*eval_ctx);
    sql::ObBitVector *relevance_evaluated_flags = nullptr;
    ObDatum *id_datums = id_proj_expr->locate_datums_for_update(*eval_ctx, capacity);
    ObDatum *relevance_datums = nullptr;
    if (iter_param_->need_project_relevance()) {
      relevance_datums = relevance_proj_expr->locate_datums_for_update(*eval_ctx, capacity);
      relevance_evaluated_flags = &relevance_proj_expr->get_evaluated_flags(*eval_ctx);
    }

    ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
    count = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < capacity && !top_k_heap_.empty(); ++i) {
      guard.set_batch_idx(i);
      const TopKItem &top_k_item = top_k_heap_.top();
      const ObDocIdExt &id = id_cache_.at(top_k_item.cache_idx_);
      set_datum_func_(id_datums[i], id);
      id_evaluated_flags.set(i);
      id_proj_expr->set_evaluated_projected(*eval_ctx);

      if (iter_param_->need_project_relevance()) {
        relevance_datums[i].set_double(top_k_item.relevance_);
        relevance_evaluated_flags->set(i);
        relevance_proj_expr->set_evaluated_projected(*eval_ctx);
      }
      ++count;
      if (OB_FAIL(top_k_heap_.pop())) {
      }
    }
  }
  return ret;
}

int ObSRBMWIterImpl::unify_dim_iters_for_next_round()
{
  int ret = OB_SUCCESS;

  ObDatum *curr_domain_id = nullptr;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDimBlockMaxIter *iter = get_iter(iter_idx);
    const ObDatum *curr_domain_id = nullptr;
    if (OB_ISNULL(iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!iter->in_shallow_status()) {
      // skip
    } else if (OB_FAIL(iter->get_curr_id(curr_domain_id))) {
    } else if (OB_FAIL(iter->advance_to(*curr_domain_id))) {
    }
  }

  return ret;
}

int ObSRBMWIterImpl::advance_dim_iters_for_next_round(
    const ObDatum &target_id,
    const bool iter_end_available)
{
  int ret = OB_SUCCESS;
  ObSRMergeItem item;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDimBlockMaxIter *iter = nullptr;
    if (is_next_round_iter_end(i)) {
      if (!iter_end_available) {
        ret = OB_ERR_UNEXPECTED;
      }
    } else if (OB_ISNULL(iter = get_iter(iter_idx))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(iter->advance_to(target_id))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
      } else {
        ret = OB_SUCCESS;
      }
    } else if (OB_FAIL(iter->get_curr_id(iter_domain_ids_[iter_idx]))) {
    } else if (OB_FAIL(iter->get_curr_score(item.relevance_))) {
    } else if (FALSE_IT(item.iter_idx_ = iter_idx)) {
    } else if (OB_FAIL(merge_heap_->push(item))) {
    }
  }

  if (OB_FAIL(ret)) {
  } else if (merge_heap_->empty()) {
    if (iter_end_available) {
      ret = OB_ITER_END;
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (0 != next_round_cnt_ && OB_FAIL(merge_heap_->rebuild())) {
  } else {
    next_round_cnt_ = 0;
  }

  return ret;
}



} // namespace storage
} // namespace oceanbase
