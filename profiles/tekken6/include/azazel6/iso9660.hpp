#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace azazel6::iso9660 {

inline constexpr std::size_t kSectorSize = 2048;
inline constexpr std::size_t kDefaultMaxFileBytes = 64u * 1024u * 1024u;

struct Entry {
    std::string name;
    std::uint32_t extent_lba = 0;
    std::uint32_t size = 0;
    bool directory = false;
};

class Image {
public:
    // The caller owns the backing bytes; they must outlive this Image.
    explicit Image(std::span<const std::uint8_t> bytes);

    [[nodiscard]] std::vector<Entry> list(std::string_view directory_path = {}) const;
    [[nodiscard]] Entry find(std::string_view path) const;
    [[nodiscard]] std::vector<std::uint8_t> read_file(
        std::string_view path,
        std::size_t max_bytes = kDefaultMaxFileBytes) const;

private:
    std::span<const std::uint8_t> bytes_;
    Entry root_;

    [[nodiscard]] std::vector<Entry> read_directory(const Entry& directory) const;
    [[nodiscard]] std::uint64_t byte_offset(const Entry& entry) const;
};

} // namespace azazel6::iso9660
