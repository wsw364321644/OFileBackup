#include "FileBackupManager.h"
#include "FileBackupInternal.h"

class FFileBackupManagerMinChunk :public IFileBackupManagerInterface {
public:
    FFileBackupManagerMinChunk() {}

    CommonHandle32_t GenFolderChunkData(const char8_t* path, TGenFolderMetaDataFinishDelegate Delegate) override;
    bool GenFolderChunkDataAddHash(CommonHandle32_t handle, TGetNextHashPairCB CB) override;
    std::tuple<TOneFileChunkDataTask, TOneFileChunkDataReadFileTick, TOneFileChunkDataPostProcessingTask> GenFolderChunkDataGetNextFileTask(CommonHandle32_t handle, TNewFileChunkDelegate) override;
    std::shared_ptr<const GenFolderMetaDataProcess_t> GenFolderChunkDataGetProcess(CommonHandle32_t handle) override;
    std::shared_ptr<const FolderManifest_t> GetFolderChunkData(CommonHandle32_t handle);
    void Tick(float delta) override;


    void GenFolderChunkDataTask(this FFileBackupManagerMinChunk& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);
    void GenFolderChunkDataReadFileTick(this FFileBackupManagerMinChunk& self, float delta, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);
    void GenFolderChunkDataPostProcessingTask(this FFileBackupManagerMinChunk& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);

    std::unordered_map<CommonHandle32_t, std::shared_ptr<GenFolderChunkDataWorkData_t>>GenFolderMetaDataWorkDataList;
};
