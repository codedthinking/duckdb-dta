#include "read_dta_function.hpp"
#include "dta_reader.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstring>

namespace duckdb {

// ─── Bind data ──────────────────────────────────────────────────────────────

struct ReadDtaBindData : public TableFunctionData {
	string file_path;
	shared_ptr<dta::DtaReader> reader;
	vector<LogicalType> return_types;
	vector<string> return_names;
	bool apply_value_labels;

	// Column index mapping: which reader column each return column maps to
	vector<idx_t> reader_col_indices;

	// Value label lookups: for each return column, the value label map (if any)
	vector<const dta::DtaValueLabel *> col_value_labels;
	// Pre-built: stata value -> enum index for each column with value labels
	vector<unordered_map<int32_t, uint32_t>> col_enum_index;
};

// ─── Init state ─────────────────────────────────────────────────────────────

struct ReadDtaGlobalState : public GlobalTableFunctionState {
	idx_t current_row;
	vector<char> row_buffer;
	bool strls_loaded;

	ReadDtaGlobalState() : current_row(0), strls_loaded(false) {
	}

	idx_t MaxThreads() const override {
		return 1;
	}
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
	if (fmt.empty()) return false;
	string lower = fmt;
	for (auto &c : lower) c = tolower(c);
	return lower.find("%td") != string::npos || lower.find("%d") != string::npos;
}

static bool IsDatetimeFormat(const string &fmt) {
	// %tc, %tC
	if (fmt.empty()) return false;
	return fmt.find("%tc") != string::npos || fmt.find("%tC") != string::npos;
}

// ─── Type mapping ───────────────────────────────────────────────────────────

static LogicalType MapDtaType(const dta::DtaColumn &col, bool apply_value_labels,
                              const dta::DtaValueLabel *vl) {
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

	// Numeric types
	switch (tc) {
	case 65530: return LogicalType::TINYINT;   // byte
	case 65529: return LogicalType::SMALLINT;   // int
	case 65528: return LogicalType::INTEGER;    // long
	case 65527: return LogicalType::FLOAT;      // float
	case 65526: { // double
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

	// Open reader
	result->reader = make_shared_ptr<dta::DtaReader>(result->file_path);
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
		result->reader_col_indices.push_back(i);
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
	return make_uniq<ReadDtaGlobalState>();
}

// ─── Scan ───────────────────────────────────────────────────────────────────

static void ReadDtaScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<ReadDtaBindData>();
	auto &gstate = data.global_state->Cast<ReadDtaGlobalState>();
	auto &reader = *bind_data.reader;

	idx_t remaining = reader.NumObs() - gstate.current_row;
	if (remaining == 0) {
		output.SetCardinality(0);
		return;
	}

	uint32_t count = static_cast<uint32_t>(std::min(remaining, static_cast<idx_t>(STANDARD_VECTOR_SIZE)));
	size_t actual = reader.ReadRows(gstate.current_row, count, gstate.row_buffer);
	if (actual == 0) {
		output.SetCardinality(0);
		return;
	}

	auto &cols = reader.Columns();
	uint32_t row_width = reader.RowWidth();

	// For each output column, extract data from the row buffer
	for (idx_t out_col = 0; out_col < output.ColumnCount(); out_col++) {
		idx_t reader_col = bind_data.reader_col_indices[out_col];
		auto &col_def = cols[reader_col];

		// Compute byte offset of this column within a row
		uint32_t col_offset = 0;
		for (idx_t c = 0; c < reader_col; c++) {
			col_offset += cols[c].byte_width;
		}

		auto &vec = output.data[out_col];
		auto &type = bind_data.return_types[out_col];

		for (idx_t row = 0; row < actual; row++) {
			const char *row_ptr = gstate.row_buffer.data() + row * row_width + col_offset;

			// Helper: write an enum value from a Stata integer
			auto write_enum = [&](int32_t stata_val) {
				auto &eidx = bind_data.col_enum_index[out_col];
				auto eit = eidx.find(stata_val);
				if (eit != eidx.end()) {
					auto enum_size = EnumType::GetSize(type);
					if (enum_size <= 256) {
						FlatVector::GetData<uint8_t>(vec)[row] = static_cast<uint8_t>(eit->second);
					} else if (enum_size <= 65536) {
						FlatVector::GetData<uint16_t>(vec)[row] = static_cast<uint16_t>(eit->second);
					} else {
						FlatVector::GetData<uint32_t>(vec)[row] = eit->second;
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
					write_enum(static_cast<int32_t>(val));
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
					write_enum(static_cast<int32_t>(val));
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
				// Read (v, o) reference from 8 bytes
				uint32_t v_ref, o_ref_lo;
				memcpy(&v_ref, row_ptr, 4);
				memcpy(&o_ref_lo, row_ptr + 4, 4);
				v_ref = reader.Swap(v_ref);
				o_ref_lo = reader.Swap(o_ref_lo);
				if (v_ref == 0 && o_ref_lo == 0) {
					FlatVector::SetNull(vec, row, true);
				} else {
					auto &str = reader.ResolveStrL(v_ref, o_ref_lo);
					FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, str);
				}
				break;
			}
			default: {
				// str1-str2045
				if (col_def.type_code >= 1 && col_def.type_code <= 2045) {
					// Find null terminator or use full width
					size_t len = strnlen(row_ptr, col_def.byte_width);
					FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, row_ptr, len);
				} else {
					FlatVector::SetNull(vec, row, true);
				}
				break;
			}
			}
		}
	}

	output.SetCardinality(actual);
	gstate.current_row += actual;
}

// ─── Register ───────────────────────────────────────────────────────────────

TableFunction GetReadDtaFunction() {
	TableFunction func("read_dta", {LogicalType::VARCHAR}, ReadDtaScan, ReadDtaBind, ReadDtaInit);
	func.named_parameters["value_labels"] = LogicalType::BOOLEAN;
	func.projection_pushdown = false; // TODO: implement projection pushdown
	return func;
}

} // namespace duckdb
