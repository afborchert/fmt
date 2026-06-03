/* 
   Copyright (c) 2015, 2016, 2020, 2023, 2024, 2026 Andreas F. Borchert
   All rights reserved.

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/*
   This header-only C++11 package provides fmt::printf which
   is intended as a type-safe and extensible drop-in replacement
   for std::printf. The principal idea is to replace

      #include <cstdio>
      // ...
      std::printf(...)

   by

      #include "printf.hpp"       or        #include <printf.hpp>
      // ...
      fmt::printf(...)

   and likewise:

      std::fprintf(fp, ...)       by        fmt::printf(out, ...)
      std::snprintf(s, n, ...)    by        fmt::snprintf(s, n, ...)
      std::wprintf(...)           by        fmt::printf(...)

   where out is an ostream, not a FILE*. As fmt::printf is
   based on variadic template constructs of C++11, this
   is possible in a typesafe way. Consequently, it no
   longer matters for fmt::printf whether you use "%f",
   "%lf", or "%Lf" as format. And all operands are supported
   for which an <<-operator exists.

   fmt::printf uses C++ I/O format flags but makes sure
   that the previous state of manipulators and flags of
   the output stream is restored to its original state
   after its invocation. Any previous state is ignored,
   i.e. fmt::printf("%x", val) will print val in hex
   even if std::cout << std::oct has been used before,
   and the previous octal conversion preference will stay
   in effect for <<-operators after the invocation of fmt::printf.

   Please note that the output format of %p is not
   standardized. As the <<-operator for void* may
   differ from the std::printf behaviour for %p, results
   can be different.

   Wide characters, wide strings, and wide streams and
   their mix are supported. However, the type of the
   format string must match that of the stream.

   Restrictions:
      - The combination of hexfloat with a precision (e.g. "%.2a") 
        is not supported by C++11 (see 22.4.2.2.2 in ISO 14882:2011)
        but supported by std::printf (see 7.21.6.1 in ISO 9899:2011).
        As this implementation depends on the C++11 library, it
        appears hard to find a reasonable workaround for this
        diverting behaviour.

   Alternatives:

   This is not the first attempt to provide printf look
   and feel in a type-safe way for C++:

    - In 1994, Cay S. Horstmann published an article about
      extending the iostreams library in C++ Report where he
      proposed a setformat. Example from his paper:

         cout << "(" << setformat("%8.2f") << x << "," 
                   << setformat("8.2f") << y << ")" << endl;

      Note that he used one setformat per placeholder as
      C++ at that time did not support variadic templates.
      This paper is available at
         https://horstmann.com/cpp/iostreams.html

    - The Boost Format library has created an approach
      that does not depend on variadic templates. The
      %-operator is used instead:

      std::cout << boost::format("(x, y) = (%4f, %4f)\n" % x % y;

      See https://www.boost.org/doc/libs/1_59_0/libs/format/doc/format.html

    - There is a proposal by Zhihao Yuan for a printf-like interface for the
      C++ streams library:

      std::cout << std::putf("(x, y) = (%4f, %4f)\n", x, y);

      See https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3506.html
      and https://github.com/lichray/formatxx

    - https://codereview.stackexchange.com/questions/63578/printf-like-formatting-for-stdostream-not-exactly-boostformat
*/

#ifndef FMT_PRINTF_HPP
#define FMT_PRINTF_HPP

#if __cplusplus < 201103L
#error This file requires compiler and library support for the \
ISO C++ 2011 standard.
#else

#include <cassert>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <streambuf>
#include <string>
#include <tuple>
#include <type_traits>

/* support extended floating point types
   that are supported by std::to_chars but for
   which we do not have output operators
   (like optional std::float16_t or std::float128_t 
   in C++23); this code requires C++17 */
#if __cplusplus >= 201703L
   #include <charconv>
   #include <system_error> /* just needed for std::to_chars */
#endif

/* avoid warnings for fallthroughs */
#if __cplusplus >= 201703L
   #define FMT_PRINTF_FALLTHROUGH [[fallthrough]];
#else
   #if defined(__GNUC__)
      #define FMT_PRINTF_FALLTHROUGH __attribute__ ((fallthrough));
   #else
      #define FMT_PRINTF_FALLTHROUGH
   #endif
   #if defined(__clang__)
      #pragma clang diagnostic ignored "-Wimplicit-fallthrough"
   #endif
#endif

namespace fmt {

namespace impl {

/* type trait to recognize encoded character types which can be distinguished
   from regular numerical types, see also [iosfwd.syn]
*/
template<typename T> struct is_char : public std::false_type {};
template<> struct is_char<char> : public std::true_type {};
#if __cplusplus >= 202002L
template<> struct is_char<char8_t> : public std::true_type {};
#endif
template<> struct is_char<wchar_t> : public std::true_type {};
template<> struct is_char<char16_t> : public std::true_type {};
template<> struct is_char<char32_t> : public std::true_type {};

/* type trait that allows to recognize types for which
   we have an output operator */
template<typename CharT, typename Traits, typename T>
class has_output_operator {
   private:
      template<typename U>
      static auto test(int) -> decltype(
         std::declval<std::basic_ostream<CharT, Traits>&>() <<
            std::declval<U>(),
            std::true_type()
      );
      template<typename>
      static std::false_type test(...);
   public:
      static constexpr bool value =
         std::is_same<decltype(test<T>(0)), std::true_type>::value;
};

#if __cplusplus >= 201703L
template<typename T>
constexpr std::size_t fp_buffer_size_for_base10(int precision) {
   using limits = std::numeric_limits<std::decay_t<T>>;
   auto len = limits::max_exponent10;
   if (precision > 0) {
      len += precision;
   } else {
      len += limits::max_digits10;
   }
   /* extras including sign, decimal point, 'e' or 'E', sign of exponent */
   len += 5;
   return len;
}

template<typename T>
constexpr int exponent2(T x) {
   if (x == 0 || !std::isfinite(x)) return 0;
   int e{}; std::frexp(x, &e);
   return e;
}
template<typename T>
constexpr int exponent10(T x) {
   int e2 = exponent2(x);
   if (e2 == 0) return 0;
   return static_cast<int>(std::floor((e2 - 1) * std::log10(2)));
}
#endif

/* printf is expected to return the number of bytes written;
   the following extensions direct all output to the given
   output stream and count all bytes written */
template<typename CharT, typename Traits = std::char_traits<CharT>>
class counting_ostreambuf : public std::basic_streambuf<CharT, Traits> {
   public:
      counting_ostreambuf(std::basic_streambuf<CharT, Traits>& sbuf) :
         sbuf(sbuf) {
      }
      std::streamsize get_count() const {
         return nbytes;
      }
   protected:
      using Base = std::basic_streambuf<CharT, Traits>;
      using char_type = typename Base::char_type;
      using int_type = typename Base::int_type;
      using traits_type = typename Base::traits_type;

      virtual std::streamsize xsputn(const char_type* s,
            std::streamsize count) {
         std::streamsize result = sbuf.sputn(s, count);
         if (result > 0) nbytes += result;
         return result;
      }
      virtual int_type overflow(int_type ch) {
         /* modeled after
            https://stackoverflow.com/questions/10921761/extending-c-ostream */
         if (ch == traits_type::eof()) {
            return traits_type::eof();
         } else {
            char_type c = traits_type::to_char_type(ch);
            return xsputn(&c, 1) == 1? ch: traits_type::eof();
         }
      }
      virtual int sync() {
         return sbuf.pubsync();
      }
   private:
      std::basic_streambuf<CharT, Traits>& sbuf;
      std::streamsize nbytes = 0;
};

template<typename CharT, typename Traits = std::char_traits<CharT>>
class counting_ostream : public std::basic_ostream<CharT, Traits> {
   public:
      using Base = std::basic_ostream<CharT, Traits>;
      counting_ostream(std::basic_ostream<CharT, Traits>& out) :
            Base(&sbuf), sbuf(*(out.rdbuf())) {
         /* inherit locale from our base stream */
         this->imbue(out.getloc());
      }
      std::streamsize get_count() const {
         return sbuf.get_count();
      }
   private:
      counting_ostreambuf<CharT, Traits> sbuf;
};

template<typename CharT, typename Traits = std::char_traits<CharT>>
class uppercase_ostreambuf : public std::basic_streambuf<CharT, Traits> {
   public:
      uppercase_ostreambuf(std::basic_streambuf<CharT, Traits>& sbuf) :
         sbuf(sbuf) {
      }
   protected:
      using Base = std::basic_streambuf<CharT, Traits>;
      using char_type = typename Base::char_type;
      using int_type = typename Base::int_type;
      using traits_type = typename Base::traits_type;

      virtual std::streamsize xsputn(const char_type* s,
            std::streamsize count) {
         for (std::streamsize i = 0; i < count; ++i) {
            char_type ch = s[i];
            if (std::islower(ch, this->getloc())) {
               ch = std::toupper(ch, this->getloc());
            }
            if (sbuf.sputc(ch) == traits_type::eof()) {
               return i;
            }
         }
         return count;
      }

      virtual int_type overflow(int_type ch) {
         if (ch == traits_type::eof()) {
            return traits_type::eof();
         } else {
            char_type c = traits_type::to_char_type(ch);
            return xsputn(&c, 1) == 1? ch: traits_type::eof();
         }
      }
      virtual int sync() {
         return sbuf.pubsync();
      }
   private:
      std::basic_streambuf<CharT, Traits>& sbuf;
};

template<typename CharT, typename Traits = std::char_traits<CharT>>
class uppercase_ostream : public std::basic_ostream<CharT, Traits> {
   public:
      using Base = std::basic_ostream<CharT, Traits>;
      uppercase_ostream(std::basic_ostream<CharT, Traits>& out) :
            Base(&sbuf), sbuf(*(out.rdbuf())) {
         this->copyfmt(out);
         /* inherit locale from our base stream */
         this->imbue(out.getloc());
      }
   private:
      uppercase_ostreambuf<CharT, Traits> sbuf;
};

/* std::numpunct extension for the format flag '\''
   that explicitly asks for thousands' grouping characters */
struct thousands_grouping : std::numpunct<char> {
   std::string do_grouping() const {
      return "\3";
   }
};

/* std::numpunct extension that suppresses the
   use of grouping characters,
   this is necessary to conform to std::printf behaviour */
struct suppress_grouping : std::numpunct<char> {
   std::string do_grouping() const {
      return "\0";
   }
};

/* retrieve thousands's grouping character */
template<typename CharT, typename Traits>
CharT thousands_sep_for_stream(std::basic_ostream<CharT, Traits>& out) {
   return std::use_facet<std::numpunct<CharT>>(out.getloc()).thousands_sep();
}

/* RAII object that saves the current formatting state of the stream
   and makes sure that the state is restored on destruction */
template<typename CharT, typename Traits>
struct format_saver {
   format_saver(std::basic_ios<CharT, Traits>& s) :
      s(s), format_keeper(nullptr) {
      format_keeper.copyfmt(s);
   }
   ~format_saver() {
      s.copyfmt(format_keeper);
   }
   std::basic_ios<CharT, Traits>& s;
   std::basic_ios<CharT, Traits> format_keeper;
};

/* reset the entire format state to its default */
template<typename CharT, typename Traits>
inline void reset_format(std::basic_ios<CharT, Traits>& s) {
   std::basic_ios<CharT, Traits> dflt(nullptr);
   s.copyfmt(dflt);
}

/* some compilers like icpc or nvcc deliver a warning even
   for template parameters that we have a "pointless comparison
   of unsigned with zero"; the function is_negative is used
   to circumvent this */
template<typename Value>
typename std::enable_if<
      std::is_integral<typename std::remove_reference<Value>::type>::value &&
      std::is_signed<typename std::remove_reference<Value>::type>::value,
      bool>::type
is_negative(Value value) {
   return value < 0;
}

template<typename Value>
typename std::enable_if<
      std::is_integral<typename std::remove_reference<Value>::type>::value &&
      !std::is_signed<typename std::remove_reference<Value>::type>::value,
      bool>::type
is_negative(Value) {
   return false;
}

/* internal signed integer type which is used for indices and byte counts */
using integer = std::make_signed<std::size_t>::type;

using flagset = unsigned short;
constexpr flagset is_pointer = 1<<0;
constexpr flagset is_charval = 1<<1;
constexpr flagset is_integer = 1<<2;
constexpr flagset is_unsigned = 1<<3;
constexpr flagset toupper = 1<<4; // when std::uppercase won't cut it
constexpr flagset space_flag = 1<<5; // add space, if non-negative
constexpr flagset plus_flag = 1<<6;
constexpr flagset dyn_width = 1<<7;
constexpr flagset precision = 1<<8; // precision was given
constexpr flagset dyn_precision = 1<<9;
constexpr flagset zero_fill = 1<<10;
constexpr flagset minus_flag = 1<<11;
constexpr flagset special_flag = 1<<12;
constexpr flagset grouping_flag = 1<<13;

/* this structure represents a segment of a format string
   up to and including at most one placeholder */
template<typename CharT>
struct format_segment {
   constexpr format_segment() :
      valid(false),
      beginp(nullptr), endp(nullptr), nextp(nullptr),
      fmtflags(), flags(0), base(0), nof_args(0),
      width(0), precision(0),
      width_index(-1), precision_index(-1), value_index(-1),
      conversion(0) {
   }
   /* valid is set to false when format parsing failed */
   bool valid;
   /* stretch of the format string which is to be
      printed in verbatim; if beginp == endp
      nothing is to be printed */
   const CharT* beginp; const CharT* endp;
   /* where to continue parsing */
   const CharT* nextp;
   /* preliminary set of flags that are to be set
      on the output stream */
   std::ios_base::fmtflags fmtflags;
   /* internal flags */
   flagset flags;
   /* base in case of numerical conversions */
   integer base;
   /* number of arguments that are to be consumed,
      this is between 0 and 2 */
   unsigned short int nof_args;
   /* width and precision, if given within the format */
   std::streamsize width;
   std::streamsize precision;
   /* indices of arguments, where required */
   integer width_index;
   integer precision_index;
   integer value_index;
   /* conversion character, i.e. d o x u etc. */
   CharT conversion;
};

/* parse integer value from format string;
   return false in case of overflows */
template<typename CharT, typename T>
bool parse_integer(const CharT*& format, T& val) {
   T v{};
   CharT ch = *format;
   constexpr T maxval = std::numeric_limits<T>::max();
   constexpr T maxval10 = maxval / 10;
   while (ch >= '0' && ch <= '9') {
      T digit = ch - '0';
      if (v > maxval10) return false;
      v *= 10;
      if (v > maxval - digit) return false;
      v += digit;
      ch = *++format;
   }
   val = v;
   return true;
}

/* parse up to one format specification and
   invoke the respective manipulators for out
   and/or set the corresponding flags */
template<typename CharT>
inline format_segment<CharT>
parse_format_segment(const CharT* format, integer arg_index) {
   format_segment<CharT> result;
   if (!format) return result;

   /* skip everything until we encounter a placeholder
      or the end of the format string */
   result.beginp = format;
   CharT ch = *format;
   while (ch && ch != '%') {
      ch = *++format;
   }
   result.endp = format;

   /* end of format string reached? */
   if (!ch) {
      result.valid = true;
      return result;
   }

   ch = *++format;
   if (!ch) return result; /* format ends with '%' */

   /* process %% */
   if (ch == '%') {
      result.valid = true;
      ++result.endp; /* include first '%' */
      result.nextp = format+1;
      return result;
   }

   /* check if we have an argument index */
   if (ch >= '1' && ch <= '9') {
      const CharT* begin = format;
      integer index;
      if (parse_integer(format, index) && *format == '$') {
         /* accept argument index */
         result.value_index = index - 1;
         ch = *++format;
      } else {
         /* reset parsing */
         format = begin; ch = *format;
      }
   }

   /* process conversion flags */
   while (ch == '\'' || ch == '-' || ch == '0' || ch == '+' ||
         ch == ' ' || ch == '#') {
      switch (ch) {
         case '\'':
            result.flags |= grouping_flag;
            break;
         case '-':
            result.flags |= minus_flag;
            result.fmtflags |= std::ios_base::left;
            break;
         case '0': result.flags |= zero_fill; break;
         case '+':
            result.flags |= plus_flag;
            result.fmtflags |= std::ios_base::showpos;
            break;
         case ' ': result.flags |= space_flag; break;
         case '#':
            result.flags |= special_flag;
            result.fmtflags |= (std::ios_base::showbase |
               std::ios_base::showpoint);
            break;
      }
      ch = *++format;
   }

   if ((result.flags & minus_flag) && (result.flags & zero_fill)) {
      /* if the 0 and - flags both appear, the 0 flag is ignored */
      result.flags &= ~zero_fill;
   }
   if ((result.flags & plus_flag) && (result.flags & space_flag)) {
      /* if the ' ' and '+' flags both appear,
         the <space> flag shall be ignored */
      result.flags &= ~space_flag;
   }
   /* minimum field width */
   std::streamsize width = 0;
   if (ch == '*') {
      result.flags |= dyn_width; ch = *++format;
      if (ch >= '1' && ch <= '9') {
         integer index;
         if (!parse_integer(format, index) || *format != '$') return result;
         ch = *++format;
         result.width_index = index - 1;
      } else {
         result.width_index = arg_index + result.nof_args;
      }
      result.nof_args++;
   } else {
      if (!parse_integer(format, width)) return result;
      ch = *format;
      result.width = width;
   }
   /* precision */
   if (ch == '.') {
      result.flags |= precision;
      ch = *++format;
      std::streamsize prec = 0;
      if (ch == '*') {
         result.flags |= dyn_precision; ch = *++format;
         if (ch >= '1' && ch <= '9') {
            integer index;
            if (!parse_integer(format, index) || *format != '$') return result;
            ch = *++format;
            result.precision_index = index - 1;
         } else {
            result.precision_index = arg_index + result.nof_args;
         }
         result.nof_args++;
      } else {
         if (ch >= '0' && ch <= '9') {
            if (!parse_integer(format, prec)) return result;
            ch = *format;
         }
         result.precision = prec;
      }
      if (result.flags & zero_fill) {
         /* if a precision is specified, the 0 flag is ignored */
         result.flags &= ~zero_fill;
      }
   }
   /* skip size specification */
   while (ch == 'l' || ch == 'L' || ch == 'h' ||
         ch == 'j' || ch == 'z' || ch == 't') {
      ch = *++format;
   }
   /* conversion operation */
   result.conversion = ch;
   switch (ch) {
      case 'u':
         result.flags |= is_unsigned;
         FMT_PRINTF_FALLTHROUGH
      case 'd':
      case 'i':
         result.flags |= is_integer;
         result.base = 10;
         break;
      case 'o':
         result.flags |= is_integer;
         result.base = 8;
         break;
      case 'x':
         result.flags |= is_integer;
         result.base = 16;
         break;
      case 'X':
         result.fmtflags |= std::ios_base::uppercase;
         result.flags |= is_integer;
         result.base = 16;
         break;
      case 'f':
         result.fmtflags |= std::ios_base::fixed;
         result.base = 10;
         break;
      case 'F':
         result.fmtflags |= (std::ios_base::fixed | std::ios_base::uppercase);
         result.flags |= toupper;
         result.base = 10;
         break;
      case 'e':
         result.fmtflags |= std::ios_base::scientific;
         result.base = 10;
         break;
      case 'E':
         result.fmtflags |=
            std::ios_base::scientific | std::ios_base::uppercase;
         result.flags |= toupper;
         result.base = 10;
         break;
      case 'g':
         /* default behaviour */
         result.base = 10;
         break;
      case 'G':
         result.fmtflags |= std::ios_base::uppercase;
         result.flags |= toupper;
         result.base = 10;
         break;
      case 'a':
         result.fmtflags |= std::ios_base::scientific | std::ios_base::fixed;
         result.base = 16;
         break;
      case 'A':
         result.fmtflags |= std::ios_base::scientific |
            std::ios_base::fixed | std::ios_base::uppercase;
         result.flags |= toupper;
         result.base = 16;
         break;
      case 'p':
         result.base = 16;
         result.flags |= is_pointer;
         break;
      case 'C':
         /* POSIX extension, equivalent to 'lc' */
      case 'c':
         result.flags |= is_charval;
         break;
      case 'S':
         /* POSIX extension, equivalent to 'ls' */
      case 's':
         /* when boolean values are printed with %s, we get
            more readable results; idea taken from N3506 */
         result.fmtflags |= std::ios_base::boolalpha;
         break;
      case 'n':
         /* nothing to be done here */
         break;
      default:
         return result;
   }
   if ((result.flags & grouping_flag) && (result.base != 10)) {
      /* grouping is just supported for %i, %d, %u, %f, %F,
         %g, and %G, i.e. all cases with base == 10 */
      result.flags &= ~grouping_flag;
   }
   result.valid = true;
   if (result.value_index < 0) {
      result.value_index = arg_index + result.nof_args;
   }
   result.nof_args++;
   ch = *++format;
   if (ch) {
      result.nextp = format;
   }
   return result;
}

/* similar to std::integer_sequence of C++14 */
template<integer... Is> struct seq {
   typedef seq<Is..., sizeof...(Is)> next;
};
template<integer N> struct gen_seq {
   typedef typename gen_seq<N-1>::type::next type;
};
template<> struct gen_seq<0> {
   typedef seq<> type;
};

/* idea taken from
   https://stackoverflow.com/questions/21062864/optimal-way-to-access-stdtuple-element-in-runtime-by-index
*/

/* apply f on the n-th element of a tuple for compile-time n */
template<integer N, typename Tuple, typename Function>
inline auto apply(const Tuple& tuple, Function&& f)
      -> decltype(f(std::get<N>(tuple))) {
   return f(std::get<N>(tuple));
}

/* helper to apply f on the n-th element of a tuple for runtime n */
template<typename Tuple, typename Function, integer... Is>
inline auto apply(const Tuple& tuple, integer index, Function&& f, seq<Is...>)
      -> decltype(f(std::get<0>(tuple))) {
   using apply_t = decltype(&apply<0, Tuple, Function>);
   static const apply_t apply_functions[] = {&apply<Is, Tuple, Function>...};
   return apply_functions[index](tuple, std::forward<Function>(f));
}

/* apply f on the n-th element of a tuple for runtime n */
template<typename Tuple, typename Function>
inline auto apply(const Tuple& tuple, integer index, Function&& f)
      -> decltype(f(std::get<0>(tuple))) {
   return apply(tuple, index, std::forward<Function>(f),
      typename gen_seq<std::tuple_size<Tuple>::value>::type());
}

/* function object class to extract an integer value by index
   from a tuple */
struct get_value_f {
   template<typename Value>
   typename std::enable_if<std::is_integral<Value>::value, integer>::type
   operator()(Value value) {
      return static_cast<integer>(value);
   }
   /* return -1 when the value is not of integral type */
   template<typename Value>
   typename std::enable_if<!std::is_integral<Value>::value, integer>::type
   operator()(Value) {
      return -1;
   }
};

/* extract an integer value by index from a tuple,
   -1 is returned in case of failures */
template<typename Tuple>
inline integer get_value(const Tuple& tuple, integer index) {
   if (index >= 0 &&
         index < static_cast<integer>(std::tuple_size<Tuple>::value)) {
      return apply(tuple, index, get_value_f());
   } else {
      return -1;
   }
}

/* set offset value in case of %n */
struct set_value_f {
   set_value_f(std::streamsize offset) : offset(offset) {
   }
   integer operator()(int* ptr) {
      *ptr = static_cast<int>(offset);
      return 0;
   }
   template<typename Value>
   integer operator()(Value) {
      return -1;
   }
   std::streamsize offset;
};

template<typename Tuple>
inline integer set_value(const Tuple& tuple, integer index,
      std::streamsize offset) {
   if (index >= 0 &&
         index < static_cast<integer>(std::tuple_size<Tuple>::value)) {
      return apply(tuple, index, set_value_f(offset));
   } else {
      return -1;
   }
}

/* general formatted output route */
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<
      !std::is_integral<
         typename std::remove_reference<Value>::type>::value &&
      !std::is_floating_point<
         typename std::remove_reference<Value>::type>::value &&
      !std::is_pointer<
         typename std::remove_reference<Value>::type>::value, bool>::type
print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>&, Value&& value) {
   out << value;
   return !!out;
}

/* formatted output of floating point values
   for which an output operator is provided
*/
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<
      std::is_floating_point<
         typename std::remove_reference<Value>::type>::value &&
      has_output_operator<CharT, Traits,
         typename std::remove_reference<Value>::type>::value,
      bool>::type
print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, Value&& value) {
   if ((fseg.flags & zero_fill) && std::isfinite(value)) {
      out << std::setfill(static_cast<CharT>('0'));
      out.setf(std::ios_base::internal);
   }
   if ((fseg.flags & space_flag) && !std::signbit(value)) {
      if (!out.put(' ')) return false;
      if (fseg.width > 0) {
         out.width(fseg.width-1);
      }
   }
   if (fseg.flags & toupper) {
      /* the default output operators fail to
         use uppercase characters in some cases */
      impl::uppercase_ostream<CharT, Traits> fpout(out);
      fpout << value;
   } else {
      out << value;
   }
   return !!out;
}

/* formatted output of floating point values
   for which no output operator is provided;
   in this case we fall back to std::to_chars;
   this variant supports floating point types
   like std::float16_t or std::float128_t which
   appeared in C++23 but are not supported by <iostream>;
   note that std::to_chars is supported since C++17
*/
#if __cplusplus >= 201703L
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<
      std::is_floating_point<
         typename std::remove_reference<Value>::type>::value &&
      !has_output_operator<CharT, Traits,
         typename std::remove_reference<Value>::type>::value,
      bool>::type
print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, Value&& value) {
   auto prec = fseg.precision;
   bool specify_precision = (fseg.flags & precision);
   if (!specify_precision) {
      prec = 6;
   }
   std::chars_format fmt = std::chars_format::general;
   bool add_hex = false; char hex_char = 0;
   if (fseg.base == 16) {
      fmt = std::chars_format::hex;
      if (std::isfinite(value)) {
         add_hex = true;
         if (fseg.flags & toupper) {
            hex_char = 'X';
         } else {
            hex_char = 'x';
         }
      }
   } else if (fseg.fmtflags & std::ios_base::fixed) {
      fmt = std::chars_format::fixed;
      specify_precision = true;
   } else if (fseg.fmtflags & std::ios_base::scientific) {
      fmt = std::chars_format::scientific;
      specify_precision = true;
   }
   if ((fseg.flags & special_flag) && fmt == std::chars_format::general) {
      if (!std::isnan(value)) {
         /* std::chars_format::general does not print trailing zeroes
            in accordance to "%g" format. However, "%#g" is supposed
            to honor precision as "%e" or "%f" do. We fix this by
            selecting fixed or scientific format and adapting the
            precision according to § 7.23.6.2 ISO 9899:2024, p. 334. */
         int exp = exponent10(value);
         if (prec > exp && exp >= -4) {
            if (prec > exp + 1) {
               prec = prec - (exp + 1);
            } else {
               prec = 0;
            }
            fmt = std::chars_format::fixed;
         } else {
            fmt = std::chars_format::scientific;
            if (prec > 0) --prec;
         }
         specify_precision = true;
      }
   } else if (fmt == std::chars_format::general) {
      /* unfortunately the default of std::to_chars does not
         match that of "%g" */
      specify_precision = true;
   }

   /* now feed it to std::to_chars */
   auto len = fp_buffer_size_for_base10<Value>(prec);
   auto buf = std::make_unique<char[]>(len);
   std::to_chars_result res;
   if (specify_precision) {
      res = std::to_chars(buf.get(), buf.get() + len,
         std::forward<Value>(value), fmt, prec);
   } else {
      res = std::to_chars(buf.get(), buf.get() + len,
         std::forward<Value>(value), fmt);
   }
   if (res.ec != std::errc{}) {
      /* this is expected to happen when len is too small */
      return false;
   }
   char* start = buf.get(); /* will skip leading '-' in case of 0-padding */
   auto nbytes = res.ptr - start;
   if (nbytes == 0) return false;

   /* prepare handling of leading '0x' and additional sign */
   auto width = fseg.width;
   if (add_hex) {
      if (width > 2) {
         width -= 2;
      } else {
         width = 0;
      }
   }
   bool add_sign = !std::signbit(value) &&
      (fseg.flags & (space_flag | plus_flag));
   char sign_ch = 0;
   if (add_sign) {
      if (fseg.flags & space_flag) {
         sign_ch = ' ';
      } else {
         sign_ch = '+';
      }
      if (width > 0) --width;
   } else if (*start == '-' && ((fseg.flags & zero_fill) || add_hex)) {
      ++start; --nbytes;
      sign_ch = '-';
      add_sign = true;
      if (width > 0) --width;
   }

   /* prepare possible grouping which is not supported
      by std::to_chars */
   bool add_grouping = false; CharT grouping_ch{};
   std::streamsize dec_period_index{};
   std::streamsize first_digit_i{};
   if (fseg.flags & grouping_flag) {
      /* we need to add manually the thousands' grouping characters */
      grouping_ch = thousands_sep_for_stream(out);
      std::streamsize i = 0;
      for (; i < nbytes; ++i) {
         if (std::isdigit(start[i])) {
            first_digit_i = i;
            break;
         }
      }
      if (i < nbytes) {
         for (;i < nbytes; ++i) {
            if (!std::isdigit(start[i]) && start[i] != '-') {
               dec_period_index = i; add_grouping = true;
               break;
            }
         }
         if (!add_grouping) {
            dec_period_index = nbytes;
            add_grouping = true;
         }
         auto digits = dec_period_index - first_digit_i;
         /* adjust width */
         if (digits > 3) {
            auto extra_space = (digits - 1) / 3;
            if (width > extra_space) {
               width -= extra_space;
            } else {
               width = 0;
            }
         } else {
            add_grouping = false;
         }
      }
   }

   /* as we have now computed the width of the floating point
      number, we are able to emit leading white space,
      optional '0x', optional sign, and optional 0-padding
      in front of the text generated by std::to_chars */
   if (width > nbytes && !(fseg.flags & minus_flag)) {
      auto fill_ch = static_cast<CharT>(' ');
      if ((fseg.flags & zero_fill) && std::isfinite(value)) {
         if (add_sign) {
            if (!out.put(sign_ch)) return false;
            add_sign = false;
         }
         if (add_hex) {
            if (!out.put('0') || !out.put(hex_char)) return false;
            add_hex = false;
         }
         fill_ch = static_cast<CharT>('0');
      }
      for (std::streamsize i = 0; i < width - nbytes; ++i) {
         if (!out.put(fill_ch)) return false;
      }
   }
   if (add_sign) {
      if (!out.put(sign_ch)) return false;
   }
   if (add_hex) {
      if (!out.put('0') || !out.put(hex_char)) return false;
   }

   /* print the text generated by std::to_chars,
      we have to take care of possible grouping and
      a possible upper case flag */
   if (fseg.flags & toupper) {
      impl::uppercase_ostream<CharT, Traits> fpout(out);
      for (std::streamsize i = 0; i < nbytes; ++i) {
         if (add_grouping && i > first_digit_i && i+3 < dec_period_index &&
               (dec_period_index - i) % 3 == 0) {
            if (!fpout.put(grouping_ch)) return false;
         }
         if (!fpout.put(static_cast<CharT>(start[i]))) return false;
      }
   } else {
      for (std::streamsize i = 0; i < nbytes; ++i) {
         if (add_grouping && i > first_digit_i && i+3 <= dec_period_index &&
               (dec_period_index - i) % 3 == 0) {
            if (!out.put(grouping_ch)) return false;
         }
         if (!out.put(static_cast<CharT>(start[i]))) return false;
      }
   }

   /* trailing white space, if any */
   if (width > nbytes && (fseg.flags & minus_flag)) {
      for (std::streamsize i = 0; i < width - nbytes; ++i) {
         if (!out.put(' ')) return false;
      }
   }
   return !!out;
}
#endif

template<typename Value>
inline integer count_digits(Value value, integer base) {
   if (value == 0) {
      return 1;
   } else {
      integer digits = 0;
      while (value != 0) {
         value /= base; ++digits;
      }
      return digits;
   }
}

/* formatted output of character values (in case of %c)
   where we got a non-char-type numerical value */
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<!is_char<Value>::value, bool>::type
print_char_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>&, Value value) {
   out << static_cast<CharT>(value);
   return !!out;
}

/* formatted output of character values (in case of %c)
   without conversion */
template<typename CharT, typename Traits>
inline bool print_char_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>&, CharT value) {
   out << value;
   return !!out;
}

/* formatted output of character values (in case of %c)
   that needs to be widened */
template<typename CharT, typename Traits>
inline typename std::enable_if<!std::is_same<char, CharT>::value,
      bool>::type
print_char_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>&, char value) {
   out << out.widen(value);
   return !!out;
}

/* formatted output of character values (in case of %c)
   that need to be converted */
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<
      is_char<Value>::value &&
      !std::is_same<Value, char>::value &&
      !std::is_same<Value, CharT>::value,
      bool>::type
print_char_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>&, Value value) {
   auto& f = std::use_facet<std::codecvt<Value, CharT, std::mbstate_t>>(
      out.getloc());
   std::mbstate_t state{};
   std::basic_string<CharT> converted(f.max_length(), 0);
   const Value* from_next;
   CharT* to_next;
   auto result = f.out(state,
      /* from */ &value, &value + 1, from_next,
      /* to */ &converted[0], &converted[converted.size()], to_next);
   if (result == std::codecvt_base::ok) {
      converted.resize(to_next - &converted[0]);
      out << converted;
   } else {
      out.setstate(std::ios_base::failbit);
   }
   return !!out;
}

/* formatted output of integral values
   which possibly need to be converted to characters first
   (in case of %c) */
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<
      std::is_integral<typename std::remove_reference<Value>::type>::value,
      bool>::type
print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, Value&& value) {
   integer padding = 0;
   if (fseg.flags & is_charval) {
      print_char_value(out, fseg, value);
   } else if (fseg.flags & is_integer) {
      if (fseg.flags & zero_fill) {
         out << std::internal << std::setfill(out.widen('0'));
      } else if (fseg.flags & precision) {
         integer digits = count_digits(value, fseg.base);
         integer signwidth = is_negative(value) ||
            (fseg.flags & (plus_flag | space_flag));
         integer extra = signwidth;
         if (value != 0 && (fseg.flags & special_flag) && fseg.base == 16) {
            extra += 2; /* '0x' */
         }
         if (fseg.flags & grouping_flag) {
            extra += digits / 3;
         }
         if (fseg.precision > digits) {
            /* padding with 0s required */
            if (fseg.width > fseg.precision + extra) {
               /* manual filling is required */
               if ((out.flags() & std::ios_base::adjustfield) ==
                     std::ios_base::left) {
                  /* padding has to be postponed */
                  padding = fseg.width - fseg.precision - extra;
               } else {
                  for (int i = 0; i < fseg.width - fseg.precision - extra;
                        ++i) {
                     out.put(out.widen(' '));
                  }
               }
            }
            out << std::internal << std::setfill(out.widen('0')) <<
               std::setw(fseg.precision + extra);
         }
      }
      if ((fseg.flags & space_flag) && !is_negative(value)) {
         if (!out.put(' ')) return false;
         auto width = out.width(0);
         if (width > 0) {
            out.width(width-1);
         }
      }
      /* convert character types to a corresponding integer type */
      using integer = decltype(value + 0);
      if (!(out << static_cast<integer>(value))) return false;
      /* print padding now when it is left adjusted */
      for (int i = 0; i < padding; ++i) {
         out.put(out.widen(' '));
      }
   } else {
      /* neither %c, %d, %o, %x etc. has been given as expected,
         we proceed with default behaviour */
      out << value;
   }
   return !!out;
}

/* special case for bool
   which helps to suppress -Wbool-compare warnings of gcc */
template<typename CharT, typename Traits>
bool print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, bool value) {
   return print_value(out, fseg, static_cast<unsigned int>(value));
}

/* formatted output of CharT strings;
   precision is honoured */
template<typename CharT, typename Traits>
inline bool print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, const CharT* value) {
   if (fseg.flags & is_pointer) {
      /* %p given: print pointer value */
      out << static_cast<const void*>(value);
   } else if (fseg.flags & is_unsigned) {
      /* print it as an unsigned integer value */
      print_value(out, fseg, reinterpret_cast<std::uintptr_t>(value));
   } else if (fseg.flags & is_integer) {
      /* print it as a signed integer value */
      print_value(out, fseg, reinterpret_cast<std::intptr_t>(value));
   } else {
      if (fseg.flags & precision) {
         integer prec = fseg.precision;
         for (integer i = 0; i < prec; ++i) {
            if (!value[i]) {
               prec = i; break;
            }
         }
         integer padding = 0;
         if (fseg.width > prec) {
            padding = fseg.width - prec;
         }
         bool left = (out.flags() & std::ios_base::adjustfield) ==
                  std::ios_base::left;
         if (!left) {
            for (integer i = 0; i < padding; ++i) {
               out.put(out.widen(' '));
            }
         }
         if (prec > 0) {
            out.write(value, prec);
         }
         if (left) {
            for (integer i = 0; i < padding; ++i) {
               out.put(out.widen(' '));
            }
         }
      } else {
         out << value;
      }
   }
   return !!out;
}

/* formatted output of std::nullptr_t strings;
   unfortunately we have no output operator for this type in C++11 */
template<typename CharT, typename Traits>
inline bool print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, std::nullptr_t value) {
   if (fseg.flags & is_pointer) {
      /* %p given: print pointer value */
      out << static_cast<const void*>(value);
      return !!out;
   } else if (fseg.flags & is_unsigned) {
      /* print it as an unsigned integer value */
      print_value(out, fseg, reinterpret_cast<std::uintptr_t>(value));
      return !!out;
   } else if (fseg.flags & is_integer) {
      /* print it as a signed integer value */
      print_value(out, fseg, reinterpret_cast<std::intptr_t>(value));
      return !!out;
   } else {
      /* fail otherwise */
      return false;
   }
}

/* formatted output of char strings that need to be widened;
   precision is honoured */
template<typename CharT, typename Traits>
inline typename std::enable_if<!std::is_same<CharT, char>::value, bool>::type
print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, const char* value) {
   if (fseg.flags & is_pointer) {
      /* %p given: print pointer value */
      out << static_cast<const void*>(value);
   } else if (fseg.flags & is_unsigned) {
      /* print it as an unsigned integer value */
      print_value(out, fseg, reinterpret_cast<std::uintptr_t>(value));
   } else if (fseg.flags & is_integer) {
      /* print it as a signed integer value */
      print_value(out, fseg, reinterpret_cast<std::intptr_t>(value));
   } else {
      integer padding = 0;
      integer len = 0;
      bool left = (out.flags() & std::ios_base::adjustfield) ==
               std::ios_base::left;
      if (fseg.flags & precision) {
         len = fseg.precision;
         for (integer i = 0; i < len; ++i) {
            if (!value[i]) {
               len = i; break;
            }
         }
      } else {
         while (value[len]) ++len;
      }
      if (fseg.width > len) {
         padding = fseg.width - len;
      }
      if (!left) {
         for (integer i = 0; i < padding; ++i) {
            out.put(out.widen(' '));
         }
      }
      for (integer i = 0; i < len; ++i) {
         out.put(out.widen(value[i]));
      }
      if (left) {
         for (integer i = 0; i < padding; ++i) {
            out.put(out.widen(' '));
         }
      }
   }
   return !!out;
}

/* formatted output of strings that need to be converted;
   precision is honoured */
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<
   !std::is_same<CharT, Value>::value && is_char<Value>::value,
   bool>::type
print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, const Value* value) {
   if (fseg.flags & is_pointer) {
      /* %p given: print pointer value */
      out << static_cast<const void*>(value);
   } else if (fseg.flags & is_unsigned) {
      /* print it as an unsigned integer value */
      print_value(out, fseg, reinterpret_cast<std::uintptr_t>(value));
   } else if (fseg.flags & is_integer) {
      /* print it as a signed integer value */
      print_value(out, fseg, reinterpret_cast<std::intptr_t>(value));
   } else {
      integer len = 0;
      if (fseg.flags & precision) {
         len = fseg.precision;
         for (integer i = 0; i < len; ++i) {
            if (!value[i]) {
               len = i; break;
            }
         }
      } else {
         while (value[len]) ++len;
      }
      auto& f = std::use_facet<std::codecvt<Value, CharT, std::mbstate_t>>(
         out.getloc());
      std::mbstate_t state{};
      std::basic_string<CharT> converted(len * f.max_length(), 0);
      const Value* from_next;
      CharT* to_next;
      auto result = f.out(state,
         /* from */ value, value + len, from_next,
         /* to */ &converted[0], &converted[converted.size()], to_next);
      if (result == std::codecvt_base::ok) {
         converted.resize(to_next - &converted[0]);
         out << converted;
      } else {
         out.setstate(std::ios_base::failbit);
      }
   }
   return !!out;
}

/* formatted output of non-char pointers that
   have possibly a %p conversion */
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<!is_char<Value>::value, bool>::type
print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, const Value* value) {
   if (fseg.flags & is_pointer) {
      /* print the value of the pointer */
      out << static_cast<const void*>(value);
   } else if (fseg.flags & is_unsigned) {
      /* print it as an unsigned integer value */
      print_value(out, fseg, reinterpret_cast<std::uintptr_t>(value));
   } else if (fseg.flags & is_integer) {
      /* print it as a signed integer value */
      print_value(out, fseg, reinterpret_cast<std::intptr_t>(value));
   } else {
      out << value;
   }
   return !!out;
}

/* formatted output of non-const char pointers
   which are delegated to the const char pointer variants */
template<typename CharT, typename Traits, typename Value>
inline typename std::enable_if<is_char<Value>::value, bool>::type
print_value(std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg, Value* value) {
   return print_value(out, fseg, static_cast<const Value*>(value));
}

template<typename CharT, typename Traits>
struct process_value_f {
   process_value_f(std::basic_ostream<CharT, Traits>& out,
         const format_segment<CharT>& fseg) :
         out(out), fseg(fseg) {
   }
   template<typename Value>
   bool operator()(Value&& value) {
      return print_value(out, fseg, std::forward<Value>(value));
   }
   std::basic_ostream<CharT, Traits>& out;
   const format_segment<CharT>& fseg;
};

template<typename Tuple, typename CharT, typename Traits>
inline bool process_value(const Tuple& tuple, integer index,
      std::basic_ostream<CharT, Traits>& out,
      const format_segment<CharT>& fseg) {
   if (index >= 0 &&
         index < static_cast<integer>(std::tuple_size<Tuple>::value)) {
      return apply(tuple, index, process_value_f<CharT, Traits>(out, fseg));
   } else {
      return false;
   }
}

template<typename Value>
inline std::enable_if<std::is_integral<Value>::value &&
   !std::is_const<Value>::value, bool>
set_value(Value* ptr, std::streamsize value) {
   *ptr = value;
   return true;
}

template<typename Value>
inline bool set_value(Value, std::streamsize) {
   return false;
}

} // namespace impl

template<typename CharT, typename Traits, typename... Values>
inline int printf(std::basic_ostream<CharT, Traits>& out,
      const CharT* format) {
   impl::counting_ostream<CharT, Traits> cout(out);

   while (format) {
      auto fseg = impl::parse_format_segment(format, 0);
      if (!fseg.valid) return -1;
      if (fseg.nof_args > 0) return -1;
      cout.write(fseg.beginp, fseg.endp - fseg.beginp);
      format = fseg.nextp;
   }
   return cout.get_count();
}

template<typename CharT, typename Traits, typename... Values>
inline int printf(std::basic_ostream<CharT, Traits>& out,
      const CharT* format, Values&&... values) {
   impl::counting_ostream<CharT, Traits> cout(out);
   if (cout.getloc() != std::locale::classic()) {
      cout.imbue(std::locale(cout.getloc(), new impl::suppress_grouping()));
   }
   std::tuple<Values&...> tuple(values...);
   impl::integer nof_args = 0;
   while (format) {
      auto fseg = impl::parse_format_segment(format, nof_args);
      if (!fseg.valid) return -1;
      nof_args += fseg.nof_args;
      if (fseg.endp > fseg.beginp) {
         cout.write(fseg.beginp, fseg.endp - fseg.beginp);
         if (!cout) return -1;
      }
      if (fseg.value_index >= 0) {
         if (fseg.conversion == 'n') {
            if (impl::set_value(tuple, fseg.value_index,
                  cout.get_count()) < 0) {
               return -1;
            }
         } else {
            if (fseg.width_index >= 0) {
               fseg.width = impl::get_value(tuple, fseg.width_index);
            }
            if (fseg.precision_index >= 0) {
               fseg.precision = impl::get_value(tuple, fseg.precision_index);
            }
            impl::format_saver<CharT, Traits> fsaver(cout);
            cout.setf(fseg.fmtflags);
            cout.setf(fseg.base == 8? std::ios_base::oct  :
                      fseg.base == 10? std::ios_base::dec :
                      fseg.base == 16? std::ios_base::hex :
                      std::ios_base::fmtflags(0), std::ios_base::basefield);
            if (fseg.width > 0) {
               cout.width(fseg.width);
            }
            if ((fseg.flags & impl::precision) && fseg.precision >= 0) {
               cout.precision(fseg.precision);
            }
            if (fseg.flags & impl::grouping_flag) {
               cout.imbue(std::locale(cout.getloc(),
                  new impl::thousands_grouping()));
            }
            if (!process_value(tuple, fseg.value_index, cout, fseg)) {
               return -1;
            }
         }
      }
      format = fseg.nextp;
   }
   return cout.get_count();
}

template<typename... Values>
inline int printf(const char* format, Values&&... values) {
   return printf(std::cout, format, std::forward<Values>(values)...);
}

template<typename... Values>
inline int printf(const wchar_t* format, Values&&... values) {
   return printf(std::wcout, format, std::forward<Values>(values)...);
}

template<typename... Values>
inline int snprintf(char* s, std::size_t n,
      const char* format, Values&&... values) {
   std::ostringstream os;
   int nbytes = printf(os, format, std::forward<Values>(values)...);
   if (nbytes < 0) return nbytes;
   if (n == 0) return nbytes;
   std::string result(os.str());
   if (static_cast<std::size_t>(nbytes) + 1 <= n) {
      std::strcpy(s, result.c_str());
      return nbytes;
   } else {
      std::strcpy(s, result.substr(0, n-1).c_str());
      s[n-1] = 0;
      return n-1;
   }
}

template<typename... Values>
inline int snprintf(wchar_t* s, std::size_t n,
      const wchar_t* format, Values&&... values) {
   std::wostringstream os;
   int nbytes = printf(os, format, std::forward<Values>(values)...);
   if (nbytes < 0) return nbytes;
   if (n == 0) return nbytes;
   std::wstring result(os.str());
   if (static_cast<std::size_t>(nbytes) + 1 <= n) {
      std::wcscpy(s, result.c_str());
      return nbytes;
   } else {
      std::wcscpy(s, result.substr(0, n-1).c_str());
      s[n] = 0;
      return n-1;
   }
}

} // namespace fmt

#endif // of #if __cplusplus < 201103L #else ...
#endif // of #ifndef FMT_PRINTF_HPP
