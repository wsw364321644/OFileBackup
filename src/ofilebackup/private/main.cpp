#include "ofilebackup_actions.h"

#include <iostream>

#include <cxxopts.hpp>

template<typename T>
struct Foo
{
    T bar;
};

//template<typename T>
//Foo(T) -> Foo<std::decay_t<T>>;

void call_lambda(auto&& lambda)
{
    lambda();
}

int main(int argc,const char* const * argv)
{
    cxxopts::Options options("OFileBackup", "");
    options.positional_help("[Action]").show_positional_help();;
    options.add_options()
        ("action", "Action to do", cxxopts::value<std::string>(), "GenFolderManifest")
        ("h,help", "Print usage")
        ("p,path", "Work path", cxxopts::value<std::string>())
        ("o,output", "Output path", cxxopts::value<std::string>())
        ;
    options.parse_positional({ "action" });
    auto result = options.parse(argc, argv);
    std::string actionStr;
    if (result.count("help")|| !result.count("action"))
    {
        goto options_error;
    }
    actionStr = result["action"].as<std::string>();
    if(actionStr =="GenFolderManifest") {
        if (!result.count("path")) {
            goto options_error;
        }
        gen_folder_manifest_action(result["path"].as<std::string>(), result.count("output")?result["output"].as<std::string>(): std::string());
    }
    else {
        goto options_error;
    }
    exit(0);
options_error:
    std::cout << options.help() << std::endl;
    exit(-1);
}