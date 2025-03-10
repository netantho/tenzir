//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2022 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include <tenzir/aggregation_function.hpp>
#include <tenzir/plugin.hpp>

namespace tenzir::plugins::sum {

namespace {

template <basic_type Type>
class sum_function final : public aggregation_function {
public:
  explicit sum_function(type input_type) noexcept
    : aggregation_function(std::move(input_type)) {
    // nop
  }

private:
  [[nodiscard]] type output_type() const override {
    TENZIR_ASSERT(caf::holds_alternative<Type>(input_type()));
    return input_type();
  }

  void add(const data_view& view) override {
    using view_type = tenzir::view<type_to_data_t<Type>>;
    if (caf::holds_alternative<caf::none_t>(view))
      return;
    if (!sum_)
      sum_ = materialize(caf::get<view_type>(view));
    else
      sum_ = *sum_ + materialize(caf::get<view_type>(view));
  }

  [[nodiscard]] caf::expected<data> finish() && override {
    return data{sum_};
  }

  std::optional<type_to_data_t<Type>> sum_ = {};
};

class plugin : public virtual aggregation_function_plugin {
  caf::error initialize([[maybe_unused]] const record& plugin_config,
                        [[maybe_unused]] const record& global_config) override {
    return {};
  }

  [[nodiscard]] std::string name() const override {
    return "sum";
  };

  [[nodiscard]] caf::expected<std::unique_ptr<aggregation_function>>
  make_aggregation_function(const type& input_type) const override {
    auto f = detail::overload{
      [&]<basic_type Type>(
        const Type&) -> caf::expected<std::unique_ptr<aggregation_function>> {
        return std::make_unique<sum_function<Type>>(input_type);
      },
      [](const time_type& type)
        -> caf::expected<std::unique_ptr<aggregation_function>> {
        return caf::make_error(ec::invalid_configuration,
                               fmt::format("sum aggregation function does not "
                                           "support type {}",
                                           type));
      },
      [](const null_type& type)
        -> caf::expected<std::unique_ptr<aggregation_function>> {
        return caf::make_error(ec::invalid_configuration,
                               fmt::format("sum aggregation function does not "
                                           "support type {}",
                                           type));
      },
      [](const string_type& type)
        -> caf::expected<std::unique_ptr<aggregation_function>> {
        return caf::make_error(ec::invalid_configuration,
                               fmt::format("sum aggregation function does not "
                                           "support type {}",
                                           type));
      },
      [](const ip_type& type)
        -> caf::expected<std::unique_ptr<aggregation_function>> {
        return caf::make_error(ec::invalid_configuration,
                               fmt::format("sum aggregation function does not "
                                           "support type {}",
                                           type));
      },
      [](const subnet_type& type)
        -> caf::expected<std::unique_ptr<aggregation_function>> {
        return caf::make_error(ec::invalid_configuration,
                               fmt::format("sum aggregation function does not "
                                           "support type {}",
                                           type));
      },
      []<complex_type Type>(const Type& type)
        -> caf::expected<std::unique_ptr<aggregation_function>> {
        return caf::make_error(ec::invalid_configuration,
                               fmt::format("sum aggregation function does not "
                                           "support complex type {}",
                                           type));
      },
    };
    return caf::visit(f, input_type);
  }
};

} // namespace

} // namespace tenzir::plugins::sum

TENZIR_REGISTER_PLUGIN(tenzir::plugins::sum::plugin)
