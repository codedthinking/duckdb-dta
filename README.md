# dta: A DuckDB Extension for Reading and Writing Stata Files

A [DuckDB](https://duckdb.org/) extension for reading and writing Stata `.dta` files (formats 117--121, corresponding to Stata 13--18).

## Installation

Installation is simple through the DuckDB Community Extension repository, just type
```sql
INSTALL dta FROM community;
LOAD dta;
```

The extension adds the table function `read_dta` (also becomes the default reader for `.dta` files) and enables writing tables to `.dta` format with `COPY`.

## Usage

```sql
-- Read a .dta file
SELECT * FROM read_dta('auto.dta');

-- Infer the .dta file format and read with the default reader
SELECT * FROM 'auto.dta';

-- Read with value labels converted to DuckDB ENUMs
SELECT * FROM read_dta('auto.dta', value_labels=true);

-- Write a table to .dta
COPY my_table TO 'output.dta' (FORMAT dta);

-- Infer the .dta file format and write with the default writer
COPY my_table TO 'output.dta';
```

## Type Mapping

### Reading (.dta to DuckDB)

| Stata type   | DuckDB type |
|--------------|-------------|
| byte         | TINYINT     |
| int          | SMALLINT    |
| long         | INTEGER     |
| float        | FLOAT       |
| double       | DOUBLE      |
| double (%td) | DATE        |
| double (%tc) | TIMESTAMP   |
| str*N*, strL | VARCHAR     |

When `value_labels=true`, columns with value labels are returned as `ENUM`.

### Writing (DuckDB to .dta)

| DuckDB type            | Stata type   |
|------------------------|--------------|
| BOOLEAN, TINYINT       | byte         |
| SMALLINT               | int          |
| INTEGER                | long         |
| BIGINT, HUGEINT        | double       |
| FLOAT                  | float        |
| DOUBLE, DECIMAL        | double       |
| DATE                   | double (%td) |
| TIMESTAMP              | double (%tc) |
| VARCHAR                | strL         |
| ENUM                   | byte/int/long + value labels |

The writer produces format 119 (Stata 15), supporting up to 2 billion variables.

## Building

```sh
make
```

## Testing

```sh
make test
```

## License

MIT License. See [LICENSE](LICENSE) for details.

Stata is a registered trademark of StataCorp LLC. This extension is not affiliated with or endorsed by StataCorp LLC.
