#include "evnet/csv.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace evnet::csv {
namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::istringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(trim(field));
    }
    return fields;
}

}  // namespace

std::size_t Row::indexOf(const std::string& column) const {
    const auto it = std::find(header_->begin(), header_->end(), column);
    if (it == header_->end()) {
        throw std::runtime_error("csv: no column '" + column + "'");
    }
    const auto index = static_cast<std::size_t>(std::distance(header_->begin(), it));
    if (index >= fields_.size()) {
        throw std::runtime_error("csv: line " + std::to_string(lineNumber_) + " is missing column '" +
                                 column + "' (row has " + std::to_string(fields_.size()) +
                                 " fields, expected " + std::to_string(header_->size()) + ")");
    }
    return index;
}

bool Row::has(const std::string& column) const {
    return std::find(header_->begin(), header_->end(), column) != header_->end();
}

const std::string& Row::str(const std::string& column) const { return fields_[indexOf(column)]; }

double Row::number(const std::string& column) const {
    const std::string& text = str(column);
    try {
        std::size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size()) throw std::invalid_argument("trailing characters");
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("csv: line " + std::to_string(lineNumber_) + ", column '" + column +
                                 "': '" + text + "' is not a number");
    }
}

int Row::integer(const std::string& column) const {
    const double value = number(column);
    const int truncated = static_cast<int>(value);
    if (static_cast<double>(truncated) != value) {
        throw std::runtime_error("csv: line " + std::to_string(lineNumber_) + ", column '" + column +
                                 "': expected an integer, got " + str(column));
    }
    return truncated;
}

bool Row::boolean(const std::string& column) const {
    const std::string& text = str(column);
    if (text == "1" || text == "true" || text == "True" || text == "yes") return true;
    if (text == "0" || text == "false" || text == "False" || text == "no") return false;
    throw std::runtime_error("csv: line " + std::to_string(lineNumber_) + ", column '" + column +
                             "': '" + text + "' is not a boolean");
}

Reader::Reader(const std::string& path) : path_(path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("csv: cannot open '" + path + "'");
    }

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        if (header_.empty()) {
            header_ = split(trimmed);
            continue;
        }
        rows_.emplace_back(&header_, split(trimmed), lineNumber);
    }

    if (header_.empty()) {
        throw std::runtime_error("csv: '" + path + "' contains no header row");
    }
}

void Reader::requireColumns(const std::vector<std::string>& columns) const {
    for (const auto& column : columns) {
        if (std::find(header_.begin(), header_.end(), column) == header_.end()) {
            throw std::runtime_error("csv: '" + path_ + "' is missing required column '" + column + "'");
        }
    }
}

std::string escape(const std::string& field) {
    if (field.find(',') == std::string::npos && field.find('"') == std::string::npos) {
        return field;
    }
    std::string out = "\"";
    for (const char c : field) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

}  // namespace evnet::csv
