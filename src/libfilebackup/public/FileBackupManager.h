#pragma once
#include "FileBackupExportDef.h"
#include "FileBackupCommon.h"
#include <handle.h>
#include <std_ext.h>
#include <span>
#include <optional>
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

typedef struct GenFolderChunkFileMapping_t {
    save_memory_operator_string RootPath;;
    save_memory_operator_string RelativeGlobPath;
    save_memory_operator_string TargetRelativePath;
    bool bRecursive{ false };
}GenFolderChunkFileMapping_t;

enum class ERecoverFileAttributes {
    RFA_None,
    RFA_Userconfig,//if exist do not overwrite
    RFA_Versionedconfig,//if exist do not be recovered
};
typedef struct GenFolderChunkFileAttributes_t {
    save_memory_operator_string Path;
    ERecoverFileAttributes Attributes;
}GenFolderChunkFileAttributes_t;

typedef struct GenFolderChunkParams_t {
    std::vector<GenFolderChunkFileMapping_t, allocator_save_memory_operator<GenFolderChunkFileMapping_t>> FileMappings;
    std::vector<GenFolderChunkFileAttributes_t, allocator_save_memory_operator<GenFolderChunkFileAttributes_t>> FileAttributes;
}GenFolderChunkParams_t;

class  IFileBackupManagerInterface {
public:

    typedef std::function<void(EGenFolderMetaDataStatus,std::error_code&)> TGenFolderMetaDataStatusChangedDelegate;
    virtual CommonHandle32_t GenFolderChunkData(const char8_t* path, TGenFolderMetaDataStatusChangedDelegate Delegate) = 0;
    virtual CommonHandle32_t GenFolderChunkData(GenFolderChunkParams_t& params, TGenFolderMetaDataStatusChangedDelegate Delegate) = 0;
    virtual void CancelTask(CommonHandle32_t handle) = 0;
    virtual void InitTask(CommonHandle32_t) = 0;
    typedef std::function<bool(char8_t*,uint32_t&)> TGetNextHashPairCB;
    virtual bool GenFolderChunkDataAddHash(CommonHandle32_t handle, TGetNextHashPairCB) = 0;

    //multithreading
    typedef std::function<void()> TOneFileChunkDataTask;
    //multithreading
    typedef std::function<void(float)> TOneFileChunkDataReadFileTick;
    //in tick thread
    typedef std::function<void()> TOneFileChunkDataPostProcessingTask;
    typedef std::function<void(IChunkConverter*, std::span<const char8_t>, std::span<const char>)> TNewFileChunkDelegate;
    /***
    * TOneFileChunkDataTask can parallel execute 
    * TOneFileChunkDataPostProcessingTask is not  multi-thread safe
    ***/
    virtual std::tuple<TOneFileChunkDataTask, TOneFileChunkDataReadFileTick, TOneFileChunkDataPostProcessingTask> GenFolderChunkDataGetNextFileTask(CommonHandle32_t handle, TNewFileChunkDelegate) = 0;

    virtual std::shared_ptr<const GenFolderMetaDataProcess_t> GenFolderChunkDataGetProgress(CommonHandle32_t handle) = 0;
    virtual std::shared_ptr<const FolderManifest_t> GetFolderChunkData(CommonHandle32_t handle) = 0;
    virtual std::optional<std::reference_wrapper<std::unordered_map<std::u8string_view, std::string>>>  GetFolderChunkLocalFileMap(CommonHandle32_t handle) = 0;

    virtual void Tick(float delta)=0;
};

LIB_FILEBACKUP_EXPORT IFileBackupManagerInterface* GetFileBackupManagerSingleton();