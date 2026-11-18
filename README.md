# libomf: Modern C++20 16-Bit OMF (`.obj` / `.lib`) Manipulation Library

`libomf` is a modern C++20 library (`OmfFile.h`, `OmfFile.cpp`) and command-line toolset (`omfcatalog`, `omfdump`, `omfedit`, `omfexplode`) for parsing, inspecting, modifying in memory, and generating 16-bit (and 32-bit extended) Relocatable Object Module Format (OMF-86 / TIS v1.1) `.obj` and `.lib` files.

Unlike existing object file utilities that only support disassembling 16-bit OMF files, `libomf` provides a complete, mutable in-memory object model capable of **transparent byte-for-byte round-tripping** as well as structural edits such as symbol renaming across public, external, communal, and `COMDAT` records.

---

## Key Features

- **Transparent Byte-for-Byte Round-Tripping:** Real-world MS-DOS compilers and assemblers (Microsoft C/C++, MASM, Borland C++/TASM, Watcom) exhibit encoding quirks such as non-canonical 2-byte variable-length indices, omitted vs. zero target displacements in `FIXUPP` records, and zeroed vs. two's-complement checksums. `OmfRecord` caches the original interior payload and checksum byte on load and only re-serializes records that have been mutated (`IsDirty( ) == true`). Loading an `.obj` or `.lib` file and saving it without changes produces an output file that is bit-for-bit identical to the input.
- **Complete OMF Record Coverage (34 Derived Classes):** Every known OMF record format has a dedicated class derived from `OmfRecord`, with `OmfUnknownRecord` serving as a fallback for custom or unrecognized record opcodes.
- **Context-Aware Symbol Renaming:** `OmfFile::RenameSymbol( )` and `OmfModule::RenameSymbol( )` rename symbols across `PUBDEF`, `LPUBDEF`, `EXTDEF`, `LEXTDEF`, `COMDEF`, `LCOMDEF`, `LOCSYM`, `DEBSYM`, `ALIAS`, and `LIBDIC`. For function-level linking (`COMDAT`, `CEXTDEF`, `LINSYM`, `NBKPAT`), which reference symbol names indirectly via 1-based indices into `LNAMES`/`LLNAMES`, `OmfModule::RenameSymbol( )` cross-references all indices in the module so `COMDAT` symbols are renamed in `LNAMES` without accidentally renaming segments (`SEGDEF`), classes, overlays, or groups (`GRPDEF`).
- **Configurable Checksum Validation & Repair:** Supports ignoring bad checksums on load (default, matching DOS linkers), firing a warning callback (`ChecksumValidation::WarnCallback`), or throwing `OmfChecksumException` (`ChecksumValidation::ThrowException`). On save, callers can preserve original checksum conventions (`ChecksumPolicy::PreserveOriginal`), compute checksums for modified records (`ChecksumPolicy::AlwaysCompute`), repair all checksums across the file (`ChecksumPolicy::RepairAll`), or emit zero checksums (`ChecksumPolicy::AlwaysZero`).
- **Complete Command-Line Toolset:** Includes utilities for cross-referencing public/external symbols across libraries and object files (`omfcatalog`), dumping OMF record structures (`omfdump`), renaming symbols or testing round-trip serialization (`omfedit`), and extracting `.obj` modules from MS-DOS `.lib` archives (`omfexplode`).

---

## Project Layout & Building (Unix)

### Source Files & Build Directories
- **`OmfFile.h`:** Public C++20 API declarations, record type enumerations, strongly-typed 1-based OMF indices, exception hierarchy, and all 34 `OmfRecord`-derived classes.
- **`OmfFile.cpp`:** Complete implementation of `OmfBinaryReader`, `OmfBinaryWriter`, record parsers/serializers, `OmfModule`, and `OmfFile`.
- **`OmfCatalog.cpp`:** Command-line utility (`bin/omfcatalog`) to catalog and cross-reference defined and unresolved external symbols across `.lib` and `.obj` files.
- **`OmfDump.cpp`:** Command-line utility (`bin/omfdump`) to inspect and dump OMF records.
- **`OmfEdit.cpp`:** Command-line utility (`bin/omfedit`) for round-trip testing and renaming symbols in `.obj` and `.lib` files.
- **`OmfExplode.cpp`:** Command-line utility (`bin/omfexplode`) to list or extract all `.obj` modules from an MS-DOS `.lib` static library.
- **`Makefile`:** GNU Make build system generating intermediate objects in `obj/`, static and shared libraries in `lib/` (`lib/libomf.a`, `lib/libomf.so`), and executables in `bin/`.

### Building with GNU Make
~~~sh
make                                   # Default: c++, RELEASE (-O2 -DNDEBUG)
make BUILD=DEBUG                       # Unoptimized debug build (-O0 -g3 -DDEBUG)
make CXX=clang++ BUILD=DEBUG SAN=1     # Clang debug build with ASan & UBSan
make clean                             # Remove obj/, lib/, and bin/ artifacts
~~~

Build options:
- **`CXX`:** C++20-capable compiler (`c++`, `g++`, `clang++`).
- **`AR`:** Static library archiver (`ar`).
- **`BUILD`:** `RELEASE` (default) or `DEBUG`.
- **`SAN`:** `0` (default) or `1` (enables `-fsanitize=address,undefined -fno-omit-frame-pointer`).

---

## Command-Line Utilities

### 1. `omfcatalog` — Cross-Reference Defined & Unresolved Symbols
Scans all object modules across one or more `.lib` archives and/or `.obj` files and produces two dynamically sized cross-reference tables: **Defined Symbols** (`Symbol`, `Defined In`, `Referenced By`) and **Unresolved External Symbols** (`Symbol`, `Referenced By`).

~~~sh
# Catalog all symbols across a library or set of .obj files
./bin/omfcatalog mthread.lib video.lib

# Exclude module-local symbols (LPUBDEF, LEXTDEF, LCOMDEF, local COMDAT)
./bin/omfcatalog --exclude-locals mthread.lib

# Include declared EXTDEF/LEXTDEF/CEXTDEF symbols even if no FIXUPP targets them
./bin/omfcatalog --show-unreferenced-externs videohlp.obj
~~~

Key behaviors of `omfcatalog`:
- **Comprehensive Record Scanning:** Collects definitions from `PUBDEF`/`PUBDEF32`, `LPUBDEF`/`LPUBDEF32`, `COMDAT`/`COMDAT32`, `COMDEF`, `LCOMDEF`, and `ALIAS` records, and external declarations from `EXTDEF`, `LEXTDEF`/`LEXTDEF32`, and `CEXTDEF` records.
- **Stateful `FIXUPP` Usage Tracking:** Tracks OMF 1-based external indices alongside stateful `FIXUPP` frame and target threads (`0..3`) and relocations across `FIXUPP`, `MODEND`, `REGINT`, `REDATA`, `RIDATA`, and `DEBSYM` records. By default, a module is listed under `Referenced By` only if it actively references the symbol in a relocation, `COMDEF`, or `ALIAS` (override with `--show-unreferenced-externs`).
- **Self-Reference Filtering:** Automatically filters out a symbol's defining module from its `Referenced By` list (for example, when Microsoft C/C++ `/Gy` emits both a `COMDAT` definition and a `CEXTDEF` reference in the same module).
- **Local Symbol Support:** Includes module-local symbols (`LPUBDEF`, `LEXTDEF`, `LCOMDEF`, and local `COMDAT`s) by default; pass `--exclude-locals` to omit them.

### 2. `omfedit` — Rename Symbols or Round-Trip Test `.obj` Files
~~~sh
# Round-trip an .obj file with no changes (verifies byte-for-byte identity)
./bin/omfedit input.obj output.obj

# Rename a symbol across all public, external, communal, and COMDAT records
./bin/omfedit input.obj output.obj _old_symbol _new_symbol
~~~

### 3. `omfexplode` — Extract `.obj` Modules from a `.lib` Archive
~~~sh
# Extract all .obj modules from mthread.lib into ./extracted_objs/
./bin/omfexplode mthread.lib extracted_objs

# List modules inside a .lib archive without writing files
./bin/omfexplode video.lib --list-only

# Preserve original uppercase/lowercase from THEADR module names
./bin/omfexplode input.lib extracted_objs --preserve-case
~~~

`omfexplode` automatically strips DOS drive letters and directory paths stored in `THEADR`/`LHEADR`/`RHEADR` records (such as `src\mthread\mthread.asm` $\rightarrow$ `mthread.obj`), disambiguates any colliding module stems (`helper_1.obj`, `helper_2.obj`), and strips `.lib` inter-module page padding so each extracted `.obj` file is a clean, standalone object module.

### 4. `omfdump` — Inspect OMF Records
Dumps the record structure and contents of 16-bit OMF `.obj` and `.lib` files for inspection and debugging.

---

## C++20 API Overview

### 1. Loading, Modifying, and Saving an `.obj` File
~~~cpp
#include "OmfFile.h"
#include <iostream>

int main( ) {
    // Load a 16-bit OMF .obj file from disk
    Omf::OmfFile objFile = Omf::OmfFile::LoadFromFile( "videohlp.obj" );

    // Rename a symbol across all modules and record types
    std::size_t const renamedCount = objFile.RenameSymbol( "_video_gotoxy", "_vid_gotoxy" );
    std::cout << "Renamed " << renamedCount << " symbol reference(s).\n";

    // Write out the updated .obj file (untouched records retain their exact original bytes)
    objFile.SaveToFile( "videohlp_patched.obj" );
    return 0;
}
~~~

### 2. Inspecting & Mutating Specific Record Types with C++20 Ranges
`OmfModule::GetRecordsOfType<T>( )` returns a lazy C++20 `std::views` filter over all records in a module matching the requested derived class `T`:

~~~cpp
for ( Omf::OmfModule& module : objFile.GetModules( ) ) {
    // Inspect module header name (THEADR / LHEADR)
    if ( auto name = module.GetModuleName( ) ) {
        std::cout << "Module: " << *name << "\n";
    }

    // Iterate over all PUBDEF / LPUBDEF records in the module
    for ( Omf::OmfPubdefRecord* pubdef : module.GetRecordsOfType<Omf::OmfPubdefRecord>( ) ) {
        for ( auto const& sym : pubdef->GetSymbols( ) ) {
            std::cout << "  Public symbol: " << sym.name
                      << " at offset 0x" << std::hex << sym.offset << std::dec << "\n";
        }
    }

    // Resolve segment names from 1-based LNAMES indices
    for ( Omf::OmfSegdefRecord const* segdef : module.GetRecordsOfType<Omf::OmfSegdefRecord>( ) ) {
        auto segName   = module.ResolveName( segdef->GetSegmentNameIndex( ) ).value_or( "<none>" );
        auto className = module.ResolveName( segdef->GetClassNameIndex( ) ).value_or( "<none>" );
        std::cout << "  Segment: " << segName << " (Class: " << className
                  << ", Length: " << segdef->GetSegmentLength( ) << " bytes)\n";
    }
}
~~~

### 3. Base Class (`Omf::OmfRecord`) Interface
Every OMF record class inherits from `Omf::OmfRecord`:
- **`RecordType GetRecordType( ) const noexcept`:** Returns the 8-bit OMF record opcode (`RecordType::Theadr`, `RecordType::Pubdef`, etc.).
- **`bool Is32Bit( ) const noexcept`:** Returns `true` if the least-significant bit of the record opcode is set (`0x8B`, `0x91`, `0x99`, etc.).
- **`std::span<const std::byte> GetRawPayload( ) const`:** Returns the interior payload bytes of the record (excluding the 3-byte `[Type:1][Length:2]` header and 1-byte trailing checksum). If the record has been modified in memory, it automatically serializes the updated fields into the payload cache first.
- **`std::vector<std::byte> SerializeRecord( ChecksumPolicy policy ) const`:** Returns the complete framed OMF record (`[Type:1][Length:2][Payload:N][Checksum:1]`).
- **`std::uint8_t GetOriginalChecksum( ) const noexcept` / `std::uint8_t ComputeChecksum( ) const` / `bool HasValidChecksum( ) const noexcept`:** Inspects the original checksum byte read from disk, computes the two's-complement modulo-$256$ checksum, and reports whether the record had a valid checksum on load.
- **`template <typename T> T* As( ) noexcept`:** Safely downcasts an `OmfRecord*` to a specific derived class (or returns `nullptr`).

### 4. Constructing Custom or Unknown Records (`Omf::OmfUnknownRecord`)
To construct a new record with an arbitrary opcode and raw byte payload, use `OmfUnknownRecord` (the only class with a public `SetRecordType` method):

~~~cpp
std::vector<std::byte> rawPayload = { std::byte{ 0x01 }, std::byte{ 0x02 } };
auto customRec = std::make_unique<Omf::OmfUnknownRecord>(
    static_cast<Omf::RecordType>( 0xCE ),
    std::move( rawPayload )
);
module.AddRecord( std::move( customRec ) );
~~~

---

## Supported OMF Record Classes

| Opcode(s) | Record Name | C++ Class | Description |
| :--- | :--- | :--- | :--- |
| `0x6E` | `RHEADR` | `OmfRheadrRecord` | Legacy Intel OMF-86 Relocatable Module Header |
| `0x70` | `REGINT` | `OmfRegintRecord` | Legacy Intel OMF-86 Register Initialization |
| `0x72` | `REDATA` | `OmfRedataRecord` | Legacy Intel OMF-86 Relocatable Enumerated Data |
| `0x74` | `RIDATA` | `OmfRidataRecord` | Legacy Intel OMF-86 Relocatable Iterated Data |
| `0x76` | `OVLDEF` | `OmfOvldefRecord` | Legacy Intel OMF-86 Overlay Definition |
| `0x78` | `ENDREC` | `OmfEndrecRecord` | Legacy Intel OMF-86 End Record for Overlay/Block |
| `0x7A` / `0x7B` | `BLKDEF` | `OmfBlkdefRecord` | Legacy Intel OMF-86 Block Definition (16/32-bit) |
| `0x7C` / `0x7D` | `BLKEND` | `OmfBlkendRecord` | Legacy Intel OMF-86 Block End (16/32-bit) |
| `0x7E` | `DEBSYM` | `OmfDebsymRecord` | Legacy Intel OMF-86 Debug Symbols |
| `0x80` / `0x82` | `THEADR` / `LHEADR` | `OmfHeaderRecord` | Translator Header / Library Module Header |
| `0x84` | `PEDATA` | `OmfPedataRecord` | Legacy Intel OMF-86 Physical Enumerated Data |
| `0x86` | `PIDATA` | `OmfPidataRecord` | Legacy Intel OMF-86 Physical Iterated Data |
| `0x88` | `COMENT` | `OmfComentRecord` | Comment, Linker Directive, or Dependency Record |
| `0x8A` / `0x8B` | `MODEND` | `OmfModendRecord` | Module End Record & Entry Point (16/32-bit) |
| `0x8C`, `0xB4`/`0xB5` | `EXTDEF` / `LEXTDEF` | `OmfExtdefRecord` | External & Local External Names Definition |
| `0x8E` | `TYPDEF` | `OmfTypdefRecord` | Type Definitions |
| `0x90`/`0x91`, `0xB6`/`0xB7` | `PUBDEF` / `LPUBDEF` | `OmfPubdefRecord` | Public & Local Public Names Definition (16/32-bit) |
| `0x92` / `0x93` | `LOCSYM` | `OmfLocsymRecord` | Local Symbols (16/32-bit) |
| `0x94` / `0x95` | `LINNUM` | `OmfLinnumRecord` | Source Line Numbers (16/32-bit) |
| `0x96` / `0xCA` | `LNAMES` / `LLNAMES` | `OmfLnamesRecord` | List of Logical Names (Global & Local) |
| `0x98` / `0x99` | `SEGDEF` | `OmfSegdefRecord` | Segment Definition (16/32-bit) |
| `0x9A` | `GRPDEF` | `OmfGrpdefRecord` | Group Definition (`DGROUP`, etc.) |
| `0x9C` / `0x9D` | `FIXUPP` | `OmfFixuppRecord` | Relocation Fixups & Frame/Target Threads (16/32-bit) |
| `0xA0` / `0xA1` | `LEDATA` | `OmfLedataRecord` | Logical Enumerated Data (16/32-bit) |
| `0xA2` / `0xA3` | `LIDATA` | `OmfLidataRecord` | Logical Iterated/Repeating Data (16/32-bit) |
| `0xA4` | `LIBHED` | `OmfLibhedRecord` | Intel OMF Library Header |
| `0xA6` | `LIBNAM` | `OmfLibnamRecord` | Intel OMF Library Module Names |
| `0xA8` | `LIBLOC` | `OmfLiblocRecord` | Intel OMF Library Module Locations |
| `0xAA` | `LIBDIC` | `OmfLibdicRecord` | Intel OMF Library Dictionary |
| `0xB0` / `0xB8` | `COMDEF` / `LCOMDEF` | `OmfComdefRecord` | Communal & Local Communal Variables |
| `0xB2` / `0xB3` | `BAKPAT` | `OmfBakpatRecord` | Backpatch Record (16/32-bit) |
| `0xBC` | `CEXTDEF` | `OmfCextdefRecord` | `COMDAT` External Names |
| `0xC2` / `0xC3` | `COMDAT` | `OmfComdatRecord` | Initialized Communal / Function-Level Data (16/32-bit) |
| `0xC4` / `0xC5` | `LINSYM` | `OmfLinsymRecord` | `COMDAT` Symbol Line Numbers (16/32-bit) |
| `0xC6` | `ALIAS` | `OmfAliasRecord` | Symbol Alias Definition |
| `0xC8` / `0xC9` | `NBKPAT` | `OmfNbkpatRecord` | Named Backpatch Record for `COMDAT` (16/32-bit) |
| `0xCC` | `VERNUM` | `OmfVernumRecord` | OMF Version Number |
| `0xCE` | `VENDEXT` | `OmfVendextRecord` | Vendor-Specific OMF Extension |
| `0xF0` | `MsLibHeader` | `OmfMsLibHeaderRecord` | Microsoft / DOS `.LIB` Archive Header |
| `0xF1` | `MsLibEnd` | `OmfMsLibEndRecord` | Microsoft / DOS `.LIB` Archive End / Padding |
| *(Any)* | *(Unknown)* | `OmfUnknownRecord` | Arbitrary / Unrecognized OMF Record |
