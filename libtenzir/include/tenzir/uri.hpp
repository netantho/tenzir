//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2016 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tenzir {

/// A URI according to RFC 3986.
///
/// > The generic URI syntax consists of a hierarchical sequence of
/// > components referred to as the scheme, authority, path, query, and
/// > fragment.
/// >
/// >    URI         = scheme ":" hier-part [ "?" query ] [ "#" fragment ]
/// >
/// >    hier-part   = "//" authority path-abempty
/// >                / path-absolute
/// >                / path-rootless
/// >                / path-empty
/// >
/// > The scheme and path components are required, though the path may be
/// > empty (no characters).  When authority is present, the path must
/// > either be empty or begin with a slash ("/") character.  When
/// > authority is not present, the path cannot begin with two slash
/// > characters ("//").  These restrictions result in five different ABNF
/// > rules for a path (Section 3.3), only one of which will match any
/// > given URI reference.
/// >
/// >
/// > The following are two example URIs and their component parts:
/// >
/// >        foo://example.com:8042/over/there?name=ferret#nose
/// >        \_/   \______________/\_________/ \_________/ \__/
/// >         |           |            |            |        |
/// >      scheme     authority       path        query   fragment
/// >         |   _____________________|__
/// >        / \ /                        ⧵
/// >        urn:example:animal:ferret:nose
struct uri {
  std::string scheme;
  std::string host;
  uint16_t port = 0;
  std::vector<std::string> path;
  std::map<std::string, std::string> query;
  std::string fragment;
};

} // namespace tenzir
