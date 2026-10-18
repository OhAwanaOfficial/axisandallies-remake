#pragma once

// VFS path rules (TGI_FORMAT §6.1): '\' → '/', no leading '/' or "./", compare ASCII
// case-insensitively. Wildcards are '*' in the last path component only.

#include <filesystem>
#include <string>
#include <string_view>

namespace corps::vfs {

// "Templates\\Foo.TGI" → "Templates/Foo.TGI" (case kept, separators unified, leading "/" and "./"
// dropped, duplicate '/' collapsed).
std::string normalizePath(std::string_view path);

// normalizePath() + ASCII lower-case: the lookup key.
std::string pathKey(std::string_view path);

// Directory part of a normalised path ("a/b/c.tgi" → "a/b", "c.tgi" → "").
std::string_view parentDir(std::string_view normalizedPath);

// Last component ("a/b/c.tgi" → "c.tgi").
std::string_view fileName(std::string_view normalizedPath);

// Case-insensitive glob on a single component; '*' matches any run of characters (including none).
bool globMatch(std::string_view pattern, std::string_view name);

// Order used inside a wildcard expansion (TGI_FORMAT §6.3): ordinal comparison of the
// upper-cased path, which is what NTFS directory enumeration returns.
bool upperOrdinalLess(std::string_view a, std::string_view b);

// UTF-8 std::string ↔ std::filesystem::path without going through the ANSI code page.
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& p);

}  // namespace corps::vfs
