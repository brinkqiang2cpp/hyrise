#include "join_reference_operator.hpp"

#include "resolve_type.hpp"
#include "type_comparison.hpp"

namespace {

template <typename T>
std::vector<T> concatenate(const std::vector<T>& l, const std::vector<T>& r) {
  auto result = l;
  result.insert(result.end(), r.begin(), r.end());
  return result;
}

}  // namespace

namespace opossum {

bool JoinReferenceOperator::supports(JoinMode join_mode, PredicateCondition predicate_condition,
                                     DataType left_data_type, DataType right_data_type, bool secondary_predicates) {
  return true;
}

JoinReferenceOperator::JoinReferenceOperator(const std::shared_ptr<const AbstractOperator>& left,
                                             const std::shared_ptr<const AbstractOperator>& right, const JoinMode mode,
                                             const OperatorJoinPredicate& primary_predicate,
                                             const std::vector<OperatorJoinPredicate>& secondary_predicates)
    : AbstractJoinOperator(OperatorType::JoinReference, left, right, mode, primary_predicate, secondary_predicates) {}

const std::string JoinReferenceOperator::name() const { return "JoinReference"; }

std::shared_ptr<const Table> JoinReferenceOperator::_on_execute() {
  const auto output_table = _initialize_output_table(TableType::Data);

  const auto left_tuples = input_table_left()->get_rows();
  const auto right_tuples = input_table_right()->get_rows();

  // Tuples with NULLs used to fill up tuples in outer joins that do not find a match
  const auto NULL_TUPLE_LEFT = Tuple(input_table_left()->column_count(), NullValue{});
  const auto NULL_TUPLE_RIGHT = Tuple(input_table_right()->column_count(), NullValue{});

  switch (_mode) {
    case JoinMode::Inner:
      for (const auto& left_tuple : left_tuples) {
        for (const auto& right_tuple : right_tuples) {
          if (_tuples_match(left_tuple, right_tuple)) {
            output_table->append(concatenate(left_tuple, right_tuple));
          }
        }
      }
      break;

    case JoinMode::Left:
      for (const auto& left_tuple : left_tuples) {
        auto has_match = false;

        for (const auto& right_tuple : right_tuples) {
          if (_tuples_match(left_tuple, right_tuple)) {
            has_match = true;
            output_table->append(concatenate(left_tuple, right_tuple));
          }
        }

        if (!has_match) {
          output_table->append(concatenate(left_tuple, NULL_TUPLE_RIGHT));
        }
      }
      break;

    case JoinMode::Right:
      for (const auto& right_tuple : right_tuples) {
        auto has_match = false;

        for (const auto& left_tuple : left_tuples) {
          if (_tuples_match(left_tuple, right_tuple)) {
            has_match = true;
            output_table->append(concatenate(left_tuple, right_tuple));
          }
        }

        if (!has_match) {
          output_table->append(concatenate(NULL_TUPLE_LEFT, right_tuple));
        }
      }
      break;

    case JoinMode::FullOuter: {
      // Track which tuples from each side have matches
      auto left_matches = std::vector<bool>(input_table_left()->row_count(), false);
      auto right_matches = std::vector<bool>(input_table_right()->row_count(), false);

      for (size_t left_tuple_idx{0}; left_tuple_idx < left_tuples.size(); ++left_tuple_idx) {
        const auto& left_tuple = left_tuples[left_tuple_idx];

        for (size_t right_tuple_idx{0}; right_tuple_idx < right_tuples.size(); ++right_tuple_idx) {
          const auto& right_tuple = right_tuples[right_tuple_idx];

          if (_tuples_match(left_tuple, right_tuple)) {
            output_table->append(concatenate(left_tuple, right_tuple));
            left_matches[left_tuple_idx] = true;
            right_matches[right_tuple_idx] = true;
          }
        }
      }

      // Add tuples without matches to output table
      for (size_t left_tuple_idx{0}; left_tuple_idx < input_table_left()->row_count(); ++left_tuple_idx) {
        if (!left_matches[left_tuple_idx]) {
          output_table->append(concatenate(left_tuples[left_tuple_idx], NULL_TUPLE_RIGHT));
        }
      }

      for (size_t right_tuple_idx{0}; right_tuple_idx < input_table_right()->row_count(); ++right_tuple_idx) {
        if (!right_matches[right_tuple_idx]) {
          output_table->append(concatenate(NULL_TUPLE_LEFT, right_tuples[right_tuple_idx]));
        }
      }
    } break;

    case JoinMode::Semi: {
      for (const auto& left_tuple : left_tuples) {
        const auto has_match = std::any_of(right_tuples.begin(), right_tuples.end(), [&](const auto& right_tuple) {
          return _tuples_match(left_tuple, right_tuple);
        });

        if (has_match) {
          output_table->append(left_tuple);
        }
      }
    } break;

    case JoinMode::AntiNullAsTrue:
    case JoinMode::AntiNullAsFalse: {
      for (const auto& left_tuple : left_tuples) {
        const auto has_no_match = std::none_of(right_tuples.begin(), right_tuples.end(), [&](const auto& right_tuple) {
          return _tuples_match(left_tuple, right_tuple);
        });

        if (has_no_match) {
          output_table->append(left_tuple);
        }
      }
    } break;

    case JoinMode::Cross:
      Fail("Cross join not supported");
      break;
  }

  return output_table;
}

bool JoinReferenceOperator::_tuples_match(const Tuple& tuple_left, const Tuple& tuple_right) const {
  if (!_evaluate_predicate(_primary_predicate, tuple_left, tuple_right)) {
    return false;
  }

  return std::all_of(_secondary_predicates.begin(), _secondary_predicates.end(), [&](const auto& secondary_predicate) {
    return _evaluate_predicate(secondary_predicate, tuple_left, tuple_right);
  });
}

bool JoinReferenceOperator::_evaluate_predicate(const OperatorJoinPredicate& predicate, const Tuple& tuple_left,
                                                const Tuple& tuple_right) const {
  auto result = false;
  const auto variant_left = tuple_left[predicate.column_ids.first];
  const auto variant_right = tuple_right[predicate.column_ids.second];

  if (variant_is_null(variant_left) || variant_is_null(variant_right)) {
    // AntiNullAsTrue is the only JoinMode that treats null-booleans as TRUE, all others treat it as FALSE
    return _mode == JoinMode::AntiNullAsTrue;
  }

  resolve_data_type(data_type_from_all_type_variant(variant_left), [&](const auto data_type_left_t) {
    using ColumnDataTypeLeft = typename decltype(data_type_left_t)::type;

    resolve_data_type(data_type_from_all_type_variant(variant_right), [&](const auto data_type_right_t) {
      using ColumnDataTypeRight = typename decltype(data_type_right_t)::type;

      if constexpr (std::is_same_v<ColumnDataTypeLeft, pmr_string> == std::is_same_v<ColumnDataTypeRight, pmr_string>) {
        with_comparator(predicate.predicate_condition, [&](const auto comparator) {
          result =
              comparator(boost::get<ColumnDataTypeLeft>(variant_left), boost::get<ColumnDataTypeRight>(variant_right));
        });
      } else {
        Fail("Cannot compare string with non-string type");
      }
    });
  });

  return result;
}

std::shared_ptr<AbstractOperator> JoinReferenceOperator::_on_deep_copy(
    const std::shared_ptr<AbstractOperator>& copied_input_left,
    const std::shared_ptr<AbstractOperator>& copied_input_right) const {
  return std::make_shared<JoinReferenceOperator>(copied_input_left, copied_input_right, _mode, _primary_predicate,
                                                 _secondary_predicates);
}

void JoinReferenceOperator::_on_set_parameters(const std::unordered_map<ParameterID, AllTypeVariant>& parameters) {}

}  // namespace opossum
