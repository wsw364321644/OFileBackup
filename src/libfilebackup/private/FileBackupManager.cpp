#include "FileBackupManager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <mbedtls/md5.h>
#include <FunctionExitHelper.h>
#include <rabinkarphash.h>
#include <set>
#include <shared_mutex>
#include <list>
#include <array>
#include <zstd.h>
#include <hex.h>
constexpr uint32_t FileChunkSize = 1<<20;
static KarpRabinHash EmptyHasher(FileChunkSize, CHAR_BIT * sizeof(uint32_t));

typedef struct FileChunkBuf_t {
    static constexpr uint32_t FileBufSize = FileChunkSize;
    static constexpr uint32_t ConsumedFileBufSize = FileChunkSize * 3;
    char OutBuf[ConsumedFileBufSize] = {};
    char Buf[ConsumedFileBufSize+FileBufSize] = {};
    char* const BufEnd= Buf+ ConsumedFileBufSize + FileBufSize;
    char* ConsumePos = Buf;
    char* StreamPos = Buf;
    uint32_t ContentSize{ 0 };
    void Clear() {
        memset(Buf, 0, ConsumedFileBufSize + FileBufSize);
        ConsumePos = Buf;
        StreamPos = Buf;
        ContentSize = 0;
    }
    std::tuple<char*, uint32_t> GetEmptyBuf() {
        //if (StreamPos < ConsumePos) {
        //    return { StreamPos,FreeSpaceSize };
        //}
        auto FreeSpaceSize = FileBufSize - ContentSize;
        return { StreamPos,StreamPos + FreeSpaceSize > BufEnd ? uint32_t(BufEnd - StreamPos) : FreeSpaceSize };
    }
    std::tuple<char*, uint32_t> GetContentBuf() {
        return { ConsumePos,ConsumePos + ContentSize > BufEnd ? uint32_t(BufEnd - ConsumePos) : ContentSize };
    }
    std::tuple < std::tuple<char*, uint32_t>, std::tuple<char*, uint32_t>> GetConsumedBuf(uint32_t reverseStart,uint32_t reverseLen) {
        auto PosAfterOffset = ConsumePos - reverseStart;
        if (PosAfterOffset <= Buf) {
            PosAfterOffset = BufEnd - (Buf - PosAfterOffset);
        }
        if (PosAfterOffset - reverseLen >= Buf) {
            return { {PosAfterOffset - reverseLen,reverseLen},{} };
        }
        auto firstLen=reverseLen - uint32_t(PosAfterOffset - Buf);
        return { {BufEnd- firstLen,firstLen},{Buf,uint32_t(PosAfterOffset - Buf)} };
    }
    char* GetContinuousConsumedBuf(uint32_t reverseStart, uint32_t reverseLen) {
        auto ConsumedBuf = GetConsumedBuf(reverseStart,reverseLen);
        if (std::get<1>(std::get<1>(ConsumedBuf)) == 0) {
            return std::get<0>(std::get<0>(ConsumedBuf));
        }

        memcpy(OutBuf, std::get<0>(std::get<0>(ConsumedBuf)), std::get<1>(std::get<0>(ConsumedBuf)));
        memcpy(OutBuf + std::get<1>(std::get<0>(ConsumedBuf)), std::get<0>(std::get<1>(ConsumedBuf)), std::get<1>(std::get<1>(ConsumedBuf)));
        return OutBuf;
    }
    void FillSize(uint32_t size) {
        AdvancePos(StreamPos, size);
        ContentSize += size;
    }
    void EatSize(uint32_t size) {
        AdvancePos(ConsumePos, size);
        ContentSize -= size;
    }

    void AdvancePos(char*& Pos, uint32_t size) {
        Pos += size;
        if (Pos >= BufEnd) {
            Pos = Pos - BufEnd + Buf;
        }
    }
}FileChunkBuf_t;

typedef struct GenFolderChunkDataFileTaskData_t {
    
    ~GenFolderChunkDataFileTaskData_t() {
        if (ZSTDBuf) {
            free(ZSTDBuf);
        }
        ZSTD_freeCCtx(CCtx);
    }

    std::filesystem::path FilePath;
    std::string FilePathStr;
    uint64_t FileSize;
    mbedtls_md5_context FileMD5ctx;
    mbedtls_md5_context MD5ctx;
    ZSTD_CCtx* CCtx{ nullptr };
    size_t ZSTDBufSize;
    void* ZSTDBuf{ nullptr };
    char HexBuf[HexNameStrLen];
    std::shared_ptr<FileChunkBuf_t> FileChunkBuf;
    IFileBackupManagerInterface::TNewFileChunkDelegate  NewFileChunkDelegate;
}GenFolderChunkDataFileTaskData_t;

typedef struct GenFolderChunkDataWorkData_t {
    EGenFolderMetaDataStatus Status{ EGenFolderMetaDataStatus::None };
    IFileBackupManagerInterface::TGenFolderMetaDataFinishDelegate FinishDelegate;
    std::filesystem::path WorkPath;
    uint64_t ToltalSize{ 0 };
    std::atomic<uint64_t> CompleteSize{ 0 };
    std::unordered_map<WeakHash_t, std::set<std::string>> AllHashMap;
    std::vector<std::shared_ptr<FileChunkBuf_t>> FileChunkBufPool;
    std::shared_ptr<GenFolderMetaDataProcess_t> OutProcess;
    std::shared_ptr<FolderManifest_t> OutFolderManifest;

    std::shared_mutex DirItrMtx;
    std::filesystem::recursive_directory_iterator DirItr;

    std::shared_mutex FileTaskMtx;
    std::unordered_map<std::string, std::shared_ptr<GenFolderChunkDataFileTaskData_t>> FileTasks;

    std::shared_mutex FolderChunkDataMtx;
    FolderManifest_t FolderManifest;
}GenFolderChunkDataWorkData_t;


void to_json(nlohmann::json& j, const FolderManifest_t& FolderManifest) {
    j = nlohmann::json(nlohmann::json::value_t::object);
    auto FilesNode = nlohmann::json(nlohmann::json::value_t::object);
    for (auto& pair : FolderManifest.Files) {
        auto FileNode = nlohmann::json(nlohmann::json::value_t::object);
        auto FileChunksNode = nlohmann::json(nlohmann::json::value_t::array);
        pair.second->FileHash[StrongHashBit / 4] = 0;
        for (auto& chunk : pair.second->Chunks) {
            FileChunksNode.push_back(nlohmann::json{ {"HexName",chunk.HexName},{"StartPos",chunk.StartPos} });
        }
        FileNode["FileHash"] = pair.second->FileHash;
        FileNode["Chunks"] = FileChunksNode;
        FilesNode[pair.first] = FileNode;
    }
    j["Files"] = FilesNode;
}

void from_json(const nlohmann::json& j, FolderManifest_t& FolderManifest) {
    if (!j.contains("Files")) {
        return;
    }
    auto& FilesNode = j["Files"];
    for (auto itFilesNode = FilesNode.begin(); itFilesNode != FilesNode.end(); ++itFilesNode)
    {
        auto pFileChunksData = std::make_shared<FileChunksData_t>();
        FolderManifest.Files[itFilesNode.key()] = pFileChunksData;
        auto& FileNode=itFilesNode.value();
        if (!FileNode.contains("Chunks") || !FileNode.contains("FileHash")|| !FileNode["Chunks"].is_array()) {
            return;
        }
        auto& FileHash=FileNode["FileHash"].get_ref<const nlohmann::json::string_t&>();
        memcpy(pFileChunksData->FileHash, FileHash.c_str(), FileHash.length()+1);
        for (auto itChunks = FileNode["Chunks"].begin(); itChunks != FileNode["Chunks"].end(); ++itChunks) {
            FileChunkData_t chunkData;
            auto& ChunkNode = itChunks.value();
            if (!ChunkNode.contains("HexName") || !ChunkNode.contains("StartPos") ) {
                return;
            }
            auto& HexName = ChunkNode["HexName"].get_ref<const nlohmann::json::string_t&>();
            memcpy(chunkData.HexName, HexName.c_str(), HexName.length() + 1);
            chunkData.StartPos = ChunkNode["StartPos"].get_ref<const nlohmann::json::number_integer_t&>();
            pFileChunksData->Chunks.insert(chunkData);
        }
    }
}

std::shared_ptr<const std::string> FolderManifest_t::to_string() const
{
    nlohmann::json j = *this;
    auto out=std::make_shared<std::string>();
    *out = j.dump();
    return out;
}

std::shared_ptr<const FolderManifest_t> FolderManifest_t::from_string(const char* jsonstr)
{
    auto j=nlohmann::json::parse(jsonstr, nullptr, false, true);
    if (j.is_discarded())
    {
        return nullptr;
    }
    auto out = std::make_shared<FolderManifest_t>();
    *out = j.get<FolderManifest_t>();
    return out;
}


class FFileBackupManager :public IFileBackupManagerInterface {
public:
    FFileBackupManager() {
        EmptyHasher.hasher = CharacterHash(maskfnc<uint32_t>(CHAR_BIT * sizeof(uint32_t)), 0, 0);
    }
    std::shared_ptr<const FolderManifest_t> ParseFolderChunkData(const char* str) override;
    CommonHandle_t GenFolderChunkData(const char* path, TGenFolderMetaDataFinishDelegate Delegate) override;
    bool GenFolderChunkDataAddHash(CommonHandle_t handle, TGetNextHashPairCB CB) override;
    TOneFileChunkDataTask GenFolderChunkDataGetNextFileTask(CommonHandle_t handle, TNewFileChunkDelegate) override;
    std::shared_ptr<const GenFolderMetaDataProcess_t> GenFolderChunkDataGetProcess(CommonHandle_t handle) override;
    std::shared_ptr<const FolderManifest_t> GetFolderChunkData(CommonHandle_t handle) override;

    void Tick(float delta) override;


    void GenFolderChunkDataFileTask(this FFileBackupManager& self,std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);

    std::unordered_map<CommonHandle_t, std::shared_ptr<GenFolderChunkDataWorkData_t>>GenFolderMetaDataWorkDataList;
};

std::shared_ptr<const FolderManifest_t> FFileBackupManager::ParseFolderChunkData(const char* str)
{
    auto ptr = std::make_shared<FolderManifest_t>();
    //ptr->name = "hajkahlklhhlkjlahkjddskjahfalkjdshflkahjdfskljshahdfalkhflakhsdfhuaewhufiewuhilewfuhelihfuewhfuijihuhiuaklreherfbrefrgeregrgergevrgergeerg";
    //ptr->strs.push_back("sdfaafa");

    return ptr;
}

CommonHandle_t FFileBackupManager::GenFolderChunkData(const char* path, TGenFolderMetaDataFinishDelegate Delegate)
{
    std::filesystem::path folderPath(path);
    if (!std::filesystem::exists(folderPath)) {
        return NullHandle;
    }
    auto [pair,res] = GenFolderMetaDataWorkDataList.try_emplace(CommonHandle_t(), std::make_shared<GenFolderChunkDataWorkData_t>());
    if (!res) {
        return NullHandle;
    } 
    auto& GenFolderMetaDataWorkData = pair->second;
    GenFolderMetaDataWorkData->WorkPath = std::move(folderPath);
    GenFolderMetaDataWorkData->FinishDelegate = Delegate;
    GenFolderMetaDataWorkData->OutProcess = std::make_shared<GenFolderMetaDataProcess_t>();
    GenFolderMetaDataWorkData->OutFolderManifest = std::make_shared<FolderManifest_t>();

    return pair->first;
}

bool FFileBackupManager::GenFolderChunkDataAddHash(CommonHandle_t handle, TGetNextHashPairCB CB)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return false;
    }
    auto& pFolderWorkData=itr->second;
    const char* weakHashBytes;
    const char* strongHashBytes;
    while (CB(weakHashBytes, strongHashBytes)) {
        auto [pair, res] = pFolderWorkData->AllHashMap.try_emplace(*(WeakHash_t*)weakHashBytes, std::set<std::string>{std::string(strongHashBytes)});
        if (!res) {
            pair->second.insert(strongHashBytes);
        }
    }
    return true;
}

IFileBackupManagerInterface::TOneFileChunkDataTask FFileBackupManager::GenFolderChunkDataGetNextFileTask(CommonHandle_t handle, TNewFileChunkDelegate NewFileChunkDelegate)
{
    auto itr = GenFolderMetaDataWorkDataList.find(handle);
    if (itr == GenFolderMetaDataWorkDataList.end()) {
        return nullptr;
    }
    auto& pFolderWorkData = itr->second;
    std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData;
    {
        std::scoped_lock lock(pFolderWorkData->DirItrMtx);
        auto& i = pFolderWorkData->DirItr;
        for (; i != std::filesystem::recursive_directory_iterator(); ++i)
        {
            if (!i->is_directory()) {
                break;
            }
        }
        if (i == std::filesystem::recursive_directory_iterator()) {
            return nullptr;
        }
        pFileTaskData = std::make_shared<GenFolderChunkDataFileTaskData_t>();
        pFileTaskData->FilePath = i->path().lexically_normal();
        pFileTaskData->FilePathStr = (const char*)pFileTaskData->FilePath.u8string().c_str();
        pFileTaskData->FileSize = i->file_size();
        pFileTaskData->FileChunkBuf = std::make_shared<FileChunkBuf_t>();
        i++;
    }

    pFileTaskData->ZSTDBufSize = ZSTD_compressBound(FileChunkSize);
    pFileTaskData->ZSTDBuf = malloc(pFileTaskData->ZSTDBufSize);
    pFileTaskData->CCtx = ZSTD_createCCtx();
    pFileTaskData->NewFileChunkDelegate=NewFileChunkDelegate;
    mbedtls_md5_init(&pFileTaskData->MD5ctx);
    mbedtls_md5_init(&pFileTaskData->FileMD5ctx);
    mbedtls_md5_starts(&pFileTaskData->FileMD5ctx);
    {
        std::scoped_lock lock(pFolderWorkData->FileTaskMtx);
        std::shared_ptr<FileChunkBuf_t> pFileChunkBuf;
        if (pFolderWorkData->FileChunkBufPool.size() > 0) {
            pFileChunkBuf = pFolderWorkData->FileChunkBufPool.back();
            pFolderWorkData->FileChunkBufPool.pop_back();
        }
        else {
            pFileChunkBuf = std::make_shared<FileChunkBuf_t>();
        }
        pFolderWorkData->FileTasks.try_emplace(pFileTaskData->FilePathStr, pFileTaskData);
    }
    TOneFileChunkDataTask func = std::bind(FFileBackupManager::GenFolderChunkDataFileTask, *this, pFolderWorkData, pFileTaskData);
    return func;
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

    std::shared_lock lock(pFolderWorkData->FolderChunkDataMtx, std::try_to_lock);
    if (lock.owns_lock()) {
        *pFolderWorkData->OutFolderManifest = pFolderWorkData->FolderManifest;
    }
    return pFolderWorkData->OutFolderManifest;
}

void FFileBackupManager::Tick(float delta)
{
    std::set<CommonHandle_t> needDel;
    for (auto& [handle, pFolderWorkData] : GenFolderMetaDataWorkDataList) {
        switch (pFolderWorkData->Status) {
        case EGenFolderMetaDataStatus::None: {
            pFolderWorkData->DirItr = std::filesystem::recursive_directory_iterator(pFolderWorkData->WorkPath);
            for (auto i = std::filesystem::recursive_directory_iterator(pFolderWorkData->WorkPath);
                i != std::filesystem::recursive_directory_iterator();
                ++i)
            {
                pFolderWorkData->ToltalSize += i->file_size();
            }
            pFolderWorkData->Status = EGenFolderMetaDataStatus::Inited;
            break;
        }
        case EGenFolderMetaDataStatus::Inited: {
            if (pFolderWorkData->DirItr == std::filesystem::recursive_directory_iterator() &&
                pFolderWorkData->FileTasks.empty()){
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

void FFileBackupManager::GenFolderChunkDataFileTask(this FFileBackupManager& self,std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData,std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData)
{
    typedef struct FileChunkCache_t {
        uint64_t StartPos;
        uint32_t WeakHash;
        unsigned char StrongHash[16];
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
    std::ifstream input(pFileTaskData->FilePath, std::ios::binary);
    unsigned char output[16];
    //auto startPos = input.tellg();
    uint64_t lastChunkEndPos{ 0 };
    std::streamoff consumedBytes{ 0 };
    int32_t waitAppendDataLen = pFileTaskData->FileSize % FileChunkSize == 0 ? 0 : FileChunkSize - pFileTaskData->FileSize % FileChunkSize;


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

    auto caculateFileHash = [&](const unsigned char* content ,uint32_t len) {
        mbedtls_md5_update(&pFileTaskData->FileMD5ctx, (const unsigned char*)content, len);
        };

    auto processChunkChacheFunc = [&]() {

        bool fAll = fileChunkCacheContainer.back()->StartPos + FileChunkSize >= pFileTaskData->FileSize;
        while (!fileChunkCacheContainer.empty()) {
            auto pChunkCache = fileChunkCacheContainer.front();
            auto chunkCache = *pChunkCache;
            auto ChunkEndPos = chunkCache.StartPos + FileChunkSize;
            if (!fAll) {
                if (consumedBytes - chunkCache.StartPos < FileChunkSize * 3) {
                    return;
                }
            }
            else {
                ChunkEndPos = ChunkEndPos > pFileTaskData->FileSize ? pFileTaskData->FileSize : ChunkEndPos;
            }
            pFolderWorkData->CompleteSize += ChunkEndPos - lastChunkEndPos;
            lastChunkEndPos = ChunkEndPos;
            fileChunkCacheContainer.pop_front();

            if (chunkCache.fChunkAlreadyExist) {
                return;
            }

            char* rawData = FileChunkBuf.GetContinuousConsumedBuf(consumedBytes - chunkCache.StartPos - FileChunkSize, FileChunkSize);
            if (!chunkCache.fStrongHash) {
                mbedtls_md5_starts(&pFileTaskData->MD5ctx);
                mbedtls_md5_update(&pFileTaskData->MD5ctx, (const unsigned char*)rawData, FileChunkSize);
                mbedtls_md5_finish(&pFileTaskData->MD5ctx, chunkCache.StrongHash);
            }
            auto strongHashStr = std::string((const char*)chunkCache.StrongHash);
            auto [pair, res] = pFolderWorkData->AllHashMap.try_emplace(*(WeakHash_t*)&chunkCache.WeakHash, std::set<std::string>{strongHashStr});
            if (!res) {
                if (pair->second.find(strongHashStr) != pair->second.end()) {
                    continue;
                }
                pair->second.insert(strongHashStr);
            }
            to_upper_hex(pFileTaskData->HexBuf, (uint8_t*)&chunkCache.WeakHash, sizeof(chunkCache.WeakHash));
            to_upper_hex(pFileTaskData->HexBuf + sizeof(hasher.hashvalue) * 2, chunkCache.StrongHash, sizeof(chunkCache.StrongHash));
            size_t const cSize = ZSTD_compressCCtx(pFileTaskData->CCtx, pFileTaskData->ZSTDBuf, pFileTaskData->ZSTDBufSize, rawData, FileChunkSize, 1);
            pFileTaskData->NewFileChunkDelegate(pFileTaskData->HexBuf, uint32_t(sizeof(chunkCache.WeakHash) * 2 + sizeof(chunkCache.StrongHash) * 2), (const char*)pFileTaskData->ZSTDBuf, cSize);
            std::shared_ptr<FileChunksData_t>pChunkData;
            {
                std::scoped_lock lock(pFolderWorkData->FolderChunkDataMtx);
                auto itr = pFolderWorkData->FolderManifest.Files.find(pFileTaskData->FilePathStr);
                if (itr == pFolderWorkData->FolderManifest.Files.end()) {
                    pChunkData = pFolderWorkData->FolderManifest.Files.try_emplace(pFileTaskData->FilePathStr, std::make_shared<FileChunksData_t>()).first->second;
                }
                else {
                    pChunkData = itr->second;
                }
            }
            FileChunkData_t ChunkData{ "",chunkCache.StartPos};
            memcpy(ChunkData.HexName, pFileTaskData->HexBuf, sizeof(pFileTaskData->HexBuf));
            pChunkData->Chunks.insert(ChunkData);
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
        if (fExist) {
            auto& firstCache = fileChunkCacheContainer.front();
            switch (fileChunkCacheContainer.size()) {
            case 2: {
                if (firstCache->StartPos + FileChunkSize * 2 >= consumedBytes) {
                    fileChunkCacheContainer.pop_back();
                }
                internalCacheNewFunc(consumedBytes, weakhash, stronghash, fExist);
                break;
            }
            case 3: {
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
                    auto hashItr = pFolderWorkData->AllHashMap.find(hasher.hashvalue);
                    if (hashItr == pFolderWorkData->AllHashMap.end()) {
                        internalCacheNewFunc(endPos, weakHash, output, false);
                    }
                    else {
                        auto stronHashItr = hashItr->second.find(std::string((const char*)output));
                        if (stronHashItr != hashItr->second.end()) {
                            internalCacheNewFunc(endPos, weakHash, output, true);
                        }
                        else {
                            internalCacheNewFunc(endPos, weakHash, output, false);
                        }
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

    while (!input.eof() || waitAppendDataLen > 0) {
        //read file to FileChunkBuf
        while (!input.eof()) {
            auto emptyBuf = FileChunkBuf.GetEmptyBuf();
            if (std::get<1>(emptyBuf) == 0) {
                break;
            }
            input.read(std::get<0>(emptyBuf), std::get<1>(emptyBuf));
            auto extractLen = input.gcount();
            if (extractLen == 0) {
                break;
            }
            caculateFileHash((const unsigned char*)std::get<0>(emptyBuf), extractLen);
            FileChunkBuf.FillSize(extractLen);
        }
        while (waitAppendDataLen > 0) {
            if (!input.eof()) {
                break;
            }
            auto emptyBuf = FileChunkBuf.GetEmptyBuf();
            if (std::get<1>(emptyBuf) == 0) {
                break;
            }
            memset(std::get<0>(emptyBuf), 0, std::get<1>(emptyBuf));
            waitAppendDataLen -= std::get<1>(emptyBuf);
            FileChunkBuf.FillSize(std::get<1>(emptyBuf));
        }
        //consume FileChunkBuf
        auto ContentBuf = FileChunkBuf.GetContentBuf();
        for (int i = 0; i < std::get<1>(ContentBuf); i++) {
            auto ConsumedBuf = FileChunkBuf.GetConsumedBuf(0, FileChunkSize);
            auto inBytePtr = std::get<0>(ContentBuf) + i;
            FileChunkBuf.EatSize(1);
            consumedBytes++;
            bytesAfterLastChunk++;
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
                hasher.update(*inBytePtr, *std::get<0>(std::get<0>(ConsumedBuf)));
            }

            auto hashItr = pFolderWorkData->AllHashMap.find(hasher.hashvalue);
            //hash not exist
            if (hashItr == pFolderWorkData->AllHashMap.end()) {
                cacheNewFunc(hasher.hashvalue);
                if (consumedBytes > pFileTaskData->FileSize) {
                    break;
                }
                continue;
            }

            //weak hash exist
            caculateHashInConsumedBuf(0, output);
            auto stronHashItr = hashItr->second.find(std::string((const char*)output));
            if (stronHashItr != hashItr->second.end()) {
                //strong hash exist
                cacheNewFunc(hasher.hashvalue, nullptr, true);
                if (consumedBytes > pFileTaskData->FileSize) {
                    break;
                }
                continue;
            }
            else {
                //hash not exist
                cacheNewFunc(hasher.hashvalue, output);
                if (consumedBytes > pFileTaskData->FileSize) {
                    break;
                }
                continue;
            }
        }
    }
    {
        std::scoped_lock lock(pFolderWorkData->FileTaskMtx);
        pFolderWorkData->FileTasks.erase(pFileTaskData->FilePathStr);
        mbedtls_md5_finish(&pFileTaskData->MD5ctx, output);
        to_upper_hex(pFolderWorkData->FolderManifest.Files[pFileTaskData->FilePathStr]->FileHash, output, sizeof(output));
    }

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
