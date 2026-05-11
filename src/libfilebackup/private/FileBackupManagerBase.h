#pragma once
#include "FileBackupManager.h"
#include "FileBackupInternal.h"

class IFileBackupManagerBase :public IFileBackupManagerInterface {
public:
    IFileBackupManagerBase() {}

    CommonHandle32_t GenFolderChunkData(const char8_t* path, TGenFolderMetaDataStatusChangedDelegate Delegate) override;
    CommonHandle32_t GenFolderChunkData(GenFolderChunkParams_t& params, TGenFolderMetaDataStatusChangedDelegate Delegate) override;
    void CancelTask(CommonHandle32_t handle) override;
    void InitTask(CommonHandle32_t) override;
    bool GenFolderChunkDataAddHash(CommonHandle32_t handle, TGetNextHashPairCB CB) override;

    std::shared_ptr<const GenFolderMetaDataProcess_t> GenFolderChunkDataGetProgress(CommonHandle32_t handle) override;
    std::shared_ptr<const FolderManifest_t> GetFolderChunkData(CommonHandle32_t handle) override;
    std::optional<std::reference_wrapper<std::unordered_map<std::u8string_view, std::string>>> GetFolderChunkLocalFileMap(CommonHandle32_t handle) override;
    void Tick(float delta) override;


    void GenFolderChunkDataReadFileTick(this IFileBackupManagerBase& self, float delta, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);
    void GenFolderChunkDataPostProcessingTask(this IFileBackupManagerBase& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);

    std::unordered_map<CommonHandle32_t, std::shared_ptr<GenFolderChunkDataWorkData_t>>GenFolderMetaDataWorkDataList;
};
