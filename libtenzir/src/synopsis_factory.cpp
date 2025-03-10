//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2019 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/synopsis_factory.hpp"

#include "tenzir/bool_synopsis.hpp"
#include "tenzir/hash/hash_append.hpp"
#include "tenzir/hash/legacy_hash.hpp"
#include "tenzir/ip_synopsis.hpp"
#include "tenzir/string_synopsis.hpp"
#include "tenzir/time_synopsis.hpp"

namespace tenzir {

void factory_traits<synopsis>::initialize() {
  factory<synopsis>::add(type{ip_type{}}, make_ip_synopsis<legacy_hash>);
  factory<synopsis>::add<bool_type, bool_synopsis>();
  factory<synopsis>::add(type{string_type{}},
                         make_string_synopsis<legacy_hash>);
  factory<synopsis>::add<time_type, time_synopsis>();
}

} // namespace tenzir
