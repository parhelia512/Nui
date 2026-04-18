#include "temp_dir.hpp"

#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>
#include <stdexcept>

TempDir::TempDir()
    : path_{[]() {
        constexpr int maxAttempts = 32;
        std::mt19937_64 rng{static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
        for (int attempt = 0; attempt < maxAttempts; ++attempt)
        {
            std::ostringstream name;
            name << "nui_inline_parser_test_" << std::hex << rng();
            auto candidate = std::filesystem::temp_directory_path() / name.str();
            std::error_code errorCode;
            if (std::filesystem::create_directory(candidate, errorCode))
                return candidate;
        }
        throw std::runtime_error{"TempDir: failed to create unique directory"};
    }()}
{}
TempDir::~TempDir()
{
    std::filesystem::remove_all(path_);
}
std::filesystem::path const& TempDir::path() const
{
    return path_;
}