#include "OmfFile.h"
#include <iostream>

int main( int argc, char* argv[] ) {
    if ( argc < 3 ) {
        std::cerr << "Usage:\n"
                  << "  omfedit <input.obj> <output.obj> [old_symbol new_symbol]\n";
        return 1;
    }

    try {
        Omf::OmfLoadOptions loadOpts{};
        loadOpts.checksumValidation = Omf::ChecksumValidation::WarnCallback;
        loadOpts.onChecksumWarning  = []( const Omf::OmfChecksumWarning& w ) {
            std::cerr << "Warning: Record #" << w.recordIndex
                      << " (type 0x" << std::hex << static_cast<int>( w.recordType )
                      << ") at offset 0x" << w.fileOffset << std::dec
                      << " has bad checksum (expected 0x" << std::hex
                      << static_cast<int>( w.expectedChecksum ) << ", got 0x"
                      << static_cast<int>( w.actualChecksum ) << std::dec << ")\n";
        };

        Omf::OmfFile objFile = Omf::OmfFile::LoadFromFile( argv[1], loadOpts );

        if ( argc >= 5 ) {
            std::size_t const count = objFile.RenameSymbol( argv[3], argv[4] );
            std::cout << "Renamed " << count << " occurrence(s) of '"
                      << argv[3] << "' -> '" << argv[4] << "'.\n";
        }

        objFile.SaveToFile( argv[2] );
        std::cout << "Saved " << objFile.GetRecords().size()
                  << " OMF record(s) to " << argv[2] << ".\n";
        return 0;
    } catch ( const Omf::OmfParseException& ex ) {
        std::cerr << "OMF Parse Error at offset 0x" << std::hex << ex.GetFileOffset()
                  << " (record 0x" << static_cast<int>( ex.GetRecordType() ) << "): "
                  << ex.what() << "\n";
        return 2;
    } catch ( const std::exception& ex ) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 3;
    }
}
