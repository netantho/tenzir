//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2021 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vast/offset.hpp"

namespace vast {

std::strong_ordering
operator<=>(const offset& lhs, const offset& rhs) noexcept {
  if (&lhs == &rhs)
    return std::strong_ordering::equal;
  const auto [lhs_mismatch, rhs_mismatch]
    = std::mismatch(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
  const auto lhs_exhausted = lhs_mismatch == lhs.end();
  const auto rhs_exhausted = rhs_mismatch == rhs.end();
  if (lhs_exhausted && rhs_exhausted)
    return std::strong_ordering::equivalent;
  if (lhs_exhausted)
    return std::strong_ordering::less;
  if (rhs_exhausted)
    return std::strong_ordering::greater;
  return *lhs_mismatch < *rhs_mismatch ? std::strong_ordering::less
                                       : std::strong_ordering::greater;
}

} // namespace vast
