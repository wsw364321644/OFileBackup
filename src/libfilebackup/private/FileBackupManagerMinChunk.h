#include "FileBackupManager.h"
#include "FileBackupManagerBase.h"
#include "FileBackupInternal.h"

class FFileBackupManagerMinChunk :public IFileBackupManagerBase {
public:
    FFileBackupManagerMinChunk() {}

    std::tuple<TOneFileChunkDataTask, TOneFileChunkDataReadFileTick, TOneFileChunkDataPostProcessingTask> GenFolderChunkDataGetNextFileTask(CommonHandle32_t handle, TNewFileChunkDelegate) override;

    void GenFolderChunkDataTask(this FFileBackupManagerMinChunk& self, std::shared_ptr<GenFolderChunkDataWorkData_t> pFolderWorkData, std::shared_ptr< GenFolderChunkDataFileTaskData_t> pFileTaskData);

};
