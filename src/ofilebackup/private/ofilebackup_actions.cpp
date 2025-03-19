#include "ofilebackup_actions.h"
#include <FileBackupManager.h>
#include <FolderRecoverHelper.h>
#include <Task/TaskManager.h>
#include <Task/TaskCounter.h>
#include <FunctionExitHelper.h>
#include <dir_util.h>

#include <filesystem>
#include <numeric>
#include <queue>
#include <fstream>
#include <iostream>
#include <cstring>

std::tuple< bool, std::shared_ptr<const FolderManifest_t>> gen_folder_manifest_by_chunklist(std::u8string_view workPathStr, std::vector<std::string>& hexNameList, std::u8string_view chunkOutPathStr, TChunkCompleteDelegate Delegate) {
    bool fExit{ false };
    std::error_code ec;
    std::shared_ptr<const FolderManifest_t> out;
    IFileBackupManagerInterface* FileBackupManager = GetFileBackupManagerInstance();
    auto workHandle = FileBackupManager->GenFolderChunkData(workPathStr.data(),
        [&](std::shared_ptr<const FolderManifest_t> MetaData) {
            fExit = true;
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

    uint8_t ParallelTaskNum = std::max(1,int(std::thread::hardware_concurrency())-1);
    FTaskSlotCounter<void> TaskCounter(ParallelTaskNum);
    typedef struct TaskData_t {
        WorkflowHandle_t WorkflowHandle{ NullHandle };
        IFileBackupManagerInterface::TOneFileChunkDataPostProcessingTask PostTask;
        CommonTaskHandle_t ReadTickHandle;
    }TaskData_t;
    std::vector<TaskData_t> taskDataList(ParallelTaskNum);
    for (auto& taskData : taskDataList)
    {
        taskData.WorkflowHandle = GetTaskManagerInstance()->NewWorkflow();
    }

    auto tickHandle = GetTaskManagerInstance()->AddTick(GetTaskManagerInstance()->GetMainThread(),
        [&](float delta) {
            FileBackupManager->Tick(delta);
            auto& finishedSlots = TaskCounter.CheckFinished();
            for (auto& slot : finishedSlots) {
                taskDataList[slot.ID].PostTask();
                GetTaskManagerInstance()->RemoveTask(taskDataList[slot.ID].ReadTickHandle);
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
                    auto [newhandle, newf] = GetTaskManagerInstance()->AddTask(taskDataList[i].WorkflowHandle, task);
                    taskDataList[i].PostTask = postTask;
                    TaskCounter.SetFuture(i, newhandle, newf);
                    taskDataList[i].ReadTickHandle = GetTaskManagerInstance()->AddTick(GetTaskManagerInstance()->GetMainThread(), readFileTick);
                }
                if (fExit) {
                    GetTaskManagerInstance()->Stop();
                }
            }
        }
    );
    FunctionExitHelper_t ExitHelper([&]() {
        for (auto& taskData : taskDataList)
        {
            GetTaskManagerInstance()->ReleaseWorkflow(taskData.WorkflowHandle);
        }
        GetTaskManagerInstance()->RemoveTask(tickHandle);
        });
    GetTaskManagerInstance()->Run();
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
    DirUtil::IterateDir(chunkOutPathStr,
        [&](DirEntry_t& entry) {
            if (entry.bDir) {
                return;
            }
            hexNameList.push_back((const char*)entry.Name);
        },
        0);
    auto [res, pFolderManifest] = gen_folder_manifest_by_chunklist(workPathStr, hexNameList, chunkOutPathStr,
        [](CompleteChunkData_t CompleteChunkData, GenProcessData_t GenProcessData) {
            std::cout << "\r" << GenProcessData.CompleteSize << "/" << GenProcessData.TotalSize << std::flush;
        }
    );
    if (!res) {
        return false;
    }
    if (manifestFilePathStr.empty()) {
        std::cout << "\r" << *pFolderManifest->to_string() << std::endl;
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
        ofs << *pFolderManifest->to_string();
        ofs.close();
    }
    return true;
}


bool compare_folder_manifest(std::u8string_view sourcePathStr, std::u8string_view targetPathStr, std::u8string_view outFilePathStr) {

    std::filesystem::path sourcePath(sourcePathStr);
    std::filesystem::path targetPath(targetPathStr);
    std::filesystem::path outFilePath(outFilePathStr);
    std::error_code ec;
    if (!std::filesystem::exists(sourcePath) || !std::filesystem::exists(targetPath)) {
        return false;
    }
    std::fstream sourceFile(sourcePath, std::ios::binary);
    if (sourceFile.is_open()) {
        return false;
    }
    auto size = std::filesystem::file_size(sourcePath, ec);
    if (ec) {
        return false;
    }
    std::vector<char> sourceContent(size);
    if (!sourceFile.read(sourceContent.data(), size)) {
        return false;
    }
    auto pSourceFolderManifest = FolderManifest_t::from_string(sourceContent.data(), size);

    std::fstream targetFile(targetPath, std::ios::binary);
    if (targetFile.is_open()) {
        return false;
    }
    size = std::filesystem::file_size(targetPath, ec);
    if (ec) {
        return false;
    }
    std::vector<char> targetContent(size);
    if (!targetFile.read(targetContent.data(), size)) {
        return false;
    }
    auto pTargetFolderManifest = FolderManifest_t::from_string(targetContent.data(), size);

    auto diffRes = CompareFolderManifest(*pSourceFolderManifest, *pTargetFolderManifest);
    if (outFilePathStr.empty()) {
        for (auto& sourcePathStr : diffRes->MissingFileChunks) {
            std::cout << sourcePathStr << std::endl;
        }
    }
    else {
        std::filesystem::create_directories(outFilePath.parent_path());
        std::fstream outstream(outFilePath);
        for (auto& sourcePathStr : diffRes->MissingFileChunks) {
            outstream << sourcePathStr << std::endl;
        }
    }
    return true;
}

EFileBackupError recover_folder(std::u8string_view workPathStr, std::u8string_view manifestFilePathStr, std::u8string_view sourceManifestFilePathStr, std::u8string_view chunkPathStr, std::u8string_view tempPathStr)
{
    auto& FolderRecoverHelper = *GetFolderRecoverHelperInstance();
    bool hasTempFolder{ false };
    std::filesystem::path workPath(workPathStr);
    std::filesystem::path manifestFilePath(manifestFilePathStr);
    std::filesystem::path sourceManifestFilePath(sourceManifestFilePathStr);
    std::filesystem::path chunkPath(chunkPathStr);
    std::filesystem::path tempPath(tempPathStr);
    std::u8string tempPathStr8;
    std::vector<char> manifestContent;
    std::vector<char> sourceManifestContent;
    std::error_code ec;
    std::shared_ptr<const FolderManifest_t> pManifest;
    std::shared_ptr<const FolderManifest_t> pSourceManifest;
    if (!std::filesystem::exists(workPath, ec) || ec) {
        if (!std::filesystem::create_directories(workPath, ec) || ec) {
            return EFileBackupError::FBE_FILE_NOT_EXIST;
        }
    }
    else if (!std::filesystem::is_directory(workPath, ec) || ec) {
        return EFileBackupError::FBE_FILE_NOT_EXIST;
    }
    if (!std::filesystem::exists(manifestFilePath, ec) || ec
        || std::filesystem::is_directory(manifestFilePath, ec) || ec
        ) {
        return EFileBackupError::FBE_FILE_NOT_EXIST;
    }
    if (!sourceManifestFilePathStr.empty()
        && (!std::filesystem::exists(sourceManifestFilePath, ec) || ec
        || std::filesystem::is_directory(sourceManifestFilePath, ec) || ec)) {
        return EFileBackupError::FBE_FILE_NOT_EXIST;
    }
    std::ifstream manifestFile(manifestFilePath, std::ios::binary);
    if (!manifestFile.is_open()) {
        return EFileBackupError::FBE_FILE_OP_ERROR;
    }
    auto size = std::filesystem::file_size(manifestFilePath, ec);
    if (ec) {
        return EFileBackupError::FBE_FILE_OP_ERROR;
    }
    manifestContent.reserve(size);
    if (!manifestFile.read(manifestContent.data(), size)) {
        return EFileBackupError::FBE_FILE_OP_ERROR;
    }
    pManifest = FolderManifest_t::from_string(manifestContent.data(), size);

    if (sourceManifestFilePathStr.empty()) {
        pSourceManifest = std::make_shared<FolderManifest_t>();
    }
    else {
        std::ifstream sourceManifestFile(sourceManifestFilePath, std::ios::binary);
        if (!sourceManifestFile.is_open()) {
            return EFileBackupError::FBE_FILE_OP_ERROR;
        }
        size = std::filesystem::file_size(sourceManifestFilePath, ec);
        if (ec) {
            return EFileBackupError::FBE_FILE_OP_ERROR;
        }
        sourceManifestContent.reserve(size);
        if (!sourceManifestFile.read(sourceManifestContent.data(), size)) {
            return EFileBackupError::FBE_FILE_OP_ERROR;
        }
        pSourceManifest = FolderManifest_t::from_string(sourceManifestContent.data(), size);
    }

    if (tempPathStr.empty()) {
        tempPath = workPath.parent_path() / (workPath.filename().string() + "_temp");
        tempPathStr8 = tempPath.u8string();
        tempPathStr = tempPathStr8;
    }
    if (!std::filesystem::exists(tempPath, ec) || ec) {
        std::filesystem::create_directories(tempPath, ec);
        if (ec) {
            return EFileBackupError::FBE_FILE_OP_ERROR;
        }
    }
    else {
        auto bDir = std::filesystem::is_directory(tempPath, ec);
        if (ec) {
            return EFileBackupError::FBE_FILE_OP_ERROR;
        }
        if (!bDir) {
            return EFileBackupError::FBE_FILE_ALREADY_EXIST;
        }
        hasTempFolder = true;
    }

    std::atomic<EFolderRecoverStatus> RecoverStatus;
    bool res{ true };
    auto recoverHandle = FolderRecoverHelper.AddTask(pManifest, pSourceManifest, [&](EFolderRecoverStatus status) {
        RecoverStatus = status;
        });
    if (!recoverHandle.IsValid()) {
        return EFileBackupError::FBE_INTERNAL_ERROR;
    }
    uint8_t ParallelTaskNum = std::max(1, int(std::thread::hardware_concurrency()) - 1);
    //uint8_t ParallelTaskNum = 1;
    FTaskSlotCounter<void> TaskCounter(ParallelTaskNum);
    typedef struct TaskData_t {
        WorkflowHandle_t WorkflowHandle{ NullHandle };
        ReserveFileSpaceData_t ReserveFileSpaceTask;
    }TaskData_t;
    std::vector<TaskData_t> taskDataList(ParallelTaskNum);
    for (auto& taskData : taskDataList)
    {
        taskData.WorkflowHandle = GetTaskManagerInstance()->NewWorkflow();
    }
    CommonTaskHandle_t tickHandle;
    tickHandle = GetTaskManagerInstance()->AddTick(GetTaskManagerInstance()->GetMainThread(),
        [&](float delta) {
            FolderRecoverHelper.Tick(delta);
            TaskCounter.CheckFinished();
            switch (RecoverStatus) {
            case EFolderRecoverStatus::FRS_ReserveSpace: {
                auto IDopt = TaskCounter.GetFreeSlot();
                if (!IDopt.has_value()) {
                    break;
                }
                auto i = *IDopt;
                auto ReserveFileSpaceDataOpt = FolderRecoverHelper.GetReserveNextFileSpaceData(recoverHandle);
                if (!ReserveFileSpaceDataOpt.has_value()) {
                    break;
                }
                std::cout<<  "\r" << FolderRecoverHelper.GetFolderRecoverProcess(recoverHandle).FileCount << "/" << FolderRecoverHelper.GetFolderRecoverProcess(recoverHandle).AllFileNum << std::flush;
                auto [newhandle, newf] = GetTaskManagerInstance()->AddTask(taskDataList[i].WorkflowHandle, [&, i]() {
                    if (!FolderRecoverHelper.ImplementReserveFileSpace(recoverHandle,taskDataList[i].ReserveFileSpaceTask, tempPathStr)) {
                        RecoverStatus = EFolderRecoverStatus::FRS_Finished;
                        res = false;
                    }
                    FolderRecoverHelper.ReserveFileSpaceComplete(recoverHandle, taskDataList[i].ReserveFileSpaceTask);
                    });
                TaskCounter.SetFuture(i, newhandle, newf);
                taskDataList[i].ReserveFileSpaceTask = ReserveFileSpaceDataOpt.value();
                break;
            }
            case EFolderRecoverStatus::FRS_ReconstructFile: {
                auto IDopt = TaskCounter.GetFreeSlot();
                if (!IDopt.has_value()) {
                    break;
                }
                auto i = *IDopt;
                auto pChunkData = FolderRecoverHelper.GetConstructNextChunkData(recoverHandle);
                if (!pChunkData) {
                    break;
                }
                std::cout << "\r" << FolderRecoverHelper.GetFolderRecoverProcess(recoverHandle).FileChunkCount << "/" << FolderRecoverHelper.GetFolderRecoverProcess(recoverHandle).AllFileChunkNum << std::flush;
                auto [newhandle, newf] = GetTaskManagerInstance()->AddTask(taskDataList[i].WorkflowHandle, [&, pChunkData]() {
                    if (!FolderRecoverHelper.ImplementForLocalChunkConstruct(recoverHandle,pChunkData, workPathStr, chunkPathStr)) {
                        RecoverStatus = EFolderRecoverStatus::FRS_Finished;
                        res = false;
                    }
                    FolderRecoverHelper.ChunkConstructComplete(recoverHandle, pChunkData);
                    });
                TaskCounter.SetFuture(i, newhandle, newf);
                break;
            }
            case EFolderRecoverStatus::FRS_MoveFile: {
                auto IDopt = TaskCounter.GetFreeSlot();
                if (!IDopt.has_value()) {
                    break;
                }
                auto i = *IDopt;
                auto opt = FolderRecoverHelper.GetNextFileNeedMove(recoverHandle);
                if (!opt.has_value()) {
                    break;
                }
                auto [newhandle, newf] = GetTaskManagerInstance()->AddTask(taskDataList[i].WorkflowHandle, [&, filename = opt.value()]() {
                    if (!FolderRecoverHelper.ImplementFileMove(recoverHandle,filename, workPathStr)) {
                        RecoverStatus = EFolderRecoverStatus::FRS_Finished;
                        res = false;
                    }
                    FolderRecoverHelper.FileMoveComplete(recoverHandle, filename);
                    });
                TaskCounter.SetFuture(i, newhandle, newf);
                break;
            }
            case EFolderRecoverStatus::FRS_Finished: {
                GetTaskManagerInstance()->RemoveTask(tickHandle);
                GetTaskManagerInstance()->Stop();
                break;
            }
            }

        }
    );
    FunctionExitHelper_t ExitHelper(
        [&]() {
            for (auto& taskData : taskDataList)
            {
                GetTaskManagerInstance()->ReleaseWorkflow(taskData.WorkflowHandle);
            }
            GetTaskManagerInstance()->RemoveTask(tickHandle);
            if (!hasTempFolder) {
                std::filesystem::remove_all(tempPath, ec);
                if (ec) {
                    return;
                }
            }
        }
    );

    GetTaskManagerInstance()->Run();

    return EFileBackupError::FBE_OK;

}
