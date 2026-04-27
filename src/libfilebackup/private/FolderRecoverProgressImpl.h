#include "FolderRecoverHelper.h"


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
    void Init(std::shared_ptr < const  FolderManifest_t> targetManifest, std::shared_ptr<const FolderManifest_t> source) override;
    std::shared_ptr <const FolderManifest_t> Manifest;
    std::shared_ptr <const FolderManifest_t> SourceManifest;

    TFilesNeedRecover FilesNeedRecover;
};
