#include "dta_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dta {

const std::string DtaReader::empty_strl_;

// ─── Latin-1 → UTF-8 (pre-118 format strings) ──────────────────────────────

bool NeedsUtf8Transcode(const char *data, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (static_cast<unsigned char>(data[i]) >= 0x80) {
			return true;
		}
	}
	return false;
}

std::string Latin1ToUtf8(const char *data, size_t len) {
	std::string out;
	out.reserve(len * 2);
	for (size_t i = 0; i < len; i++) {
		unsigned char c = static_cast<unsigned char>(data[i]);
		if (c < 0x80) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back(static_cast<char>(0xC0 | (c >> 6)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

// ─── Version params ─────────────────────────────────────────────────────────

DtaVersionParams DtaVersionParams::ForVersion(int version) {
	DtaVersionParams p;
	p.version = version;
	switch (version) {
	case 113:
	case 114:
	case 115:
		// Legacy (pre-XML) layout: Stata 8-12
		p.varname_len = 33;
		p.sortlist_entry_size = 2;
		p.fmt_len = (version == 113) ? 12 : 49;
		p.label_name_len = 33;
		p.var_label_len = 81;
		p.k_field_size = 2;
		p.n_field_size = 4;
		p.dataset_label_len_size = 0; // fixed 81-byte field, no length prefix
		p.has_alias_vars = false;
		break;
	case 117:
		p.varname_len = 33;
		p.sortlist_entry_size = 2;
		p.fmt_len = 49;
		p.label_name_len = 33;
		p.var_label_len = 81;
		p.k_field_size = 2;
		p.n_field_size = 4;
		p.dataset_label_len_size = 1;
		p.has_alias_vars = false;
		break;
	case 118:
		p.varname_len = 129;
		p.sortlist_entry_size = 2;
		p.fmt_len = 57;
		p.label_name_len = 129;
		p.var_label_len = 321;
		p.k_field_size = 2;
		p.n_field_size = 8;
		p.dataset_label_len_size = 2;
		p.has_alias_vars = false;
		break;
	case 119:
		p.varname_len = 129;
		p.sortlist_entry_size = 4;
		p.fmt_len = 57;
		p.label_name_len = 129;
		p.var_label_len = 321;
		p.k_field_size = 4;
		p.n_field_size = 8;
		p.dataset_label_len_size = 2;
		p.has_alias_vars = false;
		break;
	case 120:
		p.varname_len = 129;
		p.sortlist_entry_size = 2;
		p.fmt_len = 57;
		p.label_name_len = 129;
		p.var_label_len = 321;
		p.k_field_size = 2;
		p.n_field_size = 8;
		p.dataset_label_len_size = 2;
		p.has_alias_vars = true;
		break;
	case 121:
		p.varname_len = 129;
		p.sortlist_entry_size = 4;
		p.fmt_len = 57;
		p.label_name_len = 129;
		p.var_label_len = 321;
		p.k_field_size = 4;
		p.n_field_size = 8;
		p.dataset_label_len_size = 2;
		p.has_alias_vars = true;
		break;
	default:
		throw std::runtime_error("Unsupported .dta format version: " + std::to_string(version));
	}
	return p;
}

// ─── Missing value detection ────────────────────────────────────────────────

bool DtaMissing::IsMissingByte(int8_t val) {
	return val > 100; // >= 101 (0x65)
}

bool DtaMissing::IsMissingInt(int16_t val) {
	return val > 32740; // >= 32741 (0x7fe5)
}

bool DtaMissing::IsMissingLong(int32_t val) {
	return val > 2147483620; // >= 0x7fffffe5
}

bool DtaMissing::IsMissingFloat(float val) {
	// Float missing: z > +1.fffffeX+7e  i.e. > max nonmissing
	// Use raw bit pattern: missing starts at 0x7f000000
	uint32_t bits;
	memcpy(&bits, &val, 4);
	return (bits & 0x7fffffff) >= 0x7f000000;
}

bool DtaMissing::IsMissingDouble(double val) {
	// Double missing: z > +1.fffffffffffffX+3fe
	// Missing starts at 0x7fe0000000000000
	uint64_t bits;
	memcpy(&bits, &val, 8);
	return (bits & 0x7fffffffffffffff) >= 0x7fe0000000000000ULL;
}

// ─── Byte width for type codes ──────────────────────────────────────────────

uint16_t DtaTypeByteWidth(uint16_t type_code) {
	if (type_code >= 1 && type_code <= 2045) {
		return type_code; // str# has width = #
	}
	switch (type_code) {
	case 32768:
		return 8; // strL: 8-byte (v,o) reference
	case 65525:
		return 0; // alias: no data
	case 65526:
		return 8; // double
	case 65527:
		return 4; // float
	case 65528:
		return 4; // long
	case 65529:
		return 2; // int
	case 65530:
		return 1; // byte
	default:
		throw std::runtime_error("Unknown .dta type code: " + std::to_string(type_code));
	}
}

// Legacy (113-115) one-byte type codes, translated to the modern equivalents
// so the rest of the reader is version-agnostic
static uint16_t TranslateLegacyType(uint8_t type_code) {
	if (type_code >= 1 && type_code <= 244) {
		return type_code; // str# has width = #
	}
	switch (type_code) {
	case 251:
		return 65530; // byte
	case 252:
		return 65529; // int
	case 253:
		return 65528; // long
	case 254:
		return 65527; // float
	case 255:
		return 65526; // double
	default:
		throw std::runtime_error("Unknown legacy .dta type code: " + std::to_string(type_code));
	}
}

// ─── Byte-swap helpers ──────────────────────────────────────────────────────

static uint16_t swap16(uint16_t val) {
	return (val >> 8) | (val << 8);
}

static uint32_t swap32(uint32_t val) {
	return ((val >> 24) & 0xff) | ((val >> 8) & 0xff00) | ((val << 8) & 0xff0000) | ((val << 24) & 0xff000000);
}

static uint64_t swap64(uint64_t val) {
	val = ((val >> 8) & 0x00ff00ff00ff00ffULL) | ((val << 8) & 0xff00ff00ff00ff00ULL);
	val = ((val >> 16) & 0x0000ffff0000ffffULL) | ((val << 16) & 0xffff0000ffff0000ULL);
	return (val >> 32) | (val << 32);
}

template <>
uint16_t DtaReader::SwapIfNeeded(uint16_t val) const {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return msf_ ? swap16(val) : val;
#else
	return msf_ ? val : swap16(val);
#endif
}

template <>
uint32_t DtaReader::SwapIfNeeded(uint32_t val) const {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return msf_ ? swap32(val) : val;
#else
	return msf_ ? val : swap32(val);
#endif
}

template <>
uint64_t DtaReader::SwapIfNeeded(uint64_t val) const {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return msf_ ? swap64(val) : val;
#else
	return msf_ ? val : swap64(val);
#endif
}

template <>
int16_t DtaReader::SwapIfNeeded(int16_t val) const {
	uint16_t u;
	memcpy(&u, &val, 2);
	u = SwapIfNeeded(u);
	int16_t result;
	memcpy(&result, &u, 2);
	return result;
}

template <>
int32_t DtaReader::SwapIfNeeded(int32_t val) const {
	uint32_t u;
	memcpy(&u, &val, 4);
	u = SwapIfNeeded(u);
	int32_t result;
	memcpy(&result, &u, 4);
	return result;
}

template <>
float DtaReader::SwapIfNeeded(float val) const {
	uint32_t u;
	memcpy(&u, &val, 4);
	u = SwapIfNeeded(u);
	float result;
	memcpy(&result, &u, 4);
	return result;
}

template <>
double DtaReader::SwapIfNeeded(double val) const {
	uint64_t u;
	memcpy(&u, &val, 8);
	u = SwapIfNeeded(u);
	double result;
	memcpy(&result, &u, 8);
	return result;
}

// ─── I/O helpers ────────────────────────────────────────────────────────────

void DtaReader::ReadBytes(void *buf, size_t n) {
	if (TryReadBytes(buf, n) != n) {
		throw std::runtime_error("Unexpected end of .dta file");
	}
}

size_t DtaReader::TryReadBytes(void *buf, size_t n) {
	char *ptr = static_cast<char *>(buf);
	size_t total = 0;
	while (total < n) {
		int64_t got = handle_->Read(ptr + total, n - total);
		if (got <= 0) {
			break;
		}
		total += static_cast<size_t>(got);
	}
	return total;
}

uint16_t DtaReader::ReadU16() {
	uint16_t val;
	ReadBytes(&val, 2);
	return SwapIfNeeded(val);
}

uint32_t DtaReader::ReadU32() {
	uint32_t val;
	ReadBytes(&val, 4);
	return SwapIfNeeded(val);
}

uint64_t DtaReader::ReadU64() {
	uint64_t val;
	ReadBytes(&val, 8);
	return SwapIfNeeded(val);
}

std::string DtaReader::ReadFixedString(uint32_t len) {
	std::vector<char> buf(len);
	ReadBytes(buf.data(), len);
	// Find null terminator
	auto end = std::find(buf.begin(), buf.end(), '\0');
	std::string result(buf.begin(), end);
	if (params_.version < 118 && NeedsUtf8Transcode(result.data(), result.size())) {
		return Latin1ToUtf8(result.data(), result.size());
	}
	return result;
}

void DtaReader::ReadTag(const char *expected) {
	size_t len = strlen(expected);
	std::vector<char> buf(len);
	ReadBytes(buf.data(), len);
	if (memcmp(buf.data(), expected, len) != 0) {
		throw std::runtime_error("Expected tag '" + std::string(expected) + "' not found in .dta file");
	}
}

// ─── Constructor ────────────────────────────────────────────────────────────

DtaReader::DtaReader(duckdb::FileSystem &fs, const std::string &path)
    : file_size_(0), msf_(false), n_obs_(0), row_width_(0), data_offset_(0), strls_offset_(0), value_labels_offset_(0) {
	handle_ = fs.OpenFile(path, duckdb::FileFlags::FILE_FLAGS_READ);
	file_size_ = handle_->GetFileSize();

	// Formats 117+ start with an XML-style tag; 113-115 start with the
	// format number as a single byte
	uint8_t first_byte;
	ReadBytes(&first_byte, 1);
	handle_->Seek(0);

	if (first_byte == '<') {
		ParseHeader();
		ParseMap();
		ParseVariableTypes();
		ParseVarnames();
		// Skip sortlist
		uint64_t n_vars_for_sort = columns_.size();
		ReadTag("<sortlist>");
		handle_->Seek(handle_->SeekPosition() + (n_vars_for_sort + 1) * params_.sortlist_entry_size);
		ReadTag("</sortlist>");
		ParseFormats();
		ParseValueLabelNames();
		ParseVariableLabels();
		SkipCharacteristics();
	} else {
		params_ = DtaVersionParams::ForVersion(first_byte);
		ParseLegacyHeader();
		ParseLegacyDescriptors();
		SkipExpansionFields(); // leaves data_offset_ at the row data
	}

	// Compute row width
	uint64_t width = 0;
	for (auto &col : columns_) {
		width += col.byte_width;
	}

	// The declared observations must fit between the data offset and end of file
	if (data_offset_ > file_size_ || width > std::numeric_limits<uint32_t>::max() ||
	    (width > 0 && n_obs_ > (file_size_ - data_offset_) / width)) {
		throw std::runtime_error("Corrupt .dta file: data section extends beyond end of file");
	}
	row_width_ = static_cast<uint32_t>(width);

	if (params_.version < 117) {
		// Legacy files have no map; value labels follow the data directly
		value_labels_offset_ = data_offset_ + n_obs_ * row_width_;
	}
}

DtaReader::~DtaReader() {
}

// ─── Header parsing ─────────────────────────────────────────────────────────

void DtaReader::ParseHeader() {
	ReadTag("<stata_dta>");
	ReadTag("<header>");

	// Release
	ReadTag("<release>");
	char release_buf[4] = {};
	ReadBytes(release_buf, 3);
	ReadTag("</release>");
	int version = atoi(release_buf);
	params_ = DtaVersionParams::ForVersion(version);

	// Byteorder
	ReadTag("<byteorder>");
	char bo_buf[4] = {};
	ReadBytes(bo_buf, 3);
	ReadTag("</byteorder>");
	msf_ = (memcmp(bo_buf, "MSF", 3) == 0);

	// K (number of variables)
	ReadTag("<K>");
	uint32_t n_vars;
	if (params_.k_field_size == 2) {
		n_vars = ReadU16();
	} else {
		n_vars = ReadU32();
	}
	ReadTag("</K>");

	// N (number of observations)
	ReadTag("<N>");
	if (params_.n_field_size == 4) {
		n_obs_ = ReadU32();
	} else {
		n_obs_ = ReadU64();
	}
	ReadTag("</N>");

	// Dataset label
	ReadTag("<label>");
	uint32_t label_len;
	if (params_.dataset_label_len_size == 1) {
		uint8_t ll;
		ReadBytes(&ll, 1);
		label_len = ll;
	} else {
		label_len = ReadU16();
	}
	if (label_len > 0) {
		dataset_label_.resize(label_len);
		ReadBytes(&dataset_label_[0], label_len);
		if (params_.version < 118 && NeedsUtf8Transcode(dataset_label_.data(), dataset_label_.size())) {
			dataset_label_ = Latin1ToUtf8(dataset_label_.data(), dataset_label_.size());
		}
	}
	ReadTag("</label>");

	// Timestamp (skip)
	ReadTag("<timestamp>");
	uint8_t ts_len;
	ReadBytes(&ts_len, 1);
	if (ts_len > 0) {
		handle_->Seek(handle_->SeekPosition() + ts_len);
	}
	ReadTag("</timestamp>");

	ReadTag("</header>");

	// Each variable needs at least this much metadata (type code, name, format,
	// value-label name, variable label), so a valid K is bounded by file size
	uint64_t min_bytes_per_var =
	    2ULL + params_.varname_len + params_.fmt_len + params_.label_name_len + params_.var_label_len;
	if (static_cast<uint64_t>(n_vars) * min_bytes_per_var > file_size_) {
		throw std::runtime_error("Corrupt .dta file: variable count " + std::to_string(n_vars) + " exceeds file size");
	}

	// Pre-allocate columns
	columns_.resize(n_vars);
}

// ─── Legacy (113-115) header parsing ────────────────────────────────────────

void DtaReader::ParseLegacyHeader() {
	// ds_format(1), byteorder(1: 0x01=MSF, 0x02=LSF), filetype(1), unused(1)
	uint8_t header_bytes[4];
	ReadBytes(header_bytes, 4);
	msf_ = (header_bytes[1] == 0x01);

	uint32_t n_vars = ReadU16();
	n_obs_ = ReadU32();

	// Dataset label (fixed 81 bytes) and timestamp (fixed 18 bytes)
	dataset_label_ = ReadFixedString(81);
	handle_->Seek(handle_->SeekPosition() + 18);

	// Each variable needs at least this much metadata (type code, name, format,
	// value-label name, variable label), so a valid K is bounded by file size
	uint64_t min_bytes_per_var =
	    1ULL + params_.varname_len + params_.fmt_len + params_.label_name_len + params_.var_label_len;
	if (static_cast<uint64_t>(n_vars) * min_bytes_per_var > file_size_) {
		throw std::runtime_error("Corrupt .dta file: variable count " + std::to_string(n_vars) + " exceeds file size");
	}

	columns_.resize(n_vars);
}

void DtaReader::ParseLegacyDescriptors() {
	// typlist: one byte per variable
	std::vector<uint8_t> raw_types(columns_.size());
	if (!raw_types.empty()) {
		ReadBytes(raw_types.data(), raw_types.size());
	}
	for (size_t i = 0; i < columns_.size(); i++) {
		columns_[i].type_code = TranslateLegacyType(raw_types[i]);
		columns_[i].byte_width = DtaTypeByteWidth(columns_[i].type_code);
	}

	// varlist
	for (auto &col : columns_) {
		col.name = ReadFixedString(params_.varname_len);
	}

	// srtlist: (nvar + 1) 2-byte entries
	handle_->Seek(handle_->SeekPosition() + (columns_.size() + 1) * params_.sortlist_entry_size);

	// fmtlist
	for (auto &col : columns_) {
		col.format = ReadFixedString(params_.fmt_len);
	}

	// lbllist
	for (auto &col : columns_) {
		col.value_label_name = ReadFixedString(params_.label_name_len);
	}

	// variable labels
	for (auto &col : columns_) {
		col.label = ReadFixedString(params_.var_label_len);
	}
}

void DtaReader::SkipExpansionFields() {
	// Sequence of {data_type(1), len(4), contents}, terminated by a
	// data_type of 0 with len 0
	while (true) {
		uint8_t data_type;
		ReadBytes(&data_type, 1);
		uint32_t len = ReadU32();
		if (data_type == 0 && len == 0) {
			break;
		}
		uint64_t pos = handle_->SeekPosition();
		if (pos > file_size_ || len > file_size_ - pos) {
			throw std::runtime_error("Corrupt .dta file: expansion field extends beyond end of file");
		}
		handle_->Seek(pos + len);
	}
	data_offset_ = handle_->SeekPosition();
}

// ─── Map ────────────────────────────────────────────────────────────────────

void DtaReader::ParseMap() {
	ReadTag("<map>");
	uint64_t offsets[14];
	for (int i = 0; i < 14; i++) {
		offsets[i] = ReadU64();
	}
	ReadTag("</map>");

	// offsets[9] = <data>, offsets[10] = <strls>, offsets[11] = <value_labels>
	data_offset_ = offsets[9] + 6; // skip past the "<data>" tag to the row data
	strls_offset_ = offsets[10];
	value_labels_offset_ = offsets[11];
}

// ─── Variable types ─────────────────────────────────────────────────────────

void DtaReader::ParseVariableTypes() {
	ReadTag("<variable_types>");
	std::vector<uint16_t> raw_types(columns_.size());
	for (size_t i = 0; i < columns_.size(); i++) {
		raw_types[i] = ReadU16();
	}
	ReadTag("</variable_types>");

	// Alias variables (type 65525, formats 120/121) carry no data; they are
	// kept through metadata parsing and filtered out in ParseVariableLabels
	if (params_.has_alias_vars) {
		for (size_t i = 0; i < columns_.size(); i++) {
			columns_[i].type_code = raw_types[i];
			columns_[i].byte_width = (raw_types[i] == 65525) ? 0 : DtaTypeByteWidth(raw_types[i]);
		}
	} else {
		for (size_t i = 0; i < columns_.size(); i++) {
			columns_[i].type_code = raw_types[i];
			columns_[i].byte_width = DtaTypeByteWidth(raw_types[i]);
		}
	}
}

// ─── Variable names ─────────────────────────────────────────────────────────

void DtaReader::ParseVarnames() {
	ReadTag("<varnames>");
	for (size_t i = 0; i < columns_.size(); i++) {
		columns_[i].name = ReadFixedString(params_.varname_len);
	}
	ReadTag("</varnames>");
}

// ─── Formats ────────────────────────────────────────────────────────────────

void DtaReader::ParseFormats() {
	ReadTag("<formats>");
	for (size_t i = 0; i < columns_.size(); i++) {
		columns_[i].format = ReadFixedString(params_.fmt_len);
	}
	ReadTag("</formats>");
}

// ─── Value-label names ──────────────────────────────────────────────────────

void DtaReader::ParseValueLabelNames() {
	ReadTag("<value_label_names>");
	for (size_t i = 0; i < columns_.size(); i++) {
		columns_[i].value_label_name = ReadFixedString(params_.label_name_len);
	}
	ReadTag("</value_label_names>");
}

// ─── Variable labels ────────────────────────────────────────────────────────

void DtaReader::ParseVariableLabels() {
	ReadTag("<variable_labels>");
	for (size_t i = 0; i < columns_.size(); i++) {
		columns_[i].label = ReadFixedString(params_.var_label_len);
	}
	ReadTag("</variable_labels>");

	// Now filter out alias variables if needed
	if (params_.has_alias_vars) {
		std::vector<DtaColumn> filtered;
		for (auto &col : columns_) {
			if (col.type_code != 65525) {
				filtered.push_back(std::move(col));
			}
		}
		columns_ = std::move(filtered);
	}
}

// ─── Characteristics (skip) ─────────────────────────────────────────────────

void DtaReader::SkipCharacteristics() {
	// Use the data offset from <map> to skip directly past <characteristics>
	handle_->Seek(data_offset_);
}

// ─── Data reading ───────────────────────────────────────────────────────────

size_t DtaReader::ReadRows(uint64_t start_row, uint32_t count, std::vector<char> &buffer) {
	if (start_row >= n_obs_) {
		return 0;
	}
	uint32_t actual = static_cast<uint32_t>(std::min(static_cast<uint64_t>(count), n_obs_ - start_row));

	uint64_t byte_offset = data_offset_ + start_row * row_width_;

	size_t total_bytes = static_cast<size_t>(actual) * row_width_;
	buffer.resize(total_bytes);
	// Positional read: no shared seek state, so concurrent scans are safe
	if (handle_->OnDiskFile()) {
		handle_->Read(buffer.data(), total_bytes, byte_offset);
	} else {
		std::lock_guard<std::mutex> guard(io_mutex_);
		handle_->Read(buffer.data(), total_bytes, byte_offset);
	}
	return actual;
}

// ─── strL support ───────────────────────────────────────────────────────────

uint64_t DtaReader::StrLKey(uint32_t v, uint64_t o) const {
	// Pack exactly as the data-cell layout does: v in the top bytes,
	// o in the remaining 8 - StrLVBytes() bytes
	int o_bits = 8 * (8 - StrLVBytes());
	return (static_cast<uint64_t>(v) << o_bits) | (o & ((1ULL << o_bits) - 1));
}

void DtaReader::LoadStrLs() {
	handle_->Seek(strls_offset_);
	ReadTag("<strls>");

	while (true) {
		// Try to read "GSO" or "</strls>"
		char marker[4];
		size_t n = TryReadBytes(marker, 3);
		if (n < 3) {
			break;
		}

		if (memcmp(marker, "GSO", 3) != 0) {
			// Must be "</strls>" — seek back
			handle_->Seek(handle_->SeekPosition() - 3);
			ReadTag("</strls>");
			return;
		}

		// GSO format: GSO + v(4 bytes) + o(8 bytes) + t(1 byte) + len(4 bytes) + content
		// v and o are encoded per byteorder
		uint32_t v;
		uint64_t o;

		if (params_.version == 117) {
			// v117: v is 4 bytes, o is 4 bytes
			v = ReadU32();
			o = ReadU32();
		} else {
			// v118+: v is 4 bytes, o is 8 bytes
			v = ReadU32();
			o = ReadU64();
		}

		uint8_t t; // type: 129=binary, 130=ASCII/UTF-8
		ReadBytes(&t, 1);

		uint32_t len = ReadU32();

		uint64_t strl_pos64 = handle_->SeekPosition();
		if (strl_pos64 > file_size_ || len > file_size_ - strl_pos64) {
			throw std::runtime_error("Corrupt .dta file: strL length extends beyond end of file");
		}

		std::string content(len, '\0');
		if (len > 0) {
			ReadBytes(&content[0], len);
		}
		if (t == 130) {
			// ASCII/UTF-8 strLs include a trailing NUL in len; binary
			// strLs (t=129) must keep every byte
			if (!content.empty() && content.back() == '\0') {
				content.pop_back();
			}
			if (params_.version < 118 && NeedsUtf8Transcode(content.data(), content.size())) {
				content = Latin1ToUtf8(content.data(), content.size());
			}
		}

		strl_table_[StrLKey(v, o)] = std::move(content);
	}
}

const std::string &DtaReader::ResolveStrL(uint32_t v, uint64_t o) const {
	auto it = strl_table_.find(StrLKey(v, o));
	if (it != strl_table_.end()) {
		return it->second;
	}
	return empty_strl_;
}

// ─── Value labels ───────────────────────────────────────────────────────────

void DtaReader::ReadValueLabelTable() {
	// labname (label_name_len bytes, null-terminated)
	std::string labname = ReadFixedString(params_.label_name_len);

	// 3 bytes padding
	handle_->Seek(handle_->SeekPosition() + 3);

	// value_label_table: n(4), txtlen(4), off[n](4*n), val[n](4*n), txt[txtlen]
	uint32_t n_entries = ReadU32();
	uint32_t txtlen = ReadU32();

	uint64_t lbl_pos64 = handle_->SeekPosition();
	uint64_t remaining = file_size_ > lbl_pos64 ? file_size_ - lbl_pos64 : 0;
	if (n_entries > remaining / 8 || txtlen > remaining - static_cast<uint64_t>(n_entries) * 8) {
		throw std::runtime_error("Corrupt .dta file: value label table extends beyond end of file");
	}

	std::vector<uint32_t> off(n_entries);
	for (uint32_t i = 0; i < n_entries; i++) {
		off[i] = ReadU32();
	}

	std::vector<int32_t> val(n_entries);
	for (uint32_t i = 0; i < n_entries; i++) {
		int32_t v;
		ReadBytes(&v, 4);
		val[i] = SwapIfNeeded(v);
	}

	std::vector<char> txt(txtlen);
	if (txtlen > 0) {
		ReadBytes(txt.data(), txtlen);
	}

	DtaValueLabel vl;
	vl.name = labname;
	for (uint32_t i = 0; i < n_entries; i++) {
		if (off[i] < txtlen) {
			// The text blob may lack a terminating NUL; never scan past it
			const char *base = txt.data() + off[i];
			std::string label(base, strnlen(base, txtlen - off[i]));
			if (params_.version < 118 && NeedsUtf8Transcode(label.data(), label.size())) {
				label = Latin1ToUtf8(label.data(), label.size());
			}
			vl.mappings[val[i]] = std::move(label);
		}
	}

	value_labels_.push_back(std::move(vl));
}

void DtaReader::LoadValueLabels() {
	if (params_.version < 117) {
		// Legacy: value label tables follow the data and repeat until EOF,
		// each prefixed by a 4-byte table length (unused here)
		handle_->Seek(value_labels_offset_);
		while (true) {
			char len_buf[4];
			if (TryReadBytes(len_buf, 4) < 4) {
				return;
			}
			ReadValueLabelTable();
		}
	}

	handle_->Seek(value_labels_offset_);
	ReadTag("<value_labels>");

	while (true) {
		// Try to read "<lbl>" or "</value_labels>"
		char marker[6];
		size_t n = TryReadBytes(marker, 5);
		if (n < 5) {
			break;
		}

		if (memcmp(marker, "<lbl>", 5) != 0) {
			handle_->Seek(handle_->SeekPosition() - n);
			ReadTag("</value_labels>");
			return;
		}

		// len (4 bytes) — total length of what follows until </lbl>
		ReadU32();

		ReadValueLabelTable();

		ReadTag("</lbl>");
	}
}

} // namespace dta
