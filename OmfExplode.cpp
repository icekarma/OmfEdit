#include "OmfFile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

    // Extracts a clean, portable filename stem from a DOS/Windows/Unix THEADR module name
    // Examples:
    //   "C:\\PROJ\\SRC\\VIDEO\\VIDEO.C" -> "video"
    //   "src\\mthread\\mthread.asm"     -> "mthread"
    //   "MOUSE"                         -> "mouse"
    std::string SanitizeModuleStem(
        std::string_view rawModuleName,
        std::size_t      fallbackIndex,
        bool             preserveCase ) {
        // 1. Strip drive prefix (e.g., "C:") and directory separators ('\' or '/')
        std::size_t const lastSlash = rawModuleName.find_last_of( "\\/:" );
        std::string_view baseName = ( lastSlash == std::string_view::npos )
            ? rawModuleName
            : rawModuleName.substr( lastSlash + 1u );

        // 2. Strip trailing extension (e.g., ".c", ".cpp", ".asm", ".obj")
        std::size_t const lastDot = baseName.find_last_of( '.' );
        if ( lastDot != std::string_view::npos && lastDot > 0 ) {
            baseName = baseName.substr( 0, lastDot );
        }

        // 3. Filter out illegal filename characters and apply case rule
        std::string cleanStem;
        cleanStem.reserve( baseName.size() );
        for ( unsigned char const ch : baseName ) {
            if ( std::isalnum( ch ) || ch == '_' || ch == '-' ) {
                cleanStem.push_back( static_cast<char>(
                    preserveCase ? ch : std::tolower( ch ) ) );
            } else if ( ch == ' ' || ch == '.' ) {
                cleanStem.push_back( '_' );
            }
        }

        if ( cleanStem.empty() ) {
            std::ostringstream oss;
            oss << "module_" << std::setw( 3 ) << std::setfill( '0' ) << fallbackIndex;
            return oss.str();
        }

        return cleanStem;
    }

    void PrintUsage( const char* progName ) {
        std::cerr
            << "Usage:\n"
            << "  " << progName << " <input.lib> [output_dir] [--preserve-case] [--list-only]\n\n"
            << "Options:\n"
            << "  output_dir       Directory to write extracted .obj files (default: current directory)\n"
            << "  --preserve-case  Keep original upper/lower case from THEADR instead of lowercasing\n"
            << "  --list-only      List modules contained in the .lib without writing .obj files\n";
    }

} // anonymous namespace

int main( int argc, char* argv[] ) {
    if ( argc < 2 ) {
        PrintUsage( argv[0] );
        return 1;
    }

    std::filesystem::path inputLibPath;
    std::filesystem::path outputDir = ".";
    bool preserveCase = false;
    bool listOnly     = false;

    std::size_t positionalCount = 0;
    for ( int i = 1; i < argc; ++i ) {
        std::string_view const arg( argv[i] );
        if ( arg == "--preserve-case" ) {
            preserveCase = true;
        } else if ( arg == "--list-only" ) {
            listOnly = true;
        } else if ( arg == "-h" || arg == "--help" || arg == "/?" ) {
            PrintUsage( argv[0] );
            return 0;
        } else if ( positionalCount == 0 ) {
            inputLibPath = arg;
            ++positionalCount;
        } else if ( positionalCount == 1 ) {
            outputDir = arg;
            ++positionalCount;
        } else {
            std::cerr << "Error: Unexpected argument '" << arg << "'\n\n";
            PrintUsage( argv[0] );
            return 1;
        }
    }

    if ( inputLibPath.empty() ) {
        PrintUsage( argv[0] );
        return 1;
    }

    try {
        Omf::OmfLoadOptions loadOptions{};
        loadOptions.checksumValidation = Omf::ChecksumValidation::WarnCallback;
        loadOptions.onChecksumWarning  = []( const Omf::OmfChecksumWarning& warning ) {
            std::cerr << "Warning: Record #" << warning.recordIndex
                      << " (type 0x" << std::hex << static_cast<int>( warning.recordType )
                      << ") at file offset 0x" << warning.fileOffset << std::dec
                      << " has invalid checksum (expected 0x" << std::hex
                      << static_cast<int>( warning.expectedChecksum ) << ", got 0x"
                      << static_cast<int>( warning.actualChecksum ) << std::dec << ")\n";
        };

        Omf::OmfFile const libFile = Omf::OmfFile::LoadFromFile( inputLibPath, loadOptions );
        const auto& modules        = libFile.GetModules();

        if ( modules.empty() ) {
            std::cerr << "No object modules found in '" << inputLibPath.string() << "'.\n";
            return 2;
        }

        // Optional: Print library header info if this is an MS-DOS .LIB (0xF0)
        if ( !libFile.GetRecords().empty() ) {
            if ( const auto* msHdr = libFile.GetRecords().front()->As<Omf::OmfMsLibHeaderRecord>() ) {
                std::cout << "Library: " << inputLibPath.string()
                          << " (Page Size: " << msHdr->GetPageSize() << " bytes, "
                          << "Modules: " << modules.size() << ")\n";
            }
        }

        if ( !listOnly ) {
            std::filesystem::create_directories( outputDir );
        }

        std::unordered_map<std::string, std::size_t> usedStems;
        std::size_t moduleIndex = 1;

        for ( const Omf::OmfModule& module : modules ) {
            std::string_view const rawName = module.GetModuleName().value_or( "" );
            std::string const baseStem     = SanitizeModuleStem( rawName, moduleIndex, preserveCase );

            // Disambiguate if multiple modules in the archive share the same stem
            std::string uniqueStem = baseStem;
            std::size_t const count = ++usedStems[baseStem];
            if ( count > 1 ) {
                uniqueStem += "_" + std::to_string( count );
            }

            std::filesystem::path const outObjPath = outputDir / ( uniqueStem + ".obj" );
            std::vector<std::byte> const objBytes  = module.SerializeToMemory();

            std::cout << "  [" << std::setw( 3 ) << moduleIndex << "/"
                      << std::setw( 3 ) << modules.size() << "] "
                      << std::left << std::setw( 24 ) << ( rawName.empty() ? "<unnamed>" : rawName )
                      << " -> " << std::setw( 20 ) << outObjPath.string()
                      << std::right << " (" << objBytes.size() << " bytes, "
                      << module.GetRecords().size() << " records)\n";

            if ( !listOnly ) {
                module.SaveToFile( outObjPath );
            }

            ++moduleIndex;
        }

        if ( !listOnly ) {
            std::cout << "Successfully extracted " << modules.size()
                      << " object file(s) to '" << outputDir.string() << "'.\n";
        }

        return 0;

    } catch ( const Omf::OmfParseException& ex ) {
        std::cerr << "OMF Parse Error at offset 0x" << std::hex << ex.GetFileOffset()
                  << " (record type 0x" << static_cast<int>( ex.GetRecordType() ) << std::dec << "): "
                  << ex.what() << "\n";
        return 3;
    } catch ( const std::exception& ex ) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 4;
    }
}
