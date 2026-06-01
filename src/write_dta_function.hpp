#pragma once

#include "duckdb.hpp"
#include "duckdb/function/copy_function.hpp"

namespace duckdb {

CopyFunction GetDtaCopyFunction();

} // namespace duckdb
