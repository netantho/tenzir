//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2023 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include <tenzir/connect_to_node.hpp>
#include <tenzir/detail/load_contents.hpp>
#include <tenzir/diagnostics.hpp>
#include <tenzir/logger.hpp>
#include <tenzir/pipeline.hpp>
#include <tenzir/pipeline_executor.hpp>
#include <tenzir/plugin.hpp>
#include <tenzir/tql/parser.hpp>

#include <caf/detail/stringification_inspector.hpp>
#include <caf/json_writer.hpp>
#include <caf/scoped_actor.hpp>

namespace tenzir::plugins::exec {

namespace {

struct exec_config {
  bool no_implicit = false;
  bool dump_ast = false;
  bool dump_diagnostics = false;
  bool dump_metrics = false;
};

auto add_implicit_source_and_sink(pipeline pipe) -> caf::expected<pipeline> {
  if (pipe.infer_type<void>()) {
    // Don't add implicit source.
  } else if (pipe.infer_type<chunk_ptr>()) {
    auto op = pipeline::internal_parse_as_operator("load file -");
    if (not op) {
      return caf::make_error(ec::logic_error,
                             fmt::format("failed to prepend implicit "
                                         "'load file -': {}",
                                         op.error()));
    }
    pipe.prepend(std::move(*op));
  } else if (pipe.infer_type<table_slice>()) {
    auto op = pipeline::internal_parse_as_operator("from stdin read json");
    if (not op) {
      return caf::make_error(ec::logic_error,
                             fmt::format("failed to prepend implicit "
                                         "'from stdin read json': {}",
                                         op.error()));
    }
    pipe.prepend(std::move(*op));
  } else {
    // Pipeline is ill-typed. We don't add implicit source or sink and continue,
    // as this is handled further down the line.
    return pipe;
  }
  auto out = pipe.infer_type<void>();
  if (not out) {
    return caf::make_error(ec::logic_error,
                           fmt::format("expected pipeline to accept void here, "
                                       "but: {}",
                                       out.error()));
  }
  if (out->is<void>()) {
    // Pipeline is already closed, nothing to do here.
  } else if (out->is<chunk_ptr>()) {
    auto op = pipeline::internal_parse_as_operator("save file -");
    if (not op) {
      return caf::make_error(ec::logic_error,
                             fmt::format("failed to append implicit "
                                         "'save file -': {}",
                                         op.error()));
    }
    pipe.append(std::move(*op));
  } else if (out->is<table_slice>()) {
    auto op = pipeline::internal_parse_as_operator("to stdout write json");
    if (not op) {
      return caf::make_error(ec::logic_error,
                             fmt::format("failed to append implicit 'to stdout "
                                         "write json': {}",
                                         op.error()));
    }
    pipe.append(std::move(*op));
  }
  if (not pipe.is_closed()) {
    return caf::make_error(ec::logic_error,
                           fmt::format("expected pipeline to be closed after "
                                       "adding implicit source and sink"));
  }
  return pipe;
}

auto format_metric(const metric& metric) {
  auto result = std::string{};
  auto it = std::back_inserter(result);
  const auto locale = std::locale("en_US.UTF-8");
  constexpr auto indent = std::string_view{"  "};
  it = fmt::format_to(it, "operator #{} ({})\n", metric.operator_index + 1,
                      metric.operator_name);
  it = fmt::format_to(it, "{}total: {}\n", indent, data{metric.time_total});
  it = fmt::format_to(it, "{}scheduled: {} ({:.2f}%)\n", indent,
                      data{metric.time_scheduled},
                      100.0 * static_cast<double>(metric.time_scheduled.count())
                        / static_cast<double>(metric.time_total.count()));
  it
    = fmt::format_to(it, "{}processing: {} ({:.2f}%)\n", indent,
                     data{metric.time_processing},
                     100.0 * static_cast<double>(metric.time_processing.count())
                       / static_cast<double>(metric.time_total.count()));
  it = fmt::format_to(
    it, "{}runs: {} ({:.2f}% processing / {:.2f}% input / {:.2f}% output)\n",
    indent, metric.num_runs,
    100.0 * metric.num_runs_processing / metric.num_runs,
    100.0 * metric.num_runs_processing_input / metric.num_runs,
    100.0 * metric.num_runs_processing_output / metric.num_runs);
  if (metric.inbound_measurement.unit != "void") {
    it = fmt::format_to(it, "{}inbound:\n", indent);
    it = fmt::format_to(
      it, locale, "{}{}{}: {:L} at a rate of {:.2f}/s\n", indent, indent,
      metric.inbound_measurement.unit, metric.inbound_measurement.num_elements,
      static_cast<double>(metric.inbound_measurement.num_elements)
        / std::chrono::duration_cast<
            std::chrono::duration<double, std::chrono::seconds::period>>(
            metric.time_total)
            .count());
    if (metric.inbound_measurement.unit != operator_type_name<chunk_ptr>()) {
      it = fmt::format_to(
        it, locale, "{}{}bytes: {:L} at a rate of {:.2f}/s (estimate)\n",
        indent, indent, metric.inbound_measurement.num_approx_bytes,
        static_cast<double>(metric.inbound_measurement.num_approx_bytes)
          / std::chrono::duration_cast<
              std::chrono::duration<double, std::chrono::seconds::period>>(
              metric.time_total)
              .count());
    }
    it = fmt::format_to(
      it, locale, "{}{}batches: {:L} ({:.2f} {}/batch)\n", indent, indent,
      metric.inbound_measurement.num_batches,
      static_cast<double>(metric.inbound_measurement.num_elements)
        / static_cast<double>(metric.inbound_measurement.num_batches),
      metric.inbound_measurement.unit);
  }
  if (metric.outbound_measurement.unit != "void") {
    it = fmt::format_to(it, "{}outbound:\n", indent);
    it = fmt::format_to(
      it, locale, "{}{}{}: {:L} at a rate of {:.2f}/s\n", indent, indent,
      metric.outbound_measurement.unit,
      metric.outbound_measurement.num_elements,
      static_cast<double>(metric.outbound_measurement.num_elements)
        / std::chrono::duration_cast<
            std::chrono::duration<double, std::chrono::seconds::period>>(
            metric.time_total)
            .count());
    if (metric.outbound_measurement.unit != operator_type_name<chunk_ptr>()) {
      it = fmt::format_to(
        it, locale, "{}{}bytes: {:L} at a rate of {:.2f}/s (estimate)\n",
        indent, indent, metric.outbound_measurement.num_approx_bytes,
        static_cast<double>(metric.outbound_measurement.num_approx_bytes)
          / std::chrono::duration_cast<
              std::chrono::duration<double, std::chrono::seconds::period>>(
              metric.time_total)
              .count());
    }
    it = fmt::format_to(
      it, locale, "{}{}batches: {:L} ({:.2f} {}/batch)\n", indent, indent,
      metric.outbound_measurement.num_batches,
      static_cast<double>(metric.outbound_measurement.num_elements)
        / static_cast<double>(metric.outbound_measurement.num_batches),
      metric.outbound_measurement.unit);
  }
  return result;
}

auto exec_pipeline(pipeline pipe, caf::actor_system& sys,
                   std::unique_ptr<diagnostic_handler> diag,
                   const exec_config& cfg) -> caf::expected<void> {
  // If the pipeline ends with events, we implicitly write the output as JSON
  // to stdout, and if it ends with bytes, we implicitly write those bytes to
  // stdout.
  if (not cfg.no_implicit) {
    auto implicit_pipe = add_implicit_source_and_sink(std::move(pipe));
    if (not implicit_pipe) {
      return std::move(implicit_pipe.error());
    }
    pipe = std::move(*implicit_pipe);
  }
  pipe = pipe.optimize_if_closed();
  auto self = caf::scoped_actor{sys};
  auto result = caf::expected<void>{};
  auto metrics = std::vector<metric>{};
  // TODO: This command should probably implement signal handling, and check
  // whether a signal was raised in every iteration over the executor. This
  // will likely be easier to implement once we switch to the actor-based
  // asynchronous executor, so we may as well wait until then.
  struct handler_state {
    pipeline_executor_actor executor = {};
  };
  auto handler = self->spawn(
    [&](caf::stateful_actor<handler_state>* self) -> caf::behavior {
      self->set_down_handler([&, self](const caf::down_msg& msg) {
        TENZIR_DEBUG("command received down message `{}` from {}", msg.reason,
                     msg.source);
        if (msg.reason) {
          result = msg.reason;
        }
        self->quit();
      });
      self->state.executor = self->spawn<caf::monitored>(
        pipeline_executor, std::move(pipe),
        caf::actor_cast<receiver_actor<diagnostic>>(self),
        caf::actor_cast<receiver_actor<metric>>(self), node_actor{}, true);
      self->request(self->state.executor, caf::infinite, atom::start_v)
        .then(
          []() {
            TENZIR_DEBUG("started pipeline successfully");
          },
          [&, self](caf::error& err) {
            TENZIR_DEBUG("failed to start pipeline: {}", err);
            result = std::move(err);
            self->quit();
          });
      return {
        [&](diagnostic& d) {
          diag->emit(std::move(d));
        },
        [&](metric& m) {
          if (cfg.dump_metrics) {
            const auto idx = m.operator_index;
            if (idx >= metrics.size()) {
              metrics.resize(idx + 1);
            }
            metrics[idx] = std::move(m);
          }
        },
      };
    });
  self->wait_for(handler);
  TENZIR_DEBUG("command is done");
  if (cfg.dump_metrics) {
    for (const auto& metric : metrics) {
      fmt::print(stderr, "{}", format_metric(metric));
    }
  }
  return result;
}

void dump_diagnostics_to_stdout(std::span<const diagnostic> diagnostics,
                                std::string filename, std::string content) {
  // Replay diagnostics to reconstruct `stderr` on `stdout`.
  auto printer = make_diagnostic_printer(
    std::move(filename), std::move(content), color_diagnostics::no, std::cout);
  for (auto&& diag : diagnostics) {
    printer->emit(diag);
  }
}

auto exec_impl(std::string content, std::unique_ptr<diagnostic_handler> diag,
               const exec_config& cfg, caf::actor_system& sys)
  -> caf::expected<void> {
  auto parsed = tql::parse(std::move(content), *diag);
  if (not parsed) {
    if (not diag->has_seen_error()) {
      return caf::make_error(ec::unspecified,
                             "internal error: parsing failed without an error");
    }
    return ec::silent;
  }
  if (diag->has_seen_error()) {
    return caf::make_error(ec::unspecified,
                           "internal error: parsing successful with error");
  }
  if (cfg.dump_ast) {
    for (auto& op : *parsed) {
      fmt::print("{}\n", op.inner);
    }
    fmt::print("-----\n");
    for (auto& op : *parsed) {
      auto str = std::string{};
      auto writer = caf::detail::stringification_inspector{str};
      if (writer.apply(op)) {
        fmt::print("{}\n", str);
      } else {
        fmt::print("<error: {}>\n", writer.get_error());
      }
    }
    return {};
  }
  return exec_pipeline(tql::to_pipeline(std::move(*parsed)), sys,
                       std::move(diag), cfg);
}

class diagnostic_handler_ref final : public diagnostic_handler {
public:
  explicit diagnostic_handler_ref(diagnostic_handler& inner) : inner_{inner} {
  }

  void emit(diagnostic d) override {
    inner_.emit(std::move(d));
  }

  auto has_seen_error() const -> bool override {
    return inner_.has_seen_error();
  }

private:
  diagnostic_handler& inner_;
};

auto exec_command(const invocation& inv, caf::actor_system& sys)
  -> caf::expected<void> {
  const auto& args = inv.arguments;
  if (args.size() != 1)
    return caf::make_error(
      ec::invalid_argument,
      fmt::format("expected exactly one argument, but got {}", args.size()));
  auto cfg = exec_config{};
  cfg.dump_ast = caf::get_or(inv.options, "tenzir.exec.dump-ast", false);
  cfg.dump_diagnostics
    = caf::get_or(inv.options, "tenzir.exec.dump-diagnostics", false);
  cfg.dump_metrics
    = caf::get_or(inv.options, "tenzir.exec.dump-metrics", false);
  auto as_file = caf::get_or(inv.options, "tenzir.exec.file", false);
  cfg.no_implicit = caf::get_or(inv.options, "tenzir.exec.no-implicit", false);
  auto filename = std::string{};
  auto content = std::string{};
  if (as_file) {
    filename = args[0];
    auto result = detail::load_contents(filename);
    if (!result) {
      // TODO: Better error message.
      return result.error();
    }
    content = std::move(*result);
  } else {
    filename = "<input>";
    content = args[0];
  }
  if (cfg.dump_diagnostics) {
    auto diag = collecting_diagnostic_handler{};
    auto result = exec_impl(
      content, std::make_unique<diagnostic_handler_ref>(diag), cfg, sys);
    dump_diagnostics_to_stdout(std::move(diag).collect(), filename,
                               std::move(content));
    return result;
  }
  auto printer = make_diagnostic_printer(filename, content,
                                         color_diagnostics::yes, std::cerr);
  return exec_impl(std::move(content), std::move(printer), cfg, sys);
}

class plugin final : public virtual command_plugin {
public:
  plugin() = default;
  ~plugin() override = default;

  auto name() const -> std::string override {
    return "exec";
  }

  auto make_command() const
    -> std::pair<std::unique_ptr<command>, command::factory> override {
    auto exec = std::make_unique<command>(
      "exec", "execute a pipeline locally",
      command::opts("?tenzir.exec")
        .add<bool>("file,f", "load the pipeline definition from a file")
        .add<bool>("dump-ast",
                   "print a textual description of the AST and then exit")
        .add<bool>("dump-diagnostics",
                   "print all diagnostics to stdout before exiting")
        .add<bool>("dump-metrics",
                   "print all diagnostics to stderr before exiting")
        .add<bool>("no-implicit", "disable implicit source and sink"));
    auto factory = command::factory{
      {"exec",
       [=](const invocation& inv, caf::actor_system& sys) -> caf::message {
         auto result = exec_command(inv, sys);
         if (not result) {
           if (result != ec::silent) {
             auto diag = make_diagnostic_printer("", "", color_diagnostics::yes,
                                                 std::cerr);
             diag->emit(diagnostic::error("{}", result.error()).done());
           }
           return caf::make_message(ec::silent);
         }
         return {};
       }},
    };
    return {std::move(exec), std::move(factory)};
  };
};

} // namespace

} // namespace tenzir::plugins::exec

TENZIR_REGISTER_PLUGIN(tenzir::plugins::exec::plugin)
