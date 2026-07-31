#pragma once
#include "FileBackupCommon.h"
#include "FileBackupExportDef.h"

#include <FileBackedBuffer.h>
#include <handle.h>
#include <variant>
#include <optional>
#include <cstdlib>
#include <climits>
#include <simple_uuid.h>
#include <hex.h>



class FolderRecoverProgress {
public:
    using FlieNameLenType = uint32_t;
    using FlieNameChType = char;
#pragma pack(push, 1)
    typedef struct FolderRecoverProgressHeader_t {
        //uint32_t AllDeleteFileNum{ 0 };
        uint32_t AllFileChunkNum{ 0 };
        uint32_t AllFileNum{ 0 };
        uint32_t CompleteFileChunkCount{ 0 };
        uint32_t CompleteFileCount{ 0 };
        //uint32_t FileNameTableOffset{ 0 };
        uint32_t FileChunkStatusTableOffset{ 0 };
        char TargetID[bin_to_hex_length(UUID_128_BYTES)];
        char SourceID[bin_to_hex_length(UUID_128_BYTES)];
        bool bTempFolderExist{ false };
    }FolderRecoverProgressHeader_t;

    //typedef struct FolderDeleteFileHeader_t {
    //    uint32_t FileNameOffset{ 0 };
    //    FlieNameLenType FileNameLen{ 0 };
    //}FolderRecoverFileProgress_t;

    typedef struct FolderRecoverFileProgressHeader_t {
        //uint32_t FileNameOffset{ 0 };
        //FlieNameLenType FileNameLen{ 0 };
        uint32_t FileChunkStatusByteOffset{ 0 };
        uint32_t FileChunkStatusBitOffset{ 0 };
        uint32_t ChunkNum{ 0 };
    }FolderRecoverFileProgress_t;

#pragma pack(pop) 
    const FolderRecoverProgressHeader_t& GetFolderRecoverProgressHeader()const {
        return FileBackedBuffer->GetData<FolderRecoverProgressHeader_t>(0);
    }

    const FolderRecoverFileProgressHeader_t& GetFileProgressHeader(uint32_t index)const {
        return FileBackedBuffer->GetData<FolderRecoverFileProgressHeader_t>(sizeof(FolderRecoverProgressHeader_t) + index * sizeof(FolderRecoverFileProgressHeader_t));
    }

    bool GetFileChunkStatus(const FolderRecoverFileProgressHeader_t& FileProgressHeader, uint32_t index) const {
        auto divRes = std::div(index + FileProgressHeader.FileChunkStatusBitOffset, CHAR_BIT);
        auto& targetByte = FileBackedBuffer->GetData <uint8_t>(GetFolderRecoverProgressHeader().FileChunkStatusTableOffset + FileProgressHeader.FileChunkStatusByteOffset + divRes.quot);
        return targetByte & (uint8_t(1) << divRes.rem);
    }

    void SetFileChunkStatus(const FolderRecoverFileProgressHeader_t& FileProgressHeader, uint32_t index) {
        auto divRes = std::div(index + FileProgressHeader.FileChunkStatusBitOffset, CHAR_BIT);
        auto& targetByte = FileBackedBuffer->GetData <uint8_t>(GetFolderRecoverProgressHeader().FileChunkStatusTableOffset + FileProgressHeader.FileChunkStatusByteOffset + divRes.quot);
        auto newByte = targetByte;
        newByte |= (uint8_t(1) << divRes.rem);
        FileBackedBuffer->WriteData((void*)&targetByte, newByte);
    }

    void AddCompleteFileCount() {
        auto& header = GetFolderRecoverProgressHeader();
        FileBackedBuffer->WriteData(offsetof(FolderRecoverProgressHeader_t, CompleteFileCount), header.CompleteFileCount + 1);
    }
    void AddCompleteFileChunkCount() {
        auto& header = GetFolderRecoverProgressHeader();
        FileBackedBuffer->WriteData(offsetof(FolderRecoverProgressHeader_t, CompleteFileChunkCount), header.CompleteFileChunkCount + 1);
    }

    IFileBackedBuffer* FileBackedBuffer;
    std::unordered_set<std::u8string_view, string_hash>NeedRecoverMissingFileChunks;
    std::unordered_set<std::u8string_view, string_hash>NeedRecoverSourceFileChunks;
    std::shared_ptr <const FolderManifestCompareResult_t> CompareResult;
};

enum class EFolderRecoverStatus
{
    FRS_None,
    FRS_RecoverFile,
    FRS_FinishWork,
    FRS_Finished
};

class  IFolderRecoverHelperInterface {
public:
    typedef std::function<void(EFolderRecoverStatus,const std::error_code)> TRecoverFoldeStatusChangedDelegate;
    virtual CommonHandle32_t AddTask(std::shared_ptr <const FolderManifest_t> manifest, std::shared_ptr <const FolderManifest_t> sourceManifest,std::u8string_view workDirStr, std::u8string_view chunkDirStr, std::u8string_view tempDirStr, TRecoverFoldeStatusChangedDelegate delegate) = 0;
    virtual std::optional<std::reference_wrapper<FolderRecoverProgress>> GetFolderRecoverProcess(CommonHandle32_t handle) = 0;

    //multithreading
    typedef std::function<void()> TOneFileRecoverTask;
    typedef std::function<void()> TRecoverTask;
    typedef std::function<void()> TOneChunkRecoverTask;
    typedef std::function<void()> TFinishRecoverTask;
    //in tick thread
    typedef std::function<void()> TOneFileRecoverPostProcessingTask;
    typedef std::function<void()> TOneChunkRecoverPostProcessingTask;

    virtual std::tuple<TOneFileRecoverTask, TOneFileRecoverPostProcessingTask> GetNextRecoverFileTask(CommonHandle32_t) = 0;
    //virtual TRecoverTask GetRecoverBySourceTask(CommonHandle32_t) = 0;
    virtual TOneChunkRecoverTask GetRecoverByChunkTask(CommonHandle32_t, std::u8string_view,bool bFromSource=false) = 0;
    //virtual TFinishRecoverTask GetFinishRecoverTask(CommonHandle32_t) = 0;
    virtual void Tick(float delta) = 0;
    virtual void IOTick(float delta) = 0;

};

LIB_FILEBACKUP_EXPORT IFolderRecoverHelperInterface* GetFolderRecoverHelperInstance();