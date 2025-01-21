#pragma once
#include <string>
#include <vector>
bool gen_folder_manifest_action(std::u8string_view workPath, std::u8string_view chunkListPathStr, std::u8string_view chunkOutPathStr, std::u8string_view manifestOutPathStr);
bool compare_folder_manifest(std::u8string_view sourcePath, std::u8string_view targetPath, std::u8string_view outFilePathStr);
bool recover_folder(std::u8string_view workPathStr, std::u8string_view manifestFilePathStr, std::u8string_view sourceManifestFilePathStr, std::u8string_view chunkPathStr, std::u8string_view tempPathStr);
