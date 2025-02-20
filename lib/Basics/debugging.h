////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdlib.h>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "Basics/system-compiler.h"
#include "CrashHandler/CrashHandler.h"

/// @brief macro TRI_IF_FAILURE
/// this macro can be used in maintainer mode to make the server fail at
/// certain locations in the C code. The points at which a failure is actually
/// triggered can be defined at runtime using TRI_AddFailurePointDebugging().
#ifdef ARANGODB_ENABLE_FAILURE_TESTS

#define TRI_IF_FAILURE(what) if (TRI_ShouldFailDebugging(what))

#else

#define TRI_IF_FAILURE(what) if constexpr (false)

#endif

namespace arangodb::velocypack {
class Builder;
}

/// @brief intentionally cause a segmentation violation or other failures
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
void TRI_TerminateDebugging(std::string_view value);
#else
inline void TRI_TerminateDebugging(std::string_view) {}
#endif

/// @brief check whether we should fail at a failure point
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
bool TRI_ShouldFailDebugging(std::string_view value) noexcept;
#else
inline constexpr bool TRI_ShouldFailDebugging(std::string_view) noexcept {
  return false;
}
#endif

/// @brief add a failure point
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
void TRI_AddFailurePointDebugging(std::string_view value);
#else
inline void TRI_AddFailurePointDebugging(std::string_view) {}
#endif

/// @brief remove a failure point
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
void TRI_RemoveFailurePointDebugging(std::string_view value);
#else
inline void TRI_RemoveFailurePointDebugging(std::string_view) {}
#endif

/// @brief clear all failure points
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
void TRI_ClearFailurePointsDebugging() noexcept;
#else
inline void TRI_ClearFailurePointsDebugging() noexcept {}
#endif

/// @brief return all currently set failure points
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
void TRI_GetFailurePointsDebugging(arangodb::velocypack::Builder& builder);
#else
inline void TRI_GetFailurePointsDebugging(arangodb::velocypack::Builder&) {}
#endif

/// @brief returns whether failure point debugging can be used
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
inline constexpr bool TRI_CanUseFailurePointsDebugging() { return true; }
#else
inline constexpr bool TRI_CanUseFailurePointsDebugging() { return false; }
#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief container traits
////////////////////////////////////////////////////////////////////////////////

namespace container_traits {

using tc = char[2];

template<typename T>
struct is_container {
  static tc& test(...);

  template<typename U>
  static char test(U&&, decltype(std::begin(std::declval<U>()))* = 0);
  static constexpr bool value = sizeof(test(std::declval<T>())) == 1;
};

template<class T>
inline constexpr bool is_container_v = is_container<T>::value;

template<typename T>
struct is_associative {
  static tc& test(...);

  template<typename U>
  static char test(U&&, typename U::key_type* = 0);

  static constexpr bool value = sizeof(test(std::declval<T>())) == 1;
};

}  // namespace container_traits

namespace arangodb {

template<class T>
struct remove_cvref {
  typedef std::remove_cv_t<std::remove_reference_t<T>> type;
};
template<class T>
using remove_cvref_t = typename remove_cvref<T>::type;

template<typename>
struct is_basic_string : std::false_type {};
template<typename C, typename T, typename A>
struct is_basic_string<std::basic_string<C, T, A>> : std::true_type {};

template<typename T>
struct is_container
    : std::conditional<
          (container_traits::is_container<T>::value ||
           std::is_array<T>::value) &&
              !std::is_same_v<char*, typename std::decay<T>::type> &&
              !std::is_same_v<char const*, typename std::decay<T>::type> &&
              !std::is_same_v<unsigned char*, typename std::decay<T>::type> &&
              !std::is_same_v<unsigned char const*,
                              typename std::decay<T>::type> &&
              !is_basic_string<T>::value &&
              !std::is_same_v<T, std::string_view> &&
              !std::is_same_v<T, const std::string>,
          std::true_type, std::false_type>::type {};

template<typename T>
struct is_associative
    : std::conditional<container_traits::is_container<T>::value &&
                           container_traits::is_associative<T>::value,
                       std::true_type, std::false_type>::type {};

////////////////////////////////////////////////////////////////////////////////
/// @brief forward declaration for pair output below
////////////////////////////////////////////////////////////////////////////////

// TODO These operators cannot generally be found by ADL. It often works by
// luck,
//      e.g. because T is in the arangodb namespace, or one of its template
//      parameters is.
//      The straight-forward solution is to have it in the same namespace as
//      one of its argument. Currently that's std, to which we must not add
//      this. So instead we should add a `struct ::arangodb::StreamRef {
//      std::ostream& stream; }` and use that as its argument and return value.
//      Our macros (like TRI_ASSERT or LOG_TOPIC) should then return such a
//      StreamRef.
//      For convenience, it should handle a LoggerStreamBase instead of a
//      StreamRef as well.
template<typename T>
std::enable_if_t<is_container<T>::value, std::ostream&> operator<<(
    std::ostream& o, T const& t);

////////////////////////////////////////////////////////////////////////////////
/// @brief dump pair contents to an ostream
////////////////////////////////////////////////////////////////////////////////

template<typename T1, typename T2>
std::ostream& operator<<(std::ostream& stream, std::pair<T1, T2> const& obj) {
  stream << '(' << obj.first << ", " << obj.second << ')';
  return stream;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief dump vector contents to an ostream
////////////////////////////////////////////////////////////////////////////////

template<bool b>
struct conpar {
  static char const open;
  static char const close;
};

template<bool B, class T = void>
using enable_if_t = typename std::enable_if<B, T>::type;

template<typename T>
enable_if_t<is_container<T>::value, std::ostream&> operator<<(std::ostream& o,
                                                              T const& t) {
  o << conpar<is_associative<T>::value>::open;
  bool first = true;
  for (auto const& i : t) {
    if (first) {
      o << " ";
      first = false;
    } else {
      o << ", ";
    }
    o << i;
  }
  o << " " << conpar<is_associative<T>::value>::close;
  return o;
}
}  // namespace arangodb

#include "Assertions/Assert.h"
#include "Assertions/ProdAssert.h"
