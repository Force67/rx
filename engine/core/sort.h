#ifndef RX_CORE_SORT_H_
#define RX_CORE_SORT_H_

#include "base/algorithm.h"
#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "core/types.h"

namespace rx {

// std::stable_sort: equal elements keep their input order. base::Sort breaks
// ties by its own rules; where the order of equal elements reaches output (draw
// order, which lights win a cut), keeping input order makes the result a
// function of the input alone rather than of any one sort algorithm.
template <typename T, typename Less>
void StableSort(T* first, T* last, Less less) {
  const mem_size n = static_cast<mem_size>(last - first);
  if (n < 2) return;
  base::Vector<u32> order(n);
  for (mem_size i = 0; i < n; ++i) order[i] = static_cast<u32>(i);
  // Position breaks every tie, so any correct sort of the indices agrees.
  base::Sort(order.data(), order.data() + n, [&](u32 a, u32 b) {
    if (less(first[a], first[b])) return true;
    if (less(first[b], first[a])) return false;
    return a < b;
  });
  base::Vector<T> sorted;
  sorted.reserve(n);
  for (u32 index : order) sorted.push_back(base::move(first[index]));
  for (mem_size i = 0; i < n; ++i) first[i] = base::move(sorted[i]);
}

// std::nth_element: *nth ends up where a full sort would put it, nothing after
// it is less, nothing before it greater. Callers that need a reproducible
// split must hand in a total order (ties broken by a unique id): then which
// elements land on each side is fixed by the input, whatever the algorithm.
template <typename T, typename Less>
void NthElement(T* first, T* nth, T* last, Less less) {
  while (last - first > 16) {
    // Median of three moved to the front as the pivot.
    T* mid = first + (last - first) / 2;
    T* back = last - 1;
    if (less(*mid, *first)) base::Swap(*mid, *first);
    if (less(*back, *first)) base::Swap(*back, *first);
    if (less(*back, *mid)) base::Swap(*back, *mid);
    base::Swap(*first, *mid);
    // Hoare partition around *first.
    T* lo = first + 1;
    T* hi = last - 1;
    for (;;) {
      while (lo <= hi && less(*lo, *first)) ++lo;
      while (lo <= hi && less(*first, *hi)) --hi;
      if (lo >= hi) break;
      base::Swap(*lo, *hi);
      ++lo;
      --hi;
    }
    base::Swap(*first, *hi);
    if (hi == nth) return;
    if (nth < hi) {
      last = hi;
    } else {
      first = hi + 1;
    }
  }
  // Short ranges: insertion sort, which leaves every position final.
  for (T* i = first + 1; i < last; ++i) {
    for (T* j = i; j > first && less(*j, *(j - 1)); --j) base::Swap(*j, *(j - 1));
  }
}

}  // namespace rx

#endif  // RX_CORE_SORT_H_
