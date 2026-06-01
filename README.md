# dta: A DuckDB Extension for Reading and Writing Stata Files

A [DuckDB](https://duckdb.org/) extension for reading and writing Stata `.dta` files (formats 117--121, corresponding to Stata 13--18).

## Usage

```sql
-- Read a .dta file
SELECT * FROM read_dta('auto.dta');

-- Read with value labels converted to DuckDB ENUMs
SELECT * FROM read_dta('auto.dta', value_labels=true);

-- Write a table to .dta
COPY my_table TO 'output.dta' (FORMAT dta);
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

The writer always produces format 118 (Stata 14).

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
