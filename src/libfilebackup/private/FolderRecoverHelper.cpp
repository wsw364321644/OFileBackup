#include "FolderRecoverHelper.h"
#include "FileBackupInternal.h"
#include "FolderRecoverProgressImpl.h"
#include <FunctionExitHelper.h>
#include <string_convert.h>
#include <RawFile.h>
#include <dir_util.h>
#include <simple_error.h>
#include <singleton.h>
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

FolderRecoverProgress::~FolderRecoverProgress()
{
    if (FileBackedBuffer) {
        FreeFileBackedBuffer(FileBackedBuffer);
    }
}


class FFolderRecoverHelper :public IFolderRecoverHelperInterface {
public:

    CommonHandle32_t AddTask(std::shared_ptr < const  FolderManifest_t> manifest, std::shared_ptr < const  FolderManifest_t> sourceManifest, std::u8string_view workDirStr, std::u8string_view chunkDirStr, std::u8string_view tempDirStr, TRecoverFoldeStatusChangedDelegate delegate) override;
    std::optional<std::reference_wrapper<FolderRecoverProgress>> GetFolderRecoverProcess(CommonHandle32_t handle)override;

    std::tuple<IFolderRecoverHelperInterface::TOneFileRecoverTask, IFolderRecoverHelperInterface::TOneFileRecoverPostProcessingTask> GetNextRecoverFileTask(CommonHandle32_t) override {
        return {};
    }
    //IFolderRecoverHelperInterface::TRecoverTask GetRecoverBySourceTask(CommonHandle32_t) override;
    IFolderRecoverHelperInterface::TOneChunkRecoverTask GetRecoverByChunkTask(CommonHandle32_t,std::u8string_view chunkHexName, bool bFromSource) override;
    //IFolderRecoverHelperInterface::TFinishRecoverTask GetFinishRecoverTask(CommonHandle32_t) override;

    void Tick(float delta) override;
    void IOTick(float delta) override;

    //void RecoverBySourceTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData);
    void RecoverByChunkTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData, std::u8string_view chunkHexName, bool bFromSource);
    void FinishRecoverTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData);

    std::unordered_map<CommonHandle32_t, std::shared_ptr<FolderRecoverWorkData_t>>FolderRecoverWorkDataList;
    moodycamel::ConcurrentQueue<std::function<void()>> FnishWorkQueue;
    moodycamel::ConcurrentQueue<std::shared_ptr<FolderRecoverWorkData_t>> SaveReqQueue;
    std::unordered_set<std::shared_ptr<FolderRecoverWorkData_t>> SaveReqCache;
};


CommonHandle32_t FFolderRecoverHelper::AddTask(std::shared_ptr < const FolderManifest_t> manifest, std::shared_ptr < const  FolderManifest_t> sourceManifest, std::u8string_view workDirStr, std::u8string_view chunkDirStr, std::u8string_view tempDirStr, TRecoverFoldeStatusChangedDelegate delegate)
{
    std::error_code ec;
    auto pFolderRecoverWorkData = std::make_shared<FolderRecoverWorkData_t>();
    auto& FolderRecoverWorkData = *pFolderRecoverWorkData;


    FolderRecoverWorkData.WorkFolder= workDirStr;
    FolderRecoverWorkData.ChunkFolder=chunkDirStr;
    FolderRecoverWorkData.TempFolder=tempDirStr;
    FolderRecoverWorkData.RecoverProcess.Init(pFolderRecoverWorkData,manifest, sourceManifest,ec);
    if (ec) {
        return NullHandle;
    }

    FolderRecoverWorkData.SetStatus(EFolderRecoverStatus::FRS_RecoverFile);
    FolderRecoverWorkData.StatusDelegate = delegate;

    auto res = FolderRecoverWorkDataList.try_emplace(CommonHandle32_t::atomic_count, pFolderRecoverWorkData);
    if (!res.second) {
        return NullHandle;
    }
    return res.first->first;
}

std::optional<std::reference_wrapper<FolderRecoverProgress>> FFolderRecoverHelper::GetFolderRecoverProcess(CommonHandle32_t handle)
{

    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return std::nullopt ;
    }
    auto& pFolderWorkData = itr->second;

    return pFolderWorkData->RecoverProcess;
}

//IFolderRecoverHelperInterface::TRecoverTask FFolderRecoverHelper::GetRecoverBySourceTask(CommonHandle32_t handle)
//{
//    auto itr = FolderRecoverWorkDataList.find(handle);
//    if (itr == FolderRecoverWorkDataList.end()) {
//        return nullptr ;
//    }
//    auto& pFolderWorkData = itr->second;
//    if (pFolderWorkData->RecoverProcess.FilesNeedRecover.size()==0) {
//        return nullptr;
//    }
//    if (pFolderWorkData->ErrorCode.load()) {
//        return nullptr;
//    }
//
//
//    std::shared_ptr< RecoverFileTaskData_t> pFileTaskData = pFolderWorkData->GetFileTask();
//    pFileTaskData->FilesNeedRecover = pFolderWorkData->RecoverProcess.FilesNeedRecover;
//    auto [_, res] = pFolderWorkData->FileTasks.emplace(pFileTaskData);
//    if (!res) {
//        return nullptr;
//    }
//
//    TRecoverTask func = [this, pFolderWorkData, pFileTaskData]() {
//        RecoverBySourceTask(pFolderWorkData, pFileTaskData);
//        };
//    return func ;
//}

IFolderRecoverHelperInterface::TOneChunkRecoverTask FFolderRecoverHelper::GetRecoverByChunkTask(CommonHandle32_t handle, std::u8string_view chunkHexName, bool bFromSource)
{
    auto itr = FolderRecoverWorkDataList.find(handle);
    if (itr == FolderRecoverWorkDataList.end()) {
        return nullptr;
    }
    auto& pFolderWorkData = itr->second;
    if (pFolderWorkData->RecoverProcess.FilesNeedRecover.size() == 0) {
        return nullptr;
    }
    if (pFolderWorkData->ErrorCode.load()) {
        return nullptr;
    }


    std::shared_ptr< RecoverFileTaskData_t> pFileTaskData = pFolderWorkData->GetFileTask();
    pFileTaskData->FilesNeedRecover = pFolderWorkData->RecoverProcess.FilesNeedRecover;
    auto [_, res] = pFolderWorkData->FileTasks.emplace(pFileTaskData);
    if (!res) {
        return nullptr;
    }

    TOneChunkRecoverTask func = [this, pFolderWorkData, pFileTaskData, chunkHexName, bFromSource]() {
        RecoverByChunkTask(pFolderWorkData, pFileTaskData, chunkHexName, bFromSource);
        };
    return func;
}

//IFolderRecoverHelperInterface::TFinishRecoverTask FFolderRecoverHelper::GetFinishRecoverTask(CommonHandle32_t handle)
//{
//    auto itr = FolderRecoverWorkDataList.find(handle);
//    if (itr == FolderRecoverWorkDataList.end()) {
//        return nullptr;
//    }
//    auto& pFolderWorkData = itr->second;
//    if (pFolderWorkData->RecoverProcess.FilesNeedRecover.size() != 0) {
//        return nullptr;
//    }
//    if (pFolderWorkData->ErrorCode.load()) {
//        return nullptr;
//    }
//
//    TOneFileRecoverTask func = std::bind(&FFolderRecoverHelper::FinishRecoverTask, *this, pFolderWorkData);
//    return func;
//}


//void FFolderRecoverHelper::RecoverBySourceTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData)
//{
//    auto& FolderRecoverWorkData = *pFolderWorkData;
//    auto& FileTaskData = *pFileTaskData;
//    uint32_t readed;
//    int32_t ires;
//
//    for (auto&[ fileName, pFileNeedRecoverData] : FileTaskData.FilesNeedRecover) {
//        ires = FileTaskData.TargetFile.Open((FolderRecoverWorkData.TempFolder / fileName).u8string(), UTIL_OPEN_ALWAYS, pFileNeedRecoverData->FileData->FileSize);
//        if (ires != ERR_SUCCESS) {
//            auto expected = std::error_code();
//            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
//            return;
//        }
//        if (pFileNeedRecoverData->NeedRecoverChunks.size()==0) {
//            assert(false);
//            FolderRecoverWorkData.ChunkCompleteQueue.enqueue({ pFileNeedRecoverData,nullptr});
//        }
//        for (auto& pNeedRecoverChunk : pFileNeedRecoverData->NeedRecoverChunks) {
//            auto& NeedRecoverChunk = *pNeedRecoverChunk;
//            if (!NeedRecoverChunk.ConstructChunkData->bFromSource) {
//                continue;
//            }
//            auto reverseItr=FolderRecoverWorkData.RecoverProcess.CompareResult->SourceChunkReverseIndex.find(GetHexNameView(NeedRecoverChunk.ConstructChunkData->ChunkData->HexName));
//            auto& [pSourceFileData,pSourceFileChunkData]=reverseItr->second.ChunkInFileData[0];
//            std::filesystem::path filePath(FolderRecoverWorkData.WorkFolder);
//            filePath /= pSourceFileData->FileName;
//
//            ires = FileTaskData.SourceFile.Open(filePath.u8string(), UTIL_OPEN_EXISTING);
//            if (ires != ERR_SUCCESS) {
//                auto expected = std::error_code();
//                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
//                return;
//            }
//            ires = FileTaskData.SourceFile.Seek(pSourceFileChunkData->StartPos);
//            if (ires != ERR_SUCCESS) {
//                auto expected = std::error_code();
//                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
//                return;
//            }
//            ires = FileTaskData.SourceFile.Read(pFileTaskData->FileChunkBuf, FileChunkSize, readed);
//            if (ires != ERR_SUCCESS) {
//                auto expected = std::error_code();
//                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
//                return;
//            }
//
//            auto writeSize = std::min(pFileNeedRecoverData->FileData->FileSize - NeedRecoverChunk.ConstructChunkData->ChunkData->StartPos, (uint64_t)FileChunkSize);
//            ires = FileTaskData.TargetFile.Seek(NeedRecoverChunk.ConstructChunkData->ChunkData->StartPos);
//            if (ires != ERR_SUCCESS) {
//                auto expected = std::error_code();
//                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
//                return;
//            }
//            ires = FileTaskData.TargetFile.Write(pFileTaskData->FileChunkBuf, writeSize);
//            if (ires != ERR_SUCCESS) {
//                auto expected = std::error_code();
//                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
//                return;
//            }
//            FolderRecoverWorkData.ChunkCompleteQueue.enqueue(FolderRecoverWorkData_t::ChunkCompleteEvent_t{ pFileNeedRecoverData, pNeedRecoverChunk });
//        }
//    }
//    FileTaskData.SourceFile.Close();
//    FileTaskData.TargetFile.Close();
//    FolderRecoverWorkData.FileTaskQueue.enqueue(pFileTaskData);
//}

void FFolderRecoverHelper::RecoverByChunkTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData, std::u8string_view chunkHexName, bool bFromSource)
{
    auto& FolderRecoverWorkData = *pFolderWorkData;
    auto& FileTaskData = *pFileTaskData;
    uint32_t readed;
    int32_t ires;


    auto itr=FolderRecoverWorkData.RecoverProcess.CompareResult->TargetChunkReverseIndex.find(chunkHexName);
    if (itr == FolderRecoverWorkData.RecoverProcess.CompareResult->TargetChunkReverseIndex.end()) {
        auto expected = std::error_code();
        FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
        return;
    }

    if (bFromSource) {
        auto sourceItr = FolderRecoverWorkData.RecoverProcess.CompareResult->SourceChunkReverseIndex.find(chunkHexName);
        if (sourceItr == FolderRecoverWorkData.RecoverProcess.CompareResult->SourceChunkReverseIndex.end()) {
            auto expected = std::error_code();
            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
            return;
        }
        auto& [fileName, chunksData] = *sourceItr->second.begin();
        std::filesystem::path filePath(FolderRecoverWorkData.WorkFolder);
        filePath /= fileName;
        auto& pChunData = *chunksData.begin();
        ires = FileTaskData.SourceFile.Open(filePath.u8string(), UTIL_OPEN_EXISTING);
        if (ires != ERR_SUCCESS) {
            auto expected = std::error_code();
            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
            return;
        }
        ires = FileTaskData.SourceFile.Seek(pChunData->StartPos);
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
        filePath /= chunkHexName;
        ires = FileTaskData.SourceFile.Open(filePath.u8string(), UTIL_OPEN_EXISTING);
        if (ires != ERR_SUCCESS) {
            auto expected = std::error_code();
            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
            return;
        }
        auto ChunkFileBuf = FileTaskData.ChunkConverter->GetChunkFileBuf();
        ires = FileTaskData.SourceFile.Read(ChunkFileBuf, pFolderWorkData->RecoverProcess.Manifest->ChunkFileMaxSize, readed);
        if (ires != ERR_SUCCESS) {
            auto expected = std::error_code();
            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
            return;
        }
        FileTaskData.ChunkConverter->UpdateChunkFileSize(readed);
        FileTaskData.ChunkConverter->Convert(FileTaskData.FileChunkBuf);
    }
    for (auto& [fileName, FileChunksData] : itr->second) {
        auto fileItr=pFolderWorkData->RecoverProcess.Manifest->Files.find(fileName);
        if (fileItr == pFolderWorkData->RecoverProcess.Manifest->Files.end()) {
            auto expected = std::error_code();
            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::invalid_argument));
            return;
        }
        auto& [_,pFileData] = *fileItr;

        ires = FileTaskData.TargetFile.Open((FolderRecoverWorkData.TempFolder / pFileData->FileName).u8string(), UTIL_OPEN_ALWAYS, pFileData->FileSize);
        if (ires != ERR_SUCCESS) {
            auto expected = std::error_code();
            FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
            return;
        }

        for (auto& pFileChunkData : FileChunksData) {
            auto writeSize = std::min(pFileData->FileSize - pFileChunkData->StartPos, (uint64_t)FileChunkSize);
            ires = FileTaskData.TargetFile.Write(pFileTaskData->FileChunkBuf, writeSize, pFileChunkData->StartPos);
            if (ires != ERR_SUCCESS) {
                auto expected = std::error_code();
                FolderRecoverWorkData.ErrorCode.compare_exchange_strong(expected, std::make_error_code(std::errc::no_such_file_or_directory));
                return;
            }

            auto fileItr = FileTaskData.FilesNeedRecover.find(ConvertViewToU8View(pFileData->FileName));
            auto pFileNeedRecoverData = fileItr->second;
            auto chunkItr = pFileNeedRecoverData->NeedRecoverChunks.find(pFileChunkData->StartPos);
            FolderRecoverWorkData.ChunkCompleteQueue.enqueue(FolderRecoverWorkData_t::ChunkCompleteEvent_t{ pFileNeedRecoverData, *chunkItr });
        }
    }
    FileTaskData.SourceFile.Close();
    FileTaskData.TargetFile.Close();
    FolderRecoverWorkData.FileTaskQueue.enqueue(pFileTaskData);
}

void FFolderRecoverHelper::FinishRecoverTask(this FFolderRecoverHelper& self, std::shared_ptr<FolderRecoverWorkData_t> pFolderWorkData, std::shared_ptr<RecoverFileTaskData_t> pFileTaskData)
{
    auto& FolderRecoverWorkData = *pFolderWorkData;
    uint32_t readed;
    int32_t ires;

    for (auto& fileName : FolderRecoverWorkData.RecoverProcess.CompareResult->FilesNeedDelete) {
        std::filesystem::path delPath = FolderRecoverWorkData.WorkFolder / fileName;
        DirUtil::Delete(delPath.u8string());
    }
    for (auto& [fileName,chunks] : FolderRecoverWorkData.RecoverProcess.CompareResult->FileConstructChunks) {
        if (chunks.size() == 0) {
            std::filesystem::path createPath = FolderRecoverWorkData.WorkFolder / fileName;
            FRawFile file;
            file.Open(createPath.u8string(),UTIL_CREATE_ALWAYS,0);
        }
    }
    for (auto& [fileName,_] : FolderRecoverWorkData.RecoverProcess.CompareResult->FileConstructChunks) {
        std::filesystem::path tempFilePath = FolderRecoverWorkData.TempFolder / fileName;
        if (!DirUtil::IsExist(tempFilePath.u8string())) {
            continue;
        }
        std::filesystem::path destFilePath = FolderRecoverWorkData.WorkFolder / fileName;
        auto bres=DirUtil::Rename(tempFilePath.u8string(), destFilePath.u8string());
        if (!bres) {
            FolderRecoverWorkData.ErrorCode= std::make_error_code(std::errc::io_error);
            FolderRecoverWorkData.SetStatus(EFolderRecoverStatus::FRS_Finished);
            return;
        }
    }
    if (!FolderRecoverWorkData.RecoverProcess.GetFolderRecoverProgressHeader().bTempFolderExist) {
        DirUtil::Delete(FolderRecoverWorkData.TempFolder.u8string());
    }
    std::error_code ec;
    auto bres=pFolderWorkData->RecoverProcess.FileBackedBuffer->Clean(ec);
    if (!bres) {
        FolderRecoverWorkData.ErrorCode = ec;
    }
    FolderRecoverWorkData.SetStatus(EFolderRecoverStatus::FRS_Finished);
}



void FFolderRecoverHelper::Tick(float delta)
{
    std::set<CommonHandle32_t> needDel;
    for (auto& [handle, pFolderRecoverWorkData] : FolderRecoverWorkDataList) {
        auto& FolderRecoverWorkData = *pFolderRecoverWorkData;
        if (FolderRecoverWorkData.LastStatus != FolderRecoverWorkData.Status) {
            auto ec = FolderRecoverWorkData.ErrorCode.load();
            FolderRecoverWorkData.StatusDelegate(FolderRecoverWorkData.Status, ec);
            FolderRecoverWorkData.LastStatus= FolderRecoverWorkData.Status;
        }

        switch (FolderRecoverWorkData.Status)
        {
        case EFolderRecoverStatus::FRS_RecoverFile: {
            if (FolderRecoverWorkData.ErrorCode.load()) {
                FolderRecoverWorkData.SetStatus( EFolderRecoverStatus::FRS_Finished);
            }
            auto outSize = FolderRecoverWorkData.ChunkCompleteQueue.try_dequeue_bulk(FolderRecoverWorkData.ChunkEventCache, sizeof(FolderRecoverWorkData.ChunkEventCache) / sizeof(FolderRecoverWorkData_t::ChunkCompleteEvent_t));
            if (outSize == 0) {
                break;
            }

            for (int i = 0; i < outSize; i++) {
                auto& [pFileRecoverData,pChunkRecoverData] = FolderRecoverWorkData.ChunkEventCache[i];
                if (pChunkRecoverData) {
                    auto& FileProgressHeader = FolderRecoverWorkData.RecoverProcess.GetFileProgressHeader(pFileRecoverData->Index);
                    FolderRecoverWorkData.RecoverProcess.SetFileChunkStatus(FileProgressHeader, pChunkRecoverData->Index);
                    auto fileitr = FolderRecoverWorkData.RecoverProcess.FilesNeedRecover.find(ConvertViewToU8View(pFileRecoverData->FileData->FileName));
                    fileitr->second->NeedRecoverChunks.erase(pChunkRecoverData->ConstructChunkData->ChunkData->StartPos);
                    if (fileitr->second->NeedRecoverChunks.size() == 0) {
                        FolderRecoverWorkData.RecoverProcess.FilesNeedRecover.erase(fileitr);
                        if (FolderRecoverWorkData.RecoverProcess.FilesNeedRecover.size() == 0) {
                            FolderRecoverWorkData.SetStatus(EFolderRecoverStatus::FRS_FinishWork);
                        }
                        FolderRecoverWorkData.RecoverProcess.AddCompleteFileCount();
                    }
                    FolderRecoverWorkData.RecoverProcess.AddCompleteFileChunkCount();

                    if (pChunkRecoverData->ConstructChunkData->bFromSource) {
                        FolderRecoverWorkData.RecoverProcess.NeedRecoverSourceFileChunks.erase(GetHexNameView(pChunkRecoverData->ConstructChunkData->ChunkData->HexName));
                    }
                    else {
                        FolderRecoverWorkData.RecoverProcess.NeedRecoverMissingFileChunks.erase(GetHexNameView(pChunkRecoverData->ConstructChunkData->ChunkData->HexName));
                    }
                }
                else {
                    assert(false);
                    auto ires = FolderRecoverWorkData.RecoverProcess.FilesNeedRecover.erase(ConvertViewToU8View(pFileRecoverData->FileData->FileName));
                    if (ires > 0) {
                        FolderRecoverWorkData.RecoverProcess.AddCompleteFileCount();
                    }
                }
            }

            if (FolderRecoverWorkData.Status== EFolderRecoverStatus::FRS_FinishWork) {
                std::shared_ptr< RecoverFileTaskData_t> pFileTaskData = pFolderRecoverWorkData->GetFileTask();
                pFileTaskData->FilesNeedRecover = pFolderRecoverWorkData->RecoverProcess.FilesNeedRecover;
                auto [_, res] = pFolderRecoverWorkData->FileTasks.emplace(pFileTaskData);
                FnishWorkQueue.enqueue(
                    [this, pFolderRecoverWorkData, pFileTaskData]() {
                        FinishRecoverTask(pFolderRecoverWorkData, pFileTaskData);
                    }
                );
            }
            else {
                SaveReqQueue.enqueue(pFolderRecoverWorkData);
            }
            std::shared_ptr<RecoverFileTaskData_t> usedFileTask;
            while (FolderRecoverWorkData.FileTaskQueue.try_dequeue(usedFileTask)) {
                FolderRecoverWorkData.FileTasks.erase(usedFileTask);
                FolderRecoverWorkData.FileTaskPool.push_back(usedFileTask);
            }
            break;
        }
        case EFolderRecoverStatus::FRS_Finished: {
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

void FFolderRecoverHelper::IOTick(float delta)
{
    while (true)
    {
        std::array<std::shared_ptr<FolderRecoverWorkData_t>, 10> temp;
        auto outSize = SaveReqQueue.try_dequeue_bulk(temp.data(), temp.size());
        for (int i = 0; i < outSize;i++) {
            SaveReqCache.emplace(temp[i]);
        }
        if (outSize < temp.size()) {
            break;
        }
    }
    auto size = SaveReqCache.size();
    if (size != 0) {
        for (auto& req : SaveReqCache) {
            if (req->Status == EFolderRecoverStatus::FRS_RecoverFile) {
                req->RecoverProcess.FileBackedBuffer->IOTick(delta);
            }
        }
        SaveReqCache.clear();
    }
    std::function<void()> task;
    while (FnishWorkQueue.try_dequeue(task)) {
        task();
    }
}

LIB_FILEBACKUP_EXPORT IFolderRecoverHelperInterface* GetFolderRecoverHelperInstance()
{
    return TProvideSingletonClass<FFolderRecoverHelper>::GetSingleton().get();
}

