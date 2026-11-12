// Sparse input I/O is outside every decoder timing interval.
// The sidecar reader retains one shot, never the complete event corpus.
#pragma once

#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace external_input {
inline uint64_t unsigned_integer(const nlohmann::json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::invalid_argument("Sparse input fields must be nonnegative integers");
    return value.get<uint64_t>();
}

inline uint64_t little_u64(const unsigned char* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}

inline uint64_t read_u64(std::ifstream& input) {
    std::array<unsigned char, 8> bytes{};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Truncated sparse sample sidecar");
    return little_u64(bytes.data());
}

class Samples {
    const nlohmann::json* inline_samples_ = nullptr;
    std::ifstream offsets_, events_;
    uint64_t shots_ = 0, total_events_ = 0, event_start_ = 0;
    uint64_t next_shot_ = 0, previous_offset_ = 0, detectors_;
    std::vector<unsigned char> bytes_;

   public:
    Samples(const nlohmann::json& payload, const std::filesystem::path& input_path, uint64_t detectors,
            bool warmup = false)
        : detectors_(detectors) {
        if (detectors == 0) throw std::invalid_argument("Empty detector graph");
        const char* storage_key = warmup ? "warmup_storage" : "sample_storage";
        const char* inline_key = warmup ? "warmup" : "samples";
        const char* blossom_key = warmup ? "blossom_warmup" : "blossom_samples";
        if (!payload.contains(storage_key)) {
            inline_samples_ = &payload.at(inline_key);
            if (!inline_samples_->is_array() || inline_samples_->empty())
                throw std::invalid_argument("Samples must be a nonempty array");
            shots_ = inline_samples_->size();
            return;
        }
        if (payload.contains(inline_key) || payload.contains(blossom_key))
            throw std::invalid_argument("Ambiguous inline and sidecar samples");
        const auto& storage = payload.at(storage_key);
        if (storage.at("format") != "bb-sparse-u64-v1")
            throw std::invalid_argument("Unsupported sparse sample format");
        const std::filesystem::path name(storage.at("file").get<std::string>());
        if (name.empty() || name.is_absolute() || name.filename() != name || name == "." || name == "..")
            throw std::invalid_argument("Sample sidecar must be a sibling filename");
        const auto path = input_path.parent_path() / name;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)) ||
            !std::filesystem::is_regular_file(path))
            throw std::invalid_argument("Sample sidecar must be a regular non-symlink file");
        offsets_.open(path, std::ios::binary);
        if (!offsets_) throw std::runtime_error("Cannot open sparse sample sidecar");
        if (read_u64(offsets_) != UINT64_C(0x4242535041525331) || read_u64(offsets_) != 1)
            throw std::invalid_argument("Invalid sparse sample magic or version");
        shots_ = read_u64(offsets_);
        total_events_ = read_u64(offsets_);
        if (!shots_ || shots_ != unsigned_integer(storage.at("shots")) ||
            total_events_ != unsigned_integer(storage.at("total_events")))
            throw std::invalid_argument("Sparse sample metadata differs from header");
        const auto limit = static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max());
        if (shots_ > (limit - 40) / 8 || shots_ > std::numeric_limits<size_t>::max())
            throw std::invalid_argument("Sparse sample offsets exceed file range");
        event_start_ = 40 + shots_ * 8;
        if (total_events_ > (limit - event_start_) / 8 ||
            std::filesystem::file_size(path) != event_start_ + total_events_ * 8)
            throw std::invalid_argument("Sparse sample file size differs from header");
        uint64_t previous = read_u64(offsets_);
        if (previous != 0) throw std::invalid_argument("First sparse sample offset must be zero");
        for (uint64_t i = 0; i < shots_; ++i) {
            const auto current = read_u64(offsets_);
            if (current < previous || current > total_events_ || current - previous > detectors_)
                throw std::invalid_argument("Invalid sparse sample offset or shot length");
            previous = current;
        }
        if (previous != total_events_) throw std::invalid_argument("Final sparse sample offset differs from event count");
        events_.open(path, std::ios::binary);
        if (!events_) throw std::runtime_error("Cannot open sparse sample events");
        offsets_.seekg(40);
        events_.seekg(static_cast<std::streamoff>(event_start_));
    }

    size_t size() const { return static_cast<size_t>(shots_); }

    template <typename T> void load(size_t shot, std::vector<T>& output) {
        if (shot >= shots_) throw std::out_of_range("Sparse sample index out of range");
        output.clear();
        auto append = [&](uint64_t detector) {
            if (detector >= detectors_ || detector > static_cast<uint64_t>(std::numeric_limits<T>::max()) ||
                (!output.empty() && detector <= static_cast<uint64_t>(output.back())))
                throw std::invalid_argument("Sample detector IDs must be bounded, sorted and unique");
            output.push_back(static_cast<T>(detector));
        };
        if (inline_samples_) {
            const auto& sample = inline_samples_->at(shot);
            if (!sample.is_array() || sample.size() > detectors_)
                throw std::invalid_argument("Invalid inline sample array");
            output.reserve(sample.size());
            for (const auto& value : sample) append(unsigned_integer(value));
            return;
        }
        if (shot != next_shot_) {
            offsets_.clear(); events_.clear();
            offsets_.seekg(static_cast<std::streamoff>(32 + uint64_t(shot) * 8));
            previous_offset_ = read_u64(offsets_);
            events_.seekg(static_cast<std::streamoff>(event_start_ + previous_offset_ * 8));
        }
        const auto end = read_u64(offsets_);
        if (end < previous_offset_ || end > total_events_ || end - previous_offset_ > detectors_)
            throw std::invalid_argument("Sparse sample offsets changed or are invalid");
        const auto length = end - previous_offset_;
        if (length > std::numeric_limits<size_t>::max() / 8)
            throw std::invalid_argument("Sparse shot exceeds addressable memory");
        bytes_.resize(static_cast<size_t>(length) * 8);
        if (!bytes_.empty() && !events_.read(reinterpret_cast<char*>(bytes_.data()), bytes_.size()))
            throw std::runtime_error("Truncated sparse sample events");
        output.reserve(static_cast<size_t>(length));
        for (size_t i = 0; i < bytes_.size(); i += 8) append(little_u64(bytes_.data() + i));
        previous_offset_ = end;
        next_shot_ = uint64_t(shot) + 1;
    }
};
}  // namespace external_input
