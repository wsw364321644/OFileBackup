#include "FileBackupManager.h"
#include "FileBackupManagerBase.h"
#include "FileBackupInternal.h"

class FFileBackupManagerGatherAll :public IFileBackupManagerBase {
public:
    FFileBackupManagerGatherAll() {}

    std::tuple<TOneFileChunkDataTask, TOneFileChunkDataReadFileTick, TOneFileChunkDataPostProcessingTask> GenFolderChunkDataGetNextFileTask(CommonHandle32_t handle, TNewFileChunkDelegate) override;

    void GenFolderChunkDataTask(this FFileBackupManagerGatherAll& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);
};
