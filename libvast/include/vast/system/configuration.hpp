//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2016 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vast/fwd.hpp"

#include <caf/actor_system_config.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace vast::system {

/// @returns The config dirs of the application.
/// @param cfg The actor system config to introspect.
std::vector<std::filesystem::path>
config_dirs(const caf::actor_system_config& cfg);

/// @returns The loaded config files of the application.
/// @note This function is not threadsafe.
const std::vector<std::filesystem::path>& loaded_config_files();

/// Bundles all configuration parameters of a VAST system.
class configuration : public caf::actor_system_config {
public:
  // -- constructors, destructors, and assignment operators --------------------

  configuration();

  // -- modifiers --------------------------------------------------------------

  caf::error parse(int argc, char** argv);

  // -- configuration options --------------------------------------------------

  /// The program command line, without --caf. arguments.
  std::vector<std::string> command_line = {};

  /// The configuration files to load.
  std::vector<std::filesystem::path> config_files = {};

private:
  caf::error embed_config(const caf::settings& settings);
};

} // namespace vast::system
