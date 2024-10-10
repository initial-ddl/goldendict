/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "utf8.hh"
#include <vector>
#include <algorithm>
#include <QByteArray>
#include <QString>

namespace Utf8 {

size_t encode( wchar const * in, size_t inSize, char * out_ )
{
  unsigned char * out = (unsigned char *)out_;

  while ( inSize-- ) {
    if ( *in < 0x80 ) {
      *out++ = *in++;
    }
    else if ( *in < 0x800 ) {
      *out++ = 0xC0 | ( *in >> 6 );
      *out++ = 0x80 | ( *in++ & 0x3F );
    }
    else if ( *in < 0x10000 ) {
      *out++ = 0xE0 | ( *in >> 12 );
      *out++ = 0x80 | ( ( *in >> 6 ) & 0x3F );
      *out++ = 0x80 | ( *in++ & 0x3F );
    }
    else {
      *out++ = 0xF0 | ( *in >> 18 );
      *out++ = 0x80 | ( ( *in >> 12 ) & 0x3F );
      *out++ = 0x80 | ( ( *in >> 6 ) & 0x3F );
      *out++ = 0x80 | ( *in++ & 0x3F );
    }
  }

  return out - (unsigned char *)out_;
}

long decode( char const * in_, size_t inSize, wchar * out_ )
{
  unsigned char const * in = (unsigned char const *)in_;
  wchar * out              = out_;

  while ( inSize-- ) {
    wchar result;

    if ( *in & 0x80 ) {
      if ( *in & 0x40 ) {
        if ( *in & 0x20 ) {
          if ( *in & 0x10 ) {
            // Four-byte sequence
            if ( *in & 8 ) {
              // This can't be
              return -1;
            }

            if ( inSize < 3 ) {
              return -1;
            }

            inSize -= 3;

            result = ( (wchar)*in++ & 7 ) << 18;

            if ( ( *in & 0xC0 ) != 0x80 ) {
              return -1;
            }
            result |= ( (wchar)*in++ & 0x3F ) << 12;

            if ( ( *in & 0xC0 ) != 0x80 ) {
              return -1;
            }
            result |= ( (wchar)*in++ & 0x3F ) << 6;

            if ( ( *in & 0xC0 ) != 0x80 ) {
              return -1;
            }
            result |= (wchar)*in++ & 0x3F;
          }
          else {
            // Three-byte sequence

            if ( inSize < 2 ) {
              return -1;
            }

            inSize -= 2;

            result = ( (wchar)*in++ & 0xF ) << 12;

            if ( ( *in & 0xC0 ) != 0x80 ) {
              return -1;
            }
            result |= ( (wchar)*in++ & 0x3F ) << 6;

            if ( ( *in & 0xC0 ) != 0x80 ) {
              return -1;
            }
            result |= (wchar)*in++ & 0x3F;
          }
        }
        else {
          // Two-byte sequence
          if ( !inSize ) {
            return -1;
          }

          --inSize;

          result = ( (wchar)*in++ & 0x1F ) << 6;

          if ( ( *in & 0xC0 ) != 0x80 ) {
            return -1;
          }
          result |= (wchar)*in++ & 0x3F;
        }
      }
      else {
        // This char is from the middle of encoding, it can't be leading
        return -1;
      }
    }
    else {
      // One-byte encoding
      result = *in++;
    }

    *out++ = result;
  }

  return out - out_;
}

string encode( wstring const & in ) noexcept
{
  if ( in.empty() ) {
    return {};
  }

  std::vector< char > buffer( in.size() * 4 );

  return string( &buffer.front(), encode( in.data(), in.size(), &buffer.front() ) );
}

wstring decode( string const & in )
{
  if ( in.empty() ) {
    return {};
  }

  std::vector< wchar > buffer( in.size() );

  long result = decode( in.data(), in.size(), &buffer.front() );

  if ( result < 0 ) {
    throw exCantDecode( in );
  }

  return wstring( &buffer.front(), result );
}

bool isspace( int c )
{
  switch ( c ) {
    case ' ':
    case '\f':
    case '\n':
    case '\r':
    case '\t':
    case '\v':
      return true;

    default:
      return false;
  }
}

//get the first line in string s1. -1 if not found
int findFirstLinePosition( char * s1, int s1length, const char * s2, int s2length )
{
  char * pos = std::search( s1, s1 + s1length, s2, s2 + s2length );

  if ( pos == s1 + s1length ) {
    return pos - s1;
  }

  //the line size.
  return pos - s1 + s2length;
}

char const * getEncodingNameFor( Encoding e )
{
  switch ( e ) {
    case Utf32LE:
      return "UTF-32LE";
    case Utf32BE:
      return "UTF-32BE";
    case Utf16LE:
      return "UTF-16LE";
    case Utf16BE:
      return "UTF-16BE";
    case Windows1252:
      return "WINDOWS-1252";
    case Windows1251:
      return "WINDOWS-1251";
    case Utf8:
      return "UTF-8";
    case Windows1250:
      return "WINDOWS-1250";
    default:
      return "UTF-8";
  }
}

Encoding getEncodingForName( const QByteArray & _name )
{
  const auto name = _name.toUpper();
  if ( name == "UTF-32LE" ) {
    return Utf32LE;
  }
  if ( name == "UTF-32BE" ) {
    return Utf32BE;
  }
  if ( name == "UTF-16LE" ) {
    return Utf16LE;
  }
  if ( name == "UTF-16BE" ) {
    return Utf16BE;
  }
  if ( name == "WINDOWS-1252" ) {
    return Windows1252;
  }
  if ( name == "WINDOWS-1251" ) {
    return Windows1251;
  }
  if ( name == "UTF-8" ) {
    return Utf8;
  }
  if ( name == "WINDOWS-1250" ) {
    return Windows1250;
  }
  return Utf8;
}

LineFeed initLineFeed( const Encoding e )
{
  LineFeed lf{};
  switch ( e ) {
    case Utf8::Utf32LE:
      lf.lineFeed = new char[ 4 ]{ 0x0A, 0, 0, 0 };
      lf.length   = 4;
      break;
    case Utf8::Utf32BE:
      lf.lineFeed = new char[ 4 ]{ 0, 0, 0, 0x0A };
      lf.length   = 4;
      break;
    case Utf8::Utf16LE:
      lf.lineFeed = new char[ 2 ]{ 0x0A, 0 };
      lf.length   = 2;
      break;
    case Utf8::Utf16BE:
      lf.lineFeed = new char[ 2 ]{ 0, 0x0A };
      lf.length   = 2;
      break;
    case Utf8::Windows1252:

    case Utf8::Windows1251:

    case Utf8::Utf8:

    case Utf8::Windows1250:
    default:
      lf.length   = 1;
      lf.lineFeed = new char[ 1 ]{ 0x0A };
  }
  return lf;
}

} // namespace Utf8
