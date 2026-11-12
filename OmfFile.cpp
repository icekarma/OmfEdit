#include "OmfFile.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace Omf {

    namespace {

        //=====================================================================
        // Internal Binary Reader for OMF-86 Primitives
        //=====================================================================
        class OmfBinaryReader {
        public:
            OmfBinaryReader(
                std::span<const std::byte> buffer,
                RecordType                 recordType,
                std::size_t                baseFileOffset = 0 ) noexcept
                : _buffer( buffer ),
                  _position( 0 ),
                  _recordType( recordType ),
                  _baseFileOffset( baseFileOffset ) {}

            bool IsAtEnd() const noexcept { return _position >= _buffer.size(); }
            std::size_t GetRemainingCount() const noexcept { return _buffer.size() - _position; }
            std::size_t GetPosition() const noexcept { return _position; }

            std::uint8_t PeekUInt8() const {
                _EnsureAvailable( 1 );
                return static_cast<std::uint8_t>( _buffer[_position] );
            }

            std::uint8_t ReadUInt8() {
                _EnsureAvailable( 1 );
                return static_cast<std::uint8_t>( _buffer[_position++] );
            }

            std::uint16_t ReadUInt16() {
                _EnsureAvailable( 2 );
                std::uint16_t const lo = static_cast<std::uint8_t>( _buffer[_position] );
                std::uint16_t const hi = static_cast<std::uint8_t>( _buffer[_position + 1] );
                _position += 2;
                return static_cast<std::uint16_t>( lo | ( hi << 8 ) );
            }

            std::uint32_t ReadUInt24() {
                _EnsureAvailable( 3 );
                std::uint32_t const b0 = static_cast<std::uint8_t>( _buffer[_position] );
                std::uint32_t const b1 = static_cast<std::uint8_t>( _buffer[_position + 1] );
                std::uint32_t const b2 = static_cast<std::uint8_t>( _buffer[_position + 2] );
                _position += 3;
                return b0 | ( b1 << 8 ) | ( b2 << 16 );
            }

            std::uint32_t ReadUInt32() {
                _EnsureAvailable( 4 );
                std::uint32_t const b0 = static_cast<std::uint8_t>( _buffer[_position] );
                std::uint32_t const b1 = static_cast<std::uint8_t>( _buffer[_position + 1] );
                std::uint32_t const b2 = static_cast<std::uint8_t>( _buffer[_position + 2] );
                std::uint32_t const b3 = static_cast<std::uint8_t>( _buffer[_position + 3] );
                _position += 4;
                return b0 | ( b1 << 8 ) | ( b2 << 16 ) | ( b3 << 24 );
            }

            std::uint32_t ReadOffset( bool is32Bit ) {
                return is32Bit ? ReadUInt32() : static_cast<std::uint32_t>( ReadUInt16() );
            }

            // OMF variable-length index: 1 byte if 0..0x7F, 2 bytes (big-endian with 0x80 flag) if >= 0x80
            std::uint16_t ReadRawIndex() {
                std::uint8_t const first = ReadUInt8();
                if ( ( first & 0x80u ) == 0 ) {
                    return first;
                }
                std::uint8_t const second = ReadUInt8();
                return static_cast<std::uint16_t>( ( ( first & 0x7Fu ) << 8 ) | second );
            }

            template <typename Tag>
            OmfIndex<Tag> ReadIndex() {
                return OmfIndex<Tag>{ ReadRawIndex() };
            }

            // OMF 1-byte length-prefixed string
            std::string ReadString() {
                std::uint8_t const length = ReadUInt8();
                _EnsureAvailable( length );
                std::string result(
                    reinterpret_cast<const char*>( _buffer.data() + _position ),
                    length );
                _position += length;
                return result;
            }

            // OMF COMDEF communal variable length encoding
            std::uint32_t ReadCommunalNumber() {
                std::uint8_t const indicator = ReadUInt8();
                if ( indicator <= 0x80u ) {
                    return indicator;
                }
                switch ( indicator ) {
                    case 0x81u: return ReadUInt16();
                    case 0x84u: return ReadUInt24();
                    case 0x88u: return ReadUInt32();
                    default:
                        throw OmfParseException(
                            "Invalid COMDEF communal number indicator byte",
                            _baseFileOffset + _position - 1,
                            _recordType );
                }
            }

            std::vector<std::byte> ReadBytes( std::size_t count ) {
                _EnsureAvailable( count );
                std::vector<std::byte> result(
                    _buffer.begin() + static_cast<std::ptrdiff_t>( _position ),
                    _buffer.begin() + static_cast<std::ptrdiff_t>( _position + count ) );
                _position += count;
                return result;
            }

            std::vector<std::byte> ReadRemainingBytes() {
                return ReadBytes( GetRemainingCount() );
            }

            FixupTargetSpec ReadFixupTargetSpec( bool is32Bit ) {
                FixupTargetSpec spec{};
                std::uint8_t const fixdat = ReadUInt8();

                spec.frameUsesThread     = ( fixdat & 0x80u ) != 0;
                spec.frameMethodOrThread = static_cast<std::uint8_t>( ( fixdat >> 4 ) & 0x07u );
                spec.targetUsesThread    = ( fixdat & 0x08u ) != 0;
                spec.targetMethodOrThread= static_cast<std::uint8_t>( fixdat & 0x07u );

                // Frame Datum is present if Frame does not use a thread and method is F0..F3
                if ( !spec.frameUsesThread ) {
                    if ( spec.frameMethodOrThread <= 2u ) {
                        spec.frameDatum = ReadRawIndex();
                    } else if ( spec.frameMethodOrThread == 3u ) {
                        spec.frameDatum = ReadUInt16();
                    }
                }

                // Target Datum is present if Target does not use a thread
                if ( !spec.targetUsesThread ) {
                    std::uint8_t const baseTargetMethod = static_cast<std::uint8_t>( spec.targetMethodOrThread & 0x03u );
                    if ( baseTargetMethod <= 2u ) {
                        spec.targetDatum = ReadRawIndex();
                    } else {
                        spec.targetDatum = ReadUInt16();
                    }
                }

                // Target Displacement is present if the P bit (bit 2 of fixdat) is 0
                bool const hasDisplacement = ( fixdat & 0x04u ) == 0;
                if ( hasDisplacement ) {
                    spec.targetDisplacement = ReadOffset( is32Bit );
                }

                return spec;
            }

            OmfLidataRecord::IteratedDataBlock ReadIteratedDataBlock( bool is32Bit ) {
                OmfLidataRecord::IteratedDataBlock block{};
                block.repeatCount = is32Bit ? ReadUInt32() : static_cast<std::uint32_t>( ReadUInt16() );
                std::uint16_t const blockCount = ReadUInt16();

                if ( blockCount == 0 ) {
                    std::uint8_t const byteCount = ReadUInt8();
                    block.content = ReadBytes( byteCount );
                } else {
                    std::vector<OmfLidataRecord::IteratedDataBlock> nestedBlocks;
                    nestedBlocks.reserve( blockCount );
                    for ( std::uint16_t i = 0; i < blockCount; ++i ) {
                        nestedBlocks.push_back( ReadIteratedDataBlock( is32Bit ) );
                    }
                    block.content = std::move( nestedBlocks );
                }
                return block;
            }

        private:
            void _EnsureAvailable( std::size_t byteCount ) const {
                if ( _position + byteCount > _buffer.size() ) {
                    throw OmfParseException(
                        "Unexpected end of OMF record payload",
                        _baseFileOffset + _position,
                        _recordType );
                }
            }

            std::span<const std::byte> _buffer;
            std::size_t                _position;
            RecordType                 _recordType;
            std::size_t                _baseFileOffset;
        };

        //=====================================================================
        // Internal Binary Writer for OMF-86 Primitives
        //=====================================================================
        class OmfBinaryWriter {
        public:
            OmfBinaryWriter() = default;

            void WriteUInt8( std::uint8_t value ) {
                _buffer.push_back( static_cast<std::byte>( value ) );
            }

            void WriteUInt16( std::uint16_t value ) {
                _buffer.push_back( static_cast<std::byte>( value & 0xFFu ) );
                _buffer.push_back( static_cast<std::byte>( ( value >> 8 ) & 0xFFu ) );
            }

            void WriteUInt24( std::uint32_t value ) {
                _buffer.push_back( static_cast<std::byte>( value & 0xFFu ) );
                _buffer.push_back( static_cast<std::byte>( ( value >> 8 ) & 0xFFu ) );
                _buffer.push_back( static_cast<std::byte>( ( value >> 16 ) & 0xFFu ) );
            }

            void WriteUInt32( std::uint32_t value ) {
                _buffer.push_back( static_cast<std::byte>( value & 0xFFu ) );
                _buffer.push_back( static_cast<std::byte>( ( value >> 8 ) & 0xFFu ) );
                _buffer.push_back( static_cast<std::byte>( ( value >> 16 ) & 0xFFu ) );
                _buffer.push_back( static_cast<std::byte>( ( value >> 24 ) & 0xFFu ) );
            }

            void WriteOffset( std::uint32_t value, bool is32Bit ) {
                if ( is32Bit ) {
                    WriteUInt32( value );
                } else {
                    WriteUInt16( static_cast<std::uint16_t>( value ) );
                }
            }

            void WriteRawIndex( std::uint16_t index ) {
                if ( index > 0x7FFFu ) {
                    throw OmfException( "OMF index exceeds maximum 15-bit value (32767)" );
                }
                if ( index <= 0x7Fu ) {
                    WriteUInt8( static_cast<std::uint8_t>( index ) );
                } else {
                    WriteUInt8( static_cast<std::uint8_t>( 0x80u | ( ( index >> 8 ) & 0x7Fu ) ) );
                    WriteUInt8( static_cast<std::uint8_t>( index & 0xFFu ) );
                }
            }

            template <typename Tag>
            void WriteIndex( OmfIndex<Tag> index ) {
                WriteRawIndex( index.value );
            }

            void WriteString( std::string_view str ) {
                if ( str.size() > 255u ) {
                    throw OmfException( "OMF string length exceeds 255 bytes" );
                }
                WriteUInt8( static_cast<std::uint8_t>( str.size() ) );
                auto const* ptr = reinterpret_cast<const std::byte*>( str.data() );
                _buffer.insert( _buffer.end(), ptr, ptr + str.size() );
            }

            void WriteCommunalNumber( std::uint32_t value ) {
                if ( value <= 0x80u ) {
                    WriteUInt8( static_cast<std::uint8_t>( value ) );
                } else if ( value <= 0xFFFFu ) {
                    WriteUInt8( 0x81u );
                    WriteUInt16( static_cast<std::uint16_t>( value ) );
                } else if ( value <= 0xFFFFFFu ) {
                    WriteUInt8( 0x84u );
                    WriteUInt24( value );
                } else {
                    WriteUInt8( 0x88u );
                    WriteUInt32( value );
                }
            }

            void WriteBytes( std::span<const std::byte> bytes ) {
                _buffer.insert( _buffer.end(), bytes.begin(), bytes.end() );
            }

            void WriteFixupTargetSpec( const FixupTargetSpec& spec, bool is32Bit ) {
                std::uint8_t targetField = static_cast<std::uint8_t>( spec.targetMethodOrThread & 0x07u );
                if ( spec.targetDisplacement.has_value() ) {
                    targetField &= ~0x04u; // Clear P bit: displacement present
                } else {
                    targetField |= 0x04u;  // Set P bit: no displacement
                }

                std::uint8_t const fixdat = static_cast<std::uint8_t>(
                    ( spec.frameUsesThread ? 0x80u : 0x00u ) |
                    ( ( spec.frameMethodOrThread & 0x07u ) << 4 ) |
                    ( spec.targetUsesThread ? 0x08u : 0x00u ) |
                    targetField );

                WriteUInt8( fixdat );

                if ( !spec.frameUsesThread ) {
                    if ( spec.frameMethodOrThread <= 2u ) {
                        WriteRawIndex( spec.frameDatum.value_or( 0 ) );
                    } else if ( spec.frameMethodOrThread == 3u ) {
                        WriteUInt16( spec.frameDatum.value_or( 0 ) );
                    }
                }

                if ( !spec.targetUsesThread ) {
                    std::uint8_t const baseTargetMethod = static_cast<std::uint8_t>( targetField & 0x03u );
                    if ( baseTargetMethod <= 2u ) {
                        WriteRawIndex( spec.targetDatum.value_or( 0 ) );
                    } else {
                        WriteUInt16( spec.targetDatum.value_or( 0 ) );
                    }
                }

                if ( spec.targetDisplacement.has_value() ) {
                    WriteOffset( *spec.targetDisplacement, is32Bit );
                }
            }

            void WriteIteratedDataBlock( const OmfLidataRecord::IteratedDataBlock& block, bool is32Bit ) {
                WriteOffset( block.repeatCount, is32Bit );
                if ( std::holds_alternative<std::vector<std::byte>>( block.content ) ) {
                    const auto& rawBytes = std::get<std::vector<std::byte>>( block.content );
                    if ( rawBytes.size() > 255u ) {
                        throw OmfException( "LIDATA leaf block exceeds maximum 255 bytes" );
                    }
                    WriteUInt16( 0 );
                    WriteUInt8( static_cast<std::uint8_t>( rawBytes.size() ) );
                    WriteBytes( rawBytes );
                } else {
                    const auto& nested = std::get<std::vector<OmfLidataRecord::IteratedDataBlock>>( block.content );
                    WriteUInt16( static_cast<std::uint16_t>( nested.size() ) );
                    for ( const auto& child : nested ) {
                        WriteIteratedDataBlock( child, is32Bit );
                    }
                }
            }

            std::vector<std::byte> TakeBuffer() noexcept {
                return std::move( _buffer );
            }

        private:
            std::vector<std::byte> _buffer;
        };

        void ExpandIteratedBlockRecursive(
            const OmfLidataRecord::IteratedDataBlock& block,
            std::vector<std::byte>&                   output ) {
            for ( std::uint32_t r = 0; r < block.repeatCount; ++r ) {
                if ( std::holds_alternative<std::vector<std::byte>>( block.content ) ) {
                    const auto& bytes = std::get<std::vector<std::byte>>( block.content );
                    output.insert( output.end(), bytes.begin(), bytes.end() );
                } else {
                    const auto& nested = std::get<std::vector<OmfLidataRecord::IteratedDataBlock>>( block.content );
                    for ( const auto& child : nested ) {
                        ExpandIteratedBlockRecursive( child, output );
                    }
                }
            }
        }

    } // anonymous namespace

    //=========================================================================
    // OmfChecksumException
    //=========================================================================
    OmfChecksumException::OmfChecksumException(
        std::size_t  fileOffset,
        RecordType   recordType,
        std::uint8_t expectedChecksum,
        std::uint8_t actualChecksum )
        : OmfParseException(
              "OMF record checksum mismatch at offset " + std::to_string( fileOffset ),
              fileOffset,
              recordType ),
          _expectedChecksum( expectedChecksum ),
          _actualChecksum( actualChecksum ) {}

    //=========================================================================
    // OmfRecord (Base Class)
    //=========================================================================
    OmfRecord::OmfRecord( RecordType type ) noexcept
        : _recordType( type ),
          _isDirty( true ),
          _originalChecksum( 0 ),
          _hadValidChecksum( true ) {}

    OmfRecord::OmfRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : _recordType( type ),
          _isDirty( false ),
          _originalChecksum( originalChecksum ),
          _hadValidChecksum( hadValidChecksum ),
          _rawPayloadCache( rawPayload.begin(), rawPayload.end() ) {}

    void OmfRecord::SetRecordTypeProtected( RecordType newType ) noexcept {
        if ( _recordType != newType ) {
            _recordType = newType;
            MarkDirty();
        }
    }

    std::span<const std::byte> OmfRecord::GetRawPayload() const {
        _RebuildPayloadCacheIfNeeded();
        return _rawPayloadCache;
    }

    void OmfRecord::_RebuildPayloadCacheIfNeeded() const {
        if ( _isDirty ) {
            _rawPayloadCache = SerializePayload();
        }
    }

    std::uint8_t OmfRecord::_ComputeChecksumForPayload(
        std::span<const std::byte> payload ) const noexcept {
        std::size_t const recordLength = payload.size() + 1u;
        std::uint8_t sum = static_cast<std::uint8_t>( _recordType );
        sum = static_cast<std::uint8_t>( sum + ( recordLength & 0xFFu ) );
        sum = static_cast<std::uint8_t>( sum + ( ( recordLength >> 8 ) & 0xFFu ) );
        for ( std::byte b : payload ) {
            sum = static_cast<std::uint8_t>( sum + static_cast<std::uint8_t>( b ) );
        }
        return static_cast<std::uint8_t>( ( ~sum ) + 1u );
    }

    std::uint8_t OmfRecord::ComputeChecksum() const {
        return _ComputeChecksumForPayload( GetRawPayload() );
    }

    bool OmfRecord::HasValidChecksum() const noexcept {
        if ( _isDirty ) {
            return true;
        }
        return _hadValidChecksum;
    }

    std::vector<std::byte> OmfRecord::SerializeRecord( ChecksumPolicy policy ) const {
        std::span<const std::byte> const payload = GetRawPayload();
        std::size_t const recordLength = payload.size() + 1u;
        if ( recordLength > 0xFFFFu ) {
            throw OmfException( "OMF record length exceeds 65535 bytes" );
        }

        std::vector<std::byte> framed;
        framed.reserve( 3u + recordLength );
        framed.push_back( static_cast<std::byte>( _recordType ) );
        framed.push_back( static_cast<std::byte>( recordLength & 0xFFu ) );
        framed.push_back( static_cast<std::byte>( ( recordLength >> 8 ) & 0xFFu ) );
        framed.insert( framed.end(), payload.begin(), payload.end() );

        std::uint8_t checksumByte = 0;
        if ( _recordType == RecordType::MsLibHeader || _recordType == RecordType::MsLibEnd ) {
            // MS .LIB 0xF0 / 0xF1 records use the final byte as zero padding rather than a checksum
            checksumByte = _isDirty ? 0u : _originalChecksum;
        } else {
            switch ( policy ) {
                case ChecksumPolicy::PreserveOriginal:
                    if ( !_isDirty ) {
                        checksumByte = _originalChecksum;
                    } else if ( _originalChecksum == 0u ) {
                        checksumByte = 0u;
                    } else {
                        checksumByte = _ComputeChecksumForPayload( payload );
                    }
                    break;
                case ChecksumPolicy::AlwaysCompute:
                    checksumByte = _isDirty
                        ? _ComputeChecksumForPayload( payload )
                        : _originalChecksum;
                    break;
                case ChecksumPolicy::RepairAll:
                    checksumByte = _ComputeChecksumForPayload( payload );
                    break;
                case ChecksumPolicy::AlwaysZero:
                    checksumByte = _isDirty ? 0u : _originalChecksum;
                    break;
            }
        }

        framed.push_back( static_cast<std::byte>( checksumByte ) );
        return framed;
    }

    //=========================================================================
    // 1. OmfUnknownRecord
    //=========================================================================
    OmfUnknownRecord::OmfUnknownRecord( RecordType type, std::vector<std::byte> payload )
        : OmfRecord( type ),
          _payload( std::move( payload ) ) {}

    OmfUnknownRecord::OmfUnknownRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ),
          _payload( rawPayload.begin(), rawPayload.end() ) {}

    void OmfUnknownRecord::SetRecordType( RecordType newType ) noexcept {
        SetRecordTypeProtected( newType );
    }

    void OmfUnknownRecord::SetRawPayload( std::vector<std::byte> newPayload ) {
        _payload = std::move( newPayload );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfUnknownRecord::Clone() const {
        return std::make_unique<OmfUnknownRecord>( *this );
    }

    std::vector<std::byte> OmfUnknownRecord::SerializePayload() const {
        return _payload;
    }

    //=========================================================================
    // 2. OmfHeaderRecord (THEADR 0x80 / LHEADR 0x82)
    //=========================================================================
    OmfHeaderRecord::OmfHeaderRecord( std::string moduleName, bool isLibraryHeader )
        : OmfRecord( isLibraryHeader ? RecordType::Lheadr : RecordType::Theadr ),
          _moduleName( std::move( moduleName ) ) {}

    OmfHeaderRecord::OmfHeaderRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _moduleName = reader.ReadString();
    }

    void OmfHeaderRecord::SetIsLibraryHeader( bool isLibraryHeader ) noexcept {
        SetRecordTypeProtected( isLibraryHeader ? RecordType::Lheadr : RecordType::Theadr );
    }

    void OmfHeaderRecord::SetModuleName( std::string newName ) {
        if ( _moduleName != newName ) {
            _moduleName = std::move( newName );
            MarkDirty();
        }
    }

    std::unique_ptr<OmfRecord> OmfHeaderRecord::Clone() const {
        return std::make_unique<OmfHeaderRecord>( *this );
    }

    std::vector<std::byte> OmfHeaderRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteString( _moduleName );
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 3. OmfComentRecord (COMENT 0x88)
    //=========================================================================
    OmfComentRecord::OmfComentRecord(
        bool                   noPurge,
        bool                   noList,
        std::uint8_t           commentClass,
        std::vector<std::byte> commentData )
        : OmfRecord( RecordType::Coment ),
          _noPurge( noPurge ),
          _noList( noList ),
          _commentClass( commentClass ),
          _commentData( std::move( commentData ) ) {}

    OmfComentRecord::OmfComentRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Coment, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Coment );
        std::uint8_t const typeByte = reader.ReadUInt8();
        _noPurge      = ( typeByte & 0x80u ) != 0;
        _noList       = ( typeByte & 0x40u ) != 0;
        _commentClass = reader.ReadUInt8();
        _commentData  = reader.ReadRemainingBytes();
    }

    void OmfComentRecord::SetNoPurge( bool value ) noexcept {
        if ( _noPurge != value ) {
            _noPurge = value;
            MarkDirty();
        }
    }

    void OmfComentRecord::SetNoList( bool value ) noexcept {
        if ( _noList != value ) {
            _noList = value;
            MarkDirty();
        }
    }

    void OmfComentRecord::SetCommentClass( std::uint8_t newClass ) noexcept {
        if ( _commentClass != newClass ) {
            _commentClass = newClass;
            MarkDirty();
        }
    }

    void OmfComentRecord::SetCommentData( std::vector<std::byte> newData ) {
        _commentData = std::move( newData );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfComentRecord::Clone() const {
        return std::make_unique<OmfComentRecord>( *this );
    }

    std::vector<std::byte> OmfComentRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        std::uint8_t const typeByte = static_cast<std::uint8_t>(
            ( _noPurge ? 0x80u : 0x00u ) |
            ( _noList  ? 0x40u : 0x00u ) );
        writer.WriteUInt8( typeByte );
        writer.WriteUInt8( _commentClass );
        writer.WriteBytes( _commentData );
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 4. OmfModendRecord (MODEND 0x8A / MODEND32 0x8B)
    //=========================================================================
    OmfModendRecord::OmfModendRecord( bool isMainModule, bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Modend32 : RecordType::Modend ),
          _isMainModule( isMainModule ),
          _startAddress( std::monostate{} ) {}

    OmfModendRecord::OmfModendRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        std::uint8_t const modType = reader.ReadUInt8();
        _isMainModule             = ( modType & 0x80u ) != 0;
        bool const hasStart       = ( modType & 0x40u ) != 0;
        bool const isRelocatable  = ( modType & 0x01u ) != 0;

        if ( hasStart && !reader.IsAtEnd() ) {
            if ( isRelocatable ) {
                _startAddress = reader.ReadFixupTargetSpec( Is32Bit() );
            } else {
                AbsoluteStartAddress absAddr{};
                absAddr.frameNumber = reader.ReadUInt16();
                absAddr.offset      = reader.ReadOffset( Is32Bit() );
                _startAddress       = absAddr;
            }
        }
    }

    void OmfModendRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Modend32 : RecordType::Modend );
    }

    void OmfModendRecord::SetIsMainModule( bool value ) noexcept {
        if ( _isMainModule != value ) {
            _isMainModule = value;
            MarkDirty();
        }
    }

    void OmfModendRecord::SetStartAddress( StartAddress address ) {
        _startAddress = std::move( address );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfModendRecord::Clone() const {
        return std::make_unique<OmfModendRecord>( *this );
    }

    std::vector<std::byte> OmfModendRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        std::uint8_t modType = _isMainModule ? 0x80u : 0x00u;
        if ( std::holds_alternative<FixupTargetSpec>( _startAddress ) ) {
            modType |= 0x41u; // Start address present + Relocatable
            writer.WriteUInt8( modType );
            writer.WriteFixupTargetSpec( std::get<FixupTargetSpec>( _startAddress ), Is32Bit() );
        } else if ( std::holds_alternative<AbsoluteStartAddress>( _startAddress ) ) {
            modType |= 0x40u; // Start address present + Physical/Absolute
            writer.WriteUInt8( modType );
            const auto& absAddr = std::get<AbsoluteStartAddress>( _startAddress );
            writer.WriteUInt16( absAddr.frameNumber );
            writer.WriteOffset( absAddr.offset, Is32Bit() );
        } else {
            writer.WriteUInt8( modType );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 5. OmfExtdefRecord (EXTDEF 0x8C, LEXTDEF 0xB4 / 0xB5)
    //=========================================================================
    OmfExtdefRecord::OmfExtdefRecord( RecordType type )
        : OmfRecord( type ) {}

    OmfExtdefRecord::OmfExtdefRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        while ( !reader.IsAtEnd() ) {
            ExternalSymbol sym{};
            sym.name      = reader.ReadString();
            sym.typeIndex = reader.ReadIndex<TypeIndexTag>();
            _symbols.push_back( std::move( sym ) );
        }
    }

    void OmfExtdefRecord::SetIsLocal( bool isLocal, bool is32Bit ) noexcept {
        if ( !isLocal ) {
            SetRecordTypeProtected( RecordType::Extdef );
        } else {
            SetRecordTypeProtected( is32Bit ? RecordType::Lextdef32 : RecordType::Lextdef );
        }
    }

    void OmfExtdefRecord::SetSymbols( std::vector<ExternalSymbol> symbols ) {
        _symbols = std::move( symbols );
        MarkDirty();
    }

    void OmfExtdefRecord::SetSymbolName( std::size_t index, std::string newName ) {
        if ( index >= _symbols.size() ) {
            throw OmfException( "EXTDEF symbol index out of range" );
        }
        if ( _symbols[index].name != newName ) {
            _symbols[index].name = std::move( newName );
            MarkDirty();
        }
    }

    void OmfExtdefRecord::AddSymbol( ExternalSymbol symbol ) {
        _symbols.push_back( std::move( symbol ) );
        MarkDirty();
    }

    std::size_t OmfExtdefRecord::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t count = 0;
        for ( auto& sym : _symbols ) {
            if ( sym.name == oldName ) {
                sym.name = std::string( newName );
                ++count;
            }
        }
        if ( count > 0 ) {
            MarkDirty();
        }
        return count;
    }

    std::unique_ptr<OmfRecord> OmfExtdefRecord::Clone() const {
        return std::make_unique<OmfExtdefRecord>( *this );
    }

    std::vector<std::byte> OmfExtdefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& sym : _symbols ) {
            writer.WriteString( sym.name );
            writer.WriteIndex( sym.typeIndex );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 6. OmfCextdefRecord (CEXTDEF 0xBC)
    //=========================================================================
    OmfCextdefRecord::OmfCextdefRecord()
        : OmfRecord( RecordType::Cextdef ) {}

    OmfCextdefRecord::OmfCextdefRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Cextdef, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Cextdef );
        while ( !reader.IsAtEnd() ) {
            ComdatExternal ext{};
            ext.logicalNameIndex = reader.ReadIndex<NameIndexTag>();
            ext.typeIndex        = reader.ReadIndex<TypeIndexTag>();
            _externals.push_back( ext );
        }
    }

    void OmfCextdefRecord::SetExternals( std::vector<ComdatExternal> externals ) {
        _externals = std::move( externals );
        MarkDirty();
    }

    void OmfCextdefRecord::AddExternal( ComdatExternal ext ) {
        _externals.push_back( ext );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfCextdefRecord::Clone() const {
        return std::make_unique<OmfCextdefRecord>( *this );
    }

    std::vector<std::byte> OmfCextdefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& ext : _externals ) {
            writer.WriteIndex( ext.logicalNameIndex );
            writer.WriteIndex( ext.typeIndex );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 7. OmfTypdefRecord (TYPDEF 0x8E)
    //=========================================================================
    OmfTypdefRecord::OmfTypdefRecord()
        : OmfRecord( RecordType::Typdef ) {}

    OmfTypdefRecord::OmfTypdefRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Typdef, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Typdef );
        _typeName          = reader.ReadString();
        _rawTypeDescriptor = reader.ReadRemainingBytes();
    }

    void OmfTypdefRecord::SetTypeName( std::string name ) {
        if ( _typeName != name ) {
            _typeName = std::move( name );
            MarkDirty();
        }
    }

    void OmfTypdefRecord::SetRawTypeDescriptor( std::vector<std::byte> descriptor ) {
        _rawTypeDescriptor = std::move( descriptor );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfTypdefRecord::Clone() const {
        return std::make_unique<OmfTypdefRecord>( *this );
    }

    std::vector<std::byte> OmfTypdefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteString( _typeName );
        writer.WriteBytes( _rawTypeDescriptor );
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 8. OmfPubdefRecord (PUBDEF 0x90/0x91, LPUBDEF 0xB6/0xB7)
    //=========================================================================
    OmfPubdefRecord::OmfPubdefRecord( RecordType type )
        : OmfRecord( type ) {}

    OmfPubdefRecord::OmfPubdefRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _baseGroup   = reader.ReadIndex<GroupIndexTag>();
        _baseSegment = reader.ReadIndex<SegmentIndexTag>();
        if ( !_baseSegment.IsValid() ) {
            _baseFrame = reader.ReadUInt16();
        }
        while ( !reader.IsAtEnd() ) {
            PublicSymbol sym{};
            sym.name      = reader.ReadString();
            sym.offset    = reader.ReadOffset( Is32Bit() );
            sym.typeIndex = reader.ReadIndex<TypeIndexTag>();
            _symbols.push_back( std::move( sym ) );
        }
    }

    void OmfPubdefRecord::SetFormat( bool isLocal, bool is32Bit ) noexcept {
        if ( isLocal ) {
            SetRecordTypeProtected( is32Bit ? RecordType::Lpubdef32 : RecordType::Lpubdef );
        } else {
            SetRecordTypeProtected( is32Bit ? RecordType::Pubdef32 : RecordType::Pubdef );
        }
    }

    void OmfPubdefRecord::SetBaseGroup( GroupIndex group ) noexcept {
        if ( _baseGroup != group ) {
            _baseGroup = group;
            MarkDirty();
        }
    }

    void OmfPubdefRecord::SetBaseSegment( SegmentIndex segment ) noexcept {
        if ( _baseSegment != segment ) {
            _baseSegment = segment;
            MarkDirty();
        }
    }

    void OmfPubdefRecord::SetBaseFrame( std::optional<std::uint16_t> frame ) noexcept {
        if ( _baseFrame != frame ) {
            _baseFrame = frame;
            MarkDirty();
        }
    }

    void OmfPubdefRecord::SetSymbols( std::vector<PublicSymbol> symbols ) {
        _symbols = std::move( symbols );
        MarkDirty();
    }

    void OmfPubdefRecord::SetSymbolName( std::size_t index, std::string newName ) {
        if ( index >= _symbols.size() ) {
            throw OmfException( "PUBDEF symbol index out of range" );
        }
        if ( _symbols[index].name != newName ) {
            _symbols[index].name = std::move( newName );
            MarkDirty();
        }
    }

    void OmfPubdefRecord::SetSymbolOffset( std::size_t index, std::uint32_t newOffset ) noexcept {
        if ( index < _symbols.size() && _symbols[index].offset != newOffset ) {
            _symbols[index].offset = newOffset;
            MarkDirty();
        }
    }

    void OmfPubdefRecord::AddSymbol( PublicSymbol symbol ) {
        _symbols.push_back( std::move( symbol ) );
        MarkDirty();
    }

    std::size_t OmfPubdefRecord::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t count = 0;
        for ( auto& sym : _symbols ) {
            if ( sym.name == oldName ) {
                sym.name = std::string( newName );
                ++count;
            }
        }
        if ( count > 0 ) {
            MarkDirty();
        }
        return count;
    }

    std::unique_ptr<OmfRecord> OmfPubdefRecord::Clone() const {
        return std::make_unique<OmfPubdefRecord>( *this );
    }

    std::vector<std::byte> OmfPubdefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteIndex( _baseGroup );
        writer.WriteIndex( _baseSegment );
        if ( !_baseSegment.IsValid() ) {
            writer.WriteUInt16( _baseFrame.value_or( 0 ) );
        }
        for ( const auto& sym : _symbols ) {
            writer.WriteString( sym.name );
            writer.WriteOffset( sym.offset, Is32Bit() );
            writer.WriteIndex( sym.typeIndex );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 9. OmfLocsymRecord (LOCSYM 0x92 / LOCSYM32 0x93)
    //=========================================================================
    OmfLocsymRecord::OmfLocsymRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Locsym32 : RecordType::Locsym ) {}

    OmfLocsymRecord::OmfLocsymRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _baseGroup   = reader.ReadIndex<GroupIndexTag>();
        _baseSegment = reader.ReadIndex<SegmentIndexTag>();
        if ( !_baseSegment.IsValid() ) {
            _baseFrame = reader.ReadUInt16();
        }
        while ( !reader.IsAtEnd() ) {
            LocalSymbol sym{};
            sym.name      = reader.ReadString();
            sym.offset    = reader.ReadOffset( Is32Bit() );
            sym.typeIndex = reader.ReadIndex<TypeIndexTag>();
            _symbols.push_back( std::move( sym ) );
        }
    }

    void OmfLocsymRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Locsym32 : RecordType::Locsym );
    }

    void OmfLocsymRecord::SetBaseGroup( GroupIndex group ) noexcept {
        if ( _baseGroup != group ) {
            _baseGroup = group;
            MarkDirty();
        }
    }

    void OmfLocsymRecord::SetBaseSegment( SegmentIndex segment ) noexcept {
        if ( _baseSegment != segment ) {
            _baseSegment = segment;
            MarkDirty();
        }
    }

    void OmfLocsymRecord::SetBaseFrame( std::optional<std::uint16_t> frame ) noexcept {
        if ( _baseFrame != frame ) {
            _baseFrame = frame;
            MarkDirty();
        }
    }

    void OmfLocsymRecord::SetSymbols( std::vector<LocalSymbol> symbols ) {
        _symbols = std::move( symbols );
        MarkDirty();
    }

    std::size_t OmfLocsymRecord::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t count = 0;
        for ( auto& sym : _symbols ) {
            if ( sym.name == oldName ) {
                sym.name = std::string( newName );
                ++count;
            }
        }
        if ( count > 0 ) {
            MarkDirty();
        }
        return count;
    }

    std::unique_ptr<OmfRecord> OmfLocsymRecord::Clone() const {
        return std::make_unique<OmfLocsymRecord>( *this );
    }

    std::vector<std::byte> OmfLocsymRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteIndex( _baseGroup );
        writer.WriteIndex( _baseSegment );
        if ( !_baseSegment.IsValid() ) {
            writer.WriteUInt16( _baseFrame.value_or( 0 ) );
        }
        for ( const auto& sym : _symbols ) {
            writer.WriteString( sym.name );
            writer.WriteOffset( sym.offset, Is32Bit() );
            writer.WriteIndex( sym.typeIndex );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 10. OmfLinnumRecord (LINNUM 0x94 / LINNUM32 0x95)
    //=========================================================================
    OmfLinnumRecord::OmfLinnumRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Linnum32 : RecordType::Linnum ) {}

    OmfLinnumRecord::OmfLinnumRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _baseGroup   = reader.ReadIndex<GroupIndexTag>();
        _baseSegment = reader.ReadIndex<SegmentIndexTag>();
        while ( !reader.IsAtEnd() ) {
            LineNumberEntry entry{};
            entry.lineNumber = reader.ReadUInt16();
            entry.offset     = reader.ReadOffset( Is32Bit() );
            _entries.push_back( entry );
        }
    }

    void OmfLinnumRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Linnum32 : RecordType::Linnum );
    }

    void OmfLinnumRecord::SetBaseGroup( GroupIndex group ) noexcept {
        if ( _baseGroup != group ) {
            _baseGroup = group;
            MarkDirty();
        }
    }

    void OmfLinnumRecord::SetBaseSegment( SegmentIndex segment ) noexcept {
        if ( _baseSegment != segment ) {
            _baseSegment = segment;
            MarkDirty();
        }
    }

    void OmfLinnumRecord::SetEntries( std::vector<LineNumberEntry> entries ) {
        _entries = std::move( entries );
        MarkDirty();
    }

    void OmfLinnumRecord::AddEntry( LineNumberEntry entry ) {
        _entries.push_back( entry );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfLinnumRecord::Clone() const {
        return std::make_unique<OmfLinnumRecord>( *this );
    }

    std::vector<std::byte> OmfLinnumRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteIndex( _baseGroup );
        writer.WriteIndex( _baseSegment );
        for ( const auto& entry : _entries ) {
            writer.WriteUInt16( entry.lineNumber );
            writer.WriteOffset( entry.offset, Is32Bit() );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 11. OmfLinsymRecord (LINSYM 0xC4 / LINSYM32 0xC5)
    //=========================================================================
    OmfLinsymRecord::OmfLinsymRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Linsym32 : RecordType::Linsym ) {}

    OmfLinsymRecord::OmfLinsymRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _flags           = reader.ReadUInt8();
        _symbolNameIndex = reader.ReadIndex<NameIndexTag>();
        while ( !reader.IsAtEnd() ) {
            LineNumberEntry entry{};
            entry.lineNumber = reader.ReadUInt16();
            entry.offset     = reader.ReadOffset( Is32Bit() );
            _entries.push_back( entry );
        }
    }

    void OmfLinsymRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Linsym32 : RecordType::Linsym );
    }

    void OmfLinsymRecord::SetFlags( std::uint8_t flags ) noexcept {
        if ( _flags != flags ) {
            _flags = flags;
            MarkDirty();
        }
    }

    void OmfLinsymRecord::SetSymbolNameIndex( NameIndex index ) noexcept {
        if ( _symbolNameIndex != index ) {
            _symbolNameIndex = index;
            MarkDirty();
        }
    }

    void OmfLinsymRecord::SetEntries( std::vector<LineNumberEntry> entries ) {
        _entries = std::move( entries );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfLinsymRecord::Clone() const {
        return std::make_unique<OmfLinsymRecord>( *this );
    }

    std::vector<std::byte> OmfLinsymRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt8( _flags );
        writer.WriteIndex( _symbolNameIndex );
        for ( const auto& entry : _entries ) {
            writer.WriteUInt16( entry.lineNumber );
            writer.WriteOffset( entry.offset, Is32Bit() );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 12. OmfLnamesRecord (LNAMES 0x96 / LLNAMES 0xCA)
    //=========================================================================
    OmfLnamesRecord::OmfLnamesRecord( bool isLocal )
        : OmfRecord( isLocal ? RecordType::Llnames : RecordType::Lnames ) {}

    OmfLnamesRecord::OmfLnamesRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        while ( !reader.IsAtEnd() ) {
            _names.push_back( reader.ReadString() );
        }
    }

    void OmfLnamesRecord::SetIsLocal( bool isLocal ) noexcept {
        SetRecordTypeProtected( isLocal ? RecordType::Llnames : RecordType::Lnames );
    }

    void OmfLnamesRecord::SetNames( std::vector<std::string> names ) {
        _names = std::move( names );
        MarkDirty();
    }

    void OmfLnamesRecord::SetName( std::size_t zeroBasedIndex, std::string newName ) {
        if ( zeroBasedIndex >= _names.size() ) {
            throw OmfException( "LNAMES index out of range" );
        }
        if ( _names[zeroBasedIndex] != newName ) {
            _names[zeroBasedIndex] = std::move( newName );
            MarkDirty();
        }
    }

    void OmfLnamesRecord::AddName( std::string name ) {
        _names.push_back( std::move( name ) );
        MarkDirty();
    }

    std::size_t OmfLnamesRecord::Rename( std::string_view oldName, std::string_view newName ) {
        std::size_t count = 0;
        for ( auto& name : _names ) {
            if ( name == oldName ) {
                name = std::string( newName );
                ++count;
            }
        }
        if ( count > 0 ) {
            MarkDirty();
        }
        return count;
    }

    std::unique_ptr<OmfRecord> OmfLnamesRecord::Clone() const {
        return std::make_unique<OmfLnamesRecord>( *this );
    }

    std::vector<std::byte> OmfLnamesRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& name : _names ) {
            writer.WriteString( name );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 13. OmfSegdefRecord (SEGDEF 0x98 / SEGDEF32 0x99)
    //=========================================================================
    OmfSegdefRecord::OmfSegdefRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Segdef32 : RecordType::Segdef ) {}

    OmfSegdefRecord::OmfSegdefRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        std::uint8_t const acbp = reader.ReadUInt8();
        _alignment   = static_cast<Alignment>( ( acbp >> 5 ) & 0x07u );
        _combination = static_cast<Combination>( ( acbp >> 2 ) & 0x07u );
        _isBig       = ( acbp & 0x02u ) != 0;
        _isUse32     = ( acbp & 0x01u ) != 0;

        if ( _alignment == Alignment::Absolute ) {
            _absoluteFrame  = reader.ReadUInt16();
            _absoluteOffset = reader.ReadUInt8();
        }

        _segmentLength    = reader.ReadOffset( Is32Bit() );
        _segmentNameIndex = reader.ReadIndex<NameIndexTag>();
        _classNameIndex   = reader.ReadIndex<NameIndexTag>();
        _overlayNameIndex = reader.ReadIndex<NameIndexTag>();
    }

    void OmfSegdefRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Segdef32 : RecordType::Segdef );
    }

    void OmfSegdefRecord::SetAlignment( Alignment value ) noexcept {
        if ( _alignment != value ) {
            _alignment = value;
            MarkDirty();
        }
    }

    void OmfSegdefRecord::SetCombination( Combination value ) noexcept {
        if ( _combination != value ) {
            _combination = value;
            MarkDirty();
        }
    }

    void OmfSegdefRecord::SetMaxLength( bool value ) noexcept {
        if ( _isBig != value ) {
            _isBig = value;
            MarkDirty();
        }
    }

    void OmfSegdefRecord::SetUse32( bool value ) noexcept {
        if ( _isUse32 != value ) {
            _isUse32 = value;
            MarkDirty();
        }
    }

    void OmfSegdefRecord::SetSegmentLength( std::uint32_t length ) noexcept {
        if ( _segmentLength != length ) {
            _segmentLength = length;
            MarkDirty();
        }
    }

    void OmfSegdefRecord::SetSegmentNameIndex( NameIndex index ) noexcept {
        if ( _segmentNameIndex != index ) {
            _segmentNameIndex = index;
            MarkDirty();
        }
    }

    void OmfSegdefRecord::SetClassNameIndex( NameIndex index ) noexcept {
        if ( _classNameIndex != index ) {
            _classNameIndex = index;
            MarkDirty();
        }
    }

    void OmfSegdefRecord::SetOverlayNameIndex( NameIndex index ) noexcept {
        if ( _overlayNameIndex != index ) {
            _overlayNameIndex = index;
            MarkDirty();
        }
    }

    void OmfSegdefRecord::SetAbsoluteAddress( std::uint16_t frame, std::uint8_t offset ) noexcept {
        _alignment      = Alignment::Absolute;
        _absoluteFrame  = frame;
        _absoluteOffset = offset;
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfSegdefRecord::Clone() const {
        return std::make_unique<OmfSegdefRecord>( *this );
    }

    std::vector<std::byte> OmfSegdefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        std::uint8_t const acbp = static_cast<std::uint8_t>(
            ( ( static_cast<std::uint8_t>( _alignment ) & 0x07u ) << 5 ) |
            ( ( static_cast<std::uint8_t>( _combination ) & 0x07u ) << 2 ) |
            ( _isBig   ? 0x02u : 0x00u ) |
            ( _isUse32 ? 0x01u : 0x00u ) );
        writer.WriteUInt8( acbp );

        if ( _alignment == Alignment::Absolute ) {
            writer.WriteUInt16( _absoluteFrame.value_or( 0 ) );
            writer.WriteUInt8( _absoluteOffset.value_or( 0 ) );
        }

        writer.WriteOffset( _segmentLength, Is32Bit() );
        writer.WriteIndex( _segmentNameIndex );
        writer.WriteIndex( _classNameIndex );
        writer.WriteIndex( _overlayNameIndex );
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 14. OmfGrpdefRecord (GRPDEF 0x9A)
    //=========================================================================
    OmfGrpdefRecord::OmfGrpdefRecord( NameIndex groupNameIndex )
        : OmfRecord( RecordType::Grpdef ),
          _groupNameIndex( groupNameIndex ) {}

    OmfGrpdefRecord::OmfGrpdefRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Grpdef, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Grpdef );
        _groupNameIndex = reader.ReadIndex<NameIndexTag>();
        while ( !reader.IsAtEnd() ) {
            Component comp{};
            comp.descriptor   = reader.ReadUInt8();
            comp.segmentIndex = reader.ReadIndex<SegmentIndexTag>();
            _components.push_back( comp );
        }
    }

    void OmfGrpdefRecord::SetGroupNameIndex( NameIndex index ) noexcept {
        if ( _groupNameIndex != index ) {
            _groupNameIndex = index;
            MarkDirty();
        }
    }

    void OmfGrpdefRecord::SetComponents( std::vector<Component> components ) {
        _components = std::move( components );
        MarkDirty();
    }

    void OmfGrpdefRecord::AddComponent( Component component ) {
        _components.push_back( component );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfGrpdefRecord::Clone() const {
        return std::make_unique<OmfGrpdefRecord>( *this );
    }

    std::vector<std::byte> OmfGrpdefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteIndex( _groupNameIndex );
        for ( const auto& comp : _components ) {
            writer.WriteUInt8( comp.descriptor );
            writer.WriteIndex( comp.segmentIndex );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 15. OmfFixuppRecord (FIXUPP 0x9C / FIXUPP32 0x9D)
    //=========================================================================
    OmfFixuppRecord::OmfFixuppRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Fixupp32 : RecordType::Fixupp ) {}

    OmfFixuppRecord::OmfFixuppRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        while ( !reader.IsAtEnd() ) {
            std::uint8_t const firstByte = reader.ReadUInt8();
            if ( ( firstByte & 0x80u ) == 0 ) {
                // THREAD subrecord
                ThreadSubrecord trd{};
                trd.isFrameThread = ( firstByte & 0x40u ) != 0;
                trd.method        = static_cast<std::uint8_t>( ( firstByte >> 2 ) & 0x07u );
                trd.threadNumber  = static_cast<std::uint8_t>( firstByte & 0x03u );
                if ( !trd.isFrameThread || ( trd.method != 4u && trd.method != 5u ) ) {
                    trd.index = reader.ReadRawIndex();
                }
                _subrecords.push_back( trd );
            } else {
                // FIXUP subrecord (LOCAT is 2 bytes big-endian)
                std::uint8_t const secondByte = reader.ReadUInt8();
                FixupSubrecord fix{};
                fix.isSegmentRelative = ( firstByte & 0x40u ) != 0;
                fix.location          = static_cast<FixupLocationType>( ( firstByte >> 2 ) & 0x0Fu );
                fix.dataRecordOffset  = static_cast<std::uint16_t>(
                    ( ( firstByte & 0x03u ) << 8 ) | secondByte );
                fix.target = reader.ReadFixupTargetSpec( Is32Bit() );
                _subrecords.push_back( fix );
            }
        }
    }

    void OmfFixuppRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Fixupp32 : RecordType::Fixupp );
    }

    void OmfFixuppRecord::SetSubrecords( std::vector<Subrecord> subrecords ) {
        _subrecords = std::move( subrecords );
        MarkDirty();
    }

    void OmfFixuppRecord::AddSubrecord( Subrecord subrecord ) {
        _subrecords.push_back( std::move( subrecord ) );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfFixuppRecord::Clone() const {
        return std::make_unique<OmfFixuppRecord>( *this );
    }

    std::vector<std::byte> OmfFixuppRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& sub : _subrecords ) {
            if ( std::holds_alternative<ThreadSubrecord>( sub ) ) {
                const auto& trd = std::get<ThreadSubrecord>( sub );
                std::uint8_t const trdByte = static_cast<std::uint8_t>(
                    ( trd.isFrameThread ? 0x40u : 0x00u ) |
                    ( ( trd.method & 0x07u ) << 2 ) |
                    ( trd.threadNumber & 0x03u ) );
                writer.WriteUInt8( trdByte );
                if ( !trd.isFrameThread || ( trd.method != 4u && trd.method != 5u ) ) {
                    writer.WriteRawIndex( trd.index );
                }
            } else {
                const auto& fix = std::get<FixupSubrecord>( sub );
                std::uint8_t const locatHi = static_cast<std::uint8_t>(
                    0x80u |
                    ( fix.isSegmentRelative ? 0x40u : 0x00u ) |
                    ( ( static_cast<std::uint8_t>( fix.location ) & 0x0Fu ) << 2 ) |
                    ( ( fix.dataRecordOffset >> 8 ) & 0x03u ) );
                std::uint8_t const locatLo = static_cast<std::uint8_t>( fix.dataRecordOffset & 0xFFu );
                writer.WriteUInt8( locatHi );
                writer.WriteUInt8( locatLo );
                writer.WriteFixupTargetSpec( fix.target, Is32Bit() );
            }
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 16. OmfLedataRecord (LEDATA 0xA0 / LEDATA32 0xA1)
    //=========================================================================
    OmfLedataRecord::OmfLedataRecord(
        SegmentIndex           segmentIndex,
        std::uint32_t          dataOffset,
        std::vector<std::byte> data,
        bool                   is32Bit )
        : OmfRecord( is32Bit ? RecordType::Ledata32 : RecordType::Ledata ),
          _segmentIndex( segmentIndex ),
          _dataOffset( dataOffset ),
          _data( std::move( data ) ) {}

    OmfLedataRecord::OmfLedataRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _segmentIndex = reader.ReadIndex<SegmentIndexTag>();
        _dataOffset   = reader.ReadOffset( Is32Bit() );
        _data         = reader.ReadRemainingBytes();
    }

    void OmfLedataRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Ledata32 : RecordType::Ledata );
    }

    void OmfLedataRecord::SetSegmentIndex( SegmentIndex segment ) noexcept {
        if ( _segmentIndex != segment ) {
            _segmentIndex = segment;
            MarkDirty();
        }
    }

    void OmfLedataRecord::SetDataOffset( std::uint32_t offset ) noexcept {
        if ( _dataOffset != offset ) {
            _dataOffset = offset;
            MarkDirty();
        }
    }

    void OmfLedataRecord::SetData( std::vector<std::byte> newData ) {
        _data = std::move( newData );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfLedataRecord::Clone() const {
        return std::make_unique<OmfLedataRecord>( *this );
    }

    std::vector<std::byte> OmfLedataRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteIndex( _segmentIndex );
        writer.WriteOffset( _dataOffset, Is32Bit() );
        writer.WriteBytes( _data );
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 17. OmfLidataRecord (LIDATA 0xA2 / LIDATA32 0xA3)
    //=========================================================================
    OmfLidataRecord::OmfLidataRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Lidata32 : RecordType::Lidata ) {}

    OmfLidataRecord::OmfLidataRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _segmentIndex = reader.ReadIndex<SegmentIndexTag>();
        _dataOffset   = reader.ReadOffset( Is32Bit() );
        while ( !reader.IsAtEnd() ) {
            _blocks.push_back( reader.ReadIteratedDataBlock( Is32Bit() ) );
        }
    }

    void OmfLidataRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Lidata32 : RecordType::Lidata );
    }

    void OmfLidataRecord::SetSegmentIndex( SegmentIndex segment ) noexcept {
        if ( _segmentIndex != segment ) {
            _segmentIndex = segment;
            MarkDirty();
        }
    }

    void OmfLidataRecord::SetDataOffset( std::uint32_t offset ) noexcept {
        if ( _dataOffset != offset ) {
            _dataOffset = offset;
            MarkDirty();
        }
    }

    void OmfLidataRecord::SetBlocks( std::vector<IteratedDataBlock> blocks ) {
        _blocks = std::move( blocks );
        MarkDirty();
    }

    std::vector<std::byte> OmfLidataRecord::ExpandData() const {
        std::vector<std::byte> expanded;
        for ( const auto& block : _blocks ) {
            ExpandIteratedBlockRecursive( block, expanded );
        }
        return expanded;
    }

    std::unique_ptr<OmfRecord> OmfLidataRecord::Clone() const {
        return std::make_unique<OmfLidataRecord>( *this );
    }

    std::vector<std::byte> OmfLidataRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteIndex( _segmentIndex );
        writer.WriteOffset( _dataOffset, Is32Bit() );
        for ( const auto& block : _blocks ) {
            writer.WriteIteratedDataBlock( block, Is32Bit() );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 18. OmfComdefRecord (COMDEF 0xB0 / LCOMDEF 0xB8)
    //=========================================================================
    OmfComdefRecord::OmfComdefRecord( bool isLocal )
        : OmfRecord( isLocal ? RecordType::Lcomdef : RecordType::Comdef ) {}

    OmfComdefRecord::OmfComdefRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        while ( !reader.IsAtEnd() ) {
            CommunalSymbol sym{};
            sym.name            = reader.ReadString();
            sym.typeIndex       = reader.ReadIndex<TypeIndexTag>();
            sym.dataSegmentType = reader.ReadUInt8();
            sym.elementCount    = reader.ReadCommunalNumber();
            if ( sym.dataSegmentType == static_cast<std::uint8_t>( DataSegmentType::FarComm ) ) {
                sym.elementSize = reader.ReadCommunalNumber();
            } else {
                sym.elementSize = 1;
            }
            _symbols.push_back( std::move( sym ) );
        }
    }

    void OmfComdefRecord::SetIsLocal( bool isLocal ) noexcept {
        SetRecordTypeProtected( isLocal ? RecordType::Lcomdef : RecordType::Comdef );
    }

    void OmfComdefRecord::SetSymbols( std::vector<CommunalSymbol> symbols ) {
        _symbols = std::move( symbols );
        MarkDirty();
    }

    void OmfComdefRecord::AddSymbol( CommunalSymbol symbol ) {
        _symbols.push_back( std::move( symbol ) );
        MarkDirty();
    }

    std::size_t OmfComdefRecord::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t count = 0;
        for ( auto& sym : _symbols ) {
            if ( sym.name == oldName ) {
                sym.name = std::string( newName );
                ++count;
            }
        }
        if ( count > 0 ) {
            MarkDirty();
        }
        return count;
    }

    std::unique_ptr<OmfRecord> OmfComdefRecord::Clone() const {
        return std::make_unique<OmfComdefRecord>( *this );
    }

    std::vector<std::byte> OmfComdefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& sym : _symbols ) {
            writer.WriteString( sym.name );
            writer.WriteIndex( sym.typeIndex );
            writer.WriteUInt8( sym.dataSegmentType );
            writer.WriteCommunalNumber( sym.elementCount );
            if ( sym.dataSegmentType == static_cast<std::uint8_t>( DataSegmentType::FarComm ) ) {
                writer.WriteCommunalNumber( sym.elementSize );
            }
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 19. OmfBakpatRecord (BAKPAT 0xB2 / BAKPAT32 0xB3)
    //=========================================================================
    OmfBakpatRecord::OmfBakpatRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Bakpat32 : RecordType::Bakpat ) {}

    OmfBakpatRecord::OmfBakpatRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _segmentIndex = reader.ReadIndex<SegmentIndexTag>();
        _locationType = static_cast<FixupLocationType>( reader.ReadUInt8() );
        while ( !reader.IsAtEnd() ) {
            BackpatchEntry entry{};
            entry.offset = reader.ReadOffset( Is32Bit() );
            entry.value  = reader.ReadOffset( Is32Bit() );
            _entries.push_back( entry );
        }
    }

    void OmfBakpatRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Bakpat32 : RecordType::Bakpat );
    }

    void OmfBakpatRecord::SetSegmentIndex( SegmentIndex segment ) noexcept {
        if ( _segmentIndex != segment ) {
            _segmentIndex = segment;
            MarkDirty();
        }
    }

    void OmfBakpatRecord::SetLocationType( FixupLocationType type ) noexcept {
        if ( _locationType != type ) {
            _locationType = type;
            MarkDirty();
        }
    }

    void OmfBakpatRecord::SetEntries( std::vector<BackpatchEntry> entries ) {
        _entries = std::move( entries );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfBakpatRecord::Clone() const {
        return std::make_unique<OmfBakpatRecord>( *this );
    }

    std::vector<std::byte> OmfBakpatRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteIndex( _segmentIndex );
        writer.WriteUInt8( static_cast<std::uint8_t>( _locationType ) );
        for ( const auto& entry : _entries ) {
            writer.WriteOffset( entry.offset, Is32Bit() );
            writer.WriteOffset( entry.value, Is32Bit() );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 20. OmfNbkpatRecord (NBKPAT 0xC8 / NBKPAT32 0xC9)
    //=========================================================================
    OmfNbkpatRecord::OmfNbkpatRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Nbkpat32 : RecordType::Nbkpat ) {}

    OmfNbkpatRecord::OmfNbkpatRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _locationType    = static_cast<FixupLocationType>( reader.ReadUInt8() );
        _publicNameIndex = reader.ReadIndex<NameIndexTag>();
        while ( !reader.IsAtEnd() ) {
            BackpatchEntry entry{};
            entry.offset = reader.ReadOffset( Is32Bit() );
            entry.value  = reader.ReadOffset( Is32Bit() );
            _entries.push_back( entry );
        }
    }

    void OmfNbkpatRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Nbkpat32 : RecordType::Nbkpat );
    }

    void OmfNbkpatRecord::SetLocationType( FixupLocationType type ) noexcept {
        if ( _locationType != type ) {
            _locationType = type;
            MarkDirty();
        }
    }

    void OmfNbkpatRecord::SetPublicNameIndex( NameIndex index ) noexcept {
        if ( _publicNameIndex != index ) {
            _publicNameIndex = index;
            MarkDirty();
        }
    }

    void OmfNbkpatRecord::SetEntries( std::vector<BackpatchEntry> entries ) {
        _entries = std::move( entries );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfNbkpatRecord::Clone() const {
        return std::make_unique<OmfNbkpatRecord>( *this );
    }

    std::vector<std::byte> OmfNbkpatRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt8( static_cast<std::uint8_t>( _locationType ) );
        writer.WriteIndex( _publicNameIndex );
        for ( const auto& entry : _entries ) {
            writer.WriteOffset( entry.offset, Is32Bit() );
            writer.WriteOffset( entry.value, Is32Bit() );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 21. OmfComdatRecord (COMDAT 0xC2 / COMDAT32 0xC3)
    //=========================================================================
    OmfComdatRecord::OmfComdatRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Comdat32 : RecordType::Comdat ) {}

    OmfComdatRecord::OmfComdatRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _flags             = reader.ReadUInt8();
        std::uint8_t const attr = reader.ReadUInt8();
        _selectionCriteria = static_cast<SelectionCriteria>( attr & 0xF0u );
        _allocationType    = static_cast<AllocationType>( attr & 0x0Fu );
        _alignment         = static_cast<OmfSegdefRecord::Alignment>( reader.ReadUInt8() & 0x0Fu );
        _dataOffset        = reader.ReadOffset( Is32Bit() );
        _typeIndex         = reader.ReadIndex<TypeIndexTag>();

        if ( _allocationType == AllocationType::Explicit ) {
            _publicBaseGroup   = reader.ReadIndex<GroupIndexTag>();
            _publicBaseSegment = reader.ReadIndex<SegmentIndexTag>();
            if ( !_publicBaseSegment.IsValid() ) {
                _publicBaseFrame = reader.ReadUInt16();
            }
        }

        _publicNameIndex = reader.ReadIndex<NameIndexTag>();

        if ( IsIteratedData() ) {
            std::vector<OmfLidataRecord::IteratedDataBlock> blocks;
            while ( !reader.IsAtEnd() ) {
                blocks.push_back( reader.ReadIteratedDataBlock( Is32Bit() ) );
            }
            _dataPayload = std::move( blocks );
        } else {
            _dataPayload = reader.ReadRemainingBytes();
        }
    }

    void OmfComdatRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Comdat32 : RecordType::Comdat );
    }

    void OmfComdatRecord::SetFlags( std::uint8_t flags ) noexcept {
        if ( _flags != flags ) {
            _flags = flags;
            MarkDirty();
        }
    }

    void OmfComdatRecord::SetSelectionCriteria( SelectionCriteria criteria ) noexcept {
        if ( _selectionCriteria != criteria ) {
            _selectionCriteria = criteria;
            MarkDirty();
        }
    }

    void OmfComdatRecord::SetAllocationType( AllocationType allocType ) noexcept {
        if ( _allocationType != allocType ) {
            _allocationType = allocType;
            MarkDirty();
        }
    }

    void OmfComdatRecord::SetAlignment( OmfSegdefRecord::Alignment align ) noexcept {
        if ( _alignment != align ) {
            _alignment = align;
            MarkDirty();
        }
    }

    void OmfComdatRecord::SetDataOffset( std::uint32_t offset ) noexcept {
        if ( _dataOffset != offset ) {
            _dataOffset = offset;
            MarkDirty();
        }
    }

    void OmfComdatRecord::SetTypeIndex( TypeIndex index ) noexcept {
        if ( _typeIndex != index ) {
            _typeIndex = index;
            MarkDirty();
        }
    }

    void OmfComdatRecord::SetExplicitBase(
        GroupIndex                   group,
        SegmentIndex                 segment,
        std::optional<std::uint16_t> frame ) noexcept {
        _allocationType    = AllocationType::Explicit;
        _publicBaseGroup   = group;
        _publicBaseSegment = segment;
        _publicBaseFrame   = frame;
        MarkDirty();
    }

    void OmfComdatRecord::SetPublicNameIndex( NameIndex index ) noexcept {
        if ( _publicNameIndex != index ) {
            _publicNameIndex = index;
            MarkDirty();
        }
    }

    void OmfComdatRecord::SetDataPayload( DataPayload payload ) {
        if ( std::holds_alternative<std::vector<OmfLidataRecord::IteratedDataBlock>>( payload ) ) {
            _flags |= 0x02u;
        } else {
            _flags &= static_cast<std::uint8_t>( ~0x02u );
        }
        _dataPayload = std::move( payload );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfComdatRecord::Clone() const {
        return std::make_unique<OmfComdatRecord>( *this );
    }

    std::vector<std::byte> OmfComdatRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt8( _flags );
        std::uint8_t const attr = static_cast<std::uint8_t>(
            ( static_cast<std::uint8_t>( _selectionCriteria ) & 0xF0u ) |
            ( static_cast<std::uint8_t>( _allocationType ) & 0x0Fu ) );
        writer.WriteUInt8( attr );
        writer.WriteUInt8( static_cast<std::uint8_t>( _alignment ) );
        writer.WriteOffset( _dataOffset, Is32Bit() );
        writer.WriteIndex( _typeIndex );

        if ( _allocationType == AllocationType::Explicit ) {
            writer.WriteIndex( _publicBaseGroup );
            writer.WriteIndex( _publicBaseSegment );
            if ( !_publicBaseSegment.IsValid() ) {
                writer.WriteUInt16( _publicBaseFrame.value_or( 0 ) );
            }
        }

        writer.WriteIndex( _publicNameIndex );

        if ( std::holds_alternative<std::vector<OmfLidataRecord::IteratedDataBlock>>( _dataPayload ) ) {
            const auto& blocks = std::get<std::vector<OmfLidataRecord::IteratedDataBlock>>( _dataPayload );
            for ( const auto& block : blocks ) {
                writer.WriteIteratedDataBlock( block, Is32Bit() );
            }
        } else {
            writer.WriteBytes( std::get<std::vector<std::byte>>( _dataPayload ) );
        }

        return writer.TakeBuffer();
    }

    //=========================================================================
    // 22. OmfAliasRecord (ALIAS 0xC6)
    //=========================================================================
    OmfAliasRecord::OmfAliasRecord()
        : OmfRecord( RecordType::Alias ) {}

    OmfAliasRecord::OmfAliasRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Alias, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Alias );
        while ( !reader.IsAtEnd() ) {
            AliasEntry entry{};
            entry.aliasName      = reader.ReadString();
            entry.substituteName = reader.ReadString();
            _aliases.push_back( std::move( entry ) );
        }
    }

    void OmfAliasRecord::SetAliases( std::vector<AliasEntry> aliases ) {
        _aliases = std::move( aliases );
        MarkDirty();
    }

    void OmfAliasRecord::AddAlias( AliasEntry entry ) {
        _aliases.push_back( std::move( entry ) );
        MarkDirty();
    }

    std::size_t OmfAliasRecord::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t count = 0;
        for ( auto& entry : _aliases ) {
            if ( entry.aliasName == oldName ) {
                entry.aliasName = std::string( newName );
                ++count;
            }
            if ( entry.substituteName == oldName ) {
                entry.substituteName = std::string( newName );
                ++count;
            }
        }
        if ( count > 0 ) {
            MarkDirty();
        }
        return count;
    }

    std::unique_ptr<OmfRecord> OmfAliasRecord::Clone() const {
        return std::make_unique<OmfAliasRecord>( *this );
    }

    std::vector<std::byte> OmfAliasRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& entry : _aliases ) {
            writer.WriteString( entry.aliasName );
            writer.WriteString( entry.substituteName );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 23. OmfVernumRecord (VERNUM 0xCC) & OmfVendextRecord (VENDEXT 0xCE)
    //=========================================================================
    OmfVernumRecord::OmfVernumRecord( std::string version )
        : OmfRecord( RecordType::Vernum ),
          _version( std::move( version ) ) {}

    OmfVernumRecord::OmfVernumRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Vernum, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Vernum );
        _version = reader.ReadString();
    }

    void OmfVernumRecord::SetVersion( std::string newVersion ) {
        if ( _version != newVersion ) {
            _version = std::move( newVersion );
            MarkDirty();
        }
    }

    std::unique_ptr<OmfRecord> OmfVernumRecord::Clone() const {
        return std::make_unique<OmfVernumRecord>( *this );
    }

    std::vector<std::byte> OmfVernumRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteString( _version );
        return writer.TakeBuffer();
    }

    OmfVendextRecord::OmfVendextRecord( std::uint16_t subtype, std::vector<std::byte> extensionData )
        : OmfRecord( RecordType::Vendext ),
          _subtype( subtype ),
          _extensionData( std::move( extensionData ) ) {}

    OmfVendextRecord::OmfVendextRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Vendext, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Vendext );
        _subtype       = reader.ReadUInt16();
        _extensionData = reader.ReadRemainingBytes();
    }

    void OmfVendextRecord::SetSubtype( std::uint16_t subtype ) noexcept {
        if ( _subtype != subtype ) {
            _subtype = subtype;
            MarkDirty();
        }
    }

    void OmfVendextRecord::SetExtensionData( std::vector<std::byte> data ) {
        _extensionData = std::move( data );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfVendextRecord::Clone() const {
        return std::make_unique<OmfVendextRecord>( *this );
    }

    std::vector<std::byte> OmfVendextRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt16( _subtype );
        writer.WriteBytes( _extensionData );
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 24. Intel OMF-86 Legacy Records (0x6E..0x7E, 0x84..0x86)
    //=========================================================================
    OmfRheadrRecord::OmfRheadrRecord()
        : OmfRecord( RecordType::Rheadr ) {}

    OmfRheadrRecord::OmfRheadrRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Rheadr, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Rheadr );
        _moduleName = reader.ReadString();
        if ( !reader.IsAtEnd() ) {
            _moduleType = reader.ReadUInt8();
        }
        _trailingAttributes = reader.ReadRemainingBytes();
    }

    void OmfRheadrRecord::SetModuleName( std::string name ) {
        if ( _moduleName != name ) {
            _moduleName = std::move( name );
            MarkDirty();
        }
    }

    void OmfRheadrRecord::SetModuleType( std::uint8_t type ) noexcept {
        if ( _moduleType != type ) {
            _moduleType = type;
            MarkDirty();
        }
    }

    void OmfRheadrRecord::SetTrailingAttributes( std::vector<std::byte> attrs ) {
        _trailingAttributes = std::move( attrs );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfRheadrRecord::Clone() const {
        return std::make_unique<OmfRheadrRecord>( *this );
    }

    std::vector<std::byte> OmfRheadrRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteString( _moduleName );
        writer.WriteUInt8( _moduleType );
        writer.WriteBytes( _trailingAttributes );
        return writer.TakeBuffer();
    }

    // REGINT (0x70)
    OmfRegintRecord::OmfRegintRecord()
        : OmfRecord( RecordType::Regint ) {}

    OmfRegintRecord::OmfRegintRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Regint, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Regint );
        while ( !reader.IsAtEnd() ) {
            RegisterInitEntry entry{};
            std::uint8_t const regType = reader.ReadUInt8();
            entry.registerId    = static_cast<RegisterId>( ( regType >> 6 ) & 0x03u );
            entry.isRelocatable = ( regType & 0x01u ) != 0;
            if ( entry.isRelocatable ) {
                entry.relocTarget = reader.ReadFixupTargetSpec( false );
            } else {
                entry.explicitFrame = reader.ReadUInt16();
                if ( entry.registerId == RegisterId::CsAndIp ||
                     entry.registerId == RegisterId::SsAndSp ) {
                    entry.explicitOffset = reader.ReadUInt16();
                }
            }
            _entries.push_back( entry );
        }
    }

    void OmfRegintRecord::SetEntries( std::vector<RegisterInitEntry> entries ) {
        _entries = std::move( entries );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfRegintRecord::Clone() const {
        return std::make_unique<OmfRegintRecord>( *this );
    }

    std::vector<std::byte> OmfRegintRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& entry : _entries ) {
            std::uint8_t const regType = static_cast<std::uint8_t>(
                ( ( static_cast<std::uint8_t>( entry.registerId ) & 0x03u ) << 6 ) |
                ( entry.isRelocatable ? 0x01u : 0x00u ) );
            writer.WriteUInt8( regType );
            if ( entry.isRelocatable ) {
                writer.WriteFixupTargetSpec( entry.relocTarget, false );
            } else {
                writer.WriteUInt16( entry.explicitFrame );
                if ( entry.registerId == RegisterId::CsAndIp ||
                     entry.registerId == RegisterId::SsAndSp ) {
                    writer.WriteUInt16( entry.explicitOffset.value_or( 0 ) );
                }
            }
        }
        return writer.TakeBuffer();
    }

    // REDATA (0x72)
    OmfRedataRecord::OmfRedataRecord()
        : OmfRecord( RecordType::Redata ) {}

    OmfRedataRecord::OmfRedataRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Redata, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Redata );
        _baseSpec = reader.ReadFixupTargetSpec( false );
        _data     = reader.ReadRemainingBytes();
    }

    void OmfRedataRecord::SetBaseSpec( FixupTargetSpec spec ) {
        _baseSpec = spec;
        MarkDirty();
    }

    void OmfRedataRecord::SetData( std::vector<std::byte> data ) {
        _data = std::move( data );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfRedataRecord::Clone() const {
        return std::make_unique<OmfRedataRecord>( *this );
    }

    std::vector<std::byte> OmfRedataRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteFixupTargetSpec( _baseSpec, false );
        writer.WriteBytes( _data );
        return writer.TakeBuffer();
    }

    // RIDATA (0x74)
    OmfRidataRecord::OmfRidataRecord()
        : OmfRecord( RecordType::Ridata ) {}

    OmfRidataRecord::OmfRidataRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Ridata, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Ridata );
        _baseSpec = reader.ReadFixupTargetSpec( false );
        while ( !reader.IsAtEnd() ) {
            _blocks.push_back( reader.ReadIteratedDataBlock( false ) );
        }
    }

    void OmfRidataRecord::SetBaseSpec( FixupTargetSpec spec ) {
        _baseSpec = spec;
        MarkDirty();
    }

    void OmfRidataRecord::SetBlocks( std::vector<IteratedDataBlock> blocks ) {
        _blocks = std::move( blocks );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfRidataRecord::Clone() const {
        return std::make_unique<OmfRidataRecord>( *this );
    }

    std::vector<std::byte> OmfRidataRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteFixupTargetSpec( _baseSpec, false );
        for ( const auto& block : _blocks ) {
            writer.WriteIteratedDataBlock( block, false );
        }
        return writer.TakeBuffer();
    }

    // OVLDEF (0x76)
    OmfOvldefRecord::OmfOvldefRecord()
        : OmfRecord( RecordType::Ovldef ) {}

    OmfOvldefRecord::OmfOvldefRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Ovldef, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Ovldef );
        _overlayName        = reader.ReadString();
        _locationBlock      = reader.ReadUInt16();
        _locationByteOffset = reader.ReadUInt16();
        _overlayAttributes  = reader.ReadUInt8();
        if ( ( _overlayAttributes & 0x02u ) != 0 ) {
            _sharedOverlayIndex = reader.ReadIndex<OverlayIndexTag>();
        }
        if ( ( _overlayAttributes & 0x01u ) != 0 ) {
            _adjacentOverlayIndex = reader.ReadIndex<OverlayIndexTag>();
        }
    }

    void OmfOvldefRecord::SetOverlayName( std::string name ) {
        if ( _overlayName != name ) {
            _overlayName = std::move( name );
            MarkDirty();
        }
    }

    void OmfOvldefRecord::SetLocation( std::uint16_t block, std::uint16_t byteOffset ) noexcept {
        _locationBlock      = block;
        _locationByteOffset = byteOffset;
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfOvldefRecord::Clone() const {
        return std::make_unique<OmfOvldefRecord>( *this );
    }

    std::vector<std::byte> OmfOvldefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteString( _overlayName );
        writer.WriteUInt16( _locationBlock );
        writer.WriteUInt16( _locationByteOffset );
        writer.WriteUInt8( _overlayAttributes );
        if ( ( _overlayAttributes & 0x02u ) != 0 ) {
            writer.WriteIndex( _sharedOverlayIndex );
        }
        if ( ( _overlayAttributes & 0x01u ) != 0 ) {
            writer.WriteIndex( _adjacentOverlayIndex );
        }
        return writer.TakeBuffer();
    }

    // ENDREC (0x78)
    OmfEndrecRecord::OmfEndrecRecord( EndType endType )
        : OmfRecord( RecordType::Endrec ),
          _endType( endType ) {}

    OmfEndrecRecord::OmfEndrecRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Endrec, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Endrec );
        _endType = static_cast<EndType>( reader.ReadUInt8() );
    }

    void OmfEndrecRecord::SetEndType( EndType type ) noexcept {
        if ( _endType != type ) {
            _endType = type;
            MarkDirty();
        }
    }

    std::unique_ptr<OmfRecord> OmfEndrecRecord::Clone() const {
        return std::make_unique<OmfEndrecRecord>( *this );
    }

    std::vector<std::byte> OmfEndrecRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt8( static_cast<std::uint8_t>( _endType ) );
        return writer.TakeBuffer();
    }

    // BLKDEF (0x7A / 0x7B)
    OmfBlkdefRecord::OmfBlkdefRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Blkdef32 : RecordType::Blkdef ) {}

    OmfBlkdefRecord::OmfBlkdefRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, type );
        _baseGroup   = reader.ReadIndex<GroupIndexTag>();
        _baseSegment = reader.ReadIndex<SegmentIndexTag>();
        if ( !_baseSegment.IsValid() ) {
            _baseFrame = reader.ReadUInt16();
        }
        _blockName      = reader.ReadString();
        _blockOffset    = reader.ReadOffset( Is32Bit() );
        _blockLength    = reader.ReadOffset( Is32Bit() );
        _procedureFlags = reader.ReadUInt8();
        if ( ( _procedureFlags & 0x80u ) != 0 ) {
            _returnAddressOffset = reader.ReadOffset( ( _procedureFlags & 0x40u ) != 0 );
        }
        if ( !_blockName.empty() && !reader.IsAtEnd() ) {
            _typeIndex = reader.ReadIndex<TypeIndexTag>();
        }
    }

    void OmfBlkdefRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Blkdef32 : RecordType::Blkdef );
    }

    void OmfBlkdefRecord::SetBase(
        GroupIndex                   group,
        SegmentIndex                 segment,
        std::optional<std::uint16_t> frame ) noexcept {
        _baseGroup   = group;
        _baseSegment = segment;
        _baseFrame   = frame;
        MarkDirty();
    }

    void OmfBlkdefRecord::SetBlockName( std::string name ) {
        if ( _blockName != name ) {
            _blockName = std::move( name );
            MarkDirty();
        }
    }

    void OmfBlkdefRecord::SetBlockOffset( std::uint32_t offset ) noexcept {
        if ( _blockOffset != offset ) {
            _blockOffset = offset;
            MarkDirty();
        }
    }

    void OmfBlkdefRecord::SetBlockLength( std::uint32_t length ) noexcept {
        if ( _blockLength != length ) {
            _blockLength = length;
            MarkDirty();
        }
    }

    void OmfBlkdefRecord::SetProcedureFlags( std::uint8_t flags ) noexcept {
        if ( _procedureFlags != flags ) {
            _procedureFlags = flags;
            MarkDirty();
        }
    }

    void OmfBlkdefRecord::SetReturnAddressOffset( std::optional<std::uint32_t> offset ) noexcept {
        _returnAddressOffset = offset;
        MarkDirty();
    }

    void OmfBlkdefRecord::SetTypeIndex( TypeIndex index ) noexcept {
        if ( _typeIndex != index ) {
            _typeIndex = index;
            MarkDirty();
        }
    }

    std::unique_ptr<OmfRecord> OmfBlkdefRecord::Clone() const {
        return std::make_unique<OmfBlkdefRecord>( *this );
    }

    std::vector<std::byte> OmfBlkdefRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteIndex( _baseGroup );
        writer.WriteIndex( _baseSegment );
        if ( !_baseSegment.IsValid() ) {
            writer.WriteUInt16( _baseFrame.value_or( 0 ) );
        }
        writer.WriteString( _blockName );
        writer.WriteOffset( _blockOffset, Is32Bit() );
        writer.WriteOffset( _blockLength, Is32Bit() );
        writer.WriteUInt8( _procedureFlags );
        if ( ( _procedureFlags & 0x80u ) != 0 ) {
            writer.WriteOffset(
                _returnAddressOffset.value_or( 0 ),
                ( _procedureFlags & 0x40u ) != 0 );
        }
        if ( !_blockName.empty() ) {
            writer.WriteIndex( _typeIndex );
        }
        return writer.TakeBuffer();
    }

    // BLKEND (0x7C / 0x7D)
    OmfBlkendRecord::OmfBlkendRecord( bool is32Bit )
        : OmfRecord( is32Bit ? RecordType::Blkend32 : RecordType::Blkend ) {}

    OmfBlkendRecord::OmfBlkendRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( type, rawPayload, originalChecksum, hadValidChecksum ) {}

    void OmfBlkendRecord::SetIs32Bit( bool is32Bit ) noexcept {
        SetRecordTypeProtected( is32Bit ? RecordType::Blkend32 : RecordType::Blkend );
    }

    std::unique_ptr<OmfRecord> OmfBlkendRecord::Clone() const {
        return std::make_unique<OmfBlkendRecord>( *this );
    }

    std::vector<std::byte> OmfBlkendRecord::SerializePayload() const {
        return {};
    }

    // DEBSYM (0x7E)
    OmfDebsymRecord::OmfDebsymRecord()
        : OmfRecord( RecordType::Debsym ) {}

    OmfDebsymRecord::OmfDebsymRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Debsym, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Debsym );
        _frameMethod = reader.ReadUInt8();
        _baseSpec    = reader.ReadFixupTargetSpec( false );
        while ( !reader.IsAtEnd() ) {
            DebugSymbol sym{};
            sym.name      = reader.ReadString();
            sym.offset    = reader.ReadUInt16();
            sym.typeIndex = reader.ReadIndex<TypeIndexTag>();
            _symbols.push_back( std::move( sym ) );
        }
    }

    void OmfDebsymRecord::SetFrameMethod( std::uint8_t method ) noexcept {
        if ( _frameMethod != method ) {
            _frameMethod = method;
            MarkDirty();
        }
    }

    void OmfDebsymRecord::SetBaseSpec( FixupTargetSpec spec ) {
        _baseSpec = spec;
        MarkDirty();
    }

    void OmfDebsymRecord::SetSymbols( std::vector<DebugSymbol> symbols ) {
        _symbols = std::move( symbols );
        MarkDirty();
    }

    std::size_t OmfDebsymRecord::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t count = 0;
        for ( auto& sym : _symbols ) {
            if ( sym.name == oldName ) {
                sym.name = std::string( newName );
                ++count;
            }
        }
        if ( count > 0 ) {
            MarkDirty();
        }
        return count;
    }

    std::unique_ptr<OmfRecord> OmfDebsymRecord::Clone() const {
        return std::make_unique<OmfDebsymRecord>( *this );
    }

    std::vector<std::byte> OmfDebsymRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt8( _frameMethod );
        writer.WriteFixupTargetSpec( _baseSpec, false );
        for ( const auto& sym : _symbols ) {
            writer.WriteString( sym.name );
            writer.WriteUInt16( static_cast<std::uint16_t>( sym.offset ) );
            writer.WriteIndex( sym.typeIndex );
        }
        return writer.TakeBuffer();
    }

    // PEDATA (0x84)
    OmfPedataRecord::OmfPedataRecord(
        std::uint16_t          frameNumber,
        std::uint8_t           offset,
        std::vector<std::byte> data )
        : OmfRecord( RecordType::Pedata ),
          _frameNumber( frameNumber ),
          _offset( offset ),
          _data( std::move( data ) ) {}

    OmfPedataRecord::OmfPedataRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Pedata, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Pedata );
        _frameNumber = reader.ReadUInt16();
        _offset      = reader.ReadUInt8();
        _data        = reader.ReadRemainingBytes();
    }

    void OmfPedataRecord::SetFrameNumber( std::uint16_t frame ) noexcept {
        if ( _frameNumber != frame ) {
            _frameNumber = frame;
            MarkDirty();
        }
    }

    void OmfPedataRecord::SetOffset( std::uint8_t offset ) noexcept {
        if ( _offset != offset ) {
            _offset = offset;
            MarkDirty();
        }
    }

    void OmfPedataRecord::SetData( std::vector<std::byte> data ) {
        _data = std::move( data );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfPedataRecord::Clone() const {
        return std::make_unique<OmfPedataRecord>( *this );
    }

    std::vector<std::byte> OmfPedataRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt16( _frameNumber );
        writer.WriteUInt8( _offset );
        writer.WriteBytes( _data );
        return writer.TakeBuffer();
    }

    // PIDATA (0x86)
    OmfPidataRecord::OmfPidataRecord()
        : OmfRecord( RecordType::Pidata ) {}

    OmfPidataRecord::OmfPidataRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Pidata, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Pidata );
        _frameNumber = reader.ReadUInt16();
        _offset      = reader.ReadUInt8();
        while ( !reader.IsAtEnd() ) {
            _blocks.push_back( reader.ReadIteratedDataBlock( false ) );
        }
    }

    void OmfPidataRecord::SetFrameNumber( std::uint16_t frame ) noexcept {
        if ( _frameNumber != frame ) {
            _frameNumber = frame;
            MarkDirty();
        }
    }

    void OmfPidataRecord::SetOffset( std::uint8_t offset ) noexcept {
        if ( _offset != offset ) {
            _offset = offset;
            MarkDirty();
        }
    }

    void OmfPidataRecord::SetBlocks( std::vector<IteratedDataBlock> blocks ) {
        _blocks = std::move( blocks );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfPidataRecord::Clone() const {
        return std::make_unique<OmfPidataRecord>( *this );
    }

    std::vector<std::byte> OmfPidataRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt16( _frameNumber );
        writer.WriteUInt8( _offset );
        for ( const auto& block : _blocks ) {
            writer.WriteIteratedDataBlock( block, false );
        }
        return writer.TakeBuffer();
    }

    //=========================================================================
    // 25. Library Records (Intel OMF 0xA4..0xAA and MS-DOS .LIB 0xF0..0xF1)
    //=========================================================================
    OmfLibhedRecord::OmfLibhedRecord()
        : OmfRecord( RecordType::Libhed ) {}

    OmfLibhedRecord::OmfLibhedRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Libhed, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Libhed );
        _moduleCount           = reader.ReadUInt32();
        _dictionaryBlockNumber = reader.ReadUInt16();
        _dictionaryByteOffset  = reader.ReadUInt16();
        if ( !reader.IsAtEnd() ) {
            _flags = reader.ReadUInt8();
        }
    }

    void OmfLibhedRecord::SetModuleCount( std::uint32_t count ) noexcept {
        if ( _moduleCount != count ) {
            _moduleCount = count;
            MarkDirty();
        }
    }

    void OmfLibhedRecord::SetDictionaryLocation( std::uint16_t block, std::uint16_t byteOffset ) noexcept {
        _dictionaryBlockNumber = block;
        _dictionaryByteOffset  = byteOffset;
        MarkDirty();
    }

    void OmfLibhedRecord::SetFlags( std::uint8_t flags ) noexcept {
        if ( _flags != flags ) {
            _flags = flags;
            MarkDirty();
        }
    }

    std::unique_ptr<OmfRecord> OmfLibhedRecord::Clone() const {
        return std::make_unique<OmfLibhedRecord>( *this );
    }

    std::vector<std::byte> OmfLibhedRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt32( _moduleCount );
        writer.WriteUInt16( _dictionaryBlockNumber );
        writer.WriteUInt16( _dictionaryByteOffset );
        writer.WriteUInt8( _flags );
        return writer.TakeBuffer();
    }

    // LIBNAM (0xA6)
    OmfLibnamRecord::OmfLibnamRecord()
        : OmfRecord( RecordType::Libnam ) {}

    OmfLibnamRecord::OmfLibnamRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Libnam, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Libnam );
        while ( !reader.IsAtEnd() ) {
            _moduleNames.push_back( reader.ReadString() );
        }
    }

    void OmfLibnamRecord::SetModuleNames( std::vector<std::string> names ) {
        _moduleNames = std::move( names );
        MarkDirty();
    }

    void OmfLibnamRecord::AddModuleName( std::string name ) {
        _moduleNames.push_back( std::move( name ) );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfLibnamRecord::Clone() const {
        return std::make_unique<OmfLibnamRecord>( *this );
    }

    std::vector<std::byte> OmfLibnamRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& name : _moduleNames ) {
            writer.WriteString( name );
        }
        return writer.TakeBuffer();
    }

    // LIBLOC (0xA8)
    OmfLiblocRecord::OmfLiblocRecord()
        : OmfRecord( RecordType::Libloc ) {}

    OmfLiblocRecord::OmfLiblocRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Libloc, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Libloc );
        while ( !reader.IsAtEnd() ) {
            ModuleLocation loc{};
            loc.blockNumber = reader.ReadUInt16();
            loc.byteOffset  = reader.ReadUInt16();
            _locations.push_back( loc );
        }
    }

    void OmfLiblocRecord::SetLocations( std::vector<ModuleLocation> locations ) {
        _locations = std::move( locations );
        MarkDirty();
    }

    void OmfLiblocRecord::AddLocation( ModuleLocation location ) {
        _locations.push_back( location );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfLiblocRecord::Clone() const {
        return std::make_unique<OmfLiblocRecord>( *this );
    }

    std::vector<std::byte> OmfLiblocRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& loc : _locations ) {
            writer.WriteUInt16( loc.blockNumber );
            writer.WriteUInt16( loc.byteOffset );
        }
        return writer.TakeBuffer();
    }

    // LIBDIC (0xAA)
    OmfLibdicRecord::OmfLibdicRecord()
        : OmfRecord( RecordType::Libdic ) {}

    OmfLibdicRecord::OmfLibdicRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::Libdic, rawPayload, originalChecksum, hadValidChecksum ) {
        OmfBinaryReader reader( rawPayload, RecordType::Libdic );
        std::vector<std::string> currentGroup;
        while ( !reader.IsAtEnd() ) {
            std::string name = reader.ReadString();
            if ( name.empty() ) {
                // Empty string (0x00 byte) terminates the current module's group in LIBDIC
                _modulePublicGroups.push_back( std::move( currentGroup ) );
                currentGroup.clear();
            } else {
                currentGroup.push_back( std::move( name ) );
            }
        }
        if ( !currentGroup.empty() ) {
            _modulePublicGroups.push_back( std::move( currentGroup ) );
        }
    }

    void OmfLibdicRecord::SetModulePublicGroups( std::vector<std::vector<std::string>> groups ) {
        _modulePublicGroups = std::move( groups );
        MarkDirty();
    }

    std::size_t OmfLibdicRecord::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t count = 0;
        for ( auto& group : _modulePublicGroups ) {
            for ( auto& name : group ) {
                if ( name == oldName ) {
                    name = std::string( newName );
                    ++count;
                }
            }
        }
        if ( count > 0 ) {
            MarkDirty();
        }
        return count;
    }

    std::unique_ptr<OmfRecord> OmfLibdicRecord::Clone() const {
        return std::make_unique<OmfLibdicRecord>( *this );
    }

    std::vector<std::byte> OmfLibdicRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        for ( const auto& group : _modulePublicGroups ) {
            for ( const auto& name : group ) {
                writer.WriteString( name );
            }
            writer.WriteUInt8( 0 ); // 0x00 group terminator
        }
        return writer.TakeBuffer();
    }

    // MsLibHeader (0xF0)
    OmfMsLibHeaderRecord::OmfMsLibHeaderRecord()
        : OmfRecord( RecordType::MsLibHeader ) {}

    OmfMsLibHeaderRecord::OmfMsLibHeaderRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::MsLibHeader, rawPayload, originalChecksum, hadValidChecksum ) {
        // Record length = rawPayload.size() + 1; pageSize = recordLength + 3 = rawPayload.size() + 4
        _pageSize = static_cast<std::uint16_t>( rawPayload.size() + 4u );
        OmfBinaryReader reader( rawPayload, RecordType::MsLibHeader );
        _dictionaryFileOffset = reader.ReadUInt32();
        _dictionaryBlockCount = reader.ReadUInt16();
        if ( !reader.IsAtEnd() ) {
            _flags = reader.ReadUInt8();
        }
        _paddingBytes = reader.ReadRemainingBytes();
    }

    void OmfMsLibHeaderRecord::SetPageSize( std::uint16_t pageSize ) {
        if ( pageSize < 16u || ( pageSize & ( pageSize - 1u ) ) != 0 ) {
            throw OmfException( "MS .LIB page size must be a power of 2 >= 16" );
        }
        _pageSize = pageSize;
        std::size_t const requiredPadding = static_cast<std::size_t>( pageSize ) - 11u;
        _paddingBytes.resize( requiredPadding, std::byte{ 0 } );
        MarkDirty();
    }

    void OmfMsLibHeaderRecord::SetDictionaryFileOffset( std::uint32_t offset ) noexcept {
        if ( _dictionaryFileOffset != offset ) {
            _dictionaryFileOffset = offset;
            MarkDirty();
        }
    }

    void OmfMsLibHeaderRecord::SetDictionaryBlockCount( std::uint16_t count ) noexcept {
        if ( _dictionaryBlockCount != count ) {
            _dictionaryBlockCount = count;
            MarkDirty();
        }
    }

    void OmfMsLibHeaderRecord::SetFlags( std::uint8_t flags ) noexcept {
        if ( _flags != flags ) {
            _flags = flags;
            MarkDirty();
        }
    }

    void OmfMsLibHeaderRecord::SetPaddingBytes( std::vector<std::byte> padding ) {
        _paddingBytes = std::move( padding );
        _pageSize     = static_cast<std::uint16_t>( _paddingBytes.size() + 11u );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfMsLibHeaderRecord::Clone() const {
        return std::make_unique<OmfMsLibHeaderRecord>( *this );
    }

    std::vector<std::byte> OmfMsLibHeaderRecord::SerializePayload() const {
        OmfBinaryWriter writer;
        writer.WriteUInt32( _dictionaryFileOffset );
        writer.WriteUInt16( _dictionaryBlockCount );
        writer.WriteUInt8( _flags );
        writer.WriteBytes( _paddingBytes );
        return writer.TakeBuffer();
    }

    // MsLibEnd (0xF1)
    OmfMsLibEndRecord::OmfMsLibEndRecord( std::vector<std::byte> padding )
        : OmfRecord( RecordType::MsLibEnd ),
          _paddingBytes( std::move( padding ) ) {}

    OmfMsLibEndRecord::OmfMsLibEndRecord(
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum )
        : OmfRecord( RecordType::MsLibEnd, rawPayload, originalChecksum, hadValidChecksum ),
          _paddingBytes( rawPayload.begin(), rawPayload.end() ) {}

    void OmfMsLibEndRecord::SetPaddingBytes( std::vector<std::byte> padding ) {
        _paddingBytes = std::move( padding );
        MarkDirty();
    }

    std::unique_ptr<OmfRecord> OmfMsLibEndRecord::Clone() const {
        return std::make_unique<OmfMsLibEndRecord>( *this );
    }

    std::vector<std::byte> OmfMsLibEndRecord::SerializePayload() const {
        return _paddingBytes;
    }

    //=========================================================================
    // OmfModule Implementation
    //=========================================================================
    void OmfModule::AddRecord( std::unique_ptr<OmfRecord> record ) {
        _records.push_back( std::move( record ) );
    }

    void OmfModule::InsertRecord( std::size_t index, std::unique_ptr<OmfRecord> record ) {
        if ( index > _records.size() ) {
            throw OmfException( "OmfModule::InsertRecord index out of range" );
        }
        _records.insert( _records.begin() + static_cast<std::ptrdiff_t>( index ), std::move( record ) );
    }

    void OmfModule::RemoveRecord( std::size_t index ) {
        if ( index >= _records.size() ) {
            throw OmfException( "OmfModule::RemoveRecord index out of range" );
        }
        _records.erase( _records.begin() + static_cast<std::ptrdiff_t>( index ) );
    }

    std::optional<std::string_view> OmfModule::ResolveName( NameIndex index ) const noexcept {
        if ( !index.IsValid() ) {
            return std::nullopt;
        }
        std::size_t currentOneBased = 1;
        for ( const OmfLnamesRecord* lnames : GetRecordsOfType<OmfLnamesRecord>() ) {
            const auto& names = lnames->GetNames();
            if ( index.value >= currentOneBased &&
                 index.value < currentOneBased + names.size() ) {
                return names[index.value - currentOneBased];
            }
            currentOneBased += names.size();
        }
        return std::nullopt;
    }

    std::size_t OmfModule::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t totalRenamed = 0;

        // 1. Rename direct symbol strings in PUBDEF/LPUBDEF, EXTDEF/LEXTDEF,
        //    COMDEF/LCOMDEF, LOCSYM, DEBSYM, ALIAS, and LIBDIC
        for ( auto& recPtr : _records ) {
            if ( auto* pub = recPtr->As<OmfPubdefRecord>() ) {
                totalRenamed += pub->RenameSymbol( oldName, newName );
            } else if ( auto* ext = recPtr->As<OmfExtdefRecord>() ) {
                totalRenamed += ext->RenameSymbol( oldName, newName );
            } else if ( auto* com = recPtr->As<OmfComdefRecord>() ) {
                totalRenamed += com->RenameSymbol( oldName, newName );
            } else if ( auto* loc = recPtr->As<OmfLocsymRecord>() ) {
                totalRenamed += loc->RenameSymbol( oldName, newName );
            } else if ( auto* deb = recPtr->As<OmfDebsymRecord>() ) {
                totalRenamed += deb->RenameSymbol( oldName, newName );
            } else if ( auto* alias = recPtr->As<OmfAliasRecord>() ) {
                totalRenamed += alias->RenameSymbol( oldName, newName );
            } else if ( auto* dic = recPtr->As<OmfLibdicRecord>() ) {
                totalRenamed += dic->RenameSymbol( oldName, newName );
            }
        }

        // 2. Collect LNAMES indices referenced by COMDAT, CEXTDEF, LINSYM, and NBKPAT
        //    vs. structural LNAMES indices used for Segment, Class, Overlay, and Group names
        std::unordered_set<std::uint16_t> symbolNameIndices;
        std::unordered_set<std::uint16_t> structuralNameIndices;

        for ( const auto& recPtr : _records ) {
            if ( const auto* comdat = recPtr->As<OmfComdatRecord>() ) {
                if ( comdat->GetPublicNameIndex().IsValid() ) {
                    symbolNameIndices.insert( comdat->GetPublicNameIndex().value );
                }
            } else if ( const auto* cext = recPtr->As<OmfCextdefRecord>() ) {
                for ( const auto& ext : cext->GetExternals() ) {
                    if ( ext.logicalNameIndex.IsValid() ) {
                        symbolNameIndices.insert( ext.logicalNameIndex.value );
                    }
                }
            } else if ( const auto* linsym = recPtr->As<OmfLinsymRecord>() ) {
                if ( linsym->GetSymbolNameIndex().IsValid() ) {
                    symbolNameIndices.insert( linsym->GetSymbolNameIndex().value );
                }
            } else if ( const auto* nbkpat = recPtr->As<OmfNbkpatRecord>() ) {
                if ( nbkpat->GetPublicNameIndex().IsValid() ) {
                    symbolNameIndices.insert( nbkpat->GetPublicNameIndex().value );
                }
            } else if ( const auto* seg = recPtr->As<OmfSegdefRecord>() ) {
                if ( seg->GetSegmentNameIndex().IsValid() ) {
                    structuralNameIndices.insert( seg->GetSegmentNameIndex().value );
                }
                if ( seg->GetClassNameIndex().IsValid() ) {
                    structuralNameIndices.insert( seg->GetClassNameIndex().value );
                }
                if ( seg->GetOverlayNameIndex().IsValid() ) {
                    structuralNameIndices.insert( seg->GetOverlayNameIndex().value );
                }
            } else if ( const auto* grp = recPtr->As<OmfGrpdefRecord>() ) {
                if ( grp->GetGroupNameIndex().IsValid() ) {
                    structuralNameIndices.insert( grp->GetGroupNameIndex().value );
                }
            }
        }

        // 3. Update LNAMES/LLNAMES entries referenced by COMDAT symbols without
        //    touching segment/class/group names
        std::uint16_t currentOneBased = 1;
        for ( OmfLnamesRecord* lnames : GetRecordsOfType<OmfLnamesRecord>() ) {
            const auto& names = lnames->GetNames();
            for ( std::size_t i = 0; i < names.size(); ++i, ++currentOneBased ) {
                if ( names[i] == oldName &&
                     symbolNameIndices.contains( currentOneBased ) &&
                     !structuralNameIndices.contains( currentOneBased ) ) {
                    lnames->SetName( i, std::string( newName ) );
                    ++totalRenamed;
                }
            }
        }

        return totalRenamed;
    }

    //=========================================================================
    // OmfFile Implementation
    //=========================================================================
    std::unique_ptr<OmfRecord> OmfFile::_ParseSingleRecord(
        RecordType                 type,
        std::span<const std::byte> rawPayload,
        std::uint8_t               originalChecksum,
        bool                       hadValidChecksum,
        std::size_t                fileOffset ) {
        try {
            switch ( type ) {
                case RecordType::Theadr:
                case RecordType::Lheadr:
                    return std::make_unique<OmfHeaderRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Coment:
                    return std::make_unique<OmfComentRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Modend:
                case RecordType::Modend32:
                    return std::make_unique<OmfModendRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Extdef:
                case RecordType::Lextdef:
                case RecordType::Lextdef32:
                    return std::make_unique<OmfExtdefRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Cextdef:
                    return std::make_unique<OmfCextdefRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Typdef:
                    return std::make_unique<OmfTypdefRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Pubdef:
                case RecordType::Pubdef32:
                case RecordType::Lpubdef:
                case RecordType::Lpubdef32:
                    return std::make_unique<OmfPubdefRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Locsym:
                case RecordType::Locsym32:
                    return std::make_unique<OmfLocsymRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Linnum:
                case RecordType::Linnum32:
                    return std::make_unique<OmfLinnumRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Linsym:
                case RecordType::Linsym32:
                    return std::make_unique<OmfLinsymRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Lnames:
                case RecordType::Llnames:
                    return std::make_unique<OmfLnamesRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Segdef:
                case RecordType::Segdef32:
                    return std::make_unique<OmfSegdefRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Grpdef:
                    return std::make_unique<OmfGrpdefRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Fixupp:
                case RecordType::Fixupp32:
                    return std::make_unique<OmfFixuppRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Ledata:
                case RecordType::Ledata32:
                    return std::make_unique<OmfLedataRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Lidata:
                case RecordType::Lidata32:
                    return std::make_unique<OmfLidataRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Comdef:
                case RecordType::Lcomdef:
                    return std::make_unique<OmfComdefRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Bakpat:
                case RecordType::Bakpat32:
                    return std::make_unique<OmfBakpatRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Nbkpat:
                case RecordType::Nbkpat32:
                    return std::make_unique<OmfNbkpatRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Comdat:
                case RecordType::Comdat32:
                    return std::make_unique<OmfComdatRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Alias:
                    return std::make_unique<OmfAliasRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Vernum:
                    return std::make_unique<OmfVernumRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Vendext:
                    return std::make_unique<OmfVendextRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Rheadr:
                    return std::make_unique<OmfRheadrRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Regint:
                    return std::make_unique<OmfRegintRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Redata:
                    return std::make_unique<OmfRedataRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Ridata:
                    return std::make_unique<OmfRidataRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Ovldef:
                    return std::make_unique<OmfOvldefRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Endrec:
                    return std::make_unique<OmfEndrecRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Blkdef:
                case RecordType::Blkdef32:
                    return std::make_unique<OmfBlkdefRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Blkend:
                case RecordType::Blkend32:
                    return std::make_unique<OmfBlkendRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Debsym:
                    return std::make_unique<OmfDebsymRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Pedata:
                    return std::make_unique<OmfPedataRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Pidata:
                    return std::make_unique<OmfPidataRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Libhed:
                    return std::make_unique<OmfLibhedRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Libnam:
                    return std::make_unique<OmfLibnamRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Libloc:
                    return std::make_unique<OmfLiblocRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::Libdic:
                    return std::make_unique<OmfLibdicRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::MsLibHeader:
                    return std::make_unique<OmfMsLibHeaderRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                case RecordType::MsLibEnd:
                    return std::make_unique<OmfMsLibEndRecord>(
                        rawPayload, originalChecksum, hadValidChecksum );

                default:
                    return std::make_unique<OmfUnknownRecord>(
                        type, rawPayload, originalChecksum, hadValidChecksum );
            }
        } catch ( const OmfParseException& ) {
            throw;
        } catch ( const std::exception& ex ) {
            throw OmfParseException( ex.what(), fileOffset, type );
        }
    }

    OmfFile OmfFile::LoadFromFile(
        const std::filesystem::path& filePath,
        const OmfLoadOptions&        options ) {
        std::ifstream file( filePath, std::ios::binary | std::ios::ate );
        if ( !file ) {
            throw OmfException( "Failed to open OMF file: " + filePath.string() );
        }
        auto const fileSize = static_cast<std::size_t>( file.tellg() );
        file.seekg( 0, std::ios::beg );

        std::vector<std::byte> buffer( fileSize );
        if ( fileSize > 0 &&
             !file.read( reinterpret_cast<char*>( buffer.data() ), static_cast<std::streamsize>( fileSize ) ) ) {
            throw OmfException( "Failed to read OMF file: " + filePath.string() );
        }
        return LoadFromMemory( buffer, options );
    }

    OmfFile OmfFile::LoadFromStream(
        std::istream&         inputStream,
        const OmfLoadOptions& options ) {
        std::ostringstream ss( std::ios::binary );
        ss << inputStream.rdbuf();
        std::string const str = ss.str();
        std::span<const std::byte> const span(
            reinterpret_cast<const std::byte*>( str.data() ),
            str.size() );
        return LoadFromMemory( span, options );
    }

    OmfFile OmfFile::LoadFromMemory(
        std::span<const std::byte> buffer,
        const OmfLoadOptions&      options ) {
        OmfFile result;
        std::size_t offset      = 0;
        std::size_t recordIndex = 0;
        std::size_t libPageSize = 0;
        bool        inModule    = false;

        while ( offset < buffer.size() ) {
            if ( offset + 3u > buffer.size() ) {
                throw OmfParseException(
                    "Truncated 3-byte OMF record header",
                    offset,
                    static_cast<RecordType>( buffer[offset] ) );
            }

            RecordType const type = static_cast<RecordType>( buffer[offset] );
            std::uint16_t const recordLength = static_cast<std::uint16_t>(
                static_cast<std::uint8_t>( buffer[offset + 1] ) |
                ( static_cast<std::uint8_t>( buffer[offset + 2] ) << 8 ) );

            if ( recordLength == 0 || offset + 3u + recordLength > buffer.size() ) {
                throw OmfParseException(
                    "OMF record length extends past end of file",
                    offset,
                    type );
            }

            if ( type == RecordType::MsLibHeader ) {
                libPageSize = static_cast<std::size_t>( recordLength ) + 3u;
            }

            std::size_t const payloadLength = recordLength - 1u;
            std::span<const std::byte> const payloadSpan = buffer.subspan( offset + 3u, payloadLength );
            std::uint8_t const actualChecksum = static_cast<std::uint8_t>(
                buffer[offset + 3u + payloadLength] );

            std::uint8_t sum = 0;
            for ( std::size_t i = 0; i < 3u + payloadLength; ++i ) {
                sum = static_cast<std::uint8_t>(
                    sum + static_cast<std::uint8_t>( buffer[offset + i] ) );
            }
            std::uint8_t const expectedChecksum = static_cast<std::uint8_t>( ( ~sum ) + 1u );

            bool const isMsLibRecord =
                ( type == RecordType::MsLibHeader || type == RecordType::MsLibEnd );
            bool const hadValidChecksum =
                isMsLibRecord || ( actualChecksum == 0u ) || ( actualChecksum == expectedChecksum );

            if ( !hadValidChecksum ) {
                if ( options.checksumValidation == ChecksumValidation::ThrowException ) {
                    throw OmfChecksumException( offset, type, expectedChecksum, actualChecksum );
                } else if ( options.checksumValidation == ChecksumValidation::WarnCallback &&
                            options.onChecksumWarning ) {
                    options.onChecksumWarning( OmfChecksumWarning{
                        offset, recordIndex, type, expectedChecksum, actualChecksum } );
                }
            }

            auto parsedRecord = _ParseSingleRecord(
                type, payloadSpan, actualChecksum, hadValidChecksum, offset );

            if ( type == RecordType::Theadr ||
                 type == RecordType::Lheadr ||
                 type == RecordType::Rheadr ) {
                result._modules.emplace_back();
                inModule = true;
            } else if ( !inModule && !isMsLibRecord &&
                        type != RecordType::Libhed &&
                        type != RecordType::Libnam &&
                        type != RecordType::Libloc &&
                        type != RecordType::Libdic ) {
                result._modules.emplace_back();
                inModule = true;
            }

            offset += 3u + recordLength;
            ++recordIndex;

            if ( inModule ) {
                result._modules.back().AddRecord( parsedRecord->Clone() );
                if ( type == RecordType::Modend || type == RecordType::Modend32 ) {
                    inModule = false;

                    // In an MS-DOS .LIB file, each module is padded to a multiple of libPageSize
                    if ( libPageSize > 0 && ( offset % libPageSize ) != 0 ) {
                        std::size_t const nextBoundary =
                            ( ( offset + libPageSize - 1u ) / libPageSize ) * libPageSize;
                        std::size_t const padEnd = std::min( nextBoundary, buffer.size() );
                        result._modules.back().SetTrailingLibraryPadding(
                            std::vector<std::byte>(
                                buffer.begin() + static_cast<std::ptrdiff_t>( offset ),
                                buffer.begin() + static_cast<std::ptrdiff_t>( padEnd ) ) );
                        offset = padEnd;
                    }
                }
            }

            result._records.push_back( std::move( parsedRecord ) );

            if ( type == RecordType::MsLibEnd && offset < buffer.size() ) {
                result._trailingLibraryDictionary.assign(
                    buffer.begin() + static_cast<std::ptrdiff_t>( offset ),
                    buffer.end() );
                break;
            }
        }

        return result;
    }

    std::optional<std::string_view> OmfModule::GetModuleName() const noexcept {
        for ( const auto& rec : _records ) {
            if ( const auto* hdr = rec->As<OmfHeaderRecord>() ) {
                return hdr->GetModuleName();
            }
            if ( const auto* rhdr = rec->As<OmfRheadrRecord>() ) {
                return rhdr->GetModuleName();
            }
        }
        return std::nullopt;
    }

    std::vector<std::byte> OmfModule::SerializeToMemory( const OmfSaveOptions& options ) const {
        std::vector<std::byte> output;
        for ( const auto& rec : _records ) {
            std::vector<std::byte> const bytes = rec->SerializeRecord( options.checksumPolicy );
            output.insert( output.end(), bytes.begin(), bytes.end() );
        }
        return output;
    }

    void OmfModule::SaveToStream(
        std::ostream&         outputStream,
        const OmfSaveOptions& options ) const {
        std::vector<std::byte> const bytes = SerializeToMemory( options );
        if ( !bytes.empty() ) {
            outputStream.write(
                reinterpret_cast<const char*>( bytes.data() ),
                static_cast<std::streamsize>( bytes.size() ) );
        }
    }

    void OmfModule::SaveToFile(
        const std::filesystem::path& filePath,
        const OmfSaveOptions&        options ) const {
        std::ofstream file( filePath, std::ios::binary | std::ios::trunc );
        if ( !file ) {
            throw OmfException( "Failed to open output .obj file: " + filePath.string() );
        }
        SaveToStream( file, options );
    }

    void OmfModule::SetTrailingLibraryPadding( std::vector<std::byte> padding ) {
        _trailingLibraryPadding = std::move( padding );
    }

    std::vector<std::byte> OmfFile::SerializeToMemory( const OmfSaveOptions& options ) const {
        std::vector<std::byte> output;

        // If the file consists purely of object modules (standard .obj file), serialize
        // from _modules unless the caller mutated records directly via _records.
        bool anyFlatRecordMutated = false;
        for ( const auto& rec : _records ) {
            if ( rec->IsDirty() ) {
                anyFlatRecordMutated = true;
                break;
            }
        }

        bool const hasLibraryWrapperRecords = std::ranges::any_of(
            _records,
            []( const std::unique_ptr<OmfRecord>& rec ) {
                RecordType const t = rec->GetRecordType();
                return t == RecordType::MsLibHeader ||
                    t == RecordType::MsLibEnd    ||
                    t == RecordType::Libhed      ||
                    t == RecordType::Libnam      ||
                    t == RecordType::Libloc      ||
                    t == RecordType::Libdic;
            } );

        if ( !_modules.empty() && !hasLibraryWrapperRecords && !anyFlatRecordMutated ) {
            for ( const OmfModule& module : _modules ) {
                for ( const auto& rec : module.GetRecords() ) {
                    std::vector<std::byte> const bytes = rec->SerializeRecord( options.checksumPolicy );
                    output.insert( output.end(), bytes.begin(), bytes.end() );
                }
            }
        } else {
            std::size_t moduleIdx = 0;
            for ( const auto& rec : _records ) {
                std::vector<std::byte> const bytes = rec->SerializeRecord( options.checksumPolicy );
                output.insert( output.end(), bytes.begin(), bytes.end() );

                RecordType const t = rec->GetRecordType();
                if ( ( t == RecordType::Modend || t == RecordType::Modend32 ) &&
                    moduleIdx < _modules.size() ) {
                    auto const padding = _modules[moduleIdx].GetTrailingLibraryPadding();
                    output.insert( output.end(), padding.begin(), padding.end() );
                    ++moduleIdx;
                }
            }
        }

        if ( !_trailingLibraryDictionary.empty() ) {
            output.insert(
                output.end(),
                _trailingLibraryDictionary.begin(),
                _trailingLibraryDictionary.end() );
        }

        return output;
    }

    void OmfFile::SaveToStream(
        std::ostream&         outputStream,
        const OmfSaveOptions& options ) const {
        std::vector<std::byte> const bytes = SerializeToMemory( options );
        if ( !bytes.empty() ) {
            outputStream.write(
                reinterpret_cast<const char*>( bytes.data() ),
                static_cast<std::streamsize>( bytes.size() ) );
        }
    }

    void OmfFile::SaveToFile(
        const std::filesystem::path& filePath,
        const OmfSaveOptions&        options ) const {
        std::ofstream file( filePath, std::ios::binary | std::ios::trunc );
        if ( !file ) {
            throw OmfException( "Failed to open output OMF file: " + filePath.string() );
        }
        SaveToStream( file, options );
    }

    void OmfFile::SetTrailingLibraryDictionary( std::vector<std::byte> dictionaryBytes ) {
        _trailingLibraryDictionary = std::move( dictionaryBytes );
    }

    std::size_t OmfFile::RenameSymbol( std::string_view oldName, std::string_view newName ) {
        std::size_t moduleTotal = 0;
        for ( OmfModule& module : _modules ) {
            moduleTotal += module.RenameSymbol( oldName, newName );
        }

        // Keep the flat _records list synchronized as well
        OmfModule flatWrapper;
        for ( auto& rec : _records ) {
            flatWrapper.AddRecord( std::move( rec ) );
        }
        std::size_t const flatTotal = flatWrapper.RenameSymbol( oldName, newName );
        _records.clear();
        for ( const auto& rec : flatWrapper.GetRecords() ) {
            _records.push_back( rec->Clone() );
        }

        return std::max( moduleTotal, flatTotal );
    }

} // namespace Omf
