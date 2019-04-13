// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/gn/escape.h"

#include <stddef.h>

#include "base/logging.h"
#include "util/build_config.h"

namespace {

// A "1" in this lookup table means that char is valid in the Posix shell.
const char kShellValid[0x80] = {
    // 00-1f: all are invalid
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    // ' ' !  "  #  $  %  &  '  (  )  *  +  ,  -  .  /
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
    //  0  1  2  3  4  5  6  7  8  9  :  ;  <  =  >  ?
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0,
    //  @  A  B  C  D  E  F  G  H  I  J  K  L  M  N  O
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    //  P  Q  R  S  T  U  V  W  X  Y  Z  [  \  ]  ^  _
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
    //  `  a  b  c  d  e  f  g  h  i  j  k  l  m  n  o
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    //  p  q  r  s  t  u  v  w  x  y  z  {  |  }  ~
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};

// Ninja's escaping rules are very simple. We always escape colons even
// though they're OK in many places, in case the resulting string is used on
// the left-hand-side of a rule.
inline bool ShouldEscapeCharForNinja(char ch) {
  return ch == '$' || ch == ' ' || ch == ':';
}

size_t EscapeStringToString_Ninja(const base::StringPiece& str,
                                  const EscapeOptions& options,
                                  char* dest,
                                  bool* needed_quoting) {
  size_t i = 0;
  for (const auto& elem : str) {
    if (ShouldEscapeCharForNinja(elem))
      dest[i++] = '$';
    dest[i++] = elem;
  }
  dest[i] = '\0';
  return i;
}

size_t EscapeStringToString_NinjaPreformatted(const base::StringPiece& str,
                                              char* dest) {
  // Only Ninja-escape $.
  size_t i = 0;
  for (const auto& elem : str) {
    if (elem == '$')
      dest[i++] = '$';
    dest[i++] = elem;
  }
  dest[i] = '\0';
  return i;
}

// Escape for CommandLineToArgvW and additionally escape Ninja characters.
//
// The basic algorithm is if the string doesn't contain any parse-affecting
// characters, don't do anything (other than the Ninja processing). If it does,
// quote the string, and backslash-escape all quotes and backslashes.
// See:
//   http://blogs.msdn.com/b/twistylittlepassagesallalike/archive/2011/04/23/everyone-quotes-arguments-the-wrong-way.aspx
//   http://blogs.msdn.com/b/oldnewthing/archive/2010/09/17/10063629.aspx
size_t EscapeStringToString_WindowsNinjaFork(const base::StringPiece& str,
                                             const EscapeOptions& options,
                                             char* dest,
                                             bool* needed_quoting) {
  // We assume we don't have any whitespace chars that aren't spaces.
  DCHECK(str.find_first_of("\r\n\v\t") == std::string::npos);

  size_t i = 0;
  if (str.find_first_of(" \"") == std::string::npos) {
    // Simple case, don't quote.
    EscapeStringToString_Ninja(str, options, dest, needed_quoting);
  } else {
    if (!options.inhibit_quoting)
      dest[i++] = '"';

    for (size_t j = 0; j < str.size(); j++) {
      // Count backslashes in case they're followed by a quote.
      size_t backslash_count = 0;
      while (j < str.size() && str[j] == '\\') {
        j++;
        backslash_count++;
      }
      if (j == str.size()) {
        // Backslashes at end of string. Backslash-escape all of them since
        // they'll be followed by a quote.
        memset(dest + i, '\\', backslash_count * 2);
        i += backslash_count * 2;
      } else if (str[j] == '"') {
        // 0 or more backslashes followed by a quote. Backslash-escape the
        // backslashes, then backslash-escape the quote.
        memset(dest + i, '\\', backslash_count * 2 + 1);
        i += backslash_count * 2 + 1;
        dest[i++] = '"';
      } else {
        // Non-special Windows character, just escape for Ninja. Also, add any
        // backslashes we read previously, these are literals.
        memset(dest + i, '\\', backslash_count);
        i += backslash_count;
        if (ShouldEscapeCharForNinja(str[j]))
          dest[i++] = '$';
        dest[i++] = str[j];
      }
    }

    if (!options.inhibit_quoting)
      dest[i++] = '"';
    if (needed_quoting)
      *needed_quoting = true;
  }
  return i;
}

size_t EscapeStringToString_PosixNinjaFork(const base::StringPiece& str,
                                           const EscapeOptions& options,
                                           char* dest,
                                           bool* needed_quoting) {
  std::size_t i = 0;
  for (const auto& elem : str) {
    if (elem == '$' || elem == ' ') {
      // Space and $ are special to both Ninja and the shell. '$' escape for
      // Ninja, then backslash-escape for the shell.
      dest[i++] = '\\';
      dest[i++] = '$';
      dest[i++] = elem;
    } else if (elem == ':') {
      // Colon is the only other Ninja special char, which is not special to
      // the shell.
      dest[i++] = '$';
      dest[i++] = ':';
    } else if (static_cast<unsigned>(elem) >= 0x80 ||
               !kShellValid[static_cast<int>(elem)]) {
      // All other invalid shell chars get backslash-escaped.
      dest[i++] = '\\';
      dest[i++] = elem;
    } else {
      // Everything else is a literal.
      dest[i++] = elem;
    }
  }
  return i;
}

size_t EscapeStringToString(const base::StringPiece& str,
                            const EscapeOptions& options,
                            char* dest,
                            bool* needed_quoting) {
  switch (options.mode) {
    case ESCAPE_NONE:
      strncpy(dest, str.data(), str.size());
      return str.size();
      break;
    case ESCAPE_NINJA:
      return EscapeStringToString_Ninja(str, options, dest, needed_quoting);
      break;
    case ESCAPE_NINJA_COMMAND:
      switch (options.platform) {
        case ESCAPE_PLATFORM_CURRENT:
#if defined(OS_WIN)
          return EscapeStringToString_WindowsNinjaFork(str, options, dest,
                                                       needed_quoting);
#else
          return EscapeStringToString_PosixNinjaFork(str, options, dest,
                                                     needed_quoting);
#endif
          break;
        case ESCAPE_PLATFORM_WIN:
          return EscapeStringToString_WindowsNinjaFork(str, options, dest,
                                                       needed_quoting);
          break;
        case ESCAPE_PLATFORM_POSIX:
          return EscapeStringToString_PosixNinjaFork(str, options, dest,
                                                     needed_quoting);
          break;
        default:
          NOTREACHED();
      }
      break;
    case ESCAPE_NINJA_PREFORMATTED_COMMAND:
      return EscapeStringToString_NinjaPreformatted(str, dest);
      break;
    default:
      NOTREACHED();
  }
}

}  // namespace

std::string EscapeString(const base::StringPiece& str,
                         const EscapeOptions& options,
                         bool* needed_quoting) {
#if defined(OS_WIN)
  char dest[str.size() * 2 + 1];
#else
  char dest[str.size() * 3 + 1];
#endif
  return std::string(dest,
                     EscapeStringToString(str, options, dest, needed_quoting));
}

void EscapeStringToStream(std::ostream& out,
                          const base::StringPiece& str,
                          const EscapeOptions& options) {
#if defined(OS_WIN)
  char dest[str.size() * 2 + 1];
#else
  char dest[str.size() * 3 + 1];
#endif
  out.write(dest, EscapeStringToString(str, options, dest, nullptr));
}
