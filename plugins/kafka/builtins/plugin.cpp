//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2023 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "kafka/operator.hpp"

#include <tenzir/tql2/plugin.hpp>

namespace tenzir::plugins::kafka {
namespace {

class load_plugin final
  : public virtual operator_plugin2<loader_adapter<kafka_loader>> {
public:
  auto initialize(const record& config,
                  const record& /* global_config */) -> caf::error override {
    config_ = config;
    if (!config_.contains("bootstrap.servers")) {
      config_["bootstrap.servers"] = "localhost";
    }
    if (!config_.contains("client.id")) {
      config_["client.id"] = "tenzir";
    }
    return caf::none;
  }

  auto
  make(invocation inv, session ctx) const -> failure_or<operator_ptr> override {
    auto args = loader_args{};
    TRY(argument_parser2::operator_(name())
          .add("topic", args.topic)
          .add("count", args.count)
          .add("exit", args.exit)
          .add("offset", args.offset)
          .add("options", args.options)
          .parse(inv, ctx));
    if (args.offset and not offset_parser()(args.offset->inner)) {
      diagnostic::error("invalid `offset` value")
        .primary(args.offset->source)
        .note("must be `beginning`, `end`, `store`, `<offset>` or `-<offset>`")
        .emit(ctx);
      return failure::promise();
    }
    return std::make_unique<loader_adapter<kafka_loader>>(
      kafka_loader{std::move(args), config_});
  }

private:
  record config_;
};

class save_plugin final
  : public virtual operator_plugin2<saver_adapter<kafka_saver>> {
  auto initialize(const record& config,
                  const record& /* global_config */) -> caf::error override {
    config_ = config;
    if (!config_.contains("bootstrap.servers")) {
      config_["bootstrap.servers"] = "localhost";
    }
    if (!config_.contains("client.id")) {
      config_["client.id"] = "tenzir";
    }
    return caf::none;
  }

  auto
  make(invocation inv, session ctx) const -> failure_or<operator_ptr> override {
    auto args = saver_args{};
    auto ts = std::optional<located<time>>{};
    TRY(argument_parser2::operator_(name())
          .add("topic", args.topic)
          .add("key", args.key)
          .add("timestamp", ts)
          .add("options", args.options)
          .parse(inv, ctx));
    // HACK: Should directly accept a time
    // XXX: Verify if to_string gives the same representation as parser::time.
    if (ts) {
      args.timestamp = located{fmt::to_string(ts->inner), ts->source};
    }
    return std::make_unique<saver_adapter<kafka_saver>>(
      kafka_saver{std::move(args), config_});
  }

private:
  record config_;
};

} // namespace
} // namespace tenzir::plugins::kafka

TENZIR_REGISTER_PLUGIN(tenzir::plugins::kafka::load_plugin)
TENZIR_REGISTER_PLUGIN(tenzir::plugins::kafka::save_plugin)
