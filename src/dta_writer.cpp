#include "dta_writer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dta {

// ─── Helpers ────────────────────────────────────────────────────────────────

void DtaWriter::WriteTag(const char *tag) {
	size_t len = strlen(tag);
	if (fwrite(tag, 1, len, fp_) != len) {
		throw std::runtime_error("Failed to write to .dta file");
	}
}

void DtaWriter::WriteFixed(const std::string &s, uint32_t len) {
	std::vector<char> buf(len, 0);
	size_t copy_len = std::min(static_cast<size_t>(len - 1), s.size());
	memcpy(buf.data(), s.data(), copy_len);
	if (fwrite(buf.data(), 1, len, fp_) != len) {
		throw std::runtime_error("Failed to write to .dta file");
	}
}

void DtaWriter::WriteU16(uint16_t v) {
	// Always LSF (native little-endian)
	if (fwrite(&v, 2, 1, fp_) != 1) {
		throw std::runtime_error("Failed to write to .dta file");
	}
}

void DtaWriter::WriteU32(uint32_t v) {
	if (fwrite(&v, 4, 1, fp_) != 1) {
		throw std::runtime_error("Failed to write to .dta file");
	}
}

void DtaWriter::WriteU64(uint64_t v) {
	if (fwrite(&v, 8, 1, fp_) != 1) {
		throw std::runtime_error("Failed to write to .dta file");
	}
}

// ─── Constructor / Destructor ───────────────────────────────────────────────

DtaWriter::DtaWriter(const std::string &path, const std::vector<DtaWriteColumn> &columns,
                     const std::string &dataset_label)
    : fp_(nullptr), columns_(columns), dataset_label_(dataset_label), row_width_(0), map_content_pos_(0) {
	memset(offsets_, 0, sizeof(offsets_));

	row_width_ = 0;
	for (auto &col : columns_) {
		row_width_ += col.byte_width;
	}

	fp_ = fopen(path.c_str(), "wb");
	if (!fp_) {
		throw std::runtime_error("Cannot create .dta file: " + path);
	}
}

DtaWriter::~DtaWriter() {
	if (fp_) {
		fclose(fp_);
		fp_ = nullptr;
	}
}

// ─── WriteMetadata ──────────────────────────────────────────────────────────
// Writes everything from <stata_dta> through <data> (opening tag only).
// After this, caller writes raw row data, then calls Finalize().

void DtaWriter::WriteMetadata() {
	// Format 118: K=2 bytes, varname=129, fmt=57, label_name=129, var_label=321,
	//             N=8 bytes, dataset_label_len=2 bytes, sortlist=2 bytes

	// <stata_dta>
	offsets_[0] = 0;
	WriteTag("<stata_dta>");

	// <header>
	WriteTag("<header>");
	WriteTag("<release>");
	WriteTag("118");
	WriteTag("</release>");
	WriteTag("<byteorder>");
	WriteTag("LSF");
	WriteTag("</byteorder>");
	WriteTag("<K>");
	WriteU16(static_cast<uint16_t>(columns_.size()));
	WriteTag("</K>");
	WriteTag("<N>");
	WriteU64(0); // placeholder, rewritten in Finalize
	WriteTag("</N>");

	// Dataset label
	WriteTag("<label>");
	uint16_t label_len = static_cast<uint16_t>(std::min(dataset_label_.size(), static_cast<size_t>(320)));
	WriteU16(label_len);
	if (label_len > 0) {
		fwrite(dataset_label_.data(), 1, label_len, fp_);
	}
	WriteTag("</label>");

	// Timestamp
	WriteTag("<timestamp>");
	uint8_t ts_len = 0;
	fwrite(&ts_len, 1, 1, fp_);
	WriteTag("</timestamp>");
	WriteTag("</header>");

	// <map> — placeholder offsets, rewritten in Finalize
	offsets_[1] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<map>");
	map_content_pos_ = static_cast<uint64_t>(ftell(fp_));
	for (int i = 0; i < 14; i++) {
		WriteU64(0);
	}
	WriteTag("</map>");

	// <variable_types>
	offsets_[2] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<variable_types>");
	for (auto &col : columns_) {
		WriteU16(col.type_code);
	}
	WriteTag("</variable_types>");

	// <varnames> (129 bytes each)
	offsets_[3] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<varnames>");
	for (auto &col : columns_) {
		WriteFixed(col.name, 129);
	}
	WriteTag("</varnames>");

	// <sortlist> (K+1 entries, 2 bytes each)
	offsets_[4] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<sortlist>");
	for (size_t i = 0; i <= columns_.size(); i++) {
		WriteU16(0);
	}
	WriteTag("</sortlist>");

	// <formats> (57 bytes each)
	offsets_[5] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<formats>");
	for (auto &col : columns_) {
		WriteFixed(col.format, 57);
	}
	WriteTag("</formats>");

	// <value_label_names> (129 bytes each)
	offsets_[6] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<value_label_names>");
	for (auto &col : columns_) {
		WriteFixed(col.value_label_name, 129);
	}
	WriteTag("</value_label_names>");

	// <variable_labels> (321 bytes each)
	offsets_[7] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<variable_labels>");
	for (auto &col : columns_) {
		WriteFixed(col.label, 321);
	}
	WriteTag("</variable_labels>");

	// <characteristics> (empty)
	offsets_[8] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<characteristics>");
	WriteTag("</characteristics>");

	// <data> — opening tag only; caller writes row data
	offsets_[9] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<data>");
}

// ─── WriteRowData ───────────────────────────────────────────────────────────

void DtaWriter::WriteRowData(const char *data, size_t n_bytes) {
	if (n_bytes > 0) {
		if (fwrite(data, 1, n_bytes, fp_) != n_bytes) {
			throw std::runtime_error("Failed to write row data to .dta file");
		}
	}
}

// ─── StrL ───────────────────────────────────────────────────────────────────

void DtaWriter::AddStrL(uint32_t v, uint64_t o, const std::string &value) {
	strl_entries_.push_back({v, o, value});
}

// ─── Value labels ───────────────────────────────────────────────────────────

void DtaWriter::AddValueLabel(const WriterValueLabel &vl) {
	value_labels_.push_back(vl);
}

// ─── Finalize ───────────────────────────────────────────────────────────────

void DtaWriter::Finalize(uint64_t total_obs) {
	// Close </data>
	WriteTag("</data>");

	// <strls>
	offsets_[10] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<strls>");
	for (auto &entry : strl_entries_) {
		WriteTag("GSO");
		WriteU32(entry.v);
		WriteU64(entry.o);
		uint8_t t = 130; // ASCII/UTF-8
		fwrite(&t, 1, 1, fp_);
		uint32_t len = static_cast<uint32_t>(entry.value.size() + 1); // include null terminator
		WriteU32(len);
		fwrite(entry.value.data(), 1, entry.value.size(), fp_);
		fwrite("\0", 1, 1, fp_); // null terminator
	}
	WriteTag("</strls>");

	// <value_labels>
	offsets_[11] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("<value_labels>");
	for (auto &vl : value_labels_) {
		// Sort entries by value
		std::vector<std::pair<int32_t, std::string>> sorted(vl.mappings.begin(), vl.mappings.end());
		std::sort(sorted.begin(), sorted.end());

		// Build txt table and off table
		std::vector<int32_t> vals;
		std::vector<uint32_t> offs;
		std::string txt_concat;
		for (auto &p : sorted) {
			vals.push_back(p.first);
			offs.push_back(static_cast<uint32_t>(txt_concat.size()));
			txt_concat += p.second;
			txt_concat += '\0';
		}

		uint32_t n = static_cast<uint32_t>(sorted.size());
		uint32_t txtlen = static_cast<uint32_t>(txt_concat.size());
		// Total length of value_label_table: 4(n) + 4(txtlen) + 4*n(off) + 4*n(val) + txtlen(txt)
		uint32_t table_len = 4 + 4 + 4 * n + 4 * n + txtlen;

		WriteTag("<lbl>");
		// len: total bytes after labname+padding = table_len
		// Actually: len covers labname(129) + padding(3) + table
		uint32_t total_len = 129 + 3 + table_len;
		WriteU32(total_len);

		// labname (129 bytes, null-terminated)
		WriteFixed(vl.name, 129);

		// 3 bytes padding
		char pad[3] = {0, 0, 0};
		fwrite(pad, 1, 3, fp_);

		// value_label_table
		WriteU32(n);
		WriteU32(txtlen);
		for (auto &o : offs) {
			WriteU32(o);
		}
		for (auto &v : vals) {
			uint32_t u;
			memcpy(&u, &v, 4);
			WriteU32(u);
		}
		fwrite(txt_concat.data(), 1, txtlen, fp_);

		WriteTag("</lbl>");
	}
	WriteTag("</value_labels>");

	// </stata_dta>
	offsets_[12] = static_cast<uint64_t>(ftell(fp_));
	WriteTag("</stata_dta>");
	offsets_[13] = static_cast<uint64_t>(ftell(fp_));

	// Rewrite <N> in header
	// <N> is at a fixed position. Let's find it:
	// <stata_dta><header><release>118</release><byteorder>LSF</byteorder><K>xx</K><N>
	// Tag lengths: <stata_dta>=11, <header>=8, <release>=9, 118=3, </release>=10,
	// <byteorder>=11, LSF=3, </byteorder>=12, <K>=3, 2bytes, </K>=4, <N>=3
	// Total to start of N content = 11+8+9+3+10+11+3+12+3+2+4+3 = 79
	uint64_t n_pos = 79;
	fseek(fp_, static_cast<long>(n_pos), SEEK_SET);
	WriteU64(total_obs);

	// Rewrite <map> offsets
	fseek(fp_, static_cast<long>(map_content_pos_), SEEK_SET);
	for (int i = 0; i < 14; i++) {
		WriteU64(offsets_[i]);
	}

	fclose(fp_);
	fp_ = nullptr;
}

} // namespace dta
