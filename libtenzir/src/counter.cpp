//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2019 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/counter.hpp"

#include "tenzir/bitmap_algorithms.hpp"
#include "tenzir/logger.hpp"
#include "tenzir/query_context.hpp"
#include "tenzir/table_slice.hpp"

#include <caf/event_based_actor.hpp>

namespace tenzir {

counter_state::counter_state(caf::event_based_actor* self) : super(self) {
  // nop
}

void counter_state::init(expression expr, index_actor index,
                         bool skip_candidate_check) {
  auto query_context
    = tenzir::query_context::make_count("count", self_,
                                        skip_candidate_check
                                          ? count_query_context::estimate
                                          : count_query_context::exact,
                                        std::move(expr));
  // Transition from idle state when receiving 'run' and client handle.
  behaviors_[idle].assign([=, this, query_context = std::move(query_context)](
                            atom::run, caf::actor client) {
    client_ = std::move(client);
    start(query_context, index);
    // Stop immediately when losing the client.
    self_->monitor(client_);
    self_->set_down_handler([this](caf::down_msg& dm) {
      if (dm.source == client_)
        self_->quit(dm.reason);
    });
  });
  caf::message_handler base{
    behaviors_[await_results_until_done].as_behavior_impl()};
  behaviors_[await_results_until_done] = base.or_else(
    // Forward results to the sink.
    [this](uint64_t num_results) {
      self_->send(client_, num_results);
    });
}

void counter_state::process_done() {
  if (!request_more_results()) {
    self_->send(client_, atom::done_v);
    self_->quit();
  }
}

caf::behavior counter(caf::stateful_actor<counter_state>* self, expression expr,
                      index_actor index, bool skip_candidate_check) {
  auto normalized_expr = normalize_and_validate(std::move(expr));
  if (!normalized_expr) {
    self->quit(caf::make_error(ec::format_error,
                               fmt::format("{} failed to normalize and "
                                           "validate expression: {}",
                                           *self, normalized_expr.error())));
    return {};
  }
  self->state.init(std::move(*normalized_expr), std::move(index),
                   skip_candidate_check);
  return self->state.behavior();
}

} // namespace tenzir
