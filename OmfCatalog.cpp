#include "OmfFile.h"

#include <cstdint>

#include <algorithm>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

    enum class DefinitionKind:
        std::uint8_t
    {
        Pubdef,
        Lpubdef,
        Comdat,
        LocalComdat,
        Comdef,
        Lcomdef,
        Alias
    };

    constexpr std::string_view ToString( DefinitionKind const kind ) noexcept {
        switch ( kind ) {
            case DefinitionKind::Pubdef:      return "PUBDEF";
            case DefinitionKind::Lpubdef:     return "LPUBDEF";
            case DefinitionKind::Comdat:      return "COMDAT";
            case DefinitionKind::LocalComdat: return "COMDAT(L)";
            case DefinitionKind::Comdef:      return "COMDEF";
            case DefinitionKind::Lcomdef:     return "LCOMDEF";
            case DefinitionKind::Alias:       return "ALIAS";
        }
        return "UNKNOWN";
    }

    struct SymbolDefinition {
        std::string    moduleName;
        DefinitionKind kind { DefinitionKind::Pubdef };

        auto operator<=>( SymbolDefinition const& ) const = default;
    };

    struct SymbolEntry {
        std::set<SymbolDefinition> definitions;
        std::set<std::string>      referencedBy;
    };

    struct ExternalSlot {
        std::string name;
        bool        isLocal { };
    };

    struct FixupThreadState {
        bool          isValid { };
        std::uint8_t  method  { };
        std::uint16_t index   { };
    };

    void RecordFixupTargetUsage(
        Omf::FixupTargetSpec const&      spec,
        FixupThreadState const           (&frameThreads)[4],
        FixupThreadState const           (&targetThreads)[4],
        std::unordered_set<std::uint16_t>& usedExternalIndices
    ) {
        // 1. Check Frame specification (F2 = ExternalIndex)
        if ( !spec.frameUsesThread ) {
            if ( ( spec.frameMethodOrThread == static_cast<std::uint8_t>( Omf::FrameMethod::ExternalIndex ) ) && spec.frameDatum.has_value( ) ) {
                usedExternalIndices.insert( *spec.frameDatum );
            }
        } else {
            FixupThreadState const& thread { frameThreads[spec.frameMethodOrThread & 0x03u] };
            if ( thread.isValid && ( thread.method == static_cast<std::uint8_t>( Omf::FrameMethod::ExternalIndex ) ) ) {
                usedExternalIndices.insert( thread.index );
            }
        }

        // 2. Check Target specification (T2 = ExternalDisp [2], T6 = ExternalNoDisp [6])
        if ( !spec.targetUsesThread ) {
            if ( ( ( spec.targetMethodOrThread & 0x03u ) == static_cast<std::uint8_t>( Omf::TargetMethod::ExternalDisp ) ) && spec.targetDatum.has_value( ) ) {
                usedExternalIndices.insert( *spec.targetDatum );
            }
        } else {
            FixupThreadState const& thread { targetThreads[spec.targetMethodOrThread & 0x03u] };
            if ( thread.isValid && ( ( thread.method & 0x03u ) == static_cast<std::uint8_t>( Omf::TargetMethod::ExternalDisp ) ) ) {
                usedExternalIndices.insert( thread.index );
            }
        }
    }

    std::string FormatModuleDisplayName(
        Omf::OmfModule const& module,
        std::size_t const     moduleIndex,
        std::string_view      fallbackFileName
    ) {
        if ( auto const rawName { module.GetModuleName( ) }; rawName && !rawName->empty( ) ) {
            return std::string{ *rawName };
        }
        if ( !fallbackFileName.empty( ) ) {
            return std::format( "{}#{}", fallbackFileName, moduleIndex );
        }
        return std::format( "<module_{}>", moduleIndex );
    }

    void ScanModule(
        Omf::OmfModule const&               module,
        std::string const&                  moduleName,
        bool const                          excludeLocals,
        bool const                          showUnreferencedExterns,
        std::map<std::string, SymbolEntry>& catalog
    ) {
        // Pre-scan LNAMES / LLNAMES and local COMDATs so we know whether a 1-based NameIndex
        // referenced by CEXTDEF points to a module-local name.
        std::unordered_set<std::uint16_t> localNameIndices;
        std::uint16_t                     currentNameIndex { 1 };
        for ( Omf::OmfLnamesRecord const* lnames : module.GetRecordsOfType<Omf::OmfLnamesRecord>( ) ) {
            auto const& names { lnames->GetNames( ) };
            for ( std::size_t i { }; i < names.size( ); ++i, ++currentNameIndex ) {
                if ( lnames->IsLocal( ) ) {
                    localNameIndices.insert( currentNameIndex );
                }
            }
        }
        for ( Omf::OmfComdatRecord const* comdat : module.GetRecordsOfType<Omf::OmfComdatRecord>( ) ) {
            if ( comdat->IsLocalSymbol( ) && comdat->GetPublicNameIndex( ).IsValid( ) ) {
                localNameIndices.insert( comdat->GetPublicNameIndex( ).value );
            }
        }

        // OMF 1-based ExternalIndex table (populated sequentially by EXTDEF, LEXTDEF, CEXTDEF, COMDEF, LCOMDEF)
        std::vector<ExternalSlot>         externalSlots;
        std::unordered_set<std::uint16_t> usedExternalIndices;
        FixupThreadState                  frameThreads[4]  { };
        FixupThreadState                  targetThreads[4] { };

        for ( auto const& recordPtr : module.GetRecords( ) ) {
            // 1. PUBDEF / LPUBDEF (0x90, 0x91, 0xB6, 0xB7)
            if ( auto const* pub { recordPtr->As<Omf::OmfPubdefRecord>( ) } ) {
                if ( pub->IsLocal( ) && excludeLocals ) {
                    continue;
                }
                DefinitionKind const kind { pub->IsLocal( ) ? DefinitionKind::Lpubdef : DefinitionKind::Pubdef };
                for ( auto const& sym : pub->GetSymbols( ) ) {
                    catalog[sym.name].definitions.insert( SymbolDefinition{ moduleName, kind } );
                }
            }
            // 2. EXTDEF / LEXTDEF (0x8C, 0xB4, 0xB5)
            else if ( auto const* ext { recordPtr->As<Omf::OmfExtdefRecord>( ) } ) {
                for ( auto const& sym : ext->GetSymbols( ) ) {
                    externalSlots.push_back( ExternalSlot{ sym.name, ext->IsLocal( ) } );
                }
            }
            // 3. CEXTDEF (0xBC)
            else if ( auto const* cext { recordPtr->As<Omf::OmfCextdefRecord>( ) } ) {
                for ( auto const& ext : cext->GetExternals( ) ) {
                    std::string const resolvedName { module.ResolveName( ext.logicalNameIndex ).value_or( "" ) };
                    bool const isLocal { localNameIndices.contains( ext.logicalNameIndex.value ) };
                    externalSlots.push_back( ExternalSlot{ resolvedName, isLocal } );
                }
            }
            // 4. COMDAT / COMDAT32 (0xC2, 0xC3)
            else if ( auto const* comdat { recordPtr->As<Omf::OmfComdatRecord>( ) } ) {
                if ( comdat->IsLocalSymbol( ) && excludeLocals ) {
                    continue;
                }
                if ( auto const resolved { module.ResolveName( comdat->GetPublicNameIndex( ) ) } ) {
                    DefinitionKind const kind { comdat->IsLocalSymbol( ) ? DefinitionKind::LocalComdat : DefinitionKind::Comdat };
                    catalog[std::string{ *resolved }].definitions.insert( SymbolDefinition{ moduleName, kind } );
                }
            }
            // 5. COMDEF / LCOMDEF (0xB0, 0xB8)
            else if ( auto const* comdef { recordPtr->As<Omf::OmfComdefRecord>( ) } ) {
                DefinitionKind const kind { comdef->IsLocal( ) ? DefinitionKind::Lcomdef : DefinitionKind::Comdef };
                for ( auto const& sym : comdef->GetSymbols( ) ) {
                    externalSlots.push_back( ExternalSlot{ sym.name, comdef->IsLocal( ) } );
                    if ( !comdef->IsLocal( ) || !excludeLocals ) {
                        catalog[sym.name].definitions.insert( SymbolDefinition{ moduleName, kind } );
                    }
                }
            }
            // 6. ALIAS (0xC6)
            else if ( auto const* alias { recordPtr->As<Omf::OmfAliasRecord>( ) } ) {
                for ( auto const& item : alias->GetAliases( ) ) {
                    catalog[item.aliasName].definitions.insert( SymbolDefinition{ moduleName, DefinitionKind::Alias } );
                    catalog[item.substituteName].referencedBy.insert( moduleName );
                }
            }
            // 7. FIXUPP / FIXUPP32 (0x9C, 0x9D)
            else if ( auto const* fixupp { recordPtr->As<Omf::OmfFixuppRecord>( ) } ) {
                for ( auto const& sub : fixupp->GetSubrecords( ) ) {
                    if ( std::holds_alternative<Omf::OmfFixuppRecord::ThreadSubrecord>( sub ) ) {
                        auto const& trd { std::get<Omf::OmfFixuppRecord::ThreadSubrecord>( sub ) };
                        FixupThreadState& slot { trd.isFrameThread ? frameThreads[trd.threadNumber & 0x03u] : targetThreads[trd.threadNumber & 0x03u] };
                        slot.isValid = true;
                        slot.method  = trd.method;
                        slot.index   = trd.index;
                    } else {
                        auto const& fix { std::get<Omf::OmfFixuppRecord::FixupSubrecord>( sub ) };
                        RecordFixupTargetUsage( fix.target, frameThreads, targetThreads, usedExternalIndices );
                    }
                }
            }
            // 8. Other records containing a FixupTargetSpec (MODEND, REGINT, REDATA, RIDATA, DEBSYM)
            else if ( auto const* modend { recordPtr->As<Omf::OmfModendRecord>( ) } ) {
                if ( std::holds_alternative<Omf::FixupTargetSpec>( modend->GetStartAddress( ) ) ) {
                    RecordFixupTargetUsage( std::get<Omf::FixupTargetSpec>( modend->GetStartAddress( ) ), frameThreads, targetThreads, usedExternalIndices );
                }
            } else if ( auto const* regint { recordPtr->As<Omf::OmfRegintRecord>( ) } ) {
                for ( auto const& entry : regint->GetEntries( ) ) {
                    if ( entry.isRelocatable ) {
                        RecordFixupTargetUsage( entry.relocTarget, frameThreads, targetThreads, usedExternalIndices );
                    }
                }
            } else if ( auto const* redata { recordPtr->As<Omf::OmfRedataRecord>( ) } ) {
                RecordFixupTargetUsage( redata->GetBaseSpec( ), frameThreads, targetThreads, usedExternalIndices );
            } else if ( auto const* ridata { recordPtr->As<Omf::OmfRidataRecord>( ) } ) {
                RecordFixupTargetUsage( ridata->GetBaseSpec( ), frameThreads, targetThreads, usedExternalIndices );
            } else if ( auto const* debsym { recordPtr->As<Omf::OmfDebsymRecord>( ) } ) {
                RecordFixupTargetUsage( debsym->GetBaseSpec( ), frameThreads, targetThreads, usedExternalIndices );
            }
        }

        // Record external references according to whether they were targeted by a fixup
        // (or unconditionally if --show-unreferenced-externs was specified)
        for ( std::size_t i { }; i < externalSlots.size( ); ++i ) {
            ExternalSlot const& slot     { externalSlots[i] };
            std::uint16_t const extIndex { static_cast<std::uint16_t>( i + 1u ) };

            if ( slot.name.empty( ) || ( slot.isLocal && excludeLocals ) ) {
                continue;
            }

            if ( showUnreferencedExterns || usedExternalIndices.contains( extIndex ) ) {
                catalog[slot.name].referencedBy.insert( moduleName );
            }
        }
    }

    std::string FormatDefinitions( std::set<SymbolDefinition> const& defs ) {
        std::string result;
        for ( auto const& def : defs ) {
            if ( !result.empty( ) ) {
                result += ", ";
            }
            if ( ( def.kind == DefinitionKind::Pubdef ) || ( def.kind == DefinitionKind::Comdat ) ) {
                result += def.moduleName;
            } else {
                result += std::format( "{} [{}]", def.moduleName, ToString( def.kind ) );
            }
        }
        return result;
    }

    std::string FormatReferences( std::set<std::string> const& refs ) {
        if ( refs.empty( ) ) {
            return "<none>";
        }
        std::string result;
        for ( auto const& mod : refs ) {
            if ( !result.empty( ) ) {
                result += ", ";
            }
            result += mod;
        }
        return result;
    }

    void PrintUsage( char const* const progName ) {
        std::cerr <<
            "Usage:\n"
            "  " << progName << " [options] <input.lib | input.obj> [...]\n\n"
            "Options:\n"
            "  --exclude-locals             Exclude module-local symbols (LPUBDEF, LEXTDEF, LCOMDEF, local COMDAT)\n"
            "  --show-unreferenced-externs  Include EXTDEF/LEXTDEF/CEXTDEF symbols not referenced by any FIXUPP\n";
    }

} // anonymous namespace

int main( int argc, char* argv[] ) {
    if ( argc < 2 ) {
        PrintUsage( argv[0] );
        return 1;
    }

    bool                               excludeLocals           { };
    bool                               showUnreferencedExterns { };
    std::vector<std::filesystem::path> inputFiles;

    for ( int i { 1 }; i < argc; ++i ) {
        std::string_view const arg{ argv[i] };
        if ( arg == "--exclude-locals" ) {
            excludeLocals = true;
        } else if ( arg == "--show-unreferenced-externs" ) {
            showUnreferencedExterns = true;
        } else if ( ( arg == "-h" ) || ( arg == "--help" ) || ( arg == "/?" ) ) {
            PrintUsage( argv[0] );
            return 0;
        } else {
            inputFiles.emplace_back( arg );
        }
    }

    if ( inputFiles.empty( ) ) {
        PrintUsage( argv[0] );
        return 1;
    }

    try {
        std::map<std::string, SymbolEntry> catalog;
        std::size_t                        totalModules { };

        Omf::OmfLoadOptions loadOptions{ };
        loadOptions.checksumValidation = Omf::ChecksumValidation::WarnCallback;
        loadOptions.onChecksumWarning  = []( Omf::OmfChecksumWarning const& w ) {
            std::cerr << std::format(
                "Warning: Record #{} (type 0x{:02X}) at offset 0x{:X} has invalid checksum (expected 0x{:02X}, got 0x{:02X})\n",
                w.recordIndex,
                static_cast<unsigned>( w.recordType ),
                w.fileOffset,
                w.expectedChecksum,
                w.actualChecksum
            );
        };

        for ( auto const& filePath : inputFiles ) {
            Omf::OmfFile const omfFile { Omf::OmfFile::LoadFromFile( filePath, loadOptions ) };
            auto const&        modules { omfFile.GetModules( ) };

            std::size_t modIndex { 1 };
            for ( Omf::OmfModule const& module : modules ) {
                std::string const modName { FormatModuleDisplayName( module, modIndex, filePath.filename( ).string( ) ) };
                ScanModule( module, modName, excludeLocals, showUnreferencedExterns, catalog );
                ++modIndex;
                ++totalModules;
            }
        }

        // Partition into Defined Symbols vs. Unresolved External Symbols,
        // filtering out self-references from each symbol's referencedBy set.
        struct DefinedRow {
            std::string symbol;
            std::string definedIn;
            std::string referencedBy;
        };

        struct UnresolvedRow {
            std::string symbol;
            std::string referencedBy;
        };

        std::vector<DefinedRow>    definedRows;
        std::vector<UnresolvedRow> unresolvedRows;

        for ( auto& [name, entry] : catalog ) {
            for ( auto const& def : entry.definitions ) {
                entry.referencedBy.erase( def.moduleName );
            }

            if ( !entry.definitions.empty( ) ) {
                definedRows.push_back( DefinedRow { name, FormatDefinitions( entry.definitions ), FormatReferences( entry.referencedBy ) } );
            } else if ( !entry.referencedBy.empty( ) ) {
                unresolvedRows.push_back( UnresolvedRow { name, FormatReferences( entry.referencedBy ) } );
            }
        }

        std::cout << std::format(
            "Scanned {} module(s) across {} file(s); found {} defined symbol(s) and {} unresolved external(s).\n",
            totalModules,
            inputFiles.size( ),
            definedRows.size( ),
            unresolvedRows.size( )
        );

        // 1. Render Defined Symbols table
        if ( !definedRows.empty( ) ) {
            constexpr std::string_view kSymHeader { "Symbol"        };
            constexpr std::string_view kDefHeader { "Defined In"    };
            constexpr std::string_view kRefHeader { "Referenced By" };

            std::size_t symWidth { kSymHeader.size( ) };
            std::size_t defWidth { kDefHeader.size( ) };
            std::size_t refWidth { kRefHeader.size( ) };

            for ( auto const& row : definedRows ) {
                symWidth = std::max( symWidth, row.symbol.size( ) );
                defWidth = std::max( defWidth, row.definedIn.size( ) );
                refWidth = std::max( refWidth, row.referencedBy.size( ) );
            }

            std::cout << "\nDefined Symbols (" << definedRows.size( ) << "):\n\n"
                      << std::left
                      << std::setw( static_cast<int>( symWidth ) ) << kSymHeader << "  "
                      << std::setw( static_cast<int>( defWidth ) ) << kDefHeader << "  "
                      << kRefHeader << "\n"
                      << std::string( symWidth, '-' ) << "  "
                      << std::string( defWidth, '-' ) << "  "
                      << std::string( refWidth, '-' ) << "\n";

            for ( auto const& row : definedRows ) {
                std::cout << std::left
                          << std::setw( static_cast<int>( symWidth ) ) << row.symbol    << "  "
                          << std::setw( static_cast<int>( defWidth ) ) << row.definedIn << "  "
                          << row.referencedBy << "\n";
            }
        }

        // 2. Render Unresolved External Symbols table
        if ( !unresolvedRows.empty( ) ) {
            constexpr std::string_view kSymHeader { "Symbol"        };
            constexpr std::string_view kRefHeader { "Referenced By" };

            std::size_t symWidth { kSymHeader.size( ) };
            std::size_t refWidth { kRefHeader.size( ) };

            for ( auto const& row : unresolvedRows ) {
                symWidth = std::max( symWidth, row.symbol.size( ) );
                refWidth = std::max( refWidth, row.referencedBy.size( ) );
            }

            std::cout << "\nUnresolved External Symbols (" << unresolvedRows.size( ) << "):\n\n"
                      << std::left
                      << std::setw( static_cast<int>( symWidth ) ) << kSymHeader << "  "
                      << kRefHeader << "\n"
                      << std::string( symWidth, '-' ) << "  "
                      << std::string( refWidth, '-' ) << "\n";

            for ( auto const& row : unresolvedRows ) {
                std::cout << std::left
                          << std::setw( static_cast<int>( symWidth ) ) << row.symbol << "  "
                          << row.referencedBy << "\n";
            }
        }

        return 0;
    } catch ( Omf::OmfParseException const& ex ) {
        std::cerr << std::format(
            "OMF parse error at offset 0x{:X} (record 0x{:02X}): {}\n",
            ex.GetFileOffset( ),
            static_cast<unsigned>( ex.GetRecordType( ) ),
            ex.what( )
        );
        return 2;
    } catch ( std::exception const& ex ) {
        std::cerr << "Error: " << ex.what( ) << "\n";
        return 3;
    }
}
