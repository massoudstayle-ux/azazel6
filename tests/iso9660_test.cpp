#include "azazel6/iso9660.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using azazel6::iso9660::Image;
using azazel6::iso9660::kSectorSize;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put_both16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<std::uint8_t>(value);
    bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[at + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[at + 3] = static_cast<std::uint8_t>(value);
}

void put_both32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    for (unsigned i = 0; i < 4; ++i) bytes[at + 4 + i] = static_cast<std::uint8_t>(value >> (8 * (3 - i)));
}

std::vector<std::uint8_t> record(std::uint32_t lba, std::uint32_t size,
                                 std::uint8_t flags, std::string name) {
    const auto name_length = name.size();
    const auto length = 33 + name_length + ((name_length % 2) == 0 ? 1 : 0);
    std::vector<std::uint8_t> out(length);
    out[0] = static_cast<std::uint8_t>(length);
    put_both32(out, 2, lba);
    put_both32(out, 10, size);
    out[25] = flags;
    put_both16(out, 28, kSectorSize);
    out[32] = static_cast<std::uint8_t>(name_length);
    std::copy(name.begin(), name.end(), out.begin() + 33);
    return out;
}

std::vector<std::uint8_t> make_image() {
    std::vector<std::uint8_t> image(24 * kSectorSize);
    const auto pvd_offset = 16 * kSectorSize;
    image[pvd_offset] = 1;
    image[pvd_offset + 1] = 'C'; image[pvd_offset + 2] = 'D';
    image[pvd_offset + 3] = '0'; image[pvd_offset + 4] = '0';
    image[pvd_offset + 5] = '1'; image[pvd_offset + 6] = 1;
    put_both16(image, pvd_offset + 128, static_cast<std::uint16_t>(kSectorSize));
    const auto root = record(20, kSectorSize, 2, std::string(1, '\0'));
    std::copy(root.begin(), root.end(), image.begin() + static_cast<std::ptrdiff_t>(pvd_offset + 156));
    return image;
}

std::vector<std::uint8_t> fixture() {
    auto image = make_image();
    auto root_sector = std::span<std::uint8_t>(image).subspan(20 * kSectorSize, kSectorSize);
    std::size_t cursor = 0;
    const auto dot = record(20, kSectorSize, 2, std::string(1, '\0'));
    const auto dotdot = record(20, kSectorSize, 2, std::string(1, '\1'));
    const auto subdir = record(21, kSectorSize, 2, "DATA");
    const auto file = record(22, 5, 0, "HELLO.TXT;1");
    for (const auto* item : {&dot, &dotdot, &subdir, &file}) {
        std::copy(item->begin(), item->end(), root_sector.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += item->size();
    }

    auto child_sector = std::span<std::uint8_t>(image).subspan(21 * kSectorSize, kSectorSize);
    cursor = 0;
    const auto child_dot = record(21, kSectorSize, 2, std::string(1, '\0'));
    const auto child_dotdot = record(20, kSectorSize, 2, std::string(1, '\1'));
    for (const auto* item : {&child_dot, &child_dotdot}) {
        std::copy(item->begin(), item->end(), child_sector.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += item->size();
    }

    const std::array<std::uint8_t, 5> payload{'h', 'e', 'l', 'l', 'o'};
    std::copy(payload.begin(), payload.end(), image.begin() + 22 * kSectorSize);
    return image;
}

template<class Fn> void must_throw(Fn&& fn, const char* message) {
    bool threw = false;
    try { fn(); } catch (const std::exception&) { threw = true; }
    require(threw, message);
}

void happy_path() {
    auto bytes = fixture();
    Image image(bytes);
    const auto entries = image.list();
    require(entries.size() == 2, "Root listing count");
    require(entries[0].directory && entries[0].name == "DATA", "Directory entry");
    require(entries[1].name == "HELLO.TXT", "Version suffix normalization");
    const auto found = image.find("/data/");
    require(found.directory, "Case-insensitive directory lookup");
    const auto payload = image.read_file("hello.txt");
    require(std::string(payload.begin(), payload.end()) == "hello", "File payload");
    require(image.list("DATA").empty(), "Empty child directory");
}

void reject_bad_inputs() {
    auto bytes = fixture();
    must_throw([&] { Image truncated(std::span<const std::uint8_t>(bytes).first(10)); },
               "Truncated image accepted");

    auto bad_signature = bytes;
    bad_signature[16 * kSectorSize + 1] = 'X';
    must_throw([&] { Image image(bad_signature); }, "Bad descriptor signature accepted");

    auto bad_block_size = bytes;
    put_both16(bad_block_size, 16 * kSectorSize + 128, 1024);
    must_throw([&] { Image image(bad_block_size); }, "Unsupported block size accepted");

    auto bad_both_endian = bytes;
    bad_both_endian[16 * kSectorSize + 130] ^= 1;
    must_throw([&] { Image image(bad_both_endian); }, "Mismatched endian field accepted");

    auto bad_root_extent = bytes;
    put_both32(bad_root_extent, 16 * kSectorSize + 158, 999);
    must_throw([&] { Image image(bad_root_extent); }, "Out-of-bounds root extent accepted");

    auto multi_extent = bytes;
    multi_extent[20 * kSectorSize + 34 + 34 + 38 + 25] |= 0x80;
    must_throw([&] { Image image(multi_extent).list(); }, "Multi-extent file accepted");

    Image image(bytes);
    must_throw([&] { (void)image.find("../secret"); }, "Parent traversal accepted");
    must_throw([&] { (void)image.find("DATA/NOPE"); }, "Missing path accepted");
    must_throw([&] { (void)image.read_file("DATA"); }, "Directory read as file accepted");
    must_throw([&] { (void)image.read_file("HELLO.TXT", 4); }, "Read limit ignored");
}

} // namespace

int main() {
    try {
        happy_path();
        reject_bad_inputs();
        std::cout << "ISO9660 synthetic tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
