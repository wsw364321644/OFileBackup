#include "FolderRecoverHelper.h"
#include "FileBackupInternal.h"
#include <RawFile.h>
#include <moodycamel/concurrentqueue.h>
struct FolderRecoverWorkData_t;

typedef struct FileChunkRecoverData_t {
    std::shared_ptr<FileConstructChunkData_t> ConstructChunkData;
    uint32_t Index;
    bool operator < (const FileChunkRecoverData_t& other) const {
        return ConstructChunkData->ChunkData->StartPos < other.ConstructChunkData->ChunkData->StartPos;
    }
}FileChunkRecoverData_t;

typedef struct FileChunkRecoverDataLess_t {
    using is_transparent = void;
    bool operator ()(const FileChunkRecoverData_t& L, const FileChunkRecoverData_t& R) const {
        return L.ConstructChunkData->ChunkData->StartPos < R.ConstructChunkData->ChunkData->StartPos;
    }
    bool operator ()(const std::shared_ptr<FileChunkRecoverData_t>& L, const std::shared_ptr<FileChunkRecoverData_t>& R) const {
        return operator ()(*L, *R);
    }

    bool operator ()(uint64_t pos, const std::shared_ptr<FileChunkRecoverData_t>& ptr) const {
        return pos < ptr->ConstructChunkData->ChunkData->StartPos;
    }
    bool operator ()(const std::shared_ptr<FileChunkRecoverData_t>& ptr, uint64_t pos) const {
        return ptr->ConstructChunkData->ChunkData->StartPos < pos;
    }

}FileChunkRecoverDataLess_t;

typedef std::set<std::shared_ptr<FileChunkRecoverData_t>, FileChunkRecoverDataLess_t, allocator_save_memory_operator<std::shared_ptr<FileChunkRecoverData_t>>> TFileChunksRecoverData;

typedef struct FileNeedRecoverData_t {
    std::shared_ptr<FileChunksData_t> FileData;
    uint32_t Index;
    TFileChunksRecoverData NeedRecoverChunks;
}FileNeedRecoverData_t;

typedef std::unordered_map<std::u8string_view, std::shared_ptr<FileNeedRecoverData_t>> TFilesNeedRecover;

class FolderRecoverProgressImpl :public FolderRecoverProgress {
public:
    void Init(std::shared_ptr<FolderRecoverWorkData_t> workData,std::shared_ptr < const  FolderManifest_t> targetManifest, std::shared_ptr<const FolderManifest_t> source,std::error_code& ec);

    std::shared_ptr <const FolderManifest_t> Manifest;
    std::shared_ptr <const FolderManifest_t> SourceManifest;


    TFilesNeedRecover FilesNeedRecover;
};


typedef struct RecoverFileTaskData_t {
    TFilesNeedRecover FilesNeedRecover;
    FRawFile TargetFile;
    FRawFile SourceFile;
    uint8_t* FileChunkBuf{ nullptr };
    IChunkConverter* ChunkConverter{ nullptr };
    void Clear() {
        TargetFile.Close();
        SourceFile.Close();
    }
}RecoverFileTaskData_t;

typedef struct FolderRecoverWorkData_t {
    ~FolderRecoverWorkData_t() {
        for (auto& pTask : FileTaskPool) {
            delete[] pTask->FileChunkBuf;
            delete pTask->ChunkConverter;
        }
    }
    std::shared_ptr <RecoverFileTaskData_t> GetFileTask() {
        std::shared_ptr <RecoverFileTaskData_t> pFileTaskData;
        if (FileTaskPool.size() > 0) {
            pFileTaskData = FileTaskPool.back();
            FileTaskPool.pop_back();
            pFileTaskData->Clear();
        }
        else {
            pFileTaskData = std::make_shared<RecoverFileTaskData_t>();
            pFileTaskData->FileChunkBuf = new uint8_t[FileChunkSize];
            pFileTaskData->ChunkConverter = new FChunkConverter(EConvertDirection::ToFileChunk);
        }
        return pFileTaskData;
    }
    void SetStatus(EFolderRecoverStatus status) {
        LastStatus = Status;
        Status = status;
    }

    FolderRecoverProgressImpl RecoverProcess;
    IFolderRecoverHelperInterface::TRecoverFoldeStatusChangedDelegate StatusDelegate;
    std::atomic<EFolderRecoverStatus> Status;;
    EFolderRecoverStatus LastStatus;;
    std::filesystem::path WorkFolder;
    std::filesystem::path ChunkFolder;
    std::filesystem::path TempFolder;


    typedef struct ChunkCompleteEvent_t {
        std::shared_ptr<FileNeedRecoverData_t> FileInfo;
        std::shared_ptr<FileChunkRecoverData_t> ChunkInfo;
    }ChunkCompleteEvent_t;
    moodycamel::ConcurrentQueue<ChunkCompleteEvent_t> ChunkCompleteQueue;
    FolderRecoverWorkData_t::ChunkCompleteEvent_t ChunkEventCache[5];

    std::atomic<std::error_code> ErrorCode;
    std::set<std::shared_ptr<RecoverFileTaskData_t>> FileTasks;
    std::vector<std::shared_ptr<RecoverFileTaskData_t>> FileTaskPool;
    moodycamel::ConcurrentQueue<std::shared_ptr<RecoverFileTaskData_t>> FileTaskQueue;

    //std::unordered_map<std::u8string_view, SourceChunkReverseCheckData_t> SourceChunks;
}FolderRecoverWorkData_t;