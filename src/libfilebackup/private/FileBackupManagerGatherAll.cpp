#include "FileBackupManagerGatherAll.h"

#include <string_convert.h>
#include <FunctionExitHelper.h>
#include <std_ext.h>

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

CommonHandle32_t FFileBackupManagerGatherAll::GenFolderChunkData(const char8_t* path, TGenFolderMetaDataFinishDelegate Delegate)
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
    GenFolderMetaDataWorkData->WorkPath = std::move(folderPath);
    GenFolderMetaDataWorkData->FinishDelegate = Delegate;
    GenFolderMetaDataWorkData->OutProcess = std::make_shared<GenFolderMetaDataProcess_t>();
    GenFolderMetaDataWorkData->OutFolderManifest = std::make_shared<FolderManifest_t>();
    GenFolderMetaDataWorkData->AllHashMap.reserve(1 << 20);
    return pair->first;
}

bool FFileBackupManagerGatherAll::GenFolderChunkDataAddHash(CommonHandle32_t handle, TGetNextHashPairCB CB)
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
        auto [pair, res] = pFolderWorkData->AllHashMap.try_emplace(*reinterpret_cast<WeakHash_t*>(hexBin), HashSetType{ std::string(strongHashBytes, StrongHashBit / 8) });
        if (!res) {
            pair->second.insert(strongHashBytes);
        }
    }
    return true;
}

std::tuple<IFileBackupManagerInterface::TOneFileChunkDataTask, IFileBackupManagerInterface::TOneFileChunkDataReadFileTick, IFileBackupManagerInterface::TOneFileChunkDataPostProcessingTask> FFileBackupManagerGatherAll::GenFolderChunkDataGetNextFileTask(CommonHandle32_t handle, TNewFileChunkDelegate NewFileChunkDelegate)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return { nullptr,nullptr,nullptr };
    }
    auto& pFolderWorkData = itr->second;
    if (pFolderWorkData->FileItrList.empty()) {
        return { nullptr,nullptr,nullptr };
    }
    auto& fileList = pFolderWorkData->FileItrList.rbegin()->second;
    auto& fileName = *fileList.begin();
    auto filesItr = pFolderWorkData->FolderManifest.Files.find(fileName);
    assert(filesItr != pFolderWorkData->FolderManifest.Files.end());
    auto& pFileChunksData = filesItr->second;

    if (fileList.size() > 1) {
        fileList.erase(fileName);
    }
    else {
        pFolderWorkData->FileItrList.erase(--pFolderWorkData->FileItrList.rbegin().base());
    }

    std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData;
    {
        //std::scoped_lock lock(pFolderWorkData->FileTaskMtx);
        if (pFolderWorkData->FileTaskPool.size() > 0) {
            pFileTaskData = pFolderWorkData->FileTaskPool.back();
            pFolderWorkData->FileTaskPool.pop_back();
            pFileTaskData->Clear();
        }
        else {
            pFileTaskData = std::make_shared<GenFolderChunkDataFileTaskData_t>();
            pFileTaskData->FileChunkBuf = std::make_shared<FileChunkBuf_t>();;
            pFileTaskData->ChunkConverter.UpdateConvertDirection(EConvertDirection::ToChunkFile);
            pFileTaskData->MD5Handle = CryptoLibMD5Begin();
            if (!pFileTaskData->MD5Handle.IsValid()) {
                return { nullptr,nullptr,nullptr };
            }
        }
        pFileTaskData->FileChunksData = pFileChunksData;
        auto [itr, res] = pFolderWorkData->FileTasks.try_emplace(ConvertViewToU8View(pFileTaskData->FileChunksData->FileName), pFileTaskData);
        if (!res) {
            return { nullptr,nullptr,nullptr };
        }
        pFileTaskData->NewFileChunkDelegate = NewFileChunkDelegate;

        pFileTaskData->FileAllHashMap = pFolderWorkData->AllHashMap;
        auto filePath = pFolderWorkData->WorkPath / pFileTaskData->FileChunksData->FileName;
        pFileTaskData->FileStream = std::ifstream(filePath, std::ios::binary);
        if (!pFileTaskData->FileStream.is_open()) {
            return { nullptr,nullptr,nullptr };
        }
        auto remainder = pFileTaskData->FileChunksData->FileSize % FileChunkSize;
        pFileTaskData->WaitAppendDataLen = remainder ? FileChunkSize - remainder : 0;
    }
    TOneFileChunkDataTask func = std::bind(&FFileBackupManagerGatherAll::GenFolderChunkDataTask, *this, pFolderWorkData, pFileTaskData);
    TOneFileChunkDataPostProcessingTask postfunc = std::bind(&FFileBackupManagerGatherAll::GenFolderChunkDataPostProcessingTask, *this, pFolderWorkData, pFileTaskData);
    TOneFileChunkDataReadFileTick readFileTick = std::bind(&FFileBackupManagerGatherAll::GenFolderChunkDataReadFileTick, *this, std::placeholders::_1, pFolderWorkData, pFileTaskData);
    return { func,readFileTick,postfunc };
}

std::shared_ptr<const GenFolderMetaDataProcess_t> FFileBackupManagerGatherAll::GenFolderChunkDataGetProcess(CommonHandle32_t handle)
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

std::shared_ptr<const FolderManifest_t> FFileBackupManagerGatherAll::GetFolderChunkData(CommonHandle32_t handle)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return nullptr;
    }
    auto& pFolderWorkData = itr->second;
    *pFolderWorkData->OutFolderManifest = pFolderWorkData->FolderManifest;
    return pFolderWorkData->OutFolderManifest;
}

void FFileBackupManagerGatherAll::Tick(float delta)
{
    std::set<CommonHandle32_t> needDel;
    for (auto& [handle, pFolderWorkData] : GenFolderMetaDataWorkDataList) {
        switch (pFolderWorkData->Status) {
        case EGenFolderMetaDataStatus::None: {
            for (auto i = std::filesystem::recursive_directory_iterator(pFolderWorkData->WorkPath);
                i != std::filesystem::recursive_directory_iterator();
                ++i)
            {
                if (i->is_directory()) {
                    continue;
                }
                pFolderWorkData->ToltalSize += i->file_size();
                auto pFileChunksData = std::make_shared<FileChunksData_t>();
                pFileChunksData->FileSize = i->file_size();
                pFileChunksData->FileName = (const char*)i->path().lexically_relative(pFolderWorkData->WorkPath).u8string().c_str();
                auto [itr, res] = pFolderWorkData->FolderManifest.Files.try_emplace(ConvertViewToU8View(pFileChunksData->FileName), pFileChunksData);
                if (!res) {
                    pFolderWorkData->Status = EGenFolderMetaDataStatus::Finished;
                    pFolderWorkData->Error = EGenFolderMetaDataError::GFMDE_FS_ERROR;
                    break;
                }
                auto [fileListItr, _] = pFolderWorkData->FileItrList.try_emplace(pFileChunksData->FileSize);
                fileListItr->second.insert(ConvertViewToU8View(pFileChunksData->FileName));
            }
            pFolderWorkData->FolderManifest.HexNameLen = HexNameStrLen;
            auto pConverter = NewChunkConverter();
            pConverter->UpdateConvertDirection(EConvertDirection::ToChunkFile);
            pFolderWorkData->FolderManifest.ChunkFileMaxSize = pConverter->GetChunkFileMaxSize();
            pFolderWorkData->Status = EGenFolderMetaDataStatus::Inited;
            break;
        }
        case EGenFolderMetaDataStatus::Inited: {
            if (pFolderWorkData->FileItrList.empty() &&
                pFolderWorkData->FileTasks.empty()) {

                uint8_t uuid[UUID_128_BYTES];
                generate_uuid_128(uuid);
                to_upper_hex(pFolderWorkData->FolderManifest.ID, uuid, UUID_128_BYTES);

                GetFolderChunkData(handle);
                pFolderWorkData->FinishDelegate(pFolderWorkData->OutFolderManifest);
                needDel.insert(handle);
            }
            break;
        }
        }
    }
    for (auto& handle : needDel) {
        GenFolderMetaDataWorkDataList.erase(handle);
    }
}

void FFileBackupManagerGatherAll::GenFolderChunkDataTask(this FFileBackupManagerGatherAll& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData)
{
    typedef struct FileChunkCache_t {
        uint64_t StartPos;
        uint32_t WeakHash;
        unsigned char StrongHash[16]{};
        bool fStrongHash;
        bool fChunkAlreadyExist;
    }FileChunkCache_t;
    FileChunkBuf_t& FileChunkBuf = *pFileTaskData->FileChunkBuf;

    typedef struct FileChunkCacheContainer_t {
        FileChunkCacheContainer_t(uint32_t num = 3) {
            for (int i = 0; i < num; i++) {
                chunkCachePool.push_back(std::make_shared<FileChunkCache_t>(FileChunkCache_t{}));
            }
        }
        std::shared_ptr<FileChunkCache_t> get_available() {
            FunctionExitHelper_t helper([&]() {
                chunkCachePool.pop_front();
                });
            return chunkCachePool.front();
        }
        std::shared_ptr<FileChunkCache_t>& back() {
            return chunkCaches.back();
        }
        std::shared_ptr<FileChunkCache_t>& front() {
            return chunkCaches.front();
        }
        auto begin() {
            return chunkCaches.begin();
        }
        void push_front(std::shared_ptr<FileChunkCache_t> pFileChunkCache) {
            return chunkCaches.push_front(pFileChunkCache);
        }
        void push_back(std::shared_ptr<FileChunkCache_t> pFileChunkCache) {
            return chunkCaches.push_back(pFileChunkCache);
        }
        void pop_front() {
            chunkCachePool.push_back(chunkCaches.front());
            return chunkCaches.pop_front();
        }
        void pop_back() {
            chunkCachePool.push_back(chunkCaches.back());
            return chunkCaches.pop_back();
        }
        size_t size() {
            return chunkCaches.size();
        }
        bool empty() {
            return chunkCaches.empty();
        }
        std::list<std::shared_ptr<FileChunkCache_t>> chunkCaches;
        std::list<std::shared_ptr<FileChunkCache_t>> chunkCachePool;
    }FileChunkCacheContainer_t;
    FileChunkCacheContainer_t fileChunkCacheContainer;

    unsigned char output[16];
    uint64_t lastChunkEndPos{ 0 };
    std::streamoff consumedBytes{ 0 };
    bool bFlushAllChunkCache{ false };
    int bytesAfterLastChunk = 0;

    RollingAdler32.Reset();
    auto& hasher = RollingAdler32;


    auto tryCacheFunc = [&]() {
        auto WeakHash = hasher.Get();
        bool bWeakExist{ false };
        bool bStrongExist{ false };
        auto hashItr = pFileTaskData->FileAllHashMap.find(WeakHash);
        char* rawData;
        if (hashItr != pFileTaskData->FileAllHashMap.end()) {
            bWeakExist = true;
            rawData = FileChunkBuf.GetContinuousConsumedBuf(0, FileChunkSize);
            auto hash = XXH3_128bits(rawData, FileChunkSize);
            CopyxxHashToBuf(hash, output);
            auto stronHashItr = hashItr->second.find(std::string_view((char*)output, sizeof(output)));
            if (stronHashItr != hashItr->second.end()) {
                bStrongExist = true;
            }
        }

        if (bytesAfterLastChunk >= FileChunkSize) {
            assert(bytesAfterLastChunk == FileChunkSize);
            if (!bWeakExist) {
                rawData = FileChunkBuf.GetContinuousConsumedBuf(0, FileChunkSize);
                auto hash = XXH3_128bits(FileChunkBuf.GetContinuousConsumedBuf(0, FileChunkSize), FileChunkSize);
                CopyxxHashToBuf(hash, output);
            }
            auto pChunkData = std::make_shared<FileChunkData_t>();
            auto& ChunkData = *pChunkData;
            ChunkData.StartPos = uint64_t(consumedBytes - FileChunkSize);
            to_upper_hex(ChunkData.HexName, (uint8_t*)&WeakHash, sizeof(WeakHash));
            to_upper_hex(ChunkData.HexName + bin_to_hex_length(sizeof(WeakHash_t)), output, sizeof(output));
            ChunkData.HexName[bin_to_hex_length(sizeof(WeakHash_t)) + bin_to_hex_length(sizeof(output))] = 0;
            pFileTaskData->FileChunksData->Chunks.emplace(pChunkData);

            if (!bStrongExist) {
                pFileTaskData->NewFileChunkDelegate(&pFileTaskData->ChunkConverter, { (const char8_t*)ChunkData.HexName, bin_to_hex_length(sizeof(WeakHash_t)) + bin_to_hex_length(sizeof(output)) }, { (const char*)rawData, FileChunkSize });
            }
            pFolderWorkData->CompleteSize.fetch_add(consumedBytes>pFileTaskData->FileChunksData->FileSize? pFileTaskData->FileChunksData->FileSize- lastChunkEndPos : FileChunkSize);

            bytesAfterLastChunk = 0;
            lastChunkEndPos = consumedBytes;
        }
        else {
            if (bWeakExist) {
                if (bStrongExist) {
                    auto pChunkData = std::make_shared<FileChunkData_t>();
                    auto& ChunkData = *pChunkData;
                    ChunkData.StartPos = uint64_t(consumedBytes - FileChunkSize);
                    to_upper_hex(ChunkData.HexName, (uint8_t*)&WeakHash, sizeof(WeakHash));
                    to_upper_hex(ChunkData.HexName + bin_to_hex_length(sizeof(WeakHash_t)), output, sizeof(output));
                    ChunkData.HexName[bin_to_hex_length(sizeof(WeakHash_t)) + bin_to_hex_length(sizeof(output))] = 0;
                    pFileTaskData->FileChunksData->Chunks.emplace(pChunkData);
                }
            }
        }
        };


    while (true) {
        if (pFileTaskData->bEOF) {
            if (FileChunkBuf.ContentSize.load() == 0) {
                assert(consumedBytes >= pFileTaskData->FileChunksData->FileSize);
                assert((pFileTaskData->FileChunksData->FileSize > 0 && pFileTaskData->FileChunksData->Chunks.size() > 0) || pFileTaskData->FileChunksData->FileSize == 0);
                break;
            }
        }
        //std::unique_lock lock(pFileTaskData->FileChunkBufMtx, std::defer_lock);
        //lock.lock();
        auto contentBuf = FileChunkBuf.GetContentBuf();
        //lock.unlock();
        if (hasher.IsInited()) {
            int i = 0;
            for (; i < contentBuf.size() && !bFlushAllChunkCache; i++) {
                auto [ConsumedBufL, ConsumedBufR] = FileChunkBuf.GetConsumedBuf(0, FileChunkSize);
                auto inBytePtr = contentBuf.data() + i;
                consumedBytes++;
                bytesAfterLastChunk++;
                FileChunkBuf.EatSize(1);
                hasher.Roll(*inBytePtr, *ConsumedBufL.data());
                tryCacheFunc();
            }
            assert(!(bFlushAllChunkCache && i < contentBuf.size()));
        }
        else if (contentBuf.size() >= FileChunkSize) {
            hasher.Init((const uint8_t*)contentBuf.data(), FileChunkSize);
            auto WeakHash = hasher.Get();
            consumedBytes += FileChunkSize;
            bytesAfterLastChunk += FileChunkSize;
            FileChunkBuf.EatSize(FileChunkSize);
            tryCacheFunc();
        }
    }
    CryptoLibMD5Digest(pFileTaskData->MD5Handle, output);
    to_upper_hex(pFolderWorkData->FolderManifest.Files[ConvertViewToU8View(pFileTaskData->FileChunksData->FileName)]->FileHash, output, sizeof(output));

}

void FFileBackupManagerGatherAll::GenFolderChunkDataReadFileTick(this FFileBackupManagerGatherAll& self, float delta, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr<GenFolderChunkDataFileTaskData_t> pFileTaskData)
{
    auto caculateFileHash = [&](const unsigned char* content, uint32_t len) {
        CryptoLibMD5Update(pFileTaskData->MD5Handle, std::span<const uint8_t>(content, len));
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

void FFileBackupManagerGatherAll::GenFolderChunkDataPostProcessingTask(this FFileBackupManagerGatherAll& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr<GenFolderChunkDataFileTaskData_t> pFileTaskData)
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
