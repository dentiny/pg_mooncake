CREATE PROCEDURE mooncake.reset_duckdb()
    SET search_path = pg_catalog, pg_temp
    LANGUAGE C AS 'MODULE_PATHNAME', 'mooncake_reset_duckdb';

-- Single delta update record.
-- [file_paths], [file_sizes], [is_add_files] are arrays of the same non-zero length.
CREATE TYPE delta_update_record AS (
    -- Directory, where delta table and parquet files are stored.
    path TEXT,
    -- Json format for delta table options.
    delta_option TEXT,
    -- Array of parquet file paths.
    file_paths TEXT[],
    -- Array of parquet file sizes.
    file_sizes BIGINT[],
    -- Array of whether it's add files flag.
    is_add_files BOOLEAN[]
);

CREATE TABLE mooncake.delta_update_records (
    -- OID for the table.
    oid OID PRIMARY KEY,
    -- Delta table update records.
    records delta_update_record[] NOT NULL
);
