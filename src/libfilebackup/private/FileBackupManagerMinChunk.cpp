#include "FileBackupManagerMinChunk.h"

#include <string_convert.h>
#include <FunctionExitHelper.h>
#include <std_ext.h>

#include <rabinkarphash.h>
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


inline KarpRabinHash EmptyHasher(FileChunkSize, CHAR_BIT * sizeof(uint32_t));

struct HasherInitHelper_t {
    HasherInitHelper_t() {
        //same seed that output same hash each run
        EmptyHasher.hasher = CharacterHash(maskfnc<uint32_t>(CHAR_BIT * sizeof(uint32_t)), 0, 0);
    }
};
inline HasherInitHelper_t HasherInitHelper;

CommonHandle32_t FFileBackupManagerMinChunk::GenFolderChunkData(const char8_t* path, TGenFolderMetaDataFinishDelegate Delegate)
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

bool FFileBackupManagerMinChunk::GenFolderChunkDataAddHash(CommonHandle32_t handle, TGetNextHashPairCB CB)
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

std::tuple<IFileBackupManagerInterface::TOneFileChunkDataTask, IFileBackupManagerInterface::TOneFileChunkDataReadFileTick, IFileBackupManagerInterface::TOneFileChunkDataPostProcessingTask> FFileBackupManagerMinChunk::GenFolderChunkDataGetNextFileTask(CommonHandle32_t handle, TNewFileChunkDelegate NewFileChunkDelegate)
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
        pFileTaskData->WaitAppendDataLen = pFileTaskData->FileChunksData->FileSize > FileChunkSize ? 0 : (FileChunkSize - pFileTaskData->FileChunksData->FileSize % FileChunkSize) % FileChunkSize;
    }
    TOneFileChunkDataTask func = std::bind(&FFileBackupManagerMinChunk::GenFolderChunkDataTask, *this, pFolderWorkData, pFileTaskData);
    TOneFileChunkDataPostProcessingTask postfunc = std::bind(&FFileBackupManagerMinChunk::GenFolderChunkDataPostProcessingTask, *this, pFolderWorkData, pFileTaskData);
    TOneFileChunkDataReadFileTick readFileTick = std::bind(&FFileBackupManagerMinChunk::GenFolderChunkDataReadFileTick, *this, std::placeholders::_1, pFolderWorkData, pFileTaskData);
    return { func,readFileTick,postfunc };
}

std::shared_ptr<const GenFolderMetaDataProcess_t> FFileBackupManagerMinChunk::GenFolderChunkDataGetProcess(CommonHandle32_t handle)
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

std::shared_ptr<const FolderManifest_t> FFileBackupManagerMinChunk::GetFolderChunkData(CommonHandle32_t handle)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return nullptr;
    }
    auto& pFolderWorkData = itr->second;
    *pFolderWorkData->OutFolderManifest = pFolderWorkData->FolderManifest;
    return pFolderWorkData->OutFolderManifest;
}

void FFileBackupManagerMinChunk::Tick(float delta)
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

void FFileBackupManagerMinChunk::GenFolderChunkDataTask(this FFileBackupManagerMinChunk& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData)
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
    auto rollingHashNeedEat = FileChunkSize;
    auto hasher = EmptyHasher;
    
    auto internalCaculateHashInConsumedBuf = [&](const char* content, uint32_t reverseStart, unsigned char out[16]) {
        auto hash = XXH3_128bits(content, FileChunkSize);
        CopyxxHashToBuf(hash, out);
        };
    auto caculateHashInConsumedBuf = [&](uint32_t reverseStart, unsigned char out[16]) {
        internalCaculateHashInConsumedBuf(FileChunkBuf.GetContinuousConsumedBuf(reverseStart, FileChunkSize), reverseStart, out);
        };
    auto caculateAllHashInConsumedBuf = [&](uint32_t reverseStart, WeakHash_t& weakHash, unsigned char out[16]) {
        auto hasher = EmptyHasher;
        auto rawData = FileChunkBuf.GetContinuousConsumedBuf(reverseStart, FileChunkSize);
        internalCaculateHashInConsumedBuf(rawData, reverseStart, out);
        for (int i = 0; i < FileChunkSize; i++) {
            hasher.eat(rawData[i]);
        }
        weakHash = hasher.hashvalue;;
        };

    auto processChunkChacheFunc = [&]() {
        while (!fileChunkCacheContainer.empty()) {
            auto& pChunkCache = fileChunkCacheContainer.front();
            auto& chunkCache = *pChunkCache;
            auto ChunkEndPos = chunkCache.StartPos + FileChunkSize;
            if (!bFlushAllChunkCache) {
                if (consumedBytes - chunkCache.StartPos < FileChunkSize * 3) {
                    return;
                }
            }
            else {
                ChunkEndPos = ChunkEndPos > pFileTaskData->FileChunksData->FileSize ? pFileTaskData->FileChunksData->FileSize : ChunkEndPos;
            }
            pFolderWorkData->CompleteSize += ChunkEndPos - lastChunkEndPos;
            lastChunkEndPos = ChunkEndPos;
            fileChunkCacheContainer.pop_front();
            char* rawData;
            if (!chunkCache.fChunkAlreadyExist) {
                rawData = FileChunkBuf.GetContinuousConsumedBuf(consumedBytes - chunkCache.StartPos - FileChunkSize, FileChunkSize);
                if (!chunkCache.fStrongHash) {
                    auto hash = XXH3_128bits(rawData, FileChunkSize);
                    CopyxxHashToBuf(hash, chunkCache.StrongHash);
                }
                auto strongHashStr = std::string((const char*)chunkCache.StrongHash);
                {
                    auto itr = pFileTaskData->FileAllHashMap.find(*(WeakHash_t*)&chunkCache.WeakHash);
                    if (itr == pFileTaskData->FileAllHashMap.end()) {
                        pFileTaskData->FileAllHashMap.try_emplace(*(WeakHash_t*)&chunkCache.WeakHash, HashSetType{ strongHashStr });
                    }
                    else {
                        itr->second.insert(strongHashStr);
                    }
                }
            }
            auto pChunkData = std::make_shared<FileChunkData_t>();
            auto& ChunkData = *pChunkData;
            ChunkData.StartPos = chunkCache.StartPos;
            to_upper_hex(ChunkData.HexName, (uint8_t*)&chunkCache.WeakHash, sizeof(chunkCache.WeakHash));
            to_upper_hex(ChunkData.HexName + sizeof(WeakHash_t) * 2, chunkCache.StrongHash, sizeof(chunkCache.StrongHash));
            ChunkData.HexName[sizeof(WeakHash_t) * 2 + sizeof(chunkCache.StrongHash) * 2] = 0;
            pFileTaskData->FileChunksData->Chunks.emplace(pChunkData);
            if (!chunkCache.fChunkAlreadyExist) {
                pFileTaskData->NewFileChunkDelegate(&pFileTaskData->ChunkConverter, { (const char8_t*)ChunkData.HexName, uint32_t(sizeof(chunkCache.WeakHash) * 2 + sizeof(chunkCache.StrongHash) * 2) }, { (const char*)rawData, FileChunkSize });
            }

        }
        };
    auto internalCacheNewFunc = [&](std::streamoff posEnd, uint32_t weakhash, const unsigned char stronghash[16] = nullptr, bool fExist = false) {
        auto pChunkCache = fileChunkCacheContainer.get_available();
        pChunkCache->fChunkAlreadyExist = fExist;
        pChunkCache->StartPos = uint64_t(posEnd - FileChunkSize);
        pChunkCache->WeakHash = weakhash;
        pChunkCache->fStrongHash = !!stronghash;
        if (pChunkCache->fStrongHash)
            memcpy((void*)pChunkCache->StrongHash, stronghash, 16);
        fileChunkCacheContainer.push_back(pChunkCache);
        bytesAfterLastChunk = 0;
        };
    ///
    /// @detail 1.保证块n不会跟块n+2重叠 2.保证块n距离块n+3大于FileChunkSize
    /// 当缓存已有两个块，如果新加入的块3仍旧与块1重叠，则块3取代块2
    /// 当缓存已有三个块，如果新加入的块4仍旧与块2重叠，则块4取代块3
    /// 当缓存已有三个块，块4取代块3，重新生成位于块4前与之不重叠的块作为块2
    /// 空间利用率最差时，每块有2/3的冗余数据
    /// 
    auto cacheNewFunc = [&](uint32_t weakhash, const unsigned char stronghash[16] = nullptr, bool fExist = false) {
        assert(bytesAfterLastChunk <= FileChunkSize);
        bFlushAllChunkCache = consumedBytes >= pFileTaskData->FileChunksData->FileSize;
        if (fExist || bFlushAllChunkCache) {
            switch (fileChunkCacheContainer.size()) {
            case 2: {
                auto& firstCache = fileChunkCacheContainer.front();
                if (firstCache->StartPos + FileChunkSize * 2 >= consumedBytes) {
                    fileChunkCacheContainer.pop_back();
                }
                internalCacheNewFunc(consumedBytes, weakhash, stronghash, fExist);
                break;
            }
            case 3: {
                auto& firstCache = fileChunkCacheContainer.front();
                auto& secondeCache = *(fileChunkCacheContainer.begin()++);
                if (secondeCache->StartPos + FileChunkSize * 2 >= consumedBytes) {
                    fileChunkCacheContainer.pop_back();
                }
                else {
                    fileChunkCacheContainer.pop_back();
                    fileChunkCacheContainer.pop_back();
                    WeakHash_t weakHash;
                    auto endPos = firstCache->StartPos + FileChunkSize * 2;
                    caculateAllHashInConsumedBuf(uint32_t(consumedBytes - endPos), weakHash, output);
                    bool bWeakExist{ false };
                    bool bStrongExist{ false };
                    auto hashItr = pFileTaskData->FileAllHashMap.find(hasher.hashvalue);
                    if (hashItr != pFileTaskData->FileAllHashMap.end()) {
                        bWeakExist = true;
                        auto stronHashItr = hashItr->second.find(std::string((const char*)output));
                        if (stronHashItr != hashItr->second.end()) {
                            bStrongExist = true;
                        }
                    }
                    if (bStrongExist) {
                        internalCacheNewFunc(endPos, weakHash, output, true);
                    }
                    else {
                        internalCacheNewFunc(endPos, weakHash, output, false);
                    }
                }
                internalCacheNewFunc(consumedBytes, weakhash, stronghash, fExist);
                break;
            }
            default: {
                internalCacheNewFunc(consumedBytes, weakhash, stronghash, fExist);
            }
            }
        }
        else if (bytesAfterLastChunk == FileChunkSize) {
            internalCacheNewFunc(consumedBytes, weakhash, stronghash, fExist);
        }
        processChunkChacheFunc();
        };
    while (true) {

        if (pFileTaskData->bEOF) {
            if (FileChunkBuf.ContentSize.load() == 0) {
                assert((pFileTaskData->FileChunksData->FileSize > 0 && pFileTaskData->FileChunksData->Chunks.size() > 0) || pFileTaskData->FileChunksData->FileSize == 0);
                break;
            }
        }
        //std::unique_lock lock(pFileTaskData->FileChunkBufMtx, std::defer_lock);
        //lock.lock();
        auto contentBuf = FileChunkBuf.GetContentBuf();
        //lock.unlock();
        int i = 0;
        for (; i < contentBuf.size() && !bFlushAllChunkCache; i++) {
            auto [ConsumedBufL, ConsumedBufR] = FileChunkBuf.GetConsumedBuf(0, FileChunkSize);
            auto inBytePtr = contentBuf.data() + i;
            consumedBytes++;
            bytesAfterLastChunk++;
            FileChunkBuf.EatSize(1);
            //skip first FileChunkSize number of byte 
            if (rollingHashNeedEat > 0) {
                rollingHashNeedEat--;
                hasher.eat(*inBytePtr);
                if (rollingHashNeedEat > 0) {
                    continue;
                }
            }
            else {
                hasher.update(*ConsumedBufL.data(),*inBytePtr);
            }


            bool bWeakExist{ false };
            bool bStrongExist{ false };
            auto hashItr = pFileTaskData->FileAllHashMap.find(hasher.hashvalue);
            if (hashItr != pFileTaskData->FileAllHashMap.end()) {
                bWeakExist = true;
                caculateHashInConsumedBuf(0, output);
                auto stronHashItr = hashItr->second.find(std::string((const char*)output));
                if (stronHashItr != hashItr->second.end()) {
                    bStrongExist = true;
                }
            }

            if (bWeakExist) {
                if (bStrongExist) {
                    cacheNewFunc(hasher.hashvalue, output, true);
                }
                else {
                    cacheNewFunc(hasher.hashvalue, output);
                }
            }
            else {
                cacheNewFunc(hasher.hashvalue);
            }

        }
        assert(!(bFlushAllChunkCache && i < contentBuf.size()));
        //lock.lock();
        //FileChunkBuf.EatSize(contentBufLen-i-1);
        //lock.unlock();
    }
    CryptoLibMD5Digest(pFileTaskData->MD5Handle, output);
    to_upper_hex(pFolderWorkData->FolderManifest.Files[ConvertViewToU8View(pFileTaskData->FileChunksData->FileName)]->FileHash, output, sizeof(output));

}

void FFileBackupManagerMinChunk::GenFolderChunkDataReadFileTick(this FFileBackupManagerMinChunk& self, float delta, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr<GenFolderChunkDataFileTaskData_t> pFileTaskData)
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

void FFileBackupManagerMinChunk::GenFolderChunkDataPostProcessingTask(this FFileBackupManagerMinChunk& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr<GenFolderChunkDataFileTaskData_t> pFileTaskData)
{
    for (auto& [weekHash, strongSet] : pFileTaskData->FileAllHashMap) {
        auto [itr, res] = pFolderWorkData->AllHashMap.try_emplace(weekHash, strongSet);
        if (res) {
            continue;
        }
        itr->second.merge(strongSet);
    }
    pFolderWorkData->FileTasks.erase(ConvertViewToU8View(pFileTaskData->FileChunksData->FileName));
    pFolderWorkData->FileTaskPool.push_back(pFileTaskData);
}
