//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2021 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include <tenzir/argument_parser.hpp>
#include <tenzir/concept/parseable/string/char_class.hpp>
#include <tenzir/concept/parseable/tenzir/pipeline.hpp>
#include <tenzir/error.hpp>
#include <tenzir/logger.hpp>
#include <tenzir/pipeline.hpp>
#include <tenzir/plugin.hpp>

#include <arrow/type.h>

namespace tenzir::plugins::pass {

namespace {

// Does nothing with the input.
class pass_operator final : public crtp_operator<pass_operator> {
public:
  template <operator_input_batch T>
  auto operator()(T x) const -> T {
    return x;
  }

  auto name() const -> std::string override {
    return "pass";
  }

  auto optimize(expression const& filter, event_order order) const
    -> optimize_result override {
    return optimize_result{filter, order, nullptr};
  }

  friend auto inspect(auto& f, pass_operator& x) -> bool {
    (void)f, (void)x;
    return true;
  }
};

class plugin final : public virtual operator_plugin<pass_operator> {
public:
  auto signature() const -> operator_signature override {
    return {.transformation = true};
  }

  auto parse_operator(parser_interface& p) const -> operator_ptr override {
    argument_parser{"pass", "https://docs.tenzir.com/next/operators/"
                            "transformations/pass"}
      .parse(p);
    return std::make_unique<pass_operator>();
  }
};

} // namespace

} // namespace tenzir::plugins::pass

TENZIR_REGISTER_PLUGIN(tenzir::plugins::pass::plugin)
