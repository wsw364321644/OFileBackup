#include "ofilebackup_actions.h"

#include <FileBackupManager.h>
#include <Task/TaskManager.h>
#include <filesystem>
#include <fstream>
#include <iostream>
void gen_folder_manifest_action(std::string workPathStr, std::string outPathStr ) {
    bool fExit{ false };
    IFileBackupManagerInterface* FileBackupManager = GetFileBackupManagerInstance();
    auto workHandle = FileBackupManager->GenFolderChunkData(workPathStr.c_str(),
        [&](std::shared_ptr<const FolderManifest_t> MetaData) {
            fExit = true;
            std::cout << "\r" << *MetaData->to_string() << std::flush;
        }
    );
    
    std::filesystem::path outPath(outPathStr);

    if (!outPathStr.empty()&&!std::filesystem::exists(outPath)) {
        std::filesystem::create_directories(outPath);
    }
    GetTaskManagerInstance()->AddTick(GetTaskManagerInstance()->GetMainThread(),
        [&](float delta) {
            FileBackupManager->Tick(delta);
        }
    );
    std::future<void> f;
    CommonHandle_t handle{ NullHandle };
    while (!fExit) {
        if (!handle.IsValid()) {
            auto task = FileBackupManager->GenFolderChunkDataGetNextFileTask(workHandle,
                [&](const char* name, uint32_t namelen, const char* content, uint32_t contentlen) {
                    if (!outPathStr.empty()) {
                        auto outFilePath = outPath / std::string_view(name, namelen);
                        std::ofstream ofs(outFilePath, std::ios::binary);
                        ofs.write(content, contentlen);
                    }
                    auto process = FileBackupManager->GenFolderChunkDataGetProcess(workHandle);
                    std::cout <<"\r"<< process->CompleteSize << "/" << process->TotalSize << std::flush;
                }
            );
            if (task) {
                auto [newhandle, newf] = GetTaskManagerInstance()->AddTask(NullHandle, task);
                handle = newhandle;
                f = std::move(newf);
            }
        }
        else {
            if (f.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                handle = NullHandle;
            }
        }
        GetTaskManagerInstance()->Tick();
    }
}