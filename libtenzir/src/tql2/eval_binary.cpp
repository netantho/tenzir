//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2024 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/fwd.hpp"

#include "tenzir/detail/assert.hpp"
#include "tenzir/detail/checked_math.hpp"
#include "tenzir/series.hpp"
#include "tenzir/table_slice_builder.hpp"
#include "tenzir/tql2/arrow_utils.hpp"
#include "tenzir/tql2/ast.hpp"
#include "tenzir/tql2/eval_impl.hpp"

#include <arrow/compute/api_scalar.h>

#include <type_traits>

// TODO: This file takes very long to compile. Consider splitting it up even more.

namespace tenzir {

namespace {

template <class T>
concept numeric_type
  = concrete_type<T> and basic_type<T>
    and (std::same_as<T, int64_type> || std::same_as<T, uint64_type>
         || std::same_as<T, double_type>);

template <class T>
concept integral_type
  = numeric_type<T>
    and (std::same_as<T, int64_type> || std::same_as<T, uint64_type>);

consteval auto is_arithmetic(ast::binary_op op) -> bool {
  switch (op) {
    using enum ast::binary_op;
    case add:
    case sub:
    case mul:
    case div:
      return true;
    default:
      return false;
  }
}

constexpr auto result_if_both_null(ast::binary_op op) -> std::optional<bool> {
  using enum ast::binary_op;
  switch (op) {
    case eq:
    case ge:
    case le:
      return true;
    case neq:
      return false;
    case add:
    case sub:
    case mul:
    case div:
    case gt:
    case lt:
    case and_:
    case or_:
    case in:
      return std::nullopt;
  }
  TENZIR_UNREACHABLE();
}

consteval auto is_relational(ast::binary_op op) -> bool {
  switch (op) {
    using enum ast::binary_op;
    case eq:
    case neq:
    case gt:
    case lt:
    case ge:
    case le:
      return true;
    default:
      return false;
  }
}

template <ast::binary_op Op, class L, class R>
struct BinOpKernel;

template <ast::binary_op Op, integral_type L, integral_type R>
  requires(is_arithmetic(Op))
struct BinOpKernel<Op, L, R> {
  using result = std::common_type_t<type_to_data_t<L>, type_to_data_t<R>>;

  static auto evaluate(type_to_data_t<L> l, type_to_data_t<R> r)
    -> std::variant<result, const char*> {
    using enum ast::binary_op;
    if constexpr (Op == add) {
      return checked_math::add(l, r);
    } else if constexpr (Op == sub) {
      return checked_math::subtract(l, r);
    } else if constexpr (Op == mul) {
      return checked_math::multiply(l, r);
    } else {
      static_assert(detail::always_false_v<L>,
                    "division is handled by its own specialization");
    }
    return {};
  }
};

// if any of the operands is double, the result is a double
template <ast::binary_op Op, numeric_type L, numeric_type R>
  requires(is_arithmetic(Op)
           and (std::same_as<double_type, L> or std::same_as<double_type, R>))
struct BinOpKernel<Op, L, R> {
  using result = double;

  static auto evaluate(type_to_data_t<L> l, type_to_data_t<R> r)
    -> std::variant<result, const char*> {
    using enum ast::binary_op;
    if constexpr (Op == add) {
      return static_cast<double>(l) + static_cast<double>(r);
    }
    if constexpr (Op == sub) {
      return static_cast<double>(l) - static_cast<double>(r);
    }
    if constexpr (Op == mul) {
      return static_cast<double>(l) * static_cast<double>(r);
    }
    TENZIR_UNREACHABLE();
  }
};

template <>
struct BinOpKernel<ast::binary_op::sub, int64_type, uint64_type> {
  using result = int64_t;

  static auto evaluate(int64_t l, uint64_t r)
    -> std::variant<result, const char*> {
    // r > int_max
    if (r > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return "subtraction overflow";
    }
    // l < 0 and |l| + r > int_max -> l - r < int_min
    if (l < 0
        and static_cast<uint64_t>(abs(l)) + r
              > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                  + 1) {
      return "subtraction underflow";
    }
    return l - static_cast<int64_t>(r);
  }
};

// division always yields doubles
template <numeric_type L, numeric_type R>
struct BinOpKernel<ast::binary_op::div, L, R> {
  using result = double;

  static auto evaluate(type_to_data_t<L> l, type_to_data_t<R> r)
    -> std::variant<result, const char*> {
    if (r == decltype(r){}) {
      return "division by zero";
    }
    return static_cast<double>(l) / static_cast<double>(r);
  }
};

template <>
struct BinOpKernel<ast::binary_op::sub, time_type, duration_type> {
  using result = time;

  static auto evaluate(time l, duration r)
    -> std::variant<result, const char*> {
    return l - r;
  }
};

template <>
struct BinOpKernel<ast::binary_op::sub, time_type, time_type> {
  using result = duration;

  static auto evaluate(time l, time r) -> std::variant<result, const char*> {
    return l - r;
  }
};

template <ast::binary_op Op, basic_type L, basic_type R>
  requires(is_relational(Op) and not(integral_type<L> and integral_type<R>)
           and requires(type_to_data_t<L> l, type_to_data_t<R> r) { l <=> r; })
struct BinOpKernel<Op, L, R> {
  using result = bool;

  static auto evaluate(view<type_to_data_t<L>> l, view<type_to_data_t<R>> r)
    -> std::variant<result, const char*> {
    using enum ast::binary_op;

    if constexpr (std::same_as<L, string_type>
                  and std::same_as<R, string_type>) {
      fmt::print("{} {} {}\n", l, Op, r);
    }
    if constexpr (Op == eq) {
      return l == r;
    } else if constexpr (Op == neq) {
      return l != r;
    } else if constexpr (Op == gt) {
      return l > r;
    } else if constexpr (Op == lt) {
      return l < r;
    } else if constexpr (Op == ge) {
      return l >= r;
    } else if constexpr (Op == le) {
      return l <= r;
    } else {
      static_assert(detail::always_false_v<L>,
                    "unexpected operator in relational kernel");
    }
  }
};

template <ast::binary_op Op, integral_type L, integral_type R>
  requires(is_relational(Op))
struct BinOpKernel<Op, L, R> {
  using result = bool;

  static auto evaluate(type_to_data_t<L> l, type_to_data_t<R> r)
    -> std::variant<result, const char*> {
    using enum ast::binary_op;
    if constexpr (Op == eq) {
      return std::cmp_equal(l, r);
    } else if constexpr (Op == neq) {
      return std::cmp_not_equal(l, r);
    } else if constexpr (Op == gt) {
      return std::cmp_greater(l, r);
    } else if constexpr (Op == lt) {
      return std::cmp_less(l, r);
    } else if constexpr (Op == ge) {
      return std::cmp_greater_equal(l, r);
    } else if constexpr (Op == le) {
      return std::cmp_less_equal(l, r);
    } else {
      static_assert(detail::always_false_v<L>,
                    "unexpected operator in relational kernel");
    }
  }
};

template <ast::binary_op Op, concrete_type L, concrete_type R>
struct EvalBinOp;

// specialization for cases where a kernel is implemented
template <ast::binary_op Op, concrete_type L, concrete_type R>
  requires caf::detail::is_complete<BinOpKernel<Op, L, R>>
struct EvalBinOp<Op, L, R> {
  static auto
  eval(const type_to_arrow_array_t<L>& l, const type_to_arrow_array_t<R>& r,
       auto&& warning_emitter) -> std::shared_ptr<arrow::Array> {
    using kernel = BinOpKernel<Op, L, R>;
    using result = kernel::result;
    using result_type = data_to_type_t<result>;
    auto b = result_type::make_arrow_builder(arrow::default_memory_pool());
    auto warnings
      = detail::stack_vector<const char*, 2 * sizeof(const char*)>{};
    for (auto i = int64_t{0}; i < l.length(); ++i) {
      auto ln = l.IsNull(i);
      auto rn = r.IsNull(i);
      if (ln && rn) {
        auto constexpr res = result_if_both_null(Op);
        if constexpr (res) {
          check(b->Append(*res));
        } else {
          check(b->AppendNull());
        }
        continue;
      }
      if (ln || rn) {
        check(b->AppendNull());
        continue;
      }
      auto lv = value_at(L{}, l, i);
      auto rv = value_at(R{}, r, i);
      auto res = kernel::evaluate(lv, rv);
      if (auto r = std::get_if<result>(&res)) {
        check(append_builder(result_type{}, *b, *r));
      } else {
        check(b->AppendNull());
        auto e = std::get<const char*>(res);
        if (std::ranges::find(warnings, e) == std::ranges::end(warnings)) {
          warnings.push_back(e);
        }
      }
    }
    for (auto&& w : warnings) {
      warning_emitter(w);
    }
    return finish(*b);
  }
};

template <>
struct EvalBinOp<ast::binary_op::add, string_type, string_type> {
  static auto eval(const arrow::StringArray& l, const arrow::StringArray& r,
                   auto&&) -> std::shared_ptr<arrow::StringArray> {
    auto b = arrow::StringBuilder{};
    check(b.Reserve(l.length()));
    for (auto i = int64_t{0}; i < l.length(); ++i) {
      if (l.IsNull(i) || r.IsNull(i)) {
        b.UnsafeAppendNull();
        continue;
      }
      auto lv = l.GetView(i);
      auto rv = r.GetView(i);
      check(b.Append(lv));
      check(b.ExtendCurrent(rv));
    }
    return finish(b);
  }
};

// TODO: We probably don't want this.
template <>
struct EvalBinOp<ast::binary_op::and_, bool_type, bool_type> {
  static auto eval(const arrow::BooleanArray& l, const arrow::BooleanArray& r,
                   auto&&) -> std::shared_ptr<arrow::BooleanArray> {
    // TODO: Bad.
    auto has_null = l.null_bitmap() || r.null_bitmap();
    auto has_offset = l.offset() != 0 || r.offset() != 0;
    if (not has_null && not has_offset) {
      auto buffer = check(arrow::AllocateBitmap(l.length()));
      auto size = (l.length() + 7) / 8;
      TENZIR_ASSERT(l.values()->size() >= size);
      TENZIR_ASSERT(r.values()->size() >= size);
      auto l_ptr = l.values()->data();
      auto r_ptr = r.values()->data();
      auto o_ptr = buffer->mutable_data();
      for (auto i = int64_t{0}; i < size; ++i) {
        o_ptr[i] = l_ptr[i] & r_ptr[i];
      }
      return std::make_shared<arrow::BooleanArray>(l.length(),
                                                   std::move(buffer));
    }
    auto b = arrow::BooleanBuilder{};
    check(b.Reserve(l.length()));
    for (auto i = int64_t{0}; i < l.length(); ++i) {
      auto lv = l.Value(i);
      auto rv = r.Value(i);
      auto ln = l.IsNull(i);
      auto rn = r.IsNull(i);
      auto lt = not ln && lv;
      auto lf = not ln && not lv;
      auto rt = not rn && rv;
      auto rf = not rn && not rv;
      if (lt && rt) {
        b.UnsafeAppend(true);
      } else if (lf || rf) {
        b.UnsafeAppend(false);
      } else {
        b.UnsafeAppendNull();
      }
    }
    return finish(b);
  }
};

template <ast::binary_op Op, concrete_type L>
  requires(Op == ast::binary_op::eq || Op == ast::binary_op::neq)
struct EvalBinOp<Op, L, null_type> {
  static auto eval(const type_to_arrow_array_t<L>& l, const arrow::NullArray& r,
                   auto&&) -> std::shared_ptr<arrow::BooleanArray> {
    constexpr auto invert = Op == ast::binary_op::neq;
    // TODO: This is bad.
    TENZIR_UNUSED(r);
    auto buffer = check(arrow::AllocateBitmap(l.length()));
    auto& null_bitmap = l.null_bitmap();
    if (not null_bitmap) {
      // All non-null, except if `null_type`.
      auto value = (std::same_as<L, null_type> != invert) ? 0xFF : 0x00;
      std::memset(buffer->mutable_data(), value, buffer->size());
    } else {
      TENZIR_ASSERT(buffer->size() <= null_bitmap->size());
      auto buffer_ptr = buffer->mutable_data();
      auto null_ptr = null_bitmap->data();
      auto length = detail::narrow<size_t>(buffer->size());
      for (auto i = size_t{0}; i < length; ++i) {
        // TODO
        buffer_ptr[i] = invert ? null_ptr[i] : ~null_ptr[i];
      }
    }
    return std::make_shared<arrow::BooleanArray>(l.length(), std::move(buffer));
  }
};

template <ast::binary_op Op, concrete_type R>
  requires((Op == ast::binary_op::eq || Op == ast::binary_op::neq)
           && not std::same_as<R, null_type>)
struct EvalBinOp<Op, null_type, R> {
  static auto
  eval(const arrow::NullArray& l, const type_to_arrow_array_t<R>& r,
       auto&& warning_emitter) -> std::shared_ptr<arrow::BooleanArray> {
    return EvalBinOp<Op, R, null_type>::eval(r, l, warning_emitter);
  }
};

template <ast::binary_op Op>
  requires(Op == ast::binary_op::eq || Op == ast::binary_op::neq)
struct EvalBinOp<Op, string_type, string_type> {
  static auto eval(const arrow::StringArray& l, const arrow::StringArray& r,
                   auto&&) -> std::shared_ptr<arrow::BooleanArray> {
    // TODO: This is bad.
    constexpr auto invert = Op == ast::binary_op::neq;
    auto b = arrow::BooleanBuilder{};
    check(b.Reserve(l.length()));
    for (auto i = int64_t{0}; i < l.length(); ++i) {
      auto ln = l.IsNull(i);
      auto rn = r.IsNull(i);
      auto equal = bool{};
      if (ln != rn) {
        equal = false;
      } else if (ln && rn) {
        equal = true;
      } else {
        equal = l.Value(i) == r.Value(i);
      }
      b.UnsafeAppend(equal != invert);
    }
    return finish(b);
  }
};

} // namespace

auto evaluator::eval(const ast::binary_expr& x) -> series {
  auto eval_op
    = [&]<ast::binary_op Op>(const series& l, const series& r) -> series {
    TENZIR_ASSERT(x.op.inner == Op);
    TENZIR_ASSERT(l.length() == r.length());
    return caf::visit(
      [&]<concrete_type L, concrete_type R>(const L&, const R&) -> series {
        if constexpr (caf::detail::is_complete<EvalBinOp<Op, L, R>>) {
          using LA = type_to_arrow_array_t<L>;
          using RA = type_to_arrow_array_t<R>;
          auto& la = caf::get<LA>(*l.array);
          auto& ra = caf::get<RA>(*r.array);
          auto oa = EvalBinOp<Op, L, R>::eval(la, ra, [&](const char* w) {
            diagnostic::warning("{}", w).primary(x).emit(ctx_);
          });
          auto ot = type::from_arrow(*oa->type());
          return series{std::move(ot), std::move(oa)};
        } else {
          // TODO: Not possible?
          // TODO: Where coercion? => coercion is done in kernel.
          diagnostic::warning("binary operator `{}` not implemented for `{}` "
                              "and `{}`",
                              x.op.inner, l.type.kind(), r.type.kind())
            .primary(x)
            .emit(ctx_);
          return null();
        }
      },
      l.type, r.type);
  };
  using enum ast::binary_op;
  switch (x.op.inner) {
#define X(op)                                                                  \
  case op:                                                                     \
    return eval_op.operator()<op>(eval(x.left), eval(x.right))
    X(add);
    X(sub);
    X(mul);
    X(div);
    X(eq);
    X(neq);
    X(gt);
    X(ge);
    X(lt);
    X(le);
    X(in);
#undef X
    case and_: {
      // TODO: How does short circuiting work?
      // 1) Evaluate left.
      auto l = eval(x.left);
      // 2) Evaluate right, but discard diagnostics if false.
      // TODO
      auto r = eval(x.right);
      return eval_op.operator()<and_>(l, r);
    }
    case or_:
      TENZIR_TODO();
  }
  TENZIR_UNREACHABLE();
}

} // namespace tenzir
