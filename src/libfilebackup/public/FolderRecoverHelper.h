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
        uint32_t AllFileChunkNum{ 0 };
        uint32_t AllFileNum{ 0 };
        uint32_t FileNameTableOffset{ 0 };
        uint32_t FileChunkStatusTableOffset{ 0 };
        char TargetID[bin_to_hex_length(UUID_128_BYTES)];
        char SourceID[bin_to_hex_length(UUID_128_BYTES)];
        bool bTempFolderExist{ false };
    }FolderRecoverProgressHeader_t;

    typedef struct FolderRecoverFileProgressHeader_t {
        uint32_t FileNameOffset{ 0 };
        FlieNameLenType FileNameLen{ 0 };
        uint32_t FileChunkStatusByteOffset{ 0 };
        uint32_t FileChunkStatusBitOffset{ 0 };
        uint32_t ChunkNum{ 0 };
        bool bNeedRecover{ false };
    }FolderRecoverFileProgress_t;

#pragma pack(pop) 
    FolderRecoverProgressHeader_t& GetFolderRecoverProgressHeader() {
        auto& data = CharBuf.Data();
        return *(FolderRecoverProgressHeader_t*)data;
    }
    FolderRecoverFileProgressHeader_t& GetFileProgressHeader(uint32_t index) {
        auto& data = CharBuf.Data();
        return *(FolderRecoverFileProgressHeader_t*)((char*)data + sizeof(FolderRecoverProgressHeader_t) + index * sizeof(FolderRecoverFileProgressHeader_t));
    }
    std::basic_string_view<FlieNameChType> GetFileName(FolderRecoverFileProgressHeader_t& FileProgressHeader) {
        auto& data = CharBuf.Data();
        return { (char*)data + GetFolderRecoverProgressHeader().FileNameTableOffset + FileProgressHeader.FileNameOffset,FileProgressHeader.FileNameLen };
    }
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

    void Init(const FolderManifest_t& targetManifest) {

        CharBuf.Resize(sizeof(FolderRecoverProgressHeader_t));
        memset(&GetFolderRecoverProgressHeader(), 0, sizeof(FolderRecoverProgressHeader_t));
        GetFolderRecoverProgressHeader().AllFileNum = targetManifest.Files.size();
        GetFolderRecoverProgressHeader().FileNameTableOffset = GetFolderRecoverProgressHeader().FileChunkStatusTableOffset =
            sizeof(FolderRecoverProgressHeader_t) + GetFolderRecoverProgressHeader().AllFileNum * sizeof(FolderRecoverFileProgressHeader_t);
        for (auto& [fileName, pFileInfo] : targetManifest.Files) {
            auto& fileInfo = *pFileInfo;
            GetFolderRecoverProgressHeader().FileChunkStatusTableOffset += fileInfo.FileName.size();
            GetFolderRecoverProgressHeader().AllFileChunkNum += fileInfo.Chunks.size();
        }
        memcpy(GetFolderRecoverProgressHeader().TargetID, targetManifest.ID,sizeof(GetFolderRecoverProgressHeader().TargetID));
        auto divRes = std::div(GetFolderRecoverProgressHeader().AllFileChunkNum, CHAR_BIT);
        CharBuf.Resize(GetFolderRecoverProgressHeader().FileChunkStatusTableOffset + divRes.quot + (divRes.rem > 0 ? 1 : 0));
        memset(CharBuf.Data() + GetFolderRecoverProgressHeader().FileChunkStatusTableOffset, 0, divRes.quot + (divRes.rem > 0 ? 1 : 0));

        uint32_t FileNameOffsetCounr{ 0 };
        uint32_t FileChunkBitCount{ 0 };
        uint32_t FileChunkByteCount{ 0 };
        for (auto& [fileName, pFileInfo] : targetManifest.OrderedFiles) {
            auto& fileInfo = *pFileInfo;
            GetFileProgressHeader(fileInfo.Index).ChunkNum = fileInfo.Chunks.size();
            GetFileProgressHeader(fileInfo.Index).FileNameLen = fileInfo.FileName.size();
            GetFileProgressHeader(fileInfo.Index).FileNameOffset = FileNameOffsetCounr;
            GetFileProgressHeader(fileInfo.Index).FileChunkStatusByteOffset = FileChunkByteCount;
            GetFileProgressHeader(fileInfo.Index).FileChunkStatusBitOffset = FileChunkBitCount;
            FileNameOffsetCounr += fileInfo.FileName.size();
            divRes = std::div(fileInfo.Chunks.size() + FileChunkBitCount, CHAR_BIT);
            FileChunkByteCount += divRes.quot;
            FileChunkBitCount = divRes.rem;
            memcpy(CharBuf.Data() + GetFolderRecoverProgressHeader().FileNameTableOffset + GetFileProgressHeader(fileInfo.Index).FileNameOffset, fileInfo.FileName.data(), fileInfo.FileName.size());
        }
    }
    FCharBuffer CharBuf;
};


class  IFolderRecoverHelperInterface {
public:
    typedef std::function<void(std::error_code&)> TRecoverFolderFinishDelegate;
    virtual CommonHandle32_t AddTask(std::shared_ptr <const FolderManifest_t> manifest, std::shared_ptr <const FolderManifest_t> sourceManifest,std::u8string_view workDirStr, std::u8string_view chunkDirStr, std::u8string_view tempDirStr, TRecoverFolderFinishDelegate delegate) = 0;
    virtual std::tuple<uint32_t, uint32_t, std::optional<std::reference_wrapper<FolderRecoverProgress>>> GetFolderRecoverProcess(CommonHandle32_t handle) = 0;

    //multithreading
    typedef std::function<void()> TOneFileRecoverTask;
    //in tick thread
    typedef std::function<void()> TOneFileRecoverPostProcessingTask;
    virtual std::tuple<TOneFileRecoverTask, TOneFileRecoverPostProcessingTask> GetNextRecoverFileTask(CommonHandle32_t) = 0;

    virtual void Tick(float delta) = 0;
};

LIB_FILEBACKUP_EXPORT IFolderRecoverHelperInterface* GetFolderRecoverHelperInstance();