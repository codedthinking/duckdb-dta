#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace dta {

struct DtaWriteColumn {
	std::string name;              // max 32 UTF-8 chars (129 bytes)
	uint16_t type_code;            // 1-2045=str, 32768=strL, 65526-65530=numeric
	uint16_t byte_width;           // bytes per observation
	std::string format;            // display format (e.g. "%9.0g", "%td")
	std::string value_label_name;
	std::string label;             // variable label
};

struct WriterValueLabel {
	std::string name;
	std::unordered_map<int32_t, std::string> mappings;
};

struct StrLEntry {
	uint32_t v;   // 1-based variable index
	uint64_t o;   // 1-based observation index
	std::string value;
};

class DtaWriter {
public:
	DtaWriter(const std::string &path, const std::vector<DtaWriteColumn> &columns,
	          const std::string &dataset_label = "");
	~DtaWriter();

	// Call after construction, before writing data rows
	void WriteMetadata();

	// Write raw row data (row_width bytes per row, already packed)
	void WriteRowData(const char *data, size_t n_bytes);

	// Add a strL entry (call during sink, before Finalize)
	void AddStrL(uint32_t v, uint64_t o, const std::string &value);

	// Add a value label definition (call before Finalize)
	void AddValueLabel(const WriterValueLabel &vl);

	// Close the data section and write strls, value labels, map, footer
	void Finalize(uint64_t total_obs);

	uint32_t RowWidth() const { return row_width_; }

private:
	FILE *fp_;
	std::vector<DtaWriteColumn> columns_;
	std::string dataset_label_;
	uint32_t row_width_;

	// File positions for <map> (14 entries)
	uint64_t offsets_[14];
	uint64_t map_content_pos_; // file position where the 14 offsets start

	std::vector<StrLEntry> strl_entries_;
	std::vector<WriterValueLabel> value_labels_;

	void WriteTag(const char *tag);
	void WriteFixed(const std::string &s, uint32_t len);
	void WriteU16(uint16_t v);
	void WriteU32(uint32_t v);
	void WriteU64(uint64_t v);
};

} // namespace dta
