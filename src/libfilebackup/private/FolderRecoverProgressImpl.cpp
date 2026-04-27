#include "FolderRecoverProgressImpl.h"

void FolderRecoverProgressImpl::Init(std::shared_ptr < const  FolderManifest_t> pTargetManifest, std::shared_ptr<const FolderManifest_t> source)
{
    Manifest = pTargetManifest;
    SourceManifest= source;

    auto& targetManifest = *pTargetManifest;
    CompareResult =CompareFolderManifest(targetManifest, source);

    //init progress
    std::unordered_map<std::u8string_view, std::shared_ptr<FileNeedRecoverData_t>> OrderedFiles;
    auto& folderManifestCompareResult = *CompareResult;
    CharBuf.Resize(sizeof(FolderRecoverProgressHeader_t));
    memset(&GetFolderRecoverProgressHeader(), 0, sizeof(FolderRecoverProgressHeader_t));
    GetFolderRecoverProgressHeader().AllFileNum = folderManifestCompareResult.FileConstructChunks.size();
    //GetFolderRecoverProgressHeader().AllDeleteFileNum = folderManifestCompareResult.FilesNeedDelete.size();
    //GetFolderRecoverProgressHeader().FileNameTableOffset = GetFolderRecoverProgressHeader().FileChunkStatusTableOffset =
    //    sizeof(FolderRecoverProgressHeader_t)
    //    + GetFolderRecoverProgressHeader().AllDeleteFileNum * sizeof(FolderDeleteFileHeader_t)
    //    + GetFolderRecoverProgressHeader().AllFileNum * sizeof(FolderRecoverFileProgressHeader_t);
    GetFolderRecoverProgressHeader().FileChunkStatusTableOffset =
        sizeof(FolderRecoverProgressHeader_t)
        + GetFolderRecoverProgressHeader().AllFileNum * sizeof(FolderRecoverFileProgressHeader_t);
    for (auto& [fileName, chunks] : folderManifestCompareResult.FileConstructChunks) {
        //GetFolderRecoverProgressHeader().FileChunkStatusTableOffset += fileName.size();
        GetFolderRecoverProgressHeader().AllFileChunkNum += chunks.size();

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
        OrderedFiles.try_emplace(fileName, pFileNeedRecoverData);
    }
    memcpy(GetFolderRecoverProgressHeader().TargetID, targetManifest.ID, sizeof(GetFolderRecoverProgressHeader().TargetID));
    if (source) {
        memcpy(GetFolderRecoverProgressHeader().SourceID, source->ID, sizeof(GetFolderRecoverProgressHeader().SourceID));
    }
    auto divRes = std::div(GetFolderRecoverProgressHeader().AllFileChunkNum, CHAR_BIT);
    CharBuf.Resize(GetFolderRecoverProgressHeader().FileChunkStatusTableOffset + divRes.quot + (divRes.rem > 0 ? 1 : 0));
    memset(CharBuf.Data() + GetFolderRecoverProgressHeader().FileChunkStatusTableOffset, 0, divRes.quot + (divRes.rem > 0 ? 1 : 0));

    uint32_t FileNameOffsetCounr{ 0 };
    uint32_t FileChunkBitCount{ 0 };
    uint32_t FileChunkByteCount{ 0 };

    auto i = 0;
    for (auto& [fileName, pFileInfo] : OrderedFiles ) {
        pFileInfo->Index = i++;
        FilesNeedRecover.try_emplace(fileName, pFileInfo);
        auto& fileInfo = *pFileInfo;
        GetFileProgressHeader(fileInfo.Index).ChunkNum = fileInfo.NeedRecoverChunks.size();
        //GetFileProgressHeader(fileInfo.Index).FileNameLen = fileInfo.FileData->FileName.size();
        //GetFileProgressHeader(fileInfo.Index).FileNameOffset = FileNameOffsetCounr;
        GetFileProgressHeader(fileInfo.Index).FileChunkStatusByteOffset = FileChunkByteCount;
        GetFileProgressHeader(fileInfo.Index).FileChunkStatusBitOffset = FileChunkBitCount;
        FileNameOffsetCounr += fileInfo.FileData->FileName.size();
        divRes = std::div(fileInfo.NeedRecoverChunks.size() + FileChunkBitCount, CHAR_BIT);
        FileChunkByteCount += divRes.quot;
        FileChunkBitCount = divRes.rem;
        //memcpy(CharBuf.Data() + GetFolderRecoverProgressHeader().FileNameTableOffset + GetFileProgressHeader(fileInfo.Index).FileNameOffset, fileInfo.FileData->FileName.data(), fileInfo.FileData->FileName.size());
    }
}