#include "FolderRecoverHelper.h"
#include "FileBackupInternal.h"
#include <filesystem>
#include <shared_mutex>
#include <algorithm>
#include <mutex>
#include <list>
#include <fstream>
#include <stdio.h>
#include <FunctionExitHelper.h>
#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#elif defined(__linux)
#include <unistd.h>
#endif
constexpr uint8_t MaxChunkConstructTaskNum = 8;

typedef struct SourceChunkReverseCheckData_t {
    std::u8string_view FileName;
    const uint64_t& StartPos;
}SourceChunkReverseCheckData_t;

typedef struct FolderRecoverWorkData_t {
    ~FolderRecoverWorkData_t() {
        for (auto& pConstructTask : ConstructTaskPool) {
            delete[] pConstructTask->FileChunkBuf;
            delete pConstructTask->ChunkConverter;
        }
    }
    void ReverseBuf(uint8_t capacity) {
        if (ConstructTaskPool.size() > capacity) {
            return;
        }
        for (int i = capacity - ConstructTaskPool.size(); i > 0; i--) {
            auto pConstructTask = std::make_shared<ConstructChunkData_t>();
            pConstructTask->ChunkConverter = new FChunkConverter(EConvertDirection::ToFileChunk);
            pConstructTask->FileChunkBuf = new uint8_t[FileChunkSize];
            ConstructTaskPool.push_back(pConstructTask);
        }
    }
    std::atomic<EFolderRecoverStatus> Status{ EFolderRecoverStatus::FRS_None };
    std::shared_ptr <const FolderManifest_t> Manifest;
    std::shared_ptr <const FolderManifest_t> SourceManifest;
    IFolderRecoverHelperInterface::TConstructStatusChangedDelegate Delegate;
    std::set<std::u8string_view> FilesNeedDelete;
    std::set<std::u8string_view> FilesNeedRecover;

    std::shared_mutex NextTaskMtx;
    std::set<std::u8string_view>::const_iterator NextFileItr{};
    std::set<FileChunkData_t>::const_iterator NextChunkItr;
    uint32_t AllFileChunkNum{ 0 };
    std::atomic_uint32_t FileCount{ 0 };
    std::atomic_uint32_t FileChunkCount{ 0 };

    std::list<std::shared_ptr<ConstructChunkData_t>> ConstructTaskPool;

    std::unordered_map<std::string, SourceChunkReverseCheckData_t> SourceChunks;

}FolderRecoverWorkData_t;



class FFolderRecoverHelper :public IFolderRecoverHelperInterface {
public:
    CommonHandle_t AddTask(std::shared_ptr < const  FolderManifest_t> manifest, std::shared_ptr < const  FolderManifest_t> sourceManifest, TConstructStatusChangedDelegate Delegate) override;
    FolderRecoverProcess_t GetFolderRecoverProcess(CommonHandle_t handle)override {
        return FolderRecoverProcess_t();
    }

    std::optional<const ReserveFileSpaceData_t> GetReserveNextFileSpaceData(CommonHandle_t)override;
    void ReserveFileSpaceComplete(CommonHandle_t, ReserveFileSpaceData_t)override;
    bool ImplementReserveFileSpace(ReserveFileSpaceData_t& ConstructChunkData, std::u8string_view tempPathStr)override;

    std::shared_ptr<const ConstructChunkData_t> GetConstructNextChunkData(CommonHandle_t) override;
    void ChunkConstructComplete(CommonHandle_t, std::shared_ptr<const ConstructChunkData_t>) override;
    bool ImplementForLocalChunkConstruct(std::shared_ptr<const ConstructChunkData_t> ConstructChunkData, std::u8string_view workPathStr, std::u8string_view chunkFolderPathStr, std::u8string_view tempPathStr) override;

    std::optional<std::u8string_view> GetNextFileNeedMove(CommonHandle_t) override;
    void FileMoveComplete(CommonHandle_t, std::u8string_view) override;
    bool ImplementFileMove(std::u8string_view, std::u8string_view workPathStr, std::u8string_view tempPathStr) override;

    void Tick(float delta) override;

    
    std::unordered_map<CommonHandle_t, std::shared_ptr<FolderRecoverWorkData_t>>FolderRecoverWorkDataList;
};


CommonHandle_t FFolderRecoverHelper::AddTask(std::shared_ptr < const FolderManifest_t> manifest, std::shared_ptr < const  FolderManifest_t> sourceManifest, TConstructStatusChangedDelegate Delegate)
{
    auto pFolderRecoverWorkData = std::make_shared<FolderRecoverWorkData_t>();
    auto& FolderRecoverWorkData = *pFolderRecoverWorkData;
    FolderRecoverWorkData.Manifest = manifest;
    FolderRecoverWorkData.SourceManifest = sourceManifest;
    auto res=FolderRecoverWorkDataList.try_emplace(CommonHandle_t(), pFolderRecoverWorkData);
    if (!res.second) {
        return NullHandle;
    }
    ;
    FolderRecoverWorkData.Delegate = Delegate;
    FolderRecoverWorkData.ReverseBuf(MaxChunkConstructTaskNum);
    for (auto& pair : sourceManifest->Files) {
        auto& fileChunkData = *pair.second;
        for (auto& chunkData : fileChunkData.Chunks) {
            FolderRecoverWorkData.SourceChunks.try_emplace(chunkData.HexName, (const char8_t*)pair.first.c_str(), chunkData.StartPos);
        }
        auto itr = manifest->Files.find(pair.first);
        if (itr == manifest->Files.end()) {
            FolderRecoverWorkData.FilesNeedDelete.insert((const char8_t*)pair.first.c_str());
        }
    }

    for (auto& pair : manifest->Files) {
        auto& fileChunkData = *pair.second;
        auto itr = sourceManifest->Files.find(pair.first);
        if (itr == sourceManifest->Files.end()|| memcmp(itr->second->FileHash, fileChunkData.FileHash, StrongHashBit / 4)!=0) {
            FolderRecoverWorkData.FilesNeedRecover.insert((const char8_t*)pair.first.c_str());
            for (auto& chunkData : fileChunkData.Chunks) {
                FolderRecoverWorkData.AllFileChunkNum++;
            }
        }
    }
    FolderRecoverWorkData.NextFileItr = FolderRecoverWorkData.FilesNeedRecover.begin();
    FolderRecoverWorkData.Status = EFolderRecoverStatus::FRS_ReserveSpace;
    FolderRecoverWorkData.Delegate(FolderRecoverWorkData.Status);
    return res.first->first;
}

std::optional<const ReserveFileSpaceData_t> FFolderRecoverHelper::GetReserveNextFileSpaceData(CommonHandle_t handle)
{
    auto itr=FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return std::nullopt;
    }
    auto& FolderRecoverWorkData=*itr->second;
    if (FolderRecoverWorkData.Status != EFolderRecoverStatus::FRS_ReserveSpace) {
        return std::nullopt;
    }
    std::unique_lock lock{ FolderRecoverWorkData.NextTaskMtx,std::try_to_lock };
    if (!lock.owns_lock() || FolderRecoverWorkData.NextFileItr == FolderRecoverWorkData.FilesNeedRecover.end()) {
        return std::nullopt;
    }
    auto fileItr=FolderRecoverWorkData.Manifest->Files.find((const char*)FolderRecoverWorkData.NextFileItr->data());
    FunctionExitHelper_t helper(
        [&]() {
            FolderRecoverWorkData.NextFileItr++;
        }
    );
    return ReserveFileSpaceData_t{ FolderRecoverWorkData.NextFileItr->data(), fileItr->second->FileSize};
}

void FFolderRecoverHelper::ReserveFileSpaceComplete(CommonHandle_t handle, ReserveFileSpaceData_t data)
{
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return ;
    }
    auto& FolderRecoverWorkData = *itr->second;
    FolderRecoverWorkData.FileCount++;
}

bool FFolderRecoverHelper::ImplementReserveFileSpace(ReserveFileSpaceData_t& ConstructChunkData, std::u8string_view tempPathStr)
{
    std::error_code ec;
    std::filesystem::path tempFilePath(tempPathStr);
    tempFilePath /= ConstructChunkData.FileName;

#ifdef WIN32
    int fh;
    errno_t err;
    err = _wsopen_s(&fh, (const wchar_t*)tempFilePath.u16string().c_str(), _O_RDWR | _O_CREAT, _SH_DENYNO,
        _S_IREAD | _S_IWRITE);
    if (err != 0) {
        return false;
    }
    err = _chsize(fh, ConstructChunkData.FileSize);
    if (err != 0) {
        return false;
    }
    _close(fh);
#elif defined(__linux)
    ftruncate();
#endif
    return true;
}

std::shared_ptr<const ConstructChunkData_t> FFolderRecoverHelper::GetConstructNextChunkData(CommonHandle_t handle)
{
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return nullptr;
    }
    auto& FolderRecoverWorkData = *itr->second;
    if (FolderRecoverWorkData.Status != EFolderRecoverStatus::FRS_ReconstructFile) {
        return nullptr;
    }

    std::unique_lock lock{ FolderRecoverWorkData.NextTaskMtx,std::try_to_lock };
    if (!lock.owns_lock()) {
        return nullptr;
    }

    if (FolderRecoverWorkData.ConstructTaskPool.size() <=0) {
        return nullptr;
    }

    if (FolderRecoverWorkData.NextFileItr == FolderRecoverWorkData.FilesNeedRecover.end()) {
        return nullptr;
    }
    auto fileItr = FolderRecoverWorkData.Manifest->Files.find((const char*)FolderRecoverWorkData.NextFileItr->data());
    while (FolderRecoverWorkData.NextChunkItr == fileItr->second->Chunks.end()) {
        FolderRecoverWorkData.NextFileItr++;
        if (FolderRecoverWorkData.NextFileItr == FolderRecoverWorkData.FilesNeedRecover.end()) {
            return nullptr;
        }
        fileItr = FolderRecoverWorkData.Manifest->Files.find((const char*)FolderRecoverWorkData.NextFileItr->data());
        FolderRecoverWorkData.NextChunkItr = fileItr->second->Chunks.begin();
    }
    FunctionExitHelper_t helper(
        [&]() {
            FolderRecoverWorkData.NextChunkItr++;
        }
    );
    auto pConstructTask = FolderRecoverWorkData.ConstructTaskPool.back();
    FolderRecoverWorkData.ConstructTaskPool.pop_back();
    lock.unlock();
    auto sourceChunkItr = FolderRecoverWorkData.SourceChunks.find(FolderRecoverWorkData.NextChunkItr->HexName);
    if (sourceChunkItr== FolderRecoverWorkData.SourceChunks.end()) {
        pConstructTask->ChunkSourceData = (const char8_t*)FolderRecoverWorkData.NextChunkItr->HexName;
    }
    else {
        pConstructTask->ChunkSourceData = ChunkFromFileSource_t{ sourceChunkItr->second.FileName,sourceChunkItr->second.StartPos};
    }
    pConstructTask->TagetFileName = *FolderRecoverWorkData.NextFileItr;
    pConstructTask->TagetFileSize = fileItr->second->FileSize;
    pConstructTask->TagetFileStartPos = FolderRecoverWorkData.NextChunkItr->StartPos;
    return pConstructTask;
}

void FFolderRecoverHelper::ChunkConstructComplete(CommonHandle_t handle, std::shared_ptr<const ConstructChunkData_t> taskData)
{
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return;
    }
    auto& FolderRecoverWorkData = *itr->second;
    FolderRecoverWorkData.FileChunkCount++;
    {
        std::scoped_lock lock(FolderRecoverWorkData.NextTaskMtx);
        FolderRecoverWorkData.ConstructTaskPool.push_back(std::const_pointer_cast<ConstructChunkData_t>(taskData));
    }
}

bool FFolderRecoverHelper::ImplementForLocalChunkConstruct(std::shared_ptr<const ConstructChunkData_t> pConstructChunkData, std::u8string_view workPathStr, std::u8string_view chunkFolderPathStr, std::u8string_view tempPathStr)
{
    auto& ConstructChunkData = *std::const_pointer_cast<ConstructChunkData_t>( pConstructChunkData);
    auto pChunkFromFileSource = std::get_if<ChunkFromFileSource_t>(&ConstructChunkData.ChunkSourceData);
    auto pChunkHexName = std::get_if<std::u8string_view>(&ConstructChunkData.ChunkSourceData);

    std::filesystem::path targetFilePath(tempPathStr);
    targetFilePath /= ConstructChunkData.TagetFileName;
    std::ofstream ofs(targetFilePath, std::ios::binary);
    if (!ofs.is_open()) {
        return false;
    }

    if (pChunkFromFileSource) {
        std::filesystem::path filePath(workPathStr);
        filePath /= pChunkFromFileSource->FileName;
        std::ifstream ifs(filePath,std::ios::binary);
        if (!ifs.is_open()) {
            return false;
        }
        ifs.seekg(pChunkFromFileSource->StartPos);
        ifs.read((char*)ConstructChunkData.FileChunkBuf, FileChunkSize);
    }
    else {
        std::filesystem::path chunkPath(chunkFolderPathStr);
        chunkPath/= *pChunkHexName;
        std::ifstream ifs(chunkPath, std::ios::binary);
        if (!ifs.is_open()) {
            return false;
        }
        auto ChunkFileBuf = ConstructChunkData.ChunkConverter->GetChunkFileBuf();
        auto& ChunkFileLen = ConstructChunkData.ChunkConverter->GetChunkFileSize();
        ifs.read((char*)ChunkFileBuf, ConstructChunkData.ChunkConverter->GetChunkFileBufSize());
        ChunkFileLen=ifs.gcount();
        ConstructChunkData.ChunkConverter->Convert(ConstructChunkData.FileChunkBuf);
    }

    ofs.seekp(ConstructChunkData.TagetFileStartPos);
    ofs.write((const char*)ConstructChunkData.FileChunkBuf, std::min(ConstructChunkData.TagetFileSize - ConstructChunkData.TagetFileStartPos, (uint64_t)FileChunkSize));
    ofs.close();
    return true;
}

std::optional<std::u8string_view> FFolderRecoverHelper::GetNextFileNeedMove(CommonHandle_t handle)
{
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return std::nullopt;
    }
    auto& FolderRecoverWorkData = *itr->second;
    if (FolderRecoverWorkData.Status != EFolderRecoverStatus::FRS_MoveFile) {
        return std::nullopt;
    }
    std::unique_lock lock{ FolderRecoverWorkData.NextTaskMtx,std::try_to_lock };
    if (!lock.owns_lock() || FolderRecoverWorkData.NextFileItr == FolderRecoverWorkData.FilesNeedRecover.end()) {
        return std::nullopt;
    }
    FunctionExitHelper_t helper(
        [&]() {
            FolderRecoverWorkData.NextFileItr++;
        }
    );
    return FolderRecoverWorkData.NextFileItr->data();
}

void FFolderRecoverHelper::FileMoveComplete(CommonHandle_t handle, std::u8string_view)
{
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return ;
    }
    auto& FolderRecoverWorkData = *itr->second;
    FolderRecoverWorkData.FileCount++;
}

bool FFolderRecoverHelper::ImplementFileMove(std::u8string_view FileRelativePathStr, std::u8string_view workPathStr, std::u8string_view tempPathStr)
{
    std::error_code ec;
    std::filesystem::path targetFilePath(workPathStr);
    targetFilePath /= FileRelativePathStr;
    std::filesystem::path tempFilePath(tempPathStr);
    tempFilePath /= FileRelativePathStr;
    std::filesystem::rename(tempFilePath, targetFilePath, ec);
    if (ec) {
        return false;
    }
    return true;;
}

void FFolderRecoverHelper::Tick(float delta)
{
    for (auto& [handle, pFolderRecoverWorkData] : FolderRecoverWorkDataList) {
        auto& FolderRecoverWorkData = *pFolderRecoverWorkData;
        switch (FolderRecoverWorkData.Status)
        {
        case EFolderRecoverStatus::FRS_ReserveSpace: {
            if (FolderRecoverWorkData.FilesNeedRecover.size() == FolderRecoverWorkData.FileCount) {
                FolderRecoverWorkData.NextFileItr = FolderRecoverWorkData.FilesNeedRecover.begin();
                if (FolderRecoverWorkData.NextFileItr != FolderRecoverWorkData.FilesNeedRecover.end()) {
                    auto fileItr = FolderRecoverWorkData.Manifest->Files.find((const char*)FolderRecoverWorkData.NextFileItr->data());
                    FolderRecoverWorkData.NextChunkItr = fileItr->second->Chunks.begin();
                }
                FolderRecoverWorkData.Status = EFolderRecoverStatus::FRS_ReconstructFile;
                FolderRecoverWorkData.Delegate(FolderRecoverWorkData.Status);
            }
            break;
        }
        case EFolderRecoverStatus::FRS_ReconstructFile: {
            if (FolderRecoverWorkData.FileChunkCount == FolderRecoverWorkData.AllFileChunkNum) {
                FolderRecoverWorkData.FileCount = 0;
                FolderRecoverWorkData.NextFileItr = FolderRecoverWorkData.FilesNeedRecover.begin();
                FolderRecoverWorkData.Status = EFolderRecoverStatus::FRS_MoveFile;
                FolderRecoverWorkData.Delegate(FolderRecoverWorkData.Status);
            }
            break;
        }
        case EFolderRecoverStatus::FRS_MoveFile: {
            if (FolderRecoverWorkData.FilesNeedRecover.size() == FolderRecoverWorkData.FileCount) {
                FolderRecoverWorkData.Status = EFolderRecoverStatus::FRS_Finished;
                FolderRecoverWorkData.Delegate(FolderRecoverWorkData.Status);
            }
            break;
        }
        default:
            break;
        }
    }
}



LIB_FILEBACKUP_EXPORT IFolderRecoverHelperInterface* GetFolderRecoverHelperInstance()
{
    static std::atomic<std::shared_ptr<FFolderRecoverHelper>> AtomicPtr;
    auto oldptr = AtomicPtr.load();
    if (!oldptr) {
        std::shared_ptr<FFolderRecoverHelper> pManager(new FFolderRecoverHelper);
        AtomicPtr.compare_exchange_strong(oldptr, pManager);
    }
    return AtomicPtr.load().get();
}

