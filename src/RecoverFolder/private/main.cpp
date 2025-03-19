#include "ofilebackup_actions.h"
#include "FileBackupError.h"
#include <iostream>
#include <cxxopts.hpp>
#include <std_ext.h>
int main(int argc, const char* const* argv)
{
    EFileBackupError out(EFileBackupError::FBE_OPTION_ERROR);
    cxxopts::Options options("oRevcoverFolder", "recover folder to target manifest");
    options.positional_help("[work_path] [chunk_path] [target_manifest_path] ").show_positional_help();
    options.add_options()
        ("work_path", "target path", cxxopts::value<std::string>())
        ("target_manifest_path", "target manifest file path", cxxopts::value<std::string>())
        ("s,source_manifest_path", "source manifest file path", cxxopts::value<std::string>()->default_value(""))
        ("chunk_path", "where chunk files cached", cxxopts::value<std::string>())
        ("h,help", "print usage")

        ("o,temp_output_path", "chunk list output file path", cxxopts::value<std::string>()->default_value(std::string()))
        ;
    options.parse_positional({ "work_path","chunk_path","target_manifest_path" });
    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        goto options_error;
    }

    if (!result.count("work_path") || !result.count("target_manifest_path") || !result.count("target_manifest_path")) {
        goto options_error;
    }
    out = recover_folder((const char8_t*)result["work_path"].as<std::string>().c_str(),
        (const char8_t*)result["target_manifest_path"].as<std::string>().c_str(),
        (const char8_t*)result["source_manifest_path"].as<std::string>().c_str(),
        (const char8_t*)result["chunk_path"].as<std::string>().c_str(),
        (const char8_t*)result["temp_output_path"].as<std::string>().c_str()
    );

    exit(std::to_underlying(out));
options_error:
    std::cout << options.help() << std::endl;
    exit(std::to_underlying(out));
}