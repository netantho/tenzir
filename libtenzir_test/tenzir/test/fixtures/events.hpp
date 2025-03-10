//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2018 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tenzir/fwd.hpp"

#include "tenzir/data.hpp"
#include "tenzir/defaults.hpp"
#include "tenzir/error.hpp"
#include "tenzir/table_slice.hpp"
#include "tenzir/test/data.hpp"
#include "tenzir/test/test.hpp"

namespace fixtures {

using namespace tenzir;

struct events {
  events();

  /// Maximum size of all generated slices.
  static constexpr size_t slice_size = 8;

  // TODO: remove these entirely; all operations should be on table slices.
  static std::vector<table_slice> zeek_conn_log;
  static std::vector<table_slice> zeek_dns_log;
  static std::vector<table_slice> zeek_http_log;
  static std::vector<table_slice> random;

  static std::vector<table_slice> suricata_alert_log;
  static std::vector<table_slice> suricata_dns_log;
  static std::vector<table_slice> suricata_fileinfo_log;
  static std::vector<table_slice> suricata_flow_log;
  static std::vector<table_slice> suricata_http_log;
  static std::vector<table_slice> suricata_netflow_log;
  static std::vector<table_slice> suricata_stats_log;

  static tenzir::module suricata_module;

  static std::vector<table_slice> zeek_conn_log_full;

  /// 10000 ascending integer values, starting at 0.
  static std::vector<table_slice> ascending_integers;

  /// 10000 integer values, alternating between 0 and 1.
  static std::vector<table_slice> alternating_integers;

  template <class... Ts>
  static std::vector<std::vector<data>> make_rows(Ts... xs) {
    return {std::vector<data>{data{std::move(xs)}}...};
  }

  auto take(const std::vector<table_slice>& xs, size_t n) {
    TENZIR_ASSERT(n <= xs.size());
    auto first = xs.begin();
    auto last = first + n;
    return std::vector<table_slice>(first, last);
  }
};

} // namespace fixtures
