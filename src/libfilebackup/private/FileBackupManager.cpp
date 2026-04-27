#include "FileBackupManager.h"
#include "FileBackupManagerMinChunk.h"
#include "FileBackupManagerGatherAll.h"


LIB_FILEBACKUP_EXPORT IFileBackupManagerInterface* GetFileBackupManagerInstance()
{
    static std::atomic<std::shared_ptr<FFileBackupManagerGatherAll>> AtomicManager;
    auto oldptr = AtomicManager.load();
    if (!oldptr) {
        std::shared_ptr<FFileBackupManagerGatherAll> pManager(new FFileBackupManagerGatherAll);
        AtomicManager.compare_exchange_strong(oldptr, pManager);
    }
    return AtomicManager.load().get();
}

