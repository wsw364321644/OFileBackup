#include "FileBackupManager.h"
#include "FileBackupInternal.h"

#include <string_convert.h>
#include <hex.h>
#include <FunctionExitHelper.h>
#include <mbedtls/md5.h>
#include <rabinkarphash.h>
#include <std_ext.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <fstream>
#include <filesystem>
#include <set>
#include <shared_mutex>
#include <list>
#include <array>
#include <zstd.h>
#include <mutex>
#include <algorithm>
#include <map>


static KarpRabinHash EmptyHasher(FileChunkSize, CHAR_BIT * sizeof(uint32_t));

typedef struct FileChunkBuf_t {
    static constexpr uint32_t FileBufSize = FileChunkSize * 8;
    static constexpr uint32_t ConsumedFileBufSize = FileChunkSize * 3;
    char OutBuf[ConsumedFileBufSize] = {};
    char Buf[ConsumedFileBufSize + FileBufSize] = {};
    char* const BufEnd = Buf + ConsumedFileBufSize + FileBufSize;
    char* ConsumePos = Buf;
    char* StreamPos = Buf;
    std::atomic_uint32_t ContentSize{0};
    void Clear() {
        memset(Buf, 0, ConsumedFileBufSize + FileBufSize);
        ConsumePos = Buf;
        StreamPos = Buf;
        ContentSize = 0;
    }
    std::tuple<char*, uint32_t> GetEmptyBuf() {
        auto FreeSpaceSize = FileBufSize - ContentSize;
        return { StreamPos,StreamPos + FreeSpaceSize > BufEnd ? uint32_t(BufEnd - StreamPos) : FreeSpaceSize };
    }
    std::tuple<char*, uint32_t> GetContentBuf() {
        auto contentSize=ContentSize.load();
        return { ConsumePos,ConsumePos + contentSize > BufEnd ? uint32_t(BufEnd - ConsumePos) : contentSize };
    }
    std::tuple < std::tuple<char*, uint32_t>, std::tuple<char*, uint32_t>> GetConsumedBuf(uint32_t reverseStart, uint32_t reverseLen) {
        auto PosAfterOffset = ConsumePos - reverseStart;
        if (PosAfterOffset <= Buf) {
            PosAfterOffset = BufEnd - (Buf - PosAfterOffset);
        }
        if (PosAfterOffset - reverseLen >= Buf) {
            return { {PosAfterOffset - reverseLen,reverseLen},{} };
        }
        auto firstLen = reverseLen - uint32_t(PosAfterOffset - Buf);
        return { {BufEnd - firstLen,firstLen},{Buf,uint32_t(PosAfterOffset - Buf)} };
    }
    char* GetContinuousConsumedBuf(uint32_t reverseStart, uint32_t reverseLen) {
        auto [ConsumedBufTupleL, ConsumedBufTupleR] = GetConsumedBuf(reverseStart, reverseLen);
        auto& [ConsumedBufL, ConsumedBufLLen] = ConsumedBufTupleL;
        auto& [ConsumedBufR, ConsumedBufRLen] = ConsumedBufTupleR;
        if (ConsumedBufRLen == 0) {
            return ConsumedBufL;
        }
        memcpy(OutBuf, ConsumedBufL, ConsumedBufLLen);
        memcpy(OutBuf + ConsumedBufLLen, ConsumedBufR, ConsumedBufRLen);
        return OutBuf;
    }
    void FillSize(uint32_t size) {
        AdvancePos(StreamPos, size);
        ContentSize.fetch_add(size,std::memory_order::relaxed);
    }
    void EatSize(uint32_t size) {
        AdvancePos(ConsumePos, size);
        ContentSize.fetch_sub(size, std::memory_order::relaxed);
    }

    void AdvancePos(char*& Pos, uint32_t size) {
        Pos += size;
        if (Pos >= BufEnd) {
            Pos = Pos - BufEnd + Buf;
        }
    }
}FileChunkBuf_t;

typedef struct GenFolderChunkDataFileTaskData_t {
    boost::unordered_flat_map<WeakHash_t, boost::unordered_flat_set<std::string>> FileAllHashMap;
    mbedtls_md5_context FileMD5ctx{};
    mbedtls_md5_context MD5ctx{};
    FChunkConverter ChunkConverter{};
    std::shared_ptr<FileChunksData_t> FileChunksData;
    IFileBackupManagerInterface::TNewFileChunkDelegate  NewFileChunkDelegate;
    //both
    std::shared_mutex FileChunkBufMtx;
    std::shared_ptr<FileChunkBuf_t> FileChunkBuf;
    //read thread
    std::ifstream FileStream;
    uint32_t WaitAppendDataLen{ 0 };
}GenFolderChunkDataFileTaskData_t;

typedef struct GenFolderChunkDataWorkData_t {
    EGenFolderMetaDataStatus Status{ EGenFolderMetaDataStatus::None };
    EGenFolderMetaDataError Error{ EGenFolderMetaDataError::GFMDE_OK };
    IFileBackupManagerInterface::TGenFolderMetaDataFinishDelegate FinishDelegate;
    std::filesystem::path WorkPath;
    uint64_t ToltalSize{ 0 };
    std::atomic<uint64_t> CompleteSize{ 0 };
    std::map<uint64_t, std::set<std::u8string_view>> FileItrList;

    //std::shared_mutex FileTaskMtx;
    boost::unordered_flat_map<WeakHash_t, boost::unordered_flat_set<std::string>> AllHashMap;
    //std::unordered_map<WeakHash_t, std::set<std::string>, hash_32bit> AllHashMap;
    std::unordered_map<std::u8string_view, std::shared_ptr<GenFolderChunkDataFileTaskData_t>> FileTasks;
    std::vector<std::shared_ptr<GenFolderChunkDataFileTaskData_t>> FileTaskPool;

    FolderManifest_t FolderManifest;
    std::shared_ptr<GenFolderMetaDataProcess_t> OutProcess;
    std::shared_ptr<FolderManifest_t> OutFolderManifest;
}GenFolderChunkDataWorkData_t;


class FFileBackupManager :public IFileBackupManagerInterface {
public:
    FFileBackupManager() {
        EmptyHasher.hasher = CharacterHash(maskfnc<uint32_t>(CHAR_BIT * sizeof(uint32_t)), 0, 0);
    }

    CommonHandle_t GenFolderChunkData(const char8_t* path, TGenFolderMetaDataFinishDelegate Delegate) override;
    bool GenFolderChunkDataAddHash(CommonHandle_t handle, TGetNextHashPairCB CB) override;
    std::tuple<TOneFileChunkDataTask, TOneFileChunkDataReadFileTick, TOneFileChunkDataPostProcessingTask> GenFolderChunkDataGetNextFileTask(CommonHandle_t handle, TNewFileChunkDelegate) override;
    std::shared_ptr<const GenFolderMetaDataProcess_t> GenFolderChunkDataGetProcess(CommonHandle_t handle) override;
    std::shared_ptr<const FolderManifest_t> GetFolderChunkData(CommonHandle_t handle);
    void Tick(float delta) override;


    void GenFolderChunkDataTask(this FFileBackupManager& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);
    void GenFolderChunkDataReadFileTick(this FFileBackupManager& self, float delta, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);
    void GenFolderChunkDataPostProcessingTask(this FFileBackupManager& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);

    std::unordered_map<CommonHandle_t, std::shared_ptr<GenFolderChunkDataWorkData_t>>GenFolderMetaDataWorkDataList;
};

CommonHandle_t FFileBackupManager::GenFolderChunkData(const char8_t* path, TGenFolderMetaDataFinishDelegate Delegate)
{
    std::filesystem::path folderPath(path);
    if (!std::filesystem::exists(folderPath)) {
        return NullHandle;
    }
    auto [pair, res] = GenFolderMetaDataWorkDataList.try_emplace(CommonHandle_t::atomic_count, std::make_shared<GenFolderChunkDataWorkData_t>());
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

bool FFileBackupManager::GenFolderChunkDataAddHash(CommonHandle_t handle, TGetNextHashPairCB CB)
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
        auto [pair, res] = pFolderWorkData->AllHashMap.try_emplace(*(WeakHash_t*)hexBin, boost::unordered_flat_set<std::string>{std::string(strongHashBytes, StrongHashBit / 8)});
        if (!res) {
            pair->second.insert(strongHashBytes);
        }
    }
    return true;
}

std::tuple<IFileBackupManagerInterface::TOneFileChunkDataTask, IFileBackupManagerInterface::TOneFileChunkDataReadFileTick, IFileBackupManagerInterface::TOneFileChunkDataPostProcessingTask> FFileBackupManager::GenFolderChunkDataGetNextFileTask(CommonHandle_t handle, TNewFileChunkDelegate NewFileChunkDelegate)
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
            pFileTaskData->FileChunkBuf->Clear();
        }
        else {
            pFileTaskData = std::make_shared<GenFolderChunkDataFileTaskData_t>();
            pFileTaskData->FileChunkBuf = std::make_shared<FileChunkBuf_t>();;
            pFileTaskData->ChunkConverter.UpdateConvertDirection(EConvertDirection::ToChunkFile);
        }
        pFileTaskData->FileChunksData = pFileChunksData;
        auto [itr, res] = pFolderWorkData->FileTasks.try_emplace(ConvertStringTotU8View(pFileTaskData->FileChunksData->FileName), pFileTaskData);
        if (!res) {
            return { nullptr,nullptr,nullptr };
        }
        pFileTaskData->NewFileChunkDelegate = NewFileChunkDelegate;
        mbedtls_md5_init(&pFileTaskData->MD5ctx);
        mbedtls_md5_init(&pFileTaskData->FileMD5ctx);
        mbedtls_md5_starts(&pFileTaskData->FileMD5ctx);
        pFileTaskData->FileAllHashMap = pFolderWorkData->AllHashMap;
        auto filePath = pFolderWorkData->WorkPath / pFileTaskData->FileChunksData->FileName;
        pFileTaskData->FileStream = std::ifstream(filePath, std::ios::binary);
        if (!pFileTaskData->FileStream.is_open()) {
            return { nullptr,nullptr,nullptr };
        }
        pFileTaskData->WaitAppendDataLen = pFileTaskData->FileChunksData->FileSize > FileChunkSize ? 0 : (FileChunkSize - pFileTaskData->FileChunksData->FileSize % FileChunkSize) % FileChunkSize;
    }
    TOneFileChunkDataTask func = std::bind(FFileBackupManager::GenFolderChunkDataTask, *this, pFolderWorkData, pFileTaskData);
    TOneFileChunkDataPostProcessingTask postfunc = std::bind(FFileBackupManager::GenFolderChunkDataPostProcessingTask, *this, pFolderWorkData, pFileTaskData);
    TOneFileChunkDataReadFileTick readFileTick = std::bind(FFileBackupManager::GenFolderChunkDataReadFileTick, *this,std::placeholders::_1,  pFolderWorkData, pFileTaskData);
    return { func,readFileTick,postfunc };
}

std::shared_ptr<const GenFolderMetaDataProcess_t> FFileBackupManager::GenFolderChunkDataGetProcess(CommonHandle_t handle)
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

std::shared_ptr<const FolderManifest_t> FFileBackupManager::GetFolderChunkData(CommonHandle_t handle)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return nullptr;
    }
    auto& pFolderWorkData = itr->second;
    *pFolderWorkData->OutFolderManifest = pFolderWorkData->FolderManifest;
    return pFolderWorkData->OutFolderManifest;
}

void FFileBackupManager::Tick(float delta)
{
    std::set<CommonHandle_t> needDel;
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
                auto [itr, res] = pFolderWorkData->FolderManifest.Files.try_emplace(ConvertStringTotU8View(pFileChunksData->FileName), pFileChunksData);
                if (!res) {
                    pFolderWorkData->Status = EGenFolderMetaDataStatus::Finished;
                    pFolderWorkData->Error = EGenFolderMetaDataError::GFMDE_FS_ERROR;
                    break;
                }
                auto [fileListItr, _] = pFolderWorkData->FileItrList.try_emplace(pFileChunksData->FileSize);
                fileListItr->second.insert(ConvertStringTotU8View(pFileChunksData->FileName));
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

void FFileBackupManager::GenFolderChunkDataTask(this FFileBackupManager& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData)
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
    uint32_t lastHashValue{ std::numeric_limits<uint32_t>::max() };
    auto hasher = EmptyHasher;

    auto internalCaculateHashInConsumedBuf = [&](const char* content, uint32_t reverseStart, unsigned char out[16]) {
        mbedtls_md5_starts(&pFileTaskData->MD5ctx);
        mbedtls_md5_update(&pFileTaskData->MD5ctx, (const unsigned char*)content, FileChunkSize);
        mbedtls_md5_finish(&pFileTaskData->MD5ctx, out);
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
                    mbedtls_md5_starts(&pFileTaskData->MD5ctx);
                    mbedtls_md5_update(&pFileTaskData->MD5ctx, (const unsigned char*)rawData, FileChunkSize);
                    mbedtls_md5_finish(&pFileTaskData->MD5ctx, chunkCache.StrongHash);
                }
                auto strongHashStr = std::string((const char*)chunkCache.StrongHash);
                {
                    auto itr = pFileTaskData->FileAllHashMap.find(*(WeakHash_t*)&chunkCache.WeakHash);
                    if (itr == pFileTaskData->FileAllHashMap.end()) {
                        pFileTaskData->FileAllHashMap.try_emplace(*(WeakHash_t*)&chunkCache.WeakHash, boost::unordered_flat_set<std::string>{strongHashStr});
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
                pFileTaskData->NewFileChunkDelegate(&pFileTaskData->ChunkConverter, (const char8_t*)ChunkData.HexName, uint32_t(sizeof(chunkCache.WeakHash) * 2 + sizeof(chunkCache.StrongHash) * 2), (const char*)rawData, FileChunkSize);
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
    auto cacheNewFunc = [&](uint32_t weakhash, const unsigned char stronghash[16] = nullptr, bool fExist = false) {
        assert(bytesAfterLastChunk <= FileChunkSize);
        bFlushAllChunkCache = consumedBytes >= pFileTaskData->FileChunksData->FileSize;
        if (fExist|| bFlushAllChunkCache) {
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

    while (!bFlushAllChunkCache) {
        //std::unique_lock lock(pFileTaskData->FileChunkBufMtx, std::defer_lock);
        //lock.lock();
        auto [contentBuf, contentBufLen] = FileChunkBuf.GetContentBuf();
        //lock.unlock();
        int i = 0;
        for (; i < contentBufLen && !bFlushAllChunkCache; i++) {
            auto [ConsumedBufTupleL, ConsumedBufTupleR] = FileChunkBuf.GetConsumedBuf(0, FileChunkSize);
            auto& [ConsumedBuf, ConsumedBufLen] = ConsumedBufTupleL;
            auto inBytePtr = contentBuf + i;
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
                lastHashValue = hasher.hashvalue;
                hasher.update(*inBytePtr, *ConsumedBuf);
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
                    cacheNewFunc(hasher.hashvalue, nullptr, true);
                }
                else {
                    cacheNewFunc(hasher.hashvalue, output);
                }
            }
            else {
                cacheNewFunc(hasher.hashvalue);
            }

        }
        //lock.lock();
        //FileChunkBuf.EatSize(contentBufLen-i-1);
        //lock.unlock();
    }
    mbedtls_md5_finish(&pFileTaskData->FileMD5ctx, output);
    to_upper_hex(pFolderWorkData->FolderManifest.Files[ConvertStringTotU8View(pFileTaskData->FileChunksData->FileName)]->FileHash, output, sizeof(output));

}

void FFileBackupManager::GenFolderChunkDataReadFileTick(this FFileBackupManager& self, float delta, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr<GenFolderChunkDataFileTaskData_t> pFileTaskData)
{
    auto caculateFileHash = [&](const unsigned char* content, uint32_t len) {
        mbedtls_md5_update(&pFileTaskData->FileMD5ctx, (const unsigned char*)content, len);
        };
    FileChunkBuf_t& FileChunkBuf = *pFileTaskData->FileChunkBuf;
    if (!pFileTaskData->FileStream.eof()) {
        //std::unique_lock lock(pFileTaskData->FileChunkBufMtx, std::defer_lock);
        //lock.lock();
        auto [freeBuf, freeBufSize] = FileChunkBuf.GetEmptyBuf();
        //lock.unlock();
        if (freeBufSize == 0) {
            return;
        }
        pFileTaskData->FileStream.read(freeBuf, freeBufSize);
        auto extractLen = pFileTaskData->FileStream.gcount();
        if (extractLen == 0) {
            return;
        }
        caculateFileHash((const unsigned char*)freeBuf, extractLen);
        //lock.lock();
        FileChunkBuf.FillSize(extractLen);
        //lock.unlock();
    }
    else if(pFileTaskData->WaitAppendDataLen>0){
        //std::unique_lock lock(pFileTaskData->FileChunkBufMtx, std::defer_lock);
        //lock.lock();
        auto [freeBuf, freeBufSize] = FileChunkBuf.GetEmptyBuf();
        //lock.unlock();
        if (freeBufSize == 0) {
            return;
        }
        auto fillSize = std::min(freeBufSize, uint32_t(pFileTaskData->WaitAppendDataLen));
        memset(freeBuf, 0, fillSize);
        pFileTaskData->WaitAppendDataLen -= fillSize;
        //lock.lock();
        FileChunkBuf.FillSize(fillSize);
        //lock.unlock();
    }
}

void FFileBackupManager::GenFolderChunkDataPostProcessingTask(this FFileBackupManager& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr<GenFolderChunkDataFileTaskData_t> pFileTaskData)
{
    for (auto& [weekHash,strongSet]: pFileTaskData->FileAllHashMap) {
        auto [itr,res]=pFolderWorkData->AllHashMap.try_emplace(weekHash, strongSet);
        if (res) {
            continue;
        }
        itr->second.merge(strongSet);
    }
    pFolderWorkData->FileTasks.erase(ConvertStringTotU8View(pFileTaskData->FileChunksData->FileName));
    pFolderWorkData->FileTaskPool.push_back(pFileTaskData);
}



LIB_FILEBACKUP_EXPORT IFileBackupManagerInterface* GetFileBackupManagerInstance()
{
    static std::atomic<std::shared_ptr<FFileBackupManager>> AtomicManager;
    auto oldptr = AtomicManager.load();
    if (!oldptr) {
        std::shared_ptr<FFileBackupManager> pManager(new FFileBackupManager);
        AtomicManager.compare_exchange_strong(oldptr, pManager);
    }
    return AtomicManager.load().get();
}

