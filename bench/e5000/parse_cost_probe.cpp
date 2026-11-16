#include "model.hpp"
#include "vector_literal.hpp"
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <iostream>
#include <random>

int main() {
    std::mt19937_64 random(2026111602);
    uint64_t checksum = 0;
    for (bool short_numbers : {true, false}) {
        std::vector<std::string> inputs;
        for (size_t row = 0; row < 128; ++row) {
            std::string text = "[";
            for (size_t d = 0; d < 128; ++d) {
                double value = short_numbers ? double(int(random()%4097)-2048)/128
                                             : double(float(double(int(random()%1000001)-500000)/500000));
                std::array<char, 64> buffer;
                auto r = std::to_chars(buffer.data(), buffer.data()+buffer.size(), value,
                                      std::chars_format::general, 17);
                if (r.ec != std::errc{}) throw std::runtime_error("format corpus");
                if (d) text += ',';
                text.append(buffer.data(), r.ptr);
            }
            text += ']'; inputs.push_back(std::move(text));
        }
        for (size_t round = 0; round < 8; ++round) {
            const auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < 10000; ++i) {
                auto values = mvd::parse_vector_literal(inputs[i%inputs.size()], 128, false);
                for (float value : values) checksum = (checksum << 1 | checksum >> 63) ^ std::bit_cast<uint32_t>(value);
            }
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            std::cout << "{\"short\":" << (short_numbers ? "true" : "false")
                      << ",\"round\":" << round << ",\"ns\":" << ns
                      << ",\"checksum\":" << checksum << "}\n";
        }
    }
}
