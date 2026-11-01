// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <vector>

// The benchmarks register themselves from main(), so that their names are readable (read/u16/as_float/frame) and no
// macro defines statics. The numbers are what performance decisions rest on, so the data look like an astronomical
// image: a smooth background with a little noise.

namespace openfits::bench {

/// A synthetic sky of width x height pixels, in [0, 1]: a background between about 0.05 and 0.15 with noise of about
/// 0.001. The same arguments give the same values.
[[nodiscard]] std::vector<float> sky(std::size_t width, std::size_t height);

/// Images read into memory of the application and written: 16-bit images of the unsigned convention read as themselves
/// and as float, floating-point images, scaled 16-bit images read as float, the region of a stacking read, and the
/// writes of each, at 1024 x 1024 pixels (small) and at the 6000 x 4000 of a camera (frame), whose files the program
/// writes once into a directory of its own in the temporary directory of the system and removes at its end.
void register_image_benchmarks();

/// Removes the files of the benchmarks.
void remove_benchmark_files();

} // namespace openfits::bench
