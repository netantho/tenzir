//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2016 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/sink.hpp"

#include "tenzir/error.hpp"
#include "tenzir/format/zeek.hpp"
#include "tenzir/test/data.hpp"
#include "tenzir/test/fixtures/actor_system_and_events.hpp"
#include "tenzir/test/test.hpp"

using namespace tenzir;
using namespace tenzir;

namespace {

class fixture : public fixtures::actor_system_and_events {
public:
  fixture() : fixtures::actor_system_and_events(TENZIR_PP_STRINGIFY(SUITE)) {
  }
};

} // namespace

FIXTURE_SCOPE(sink_tests, fixture)

TEST(zeek sink) {
  MESSAGE("constructing a sink");
  caf::settings options;
  caf::put(options, "tenzir.export.write", directory.string());
  auto writer = std::make_unique<format::zeek::writer>(options);
  auto snk = self->spawn(sink, std::move(writer), 20u);
  MESSAGE("sending table slices");
  for (auto& slice : zeek_conn_log)
    self->send(snk, slice);
  MESSAGE("shutting down");
  self->send_exit(snk, caf::exit_reason::user_shutdown);
  self->wait_for(snk);
  CHECK(exists(directory / "zeek.conn.log"));
}

FIXTURE_SCOPE_END()
