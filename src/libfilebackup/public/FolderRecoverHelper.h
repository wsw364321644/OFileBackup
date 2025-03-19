#pragma once
#include "FileBackupExportDef.h"
#include "FileBackupCommon.h"
#include <handle.h>
#include <variant>
#include <optional>
enum EFolderRecoverStatus
{
    FRS_None,
    FRS_ReserveSpace,
    FRS_ReconstructFile,
    FRS_MoveFile,
    FRS_Finished
};


typedef struct ReserveFileSpaceData_t {
    std::u8string_view  FileName;
    uint64_t FileSize{ 0 };
}ReserveFileSpaceData_t;

typedef struct ChunkFromFileSource_t {
    std::u8string_view  FileName;
    uint64_t StartPos{ 0 };
}ChunkFromFileSource_t;

typedef struct ConstructChunkData_t {
    std::variant<std::u8string_view, ChunkFromFileSource_t> ChunkSourceData;
    std::u8string_view  TagetFileName;
    uint64_t TagetFileSize{};
    uint64_t TagetFileStartPos{};
    IChunkConverter* ChunkConverter{ nullptr };
    uint8_t* FileChunkBuf{ nullptr };
    uint32_t ChunkFileMaxSize{ 0 };
}ConstructChunkData_t;


typedef struct FolderRecoverProgress_t {
    uint32_t AllFileChunkNum{ 0 };
    uint32_t AllFileNum{ 0 };
    uint32_t FileCount{ 0 };
    uint32_t FileChunkCount{ 0 };
}FolderRecoverProgress_t;



class  IFolderRecoverHelperInterface {
public:
    typedef std::function<void(EFolderRecoverStatus)> TConstructStatusChangedDelegate;
    virtual CommonHandle_t AddTask(std::shared_ptr <const FolderManifest_t> manifest, std::shared_ptr <const FolderManifest_t> sourceManifest, TConstructStatusChangedDelegate Delegate) = 0;
    virtual FolderRecoverProgress_t& GetFolderRecoverProcess(CommonHandle_t handle) = 0;

    virtual std::optional<const ReserveFileSpaceData_t> GetReserveNextFileSpaceData(CommonHandle_t) = 0;
    virtual void ReserveFileSpaceComplete(CommonHandle_t, ReserveFileSpaceData_t) = 0;
    virtual bool ImplementReserveFileSpace(CommonHandle_t,ReserveFileSpaceData_t& ConstructChunkData, std::u8string_view tempPathStr) = 0;

    virtual std::shared_ptr<const ConstructChunkData_t> GetConstructNextChunkData(CommonHandle_t) = 0;
    virtual void ChunkConstructComplete(CommonHandle_t, std::shared_ptr<const ConstructChunkData_t>) = 0;
    virtual bool ImplementForLocalChunkConstruct(CommonHandle_t,std::shared_ptr<const ConstructChunkData_t> ConstructChunkData, std::u8string_view workPathStr, std::u8string_view chunkFolderPathStr) = 0;

    virtual std::optional<std::u8string_view> GetNextFileNeedMove(CommonHandle_t) = 0;
    virtual void FileMoveComplete(CommonHandle_t, std::u8string_view) = 0;
    virtual bool ImplementFileMove(CommonHandle_t, std::u8string_view, std::u8string_view workPathStr) = 0;

    virtual void Tick(float delta) = 0;
};

LIB_FILEBACKUP_EXPORT IFolderRecoverHelperInterface* GetFolderRecoverHelperInstance();