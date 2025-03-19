#include "FolderRecoverHelper.h"
#include "FileBackupInternal.h"
#include <filesystem>
#include <shared_mutex>
#include <algorithm>
#include <mutex>
#include <list>
#include <fstream>
#include <cstring>
#include <map>
#include <stdio.h>
#include <FunctionExitHelper.h>
#include <string_convert.h>
#include <raw_file.h>
#include <assert.h>
#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#elif defined(__linux)
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif
constexpr uint8_t MaxChunkConstructTaskNum = 8;

typedef struct SourceChunkReverseCheckData_t {
    std::u8string_view FileName;
    const uint64_t& StartPos;
}SourceChunkReverseCheckData_t;

struct pFileChunkDataLess {
    bool operator()(const std::shared_ptr<FileChunkData_t>& _Left, const std::shared_ptr<FileChunkData_t>& _Right) const
    {
        return _Left->StartPos < _Right->StartPos;
    }
};
typedef struct FolderRecoverWorkData_t {
    ~FolderRecoverWorkData_t() {
        for (auto& pConstructTask : ConstructTaskPool) {
            delete[] pConstructTask->FileChunkBuf;
            delete pConstructTask->ChunkConverter;
        }
    }
    void ReverseBuf(uint8_t capacity , std::shared_ptr <const FolderManifest_t> manifest) {
        if (ConstructTaskPool.size() > capacity) {
            return;
        }
        for (int i = capacity - ConstructTaskPool.size(); i > 0; i--) {
            auto pConstructTask = std::make_shared<ConstructChunkData_t>();
            pConstructTask->ChunkConverter = new FChunkConverter(EConvertDirection::ToFileChunk);
            pConstructTask->FileChunkBuf = new uint8_t[FileChunkSize];
            
            pConstructTask->ChunkFileMaxSize = manifest->ChunkFileMaxSize;
            ConstructTaskPool.push_back(pConstructTask);
        }
    }
    std::atomic<EFolderRecoverStatus> Status{ EFolderRecoverStatus::FRS_None };
    std::shared_ptr <const FolderManifest_t> Manifest;
    std::shared_ptr <const FolderManifest_t> SourceManifest;
    IFolderRecoverHelperInterface::TConstructStatusChangedDelegate Delegate;
    std::set<std::u8string_view> FilesNeedDelete;
    typedef std::set<std::shared_ptr<FileChunkData_t>, struct pFileChunkDataLess> TFilesNeedRecoverChunks;
    typedef struct FilesNeedRecoverChunksData_t{
        std::shared_mutex RawFileMtx;
        CRawFile RawFile;
        uint64_t FileSize;
        TFilesNeedRecoverChunks Chunks;
    }FilesNeedRecoverChunksData_t;
    typedef std::unordered_map<std::u8string_view, std::shared_ptr<FilesNeedRecoverChunksData_t>> TFilesNeedRecover;
    std::string TempFolder;
    TFilesNeedRecover FilesNeedRecover;
    std::shared_mutex NextTaskMtx;
    TFilesNeedRecover::const_iterator NextFileItr{};
    TFilesNeedRecoverChunks::const_iterator NextChunkItr{};
    uint32_t AllFileChunkNum{ 0 };
    std::atomic_uint32_t FileCount{ 0 };
    std::atomic_uint32_t FileChunkCount{ 0 };
    std::list<std::shared_ptr<ConstructChunkData_t>> ConstructTaskPool;
    std::unordered_map<std::u8string_view, SourceChunkReverseCheckData_t> SourceChunks;
}FolderRecoverWorkData_t;



class FFolderRecoverHelper :public IFolderRecoverHelperInterface {
public:
    CommonHandle_t AddTask(std::shared_ptr < const  FolderManifest_t> manifest, std::shared_ptr < const  FolderManifest_t> sourceManifest, TConstructStatusChangedDelegate Delegate) override;
    FolderRecoverProgress_t& GetFolderRecoverProcess(CommonHandle_t handle)override {
        return OutProcess;
    }

    std::optional<const ReserveFileSpaceData_t> GetReserveNextFileSpaceData(CommonHandle_t)override;
    void ReserveFileSpaceComplete(CommonHandle_t, ReserveFileSpaceData_t)override;
    bool ImplementReserveFileSpace(CommonHandle_t recoverHandle,ReserveFileSpaceData_t& ConstructChunkData, std::u8string_view tempPathStr)override;

    std::shared_ptr<const ConstructChunkData_t> GetConstructNextChunkData(CommonHandle_t) override;
    void ChunkConstructComplete(CommonHandle_t, std::shared_ptr<const ConstructChunkData_t>) override;
    bool ImplementForLocalChunkConstruct(CommonHandle_t recoverHandle,std::shared_ptr<const ConstructChunkData_t> ConstructChunkData, std::u8string_view workPathStr, std::u8string_view chunkFolderPathStr) override;

    std::optional<std::u8string_view> GetNextFileNeedMove(CommonHandle_t) override;
    void FileMoveComplete(CommonHandle_t, std::u8string_view) override;
    bool ImplementFileMove(CommonHandle_t, std::u8string_view, std::u8string_view workPathStr) override;

    void Tick(float delta) override;

    FolderRecoverProgress_t OutProcess;
    std::unordered_map<CommonHandle_t, std::shared_ptr<FolderRecoverWorkData_t>>FolderRecoverWorkDataList;
};


CommonHandle_t FFolderRecoverHelper::AddTask(std::shared_ptr < const FolderManifest_t> manifest, std::shared_ptr < const  FolderManifest_t> sourceManifest, TConstructStatusChangedDelegate Delegate)
{
    auto pFolderRecoverWorkData = std::make_shared<FolderRecoverWorkData_t>();
    auto& FolderRecoverWorkData = *pFolderRecoverWorkData;
    FolderRecoverWorkData.Manifest = manifest;
    FolderRecoverWorkData.SourceManifest = sourceManifest;
    auto res=FolderRecoverWorkDataList.try_emplace(CommonHandle_t::atomic_count, pFolderRecoverWorkData);
    if (!res.second) {
        return NullHandle;
    }

    FolderRecoverWorkData.Delegate = Delegate;
    FolderRecoverWorkData.ReverseBuf(MaxChunkConstructTaskNum, manifest);
    for (auto& [fileName, fileChunkData] : sourceManifest->Files) {
        for (auto& pChunkData : fileChunkData->Chunks) {
            auto& chunkData = *pChunkData;
            FolderRecoverWorkData.SourceChunks.try_emplace(ConvertStringTotU8View(chunkData.HexName), fileName, chunkData.StartPos);
        }
        auto itr = manifest->Files.find(fileName);
        if (itr == manifest->Files.end()) {
            FolderRecoverWorkData.FilesNeedDelete.insert(fileName);
        }
    }

    for (auto& [fileName, fileChunkData] : manifest->Files) {
        auto itr = sourceManifest->Files.find(fileName);
        if (itr == sourceManifest->Files.end()|| memcmp(itr->second->FileHash, fileChunkData->FileHash, StrongHashBit / 4)!=0) {
            auto [ritr, res] = FolderRecoverWorkData.FilesNeedRecover.try_emplace(fileName,std::make_shared<FolderRecoverWorkData_t::FilesNeedRecoverChunksData_t>());
            auto& [rfileName, pChunksData] = *ritr;
            auto& chunksData = *pChunksData;
            chunksData.FileSize = fileChunkData->FileSize;
            for (auto& chunkData : fileChunkData->Chunks) {
                chunksData.Chunks.emplace(chunkData);
                FolderRecoverWorkData.AllFileChunkNum++;
            }
        }
    }
    FolderRecoverWorkData.NextFileItr = FolderRecoverWorkData.FilesNeedRecover.begin();
    FolderRecoverWorkData.Status = EFolderRecoverStatus::FRS_ReserveSpace;
    OutProcess.AllFileChunkNum = FolderRecoverWorkData.AllFileChunkNum;
    OutProcess.AllFileNum = FolderRecoverWorkData.FilesNeedRecover.size();
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
    auto& [FileName, pChunksData] = *FolderRecoverWorkData.NextFileItr;
    auto& chunksData = *pChunksData;
    FunctionExitHelper_t helper(
        [&]() {
            FolderRecoverWorkData.NextFileItr++;
        }
    );
    return ReserveFileSpaceData_t{ FileName, chunksData.FileSize};
}

void FFolderRecoverHelper::ReserveFileSpaceComplete(CommonHandle_t handle, ReserveFileSpaceData_t data)
{
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return ;
    }
    auto& FolderRecoverWorkData = *itr->second;
    FolderRecoverWorkData.FileCount++;
    OutProcess.FileCount = FolderRecoverWorkData.FileCount;
}

bool FFolderRecoverHelper::ImplementReserveFileSpace(CommonHandle_t recoverHandle, ReserveFileSpaceData_t& ConstructChunkData, std::u8string_view tempPathStr)
{
    auto itr = FolderRecoverWorkDataList.find(recoverHandle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return false;
    }
    auto& FolderRecoverWorkData = *itr->second;
    if (FolderRecoverWorkData.Status != EFolderRecoverStatus::FRS_ReserveSpace) {
        return false;
    }

    std::error_code ec;
    std::filesystem::path tempFilePath(tempPathStr);
    tempFilePath /= ConstructChunkData.FileName;
    FolderRecoverWorkData.TempFolder.assign((const char*)tempPathStr.data(), (const char*)tempPathStr.data()+tempPathStr.size());
    if (FolderRecoverWorkData.FilesNeedRecover[ConstructChunkData.FileName]->RawFile.Open(tempFilePath.u8string().c_str(), UTIL_CREATE_ALWAYS, ConstructChunkData.FileSize)) {
        return false;
    }
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

    //auto fileItr = FolderRecoverWorkData.Manifest->Files.find(FolderRecoverWorkData.NextFileItr->data());
    while (true) {
        auto& [FileName, pChunksData] = *FolderRecoverWorkData.NextFileItr;
        auto& chunksData = *pChunksData;
        if (FolderRecoverWorkData.NextChunkItr != chunksData.Chunks.cend()) {
            break;
        }
        else {
            FolderRecoverWorkData.NextFileItr++;
            if (FolderRecoverWorkData.NextFileItr == FolderRecoverWorkData.FilesNeedRecover.end()) {
                return nullptr;
            }
            auto& [FileName, pChunksData] = *FolderRecoverWorkData.NextFileItr;
            auto& chunksData = *pChunksData;
            FolderRecoverWorkData.NextChunkItr = chunksData.Chunks.begin();
        }
    }
    auto& [FileName, pChunksData] = *FolderRecoverWorkData.NextFileItr;
    auto& chunksData = *pChunksData;
    auto pConstructTask = FolderRecoverWorkData.ConstructTaskPool.back();
    FolderRecoverWorkData.ConstructTaskPool.pop_back();
    lock.unlock();
    FunctionExitHelper_t helper(
        [&]() {
            FolderRecoverWorkData.NextChunkItr++;
        }
    );

    auto& pFileChunkData = *FolderRecoverWorkData.NextChunkItr;
    auto& fileChunkData = *pFileChunkData;
    auto chunkName = FolderRecoverWorkData.SourceManifest->GetHexNameView(fileChunkData.HexName);
    auto sourceChunkItr = FolderRecoverWorkData.SourceChunks.find(chunkName);
    if (sourceChunkItr== FolderRecoverWorkData.SourceChunks.end()) {
        pConstructTask->ChunkSourceData = chunkName;
    }
    else {
        pConstructTask->ChunkSourceData = ChunkFromFileSource_t{ sourceChunkItr->second.FileName,sourceChunkItr->second.StartPos};
    }
    pConstructTask->TagetFileName = FileName;
    pConstructTask->TagetFileSize = chunksData.FileSize;
    pConstructTask->TagetFileStartPos = fileChunkData.StartPos;
    return pConstructTask;
}

void FFolderRecoverHelper::ChunkConstructComplete(CommonHandle_t handle, std::shared_ptr<const ConstructChunkData_t> pConstructChunkData)
{
    auto& ConstructChunkData = *pConstructChunkData;
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return;
    }
    auto& FolderRecoverWorkData = *itr->second;
    FolderRecoverWorkData.FileChunkCount++;
    {
        std::scoped_lock lock(FolderRecoverWorkData.NextTaskMtx);
        FolderRecoverWorkData.ConstructTaskPool.push_back(std::const_pointer_cast<ConstructChunkData_t>(pConstructChunkData));
    }
    OutProcess.FileChunkCount = FolderRecoverWorkData.FileChunkCount;
}

bool FFolderRecoverHelper::ImplementForLocalChunkConstruct(CommonHandle_t recoverHandle, std::shared_ptr<const ConstructChunkData_t> pConstructChunkData, std::u8string_view workPathStr, std::u8string_view chunkFolderPathStr)
{
    auto& ConstructChunkData = *std::const_pointer_cast<ConstructChunkData_t>( pConstructChunkData);
    auto pChunkFromFileSource = std::get_if<ChunkFromFileSource_t>(&ConstructChunkData.ChunkSourceData);
    auto pChunkHexName = std::get_if<std::u8string_view>(&ConstructChunkData.ChunkSourceData);

    auto itr = FolderRecoverWorkDataList.find(recoverHandle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return false;
    }
    auto& FolderRecoverWorkData = *itr->second;
    if (FolderRecoverWorkData.Status != EFolderRecoverStatus::FRS_ReconstructFile) {
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
        ifs.read((char*)ChunkFileBuf, ConstructChunkData.ChunkFileMaxSize);
        auto ChunkFileLen=ifs.gcount();
        ConstructChunkData.ChunkConverter->UpdateChunkFileSize(ChunkFileLen);
        ConstructChunkData.ChunkConverter->Convert(ConstructChunkData.FileChunkBuf);
    }
    auto writeSize = std::min(ConstructChunkData.TagetFileSize - ConstructChunkData.TagetFileStartPos, (uint64_t)FileChunkSize);

    {
        std::scoped_lock lock(FolderRecoverWorkData.FilesNeedRecover[ConstructChunkData.TagetFileName]->RawFileMtx);
        FolderRecoverWorkData.FilesNeedRecover[ConstructChunkData.TagetFileName]->RawFile.Seek(ConstructChunkData.TagetFileStartPos);
        FolderRecoverWorkData.FilesNeedRecover[ConstructChunkData.TagetFileName]->RawFile.Write(ConstructChunkData.FileChunkBuf, writeSize);
    }

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
    auto& [FileName, Chunks] = *FolderRecoverWorkData.NextFileItr;
    FunctionExitHelper_t helper(
        [&]() {
            FolderRecoverWorkData.NextFileItr++;
        }
    );
    return FileName;
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

bool FFolderRecoverHelper::ImplementFileMove(CommonHandle_t recoverHandle, std::u8string_view FileRelativePathStr, std::u8string_view workPathStr)
{
    auto itr = FolderRecoverWorkDataList.find(recoverHandle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return false;
    }
    auto& FolderRecoverWorkData = *itr->second;
    if (FolderRecoverWorkData.Status != EFolderRecoverStatus::FRS_MoveFile) {
        return false;
    }

    std::error_code ec;
    std::filesystem::path targetFilePath(workPathStr);
    targetFilePath /= FileRelativePathStr;
    std::filesystem::path tempFilePath(FolderRecoverWorkData.FilesNeedRecover[FileRelativePathStr]->RawFile.GetFileName());
    std::filesystem::create_directories(targetFilePath.parent_path(), ec);
    if (ec) {
        return false;
    }
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
                auto& [FileName, pChunksData] = *FolderRecoverWorkData.NextFileItr;
                auto& chunksData = *pChunksData;
                if (FolderRecoverWorkData.NextFileItr != FolderRecoverWorkData.FilesNeedRecover.end()) {
                    FolderRecoverWorkData.NextChunkItr = chunksData.Chunks.begin();
                }
                FolderRecoverWorkData.Status = EFolderRecoverStatus::FRS_ReconstructFile;
                FolderRecoverWorkData.Delegate(FolderRecoverWorkData.Status);
            }
            break;
        }
        case EFolderRecoverStatus::FRS_ReconstructFile: {
            if (FolderRecoverWorkData.FileChunkCount == FolderRecoverWorkData.AllFileChunkNum) {
                for (auto [fname ,pChunksData] : pFolderRecoverWorkData->FilesNeedRecover) {
                    pChunksData->RawFile.Close();
                }
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
                if (!FolderRecoverWorkData.TempFolder.empty()) {
                    std::filesystem::remove_all(FolderRecoverWorkData.TempFolder);
                }
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

