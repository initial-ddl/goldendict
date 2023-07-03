/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "dsl.hh"
#include "dsl_details.hh"
#include "btreeidx.hh"
#include "folding.hh"
#include "utf8.hh"
#include "chunkedstorage.hh"
#include "dictzip.hh"
#include "htmlescape.hh"
#include "iconv.hh"
#include "filetype.hh"

#include "audiolink.hh"
#include "langcoder.hh"
#include "wstring_qt.hh"
#include "indexedzip.hh"
#include "gddebug.hh"
#include "tiff.hh"
#include "ftshelpers.hh"

#include <map>
#include <set>
#include <string>
#include <vector>
#include <list>
#include <wctype.h>

#ifdef _MSC_VER
#include <stub_msvc.h>
#endif

#include <QSemaphore>
#include <QThreadPool>
#include <QAtomicInt>
#include <QUrl>

#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QStringList>

#include <QRegularExpression>

// For TIFF conversion
#include <QImage>
#include <QByteArray>
#include <QBuffer>

// For SVG handling
#include <QtSvg/QSvgRenderer>

#include <QtConcurrent>

#include "utils.hh"

namespace Dsl {

using namespace Details;

using std::map;
using std::multimap;
using std::pair;
using std::set;
using std::string;
using gd::wstring;
using gd::wchar;
using std::vector;
using std::list;
using Utf8::Encoding;

using BtreeIndexing::WordArticleLink;
using BtreeIndexing::IndexedWords;
using BtreeIndexing::IndexInfo;

namespace {

DEF_EX( exUserAbort, "User abort", Dictionary::Ex )
DEF_EX_STR( exDictzipError, "DICTZIP error", Dictionary::Ex )

enum
{
  Signature = 0x584c5344, // DSLX on little-endian, XLSD on big-endian
  CurrentFormatVersion = 23 + BtreeIndexing::FormatVersion + Folding::Version,
  CurrentZipSupportVersion = 2,
  CurrentFtsIndexVersion = 7
};

struct IdxHeader
{
  uint32_t signature; // First comes the signature, DSLX
  uint32_t formatVersion; // File format version (CurrentFormatVersion)
  uint32_t zipSupportVersion; // Zip support version -- narrows down reindexing
                              // when it changes only for dictionaries with the
                              // zip files
  int dslEncoding; // Which encoding is used for the file indexed
  uint32_t chunksOffset; // The offset to chunks' storage
  uint32_t hasAbrv; // Non-zero means file has abrvs at abrvAddress
  uint32_t abrvAddress; // Address of abrv map in the chunked storage
  uint32_t indexBtreeMaxElements; // Two fields from IndexInfo
  uint32_t indexRootOffset;
  uint32_t articleCount; // Number of articles this dictionary has
  uint32_t wordCount; // Number of headwords this dictionary has
  uint32_t langFrom;  // Source language
  uint32_t langTo;    // Target language
  uint32_t hasZipFile; // Non-zero means there's a zip file with resources
                       // present
  uint32_t hasSoundDictionaryName;
  uint32_t zipIndexBtreeMaxElements; // Two fields from IndexInfo of the zip
                                     // resource index.
  uint32_t zipIndexRootOffset;
}
#ifndef _MSC_VER
__attribute__((packed))
#endif
;

struct InsidedCard
{
  uint32_t offset;
  uint32_t size;
  QVector< wstring > headwords;
  InsidedCard( uint32_t _offset, uint32_t _size, QVector< wstring > const & words ) :
  offset( _offset ), size( _size ), headwords( words )
  {}
  InsidedCard() = default;
};

bool indexIsOldOrBad( string const & indexFile, bool hasZipFile )
{
  File::Class idx( indexFile, "rb" );

  IdxHeader header;

  return idx.readRecords( &header, sizeof( header ), 1 ) != 1 ||
         header.signature != Signature ||
         header.formatVersion != CurrentFormatVersion ||
         (bool) header.hasZipFile != hasZipFile ||
         ( hasZipFile && header.zipSupportVersion != CurrentZipSupportVersion );
}

class DslDictionary: public BtreeIndexing::BtreeDictionary
{
  QMutex idxMutex;
  File::Class idx;
  IdxHeader idxHeader;
  sptr< ChunkedStorage::Reader > chunks;
  string preferredSoundDictionary;
  map< string, string > abrv;
  QMutex dzMutex;
  dictData * dz;
  QMutex resourceZipMutex;
  IndexedZip resourceZip;
  BtreeIndex resourceZipIndex;

  QAtomicInt deferredInitDone;
  QMutex deferredInitMutex;
  bool deferredInitRunnableStarted;
  QSemaphore deferredInitRunnableExited;

  string initError;

  int optionalPartNom;
  quint8 articleNom;
  int maxPictureWidth;

  wstring currentHeadword;
  string resourceDir1, resourceDir2;

public:

  DslDictionary( string const & id, string const & indexFile,
                 vector< string > const & dictionaryFiles,
                 int maxPictureWidth_ );

  void deferredInit() override;

  ~DslDictionary();

  string getName() noexcept override
  { return dictionaryName; }

  map< Dictionary::Property, string > getProperties() noexcept override
  { return map< Dictionary::Property, string >(); }

  unsigned long getArticleCount() noexcept override
  { return idxHeader.articleCount; }

  unsigned long getWordCount() noexcept override
  { return idxHeader.wordCount; }

  inline quint32 getLangFrom() const override
  { return idxHeader.langFrom; }

  inline quint32 getLangTo() const override
  { return idxHeader.langTo; }

  inline virtual string getResourceDir1() const
  { return resourceDir1; }

  inline virtual string getResourceDir2() const
  { return resourceDir2; }



  sptr< Dictionary::DataRequest > getArticle( wstring const &,
                                                      vector< wstring > const & alts,
                                                      wstring const &,
                                                      bool ignoreDiacritics ) override
    ;

  sptr< Dictionary::DataRequest > getResource( string const & name ) override
    ;

  sptr< Dictionary::DataRequest >
  getSearchResults( QString const & searchString, int searchMode, bool matchCase, bool ignoreDiacritics ) override;
  QString const& getDescription() override;

  QString getMainFilename() override;

  void getArticleText( uint32_t articleAddress, QString & headword, QString & text ) override;

  void makeFTSIndex(QAtomicInt & isCancelled, bool firstIteration ) override;

  void setFTSParameters( Config::FullTextSearch const & fts ) override
  {
    if( ensureInitDone().size() )
      return;

    can_FTS = fts.enabled
              && !fts.disabledTypes.contains( "DSL", Qt::CaseInsensitive )
              && ( fts.maxDictionarySize == 0 || getArticleCount() <= fts.maxDictionarySize );
  }

  uint32_t getFtsIndexVersion() override
  { return CurrentFtsIndexVersion; }

protected:

  void loadIcon() noexcept override;

private:

  string const & ensureInitDone() override;
  void doDeferredInit();

  /// Loads the article. Does not process the DSL language.
  void loadArticle( uint32_t address,
                    wstring const & requestedHeadwordFolded,
                    bool ignoreDiacritics,
                    wstring & tildeValue,
                    wstring & displayedHeadword,
                    unsigned & headwordIndex,
                    wstring & articleText );

  /// Converts DSL language to an Html.
  string dslToHtml( wstring const &, wstring const & headword = wstring() );

  // Parts of dslToHtml()
  string nodeToHtml( ArticleDom::Node const & );
  string processNodeChildren( ArticleDom::Node const & node );
  string getNodeLink( ArticleDom::Node const & node );

  bool hasHiddenZones()           /// Return true if article has hidden zones
  { return optionalPartNom != 0; }

  friend class DslArticleRequest;
  friend class DslResourceRequest;
  friend class DslFTSResultsRequest;
};

DslDictionary::DslDictionary( string const & id,
                              string const & indexFile,
                              vector< string > const & dictionaryFiles,
                              int maxPictureWidth_ ):
  BtreeDictionary( id, dictionaryFiles ),
  idx( indexFile, "rb" ),
  idxHeader( idx.read< IdxHeader >() ),
  dz( 0 ),
  deferredInitRunnableStarted( false ),
  optionalPartNom( 0 ),
  articleNom( 0 ),
  maxPictureWidth( maxPictureWidth_ )
{
  can_FTS = true;

  ftsIdxName = indexFile + Dictionary::getFtsSuffix();

  if ( !Dictionary::needToRebuildIndex( dictionaryFiles, ftsIdxName ) && !FtsHelpers::ftsIndexIsOldOrBad( this ) )
    FTS_index_completed.ref();

  // Read the dictionary name

  idx.seek( sizeof( idxHeader ) );

  vector< char > dName( idx.read< uint32_t >() );
  if( dName.size() > 0 )
  {
    idx.read( &dName.front(), dName.size() );
    dictionaryName = string( &dName.front(), dName.size() );
  }

  vector< char > sName( idx.read< uint32_t >() );
  if( sName.size() > 0 )
  {
    idx.read( &sName.front(), sName.size() );
    preferredSoundDictionary = string( &sName.front(), sName.size() );
  }

  resourceDir1 = getDictionaryFilenames()[ 0 ] + ".files" + Utils::Fs::separator();
  QString s    = QString::fromStdString( getDictionaryFilenames()[ 0 ] );
  if( s.endsWith( QString::fromLatin1( ".dz" ), Qt::CaseInsensitive ) )
    s.chop( 3 );
  resourceDir2 = s.toStdString() + ".files" + Utils::Fs::separator();

  // Everything else would be done in deferred init
}

DslDictionary::~DslDictionary()
{
  QMutexLocker _( &deferredInitMutex );

  // Wait for init runnable to complete if it was ever started
  // if ( deferredInitRunnableStarted )
  //   deferredInitRunnableExited.acquire();

  if ( dz )
    dict_data_close( dz );
}

//////// DslDictionary::deferredInit()

void DslDictionary::deferredInit()
{
  if ( !Utils::AtomicInt::loadAcquire( deferredInitDone ) )
  {
    QMutexLocker _( &deferredInitMutex );

    if ( Utils::AtomicInt::loadAcquire( deferredInitDone ) )
      return;

    if ( !deferredInitRunnableStarted )
    {
      QThreadPool::globalInstance()->start( [ this ]() { this->doDeferredInit(); }, -1000 );
      deferredInitRunnableStarted = true;
    }
  }
}


string const & DslDictionary::ensureInitDone()
{
  // Simple, really.
  doDeferredInit();

  return initError;
}

void DslDictionary::doDeferredInit()
{
  if ( !Utils::AtomicInt::loadAcquire( deferredInitDone ) )
  {
    QMutexLocker _( &deferredInitMutex );

    if ( Utils::AtomicInt::loadAcquire( deferredInitDone ) )
      return;

    // Do deferred init

    try
    {
      // Don't lock index file - no one should be working with it until
      // the init is complete.
      //QMutexLocker _( &idxMutex );

      chunks = std::shared_ptr<ChunkedStorage::Reader>(new ChunkedStorage::Reader(idx, idxHeader.chunksOffset));

      // Open the .dsl file

      DZ_ERRORS error;
      dz = dict_data_open( getDictionaryFilenames()[ 0 ].c_str(), &error, 0 );

      if ( !dz )
        throw exDictzipError( string( dz_error_str( error ) )
                              + "(" + getDictionaryFilenames()[ 0 ] + ")" );

      // Read the abrv, if any

      if ( idxHeader.hasAbrv )
      {
        vector< char > chunk;

        char * abrvBlock = chunks->getBlock( idxHeader.abrvAddress, chunk );

        uint32_t total;
        memcpy( &total, abrvBlock, sizeof( uint32_t ) );
        abrvBlock += sizeof( uint32_t );

        GD_DPRINTF( "Loading %u abbrv\n", total );

        while( total-- )
        {
          uint32_t keySz;
          memcpy( &keySz, abrvBlock, sizeof( uint32_t ) );
          abrvBlock += sizeof( uint32_t );

          char * key = abrvBlock;

          abrvBlock += keySz;

          uint32_t valueSz;
          memcpy( &valueSz, abrvBlock, sizeof( uint32_t ) );
          abrvBlock += sizeof( uint32_t );

          abrv[ string( key, keySz ) ] = string( abrvBlock, valueSz );

          abrvBlock += valueSz;
        }
      }

      // Initialize the index

      openIndex( IndexInfo( idxHeader.indexBtreeMaxElements,
                            idxHeader.indexRootOffset ),
                 idx, idxMutex );

      // Open a resource zip file, if there's one

      if ( idxHeader.hasZipFile &&
           ( idxHeader.zipIndexBtreeMaxElements ||
             idxHeader.zipIndexRootOffset ) )
      {
        resourceZip.openIndex( IndexInfo( idxHeader.zipIndexBtreeMaxElements,
                                          idxHeader.zipIndexRootOffset ),
                               idx, idxMutex );

        QString zipName = QDir::fromNativeSeparators( getDictionaryFilenames().back().c_str() );

        if ( zipName.endsWith( ".zip", Qt::CaseInsensitive ) ) // Sanity check
          resourceZip.openZipFile( zipName );
      }
    }
    catch( std::exception & e )
    {
      initError = e.what();
    }
    catch( ... )
    {
      initError = "Unknown error";
    }

    deferredInitDone.ref();
  }
}


void DslDictionary::loadIcon() noexcept
{
  if ( dictionaryIconLoaded )
    return;

  QString fileName = QDir::fromNativeSeparators( getDictionaryFilenames()[ 0 ].c_str() );

  // Remove the extension
  if ( fileName.endsWith( ".dsl.dz", Qt::CaseInsensitive ) )
    fileName.chop( 6 );
  else
    fileName.chop( 3 );

  if ( !loadIconFromFile( fileName ) )
  {
    // Load failed -- use default icons
    dictionaryIcon = QIcon(":/icons/icon32_dsl.png");
  }

  dictionaryIconLoaded = true;
}

/// Determines whether or not this char is treated as whitespace for dsl
/// parsing or not. We can't rely on any Unicode standards here, since the
/// only standard that matters here is the original Dsl compiler's insides.
/// Some dictionaries, for instance, are known to specifically use a non-
/// breakable space (0xa0) to indicate that a headword begins with a space,
/// so nbsp is not a whitespace character for Dsl compiler.
/// For now we have only space and tab, since those are most likely the only
/// ones recognized as spaces by that compiler.
bool isDslWs( wchar ch )
{
  switch( ch )
  {
    case ' ':
    case '\t':
      return true;
    default:
      return false;
  }
}

void DslDictionary::loadArticle( uint32_t address,
                                 wstring const & requestedHeadwordFolded,
                                 bool ignoreDiacritics,
                                 wstring & tildeValue,
                                 wstring & displayedHeadword,
                                 unsigned & headwordIndex,
                                 wstring & articleText )
{
  wstring articleData;

  {
    vector< char > chunk;

    char * articleProps;

    {
      QMutexLocker _( &idxMutex );

      articleProps = chunks->getBlock( address, chunk );
    }

    uint32_t articleOffset, articleSize;

    memcpy( &articleOffset, articleProps, sizeof( articleOffset ) );
    memcpy( &articleSize, articleProps + sizeof( articleOffset ),
            sizeof( articleSize ) );

    GD_DPRINTF( "offset = %x\n", articleOffset );


    char * articleBody;

    {
      QMutexLocker _( &dzMutex );

      articleBody = dict_data_read_( dz, articleOffset, articleSize, 0, 0 );
    }

    if ( !articleBody )
    {
//      throw exCantReadFile( getDictionaryFilenames()[ 0 ] );
articleData =  U"\n\r\t"  + gd::toWString( QString( "DICTZIP error: " ) + dict_error_str( dz ) );
    }
    else
    {
      try
      {
        articleData =
          Iconv::toWstring(
            Utf8::getEncodingNameFor( Encoding( idxHeader.dslEncoding ) ),
            articleBody, articleSize );
        free( articleBody );

        // Strip DSL comments
        bool b = false;
        stripComments( articleData, b );
      }
      catch( ... )
      {
        free( articleBody );
        throw;
      }
    }
  }

  size_t pos = 0;
  bool hadFirstHeadword = false;
  bool foundDisplayedHeadword = false;

  // Check is we retrieve insided card
  bool insidedCard = isDslWs( articleData.at( 0 ) );

  wstring tildeValueWithUnsorted; // This one has unsorted parts left
  for( headwordIndex = 0; ; )
  {
    size_t begin = pos;

    pos = articleData.find_first_of(  U"\n\r" , begin );

    if ( pos == wstring::npos )
      pos = articleData.size();

    if ( !foundDisplayedHeadword )
    {
      // Process the headword

      wstring rawHeadword = wstring( articleData, begin, pos - begin );

      if( insidedCard && !rawHeadword.empty() && isDslWs( rawHeadword[ 0 ] ) )
      {
        // Headword of the insided card
        wstring::size_type hpos = rawHeadword.find( L'@' );
        if( hpos != string::npos )
        {
          wstring head = Folding::trimWhitespace( rawHeadword.substr( hpos + 1 ) );
          hpos = head.find( L'~' );
          while( hpos != string::npos )
          {
            if( hpos == 0 || head[ hpos ] != L'\\' )
              break;
            hpos = head.find( L'~', hpos + 1 );
          }
          if( hpos == string::npos )
            rawHeadword = head;
          else
            rawHeadword.clear();
        }
      }

      if( !rawHeadword.empty() )
      {
        if ( !hadFirstHeadword )
        {
          // We need our tilde expansion value
          tildeValue = rawHeadword;

          list< wstring > lst;

          expandOptionalParts( tildeValue, &lst );

          if ( lst.size() ) // Should always be
            tildeValue = lst.front();

          tildeValueWithUnsorted = tildeValue;

          processUnsortedParts( tildeValue, false );
        }
        wstring str = rawHeadword;

        if ( hadFirstHeadword )
          expandTildes( str, tildeValueWithUnsorted );

        processUnsortedParts( str, true );

        str = Folding::applySimpleCaseOnly( str );

        list< wstring > lst;
        expandOptionalParts( str, &lst );

        // Does one of the results match the requested word? If so, we'd choose
        // it as our headword.

        for ( auto & i : lst ) {
          unescapeDsl( i );
          normalizeHeadword( i );

          bool found;
          if( ignoreDiacritics )
            found = Folding::applyDiacriticsOnly( Folding::trimWhitespace( i ) ) == Folding::applyDiacriticsOnly( requestedHeadwordFolded );
          else
            found = Folding::trimWhitespace( i ) == requestedHeadwordFolded;

          if ( found )
          {
            // Found it. Now we should make a displayed headword for it.
            if ( hadFirstHeadword )
              expandTildes( rawHeadword, tildeValueWithUnsorted );

            processUnsortedParts( rawHeadword, false );

            displayedHeadword = rawHeadword;

            foundDisplayedHeadword = true;
            break;
          }
        }

        if ( !foundDisplayedHeadword )
        {
          ++headwordIndex;
          hadFirstHeadword = true;
        }
      }
    }


    if ( pos == articleData.size() )
      break;

    // Skip \n\r

    if ( articleData[ pos ] == '\r' )
      ++pos;

    if ( pos != articleData.size() )
    {
      if ( articleData[ pos ] == '\n' )
        ++pos;
    }

    if ( pos == articleData.size() )
    {
      // Ok, it's end of article
      break;
    }
    if( isDslWs( articleData[ pos ] ) )
    {
     // Check for begin article text
      if( insidedCard )
      {
        // Check for next insided headword
        wstring::size_type hpos = articleData.find_first_of(  U"\n\r" , pos );
        if ( hpos == wstring::npos )
          hpos = articleData.size();

        wstring str = wstring( articleData, pos, hpos - pos );

        hpos = str.find( L'@');
        if( hpos == wstring::npos || str[ hpos - 1 ] == L'\\' || !isAtSignFirst( str ) )
          break;
      }
      else
        break;
    }
  }

  if ( !foundDisplayedHeadword )
  {
    // This is strange. Anyway, use tilde expansion value, it's better
    // than nothing (or requestedHeadwordFolded for insided card.
    if( insidedCard )
      displayedHeadword = requestedHeadwordFolded;
    else
      displayedHeadword = tildeValue;
  }

  if ( pos != articleData.size() )
    articleText = wstring( articleData, pos );
  else
    articleText.clear();
}

string DslDictionary::dslToHtml( wstring const & str, wstring const & headword )
{
 // Normalize the string
  wstring normalizedStr = gd::normalize( str );
  currentHeadword = headword;

  ArticleDom dom( normalizedStr, getName(), headword );

  optionalPartNom = 0;

  string html = processNodeChildren( dom.root );

  return html;
}

string DslDictionary::processNodeChildren( ArticleDom::Node const & node )
{
  string result;

  for ( const auto & i : node )
    result += nodeToHtml( i );

  return result;
}

string DslDictionary::getNodeLink( ArticleDom::Node const & node )
{
  string link;
  if ( !node.tagAttrs.empty() ) {
    QString attrs = QString::fromStdU32String( node.tagAttrs );
    int n         = attrs.indexOf( "target=\"" );
    if ( n >= 0 ) {
      int n_end      = attrs.indexOf( '\"', n + 8 );
      QString target = attrs.mid( n + 8, n_end > n + 8 ? n_end - ( n + 8 ) : -1 );
      link           = Html::escape( Filetype::simplifyString( string( target.toUtf8().data() ), false ) );
    }
  }
  if ( link.empty() )
    link = Html::escape( Filetype::simplifyString( Utf8::encode( node.renderAsText() ), false ) );

  return link;
}

string DslDictionary::nodeToHtml( ArticleDom::Node const & node )
{
  string result;

  if ( !node.isTag )
  {
    result = Html::escape( Utf8::encode( node.text ) );

    // Handle all end-of-line

    string::size_type n;

    // Strip all '\r'
    while( ( n = result.find( '\r' ) ) != string::npos )
      result.erase( n, 1 );

    // Replace all '\n'
    while( ( n = result.find( '\n' ) ) != string::npos )
      result.replace( n, 1, "<p></p>" );

    return result;
  }

  if( node.tagName ==  U"b"  )
    result += "<b class=\"dsl_b\">" + processNodeChildren( node ) + "</b>";
  else if( node.tagName ==  U"i"  )
    result += "<i class=\"dsl_i\">" + processNodeChildren( node ) + "</i>";
  else if( node.tagName ==  U"u"  )
  {
    string nodeText = processNodeChildren( node );

    if ( nodeText.size() && isDslWs( nodeText[ 0 ] ) )
      result.push_back( ' ' ); // Fix a common problem where in "foo[i] bar[/i]"
                               // the space before "bar" gets underlined.

    result += "<span class=\"dsl_u\">" + nodeText + "</span>";
  }
  else if( node.tagName ==  U"c"  )
  {
    if( node.tagAttrs.empty() )
    {
      result += "<span class=\"c_default_color\">"
        + processNodeChildren( node ) + "</span>";
    }
    else
    {
      result += "<font color=\""
        + Html::escape( Utf8::encode( node.tagAttrs ) ) + "\">"
        + processNodeChildren( node ) + "</font>";
    }
  }
  else if( node.tagName ==  U"*"  )
  {
      string id = "O" + getId().substr( 0, 7 ) + "_" +
                QString::number( articleNom ).toStdString() +
                "_opt_" + QString::number( optionalPartNom++ ).toStdString();
    result += R"(<span class="dsl_opt" id=")" + id + "\">" + processNodeChildren( node ) + "</span>";
  }
  else if( node.tagName ==  U"m"  )
    result += "<div class=\"dsl_m\">" + processNodeChildren( node ) + "</div>";
  else
  if ( node.tagName.size() == 2 && node.tagName[ 0 ] == L'm' &&
       iswdigit( node.tagName[ 1 ] ) )
    result += "<div class=\"dsl_" + Utf8::encode( node.tagName ) + "\">" + processNodeChildren( node ) + "</div>";
  else if( node.tagName ==  U"trn"  )
    result += "<span class=\"dsl_trn\">" + processNodeChildren( node ) + "</span>";
  else if( node.tagName ==  U"ex"  )
    result += "<span class=\"dsl_ex\">" + processNodeChildren( node ) + "</span>";
  else if( node.tagName ==  U"com"  )
    result += "<span class=\"dsl_com\">" + processNodeChildren( node ) + "</span>";
  else if( node.tagName ==  U"s"  || node.tagName ==  U"video"  )
  {
    string filename = Filetype::simplifyString( Utf8::encode( node.renderAsText() ), false );
    string n        = resourceDir1 + filename;

    if ( Filetype::isNameOfSound( filename ) )
    {
      // If we have the file here, do the exact reference to this dictionary.
      // Otherwise, make a global 'search' one.

      bool search = !File::exists( n ) && !File::exists( resourceDir2 + filename )
        && !File::exists( getContainingFolder().toStdString() + Utils::Fs::separator() + filename )
        && ( !resourceZip.isOpen() || !resourceZip.hasFile( Utf8::decode( filename ) ) );

      QUrl url;
      url.setScheme( "gdau" );
      url.setHost( QString::fromUtf8( search ? "search" : getId().c_str() ) );
      url.setPath( Utils::Url::ensureLeadingSlash( QString::fromUtf8( filename.c_str() ) ) );
      if( search && idxHeader.hasSoundDictionaryName )
        Utils::Url::setFragment( url, QString::fromUtf8( preferredSoundDictionary.c_str() ) );

      string ref = string( "\"" ) + url.toEncoded().data() + "\"";

      result += addAudioLink( ref, getId() );

      result += "<span class=\"dsl_s_wav\"><a href=" + ref + R"(><img src="qrc:///icons/playsound.png" border="0" align="absmiddle" alt="Play"/></a></span>)";
    }
    else
    if ( Filetype::isNameOfPicture( filename ) )
    {
      QUrl url;
      url.setScheme( "bres" );
      url.setHost( QString::fromUtf8( getId().c_str() ) );
      url.setPath( Utils::Url::ensureLeadingSlash( QString::fromUtf8( filename.c_str() ) ) );

      vector< char > imgdata;
      bool resize = false;

      try
      {
        File::loadFromFile( n, imgdata );
      }
      catch( File::exCantOpen & )
      {
        try
        {
          n = resourceDir2 + filename;
          File::loadFromFile( n, imgdata );
        }
        catch( File::exCantOpen & )
        {
          try
          {
            n = getContainingFolder().toStdString() + Utils::Fs::separator() + filename;
            File::loadFromFile( n, imgdata );
          }
          catch( File::exCantOpen & )
          {
            // Try reading from zip file
            if ( resourceZip.isOpen() )
            {
              QMutexLocker _( &resourceZipMutex );
              resourceZip.loadFile( Utf8::decode( filename ), imgdata );
            }
          }
        }
      }
      catch(...)
      {
      }

      if( !imgdata.empty() )
      {
        if( Filetype::isNameOfSvg( filename ) )
        {
          // We don't need to render svg file now

          QSvgRenderer svg;
          svg.load( QByteArray::fromRawData( imgdata.data(), imgdata.size() ) );
          if( svg.isValid() )
          {
            QSize imgsize = svg.defaultSize();
            resize = maxPictureWidth > 0
                     && imgsize.width() > maxPictureWidth;
          }
        }
        else
        {
          QImage img = QImage::fromData( (unsigned char *) &imgdata.front(),
                                         imgdata.size() );

          resize = maxPictureWidth > 0
                   && img.width() > maxPictureWidth;
        }
      }

      if( resize )
      {
        string link( url.toEncoded().data() );
        link.replace( 0, 4, "gdpicture" );
        result += string( "<a href=\"" ) + link + "\">"
                          + "<img src=\"" + url.toEncoded().data()
                          + "\" alt=\"" + Html::escape( filename ) + "\""
                          + "width=\"" + QString::number( maxPictureWidth).toStdString() + "\"/>"
                          + "</a>";
      }
      else
        result += string( "<img src=\"" ) + url.toEncoded().data()
                  + "\" alt=\"" + Html::escape( filename ) + "\"/>";
    }
    else
    if ( Filetype::isNameOfVideo( filename ) ) {
      QUrl url;
      url.setScheme( "gdvideo" );
      url.setHost( QString::fromUtf8( getId().c_str() ) );
      url.setPath( Utils::Url::ensureLeadingSlash( QString::fromUtf8( filename.c_str() ) ) );

      result += string( R"(<a class="dsl_s dsl_video" href=")" ) + url.toEncoded().data() + "\">"
             + "<span class=\"img\"></span>"
             + "<span class=\"filename\">" + processNodeChildren( node ) + "</span>" + "</a>";
    }
    else
    {
      // Unknown file type, downgrade to a hyperlink

      QUrl url;
      url.setScheme( "bres" );
      url.setHost( QString::fromUtf8( getId().c_str() ) );
      url.setPath( Utils::Url::ensureLeadingSlash( QString::fromUtf8( filename.c_str() ) ) );

      result += string( R"(<a class="dsl_s" href=")" ) + url.toEncoded().data()
             + "\">" + processNodeChildren( node ) + "</a>";
    }
  }
  else if( node.tagName ==  U"url"  )
  {
    string link = getNodeLink( node );
    if ( QUrl::fromEncoded( link.c_str() ).scheme().isEmpty() )
      link = "http://" + link;

    QUrl url( QString::fromUtf8( link.c_str() ) );
    if( url.isLocalFile() && url.host().isEmpty() )
    {
      // Convert relative links to local files to absolute ones
      QString name = QFileInfo( getMainFilename() ).absolutePath();
      name += url.toLocalFile();
      QFileInfo info( name );
      if( info.isFile() )
      {
        name = info.canonicalFilePath();
        url.setPath( Utils::Url::ensureLeadingSlash( QUrl::fromLocalFile( name ).path() ) );
        link = string( url.toEncoded().data() );
      }
    }

    result += R"(<a class="dsl_url" href=")" + link +"\">" + processNodeChildren( node ) + "</a>";
  }
  else if( node.tagName ==  U"!trs"  )
  {
    result += "<span class=\"dsl_trs\">" + processNodeChildren( node ) + "</span>";
  }
  else if( node.tagName ==  U"p"  )
  {
    result += "<span class=\"dsl_p\"";

    string val = Utf8::encode( node.renderAsText() );

    // If we have such a key, display a title

    auto i = abrv.find( val );

    if ( i != abrv.end() )
    {
      string title = i->second;

      result += " title=\"" + Html::escape( title ) + "\"";
    }

    result += ">" + processNodeChildren( node ) + "</span>";
  }
  else if( node.tagName ==  U"'"  )
  {
    // There are two ways to display the stress: by adding an accent sign or via font styles.
    // We generate two spans, one with accented data and another one without it, so the
    // user could pick up the best suitable option.
    string data = processNodeChildren( node );
    result += R"(<span class="dsl_stress"><span class="dsl_stress_without_accent">)" + data + "</span>"
        + "<span class=\"dsl_stress_with_accent\">" + data + Utf8::encode( wstring( 1, 0x301 ) )
        + "</span></span>";
  }
  else if( node.tagName ==  U"lang"  )
  {
    result += "<span class=\"dsl_lang\"";
    if( !node.tagAttrs.empty() )
    {
      // Find ISO 639-1 code
      string langcode;
      QString attr = QString::fromStdU32String( node.tagAttrs );
      int n = attr.indexOf( "id=" );
      if( n >= 0 )
      {
        int id = attr.mid( n + 3 ).toInt();
        if( id )
          langcode = findCodeForDslId( id );
      }
      else
      {
        n = attr.indexOf( "name=\"" );
        if( n >= 0 )
        {
          int n2 = attr.indexOf( '\"', n + 6 );
          if( n2 > 0 )
          {
            quint32 id = dslLanguageToId( gd::toWString( attr.mid( n + 6, n2 - n - 6 ) ) );
            langcode = LangCoder::intToCode2( id ).toStdString();
          }
        }
      }
      if( !langcode.empty() )
        result += " lang=\"" + langcode + "\"";
    }
    result += ">" + processNodeChildren( node ) + "</span>";
  }
  else if( node.tagName ==  U"ref"  )
  {
    QUrl url;

    url.setScheme( "gdlookup" );
    url.setHost( "localhost" );
    auto nodeStr = Utf8::decode( getNodeLink( node ) );

    normalizeHeadword( nodeStr );
    url.setPath( Utils::Url::ensureLeadingSlash( QString::fromStdU32String( nodeStr ) ) );
    if( !node.tagAttrs.empty() )
    {
      QString attr = QString::fromStdU32String( node.tagAttrs ).remove( '\"' );
      int n = attr.indexOf( '=' );
      if( n > 0 )
      {
        QList< QPair< QString, QString > > query;
        query.append( QPair< QString, QString >( attr.left( n ), attr.mid( n + 1 ) ) );
        Utils::Url::setQueryItems( url, query );
      }
    }

    result += string( R"(<a class="dsl_ref" href=")" ) + url.toEncoded().data() +"\">"
              + processNodeChildren( node ) + "</a>";
  }
  else if( node.tagName ==  U"@"  )
  {
    // Special case - insided card header was not parsed

    QUrl url;

    url.setScheme( "gdlookup" );
    url.setHost( "localhost" );
    wstring nodeStr = node.renderAsText();
    normalizeHeadword( nodeStr );
    url.setPath( Utils::Url::ensureLeadingSlash( QString::fromStdU32String( nodeStr ) ) );

    result += string( R"(<a class="dsl_ref" href=")" ) + url.toEncoded().data() +"\">"
              + processNodeChildren( node ) + "</a>";
  }
  else if( node.tagName ==  U"sub"  )
  {
    result += "<sub>" + processNodeChildren( node ) + "</sub>";
  }
  else if( node.tagName ==  U"sup"  )
  {
    result += "<sup>" + processNodeChildren( node ) + "</sup>";
  }
  else if( node.tagName ==  U"t"  )
  {
    result += "<span class=\"dsl_t\">" + processNodeChildren( node ) + "</span>";
  }
  else if( node.tagName ==  U"br"  )
  {
    result += "<br />";
  }
  else
  {
    gdWarning( R"(DSL: Unknown tag "%s" with attributes "%s" found in "%s", article "%s".)",
               QString::fromStdU32String( node.tagName ).toUtf8().data(), QString::fromStdU32String( node.tagAttrs ).toUtf8().data(),
               getName().c_str(), QString::fromStdU32String( currentHeadword ).toUtf8().data() );

    result += "<span class=\"dsl_unknown\">[" + string( QString::fromStdU32String( node.tagName ).toUtf8().data() );
    if( !node.tagAttrs.empty() )
      result += " " + string( QString::fromStdU32String( node.tagAttrs ).toUtf8().data() );
    result += "]" + processNodeChildren( node ) + "</span>";
  }

  return result;
}

QString const& DslDictionary::getDescription()
{
    if( !dictionaryDescription.isEmpty() )
        return dictionaryDescription;

    dictionaryDescription = "NONE";

    QString fileName = QDir::fromNativeSeparators( getDictionaryFilenames()[ 0 ].c_str() );

    // Remove the extension
    if ( fileName.endsWith( ".dsl.dz", Qt::CaseInsensitive ) )
      fileName.chop( 6 );
    else
      fileName.chop( 3 );

    fileName += "ann";
    QFileInfo info( fileName );

    if ( info.exists() )
    {
        QFile annFile( fileName );
        if( !annFile.open( QFile::ReadOnly | QFile::Text ) )
            return dictionaryDescription;

        QTextStream annStream( &annFile );
        QString data, str;

        str = annStream.readLine();

        if( str.left( 10 ).compare( "#LANGUAGE " ) != 0 )
        {
            annStream.seek( 0 );
            dictionaryDescription = annStream.readAll();
        }
        else
        {
            // Multilanguage annotation

            qint32 gdLang, annLang;
            QString langStr;
            gdLang = LangCoder::code2toInt( QLocale::system().name().left( 2 ).toLatin1().data() );
            for(;;)
            {
                data.clear();
                langStr = str.mid( 10 ).replace( '\"', ' ').trimmed();
                annLang = LangCoder::findIdForLanguage( gd::toWString( langStr ) );
                do
                {
                    str = annStream.readLine();
                    if( str.left( 10 ).compare( "#LANGUAGE " ) == 0 )
                        break;
                    if( !str.endsWith( '\n' ) )
                        str.append( '\n' );
                    data += str;
                }
                while ( !annStream.atEnd() );
                if( dictionaryDescription.compare( "NONE ") == 0 || langStr.compare( "English", Qt::CaseInsensitive ) == 0 || gdLang == annLang )
                    dictionaryDescription = data.trimmed();
                if( gdLang == annLang || annStream.atEnd() )
                    break;
            }
        }
    }
    return dictionaryDescription;
}

QString DslDictionary::getMainFilename() { return getDictionaryFilenames()[ 0 ].c_str(); }

void DslDictionary::makeFTSIndex( QAtomicInt & isCancelled, bool firstIteration )
{
  if ( !( Dictionary::needToRebuildIndex( getDictionaryFilenames(), ftsIdxName )
          || FtsHelpers::ftsIndexIsOldOrBad( this ) ) )
    FTS_index_completed.ref();


  if ( haveFTSIndex() )
    return;

  if( ensureInitDone().size() )
    return;

  if( firstIteration && getArticleCount() > FTS::MaxDictionarySizeForFastSearch )
    return;

  gdDebug( "Dsl: Building the full-text index for dictionary: %s\n",
           getName().c_str() );

  try
  {
    FtsHelpers::makeFTSIndex( this, isCancelled );
    FTS_index_completed.ref();
  }
  catch( std::exception &ex )
  {
    gdWarning( "DSL: Failed building full-text search index for \"%s\", reason: %s\n", getName().c_str(), ex.what() );
    QFile::remove( ftsIdxName.c_str() );
  }
}

void DslDictionary::getArticleText( uint32_t articleAddress, QString & headword, QString & text )
{
  headword.clear();
  text.clear();

  vector< char > chunk;

  char * articleProps;
  wstring articleData;

  {
    QMutexLocker _( &idxMutex );
    articleProps = chunks->getBlock( articleAddress, chunk );
  }

  uint32_t articleOffset, articleSize;

  memcpy( &articleOffset, articleProps, sizeof( articleOffset ) );
  memcpy( &articleSize, articleProps + sizeof( articleOffset ),
          sizeof( articleSize ) );

  char * articleBody;

  {
    QMutexLocker _( &dzMutex );
    articleBody = dict_data_read_( dz, articleOffset, articleSize, 0, 0 );
  }

  if ( !articleBody )
  {
    return;
  }
  else
  {
    try
    {
      articleData =
        Iconv::toWstring(
          getEncodingNameFor( Encoding( idxHeader.dslEncoding ) ),
          articleBody, articleSize );
      free( articleBody );

      // Strip DSL comments
      bool b = false;
      stripComments( articleData, b );
    }
    catch( ... )
    {
      free( articleBody );
      return;
    }
  }

  // Skip headword

  size_t pos = 0;
  wstring articleHeadword, tildeValue;

  // Check if we retrieve insided card
  bool insidedCard = isDslWs( articleData.at( 0 ) );

  for( ; ; )
  {
    size_t begin = pos;

    pos = articleData.find_first_of(  U"\n\r" , begin );

    if ( articleHeadword.empty() )
    {
      // Process the headword

      articleHeadword = wstring( articleData, begin, pos - begin );

      if( insidedCard && !articleHeadword.empty() && isDslWs( articleHeadword[ 0 ] ) )
      {
        // Headword of the insided card
        wstring::size_type hpos = articleHeadword.find( L'@' );
        if( hpos != string::npos )
        {
          wstring head = Folding::trimWhitespace( articleHeadword.substr( hpos + 1 ) );
          hpos = head.find( L'~' );
          while( hpos != string::npos )
          {
            if( hpos == 0 || head[ hpos ] != L'\\' )
              break;
            hpos = head.find( L'~', hpos + 1 );
          }
          if( hpos == string::npos )
            articleHeadword = head;
          else
            articleHeadword.clear();
        }
      }

      if( !articleHeadword.empty() )
      {
        list< wstring > lst;

        tildeValue = articleHeadword;

        processUnsortedParts( articleHeadword, true );
        expandOptionalParts( articleHeadword, &lst );

        if ( lst.size() ) // Should always be
          articleHeadword = lst.front();
      }
    }

    if ( pos == articleData.size() )
      break;

    // Skip \n\r

    if ( articleData[ pos ] == '\r' )
      ++pos;

    if ( pos != articleData.size() )
    {
      if ( articleData[ pos ] == '\n' )
        ++pos;
    }

    if ( pos == articleData.size() )
    {
      // Ok, it's end of article
      break;
    }
    if( isDslWs( articleData[ pos ] ) )
    {
     // Check for begin article text
      if( insidedCard )
      {
        // Check for next insided headword
        wstring::size_type hpos = articleData.find_first_of(  U"\n\r" , pos );
        if ( hpos == wstring::npos )
          hpos = articleData.size();

        wstring str = wstring( articleData, pos, hpos - pos );

        hpos = str.find( L'@');
        if( hpos == wstring::npos || str[ hpos - 1 ] == L'\\' || !isAtSignFirst( str ) )
          break;
      }
      else
        break;
    }
  }

  if( !articleHeadword.empty() )
  {
    unescapeDsl( articleHeadword );
    normalizeHeadword( articleHeadword );
    headword = QString::fromStdU32String( articleHeadword );
  }

  wstring articleText;

  if ( pos != articleData.size() )
    articleText = wstring( articleData, pos );
  else
    articleText.clear();

  if( !tildeValue.empty() )
  {
    list< wstring > lst;

    processUnsortedParts( tildeValue, false );
    expandOptionalParts( tildeValue, &lst );

    if ( lst.size() ) // Should always be
      expandTildes( articleText, lst.front() );
  }

  if( !articleText.empty() )
  {
    text = QString::fromStdU32String( articleText ).normalized( QString::NormalizationForm_C );

    articleText.clear();

    // Parse article text

    // Strip some areas

    const int stripTagsNumber = 5;
    static QString stripTags[ stripTagsNumber ] =
                                                  {
                                                    "s",
                                                    "url",
                                                    "!trs",
                                                    "video",
                                                    "preview"
                                                  };
    static QString stripEndTags[ stripTagsNumber ] =
                                                  {
                                                    "[/s]",
                                                    "[/url]",
                                                    "[/!trs]",
                                                    "[/video]",
                                                    "[/preview]"
                                                  };

    int pos = 0;
    while( pos >= 0 )
    {
      pos = text.indexOf( '[', pos, Qt::CaseInsensitive );
      if( pos >= 0 )
      {
        if( ( pos > 0 && text[ pos - 1 ] == '\\' && ( pos < 2 || text[ pos - 2 ] != '\\' ) )
              || ( pos > text.size() - 2 || text[ pos + 1 ] == '/' ) )
        {
          pos += 1;
          continue;
        }

        int pos2 = text.indexOf( ']',  pos + 1, Qt::CaseInsensitive );
        if( pos2 < 0 )
          break;

        QString tag = text.mid( pos + 1, pos2 - pos - 1 );

        int n;
        for( n = 0; n < stripTagsNumber; n++ )
        {
          if( tag.compare( stripTags[ n ], Qt::CaseInsensitive ) == 0 )
          {
            pos2 = text.indexOf( stripEndTags[ n ] , pos + stripTags[ n ].size() + 2, Qt::CaseInsensitive );
            text.replace( pos, pos2 > 0 ? pos2 - pos + stripEndTags[ n ].length() : text.length() - pos, " " );
            break;
          }
        }

        if( n >= stripTagsNumber )
          pos += 1;
      }
    }

    // Strip tags

    text.replace( QRegularExpression( R"(\[(|/)(p|trn|ex|com|\*|t|br|m[0-9]?)\])" ), " " );
    text.replace( QRegularExpression( R"(\[(|/)lang(\s[^\]]*)?\])" ), " " );
    text.remove( QRegularExpression( R"(\[[^\\\[\]]+\])" ) );

    text.remove( QString::fromLatin1( "<<" ) );
    text.remove( QString::fromLatin1( ">>" ) );

    // Chech for insided cards

    bool haveInsidedCards = false;
    pos = 0;
    while( pos >= 0 )
    {
      pos = text.indexOf( "@", pos );
      if( pos > 0 && text.at( pos - 1 ) != '\\' )
      {
        haveInsidedCards = true;
        break;
      }

      if( pos >= 0 )
        pos += 1;
    }

    if( haveInsidedCards )
    {
      // Use base DSL parser for articles with insided cards
      ArticleDom dom( gd::toWString( text ), getName(), articleHeadword );
      text = QString::fromStdU32String( dom.root.renderAsText( true ) );
    }
    else
    {
      // Unescape DSL symbols
      pos = 0;
      while( pos >= 0 )
      {
        pos = text.indexOf( '\\', pos );
        if( pos >= 0 )
        {
          if( text[ pos + 1 ] == '\\' )
            pos += 1;

          text.remove( pos, 1 );
        }
      }
    }
  }
}

/// DslDictionary::getArticle()

class DslArticleRequest: public Dictionary::DataRequest
{
  wstring word;
  vector< wstring > alts;
  DslDictionary & dict;
  bool ignoreDiacritics;

  QAtomicInt isCancelled;
  QSemaphore hasExited;
  QFuture< void > f;

public:

  DslArticleRequest( wstring const & word_,
                     vector< wstring > const & alts_,
                     DslDictionary & dict_, bool ignoreDiacritics_ ):
    word( word_ ), alts( alts_ ), dict( dict_ ), ignoreDiacritics( ignoreDiacritics_ )
  {
    f = QtConcurrent::run( [ this ]() { this->run(); } );
  }

  void run();

  void cancel() override
  {
    isCancelled.ref();
  }

  ~DslArticleRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};

void DslArticleRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) )
  {
    finish();
    return;
  }

  if ( dict.ensureInitDone().size() )
  {
    setErrorString( QString::fromUtf8( dict.ensureInitDone().c_str() ) );
    finish();
    return;
  }

  vector< WordArticleLink > chain = dict.findArticles( word, ignoreDiacritics );

  for ( const auto & alt : alts ) {
    /// Make an additional query for each alt

    vector< WordArticleLink > altChain = dict.findArticles( alt, ignoreDiacritics );

    chain.insert( chain.end(), altChain.begin(), altChain.end() );
  }

  // Some synonyms make it that the articles appear several times. We combat
  // this by only allowing them to appear once. Dsl treats different headwords
  // of the same article as different articles, so we also include headword
  // index here.
  set< pair< uint32_t, unsigned > > articlesIncluded;

  wstring wordCaseFolded = Folding::applySimpleCaseOnly( word );

  for ( auto & x : chain ) {
    // Check if we're cancelled occasionally
    if ( Utils::AtomicInt::loadAcquire( isCancelled ) )
    {
      finish();
      return;
    }

    // Grab that article

    wstring tildeValue;
    wstring displayedHeadword;
    wstring articleBody;
    unsigned headwordIndex;

    string articleText, articleAfter;

    try
    {
      dict.loadArticle( x.articleOffset, wordCaseFolded, ignoreDiacritics, tildeValue,
                        displayedHeadword, headwordIndex, articleBody );

      if ( !articlesIncluded.insert( std::make_pair( x.articleOffset,
                                                     headwordIndex ) ).second )
        continue; // We already have this article in the body.

      dict.articleNom += 1;

      if( displayedHeadword.empty() || isDslWs( displayedHeadword[ 0 ] ) )
        displayedHeadword = word; // Special case - insided card

      articleText += "<div class=\"dsl_article\">";
      articleText += "<div class=\"dsl_headwords\"";
      if( dict.isFromLanguageRTL() )
        articleText += " dir=\"rtl\"";
      articleText += "><p>";

      if( displayedHeadword.size() == 1 && displayedHeadword[0] == '<' )  // Fix special case - "<" header
          articleText += "<";                                             // dslToHtml can't handle it correctly.
      else
        articleText += dict.dslToHtml( displayedHeadword, displayedHeadword );

      /// After this may be expand button will be inserted

      articleAfter += "</p></div>";

      expandTildes( articleBody, tildeValue );

      articleAfter += "<div class=\"dsl_definition\"";
      if( dict.isToLanguageRTL() )
        articleAfter += " dir=\"rtl\"";
      articleAfter += ">";

      articleAfter += dict.dslToHtml( articleBody, displayedHeadword );
      articleAfter += "</div>";
      articleAfter += "</div>";

      if( dict.hasHiddenZones() )
      {
        string prefix = "O" + dict.getId().substr( 0, 7 ) + "_" + QString::number( dict.articleNom ).toStdString();
        string id1 = prefix + "_expand";
        string id2 = prefix + "_opt_";
        string button = R"( <img src="qrc:///icons/expand_opt.png" class="hidden_expand_opt" id=")" + id1 +
                        "\" onclick=\"gdExpandOptPart('" + id1 + "','" + id2 +"')\" alt=\"[+]\"/>";
        if( articleText.compare( articleText.size() - 4, 4, "</p>" ) == 0 )
          articleText.insert( articleText.size() - 4, " " + button );
        else
          articleText += button;
      }

      articleText += articleAfter;
    }
    catch( std::exception &ex )
    {
      gdWarning( "DSL: Failed loading article from \"%s\", reason: %s\n", dict.getName().c_str(), ex.what() );
      articleText = string( "<span class=\"dsl_article\">" )
                    + QObject::tr( "Article loading error" ).toStdString()
                    + "</span>";
    }

    appendString(articleText);

    hasAnyData = true;
  }

  finish();
}

sptr< Dictionary::DataRequest > DslDictionary::getArticle( wstring const & word,
                                                           vector< wstring > const & alts,
                                                           wstring const &,
                                                           bool ignoreDiacritics )
  
{
  return std::make_shared<DslArticleRequest>( word, alts, *this, ignoreDiacritics );
}

//// DslDictionary::getResource()

class DslResourceRequest: public Dictionary::DataRequest
{
  DslDictionary & dict;

  string resourceName;

  QAtomicInt isCancelled;
  QSemaphore hasExited;
  QFuture< void > f;

public:

  DslResourceRequest( DslDictionary & dict_,
                      string const & resourceName_ ):
    dict( dict_ ),
    resourceName( resourceName_ )
  {
    f = QtConcurrent::run( [ this ]() { this->run(); } );
  }

  void run();

  void cancel() override
  {
    isCancelled.ref();
  }

  ~DslResourceRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
    //hasExited.acquire();
  }
};

void DslResourceRequest::run()
{
  // Some runnables linger enough that they are cancelled before they start
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) )
  {
    finish();
    return;
  }

  if ( dict.ensureInitDone().size() )
  {
    setErrorString( QString::fromUtf8( dict.ensureInitDone().c_str() ) );
    finish();
    return;
  }

  string n = dict.getContainingFolder().toStdString() + Utils::Fs::separator() + resourceName;

  GD_DPRINTF( "n is %s\n", n.c_str() );

  try
  {
    try
    {
      QMutexLocker _( &dataMutex );

      File::loadFromFile( n, data );
    }
    catch( File::exCantOpen & )
    {
      n = dict.getResourceDir1() + resourceName;
      try {
        QMutexLocker _( &dataMutex );

        File::loadFromFile( n, data );
      }
      catch( File::exCantOpen & )
      {
        n = dict.getResourceDir2() + resourceName;

        try
        {
          QMutexLocker _( &dataMutex );

          File::loadFromFile( n, data );
        }
        catch( File::exCantOpen & )
        {
          // Try reading from zip file

          if ( dict.resourceZip.isOpen() )
          {
            QMutexLocker _( &dataMutex );

            if ( !dict.resourceZip.loadFile( Utf8::decode( resourceName ), data ) )
              throw; // Make it fail since we couldn't read the archive
          }
          else
            throw;
        }
      }
    }

    if ( Filetype::isNameOfTiff( resourceName ) )
    {
      // Convert it

      QMutexLocker _( &dataMutex );
      GdTiff::tiff2img( data );
    }

    QMutexLocker _( &dataMutex );

    hasAnyData = true;
  }
  catch( std::exception &ex )
  {
    gdWarning( "DSL: Failed loading resource \"%s\" for \"%s\", reason: %s\n",
               resourceName.c_str(), dict.getName().c_str(), ex.what() );
    // Resource not loaded -- we don't set the hasAnyData flag then
  }

  finish();
}

sptr< Dictionary::DataRequest > DslDictionary::getResource( string const & name )
  
{
  return std::make_shared<DslResourceRequest>( *this, name );
}


sptr< Dictionary::DataRequest > DslDictionary::getSearchResults( QString const & searchString,
                                                                 int searchMode,
                                                                 bool matchCase,

                                                                 bool ignoreDiacritics )
{
  return std::make_shared< FtsHelpers::FTSResultsRequest >( *this,
                                                            searchString,
                                                            searchMode,
                                                            matchCase,
                                                            ignoreDiacritics );
}

} // anonymous namespace

/// makeDictionaries

vector< sptr< Dictionary::Class > > makeDictionaries(
                                      vector< string > const & fileNames,
                                      string const & indicesDir,
                                      Dictionary::Initializing & initializing,
                                      int maxPictureWidth, unsigned int maxHeadwordSize )
  
{
  vector< sptr< Dictionary::Class > > dictionaries;

  for ( const auto & fileName : fileNames ) {
    // Try .dsl and .dsl.dz suffixes

    bool uncompressedDsl = ( fileName.size() >= 4 &&
                             strcasecmp( fileName.c_str() + ( fileName.size() - 4 ), ".dsl" ) == 0 );
    if ( !uncompressedDsl &&
         ( fileName.size() < 7 ||
           strcasecmp( fileName.c_str() + ( fileName.size() - 7 ), ".dsl.dz" ) != 0 ) )
      continue;

    // Make sure it's not an abbreviation file

    int extSize = ( uncompressedDsl ? 4 : 7 );
    if ( fileName.size() - extSize >= 5 &&
         strncasecmp( fileName.c_str() + fileName.size() - extSize - 5, "_abrv", 5 ) == 0 )
    {
      // It is, skip it
      continue;
    }

    unsigned atLine = 0; // Indicates current line in .dsl, for debug purposes

    try
    {
      vector< string > dictFiles( 1, fileName );

      // Check if there is an 'abrv' file present
      string baseName = ( fileName[ fileName.size() - 4 ] == '.' ) ?
               string( fileName, 0, fileName.size() - 4 ) : string( fileName, 0, fileName.size() - 7 );

      string abrvFileName;

      if ( File::tryPossibleName( baseName + "_abrv.dsl", abrvFileName ) ||
           File::tryPossibleName( baseName + "_abrv.dsl.dz", abrvFileName ) ||
           File::tryPossibleName( baseName + "_ABRV.DSL", abrvFileName ) ||
           File::tryPossibleName( baseName + "_ABRV.DSL.DZ", abrvFileName ) ||
           File::tryPossibleName( baseName + "_ABRV.DSL.dz", abrvFileName ) )
        dictFiles.push_back( abrvFileName );

      string dictId = Dictionary::makeDictionaryId( dictFiles );

      // See if there's a zip file with resources present. If so, include it.

      string zipFileName;

      if ( File::tryPossibleZipName( baseName + ".dsl.files.zip", zipFileName ) ||
           File::tryPossibleZipName( baseName + ".dsl.dz.files.zip", zipFileName ) ||
           File::tryPossibleZipName( baseName + ".DSL.FILES.ZIP", zipFileName ) ||
           File::tryPossibleZipName( baseName + ".DSL.DZ.FILES.ZIP", zipFileName ) )
        dictFiles.push_back( zipFileName );

      string indexFile = indicesDir + dictId;

      if ( Dictionary::needToRebuildIndex( dictFiles, indexFile ) ||
           indexIsOldOrBad( indexFile, zipFileName.size() ) )
      {
        DslScanner scanner( fileName );

        try { // Here we intercept any errors during the read to save line at
              // which the incident happened. We need alive scanner for that.

          if( scanner.getDictionaryName() ==  U"Abbrev"  )
            continue; // For now just skip abbreviations

          // Building the index
          initializing.indexingDictionary( Utf8::encode( scanner.getDictionaryName() ) );

          gdDebug( "Dsl: Building the index for dictionary: %s\n",
                   QString::fromStdU32String( scanner.getDictionaryName() ).toUtf8().data() );

          File::Class idx( indexFile, "wb" );

          IdxHeader idxHeader;

          memset( &idxHeader, 0, sizeof( idxHeader ) );

          // We write a dummy header first. At the end of the process the header
          // will be rewritten with the right values.

          idx.write( idxHeader );

          string dictionaryName = Utf8::encode( scanner.getDictionaryName() );

          idx.write( (uint32_t)dictionaryName.size() );
          idx.write( dictionaryName.data(), dictionaryName.size() );

          string soundDictName = Utf8::encode( scanner.getSoundDictionaryName() );
          if( !soundDictName.empty() )
          {
            idxHeader.hasSoundDictionaryName = 1;
            idx.write( (uint32_t)soundDictName.size() );
            idx.write( soundDictName.data(), soundDictName.size() );
        }

        idxHeader.dslEncoding = scanner.getEncoding();

        IndexedWords indexedWords;

        ChunkedStorage::Writer chunks( idx );

        // Read the abbreviations

        if ( abrvFileName.size() )
        {
          try
          {
            DslScanner abrvScanner( abrvFileName );

            map< string, string > abrv;

            wstring curString;
            size_t curOffset;

            for( ; ; )
            {
              // Skip any whitespace
              if ( !abrvScanner.readNextLineWithoutComments( curString, curOffset, true ) )
                break;
              if ( curString.empty() || isDslWs( curString[ 0 ] ) )
                continue;

              list< wstring > keys;

              bool eof = false;

              // Insert the key and read more, or get to the definition
              for( ; ; )
              {
                processUnsortedParts( curString, true );

                if ( keys.size() )
                  expandTildes( curString, keys.front() );

                expandOptionalParts( curString, &keys );

                if ( !abrvScanner.readNextLineWithoutComments( curString, curOffset ) || curString.empty() )
                {
                  gdWarning( "Premature end of file %s\n", abrvFileName.c_str() );
                  eof = true;
                  break;
                }

                if ( isDslWs( curString[ 0 ] ) )
                  break;
              }

              if ( eof )
                break;

              curString.erase( 0, curString.find_first_not_of(  U" \t"  ) );

              if ( keys.size() )
                expandTildes( curString, keys.front() );

              // If the string has any dsl markup, we strip it
              string value = Utf8::encode( ArticleDom( curString ).root.renderAsText() );

              for ( auto & key : keys ) {
                unescapeDsl( key );
                normalizeHeadword( key );

                abrv[ Utf8::encode( Folding::trimWhitespace( key ) ) ] = value;
              }
            }

            idxHeader.hasAbrv = 1;
            idxHeader.abrvAddress = chunks.startNewBlock();

            uint32_t sz = abrv.size();

            chunks.addToBlock( &sz, sizeof( uint32_t ) );

            for ( const auto & i : abrv ) {
//              GD_DPRINTF( "%s:%s\n", i->first.c_str(), i->second.c_str() );

              sz = i.first.size();
              chunks.addToBlock( &sz, sizeof( uint32_t ) );
              chunks.addToBlock( i.first.data(), sz );
              sz = i.second.size();
              chunks.addToBlock( &sz, sizeof( uint32_t ) );
              chunks.addToBlock( i.second.data(), sz );
            }
          }
          catch( std::exception & e )
          {
            gdWarning( "Error reading abrv file \"%s\", error: %s. Skipping it.\n",
                       abrvFileName.c_str(), e.what() );
          }
        }

        bool hasString = false;
        wstring curString;
        size_t curOffset;

        uint32_t articleCount = 0, wordCount = 0;

        for( ; ; )
        {
          // Find the main headword

          if ( !hasString && !scanner.readNextLineWithoutComments( curString, curOffset, true) )
            break; // Clean end of file

          hasString = false;

          // The line read should either consist of pure whitespace, or be a headword
          // skip too long headword,it can never be headword.
          if( curString.empty() || curString.size() > 100 )
            continue;

          if ( isDslWs( curString[ 0 ] ) )
          {
            // The first character is blank. Let's make sure that all other
            // characters are blank, too.
            for( size_t x = 1; x < curString.size(); ++x )
            {
              if ( !isDslWs( curString[ x ] ) )
              {
                gdWarning( "Garbage string in %s at offset 0x%lX\n", fileName.c_str(), curOffset );
                break;
              }
            }
            continue;
          }

          // Ok, got the headword

          list< wstring > allEntryWords;

          processUnsortedParts( curString, true );
          expandOptionalParts( curString, &allEntryWords );

          uint32_t articleOffset = curOffset;

          //GD_DPRINTF( "Headword: %ls\n", curString.c_str() );

          // More headwords may follow

          for( ; ; )
          {
            if ( ! ( hasString = scanner.readNextLineWithoutComments( curString, curOffset ) ) )
            {
              gdWarning( "Premature end of file %s\n", fileName.c_str() );
              break;
            }

            // Lingvo skips empty strings between the headwords
            if ( curString.empty() )
              continue;

            if ( isDslWs( curString[ 0 ] ) )
              break; // No more headwords

#ifdef QT_DEBUG
            qDebug() << "Alt headword" << QString::fromStdU32String( curString );
#endif

            processUnsortedParts( curString, true );
            expandTildes( curString, allEntryWords.front() );
            expandOptionalParts( curString, &allEntryWords );
          }

          if ( !hasString )
            break;

          // Insert new entry

          uint32_t descOffset = chunks.startNewBlock();

          chunks.addToBlock( &articleOffset, sizeof( articleOffset ) );

          for ( auto & allEntryWord : allEntryWords ) {
            unescapeDsl( allEntryWord );
            normalizeHeadword( allEntryWord );
            indexedWords.addWord( allEntryWord, descOffset, maxHeadwordSize );
          }

          ++articleCount;
          wordCount += allEntryWords.size();

          int insideInsided = 0;
          wstring headword;
          QVector< InsidedCard > insidedCards;
          uint32_t offset = curOffset;
          QVector< wstring > insidedHeadwords;
          unsigned linesInsideCard = 0;
          int dogLine = 0;
          bool wasEmptyLine = false;
          int headwordLine = scanner.getLinesRead() - 2;
          bool noSignificantLines = Folding::applyWhitespaceOnly( curString ).empty();
          bool haveLine = !noSignificantLines;

          // Skip the article's body
          for( ; ; )
          {
            hasString = haveLine ? true : scanner.readNextLineWithoutComments( curString, curOffset);
            haveLine = false;

            if ( !hasString || ( curString.size() && !isDslWs( curString[ 0 ] ) ) )
            {
              if( insideInsided )
              {
                gdWarning( "Unclosed tag '@' at line %i", dogLine );
                insidedCards.append( InsidedCard( offset, curOffset - offset, insidedHeadwords ) );
              }
              if( noSignificantLines )
                gdWarning( "Orphan headword at line %i", headwordLine );

              break;
            }

            // Check for orphan strings

            if( curString.empty() )
            {
              wasEmptyLine = true;
              continue;
            }
            else
            {
              if( wasEmptyLine && !Folding::applyWhitespaceOnly( curString ).empty() )
                gdWarning( "Orphan string at line %i", scanner.getLinesRead() - 1 );
            }

            if( noSignificantLines )
              noSignificantLines = Folding::applyWhitespaceOnly( curString ).empty();

            // Find embedded cards

            wstring::size_type n = curString.find( L'@' );
            if( n == wstring::npos || curString[ n - 1 ] == L'\\' )
            {
              if( insideInsided )
                linesInsideCard++;

              continue;
            }
            else
            {
              // Embedded card tag must be placed at first position in line after spaces
              if( !isAtSignFirst( curString ) )
              {
                gdWarning( "Unescaped '@' symbol at line %i", scanner.getLinesRead() - 1 );

                if( insideInsided )
                  linesInsideCard++;

                continue;
              }
            }

            dogLine = scanner.getLinesRead() - 1;

            // Handle embedded card

            if( insideInsided )
            {
              if( linesInsideCard )
              {
                insidedCards.append( InsidedCard( offset, curOffset - offset, insidedHeadwords ) );

                insidedHeadwords.clear();
                linesInsideCard = 0;
                offset = curOffset;
              }
            }
            else
            {
              offset = curOffset;
              linesInsideCard = 0;
            }

            headword = Folding::trimWhitespace( curString.substr( n + 1 ) );

            if( !headword.empty() )
            {
              processUnsortedParts( headword, true );
              expandTildes( headword, allEntryWords.front() );
              insidedHeadwords.append( headword );
              insideInsided = true;
            }
            else
              insideInsided = false;
          }

          // Now that we're having read the first string after the article
          // itself, we can use its offset to calculate the article's size.
          // An end of file works here, too.

          uint32_t articleSize = ( curOffset - articleOffset );

          chunks.addToBlock( &articleSize, sizeof( articleSize ) );

          for ( auto & insidedCard : insidedCards ) {
            uint32_t desc_offset = chunks.startNewBlock();
            chunks.addToBlock( &insidedCard.offset, sizeof( insidedCard.offset ) );
            chunks.addToBlock( &insidedCard.size, sizeof( insidedCard.size ) );

            for ( auto & hw : insidedCard.headwords ) {
              allEntryWords.clear();
              expandOptionalParts( hw, &allEntryWords );

              for ( auto & allEntryWord : allEntryWords ) {
                unescapeDsl( allEntryWord );
                normalizeHeadword( allEntryWord );
                indexedWords.addWord( allEntryWord, desc_offset, maxHeadwordSize );
              }

              wordCount += allEntryWords.size();
            }
            ++articleCount;
          }

          if ( !hasString )
            break;
        }

        // Finish with the chunks

        idxHeader.chunksOffset = chunks.finish();

        // Build index

        IndexInfo idxInfo = BtreeIndexing::buildIndex( indexedWords, idx );

        idxHeader.indexBtreeMaxElements = idxInfo.btreeMaxElements;
        idxHeader.indexRootOffset = idxInfo.rootOffset;

        indexedWords.clear(); // Release memory -- no need for this data

        // If there was a zip file, index it too

        if ( zipFileName.size() )
        {
          GD_DPRINTF( "Indexing zip file\n" );

          idxHeader.hasZipFile = 1;

          IndexedWords zipFileNames;
          IndexedZip zipFile;
          if ( zipFile.openZipFile( QDir::fromNativeSeparators( zipFileName.c_str() ) ) )
            zipFile.indexFile( zipFileNames );

          if( !zipFileNames.empty() )
          {
            // Build the resulting zip file index

            IndexInfo idxInfo = BtreeIndexing::buildIndex( zipFileNames, idx );

            idxHeader.zipIndexBtreeMaxElements = idxInfo.btreeMaxElements;
            idxHeader.zipIndexRootOffset = idxInfo.rootOffset;
          }
          else
          {
            // Bad zip file -- no index (though the mark that we have one
            // remains)
            idxHeader.zipIndexBtreeMaxElements = 0;
            idxHeader.zipIndexRootOffset = 0;
          }
        }
        else
          idxHeader.hasZipFile = 0;

        // That concludes it. Update the header.

        idxHeader.signature = Signature;
        idxHeader.formatVersion = CurrentFormatVersion;
        idxHeader.zipSupportVersion = CurrentZipSupportVersion;

        idxHeader.articleCount = articleCount;
        idxHeader.wordCount = wordCount;

        idxHeader.langFrom = dslLanguageToId( scanner.getLangFrom() );
        idxHeader.langTo = dslLanguageToId( scanner.getLangTo() );

        idx.rewind();

        idx.write( &idxHeader, sizeof( idxHeader ) );

      } // In-place try for saving line count
      catch( ... )
      {
        atLine = scanner.getLinesRead();
        throw;
      }

      } // if need to rebuild

      dictionaries.push_back( std::make_shared<DslDictionary>( dictId,
                                                 indexFile,
                                                 dictFiles,
                                                 maxPictureWidth ) );
    }
    catch( std::exception & e )
    {
      gdWarning( "DSL dictionary reading failed: %s:%u, error: %s\n",
                 fileName.c_str(), atLine, e.what() );
    }
  }

  return dictionaries;
}


}
