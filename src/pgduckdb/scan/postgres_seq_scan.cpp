#include "duckdb.hpp"

#include "pgduckdb/scan/postgres_seq_scan.hpp"
#include "pgduckdb/pgduckdb_types.hpp"
#include "pgduckdb/logger.hpp"
#include "pgduckdb/scan/heap_reader.hpp"
#include "pgduckdb/pg/relations.hpp"

#include "pgduckdb/utility/cpp_only_file.hpp" // Must be last include.

namespace pgduckdb {

//
// PostgresSeqScanGlobalState
//

PostgresSeqScanGlobalState::PostgresSeqScanGlobalState(Relation rel, duckdb::TableFunctionInitInput &input)
    : m_global_state(duckdb::make_shared_ptr<PostgresScanGlobalState>()),
      m_heap_reader_global_state(duckdb::make_shared_ptr<HeapReaderGlobalState>(rel)), m_rel(rel) {
	m_global_state->InitGlobalState(input);
	m_global_state->m_tuple_desc = RelationGetDescr(m_rel);
	m_global_state->InitRelationMissingAttrs(m_global_state->m_tuple_desc);
	pd_log(DEBUG2, "(DuckDB/PostgresSeqScanGlobalState) Running %" PRIu64 " threads -- ", (uint64_t)MaxThreads());
}

PostgresSeqScanGlobalState::~PostgresSeqScanGlobalState() {
}

//
// PostgresSeqScanLocalState
//

PostgresSeqScanLocalState::PostgresSeqScanLocalState(Relation rel,
                                                     duckdb::shared_ptr<HeapReaderGlobalState> heap_reder_global_state,
                                                     duckdb::shared_ptr<PostgresScanGlobalState> global_state) {
	m_local_state = duckdb::make_shared_ptr<PostgresScanLocalState>(global_state.get());
	m_heap_table_reader = duckdb::make_uniq<HeapReader>(rel, heap_reder_global_state, global_state, m_local_state);
}

PostgresSeqScanLocalState::~PostgresSeqScanLocalState() {
}

//
// PostgresSeqScanFunctionData
//

PostgresSeqScanFunctionData::PostgresSeqScanFunctionData(Relation rel, uint64_t cardinality, Snapshot snapshot)
    : m_rel(rel), m_cardinality(cardinality), m_snapshot(snapshot) {
}

PostgresSeqScanFunctionData::~PostgresSeqScanFunctionData() {
}

//
// PostgresSeqScanFunction
//

PostgresSeqScanFunction::PostgresSeqScanFunction()
    : TableFunction("postgres_seq_scan", {}, PostgresSeqScanFunc, nullptr, PostgresSeqScanInitGlobal,
                    PostgresSeqScanInitLocal) {
	named_parameters["cardinality"] = duckdb::LogicalType::UBIGINT;
	named_parameters["relid"] = duckdb::LogicalType::UINTEGER;
	named_parameters["snapshot"] = duckdb::LogicalType::POINTER;
	projection_pushdown = true;
	filter_pushdown = true;
	filter_prune = true;
	cardinality = PostgresSeqScanCardinality;
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState>
PostgresSeqScanFunction::PostgresSeqScanInitGlobal(duckdb::ClientContext &, duckdb::TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->CastNoConst<PostgresSeqScanFunctionData>();
	auto global_state = duckdb::make_uniq<PostgresSeqScanGlobalState>(bind_data.m_rel, input);
	global_state->m_global_state->m_snapshot = bind_data.m_snapshot;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-move"
	return std::move(global_state);
#pragma GCC diagnostic pop
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState>
PostgresSeqScanFunction::PostgresSeqScanInitLocal(duckdb::ExecutionContext &, duckdb::TableFunctionInitInput &,
                                                  duckdb::GlobalTableFunctionState *gstate) {
	auto global_state = reinterpret_cast<PostgresSeqScanGlobalState *>(gstate);
	return duckdb::make_uniq<PostgresSeqScanLocalState>(global_state->m_rel, global_state->m_heap_reader_global_state,
	                                                    global_state->m_global_state);
}

void
PostgresSeqScanFunction::PostgresSeqScanFunc(duckdb::ClientContext &, duckdb::TableFunctionInput &data,
                                             duckdb::DataChunk &output) {
	auto &local_state = data.local_state->Cast<PostgresSeqScanLocalState>();

	local_state.m_local_state->m_output_vector_size = 0;

	/* We have exhausted seq scan of heap table so we can return */
	if (local_state.m_local_state->m_exhausted_scan) {
		output.SetCardinality(0);
		return;
	}

	auto hasTuple = local_state.m_heap_table_reader->ReadPageTuples(output);

	if (!hasTuple || !IsValidBlockNumber(local_state.m_heap_table_reader->GetCurrentBlockNumber())) {
		local_state.m_local_state->m_exhausted_scan = true;
	}
}

duckdb::unique_ptr<duckdb::NodeStatistics>
PostgresSeqScanFunction::PostgresSeqScanCardinality(duckdb::ClientContext &, const duckdb::FunctionData *data) {
	auto &bind_data = data->Cast<PostgresSeqScanFunctionData>();
	return duckdb::make_uniq<duckdb::NodeStatistics>(bind_data.m_cardinality, bind_data.m_cardinality);
}

} // namespace pgduckdb
