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


std::tuple<IFileBackupManagerInterface::TOneFileChunkDataTask, IFileBackupManagerInterface::TOneFileChunkDataReadFileTick, IFileBackupManagerInterface::TOneFileChunkDataPostProcessingTask> FFileBackupManagerGatherAll::GenFolderChunkDataGetNextFileTask(CommonHandle32_t handle, TNewFileChunkDelegate NewFileChunkDelegate)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return { nullptr,nullptr,nullptr };
    }
    auto& pFolderWorkData = itr->second;
    if (pFolderWorkData->Status != EGenFolderMetaDataStatus::Inited) {
        return { nullptr,nullptr,nullptr };
    }
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
            pFileTaskData->XXH3State = XXH3_createState();
            if (!pFileTaskData->XXH3State) {
                return { nullptr,nullptr,nullptr };
            }
            pFileTaskData->Clear();
        }
        pFileTaskData->FileChunksData = pFileChunksData;
        auto [itr, res] = pFolderWorkData->FileTasks.try_emplace(ConvertViewToU8View(pFileTaskData->FileChunksData->FileName), pFileTaskData);
        if (!res) {
            return { nullptr,nullptr,nullptr };
        }
        pFileTaskData->NewFileChunkDelegate = NewFileChunkDelegate;

        pFileTaskData->FileAllHashMap = pFolderWorkData->AllHashMap;
        auto& filePath = pFolderWorkData->FileLocalPathMap[ConvertViewToU8View(pFileTaskData->FileChunksData->FileName)];
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
    auto xxhash = XXH3_128bits_digest(pFileTaskData->XXH3State);
    CopyxxHashToBuf(xxhash, output);
    to_upper_hex(pFolderWorkData->FolderManifest.Files[ConvertViewToU8View(pFileTaskData->FileChunksData->FileName)]->FileHash, output, sizeof(output));

}
