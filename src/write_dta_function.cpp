#include "write_dta_function.hpp"
#include "dta_writer.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstring>
#include <mutex>

namespace duckdb {

// ─── Bind data ──────────────────────────────────────────────────────────────

struct WriteDtaBindData : public FunctionData {
	vector<dta::DtaWriteColumn> columns;
	string dataset_label;
	// ENUM columns: column index -> value label
	vector<dta::WriterValueLabel> value_labels;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<WriteDtaBindData>();
		copy->columns = columns;
		copy->dataset_label = dataset_label;
		copy->value_labels = value_labels;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<WriteDtaBindData>();
		return columns.size() == other.columns.size() && dataset_label == other.dataset_label;
	}
};

// ─── Global state ───────────────────────────────────────────────────────────

struct WriteDtaGlobalState : public GlobalFunctionData {
	unique_ptr<dta::DtaWriter> writer;
	mutex lock;
	uint64_t rows_written;

	WriteDtaGlobalState() : rows_written(0) {
	}
};

// ─── Local state ────────────────────────────────────────────────────────────

struct WriteDtaLocalState : public LocalFunctionData {};

// ─── Constants ──────────────────────────────────────────────────────────────

static constexpr int32_t STATA_EPOCH_OFFSET = 3653;
static constexpr int64_t STATA_TC_EPOCH_OFFSET_MS = 3653LL * 24 * 60 * 60 * 1000;

// Missing value sentinels (LSF byte order)
static constexpr int8_t MISSING_BYTE = 101;    // 0x65
static constexpr int16_t MISSING_INT = 32741;  // 0x7fe5
static constexpr int32_t MISSING_LONG = 2147483621; // 0x7fffffe5

static double MissingDouble() {
	uint64_t bits = 0x7fe0000000000000ULL;
	double val;
	memcpy(&val, &bits, 8);
	return val;
}

static float MissingFloat() {
	uint32_t bits = 0x7f000000U;
	float val;
	memcpy(&val, &bits, 4);
	return val;
}

// ─── Type mapping ───────────────────────────────────────────────────────────

static dta::DtaWriteColumn MapDuckDBType(const string &name, const LogicalType &type) {
	dta::DtaWriteColumn col;
	col.name = name;

	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		col.type_code = 65530; // byte
		col.byte_width = 1;
		col.format = "%8.0g";
		break;
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		col.type_code = 65529; // int (2-byte)
		col.byte_width = 2;
		col.format = "%8.0g";
		break;
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
		col.type_code = 65528; // long (4-byte)
		col.byte_width = 4;
		col.format = "%12.0g";
		break;
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
		col.type_code = 65526; // double
		col.byte_width = 8;
		col.format = "%10.0g";
		break;
	case LogicalTypeId::FLOAT:
		col.type_code = 65527; // float
		col.byte_width = 4;
		col.format = "%9.0g";
		break;
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		col.type_code = 65526; // double
		col.byte_width = 8;
		col.format = "%10.0g";
		break;
	case LogicalTypeId::DATE:
		col.type_code = 65526; // double
		col.byte_width = 8;
		col.format = "%td";
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		col.type_code = 65526; // double
		col.byte_width = 8;
		col.format = "%tc";
		break;
	case LogicalTypeId::VARCHAR:
		col.type_code = 32768; // strL
		col.byte_width = 8;   // (v, o) reference
		col.format = "%9s";
		break;
	case LogicalTypeId::ENUM: {
		// Map ENUM to the smallest integer type that fits
		auto enum_size = EnumType::GetSize(type);
		if (enum_size <= 100) {
			col.type_code = 65530; // byte
			col.byte_width = 1;
		} else if (enum_size <= 32740) {
			col.type_code = 65529; // int
			col.byte_width = 2;
		} else {
			col.type_code = 65528; // long
			col.byte_width = 4;
		}
		col.format = "%8.0g";
		break;
	}
	default:
		// Fall back to strL for everything else
		col.type_code = 32768;
		col.byte_width = 8;
		col.format = "%9s";
		break;
	}

	return col;
}

// ─── Bind ───────────────────────────────────────────────────────────────────

static unique_ptr<FunctionData> WriteDtaBind(ClientContext &context, CopyFunctionBindInput &input,
                                             const vector<string> &names, const vector<LogicalType> &sql_types) {
	auto result = make_uniq<WriteDtaBindData>();

	for (idx_t i = 0; i < names.size(); i++) {
		auto col = MapDuckDBType(names[i], sql_types[i]);

		// Handle ENUM: create value label
		if (sql_types[i].id() == LogicalTypeId::ENUM) {
			string label_name = names[i];
			col.value_label_name = label_name;

			dta::WriterValueLabel vl;
			vl.name = label_name;
			auto enum_size = EnumType::GetSize(sql_types[i]);
			for (idx_t ei = 0; ei < enum_size; ei++) {
				auto str = EnumType::GetString(sql_types[i], ei);
				vl.mappings[static_cast<int32_t>(ei)] = str.GetString();
			}
			result->value_labels.push_back(std::move(vl));
		}

		result->columns.push_back(std::move(col));
	}

	return std::move(result);
}

// ─── Init global ────────────────────────────────────────────────────────────

static unique_ptr<GlobalFunctionData> WriteDtaInitGlobal(ClientContext &context, FunctionData &bind_data,
                                                         const string &file_path) {
	auto &bdata = bind_data.Cast<WriteDtaBindData>();
	auto gstate = make_uniq<WriteDtaGlobalState>();

	gstate->writer = make_uniq<dta::DtaWriter>(file_path, bdata.columns, bdata.dataset_label);
	gstate->writer->WriteMetadata();

	// Register value labels
	for (auto &vl : bdata.value_labels) {
		gstate->writer->AddValueLabel(vl);
	}

	return std::move(gstate);
}

// ─── Init local ─────────────────────────────────────────────────────────────

static unique_ptr<LocalFunctionData> WriteDtaInitLocal(ExecutionContext &context, FunctionData &bind_data) {
	return make_uniq<WriteDtaLocalState>();
}

// ─── Sink ───────────────────────────────────────────────────────────────────

static void WriteDtaSink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate_p,
                         LocalFunctionData &lstate, DataChunk &input) {
	auto &bdata = bind_data.Cast<WriteDtaBindData>();
	auto &gstate = gstate_p.Cast<WriteDtaGlobalState>();
	lock_guard<mutex> guard(gstate.lock);

	auto &writer = *gstate.writer;
	uint32_t row_width = writer.RowWidth();
	idx_t count = input.size();

	// Flatten all vectors for direct access
	input.Flatten();

	// Row buffer for transposition
	vector<char> row_buf(row_width, 0);

	for (idx_t row = 0; row < count; row++) {
		memset(row_buf.data(), 0, row_width);
		uint32_t offset = 0;
		uint64_t obs_idx = gstate.rows_written + row + 1; // 1-based for strL

		for (idx_t col = 0; col < input.ColumnCount(); col++) {
			auto &vec = input.data[col];
			auto &col_def = bdata.columns[col];
			char *dest = row_buf.data() + offset;

			if (FlatVector::IsNull(vec, row)) {
				// Write missing value sentinel
				switch (col_def.type_code) {
				case 65530: { int8_t mv = MISSING_BYTE; memcpy(dest, &mv, 1); break; }
				case 65529: { int16_t mv = MISSING_INT; memcpy(dest, &mv, 2); break; }
				case 65528: { int32_t mv = MISSING_LONG; memcpy(dest, &mv, 4); break; }
				case 65527: { float mv = MissingFloat(); memcpy(dest, &mv, 4); break; }
				case 65526: { double mv = MissingDouble(); memcpy(dest, &mv, 8); break; }
				case 32768: { memset(dest, 0, 8); break; } // strL: (0,0) = NULL
				default: break;
				}
			} else {
				auto val_type = vec.GetType().id();

				switch (col_def.type_code) {
				case 65530: { // byte
					int8_t val;
					if (val_type == LogicalTypeId::BOOLEAN) {
						val = FlatVector::GetData<bool>(vec)[row] ? 1 : 0;
					} else if (val_type == LogicalTypeId::ENUM) {
						auto enum_size = EnumType::GetSize(vec.GetType());
						if (enum_size <= 256) {
							val = static_cast<int8_t>(FlatVector::GetData<uint8_t>(vec)[row]);
						} else {
							val = static_cast<int8_t>(FlatVector::GetData<uint16_t>(vec)[row]);
						}
					} else if (val_type == LogicalTypeId::UTINYINT) {
						val = static_cast<int8_t>(FlatVector::GetData<uint8_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::TINYINT) {
						val = FlatVector::GetData<int8_t>(vec)[row];
					} else {
						val = static_cast<int8_t>(vec.GetValue(row).CastAs(context.client, LogicalType::TINYINT).GetValue<int8_t>());
					}
					memcpy(dest, &val, 1);
					break;
				}
				case 65529: { // int (2-byte)
					int16_t val;
					if (val_type == LogicalTypeId::ENUM) {
						val = static_cast<int16_t>(FlatVector::GetData<uint16_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::USMALLINT) {
						val = static_cast<int16_t>(FlatVector::GetData<uint16_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::SMALLINT) {
						val = FlatVector::GetData<int16_t>(vec)[row];
					} else {
						val = vec.GetValue(row).CastAs(context.client, LogicalType::SMALLINT).GetValue<int16_t>();
					}
					memcpy(dest, &val, 2);
					break;
				}
				case 65528: { // long (4-byte)
					int32_t val;
					if (val_type == LogicalTypeId::ENUM) {
						val = static_cast<int32_t>(FlatVector::GetData<uint32_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::INTEGER) {
						val = FlatVector::GetData<int32_t>(vec)[row];
					} else if (val_type == LogicalTypeId::UINTEGER) {
						val = static_cast<int32_t>(FlatVector::GetData<uint32_t>(vec)[row]);
					} else {
						val = vec.GetValue(row).CastAs(context.client, LogicalType::INTEGER).GetValue<int32_t>();
					}
					memcpy(dest, &val, 4);
					break;
				}
				case 65527: { // float
					float val;
					if (val_type == LogicalTypeId::FLOAT) {
						val = FlatVector::GetData<float>(vec)[row];
					} else {
						val = vec.GetValue(row).CastAs(context.client, LogicalType::FLOAT).GetValue<float>();
					}
					memcpy(dest, &val, 4);
					break;
				}
				case 65526: { // double
					double val;
					if (val_type == LogicalTypeId::DATE) {
						auto date = FlatVector::GetData<date_t>(vec)[row];
						val = static_cast<double>(date.days + STATA_EPOCH_OFFSET);
					} else if (val_type == LogicalTypeId::TIMESTAMP || val_type == LogicalTypeId::TIMESTAMP_TZ) {
						auto ts = FlatVector::GetData<timestamp_t>(vec)[row];
						int64_t unix_us = ts.value;
						int64_t stata_ms = unix_us / 1000 + STATA_TC_EPOCH_OFFSET_MS;
						val = static_cast<double>(stata_ms);
					} else if (val_type == LogicalTypeId::DOUBLE) {
						val = FlatVector::GetData<double>(vec)[row];
					} else if (val_type == LogicalTypeId::FLOAT) {
						val = static_cast<double>(FlatVector::GetData<float>(vec)[row]);
					} else if (val_type == LogicalTypeId::BIGINT) {
						val = static_cast<double>(FlatVector::GetData<int64_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::UBIGINT) {
						val = static_cast<double>(FlatVector::GetData<uint64_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::INTEGER) {
						val = static_cast<double>(FlatVector::GetData<int32_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::SMALLINT) {
						val = static_cast<double>(FlatVector::GetData<int16_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::TINYINT) {
						val = static_cast<double>(FlatVector::GetData<int8_t>(vec)[row]);
					} else {
						// Fallback: use Value conversion
						val = vec.GetValue(row).CastAs(context.client, LogicalType::DOUBLE).GetValue<double>();
					}
					memcpy(dest, &val, 8);
					break;
				}
				case 32768: { // strL
					string s;
					if (val_type == LogicalTypeId::VARCHAR) {
						s = FlatVector::GetData<string_t>(vec)[row].GetString();
					} else {
						s = vec.GetValue(row).ToString();
					}
					if (s.empty()) {
						memset(dest, 0, 8);
					} else {
						// v = 1-based column index, o = 1-based observation index
						uint32_t v_ref = static_cast<uint32_t>(col + 1);
						uint32_t o_ref = static_cast<uint32_t>(obs_idx);
						memcpy(dest, &v_ref, 4);
						memcpy(dest + 4, &o_ref, 4);
						writer.AddStrL(v_ref, obs_idx, s);
					}
					break;
				}
				default:
					break;
				}
			}
			offset += col_def.byte_width;
		}

		writer.WriteRowData(row_buf.data(), row_width);
	}

	gstate.rows_written += count;
}

// ─── Combine ────────────────────────────────────────────────────────────────

static void WriteDtaCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                            LocalFunctionData &lstate) {
	// no-op
}

// ─── Finalize ───────────────────────────────────────────────────────────────

static void WriteDtaFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate_p) {
	auto &gstate = gstate_p.Cast<WriteDtaGlobalState>();
	gstate.writer->Finalize(gstate.rows_written);
}

// ─── Register ───────────────────────────────────────────────────────────────

CopyFunction GetDtaCopyFunction() {
	CopyFunction func("dta");
	func.copy_to_bind = WriteDtaBind;
	func.copy_to_initialize_global = WriteDtaInitGlobal;
	func.copy_to_initialize_local = WriteDtaInitLocal;
	func.copy_to_sink = WriteDtaSink;
	func.copy_to_combine = WriteDtaCombine;
	func.copy_to_finalize = WriteDtaFinalize;
	func.extension = "dta";
	return func;
}

} // namespace duckdb
