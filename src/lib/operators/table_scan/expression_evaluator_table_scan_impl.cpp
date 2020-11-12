#include "expression_evaluator_table_scan_impl.hpp"

#include "expression/evaluation/expression_evaluator.hpp"
#include "expression/expression_utils.hpp"

namespace opossum {

ExpressionEvaluatorTableScanImpl::ExpressionEvaluatorTableScanImpl(
    const std::shared_ptr<const Table>& in_table, const std::shared_ptr<const AbstractExpression>& expression)
    : _in_table(in_table), _expression(expression) {
  _uncorrelated_subquery_results = ExpressionEvaluator::populate_uncorrelated_subquery_results_cache
      ({std::const_pointer_cast<AbstractExpression>(expression)});
}

std::string ExpressionEvaluatorTableScanImpl::description() const { return "ExpressionEvaluator"; }

std::shared_ptr<RowIDPosList> ExpressionEvaluatorTableScanImpl::scan_chunk(ChunkID chunk_id) {
  return std::make_shared<RowIDPosList>(
      ExpressionEvaluator{_in_table, chunk_id, _uncorrelated_subquery_results}.evaluate_expression_to_pos_list(
          *_expression));
}

}  // namespace opossum
