#pragma once

#include <cstdint>

#include <optional>
#include <string>
#include <string_view>

namespace Omf {

    //=========================================================================
    // Name demangling styles & options (MSC, Borland C++, OpenWatcom C++)
    //=========================================================================

    enum class DemangleStyle:
        std::uint8_t
    {
        None,       // Do not demangle; return symbol verbatim
        Auto,       // Auto-detect MSC, Borland, or OpenWatcom
        Msc,        // Microsoft C/C++ (MSC 7.00 / MSVC 1.x / 32-bit MSVC)
        Borland,    // Borland C++ / Turbo C++
        OpenWatcom  // Watcom / OpenWatcom C/C++
    };

    enum class DemangleArgResult:
        std::uint8_t
    {
        NotMatched,  // Argument does not begin with "--demangle"
        Parsed,      // Valid "--demangle" or "--demangle=msc|bc|ow"
        InvalidValue // Recognized "--demangle=..." prefix, but unknown style
    };

    struct DemangleOptions {
        DemangleStyle style              { DemangleStyle::Auto };
        bool          includeReturnType  { true                };
        bool          includeCallingConv { true                };
        bool          includeAccessSpec  { true                };
        bool          includeMemoryModel { true                };
    };

    struct DemangleResult {
        std::string   demangled;
        DemangleStyle detectedStyle { DemangleStyle::None };
        bool          wasDemangled  { false               };
    };

    // Demangles a symbol and reports which compiler scheme matched.
    // If style == None or the symbol is not recognized as mangled, returns the original symbol.
    DemangleResult DemangleSymbolDetailed( std::string_view const symbol, DemangleOptions const& options = { } );

    // Convenience wrappers returning either the demangled name or the original symbol.
    std::string DemangleSymbol( std::string_view const symbol, DemangleStyle const style = DemangleStyle::Auto );
    std::string DemangleSymbol( std::string_view const symbol, DemangleOptions const& options );

    // Heuristic detector used by DemangleStyle::Auto.
    DemangleStyle DetectManglingStyle( std::string_view const symbol ) noexcept;

    // String & CLI option helpers ("msc", "bc", "ow", "auto", "none").
    std::string_view             ToString( DemangleStyle const style ) noexcept;
    std::optional<DemangleStyle> ParseDemangleStyle( std::string_view const text ) noexcept;
    DemangleArgResult            ParseDemangleOption( std::string_view const arg, DemangleStyle& outStyle ) noexcept;

} // namespace Omf
