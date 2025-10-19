//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_COMPILE_H
#define LUAXE_COMPILE_H

#include "src/lua/lef.h"

static void parse_and_compile(std::ostream& out, int i) {
    if (__argc <= i + 1) {
        out << "No source provided, try \"luaxe help compile\"." << std::endl;
        return;
    }
    bool strip = false, pack = false, verbose = false;
    std::string source, output, main;
    std::vector<std::string> args;
    bool found_args = false;
    source = __argv[++i];
    for (int j = ++i; j < __argc; j++) {
        if (!found_args && __argv[j][0] == '-') {
            if (__argv[j][1] == 'o') {
                output = __argv[j + 1];
                j++;
                if (verbose) out << "Output file: " << output << std::endl;
            } else if (__argv[j][1] == 's') {
                strip = true;
                if (verbose) out << "Stripping debug info" << std::endl;
            } else if (__argv[j][1] == 'l') {
                pack = true;
                if (verbose) out << "Packing enabled" << std::endl;
            } else if (__argv[j][1] == 'v') {
                verbose = true;
                out << "Verbose enabled" << std::endl;
            } else if (__argv[j][1] == 'm') {
                main = __argv[j + 1];
                j++;
                if (verbose) out << "Main file: " << main << std::endl;
            } else if (__argv[j][1] == '-') {
                found_args = true;
                if (verbose) out << "Arguments: ";
            }
        } else {
            args.emplace_back(__argv[j]);
            if (verbose) { if (args.size() == 1) out << "Args: " << __argv[j] << " "; else out << __argv[j] << " "; }
        }
    }
    if (verbose) out << std::endl;
    if (main.empty()) {
        main = "main.lua";
        if (verbose) out << "Main file: " << main << std::endl;
    }
    if (output.empty()) {
        std::filesystem::path p = source;
        if (p.extension() == ".lua") {
            output = source.substr(0, source.size() - 4);
        } else if (std::filesystem::is_directory(p)) {
            output = p.parent_path().filename().string();
            if (output.empty()) output = "main";
        } else {
            output = source;
        }
        output += pack ? ".lef" : ".exe";
        if (verbose) out << "Output file: " << output << std::endl;
    }
    if (verbose) out << "Source: " << source << std::endl;
    LefFile::store_as_lef(output, source, main, args, strip, verbose);
}

#endif //LUAXE_COMPILE_H
