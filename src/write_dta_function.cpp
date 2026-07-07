#include "write_dta_function.hpp"
#include "dta_writer.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <cmath>
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
		if (columns.size() != other.columns.size() || dataset_label != other.dataset_label) {
			return false;
		}
		for (idx_t i = 0; i < columns.size(); i++) {
			auto &a = columns[i];
			auto &b = other.columns[i];
			if (a.name != b.name || a.type_code != b.type_code || a.byte_width != b.byte_width ||
			    a.format != b.format || a.value_label_name != b.value_label_name || a.label != b.label) {
				return false;
			}
		}
		return true;
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
static constexpr int8_t MISSING_BYTE = 101;         // 0x65
static constexpr int16_t MISSING_INT = 32741;       // 0x7fe5
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

// Read an ENUM value's index according to its physical storage type
static uint32_t GetEnumIndex(Vector &vec, idx_t row) {
	switch (vec.GetType().InternalType()) {
	case PhysicalType::UINT8:
		return FlatVector::GetData<uint8_t>(vec)[row];
	case PhysicalType::UINT16:
		return FlatVector::GetData<uint16_t>(vec)[row];
	default:
		return FlatVector::GetData<uint32_t>(vec)[row];
	}
}

// ─── Type mapping ───────────────────────────────────────────────────────────

static dta::DtaWriteColumn MapDuckDBType(const string &name, const LogicalType &type) {
	dta::DtaWriteColumn col;
	col.name = name;

	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
		col.type_code = 65530; // byte
		col.byte_width = 1;
		col.format = "%8.0g";
		break;
	case LogicalTypeId::UTINYINT: // 0..255 exceeds byte's max of 100
	case LogicalTypeId::SMALLINT:
		col.type_code = 65529; // int (2-byte)
		col.byte_width = 2;
		col.format = "%8.0g";
		break;
	case LogicalTypeId::USMALLINT: // 0..65535 exceeds int's max of 32740
	case LogicalTypeId::INTEGER:
		col.type_code = 65528; // long (4-byte)
		col.byte_width = 4;
		col.format = "%12.0g";
		break;
	case LogicalTypeId::UINTEGER: // 0..2^32-1 exceeds long's max of 2147483620
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
		col.byte_width = 8;    // (v, o) reference
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

	auto &fs = FileSystem::GetFileSystem(context);
	gstate->writer = make_uniq<dta::DtaWriter>(fs, file_path, bdata.columns, bdata.dataset_label);
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
				case 65530: {
					int8_t mv = MISSING_BYTE;
					memcpy(dest, &mv, 1);
					break;
				}
				case 65529: {
					int16_t mv = MISSING_INT;
					memcpy(dest, &mv, 2);
					break;
				}
				case 65528: {
					int32_t mv = MISSING_LONG;
					memcpy(dest, &mv, 4);
					break;
				}
				case 65527: {
					float mv = MissingFloat();
					memcpy(dest, &mv, 4);
					break;
				}
				case 65526: {
					double mv = MissingDouble();
					memcpy(dest, &mv, 8);
					break;
				}
				case 32768: {
					memset(dest, 0, 8);
					break;
				} // strL: (0,0) = NULL
				default:
					break;
				}
			} else {
				auto val_type = vec.GetType().id();

				switch (col_def.type_code) {
				case 65530: { // byte
					int8_t val;
					if (val_type == LogicalTypeId::BOOLEAN) {
						val = FlatVector::GetData<bool>(vec)[row] ? 1 : 0;
					} else if (val_type == LogicalTypeId::ENUM) {
						val = static_cast<int8_t>(GetEnumIndex(vec, row));
					} else if (val_type == LogicalTypeId::TINYINT) {
						val = FlatVector::GetData<int8_t>(vec)[row];
					} else {
						val = vec.GetValue(row).CastAs(context.client, LogicalType::TINYINT).GetValue<int8_t>();
					}
					if (val > 100 || val < -127) {
						throw InvalidInputException(
						    "Value %d in column \"%s\" is out of range for Stata byte (-127..100); "
						    "cast the column to SMALLINT",
						    val, col_def.name);
					}
					memcpy(dest, &val, 1);
					break;
				}
				case 65529: { // int (2-byte)
					int16_t val;
					if (val_type == LogicalTypeId::ENUM) {
						val = static_cast<int16_t>(GetEnumIndex(vec, row));
					} else if (val_type == LogicalTypeId::UTINYINT) {
						val = FlatVector::GetData<uint8_t>(vec)[row];
					} else if (val_type == LogicalTypeId::SMALLINT) {
						val = FlatVector::GetData<int16_t>(vec)[row];
					} else {
						val = vec.GetValue(row).CastAs(context.client, LogicalType::SMALLINT).GetValue<int16_t>();
					}
					if (val > 32740 || val < -32767) {
						throw InvalidInputException(
						    "Value %d in column \"%s\" is out of range for Stata int (-32767..32740); "
						    "cast the column to INTEGER",
						    val, col_def.name);
					}
					memcpy(dest, &val, 2);
					break;
				}
				case 65528: { // long (4-byte)
					int32_t val;
					if (val_type == LogicalTypeId::ENUM) {
						val = static_cast<int32_t>(GetEnumIndex(vec, row));
					} else if (val_type == LogicalTypeId::USMALLINT) {
						val = FlatVector::GetData<uint16_t>(vec)[row];
					} else if (val_type == LogicalTypeId::INTEGER) {
						val = FlatVector::GetData<int32_t>(vec)[row];
					} else {
						val = vec.GetValue(row).CastAs(context.client, LogicalType::INTEGER).GetValue<int32_t>();
					}
					if (val > 2147483620 || val < -2147483647) {
						throw InvalidInputException(
						    "Value %d in column \"%s\" is out of range for Stata long (-2147483647..2147483620); "
						    "cast the column to BIGINT",
						    val, col_def.name);
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
					} else if (val_type == LogicalTypeId::UINTEGER) {
						val = static_cast<double>(FlatVector::GetData<uint32_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::INTEGER) {
						val = static_cast<double>(FlatVector::GetData<int32_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::SMALLINT) {
						val = static_cast<double>(FlatVector::GetData<int16_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::TINYINT) {
						val = static_cast<double>(FlatVector::GetData<int8_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::HUGEINT) {
						val = Hugeint::Cast<double>(FlatVector::GetData<hugeint_t>(vec)[row]);
					} else if (val_type == LogicalTypeId::DECIMAL) {
						double divisor = std::pow(10.0, DecimalType::GetScale(vec.GetType()));
						switch (vec.GetType().InternalType()) {
						case PhysicalType::INT16:
							val = FlatVector::GetData<int16_t>(vec)[row] / divisor;
							break;
						case PhysicalType::INT32:
							val = FlatVector::GetData<int32_t>(vec)[row] / divisor;
							break;
						case PhysicalType::INT64:
							val = FlatVector::GetData<int64_t>(vec)[row] / divisor;
							break;
						default:
							val = Hugeint::Cast<double>(FlatVector::GetData<hugeint_t>(vec)[row]) / divisor;
							break;
						}
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
						// v = 1-based column index, o = 1-based observation index;
						// identical strings reuse the first occurrence's reference.
						// Stored as v(3 bytes) + o(5 bytes) little-endian per format 119
						auto ref = writer.AddStrL(static_cast<uint32_t>(col + 1), obs_idx, s);
						for (int b = 0; b < 3; b++) {
							dest[b] = static_cast<char>((ref.v >> (8 * b)) & 0xFF);
						}
						for (int b = 0; b < 5; b++) {
							dest[3 + b] = static_cast<char>((ref.o >> (8 * b)) & 0xFF);
						}
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
