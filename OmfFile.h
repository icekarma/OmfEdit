#pragma once

#include <cstddef>
#include <cstdint>

#include <concepts>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <string>
#include <variant>
#include <vector>

namespace Omf {

    //=========================================================================
    // OMF Record Type Enumeration (Intel OMF-86, TIS v1.1, MS-DOS .LIB)
    //=========================================================================

    enum class RecordType:
        std::uint8_t
    {
        Rheadr      = 0x6E, // Relocatable Module Header (Intel OMF-86)
        Regint      = 0x70, // Register Initialization (Intel OMF-86)
        Redata      = 0x72, // Relocatable Enumerated Data (Intel OMF-86)
        Ridata      = 0x74, // Relocatable Iterated Data (Intel OMF-86)
        Ovldef      = 0x76, // Overlay Definition (Intel OMF-86)
        Endrec      = 0x78, // End Record for Overlay/Block (Intel OMF-86)
        Blkdef      = 0x7A, // Block Definition (Intel OMF-86)
        Blkdef32    = 0x7B, // 32-bit Block Definition
        Blkend      = 0x7C, // Block End (Intel OMF-86)
        Blkend32    = 0x7D, // 32-bit Block End
        Debsym      = 0x7E, // Debug Symbols (Intel OMF-86)
        Theadr      = 0x80, // Translator Header
        Lheadr      = 0x82, // Library Module Header
        Pedata      = 0x84, // Physical Enumerated Data (Intel OMF-86)
        Pidata      = 0x86, // Physical Iterated Data (Intel OMF-86)
        Coment      = 0x88, // Comment / Option / Class Record
        Modend      = 0x8A, // Module End (16-bit)
        Modend32    = 0x8B, // Module End (32-bit)
        Extdef      = 0x8C, // External Names Definition
        Typdef      = 0x8E, // Type Definitions
        Pubdef      = 0x90, // Public Names Definition (16-bit)
        Pubdef32    = 0x91, // Public Names Definition (32-bit)
        Locsym      = 0x92, // Local Symbols (16-bit)
        Locsym32    = 0x93, // Local Symbols (32-bit)
        Linnum      = 0x94, // Line Numbers (16-bit)
        Linnum32    = 0x95, // Line Numbers (32-bit)
        Lnames      = 0x96, // List of Names
        Segdef      = 0x98, // Segment Definition (16-bit)
        Segdef32    = 0x99, // Segment Definition (32-bit)
        Grpdef      = 0x9A, // Group Definition
        Fixupp      = 0x9C, // Fixup Record (16-bit)
        Fixupp32    = 0x9D, // Fixup Record (32-bit)
        Ledata      = 0xA0, // Logical Enumerated Data (16-bit)
        Ledata32    = 0xA1, // Logical Enumerated Data (32-bit)
        Lidata      = 0xA2, // Logical Iterated Data (16-bit)
        Lidata32    = 0xA3, // Logical Iterated Data (32-bit)
        Libhed      = 0xA4, // Intel OMF Library Header
        Libnam      = 0xA6, // Intel OMF Library Module Names
        Libloc      = 0xA8, // Intel OMF Library Module Locations
        Libdic      = 0xAA, // Intel OMF Library Dictionary
        Comdef      = 0xB0, // Communal Names Definition
        Bakpat      = 0xB2, // Backpatch Record (16-bit)
        Bakpat32    = 0xB3, // Backpatch Record (32-bit)
        Lextdef     = 0xB4, // Local External Names (16-bit)
        Lextdef32   = 0xB5, // Local External Names (32-bit)
        Lpubdef     = 0xB6, // Local Public Names (16-bit)
        Lpubdef32   = 0xB7, // Local Public Names (32-bit)
        Lcomdef     = 0xB8, // Local Communal Names
        Cextdef     = 0xBC, // COMDAT External Names
        Comdat      = 0xC2, // Initialized Communal Data (16-bit)
        Comdat32    = 0xC3, // Initialized Communal Data (32-bit)
        Linsym      = 0xC4, // Symbol Line Numbers for COMDAT (16-bit)
        Linsym32    = 0xC5, // Symbol Line Numbers for COMDAT (32-bit)
        Alias       = 0xC6, // Alias Definition
        Nbkpat      = 0xC8, // Named Backpatch Record for COMDAT (16-bit)
        Nbkpat32    = 0xC9, // Named Backpatch Record for COMDAT (32-bit)
        Llnames     = 0xCA, // Local Logical Names
        Vernum      = 0xCC, // OMF Version Number
        Vendext     = 0xCE, // Vendor-Specific OMF Extension
        MsLibHeader = 0xF0, // Microsoft/DOS .LIB Header Record
        MsLibEnd    = 0xF1  // Microsoft/DOS .LIB End/Padding Record
    };

    //=========================================================================
    // Exceptions & Checksum Policies
    //=========================================================================

    class OmfException:
        public std::runtime_error
    {

    public:

        using std::runtime_error::runtime_error;

    };

    class OmfParseException:
        public OmfException
    {

    public:

        OmfParseException( std::string const message, std::size_t const fileOffset, RecordType const recordType ):
            OmfException ( std::move( message ) ),
            _fileOffset  ( fileOffset           ),
            _recordType  ( recordType           )
        {
            /*empty*/
        }

        std::size_t GetFileOffset( ) const noexcept { return _fileOffset; }
        RecordType  GetRecordType( ) const noexcept { return _recordType; }

    private:

        std::size_t _fileOffset;
        RecordType  _recordType;

    };

    class OmfChecksumException final:
        public OmfParseException
    {

    public:

        OmfChecksumException( std::size_t const fileOffset, RecordType const recordType, std::uint8_t const expectedChecksum, std::uint8_t const actualChecksum );

        std::uint8_t GetExpectedChecksum( ) const noexcept { return _expectedChecksum; }
        std::uint8_t GetActualChecksum( )   const noexcept { return   _actualChecksum; }

    private:

        std::uint8_t _expectedChecksum;
        std::uint8_t   _actualChecksum;

    };

    struct OmfChecksumWarning {
        std::size_t  fileOffset       { };
        std::size_t  recordIndex      { };
        RecordType   recordType       { RecordType::Theadr };
        std::uint8_t expectedChecksum { };
        std::uint8_t actualChecksum   { };
    };

    enum class ChecksumValidation:
        std::uint8_t
    {
        Ignore,        // Do not throw; record HasValidChecksum( ) == false on OmfRecord
        WarnCallback,  // Invoke OmfLoadOptions::onChecksumWarning and continue
        ThrowException // Throw OmfChecksumException on non-zero checksum mismatch
    };

    enum class ChecksumPolicy:
        std::uint8_t
    {
        PreserveOriginal, // Untouched records keep exact original byte; dirty records match original style
        AlwaysCompute,    // Recompute 2's-complement checksum for every dirty record
        RepairAll,        // Recompute 2's-complement checksum for ALL records (even untouched)
        AlwaysZero        // Write 0x00 checksum for every dirty record
    };

    struct OmfLoadOptions {
        ChecksumValidation                             checksumValidation { ChecksumValidation::Ignore };
        std::function<void(OmfChecksumWarning const&)> onChecksumWarning  { nullptr                    };
    };

    struct OmfSaveOptions {
        ChecksumPolicy checksumPolicy { ChecksumPolicy::PreserveOriginal };
    };

    //=========================================================================
    // Strongly-Typed 1-Based OMF Indices (0 = Unspecified / None)
    //=========================================================================

    template <typename Tag>
    struct OmfIndex {

        std::uint16_t value { };

        constexpr bool IsValid( ) const noexcept { return value != 0; }

        constexpr auto operator<=>( OmfIndex const& ) const noexcept = default;

    };

    using NameIndex     = OmfIndex<struct NameIndexTag>;
    using SegmentIndex  = OmfIndex<struct SegmentIndexTag>;
    using GroupIndex    = OmfIndex<struct GroupIndexTag>;
    using ExternalIndex = OmfIndex<struct ExternalIndexTag>;
    using TypeIndex     = OmfIndex<struct TypeIndexTag>;
    using OverlayIndex  = OmfIndex<struct OverlayIndexTag>;

    //=========================================================================
    // Common Fixup / Frame / Target Specification Types
    //=========================================================================

    enum class FixupLocationType:
        std::uint8_t
    {
        LowByte          = 0,  // 8-bit low byte (lobyte)
        Offset16         = 1,  // 16-bit offset
        SegmentBase      = 2,  // 16-bit base (segment/selector)
        FarPointer16_16  = 3,  // 32-bit far pointer (16:16)
        HighByte         = 4,  // 8-bit high byte (hibyte)
        LoaderResolved16 = 5,  // 16-bit loader-resolved offset (Phar Lap: 32-bit offset)
        Offset32         = 9,  // 32-bit offset (MS/TIS)
        FarPointer16_32  = 11, // 48-bit far pointer (16:32)
        LoaderResolved32 = 13  // 32-bit loader-resolved offset
    };

    enum class FrameMethod:
        std::uint8_t
    {
        SegmentIndex  = 0, // F0: Specified by SegmentIndex
        GroupIndex    = 1, // F1: Specified by GroupIndex
        ExternalIndex = 2, // F2: Specified by ExternalIndex
        ExplicitFrame = 3, // F3: Explicit 16-bit paragraph frame number
        LocationSeg   = 4, // F4: Frame is the LEDATA/LIDATA/COMDAT segment
        TargetSeg     = 5  // F5: Frame is determined by the Target
    };

    enum class TargetMethod:
        std::uint8_t
    {
        SegmentDisp    = 0, // T0: SegmentIndex + Displacement
        GroupDisp      = 1, // T1: GroupIndex + Displacement
        ExternalDisp   = 2, // T2: ExternalIndex + Displacement
        ExplicitDisp   = 3, // T3: Explicit frame + Displacement
        SegmentNoDisp  = 4, // T4: SegmentIndex (Displacement = 0)
        GroupNoDisp    = 5, // T5: GroupIndex (Displacement = 0)
        ExternalNoDisp = 6, // T6: ExternalIndex (Displacement = 0)
        ExplicitNoDisp = 7  // T7: Explicit frame (Displacement = 0)
    };

    struct FixupTargetSpec {
        bool                         frameUsesThread      { false };
        std::uint8_t                 frameMethodOrThread  { };
        std::optional<std::uint16_t> frameDatum;
        bool                         targetUsesThread     { false };
        std::uint8_t                 targetMethodOrThread { };
        std::optional<std::uint16_t> targetDatum;
        std::optional<std::uint32_t> targetDisplacement;
    };

    //=========================================================================
    // Base Class Shared Among All OMF Record Types
    //=========================================================================

    class OmfRecord {

    public:

        virtual ~OmfRecord( ) = default;

        RecordType GetRecordType( ) const noexcept { return _recordType; }

        bool Is32Bit( ) const noexcept {
            return ( static_cast<std::uint8_t>( _recordType ) & 0x01u ) != 0;
        }

        // Returns ONLY the interior payload bytes (excluding 3-byte header and 1-byte checksum).
        // If the record has been modified in memory, regenerates the payload cache first.
        std::span<std::byte const> GetRawPayload( ) const;

        // Serializes the complete framed OMF record: [Type:1][Length:2][Payload:N][Checksum:1]
        std::vector<std::byte> SerializeRecord( ChecksumPolicy const policy = ChecksumPolicy::PreserveOriginal ) const;

        std::uint8_t GetOriginalChecksum( ) const noexcept { return _originalChecksum; }
        std::uint8_t ComputeChecksum( ) const;
        bool         HasValidChecksum( ) const noexcept;

        bool IsDirty( ) const noexcept { return _isDirty; }
        void MarkDirty( ) noexcept     { _isDirty = true; }

        virtual std::unique_ptr<OmfRecord> Clone( ) const = 0;

        template <typename T>
            requires std::derived_from<T, OmfRecord>
        T* As( ) noexcept {
            return dynamic_cast<T*>( this );
        }

        template <typename T>
            requires std::derived_from<T, OmfRecord>
        T const* As( ) const noexcept {
            return dynamic_cast<T const*>( this );
        }

    protected:

        explicit OmfRecord( RecordType const type ) noexcept;

        OmfRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        // Protected helper for derived classes that support toggling between
        // paired opcodes (e.g., THEADR/LHEADR, PUBDEF/LPUBDEF, 16-bit/32-bit)
        void SetRecordTypeProtected( RecordType const newType ) noexcept;

        // Implemented by every derived class to encode its current fields into payload bytes
        virtual std::vector<std::byte> SerializePayload( ) const = 0;

    private:

        void         _RebuildPayloadCacheIfNeeded( ) const;
        std::uint8_t _ComputeChecksumForPayload( std::span<std::byte const> const payload ) const noexcept;

        RecordType                     _recordType;
        mutable bool                   _isDirty;
        std::uint8_t                   _originalChecksum;
        bool                           _hadValidChecksum;
        mutable std::vector<std::byte> _rawPayloadCache;

    };

    //=========================================================================
    // 1. Unknown / Arbitrary Record (Only class with public SetRecordType)
    //=========================================================================

    class OmfUnknownRecord final:
        public OmfRecord
    {

    public:

        OmfUnknownRecord( RecordType const type, std::vector<std::byte> const payload );
        OmfUnknownRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void SetRecordType( RecordType const newType ) noexcept;
        void SetRawPayload( std::vector<std::byte> const newPayload );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<std::byte> _payload;

    };

    //=========================================================================
    // 2. Module Header Record (THEADR 0x80 / LHEADR 0x82)
    //=========================================================================

    class OmfHeaderRecord final:
        public OmfRecord
    {

    public:

        explicit OmfHeaderRecord( std::string const moduleName, bool const isLibraryHeader = false );
        OmfHeaderRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        bool IsLibraryHeader( ) const noexcept { return GetRecordType( ) == RecordType::Lheadr; }
        void SetIsLibraryHeader( bool const isLibraryHeader ) noexcept;

        std::string_view GetModuleName( ) const noexcept { return _moduleName; }
        void             SetModuleName( std::string const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::string _moduleName;

    };

    //=========================================================================
    // 3. Comment Record (COMENT 0x88)
    //=========================================================================

    class OmfComentRecord final:
        public OmfRecord
    {

    public:

        OmfComentRecord( bool const noPurge, bool const noList, std::uint8_t const commentClass, std::vector<std::byte> const commentData );
        OmfComentRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        bool IsNoPurge( ) const noexcept { return _noPurge; }
        void SetNoPurge( bool const value ) noexcept;

        bool IsNoList( ) const noexcept { return _noList; }
        void SetNoList( bool const value ) noexcept;

        std::uint8_t GetCommentClass( ) const noexcept { return _commentClass; }
        void         SetCommentClass( std::uint8_t const newClass ) noexcept;

        std::span<std::byte const> GetCommentData( ) const noexcept { return _commentData; }
        void                       SetCommentData( std::vector<std::byte> const newData );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        bool                   _noPurge      { false };
        bool                   _noList       { false };
        std::uint8_t           _commentClass { };
        std::vector<std::byte> _commentData;

    };

    //=========================================================================
    // 4. Module End Record (MODEND 0x8A / MODEND32 0x8B)
    //=========================================================================

    class OmfModendRecord final:
        public OmfRecord
    {

    public:

        struct AbsoluteStartAddress {
            std::uint16_t frameNumber { };
            std::uint32_t offset      { };
        };

        using StartAddress = std::variant<std::monostate, FixupTargetSpec, AbsoluteStartAddress>;

        explicit OmfModendRecord( bool const isMainModule = false, bool const is32Bit = false );
        OmfModendRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void SetIs32Bit( bool const is32Bit ) noexcept;

        bool IsMainModule( ) const noexcept { return _isMainModule; }
        void SetIsMainModule( bool const value ) noexcept;

        bool HasStartAddress( ) const noexcept {
            return !std::holds_alternative<std::monostate>( _startAddress );
        }

        bool IsStartRelocatable( ) const noexcept {
            return std::holds_alternative<FixupTargetSpec>( _startAddress );
        }

        StartAddress const& GetStartAddress( ) const noexcept { return _startAddress; }
        void                SetStartAddress( StartAddress const address );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        bool         _isMainModule { false };
        StartAddress _startAddress;

    };

    //=========================================================================
    // 5. External Names Record (EXTDEF 0x8C, LEXTDEF 0xB4 / 0xB5)
    //=========================================================================

    class OmfExtdefRecord final:
        public OmfRecord
    {

    public:

        struct ExternalSymbol {
            std::string name;
            TypeIndex   typeIndex;
        };

        explicit OmfExtdefRecord( RecordType const type = RecordType::Extdef );
        OmfExtdefRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        bool IsLocal( ) const noexcept {
            return ( GetRecordType( ) == RecordType::Lextdef ) ||
                   ( GetRecordType( ) == RecordType::Lextdef32 );
        }

        void SetIsLocal( bool const isLocal, bool const is32Bit = false ) noexcept;

        std::vector<ExternalSymbol> const& GetSymbols( ) const noexcept { return _symbols; }
        void                               SetSymbols( std::vector<ExternalSymbol> const symbols );
        void                               SetSymbolName( std::size_t const index, std::string const newName );
        void                               AddSymbol( ExternalSymbol const symbol );
        std::size_t                        RenameSymbol( std::string_view const oldName, std::string_view const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<ExternalSymbol> _symbols;

    };

    //=========================================================================
    // 6. COMDAT External Names Record (CEXTDEF 0xBC)
    //=========================================================================

    class OmfCextdefRecord final:
        public OmfRecord
    {

    public:

        struct ComdatExternal {
            NameIndex logicalNameIndex; // Index into LNAMES / LLNAMES
            TypeIndex typeIndex;
        };

        OmfCextdefRecord( );
        OmfCextdefRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::vector<ComdatExternal> const& GetExternals( ) const noexcept { return _externals; }
        void                               SetExternals( std::vector<ComdatExternal> const externals );
        void                               AddExternal( ComdatExternal const ext );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<ComdatExternal> _externals;

    };

    //=========================================================================
    // 7. Type Definitions Record (TYPDEF 0x8E)
    //=========================================================================

    class OmfTypdefRecord final:
        public OmfRecord
    {

    public:

        struct EightLeafGroup {
            std::uint8_t           niceBits { }; // Bitmask (1 = EN_EASY, 0 = EN_HARD) for up to 8 leaves
            std::vector<std::byte> leafBytes;
        };

        OmfTypdefRecord( );
        OmfTypdefRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::string_view GetTypeName( ) const noexcept { return _typeName; }
        void             SetTypeName( std::string const name );

        std::span<std::byte const> GetRawTypeDescriptor( ) const noexcept { return _rawTypeDescriptor; }
        void                       SetRawTypeDescriptor( std::vector<std::byte> const descriptor );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::string            _typeName;
        std::vector<std::byte> _rawTypeDescriptor;

    };

    //=========================================================================
    // 8. Public Names Record (PUBDEF 0x90/0x91, LPUBDEF 0xB6/0xB7)
    //=========================================================================

    class OmfPubdefRecord final:
        public OmfRecord
    {

    public:

        struct PublicSymbol {
            std::string   name;
            std::uint32_t offset { };
            TypeIndex     typeIndex;
        };

        explicit OmfPubdefRecord( RecordType const type = RecordType::Pubdef );
        OmfPubdefRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        bool IsLocal( ) const noexcept {
            return ( GetRecordType( ) == RecordType::Lpubdef ) ||
                   ( GetRecordType( ) == RecordType::Lpubdef32 );
        }

        void SetFormat( bool const isLocal, bool const is32Bit ) noexcept;

        GroupIndex                   GetBaseGroup( ) const noexcept   { return _baseGroup;   }
        void                         SetBaseGroup( GroupIndex const group ) noexcept;

        SegmentIndex                 GetBaseSegment( ) const noexcept { return _baseSegment; }
        void                         SetBaseSegment( SegmentIndex const segment ) noexcept;

        std::optional<std::uint16_t> GetBaseFrame( ) const noexcept   { return _baseFrame;   }
        void                         SetBaseFrame( std::optional<std::uint16_t> const frame ) noexcept;

        std::vector<PublicSymbol> const& GetSymbols( ) const noexcept { return _symbols; }
        void                             SetSymbols( std::vector<PublicSymbol> const symbols );
        void                             SetSymbolName( std::size_t const index, std::string const newName );
        void                             SetSymbolOffset( std::size_t const index, std::uint32_t const newOffset ) noexcept;
        void                             AddSymbol( PublicSymbol const symbol );
        std::size_t                      RenameSymbol( std::string_view const oldName, std::string_view const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        GroupIndex                   _baseGroup;
        SegmentIndex                 _baseSegment;
        std::optional<std::uint16_t> _baseFrame;
        std::vector<PublicSymbol>    _symbols;

    };

    //=========================================================================
    // 9. Local Symbols Record (LOCSYM 0x92 / LOCSYM32 0x93)
    //=========================================================================

    class OmfLocsymRecord final:
        public OmfRecord
    {

    public:

        using LocalSymbol = OmfPubdefRecord::PublicSymbol;

        explicit OmfLocsymRecord( bool const is32Bit = false );
        OmfLocsymRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void SetIs32Bit( bool const is32Bit ) noexcept;

        GroupIndex                   GetBaseGroup( ) const noexcept   { return _baseGroup;   }
        void                         SetBaseGroup( GroupIndex const group ) noexcept;

        SegmentIndex                 GetBaseSegment( ) const noexcept { return _baseSegment; }
        void                         SetBaseSegment( SegmentIndex const segment ) noexcept;

        std::optional<std::uint16_t> GetBaseFrame( ) const noexcept   { return _baseFrame;   }
        void                         SetBaseFrame( std::optional<std::uint16_t> const frame ) noexcept;

        std::vector<LocalSymbol> const& GetSymbols( ) const noexcept  { return _symbols;     }
        void                            SetSymbols( std::vector<LocalSymbol> const symbols );
        std::size_t                     RenameSymbol( std::string_view const oldName, std::string_view const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        GroupIndex                   _baseGroup;
        SegmentIndex                 _baseSegment;
        std::optional<std::uint16_t> _baseFrame;
        std::vector<LocalSymbol>     _symbols;

    };

    //=========================================================================
    // 10. Line Numbers Record (LINNUM 0x94 / LINNUM32 0x95)
    //=========================================================================

    class OmfLinnumRecord final:
        public OmfRecord
    {

    public:

        struct LineNumberEntry {
            std::uint16_t lineNumber { };
            std::uint32_t offset     { };
        };

        explicit OmfLinnumRecord( bool const is32Bit = false );
        OmfLinnumRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void SetIs32Bit( bool const is32Bit ) noexcept;

        GroupIndex   GetBaseGroup( ) const noexcept   { return _baseGroup;   }
        void         SetBaseGroup( GroupIndex const group ) noexcept;

        SegmentIndex GetBaseSegment( ) const noexcept { return _baseSegment; }
        void         SetBaseSegment( SegmentIndex const segment ) noexcept;

        std::vector<LineNumberEntry> const& GetEntries( ) const noexcept { return _entries; }
        void                                SetEntries( std::vector<LineNumberEntry> const entries );
        void                                AddEntry( LineNumberEntry const entry );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        GroupIndex                   _baseGroup;
        SegmentIndex                 _baseSegment;
        std::vector<LineNumberEntry> _entries;

    };

    //=========================================================================
    // 11. COMDAT Symbol Line Numbers Record (LINSYM 0xC4 / LINSYM32 0xC5)
    //=========================================================================

    class OmfLinsymRecord final:
        public OmfRecord
    {

    public:

        using LineNumberEntry = OmfLinnumRecord::LineNumberEntry;

        explicit OmfLinsymRecord( bool const is32Bit = false );
        OmfLinsymRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void SetIs32Bit( bool const is32Bit ) noexcept;

        std::uint8_t GetFlags( ) const noexcept           { return _flags;                  }
        void         SetFlags( std::uint8_t const flags ) noexcept;
        bool         IsContinuation( ) const noexcept     { return ( _flags & 0x01u ) != 0; }

        NameIndex    GetSymbolNameIndex( ) const noexcept { return _symbolNameIndex;        }
        void         SetSymbolNameIndex( NameIndex const index ) noexcept;

        std::vector<LineNumberEntry> const& GetEntries( ) const noexcept { return _entries; }
        void                                SetEntries( std::vector<LineNumberEntry> const entries );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::uint8_t                 _flags { };
        NameIndex                    _symbolNameIndex;
        std::vector<LineNumberEntry> _entries;

    };

    //=========================================================================
    // 12. List of Names Record (LNAMES 0x96 / LLNAMES 0xCA)
    //=========================================================================

    class OmfLnamesRecord final:
        public OmfRecord
    {

    public:

        explicit OmfLnamesRecord( bool const isLocal = false );
        OmfLnamesRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        bool IsLocal( ) const noexcept { return GetRecordType( ) == RecordType::Llnames; }
        void SetIsLocal( bool const isLocal ) noexcept;

        std::vector<std::string> const& GetNames( ) const noexcept { return _names; }
        void                            SetNames( std::vector<std::string> const names );
        void                            SetName( std::size_t const zeroBasedIndex, std::string const newName );
        void                            AddName( std::string const name );
        std::size_t                     Rename( std::string_view const oldName, std::string_view const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<std::string> _names;

    };

    //=========================================================================
    // 13. Segment Definition Record (SEGDEF 0x98 / SEGDEF32 0x99)
    //=========================================================================

    class OmfSegdefRecord final:
        public OmfRecord
    {

    public:

        enum class Alignment:
            std::uint8_t
        {
            Absolute             = 0,
            ByteRelocatable      = 1,
            WordRelocatable      = 2,
            ParagraphRelocatable = 3,
            PageRelocatable      = 4,
            DwordRelocatable     = 5,
            LoadTimeParagraph    = 6
        };

        enum class Combination:
            std::uint8_t
        {
            Private = 0,
            Public  = 2,
            Public4 = 4,
            Stack   = 5,
            Common  = 6,
            Public7 = 7
        };

        explicit OmfSegdefRecord( bool const is32Bit = false );
        OmfSegdefRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void          SetIs32Bit( bool const is32Bit ) noexcept;

        Alignment     GetAlignment( ) const noexcept        { return _alignment;        }
        void          SetAlignment( Alignment const value ) noexcept;

        Combination   GetCombination( ) const noexcept      { return _combination;      }
        void          SetCombination( Combination const value ) noexcept;

        bool          IsMaxLength( ) const noexcept         { return _isBig;            }
        void          SetMaxLength( bool const value ) noexcept;

        bool          IsUse32( ) const noexcept             { return _isUse32;          }
        void          SetUse32( bool const value ) noexcept;

        std::uint32_t GetSegmentLength( ) const noexcept    { return _segmentLength;    }
        void          SetSegmentLength( std::uint32_t const length ) noexcept;

        NameIndex     GetSegmentNameIndex( ) const noexcept { return _segmentNameIndex; }
        void          SetSegmentNameIndex( NameIndex const index ) noexcept;

        NameIndex     GetClassNameIndex( ) const noexcept   { return _classNameIndex;   }
        void          SetClassNameIndex( NameIndex const index ) noexcept;

        NameIndex     GetOverlayNameIndex( ) const noexcept { return _overlayNameIndex; }
        void          SetOverlayNameIndex( NameIndex const index ) noexcept;

        std::optional<std::uint16_t> GetAbsoluteFrame( ) const noexcept  { return _absoluteFrame;  }
        std::optional<std::uint8_t>  GetAbsoluteOffset( ) const noexcept { return _absoluteOffset; }
        void                         SetAbsoluteAddress( std::uint16_t const frame, std::uint8_t const offset ) noexcept;

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        Alignment                    _alignment     { Alignment::ByteRelocatable };
        Combination                  _combination   { Combination::Private       };
        bool                         _isBig         { false                      };
        bool                         _isUse32       { false                      };
        std::optional<std::uint16_t> _absoluteFrame;
        std::optional<std::uint8_t>  _absoluteOffset;
        std::uint32_t                _segmentLength { };
        NameIndex                    _segmentNameIndex;
        NameIndex                    _classNameIndex;
        NameIndex                    _overlayNameIndex;

    };

    //=========================================================================
    // 14. Group Definition Record (GRPDEF 0x9A)
    //=========================================================================

    class OmfGrpdefRecord final:
        public OmfRecord
    {

    public:

        struct Component {
            std::uint8_t descriptor { 0xFF }; // 0xFF = SegmentIndex, 0xFE = External, 0xFD = Seg/Class/Ovl, 0xFA = LTL
            SegmentIndex segmentIndex;
        };

        explicit OmfGrpdefRecord( NameIndex const groupNameIndex = { } );
        OmfGrpdefRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        NameIndex GetGroupNameIndex( ) const noexcept { return _groupNameIndex; }
        void      SetGroupNameIndex( NameIndex const index ) noexcept;

        std::vector<Component> const& GetComponents( ) const noexcept { return _components; }
        void                          SetComponents( std::vector<Component> const components );
        void                          AddComponent( Component const component );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        NameIndex              _groupNameIndex;
        std::vector<Component> _components;

    };

    //=========================================================================
    // 15. Fixup Record (FIXUPP 0x9C / FIXUPP32 0x9D)
    //=========================================================================

    class OmfFixuppRecord final:
        public OmfRecord
    {

    public:

        struct ThreadSubrecord {
            bool          isFrameThread { false }; // false = Target thread, true = Frame thread
            std::uint8_t  threadNumber  { };       // 0..3
            std::uint8_t  method        { };       // T0..T6 or F0..F5
            std::uint16_t index         { };       // Present unless Frame method is F4 or F5
        };

        struct FixupSubrecord {
            bool              isSegmentRelative { true }; // false = Self-relative
            FixupLocationType location          { FixupLocationType::Offset16 };
            std::uint16_t     dataRecordOffset  { };      // 0..1023
            FixupTargetSpec   target;
        };

        using Subrecord = std::variant<ThreadSubrecord, FixupSubrecord>;

        explicit OmfFixuppRecord( bool const is32Bit = false );
        OmfFixuppRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void SetIs32Bit( bool const is32Bit ) noexcept;

        std::vector<Subrecord> const& GetSubrecords( ) const noexcept { return _subrecords; }
        void                          SetSubrecords( std::vector<Subrecord> const subrecords );
        void                          AddSubrecord( Subrecord const subrecord );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<Subrecord> _subrecords;

    };

    //=========================================================================
    // 16. Logical Enumerated Data Record (LEDATA 0xA0 / LEDATA32 0xA1)
    //=========================================================================

    class OmfLedataRecord final:
        public OmfRecord
    {

    public:

        OmfLedataRecord( SegmentIndex const segmentIndex, std::uint32_t const dataOffset, std::vector<std::byte> const data, bool const is32Bit = false );
        OmfLedataRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void          SetIs32Bit( bool const is32Bit ) noexcept;

        SegmentIndex  GetSegmentIndex( ) const noexcept { return _segmentIndex; }
        void          SetSegmentIndex( SegmentIndex const segment ) noexcept;

        std::uint32_t GetDataOffset( ) const noexcept   { return _dataOffset;   }
        void          SetDataOffset( std::uint32_t const offset ) noexcept;

        std::span<std::byte const> GetData( ) const noexcept { return _data; }
        std::span<std::byte>       GetMutableData( ) noexcept {
            MarkDirty( );
            return _data;
        }

        void SetData( std::vector<std::byte> const newData );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        SegmentIndex           _segmentIndex;
        std::uint32_t          _dataOffset { };
        std::vector<std::byte> _data;

    };

    //=========================================================================
    // 17. Logical Iterated Data Record (LIDATA 0xA2 / LIDATA32 0xA3)
    //=========================================================================

    class OmfLidataRecord final:
        public OmfRecord
    {

    public:

        struct IteratedDataBlock {
            std::uint32_t                                                    repeatCount { 1 };
            std::variant<std::vector<std::byte>,
                         std::vector<IteratedDataBlock>>                     content;
        };

        explicit OmfLidataRecord( bool const is32Bit = false );
        OmfLidataRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void          SetIs32Bit( bool const is32Bit ) noexcept;

        SegmentIndex  GetSegmentIndex( ) const noexcept { return _segmentIndex; }
        void          SetSegmentIndex( SegmentIndex const segment ) noexcept;

        std::uint32_t GetDataOffset( ) const noexcept   { return _dataOffset;   }
        void          SetDataOffset( std::uint32_t const offset ) noexcept;

        std::vector<IteratedDataBlock> const& GetBlocks( ) const noexcept { return _blocks; }
        void                                  SetBlocks( std::vector<IteratedDataBlock> const blocks );
        std::vector<std::byte>                ExpandData( ) const;

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        SegmentIndex                   _segmentIndex;
        std::uint32_t                  _dataOffset { };
        std::vector<IteratedDataBlock> _blocks;

    };

    //=========================================================================
    // 18. Communal Names Record (COMDEF 0xB0 / LCOMDEF 0xB8)
    //=========================================================================

    class OmfComdefRecord final:
        public OmfRecord
    {

    public:

        enum class DataSegmentType:
            std::uint8_t
        {
            FarComm  = 0x61, // Both elementCount and elementSize are present
            NearComm = 0x62  // Only byteLength (stored in elementCount) is present
            // Values 0x01..0x5F represent Borland/PharLap segment indices (1-based)
        };

        struct CommunalSymbol {
            std::string   name;
            TypeIndex     typeIndex;
            std::uint8_t  dataSegmentType { static_cast<std::uint8_t>( DataSegmentType::NearComm ) };
            std::uint32_t elementCount    { };   // Or total byte size when NearComm
            std::uint32_t elementSize     { 1 }; // Used when dataSegmentType == FarComm (0x61)
        };

        explicit OmfComdefRecord( bool const isLocal = false );
        OmfComdefRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        bool IsLocal( ) const noexcept { return GetRecordType( ) == RecordType::Lcomdef; }
        void SetIsLocal( bool const isLocal ) noexcept;

        std::vector<CommunalSymbol> const& GetSymbols( ) const noexcept { return _symbols; }
        void                               SetSymbols( std::vector<CommunalSymbol> const symbols );
        void                               AddSymbol( CommunalSymbol const symbol );
        std::size_t                        RenameSymbol( std::string_view const oldName, std::string_view const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<CommunalSymbol> _symbols;

    };

    //=========================================================================
    // 19. Backpatch Record (BAKPAT 0xB2 / BAKPAT32 0xB3)
    //=========================================================================

    class OmfBakpatRecord final:
        public OmfRecord
    {

    public:

        struct BackpatchEntry {
            std::uint32_t offset { };
            std::uint32_t value  { };
        };

        explicit OmfBakpatRecord( bool const is32Bit = false );
        OmfBakpatRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void              SetIs32Bit( bool const is32Bit ) noexcept;

        SegmentIndex      GetSegmentIndex( ) const noexcept { return _segmentIndex; }
        void              SetSegmentIndex( SegmentIndex const segment ) noexcept;

        FixupLocationType GetLocationType( ) const noexcept { return _locationType; }
        void              SetLocationType( FixupLocationType const type ) noexcept;

        std::vector<BackpatchEntry> const& GetEntries( ) const noexcept { return _entries; }
        void                               SetEntries( std::vector<BackpatchEntry> const entries );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        SegmentIndex                _segmentIndex;
        FixupLocationType           _locationType { FixupLocationType::Offset16 };
        std::vector<BackpatchEntry> _entries;

    };

    //=========================================================================
    // 20. Named Backpatch Record for COMDAT (NBKPAT 0xC8 / NBKPAT32 0xC9)
    //=========================================================================

    class OmfNbkpatRecord final:
        public OmfRecord
    {

    public:

        using BackpatchEntry = OmfBakpatRecord::BackpatchEntry;

        explicit OmfNbkpatRecord( bool const is32Bit = false );
        OmfNbkpatRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void              SetIs32Bit( bool const is32Bit ) noexcept;

        FixupLocationType GetLocationType( ) const noexcept    { return _locationType;    }
        void              SetLocationType( FixupLocationType const type ) noexcept;

        NameIndex         GetPublicNameIndex( ) const noexcept { return _publicNameIndex; }
        void              SetPublicNameIndex( NameIndex const index ) noexcept;

        std::vector<BackpatchEntry> const& GetEntries( ) const noexcept { return _entries; }
        void                               SetEntries( std::vector<BackpatchEntry> const entries );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        FixupLocationType           _locationType { FixupLocationType::Offset16 };
        NameIndex                   _publicNameIndex;
        std::vector<BackpatchEntry> _entries;

    };

    //=========================================================================
    // 21. Initialized Communal Data Record (COMDAT 0xC2 / COMDAT32 0xC3)
    //=========================================================================

    class OmfComdatRecord final:
        public OmfRecord
    {

    public:

        enum class SelectionCriteria:
            std::uint8_t
        {
            NoMatch     = 0x00,
            PickAny     = 0x10,
            SameSize    = 0x20,
            ExactMatch  = 0x30,
            Associative = 0x40,
            Largest     = 0x50
        };

        enum class AllocationType:
            std::uint8_t
        {
            Explicit = 0x00, // Uses explicit publicBaseGroup / publicBaseSegment
            FarCode  = 0x01, // 16-bit CODE
            FarData  = 0x02, // 16-bit FAR_DATA
            Code32   = 0x03, // 32-bit FLAT code
            Data32   = 0x04  // 32-bit FLAT data
        };

        using DataPayload = std::variant<std::vector<std::byte>,
                                         std::vector<OmfLidataRecord::IteratedDataBlock>>;

        explicit OmfComdatRecord( bool const is32Bit = false );
        OmfComdatRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void                       SetIs32Bit( bool const is32Bit ) noexcept;

        std::uint8_t               GetFlags( ) const noexcept             { return _flags;                  }
        void                       SetFlags( std::uint8_t const flags ) noexcept;
        bool                       IsContinuation( ) const noexcept       { return ( _flags & 0x01u ) != 0; }
        bool                       IsIteratedData( ) const noexcept       { return ( _flags & 0x02u ) != 0; }
        bool                       IsLocalSymbol( ) const noexcept        { return ( _flags & 0x04u ) != 0; }
        bool                       IsCodeSegmentData( ) const noexcept    { return ( _flags & 0x08u ) != 0; }

        SelectionCriteria          GetSelectionCriteria( ) const noexcept { return _selectionCriteria;      }
        void                       SetSelectionCriteria( SelectionCriteria const criteria ) noexcept;

        AllocationType             GetAllocationType( ) const noexcept    { return _allocationType;         }
        void                       SetAllocationType( AllocationType const allocType ) noexcept;

        OmfSegdefRecord::Alignment GetAlignment( ) const noexcept         { return _alignment;              }
        void                       SetAlignment( OmfSegdefRecord::Alignment const align ) noexcept;

        std::uint32_t              GetDataOffset( ) const noexcept        { return _dataOffset;             }
        void                       SetDataOffset( std::uint32_t const offset ) noexcept;

        TypeIndex                  GetTypeIndex( ) const noexcept         { return _typeIndex;              }
        void                       SetTypeIndex( TypeIndex const index ) noexcept;

        GroupIndex                   GetPublicBaseGroup( ) const noexcept   { return _publicBaseGroup;   }
        SegmentIndex                 GetPublicBaseSegment( ) const noexcept { return _publicBaseSegment; }
        std::optional<std::uint16_t> GetPublicBaseFrame( ) const noexcept   { return _publicBaseFrame;   }
        void                         SetExplicitBase( GroupIndex const group, SegmentIndex const segment, std::optional<std::uint16_t> const frame = std::nullopt ) noexcept;

        NameIndex                  GetPublicNameIndex( ) const noexcept   { return _publicNameIndex;        }
        void                       SetPublicNameIndex( NameIndex const index ) noexcept;

        DataPayload const&         GetDataPayload( ) const noexcept       { return _dataPayload;            }
        void                       SetDataPayload( DataPayload const payload );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::uint8_t                 _flags             { };
        SelectionCriteria            _selectionCriteria { SelectionCriteria::PickAny           };
        AllocationType               _allocationType    { AllocationType::Explicit             };
        OmfSegdefRecord::Alignment   _alignment         { OmfSegdefRecord::Alignment::Absolute };
        std::uint32_t                _dataOffset        { };
        TypeIndex                    _typeIndex;
        GroupIndex                   _publicBaseGroup;
        SegmentIndex                 _publicBaseSegment;
        std::optional<std::uint16_t> _publicBaseFrame;
        NameIndex                    _publicNameIndex;
        DataPayload                  _dataPayload;

    };

    //=========================================================================
    // 22. Alias Definition Record (ALIAS 0xC6)
    //=========================================================================

    class OmfAliasRecord final:
        public OmfRecord
    {

    public:

        struct AliasEntry {
            std::string aliasName;
            std::string substituteName;
        };

        OmfAliasRecord( );
        OmfAliasRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::vector<AliasEntry> const& GetAliases( ) const noexcept { return _aliases; }
        void                           SetAliases( std::vector<AliasEntry> const aliases );
        void                           AddAlias( AliasEntry const entry );
        std::size_t                    RenameSymbol( std::string_view const oldName, std::string_view const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<AliasEntry> _aliases;

    };

    //=========================================================================
    // 23. OMF Version & Vendor Extension Records (VERNUM 0xCC, VENDEXT 0xCE)
    //=========================================================================

    class OmfVernumRecord final:
        public OmfRecord
    {

    public:

        explicit OmfVernumRecord( std::string const version );
        OmfVernumRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::string_view GetVersion( ) const noexcept { return _version; }
        void             SetVersion( std::string const newVersion );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::string _version;

    };

    class OmfVendextRecord final:
        public OmfRecord
    {

    public:

        OmfVendextRecord( std::uint16_t const subtype, std::vector<std::byte> const extensionData );
        OmfVendextRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::uint16_t              GetSubtype( ) const noexcept       { return _subtype;       }
        void                       SetSubtype( std::uint16_t const subtype ) noexcept;

        std::span<std::byte const> GetExtensionData( ) const noexcept { return _extensionData; }
        void                       SetExtensionData( std::vector<std::byte> const data );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::uint16_t          _subtype { };
        std::vector<std::byte> _extensionData;

    };

    //=========================================================================
    // 24. Intel OMF-86 Legacy Records (0x6E..0x7E, 0x84..0x86)
    //=========================================================================

    // Relocatable Module Header (RHEADR 0x6E)
    class OmfRheadrRecord final:
        public OmfRecord
    {

    public:

        OmfRheadrRecord( );
        OmfRheadrRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::string_view           GetModuleName( ) const noexcept         { return _moduleName;         }
        void                       SetModuleName( std::string const name );

        std::uint8_t               GetModuleType( ) const noexcept         { return _moduleType;         }
        void                       SetModuleType( std::uint8_t const type ) noexcept;

        std::span<std::byte const> GetTrailingAttributes( ) const noexcept { return _trailingAttributes; }
        void                       SetTrailingAttributes( std::vector<std::byte> const attrs );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::string            _moduleName;
        std::uint8_t           _moduleType { };
        std::vector<std::byte> _trailingAttributes;

    };

    // Register Initialization Record (REGINT 0x70)
    class OmfRegintRecord final:
        public OmfRecord
    {

    public:

        enum class RegisterId:
            std::uint8_t
        {
            CsAndIp = 0,
            SsAndSp = 1,
            Ds      = 2,
            Es      = 3
        };

        struct RegisterInitEntry {
            RegisterId                   registerId     { RegisterId::Ds };
            bool                         isRelocatable  { true           };
            FixupTargetSpec              relocTarget;
            std::uint16_t                explicitFrame  { };
            std::optional<std::uint16_t> explicitOffset;
        };

        OmfRegintRecord( );
        OmfRegintRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::vector<RegisterInitEntry> const& GetEntries( ) const noexcept { return _entries; }
        void                                  SetEntries( std::vector<RegisterInitEntry> const entries );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<RegisterInitEntry> _entries;

    };

    // Relocatable Enumerated Data (REDATA 0x72) & Relocatable Iterated Data (RIDATA 0x74)
    class OmfRedataRecord final:
        public OmfRecord
    {

    public:

        OmfRedataRecord( );
        OmfRedataRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        FixupTargetSpec            GetBaseSpec( ) const noexcept { return _baseSpec; }
        void                       SetBaseSpec( FixupTargetSpec const spec );

        std::span<std::byte const> GetData( ) const noexcept     { return _data;     }
        void                       SetData( std::vector<std::byte> const data );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        FixupTargetSpec        _baseSpec;
        std::vector<std::byte> _data;

    };

    class OmfRidataRecord final:
        public OmfRecord
    {

    public:

        using IteratedDataBlock = OmfLidataRecord::IteratedDataBlock;

        OmfRidataRecord( );
        OmfRidataRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        FixupTargetSpec                       GetBaseSpec( ) const noexcept { return _baseSpec; }
        void                                  SetBaseSpec( FixupTargetSpec const spec );

        std::vector<IteratedDataBlock> const& GetBlocks( ) const noexcept   { return _blocks;   }
        void                                  SetBlocks( std::vector<IteratedDataBlock> const blocks );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        FixupTargetSpec                _baseSpec;
        std::vector<IteratedDataBlock> _blocks;

    };

    // Overlay Definition (OVLDEF 0x76) & End Record (ENDREC 0x78)
    class OmfOvldefRecord final:
        public OmfRecord
    {

    public:

        OmfOvldefRecord( );
        OmfOvldefRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::string_view GetOverlayName( ) const noexcept          { return _overlayName;          }
        void             SetOverlayName( std::string const name );

        std::uint16_t    GetLocationBlock( ) const noexcept        { return _locationBlock;        }
        std::uint16_t    GetLocationByteOffset( ) const noexcept   { return _locationByteOffset;   }
        void             SetLocation( std::uint16_t const block, std::uint16_t const byteOffset ) noexcept;

        std::uint8_t     GetOverlayAttributes( ) const noexcept    { return _overlayAttributes;    }
        OverlayIndex     GetSharedOverlayIndex( ) const noexcept   { return _sharedOverlayIndex;   }
        OverlayIndex     GetAdjacentOverlayIndex( ) const noexcept { return _adjacentOverlayIndex; }

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::string   _overlayName;
        std::uint16_t _locationBlock      { };
        std::uint16_t _locationByteOffset { };
        std::uint8_t  _overlayAttributes  { };
        OverlayIndex  _sharedOverlayIndex;
        OverlayIndex  _adjacentOverlayIndex;

    };

    class OmfEndrecRecord final:
        public OmfRecord
    {

    public:

        enum class EndType:
            std::uint8_t
        {
            Overlay = 0,
            Block   = 1
        };

        explicit OmfEndrecRecord( EndType const endType = EndType::Block );
        OmfEndrecRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        EndType GetEndType( ) const noexcept { return _endType; }
        void    SetEndType( EndType const type ) noexcept;

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        EndType _endType { EndType::Block };

    };

    // Block Definition (BLKDEF 0x7A/0x7B) & Block End (BLKEND 0x7C/0x7D)
    class OmfBlkdefRecord final:
        public OmfRecord
    {

    public:

        explicit OmfBlkdefRecord( bool const is32Bit = false );
        OmfBlkdefRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void                         SetIs32Bit( bool const is32Bit ) noexcept;

        GroupIndex                   GetBaseGroup( ) const noexcept           { return _baseGroup;           }
        SegmentIndex                 GetBaseSegment( ) const noexcept         { return _baseSegment;         }
        std::optional<std::uint16_t> GetBaseFrame( ) const noexcept           { return _baseFrame;           }
        void                         SetBase( GroupIndex const group, SegmentIndex const segment, std::optional<std::uint16_t> const frame = std::nullopt ) noexcept;

        std::string_view             GetBlockName( ) const noexcept           { return _blockName;           }
        void                         SetBlockName( std::string const name );

        std::uint32_t                GetBlockOffset( ) const noexcept         { return _blockOffset;         }
        void                         SetBlockOffset( std::uint32_t const offset ) noexcept;

        std::uint32_t                GetBlockLength( ) const noexcept         { return _blockLength;         }
        void                         SetBlockLength( std::uint32_t const length ) noexcept;

        std::uint8_t                 GetProcedureFlags( ) const noexcept      { return _procedureFlags;      }
        void                         SetProcedureFlags( std::uint8_t const flags ) noexcept;

        std::optional<std::uint32_t> GetReturnAddressOffset( ) const noexcept { return _returnAddressOffset; }
        void                         SetReturnAddressOffset( std::optional<std::uint32_t> const offset ) noexcept;

        TypeIndex                    GetTypeIndex( ) const noexcept           { return _typeIndex;           }
        void                         SetTypeIndex( TypeIndex const index ) noexcept;

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        GroupIndex                   _baseGroup;
        SegmentIndex                 _baseSegment;
        std::optional<std::uint16_t> _baseFrame;
        std::string                  _blockName;
        std::uint32_t                _blockOffset    { };
        std::uint32_t                _blockLength    { };
        std::uint8_t                 _procedureFlags { };
        std::optional<std::uint32_t> _returnAddressOffset;
        TypeIndex                    _typeIndex;

    };

    class OmfBlkendRecord final:
        public OmfRecord
    {

    public:

        explicit OmfBlkendRecord( bool const is32Bit = false );
        OmfBlkendRecord( RecordType const type, std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        void SetIs32Bit( bool const is32Bit ) noexcept;

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    };

    // Debug Symbols (DEBSYM 0x7E)
    class OmfDebsymRecord final:
        public OmfRecord
    {

    public:

        using DebugSymbol = OmfPubdefRecord::PublicSymbol;

        OmfDebsymRecord( );
        OmfDebsymRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::uint8_t                    GetFrameMethod( ) const noexcept { return _frameMethod; }
        void                            SetFrameMethod( std::uint8_t const method ) noexcept;

        FixupTargetSpec                 GetBaseSpec( ) const noexcept    { return _baseSpec;    }
        void                            SetBaseSpec( FixupTargetSpec const spec );

        std::vector<DebugSymbol> const& GetSymbols( ) const noexcept     { return _symbols;     }
        void                            SetSymbols( std::vector<DebugSymbol> const symbols );
        std::size_t                     RenameSymbol( std::string_view const oldName, std::string_view const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::uint8_t             _frameMethod { };
        FixupTargetSpec          _baseSpec;
        std::vector<DebugSymbol> _symbols;

    };

    // Physical Enumerated Data (PEDATA 0x84) & Physical Iterated Data (PIDATA 0x86)
    class OmfPedataRecord final:
        public OmfRecord
    {

    public:

        OmfPedataRecord( std::uint16_t const frameNumber, std::uint8_t const offset, std::vector<std::byte> const data );
        OmfPedataRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::uint16_t              GetFrameNumber( ) const noexcept { return _frameNumber; }
        void                       SetFrameNumber( std::uint16_t const frame ) noexcept;

        std::uint8_t               GetOffset( ) const noexcept      { return _offset;      }
        void                       SetOffset( std::uint8_t const offset ) noexcept;

        std::span<std::byte const> GetData( ) const noexcept        { return _data;        }
        void                       SetData( std::vector<std::byte> const data );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::uint16_t          _frameNumber { };
        std::uint8_t           _offset      { };
        std::vector<std::byte> _data;

    };

    class OmfPidataRecord final:
        public OmfRecord
    {

    public:

        using IteratedDataBlock = OmfLidataRecord::IteratedDataBlock;

        OmfPidataRecord( );
        OmfPidataRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::uint16_t                         GetFrameNumber( ) const noexcept { return _frameNumber; }
        void                                  SetFrameNumber( std::uint16_t const frame ) noexcept;

        std::uint8_t                          GetOffset( ) const noexcept      { return _offset;      }
        void                                  SetOffset( std::uint8_t const offset ) noexcept;

        std::vector<IteratedDataBlock> const& GetBlocks( ) const noexcept      { return _blocks;      }
        void                                  SetBlocks( std::vector<IteratedDataBlock> const blocks );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::uint16_t                  _frameNumber { };
        std::uint8_t                   _offset      { };
        std::vector<IteratedDataBlock> _blocks;

    };

    //=========================================================================
    // 25. Library Records (Intel OMF 0xA4..0xAA and MS-DOS .LIB 0xF0..0xF1)
    //=========================================================================

    // Intel OMF Library Header (LIBHED 0xA4)
    class OmfLibhedRecord final:
        public OmfRecord
    {

    public:

        OmfLibhedRecord( );
        OmfLibhedRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::uint32_t GetModuleCount( ) const noexcept           { return _moduleCount;           }
        void          SetModuleCount( std::uint32_t const count ) noexcept;

        std::uint16_t GetDictionaryBlockNumber( ) const noexcept { return _dictionaryBlockNumber; }
        std::uint16_t GetDictionaryByteOffset( ) const noexcept  { return _dictionaryByteOffset;  }
        void          SetDictionaryLocation( std::uint16_t const block, std::uint16_t const byteOffset ) noexcept;

        std::uint8_t  GetFlags( ) const noexcept                 { return _flags;                 }
        void          SetFlags( std::uint8_t const flags ) noexcept;

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::uint32_t _moduleCount           { };
        std::uint16_t _dictionaryBlockNumber { };
        std::uint16_t _dictionaryByteOffset  { };
        std::uint8_t  _flags                 { };

    };

    // Intel OMF Library Module Names (LIBNAM 0xA6)
    class OmfLibnamRecord final:
        public OmfRecord
    {

    public:

        OmfLibnamRecord( );
        OmfLibnamRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::vector<std::string> const& GetModuleNames( ) const noexcept { return _moduleNames; }
        void                            SetModuleNames( std::vector<std::string> const names );
        void                            AddModuleName( std::string const name );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<std::string> _moduleNames;

    };

    // Intel OMF Library Module Locations (LIBLOC 0xA8)
    class OmfLiblocRecord final:
        public OmfRecord
    {

    public:

        struct ModuleLocation {
            std::uint16_t blockNumber { };
            std::uint16_t byteOffset  { };
        };

        OmfLiblocRecord( );
        OmfLiblocRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::vector<ModuleLocation> const& GetLocations( ) const noexcept { return _locations; }
        void                               SetLocations( std::vector<ModuleLocation> const locations );
        void                               AddLocation( ModuleLocation const location );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<ModuleLocation> _locations;

    };

    // Intel OMF Library Dictionary (LIBDIC 0xAA)
    class OmfLibdicRecord final:
        public OmfRecord
    {

    public:

        OmfLibdicRecord( );
        OmfLibdicRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        // Each inner vector represents the public symbols exported by one module
        std::vector<std::vector<std::string>> const& GetModulePublicGroups( ) const noexcept {
            return _modulePublicGroups;
        }

        void        SetModulePublicGroups( std::vector<std::vector<std::string>> const groups );
        std::size_t RenameSymbol( std::string_view const oldName, std::string_view const newName );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<std::vector<std::string>> _modulePublicGroups;

    };

    // Microsoft / DOS .LIB Library Header Record (0xF0)
    class OmfMsLibHeaderRecord final:
        public OmfRecord
    {

    public:

        OmfMsLibHeaderRecord( );
        OmfMsLibHeaderRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        // Note: In MS .LIB files, pageSize is encoded in the record length field as (pageSize - 3)
        std::uint16_t              GetPageSize( ) const noexcept             { return _pageSize;                }
        void                       SetPageSize( std::uint16_t const pageSize );

        std::uint32_t              GetDictionaryFileOffset( ) const noexcept { return _dictionaryFileOffset;    }
        void                       SetDictionaryFileOffset( std::uint32_t const offset ) noexcept;

        std::uint16_t              GetDictionaryBlockCount( ) const noexcept { return _dictionaryBlockCount;    }
        void                       SetDictionaryBlockCount( std::uint16_t const count ) noexcept;

        std::uint8_t               GetFlags( ) const noexcept                { return _flags;                   }
        void                       SetFlags( std::uint8_t const flags ) noexcept;
        bool                       IsCaseSensitive( ) const noexcept         { return ( _flags & 0x01u ) != 0;  }

        std::span<std::byte const> GetPaddingBytes( ) const noexcept         { return _paddingBytes;            }
        void                       SetPaddingBytes( std::vector<std::byte> const padding );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::uint16_t          _pageSize             { 16 };
        std::uint32_t          _dictionaryFileOffset { };
        std::uint16_t          _dictionaryBlockCount { };
        std::uint8_t           _flags                { };
        std::vector<std::byte> _paddingBytes;

    };

    // Microsoft / DOS .LIB Library End Record (0xF1)
    class OmfMsLibEndRecord final:
        public OmfRecord
    {

    public:

        explicit OmfMsLibEndRecord( std::vector<std::byte> const padding = { } );
        OmfMsLibEndRecord( std::span<std::byte const> const rawPayload, std::uint8_t const originalChecksum, bool const hadValidChecksum );

        std::span<std::byte const> GetPaddingBytes( ) const noexcept { return _paddingBytes; }
        void                       SetPaddingBytes( std::vector<std::byte> const padding );

        std::unique_ptr<OmfRecord> Clone( ) const override;

    protected:

        std::vector<std::byte> SerializePayload( ) const override;

    private:

        std::vector<std::byte> _paddingBytes;

    };

    //=========================================================================
    // Object Module & File Containers
    //=========================================================================

    class OmfModule {

    public:

        OmfModule( ) = default;

        std::span<std::unique_ptr<OmfRecord> const> GetRecords( ) const noexcept {
            return _records;
        }

        template <typename T>
            requires std::derived_from<T, OmfRecord>
        auto GetRecordsOfType( ) const {
            return _records
                | std::views::transform( []( auto const& ptr ) { return ptr->template As<T>( ); } )
                | std::views::filter( []( T const* const ptr ) { return ptr != nullptr; } );
        }

        template <typename T>
            requires std::derived_from<T, OmfRecord>
        auto GetRecordsOfType( ) {
            return _records
                | std::views::transform( []( auto const& ptr ) { return ptr->template As<T>( ); } )
                | std::views::filter( []( T* const ptr ) { return ptr != nullptr; } );
        }

        void AddRecord( std::unique_ptr<OmfRecord> record );
        void InsertRecord( std::size_t const index, std::unique_ptr<OmfRecord> record );
        void RemoveRecord( std::size_t const index );

        // Returns the raw module name from THEADR, LHEADR, or RHEADR (if present)
        std::optional<std::string_view> GetModuleName( ) const noexcept;

        // Serializes this single module as a standalone .obj file (without .lib page padding)
        std::vector<std::byte> SerializeToMemory( OmfSaveOptions const& options = { } ) const;
        void                   SaveToStream( std::ostream& outputStream, OmfSaveOptions const& options = { } ) const;
        void                   SaveToFile( std::filesystem::path const& filePath, OmfSaveOptions const& options = { } ) const;

        // Inter-module page-alignment padding bytes (present when loaded from a .LIB archive)
        std::span<std::byte const> GetTrailingLibraryPadding( ) const noexcept {
            return _trailingLibraryPadding;
        }

        void SetTrailingLibraryPadding( std::vector<std::byte> const padding );

        // Resolves a 1-based NameIndex across all OmfLnamesRecord instances in this module
        std::optional<std::string_view> ResolveName( NameIndex const index ) const noexcept;

        std::size_t RenameSymbol( std::string_view const oldName, std::string_view const newName );

    private:

        std::vector<std::unique_ptr<OmfRecord>> _records;
        std::vector<std::byte>                  _trailingLibraryPadding;

    };

    class OmfFile {

    public:

        static OmfFile LoadFromFile( std::filesystem::path const& filePath, OmfLoadOptions const& options = { } );
        static OmfFile LoadFromStream( std::istream& inputStream, OmfLoadOptions const& options = { } );
        static OmfFile LoadFromMemory( std::span<std::byte const> const buffer, OmfLoadOptions const& options = { } );

        void                   SaveToFile( std::filesystem::path const& filePath, OmfSaveOptions const& options = { } ) const;
        void                   SaveToStream( std::ostream& outputStream, OmfSaveOptions const& options = { } ) const;
        std::vector<std::byte> SerializeToMemory( OmfSaveOptions const& options = { } ) const;

        // Flat access to all records in exact file order (including any library records)
        std::span<std::unique_ptr<OmfRecord> const> GetRecords( ) const noexcept {
            return _records;
        }

        // Grouped view of object modules (THEADR/LHEADR/RHEADR .. MODEND)
        std::vector<OmfModule> const& GetModules( ) const noexcept { return _modules; }
        std::vector<OmfModule>&       GetModules( ) noexcept       { return _modules; }

        // Optional trailing MS-DOS .LIB 512-byte hashed dictionary blocks (after 0xF1 MsLibEnd)
        std::span<std::byte const> GetTrailingLibraryDictionary( ) const noexcept {
            return _trailingLibraryDictionary;
        }

        void        SetTrailingLibraryDictionary( std::vector<std::byte> const dictionaryBytes );
        std::size_t RenameSymbol( std::string_view const oldName, std::string_view const newName );

    private:

        static std::unique_ptr<OmfRecord> _ParseSingleRecord(
            RecordType const                 type,
            std::span<std::byte const> const rawPayload,
            std::uint8_t const               originalChecksum,
            bool const                       hadValidChecksum,
            std::size_t const                fileOffset
        );

        std::vector<std::unique_ptr<OmfRecord>> _records;
        std::vector<OmfModule>                  _modules;
        std::vector<std::byte>                  _trailingLibraryDictionary;

    };

} // namespace Omf
