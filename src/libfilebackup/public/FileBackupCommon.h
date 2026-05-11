#pragma once


#include <stdint.h>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <CharBuffer.h>
#include <std_ext.h>
#include <simple_uuid.h>
#include <hex.h>
#include "FileBackupExportDef.h"
typedef uint32_t WeakHash_t;
constexpr uint8_t StrongHashBit = 1 << 7;
constexpr uint8_t FileHashLen = bin_to_hex_length(StrongHashBit / CHAR_BIT);
//constexpr uint8_t HexNameStrLen = (sizeof(WeakHash_t) * CHAR_BIT + StrongHashBit) / 4 ;
constexpr uint8_t HexNameStrLen = bin_to_hex_length(sizeof(WeakHash_t) + StrongHashBit/ CHAR_BIT);
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
    typedef std::set<std::shared_ptr<FileChunkData_t>, FileChunkDataLess_t, allocator_save_memory_operator<std::shared_ptr<FileChunkData_t>>> TFileChunks;
    string_save_memory_operator FileName;
    char FileHash[FileHashLen + 1]{0};
    uint64_t FileSize;
    TFileChunks Chunks;
}FileChunksData_t;

inline std::u8string_view GetHexNameView(char* HexName) {
    return std::u8string_view((const char8_t*)HexName, HexNameStrLen);
}

typedef struct FolderManifest_t {
    typedef std::unordered_map<std::u8string_view, std::shared_ptr<FileChunksData_t>, string_hash, std::equal_to<>, allocator_save_memory_operator<std::pair<const std::u8string_view, std::shared_ptr<FileChunksData_t>>>> TFiles;
    TFiles Files;
    uint8_t HexNameLen{ HexNameStrLen };
    uint32_t ChunkFileMaxSize{ 0 };
    char ID[bin_to_hex_length(UUID_128_BYTES)+1]{ 0 };
    LIB_FILEBACKUP_EXPORT void to_string(FCharBuffer& charBuf ,std::error_code&ec) const;
    LIB_FILEBACKUP_EXPORT static std::shared_ptr<const FolderManifest_t>from_string(FCharBuffer& str, std::error_code& ec);
    LIB_FILEBACKUP_EXPORT static int32_t get_string_extra_space();
}FolderManifest_t;

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
    virtual void Convert(const uint8_t* FileChunk) = 0;
};
LIB_FILEBACKUP_EXPORT std::shared_ptr<IChunkConverter> NewChunkConverter();


typedef struct ChunkReverseCheckData_t {
    std::vector<std::pair<std::shared_ptr<FileChunksData_t>, std::shared_ptr<FileChunkData_t>>, allocator_save_memory_operator<std::pair<std::shared_ptr<FileChunksData_t>, std::shared_ptr<FileChunkData_t>>>> ChunkInFileData;
}ChunkReverseCheckData_t;

typedef struct FileConstructChunkData_t {
    std::shared_ptr<FileChunkData_t> ChunkData;
    bool bFromSource{ false };
}FileConstructChunkData_t;

typedef struct FileConstructChunkDataLess_t {
    bool operator ()(const FileConstructChunkData_t& L, const FileConstructChunkData_t& R) const {
        return L.ChunkData->StartPos < R.ChunkData->StartPos;
    }
    bool operator ()(const std::shared_ptr<FileConstructChunkData_t>& L, const std::shared_ptr<FileConstructChunkData_t>& R) const {
        return operator ()(*L, *R);
    }
}FileConstructChunkDataLess_t;
typedef std::set<std::shared_ptr<FileConstructChunkData_t>, FileConstructChunkDataLess_t, allocator_save_memory_operator<std::shared_ptr<FileConstructChunkData_t>>> TFileConstructChunks;


typedef struct FolderManifestCompareResult_t {
    std::unordered_set<std::u8string_view, string_hash, std::equal_to<>, allocator_save_memory_operator<std::u8string_view>> MissingFileChunks;
    std::unordered_set<std::u8string_view, string_hash, std::equal_to<>, allocator_save_memory_operator<std::u8string_view>> FilesNeedDelete;
    std::unordered_map<std::u8string_view, ChunkReverseCheckData_t, string_hash, std::equal_to<>, allocator_save_memory_operator<std::pair<const std::u8string_view, ChunkReverseCheckData_t>>> SourceChunkReverseIndex;
    std::unordered_map<std::u8string_view, ChunkReverseCheckData_t, string_hash, std::equal_to<>, allocator_save_memory_operator<std::pair<const std::u8string_view, ChunkReverseCheckData_t>>> TargetChunkReverseIndex;
    std::unordered_map<std::u8string_view, TFileConstructChunks, string_hash, std::equal_to<>, allocator_save_memory_operator<std::pair<const std::u8string_view, TFileConstructChunks>>> FileConstructChunks;
}FolderManifestCompareResult_t;


LIB_FILEBACKUP_EXPORT std::shared_ptr<const FolderManifestCompareResult_t> CompareFolderManifest( const FolderManifest_t& target, std::shared_ptr<const FolderManifest_t> source);

