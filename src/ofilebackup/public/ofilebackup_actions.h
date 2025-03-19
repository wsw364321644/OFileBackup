#pragma once
#include <FileBackupError.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <FileBackupCommon.h>
typedef struct CompleteChunkData_t{
    const char8_t* name;
    uint32_t namelen;
    const char* content;
    uint32_t contentlen;
}CompleteChunkData_t;
typedef struct GenProcessData_t{
    uint64_t TotalSize;
    uint64_t CompleteSize;
}GenProcessData_t;
typedef std::function<void(CompleteChunkData_t, GenProcessData_t)> TChunkCompleteDelegate;
std::tuple< bool, std::shared_ptr<const FolderManifest_t>> gen_folder_manifest_by_chunklist(std::u8string_view workPathStr, std::vector<std::string>& hexNameList, std::u8string_view chunkOutPathStr, TChunkCompleteDelegate Delegate=nullptr);
bool gen_folder_manifest_action(std::u8string_view workPath, std::u8string_view chunkListPathStr, std::u8string_view chunkOutPathStr, std::u8string_view manifestOutPathStr);
bool compare_folder_manifest(std::u8string_view sourcePath, std::u8string_view targetPath, std::u8string_view outFilePathStr);
EFileBackupError recover_folder(std::u8string_view workPathStr, std::u8string_view manifestFilePathStr, std::u8string_view sourceManifestFilePathStr, std::u8string_view chunkPathStr, std::u8string_view tempPathStr);
