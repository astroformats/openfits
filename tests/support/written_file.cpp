// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "support/written_file.h"

#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/verify.h>

#include "support/environment.h"
#include "support/files.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace openfits::test {

void record_file(std::span<const std::byte> file)
{
    const std::optional<std::string> directory = environment_variable("OPENFITS_WRITTEN_FILES_DIR");
    if (!directory) {
        return;
    }
    // The files of a test are numbered in order, also when threads write them.
    static std::mutex mutex;
    static std::string last_test;
    static int count = 0;
    const std::scoped_lock lock(mutex);
    const testing::TestInfo* test = testing::UnitTest::GetInstance()->current_test_info();
    std::string name = test == nullptr ? "unknown" : std::string(test->test_suite_name()) + "." + test->name();
    count = name == last_test ? count + 1 : 1;
    last_test = name;
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') {
            c = '_';
        }
    }
    write_file(path_of(*directory) / path_of(name + "." + std::to_string(count) + ".fits"), file);
}

std::vector<std::byte> written(const writer& output, const write_options& options, sink_kind kind)
{
    std::vector<std::byte> file;
    if (kind == sink_kind::rewritable) {
        memory_sink sink;
        output.save(sink, options);
        file = sink.release();
    } else {
        callback_sink sink(
            [&file](std::span<const std::byte> data) { file.insert(file.end(), data.begin(), data.end()); });
        output.save(sink, options);
    }
    record_file(file);
    // Every file that the writer writes conforms to the standard: its report holds no finding.
    const reader written_file(std::make_unique<memory_source>(std::vector<std::byte>(file.begin(), file.end())));
    for (const diagnostic& finding : verify(written_file)) {
        ADD_FAILURE() << "the written file departs from standard " << finding.section << ": " << finding.message;
    }
    return file;
}

} // namespace openfits::test
