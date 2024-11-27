#include <iostream>
#include <string>
#include <getopt.h>
#include "slicer.h"

int main(int argc, char *argv[]) {
    std::string filename;
    std::string save_dir = "./";  // Default save directory
    int processing_sr = 16000;   // Default sample rate
    
    int opt;
    int profile = 1;
    while ((opt = getopt(argc, argv, "i:s:sr:p")) != -1) {
        switch (opt) {
            case 'i':
                filename = optarg;
                break;
            case 's':
                save_dir = optarg;
                break;
            case 'r':
                processing_sr = std::stoi(optarg);
                break;
            case 'p':
                profile = 10;
                std::cout << "Running program " << profile << " times to profile code" << std::endl;
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " -i <filename> -s <save_dir> -sr <samplerate> -p <profile>" << std::endl;
                return 1;
        }
    }

    if (filename.empty()) {
        std::cerr << "Error: Input filename is required!" << std::endl;
        return 1;
    }

    for (int i = 0; i < profile; i++){
        try {
            AudioSegmentor segmentor(filename.c_str(), save_dir, processing_sr);

            // Split vocals
            if (segmentor.split_vocals()) {
                std::cout << "Vocal splitting completed successfully!" << std::endl;
            } else {
                std::cerr << "Vocal splitting failed." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
    }

    return 0;
}
