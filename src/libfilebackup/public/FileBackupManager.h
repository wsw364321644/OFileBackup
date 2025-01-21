#pragma once
#include "FileBackupExportDef.h"
#include "FileBackupCommon.h"
#include <handle.h>

enum class EGenFolderMetaDataStatus
{
    None,
    Inited,
    Finished
};


typedef struct GenFolderMetaDataProcess_t {
    EGenFolderMetaDataStatus Status;
    uint64_t TotalSize;
    uint64_t CompleteSize;
}GenFolderMetaDataProcess_t;


class  IFileBackupManagerInterface {
public:

    typedef std::function<void(std::shared_ptr<const FolderManifest_t>)> TGenFolderMetaDataFinishDelegate;
    virtual CommonHandle_t GenFolderChunkData(const char8_t* path, TGenFolderMetaDataFinishDelegate Delegate) = 0;

    typedef std::function<bool(char8_t*,uint32_t&)> TGetNextHashPairCB;
    virtual bool GenFolderChunkDataAddHash(CommonHandle_t handle, TGetNextHashPairCB) = 0;

    typedef std::function<void()> TOneFileChunkDataTask;
    typedef std::function<void(const char8_t*, const uint32_t, const char8_t*, const uint32_t)> TNewFileChunkDelegate;
    virtual TOneFileChunkDataTask GenFolderChunkDataGetNextFileTask(CommonHandle_t handle, TNewFileChunkDelegate) = 0;

    virtual std::shared_ptr<const GenFolderMetaDataProcess_t> GenFolderChunkDataGetProcess(CommonHandle_t handle) = 0;

    //todo 
    // FileChunksData_t is writting in other thread
    //virtual std::shared_ptr<const FolderManifest_t> GetFolderChunkData(CommonHandle_t handle) = 0;

    virtual void Tick(float delta)=0;
};

LIB_FILEBACKUP_EXPORT IFileBackupManagerInterface* GetFileBackupManagerInstance();