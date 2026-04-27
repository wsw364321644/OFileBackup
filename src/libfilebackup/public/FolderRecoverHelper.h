#pragma once

#include <handle.h>
#include <variant>
#include <optional>
#include <cstdlib>
#include <climits>
#include <simple_uuid.h>
#include <hex.h>
#include "FileBackupExportDef.h"
#include "FileBackupCommon.h"


class FolderRecoverProgress {
public:
    using FlieNameLenType = uint32_t;
    using FlieNameChType = char;
#pragma pack(push, 1)
    typedef struct FolderRecoverProgressHeader_t {
        //uint32_t AllDeleteFileNum{ 0 };
        uint32_t AllFileChunkNum{ 0 };
        uint32_t CompleteFileChunkCount{ 0 };
        uint32_t AllFileNum{ 0 };
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
    FolderRecoverProgressHeader_t& GetFolderRecoverProgressHeader() {
        auto& data = CharBuf.Data();
        return *(FolderRecoverProgressHeader_t*)data;
    }

    //FolderDeleteFileHeader_t& GetDeleteFileHeader(uint32_t index) {
    //    auto& data = CharBuf.Data();
    //    return *(FolderDeleteFileHeader_t*)((char*)data + sizeof(FolderRecoverProgressHeader_t) + index * sizeof(FolderDeleteFileHeader_t));
    //}

    //FolderRecoverFileProgressHeader_t& GetFileProgressHeader(uint32_t index) {
    //    auto& data = CharBuf.Data();
    //    return *(FolderRecoverFileProgressHeader_t*)((char*)&GetDeleteFileHeader(GetFolderRecoverProgressHeader().AllDeleteFileNum) + index * sizeof(FolderRecoverFileProgressHeader_t));
    //}
    FolderRecoverFileProgressHeader_t& GetFileProgressHeader(uint32_t index) {
        auto& data = CharBuf.Data();
        return *(FolderRecoverFileProgressHeader_t*)((char*)data + sizeof(FolderRecoverProgressHeader_t) + index * sizeof(FolderRecoverFileProgressHeader_t));
    }

    //std::basic_string_view<FlieNameChType> GetFileName(FolderDeleteFileHeader_t& FileHeader) {
    //    auto& data = CharBuf.Data();
    //    return { (char*)data + GetFolderRecoverProgressHeader().FileNameTableOffset + FileHeader.FileNameOffset,FileHeader.FileNameLen };
    //}
    //std::basic_string_view<FlieNameChType> GetFileName(FolderRecoverFileProgressHeader_t& FileProgressHeader) {
    //    auto& data = CharBuf.Data();
    //    return { (char*)data + GetFolderRecoverProgressHeader().FileNameTableOffset + FileProgressHeader.FileNameOffset,FileProgressHeader.FileNameLen };
    //}


    bool GetFileChunkStatus(FolderRecoverFileProgressHeader_t& FileProgressHeader, uint32_t index) {
        auto& data = CharBuf.Data();
        auto res = std::div(index + FileProgressHeader.FileChunkStatusBitOffset, CHAR_BIT);
        auto& targetByte = *((char*)data + GetFolderRecoverProgressHeader().FileChunkStatusTableOffset + FileProgressHeader.FileChunkStatusByteOffset + res.quot);
        return targetByte & (uint8_t(1) << res.rem);
    }
    void SetFileChunkStatus(FolderRecoverFileProgressHeader_t& FileProgressHeader, uint32_t index) {
        auto& data = CharBuf.Data();
        auto divRes = std::div(index + FileProgressHeader.FileChunkStatusBitOffset, CHAR_BIT);
        auto& targetByte = *((char*)data + GetFolderRecoverProgressHeader().FileChunkStatusTableOffset + FileProgressHeader.FileChunkStatusByteOffset + divRes.quot);
        targetByte |= (uint8_t(1) << divRes.rem);
    }

    virtual void Init(std::shared_ptr < const  FolderManifest_t> targetManifest, std::shared_ptr<const FolderManifest_t> source) = 0;
    FCharBuffer CharBuf;
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
    typedef std::function<void(EFolderRecoverStatus,std::error_code&)> TRecoverFoldeStatusChangedDelegate;
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
    virtual TRecoverTask GetRecoverBySourceTask(CommonHandle32_t) = 0;
    virtual TOneChunkRecoverTask GetRecoverByChunkTask(CommonHandle32_t, std::u8string_view) = 0;
    virtual IFolderRecoverHelperInterface::TFinishRecoverTask GetFinishRecoverTask(CommonHandle32_t) = 0;
    virtual void Tick(float delta) = 0;

};

LIB_FILEBACKUP_EXPORT IFolderRecoverHelperInterface* GetFolderRecoverHelperInstance();