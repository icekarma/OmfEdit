#include "OmfFile.h"

#include <cctype>
#include <cstddef>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

    // Extracts a clean, portable filename stem from a DOS/Windows/Unix THEADR module name
    // Examples:
    //   "C:\\PROJ\\SRC\\VIDEO\\VIDEO.C" -> "video"
    //   "src\\mthread\\mthread.asm"     -> "mthread"
    //   "MOUSE"                         -> "mouse"
    std::string SanitizeModuleStem( std::string_view const rawModuleName, std::size_t const fallbackIndex, bool const preserveCase ) {
        // 1. Strip drive prefix (e.g., "C:") and directory separators ('\' or '/')
        std::size_t const lastSlash { rawModuleName.find_last_of( "\\/:" ) };
        std::string_view  baseName  { ( lastSlash == std::string_view::npos ) ? rawModuleName : rawModuleName.substr( lastSlash + 1u ) };

        // 2. Strip trailing extension (e.g., ".c", ".cpp", ".asm", ".obj")
        std::size_t const lastDot { baseName.find_last_of( '.' ) };
        if ( ( lastDot != std::string_view::npos ) && ( lastDot > 0 ) ) {
            baseName = baseName.substr( 0, lastDot );
        }

        // 3. Filter out illegal filename characters and apply case rule
        std::string cleanStem;
        cleanStem.reserve( baseName.size( ) );
        for ( unsigned char const ch : baseName ) {
            if ( std::isalnum( ch ) || ( ch == '_' ) || ( ch == '-' ) ) {
                cleanStem.push_back( static_cast<char>( preserveCase ? ch : std::tolower( ch ) ) );
            } else if ( ( ch == ' ' ) || ( ch == '.' ) ) {
                cleanStem.push_back( '_' );
            }
        }

        if ( cleanStem.empty( ) ) {
            return std::format( "module_{:03}", fallbackIndex );
        }

        return cleanStem;
    }

    void PrintUsage( char const* progName ) {
        std::cerr << std::format(
            "Usage:\n"
            "  {} <input.lib> [output_dir] [--preserve-case] [--list-only]\n\n"
            "Options:\n"
            "  output_dir       Directory to write extracted .obj files (default: current directory)\n"
            "  --preserve-case  Keep original upper/lower case from THEADR instead of lowercasing\n"
            "  --list-only      List modules contained in the .lib without writing .obj files\n",
            progName
        );
    }

} // anonymous namespace

int main( int argc, char* argv[] ) {
    if ( argc < 2 ) {
        PrintUsage( argv[0] );
        return 1;
    }

    std::filesystem::path inputLibPath;
    std::filesystem::path outputDir       { "." };
    bool                  preserveCase    { };
    bool                  listOnly        { };
    std::size_t           positionalCount { };

    for ( int i { 1 }; i < argc; ++i ) {
        std::string_view const arg { argv[i] };
        if ( arg == "--preserve-case" ) {
            preserveCase = true;
        } else if ( arg == "--list-only" ) {
            listOnly = true;
        } else if ( ( arg == "--help" ) || ( arg == "-h" ) || ( arg == "/h" ) || ( arg == "/?" ) ) {
            PrintUsage( argv[0] );
            return 0;
        } else if ( positionalCount == 0 ) {
            inputLibPath = arg;
            ++positionalCount;
        } else if ( positionalCount == 1 ) {
            outputDir = arg;
            ++positionalCount;
        } else {
            std::cerr << std::format( "Error: Unexpected argument '{}'\n\n", arg );
            PrintUsage( argv[0] );
            return 1;
        }
    }

    if ( inputLibPath.empty( ) ) {
        PrintUsage( argv[0] );
        return 1;
    }

    try {
        Omf::OmfLoadOptions loadOptions { };
        loadOptions.checksumValidation = Omf::ChecksumValidation::WarnCallback;
        loadOptions.onChecksumWarning  = []( Omf::OmfChecksumWarning const& warning ) {
            std::cerr << std::format(
                "Warning: Record #{} (type 0x{:02X}) at file offset 0x{:X} has invalid checksum (expected 0x{:02X}, got 0x{:02X})\n",
                warning.recordIndex,
                static_cast<unsigned>( warning.recordType ),
                warning.fileOffset,
                static_cast<unsigned>( warning.expectedChecksum ),
                static_cast<unsigned>( warning.actualChecksum )
            );
        };

        Omf::OmfFile const libFile { Omf::OmfFile::LoadFromFile( inputLibPath, loadOptions ) };
        auto const&        modules { libFile.GetModules( ) };

        if ( modules.empty( ) ) {
            std::cerr << std::format( "No object modules found in '{}'.\n", inputLibPath.string( ) );
            return 2;
        }

        // Optional: Print library header info if this is an MS-DOS .LIB (0xF0)
        if ( !libFile.GetRecords( ).empty( ) ) {
            if ( auto const* msHdr { libFile.GetRecords( ).front( )->As<Omf::OmfMsLibHeaderRecord>( ) }; msHdr ) {
                std::cout << std::format( "Library: {} (Page Size: {} bytes, Modules: {})\n", inputLibPath.string( ), msHdr->GetPageSize( ), modules.size( ) );
            }
        }

        if ( !listOnly ) {
            std::filesystem::create_directories( outputDir );
        }

        std::unordered_map<std::string, std::size_t> usedStems;
        std::size_t                                  moduleIndex { 1 };

        for ( Omf::OmfModule const& module : modules ) {
            std::string_view const rawName  { module.GetModuleName( ).value_or( "" )                   };
            std::string      const baseStem { SanitizeModuleStem( rawName, moduleIndex, preserveCase ) };

            // Disambiguate if multiple modules in the archive share the same stem
            std::string       uniqueStem { baseStem               };
            std::size_t const count      { ++usedStems[baseStem]  };
            if ( count > 1 ) {
                uniqueStem += std::format( "_{}", count );
            }

            std::filesystem::path const  outObjPath { outputDir / ( uniqueStem + ".obj" ) };
            std::vector<std::byte> const objBytes   { module.SerializeToMemory( )         };

            std::cout << std::format(
                "  [{:3}/{:3}] {:<24} -> {:<20} ({} bytes, {} records)\n",
                moduleIndex,
                modules.size( ),
                rawName.empty( ) ? "<unnamed>" : rawName,
                outObjPath.string( ),
                objBytes.size( ),
                module.GetRecords( ).size( )
            );

            if ( !listOnly ) {
                module.SaveToFile( outObjPath );
            }

            ++moduleIndex;
        }

        if ( !listOnly ) {
            std::cout << std::format( "Successfully extracted {} object file(s) to '{}'.\n", modules.size( ), outputDir.string( ) );
        }

        return 0;
    } catch ( Omf::OmfParseException const& ex ) {
        std::cerr << std::format( "OMF parse error at offset 0x{:X} (record type 0x{:02X}): {}\n", ex.GetFileOffset( ), static_cast<unsigned>( ex.GetRecordType( ) ), ex.what( ) );
        return 3;
    } catch ( std::exception const& ex ) {
        std::cerr << std::format( "Error: {}\n", ex.what( ) );
        return 4;
    }
}
