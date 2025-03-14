//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2016 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/importer.hpp"

#include "tenzir/fwd.hpp"

#include "tenzir/atoms.hpp"
#include "tenzir/concept/printable/numeric/integral.hpp"
#include "tenzir/concept/printable/std/chrono.hpp"
#include "tenzir/concept/printable/tenzir/error.hpp"
#include "tenzir/concept/printable/to_string.hpp"
#include "tenzir/defaults.hpp"
#include "tenzir/detail/fill_status_map.hpp"
#include "tenzir/detail/shutdown_stream_stage.hpp"
#include "tenzir/detail/weak_run_delayed.hpp"
#include "tenzir/error.hpp"
#include "tenzir/logger.hpp"
#include "tenzir/plugin.hpp"
#include "tenzir/report.hpp"
#include "tenzir/si_literals.hpp"
#include "tenzir/status.hpp"
#include "tenzir/table_slice.hpp"
#include "tenzir/uuid.hpp"

#include <caf/config_value.hpp>
#include <caf/detail/stream_stage_impl.hpp>
#include <caf/settings.hpp>
#include <caf/stream_stage_driver.hpp>

#include <filesystem>
#include <fstream>

namespace tenzir {

namespace {

class driver : public caf::stream_stage_driver<
                 table_slice, caf::broadcast_downstream_manager<table_slice>> {
public:
  driver(caf::broadcast_downstream_manager<table_slice>& out,
         importer_state& state)
    : stream_stage_driver(out), state{state} {
    // nop
  }

  void process(caf::downstream<table_slice>& out,
               std::vector<table_slice>& slices) override {
    TENZIR_TRACE_SCOPE("{}", TENZIR_ARG(slices));
    uint64_t events = 0;
    auto t = timer::start(state.measurement_);
    for (auto&& slice : std::exchange(slices, {})) {
      auto rows = slice.rows();
      events += rows;
      auto name = slice.schema().name();
      if (auto it = state.schema_counters.find(name);
          it != state.schema_counters.end())
        it.value() += rows;
      else
        state.schema_counters.emplace(std::string{name}, rows);
      slice.import_time(time::clock::now());
      out.push(std::move(slice));
    }
    t.stop(events);
  }

  void finalize(const caf::error& err) override {
    TENZIR_DEBUG("{} stopped with message: {}", *state.self, render(err));
  }

  importer_state& state;
};

class stream_stage : public caf::detail::stream_stage_impl<driver> {
public:
  /// Constructs the import stream stage.
  /// @note This must explictly initialize the stream_manager because it does
  /// not provide a default constructor, and for reason unbeknownst to me the
  /// forwaring in the stream_stage_impl does not suffice.
  stream_stage(importer_actor::stateful_pointer<importer_state> self)
    : stream_manager(self), stream_stage_impl(self, self->state) {
    // nop
  }

  void register_input_path(caf::inbound_path* ptr) override {
    driver_.state.inbound_descriptions[ptr]
      = std::exchange(driver_.state.inbound_description, "anonymous");
    TENZIR_INFO("{} adds {} source", *driver_.state.self,
                driver_.state.inbound_descriptions[ptr]);
    super::register_input_path(ptr);
  }

  void deregister_input_path(caf::inbound_path* ptr) noexcept override {
    if ((flags_ & (is_stopped_flag | is_shutting_down_flag)) == 0) {
      TENZIR_INFO("{} removes {} source", *driver_.state.self,
                  driver_.state.inbound_descriptions[ptr]);
      driver_.state.inbound_descriptions.erase(ptr);
    }
    super::deregister_input_path(ptr);
  }
};

caf::intrusive_ptr<stream_stage>
make_importer_stage(importer_actor::stateful_pointer<importer_state> self) {
  auto result = caf::make_counted<stream_stage>(self);
  result->continuous(true);
  return result;
}

} // namespace

importer_state::importer_state(importer_actor::pointer self) : self{self} {
  // nop
}

importer_state::~importer_state() = default;

caf::typed_response_promise<record>
importer_state::status(status_verbosity v) const {
  auto rs = make_status_request_state(self);
  // Gather general importer status.
  // TODO: caf::config_value can only represent signed 64 bit integers, which
  // may make it look like overflow happened in the status report. As an
  // intermediate workaround, we convert the values to strings.
  record result;
  if (v >= status_verbosity::detailed) {
    auto sources_status = list{};
    sources_status.reserve(inbound_descriptions.size());
    for (const auto& kv : inbound_descriptions)
      sources_status.emplace_back(kv.second);
    rs->content["sources"] = std::move(sources_status);
  }
  // General state such as open streams.
  if (v >= status_verbosity::debug)
    detail::fill_status_map(rs->content, self);
  return rs->promise;
}

void importer_state::send_report() {
  auto now = stopwatch::now();
  using namespace std::string_literals;
  auto elapsed = std::chrono::duration_cast<duration>(now - last_report);
  auto node_throughput = measurement{elapsed, measurement_.events};
  auto num_schemas_seen = schema_counters.size();
  std::vector<performance_sample> samples = {};
  samples.reserve(num_schemas_seen);
  samples.push_back(performance_sample{"importer"s, measurement_});
  samples.push_back(performance_sample{"node_throughput"s, node_throughput});
  auto total_count = uint64_t{0};
  for (const auto& [name, count] : schema_counters) {
    total_count += count;
    samples.push_back(performance_sample{
      "ingest", measurement{elapsed, count}, {{"schema", name}}});
  }
  auto total_measurement = measurement{elapsed, total_count};
  samples.push_back(performance_sample{"ingest-total"s, total_measurement});
  schema_counters.clear();
  auto r = performance_report{
    .data = std::move(samples),
  };
#if TENZIR_LOG_LEVEL >= TENZIR_LOG_LEVEL_VERBOSE
  auto beat = [&](const auto& sample) {
    if (sample.value.events > 0) {
      if (auto rate = sample.value.rate_per_sec(); std::isfinite(rate))
        TENZIR_VERBOSE("{} handled {} events at a rate of {} events/sec in {}",
                       *self, sample.value.events, static_cast<uint64_t>(rate),
                       to_string(sample.value.duration));
      else
        TENZIR_VERBOSE("{} handled {} events in {}", *self, sample.value.events,
                       to_string(sample.value.duration));
    }
  };
  beat(r.data[1]);
#endif
  measurement_ = measurement{};
  self->send(accountant, atom::metrics_v, std::move(r));
  last_report = now;
}

importer_actor::behavior_type
importer(importer_actor::stateful_pointer<importer_state> self,
         const std::filesystem::path& dir, index_actor index,
         accountant_actor accountant) {
  TENZIR_TRACE_SCOPE("importer {} {}", TENZIR_ARG(self->id()), TENZIR_ARG(dir));
  if (auto ec = std::error_code{};
      std::filesystem::exists(dir / "current_id_block", ec))
    std::filesystem::remove(dir / "current_id_block", ec);
  namespace defs = defaults;
  self->set_exit_handler([=](const caf::exit_msg& msg) {
    self->state.send_report();
    for (auto* inbound : self->state.stage->inbound_paths()) {
      self->send_exit(inbound->hdl, msg.reason);
    }
    self->quit(msg.reason);
  });
  self->state.stage = make_importer_stage(self);
  if (index) {
    self->state.index = std::move(index);
    self->state.stage->add_outbound_path(self->state.index);
  }
  if (accountant) {
    TENZIR_DEBUG("{} registers accountant {}", *self, accountant);
    self->state.accountant = std::move(accountant);
    self->send(self->state.accountant, atom::announce_v, self->name());
    detail::weak_run_delayed_loop(self, defaults::telemetry_rate, [self] {
      self->state.send_report();
    });
  }
  return {
    // Add a new sink.
    [self](stream_sink_actor<table_slice> sink) {
      TENZIR_DEBUG("{} adds a new sink: {}", *self, sink);
      return self->state.stage->add_outbound_path(sink);
    },
    // Register a FLUSH LISTENER actor.
    [self](atom::subscribe, atom::flush, flush_listener_actor listener) {
      TENZIR_DEBUG("{} adds new subscriber {}", *self, listener);
      TENZIR_ASSERT(self->state.stage != nullptr);
      self->send(self->state.index, atom::subscribe_v, atom::flush_v,
                 std::move(listener));
    },
    // -- stream_sink_actor<table_slice> ---------------------------------------
    [self](caf::stream<table_slice> in) {
      // NOTE: Architecturally it would make more sense to put the transformer
      // stage *before* the import actor, but that is not possible: The message
      // sent is originally sent from the other side is a `caf::open_stream_msg`.
      // This contains a field `msg` with a `caf::stream<>`. The caf streaming
      // system recognizes this message and only passes the `caf::stream<>` to
      // the handler. This means we can not delegate() this message, since we
      // would only create a new message containing a `caf::stream` object but
      // lose the surrounding `open_stream_msg` which contains the important
      // parts. Sadly, the current actor is already stored as the "other side"
      // of the stream in the outbound path, so we can't even hack around this
      // with `caf::unsafe_send_as()` or similar black magic.
      TENZIR_DEBUG("{} adds a new source", *self);
      return self->state.stage->add_inbound_path(in);
    },
    // -- stream_sink_actor<table_slice, std::string> --------------------------
    [self](caf::stream<table_slice> in, std::string desc) {
      self->state.inbound_description = std::move(desc);
      TENZIR_DEBUG("{} adds a new {} source", *self, desc);
      return self->state.stage->add_inbound_path(in);
    },
    // -- status_client_actor --------------------------------------------------
    [self](atom::status, status_verbosity v, duration) { //
      return self->state.status(v);
    },
  };
}

} // namespace tenzir
