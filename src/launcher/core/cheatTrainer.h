#pragma once

// The MC4 and SHN cheat files of the HEN Cheats Collection (cheatCatalog.h), converted to the
// cheat JSON of cheats.h so that the cheats dialog can preview and import them. Ported from
// upstream KytyPS5 commit 91c5820c (Cheats::ConvertTrainer, src/launcher/src/cheatFile.cpp).
//
// An SHN file is a trainer in XML:
//   <Trainer Game="..." Moder="..." Cusa="PPSA01234" Version="01.000" Process="eboot.bin">
//     <Cheat Text="Infinite HP">
//       <Cheatline><Offset>0x1234</Offset><Section>0</Section>
//         <ValueOn>90-90</ValueOn><ValueOff>0F-85</ValueOff></Cheatline>
//     </Cheat>
//   </Trainer>
// An MC4 file is the same XML encrypted with AES-256 in CBC mode under a fixed key and IV, then
// encoded in base64.

#include "launcher/core/cheats.h"
#include "launcher/core/expected.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kyty::core {

// The base64 alphabet's characters of text, decoded; every other character ('=' and line breaks
// included) is skipped, as Qt's QByteArray::fromBase64 does without AbortOnBase64DecodingErrors.
// Bits that do not fill a byte at the end are dropped.
std::string DecodeBase64Lenient(std::string_view text);

// AES-256 (FIPS 197) decryption in CBC mode (NIST SP 800-38A) of the whole 16-byte blocks of data,
// in place; the bytes after the last whole block stay as they are. No padding is removed.
void DecryptAes256Cbc(std::span<uint8_t> data, const std::array<uint8_t, 32>& key,
                      const std::array<uint8_t, 16>& iv);

// The cheat JSON of a trainer (upstream's ConvertTrainer), or nullopt when the XML is not
// well-formed up to the root's end tag or the root is not <Trainer>. With encrypted (MC4), the
// bytes are decoded as base64 and decrypted first; whatever follows the root's end tag (the
// padding) is not read. The document is
//   {"name": Game, "id": Cusa, "version": Version, "process": Process, "credits": [Moder],
//    "mods": [{"name": Text, "memory": [write, ...]}, ...]}
// with the <Trainer> attributes ("" when one is missing), one mod per <Cheat> child of the root
// (other children are skipped), and one write per child element of a <Cheat>. A write has the
// trimmed texts of that element's children: Offset as "offset", ValueOn and ValueOff without '-'
// as "on" and "off", Section as "section" unless it is "0", and "absolute": true for an Absolute
// element. A child of a write element that has child elements itself makes the XML invalid.
// ValidateRemoteImport refuses "section" and "absolute" (addressing features).
std::optional<std::string> ConvertCheatTrainer(std::string_view bytes, bool encrypted);

// ParseCheatDocument (cheats.h) of ConvertCheatTrainer(bytes, encrypted), its errors naming
// file_name; a trainer that cannot be converted is CheatProblemKind::InvalidXml, with the message
// "<file_name>: Invalid cheat XML.".
Expected<CheatDocument, CheatProblem> ParseCheatTrainer(std::string_view bytes, bool encrypted,
                                                        std::string_view file_name);

} // namespace kyty::core
