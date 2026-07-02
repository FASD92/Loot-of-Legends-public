#pragma once

#include <string>
#include <vector>

namespace Util {
struct PrometheusMetricLabel {
    std::string name;
    std::string value;
};

struct PrometheusMetricSample {
    std::string name;
    double value{0.0};
    std::vector<PrometheusMetricLabel> labels;
};

std::string renderPrometheusTextfile(
    const std::vector<PrometheusMetricSample>& samples);

bool writePrometheusTextfileAtomically(
    const std::string& targetPath,
    const std::string& content,
    std::string* errorMessage = nullptr);
}  // namespace Util
