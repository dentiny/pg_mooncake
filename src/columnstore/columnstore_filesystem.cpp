#include "columnstore/columnstore_filesystem.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/vector.hpp"
#include "pgmooncake_guc.hpp"
#include "utils/scope_guard.h"

namespace duckdb {

namespace {
// Get local cache filename for the given [remote_file].
string GetLocalCacheFile(const string &remote_file) {
    const string fname = StringUtil::GetFileName(remote_file);
    return x_mooncake_local_cache + fname;
}

// Columnstore read cache filesystem name.
constexpr const char *const kColumnstoreReadCacheFileSystemName = "ColumnstoreReadCacheFileSystem";
} // namespace

ColumnstoreReadCacheFileSystem::ColumnstoreReadCacheFileSystem(unique_ptr<FileSystem> internal_filesystem_p)
    : internal_filesystem(std::move(internal_filesystem_p)) {
    D_ASSERT(internal_filesystem);
    D_ASSERT(internal_filesystem->GetName() != kColumnstoreReadCacheFileSystemName);
}

std::string ColumnstoreReadCacheFileSystem::GetName() const {
    return kColumnstoreReadCacheFileSystemName;
}

unique_ptr<FileHandle> ColumnstoreReadCacheFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                                optional_ptr<FileOpener> opener) {
    // TODO(hjiang): If open for read, aggressively read the whole file synchronously and cache on local filesystem.
    // This implementation is initial and coarse, there're a few things to consider and optimize.
    // Reference: https://github.com/Mooncake-Labs/pg_mooncake/discussions/69#discussioncomment-11760100
    //
    // TODO(hjiang): Always enable caching for testing, revert before PR.
    // if (flags.OpenForReading() && IsRemoteFile(path) && mooncake_enable_local_cache) {
    if (flags.OpenForReading() && StringUtil::StartsWith(path, "/usr/local/pgsql/data")) {
        if (local_filesystem == nullptr) {
            local_filesystem = FileSystem::CreateLocal();
        }
        CacheRemoteFileIfEnabled(path, flags, opener);
        const string local_cache_file = GetLocalCacheFile(path);
        return local_filesystem->OpenFile(local_cache_file, flags, opener);
    }

    return internal_filesystem->OpenFile(path, flags, opener);
}

void ColumnstoreReadCacheFileSystem::CacheRemoteFileIfEnabled(const string &remote_path, const FileOpenFlags &flags,
                                                              optional_ptr<FileOpener> opener) {
    // Check whether cache file already exists, return if already exists.
    const string local_cache_file = GetLocalCacheFile(remote_path);
    if (local_filesystem->FileExists(local_cache_file)) {
        return;
    }

    // Read the whole content from remote file.
    vector<char> file_content;
    {
        auto file_handle = internal_filesystem->OpenFile(remote_path, flags, opener);
        SCOPE_EXIT {
            file_handle->Close();
        };
        const int64_t file_size = internal_filesystem->GetFileSize(*file_handle);
        // TODO(hjiang): It's better to use string and leverage resize without initialization.
        // Reference for abseil implementation:
        // https://github.com/abseil/abseil-cpp/blob/master/absl/strings/internal/resize_uninitialized.h
        file_content = vector<char>(file_size, '\0');
        internal_filesystem->Read(*file_handle, file_content.data(), file_size, /*location=*/0);
    }

    // Write the whole content into local cache file.
    const auto fname = StringUtil::GetFileName(remote_path);
    const auto local_temp_file =
        StringUtil::Format("%s%s.%s", x_mooncake_local_cache, fname, UUID::ToString(UUID::GenerateRandomUUID()));
    {
        auto file_handle = local_filesystem->OpenFile(
            local_temp_file, FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW, opener);
        SCOPE_EXIT {
            file_handle->Close();
        };
        local_filesystem->Write(*file_handle, file_content.data(), file_content.size(), /*location=*/0);
        file_handle->Sync();
    }

    // We could have multiple thread caching to the same file, cache to a temporary file, then atomically swap to
    // destination cache file.
    //
    // TODO(hjiang): We could leave temporary file on local filesystem, which we could cleanup at instance setup;
    // it will handled in the followup PR.
    local_filesystem->MoveFile(/*source=*/local_temp_file, /*target=*/local_cache_file);
}

unique_ptr<FileSystem> WrapColumnstoreReadCacheFileSystem(unique_ptr<FileSystem> internal_filesystem) {
    // We don't want recursive read-cache wrapper.
    if (internal_filesystem->GetName() == kColumnstoreReadCacheFileSystemName) {
        return internal_filesystem;
    }
    // TODO(hjiang): Always enable for testing purpose, revert before PR.
    // if (mooncake_enable_local_cache) {
    //     return make_uniq<ColumnstoreReadCacheFileSystem>(std::move(internal_filesystem));
    // }
    return make_uniq<ColumnstoreReadCacheFileSystem>(std::move(internal_filesystem));
}

} // namespace duckdb
