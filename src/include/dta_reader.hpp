#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace dta {

// Version-dependent parameters for .dta formats 117-121
struct DtaVersionParams {
	int version;                    // 117-121
	uint32_t varname_len;           // 33 (117) or 129 (118+)
	uint32_t sortlist_entry_size;   // 2 (117/118/120) or 4 (119/121)
	uint32_t fmt_len;               // 49 (117) or 57 (118+)
	uint32_t label_name_len;        // 33 (117) or 129 (118+)
	uint32_t var_label_len;         // 81 (117) or 321 (118+)
	uint32_t k_field_size;          // 2 (117/118/120) or 4 (119/121)
	uint32_t n_field_size;          // 4 (117) or 8 (118+)
	uint32_t dataset_label_len_size; // 1 (117) or 2 (118+)
	bool has_alias_vars;            // true for 120, 121

	static DtaVersionParams ForVersion(int version);
};

struct DtaColumn {
	std::string name;
	uint16_t type_code;        // 1-2045=str, 32768=strL, 65525=alias, 65526-65530=numeric
	uint16_t byte_width;       // bytes per observation for this variable
	std::string format;        // display format (e.g. "%td", "%tc", "%9.0g")
	std::string value_label_name;
	std::string label;         // variable label
};

struct DtaValueLabel {
	std::string name;
	std::unordered_map<int32_t, std::string> mappings; // val -> text
};

// Missing value sentinel boundaries (values >= these are missing)
struct DtaMissing {
	static bool IsMissingByte(int8_t val);
	static bool IsMissingInt(int16_t val);
	static bool IsMissingLong(int32_t val);
	static bool IsMissingFloat(float val);
	static bool IsMissingDouble(double val);
};

class DtaReader {
public:
	explicit DtaReader(const std::string &path);
	~DtaReader();

	// Metadata (available after construction)
	int Version() const { return params_.version; }
	bool IsMSF() const { return msf_; }
	uint64_t NumObs() const { return n_obs_; }
	uint32_t NumVars() const { return static_cast<uint32_t>(columns_.size()); }
	const std::vector<DtaColumn> &Columns() const { return columns_; }
	const std::string &DatasetLabel() const { return dataset_label_; }

	// Byte-swap a value according to the file's byte order
	template <typename T>
	T Swap(T val) const { return SwapIfNeeded(val); }

	// Data reading
	uint32_t RowWidth() const { return row_width_; }
	uint64_t DataOffset() const { return data_offset_; }
	size_t ReadRows(uint64_t start_row, uint32_t count, std::vector<char> &buffer);

	// strL support
	void LoadStrLs();
	const std::string &ResolveStrL(uint32_t v, uint64_t o) const;

	// Value labels
	void LoadValueLabels();
	const std::vector<DtaValueLabel> &ValueLabels() const { return value_labels_; }

private:
	FILE *fp_;
	DtaVersionParams params_;
	bool msf_;
	uint64_t n_obs_;
	std::string dataset_label_;
	std::vector<DtaColumn> columns_;
	uint32_t row_width_;
	uint64_t data_offset_;
	uint64_t strls_offset_;
	uint64_t value_labels_offset_;
	std::unordered_map<uint64_t, std::string> strl_table_; // packed(v,o) -> string
	std::vector<DtaValueLabel> value_labels_;
	static const std::string empty_strl_;

	void ParseHeader();
	void ParseMap();
	void ParseVariableTypes();
	void ParseVarnames();
	void ParseFormats();
	void ParseValueLabelNames();
	void ParseVariableLabels();
	void SkipCharacteristics();

	// I/O helpers
	template <typename T>
	T SwapIfNeeded(T val) const;
	void ReadTag(const char *expected);
	void ReadBytes(void *buf, size_t n);
	uint16_t ReadU16();
	uint32_t ReadU32();
	uint64_t ReadU64();
	std::string ReadFixedString(uint32_t len);
};

// Byte width for a given type code
uint16_t DtaTypeByteWidth(uint16_t type_code);

} // namespace dta
