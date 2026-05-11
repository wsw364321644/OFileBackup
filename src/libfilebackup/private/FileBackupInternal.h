#pragma once
#include "FileBackupCommon.h"
#include "FileBackupManager.h"
#include <simple_adler32.h>
#include <endian_helper.h>

#include <xxhash.h>
#include <zstd.h>
#ifdef BOOST_FOUND
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#else
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#endif

#include <fstream>
#include <atomic>
#include <span>

#ifdef BOOST_FOUND
typedef boost::unordered_flat_set<std::string> HashSetType;
typedef boost::unordered_flat_map<WeakHash_t, HashSetType> HashMapType;
#else
typedef absl::flat_hash_set<std::string> HashSetType;
typedef absl::flat_hash_map<WeakHash_t, HashSetType> HashMapType;
#endif

inline thread_local FRollingAdler32 RollingAdler32;

class FChunkConverter:public IChunkConverter {
public:
    FChunkConverter();
    FChunkConverter(EConvertDirection Direction);
    ~FChunkConverter();
    void* GetChunkFileBuf() override {
        return  ZSTDBuf ;
    }
    void UpdateChunkFileSize(size_t newSize) override {
        ZSTDBufContentSize = newSize;
    }
    size_t GetChunkFileSize() const override {
        return ZSTDBufContentSize;
    }
    size_t GetChunkFileMaxSize()const override {
        return ZSTDBufSize;
    }
    void UpdateConvertDirection(EConvertDirection Direction) override;
    void Convert(const uint8_t* FileChunk) override;

    EConvertDirection Direction{ EConvertDirection::None };
    ZSTD_CCtx* CCtx{ nullptr };
    ZSTD_DCtx* DCtx{ nullptr };
    size_t ZSTDBufSize{0};
    size_t ZSTDBufContentSize{0};
    void* ZSTDBuf{ nullptr };
};

typedef struct FileChunkBuf_t {
    static constexpr uint32_t FileBufSize = FileChunkSize * 8; // 文件缓冲区总大小
    static constexpr uint32_t ConsumedFileBufSize = FileChunkSize * 3; // 已消费缓冲区大小
    char OutBuf[ConsumedFileBufSize] = {};
    char Buf[ConsumedFileBufSize + FileBufSize] = {};
    char* const BufEnd = Buf + ConsumedFileBufSize + FileBufSize;
    char* ConsumePos = Buf;//读取位置
    char* StreamPos = Buf;//写入位置
    std::atomic_uint32_t ContentSize{ 0 };
    void Clear() {
        memset(Buf, 0, ConsumedFileBufSize + FileBufSize);
        ConsumePos = Buf;
        StreamPos = Buf;
        ContentSize = 0;
    }

    std::span<char> GetEmptyBuf() {
        auto FreeSpaceSize = FileBufSize - ContentSize;
        return { StreamPos,StreamPos + FreeSpaceSize > BufEnd ? uint32_t(BufEnd - StreamPos) : FreeSpaceSize };
    }
    std::span<char> GetContentBuf() {
        auto contentSize = ContentSize.load();
        return { ConsumePos,ConsumePos + contentSize > BufEnd ? uint32_t(BufEnd - ConsumePos) : contentSize };
    }
    std::tuple<std::span<char>, std::span<char>> GetConsumedBuf(uint32_t reverseStart, uint32_t reverseLen) {
        auto PosAfterOffset = ConsumePos - reverseStart;
        if (PosAfterOffset <= Buf) {
            PosAfterOffset = BufEnd - (Buf - PosAfterOffset);
        }
        if (PosAfterOffset - reverseLen >= Buf) {
            return { {PosAfterOffset - reverseLen,reverseLen},{} };
        }
        auto firstLen = reverseLen - uint32_t(PosAfterOffset - Buf);
        return { {BufEnd - firstLen,firstLen},{Buf,uint32_t(PosAfterOffset - Buf)} };
    }
    char* GetContinuousConsumedBuf(uint32_t reverseStart, uint32_t reverseLen) {
        auto [ConsumedBufTupleL, ConsumedBufTupleR] = GetConsumedBuf(reverseStart, reverseLen);
        auto ConsumedBufL = ConsumedBufTupleL;
        auto ConsumedBufR = ConsumedBufTupleR;
        if (ConsumedBufR.size() == 0) {
            return ConsumedBufL.data();
        }
        memcpy(OutBuf, ConsumedBufL.data(), ConsumedBufL.size());
        memcpy(OutBuf + ConsumedBufL.size(), ConsumedBufR.data(), ConsumedBufR.size());
        return OutBuf;
    }
    void FillSize(uint32_t size) {
        AdvancePos(StreamPos, size);
        ContentSize.fetch_add(size, std::memory_order::relaxed);
    }
    void EatSize(uint32_t size) {
        AdvancePos(ConsumePos, size);
        ContentSize.fetch_sub(size, std::memory_order::relaxed);
    }

    void AdvancePos(char*& Pos, uint32_t size) {
        Pos += size;
        if (Pos >= BufEnd) {
            Pos = Pos - BufEnd + Buf;
        }
    }
}FileChunkBuf_t;

typedef struct GenFolderChunkDataFileTaskData_t {
    HashMapType FileAllHashMap;
    XXH3_state_t* XXH3State;
    FChunkConverter ChunkConverter{};
    std::shared_ptr<FileChunksData_t> FileChunksData;
    IFileBackupManagerInterface::TNewFileChunkDelegate  NewFileChunkDelegate;
    //both
    //std::shared_mutex FileChunkBufMtx;
    std::shared_ptr<FileChunkBuf_t> FileChunkBuf;//Guard by ContentSize
    bool bEOF{ false };
    //read thread
    std::ifstream FileStream;
    uint32_t WaitAppendDataLen{ 0 };

    ~GenFolderChunkDataFileTaskData_t() {
        XXH3_createState();
        if (XXH3State) {
            XXH3_freeState(XXH3State);
        }
    }
    void Clear() {
        if (FileStream.is_open()) {
            FileStream.close();
        }
        bEOF = false;
        WaitAppendDataLen = 0;
        FileChunkBuf->Clear();
        if (XXH3State) {
            XXH3_128bits_reset(XXH3State);
        }
    }
}GenFolderChunkDataFileTaskData_t;

typedef struct GenFolderChunkDataWorkData_t {
    std::atomic<EGenFolderMetaDataStatus> Status{ EGenFolderMetaDataStatus::None };
    EGenFolderMetaDataStatus LastStatus{ EGenFolderMetaDataStatus::None };
    IFileBackupManagerInterface::TGenFolderMetaDataStatusChangedDelegate StatusChangedDelegate;

    GenFolderChunkParams_t Params;
    uint64_t ToltalSize{ 0 };
    std::atomic<uint64_t> CompleteSize{ 0 };
    std::map<uint64_t, std::set<std::u8string_view>> FileItrList; //order file name by file sizes
    std::unordered_map<std::u8string_view, std::string> FileLocalPathMap; //Get local file path from file name

    //std::shared_mutex FileTaskMtx;
    HashMapType AllHashMap;
    //std::unordered_map<WeakHash_t, std::set<std::string>, hash_32bit> AllHashMap;
    std::unordered_map<std::u8string_view, std::shared_ptr<GenFolderChunkDataFileTaskData_t>> FileTasks;
    std::vector<std::shared_ptr<GenFolderChunkDataFileTaskData_t>> FileTaskPool;

    FolderManifest_t FolderManifest;
    std::shared_ptr<GenFolderMetaDataProcess_t> OutProcess;
    std::shared_ptr<FolderManifest_t> OutFolderManifest;
    std::error_code EC;

}GenFolderChunkDataWorkData_t;


inline void CopyxxHashToBuf(XXH128_hash_t& hash, unsigned char buf[16]) {
    auto highbe64 = htobe64(hash.high64);
    auto lowbe64 = htobe64(hash.low64);
    memcpy(buf,&highbe64, sizeof(highbe64));
    memcpy(buf+ sizeof(highbe64),&lowbe64, sizeof(lowbe64));
}