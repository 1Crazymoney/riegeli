// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef RIEGELI_CSV_CONTAINERS_H_
#define RIEGELI_CSV_CONTAINERS_H_

#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "absl/strings/string_view.h"

namespace riegeli {
namespace internal {

namespace adl_begin_sandbox {

using std::begin;

template <typename T>
using DereferenceIterableT = decltype(*begin(std::declval<T&>()));

}  // namespace adl_begin_sandbox

// `IsIterableOf<Iterable, Element>::value` is `true` if iterating over
// `Iterable` yields elements convertible to `Element`.
template <typename Iterable, typename Element, typename Enable = void>
struct IsIterableOf : public std::false_type {};

template <typename Iterable, typename Element>
struct IsIterableOf<
    Iterable, Element,
    std::enable_if_t<std::is_convertible<
        adl_begin_sandbox::DereferenceIterableT<Iterable>, Element>::value>>
    : public std::true_type {};

// `HasMovableElements<Iterable>::value` is `true` if moving (rather than
// copying) out of elements of `Iterable` is safe.
template <typename Iterable, typename Enable = void>
struct HasMovableElements : public std::false_type {};

// Moving out of elements of `Iterable` is unsafe if it is an lvalue, or a view
// container like `absl::Span<T>`. View containers are detected by checking
// whether iterating over `Iterable&` and `const Iterable&` yields elements of
// the same type. This also catches cases where `Iterable` always yields const
// elements, where moving would be ineffective anyway.
template <typename Iterable>
struct HasMovableElements<
    Iterable,
    std::enable_if_t<!std::is_lvalue_reference<Iterable>::value &&
                     !std::is_same_v<adl_begin_sandbox::DereferenceIterableT<
                                         std::decay_t<Iterable>>,
                                     adl_begin_sandbox::DereferenceIterableT<
                                         const std::decay_t<Iterable>>>>>
    : public std::true_type {};

// `MaybeMakeMoveIterator<Src>(iterator)` is `std::make_move_iterator(iterator)`
// or `iterator`, depending on whether moving out of elements of `Src` is safe.

template <typename Src, typename Iterator,
          std::enable_if_t<!HasMovableElements<Src>::value, int> = 0>
inline Iterator MaybeMakeMoveIterator(Iterator iterator) {
  return iterator;
}

template <typename Src, typename Iterator,
          std::enable_if_t<HasMovableElements<Src>::value, int> = 0>
inline std::move_iterator<Iterator> MaybeMakeMoveIterator(Iterator iterator) {
  return std::move_iterator<Iterator>(iterator);
}

// `ToVectorOfStrings()` converts an iterable of elements convertible to
// `absl::string_view` to `std::vector<std::string>`.

template <
    typename Values,
    std::enable_if_t<IsIterableOf<Values, absl::string_view>::value, int> = 0>
std::vector<std::string> ToVectorOfStrings(Values&& values) {
  using std::begin;
  using std::end;
  return std::vector<std::string>(MaybeMakeMoveIterator<Values>(begin(values)),
                                  MaybeMakeMoveIterator<Values>(end(values)));
}

}  // namespace internal
}  // namespace riegeli

#endif  // RIEGELI_CSV_CONTAINERS_H_
