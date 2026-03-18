#include "ofilebackup_actions.h"

#include <FileBackupManager.h>
#include <FolderRecoverHelper.h>
#include <Task/TaskManager.h>
#include <Task/TaskCounter.h>
#include <FunctionExitHelper.h>
#include <dir_util.h>
#include <char_buffer_extension.h>

#include <filesystem>
#include <numeric>
#include <queue>
#include <fstream>
#include <iostream>
#include <cstring>

std::tuple< bool, std::shared_ptr<const FolderManifest_t>> gen_folder_manifest_by_chunklist(std::u8string_view workPathStr, std::vector<std::string>& hexNameList, std::u8string_view chunkOutPathStr, TChunkCompleteDelegate Delegate) {
    bool bExit{ false };
    std::error_code ec;
    std::shared_ptr<const FolderManifest_t> out;
    IFileBackupManagerInterface* FileBackupManager = GetFileBackupManagerInstance();
    auto workHandle = FileBackupManager->GenFolderChunkData(workPathStr.data(),
        [&](std::shared_ptr<const FolderManifest_t> MetaData) {
            bExit = true;
            out = MetaData;
        }
    );
    auto hexNameItr = hexNameList.begin();
    bool bAddHashRes{ true };
    bAddHashRes = FileBackupManager->GenFolderChunkDataAddHash(workHandle,
        [&](char8_t* hexName, uint32_t& hexNameLen)->bool {
            while (hexNameItr != hexNameList.end()) {
                if (hexNameLen < (*hexNameItr).length()) {
                    hexNameItr++;
                    continue;
                }
            }
            if (hexNameItr == hexNameList.end()) {
                return false;
            }
            hexNameLen = (*hexNameItr).length();
            memcpy(hexName, (*hexNameItr).c_str(), hexNameLen);
            return true;
        }
    );
    if (!bAddHashRes) {
        return { false ,nullptr };
    }
    std::filesystem::path chunkOutPath(chunkOutPathStr);
    if (!chunkOutPathStr.empty() && !std::filesystem::exists(chunkOutPath, ec)) {
        std::filesystem::create_directories(chunkOutPath);
    }

    uint8_t ParallelTaskNum = std::max(1, int(std::thread::hardware_concurrency()) - 1);
    FTaskSlotCounter<void> TaskCounter(ParallelTaskNum);
    typedef struct TaskData_t {
        WorkflowHandle_t WorkflowHandle{ NullHandle };
        IFileBackupManagerInterface::TOneFileChunkDataPostProcessingTask PostTask;
        CommonTaskHandle_t ReadTickHandle;
    }TaskData_t;
    std::vector<TaskData_t> taskDataList(ParallelTaskNum);
    for (auto& taskData : taskDataList)
    {
        taskData.WorkflowHandle = GetTaskManagerSingleton()->NewWorkflow();
    }

    auto tickHandle = GetTaskManagerSingleton()->AddTick(GetTaskManagerSingleton()->GetMainThread(),
        [&](float delta) {
            FileBackupManager->Tick(delta);
            auto& finishedSlots = TaskCounter.CheckFinished();
            for (auto& slot : finishedSlots) {
                taskDataList[slot.ID].PostTask();
                GetTaskManagerSingleton()->RemoveTask(taskDataList[slot.ID].ReadTickHandle);
            }

            auto IDopt = TaskCounter.GetFreeSlot();
            if (IDopt.has_value()) {
                auto i = *IDopt;
                auto [task, readFileTick, postTask] = FileBackupManager->GenFolderChunkDataGetNextFileTask(workHandle,
                    [&](IChunkConverter* ChunkConverter, const char8_t* name, uint32_t namelen, const char* content, uint32_t contentlen) {
                        if (!chunkOutPathStr.empty()) {
                            auto outFilePath = chunkOutPath / std::u8string_view(name, namelen);
                            std::ofstream ofs(outFilePath, std::ios::binary);
                            if (ofs.is_open()) {
                                ChunkConverter->Convert((uint8_t*)content);
                                auto ChunkFileBuf = ChunkConverter->GetChunkFileBuf();
                                auto ChunkFileLen = ChunkConverter->GetChunkFileSize();
                                ofs.write((const char*)ChunkFileBuf, ChunkFileLen);
                                ofs.close();
                            }
                        }
                        auto process = FileBackupManager->GenFolderChunkDataGetProcess(workHandle);
                        if (Delegate) {
                            Delegate(CompleteChunkData_t{ name, namelen, content, contentlen }, GenProcessData_t{ process->TotalSize,process->CompleteSize });
                        }
                    }
                );
                if (task) {
                    auto [newhandle, newf] = GetTaskManagerSingleton()->AddTask(taskDataList[i].WorkflowHandle, task);
                    taskDataList[i].PostTask = postTask;
                    TaskCounter.SetFuture(i, newhandle, newf);
                    taskDataList[i].ReadTickHandle = GetTaskManagerSingleton()->AddTick(GetTaskManagerSingleton()->GetMainThread(), readFileTick);
                }
                if (bExit) {
                    GetTaskManagerSingleton()->Stop();
                }
            }
        }
    );
    FunctionExitHelper_t ExitHelper([&]() {
        for (auto& taskData : taskDataList)
        {
            GetTaskManagerSingleton()->ReleaseWorkflow(taskData.WorkflowHandle);
        }
        GetTaskManagerSingleton()->RemoveTask(tickHandle);
        });
    GetTaskManagerSingleton()->Run();
    return { true, out };
}
bool gen_folder_manifest_action(std::u8string_view workPathStr, std::u8string_view chunkListPathStr, std::u8string_view chunkOutPathStr, std::u8string_view manifestFilePathStr) {

    std::vector<std::string> hexNameList;
    std::error_code ec;
    if (!chunkListPathStr.empty()) {
        std::filesystem::path chunkListPath(chunkListPathStr);
        if (!std::filesystem::exists(chunkListPath, ec) || ec) {
            return false;
        }
        std::ifstream ifs(chunkListPath);
        std::string line;
        while (std::getline(ifs, line)) {
            hexNameList.push_back(line);
        }
    }
    if (!chunkOutPathStr.empty()) {
        DirUtil::IterateDir(chunkOutPathStr,
            [&](DirEntry_t& entry)->bool {
                if (entry.bDir) {
                    return true;
                }
                hexNameList.push_back((const char*)entry.Name);
                return true;
            },
            0);
    }
    auto [res, pFolderManifest] = gen_folder_manifest_by_chunklist(workPathStr, hexNameList, chunkOutPathStr,
        [](CompleteChunkData_t CompleteChunkData, GenProcessData_t GenProcessData) {
            GetTaskManagerSingleton()->AddTask(GetTaskManagerSingleton()->GetMainThread(),
                [GenProcessData]() {
                    std::cout << "\r" << GenProcessData.CompleteSize << "/" << GenProcessData.TotalSize << std::flush;
                }
            );
        }
    );
    if (!res) {
        return false;
    }
    auto& charBuf = *FCharBuffer::GetThreadSingleton();
    pFolderManifest->to_string(charBuf, ec);
    if (ec) {
        return false;
    }
    if (manifestFilePathStr.empty()) {
        std::cout << "\r" << charBuf.View() << std::endl;
    }
    else {
        std::filesystem::path manifestFilePath(manifestFilePathStr);
        std::filesystem::create_directories(manifestFilePath.parent_path(), ec);
        if (ec) {
            return false;
        }
        std::ofstream ofs(manifestFilePath, std::ios::binary);
        if (!ofs.is_open()) {
            return false;
        }
        ofs << charBuf.View();
        ofs.close();
    }
    return true;
}


bool compare_folder_manifest(std::u8string_view sourcePathStr, std::u8string_view targetPathStr, std::u8string_view outFilePathStr) {
    std::error_code ec;
    FRawFile sourceFile;
    FRawFile targetFile;
    FRawFile outFile;
    if (sourceFile.Open(sourcePathStr, UTIL_OPEN_EXISTING) != ERR_SUCCESS) {
        return false;
    }

    if (targetFile.Open(targetPathStr, UTIL_OPEN_EXISTING) != ERR_SUCCESS) {
        return false;
    }
    auto& charBuf=*FCharBuffer::GetThreadSingleton();
    if (!LoadFileToCharBuffer(sourceFile, charBuf, FolderManifest_t::get_string_extra_space())) {
        return false;
    }
    auto pSourceFolderManifest = FolderManifest_t::from_string(charBuf, ec);
    if (ec) {
        return false;
    }
    if (!LoadFileToCharBuffer(targetFile, charBuf, FolderManifest_t::get_string_extra_space())) {
        return false;
    }
    auto pTargetFolderManifest = FolderManifest_t::from_string(charBuf, ec);
    if (ec) {
        return false;
    }
    auto diffRes = CompareFolderManifest(*pSourceFolderManifest, *pTargetFolderManifest);
    if (outFilePathStr.empty()) {
        for (auto& sourcePathStr : diffRes->MissingFileChunks) {
            std::cout << sourcePathStr << std::endl;
        }
    }
    else {
        if (outFile.Open(outFilePathStr, UTIL_CREATE_ALWAYS) != ERR_SUCCESS) {
            return false;
        }
        for (auto& sourcePathStr : diffRes->MissingFileChunks) {
            outFile.Write(sourcePathStr.data(), sourcePathStr.size());
        }
    }
    return true;
}

EFileBackupError recover_folder(std::u8string_view workPathStr, std::u8string_view manifestFilePathStr, std::u8string_view sourceManifestFilePathStr, std::u8string_view chunkPathStr, std::u8string_view tempPathStr)
{
    auto& FolderRecoverHelper = *GetFolderRecoverHelperInstance();
    std::error_code ec;
    bool bExit{ false };
    FRawFile Manifest;
    FRawFile SourceManifest;
    int AllFileNum{ 0 };
    int FileMoveCount{ 0 };
    std::shared_ptr<const FolderManifest_t> pManifest;
    std::shared_ptr<const FolderManifest_t> pSourceManifest;
    std::u8string tempPathStr8;
    FCharBuffer&charBuf=*FCharBuffer::GetThreadSingleton();
    FPathBuf &pathBuf=*FPathBuf::GetThreadSingleton();
    pathBuf.SetPath(ConvertU8ViewToView(workPathStr));
    if (!DirUtil::IsExist(pathBuf)) {
        if (!DirUtil::CreateDir(pathBuf)) {
            return EFileBackupError::FBE_FILE_NOT_EXIST;
        }
    }
    else {
        if (!DirUtil::IsDirectory(pathBuf)) {
            return EFileBackupError::FBE_FILE_NOT_EXIST;
        }
    }

    if (tempPathStr.empty()) {
        std::filesystem::path tempPath{ workPathStr };
        tempPath = tempPath.parent_path() / (tempPath.filename().string() + "_temp");
        tempPathStr8 = tempPath.u8string();
        tempPathStr = tempPathStr8;
    }

    pathBuf.SetPath(ConvertU8ViewToView(manifestFilePathStr));
    if (!DirUtil::IsExist(pathBuf) || DirUtil::IsDirectory(pathBuf)) {
        return EFileBackupError::FBE_FILE_NOT_EXIST;
    }
    if (Manifest.Open(pathBuf, UTIL_OPEN_EXISTING) != ERR_SUCCESS) {
        return EFileBackupError::FBE_FILE_OP_ERROR;
    }
    if (!LoadFileToCharBuffer(Manifest, charBuf)) {
        return EFileBackupError::FBE_FILE_OP_ERROR;
    }
    pManifest = FolderManifest_t::from_string(charBuf,ec);
    if (ec) {
        return EFileBackupError::FBE_PARAMS_ERROR;
    }

    if (!sourceManifestFilePathStr.empty()) {
        pathBuf.SetPath(ConvertU8ViewToView(sourceManifestFilePathStr));
        if (!DirUtil::IsExist(pathBuf) || DirUtil::IsDirectory(pathBuf)) {
            return EFileBackupError::FBE_FILE_NOT_EXIST;
        }
        if (SourceManifest.Open(pathBuf, UTIL_OPEN_EXISTING) != ERR_SUCCESS) {
            return EFileBackupError::FBE_FILE_OP_ERROR;
        }
        if (!LoadFileToCharBuffer(SourceManifest, charBuf)) {
            return EFileBackupError::FBE_FILE_OP_ERROR;
        }
        pSourceManifest = FolderManifest_t::from_string(charBuf, ec);
        if (ec) {
            return EFileBackupError::FBE_PARAMS_ERROR;
        }
    }

    pathBuf.SetPath(ConvertU8ViewToView(tempPathStr));
    if (DirUtil::IsExist(pathBuf)) {
        if (!DirUtil::IsDirectory(pathBuf)) {
            return EFileBackupError::FBE_FILE_ALREADY_EXIST;
        }
    }

    bool res{ true };
    auto recoverHandle = FolderRecoverHelper.AddTask(pManifest, pSourceManifest, workPathStr, chunkPathStr, tempPathStr,[&](std::error_code& ec) {
        bExit = true;
        });
    if (!recoverHandle.IsValid()) {
        return EFileBackupError::FBE_INTERNAL_ERROR;
    }
    uint8_t ParallelTaskNum = std::max(1, int(std::thread::hardware_concurrency()));
    //uint8_t ParallelTaskNum = 1;
    FTaskSlotCounter<void> TaskCounter(ParallelTaskNum);
    typedef struct TaskData_t {
        WorkflowHandle_t WorkflowHandle{ NullHandle };
        IFolderRecoverHelperInterface::TOneFileRecoverPostProcessingTask PostTask;
    }TaskData_t;
    std::vector<TaskData_t> taskDataList(ParallelTaskNum);
    for (auto& taskData : taskDataList)
    {
        taskData.WorkflowHandle = GetTaskManagerSingleton()->NewWorkflow();
    }
    CommonTaskHandle_t tickHandle;
    uint32_t lastfileChunkCount{ 0 };
    tickHandle = GetTaskManagerSingleton()->AddTick(GetTaskManagerSingleton()->GetMainThread(),
        [&](float delta) {
            if (bExit) {
                GetTaskManagerSingleton()->Stop();
            }
            FolderRecoverHelper.Tick(delta);
            auto& finishedSlots = TaskCounter.CheckFinished();
            for (auto& slot : finishedSlots) {
                taskDataList[slot.ID].PostTask();
            }
            auto IDopt = TaskCounter.GetFreeSlot();
            if (IDopt.has_value()) {
                auto i = *IDopt;
                auto [task, postTask] = FolderRecoverHelper.GetNextRecoverFileTask(recoverHandle);
                if (task) {
                    auto [newhandle, newf] = GetTaskManagerSingleton()->AddTask(taskDataList[i].WorkflowHandle, task);
                    taskDataList[i].PostTask = postTask;
                    TaskCounter.SetFuture(i, newhandle, newf);
                }
            }
            auto [_,fileChunkCount, opt]=FolderRecoverHelper.GetFolderRecoverProcess(recoverHandle);
            if (opt.has_value()) {
                auto& progress= opt.value().get();
                if (lastfileChunkCount != fileChunkCount) {
                    lastfileChunkCount = fileChunkCount;
                    std::cout << "\r" << fileChunkCount << "/" << progress.GetFolderRecoverProgressHeader().AllFileChunkNum << std::flush;
                }
            }

        }
    );
    FunctionExitHelper_t ExitHelper(
        [&]() {
            for (auto& taskData : taskDataList)
            {
                GetTaskManagerSingleton()->ReleaseWorkflow(taskData.WorkflowHandle);
            }
        }
    );

    GetTaskManagerSingleton()->Run();

    if (res) {
        return EFileBackupError::FBE_OK;
    }
    return EFileBackupError::FBE_INTERNAL_ERROR;
}
