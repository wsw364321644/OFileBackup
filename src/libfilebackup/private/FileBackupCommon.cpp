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
    rapidjson::Value filesNode{ rapidjson::kObjectType };
    auto& a = doc.GetAllocator();
    for (auto& [fileName, fileData] : FolderManifest.Files) {
        rapidjson::Value fileNode{ rapidjson::kObjectType };
        rapidjson::Value fileChunksNode{ rapidjson::kArrayType };
        fileData->FileHash[StrongHashBit / 4] = 0;
        for (auto& pChunk : fileData->Chunks) {
            rapidjson::Value fileChunkNode{ rapidjson::kObjectType };
            auto& chunk = *pChunk;
            fileChunkNode.AddMember("hexName", rapidjson::StringRef(chunk.HexName), a);
            fileChunkNode.AddMember("startPos", chunk.StartPos, a);
            fileChunksNode.PushBack(fileChunkNode, a);
        }
        fileNode.AddMember("fileHash", rapidjson::StringRef(fileData->FileHash), a);
        fileNode.AddMember("fileSize", fileData->FileSize, a);
        if (fileData->Chunks.size() > 0) {
            fileNode.AddMember("chunks", fileChunksNode, a);
        }
        filesNode.AddMember(rapidjson::StringRef(fileData->FileName.c_str()), fileNode, a);
    }
    doc.AddMember("id", rapidjson::StringRef(FolderManifest.ID, bin_to_hex_length(UUID_128_BYTES)), a);
    doc.AddMember("chunkFileMaxSize",FolderManifest.ChunkFileMaxSize,a);
    doc.AddMember("hexNameLen", FolderManifest.HexNameLen, a);
    doc.AddMember("files", filesNode, a);
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

    auto strRes = rootRes["id"].get_string();
    if (strRes.error() != simdjson::error_code::SUCCESS) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }
    if (strRes.value_unsafe().size() >= sizeof(manifest.ID)) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }
    memcpy(manifest.ID, strRes.value_unsafe().data(), strRes.value_unsafe().size());

    auto u64Res= rootRes["chunkFileMaxSize"].get_uint64();
    if (u64Res.error() != simdjson::error_code::SUCCESS) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }
    manifest.ChunkFileMaxSize = u64Res.value_unsafe();

    u64Res = rootRes["hexNameLen"].get_uint64();
    if (u64Res.error() != simdjson::error_code::SUCCESS) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }
    manifest.HexNameLen = u64Res.value_unsafe();

    auto filesRes = rootRes["files"].get_object();
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

        strRes=fileRes["fileHash"].get_string();
        if(strRes.error() != simdjson::error_code::SUCCESS) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }
        memcpy(FileChunksData.FileHash ,strRes.value_unsafe().data(), strRes.value_unsafe().size());
        FileChunksData.FileHash[strRes.value_unsafe().size()] = '\0';

        u64Res = fileRes["fileSize"].get_uint64();
        if (u64Res.error() != simdjson::error_code::SUCCESS) {
            ec = std::make_error_code(std::errc::invalid_argument);
            return nullptr;
        }
        FileChunksData.FileSize = u64Res.value_unsafe();

        auto chunksRes = fileRes["chunks"].get_array();
        if (chunksRes.error() == simdjson::error_code::SUCCESS) {
            for (auto chunkRes : chunksRes) {
                auto pChunkData = std::make_shared<FileChunkData_t>();
                FileChunkData_t& chunkData = *pChunkData;
                if (chunkRes.error() != simdjson::error_code::SUCCESS) {
                    ec = std::make_error_code(std::errc::invalid_argument);
                    return nullptr;
                }

                strRes = chunkRes["hexName"].get_string();
                if (strRes.error() != simdjson::error_code::SUCCESS) {
                    ec = std::make_error_code(std::errc::invalid_argument);
                    return nullptr;
                }
                memcpy(chunkData.HexName, strRes.value_unsafe().data(), strRes.value_unsafe().size());
                chunkData.HexName[strRes.value_unsafe().size()] = '\0';

                u64Res = chunkRes["startPos"].get_uint64();
                if (u64Res.error() != simdjson::error_code::SUCCESS) {
                    ec = std::make_error_code(std::errc::invalid_argument);
                    return nullptr;
                }
                chunkData.StartPos = u64Res.value_unsafe();
                FileChunksData.Chunks.emplace(pChunkData);
            }
        }
    }

    return out;
}

int32_t FolderManifest_t::get_string_extra_space()
{
    return simdjson::SIMDJSON_PADDING;
}

std::shared_ptr<const FolderManifestCompareResult_t> CompareFolderManifest(const FolderManifest_t& target, std::shared_ptr<const FolderManifest_t> source) {

    auto out = std::make_shared<FolderManifestCompareResult_t>();
    if (!out) {
        return nullptr;
    }
    // 1. 构建源manifest中所有已知的chunk名称集合

    if (source) {
        for (const auto& [pathstr, FileChunksData] : source->Files) {
            out->FilesNeedDelete.emplace(pathstr);
            for (auto& pFileChunk : FileChunksData->Chunks) {
                auto& FileChunk = *pFileChunk;

                auto res=out->SourceChunkReverseIndex.try_emplace(GetHexNameView( FileChunk.HexName));
                res.first->second.ChunkInFileData.emplace_back( FileChunksData , pFileChunk);
            }
        }
    }

    // 2. 对目标manifest中的每个文件进行处理
    for (const auto& [target_filename, target_file_data] : target.Files) {
        if (source) {
            out->FilesNeedDelete.erase(target_filename);
            auto itr=source->Files.find(target_filename);
            if (itr!=source->Files.end()&&memcmp(itr->second->FileHash, target_file_data->FileHash, FileHashLen) == 0) {
                continue;
            }
        }
        // 获取目标文件的所有chunks，它们已经是有序的
        std::vector<std::shared_ptr<FileChunkData_t>> all_target_chunks;
        for (const auto& chunk_ptr : target_file_data->Chunks) {
            all_target_chunks.push_back(chunk_ptr);

            auto res = out->TargetChunkReverseIndex.try_emplace(GetHexNameView(chunk_ptr->HexName));
            res.first->second.ChunkInFileData.emplace_back(target_file_data, chunk_ptr);
        }

        if (all_target_chunks.empty()) {
            out->FileConstructChunks.try_emplace(target_filename);
            continue;
        }

        // 整个文件区间
        uint64_t file_start = all_target_chunks.front()->StartPos;
        uint64_t file_end = all_target_chunks.back()->StartPos + FileChunkSize;

        // 使用贪心算法选择最优的chunk组合来覆盖整个文件
        std::set<std::shared_ptr<FileConstructChunkData_t>, FileConstructChunkDataLess_t,
            allocator_save_memory_operator<std::shared_ptr<FileConstructChunkData_t>>> needed_chunks;

        uint64_t current_pos = file_start;

        while (current_pos < file_end) {
            // 找出所有能覆盖当前位置的chunks
            std::vector<std::shared_ptr<FileChunkData_t>> candidates;

            for (const auto& chunk_ptr : all_target_chunks) {
                uint64_t chunk_start = chunk_ptr->StartPos;
                uint64_t chunk_end = chunk_start + FileChunkSize;

                if (chunk_start <= current_pos && chunk_end > current_pos) {
                    // 这个chunk能覆盖当前位置
                    candidates.push_back(chunk_ptr);
                }
                else if (chunk_start > current_pos && !candidates.empty()) {
                    // 如果已经找到了覆盖当前位置的chunks，并且遇到不重叠的chunk，
                    // 由于chunks是按位置排序的，后面的chunks也不会覆盖当前位置
                    break;
                }
            }

            if (candidates.empty()) {
                // 理论上不会发生，因为chunks应该是连续的
                break;
            }

            // 从中选择最优的一个：优先选择源中存在的，其次选择覆盖范围更广的
            std::shared_ptr<FileChunkData_t> best_chunk = nullptr;
            uint64_t best_end = current_pos;
            bool best_from_source = false;

            for (const auto& chunk_ptr : candidates) {
                uint64_t chunk_end = chunk_ptr->StartPos + FileChunkSize;
                bool is_from_source = out->SourceChunkReverseIndex.count(GetHexNameView(chunk_ptr->HexName));

                // 优先选择源中存在的，如果都是同类型则选择覆盖范围更广的
                bool should_update = false;
                if (!best_chunk) {
                    should_update = true;
                }
                else if (is_from_source && !best_from_source) {
                    should_update = true;
                }
                else if (is_from_source == best_from_source && chunk_end > best_end) {
                    should_update = true;
                }

                if (should_update) {
                    best_chunk = chunk_ptr;
                    best_end = chunk_end;
                    best_from_source = is_from_source;
                }
            }

            if (!best_chunk) {
                break;
            }

            // 添加选中的chunk

            if (best_from_source) {
                // 这个chunk在源中存在
                //out->SourceChunkReverseIndex[chunk_name] = { target_filename, std::cref(best_chunk->StartPos) };
                auto construct_chunk = std::make_shared<FileConstructChunkData_t>(best_chunk ,true );
                needed_chunks.insert(construct_chunk);
            }
            else {
                // 这个chunk在源中不存在，需要获取
                out->MissingFileChunks.insert(GetHexNameView(best_chunk->HexName));
                auto construct_chunk = std::make_shared<FileConstructChunkData_t>(best_chunk,false);
                needed_chunks.insert(construct_chunk);
            }

            // 更新当前覆盖位置
            current_pos = best_end;
        }

        if (!needed_chunks.empty()) {
            out->FileConstructChunks[target_filename] = std::move(needed_chunks);
        }

    }

    return out;
}