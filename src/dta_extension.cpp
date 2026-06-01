#define DUCKDB_EXTENSION_MAIN

#include "dta_extension.hpp"
#include "read_dta_function.hpp"
#include "write_dta_function.hpp"
#include "duckdb.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GetReadDtaFunction());
	loader.RegisterFunction(GetDtaCopyFunction());
}

void DtaExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DtaExtension::Name() {
	return "dta";
}

std::string DtaExtension::Version() const {
#ifdef EXT_VERSION_DTA
	return EXT_VERSION_DTA;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dta, loader) {
	duckdb::LoadInternal(loader);
}
}
