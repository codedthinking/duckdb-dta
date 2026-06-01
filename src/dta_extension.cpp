#define DUCKDB_EXTENSION_MAIN

#include "dta_extension.hpp"
#include "read_dta_function.hpp"
#include "write_dta_function.hpp"
#include "duckdb.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

static unique_ptr<TableRef> DtaReplacementScan(ClientContext &context, ReplacementScanInput &input,
                                               optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	if (!ReplacementScan::CanReplace(table_name, {"dta"})) {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>("read_dta", std::move(children));
	if (!FileSystem::HasGlob(table_name)) {
		auto &fs = FileSystem::GetFileSystem(context);
		table_function->alias = fs.ExtractBaseName(table_name);
	}
	return std::move(table_function);
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GetReadDtaFunction());
	loader.RegisterFunction(GetDtaCopyFunction());
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(DtaReplacementScan);
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
