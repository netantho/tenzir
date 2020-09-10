/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#include "vast/system/index.hpp"

#include "vast/chunk.hpp"
#include "vast/concept/parseable/to.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/bitmap.hpp"
#include "vast/concept/printable/vast/error.hpp"
#include "vast/concept/printable/vast/expression.hpp"
#include "vast/concept/printable/vast/table_slice.hpp"
#include "vast/concept/printable/vast/uuid.hpp"
#include "vast/defaults.hpp"
#include "vast/detail/assert.hpp"
#include "vast/detail/cache.hpp"
#include "vast/detail/fill_status_map.hpp"
#include "vast/detail/narrow.hpp"
#include "vast/detail/notifying_stream_manager.hpp"
#include "vast/error.hpp"
#include "vast/expression_visitors.hpp"
#include "vast/fbs/index.hpp"
#include "vast/fbs/meta_index.hpp"
#include "vast/fbs/utils.hpp"
#include "vast/fbs/uuid.hpp"
#include "vast/fbs/version.hpp"
#include "vast/fwd.hpp"
#include "vast/ids.hpp"
#include "vast/io/read.hpp"
#include "vast/io/save.hpp"
#include "vast/json.hpp"
#include "vast/load.hpp"
#include "vast/logger.hpp"
#include "vast/save.hpp"
#include "vast/status.hpp"
#include "vast/system/accountant.hpp"
#include "vast/system/evaluator.hpp"
#include "vast/system/filesystem.hpp"
#include "vast/system/partition.hpp"
#include "vast/system/query_supervisor.hpp"
#include "vast/system/shutdown.hpp"
#include "vast/table_slice.hpp"
#include "vast/value_index.hpp"

#include <caf/make_counted.hpp>
#include <caf/stateful_actor.hpp>

#include <flatbuffers/flatbuffers.h>

#include <chrono>
#include <deque>
#include <memory>
#include <unordered_set>

#include "caf/error.hpp"
#include "caf/response_promise.hpp"

using namespace std::chrono;

// clang-format off
//
// The index is implemented as a stream stage that hooks into the table slice
// stream coming from the importer, and forwards them to the current active
// partition
//
//              table slice              table slice                      table slice column
//   importer ----------------> index ---------------> active partition ------------------------> indexer
//                                                                      ------------------------> indexer
//                                                                                ...
//
// At the same time, the index is also involved in the lookup path, where it
// receives an expression and loads the partitions that might contain relevant
// results into memory.
//
//                     expression                atom::evaluate
// query_supervisor    ------------>  index     ----------------->   partition
//                                                                      |
//                                                  [indexer]           |
//                                  (spawns     <-----------------------/     
//                                   evaluators) 
//
//                                                  curried_predicate
//                                   evaluator  -------------------------------> indexer
//
//                                                      ids
//                     <--------------------------------------------------------
// clang-format on

namespace vast::system {

caf::actor partition_factory::operator()(const uuid& id) const {
  // Load partition from disk.
  VAST_ASSERT(std::find(state_.persisted_partitions.begin(),
                        state_.persisted_partitions.end(), id)
              != state_.persisted_partitions.end());
  auto path = state_.dir / to_string(id);
  VAST_DEBUG(state_.self, "loads partition", id, "for path", path);
  return state_.self->spawn(passive_partition, id, fs_, path);
}

filesystem_type& partition_factory::fs() {
  return fs_;
}

partition_factory::partition_factory(index_state& state) : state_{state} {
  // nop
}

index_state::index_state(caf::stateful_actor<index_state>* self)
  : self{self}, inmem_partitions{0, partition_factory{*this}} {
}

caf::error index_state::load_from_disk() {
  // We dont use the filesystem actor here because this function is only
  // called once during startup, when no other actors exist yet.
  if (!exists(dir)) {
    VAST_INFO(self, "found no prior state, starting with clean slate");
    return caf::none;
  }
  if (auto fname = index_filename(); exists(fname)) {
    VAST_VERBOSE(self, "loads state from", fname);
    auto buffer = io::read(fname);
    if (!buffer) {
      VAST_ERROR(self, "failed to read index file:",
                 self->system().render(buffer.error()));
      return buffer.error();
    }
    // TODO: Create a `index_ondisk_state` struct and move this part of the
    // code into an `unpack()` function.
    auto fb = span<const byte>{buffer->data(), buffer->size()};
    auto maybe_index
      = fbs::as_versioned_flatbuffer<fbs::Index>(fb, fbs::Version::v0);
    if (!maybe_index)
      return maybe_index.error();
    auto& index = *maybe_index;
    // Sanity check.
    auto fbversion = index->version();
    if (fbversion != fbs::Version::v0)
      return make_error(ec::format_error,
                        "unsupported index version, either remove the existing "
                        "vast.db "
                        "directory or try again with a newer version of VAST");
    auto meta_idx = index->meta_index();
    VAST_ASSERT(meta_idx);
    auto error = unpack(*meta_idx, this->meta_idx);
    if (error)
      return error;
    auto partition_uuids = index->partitions();
    VAST_ASSERT(partition_uuids);
    for (auto uuid_fb : *partition_uuids) {
      VAST_ASSERT(uuid_fb);
      vast::uuid partition_uuid;
      unpack(*uuid_fb, partition_uuid);
      if (exists(dir / to_string(partition_uuid)))
        persisted_partitions.insert(partition_uuid);
      else
        // TODO: Either remove the problematic uuid from the meta index if we
        // get here, or offer a user tool to regenerate the partition from the
        // archive state.
        VAST_WARNING(self, "found partition", partition_uuid,
                     "in the index state but not on disk. This may have been "
                     "caused by an unclean shutdown.");
    }
    auto stats = index->stats();
    if (!stats)
      return make_error(ec::format_error, "no stats in index flatbuffer");
    for (const auto stat : *stats) {
      this->stats.layouts[stat->name()->str()]
        = layout_statistics{stat->count()};
    }
  } else {
    VAST_WARNING(self, "found existing database dir", dir,
                 "without index statefile, will start with fresh state. If "
                 "this database was not empty, results will be missing from "
                 "queries.");
  }
  return caf::none;
}

bool index_state::worker_available() {
  return !idle_workers.empty();
}

caf::actor index_state::next_worker() {
  auto result = std::move(idle_workers.back());
  idle_workers.pop_back();
  return result;
}

void index_state::add_flush_listener(caf::actor listener) {
  VAST_DEBUG(self, "adds a new 'flush' subscriber:", listener);
  flush_listeners.emplace_back(std::move(listener));
  detail::notify_listeners_if_clean(*this, *stage);
}

void index_state::notify_flush_listeners() {
  VAST_DEBUG(self, "sends 'flush' messages to", flush_listeners.size(),
             "listeners");
  for (auto& listener : flush_listeners)
    self->send(listener, atom::flush_v);
  flush_listeners.clear();
}

caf::dictionary<caf::config_value>
index_state::status(status_verbosity v) const {
  using caf::put;
  using caf::put_dictionary;
  using caf::put_list;
  auto result = caf::settings{};
  auto& index_status = put_dictionary(result, "index");
  if (v >= status_verbosity::info) {
    // nop
  }
  if (v >= status_verbosity::detailed) {
    auto& stats_object = put_dictionary(index_status, "statistics");
    auto& layout_object = put_dictionary(stats_object, "layouts");
    for (auto& [name, layout_stats] : stats.layouts) {
      auto xs = caf::dictionary<caf::config_value>{};
      xs.emplace("count", layout_stats.count);
      // We cannot use put_dictionary(layout_object, name) here, because this
      // function splits the key at '.', which occurs in every layout name.
      // Hence the fallback to low-level primitives.
      layout_object.insert_or_assign(name, std::move(xs));
    }
  }
  if (v >= status_verbosity::debug) {
    // Resident partitions.
    auto& partitions = put_dictionary(index_status, "partitions");
    if (active_partition.actor != nullptr)
      partitions.emplace("active", to_string(active_partition.id));
    auto& cached = put_list(partitions, "cached");
    for (auto& kv : inmem_partitions)
      cached.emplace_back(to_string(kv.first));
    auto& unpersisted = put_list(partitions, "unpersisted");
    for (auto& kvp : this->unpersisted)
      unpersisted.emplace_back(to_string(kvp.first));
    // General state such as open streams.
    detail::fill_status_map(index_status, self);
  }
  return result;
}

std::vector<std::pair<uuid, caf::actor>>
index_state::collect_query_actors(query_state& lookup,
                                  uint32_t num_partitions) {
  VAST_TRACE(VAST_ARG(lookup), VAST_ARG(num_partitions));
  std::vector<std::pair<uuid, caf::actor>> result;
  if (num_partitions == 0 || lookup.partitions.empty())
    return result;
  // Prefer partitions that are already available in RAM.
  auto partition_is_loaded = [&](const uuid& candidate) {
    return (active_partition.actor != nullptr
            && active_partition.id == candidate)
           || unpersisted.count(candidate)
           || inmem_partitions.contains(candidate);
  };
  std::partition(lookup.partitions.begin(), lookup.partitions.end(),
                 partition_is_loaded);
  // Helper function to spin up EVALUATOR actors for a single partition.
  auto spin_up = [&](const uuid& partition_id) -> caf::actor {
    // We need to first check whether the ID is the active partition or one
    // of our unpersisted ones. Only then can we dispatch to our LRU cache.
    caf::actor part;
    if (active_partition.actor != nullptr
        && active_partition.id == partition_id)
      part = active_partition.actor;
    else if (auto it = unpersisted.find(partition_id); it != unpersisted.end())
      part = it->second;
    else if (auto it = persisted_partitions.find(partition_id);
             it != persisted_partitions.end())
      part = inmem_partitions.get_or_load(partition_id);
    if (!part)
      VAST_ERROR(self, "could not load partition", partition_id,
                 "that was part of a query");
    return part;
  };
  // Loop over the candidate set until we either successfully scheduled
  // num_partitions partitions or run out of candidates.
  auto it = lookup.partitions.begin();
  auto last = lookup.partitions.end();
  while (it != last && result.size() < num_partitions) {
    auto partition_id = *it++;
    if (auto partition_actor = spin_up(partition_id))
      result.push_back(std::make_pair(partition_id, partition_actor));
  }
  lookup.partitions.erase(lookup.partitions.begin(), it);
  VAST_DEBUG(self, "launched", result.size(),
             "await handlers to fill the pending query map");
  return result;
}

/// Sends an `evaluate` atom to all partition actors passed into this function,
/// and collects the resulting
/// @param c Continuation that takes a single argument of type
/// `caf::expected<pending_query_map>`. The continuation will be called in the
/// context of `self`.
//
// TODO: At some point we should add some more template magic on top of
// this and turn it into a generic functions that maps
//
//   (map from U to A, request param pack R, result handler with param X) ->
//   expected<map from U to X>
template <typename Continuation>
void await_evaluation_maps(
  caf::stateful_actor<index_state>* self, const expression& expr,
  const std::vector<std::pair<vast::uuid, caf::actor>>& actors,
  Continuation then) {
  struct counter {
    size_t received;
    pending_query_map pqm;
  };
  auto expected = actors.size();
  auto shared_counter = std::make_shared<counter>();
  for (auto& [id, actor] : actors) {
    auto& partition_id = id; // Can't use structured binding inside lambda.
    self->request(actor, caf::infinite, expr)
      .then(
        [=](evaluation_triples triples) {
          auto received = ++shared_counter->received;
          if (!triples.empty())
            shared_counter->pqm.emplace(partition_id, std::move(triples));
          if (received == expected)
            then(std::move(shared_counter->pqm));
        },
        [=](caf::error err) {
          // Don't increase `received` to ensure the sucess handler never gets
          // called.
          then(err);
        });
  }
}

query_map
index_state::launch_evaluators(pending_query_map& pqm, expression expr) {
  query_map result;
  for (auto& [id, eval] : pqm) {
    std::vector<caf::actor> xs{self->spawn(evaluator, expr, std::move(eval))};
    result.emplace(id, std::move(xs));
  }
  pqm.clear();
  return result;
}

path index_state::index_filename(path basename) const {
  return basename / dir / "index.bin";
}

caf::expected<flatbuffers::Offset<fbs::Index>>
pack(flatbuffers::FlatBufferBuilder& builder, const index_state& state) {
  auto meta_idx = pack(builder, state.meta_idx);
  if (!meta_idx)
    return meta_idx.error();
  VAST_VERBOSE(state.self, "persisting", state.persisted_partitions.size(),
               " definitely persisted and ", state.unpersisted.size(),
               " maybe persisted partitions uuids");
  std::vector<flatbuffers::Offset<fbs::UUID>> partition_offsets;
  for (auto uuid : state.persisted_partitions) {
    if (auto uuid_fb = pack(builder, uuid))
      partition_offsets.push_back(*uuid_fb);
    else
      return uuid_fb.error();
  }
  // We don't know if these will make it to disk before the index and the rest
  // of the system is shut down (in case of a hard/dirty shutdown), so we just
  // store everything and throw out the missing partitions when loading the
  // index.
  for (auto& kv : state.unpersisted) {
    if (auto uuid_fb = pack(builder, kv.first))
      partition_offsets.push_back(*uuid_fb);
    else
      return uuid_fb.error();
  }
  auto partitions = builder.CreateVector(partition_offsets);
  std::vector<flatbuffers::Offset<fbs::LayoutStatistics>> stats_offsets;
  for (auto& [name, layout_stats] : state.stats.layouts) {
    auto name_fb = builder.CreateString(name);
    fbs::LayoutStatisticsBuilder stats_builder(builder);
    stats_builder.add_name(name_fb);
    stats_builder.add_count(layout_stats.count);
    auto offset = stats_builder.Finish();
    stats_offsets.push_back(offset);
  }
  auto stats = builder.CreateVector(stats_offsets);
  fbs::IndexBuilder index_builder(builder);
  index_builder.add_version(fbs::Version::v0);
  index_builder.add_meta_index(*meta_idx);
  index_builder.add_partitions(partitions);
  index_builder.add_stats(stats);
  return index_builder.Finish();
}

/// Persists the state to disk.
void index_state::flush_to_disk() {
  auto builder = new flatbuffers::FlatBufferBuilder();
  auto index = pack(*builder, *this);
  if (!index) {
    VAST_WARNING(self, "couldnt pack index", index.error());
    return;
  }
  builder->Finish(*index, "I000");
  auto ptr = builder->GetBufferPointer();
  auto size = builder->GetSize();
  auto chunk = vast::chunk::make(size, ptr, [=] { delete builder; });
  self
    ->request(caf::actor_cast<caf::actor>(filesystem), caf::infinite,
              atom::write_v, index_filename(), chunk)
    .then(
      [=](atom::ok) { VAST_DEBUG(self, "successfully persisted index state"); },
      [=](caf::error err) {
        VAST_WARNING(self, "failed to persist index state", err);
      });
}

caf::behavior index(caf::stateful_actor<index_state>* self, filesystem_type fs,
                    path dir, size_t partition_capacity,
                    size_t max_inmem_partitions, size_t taste_partitions,
                    size_t num_workers, bool delay_flush_until_shutdown) {
  VAST_VERBOSE(self, "initializes index in", dir);
  VAST_VERBOSE(self, "caps partition size at", partition_capacity, "events");
  // Set members.
  self->state.self = self;
  self->state.filesystem = fs;
  self->state.dir = dir;
  self->state.delay_flush_until_shutdown = delay_flush_until_shutdown;
  self->state.partition_capacity = partition_capacity;
  self->state.taste_partitions = taste_partitions;
  self->state.inmem_partitions.factory().fs() = fs;
  self->state.inmem_partitions.resize(max_inmem_partitions);
  // Read persistent state.
  if (auto err = self->state.load_from_disk()) {
    VAST_ERROR(self, "cannot load index state from disk:", err);
    VAST_ERROR_ANON("Please try again or remove "
                    "it to start with a clean state (after making a backup");
    self->quit(err);
    return {};
  }
  // Creates a new active partition and updates index state.
  auto create_active_partition = [=] {
    auto id = uuid::random();
    caf::settings index_opts;
    // TODO: Set the 'cardinality' option once ch19167 is resolved.
    // index_opts["cardinality"] = partition_capacity;
    auto part
      = self->spawn(active_partition, id, self->state.filesystem, index_opts);
    auto slot = self->state.stage->add_outbound_path(part);
    self->state.active_partition.actor = part;
    self->state.active_partition.stream_slot = slot;
    self->state.active_partition.capacity = partition_capacity;
    self->state.active_partition.id = id;
    VAST_DEBUG(self, "created new partition", to_string(id));
  };
  auto decomission_active_partition = [=] {
    auto& active = self->state.active_partition;
    auto id = active.id;
    caf::actor actor = nullptr;
    std::swap(actor, active.actor);
    self->state.unpersisted[id] = actor;
    // Send buffered batches.
    self->state.stage->out().fan_out_flush();
    self->state.stage->out().force_emit_batches();
    // Remove active partition from the stream.
    self->state.stage->out().close(active.stream_slot);
    // Persist active partition asynchronously.
    auto part_dir = dir / to_string(id);
    VAST_DEBUG(self, "persists active partition to", part_dir);
    self->request(actor, caf::infinite, atom::persist_v, part_dir)
      .then(
        [=](atom::ok) {
          VAST_VERBOSE(self, "successfully persisted partition", id);
          self->state.unpersisted.erase(id);
          self->state.persisted_partitions.insert(id);
        },
        [=](const caf::error& err) {
          VAST_ERROR(self, "failed to persist partition", id, ":", err);
          self->quit(err);
        });
  };
  // Setup stream manager.
  self->state.stage = detail::attach_notifying_stream_stage(
    self,
    /* continuous = */ true,
    [=](caf::unit_t&) {
      VAST_DEBUG(self, "initializes new table slice stream");
    },
    [=](caf::unit_t&, caf::downstream<table_slice_ptr>& out,
        table_slice_ptr x) {
      self->state.stats.layouts[x->layout().name()].count += x->rows();
      auto& active = self->state.active_partition;
      if (!active.actor) {
        create_active_partition();
      } else if (x->rows() > active.capacity) {
        VAST_DEBUG(self, "exceeds active capacity by",
                   (x->rows() - active.capacity));
        decomission_active_partition();
        if (!self->state.delay_flush_until_shutdown)
          self->state.flush_to_disk();
        create_active_partition();
      }
      VAST_DEBUG(self, "forwards table slice", to_string(*x));
      VAST_DEBUG(self, "slice info:", active.capacity,
                 self->state.partition_capacity, x->rows());

      out.push(x);
      self->state.meta_idx.add(active.id, *x);
      if (active.capacity == self->state.partition_capacity
          && x->rows() > active.capacity) {
        VAST_WARNING(self, "got table slice with", x->rows(),
                     "rows that exceeds the default partition capacity",
                     self->state.partition_capacity);
        active.capacity = 0;
      } else {
        VAST_ASSERT(active.capacity >= x->rows());
        active.capacity -= x->rows();
        VAST_DEBUG(self, "reduces active partition capacity to",
                   (std::to_string(active.capacity) + '/'
                    + std::to_string(self->state.partition_capacity)));
      }
    },
    [=](caf::unit_t&, const caf::error& err) {
      // We get an 'unreachable' error when the stream becomes unreachable
      // because the actor was destroyed; in this case we can't use `self`
      // anymore.
      if (err
          && caf::exit_reason{err.code()} != caf::exit_reason::unreachable) {
        VAST_ERROR(self, "aborted with error", self->system().render(err));
        // We can shutdown now because we only get a single stream from the
        // importer.
        self->send_exit(self, err);
      }
      VAST_DEBUG_ANON("index finalized streaming");
    });
  self->set_exit_handler([=](const caf::exit_msg& msg) {
    VAST_DEBUG(self, "received EXIT from", msg.source,
               "with reason:", msg.reason);
    // Flush buffered batches and end stream.
    self->state.stage->out().fan_out_flush();
    self->state.stage->out().force_emit_batches();
    self->state.stage->out().close();
    self->state.stage->shutdown();
    // Bring down active partition.
    if (self->state.active_partition.actor)
      decomission_active_partition();
    // Collect partitions for termination.
    std::vector<caf::actor> partitions;
    partitions.reserve(self->state.inmem_partitions.size() + 1);
    // partitions.push_back(self->state.active_partition.actor);
    for ([[maybe_unused]] auto& [_, part] : self->state.unpersisted)
      partitions.push_back(part);
    for ([[maybe_unused]] auto& [_, part] : self->state.inmem_partitions)
      partitions.push_back(part);
    // Receiving an EXIT message does not need to coincide with the state being
    // destructed, so we explicitly clear the tables to release the references.
    self->state.unpersisted.clear();
    self->state.inmem_partitions.clear();
    // Terminate partition actors.
    VAST_DEBUG(self, "brings down", partitions.size(), "partitions");
    self->state.flush_to_disk();
    shutdown<policy::parallel>(self, std::move(partitions));
  });
  // Launch workers for resolving queries.
  for (size_t i = 0; i < num_workers; ++i)
    self->spawn(query_supervisor, self);
  // We switch between has_worker behavior and the default behavior (which
  // simply waits for a worker).
  self->set_default_handler(caf::skip);
  self->state.has_worker.assign(
    [=](caf::stream<table_slice_ptr> in) {
      VAST_DEBUG(self, "got a new table slice stream");
      return self->state.stage->add_inbound_path(in);
    },
    // The partition delegates the actual writing to the filesystem actor,
    // so we dont really get more information than a binary ok/not-ok here.
    [=](caf::result<atom::ok> write_result) {
      if (write_result.err)
        VAST_ERROR(self, "could not persist:", write_result.err);
      else
        VAST_VERBOSE(self, "successfully persisted partition");
    },
    // Query handling
    [=](vast::expression expr) {
      auto& st = self->state;
      auto mid = self->current_message_id();
      auto sender = self->current_sender();
      auto client = caf::actor_cast<caf::actor>(sender);
      auto respond = [=](auto&&... xs) {
        unsafe_response(self, sender, {}, mid.response_id(),
                        std::forward<decltype(xs)>(xs)...);
      };
      // Convenience function for dropping out without producing hits.
      // Makes sure that clients always receive a 'done' message.
      auto no_result = [=] {
        respond(uuid::nil(), uint32_t{0}, uint32_t{0});
        self->send(client, atom::done_v);
      };
      // Sanity check.
      if (self->current_sender() == nullptr) {
        VAST_ERROR(self, "got an anonymous query (ignored)");
        respond(caf::sec::invalid_argument);
        return;
      }
      // Get all potentially matching partitions.
      auto candidates = st.meta_idx.lookup(expr);
      if (candidates.empty()) {
        VAST_DEBUG(self, "returns without result: no partitions qualify");
        no_result();
        return;
      }
      // Allows the client to query further results after initial taste.
      auto query_id = uuid::random();
      auto total = candidates.size();
      auto scheduled = detail::narrow<uint32_t>(
        std::min(candidates.size(), st.taste_partitions));
      auto lookup = query_state{query_id, expr, std::move(candidates)};
      auto result = st.pending.emplace(query_id, std::move(lookup));
      VAST_ASSERT(result.second);
      // NOTE: The previous version of the index used to do much more
      // validation before assigning a query id; in particular it did
      // evaluate the entries of the pending query map and checked that
      // at least one of them actually produced an evaluation triple.
      // However, the query_processor doesnt really care about the id
      // anyways, so hopefully that shouldnt make too big of a difference.
      respond(query_id, detail::narrow<uint32_t>(total), scheduled);
      self->delegate(caf::actor_cast<caf::actor>(self), query_id, scheduled);
      return;
    },
    [=](const uuid& query_id, uint32_t num_partitions) {
      auto& st = self->state;
      auto mid = self->current_message_id();
      auto sender = self->current_sender();
      auto client = caf::actor_cast<caf::actor>(sender);
      auto respond = [=](auto&&... xs) {
        unsafe_response(self, sender, {}, mid.response_id(),
                        std::forward<decltype(xs)>(xs)...);
      };
      // Sanity checks.
      if (sender == nullptr) {
        VAST_ERROR(self, "got an anonymous query (ignored)");
        return;
      }
      // A zero as second argument means the client drops further results.
      if (num_partitions == 0) {
        VAST_DEBUG(self, "dropped remaining results for query ID", query_id);
        st.pending.erase(query_id);
        return;
      }
      auto iter = st.pending.find(query_id);
      if (iter == st.pending.end()) {
        self->send(client, atom::done_v);
        return;
      }
      // Get partition actors, spawning new ones if needed.
      auto actors = st.collect_query_actors(iter->second, num_partitions);
      // Send an evaluate atom to all the actors and collect the returned
      // evaluation triples in a `pending_query_map`, then run the continuation
      // below in the same actor context.
      await_evaluation_maps(
        self, iter->second.expression, actors,
        [self, client, query_id,
         num_partitions](caf::expected<pending_query_map> maybe_pqm) {
          auto& st = self->state;
          auto iter = st.pending.find(query_id);
          if (iter == st.pending.end()) {
            VAST_ERROR(self, "ignoring continuation for unknown query",
                       query_id);
            self->send(client, atom::done_v);
            return;
          }
          if (!maybe_pqm) {
            VAST_ERROR(self, "error collecting pending query map",
                       maybe_pqm.error());
            self->send(client, atom::done_v);
            return;
          }
          auto& pqm = *maybe_pqm;
          if (pqm.empty()) {
            if (!iter->second.partitions.empty()) {
              // None of the partitions of this round produced an evaluation
              // triple, but there are still more to go.
              self->delegate(caf::actor_cast<caf::actor>(self), query_id,
                             num_partitions);
              return;
            }
            st.pending.erase(iter);
            VAST_DEBUG(self, "returns without result: no partitions qualify");
            self->send(client, atom::done_v);
            return;
          }
          auto qm = st.launch_evaluators(pqm, iter->second.expression);
          // Delegate to query supervisor (uses up this worker) and report
          // query ID + some stats to the client.
          VAST_DEBUG(self, "schedules", qm.size(),
                     "more partition(s) for query", iter->first, "with",
                     iter->second.partitions.size(), "remaining");
          self->send(st.next_worker(), iter->second.expression, std::move(qm),
                     client);
          // Cleanup if we exhausted all candidates.
          if (iter->second.partitions.empty())
            st.pending.erase(iter);
        });
    },
    [=](atom::worker, caf::actor& worker) {
      self->state.idle_workers.emplace_back(std::move(worker));
    },
    [=](atom::done, uuid partition_id) {
      // Nothing to do.
      VAST_VERBOSE(self, "query for partition", partition_id, "is done");
    },
    [=](caf::stream<table_slice_ptr> in) {
      VAST_DEBUG(self, "got a new source");
      return self->state.stage->add_inbound_path(in);
    },
    [=](atom::status, status_verbosity v) -> caf::config_value::dictionary {
      return self->state.status(v);
    },
    [=](atom::subscribe, atom::flush, const caf::actor& listener) {
      self->state.add_flush_listener(listener);
    });
  return {
    // The default behaviour
    [=](atom::worker, caf::actor& worker) {
      auto& st = self->state;
      st.idle_workers.emplace_back(std::move(worker));
      self->become(caf::keep_behavior, st.has_worker);
    },
    [=](atom::done, uuid partition_id) {
      VAST_VERBOSE(self, "received DONE for partition", partition_id);
    },
    [=](caf::stream<table_slice_ptr> in) {
      VAST_DEBUG(self, "got a new source");
      return self->state.stage->add_inbound_path(in);
    },
    [=](accountant_type accountant) {
      self->state.accountant = std::move(accountant);
    },
    [=](atom::status, status_verbosity v) -> caf::config_value::dictionary {
      return self->state.status(v);
    },
    [=](atom::subscribe, atom::flush, const caf::actor& listener) {
      self->state.add_flush_listener(listener);
    },
  };
}

} // namespace vast::system
