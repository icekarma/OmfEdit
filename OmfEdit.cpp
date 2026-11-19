#include "OmfFile.h"
#include "OmfDemangle.h"

#include <cstddef>

#include <exception>
#include <format>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

    void PrintUsage( char const* const progName ) {
        std::cerr << std::format(
            "Usage:\n"
            "  {} [options] <input.obj> <output.obj> [old_symbol new_symbol]\n\n"
            "Options:\n"
            "  --demangle[=msc|bc|ow]  Demangle C++ symbol names in status output (default: auto-detect when flag is given)\n",
            progName
        );
    }

} // anonymous namespace

int main( int argc, char* argv[] ) {
    if ( argc < 3 ) {
        PrintUsage( argv[0] );
        return 1;
    }

    Omf::DemangleStyle            demangleStyle { Omf::DemangleStyle::None };
    std::vector<std::string_view> positional;

    for ( int i { 1 }; i < argc; ++i ) {
        std::string_view const arg { argv[i] };
        if ( arg == "--" ) {
            for ( int j { i + 1 }; j < argc; ++j ) {
                positional.emplace_back( argv[j] );
            }
            break;
        } else if ( auto const res { Omf::ParseDemangleOption( arg, demangleStyle ) }; res == Omf::DemangleArgResult::Parsed ) {
            continue;
        } else if ( res == Omf::DemangleArgResult::InvalidValue ) {
            std::cerr << std::format( "Error: Invalid demangle style in '{}' (expected msc, bc, or ow)\n\n", arg );
            PrintUsage( argv[0] );
            return 1;
        } else if ( ( arg == "--help" ) || ( arg == "-h" ) || ( arg == "/h" ) || ( arg == "/?" ) ) {
            PrintUsage( argv[0] );
            return 0;
        } else if ( arg.starts_with( '-' ) ) {
            std::cerr << std::format( "Error: Unknown option '{}'\n\n", arg );
            PrintUsage( argv[0] );
            return 1;
        } else {
            positional.push_back( arg );
        }
    }

    if ( ( positional.size( ) != 2u ) && ( positional.size( ) != 4u ) ) {
        PrintUsage( argv[0] );
        return 1;
    }

    try {
        Omf::OmfLoadOptions loadOpts { };
        loadOpts.checksumValidation = Omf::ChecksumValidation::WarnCallback;
        loadOpts.onChecksumWarning  = []( Omf::OmfChecksumWarning const& w ) {
            std::cerr << std::format(
                "Warning: Record #{} (type 0x{:02X}) at offset 0x{:X} has bad checksum (expected 0x{:02X}, got 0x{:02X})\n",
                w.recordIndex,
                static_cast<unsigned>( w.recordType ),
                w.fileOffset,
                static_cast<unsigned>( w.expectedChecksum ),
                static_cast<unsigned>( w.actualChecksum )
            );
        };

        Omf::OmfFile objFile { Omf::OmfFile::LoadFromFile( positional[0], loadOpts ) };

        if ( positional.size( ) == 4u ) {
            std::string_view const oldSym { positional[2] };
            std::string_view const newSym { positional[3] };
            std::size_t const      count  { objFile.RenameSymbol( oldSym, newSym ) };
            std::cout << std::format(
                "Renamed {} occurrence(s) of '{}' -> '{}'.\n",
                count,
                Omf::DemangleSymbol( oldSym, demangleStyle ),
                Omf::DemangleSymbol( newSym, demangleStyle )
            );
        }

        objFile.SaveToFile( positional[1] );
        std::cout << std::format(
            "Saved {} OMF record(s) to {}.\n",
            objFile.GetRecords( ).size( ),
            positional[1]
        );
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
        std::cerr << std::format( "Error: {}\n", ex.what( ) );
        return 3;
    }
}
