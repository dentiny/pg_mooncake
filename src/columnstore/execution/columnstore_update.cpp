#include "columnstore/execution/columnstore_update.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/table/update_state.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class ColumnstoreUpdateGlobalSinkState : public GlobalSinkState {
public:
    ColumnstoreUpdateGlobalSinkState(ClientContext &context, const vector<LogicalType> &table_types,
                                     const vector<unique_ptr<Expression>> &expressions,
                                     const vector<unique_ptr<Expression>> &bound_defaults,
                                     const vector<unique_ptr<BoundConstraint>> &bound_constraints)
        : updated_count(0), return_collection(context, table_types), default_executor(context, bound_defaults),
          bound_constraints(bound_constraints) {
        auto &allocator = Allocator::Get(context);
        vector<LogicalType> update_types;
        update_types.reserve(expressions.size());
        for (auto &expr : expressions) {
            update_types.emplace_back(expr->return_type);
        }
        update_chunk.Initialize(allocator, update_types);
    }

    idx_t updated_count;
    vector<row_t> updated_rows;
    ColumnDataCollection return_collection;
    DataChunk update_chunk;
    ExpressionExecutor default_executor;
    unique_ptr<TableDeleteState> delete_state;
    unique_ptr<TableUpdateState> update_state;
    const vector<unique_ptr<BoundConstraint>> &bound_constraints;

public:
    TableDeleteState &GetDeleteState(DataTable &table, TableCatalogEntry &tableref, ClientContext &context) {
        if (delete_state == nullptr) {
            delete_state = table.InitializeDelete(tableref, context, bound_constraints);
        }
        return *delete_state;
    }

    TableUpdateState &GetUpdateState(DataTable &table, TableCatalogEntry &tableref, ClientContext &context) {
        if (update_state == nullptr) {
            update_state = table.InitializeUpdate(tableref, context, bound_constraints);
        }
        return *update_state;
    }
};

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class ColumnstoreUpdateSourceState : public GlobalSourceState {
public:
    explicit ColumnstoreUpdateSourceState(const ColumnstoreUpdate &op) {
        // TODO(hjiang): Assume always return chunk.
        D_ASSERT(op.sink_state);
        auto &g = op.sink_state->Cast<ColumnstoreUpdateGlobalSinkState>();
        g.return_collection.InitializeScan(scan_state);
    }

    ColumnDataScanState scan_state;
};

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
ColumnstoreUpdate::ColumnstoreUpdate(vector<LogicalType> types, TableCatalogEntry &tableref, ColumnstoreTable &table,
                                     vector<PhysicalIndex> columns, vector<unique_ptr<Expression>> expressions,
                                     vector<unique_ptr<Expression>> bound_defaults,
                                     vector<unique_ptr<BoundConstraint>> bound_constraints, idx_t estimated_cardinality)
    : PhysicalOperator(PhysicalOperatorType::UPDATE, std::move(types), estimated_cardinality), tableref(tableref),
      table(table), columns(std::move(columns)), expressions(std::move(expressions)),
      bound_defaults(std::move(bound_defaults)), bound_constraints(std::move(bound_constraints)) {}

unique_ptr<GlobalSourceState> ColumnstoreUpdate::GetGlobalSourceState(ClientContext &context) const {
    return make_uniq<ColumnstoreUpdateSourceState>(*this);
}

SourceResultType ColumnstoreUpdate::GetData(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSourceInput &input) const {
    // TODO(hjiang): Assume always return chunk for now.
    auto &state = input.global_state.Cast<ColumnstoreUpdateSourceState>();
    auto &g = sink_state->Cast<ColumnstoreUpdateGlobalSinkState>();
    g.return_collection.Scan(state.scan_state, chunk);
    return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

SinkResultType ColumnstoreUpdate::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
    auto &gstate = input.global_state.Cast<ColumnstoreUpdateGlobalSinkState>();
    DataChunk &update_chunk = gstate.update_chunk;
    chunk.Flatten();
    gstate.default_executor.SetChunk(chunk);

    auto &row_ids = chunk.data[chunk.ColumnCount() - 1];
    update_chunk.Reset();
    update_chunk.SetCardinality(chunk);

    // Execute query.
    for (idx_t idx = 0; idx < expressions.size(); ++idx) {
        if (expressions[idx]->type == ExpressionType::VALUE_DEFAULT) {
            gstate.default_executor.ExecuteExpression(columns[idx].index, update_chunk.data[idx]);
        } else {
            D_ASSERT(expressions[idx]->type == ExpressionType::BOUND_REF);
            auto &binding = expressions[idx]->Cast<BoundReferenceExpression>();
            update_chunk.data[idx].Reference(chunk.data[binding.index]);
        }
    }

    auto row_id_data = FlatVector::GetData<row_t>(row_ids);
    SelectionVector sel(STANDARD_VECTOR_SIZE);
    idx_t update_count = 0;
    for (idx_t idx = 0; idx < update_chunk.size(); ++idx) {
        auto row_id = row_id_data[idx];
        gstate.updated_rows.emplace_back(row_id);
        sel.set_index(update_count++, idx);
    }
    if (update_count != update_chunk.size()) {
        update_chunk.Slice(sel, update_count);
    }
    table.Insert(context.client, update_chunk);

    // TODO(hjiang): Assume always return chunk.
    gstate.return_collection.Append(update_chunk);
    gstate.updated_count += chunk.size();

    return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType ColumnstoreUpdate::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                             OperatorSinkFinalizeInput &input) const {
    auto &gstate = input.global_state.Cast<ColumnstoreUpdateGlobalSinkState>();
    table.Delete(context, gstate.updated_rows);
    return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> ColumnstoreUpdate::GetGlobalSinkState(ClientContext &context) const {
    return make_uniq<ColumnstoreUpdateGlobalSinkState>(context, table.GetTypes(), expressions, bound_defaults,
                                                       bound_constraints);
}

unique_ptr<PhysicalOperator> Columnstore::PlanUpdate(ClientContext &context, LogicalUpdate &op,
                                                     unique_ptr<PhysicalOperator> plan) {
    auto update = make_uniq<ColumnstoreUpdate>(op.types, op.table, op.table.Cast<ColumnstoreTable>(), op.columns,
                                               std::move(op.expressions), std::move(op.bound_defaults),
                                               std::move(op.bound_constraints), op.estimated_cardinality);
    update->children.emplace_back(std::move(plan));
    return std::move(update);
    // TODO(hjiang): Assert we only accept delete and insert.
}

} // namespace duckdb
