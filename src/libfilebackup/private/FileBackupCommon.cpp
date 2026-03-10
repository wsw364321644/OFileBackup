#include "FileBackupCommon.h"
#include <simple_error.h>
#include <string_convert.h>
#include <simdjson.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>


//rapidjson::doc to_json(nlohmann::json& j, const FolderManifest_t& FolderManifest) {
//    j = nlohmann::json(nlohmann::json::value_t::object);
//    auto FilesNode = nlohmann::json(nlohmann::json::value_t::object);
//    for (auto& [fileName,fileData] : FolderManifest.Files) {
//        auto FileNode = nlohmann::json(nlohmann::json::value_t::object);
//        auto FileChunksNode = nlohmann::json(nlohmann::json::value_t::array);
//        fileData->FileHash[StrongHashBit / 4] = 0;
//        for (auto& pChunk : fileData->Chunks) {
//            auto& chunk = *pChunk;
//            FileChunksNode.push_back(nlohmann::json{ {"HexName",chunk.HexName},{"StartPos",chunk.StartPos} });
//        }
//        FileNode["FileHash"] = fileData->FileHash;
//        FileNode["FileSize"] = fileData->FileSize;
//        FileNode["Chunks"] = FileChunksNode;
//        FilesNode[fileData->FileName] = FileNode;
//    }
//    j["Files"] = FilesNode;
//    j["HexNameLen"] = FolderManifest.HexNameLen;
//    j["ChunkFileMaxSize"] = FolderManifest.ChunkFileMaxSize;
//}
//
//void from_json(const nlohmann::json& j, FolderManifest_t& FolderManifest) {
//    if (!j.contains("Files")) {
//        return;
//    }
//    FolderManifest.ChunkFileMaxSize = j["ChunkFileMaxSize"].get_ref<const nlohmann::json::number_unsigned_t&>();
//    FolderManifest.HexNameLen = j["HexNameLen"].get_ref<const nlohmann::json::number_unsigned_t&>();
//    auto& FilesNode = j["Files"];
//    for (auto itFilesNode = FilesNode.begin(); itFilesNode != FilesNode.end(); ++itFilesNode)
//    {
//        auto pFileChunksData = std::make_shared<FileChunksData_t>();
//        pFileChunksData->FileName = itFilesNode.key();
//        FolderManifest.Files[ConvertStringTotU8View(pFileChunksData->FileName)] = pFileChunksData;
//        auto& FileNode = itFilesNode.value();
//        if (!FileNode.contains("Chunks") || !FileNode.contains("FileHash") || !FileNode["Chunks"].is_array()) {
//            return;
//        }
//        auto& FileHash = FileNode["FileHash"].get_ref<const nlohmann::json::string_t&>();
//        memcpy(pFileChunksData->FileHash, FileHash.c_str(), FileHash.length() + 1);
//        pFileChunksData->FileSize= FileNode["FileSize"].get_ref<const nlohmann::json::number_unsigned_t&>();
//        for (auto itChunks = FileNode["Chunks"].begin(); itChunks != FileNode["Chunks"].end(); ++itChunks) {
//            auto pChunkData=std::make_shared<FileChunkData_t> ();
//            FileChunkData_t& chunkData = *pChunkData;
//            auto& ChunkNode = itChunks.value();
//            if (!ChunkNode.contains("HexName") || !ChunkNode.contains("StartPos")) {
//                return;
//            }
//            auto& HexName = ChunkNode["HexName"].get_ref<const nlohmann::json::string_t&>();
//            memcpy(chunkData.HexName, HexName.c_str(), HexName.length() + 1);
//            chunkData.StartPos = ChunkNode["StartPos"].get_ref<const nlohmann::json::number_integer_t&>();
//            pFileChunksData->Chunks.emplace(pChunkData);
//        }
//    }
//}

rapidjson::Document to_json(const FolderManifest_t& FolderManifest) {
    rapidjson::Document doc{ rapidjson::kObjectType };
    rapidjson::Value filesNode{ rapidjson::kArrayType };
    auto& a = doc.GetAllocator();
    for (auto& [fileName, fileData] : FolderManifest.Files) {
        rapidjson::Value fileNode{ rapidjson::kObjectType };
        rapidjson::Value fileChunksNode{ rapidjson::kArrayType };
        rapidjson::Value fileChunkNode{ rapidjson::kObjectType };
        fileData->FileHash[StrongHashBit / 4] = 0;
        for (auto& pChunk : fileData->Chunks) {
            auto& chunk = *pChunk;
            fileChunkNode.AddMember("HexName", rapidjson::StringRef(chunk.HexName), a);
            fileChunkNode.AddMember("StartPos", chunk.StartPos, a);
            fileChunksNode.PushBack(fileChunkNode, a);
        }
        fileNode.AddMember("FileHash", rapidjson::StringRef(fileData->FileHash), a);
        fileNode.AddMember("FileSize", fileData->FileSize, a);
        fileNode.AddMember("Chunks", fileChunksNode, a);
        filesNode.AddMember(rapidjson::StringRef(fileData->FileName.c_str()), fileNode, a);
    }

    doc.AddMember("ChunkFileMaxSize",FolderManifest.ChunkFileMaxSize,a);
    doc.AddMember("HexNameLen", FolderManifest.HexNameLen, a);
    doc.AddMember("Files", filesNode, a);
    return doc;
}

void FolderManifest_t::to_string( FCharBuffer& charBuf, std::error_code& ec) const
{
    ec.clear();
    auto doc = to_json(*this);
    rapidjson::Writer<FCharBuffer> writer(charBuf);
    if (!doc.Accept(writer)) {
        ec = utilpp::make_common_used_error(utilpp::ECommonUsedError::CUE_UNKNOW);
    }
    return ;
}

std::shared_ptr<const FolderManifest_t> FolderManifest_t::from_string(FCharBuffer& str, std::error_code& ec)
{
    ec.clear();
    str.Reverse(str.Size() + simdjson::SIMDJSON_PADDING);
    auto out = std::make_shared<FolderManifest_t>();
    auto& manifest = *out;
    simdjson::ondemand::parser parser;
    auto doc=parser.iterate(str.Data(), str.Size(), str.Capacity());
    auto rootRes=doc.get_object();
    if (rootRes.error()!= simdjson::error_code::SUCCESS) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }

    auto u64Res= rootRes["ChunkFileMaxSize"].get_uint64();
    if (u64Res.error() != simdjson::error_code::SUCCESS) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }
    manifest.ChunkFileMaxSize = u64Res.value_unsafe();

    u64Res = rootRes["HexNameLen"].get_uint64();
    if (u64Res.error() != simdjson::error_code::SUCCESS) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }
    manifest.HexNameLen = u64Res.value_unsafe();

    auto filesRes = rootRes["Files"].get_object();
    if (filesRes.error() != simdjson::error_code::SUCCESS) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }
    for (auto field : filesRes) {
        if(field.error() != simdjson::error_code::SUCCESS) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }
        auto pFileChunksData = std::make_shared<FileChunksData_t>();
        auto& FileChunksData = *pFileChunksData;
        auto strRes = field.unescaped_key();
        if(strRes.error() != simdjson::error_code::SUCCESS) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }
        FileChunksData.FileName = strRes.value_unsafe();
        auto insertRes=manifest.Files.try_emplace(ConvertViewToU8View(FileChunksData.FileName), pFileChunksData);
        if (!insertRes.second) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }

        auto fileRes = field.value().get_object();
        if (fileRes.error() != simdjson::error_code::SUCCESS) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }

        strRes=fileRes["FileHash"].get_string();
        if(strRes.error() != simdjson::error_code::SUCCESS) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }
        memcpy(FileChunksData.FileHash ,strRes.value_unsafe().data(), strRes.value_unsafe().size());
        FileChunksData.FileHash[strRes.value_unsafe().size()] = '\0';

        u64Res = fileRes["FileSize"].get_uint64();
        if (u64Res.error() != simdjson::error_code::SUCCESS) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }
        FileChunksData.FileSize = u64Res.value_unsafe();

        auto chunksRes = fileRes["Chunks"].get_array();
        if (chunksRes.error() != simdjson::error_code::SUCCESS) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }
        for (auto chunkRes: chunksRes) {
            auto pChunkData = std::make_shared<FileChunkData_t>();
            FileChunkData_t& chunkData = *pChunkData;
            if(chunkRes.error() != simdjson::error_code::SUCCESS) {
                ec = std::make_error_code(std::errc::invalid_argument);
                return nullptr;
            }

            strRes=chunkRes["HexName"].get_string();
            if (strRes.error() != simdjson::error_code::SUCCESS) {
                ec = std::make_error_code(std::errc::invalid_argument);
                return nullptr;
            }
            memcpy(chunkData.HexName, strRes.value_unsafe().data(), strRes.value_unsafe().size());
            chunkData.HexName[strRes.value_unsafe().size()] = '\0';

            u64Res = chunkRes["StartPos"].get_uint64();
            if (u64Res.error() != simdjson::error_code::SUCCESS) {
                ec = std::make_error_code(std::errc::invalid_argument);
                return nullptr;
            }
            chunkData.StartPos = u64Res.value_unsafe();
        }
    }

    return out;
}

int32_t FolderManifest_t::get_string_extra_space()
{
    return simdjson::SIMDJSON_PADDING;
}

std::shared_ptr<const FolderManifestCompareResult_t> CompareFolderManifest(const FolderManifest_t& source, const FolderManifest_t& target)
{
    auto out = std::make_shared<FolderManifestCompareResult_t>();
    auto& HexNames = out->MissingFileChunks;
    for (auto& [pathstr, FileChunksData] : target.Files) {
        for (auto& pFileChunk : FileChunksData->Chunks) {
            auto& FileChunk = *pFileChunk;
            HexNames.insert(FileChunk.HexName);
        }
    }
    for (auto& [pathstr, FileChunksData] : source.Files) {
        for (auto& pFileChunk : FileChunksData->Chunks) {
            auto& FileChunk = *pFileChunk;
            HexNames.erase(FileChunk.HexName);
        }
    }
    return out;
}
