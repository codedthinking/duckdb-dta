#include "read_dta_function.hpp"
#include "dta_reader.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/partition_info.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/table_filter.hpp"

#include <cstring>
#include <mutex>

namespace duckdb {

// ─── Bind data ──────────────────────────────────────────────────────────────

struct ReadDtaBindData : public TableFunctionData {
	string file_path;
	shared_ptr<dta::DtaReader> reader;
	vector<LogicalType> return_types;
	vector<string> return_names;
	bool apply_value_labels;

	// Byte offset of each reader column within a row
	vector<uint32_t> col_offsets;

	// Value label lookups: for each reader column, the value label map (if any)
	vector<const dta::DtaValueLabel *> col_value_labels;
	// Pre-built: stata value -> enum index for each column with value labels
	vector<unordered_map<int32_t, uint32_t>> col_enum_index;
};

// ─── Init state ─────────────────────────────────────────────────────────────

struct ReadDtaGlobalState : public GlobalTableFunctionState {
	mutex lock;
	idx_t next_row = 0;
	vector<column_t> column_ids;
	idx_t max_threads = 1;
	// Pushed-down filters combined into one predicate over the output columns
	unique_ptr<Expression> filter_expr;

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

struct ReadDtaLocalState : public LocalTableFunctionState {
	vector<char> row_buffer;
	idx_t batch_index = 0;
	unique_ptr<ExpressionExecutor> filter_executor;
	SelectionVector filter_sel;
};

// ─── Date/time conversion helpers ───────────────────────────────────────────

// Stata epoch: 1960-01-01, DuckDB epoch: 1970-01-01
// Difference: 3653 days
static constexpr int32_t STATA_EPOCH_OFFSET = 3653;

// Stata %tc: milliseconds since 1960-01-01 00:00:00
// DuckDB: microseconds since 1970-01-01 00:00:00
static constexpr int64_t STATA_TC_EPOCH_OFFSET_MS = 3653LL * 24 * 60 * 60 * 1000;

static bool IsDateFormat(const string &fmt) {
	// %td, %d, %-td, %-d
	if (fmt.empty())
		return false;
	string lower = fmt;
	for (auto &c : lower)
		c = tolower(c);
	return lower.find("%td") != string::npos || lower.find("%d") != string::npos;
}

// Casting a double outside int32 range (or NaN) to int32 is undefined behavior
static bool FitsInInt32(double val) {
	return val >= -2147483648.0 && val < 2147483648.0;
}

static bool IsDatetimeFormat(const string &fmt) {
	// %tc, %tC
	if (fmt.empty())
		return false;
	return fmt.find("%tc") != string::npos || fmt.find("%tC") != string::npos;
}

// ─── Type mapping ───────────────────────────────────────────────────────────

static LogicalType MapDtaType(const dta::DtaColumn &col, bool apply_value_labels, const dta::DtaValueLabel *vl) {
	uint16_t tc = col.type_code;

	// String types
	if (tc >= 1 && tc <= 2045) {
		return LogicalType::VARCHAR;
	}
	if (tc == 32768) { // strL
		return LogicalType::VARCHAR;
	}

	// Numeric types with value labels -> ENUM
	if (apply_value_labels && vl && !vl->mappings.empty()) {
		// Build enum from value label
		// Sort values to get deterministic order
		vector<pair<int32_t, string>> sorted_pairs(vl->mappings.begin(), vl->mappings.end());
		std::sort(sorted_pairs.begin(), sorted_pairs.end());
		Vector enum_strings(LogicalType::VARCHAR, sorted_pairs.size());
		auto str_data = FlatVector::GetData<string_t>(enum_strings);
		for (idx_t i = 0; i < sorted_pairs.size(); i++) {
			str_data[i] = StringVector::AddString(enum_strings, sorted_pairs[i].second);
		}
		return LogicalType::ENUM(enum_strings, sorted_pairs.size());
	}

	// %td dates can be stored in any numeric type; %tc needs double precision
	switch (tc) {
	case 65530:
		return IsDateFormat(col.format) ? LogicalType::DATE : LogicalType::TINYINT; // byte
	case 65529:
		return IsDateFormat(col.format) ? LogicalType::DATE : LogicalType::SMALLINT; // int
	case 65528:
		return IsDateFormat(col.format) ? LogicalType::DATE : LogicalType::INTEGER; // long
	case 65527:
		return IsDateFormat(col.format) ? LogicalType::DATE : LogicalType::FLOAT; // float
	case 65526: {                                                                 // double
		if (IsDateFormat(col.format)) {
			return LogicalType::DATE;
		}
		if (IsDatetimeFormat(col.format)) {
			return LogicalType::TIMESTAMP;
		}
		return LogicalType::DOUBLE;
	}
	default:
		return LogicalType::VARCHAR;
	}
}

// ─── Bind ───────────────────────────────────────────────────────────────────

static unique_ptr<FunctionData> ReadDtaBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadDtaBindData>();

	result->file_path = input.inputs[0].GetValue<string>();

	// Parse named parameters
	result->apply_value_labels = false;
	for (auto &kv : input.named_parameters) {
		if (kv.first == "value_labels") {
			result->apply_value_labels = kv.second.GetValue<bool>();
		}
	}

	// Open reader through DuckDB's virtual filesystem (httpfs, WASM, etc.)
	auto &fs = FileSystem::GetFileSystem(context);
	result->reader = make_shared_ptr<dta::DtaReader>(fs, result->file_path);
	auto &reader = *result->reader;

	// Load value labels if needed
	if (result->apply_value_labels) {
		reader.LoadValueLabels();
	}

	// Load strLs (we need them during scanning)
	// Check if there are any strL columns
	bool has_strls = false;
	for (auto &col : reader.Columns()) {
		if (col.type_code == 32768) {
			has_strls = true;
			break;
		}
	}
	if (has_strls) {
		reader.LoadStrLs();
	}

	// Map columns
	auto &cols = reader.Columns();
	uint32_t running_offset = 0;
	for (idx_t i = 0; i < cols.size(); i++) {
		// Find value label for this column
		const dta::DtaValueLabel *vl = nullptr;
		if (result->apply_value_labels && !cols[i].value_label_name.empty()) {
			for (auto &label : reader.ValueLabels()) {
				if (label.name == cols[i].value_label_name) {
					vl = &label;
					break;
				}
			}
		}

		LogicalType type = MapDtaType(cols[i], result->apply_value_labels, vl);
		return_types.push_back(type);
		names.push_back(cols[i].name);
		result->col_offsets.push_back(running_offset);
		running_offset += cols[i].byte_width;
		result->col_value_labels.push_back(vl);

		// Build enum index: stata_value -> sorted enum position
		unordered_map<int32_t, uint32_t> enum_idx;
		if (vl && type.id() == LogicalTypeId::ENUM) {
			vector<pair<int32_t, string>> sorted_pairs(vl->mappings.begin(), vl->mappings.end());
			std::sort(sorted_pairs.begin(), sorted_pairs.end());
			for (uint32_t ei = 0; ei < sorted_pairs.size(); ei++) {
				enum_idx[sorted_pairs[ei].first] = ei;
			}
		}
		result->col_enum_index.push_back(std::move(enum_idx));
	}

	result->return_types = return_types;
	result->return_names = names;
	return std::move(result);
}

// ─── Init ───────────────────────────────────────────────────────────────────

static unique_ptr<GlobalTableFunctionState> ReadDtaInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ReadDtaBindData>();
	auto gstate = make_uniq<ReadDtaGlobalState>();
	gstate->column_ids = input.column_ids;
	gstate->max_threads = MaxValue<idx_t>(bind_data.reader->NumObs() / STANDARD_VECTOR_SIZE, 1);

	// Turn pushed-down table filters into one boolean expression over the
	// output chunk (filter keys are relative indexes into column_ids)
	if (input.filters) {
		vector<unique_ptr<Expression>> conjuncts;
		for (auto &entry : input.filters->filters) {
			idx_t out_idx = entry.first;
			column_t col_id = input.column_ids[out_idx];
			auto type = IsRowIdColumnId(col_id) ? LogicalType(LogicalType::BIGINT) : bind_data.return_types[col_id];
			BoundReferenceExpression col_ref(std::move(type), out_idx);
			conjuncts.push_back(entry.second->ToExpression(col_ref));
		}
		if (conjuncts.size() == 1) {
			gstate->filter_expr = std::move(conjuncts[0]);
		} else if (!conjuncts.empty()) {
			auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			conjunction->children = std::move(conjuncts);
			gstate->filter_expr = std::move(conjunction);
		}
	}
	return std::move(gstate);
}

static unique_ptr<LocalTableFunctionState> ReadDtaInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                            GlobalTableFunctionState *gstate_p) {
	auto lstate = make_uniq<ReadDtaLocalState>();
	auto &gstate = gstate_p->Cast<ReadDtaGlobalState>();
	if (gstate.filter_expr) {
		lstate->filter_executor = make_uniq<ExpressionExecutor>(context.client, *gstate.filter_expr);
		lstate->filter_sel.Initialize(STANDARD_VECTOR_SIZE);
	}
	return std::move(lstate);
}

static OperatorPartitionData ReadDtaGetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("read_dta cannot return partition columns");
	}
	auto &lstate = input.local_state->Cast<ReadDtaLocalState>();
	return OperatorPartitionData(lstate.batch_index);
}

// ─── Scan ───────────────────────────────────────────────────────────────────

static void ReadDtaScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<ReadDtaBindData>();
	auto &gstate = data.global_state->Cast<ReadDtaGlobalState>();
	auto &lstate = data.local_state->Cast<ReadDtaLocalState>();
	auto &reader = *bind_data.reader;

	// Keep claiming ranges until one yields rows that survive the filters
	while (true) {
		// Claim the next range of rows
		uint64_t start;
		uint32_t count;
		{
			lock_guard<mutex> guard(gstate.lock);
			uint64_t n_obs = reader.NumObs();
			if (gstate.next_row >= n_obs) {
				output.SetCardinality(0);
				return;
			}
			start = gstate.next_row;
			count = static_cast<uint32_t>(MinValue<uint64_t>(STANDARD_VECTOR_SIZE, n_obs - start));
			gstate.next_row += count;
		}
		lstate.batch_index = start / STANDARD_VECTOR_SIZE;

		// Only touch the file when a real column is projected (count(*) reads nothing)
		bool needs_data = false;
		for (auto col_id : gstate.column_ids) {
			if (!IsRowIdColumnId(col_id)) {
				needs_data = true;
				break;
			}
		}
		size_t actual = count;
		if (needs_data) {
			actual = reader.ReadRows(start, count, lstate.row_buffer);
			if (actual == 0) {
				output.SetCardinality(0);
				return;
			}
		}

		auto &cols = reader.Columns();
		uint32_t row_width = reader.RowWidth();
		bool latin1 = reader.Version() == 117;

		// For each output column, extract data from the row buffer
		for (idx_t out_col = 0; out_col < output.ColumnCount(); out_col++) {
			auto &vec = output.data[out_col];
			column_t col_id = gstate.column_ids[out_col];
			if (IsRowIdColumnId(col_id)) {
				auto row_ids = FlatVector::GetData<row_t>(vec);
				for (idx_t row = 0; row < actual; row++) {
					row_ids[row] = static_cast<row_t>(start + row);
				}
				continue;
			}

			idx_t reader_col = col_id;
			auto &col_def = cols[reader_col];
			uint32_t col_offset = bind_data.col_offsets[reader_col];
			auto &type = bind_data.return_types[reader_col];

			for (idx_t row = 0; row < actual; row++) {
				const char *row_ptr = lstate.row_buffer.data() + row * row_width + col_offset;

				// Helper: write an enum value from a Stata integer
				auto write_enum = [&](int32_t stata_val) {
					auto &eidx = bind_data.col_enum_index[reader_col];
					auto eit = eidx.find(stata_val);
					if (eit != eidx.end()) {
						// Match the enum's physical storage type exactly
						switch (type.InternalType()) {
						case PhysicalType::UINT8:
							FlatVector::GetData<uint8_t>(vec)[row] = static_cast<uint8_t>(eit->second);
							break;
						case PhysicalType::UINT16:
							FlatVector::GetData<uint16_t>(vec)[row] = static_cast<uint16_t>(eit->second);
							break;
						default:
							FlatVector::GetData<uint32_t>(vec)[row] = eit->second;
							break;
						}
					} else {
						FlatVector::SetNull(vec, row, true);
					}
				};

				bool is_enum = (type.id() == LogicalTypeId::ENUM);

				switch (col_def.type_code) {
				case 65530: { // byte
					int8_t val;
					memcpy(&val, row_ptr, 1);
					if (dta::DtaMissing::IsMissingByte(val)) {
						FlatVector::SetNull(vec, row, true);
					} else if (is_enum) {
						write_enum(val);
					} else if (type.id() == LogicalTypeId::DATE) {
						FlatVector::GetData<date_t>(vec)[row] = date_t(val - STATA_EPOCH_OFFSET);
					} else {
						FlatVector::GetData<int8_t>(vec)[row] = val;
					}
					break;
				}
				case 65529: { // int (2-byte)
					int16_t val;
					memcpy(&val, row_ptr, 2);
					val = reader.Swap(val);
					if (dta::DtaMissing::IsMissingInt(val)) {
						FlatVector::SetNull(vec, row, true);
					} else if (is_enum) {
						write_enum(val);
					} else if (type.id() == LogicalTypeId::DATE) {
						FlatVector::GetData<date_t>(vec)[row] = date_t(val - STATA_EPOCH_OFFSET);
					} else {
						FlatVector::GetData<int16_t>(vec)[row] = val;
					}
					break;
				}
				case 65528: { // long (4-byte)
					int32_t val;
					memcpy(&val, row_ptr, 4);
					val = reader.Swap(val);
					if (dta::DtaMissing::IsMissingLong(val)) {
						FlatVector::SetNull(vec, row, true);
					} else if (is_enum) {
						write_enum(val);
					} else if (type.id() == LogicalTypeId::DATE) {
						FlatVector::GetData<date_t>(vec)[row] = date_t(val - STATA_EPOCH_OFFSET);
					} else {
						FlatVector::GetData<int32_t>(vec)[row] = val;
					}
					break;
				}
				case 65527: { // float
					float val;
					memcpy(&val, row_ptr, 4);
					val = reader.Swap(val);
					if (dta::DtaMissing::IsMissingFloat(val)) {
						FlatVector::SetNull(vec, row, true);
					} else if (is_enum) {
						if (FitsInInt32(val)) {
							write_enum(static_cast<int32_t>(val));
						} else {
							FlatVector::SetNull(vec, row, true);
						}
					} else if (type.id() == LogicalTypeId::DATE) {
						if (FitsInInt32(val)) {
							FlatVector::GetData<date_t>(vec)[row] =
							    date_t(static_cast<int32_t>(val) - STATA_EPOCH_OFFSET);
						} else {
							FlatVector::SetNull(vec, row, true);
						}
					} else {
						FlatVector::GetData<float>(vec)[row] = val;
					}
					break;
				}
				case 65526: { // double
					double val;
					memcpy(&val, row_ptr, 8);
					val = reader.Swap(val);
					if (dta::DtaMissing::IsMissingDouble(val)) {
						FlatVector::SetNull(vec, row, true);
					} else if (is_enum) {
						if (FitsInInt32(val)) {
							write_enum(static_cast<int32_t>(val));
						} else {
							FlatVector::SetNull(vec, row, true);
						}
					} else if (type.id() == LogicalTypeId::DATE) {
						int32_t stata_days = static_cast<int32_t>(val);
						FlatVector::GetData<date_t>(vec)[row] = date_t(stata_days - STATA_EPOCH_OFFSET);
					} else if (type.id() == LogicalTypeId::TIMESTAMP) {
						int64_t stata_ms = static_cast<int64_t>(val);
						int64_t unix_ms = stata_ms - STATA_TC_EPOCH_OFFSET_MS;
						FlatVector::GetData<timestamp_t>(vec)[row] = timestamp_t(unix_ms * 1000);
					} else {
						FlatVector::GetData<double>(vec)[row] = val;
					}
					break;
				}
				case 32768: { // strL
					// The 8-byte reference splits into v+o differently per format:
					// v(4)+o(4) in 117, v(2)+o(6) in 118/120, v(3)+o(5) in 119/121
					auto bytes = reinterpret_cast<const uint8_t *>(row_ptr);
					int v_bytes = reader.StrLVBytes();
					uint64_t v_ref = 0;
					uint64_t o_ref = 0;
					if (reader.IsMSF()) {
						for (int b = 0; b < v_bytes; b++) {
							v_ref = (v_ref << 8) | bytes[b];
						}
						for (int b = v_bytes; b < 8; b++) {
							o_ref = (o_ref << 8) | bytes[b];
						}
					} else {
						for (int b = v_bytes - 1; b >= 0; b--) {
							v_ref = (v_ref << 8) | bytes[b];
						}
						for (int b = 7; b >= v_bytes; b--) {
							o_ref = (o_ref << 8) | bytes[b];
						}
					}
					if (v_ref == 0 && o_ref == 0) {
						FlatVector::SetNull(vec, row, true);
					} else {
						auto &str = reader.ResolveStrL(static_cast<uint32_t>(v_ref), o_ref);
						FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, str);
					}
					break;
				}
				default: {
					// str1-str2045
					if (col_def.type_code >= 1 && col_def.type_code <= 2045) {
						// Find null terminator or use full width
						size_t len = strnlen(row_ptr, col_def.byte_width);
						if (latin1 && dta::NeedsUtf8Transcode(row_ptr, len)) {
							auto utf8 = dta::Latin1ToUtf8(row_ptr, len);
							FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, utf8);
						} else {
							FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, row_ptr, len);
						}
					} else {
						FlatVector::SetNull(vec, row, true);
					}
					break;
				}
				}
			}
		}

		output.SetCardinality(actual);

		// Apply pushed-down filters; on an empty result claim the next range
		// instead of returning (an empty chunk would end the scan)
		if (lstate.filter_executor) {
			idx_t keep = lstate.filter_executor->SelectExpression(output, lstate.filter_sel);
			if (keep == 0) {
				output.Reset();
				continue;
			}
			output.Slice(lstate.filter_sel, keep);
		}
		return;
	}
}

// ─── Register ───────────────────────────────────────────────────────────────

TableFunction GetReadDtaFunction() {
	TableFunction func("read_dta", {LogicalType::VARCHAR}, ReadDtaScan, ReadDtaBind, ReadDtaInit, ReadDtaInitLocal);
	func.named_parameters["value_labels"] = LogicalType::BOOLEAN;
	func.projection_pushdown = true;
	func.filter_pushdown = true;
	func.get_partition_data = ReadDtaGetPartitionData;
	return func;
}

} // namespace duckdb
