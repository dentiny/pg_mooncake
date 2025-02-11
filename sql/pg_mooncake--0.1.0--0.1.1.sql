CREATE PROCEDURE mooncake.reset_duckdb()
    SET search_path = pg_catalog, pg_temp
    LANGUAGE C AS 'MODULE_PATHNAME', 'mooncake_reset_duckdb';

CREATE TABLE mooncake.delta_update_records (
    -- Id of the delta table which automatically increases, to tell the order for update records.
    -- Intentionally we don't add any index to the table, because we cannot open index at commit stage.
    id BIGINT,
    -- Directory, where delta table and parquet files are stored.
    path TEXT,
    -- Json format for delta table options.
    delta_option TEXT,
    -- Array of parquet file paths.
    file_paths TEXT[],
    -- Array of parquet file sizes.
    file_sizes BIGINT[],
    -- Array of whether it's add files flag.
    is_add_files SMALLINT[]
);
