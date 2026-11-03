#include "azazel6/iso9660.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace azazel6::iso9660 {
namespace {

[[noreturn]] void invalid(const char* message) {
    throw std::runtime_error(message);
}

std::uint16_t read_le16(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 2) invalid("Truncated ISO9660 16-bit field");
    return static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8);
}

std::uint32_t read_le32(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) invalid("Truncated ISO9660 32-bit field");
    return static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::uint16_t read_be16(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 2) invalid("Truncated ISO9660 16-bit field");
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) << 8) |
        static_cast<std::uint16_t>(data[offset + 1]);
}

std::uint32_t read_be32(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) invalid("Truncated ISO9660 32-bit field");
    return (static_cast<std::uint32_t>(data[offset]) << 24) |
        (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
        static_cast<std::uint32_t>(data[offset + 3]);
}

std::uint16_t both16(std::span<const std::uint8_t> data, std::size_t offset) {
    const auto little = read_le16(data, offset);
    const auto big = read_be16(data, offset + 2);
    if (little != big) invalid("Mismatched ISO9660 both-endian 16-bit field");
    return little;
}

std::uint32_t both32(std::span<const std::uint8_t> data, std::size_t offset) {
    const auto little = read_le32(data, offset);
    const auto big = read_be32(data, offset + 4);
    if (little != big) invalid("Mismatched ISO9660 both-endian 32-bit field");
    return little;
}

bool range_fits(std::size_t total, std::uint64_t offset, std::uint64_t length) {
    return offset <= total && length <= static_cast<std::uint64_t>(total) - offset;
}

Entry parse_record(std::span<const std::uint8_t> record, std::size_t image_size) {
    if (record.size() < 34) invalid("ISO9660 directory record is too short");
    const auto record_length = record[0];
    const auto name_length = record[32];
    if (record_length != record.size() || name_length == 0 ||
        static_cast<std::size_t>(33 + name_length) > record.size()) {
        invalid("Malformed ISO9660 directory record length");
    }
    if (record[25] & 0x80u) invalid("Multi-extent ISO9660 files are not supported yet");

    Entry entry;
    entry.extent_lba = both32(record, 2);
    entry.size = both32(record, 10);
    entry.directory = (record[25] & 0x02u) != 0;

    const auto offset = static_cast<std::uint64_t>(entry.extent_lba) * kSectorSize;
    if (!range_fits(image_size, offset, entry.size)) invalid("ISO9660 extent exceeds image bounds");

    if (name_length == 1 && (record[33] == 0 || record[33] == 1)) {
        entry.name = record[33] == 0 ? "." : "..";
        return entry;
    }

    entry.name.reserve(name_length);
    for (std::size_t i = 0; i < name_length; ++i) {
        const auto ch = record[33 + i];
        if (ch < 0x20 || ch > 0x7e || ch == '/' || ch == '\\' || ch == 0) {
            invalid("Unsupported character in ISO9660 identifier");
        }
        entry.name.push_back(static_cast<char>(ch));
    }
    const auto version = entry.name.find(';');
    if (version != std::string::npos) entry.name.resize(version);
    if (!entry.name.empty() && entry.name.back() == '.') entry.name.pop_back();
    if (entry.name.empty()) invalid("Empty ISO9660 identifier");
    return entry;
}

std::string normalize_component(std::string_view component) {
    if (component.empty() || component == ".") return {};
    if (component == "..") invalid("Parent traversal is not allowed in ISO9660 paths");
    std::string result;
    result.reserve(component.size());
    for (const unsigned char ch : component) {
        if (ch < 0x20 || ch > 0x7e || ch == '\\' || ch == ':') {
            invalid("Invalid character in ISO9660 path");
        }
        result.push_back(static_cast<char>(std::toupper(ch)));
    }
    return result;
}

bool name_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(left[i])) !=
            std::toupper(static_cast<unsigned char>(right[i]))) return false;
    }
    return true;
}

std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && path[start] == '/') ++start;
        if (start == path.size()) break;
        const auto end = path.find('/', start);
        const auto length = (end == std::string_view::npos ? path.size() : end) - start;
        auto part = normalize_component(path.substr(start, length));
        if (!part.empty()) parts.push_back(std::move(part));
        start = end == std::string_view::npos ? path.size() : end + 1;
    }
    if (parts.size() > 64) invalid("ISO9660 path exceeds maximum depth");
    return parts;
}

} // namespace

Image::Image(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
    if (bytes_.size() < 17 * kSectorSize) invalid("ISO9660 image is too small for a volume descriptor");
    constexpr std::size_t descriptor_limit = 64;
    bool found_primary = false;
    std::span<const std::uint8_t> primary;

    for (std::size_t index = 16; index < 16 + descriptor_limit; ++index) {
        const auto offset = index * kSectorSize;
        if (!range_fits(bytes_.size(), offset, kSectorSize)) invalid("Truncated ISO9660 volume descriptor");
        const auto descriptor = bytes_.subspan(offset, kSectorSize);
        if (descriptor[1] != 'C' || descriptor[2] != 'D' ||
            descriptor[3] != '0' || descriptor[4] != '0' || descriptor[5] != '1' ||
            descriptor[6] != 1) invalid("Invalid ISO9660 volume descriptor signature");
        if (descriptor[0] == 1) {
            primary = descriptor;
            found_primary = true;
            break;
        }
        if (descriptor[0] == 255) break;
    }
    if (!found_primary) invalid("ISO9660 primary volume descriptor not found");
    if (both16(primary, 128) != kSectorSize) invalid("Unsupported ISO9660 logical block size");

    const auto root_length = primary[156];
    if (root_length < 34 || root_length > 255) invalid("Invalid ISO9660 root directory record");
    root_ = parse_record(primary.subspan(156, root_length), bytes_.size());
    if (!root_.directory) invalid("ISO9660 root record is not a directory");
    root_.name = "/";
}

std::uint64_t Image::byte_offset(const Entry& entry) const {
    return static_cast<std::uint64_t>(entry.extent_lba) * kSectorSize;
}

std::vector<Entry> Image::read_directory(const Entry& directory) const {
    if (!directory.directory) invalid("ISO9660 path component is not a directory");
    const auto offset = byte_offset(directory);
    if (!range_fits(bytes_.size(), offset, directory.size)) invalid("ISO9660 directory exceeds image bounds");
    const auto data = bytes_.subspan(static_cast<std::size_t>(offset), directory.size);
    std::vector<Entry> entries;
    std::size_t cursor = 0;
    while (cursor < data.size()) {
        const auto record_length = data[cursor];
        if (record_length == 0) {
            const auto next_sector = ((cursor / kSectorSize) + 1) * kSectorSize;
            cursor = std::min(next_sector, data.size());
            continue;
        }
        if (record_length < 34 || record_length > data.size() - cursor ||
            (cursor % kSectorSize) + record_length > kSectorSize) {
            invalid("Malformed or cross-sector ISO9660 directory record");
        }
        auto entry = parse_record(data.subspan(cursor, record_length), bytes_.size());
        if (entry.name != "." && entry.name != "..") entries.push_back(std::move(entry));
        cursor += record_length;
    }
    return entries;
}

std::vector<Entry> Image::list(std::string_view directory_path) const {
    const auto parts = split_path(directory_path);
    Entry current = root_;
    std::unordered_set<std::uint64_t> visited;
    visited.insert((static_cast<std::uint64_t>(current.extent_lba) << 32) | current.size);
    for (const auto& part : parts) {
        const auto children = read_directory(current);
        const auto found = std::find_if(children.begin(), children.end(),
            [&](const Entry& item) { return name_equal(item.name, part); });
        if (found == children.end()) invalid("ISO9660 path not found");
        if (!found->directory) invalid("ISO9660 path component is not a directory");
        const auto key = (static_cast<std::uint64_t>(found->extent_lba) << 32) | found->size;
        if (!visited.insert(key).second) invalid("ISO9660 directory cycle detected");
        current = *found;
    }
    return read_directory(current);
}

Entry Image::find(std::string_view path) const {
    const auto parts = split_path(path);
    if (parts.empty()) return root_;
    Entry current = root_;
    std::unordered_set<std::uint64_t> visited;
    visited.insert((static_cast<std::uint64_t>(current.extent_lba) << 32) | current.size);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto children = read_directory(current);
        const auto found = std::find_if(children.begin(), children.end(),
            [&](const Entry& item) { return name_equal(item.name, parts[i]); });
        if (found == children.end()) invalid("ISO9660 path not found");
        current = *found;
        if (i + 1 < parts.size()) {
            if (!current.directory) invalid("ISO9660 path component is not a directory");
            const auto key = (static_cast<std::uint64_t>(current.extent_lba) << 32) | current.size;
            if (!visited.insert(key).second) invalid("ISO9660 directory cycle detected");
        }
    }
    return current;
}

std::vector<std::uint8_t> Image::read_file(std::string_view path, std::size_t max_bytes) const {
    const auto entry = find(path);
    if (entry.directory) invalid("Cannot read an ISO9660 directory as a file");
    if (entry.size > max_bytes) invalid("ISO9660 file exceeds configured read limit");
    const auto offset = byte_offset(entry);
    if (!range_fits(bytes_.size(), offset, entry.size)) invalid("ISO9660 file exceeds image bounds");
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return {begin, begin + static_cast<std::ptrdiff_t>(entry.size)};
}

} // namespace azazel6::iso9660
