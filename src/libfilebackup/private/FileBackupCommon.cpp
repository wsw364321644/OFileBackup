#include "FileBackupCommon.h"
#include <string_convert.h>
#include <nlohmann/json.hpp>

void to_json(nlohmann::json& j, const FolderManifest_t& FolderManifest) {
    j = nlohmann::json(nlohmann::json::value_t::object);
    auto FilesNode = nlohmann::json(nlohmann::json::value_t::object);
    for (auto& [fileName,fileData] : FolderManifest.Files) {
        auto FileNode = nlohmann::json(nlohmann::json::value_t::object);
        auto FileChunksNode = nlohmann::json(nlohmann::json::value_t::array);
        fileData->FileHash[StrongHashBit / 4] = 0;
        for (auto& [chunkName,chunk] : fileData->Chunks) {
            FileChunksNode.push_back(nlohmann::json{ {"HexName",chunk.HexName},{"StartPos",chunk.StartPos} });
        }
        FileNode["FileHash"] = fileData->FileHash;
        FileNode["FileSize"] = fileData->FileSize;
        FileNode["Chunks"] = FileChunksNode;
        FilesNode[fileData->FileName] = FileNode;
    }
    j["Files"] = FilesNode;
    j["HexNameLen"] = FolderManifest.HexNameLen;
    j["ChunkFileMaxSize"] = FolderManifest.ChunkFileMaxSize;
}

void from_json(const nlohmann::json& j, FolderManifest_t& FolderManifest) {
    if (!j.contains("Files")) {
        return;
    }
    FolderManifest.ChunkFileMaxSize = j["ChunkFileMaxSize"].get_ref<const nlohmann::json::number_unsigned_t&>();
    FolderManifest.HexNameLen = j["HexNameLen"].get_ref<const nlohmann::json::number_unsigned_t&>();
    auto& FilesNode = j["Files"];
    for (auto itFilesNode = FilesNode.begin(); itFilesNode != FilesNode.end(); ++itFilesNode)
    {
        auto pFileChunksData = std::make_shared<FileChunksData_t>();
        pFileChunksData->FileName = itFilesNode.key();
        FolderManifest.Files[ConvertStringTotU8View(pFileChunksData->FileName)] = pFileChunksData;
        auto& FileNode = itFilesNode.value();
        if (!FileNode.contains("Chunks") || !FileNode.contains("FileHash") || !FileNode["Chunks"].is_array()) {
            return;
        }
        auto& FileHash = FileNode["FileHash"].get_ref<const nlohmann::json::string_t&>();
        memcpy(pFileChunksData->FileHash, FileHash.c_str(), FileHash.length() + 1);
        pFileChunksData->FileSize= FileNode["FileSize"].get_ref<const nlohmann::json::number_unsigned_t&>();
        for (auto itChunks = FileNode["Chunks"].begin(); itChunks != FileNode["Chunks"].end(); ++itChunks) {
            FileChunkData_t chunkData;
            auto& ChunkNode = itChunks.value();
            if (!ChunkNode.contains("HexName") || !ChunkNode.contains("StartPos")) {
                return;
            }
            auto& HexName = ChunkNode["HexName"].get_ref<const nlohmann::json::string_t&>();
            memcpy(chunkData.HexName, HexName.c_str(), HexName.length() + 1);
            chunkData.StartPos = ChunkNode["StartPos"].get_ref<const nlohmann::json::number_integer_t&>();
            pFileChunksData->Chunks.try_emplace(std::u8string_view((const char8_t*)chunkData.HexName), chunkData);
        }
    }
}

std::shared_ptr<const std::string> FolderManifest_t::to_string() const
{
    nlohmann::json j = *this;
    auto out = std::make_shared<std::string>();
    *out = j.dump();
    return out;
}

std::shared_ptr<const FolderManifest_t> FolderManifest_t::from_string(const char* jsonstr)
{
    auto j = nlohmann::json::parse(jsonstr, nullptr, false, true);
    if (j.is_discarded())
    {
        return nullptr;
    }
    auto out = std::make_shared<FolderManifest_t>();
    *out = j.get<FolderManifest_t>();
    return out;
}

std::shared_ptr<const FolderManifest_t> FolderManifest_t::from_string(const char* content, uint32_t size)
{
    auto j = nlohmann::json::parse(content, content+ size, nullptr, false, true);
    if (j.is_discarded())
    {
        return nullptr;
    }
    auto out = std::make_shared<FolderManifest_t>();
    *out = j.get<FolderManifest_t>();
    return out;
}

std::shared_ptr<const FolderManifestCompareResult_t> CompareFolderManifest(const FolderManifest_t& source, const FolderManifest_t& target)
{
    auto out = std::make_shared<FolderManifestCompareResult_t>();
    auto& HexNames = out->MissingFileChunks;
    for (auto& [pathstr, FileChunksData] : target.Files) {
        for (auto& [chunkName,FileChunk] : FileChunksData->Chunks) {
            HexNames.insert(FileChunk.HexName);
        }
    }
    for (auto& [pathstr, FileChunksData] : source.Files) {
        for (auto& [chunkName, FileChunk] : FileChunksData->Chunks) {
            HexNames.erase(FileChunk.HexName);
        }
    }
    return out;
}
