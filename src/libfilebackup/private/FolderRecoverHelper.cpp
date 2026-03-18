#include "FolderRecoverHelper.h"
#include "FileBackupInternal.h"

#include <FunctionExitHelper.h>
#include <string_convert.h>
#include <RawFile.h>
#include <dir_util.h>

#include <moodycamel/concurrentqueue.h>
#include <assert.h>
#include <filesystem>
#include <shared_mutex>
#include <algorithm>
#include <mutex>
#include <list>
#include <fstream>
#include <cstring>
#include <map>
#include <stdio.h>
constexpr uint8_t MaxChunkConstructTaskNum = 8;

enum EFolderRecoverStatus
{
    FRS_None,
    FRS_ReserveFile,
    FRS_Finished
};

typedef struct SourceChunkReverseCheckData_t {
    std::u8string_view FileName;
    const uint64_t& StartPos;
}SourceChunkReverseCheckData_t;

typedef struct FileNeedRecoverData_t {
    std::shared_ptr<FileChunksData_t> FileData;
    TFileChunks NeedRecoverChunks;
}FileNeedRecoverData_t;

typedef struct RecoverFileTaskData_t {
    std::shared_ptr<FileNeedRecoverData_t> FileNeedRecoverData;
    FRawFile TargetFile;
    FRawFile SourceFile;
    uint8_t* FileChunkBuf{ nullptr };
    IChunkConverter* ChunkConverter{ nullptr };
    void Clear() {
        TargetFile.Close();
        SourceFile.Close();
    }
}RecoverFileTaskData_t;
typedef struct FolderRecoverWorkData_t {
    ~FolderRecoverWorkData_t() {
        for (auto& pTask : FileTaskPool) {
            delete[] pTask->FileChunkBuf;
            delete pTask->ChunkConverter;
        }
    }
    std::shared_ptr <RecoverFileTaskData_t> GetFileTask() {
        std::shared_ptr <RecoverFileTaskData_t> pFileTaskData;
        if (FileTaskPool.size() > 0) {
            pFileTaskData = FileTaskPool.back();
            FileTaskPool.pop_back();
            pFileTaskData->Clear();
        }
        else {
            pFileTaskData = std::make_shared<RecoverFileTaskData_t>();
            pFileTaskData->FileChunkBuf = new uint8_t[FileChunkSize];
            pFileTaskData->ChunkConverter = new FChunkConverter(EConvertDirection::ToFileChunk);
        }
        return pFileTaskData;
    }
    std::shared_ptr <const FolderManifest_t> Manifest;
    std::shared_ptr <const FolderManifest_t> SourceManifest;
    FolderRecoverProgress OutProcess;
    std::set<std::u8string_view> FilesNeedDelete;
    IFolderRecoverHelperInterface::TRecoverFolderFinishDelegate FinishDelegate;
    std::atomic<EFolderRecoverStatus> Status;

    std::filesystem::path WorkFolder;
    std::filesystem::path ChunkFolder;
    std::filesystem::path TempFolder;
    uint32_t FileCount{ 0 };
    std::atomic<uint32_t> FileChunkCount{ 0 };
    typedef std::unordered_map<std::u8string_view, std::shared_ptr<FileNeedRecoverData_t>> TFilesNeedRecover;
    TFilesNeedRecover FilesNeedRecover;
    typedef struct ChunkCompleteEvent_t {
        std::shared_ptr<FileChunksData_t> FileInfo;
        std::shared_ptr<FileChunkData_t> ChunkInfo;
    }ChunkCompleteEvent_t;
    moodycamel::ConcurrentQueue<ChunkCompleteEvent_t> ChunkCompleteQueue;

    FolderRecoverWorkData_t::ChunkCompleteEvent_t ChunkEventCache[5];
    std::atomic<std::error_code> ErrorCode;
    std::unordered_map<std::u8string_view, std::shared_ptr<RecoverFileTaskData_t>> FileTasks;
    std::vector<std::shared_ptr<RecoverFileTaskData_t>> FileTaskPool;
    std::unordered_map<std::u8string_view, SourceChunkReverseCheckData_t> SourceChunks;
}FolderRecoverWorkData_t;




class FFolderRecoverHelper :public IFolderRecoverHelperInterface {
public:

    CommonHandle32_t AddTask(std::shared_ptr < const  FolderManifest_t> manifest, std::shared_ptr < const  FolderManifest_t> sourceManifest, std::u8string_view workDirStr, std::u8string_view chunkDirStr, std::u8string_view tempDirStr, TRecoverFolderFinishDelegate delegate) override;
    std::tuple<uint32_t, uint32_t, std::optional<std::reference_wrapper<FolderRecoverProgress>>> GetFolderRecoverProcess(CommonHandle32_t handle)override;

    std::tuple<IFolderRecoverHelperInterface::TOneFileRecoverTask, IFolderRecoverHelperInterface::TOneFileRecoverPostProcessingTask> GetNextRecoverFileTask(CommonHandle32_t) override;

    void Tick(float delta) override;


    void RecoverFileTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData);
    void RecoverFilePostProcessingTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData);



    std::unordered_map<CommonHandle32_t, std::shared_ptr<FolderRecoverWorkData_t>>FolderRecoverWorkDataList;
};


CommonHandle32_t FFolderRecoverHelper::AddTask(std::shared_ptr < const FolderManifest_t> manifest, std::shared_ptr < const  FolderManifest_t> sourceManifest, std::u8string_view workDirStr, std::u8string_view chunkDirStr, std::u8string_view tempDirStr, TRecoverFolderFinishDelegate delegate)
{
    auto pFolderRecoverWorkData = std::make_shared<FolderRecoverWorkData_t>();
    auto& FolderRecoverWorkData = *pFolderRecoverWorkData;
    FolderRecoverWorkData.Manifest = manifest;
    FolderRecoverWorkData.SourceManifest = sourceManifest;
    auto res = FolderRecoverWorkDataList.try_emplace(CommonHandle32_t::atomic_count, pFolderRecoverWorkData);
    if (!res.second) {
        return NullHandle;
    }
    if (sourceManifest) {
        for (auto& [fileName, fileChunkData] : sourceManifest->Files) {
            for (auto& pChunkData : fileChunkData->Chunks) {
                auto& chunkData = *pChunkData;
                FolderRecoverWorkData.SourceChunks.try_emplace(ConvertViewToU8View(chunkData.HexName), fileName, chunkData.StartPos);
            }
            auto itr = manifest->Files.find(fileName);
            if (itr == manifest->Files.end()) {
                FolderRecoverWorkData.FilesNeedDelete.insert(fileName);
            }
        }
    }
    FolderRecoverWorkData.WorkFolder.assign((const char*)workDirStr.data(), (const char*)workDirStr.data() + workDirStr.size());
    FolderRecoverWorkData.ChunkFolder.assign((const char*)chunkDirStr.data(), (const char*)chunkDirStr.data() + chunkDirStr.size());
    FolderRecoverWorkData.TempFolder.assign((const char*)tempDirStr.data(), (const char*)tempDirStr.data() + tempDirStr.size());
    FolderRecoverWorkData.OutProcess.Init(*manifest);
    FolderRecoverWorkData.OutProcess.GetFolderRecoverProgressHeader().bTempFolderExist = DirUtil::IsExist(FolderRecoverWorkData.TempFolder.u8string());
    FolderRecoverWorkData.Status = EFolderRecoverStatus::FRS_ReserveFile;
    FolderRecoverWorkData.FinishDelegate = delegate;
    for (auto& [fileName, pFileInfo] : manifest->OrderedFiles) {
        auto& fileInfo = *pFileInfo;
        if (sourceManifest) {
            memcpy(FolderRecoverWorkData.OutProcess.GetFolderRecoverProgressHeader().SourceID, sourceManifest->ID, sizeof(FolderRecoverProgress::FolderRecoverProgressHeader_t::SourceID));
            auto itr = sourceManifest->Files.find(fileName);
            if (itr != sourceManifest->Files.end()) {
                auto& sourceFileChunkData = *itr->second;
                if (memcpy(sourceFileChunkData.FileHash, fileInfo.FileHash, sizeof(sourceFileChunkData.FileHash)) == 0) {
                    for (auto& pChunk : fileInfo.Chunks) {
                        FolderRecoverWorkData.OutProcess.SetFileChunkStatus(FolderRecoverWorkData.OutProcess.GetFileProgressHeader(fileInfo.Index), pChunk->Index);
                    }
                    continue;
                }
            }
        }
        auto [ritr, res] = FolderRecoverWorkData.FilesNeedRecover.try_emplace(fileName, std::make_shared<FileNeedRecoverData_t>());
        auto& [rfileName, pChunksData] = *ritr;
        auto& chunksData = *pChunksData;
        chunksData.FileData = pFileInfo;
        for (auto& chunkData : fileInfo.Chunks) {
            chunksData.NeedRecoverChunks.emplace(chunkData);
        }
        FolderRecoverWorkData.OutProcess.GetFileProgressHeader(fileInfo.Index).bNeedRecover = true;
    }
    return res.first->first;
}

std::tuple<uint32_t, uint32_t, std::optional<std::reference_wrapper<FolderRecoverProgress>>> FFolderRecoverHelper::GetFolderRecoverProcess(CommonHandle32_t handle)
{
    static FolderRecoverProgress empty;
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return { 0,0,std::nullopt };
    }
    auto& pFolderWorkData = itr->second;

    return { pFolderWorkData->FileCount,    pFolderWorkData->FileChunkCount,pFolderWorkData->OutProcess };
}

std::tuple<IFolderRecoverHelperInterface::TOneFileRecoverTask, IFolderRecoverHelperInterface::TOneFileRecoverPostProcessingTask> FFolderRecoverHelper::GetNextRecoverFileTask(CommonHandle32_t handle)
{
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return { nullptr,nullptr };
    }
    auto& pFolderWorkData = itr->second;
    if (pFolderWorkData->FilesNeedRecover.empty()) {
        return { nullptr,nullptr };
    }
    if (pFolderWorkData->ErrorCode.load()) {
        return { nullptr,nullptr };
    }
    auto fileItr = pFolderWorkData->FilesNeedRecover.begin();
    auto& pFileWorkData = fileItr->second;

    std::shared_ptr< RecoverFileTaskData_t> pFileTaskData = pFolderWorkData->GetFileTask();
    pFileTaskData->FileNeedRecoverData = pFileWorkData;
    auto [_, res] = pFolderWorkData->FileTasks.try_emplace(ConvertViewToU8View(pFileTaskData->FileNeedRecoverData->FileData->FileName), pFileTaskData);
    if (!res) {
        return { nullptr,nullptr };
    }
    std::filesystem::path outFilePath = pFolderWorkData->TempFolder / pFileTaskData->FileNeedRecoverData->FileData->FileName;
    if (pFileTaskData->TargetFile.Open(outFilePath.u8string(), UTIL_OPEN_ALWAYS, pFileTaskData->FileNeedRecoverData->FileData->FileSize) != ERR_SUCCESS) {
        return { nullptr,nullptr };
    }

    TOneFileRecoverTask func = std::bind(&FFolderRecoverHelper::RecoverFileTask, *this, pFolderWorkData, pFileTaskData);
    TOneFileRecoverPostProcessingTask readFileTick = std::bind(&FFolderRecoverHelper::RecoverFilePostProcessingTask, *this, pFolderWorkData, pFileTaskData);
    pFolderWorkData->FilesNeedRecover.erase(fileItr);
    return{ func,readFileTick };
}


void FFolderRecoverHelper::RecoverFileTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData)
{
    auto& FolderRecoverWorkData = *pFolderWorkData;
    auto& FileTaskData = *pFileTaskData;
    uint32_t readed;
    int32_t ires;
    for (auto& NeedRecoverChunk : FileTaskData.FileNeedRecoverData->NeedRecoverChunks) {
        auto chunkName = GetHexNameView(NeedRecoverChunk->HexName);

        auto sourceChunkItr = FolderRecoverWorkData.SourceChunks.find(chunkName);
        if (sourceChunkItr != FolderRecoverWorkData.SourceChunks.end()) {
            auto [_, SourceChunkReverseCheckData] = *sourceChunkItr;
            std::filesystem::path filePath(FolderRecoverWorkData.WorkFolder);
            filePath /= SourceChunkReverseCheckData.FileName;
            ires = FileTaskData.SourceFile.Open(filePath.u8string(), UTIL_OPEN_EXISTING);
            if (ires != ERR_SUCCESS) {
                auto expected = std::error_code();
                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
                return;
            }
            ires = FileTaskData.SourceFile.Seek(SourceChunkReverseCheckData.StartPos);
            if (ires != ERR_SUCCESS) {
                auto expected = std::error_code();
                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
                return;
            }
            ires = FileTaskData.SourceFile.Read(pFileTaskData->FileChunkBuf, FileChunkSize, readed);
            if (ires != ERR_SUCCESS) {
                auto expected = std::error_code();
                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
                return;
            }
            memset(pFileTaskData->FileChunkBuf + readed, 0, FileChunkSize - readed);
        }
        else {
            std::filesystem::path filePath(FolderRecoverWorkData.ChunkFolder);
            filePath /= chunkName;
            ires = FileTaskData.SourceFile.Open(filePath.u8string(), UTIL_OPEN_EXISTING);
            if (ires != ERR_SUCCESS) {
                auto expected = std::error_code();
                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
                return;
            }
            auto ChunkFileBuf = FileTaskData.ChunkConverter->GetChunkFileBuf();
            ires = FileTaskData.SourceFile.Read(ChunkFileBuf, pFolderWorkData->Manifest->ChunkFileMaxSize, readed);
            if (ires != ERR_SUCCESS) {
                auto expected = std::error_code();
                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
                return;
            }
            FileTaskData.ChunkConverter->UpdateChunkFileSize(readed);
            FileTaskData.ChunkConverter->Convert(FileTaskData.FileChunkBuf);
        }
        auto writeSize = std::min(FileTaskData.FileNeedRecoverData->FileData->FileSize - NeedRecoverChunk->StartPos, (uint64_t)FileChunkSize);
        ires = FileTaskData.TargetFile.Seek(NeedRecoverChunk->StartPos);
        if (ires != ERR_SUCCESS) {
            auto expected = std::error_code();
            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
            return;
        }
        ires = FileTaskData.TargetFile.Write(pFileTaskData->FileChunkBuf, writeSize);
        if (ires != ERR_SUCCESS) {
            auto expected = std::error_code();
            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
            return;
        }
        ++FolderRecoverWorkData.FileChunkCount;
        FolderRecoverWorkData.ChunkCompleteQueue.enqueue(FolderRecoverWorkData_t::ChunkCompleteEvent_t{ FileTaskData.FileNeedRecoverData->FileData, NeedRecoverChunk });
    }
    FileTaskData.TargetFile.Close();
    FileTaskData.SourceFile.Close();
}


void FFolderRecoverHelper::RecoverFilePostProcessingTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData)
{
    auto& FolderRecoverWorkData = *pFolderWorkData;
    auto& FileTaskData = *pFileTaskData;
    FolderRecoverWorkData.FileTasks.erase(ConvertViewToU8View(pFileTaskData->FileNeedRecoverData->FileData->FileName));
    FolderRecoverWorkData.FileTaskPool.push_back(pFileTaskData);
    FolderRecoverWorkData.FileCount++;
    auto& pathBuf = *FPathBuf::GetThreadSingleton();
    if (FolderRecoverWorkData.FileCount == FolderRecoverWorkData.OutProcess.GetFolderRecoverProgressHeader().AllFileNum) {
        for (auto fileName : FolderRecoverWorkData.FilesNeedDelete) {
            std::filesystem::path delPath = FolderRecoverWorkData.WorkFolder / fileName;
            DirUtil::Delete(delPath.u8string());
        }
        for (int i = 0; i < FolderRecoverWorkData.OutProcess.GetFolderRecoverProgressHeader().AllFileNum; i++) {
            auto& fileHeader = FolderRecoverWorkData.OutProcess.GetFileProgressHeader(i);
            if (!fileHeader.bNeedRecover) {
                continue;
            }
            std::filesystem::path tempFilePath = FolderRecoverWorkData.TempFolder / FolderRecoverWorkData.OutProcess.GetFileName(fileHeader);
            std::filesystem::path destFilePath = FolderRecoverWorkData.WorkFolder / FolderRecoverWorkData.OutProcess.GetFileName(fileHeader);
            DirUtil::Rename(tempFilePath.u8string(), destFilePath.u8string());
        }

        if (!FolderRecoverWorkData.OutProcess.GetFolderRecoverProgressHeader().bTempFolderExist) {
            DirUtil::Delete(FolderRecoverWorkData.TempFolder.u8string());
        }


        FolderRecoverWorkData.Status = EFolderRecoverStatus::FRS_Finished;
    }
}

void FFolderRecoverHelper::Tick(float delta)
{
    std::set<CommonHandle32_t> needDel;
    for (auto& [handle, pFolderRecoverWorkData] : FolderRecoverWorkDataList) {
        auto& FolderRecoverWorkData = *pFolderRecoverWorkData;

        switch (FolderRecoverWorkData.Status)
        {
        case EFolderRecoverStatus::FRS_ReserveFile: {
            auto outSize = FolderRecoverWorkData.ChunkCompleteQueue.try_dequeue_bulk(FolderRecoverWorkData.ChunkEventCache, sizeof(FolderRecoverWorkData.ChunkEventCache) / sizeof(FolderRecoverWorkData_t::ChunkCompleteEvent_t));
            if (outSize == 0) {
                break;
            }
            for (int i = 0; i < outSize; i++) {
                FolderRecoverWorkData.OutProcess.SetFileChunkStatus(FolderRecoverWorkData.OutProcess.GetFileProgressHeader(FolderRecoverWorkData.ChunkEventCache[i].FileInfo->Index), FolderRecoverWorkData.ChunkEventCache[i].ChunkInfo->Index);
                //todo write temp file
            }
            break;
        }
        case EFolderRecoverStatus::FRS_Finished: {
            auto ec = FolderRecoverWorkData.ErrorCode.load();
            FolderRecoverWorkData.FinishDelegate(ec);
            needDel.insert(handle);
            break;
        }
        default:
            break;
        }
    }
    for (auto& h : needDel) {
        FolderRecoverWorkDataList.erase(h);
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

