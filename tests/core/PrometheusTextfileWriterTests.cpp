#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Util/PrometheusTextfileWriter.hpp"

namespace {
std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}
}  // namespace

TEST(PrometheusTextfileWriterTests, RendersCounterWithoutLabels) {
    const std::vector<Util::PrometheusMetricSample> samples{
        {"lol_server_tick_total", 42.0, {}}};

    EXPECT_EQ(
        "lol_server_tick_total 42\n",
        Util::renderPrometheusTextfile(samples));
}

TEST(PrometheusTextfileWriterTests, RendersLabelsWithEscapedValues) {
    const std::vector<Util::PrometheusMetricSample> samples{
        {"lol_input_rejected_total",
         3.0,
         {{"op", "Move"}, {"reason", "bad\\\"line\nnext"}}}};

    EXPECT_EQ(
        "lol_input_rejected_total{op=\"Move\",reason=\"bad\\\\\\\"line\\nnext\"} 3\n",
        Util::renderPrometheusTextfile(samples));
}

TEST(PrometheusTextfileWriterTests, RejectsInvalidMetricNames) {
    const std::vector<Util::PrometheusMetricSample> samples{{"1bad", 1.0, {}}};

    EXPECT_THROW(Util::renderPrometheusTextfile(samples), std::invalid_argument);
}

TEST(PrometheusTextfileWriterTests, WritesTextfileAtomically) {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path target =
        std::filesystem::temp_directory_path() /
        ("lol_textfile_writer_" + std::to_string(suffix) + ".prom");
    const std::string content = "lol_server_tick_total 42\n";
    std::string errorMessage;

    ASSERT_TRUE(Util::writePrometheusTextfileAtomically(
        target.string(),
        content,
        &errorMessage))
        << errorMessage;

    EXPECT_EQ(content, readFile(target));

    std::filesystem::remove(target);
    std::filesystem::remove(target.string() + ".tmp");
}
