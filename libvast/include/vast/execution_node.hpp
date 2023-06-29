#pragma once

#include "vast/fwd.hpp"

#include "vast/actors.hpp"
#include "vast/pipeline.hpp"

#include <caf/typed_event_based_actor.hpp>

#include <queue>

namespace vast {

/// Spawns and monitors an execution node for the given operator and a known
/// input type.
///
/// The execution node sits at the very core of Tenzir's pipeline execution. It
/// provides an actor-based abstraction of a single operator in a pipeline.
///
/// The execution follows the Volcano model, with some small tweaks:
/// - Every execution node with an upstream oeprator has an inbound buffer.
/// - Every execution node with a downstream operator has an outbound buffer.
/// - Starting an execution node primes its generator. This corresponds to the
///   Volcano model's _open_ function.
/// - Execution nodes to fill the outbound and inbound buffers eagerly. To this
///   end, operators with an upstream operator request demand from the previous
///   execution node. Execution nodes respond to demand by requesting a set of
///   results that matches the demand to be accepted by the execution node that
///   generated the demand. Once the result set is accepted, the demand request
///   is responded to. This corresponds to the Volcano model's _next_ function.
/// - Graceful shutdowns propagate downstream once the outbound buffer is empty.
///   Ungraceful shutdowns propagate downstream immediately. Starting an
///   execution node advances the operator's generator up to the first element
///   it can yield. This corresponds to the Volcano model's _close_ function.
/// - Yielding from an operator's generator is guaranteed to return control to
///   the scheduler before the generator is resumed.
/// - Execution nodes are guaranteed to be started right-to-left in the
///   pipeline, and should be spawned left-to-right by the pipeline executor.
///
/// @param self The actor that spawns and monitors the execution node.
/// @param op The operator to execute.
/// @param input_type The input type to assume for the operator.
/// @param node The node actor to expose in the control plane. Must be set if
/// the operator runs at a remote node.
/// @param diagnostics_handler The handler asked to spawn diagnostics.
/// @returns The execution node actor and its output type, or an error.
/// @pre op != nullptr
/// @pre node != nullptr or not (op->location() == operator_location::remote)
/// @pre diagnostics_handler != nullptr
auto spawn_exec_node(caf::scheduled_actor* self, operator_ptr op,
                     operator_type input_type, node_actor node,
                     receiver_actor<diagnostic> diagnostics_handler)
  -> caf::expected<std::pair<exec_node_actor, operator_type>>;

} // namespace vast
