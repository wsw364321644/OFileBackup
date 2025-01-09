#pragma once
#include "FileBackupExportDef.h"
#include <handle.h>
#include <ThroughCRTWrapper.h>
#include <memory>
#include <string>
#include <set>
#include <map>
#include <functional>

typedef uint32_t WeakHash_t;
constexpr uint32_t StrongHashBit = 1 << 7;
constexpr uint32_t HexNameStrLen = (sizeof(WeakHash_t) * CHAR_BIT + StrongHashBit) / 4 + 1;


enum EGenFolderMetaDataStatus
{
    None,
    Inited,
    Finished
};

typedef struct FileChunkData_t {
    char HexName[HexNameStrLen];
    uint64_t StartPos;
    bool operator < (const FileChunkData_t& other) const {
        return StartPos < other.StartPos;
    }
}FileChunkData_t;

typedef struct FileChunksData_t {
    char FileHash[StrongHashBit / 4 + 1];
    std::set<FileChunkData_t> Chunks;
}FileChunksData_t;

typedef struct FolderManifest_t {
    std::map<std::string, std::shared_ptr<FileChunksData_t>> Files;
    LIB_FILEBACKUP_EXPORT std::shared_ptr<const std::string> to_string() const;
    LIB_FILEBACKUP_EXPORT static std::shared_ptr<const FolderManifest_t>from_string(const char*);
}FolderManifest_t;




typedef struct GenFolderMetaDataProcess_t {
    EGenFolderMetaDataStatus Status;
    uint64_t TotalSize;
    uint64_t CompleteSize;
}GenFolderMetaDataProcess_t;

class  IFileBackupManagerInterface {
public:
    virtual std::shared_ptr<const FolderManifest_t> ParseFolderChunkData(const char* str) = 0;

    typedef std::function<void(std::shared_ptr<const FolderManifest_t>)> TGenFolderMetaDataFinishDelegate;
    virtual CommonHandle_t GenFolderChunkData(const char* path, TGenFolderMetaDataFinishDelegate Delegate) = 0;

    typedef std::function<bool(const char*&, const char*&)> TGetNextHashPairCB;
    virtual bool GenFolderChunkDataAddHash(CommonHandle_t handle, TGetNextHashPairCB) = 0;

    typedef std::function<void()> TOneFileChunkDataTask;
    typedef std::function<void(const char*, const uint32_t, const char*, const uint32_t)> TNewFileChunkDelegate;
    virtual TOneFileChunkDataTask GenFolderChunkDataGetNextFileTask(CommonHandle_t handle, TNewFileChunkDelegate) = 0;

    virtual std::shared_ptr<const GenFolderMetaDataProcess_t> GenFolderChunkDataGetProcess(CommonHandle_t handle) = 0;
    virtual std::shared_ptr<const FolderManifest_t> GetFolderChunkData(CommonHandle_t handle) = 0;
    virtual void Tick(float delta)=0;
};

LIB_FILEBACKUP_EXPORT IFileBackupManagerInterface* GetFileBackupManagerInstance();