#pragma once
#include "FileBackupCommon.h"
#include <zstd.h>


class FChunkConverter:public IChunkConverter {
public:
    FChunkConverter();
    FChunkConverter(EConvertDirection Direction);
    ~FChunkConverter();
    void* GetChunkFileBuf() override {
        return  ZSTDBuf ;
    }
    size_t& GetChunkFileSize() override {
        return ZSTDBufContentSize;
    }
    size_t GetChunkFileBufSize()const override {
        return ZSTDBufSize;
    }
    void UpdateConvertDirection(EConvertDirection Direction) override;

    void Convert(uint8_t* FileChunk) override;

    EConvertDirection Direction{ EConvertDirection::None };
    ZSTD_CCtx* CCtx{ nullptr };
    ZSTD_DCtx* DCtx{ nullptr };
    size_t ZSTDBufSize{0};
    size_t ZSTDBufContentSize{0};
    void* ZSTDBuf{ nullptr };
};

