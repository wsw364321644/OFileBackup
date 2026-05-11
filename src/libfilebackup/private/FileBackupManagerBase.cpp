#include "FileBackupManagerBase.h"

#include <string_convert.h>
#include <FunctionExitHelper.h>
#include <std_ext.h>
#include <simple_error.h>

#include <glob/glob.hpp>
#include <xxhash.h>
#include <zstd.h>
#include <filesystem>
#include <set>
#include <shared_mutex>
#include <list>
#include <array>
#include <mutex>
#include <algorithm>
#include <map>

CommonHandle32_t IFileBackupManagerBase::GenFolderChunkData(const char8_t* path, TGenFolderMetaDataStatusChangedDelegate Delegate)
{
    std::filesystem::path folderPath(path);
    if (!std::filesystem::exists(folderPath)) {
        return NullHandle;
    }
    auto [pair, res] = GenFolderMetaDataWorkDataList.try_emplace(CommonHandle32_t::atomic_count, std::make_shared<GenFolderChunkDataWorkData_t>());
    if (!res) {
        return NullHandle;
    }
    auto& GenFolderMetaDataWorkData = pair->second;
    auto fileMapping=GenFolderMetaDataWorkData->Params.FileMappings.emplace_back();
    fileMapping.RootPath = ConvertU8ViewToView(path);
    fileMapping.RelativeGlobPath = "*";
    fileMapping.bRecursive = true;
    fileMapping.TargetRelativePath = ".";
    GenFolderMetaDataWorkData->StatusChangedDelegate = Delegate;
    GenFolderMetaDataWorkData->OutProcess = std::make_shared<GenFolderMetaDataProcess_t>();
    GenFolderMetaDataWorkData->OutFolderManifest = std::make_shared<FolderManifest_t>();
    GenFolderMetaDataWorkData->AllHashMap.reserve(1 << 20);
    return pair->first;
}

CommonHandle32_t IFileBackupManagerBase::GenFolderChunkData(GenFolderChunkParams_t& params, TGenFolderMetaDataStatusChangedDelegate Delegate)
{
    auto [pair, res] = GenFolderMetaDataWorkDataList.try_emplace(CommonHandle32_t::atomic_count, std::make_shared<GenFolderChunkDataWorkData_t>());
    if (!res) {
        return NullHandle;
    }
    auto& GenFolderMetaDataWorkData = pair->second;
    auto fileMapping = GenFolderMetaDataWorkData->Params= params;
    GenFolderMetaDataWorkData->StatusChangedDelegate = Delegate;
    GenFolderMetaDataWorkData->OutProcess = std::make_shared<GenFolderMetaDataProcess_t>();
    GenFolderMetaDataWorkData->OutFolderManifest = std::make_shared<FolderManifest_t>();
    GenFolderMetaDataWorkData->AllHashMap.reserve(1 << 20);
    return pair->first;
}

void IFileBackupManagerBase::CancelTask(CommonHandle32_t handle)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return;
    }
    auto& pFolderWorkData = itr->second;
    pFolderWorkData->Status= EGenFolderMetaDataStatus::Finished;
    pFolderWorkData->EC= utilpp::make_common_used_error(utilpp::ECommonUsedError::CUE_CANCELED);
}

void IFileBackupManagerBase::InitTask(CommonHandle32_t handle)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return;
    }
    auto& pFolderWorkData = itr->second;
    if (pFolderWorkData->Status != EGenFolderMetaDataStatus::None) {
        pFolderWorkData->Status = EGenFolderMetaDataStatus::Finished;
        pFolderWorkData->EC = utilpp::make_common_used_error(utilpp::ECommonUsedError::CUE_REQ_TOO_MANY);
        return;
    }
    std::vector<std::filesystem::path> paths;
    for (auto& FileMapping : pFolderWorkData->Params.FileMappings)
    {
        std::string RootPath(FileMapping.RootPath);
        if (glob::has_magic(RootPath)) {
            pFolderWorkData->Status = EGenFolderMetaDataStatus::Finished;
            pFolderWorkData->EC = std::make_error_code(std::errc::invalid_argument);
            return;
        }
        std::string GlobPath = ConvertU8ViewToString((std::filesystem::path(RootPath) / FileMapping.RelativeGlobPath).u8string());
        if (FileMapping.bRecursive) {
            paths = glob::rglob(GlobPath);
        }
        else {
            paths = glob::glob(GlobPath);
        }
        for (auto& p : paths) {
            auto pFileChunksData = std::make_shared<FileChunksData_t>();
            pFileChunksData->FileSize = std::filesystem::file_size(p);
            pFileChunksData->FileName = ConvertU8ViewToView((std::filesystem::path(FileMapping.TargetRelativePath)/ p.lexically_relative(RootPath)).lexically_normal().u8string());
            auto [itr, res] = pFolderWorkData->FolderManifest.Files.try_emplace(ConvertViewToU8View(pFileChunksData->FileName), pFileChunksData);
            if (!res) {
                pFolderWorkData->Status = EGenFolderMetaDataStatus::Finished;
                pFolderWorkData->EC = std::make_error_code(std::errc::invalid_argument);
                return;
            }
            pFolderWorkData->FileLocalPathMap.try_emplace(ConvertViewToU8View(pFileChunksData->FileName), ConvertU8ViewToView(p.u8string()));
            auto [fileListItr, _] = pFolderWorkData->FileItrList.try_emplace(pFileChunksData->FileSize);
            fileListItr->second.insert(ConvertViewToU8View(pFileChunksData->FileName));
            pFolderWorkData->ToltalSize += pFileChunksData->FileSize;
        }
    }
    pFolderWorkData->FolderManifest.HexNameLen = HexNameStrLen;
    auto pConverter = NewChunkConverter();
    pConverter->UpdateConvertDirection(EConvertDirection::ToChunkFile);
    pFolderWorkData->FolderManifest.ChunkFileMaxSize = pConverter->GetChunkFileMaxSize();
    pFolderWorkData->Status = EGenFolderMetaDataStatus::Inited;
}

bool IFileBackupManagerBase::GenFolderChunkDataAddHash(CommonHandle32_t handle, TGetNextHashPairCB CB)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return false;
    }
    auto& pFolderWorkData = itr->second;
    char hexName[HexNameStrLen + 1];
    uint32_t inHexNameStrLen{ HexNameStrLen };
    uint8_t hexBin[HexNameStrLen / 2];
    const char* strongHashBytes = (char*)hexBin + sizeof(WeakHash_t);
    while (CB((char8_t*)hexName, inHexNameStrLen)) {
        FunctionExitHelper_t helper([&]() {
            inHexNameStrLen = HexNameStrLen;
            });
        if (inHexNameStrLen != HexNameStrLen) {
            continue;
        }
        auto hexres = hex_to_bin(hexBin, hexName, inHexNameStrLen);
        if (!hexres) {
            continue;
        }
        auto [pair, res] = pFolderWorkData->AllHashMap.try_emplace(*reinterpret_cast<const WeakHash_t*>(hexBin), HashSetType{ std::string(strongHashBytes, StrongHashBit / 8) });
        if (!res) {
            pair->second.emplace(std::string_view(strongHashBytes, StrongHashBit / 8));
        }
    }
    return true;
}

std::shared_ptr<const GenFolderMetaDataProcess_t> IFileBackupManagerBase::GenFolderChunkDataGetProgress(CommonHandle32_t handle)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return nullptr;
    }
    auto& pFolderWorkData = itr->second;
    pFolderWorkData->OutProcess->CompleteSize = pFolderWorkData->CompleteSize;
    pFolderWorkData->OutProcess->TotalSize = pFolderWorkData->ToltalSize;
    pFolderWorkData->OutProcess->Status = pFolderWorkData->Status;
    return pFolderWorkData->OutProcess;
}

std::shared_ptr<const FolderManifest_t> IFileBackupManagerBase::GetFolderChunkData(CommonHandle32_t handle)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return nullptr;
    }
    auto& pFolderWorkData = itr->second;
    if (pFolderWorkData->Status != EGenFolderMetaDataStatus::Finished) {
        return nullptr;
    }
    *pFolderWorkData->OutFolderManifest = pFolderWorkData->FolderManifest;
    return pFolderWorkData->OutFolderManifest;
}

std::optional<std::reference_wrapper<std::unordered_map<std::u8string_view, std::string>>>  IFileBackupManagerBase::GetFolderChunkLocalFileMap(CommonHandle32_t handle)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return std::nullopt;
    }
    return itr->second->FileLocalPathMap;
}

void IFileBackupManagerBase::Tick(float delta)
{
    std::set<CommonHandle32_t> needDel;
    for (auto& [handle, pFolderWorkData] : GenFolderMetaDataWorkDataList) {
        if (pFolderWorkData->Status != pFolderWorkData->LastStatus) {
            pFolderWorkData->StatusChangedDelegate(pFolderWorkData->Status, pFolderWorkData->EC);
            pFolderWorkData->LastStatus = pFolderWorkData->Status;
        }
        switch (pFolderWorkData->Status) {
        case EGenFolderMetaDataStatus::None: {
            break;
        }
        case EGenFolderMetaDataStatus::Inited: {
            if (pFolderWorkData->FileItrList.empty() &&
                pFolderWorkData->FileTasks.empty()) {
                uint8_t uuid[UUID_128_BYTES];
                generate_uuid_128(uuid);
                to_upper_hex(pFolderWorkData->FolderManifest.ID, uuid, UUID_128_BYTES);
                pFolderWorkData->Status= EGenFolderMetaDataStatus::Finished;
            }
            break;
        }
        case EGenFolderMetaDataStatus::Finished: {
            needDel.insert(handle);
        }
        }
    }
    for (auto& handle : needDel) {
        GenFolderMetaDataWorkDataList.erase(handle);
    }
}

void IFileBackupManagerBase::GenFolderChunkDataReadFileTick(this IFileBackupManagerBase& self, float delta, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr<GenFolderChunkDataFileTaskData_t> pFileTaskData)
{
    auto caculateFileHash = [&](const unsigned char* content, uint32_t len) {
        XXH3_128bits_update(pFileTaskData->XXH3State, content, len);
        };
    FileChunkBuf_t& FileChunkBuf = *pFileTaskData->FileChunkBuf;
    if (!pFileTaskData->FileStream.eof()) {
        //std::unique_lock lock(pFileTaskData->FileChunkBufMtx, std::defer_lock);
        //lock.lock();
        auto freeBuf = FileChunkBuf.GetEmptyBuf();
        //lock.unlock();
        if (freeBuf.size() == 0) {
            return;
        }
        pFileTaskData->FileStream.read(freeBuf.data(), freeBuf.size());
        auto extractLen = pFileTaskData->FileStream.gcount();
        if (extractLen == 0) {
            return;
        }
        caculateFileHash((const unsigned char*)freeBuf.data(), extractLen);
        //lock.lock();
        FileChunkBuf.FillSize(extractLen);
        //lock.unlock();
    }
    else {
        if (pFileTaskData->WaitAppendDataLen > 0) {
            //std::unique_lock lock(pFileTaskData->FileChunkBufMtx, std::defer_lock);
            //lock.lock();
            auto freeBuf = FileChunkBuf.GetEmptyBuf();
            //lock.unlock();
            if (freeBuf.size() == 0) {
                return;
            }
            auto fillSize = std::min(freeBuf.size(), size_t(pFileTaskData->WaitAppendDataLen));
            memset(freeBuf.data(), 0, fillSize);
            pFileTaskData->WaitAppendDataLen -= fillSize;
            //lock.lock();
            FileChunkBuf.FillSize(fillSize);
            //lock.unlock();
        }
        pFileTaskData->bEOF = true;
    }
}

void IFileBackupManagerBase::GenFolderChunkDataPostProcessingTask(this IFileBackupManagerBase& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr<GenFolderChunkDataFileTaskData_t> pFileTaskData)
{
    for (auto& [weekHash, strongSet] : pFileTaskData->FileAllHashMap) {
        auto [itr, res] = pFolderWorkData->AllHashMap.try_emplace(weekHash, strongSet);
        if (res) {
            continue;
        }
        itr->second.merge(strongSet);
    }
    pFileTaskData->Clear();
    pFolderWorkData->FileTasks.erase(ConvertViewToU8View(pFileTaskData->FileChunksData->FileName));
    pFolderWorkData->FileTaskPool.push_back(pFileTaskData);
}
