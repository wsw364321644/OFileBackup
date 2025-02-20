#include "FileBackupInternal.h"


FChunkConverter::FChunkConverter()
{
}

FChunkConverter::FChunkConverter(EConvertDirection Direction)
{
    UpdateConvertDirection(Direction);
}

FChunkConverter::~FChunkConverter()
{
    if (ZSTDBuf) {
        free(ZSTDBuf);
    }
    if (CCtx) {
        ZSTD_freeCCtx(CCtx);
    }
    if (DCtx) {
        ZSTD_freeDCtx(DCtx);
    }
}

void FChunkConverter::UpdateConvertDirection(EConvertDirection newDirection)
{
    Direction = newDirection;
    if (!ZSTDBuf) {
        ZSTDBufSize = ZSTD_compressBound(FileChunkSize);
        ZSTDBuf = malloc(ZSTDBufSize);
    }
    switch (Direction)
    {
    case EConvertDirection::ToFileChunk:
    {
        if (DCtx) {
            return;
        }
        DCtx = ZSTD_createDCtx();
        break;
    }
    case EConvertDirection::ToChunkFile:
    {
        if (CCtx) {
            return;
        }
        CCtx = ZSTD_createCCtx();
        break;
    }
    default:
        break;
    }
}

void FChunkConverter::Convert(uint8_t* FileChunk)
{
    switch (Direction)
    {
    case EConvertDirection::ToFileChunk:
    {
        size_t const dSize = ZSTD_decompressDCtx(DCtx, FileChunk, FileChunkSize, ZSTDBuf, ZSTDBufContentSize);
        break;
    }
    case EConvertDirection::ToChunkFile:
    {
        ZSTDBufContentSize = ZSTD_compressCCtx(CCtx, ZSTDBuf, ZSTDBufSize, FileChunk, FileChunkSize, 1);
        break;
    }
    default:
        break;
    }
}

std::shared_ptr<IChunkConverter> NewChunkConverter() {
    return std::make_shared<FChunkConverter>();
}