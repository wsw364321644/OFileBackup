#include "FileBackupManager.h"
#include "FileBackupManagerMinChunk.h"
#include "FileBackupManagerGatherAll.h"
#include <singleton.h>


LIB_FILEBACKUP_EXPORT IFileBackupManagerInterface* GetFileBackupManagerSingleton()
{
    return TClassSingletonHelper<FFileBackupManagerGatherAll>::GetClassSingleton().get();
}

