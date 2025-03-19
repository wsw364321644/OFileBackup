#pragma once
#include "FileBackupExportDef.h"
#include <std_ext.h>
#include <stdint.h>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <string>
#include <functional>

typedef uint32_t WeakHash_t;
constexpr uint8_t StrongHashBit = 1 << 7;
constexpr uint8_t HexNameStrLen = (sizeof(WeakHash_t) * CHAR_BIT + StrongHashBit) / 4 ;
constexpr uint32_t FileChunkSize = 1 << 20;

typedef struct FileChunkData_t {
    char HexName[HexNameStrLen + 1] ;
    uint64_t StartPos;
    bool operator < (const FileChunkData_t& other) const {
        return StartPos < other.StartPos;
    }
}FileChunkData_t;

typedef struct FileChunkDataLess_t {
    bool operator ()(const FileChunkData_t& L, const FileChunkData_t& R) const {
        return L.StartPos < R.StartPos;
    }
    bool operator ()(const std::shared_ptr<FileChunkData_t>& L, const std::shared_ptr<FileChunkData_t>& R) const {
        return operator ()(*L, *R);
    }
}FileChunkDataLess_t;

typedef struct FileChunksData_t {
    std::string FileName;
    char FileHash[StrongHashBit / 4 + 1]{};
    uint64_t FileSize;
    std::set<std::shared_ptr<FileChunkData_t>, FileChunkDataLess_t> Chunks;
}FileChunksData_t;

typedef struct FolderManifest_t {
    inline std::u8string_view GetHexNameView(char* HexName) const {
        return std::u8string_view((const char8_t*)HexName, HexNameLen);
    }
    std::unordered_map<std::u8string_view, std::shared_ptr<FileChunksData_t>, string_hash> Files;
    uint8_t HexNameLen{ HexNameStrLen };
    uint32_t ChunkFileMaxSize{ 0 };
    LIB_FILEBACKUP_EXPORT std::shared_ptr<const std::string> to_string() const;
    LIB_FILEBACKUP_EXPORT static std::shared_ptr<const FolderManifest_t>from_string(const char*);
    LIB_FILEBACKUP_EXPORT static std::shared_ptr<const FolderManifest_t>from_string(const char* content,uint32_t size);
}FolderManifest_t;


typedef struct FolderManifestCompareResult_t {
    std::set<std::string> MissingFileChunks;
}FolderManifestCompareResult_t;


enum class EConvertDirection
{
    None,
    ToFileChunk,
    ToChunkFile
};
class IChunkConverter {
public:
    virtual void* GetChunkFileBuf() = 0;
    virtual void UpdateChunkFileSize(size_t) = 0;
    virtual size_t GetChunkFileSize()const = 0;
    virtual size_t GetChunkFileMaxSize()const = 0;
    virtual void UpdateConvertDirection(EConvertDirection Direction) = 0;
    virtual void Convert(uint8_t* FileChunk) = 0;
};
LIB_FILEBACKUP_EXPORT std::shared_ptr<IChunkConverter> NewChunkConverter();
LIB_FILEBACKUP_EXPORT std::shared_ptr<const FolderManifestCompareResult_t> CompareFolderManifest(const FolderManifest_t& source, const FolderManifest_t& target);

