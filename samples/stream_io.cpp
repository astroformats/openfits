// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The adapters of C and C++ streams. An application that already holds a stream gives it to the library through
// openfits/stream_io.h, whose adapters are inline, so that the stream is used by the C runtime and the standard library
// of the application. This program writes an image through a std::ofstream, reads it back through a std::ifstream and
// through a FILE*, and reads it once more the way a pipe is read: the stream whole into memory, since a reader needs a
// source it can read at any offset.
//
// Usage: stream_io <path>

#include <openfits/openfits.h>
#include <openfits/stream_io.h>

#include "arguments.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// The path of a UTF-8 name for the streams of the standard library, which take a narrow string in the code page of the
// system on Windows.
std::filesystem::path path_of(const std::string& name)
{
    return {std::u8string(name.begin(), name.end())};
}

// The file at path, opened for reading in binary mode, or null. On Windows the path reaches the system in UTF-16, which
// every name of a file has.
std::FILE* open_for_reading(const std::string& path)
{
#if defined(_WIN32)
    std::wstring wide(path.size(), L'\0');
    const int length = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(),
                                           static_cast<int>(wide.size()));
    wide.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
    std::FILE* stream = nullptr;
    return _wfopen_s(&stream, wide.c_str(), L"rb") == 0 ? stream : nullptr;
#else
    return std::fopen(path.c_str(), "rb"); // NOLINT(cppcoreguidelines-owning-memory): c_stream closes it
#endif
}

// A C stream of the file at path, closed by the destructor.
struct c_stream
{
    explicit c_stream(const std::string& path) : file(open_for_reading(path))
    {
        if (file == nullptr) {
            throw std::runtime_error("cannot open " + path);
        }
    }

    c_stream(const c_stream&) = delete;
    c_stream& operator=(const c_stream&) = delete;
    c_stream(c_stream&&) = delete;
    c_stream& operator=(c_stream&&) = delete;

    ~c_stream()
    {
        (void)std::fclose(file); // NOLINT(cppcoreguidelines-owning-memory): the stream is owned
    }

    std::FILE* file = nullptr;
};

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> arguments = sample::arguments(argc, argv);
        if (arguments.size() != 1) {
            std::cerr << "usage: stream_io <path>\n";
            return 2;
        }
        const std::string& path = arguments[0];

        // An image of 16 x 8 values written through a C++ stream, in binary mode. The sink can rewrite, since a file
        // stream can seek, though this writer needs no rewrite.
        std::vector<float> values(std::size_t{16} * 8);
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<float>(i) / 4.0F;
        }
        openfits::image_info image;
        image.geometry.axes = {16, 8};
        image.header.set("OBJECT", "through streams");
        openfits::writer output;
        (void)output.add_image(image, std::span<const float>(values));
        {
            std::ofstream stream(path_of(path), std::ios::binary);
            openfits::ostream_sink sink(stream);
            output.save(sink);
            stream.close();
            if (!stream) {
                throw std::runtime_error("cannot write " + path);
            }
        }

        // Read back through a C++ stream. The reader owns its source, which refers to the stream.
        std::ifstream input(path_of(path), std::ios::binary);
        const openfits::reader from_stream(std::make_unique<openfits::istream_source>(input));
        const std::vector<float> first = from_stream.read_pixels<float>(0);

        // And through a C stream.
        const c_stream c_file(path);
        const openfits::reader from_file(std::make_unique<openfits::stdio_source>(c_file.file));
        const std::vector<float> second = from_file.read_pixels<float>(0);

        // A pipe cannot seek: its bytes are read whole into memory first, within limits::max_allocation.
        const c_stream pipe_like(path);
        const openfits::reader from_memory(openfits::read_into_memory(pipe_like.file));
        const std::vector<float> third = from_memory.read_pixels<float>(0);

        std::cout << path << ": " << from_stream.size()
                  << " bytes, OBJECT = " << from_file.hdu(0).header().value<std::string>("OBJECT") << ", value 37 is "
                  << third[37] << '\n';
        return first == values && second == values && third == values ? 0 : 1;
    } catch (const std::exception& failure) {
        (void)std::fputs("stream_io: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
