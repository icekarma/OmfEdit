#include "OmfFile.h"

#include <cctype>
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

    struct DumpOptions {
        bool showRawPayload { false };
        bool expandLidata   { false };
        bool summaryOnly    { false };
    };

    // Tracks 1-based OMF tables within the current module so indices in SEGDEF,
    // GRPDEF, PUBDEF, COMDAT, and FIXUPP can be printed with resolved names.
    class ModuleContext {

    public:

        void Reset( ) {
            _lnames   .clear( );
            _segments .clear( );
            _groups   .clear( );
            _externals.clear( );
            for ( auto& t : _targetThreads ) {
                t.reset( );
            }
            for ( auto& f : _frameThreads ) {
                f.reset( );
            }
        }

        std::size_t AddLname( std::string_view name ) {
            _lnames.emplace_back( name );
            return _lnames.size( );
        }

        std::size_t AddSegment( Omf::NameIndex segNameIdx, Omf::NameIndex classNameIdx ) {
            std::string label { ResolveNameRaw(   segNameIdx ) };
            std::string cls   { ResolveNameRaw( classNameIdx ) };
            if ( !cls.empty( ) ) {
                label += " [class " + cls + "]";
            }
            _segments.push_back( std::move( label ) );
            return _segments.size( );
        }

        std::size_t AddGroup( Omf::NameIndex groupNameIdx ) {
            _groups.push_back( ResolveNameRaw( groupNameIdx ) );
            return _groups.size( );
        }

        std::size_t AddExternal( std::string_view name ) {
            _externals.emplace_back( name );
            return _externals.size( );
        }

        std::size_t AddComdatExternal( Omf::NameIndex logicalNameIdx ) {
            _externals.push_back( ResolveNameRaw( logicalNameIdx ) );
            return _externals.size( );
        }

        void SetThread( Omf::OmfFixuppRecord::ThreadSubrecord const& trd ) {
            std::uint8_t const num = trd.threadNumber & 0x03u;
            if ( trd.isFrameThread ) {
                _frameThreads[num] = trd;
            } else {
                _targetThreads[num] = trd;
            }
        }

        std::optional<Omf::OmfFixuppRecord::ThreadSubrecord> GetTargetThread( std::uint8_t num ) const {
            return _targetThreads[num & 0x03u];
        }

        std::optional<Omf::OmfFixuppRecord::ThreadSubrecord> GetFrameThread( std::uint8_t num ) const {
            return _frameThreads[num & 0x03u];
        }

        std::string ResolveNameRaw( Omf::NameIndex idx ) const {
            if ( !idx.IsValid( ) ) {
                return "";
            }
            if ( idx.value <= _lnames.size( ) ) {
                return _lnames[idx.value - 1u];
            }
            return "<out-of-range>";
        }

        std::string FormatNameIndex( Omf::NameIndex idx ) const {
            if ( !idx.IsValid( ) ) {
                return "0 (none)";
            }
            std::ostringstream oss;
            oss << "#" << idx.value << " (\"" << ResolveNameRaw( idx ) << "\")";
            return oss.str( );
        }

        std::string FormatSegmentIndex( Omf::SegmentIndex idx ) const {
            if ( !idx.IsValid( ) ) {
                return "0 (none)";
            }
            std::ostringstream oss;
            oss << "#" << idx.value;
            if ( idx.value <= _segments.size( ) ) {
                oss << " (" << _segments[idx.value - 1u] << ")";
            } else {
                oss << " (<unresolved segment>)";
            }
            return oss.str( );
        }

        std::string FormatGroupIndex( Omf::GroupIndex idx ) const {
            if ( !idx.IsValid( ) ) {
                return "0 (none)";
            }
            std::ostringstream oss;
            oss << "#" << idx.value;
            if ( idx.value <= _groups.size( ) ) {
                oss << " (" << _groups[idx.value - 1u] << ")";
            } else {
                oss << " (<unresolved group>)";
            }
            return oss.str( );
        }

        std::string FormatExternalIndex( Omf::ExternalIndex idx ) const {
            if ( !idx.IsValid( ) ) {
                return "0 (none)";
            }
            std::ostringstream oss;
            oss << "#" << idx.value;
            if ( idx.value <= _externals.size( ) ) {
                oss << " (\"" << _externals[idx.value - 1u] << "\")";
            } else {
                oss << " (<unresolved ext>)";
            }
            return oss.str( );
        }

    private:

        std::vector<std::string>                             _lnames;
        std::vector<std::string>                             _segments;
        std::vector<std::string>                             _groups;
        std::vector<std::string>                             _externals;
        std::optional<Omf::OmfFixuppRecord::ThreadSubrecord> _targetThreads[4];
        std::optional<Omf::OmfFixuppRecord::ThreadSubrecord> _frameThreads[4];

    };

    //=========================================================================
    // Formatting Helpers
    //=========================================================================

    std::string Hex8( std::uint8_t val ) {
        std::ostringstream oss;
        oss << "0x" << std::uppercase << std::hex << std::setw( 2 ) << std::setfill( '0' ) << static_cast<unsigned>( val );
        return oss.str( );
    }

    std::string Hex16( std::uint16_t val ) {
        std::ostringstream oss;
        oss << "0x" << std::uppercase << std::hex << std::setw( 4 ) << std::setfill( '0' ) << static_cast<unsigned>( val );
        return oss.str( );
    }

    std::string Hex32( std::uint32_t val ) {
        std::ostringstream oss;
        oss << "0x" << std::uppercase << std::hex << std::setw( 8 ) << std::setfill( '0' ) << val;
        return oss.str( );
    }

    std::string HexOffset( std::uint32_t val, bool is32Bit ) {
        return is32Bit ? Hex32( val ) : Hex16( static_cast<std::uint16_t>( val ) );
    }

    std::string_view GetRecordTypeName( Omf::RecordType type ) noexcept {
        using Omf::RecordType;
        switch ( type ) {
            case RecordType::Rheadr:      return "RHEADR";
            case RecordType::Regint:      return "REGINT";
            case RecordType::Redata:      return "REDATA";
            case RecordType::Ridata:      return "RIDATA";
            case RecordType::Ovldef:      return "OVLDEF";
            case RecordType::Endrec:      return "ENDREC";
            case RecordType::Blkdef:      return "BLKDEF";
            case RecordType::Blkdef32:    return "BLKDEF32";
            case RecordType::Blkend:      return "BLKEND";
            case RecordType::Blkend32:    return "BLKEND32";
            case RecordType::Debsym:      return "DEBSYM";
            case RecordType::Theadr:      return "THEADR";
            case RecordType::Lheadr:      return "LHEADR";
            case RecordType::Pedata:      return "PEDATA";
            case RecordType::Pidata:      return "PIDATA";
            case RecordType::Coment:      return "COMENT";
            case RecordType::Modend:      return "MODEND";
            case RecordType::Modend32:    return "MODEND32";
            case RecordType::Extdef:      return "EXTDEF";
            case RecordType::Typdef:      return "TYPDEF";
            case RecordType::Pubdef:      return "PUBDEF";
            case RecordType::Pubdef32:    return "PUBDEF32";
            case RecordType::Locsym:      return "LOCSYM";
            case RecordType::Locsym32:    return "LOCSYM32";
            case RecordType::Linnum:      return "LINNUM";
            case RecordType::Linnum32:    return "LINNUM32";
            case RecordType::Lnames:      return "LNAMES";
            case RecordType::Segdef:      return "SEGDEF";
            case RecordType::Segdef32:    return "SEGDEF32";
            case RecordType::Grpdef:      return "GRPDEF";
            case RecordType::Fixupp:      return "FIXUPP";
            case RecordType::Fixupp32:    return "FIXUPP32";
            case RecordType::Ledata:      return "LEDATA";
            case RecordType::Ledata32:    return "LEDATA32";
            case RecordType::Lidata:      return "LIDATA";
            case RecordType::Lidata32:    return "LIDATA32";
            case RecordType::Libhed:      return "LIBHED";
            case RecordType::Libnam:      return "LIBNAM";
            case RecordType::Libloc:      return "LIBLOC";
            case RecordType::Libdic:      return "LIBDIC";
            case RecordType::Comdef:      return "COMDEF";
            case RecordType::Bakpat:      return "BAKPAT";
            case RecordType::Bakpat32:    return "BAKPAT32";
            case RecordType::Lextdef:     return "LEXTDEF";
            case RecordType::Lextdef32:   return "LEXTDEF32";
            case RecordType::Lpubdef:     return "LPUBDEF";
            case RecordType::Lpubdef32:   return "LPUBDEF32";
            case RecordType::Lcomdef:     return "LCOMDEF";
            case RecordType::Cextdef:     return "CEXTDEF";
            case RecordType::Comdat:      return "COMDAT";
            case RecordType::Comdat32:    return "COMDAT32";
            case RecordType::Linsym:      return "LINSYM";
            case RecordType::Linsym32:    return "LINSYM32";
            case RecordType::Alias:       return "ALIAS";
            case RecordType::Nbkpat:      return "NBKPAT";
            case RecordType::Nbkpat32:    return "NBKPAT32";
            case RecordType::Llnames:     return "LLNAMES";
            case RecordType::Vernum:      return "VERNUM";
            case RecordType::Vendext:     return "VENDEXT";
            case RecordType::MsLibHeader: return "MSLIBHDR";
            case RecordType::MsLibEnd:    return "MSLIBEND";
            default:                      return "UNKNOWN";
        }
    }

    std::string_view GetComentClassDescription( std::uint8_t cls ) noexcept {
        switch ( cls ) {
            case 0x00: return "Translator / Compiler Identification";
            case 0x01: return "Intel Copyright Notice";
            case 0x81: return "Library Specifier ( Deprecated )";
            case 0x9C: return "MS-DOS Version ( Deprecated )";
            case 0x9D: return "Memory Model & Processor Options";
            case 0x9E: return "DOSSEG Segment Ordering Directive";
            case 0x9F: return "Default Library Search Name";
            case 0xA0: return "OMF Extension ( IMPDEF / EXPDEF / INCDEF / LNKDIR )";
            case 0xA1: return "Symbolic Debug Information ( New OMF / CodeView )";
            case 0xA2: return "Linker Pass 2 Marker";
            case 0xA3: return "Library Module Name ( LIBMOD )";
            case 0xA4: return "Executable String ( EXESTR )";
            case 0xA6: return "Incremental Compilation Error ( INCERR )";
            case 0xA7: return "No Segment Padding ( NOPAD )";
            case 0xA8: return "Weak External ( WKEXT )";
            case 0xA9: return "Lazy External ( LZEXT )";
            case 0xAA: return "Phar Lap Comment";
            case 0xDA: return "Comment / Pragmas";
            case 0xDB: return "Compiler Timestamp / Options";
            case 0xDC: return "User / Date Stamp";
            case 0xDD: return "Timestamp / Dependencies";
            case 0xDF: return "QuickC / CodeView Options";
            case 0xE9: return "Borland Dependency File";
            default:   return "Vendor / Custom Comment Class";
        }
    }

    std::string_view GetAlignmentName( Omf::OmfSegdefRecord::Alignment align ) noexcept {
        using Alignment = Omf::OmfSegdefRecord::Alignment;
        switch ( align ) {
            case Alignment::Absolute:             return "Absolute (A=0)";
            case Alignment::ByteRelocatable:      return "Byte / 1-byte (A=1)";
            case Alignment::WordRelocatable:      return "Word / 2-byte (A=2)";
            case Alignment::ParagraphRelocatable: return "Paragraph / 16-byte (A=3)";
            case Alignment::PageRelocatable:      return "Page / 256-byte (A=4)";
            case Alignment::DwordRelocatable:     return "Dword / 4-byte (A=5)";
            case Alignment::LoadTimeParagraph:    return "Load-Time Paragraph / LTL (A=6)";
            default:                              return "Unknown Alignment";
        }
    }

    std::string_view GetCombinationName( Omf::OmfSegdefRecord::Combination comb ) noexcept {
        using Combination = Omf::OmfSegdefRecord::Combination;
        switch ( comb ) {
            case Combination::Private: return "Private (C=0)";
            case Combination::Public:  return "Public (C=2)";
            case Combination::Public4: return "Public (C=4)";
            case Combination::Stack:   return "Stack (C=5)";
            case Combination::Common:  return "Common (C=6)";
            case Combination::Public7: return "Public (C=7)";
            default:                   return "Reserved/Unknown";
        }
    }

    std::string_view GetFixupLocationName( Omf::FixupLocationType loc ) noexcept {
        using Omf::FixupLocationType;
        switch ( loc ) {
            case FixupLocationType::LowByte:          return "LowByte (8-bit lobyte, loc=0)";
            case FixupLocationType::Offset16:         return "Offset16 (16-bit offset, loc=1)";
            case FixupLocationType::SegmentBase:      return "SegmentBase (16-bit base, loc=2)";
            case FixupLocationType::FarPointer16_16:  return "FarPtr16:16 (32-bit far ptr, loc=3)";
            case FixupLocationType::HighByte:         return "HighByte (8-bit hibyte, loc=4)";
            case FixupLocationType::LoaderResolved16: return "LoaderResolved16 (16-bit offset, loc=5)";
            case FixupLocationType::Offset32:         return "Offset32 (32-bit offset, loc=9)";
            case FixupLocationType::FarPointer16_32:  return "FarPtr16:32 (48-bit far ptr, loc=11)";
            case FixupLocationType::LoaderResolved32: return "LoaderResolved32 (32-bit offset, loc=13)";
            default:                                  return "Unknown Location Type";
        }
    }

    std::string FormatFrameMethod( std::uint8_t method, std::uint16_t datum, ModuleContext const& ctx ) {
        switch ( method & 0x07u ) {
            case 0: return "F0: Segment " + ctx.FormatSegmentIndex( Omf::SegmentIndex{ datum } );
            case 1: return "F1: Group "   + ctx.FormatGroupIndex( Omf::GroupIndex{ datum } );
            case 2: return "F2: External " + ctx.FormatExternalIndex( Omf::ExternalIndex{ datum } );
            case 3: return "F3: Explicit Frame " + Hex16( datum );
            case 4: return "F4: Location Segment (inferred from data record)";
            case 5: return "F5: Target's Frame (determined by target)";
            default: return "F" + std::to_string( method ) + ": Reserved";
        }
    }

    std::string FormatTargetMethod( std::uint8_t method, std::uint16_t datum, ModuleContext const& ctx ) {
        switch ( method & 0x03u ) {
            case 0: return "T" + std::to_string( method ) + ": Segment "  + ctx.FormatSegmentIndex( Omf::SegmentIndex{ datum } );
            case 1: return "T" + std::to_string( method ) + ": Group "    + ctx.FormatGroupIndex( Omf::GroupIndex{ datum } );
            case 2: return "T" + std::to_string( method ) + ": External " + ctx.FormatExternalIndex( Omf::ExternalIndex{ datum } );
            case 3: return "T" + std::to_string( method ) + ": Explicit Frame " + Hex16( datum );
            default: return "Unknown Target";
        }
    }

    void PrintFixupTargetSpec(
        Omf::FixupTargetSpec const& spec,
        bool                        is32Bit,
        ModuleContext const&        ctx,
        std::string_view            indent
    ) {
        std::cout << indent << "Frame  : ";
        if ( spec.frameUsesThread ) {
            std::uint8_t const tNum = spec.frameMethodOrThread & 0x03u;
            std::cout << "Thread #" << static_cast<unsigned>( tNum );
            if ( auto const trd = ctx.GetFrameThread( tNum ) ) {
                std::cout << " -> [" << FormatFrameMethod( trd->method, trd->index, ctx ) << "]";
            } else {
                std::cout << " -> [<unset frame thread>]";
            }
            std::cout << "\n";
        } else {
            std::cout << FormatFrameMethod( spec.frameMethodOrThread, spec.frameDatum.value_or( 0 ), ctx ) << "\n";
        }

        std::cout << indent << "Target : ";
        if ( spec.targetUsesThread ) {
            std::uint8_t const tNum = spec.targetMethodOrThread & 0x03u;
            std::cout << "Thread #" << static_cast<unsigned>( tNum );
            if ( auto const trd = ctx.GetTargetThread( tNum ) ) {
                std::cout << " -> [" << FormatTargetMethod( trd->method, trd->index, ctx ) << "]";
            } else {
                std::cout << " -> [<unset target thread>]";
            }
        } else {
            std::cout << FormatTargetMethod( spec.targetMethodOrThread, spec.targetDatum.value_or( 0 ), ctx );
        }

        if ( spec.targetDisplacement.has_value( ) ) {
            std::cout << " + Disp " << HexOffset( *spec.targetDisplacement, is32Bit );
        } else {
            std::cout << " (No Displacement, P=1)";
        }
        std::cout << "\n";
    }

    void PrintHexDump(
        std::span<const std::byte> data,
        std::uint32_t              startOffset = 0,
        std::string_view           indent      = "      "
    ) {
        if ( data.empty( ) ) {
            std::cout << indent << "<empty data>\n";
            return;
        }

        std::ios::fmtflags const oldFlags { std::cout.flags( ) };
        char               const oldFill  { std::cout.fill( )  };

        for ( std::size_t i = 0; i < data.size( ); i += 16u ) {
            std::size_t const lineCount = std::min<std::size_t>( 16u, data.size( ) - i );
            std::cout << indent << Hex32( startOffset + static_cast<std::uint32_t>( i ) ) << " : ";

            for ( std::size_t j = 0; j < 16u; ++j ) {
                if ( j == 8u ) {
                    std::cout << " ";
                }
                if ( j < lineCount ) {
                    std::cout << std::uppercase << std::hex << std::setw( 2 ) << std::setfill( '0' )
                              << static_cast<unsigned>( std::to_integer<std::uint8_t>( data[i + j] ) )
                              << std::dec << std::setfill( ' ' ) << " ";
                } else {
                    std::cout << "   ";
                }
            }

            std::cout << " |";
            for ( std::size_t j = 0; j < lineCount; ++j ) {
                unsigned char const ch = std::to_integer<unsigned char>( data[i + j] );
                std::cout << ( ( ch >= 0x20u && ch <= 0x7Eu ) ? static_cast<char>( ch ) : '.' );
            }
            std::cout << "|\n";
        }

        std::cout.flags( oldFlags );
        std::cout.fill( oldFill );
    }

    void PrintIteratedDataBlocks(
        std::vector<Omf::OmfLidataRecord::IteratedDataBlock> const& blocks,
        std::string const&                                          indent = "      "
    ) {
        for ( std::size_t i = 0; i < blocks.size( ); ++i ) {
            auto const& block = blocks[i];
            if ( std::holds_alternative<std::vector<std::byte>>( block.content ) ) {
                auto const& bytes = std::get<std::vector<std::byte>>( block.content );
                std::cout << indent << "Block [" << i << "]: Repeat=" << block.repeatCount
                          << ", Leaf Bytes (" << bytes.size( ) << " bytes):\n";
                PrintHexDump( bytes, 0, indent + "  " );
            } else {
                auto const& nested = std::get<std::vector<Omf::OmfLidataRecord::IteratedDataBlock>>( block.content );
                std::cout << indent << "Block [" << i << "]: Repeat=" << block.repeatCount
                          << ", Nested Blocks (" << nested.size( ) << "):\n";
                PrintIteratedDataBlocks( nested, indent + "  " );
            }
        }
    }

    //=========================================================================
    // Per-Record Verbose Printer
    //=========================================================================

    void DumpRecordDetails(
        Omf::OmfRecord const& rec,
        ModuleContext&        ctx,
        DumpOptions const&    opts
    ) {
        bool const is32 = rec.Is32Bit( );

        if ( auto const* hdr = rec.As<Omf::OmfHeaderRecord>( ) ) {
            ctx.Reset( );
            std::cout << "    Header Kind : "
                      << ( hdr->IsLibraryHeader( ) ? "LHEADR (Library Module)" : "THEADR (Translator Module)" ) << "\n"
                      << "    Module Name : \"" << hdr->GetModuleName( ) << "\"\n";

        } else if ( auto const* rhdr = rec.As<Omf::OmfRheadrRecord>( ) ) {
            ctx.Reset( );
            std::cout << "    Module Name : \"" << rhdr->GetModuleName( ) << "\"\n"
                      << "    Module Type : " << Hex8( rhdr->GetModuleType( ) ) << "\n";
            if ( !rhdr->GetTrailingAttributes( ).empty( ) ) {
                std::cout << "    Attributes  : " << rhdr->GetTrailingAttributes( ).size( ) << " byte(s)\n";
                PrintHexDump( rhdr->GetTrailingAttributes( ), 0, "      " );
            }

        } else if ( auto const* coment = rec.As<Omf::OmfComentRecord>( ) ) {
            std::uint8_t const cls  = coment->GetCommentClass( );
            auto const         data = coment->GetCommentData( );

            std::cout << "    Flags       : NoPurge=" << ( coment->IsNoPurge( ) ? "true" : "false" )
                      << ", NoList=" << ( coment->IsNoList( ) ? "true" : "false" ) << "\n"
                      << "    Class       : " << Hex8( cls ) << " (" << GetComentClassDescription( cls ) << ")\n";

            // Check if the comment data is purely printable ASCII text
            bool isPrintable = !data.empty( );
            for ( std::byte b : data ) {
                unsigned char const c = std::to_integer<unsigned char>( b );
                if ( ( c < 0x20u || c > 0x7Eu ) && c != '\t' && c != '\r' && c != '\n' ) {
                    isPrintable = false;
                    break;
                }
            }

            if ( isPrintable ) {
                std::string_view const text(
                    reinterpret_cast<char const*>( data.data( ) ),
                    data.size( )
                );
                std::cout << "    Text        : \"" << text << "\"\n";
            }
            if ( !data.empty( ) ) {
                std::cout << "    Data Bytes  : " << data.size( ) << " byte(s)\n";
                PrintHexDump( data, 0, "      " );
            }

        } else if ( auto const* lnames = rec.As<Omf::OmfLnamesRecord>( ) ) {
            auto const& names = lnames->GetNames( );
            std::cout << "    Scope       : " << ( lnames->IsLocal( ) ? "LLNAMES (Local)" : "LNAMES (Global)" ) << "\n"
                      << "    Name Count  : " << names.size( ) << "\n";
            for ( auto const& name : names ) {
                std::size_t const oneBasedIdx = ctx.AddLname( name );
                std::cout << "      Name #" << std::setw( 3 ) << oneBasedIdx << " : \"" << name << "\"\n";
            }

        } else if ( auto const* seg = rec.As<Omf::OmfSegdefRecord>( ) ) {
            std::size_t const segNumber = ctx.AddSegment( seg->GetSegmentNameIndex( ), seg->GetClassNameIndex( ) );
            std::cout << "    Segment #   : " << segNumber << "\n"
                      << "    Segment Name: " << ctx.FormatNameIndex( seg->GetSegmentNameIndex( ) ) << "\n"
                      << "    Class Name  : " << ctx.FormatNameIndex( seg->GetClassNameIndex( ) ) << "\n"
                      << "    Overlay Name: " << ctx.FormatNameIndex( seg->GetOverlayNameIndex( ) ) << "\n"
                      << "    Alignment   : " << GetAlignmentName( seg->GetAlignment( ) ) << "\n"
                      << "    Combination : " << GetCombinationName( seg->GetCombination( ) ) << "\n"
                      << "    Attributes  : Big/MaxLength=" << ( seg->IsMaxLength( ) ? "true (64K/4G)" : "false" )
                      << ", Use32=" << ( seg->IsUse32( ) ? "true (32-bit)" : "false (16-bit)" ) << "\n"
                      << "    Seg Length  : " << HexOffset( seg->GetSegmentLength( ), is32 )
                      << " (" << seg->GetSegmentLength( ) << " bytes)\n";
            if ( seg->GetAlignment( ) == Omf::OmfSegdefRecord::Alignment::Absolute ) {
                std::cout << "    Abs Address : Frame=" << Hex16( seg->GetAbsoluteFrame( ).value_or( 0 ) )
                          << ", Offset=" << Hex8( seg->GetAbsoluteOffset( ).value_or( 0 ) ) << "\n";
            }

        } else if ( auto const* grp = rec.As<Omf::OmfGrpdefRecord>( ) ) {
            std::size_t const grpNumber = ctx.AddGroup( grp->GetGroupNameIndex( ) );
            auto const&       comps     = grp->GetComponents( );
            std::cout << "    Group #     : " << grpNumber << "\n"
                      << "    Group Name  : " << ctx.FormatNameIndex( grp->GetGroupNameIndex( ) ) << "\n"
                      << "    Components  : " << comps.size( ) << "\n";
            for ( std::size_t i = 0; i < comps.size( ); ++i ) {
                std::cout << "      [" << i << "] Descriptor=" << Hex8( comps[i].descriptor )
                          << ", Segment=" << ctx.FormatSegmentIndex( comps[i].segmentIndex ) << "\n";
            }

        } else if ( auto const* pub = rec.As<Omf::OmfPubdefRecord>( ) ) {
            auto const& syms = pub->GetSymbols( );
            std::cout << "    Visibility  : " << ( pub->IsLocal( ) ? "Local (LPUBDEF)" : "Public (PUBDEF)" ) << "\n"
                      << "    Base Group  : " << ctx.FormatGroupIndex( pub->GetBaseGroup( ) ) << "\n"
                      << "    Base Segment: " << ctx.FormatSegmentIndex( pub->GetBaseSegment( ) ) << "\n";
            if ( pub->GetBaseFrame( ).has_value( ) ) {
                std::cout << "    Base Frame  : " << Hex16( *pub->GetBaseFrame( ) ) << "\n";
            }
            std::cout << "    Symbol Count: " << syms.size( ) << "\n";
            for ( std::size_t i = 0; i < syms.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] Offset="
                          << HexOffset( syms[i].offset, is32 )
                          << ", Type=#" << syms[i].typeIndex.value
                          << ", Name=\"" << syms[i].name << "\"\n";
            }

        } else if ( auto const* ext = rec.As<Omf::OmfExtdefRecord>( ) ) {
            auto const& syms = ext->GetSymbols( );
            std::cout << "    Visibility  : " << ( ext->IsLocal( ) ? "Local (LEXTDEF)" : "Global (EXTDEF)" ) << "\n"
                      << "    Symbol Count: " << syms.size( ) << "\n";
            for ( auto const& sym : syms ) {
                std::size_t const extIdx = ctx.AddExternal( sym.name );
                std::cout << "      Ext #" << std::setw( 3 ) << extIdx
                          << " : Type=#" << sym.typeIndex.value
                          << ", Name=\"" << sym.name << "\"\n";
            }

        } else if ( auto const* cext = rec.As<Omf::OmfCextdefRecord>( ) ) {
            auto const& exts = cext->GetExternals( );
            std::cout << "    External Cnt: " << exts.size( ) << "\n";
            for ( auto const& e : exts ) {
                std::size_t const extIdx = ctx.AddComdatExternal( e.logicalNameIndex );
                std::cout << "      Ext #" << std::setw( 3 ) << extIdx
                          << " : LogicalName=" << ctx.FormatNameIndex( e.logicalNameIndex )
                          << ", Type=#" << e.typeIndex.value << "\n";
            }

        } else if ( auto const* typ = rec.As<Omf::OmfTypdefRecord>( ) ) {
            std::cout << "    Type Name   : \"" << typ->GetTypeName( ) << "\"\n"
                      << "    Descriptor  : " << typ->GetRawTypeDescriptor( ).size( ) << " byte(s)\n";
            PrintHexDump( typ->GetRawTypeDescriptor( ), 0, "      " );

        } else if ( auto const* loc = rec.As<Omf::OmfLocsymRecord>( ) ) {
            auto const& syms = loc->GetSymbols( );
            std::cout << "    Base Group  : " << ctx.FormatGroupIndex( loc->GetBaseGroup( ) ) << "\n"
                      << "    Base Segment: " << ctx.FormatSegmentIndex( loc->GetBaseSegment( ) ) << "\n";
            if ( loc->GetBaseFrame( ).has_value( ) ) {
                std::cout << "    Base Frame  : " << Hex16( *loc->GetBaseFrame( ) ) << "\n";
            }
            std::cout << "    Symbol Count: " << syms.size( ) << "\n";
            for ( std::size_t i = 0; i < syms.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] Offset="
                          << HexOffset( syms[i].offset, is32 )
                          << ", Type=#" << syms[i].typeIndex.value
                          << ", Name=\"" << syms[i].name << "\"\n";
            }

        } else if ( auto const* lin = rec.As<Omf::OmfLinnumRecord>( ) ) {
            auto const& entries = lin->GetEntries( );
            std::cout << "    Base Group  : " << ctx.FormatGroupIndex( lin->GetBaseGroup( ) ) << "\n"
                      << "    Base Segment: " << ctx.FormatSegmentIndex( lin->GetBaseSegment( ) ) << "\n"
                      << "    Line Entries: " << entries.size( ) << "\n";
            for ( std::size_t i = 0; i < entries.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] Line "
                          << std::setw( 5 ) << entries[i].lineNumber
                          << " -> Offset " << HexOffset( entries[i].offset, is32 ) << "\n";
            }

        } else if ( auto const* linsym = rec.As<Omf::OmfLinsymRecord>( ) ) {
            auto const& entries = linsym->GetEntries( );
            std::cout << "    Flags       : " << Hex8( linsym->GetFlags( ) )
                      << ( linsym->IsContinuation( ) ? " (Continuation)" : " (First/Primary)" ) << "\n"
                      << "    Symbol Name : " << ctx.FormatNameIndex( linsym->GetSymbolNameIndex( ) ) << "\n"
                      << "    Line Entries: " << entries.size( ) << "\n";
            for ( std::size_t i = 0; i < entries.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] Line "
                          << std::setw( 5 ) << entries[i].lineNumber
                          << " -> Offset " << HexOffset( entries[i].offset, is32 ) << "\n";
            }

        } else if ( auto const* ledata = rec.As<Omf::OmfLedataRecord>( ) ) {
            auto const data = ledata->GetData( );
            std::cout << "    Segment     : " << ctx.FormatSegmentIndex( ledata->GetSegmentIndex( ) ) << "\n"
                      << "    Data Offset : " << HexOffset( ledata->GetDataOffset( ), is32 ) << "\n"
                      << "    Data Length : " << data.size( ) << " byte(s)\n";
            PrintHexDump( data, ledata->GetDataOffset( ), "      " );

        } else if ( auto const* lidata = rec.As<Omf::OmfLidataRecord>( ) ) {
            std::cout << "    Segment     : " << ctx.FormatSegmentIndex( lidata->GetSegmentIndex( ) ) << "\n"
                      << "    Data Offset : " << HexOffset( lidata->GetDataOffset( ), is32 ) << "\n"
                      << "    Top Blocks  : " << lidata->GetBlocks( ).size( ) << "\n";
            PrintIteratedDataBlocks( lidata->GetBlocks( ), "      " );
            if ( opts.expandLidata ) {
                auto const expanded = lidata->ExpandData( );
                std::cout << "    Expanded    : " << expanded.size( ) << " byte(s)\n";
                PrintHexDump( expanded, lidata->GetDataOffset( ), "      " );
            }

        } else if ( auto const* fixupp = rec.As<Omf::OmfFixuppRecord>( ) ) {
            auto const& subs = fixupp->GetSubrecords( );
            std::cout << "    Subrecords  : " << subs.size( ) << "\n";
            for ( std::size_t i = 0; i < subs.size( ); ++i ) {
                if ( std::holds_alternative<Omf::OmfFixuppRecord::ThreadSubrecord>( subs[i] ) ) {
                    auto const& trd = std::get<Omf::OmfFixuppRecord::ThreadSubrecord>( subs[i] );
                    ctx.SetThread( trd );
                    std::cout << "      [" << std::setw( 3 ) << i << "] THREAD ("
                              << ( trd.isFrameThread ? "Frame" : "Target" )
                              << " Thread #" << static_cast<unsigned>( trd.threadNumber ) << "): ";
                    if ( trd.isFrameThread ) {
                        std::cout << FormatFrameMethod( trd.method, trd.index, ctx ) << "\n";
                    } else {
                        std::cout << FormatTargetMethod( trd.method, trd.index, ctx ) << "\n";
                    }
                } else {
                    auto const& fix = std::get<Omf::OmfFixuppRecord::FixupSubrecord>( subs[i] );
                    std::cout << "      [" << std::setw( 3 ) << i << "] FIXUP at DataRecordOffset="
                              << Hex16( fix.dataRecordOffset ) << " (" << fix.dataRecordOffset << "):\n"
                              << "            Mode   : "
                              << ( fix.isSegmentRelative ? "Segment-Relative (M=1)" : "Self-Relative (M=0)" ) << "\n"
                              << "            Loc    : " << GetFixupLocationName( fix.location ) << "\n";
                    PrintFixupTargetSpec( fix.target, is32, ctx, "            " );
                }
            }

        } else if ( auto const* comdef = rec.As<Omf::OmfComdefRecord>( ) ) {
            auto const& syms = comdef->GetSymbols( );
            std::cout << "    Visibility  : " << ( comdef->IsLocal( ) ? "Local (LCOMDEF)" : "Global (COMDEF)" ) << "\n"
                      << "    Symbol Count: " << syms.size( ) << "\n";
            for ( auto const& sym : syms ) {
                std::size_t const extIdx = ctx.AddExternal( sym.name );
                bool const isFar = ( sym.dataSegmentType == static_cast<std::uint8_t>( Omf::OmfComdefRecord::DataSegmentType::FarComm ) );
                bool const isNear = ( sym.dataSegmentType == static_cast<std::uint8_t>( Omf::OmfComdefRecord::DataSegmentType::NearComm ) );
                std::cout << "      Ext #" << std::setw( 3 ) << extIdx
                          << " : Name=\"" << sym.name << "\", Type=#" << sym.typeIndex.value
                          << ", SegType=" << Hex8( sym.dataSegmentType )
                          << ( isFar ? " (FAR_COMM)" : ( isNear ? " (NEAR_COMM)" : " (SegIdx)" ) );
                if ( isFar ) {
                    std::cout << ", Elements=" << sym.elementCount
                              << ", ElemSize=" << sym.elementSize
                              << " (Total=" << ( sym.elementCount * sym.elementSize ) << " bytes)\n";
                } else {
                    std::cout << ", Size=" << sym.elementCount << " byte(s)\n";
                }
            }

        } else if ( auto const* comdat = rec.As<Omf::OmfComdatRecord>( ) ) {
            std::cout << "    Symbol Name : " << ctx.FormatNameIndex( comdat->GetPublicNameIndex( ) ) << "\n"
                      << "    Flags       : " << Hex8( comdat->GetFlags( ) )
                      << " [Continuation=" << ( comdat->IsContinuation( ) ? "yes" : "no" )
                      << ", Iterated=" << ( comdat->IsIteratedData( ) ? "yes" : "no" )
                      << ", Local=" << ( comdat->IsLocalSymbol( ) ? "yes" : "no" )
                      << ", CodeSeg=" << ( comdat->IsCodeSegmentData( ) ? "yes" : "no" ) << "]\n"
                      << "    Selection   : " << Hex8( static_cast<std::uint8_t>( comdat->GetSelectionCriteria( ) ) ) << "\n"
                      << "    Allocation  : " << Hex8( static_cast<std::uint8_t>( comdat->GetAllocationType( ) ) ) << "\n"
                      << "    Alignment   : " << GetAlignmentName( comdat->GetAlignment( ) ) << "\n"
                      << "    Data Offset : " << HexOffset( comdat->GetDataOffset( ), is32 ) << "\n"
                      << "    Type Index  : #" << comdat->GetTypeIndex( ).value << "\n";

            if ( comdat->GetAllocationType( ) == Omf::OmfComdatRecord::AllocationType::Explicit ) {
                std::cout << "    Base Group  : " << ctx.FormatGroupIndex( comdat->GetPublicBaseGroup( ) ) << "\n"
                          << "    Base Segment: " << ctx.FormatSegmentIndex( comdat->GetPublicBaseSegment( ) ) << "\n";
                if ( comdat->GetPublicBaseFrame( ).has_value( ) ) {
                    std::cout << "    Base Frame  : " << Hex16( *comdat->GetPublicBaseFrame( ) ) << "\n";
                }
            }

            auto const& payload = comdat->GetDataPayload( );
            if ( std::holds_alternative<std::vector<std::byte>>( payload ) ) {
                auto const& bytes = std::get<std::vector<std::byte>>( payload );
                std::cout << "    Data Bytes  : " << bytes.size( ) << " byte(s)\n";
                PrintHexDump( bytes, comdat->GetDataOffset( ), "      " );
            } else {
                auto const& blocks = std::get<std::vector<Omf::OmfLidataRecord::IteratedDataBlock>>( payload );
                std::cout << "    Iterated Blk: " << blocks.size( ) << "\n";
                PrintIteratedDataBlocks( blocks, "      " );
            }

        } else if ( auto const* bakpat = rec.As<Omf::OmfBakpatRecord>( ) ) {
            auto const& entries = bakpat->GetEntries( );
            std::cout << "    Segment     : " << ctx.FormatSegmentIndex( bakpat->GetSegmentIndex( ) ) << "\n"
                      << "    Location    : " << GetFixupLocationName( bakpat->GetLocationType( ) ) << "\n"
                      << "    Patch Count : " << entries.size( ) << "\n";
            for ( std::size_t i = 0; i < entries.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] Offset="
                          << HexOffset( entries[i].offset, is32 )
                          << " <- Value=" << HexOffset( entries[i].value, is32 ) << "\n";
            }

        } else if ( auto const* nbkpat = rec.As<Omf::OmfNbkpatRecord>( ) ) {
            auto const& entries = nbkpat->GetEntries( );
            std::cout << "    COMDAT Name : " << ctx.FormatNameIndex( nbkpat->GetPublicNameIndex( ) ) << "\n"
                      << "    Location    : " << GetFixupLocationName( nbkpat->GetLocationType( ) ) << "\n"
                      << "    Patch Count : " << entries.size( ) << "\n";
            for ( std::size_t i = 0; i < entries.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] Offset="
                          << HexOffset( entries[i].offset, is32 )
                          << " <- Value=" << HexOffset( entries[i].value, is32 ) << "\n";
            }

        } else if ( auto const* alias = rec.As<Omf::OmfAliasRecord>( ) ) {
            auto const& aliases = alias->GetAliases( );
            std::cout << "    Alias Count : " << aliases.size( ) << "\n";
            for ( std::size_t i = 0; i < aliases.size( ); ++i ) {
                std::cout << "      [" << i << "] \"" << aliases[i].aliasName
                          << "\" -> \"" << aliases[i].substituteName << "\"\n";
            }

        } else if ( auto const* vernum = rec.As<Omf::OmfVernumRecord>( ) ) {
            std::cout << "    OMF Version : \"" << vernum->GetVersion( ) << "\"\n";

        } else if ( auto const* vendext = rec.As<Omf::OmfVendextRecord>( ) ) {
            std::cout << "    Subtype     : " << Hex16( vendext->GetSubtype( ) ) << "\n"
                      << "    Ext Data    : " << vendext->GetExtensionData( ).size( ) << " byte(s)\n";
            PrintHexDump( vendext->GetExtensionData( ), 0, "      " );

        } else if ( auto const* modend = rec.As<Omf::OmfModendRecord>( ) ) {
            std::cout << "    Main Module : " << ( modend->IsMainModule( ) ? "true" : "false" ) << "\n"
                      << "    Has Start   : " << ( modend->HasStartAddress( ) ? "true" : "false" ) << "\n";
            if ( modend->HasStartAddress( ) ) {
                auto const& start = modend->GetStartAddress( );
                if ( std::holds_alternative<Omf::FixupTargetSpec>( start ) ) {
                    std::cout << "    Start Kind  : Relocatable Entry Point\n";
                    PrintFixupTargetSpec( std::get<Omf::FixupTargetSpec>( start ), is32, ctx, "      " );
                } else if ( std::holds_alternative<Omf::OmfModendRecord::AbsoluteStartAddress>( start ) ) {
                    auto const& absAddr = std::get<Omf::OmfModendRecord::AbsoluteStartAddress>( start );
                    std::cout << "    Start Kind  : Physical / Absolute Entry Point\n"
                              << "      Frame     : " << Hex16( absAddr.frameNumber ) << "\n"
                              << "      Offset    : " << HexOffset( absAddr.offset, is32 ) << "\n";
                }
            }

        } else if ( auto const* regint = rec.As<Omf::OmfRegintRecord>( ) ) {
            auto const& entries = regint->GetEntries( );
            std::cout << "    Reg Entries : " << entries.size( ) << "\n";
            for ( std::size_t i = 0; i < entries.size( ); ++i ) {
                char const* regName = "Unknown";
                switch ( entries[i].registerId ) {
                    case Omf::OmfRegintRecord::RegisterId::CsAndIp: regName = "CS:IP"; break;
                    case Omf::OmfRegintRecord::RegisterId::SsAndSp: regName = "SS:SP"; break;
                    case Omf::OmfRegintRecord::RegisterId::Ds:      regName = "DS";    break;
                    case Omf::OmfRegintRecord::RegisterId::Es:      regName = "ES";    break;
                }
                std::cout << "      [" << i << "] Register=" << regName
                          << ", Relocatable=" << ( entries[i].isRelocatable ? "true" : "false" ) << "\n";
                if ( entries[i].isRelocatable ) {
                    PrintFixupTargetSpec( entries[i].relocTarget, false, ctx, "            " );
                } else {
                    std::cout << "            Frame=" << Hex16( entries[i].explicitFrame );
                    if ( entries[i].explicitOffset.has_value( ) ) {
                        std::cout << ", Offset=" << Hex16( *entries[i].explicitOffset );
                    }
                    std::cout << "\n";
                }
            }

        } else if ( auto const* redata = rec.As<Omf::OmfRedataRecord>( ) ) {
            std::cout << "    Base Spec   :\n";
            PrintFixupTargetSpec( redata->GetBaseSpec( ), false, ctx, "      " );
            std::cout << "    Data Bytes  : " << redata->GetData( ).size( ) << " byte(s)\n";
            PrintHexDump( redata->GetData( ), 0, "      " );

        } else if ( auto const* ridata = rec.As<Omf::OmfRidataRecord>( ) ) {
            std::cout << "    Base Spec   :\n";
            PrintFixupTargetSpec( ridata->GetBaseSpec( ), false, ctx, "      " );
            PrintIteratedDataBlocks( ridata->GetBlocks( ), "      " );

        } else if ( auto const* ovldef = rec.As<Omf::OmfOvldefRecord>( ) ) {
            std::cout << "    Overlay Name: \"" << ovldef->GetOverlayName( ) << "\"\n"
                      << "    Location    : Block=" << ovldef->GetLocationBlock( )
                      << ", ByteOffset=" << Hex16( ovldef->GetLocationByteOffset( ) ) << "\n"
                      << "    Attributes  : " << Hex8( ovldef->GetOverlayAttributes( ) )
                      << ", SharedIdx=#" << ovldef->GetSharedOverlayIndex( ).value
                      << ", AdjacentIdx=#" << ovldef->GetAdjacentOverlayIndex( ).value << "\n";

        } else if ( auto const* endrec = rec.As<Omf::OmfEndrecRecord>( ) ) {
            std::cout << "    End Type    : "
                      << ( endrec->GetEndType( ) == Omf::OmfEndrecRecord::EndType::Overlay ? "Overlay" : "Block" ) << "\n";

        } else if ( auto const* blkdef = rec.As<Omf::OmfBlkdefRecord>( ) ) {
            std::cout << "    Block Name  : \"" << blkdef->GetBlockName( ) << "\"\n"
                      << "    Base Group  : " << ctx.FormatGroupIndex( blkdef->GetBaseGroup( ) ) << "\n"
                      << "    Base Segment: " << ctx.FormatSegmentIndex( blkdef->GetBaseSegment( ) ) << "\n"
                      << "    Block Offset: " << HexOffset( blkdef->GetBlockOffset( ), is32 ) << "\n"
                      << "    Block Length: " << HexOffset( blkdef->GetBlockLength( ), is32 ) << "\n"
                      << "    Proc Flags  : " << Hex8( blkdef->GetProcedureFlags( ) ) << "\n"
                      << "    Type Index  : #" << blkdef->GetTypeIndex( ).value << "\n";

        } else if ( rec.As<Omf::OmfBlkendRecord>( ) != nullptr ) {
            std::cout << "    <Block End>\n";

        } else if ( auto const* debsym = rec.As<Omf::OmfDebsymRecord>( ) ) {
            auto const& syms = debsym->GetSymbols( );
            std::cout << "    Frame Method: " << Hex8( debsym->GetFrameMethod( ) ) << "\n";
            PrintFixupTargetSpec( debsym->GetBaseSpec( ), false, ctx, "      " );
            std::cout << "    Symbol Count: " << syms.size( ) << "\n";
            for ( std::size_t i = 0; i < syms.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] Offset="
                          << Hex16( static_cast<std::uint16_t>( syms[i].offset ) )
                          << ", Type=#" << syms[i].typeIndex.value
                          << ", Name=\"" << syms[i].name << "\"\n";
            }

        } else if ( auto const* pedata = rec.As<Omf::OmfPedataRecord>( ) ) {
            std::cout << "    Frame Number: " << Hex16( pedata->GetFrameNumber( ) ) << "\n"
                      << "    Offset      : " << Hex8( pedata->GetOffset( ) ) << "\n"
                      << "    Data Bytes  : " << pedata->GetData( ).size( ) << " byte(s)\n";
            PrintHexDump( pedata->GetData( ), pedata->GetOffset( ), "      " );

        } else if ( auto const* pidata = rec.As<Omf::OmfPidataRecord>( ) ) {
            std::cout << "    Frame Number: " << Hex16( pidata->GetFrameNumber( ) ) << "\n"
                      << "    Offset      : " << Hex8( pidata->GetOffset( ) ) << "\n";
            PrintIteratedDataBlocks( pidata->GetBlocks( ), "      " );

        } else if ( auto const* libhed = rec.As<Omf::OmfLibhedRecord>( ) ) {
            std::cout << "    Module Count: " << libhed->GetModuleCount( ) << "\n"
                      << "    Dictionary  : Block=" << libhed->GetDictionaryBlockNumber( )
                      << ", ByteOffset=" << Hex16( libhed->GetDictionaryByteOffset( ) ) << "\n"
                      << "    Flags       : " << Hex8( libhed->GetFlags( ) ) << "\n";

        } else if ( auto const* libnam = rec.As<Omf::OmfLibnamRecord>( ) ) {
            auto const& names = libnam->GetModuleNames( );
            std::cout << "    Module Names: " << names.size( ) << "\n";
            for ( std::size_t i = 0; i < names.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] \"" << names[i] << "\"\n";
            }

        } else if ( auto const* libloc = rec.As<Omf::OmfLiblocRecord>( ) ) {
            auto const& locs = libloc->GetLocations( );
            std::cout << "    Locations   : " << locs.size( ) << "\n";
            for ( std::size_t i = 0; i < locs.size( ); ++i ) {
                std::cout << "      [" << std::setw( 3 ) << i << "] Block=" << locs[i].blockNumber
                          << ", ByteOffset=" << Hex16( locs[i].byteOffset ) << "\n";
            }

        } else if ( auto const* libdic = rec.As<Omf::OmfLibdicRecord>( ) ) {
            auto const& groups = libdic->GetModulePublicGroups( );
            std::cout << "    Module Grps : " << groups.size( ) << "\n";
            for ( std::size_t g = 0; g < groups.size( ); ++g ) {
                std::cout << "      Module [" << g << "] (" << groups[g].size( ) << " symbol(s)):\n";
                for ( auto const& sym : groups[g] ) {
                    std::cout << "        - \"" << sym << "\"\n";
                }
            }

        } else if ( auto const* mshdr = rec.As<Omf::OmfMsLibHeaderRecord>( ) ) {
            std::cout << "    Page Size   : " << mshdr->GetPageSize( ) << " bytes\n"
                      << "    Dict Offset : " << Hex32( mshdr->GetDictionaryFileOffset( ) )
                      << " (" << mshdr->GetDictionaryFileOffset( ) << ")\n"
                      << "    Dict Blocks : " << mshdr->GetDictionaryBlockCount( ) << " (512-byte blocks)\n"
                      << "    Flags       : " << Hex8( mshdr->GetFlags( ) )
                      << ( mshdr->IsCaseSensitive( ) ? " (Case-Sensitive)" : " (Case-Insensitive)" ) << "\n";

        } else if ( auto const* msend = rec.As<Omf::OmfMsLibEndRecord>( ) ) {
            std::cout << "    Padding     : " << msend->GetPaddingBytes( ).size( ) << " byte(s)\n";

        } else if ( auto const* unk = rec.As<Omf::OmfUnknownRecord>( ) ) {
            std::cout << "    Unknown Rec : " << unk->GetRawPayload( ).size( ) << " payload byte(s)\n";
            PrintHexDump( unk->GetRawPayload( ), 0, "      " );
        }

        if ( opts.showRawPayload && ( rec.As<Omf::OmfUnknownRecord>( ) == nullptr ) ) {
            std::cout << "    Raw Payload (" << rec.GetRawPayload( ).size( ) << " bytes):\n";
            PrintHexDump( rec.GetRawPayload( ), 0, "      " );
        }
    }

    void PrintUsage( char const* progName ) {
        std::cerr <<
            "Usage:\n"
            "  " << progName << " <input.obj|input.lib> [options]\n\n"
            "Options:\n"
            "  --raw            Hex-dump the raw interior payload bytes of every record\n"
            "  --expand-lidata  Expand repeating LIDATA blocks and hex-dump the expanded bytes\n"
            "  --summary        Print only the 1-line header summary for each record\n";
    }

} // anonymous namespace

int main( int argc, char* argv[] ) {
    if ( argc < 2 ) {
        PrintUsage( argv[0] );
        return 1;
    }

    std::filesystem::path inputPath;
    DumpOptions           opts      { };

    for ( int i { 1 }; i < argc; ++i ) {
        std::string_view const arg { argv[i] };
        if ( arg == "--raw" ) {
            opts.showRawPayload = true;
        } else if ( arg == "--expand-lidata" ) {
            opts.expandLidata = true;
        } else if ( arg == "--summary" ) {
            opts.summaryOnly = true;
        } else if ( ( arg == "--help" ) || ( arg == "-h" ) || ( arg == "/h" ) || ( arg == "/?" ) ) {
            PrintUsage( argv[0] );
            return 0;
        } else if ( inputPath.empty( ) ) {
            inputPath = arg;
        } else {
            std::cerr << "Error: Unexpected argument '" << arg << "'\n\n";
            PrintUsage( argv[0] );
            return 1;
        }
    }

    if ( inputPath.empty( ) ) {
        PrintUsage( argv[0] );
        return 1;
    }

    try {
        Omf::OmfFile const omfFile { Omf::OmfFile::LoadFromFile( inputPath ) };
        auto const         records { omfFile.GetRecords( ) };
        auto const&        modules { omfFile.GetModules( ) };

        std::cout <<
            "=============================================================================\n"
            "File:        " << inputPath.string( ) << "\n"
            "Records:     " <<   records.size( )   << "\n"
            "Modules:     " <<   modules.size( )   << "\n"
            "=============================================================================\n\n";

        ModuleContext ctx;
        std::size_t   fileOffset  { };
        std::size_t   moduleIndex { };

        for ( std::size_t i { }, limit { records.size( ) }; i < limit; ++i ) {
            Omf::OmfRecord  const& rec          { *records[i]                  };
            Omf::RecordType const  type         { rec.GetRecordType( )         };
            std::size_t     const  payloadLen   { rec.GetRawPayload( ).size( ) };
            std::size_t     const  recordLen    { payloadLen + 1u              };
            std::size_t     const  totalBytes   {  recordLen + 3u              };
            std::uint8_t    const  origChecksum { rec.GetOriginalChecksum( )   };
            std::uint8_t    const  calcChecksum { rec.ComputeChecksum( )       };
            bool            const  isMsLib      { ( type == Omf::RecordType::MsLibHeader ) || ( type == Omf::RecordType::MsLibEnd ) };

            std::cout << "[" << std::setw( 4 ) << i << "] "
                      << Hex32( static_cast<std::uint32_t>( fileOffset ) ) << "  "
                      << std::left << std::setw( 10 ) << GetRecordTypeName( type ) << std::right
                      << " (" << Hex8( static_cast<std::uint8_t>( type ) ) << ")"
                      << "  Len=" << std::setw( 5 ) << recordLen
                      << " (Payload=" << std::setw( 5 ) << payloadLen << ")  ";

            if ( isMsLib ) {
                std::cout << "PadByte=" << Hex8( origChecksum );
            } else if ( origChecksum == 0u ) {
                std::cout << "Chksum=0x00 (Unchecked)";
            } else if ( rec.HasValidChecksum( ) ) {
                std::cout << "Chksum=" << Hex8( origChecksum ) << " (OK)";
            } else {
                std::cout << "Chksum=" << Hex8( origChecksum )
                          << " (BAD: expected " << Hex8( calcChecksum ) << ")";
            }
            std::cout << "\n";

            if ( !opts.summaryOnly ) {
                DumpRecordDetails( rec, ctx, opts );
                std::cout << "\n";
            }

            fileOffset += totalBytes;

            // Advance fileOffset past any .LIB inter-module page alignment padding
            if ( ( ( type == Omf::RecordType::Modend ) || ( type == Omf::RecordType::Modend32 ) ) &&
                 ( moduleIndex < modules.size( ) ) ) {
                auto const padding = modules[moduleIndex].GetTrailingLibraryPadding( );
                if ( !padding.empty( ) ) {
                    if ( !opts.summaryOnly ) {
                        std::cout << "       " << Hex32( static_cast<std::uint32_t>( fileOffset ) )
                                  << "  <.LIB Page Alignment Padding: " << padding.size( ) << " byte(s)>\n\n";
                    }
                    fileOffset += padding.size( );
                }
                ++moduleIndex;
            }
        }

        auto const trailingDict = omfFile.GetTrailingLibraryDictionary( );
        if ( !trailingDict.empty( ) ) {
            std::cout << "Trailing MS-DOS .LIB Hashed Dictionary at "
                      << Hex32( static_cast<std::uint32_t>( fileOffset ) )
                      << " (" << trailingDict.size( ) << " bytes, "
                      << ( trailingDict.size( ) / 512u ) << " block(s))\n";
            if ( opts.showRawPayload && !opts.summaryOnly ) {
                PrintHexDump( trailingDict, static_cast<std::uint32_t>( fileOffset ), "  " );
            }
        }

        return 0;
    } catch ( Omf::OmfParseException const& ex ) {
        std::cerr << "OMF Parse Error at offset 0x" << std::hex << ex.GetFileOffset( )
                  << " (record type 0x" << static_cast<int>( ex.GetRecordType( ) ) << std::dec << "): "
                  << ex.what( ) << "\n";
        return 2;
    } catch ( std::exception const& ex ) {
        std::cerr << "Error: " << ex.what( ) << "\n";
        return 3;
    }
}
