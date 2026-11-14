#include "OmfFile.h"

#include <cctype>
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
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
            std::uint8_t const num { static_cast<std::uint8_t>( trd.threadNumber & 0x03u ) };
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
            return std::format( "#{} (\"{}\")", idx.value, ResolveNameRaw( idx ) );
        }

        std::string FormatSegmentIndex( Omf::SegmentIndex idx ) const {
            if ( !idx.IsValid( ) ) {
                return "0 (none)";
            }
            if ( idx.value <= _segments.size( ) ) {
                return std::format( "#{} ({})", idx.value, _segments[idx.value - 1u] );
            }
            return std::format( "#{} (<unresolved segment>)", idx.value );
        }

        std::string FormatGroupIndex( Omf::GroupIndex idx ) const {
            if ( !idx.IsValid( ) ) {
                return "0 (none)";
            }
            if ( idx.value <= _groups.size( ) ) {
                return std::format( "#{} ({})", idx.value, _groups[idx.value - 1u] );
            }
            return std::format( "#{} (<unresolved group>)", idx.value );
        }

        std::string FormatExternalIndex( Omf::ExternalIndex idx ) const {
            if ( !idx.IsValid( ) ) {
                return "0 (none)";
            }
            if ( idx.value <= _externals.size( ) ) {
                return std::format( "#{} (\"{}\")", idx.value, _externals[idx.value - 1u] );
            }
            return std::format( "#{} (<unresolved ext>)", idx.value );
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
        return std::format( "0x{:02X}", static_cast<unsigned>( val ) );
    }

    std::string Hex16( std::uint16_t val ) {
        return std::format( "0x{:04X}", static_cast<unsigned>( val ) );
    }

    std::string Hex32( std::uint32_t val ) {
        return std::format( "0x{:08X}", val );
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
            case 0:  return "F0: Segment "  + ctx.FormatSegmentIndex( Omf::SegmentIndex{ datum } );
            case 1:  return "F1: Group "    + ctx.FormatGroupIndex( Omf::GroupIndex{ datum } );
            case 2:  return "F2: External " + ctx.FormatExternalIndex( Omf::ExternalIndex{ datum } );
            case 3:  return "F3: Explicit Frame " + Hex16( datum );
            case 4:  return "F4: Location Segment (inferred from data record)";
            case 5:  return "F5: Target's Frame (determined by target)";
            default: return std::format( "F{}: Reserved", method );
        }
    }

    std::string FormatTargetMethod( std::uint8_t method, std::uint16_t datum, ModuleContext const& ctx ) {
        switch ( method & 0x03u ) {
            case 0:  return std::format( "T{}: Segment {}",        method, ctx.FormatSegmentIndex( Omf::SegmentIndex{ datum } ) );
            case 1:  return std::format( "T{}: Group {}",          method, ctx.FormatGroupIndex( Omf::GroupIndex{ datum } ) );
            case 2:  return std::format( "T{}: External {}",       method, ctx.FormatExternalIndex( Omf::ExternalIndex{ datum } ) );
            case 3:  return std::format( "T{}: Explicit Frame {}", method, Hex16( datum ) );
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
            std::uint8_t const tNum { static_cast<std::uint8_t>( spec.frameMethodOrThread & 0x03u ) };
            if ( auto const trd { ctx.GetFrameThread( tNum ) }; trd ) {
                std::cout << std::format( "Thread #{} -> [{}]\n", static_cast<unsigned>( tNum ), FormatFrameMethod( trd->method, trd->index, ctx ) );
            } else {
                std::cout << std::format( "Thread #{} -> [<unset frame thread>]\n", static_cast<unsigned>( tNum ) );
            }
        } else {
            std::cout << std::format( "{}\n", FormatFrameMethod( spec.frameMethodOrThread, spec.frameDatum.value_or( 0 ), ctx ) );
        }

        std::cout << indent << "Target : ";
        if ( spec.targetUsesThread ) {
            std::uint8_t const tNum { static_cast<std::uint8_t>( spec.targetMethodOrThread & 0x03u ) };
            if ( auto const trd { ctx.GetTargetThread( tNum ) }; trd ) {
                std::cout << std::format( "Thread #{} -> [{}]", static_cast<unsigned>( tNum ), FormatTargetMethod( trd->method, trd->index, ctx ) );
            } else {
                std::cout << std::format( "Thread #{} -> [<unset target thread>]", static_cast<unsigned>( tNum ) );
            }
        } else {
            std::cout << FormatTargetMethod( spec.targetMethodOrThread, spec.targetDatum.value_or( 0 ), ctx );
        }

        if ( spec.targetDisplacement.has_value( ) ) {
            std::cout << std::format( " + Disp {}\n", HexOffset( *spec.targetDisplacement, is32Bit ) );
        } else {
            std::cout << " (No Displacement, P=1)\n";
        }
    }

    void PrintHexDump( std::span<std::byte const> data, std::uint32_t startOffset = 0, std::string_view indent = "      " ) {
        if ( data.empty( ) ) {
            std::cout << indent << "<empty data>\n";
            return;
        }

        for ( std::size_t i { }, limit { data.size( ) }; i < limit; i += 16u ) {
            std::size_t const lineCount { std::min<std::size_t>( 16u, data.size( ) - i ) };
            std::cout << indent << Hex32( startOffset + static_cast<std::uint32_t>( i ) ) << " : ";

            for ( std::size_t j { }; j < 16u; ++j ) {
                if ( j == 8u ) {
                    std::cout << " ";
                }
                if ( j < lineCount ) {
                    std::cout << std::format( "{:02X} ", std::to_integer<unsigned>( data[i + j] ) );
                } else {
                    std::cout << "   ";
                }
            }

            std::cout << " |";
            for ( std::size_t j { }; j < lineCount; ++j ) {
                unsigned char const ch { std::to_integer<unsigned char>( data[i + j] ) };
                std::cout << ( ( ( ch >= 0x20u ) && ( ch <= 0x7Eu ) ) ? static_cast<char>( ch ) : '.' );
            }
            std::cout << "|\n";
        }
    }

    void PrintIteratedDataBlocks( std::vector<Omf::OmfLidataRecord::IteratedDataBlock> const& blocks, std::string const& indent = "      " ) {
        std::string const newIndent { indent + "  " };
        for ( std::size_t i { }, limit { blocks.size( ) }; i < limit; ++i ) {
            auto const& block { blocks[i] };
            if ( std::holds_alternative<std::vector<std::byte>>( block.content ) ) {
                auto const& bytes { std::get<std::vector<std::byte>>( block.content ) };
                std::cout << indent << std::format( "Block [{}]: Repeat={}, Leaf Bytes ({} bytes):\n", i, block.repeatCount, bytes.size( ) );
                PrintHexDump( bytes, 0, newIndent );
            } else {
                auto const& nested { std::get<std::vector<Omf::OmfLidataRecord::IteratedDataBlock>>( block.content ) };
                std::cout << indent << std::format( "Block [{}]: Repeat={}, Nested Blocks ({}):\n", i, block.repeatCount, nested.size( ) );
                PrintIteratedDataBlocks( nested, newIndent );
            }
        }
    }

    //=========================================================================
    // Per-Record Verbose Printer
    //=========================================================================

    void DumpRecordDetails( Omf::OmfRecord const& rec, ModuleContext& ctx, DumpOptions const& opts ) {
        bool const is32 { rec.Is32Bit( ) };

        if ( auto const* hdr { rec.As<Omf::OmfHeaderRecord>( ) }; hdr ) {
            ctx.Reset( );
            std::cout << std::format(
                "    Header Kind : {}\n"
                "    Module Name : \"{}\"\n",
                hdr->IsLibraryHeader( ) ? "LHEADR (Library Module)" : "THEADR (Translator Module)",
                hdr->GetModuleName( )
            );
        } else if ( auto const* rhdr { rec.As<Omf::OmfRheadrRecord>( ) }; rhdr ) {
            ctx.Reset( );
            std::cout << std::format(
                "    Module Name : \"{}\"\n"
                "    Module Type : {}\n",
                rhdr->GetModuleName( ),
                Hex8( rhdr->GetModuleType( ) )
            );
            if ( !rhdr->GetTrailingAttributes( ).empty( ) ) {
                std::cout << std::format( "    Attributes  : {} byte(s)\n", rhdr->GetTrailingAttributes( ).size( ) );
                PrintHexDump( rhdr->GetTrailingAttributes( ), 0, "      " );
            }
        } else if ( auto const* coment { rec.As<Omf::OmfComentRecord>( ) }; coment ) {
            std::uint8_t const cls  { coment->GetCommentClass( ) };
            auto const         data { coment->GetCommentData( )  };

            std::cout << std::format(
                "    Flags       : NoPurge={}, NoList={}\n"
                "    Class       : {} ({})\n",
                ( coment->IsNoPurge( ) ? "true" : "false" ), ( coment->IsNoList( ) ? "true" : "false" ),
                Hex8( cls ), GetComentClassDescription( cls )
            );

            // Check if the comment data is purely printable ASCII text
            bool isPrintable { !data.empty( ) };
            for ( std::byte const b : data ) {
                unsigned char const c { std::to_integer<unsigned char>( b ) };
                if ( ( ( c < 0x20u ) || ( c > 0x7Eu ) ) && ( c != '\t' ) && ( c != '\r' ) && ( c != '\n' ) ) {
                    isPrintable = false;
                    break;
                }
            }

            if ( isPrintable ) {
                std::string_view const text( reinterpret_cast<char const*>( data.data( ) ), data.size( ) );
                std::cout << std::format( "    Text        : \"{}\"\n", text );
            }
            if ( !data.empty( ) ) {
                std::cout << std::format( "    Data Bytes  : {} byte(s)\n", data.size( ) );
                PrintHexDump( data, 0, "      " );
            }
        } else if ( auto const* lnames { rec.As<Omf::OmfLnamesRecord>( ) }; lnames ) {
            auto const& names { lnames->GetNames( ) };
            std::cout << std::format(
                "    Scope       : {}\n"
                "    Name Count  : {}\n",
                ( lnames->IsLocal( ) ? "LLNAMES (Local)" : "LNAMES (Global)" ),
                names.size( )
            );
            for ( auto const& name : names ) {
                std::size_t const oneBasedIdx { ctx.AddLname( name ) };
                std::cout << std::format( "      Name #{:3} : \"{}\"\n", oneBasedIdx, name );
            }
        } else if ( auto const* seg { rec.As<Omf::OmfSegdefRecord>( ) }; seg ) {
            std::size_t const segNumber { ctx.AddSegment( seg->GetSegmentNameIndex( ), seg->GetClassNameIndex( ) ) };
            std::cout << std::format(
                "    Segment #   : {}\n"
                "    Segment Name: {}\n"
                "    Class Name  : {}\n"
                "    Overlay Name: {}\n"
                "    Alignment   : {}\n"
                "    Combination : {}\n"
                "    Attributes  : Big/MaxLength={}, Use32={}\n"
                "    Seg Length  : {} ({})\n",
                segNumber,
                ctx.FormatNameIndex( seg->GetSegmentNameIndex( ) ),
                ctx.FormatNameIndex( seg->  GetClassNameIndex( ) ),
                ctx.FormatNameIndex( seg->GetOverlayNameIndex( ) ),
                GetAlignmentName( seg->GetAlignment( ) ),
                GetCombinationName( seg->GetCombination( ) ),
                seg->IsMaxLength( ) ? "true (64K/4G)" : "false",
                seg->IsUse32( ) ? "true (32-bit)" : "false (16-bit)",
                HexOffset( seg->GetSegmentLength( ), is32 ),
                seg->GetSegmentLength( )
            );
            if ( seg->GetAlignment( ) == Omf::OmfSegdefRecord::Alignment::Absolute ) {
                std::cout << std::format(
                    "    Abs Address : Frame={}, Offset={}\n",
                    Hex16( seg->GetAbsoluteFrame( ).value_or( 0 ) ),
                    Hex8( seg->GetAbsoluteOffset( ).value_or( 0 ) )
                );
            }
        } else if ( auto const* grp { rec.As<Omf::OmfGrpdefRecord>( ) }; grp ) {
            std::size_t const grpNumber { ctx.AddGroup( grp->GetGroupNameIndex( ) ) };
            auto const&       comps     { grp->GetComponents( )                     };
            std::cout << std::format(
                "    Group #     : {}\n"
                "    Group Name  : {}\n"
                "    Components  : {}\n",
                grpNumber,
                ctx.FormatNameIndex( grp->GetGroupNameIndex( ) ),
                comps.size( )
            );
            for ( std::size_t i { }, limit { comps.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{}] Descriptor={}, Segment={}\n",
                    i,
                    Hex8( comps[i].descriptor ),
                    ctx.FormatSegmentIndex( comps[i].segmentIndex )
                );
            }
        } else if ( auto const* pub { rec.As<Omf::OmfPubdefRecord>( ) }; pub ) {
            auto const& syms { pub->GetSymbols( ) };
            std::cout << std::format(
                "    Visibility  : {}\n"
                "    Base Group  : {}\n"
                "    Base Segment: {}\n",
                pub->IsLocal( ) ? "Local (LPUBDEF)" : "Public (PUBDEF)",
                ctx.FormatGroupIndex( pub->GetBaseGroup( ) ),
                ctx.FormatSegmentIndex( pub->GetBaseSegment( ) )
            );
            if ( pub->GetBaseFrame( ).has_value( ) ) {
                std::cout << std::format( "    Base Frame  : {}\n", Hex16( *pub->GetBaseFrame( ) ) );
            }
            std::cout << std::format( "    Symbol Count: {}\n", syms.size( ) );
            for ( std::size_t i { }, limit { syms.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{:3}] Offset={}, Type=#{}, Name=\"{}\"\n",
                    i,
                    HexOffset( syms[i].offset, is32 ),
                    syms[i].typeIndex.value,
                    syms[i].name
                );
            }
        } else if ( auto const* ext { rec.As<Omf::OmfExtdefRecord>( ) }; ext ) {
            auto const& syms { ext->GetSymbols( ) };
            std::cout << std::format(
                "    Visibility  : {}\n"
                "    Symbol Count: {}\n",
                ext->IsLocal( ) ? "Local (LEXTDEF)" : "Global (EXTDEF)",
                syms.size( )
            );
            for ( auto const& sym : syms ) {
                std::size_t const extIdx { ctx.AddExternal( sym.name ) };
                std::cout << std::format(
                    "      Ext #{:3} : Type=#{}, Name=\"{}\"\n",
                    extIdx,
                    sym.typeIndex.value,
                    sym.name
                );
            }
        } else if ( auto const* cext { rec.As<Omf::OmfCextdefRecord>( ) }; cext ) {
            auto const& exts { cext->GetExternals( ) };
            std::cout << std::format( "    External Cnt: {}\n", exts.size( ) );
            for ( auto const& e : exts ) {
                std::size_t const extIdx { ctx.AddComdatExternal( e.logicalNameIndex ) };
                std::cout << std::format(
                    "      Ext #{:3} : LogicalName={}, Type=#{}\n",
                    extIdx,
                    ctx.FormatNameIndex( e.logicalNameIndex ),
                    e.typeIndex.value
                );
            }
        } else if ( auto const* typ { rec.As<Omf::OmfTypdefRecord>( ) }; typ ) {
            std::cout << std::format(
                "    Type Name   : \"{}\"\n"
                "    Descriptor  : {} byte(s)\n",
                typ->GetTypeName( ),
                typ->GetRawTypeDescriptor( ).size( )
            );
            PrintHexDump( typ->GetRawTypeDescriptor( ), 0, "      " );
        } else if ( auto const* loc { rec.As<Omf::OmfLocsymRecord>( ) }; loc ) {
            auto const& syms { loc->GetSymbols( ) };
            std::cout << std::format(
                "    Base Group  : {}\n"
                "    Base Segment: {}\n",
                ctx.FormatGroupIndex( loc->GetBaseGroup( ) ),
                ctx.FormatSegmentIndex( loc->GetBaseSegment( ) )
            );
            if ( loc->GetBaseFrame( ).has_value( ) ) {
                std::cout << std::format( "    Base Frame  : {}\n", Hex16( *loc->GetBaseFrame( ) ) );
            }
            std::cout << std::format( "    Symbol Count: {}\n", syms.size( ) );
            for ( std::size_t i { }, limit { syms.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{:3}] Offset={}, Type=#{}, Name=\"{}\"\n",
                    i,
                    HexOffset( syms[i].offset, is32 ),
                    syms[i].typeIndex.value,
                    syms[i].name
                );
            }
        } else if ( auto const* lin { rec.As<Omf::OmfLinnumRecord>( ) }; lin ) {
            auto const& entries { lin->GetEntries( ) };
            std::cout << std::format(
                "    Base Group  : {}\n"
                "    Base Segment: {}\n"
                "    Line Entries: {}\n",
                ctx.FormatGroupIndex( lin->GetBaseGroup( ) ),
                ctx.FormatSegmentIndex( lin->GetBaseSegment( ) ),
                entries.size( )
            );
            for ( std::size_t i { }, limit { entries.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{:3}] Line {:5} -> Offset {}\n",
                    i,
                    entries[i].lineNumber,
                    HexOffset( entries[i].offset, is32 )
                );
            }
        } else if ( auto const* linsym { rec.As<Omf::OmfLinsymRecord>( ) }; linsym ) {
            auto const& entries { linsym->GetEntries( ) };
            std::cout << std::format(
                "    Flags       : {} ({})\n"
                "    Symbol Name : {}\n"
                "    Line Entries: {}\n",
                Hex8( linsym->GetFlags( ) ),
                linsym->IsContinuation( ) ? "Continuation" : "First/Primary",
                ctx.FormatNameIndex( linsym->GetSymbolNameIndex( ) ),
                entries.size( )
            );
            for ( std::size_t i { }, limit { entries.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{:3}] Line {:5} -> Offset {}\n",
                    i,
                    entries[i].lineNumber,
                    HexOffset( entries[i].offset, is32 )
                );
            }
        } else if ( auto const* ledata { rec.As<Omf::OmfLedataRecord>( ) }; ledata ) {
            auto const data { ledata->GetData( ) };
            std::cout << std::format(
                "    Segment     : {}\n"
                "    Data Offset : {}\n"
                "    Data Length : {} byte(s)\n",
                ctx.FormatSegmentIndex( ledata->GetSegmentIndex( ) ),
                HexOffset( ledata->GetDataOffset( ), is32 ),
                data.size( )
            );
            PrintHexDump( data, ledata->GetDataOffset( ), "      " );
        } else if ( auto const* lidata { rec.As<Omf::OmfLidataRecord>( ) }; lidata ) {
            std::cout << std::format(
                "    Segment     : {}\n"
                "    Data Offset : {}\n"
                "    Top Blocks  : {}\n",
                ctx.FormatSegmentIndex( lidata->GetSegmentIndex( ) ),
                HexOffset( lidata->GetDataOffset( ), is32 ),
                lidata->GetBlocks( ).size( )
            );
            PrintIteratedDataBlocks( lidata->GetBlocks( ), "      " );
            if ( opts.expandLidata ) {
                auto const expanded { lidata->ExpandData( ) };
                std::cout << std::format( "    Expanded    : {} byte(s)\n", expanded.size( ) );
                PrintHexDump( expanded, lidata->GetDataOffset( ), "      " );
            }
        } else if ( auto const* fixupp { rec.As<Omf::OmfFixuppRecord>( ) }; fixupp ) {
            auto const& subs { fixupp->GetSubrecords( ) };
            std::cout << std::format( "    Subrecords  : {}\n", subs.size( ) );
            for ( std::size_t i { }, limit { subs.size( ) }; i < limit; ++i ) {
                if ( std::holds_alternative<Omf::OmfFixuppRecord::ThreadSubrecord>( subs[i] ) ) {
                    auto const& trd { std::get<Omf::OmfFixuppRecord::ThreadSubrecord>( subs[i] ) };
                    ctx.SetThread( trd );
                    std::cout << std::format(
                        "      [{:3}] THREAD ({} Thread #{}): {}\n",
                        i,
                        trd.isFrameThread ? "Frame" : "Target",
                        static_cast<unsigned>( trd.threadNumber ),
                        trd.isFrameThread
                            ? FormatFrameMethod( trd.method, trd.index, ctx )
                            : FormatTargetMethod( trd.method, trd.index, ctx )
                    );
                } else {
                    auto const& fix { std::get<Omf::OmfFixuppRecord::FixupSubrecord>( subs[i] ) };
                    std::cout << std::format(
                        "      [{:3}] FIXUP at DataRecordOffset={} ({}):\n"
                        "            Mode   : {}\n"
                        "            Loc    : {}\n",
                        i,
                        Hex16( fix.dataRecordOffset ),
                        fix.dataRecordOffset,
                        fix.isSegmentRelative ? "Segment-Relative (M=1)" : "Self-Relative (M=0)",
                        GetFixupLocationName( fix.location )
                    );
                    PrintFixupTargetSpec( fix.target, is32, ctx, "            " );
                }
            }
        } else if ( auto const* comdef { rec.As<Omf::OmfComdefRecord>( ) }; comdef ) {
            auto const& syms { comdef->GetSymbols( ) };
            std::cout << std::format(
                "    Visibility  : {}\n"
                "    Symbol Count: {}\n",
                comdef->IsLocal( ) ? "Local (LCOMDEF)" : "Global (COMDEF)",
                syms.size( )
            );
            for ( auto const& sym : syms ) {
                std::size_t const extIdx { ctx.AddExternal( sym.name ) };
                bool const        isFar  { sym.dataSegmentType == static_cast<std::uint8_t>( Omf::OmfComdefRecord::DataSegmentType::FarComm )  };
                bool const        isNear { sym.dataSegmentType == static_cast<std::uint8_t>( Omf::OmfComdefRecord::DataSegmentType::NearComm ) };
                std::cout << std::format(
                    "      Ext #{:3} : Name=\"{}\", Type=#{}, SegType={} ({})",
                    extIdx,
                    sym.name,
                    sym.typeIndex.value,
                    Hex8( sym.dataSegmentType ),
                    isFar ? "FAR_COMM" : ( isNear ? "NEAR_COMM" : "SegIdx" )
                );
                if ( isFar ) {
                    std::cout << std::format(
                        ", Elements={}, ElemSize={} (Total={} bytes)\n",
                        sym.elementCount,
                        sym.elementSize,
                        sym.elementCount * sym.elementSize
                    );
                } else {
                    std::cout << std::format( ", Size={} byte(s)\n", sym.elementCount );
                }
            }
        } else if ( auto const* comdat { rec.As<Omf::OmfComdatRecord>( ) }; comdat ) {
            std::cout << std::format(
                "    Symbol Name : {}\n"
                "    Flags       : {} [Continuation={}, Iterated={}, Local={}, CodeSeg={}]\n"
                "    Selection   : {}\n"
                "    Allocation  : {}\n"
                "    Alignment   : {}\n"
                "    Data Offset : {}\n"
                "    Type Index  : #{}\n",
                ctx.FormatNameIndex( comdat->GetPublicNameIndex( ) ),
                Hex8( comdat->GetFlags( ) ),
                comdat->IsContinuation( )    ? "yes" : "no",
                comdat->IsIteratedData( )    ? "yes" : "no",
                comdat->IsLocalSymbol( )     ? "yes" : "no",
                comdat->IsCodeSegmentData( ) ? "yes" : "no",
                Hex8( static_cast<std::uint8_t>( comdat->GetSelectionCriteria( ) ) ),
                Hex8( static_cast<std::uint8_t>( comdat->GetAllocationType( ) ) ),
                GetAlignmentName( comdat->GetAlignment( ) ),
                HexOffset( comdat->GetDataOffset( ), is32 ),
                comdat->GetTypeIndex( ).value
            );

            if ( comdat->GetAllocationType( ) == Omf::OmfComdatRecord::AllocationType::Explicit ) {
                std::cout << std::format(
                    "    Base Group  : {}\n"
                    "    Base Segment: {}\n",
                    ctx.FormatGroupIndex( comdat->GetPublicBaseGroup( ) ),
                    ctx.FormatSegmentIndex( comdat->GetPublicBaseSegment( ) )
                );
                if ( comdat->GetPublicBaseFrame( ).has_value( ) ) {
                    std::cout << std::format( "    Base Frame  : {}\n", Hex16( *comdat->GetPublicBaseFrame( ) ) );
                }
            }

            auto const& payload { comdat->GetDataPayload( ) };
            if ( std::holds_alternative<std::vector<std::byte>>( payload ) ) {
                auto const& bytes { std::get<std::vector<std::byte>>( payload ) };
                std::cout << std::format( "    Data Bytes  : {} byte(s)\n", bytes.size( ) );
                PrintHexDump( bytes, comdat->GetDataOffset( ), "      " );
            } else {
                auto const& blocks { std::get<std::vector<Omf::OmfLidataRecord::IteratedDataBlock>>( payload ) };
                std::cout << std::format( "    Iterated Blk: {}\n", blocks.size( ) );
                PrintIteratedDataBlocks( blocks, "      " );
            }
        } else if ( auto const* bakpat { rec.As<Omf::OmfBakpatRecord>( ) }; bakpat ) {
            auto const& entries { bakpat->GetEntries( ) };
            std::cout << std::format(
                "    Segment     : {}\n"
                "    Location    : {}\n"
                "    Patch Count : {}\n",
                ctx.FormatSegmentIndex( bakpat->GetSegmentIndex( ) ),
                GetFixupLocationName( bakpat->GetLocationType( ) ),
                entries.size( )
            );
            for ( std::size_t i { }, limit { entries.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{:3}] Offset={} <- Value={}\n",
                    i,
                    HexOffset( entries[i].offset, is32 ),
                    HexOffset( entries[i].value, is32 )
                );
            }
        } else if ( auto const* nbkpat { rec.As<Omf::OmfNbkpatRecord>( ) }; nbkpat ) {
            auto const& entries { nbkpat->GetEntries( ) };
            std::cout << std::format(
                "    COMDAT Name : {}\n"
                "    Location    : {}\n"
                "    Patch Count : {}\n",
                ctx.FormatNameIndex( nbkpat->GetPublicNameIndex( ) ),
                GetFixupLocationName( nbkpat->GetLocationType( ) ),
                entries.size( )
            );
            for ( std::size_t i { }, limit { entries.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{:3}] Offset={} <- Value={}\n",
                    i,
                    HexOffset( entries[i].offset, is32 ),
                    HexOffset( entries[i].value, is32 )
                );
            }
        } else if ( auto const* alias { rec.As<Omf::OmfAliasRecord>( ) }; alias ) {
            auto const& aliases { alias->GetAliases( ) };
            std::cout << std::format( "    Alias Count : {}\n", aliases.size( ) );
            for ( std::size_t i { }, limit { aliases.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{}] \"{}\" -> \"{}\"\n",
                    i,
                    aliases[i].aliasName,
                    aliases[i].substituteName
                );
            }
        } else if ( auto const* vernum { rec.As<Omf::OmfVernumRecord>( ) }; vernum ) {
            std::cout << std::format( "    OMF Version : \"{}\"\n", vernum->GetVersion( ) );
        } else if ( auto const* vendext { rec.As<Omf::OmfVendextRecord>( ) }; vendext ) {
            std::cout << std::format(
                "    Subtype     : {}\n"
                "    Ext Data    : {} byte(s)\n",
                Hex16( vendext->GetSubtype( ) ),
                vendext->GetExtensionData( ).size( )
            );
            PrintHexDump( vendext->GetExtensionData( ), 0, "      " );
        } else if ( auto const* modend { rec.As<Omf::OmfModendRecord>( ) }; modend ) {
            std::cout << std::format(
                "    Main Module : {}\n"
                "    Has Start   : {}\n",
                modend->IsMainModule( )     ? "true" : "false",
                modend->HasStartAddress( )  ? "true" : "false"
            );
            if ( modend->HasStartAddress( ) ) {
                auto const& start { modend->GetStartAddress( ) };
                if ( std::holds_alternative<Omf::FixupTargetSpec>( start ) ) {
                    std::cout << "    Start Kind  : Relocatable Entry Point\n";
                    PrintFixupTargetSpec( std::get<Omf::FixupTargetSpec>( start ), is32, ctx, "      " );
                } else if ( std::holds_alternative<Omf::OmfModendRecord::AbsoluteStartAddress>( start ) ) {
                    auto const& absAddr { std::get<Omf::OmfModendRecord::AbsoluteStartAddress>( start ) };
                    std::cout << std::format(
                        "    Start Kind  : Physical / Absolute Entry Point\n"
                        "      Frame     : {}\n"
                        "      Offset    : {}\n",
                        Hex16( absAddr.frameNumber ),
                        HexOffset( absAddr.offset, is32 )
                    );
                }
            }
        } else if ( auto const* regint { rec.As<Omf::OmfRegintRecord>( ) }; regint ) {
            auto const& entries { regint->GetEntries( ) };
            std::cout << std::format( "    Reg Entries : {}\n", entries.size( ) );
            for ( std::size_t i { }, limit { entries.size( ) }; i < limit; ++i ) {
                char const* regName { "Unknown" };
                switch ( entries[i].registerId ) {
                    case Omf::OmfRegintRecord::RegisterId::CsAndIp: regName = "CS:IP"; break;
                    case Omf::OmfRegintRecord::RegisterId::SsAndSp: regName = "SS:SP"; break;
                    case Omf::OmfRegintRecord::RegisterId::Ds:      regName = "DS";    break;
                    case Omf::OmfRegintRecord::RegisterId::Es:      regName = "ES";    break;
                }
                std::cout << std::format(
                    "      [{}] Register={}, Relocatable={}\n",
                    i,
                    regName,
                    entries[i].isRelocatable ? "true" : "false"
                );
                if ( entries[i].isRelocatable ) {
                    PrintFixupTargetSpec( entries[i].relocTarget, false, ctx, "            " );
                } else if ( entries[i].explicitOffset.has_value( ) ) {
                    std::cout << std::format(
                        "            Frame={}, Offset={}\n",
                        Hex16( entries[i].explicitFrame ),
                        Hex16( *entries[i].explicitOffset )
                    );
                } else {
                    std::cout << std::format( "            Frame={}\n", Hex16( entries[i].explicitFrame ) );
                }
            }
        } else if ( auto const* redata { rec.As<Omf::OmfRedataRecord>( ) }; redata ) {
            std::cout << "    Base Spec   :\n";
            PrintFixupTargetSpec( redata->GetBaseSpec( ), false, ctx, "      " );
            std::cout << std::format( "    Data Bytes  : {} byte(s)\n", redata->GetData( ).size( ) );
            PrintHexDump( redata->GetData( ), 0, "      " );
        } else if ( auto const* ridata { rec.As<Omf::OmfRidataRecord>( ) }; ridata ) {
            std::cout << "    Base Spec   :\n";
            PrintFixupTargetSpec( ridata->GetBaseSpec( ), false, ctx, "      " );
            PrintIteratedDataBlocks( ridata->GetBlocks( ), "      " );
        } else if ( auto const* ovldef { rec.As<Omf::OmfOvldefRecord>( ) }; ovldef ) {
            std::cout << std::format(
                "    Overlay Name: \"{}\"\n"
                "    Location    : Block={}, ByteOffset={}\n"
                "    Attributes  : {}, SharedIdx=#{}, AdjacentIdx=#{}\n",
                ovldef->GetOverlayName( ),
                ovldef->GetLocationBlock( ),
                Hex16( ovldef->GetLocationByteOffset( ) ),
                Hex8( ovldef->GetOverlayAttributes( ) ),
                ovldef->GetSharedOverlayIndex( ).value,
                ovldef->GetAdjacentOverlayIndex( ).value
            );
        } else if ( auto const* endrec { rec.As<Omf::OmfEndrecRecord>( ) }; endrec ) {
            std::cout << std::format(
                "    End Type    : {}\n",
                endrec->GetEndType( ) == Omf::OmfEndrecRecord::EndType::Overlay ? "Overlay" : "Block"
            );
        } else if ( auto const* blkdef { rec.As<Omf::OmfBlkdefRecord>( ) }; blkdef ) {
            std::cout << std::format(
                "    Block Name  : \"{}\"\n"
                "    Base Group  : {}\n"
                "    Base Segment: {}\n"
                "    Block Offset: {}\n"
                "    Block Length: {}\n"
                "    Proc Flags  : {}\n"
                "    Type Index  : #{}\n",
                blkdef->GetBlockName( ),
                ctx.FormatGroupIndex( blkdef->GetBaseGroup( ) ),
                ctx.FormatSegmentIndex( blkdef->GetBaseSegment( ) ),
                HexOffset( blkdef->GetBlockOffset( ), is32 ),
                HexOffset( blkdef->GetBlockLength( ), is32 ),
                Hex8( blkdef->GetProcedureFlags( ) ),
                blkdef->GetTypeIndex( ).value
            );
        } else if ( rec.As<Omf::OmfBlkendRecord>( ) ) {
            std::cout << "    <Block End>\n";
        } else if ( auto const* debsym { rec.As<Omf::OmfDebsymRecord>( ) }; debsym ) {
            auto const& syms { debsym->GetSymbols( ) };
            std::cout << std::format( "    Frame Method: {}\n", Hex8( debsym->GetFrameMethod( ) ) );
            PrintFixupTargetSpec( debsym->GetBaseSpec( ), false, ctx, "      " );
            std::cout << std::format( "    Symbol Count: {}\n", syms.size( ) );
            for ( std::size_t i { }, limit { syms.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{:3}] Offset={}, Type=#{}, Name=\"{}\"\n",
                    i,
                    Hex16( static_cast<std::uint16_t>( syms[i].offset ) ),
                    syms[i].typeIndex.value,
                    syms[i].name
                );
            }
        } else if ( auto const* pedata { rec.As<Omf::OmfPedataRecord>( ) }; pedata ) {
            std::cout << std::format(
                "    Frame Number: {}\n"
                "    Offset      : {}\n"
                "    Data Bytes  : {} byte(s)\n",
                Hex16( pedata->GetFrameNumber( ) ),
                Hex8( pedata->GetOffset( ) ),
                pedata->GetData( ).size( )
            );
            PrintHexDump( pedata->GetData( ), pedata->GetOffset( ), "      " );
        } else if ( auto const* pidata { rec.As<Omf::OmfPidataRecord>( ) }; pidata ) {
            std::cout << std::format(
                "    Frame Number: {}\n"
                "    Offset      : {}\n",
                Hex16( pidata->GetFrameNumber( ) ),
                Hex8( pidata->GetOffset( ) )
            );
            PrintIteratedDataBlocks( pidata->GetBlocks( ), "      " );
        } else if ( auto const* libhed { rec.As<Omf::OmfLibhedRecord>( ) }; libhed ) {
            std::cout << std::format(
                "    Module Count: {}\n"
                "    Dictionary  : Block={}, ByteOffset={}\n"
                "    Flags       : {}\n",
                libhed->GetModuleCount( ),
                libhed->GetDictionaryBlockNumber( ),
                Hex16( libhed->GetDictionaryByteOffset( ) ),
                Hex8( libhed->GetFlags( ) )
            );
        } else if ( auto const* libnam { rec.As<Omf::OmfLibnamRecord>( ) }; libnam ) {
            auto const& names { libnam->GetModuleNames( ) };
            std::cout << std::format( "    Module Names: {}\n", names.size( ) );
            for ( std::size_t i { }, limit { names.size( ) }; i < limit; ++i ) {
                std::cout << std::format( "      [{:3}] \"{}\"\n", i, names[i] );
            }
        } else if ( auto const* libloc { rec.As<Omf::OmfLiblocRecord>( ) }; libloc ) {
            auto const& locs { libloc->GetLocations( ) };
            std::cout << std::format( "    Locations   : {}\n", locs.size( ) );
            for ( std::size_t i { }, limit { locs.size( ) }; i < limit; ++i ) {
                std::cout << std::format(
                    "      [{:3}] Block={}, ByteOffset={}\n",
                    i,
                    locs[i].blockNumber,
                    Hex16( locs[i].byteOffset )
                );
            }
        } else if ( auto const* libdic { rec.As<Omf::OmfLibdicRecord>( ) }; libdic ) {
            auto const& groups { libdic->GetModulePublicGroups( ) };
            std::cout << std::format( "    Module Grps : {}\n", groups.size( ) );
            for ( std::size_t g { }, limit { groups.size( ) }; g < limit; ++g ) {
                std::cout << std::format( "      Module [{}] ({} symbol(s)):\n", g, groups[g].size( ) );
                for ( auto const& sym : groups[g] ) {
                    std::cout << std::format( "        - \"{}\"\n", sym );
                }
            }
        } else if ( auto const* mshdr { rec.As<Omf::OmfMsLibHeaderRecord>( ) }; mshdr ) {
            std::cout << std::format(
                "    Page Size   : {} bytes\n"
                "    Dict Offset : {} ({})\n"
                "    Dict Blocks : {} (512-byte blocks)\n"
                "    Flags       : {} ({})\n",
                mshdr->GetPageSize( ),
                Hex32( mshdr->GetDictionaryFileOffset( ) ),
                mshdr->GetDictionaryFileOffset( ),
                mshdr->GetDictionaryBlockCount( ),
                Hex8( mshdr->GetFlags( ) ),
                mshdr->IsCaseSensitive( ) ? "Case-Sensitive" : "Case-Insensitive"
            );
        } else if ( auto const* msend { rec.As<Omf::OmfMsLibEndRecord>( ) }; msend ) {
            std::cout << std::format( "    Padding     : {} byte(s)\n", msend->GetPaddingBytes( ).size( ) );
        } else if ( auto const* unk { rec.As<Omf::OmfUnknownRecord>( ) }; unk ) {
            std::cout << std::format( "    Unknown Rec : {} payload byte(s)\n", unk->GetRawPayload( ).size( ) );
            PrintHexDump( unk->GetRawPayload( ), 0, "      " );
        }

        if ( opts.showRawPayload && !rec.As<Omf::OmfUnknownRecord>( ) ) {
            std::cout << std::format( "    Raw Payload ({} bytes):\n", rec.GetRawPayload( ).size( ) );
            PrintHexDump( rec.GetRawPayload( ), 0, "      " );
        }
    }

    void PrintUsage( char const* progName ) {
        std::cerr << std::format(
            "Usage:\n"
            "  {} <input.obj|input.lib> [options]\n\n"
            "Options:\n"
            "  --raw            Hex-dump the raw interior payload bytes of every record\n"
            "  --expand-lidata  Expand repeating LIDATA blocks and hex-dump the expanded bytes\n"
            "  --summary        Print only the 1-line header summary for each record\n",
            progName
        );
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
            std::cerr << std::format( "Error: Unexpected argument '{}'\n\n", arg );
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

        std::cout << std::format(
            "=============================================================================\n"
            "File:        {}\n"
            "Records:     {}\n"
            "Modules:     {}\n"
            "=============================================================================\n\n",
            inputPath.string( ),
            records.size( ),
            modules.size( )
        );

        ModuleContext ctx;
        std::size_t   fileOffset  { };
        std::size_t   moduleIndex { };

        for ( std::size_t i { }, limit { records.size( ) }; i < limit; ++i ) {
            Omf::OmfRecord  const& rec          { *records[i]                   };
            Omf::RecordType const  type         { rec.GetRecordType( )          };
            std::size_t     const  payloadLen   { rec.GetRawPayload( ).size( )  };
            std::size_t     const  recordLen    { payloadLen + 1u               };
            std::size_t     const  totalBytes   {  recordLen + 3u               };
            std::uint8_t    const  origChecksum { rec.GetOriginalChecksum( )    };
            std::uint8_t    const  calcChecksum { rec.ComputeChecksum( )        };
            bool            const  isMsLib      { ( type == Omf::RecordType::MsLibHeader ) || ( type == Omf::RecordType::MsLibEnd ) };

            std::cout << std::format(
                "[{:4}] {}  {:<10} ({})  Len={:5} (Payload={:5})  ",
                i,
                Hex32( static_cast<std::uint32_t>( fileOffset ) ),
                GetRecordTypeName( type ),
                Hex8( static_cast<std::uint8_t>( type ) ),
                recordLen,
                payloadLen
            );

            if ( isMsLib ) {
                std::cout << std::format( "PadByte={}\n", Hex8( origChecksum ) );
            } else if ( origChecksum == 0u ) {
                std::cout << "Chksum=0x00 (Unchecked)\n";
            } else if ( rec.HasValidChecksum( ) ) {
                std::cout << std::format( "Chksum={} (OK)\n", Hex8( origChecksum ) );
            } else {
                std::cout << std::format( "Chksum={} (BAD: expected {})\n", Hex8( origChecksum ), Hex8( calcChecksum ) );
            }

            if ( !opts.summaryOnly ) {
                DumpRecordDetails( rec, ctx, opts );
                std::cout << "\n";
            }

            fileOffset += totalBytes;

            // Advance fileOffset past any .LIB inter-module page alignment padding
            if ( ( ( type == Omf::RecordType::Modend ) || ( type == Omf::RecordType::Modend32 ) ) &&
                 ( moduleIndex < modules.size( ) ) ) {
                auto const padding { modules[moduleIndex].GetTrailingLibraryPadding( ) };
                if ( !padding.empty( ) ) {
                    if ( !opts.summaryOnly ) {
                        std::cout << std::format(
                            "       {}  <.LIB Page Alignment Padding: {} byte(s)>\n\n",
                            Hex32( static_cast<std::uint32_t>( fileOffset ) ),
                            padding.size( )
                        );
                    }
                    fileOffset += padding.size( );
                }
                ++moduleIndex;
            }
        }

        auto const trailingDict { omfFile.GetTrailingLibraryDictionary( ) };
        if ( !trailingDict.empty( ) ) {
            std::cout << std::format(
                "Trailing MS-DOS .LIB Hashed Dictionary at {} ({} bytes, {} block(s))\n",
                Hex32( static_cast<std::uint32_t>( fileOffset ) ),
                trailingDict.size( ),
                trailingDict.size( ) / 512u
            );
            if ( opts.showRawPayload && !opts.summaryOnly ) {
                PrintHexDump( trailingDict, static_cast<std::uint32_t>( fileOffset ), "  " );
            }
        }

        return 0;
    } catch ( Omf::OmfParseException const& ex ) {
        std::cerr << std::format(
            "OMF Parse Error at offset 0x{:X} (record type 0x{:02X}): {}\n",
            ex.GetFileOffset( ),
            static_cast<unsigned>( ex.GetRecordType( ) ),
            ex.what( )
        );
        return 2;
    } catch ( std::exception const& ex ) {
        std::cerr << std::format( "Error: {}\n", ex.what( ) );
        return 3;
    }
}
