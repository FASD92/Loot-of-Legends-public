#include "Util/PrometheusTextfileWriter.hpp"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {
bool isMetricNameStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) || value == '_' || value == ':';
}

bool isMetricNameChar(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) || value == '_' || value == ':';
}

bool isLabelNameStart(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalpha(ch) || value == '_';
}

bool isLabelNameChar(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) || value == '_';
}

void validateName(
    const std::string& name,
    const char* kind,
    bool (*isStart)(char),
    bool (*isChar)(char)) {
    if (name.empty() || !isStart(name.front())) {
        throw std::invalid_argument(std::string(kind) + " has invalid name: " + name);
    }
    for (char value : name) {
        if (!isChar(value)) {
            throw std::invalid_argument(
                std::string(kind) + " has invalid name: " + name);
        }
    }
}

std::string escapeLabelValue(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\') {
            escaped += "\\\\";
        } else if (ch == '"') {
            escaped += "\\\"";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

void setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}
}  // namespace

namespace Util {
std::string renderPrometheusTextfile(
    const std::vector<PrometheusMetricSample>& samples) {
    std::ostringstream output;
    output << std::setprecision(17);

    for (const PrometheusMetricSample& sample : samples) {
        validateName(sample.name, "metric", isMetricNameStart, isMetricNameChar);
        output << sample.name;

        if (!sample.labels.empty()) {
            output << '{';
            for (size_t index = 0; index < sample.labels.size(); ++index) {
                const PrometheusMetricLabel& label = sample.labels[index];
                validateName(label.name, "label", isLabelNameStart, isLabelNameChar);
                if (index > 0) {
                    output << ',';
                }
                output << label.name << "=\"" << escapeLabelValue(label.value) << '"';
            }
            output << '}';
        }

        output << ' ' << sample.value << '\n';
    }

    return output.str();
}

bool writePrometheusTextfileAtomically(
    const std::string& targetPath,
    const std::string& content,
    std::string* errorMessage) {
    const std::string tempPath = targetPath + ".tmp";
    std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        setError(errorMessage, "failed to open temp metrics file: " + tempPath);
        return false;
    }

    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        setError(errorMessage, "failed to write temp metrics file: " + tempPath);
        std::remove(tempPath.c_str());
        return false;
    }

    if (std::rename(tempPath.c_str(), targetPath.c_str()) != 0) {
        setError(
            errorMessage,
            "failed to rename temp metrics file: " + std::string(std::strerror(errno)));
        std::remove(tempPath.c_str());
        return false;
    }

    setError(errorMessage, "");
    return true;
}
}  // namespace Util
