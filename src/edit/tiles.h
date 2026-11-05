// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/hdu.h>
#include <openfits/limits.h>

#include "compress/image_tiles.h"
#include "edit/overlay.h"
#include "edit/patch.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Compressed images that the editor writes values into (standard §10.1): each tile that the writes meet decoded,
// changed and encoded again, the others kept as they are, in their table built again.

namespace openfits::detail {

/// A compressed image whose tiles that writes changed were encoded again.
struct recompressed_image
{
    /// The data of its table, the rows then the heap, compact, without their fill.
    std::vector<std::byte> data{};
    /// The structural cards of the table that change, each in the place of the card of its keyword or added: NAXIS1,
    /// PCOUNT, TFIELDS, the TTYPEn and TFORMn of columns added, the TFORMn of the columns of arrays, ZBLANK.
    std::vector<card> cards{};
    /// The keywords that the table no longer has: THEAP, its heap following its rows.
    std::vector<std::string> removed{};
    /// The sums of the stored values of the tiles encoded again, before and after, each at its place in the data of the
    /// image uncompressed: what changes the sum that ZDATASUM records (standard §4.4.2.7, Appendix J.4).
    sum_change change{};
};

/// The compressed image of unit, which image describes, with writes laid over the stored values of its image,
/// big-endian at their offsets in the data of the image uncompressed: each tile that they meet decoded, changed and
/// encoded again with the algorithm and parameters of the table, a quantized tile at the spacing of its levels
/// (standard §10.2), and the other tiles kept as they are, byte for byte.
/// @throws unsupported_error (errc::not_editable) for a compressed image whose tiles the library does not encode again:
///         ZSCALE and ZZERO as keywords, a BYTEPIX of RICE_1 other than the size of the values, or undefined values for
///         a ZBLANK keyword other than the null of the writer; limit_error beyond limits; and the exceptions of the
///         decoding and the encoding of tiles and of the source.
[[nodiscard]] recompressed_image recompress_image(const thread_safe_source& source, const hdu_info& unit,
                                                  const compressed_image& image, const byte_overlay& writes,
                                                  const openfits::limits& limits);

} // namespace openfits::detail
