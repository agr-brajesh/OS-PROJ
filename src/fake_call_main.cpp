#include "logger.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n\n"
              << "Options:\n"
              << "  -d, --device <dev>    Path to webcam device (default: /dev/video10)\n"
              << "  -f, --fps <val>       Target frames per second (default: 30)\n"
              << "  -t, --duration <sec>  Run duration in seconds (default: 10)\n"
              << "  -o, --output <file>   Path to output CSV metrics file\n"
              << "  -h, --help            Show this help message and exit\n";
}

int main(int argc, char* argv[]) {
    std::string device = "/dev/video10";
    int fps = 30;
    int duration_sec = 10;
    std::string output_csv = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-d" || arg == "--device") {
            if (i + 1 < argc) {
                device = argv[++i];
            } else {
                std::cerr << "Error: --device requires a device path\n";
                return 1;
            }
        } else if (arg == "-f" || arg == "--fps") {
            if (i + 1 < argc) {
                fps = std::stoi(argv[++i]);
            }
        } else if (arg == "-t" || arg == "--duration") {
            if (i + 1 < argc) {
                duration_sec = std::stoi(argv[++i]);
            }
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                output_csv = argv[++i];
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    ISP_LOG_INFO("Fake Video Call starting session:");
    ISP_LOG_INFO("  device:   " << device);
    ISP_LOG_INFO("  fps:      " << fps);
    ISP_LOG_INFO("  duration: " << duration_sec << "s");

    // Open video device and hold descriptor open for entire session
    int fd = open(device.c_str(), O_RDWR);
    if (fd < 0) {
        fd = open(device.c_str(), O_RDONLY);
    }

    if (fd >= 0) {
        ISP_LOG_INFO("Opened device " << device << " (fd: " << fd << "). Session active.");
    } else {
        ISP_LOG_WARN("Could not open " << device << ": " << strerror(errno));
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(duration_sec);
    double frame_interval_ms = 1000.0 / static_cast<double>(fps);

    int total_frames = 0;
    auto next_frame = start_time;

    while (std::chrono::steady_clock::now() < end_time) {
        next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(frame_interval_ms));
        
        ++total_frames;

        // Sleep until next frame boundary
        std::this_thread::sleep_until(next_frame);
    }

    if (fd >= 0) {
        close(fd);
        ISP_LOG_INFO("Closed device " << device << ". Session ended.");
    }

    ISP_LOG_INFO("Fake Video Call session completed (" << total_frames << " frames processed).");
    return 0;
}
