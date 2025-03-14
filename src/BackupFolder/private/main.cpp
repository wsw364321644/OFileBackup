#include "ofilebackup_actions.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cxxopts.hpp>
#include <assert.h>
int main(int argc,const char* const * argv)
{
    //_set_error_mode(_OUT_TO_MSGBOX);
    //assert(0);
    cxxopts::Options options("oBackupFolder", "input folder path to generate folder manifest and file chunks");
    options.positional_help("[path]").show_positional_help();
    options.add_options()
        ("path", "backup path", cxxopts::value<std::string>())
        ("h,help", "print usage")
        ("chunk_list_file_path", "file contain existing chunk name", cxxopts::value<std::string>()->default_value(std::string()))
        ("chunk_dir", "where chunk saved", cxxopts::value<std::string>()->default_value(std::string()))
        ("manifest_output_path", "manifest file output name path", cxxopts::value<std::string>()->default_value(std::string()))
        ;
    options.parse_positional({ "path" });
    auto result = options.parse(argc, argv);
    std::vector<std::string> hexNameList;

    if (result.count("help"))
    {
        goto options_error;
    }

    if (!result.count("path")) {
        goto options_error;
    }

    if (!gen_folder_manifest_action((const char8_t*)result["path"].as<std::string>().c_str(), 
        (const char8_t*)result["chunk_list_file_path"].as<std::string>().c_str(),
        (const char8_t*)result["chunk_dir"].as<std::string>().c_str(),
        (const char8_t*)result["manifest_output_path"].as<std::string>().c_str())
        ) {
        goto options_error;
    }

    exit(0);
options_error:
    std::cout << options.help() << std::endl;
    exit(-1);
}