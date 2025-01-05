#include "duckdb/common/file_system.hpp"
#include "pgduckdb/pgduckdb_xact.hpp"

extern "C" {
#include "postgres.h"

#include "fmgr.h"
}

void MooncakeInitGUC();
void DuckdbInitHooks();

bool mooncake_allow_local_tables = true;
char *mooncake_default_bucket = strdup("");
bool mooncake_enable_local_cache = true;

extern "C" {
PG_MODULE_MAGIC;

void DuckdbInitNode();

void _PG_init() {
    MooncakeInitGUC();
    DuckdbInitHooks();
    DuckdbInitNode();
    pgduckdb::RegisterDuckdbXactCallback();

    auto local_fs = duckdb::FileSystem::CreateLocal();
    local_fs->CreateDirectory("mooncake_local_cache");
    local_fs->CreateDirectory("mooncake_local_tables");
    // TODO(hjiang): One way to deal with unconstrained read cache size, is to cleanup read cache files based on their
    // modification timestamp at mooncake startup, with read cache "touched" at `GetFilePathsAndWarmCache`, so their
    // modification timestamp gets updated.
    //
    // But duckdb allows multiple instances issuing read request, the risk here is there's still slight chance another
    // duckdb session happens to read the oldest files. It's kind of acceptable since the data file on remote storage is
    // accessible, so a retry should work; we probably need to think of a fallback policy.
}
}
