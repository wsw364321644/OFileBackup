#include "FolderRecoverProgressImpl.h"
#include "FolderRecoverHelper.h"
#include <dir_util.h>
void FolderRecoverProgressImpl::Init(std::shared_ptr<FolderRecoverWorkData_t> workData, std::shared_ptr < const  FolderManifest_t> pTargetManifest, std::shared_ptr<const FolderManifest_t> pSourceManifest, std::error_code& ec)
{
    ec.clear();
    Manifest = pTargetManifest;
    SourceManifest = pSourceManifest;
    if (!FileBackedBuffer) {
        FileBackedBuffer = NewFileBackedBuffer();
    }

    auto& targetManifest = *pTargetManifest;
    CompareResult = CompareFolderManifest(targetManifest, pSourceManifest);
    auto& FolderRecoverWorkData = *workData;
    //init progress
    std::map<std::u8string_view, std::shared_ptr<FileNeedRecoverData_t>> OrderedFiles;
    auto& folderManifestCompareResult = *CompareResult;
    FolderRecoverProgressHeader_t FolderRecoverProgressHeader;
    FolderRecoverProgressHeader.AllFileNum = folderManifestCompareResult.FileConstructChunks.size();
    FolderRecoverProgressHeader.FileChunkStatusTableOffset = sizeof(FolderRecoverProgressHeader_t)
        + FolderRecoverProgressHeader.AllFileNum * sizeof(FolderRecoverFileProgressHeader_t);
    for (auto& [fileName, chunks] : folderManifestCompareResult.FileConstructChunks) {
        if (chunks.size() <= 0) {
            continue;
        }
        FolderRecoverProgressHeader.AllFileChunkNum += chunks.size();

        //init ram data for construct file
        auto pFileNeedRecoverData = std::make_shared<FileNeedRecoverData_t>();
        pFileNeedRecoverData->FileData = targetManifest.Files.at(fileName);
        for (auto& chunk : chunks) {
            auto pFileChunkRecoverData = std::make_shared<FileChunkRecoverData_t>();
            pFileChunkRecoverData->ConstructChunkData = chunk;
            pFileNeedRecoverData->NeedRecoverChunks.emplace(pFileChunkRecoverData);
        }
        int i = 0;
        for (auto& NeedRecoverChunk : pFileNeedRecoverData->NeedRecoverChunks) {
            NeedRecoverChunk->Index = i++;
        }
        FilesNeedRecover.try_emplace(fileName, pFileNeedRecoverData);
        OrderedFiles.try_emplace(fileName, pFileNeedRecoverData);
    }
    memcpy(FolderRecoverProgressHeader.TargetID, targetManifest.ID, sizeof(GetFolderRecoverProgressHeader().TargetID));
    if (pSourceManifest) {
        memcpy(FolderRecoverProgressHeader.SourceID, pSourceManifest->ID, sizeof(GetFolderRecoverProgressHeader().SourceID));
    }
    FolderRecoverProgressHeader.bTempFolderExist = DirUtil::IsExist(FolderRecoverWorkData.TempFolder.u8string());

    auto fileName = FolderRecoverWorkData.WorkFolder.filename();
    fileName.replace_extension("rcv");
    auto filePath = FolderRecoverWorkData.TempFolder.parent_path() / fileName;
    auto divRes = std::div(FolderRecoverProgressHeader.AllFileChunkNum, CHAR_BIT);
    auto bres = FileBackedBuffer->Init(FolderRecoverProgressHeader.FileChunkStatusTableOffset + divRes.quot + (divRes.rem > 0 ? 1 : 0), filePath.u8string(), ec);
    if (!bres) {
        return;
    }

    auto i = 0;
    /// old data exist
    do {
        if (ec != std::make_error_code(std::errc::file_exists)) {
            break;
        }
        ec.clear();
        if (memcmp(GetFolderRecoverProgressHeader().TargetID, pTargetManifest->ID, sizeof(FolderRecoverProgressHeader.TargetID)) != 0) {
            break;
        }
        if (pSourceManifest) {
            if (memcmp(GetFolderRecoverProgressHeader().SourceID, pSourceManifest->ID, sizeof(FolderRecoverProgressHeader.SourceID)) != 0) {
                break;
            }
        }
        else {
            if (memcmp(GetFolderRecoverProgressHeader().SourceID, FolderRecoverProgressHeader.SourceID, sizeof(FolderRecoverProgressHeader.SourceID)) != 0) {
                break;
            }
        }
        for (auto& [fileName, pFileInfo] : OrderedFiles) {
            auto& fileInfo = *pFileInfo;
            fileInfo.Index = i++;
            auto& fileProgress = GetFileProgressHeader(fileInfo.Index);
            for (auto itr = fileInfo.NeedRecoverChunks.begin(); itr != fileInfo.NeedRecoverChunks.end(); ) {
                auto& ChunksRecoverData = *itr;
                if (GetFileChunkStatus(fileProgress, ChunksRecoverData->Index)) {
                    itr = fileInfo.NeedRecoverChunks.erase(itr);
                }
                else {
                    if (ChunksRecoverData->ConstructChunkData->bFromSource) {
                        NeedRecoverSourceFileChunks.emplace(GetHexNameView(ChunksRecoverData->ConstructChunkData->ChunkData->HexName));
                    }
                    else {
                        NeedRecoverMissingFileChunks.emplace(GetHexNameView(ChunksRecoverData->ConstructChunkData->ChunkData->HexName));
                    }
                    itr++;
                }
            }
            if (fileInfo.NeedRecoverChunks.size() == 0) {
                FilesNeedRecover.erase(fileName);
            }
        }
        return;
    } while (true);
    ec.clear();
    /// gen new data

    FileBackedBuffer->WriteData(0u, FolderRecoverProgressHeader);
    uint32_t FileNameOffsetCounr{ 0 };
    uint32_t FileChunkBitCount{ 0 };
    uint32_t FileChunkByteCount{ 0 };
    for (auto& [fileName, pFileInfo] : OrderedFiles) {
        FolderRecoverFileProgressHeader_t FolderRecoverFileProgressHeader;
        auto& fileInfo = *pFileInfo;
        fileInfo.Index = i++;

        FolderRecoverFileProgressHeader.ChunkNum = fileInfo.NeedRecoverChunks.size();
        FolderRecoverFileProgressHeader.FileChunkStatusByteOffset = FileChunkByteCount;
        FolderRecoverFileProgressHeader.FileChunkStatusBitOffset = FileChunkBitCount;

        FileNameOffsetCounr += fileInfo.FileData->FileName.size();
        divRes = std::div(fileInfo.NeedRecoverChunks.size() + FileChunkBitCount, CHAR_BIT);
        FileChunkByteCount += divRes.quot;
        FileChunkBitCount = divRes.rem;

        FileBackedBuffer->WriteData(sizeof(FolderRecoverProgressHeader_t) + fileInfo.Index * sizeof(FolderRecoverFileProgressHeader_t), FolderRecoverFileProgressHeader);

        for (auto& chunk : fileInfo.NeedRecoverChunks) {
            if (chunk->ConstructChunkData->bFromSource) {
                NeedRecoverSourceFileChunks.emplace(GetHexNameView(chunk->ConstructChunkData->ChunkData->HexName));
            }
            else {
                NeedRecoverMissingFileChunks.emplace(GetHexNameView(chunk->ConstructChunkData->ChunkData->HexName));
            }
        }
    }

}