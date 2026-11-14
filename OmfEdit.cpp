#include "OmfFile.h"

#include <cstddef>

#include <exception>
#include <format>
#include <iostream>

int main( int argc, char* argv[] ) {
    if ( argc < 3 ) {
        std::cerr << std::format(
            "Usage:\n"
            "  {} <input.obj> <output.obj> [old_symbol new_symbol]\n",
            argv[0]
        );
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

        Omf::OmfFile objFile { Omf::OmfFile::LoadFromFile( argv[1], loadOpts ) };

        if ( argc >= 5 ) {
            std::size_t const count { objFile.RenameSymbol( argv[3], argv[4] ) };
            std::cout << std::format(
                "Renamed {} occurrence(s) of '{}' -> '{}'.\n",
                count,
                argv[3],
                argv[4]
            );
        }

        objFile.SaveToFile( argv[2] );
        std::cout << std::format(
            "Saved {} OMF record(s) to {}.\n",
            objFile.GetRecords( ).size( ),
            argv[2]
        );
        return 0;
    } catch ( Omf::OmfParseException const& ex ) {
        std::cerr << std::format(
            "OMF Parse Error at offset 0x{:X} (record 0x{:02X}): {}\n",
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
