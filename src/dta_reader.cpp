#include "dta_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dta {

const std::string DtaReader::empty_strl_;

// ─── Version params ─────────────────────────────────────────────────────────

DtaVersionParams DtaVersionParams::ForVersion(int version) {
	DtaVersionParams p;
	p.version = version;
	switch (version) {
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
	case 32768: return 8;  // strL: 8-byte (v,o) reference
	case 65525: return 0;  // alias: no data
	case 65526: return 8;  // double
	case 65527: return 4;  // float
	case 65528: return 4;  // long
	case 65529: return 2;  // int
	case 65530: return 1;  // byte
	default:
		throw std::runtime_error("Unknown .dta type code: " + std::to_string(type_code));
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
	if (fread(buf, 1, n, fp_) != n) {
		throw std::runtime_error("Unexpected end of .dta file");
	}
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
	return std::string(buf.begin(), end);
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

DtaReader::DtaReader(const std::string &path)
    : fp_(nullptr), msf_(false), n_obs_(0), row_width_(0), data_offset_(0), strls_offset_(0),
      value_labels_offset_(0) {
	fp_ = fopen(path.c_str(), "rb");
	if (!fp_) {
		throw std::runtime_error("Cannot open .dta file: " + path);
	}

	ParseHeader();
	ParseMap();
	ParseVariableTypes();
	ParseVarnames();
	// Skip sortlist
	uint32_t n_vars_for_sort = static_cast<uint32_t>(columns_.size());
	ReadTag("<sortlist>");
	fseek(fp_, (n_vars_for_sort + 1) * params_.sortlist_entry_size, SEEK_CUR);
	ReadTag("</sortlist>");
	ParseFormats();
	ParseValueLabelNames();
	ParseVariableLabels();
	SkipCharacteristics();

	// Compute row width
	row_width_ = 0;
	for (auto &col : columns_) {
		row_width_ += col.byte_width;
	}
}

DtaReader::~DtaReader() {
	if (fp_) {
		fclose(fp_);
	}
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
	}
	ReadTag("</label>");

	// Timestamp (skip)
	ReadTag("<timestamp>");
	uint8_t ts_len;
	ReadBytes(&ts_len, 1);
	if (ts_len > 0) {
		fseek(fp_, ts_len, SEEK_CUR);
	}
	ReadTag("</timestamp>");

	ReadTag("</header>");

	// Pre-allocate columns
	columns_.resize(n_vars);
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
	data_offset_ = offsets[9];
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

	// Filter out alias variables (type 65525) for formats 120/121
	if (params_.has_alias_vars) {
		std::vector<DtaColumn> filtered;
		for (size_t i = 0; i < raw_types.size(); i++) {
			if (raw_types[i] != 65525) {
				columns_[i].type_code = raw_types[i];
				columns_[i].byte_width = DtaTypeByteWidth(raw_types[i]);
				filtered.push_back(columns_[i]);
			}
		}
		// We need to track original indices for varnames/formats/labels parsing
		// For now, store all columns then filter after all metadata is parsed
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
	// Use the data_offset_ from <map> to skip directly past <characteristics>
	// data_offset_ points to "<data>", so we just seek there
	fseek(fp_, static_cast<long>(data_offset_), SEEK_SET);
}

// ─── Data reading ───────────────────────────────────────────────────────────

size_t DtaReader::ReadRows(uint64_t start_row, uint32_t count, std::vector<char> &buffer) {
	if (start_row >= n_obs_) {
		return 0;
	}
	uint32_t actual = static_cast<uint32_t>(std::min(static_cast<uint64_t>(count), n_obs_ - start_row));

	// Seek past <data> tag to the actual data content
	// data_offset_ points to the start of "<data>"
	// "<data>" is 6 bytes
	uint64_t data_content_offset = data_offset_ + 6;
	uint64_t byte_offset = data_content_offset + start_row * row_width_;

	fseek(fp_, static_cast<long>(byte_offset), SEEK_SET);

	size_t total_bytes = static_cast<size_t>(actual) * row_width_;
	buffer.resize(total_bytes);
	ReadBytes(buffer.data(), total_bytes);
	return actual;
}

// ─── strL support ───────────────────────────────────────────────────────────

void DtaReader::LoadStrLs() {
	fseek(fp_, static_cast<long>(strls_offset_), SEEK_SET);
	ReadTag("<strls>");

	while (true) {
		// Try to read "GSO" or "</strls>"
		char marker[4];
		size_t n = fread(marker, 1, 3, fp_);
		if (n < 3) {
			break;
		}

		if (memcmp(marker, "GSO", 3) != 0) {
			// Must be "</strls>" — seek back
			fseek(fp_, -3, SEEK_CUR);
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

		std::string content(len, '\0');
		if (len > 0) {
			ReadBytes(&content[0], len);
		}
		// Remove trailing null if present
		if (!content.empty() && content.back() == '\0') {
			content.pop_back();
		}

		// Pack (v, o) into a single 64-bit key
		uint64_t key = (static_cast<uint64_t>(v) << 32) | (o & 0xffffffffULL);
		strl_table_[key] = std::move(content);
	}
}

const std::string &DtaReader::ResolveStrL(uint32_t v, uint64_t o) const {
	uint64_t key = (static_cast<uint64_t>(v) << 32) | (o & 0xffffffffULL);
	auto it = strl_table_.find(key);
	if (it != strl_table_.end()) {
		return it->second;
	}
	return empty_strl_;
}

// ─── Value labels ───────────────────────────────────────────────────────────

void DtaReader::LoadValueLabels() {
	fseek(fp_, static_cast<long>(value_labels_offset_), SEEK_SET);
	ReadTag("<value_labels>");

	while (true) {
		// Try to read "<lbl>" or "</value_labels>"
		char marker[6];
		size_t n = fread(marker, 1, 5, fp_);
		if (n < 5) {
			break;
		}

		if (memcmp(marker, "<lbl>", 5) != 0) {
			fseek(fp_, -static_cast<long>(n), SEEK_CUR);
			ReadTag("</value_labels>");
			return;
		}

		// len (4 bytes) — total length of what follows until </lbl>
		uint32_t total_len = ReadU32();

		// labname (label_name_len bytes, null-terminated)
		std::string labname = ReadFixedString(params_.label_name_len);

		// 3 bytes padding
		fseek(fp_, 3, SEEK_CUR);

		// value_label_table: n(4), txtlen(4), off[n](4*n), val[n](4*n), txt[txtlen]
		uint32_t n_entries = ReadU32();
		uint32_t txtlen = ReadU32();

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
				std::string label(&txt[off[i]]);
				vl.mappings[val[i]] = std::move(label);
			}
		}

		value_labels_.push_back(std::move(vl));

		ReadTag("</lbl>");
	}
}

} // namespace dta
