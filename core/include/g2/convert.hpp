// Conversion of old patch and performance files (versions 12..22) to the
// current format (23), as Clavia's editor v1.62 does when it opens a file.
//
// CPatchFilePort::Load @0006cba4 reads the file into the class of its version
// (CPatchFile_N / CPerformanceFile_N), then CFileConverter::Convert @00051224
// applies ConvertPatchNtoN+1 (Global.c @000480c6 ... @0004f09c) until the file
// is at version 23. Each step copies the file objects and runs that version's
// per-module update (parameters added, removed, inverted, rescaled, cables and
// selector values repaired, morph groups 4 and 5 swapped, ...). The editor
// then builds its patch from the converted object; saving writes version 23
// (CPatch::SaveToFile @000da3f4, after CFilePortUtils::ConvertAlert).
//
// toCurrent() returns that converted object as the v1.62 writer lays it out
// (CPatchFile_13::WriteMolecules @00051e7c): lists in their container order
// (modules by index, morphs and assignments by key), empty custom data dropped,
// reserved bits zero. See re/notes/pch2-format.md §7.
#pragma once

#include "g2/file.hpp"

#include <cstdint>

namespace g2::convert {

// Versions the original cannot convert: 11 and older (CPatchFile_11::Convert
// @00052c70) and numbers it has no file class for (CFileReader_G2_1::
// CreateDataFile @00074ef8). The message is the original's.
struct UnsupportedVersion : FormatError {
    UnsupportedVersion() : FormatError("Unsupported file version") {}
};

inline constexpr std::uint16_t kOldestConvertible = 12;

// The version the original reads a file as: the number on the third line of
// the text header ("Version=%d", CreateDataFile @00074ef8), or the binary
// version word for a file without one.
std::uint16_t sourceVersion(const file::File& f);

// The file converted to version 23, or unchanged if it is already at 23.
// Throws UnsupportedVersion, or FormatError when a section could not be
// decoded or the sections are not those of the file's version.
file::File toCurrent(const file::File& f);

} // namespace g2::convert
