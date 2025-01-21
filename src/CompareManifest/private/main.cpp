#include "ofilebackup_actions.h"
#include <iostream>
#include <cxxopts.hpp>

int main(int argc, const char* const* argv)
{
    cxxopts::Options options("oCompareManifest", "compare two folder manifest to get needed chunk");
    options.positional_help("[source_manifest_path] [target_manifest_path]").show_positional_help();
    options.add_options()
        ("h,help", "print usage")
        ("target_manifest_path", "target manifest path", cxxopts::value<std::string>())
        ("source_manifest_path", "source manifest path", cxxopts::value<std::string>())
        ("o,chunk_output_path", "chunk list output file path", cxxopts::value<std::string>()->default_value(std::string()))
        ;
    options.parse_positional({ "source_manifest_path","target_manifest_path"});
    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        goto options_error;
    }

    if (!result.count("source_manifest_path")|| !result.count("target_manifest_path")) {
        goto options_error;
    }
    if (!compare_folder_manifest((const char8_t*)result["source_manifest_path"].as<std::string>().c_str(),
        (const char8_t*)result["target_manifest_path"].as<std::string>().c_str(),
        (const char8_t*)result["chunk_output_path"].as<std::string>().c_str())
        ) {
        goto options_error;
    }

    exit(0);
options_error:
    std::cout << options.help() << std::endl;
    exit(-1);
file_not_exist:
    exit(-2);
}