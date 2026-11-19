#include "OmfDemangle.h"

#include <cctype>
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Omf {

    namespace {

        constexpr std::size_t kMaxRecursionDepth { 64u };

        bool IsAsciiDigit( char const ch ) noexcept {
            return ( ch >= '0' ) && ( ch <= '9' );
        }

        bool IsAsciiIdentChar( char const ch ) noexcept {
            unsigned char const uc { static_cast<unsigned char>( ch ) };
            return ( std::isalnum( uc ) != 0 ) || ( ch == '_' ) || ( ch == '$' );
        }

        bool IsAllDigits( std::string_view const sv ) noexcept {
            if ( sv.empty( ) ) {
                return false;
            }
            for ( char const ch : sv ) {
                if ( !IsAsciiDigit( ch ) ) {
                    return false;
                }
            }
            return true;
        }

        std::string ToLowerAscii( std::string_view const sv ) {
            std::string out;
            out.reserve( sv.size( ) );
            for ( char const ch : sv ) {
                out.push_back( static_cast<char>( std::tolower( static_cast<unsigned char>( ch ) ) ) );
            }
            return out;
        }

        std::string JoinQualifiedScopesReversed( std::vector<std::string> const& scopes, std::string_view const leaf ) {
            std::string out;
            for ( auto it { scopes.rbegin( ) }; it != scopes.rend( ); ++it ) {
                out += *it;
                out += "::";
            }
            out += leaf;
            return out;
        }

        std::string JoinQualifiedScopesForward( std::vector<std::string> const& scopes, std::string_view const leaf ) {
            std::string out;
            for ( auto const& scope : scopes ) {
                out += scope;
                out += "::";
            }
            out += leaf;
            return out;
        }

        std::string AppendSpaceIfNeeded( std::string_view const prefix, std::string_view const suffix ) {
            if ( prefix.empty( ) ) {
                return std::string { suffix };
            }
            if ( suffix.empty( ) ) {
                return std::string { prefix };
            }
            if ( ( prefix.back( ) == ' ' ) || ( prefix.back( ) == '*' ) || ( prefix.back( ) == '&' ) ) {
                return std::string { prefix } + std::string { suffix };
            }
            return std::format( "{} {}", prefix, suffix );
        }

        //=====================================================================
        // Base cursor helper
        //=====================================================================

        class DemangleCursor {

        protected:

            explicit DemangleCursor( std::string_view const input, DemangleOptions const& options ) noexcept:
                _input   { input   },
                _pos     { 0       },
                _depth   { 0       },
                _options { options }
            {
                /*empty*/
            }

            bool AtEnd( ) const noexcept { return _pos >= _input.size( ); }

            char Peek( std::size_t const lookahead = 0 ) const noexcept {
                return ( ( _pos + lookahead ) < _input.size( ) ) ? _input[_pos + lookahead] : '\0';
            }

            char Get( ) noexcept {
                return ( _pos < _input.size( ) ) ? _input[_pos++] : '\0';
            }

            bool Consume( char const expected ) noexcept {
                if ( Peek( ) == expected ) {
                    ++_pos;
                    return true;
                }
                return false;
            }

            bool ConsumePrefix( std::string_view const prefix ) noexcept {
                if ( _input.substr( _pos ).starts_with( prefix ) ) {
                    _pos += prefix.size( );
                    return true;
                }
                return false;
            }

            bool EnterRecursion( ) noexcept {
                if ( _depth >= kMaxRecursionDepth ) {
                    return false;
                }
                ++_depth;
                return true;
            }

            void LeaveRecursion( ) noexcept {
                if ( _depth > 0 ) {
                    --_depth;
                }
            }

            std::string_view       _input;
            std::size_t            _pos;
            std::size_t            _depth;
            DemangleOptions const& _options;

        };

        //=====================================================================
        // 1. Microsoft C/C++ demangler (MSC 7.00 / MSVC 1.x / 32-bit MSVC)
        //=====================================================================

        class MscDemangler:
            private DemangleCursor
        {

        public:

            MscDemangler( std::string_view const input, DemangleOptions const& options ) noexcept:
                DemangleCursor { input, options }
            {
                /*empty*/
            }

            std::optional<std::string> Demangle( ) {
                if ( _input.empty( ) ) {
                    return std::nullopt;
                }

                // Check C calling convention decorations first:
                //   __stdcall:  _Name@Bytes
                //   __fastcall: @Name@Bytes
                if ( _input.front( ) != '?' ) {
                    return _TryDemangleCDecorated( );
                }

                Get( ); // consume '?'

                SpecialNameKind specialKind { SpecialNameKind::None };
                std::string     baseName;
                if ( !_ParseUnqualifiedName( baseName, specialKind ) ) {
                    return std::nullopt;
                }

                std::vector<std::string> scopes;
                if ( !_ParseScopeList( scopes ) ) {
                    return std::nullopt;
                }

                if ( specialKind == SpecialNameKind::Constructor ) {
                    baseName = scopes.empty( ) ? std::string{ "ctor" } : scopes.front( );
                } else if ( specialKind == SpecialNameKind::Destructor ) {
                    baseName = scopes.empty( ) ? std::string{ "~dtor" } : ( "~" + scopes.front( ) );
                }

                std::string const qualifiedName { JoinQualifiedScopesReversed( scopes, baseName ) };

                if ( AtEnd( ) ) {
                    return qualifiedName;
                }

                char const storageCode { Get( ) };

                // Data / variable symbols ('0'..'4')
                if ( ( storageCode >= '0' ) && ( storageCode <= '4' ) ) {
                    return _DemangleDataSymbol( storageCode, qualifiedName );
                }

                // Vftable / vbtable ('6', '7')
                if ( ( storageCode == '6' ) || ( storageCode == '7' ) ) {
                    return _DemangleSpecialTable( qualifiedName );
                }

                // Functions ('A'..'Z')
                if ( ( storageCode >= 'A' ) && ( storageCode <= 'Z' ) ) {
                    return _DemangleFunctionSymbol( storageCode, qualifiedName, specialKind );
                }

                return std::nullopt;
            }

        private:

            enum class SpecialNameKind:
                std::uint8_t
            {
                None,
                Constructor,
                Destructor,
                ConversionOperator,
                OtherSpecial
            };

            std::optional<std::string> _TryDemangleCDecorated( ) const {
                if ( _input.size( ) < 4u ) {
                    return std::nullopt;
                }

                bool const isStdcall  { _input.front( ) == '_' };
                bool const isFastcall { _input.front( ) == '@' };
                if ( !isStdcall && !isFastcall ) {
                    return std::nullopt;
                }

                std::size_t const atPos { _input.find( '@', 1u ) };
                if ( ( atPos == std::string_view::npos ) || ( atPos == 1u ) ) {
                    return std::nullopt;
                }

                // Must not contain another '@' or '$'
                if ( _input.find( '@', atPos + 1u ) != std::string_view::npos ) {
                    return std::nullopt;
                }

                std::string_view const  namePart { _input.substr( 1u, atPos - 1u ) };
                std::string_view const bytesPart { _input.substr(     atPos + 1u ) };
                if ( !IsAllDigits( bytesPart ) ) {
                    return std::nullopt;
                }

                for ( char const ch : namePart ) {
                    if ( !IsAsciiIdentChar( ch ) ) {
                        return std::nullopt;
                    }
                }

                if ( _options.includeCallingConv ) {
                    return std::format( "{} {}", isStdcall ? "__stdcall" : "__fastcall", namePart );
                }
                return std::string{ namePart };
            }

            void _AddNameBackref( std::string const& name ) {
                if ( _nameBackrefs.size( ) < 10u ) {
                    _nameBackrefs.push_back( name );
                }
            }

            void _AddTypeBackref( std::string const& typeName ) {
                if ( _typeBackrefs.size( ) < 10u ) {
                    _typeBackrefs.push_back( typeName );
                }
            }

            bool _ReadIdentifier( std::string& outName ) {
                if ( AtEnd( ) ) {
                    return false;
                }
                char const ch { Peek( ) };
                if ( IsAsciiDigit( ch ) ) {
                    std::size_t const idx { static_cast<std::size_t>( Get( ) - '0' ) };
                    if ( idx >= _nameBackrefs.size( ) ) {
                        return false;
                    }
                    outName = _nameBackrefs[idx];
                    return true;
                }

                // Template instantiation name: ?$Name@Args@
                if ( ( ch == '?' ) && ( Peek( 1 ) == '$' ) ) {
                    _pos += 2u;
                    return _ParseTemplateName( outName );
                }

                std::size_t const start { _pos };
                while ( !AtEnd( ) && ( Peek( ) != '@' ) ) {
                    Get( );
                }
                if ( ( _pos == start ) || !Consume( '@' ) ) {
                    return false;
                }
                outName.assign( _input.substr( start, _pos - start - 1u ) );
                _AddNameBackref( outName );
                return true;
            }

            bool _ParseTemplateName( std::string& outName ) {
                if ( !EnterRecursion( ) ) {
                    return false;
                }

                std::size_t const start { _pos };
                while ( !AtEnd( ) && ( Peek( ) != '@' ) ) {
                    Get( );
                }
                if ( ( _pos == start ) || !Consume( '@' ) ) {
                    LeaveRecursion( );
                    return false;
                }

                std::string tmplName { _input.substr( start, _pos - start - 1u ) };
                std::vector<std::string> args;
                while ( !AtEnd( ) && ( Peek( ) != '@' ) ) {
                    if ( Peek( ) == '$' ) {
                        Get( );
                        char const kind { Get( ) };
                        if ( kind == '0' ) {
                            std::optional<std::int64_t> const num { _ParseEncodedNumber( ) };
                            if ( !num.has_value( ) ) {
                                LeaveRecursion( );
                                return false;
                            }
                            args.push_back( std::to_string( *num ) );
                            continue;
                        }
                        LeaveRecursion( );
                        return false;
                    }

                    std::string argType;
                    if ( !_ParseType( argType, false ) ) {
                        LeaveRecursion( );
                        return false;
                    }
                    args.push_back( std::move( argType ) );
                }

                if ( !Consume( '@' ) ) {
                    LeaveRecursion( );
                    return false;
                }

                outName = tmplName + "<";
                for ( std::size_t i { }; i < args.size( ); ++i ) {
                    if ( i > 0u ) {
                        outName += ", ";
                    }
                    outName += args[i];
                }
                outName += ">";
                _AddNameBackref( outName );
                LeaveRecursion( );
                return true;
            }

            bool _ParseUnqualifiedName( std::string& outName, SpecialNameKind& outSpecialKind ) {
                outSpecialKind = SpecialNameKind::None;
                if ( AtEnd( ) ) {
                    return false;
                }

                if ( Peek( ) == '?' ) {
                    if ( Peek( 1 ) == '$' ) {
                        _pos += 2u;
                        return _ParseTemplateName( outName );
                    }
                    Get( ); // consume '?'
                    if ( AtEnd( ) ) {
                        return false;
                    }
                    char const code { Get( ) };
                    if ( code == '_' ) {
                        if ( AtEnd( ) ) {
                            return false;
                        }
                        char const extCode { Get( ) };
                        outSpecialKind = SpecialNameKind::OtherSpecial;
                        switch ( extCode ) {
                            case '0': outName = "operator/=";                             return true;
                            case '1': outName = "operator%=";                             return true;
                            case '2': outName = "operator>>=";                            return true;
                            case '3': outName = "operator<<=";                            return true;
                            case '4': outName = "operator&=";                             return true;
                            case '5': outName = "operator|=";                             return true;
                            case '6': outName = "operator^=";                             return true;
                            case '7': outName = "`vftable'";                              return true;
                            case '8': outName = "`vbtable'";                              return true;
                            case '9': outName = "`vcall'";                                return true;
                            case 'A': outName = "`typeof'";                               return true;
                            case 'B': outName = "`local static guard'";                   return true;
                            case 'C': outName = "`string'";                               return true;
                            case 'D': outName = "`vbase destructor'";                     return true;
                            case 'E': outName = "`vector deleting destructor'";           return true;
                            case 'F': outName = "`default constructor closure'";          return true;
                            case 'G': outName = "`scalar deleting destructor'";           return true;
                            case 'H': outName = "`vector constructor iterator'";          return true;
                            case 'I': outName = "`vector destructor iterator'";           return true;
                            case 'J': outName = "`vector vbase constructor iterator'";    return true;
                            case 'K': outName = "`virtual displacement map'";             return true;
                            case 'L': outName = "`eh vector constructor iterator'";       return true;
                            case 'M': outName = "`eh vector destructor iterator'";        return true;
                            case 'N': outName = "`eh vector vbase constructor iterator'"; return true;
                            case 'O': outName = "`copy constructor closure'";             return true;
                            case 'U': outName = "operator new[]";                         return true;
                            case 'V': outName = "operator delete[]";                      return true;
                            default:  return false;
                        }
                    }

                    outSpecialKind = SpecialNameKind::OtherSpecial;
                    switch ( code ) {
                        case '0': outSpecialKind = SpecialNameKind::Constructor;        return true;
                        case '1': outSpecialKind = SpecialNameKind::Destructor;         return true;
                        case 'B': outSpecialKind = SpecialNameKind::ConversionOperator; return true;
                        case '2': outName = "operator new";                             return true;
                        case '3': outName = "operator delete";                          return true;
                        case '4': outName = "operator=";                                return true;
                        case '5': outName = "operator>>";                               return true;
                        case '6': outName = "operator<<";                               return true;
                        case '7': outName = "operator!";                                return true;
                        case '8': outName = "operator==";                               return true;
                        case '9': outName = "operator!=";                               return true;
                        case 'A': outName = "operator[]";                               return true;
                        case 'C': outName = "operator->";                               return true;
                        case 'D': outName = "operator*";                                return true;
                        case 'E': outName = "operator++";                               return true;
                        case 'F': outName = "operator--";                               return true;
                        case 'G': outName = "operator-";                                return true;
                        case 'H': outName = "operator+";                                return true;
                        case 'I': outName = "operator&";                                return true;
                        case 'J': outName = "operator->*";                              return true;
                        case 'K': outName = "operator/";                                return true;
                        case 'L': outName = "operator%";                                return true;
                        case 'M': outName = "operator<";                                return true;
                        case 'N': outName = "operator<=";                               return true;
                        case 'O': outName = "operator>";                                return true;
                        case 'P': outName = "operator>=";                               return true;
                        case 'Q': outName = "operator,";                                return true;
                        case 'R': outName = "operator()";                               return true;
                        case 'S': outName = "operator~";                                return true;
                        case 'T': outName = "operator^";                                return true;
                        case 'U': outName = "operator|";                                return true;
                        case 'V': outName = "operator&&";                               return true;
                        case 'W': outName = "operator||";                               return true;
                        case 'X': outName = "operator*=";                               return true;
                        case 'Y': outName = "operator+=";                               return true;
                        case 'Z': outName = "operator-=";                               return true;
                        default:  return false;
                    }
                }

                return _ReadIdentifier( outName );
            }

            bool _ParseScopeList( std::vector<std::string>& outScopes ) {
                while ( !AtEnd( ) ) {
                    if ( Consume( '@' ) ) {
                        return true;
                    }
                    // Numbered namespace / local scope (?A0x...@ or ?<num>?)
                    if ( ( Peek( ) == '?' ) && ( Peek( 1 ) != '$' ) ) {
                        Get( );
                        std::string localScope;
                        if ( Peek( ) == 'A' ) {
                            while ( !AtEnd( ) && ( Peek( ) != '@' ) ) {
                                Get( );
                            }
                            if ( !Consume( '@' ) ) {
                                return false;
                            }
                            outScopes.emplace_back( "`anonymous namespace'" );
                            continue;
                        }
                        if ( auto const num { _ParseEncodedNumber( ) }; num.has_value( ) ) {
                            outScopes.push_back( std::format( "`{}'", *num ) );
                            continue;
                        }
                        return false;
                    }
                    std::string part;
                    if ( !_ReadIdentifier( part ) ) {
                        return false;
                    }
                    outScopes.push_back( std::move( part ) );
                }
                return false;
            }

            std::optional<std::int64_t> _ParseEncodedNumber( ) {
                if ( AtEnd( ) ) {
                    return std::nullopt;
                }
                bool isNegative { false };
                if ( Consume( '?' ) ) {
                    isNegative = true;
                }
                if ( AtEnd( ) ) {
                    return std::nullopt;
                }
                char const first { Peek( ) };
                if ( IsAsciiDigit( first ) ) {
                    Get( );
                    std::int64_t const val { static_cast<std::int64_t>( first - '0' ) + 1 };
                    return isNegative ? -val : val;
                }
                std::int64_t value { };
                while ( !AtEnd( ) && ( Peek( ) != '@' ) ) {
                    char const ch { Get( ) };
                    if ( ( ch < 'A' ) || ( ch > 'P' ) ) {
                        return std::nullopt;
                    }
                    value = ( value << 4 ) | static_cast<std::int64_t>( ch - 'A' );
                }
                if ( !Consume( '@' ) ) {
                    return std::nullopt;
                }
                return isNegative ? -value : value;
            }

            static std::string_view _DecodeCallingConv( char const ch ) noexcept {
                switch ( ch ) {
                    case 'A': return "__cdecl";
                    case 'B': return "__cdecl";
                    case 'C': return "__pascal";
                    case 'D': return "__pascal";
                    case 'E': return "__thiscall";
                    case 'F': return "__thiscall";
                    case 'G': return "__stdcall";
                    case 'H': return "__stdcall";
                    case 'I': return "__fastcall";
                    case 'J': return "__fastcall";
                    case 'K': return "__interrupt";
                    default:  return "";
                }
            }

            void _DecodeCvModel( char const ch, std::string& outCv, std::string& outModel ) const {
                // MSC 16-bit & 32-bit storage/this qualifiers:
                //   A..D = near (none / const / volatile / const volatile)
                //   E..H = far  (none / const / volatile / const volatile)
                //   I..L = huge (none / const / volatile / const volatile)
                unsigned const idx {
                    ( ( ch >= 'A' ) && ( ch <= 'L' ) )
                        ? static_cast<unsigned>( ch - 'A' )
                        : 0u
                };
                unsigned const cvBits    { idx & 0x03u };
                unsigned const modelBits { idx >> 2u   };

                if ( ( cvBits & 0x01u ) != 0u ) {
                    outCv += "const";
                }
                if ( ( cvBits & 0x02u ) != 0u ) {
                    if ( !outCv.empty( ) ) {
                        outCv += " ";
                    }
                    outCv += "volatile";
                }

                if ( _options.includeMemoryModel ) {
                    if ( modelBits == 1u ) {
                        outModel = "__far";
                    } else if ( modelBits == 2u ) {
                        outModel = "__huge";
                    }
                }
            }

            std::optional<std::string> _DemangleDataSymbol( char const storageCode, std::string const& qualifiedName ) {
                std::string typeStr;
                if ( !_ParseType( typeStr, false ) ) {
                    return std::nullopt;
                }

                std::string cvStr;
                std::string modelStr;
                if ( !AtEnd( ) ) {
                    _DecodeCvModel( Get( ), cvStr, modelStr );
                }

                std::string result;
                if ( _options.includeAccessSpec ) {
                    switch ( storageCode ) {
                        case '0': result += "private: static ";   break;
                        case '1': result += "protected: static "; break;
                        case '2': result += "public: static ";    break;
                        case '4': result += "static ";            break;
                        default:  break;
                    }
                }
                result += typeStr;
                if ( !cvStr.empty( ) ) {
                    result = AppendSpaceIfNeeded( result, cvStr );
                }
                if ( !modelStr.empty( ) ) {
                    result = AppendSpaceIfNeeded( result, modelStr );
                }
                result = AppendSpaceIfNeeded( result, qualifiedName );
                return result;
            }

            std::optional<std::string> _DemangleSpecialTable( std::string const& qualifiedName ) {
                std::string cvStr;
                std::string modelStr;
                if ( !AtEnd( ) ) {
                    _DecodeCvModel( Get( ), cvStr, modelStr );
                }

                std::string result;
                if ( !cvStr.empty( ) ) {
                    result += cvStr + " ";
                }
                if ( !modelStr.empty( ) ) {
                    result += modelStr + " ";
                }
                result += qualifiedName;
                return result;
            }

            std::optional<std::string> _DemangleFunctionSymbol(
                char const            storageCode,
                std::string const&    qualifiedName,
                SpecialNameKind const specialKind
            ) {
                bool const isFarFunc { ( ( storageCode - 'A' ) & 1 ) != 0 };
                bool isMemberNonStatic { false };
                bool isThunk           { false };

                std::string accessPrefix;
                switch ( storageCode ) {
                    case 'A': case 'B': accessPrefix = "private:";           isMemberNonStatic = true;                 break;
                    case 'C': case 'D': accessPrefix = "private: static";                                              break;
                    case 'E': case 'F': accessPrefix = "private: virtual";   isMemberNonStatic = true;                 break;
                    case 'G': case 'H': accessPrefix = "private: [thunk]";   isMemberNonStatic = true; isThunk = true; break;
                    case 'I': case 'J': accessPrefix = "protected:";         isMemberNonStatic = true;                 break;
                    case 'K': case 'L': accessPrefix = "protected: static";                                            break;
                    case 'M': case 'N': accessPrefix = "protected: virtual"; isMemberNonStatic = true;                 break;
                    case 'O': case 'P': accessPrefix = "protected: [thunk]"; isMemberNonStatic = true; isThunk = true; break;
                    case 'Q': case 'R': accessPrefix = "public:";            isMemberNonStatic = true;                 break;
                    case 'S': case 'T': accessPrefix = "public: static";                                               break;
                    case 'U': case 'V': accessPrefix = "public: virtual";    isMemberNonStatic = true;                 break;
                    case 'W': case 'X': accessPrefix = "public: [thunk]";    isMemberNonStatic = true; isThunk = true; break;
                    case 'Y': case 'Z': break;
                    default: return std::nullopt;
                }

                if ( isThunk ) {
                    if ( !_ParseEncodedNumber( ).has_value( ) ) {
                        return std::nullopt;
                    }
                }

                std::string thisCv;
                std::string thisModel;
                if ( isMemberNonStatic ) {
                    if ( AtEnd( ) ) {
                        return std::nullopt;
                    }
                    _DecodeCvModel( Get( ), thisCv, thisModel );
                }

                if ( AtEnd( ) ) {
                    return std::nullopt;
                }
                std::string_view const callingConv { _DecodeCallingConv( Get( ) ) };

                // Return type ('@' means none, e.g. constructor/destructor)
                std::string returnType;
                if ( !Consume( '@' ) ) {
                    if ( !_ParseType( returnType, false ) ) {
                        return std::nullopt;
                    }
                }

                // Parameter list
                std::string paramList;
                if ( !_ParseParamList( paramList ) ) {
                    return std::nullopt;
                }

                // Trailing throw spec ('Z' = none)
                Consume( 'Z' );

                // Assemble full signature
                std::string displayName { qualifiedName };
                if ( specialKind == SpecialNameKind::ConversionOperator ) {
                    std::string const opName { "operator " + returnType };
                    displayName = qualifiedName + opName;
                    returnType.clear( );
                }

                std::string out;
                if ( _options.includeAccessSpec && !accessPrefix.empty( ) ) {
                    out += accessPrefix + " ";
                }
                if ( _options.includeReturnType && !returnType.empty( ) ) {
                    out += returnType + " ";
                }
                if ( _options.includeMemoryModel && isFarFunc ) {
                    out += "__far ";
                }
                if ( _options.includeCallingConv && !callingConv.empty( ) ) {
                    out += std::string{ callingConv } + " ";
                }
                out += displayName;
                out += "(";
                out += paramList;
                out += ")";

                if ( !thisCv.empty( ) ) {
                    out += " " + thisCv;
                }
                if ( _options.includeMemoryModel && !thisModel.empty( ) ) {
                    out += " " + thisModel;
                }

                return out;
            }

            bool _ParseParamList( std::string& outParams ) {
                if ( AtEnd( ) ) {
                    return false;
                }
                if ( Consume( 'X' ) ) {
                    outParams = "void";
                    return true;
                }

                std::vector<std::string> params;
                while ( !AtEnd( ) ) {
                    if ( Consume( '@' ) ) {
                        break;
                    }
                    if ( Consume( 'Z' ) ) {
                        params.emplace_back( "..." );
                        break;
                    }

                    std::string paramType;
                    if ( !_ParseType( paramType, true ) ) {
                        return false;
                    }
                    params.push_back( std::move( paramType ) );
                }

                outParams.clear( );
                for ( std::size_t i { }; i < params.size( ); ++i ) {
                    if ( i > 0u ) {
                        outParams += ", ";
                    }
                    outParams += params[i];
                }
                return true;
            }

            bool _ParseTagType( std::string_view const keyword, std::string& outType ) {
                std::string leaf;
                if ( !_ReadIdentifier( leaf ) ) {
                    return false;
                }
                std::vector<std::string> scopes;
                if ( !_ParseScopeList( scopes ) ) {
                    return false;
                }
                outType = std::format( "{} {}", keyword, JoinQualifiedScopesReversed( scopes, leaf ) );
                return true;
            }

            bool _ParsePointerOrRef( char const code, std::string& outType ) {
                bool const isRef { ( code == 'A' ) || ( code == 'B' ) };
                std::string ptrTopQual;
                if ( ( code == 'Q' ) || ( code == 'B' ) ) {
                    ptrTopQual = "const";
                } else if ( code == 'R' ) {
                    ptrTopQual = "volatile";
                } else if ( code == 'S' ) {
                    ptrTopQual = "const volatile";
                }

                if ( AtEnd( ) ) {
                    return false;
                }

                // Function pointer: P6<cc><ret><params>@Z (near) or P7... (far)
                if ( !isRef && ( ( Peek( ) == '6' ) || ( Peek( ) == '7' ) ) ) {
                    bool const isFarFnPtr { Get( ) == '7' };
                    if ( AtEnd( ) ) {
                        return false;
                    }
                    std::string_view const cc { _DecodeCallingConv( Get( ) ) };
                    std::string retType;
                    if ( !_ParseType( retType, false ) ) {
                        return false;
                    }
                    std::string params;
                    if ( !_ParseParamList( params ) ) {
                        return false;
                    }
                    Consume( 'Z' );

                    std::string ptrSpec;
                    if ( _options.includeMemoryModel && isFarFnPtr ) {
                        ptrSpec += "__far ";
                    }
                    if ( _options.includeCallingConv && !cc.empty( ) ) {
                        ptrSpec += std::string{ cc } + " ";
                    }
                    ptrSpec += "*";
                    if ( !ptrTopQual.empty( ) ) {
                        ptrSpec += " " + ptrTopQual;
                    }
                    outType = std::format( "{} ({})({})", retType, ptrSpec, params );
                    return true;
                }

                // Pointer-to-member-function: P8<class>@<this-qual><cc><ret><params>@Z
                if ( !isRef && ( Peek( ) == '8' ) ) {
                    Get( );
                    std::string clsLeaf;
                    if ( !_ReadIdentifier( clsLeaf ) ) {
                        return false;
                    }
                    std::vector<std::string> clsScopes;
                    if ( !_ParseScopeList( clsScopes ) ) {
                        return false;
                    }
                    std::string const clsName { JoinQualifiedScopesReversed( clsScopes, clsLeaf ) };

                    std::string thisCv;
                    std::string thisModel;
                    if ( AtEnd( ) ) {
                        return false;
                    }
                    _DecodeCvModel( Get( ), thisCv, thisModel );
                    if ( AtEnd( ) ) {
                        return false;
                    }
                    std::string_view const cc { _DecodeCallingConv( Get( ) ) };
                    std::string retType;
                    if ( !_ParseType( retType, false ) ) {
                        return false;
                    }
                    std::string params;
                    if ( !_ParseParamList( params ) ) {
                        return false;
                    }
                    Consume( 'Z' );

                    std::string ptrSpec;
                    if ( _options.includeCallingConv && !cc.empty( ) ) {
                        ptrSpec += std::string{ cc } + " ";
                    }
                    ptrSpec += clsName + "::*";
                    outType = std::format( "{} ({})({})", retType, ptrSpec, params );
                    if ( !thisCv.empty( ) ) {
                        outType += " " + thisCv;
                    }
                    return true;
                }

                std::string targetCv;
                std::string targetModel;
                _DecodeCvModel( Get( ), targetCv, targetModel );

                std::string pointee;
                if ( !_ParseType( pointee, false ) ) {
                    return false;
                }

                outType = pointee;
                if ( !targetCv.empty( ) ) {
                    outType = AppendSpaceIfNeeded( outType, targetCv );
                }
                if ( !targetModel.empty( ) ) {
                    outType = AppendSpaceIfNeeded( outType, targetModel );
                }
                outType = AppendSpaceIfNeeded( outType, isRef ? "&" : "*" );
                if ( !ptrTopQual.empty( ) ) {
                    outType += " " + ptrTopQual;
                }
                return true;
            }

            bool _ParseType( std::string& outType, bool const recordParamBackref ) {
                if ( AtEnd( ) || !EnterRecursion( ) ) {
                    return false;
                }

                char const ch { Get( ) };
                if ( IsAsciiDigit( ch ) ) {
                    std::size_t const idx { static_cast<std::size_t>( ch - '0' ) };
                    LeaveRecursion( );
                    if ( idx >= _typeBackrefs.size( ) ) {
                        return false;
                    }
                    outType = _typeBackrefs[idx];
                    return true;
                }

                bool ok          { true  };
                bool isCompound  { false };

                switch ( ch ) {
                    case 'C': outType =   "signed char";        break;
                    case 'D': outType =          "char";        break;
                    case 'E': outType = "unsigned char";        break;
                    case 'F': outType =          "short";       break;
                    case 'G': outType = "unsigned short";       break;
                    case 'H': outType =          "int";         break;
                    case 'I': outType = "unsigned int";         break;
                    case 'J': outType =          "long";        break;
                    case 'K': outType = "unsigned long";        break;
                    case 'L': outType =          "__segment";   break;
                    case 'M': outType =          "float";       break;
                    case 'N': outType =          "double";      break;
                    case 'O': outType =          "long double"; break;
                    case 'X': outType =          "void";        break;
                    case 'Z': outType =          "...";         break;

                    case '_': {
                        if ( AtEnd( ) ) {
                            ok = false;
                            break;
                        }
                        char const ext { Get( ) };
                        switch ( ext ) {
                            case 'D': outType =          "__int8";  break;
                            case 'E': outType = "unsigned __int8";  break;
                            case 'F': outType =          "__int16"; break;
                            case 'G': outType = "unsigned __int16"; break;
                            case 'H': outType =          "__int32"; break;
                            case 'I': outType = "unsigned __int32"; break;
                            case 'J': outType =          "__int64"; break;
                            case 'K': outType = "unsigned __int64"; break;
                            case 'N': outType =          "bool";    break;
                            case 'W': outType =          "wchar_t"; break;
                            default:  ok = false;                   break;
                        }
                        break;
                    }

                    case 'P':
                    case 'Q':
                    case 'R':
                    case 'S':
                    case 'A':
                    case 'B':
                        isCompound = true;
                        ok = _ParsePointerOrRef( ch, outType );
                        break;

                    case 'U':
                        isCompound = true;
                        ok = _ParseTagType( "struct", outType );
                        break;

                    case 'V':
                        isCompound = true;
                        ok = _ParseTagType( "class", outType );
                        break;

                    case 'T':
                        isCompound = true;
                        ok = _ParseTagType( "union", outType );
                        break;

                    case 'W':
                        isCompound = true;
                        if ( AtEnd( ) ) {
                            ok = false;
                        } else {
                            Get( ); // underlying enum type digit ('0'..'7')
                            ok = _ParseTagType( "enum", outType );
                        }
                        break;

                    case 'Y': {
                        isCompound = true;
                        auto const dimCount { _ParseEncodedNumber( ) };
                        if ( !dimCount.has_value( ) || ( *dimCount <= 0 ) || ( *dimCount > 16 ) ) {
                            ok = false;
                            break;
                        }
                        std::string dims;
                        for ( std::int64_t d { }; d < *dimCount; ++d ) {
                            auto const dim { _ParseEncodedNumber( ) };
                            if ( !dim.has_value( ) ) {
                                ok = false;
                                break;
                            }
                            dims += std::format( "[{}]", *dim );
                        }
                        if ( ok ) {
                            std::string elemType;
                            ok = _ParseType( elemType, false );
                            if ( ok ) {
                                outType = elemType + dims;
                            }
                        }
                        break;
                    }

                    case '?': {
                        // Top-level cv-qualified parameter/return type: ?<cv><type>
                        if ( AtEnd( ) ) {
                            ok = false;
                            break;
                        }
                        std::string cvStr;
                        std::string modelStr;
                        _DecodeCvModel( Get( ), cvStr, modelStr );
                        std::string inner;
                        ok = _ParseType( inner, false );
                        if ( ok ) {
                            outType = inner;
                            if ( !cvStr.empty( ) ) {
                                outType = AppendSpaceIfNeeded( outType, cvStr );
                            }
                            if ( !modelStr.empty( ) ) {
                                outType = AppendSpaceIfNeeded( outType, modelStr );
                            }
                        }
                        break;
                    }

                    default:
                        ok = false;
                        break;
                }

                LeaveRecursion( );
                if ( ok && recordParamBackref && isCompound ) {
                    _AddTypeBackref( outType );
                }
                return ok;
            }

            std::vector<std::string> _nameBackrefs;
            std::vector<std::string> _typeBackrefs;

        };

        //=====================================================================
        // 2. Borland C++ demangler (Turbo C++ 3.0 / BC++ 3.1 / 4.x / 5.x)
        //=====================================================================

        class BorlandDemangler:
            private DemangleCursor
        {

        public:

            BorlandDemangler( std::string_view const input, DemangleOptions const& options ) noexcept:
                DemangleCursor ( input, options )
            {
                /*empty*/
            }

            std::optional<std::string> Demangle( ) {
                if ( ( _input.size( ) < 2u ) || ( _input.front( ) != '@' ) ) {
                    return std::nullopt;
                }

                Get( ); // consume leading '@'

                // Split outer-to-inner scopes separated by '@' up to the function signature '$'
                // (note: special names like $bctr/$bdtr/$o... and template blocks %...% contain '$')
                std::vector<std::string> parts;
                bool                     hasSignature { false };

                while ( !AtEnd( ) ) {
                    std::string part;
                    if ( Peek( ) == '%' ) {
                        if ( !_ParseTemplateSegment( part ) ) {
                            return std::nullopt;
                        }
                    } else if ( Peek( ) == '$' ) {
                        // Could be a special name ($bctr, $bdtr, $o...) if it's the final name part
                        std::size_t const start { _pos };
                        Get( ); // consume '$'
                        while ( !AtEnd( ) && ( Peek( ) != '$' ) && ( Peek( ) != '@' ) ) {
                            Get( );
                        }
                        part.assign( _input.substr( start, _pos - start ) );
                    } else {
                        std::size_t const start { _pos };
                        while ( !AtEnd( ) && ( Peek( ) != '@' ) && ( Peek( ) != '$' ) ) {
                            Get( );
                        }
                        part.assign( _input.substr( start, _pos - start ) );
                    }

                    if ( part.empty( ) ) {
                        return std::nullopt;
                    }
                    parts.push_back( std::move( part ) );

                    if ( Consume( '@' ) ) {
                        continue;
                    }
                    if ( Consume( '$' ) ) {
                        hasSignature = true;
                        break;
                    }
                    break;
                }

                if ( parts.empty( ) ) {
                    return std::nullopt;
                }

                // A bare "@name" with no second '@' and no '$' is not a Borland C++ mangled symbol
                if ( !hasSignature && ( parts.size( ) < 2u ) ) {
                    return std::nullopt;
                }

                std::string leaf { std::move( parts.back( ) ) };
                parts.pop_back( );

                std::string const enclosingClass { parts.empty( ) ? std::string{ } : parts.back( ) };
                std::string       displayLeaf;
                if ( !leaf.empty( ) && ( leaf.front( ) == '$' ) ) {
                    if ( !_DecodeSpecialName( leaf, enclosingClass, displayLeaf ) ) {
                        return std::nullopt;
                    }
                } else {
                    displayLeaf = std::move( leaf );
                }

                std::string const qualifiedName { JoinQualifiedScopesForward( parts, displayLeaf ) };

                if ( !hasSignature ) {
                    return qualifiedName;
                }

                // Parse optional member cv-qualifiers before 'q' ('x' = const, 'w' = volatile)
                std::string memberCv;
                while ( !AtEnd( ) && ( ( Peek( ) == 'x' ) || ( Peek( ) == 'w' ) ) ) {
                    char const q { Get( ) };
                    if ( !memberCv.empty( ) ) {
                        memberCv += " ";
                    }
                    memberCv += ( q == 'x' ) ? "const" : "volatile";
                }

                if ( !Consume( 'q' ) ) {
                    return std::nullopt;
                }

                std::string_view callingConv;
                if ( Consume( 'q' ) ) {
                    // Extended calling convention ($qqr = fastcall, $qqs = stdcall, $qqp = pascal, $qqf = fortran, $qqc = cdecl)
                    if ( AtEnd( ) ) {
                        return std::nullopt;
                    }
                    switch ( Get( ) ) {
                        case 'r': callingConv = "__fastcall"; break;
                        case 's': callingConv = "__stdcall";  break;
                        case 'p': callingConv = "__pascal";   break;
                        case 'f': callingConv = "__fortran";  break;
                        case 'c': callingConv = "__cdecl";    break;
                        default:  return std::nullopt;
                    }
                }

                std::vector<std::string> params;
                if ( !_ParseParamSequence( params, '\0' ) ) {
                    return std::nullopt;
                }

                std::string out;
                if ( _options.includeCallingConv && !callingConv.empty( ) ) {
                    out += std::string{ callingConv } + " ";
                }
                out += qualifiedName;
                out += "(";
                for ( std::size_t i { }; i < params.size( ); ++i ) {
                    if ( i > 0u ) {
                        out += ", ";
                    }
                    out += params[i];
                }
                out += ")";
                if ( !memberCv.empty( ) ) {
                    out += " " + memberCv;
                }

                return out;
            }

        private:

            bool _ParseTemplateSegment( std::string& outName ) {
                if ( !Consume( '%' ) || !EnterRecursion( ) ) {
                    return false;
                }
                std::size_t const start { _pos };
                while ( !AtEnd( ) && ( Peek( ) != '$' ) && ( Peek( ) != '%' ) ) {
                    Get( );
                }
                std::string base { _input.substr( start, _pos - start ) };
                if ( base.empty( ) ) {
                    LeaveRecursion( );
                    return false;
                }
                if ( Consume( '%' ) ) {
                    outName = std::move( base );
                    LeaveRecursion( );
                    return true;
                }
                if ( !Consume( '$' ) ) {
                    LeaveRecursion( );
                    return false;
                }

                std::vector<std::string> args;
                while ( !AtEnd( ) && ( Peek( ) != '%' ) ) {
                    if ( Peek( ) == 'i' && IsAsciiDigit( Peek( 1 ) ) ) {
                        Get( ); // non-type integer template arg
                        std::size_t const numStart { _pos };
                        while ( !AtEnd( ) && IsAsciiDigit( Peek( ) ) ) {
                            Get( );
                        }
                        args.emplace_back( _input.substr( numStart, _pos - numStart ) );
                        Consume( '$' );
                        continue;
                    }
                    std::string argType;
                    if ( !_ParseType( argType ) ) {
                        LeaveRecursion( );
                        return false;
                    }
                    args.push_back( std::move( argType ) );
                    Consume( '$' );
                }

                if ( !Consume( '%' ) ) {
                    LeaveRecursion( );
                    return false;
                }

                outName = base + "<";
                for ( std::size_t i { }; i < args.size( ); ++i ) {
                    if ( i > 0u ) {
                        outName += ", ";
                    }
                    outName += args[i];
                }
                outName += ">";
                LeaveRecursion( );
                return true;
            }

            bool _DecodeSpecialName( std::string_view const special, std::string_view const enclosingClass, std::string& outName ) {
                if ( ( special == "$bctr" ) || ( special == "$bctr1" ) || ( special == "$bctr2" ) ) {
                    outName = enclosingClass.empty( ) ? std::string{ "ctor" } : std::string{ enclosingClass };
                    return true;
                }
                if ( special == "$bdtr" ) {
                    outName = enclosingClass.empty( ) ? std::string{ "~dtor" } : ( "~" + std::string{ enclosingClass } );
                    return true;
                }

                // Conversion operator: $o<type> or $bo<type>
                if ( special.starts_with( "$o" ) || special.starts_with( "$bo" ) ) {
                    std::string_view const typeSub {
                        special.starts_with( "$bo" ) ? special.substr( 3u ) : special.substr( 2u )
                    };
                    BorlandDemangler sub( typeSub, _options );
                    std::string convType;
                    if ( !sub._ParseType( convType ) ) {
                        return false;
                    }
                    outName = "operator " + convType;
                    return true;
                }

                struct OpEntry {
                    std::string_view code;
                    std::string_view name;
                };

                static constexpr std::array<OpEntry, 40> kOps {{
                    { "$bnew",  "operator new"      },
                    { "$bdele", "operator delete"   },
                    { "$bnwa",  "operator new[]"    },
                    { "$bdla",  "operator delete[]" },
                    { "$basg",  "operator="         },
                    { "$badd",  "operator+"         },
                    { "$bsub",  "operator-"         },
                    { "$bmul",  "operator*"         },
                    { "$bdiv",  "operator/"         },
                    { "$bmod",  "operator%"         },
                    { "$band",  "operator&"         },
                    { "$bor",   "operator|"         },
                    { "$bxor",  "operator^"         },
                    { "$bcmp",  "operator~"         },
                    { "$bnot",  "operator!"         },
                    { "$beql",  "operator=="        },
                    { "$bneq",  "operator!="        },
                    { "$blss",  "operator<"         },
                    { "$bleq",  "operator<="        },
                    { "$bgtr",  "operator>"         },
                    { "$bgeq",  "operator>="        },
                    { "$blsh",  "operator<<"        },
                    { "$brsh",  "operator>>"        },
                    { "$binc",  "operator++"        },
                    { "$bdec",  "operator--"        },
                    { "$bsubs", "operator[]"        },
                    { "$bcall", "operator()"        },
                    { "$barow", "operator->"        },
                    { "$bcoma", "operator,"         },
                    { "$bplu",  "operator+="        },
                    { "$bmin",  "operator-="        },
                    { "$brmu",  "operator*="        },
                    { "$brdv",  "operator/="        },
                    { "$brmd",  "operator%="        },
                    { "$bran",  "operator&="        },
                    { "$bror",  "operator|="        },
                    { "$brxo",  "operator^="        },
                    { "$brls",  "operator<<="       },
                    { "$brrs",  "operator>>="       },
                    { "$bads",  "operator->*"       }
                }};

                for ( auto const& entry : kOps ) {
                    if ( special == entry.code ) {
                        outName = std::string{ entry.name };
                        return true;
                    }
                }
                if ( special == "$bland" ) { outName = "operator&&"; return true; }
                if ( special == "$blor"  ) { outName = "operator||"; return true; }
                if ( special == "$vft"   ) { outName = "`vftable'";  return true; }
                if ( special == "$vbt"   ) { outName = "`vbtable'";  return true; }
                return false;
            }

            bool _ParseParamSequence( std::vector<std::string>& outParams, char const terminator ) {
                while ( !AtEnd( ) && ( Peek( ) != terminator ) ) {
                    // Repeated consecutive parameter: n<count><type> (e.g. n3i -> int, int, int)
                    if ( ( Peek( ) == 'n' ) && IsAsciiDigit( Peek( 1 ) ) ) {
                        Get( ); // consume 'n'
                        std::size_t const repeatCount { static_cast<std::size_t>( Get( ) - '0' ) };
                        if ( repeatCount == 0u ) {
                            return false;
                        }
                        std::string repeatedType;
                        if ( !_ParseSingleParam( repeatedType ) ) {
                            return false;
                        }
                        for ( std::size_t r { }; r < repeatCount; ++r ) {
                            outParams.push_back( repeatedType );
                        }
                        continue;
                    }

                    std::string paramType;
                    if ( !_ParseSingleParam( paramType ) ) {
                        return false;
                    }
                    outParams.push_back( std::move( paramType ) );
                }
                return true;
            }

            bool _ParseSingleParam( std::string& outParam ) {
                // Parameter backreference: t1..t9 (1..9), ta..tz (10..35)
                if ( Peek( ) == 't' ) {
                    Get( );
                    if ( AtEnd( ) ) {
                        return false;
                    }
                    char const idxChar { Get( ) };
                    std::size_t idx { };
                    if ( ( idxChar >= '1' ) && ( idxChar <= '9' ) ) {
                        idx = static_cast<std::size_t>( idxChar - '1' );
                    } else if ( ( idxChar >= 'a' ) && ( idxChar <= 'z' ) ) {
                        idx = static_cast<std::size_t>( idxChar - 'a' ) + 9u;
                    } else {
                        return false;
                    }
                    if ( idx >= _paramBackrefs.size( ) ) {
                        return false;
                    }
                    outParam = _paramBackrefs[idx];
                    return true;
                }

                if ( !_ParseType( outParam ) ) {
                    return false;
                }
                _paramBackrefs.push_back( outParam );
                return true;
            }

            bool _ParseLengthPrefixedClassName( std::string& outName ) {
                std::size_t len { };
                while ( !AtEnd( ) && IsAsciiDigit( Peek( ) ) ) {
                    len = ( len * 10u ) + static_cast<std::size_t>( Get( ) - '0' );
                    if ( len > _input.size( ) ) {
                        return false;
                    }
                }
                if ( ( len == 0u ) || ( ( _pos + len ) > _input.size( ) ) ) {
                    return false;
                }

                std::string_view const raw { _input.substr( _pos, len ) };
                _pos += len;

                if ( !raw.empty( ) && ( raw.front( ) == '%' ) ) {
                    BorlandDemangler sub( raw, _options );
                    if ( sub._ParseTemplateSegment( outName ) ) {
                        return true;
                    }
                }

                // Replace internal '@' scope separators with "::"
                outName.clear( );
                outName.reserve( raw.size( ) );
                for ( char const ch : raw ) {
                    if ( ch == '@' ) {
                        outName += "::";
                    } else {
                        outName.push_back( ch );
                    }
                }
                return true;
            }

            bool _ParseType( std::string& outType ) {
                if ( AtEnd( ) || !EnterRecursion( ) ) {
                    return false;
                }

                std::string prefixCv;
                std::string signQual;
                std::string modelQual;

                while ( !AtEnd( ) ) {
                    char const ch { Peek( ) };
                    if ( ch == 'x' ) {
                        Get( );
                        prefixCv = AppendSpaceIfNeeded( prefixCv, "const" );
                    } else if ( ch == 'w' ) {
                        Get( );
                        prefixCv = AppendSpaceIfNeeded( prefixCv, "volatile" );
                    } else if ( ch == 'u' ) {
                        Get( );
                        signQual = "unsigned";
                    } else if ( ch == 'z' ) {
                        Get( );
                        signQual = "signed";
                    } else if ( ( ch == 'n' ) && ( ( Peek( 1 ) == 'p' ) || ( Peek( 1 ) == 'r' ) ) ) {
                        Get( );
                        // near pointer/ref (default; omit unless needed)
                    } else if ( ( ch == 'h' ) && ( ( Peek( 1 ) == 'p' ) || ( Peek( 1 ) == 'r' ) ) ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            modelQual = "__huge";
                        }
                    } else {
                        break;
                    }
                }

                if ( AtEnd( ) ) {
                    LeaveRecursion( );
                    return false;
                }

                bool       ok { true };
                char const ch { Peek( ) };

                if ( IsAsciiDigit( ch ) ) {
                    std::string clsName;
                    ok = _ParseLengthPrefixedClassName( clsName );
                    if ( ok ) {
                        outType = AppendSpaceIfNeeded( prefixCv, clsName );
                    }
                } else if ( ( ch == 'p' ) || ( ch == 'r' ) ) {
                    Get( );
                    bool const isRef { ch == 'r' };
                    // Function pointer: pq<params>$<ret>
                    if ( !isRef && ( Peek( ) == 'q' ) ) {
                        Get( );
                        std::vector<std::string> fnParams;
                        ok = _ParseParamSequence( fnParams, '$' ) && Consume( '$' );
                        std::string retType;
                        if ( ok ) {
                            ok = _ParseType( retType );
                        }
                        if ( ok ) {
                            std::string paramStr;
                            for ( std::size_t i { }; i < fnParams.size( ); ++i ) {
                                if ( i > 0u ) {
                                    paramStr += ", ";
                                }
                                paramStr += fnParams[i];
                            }
                            outType = std::format( "{} (*)({})", retType, paramStr );
                        }
                    } else {
                        std::string pointee;
                        ok = _ParseType( pointee );
                        if ( ok ) {
                            outType = pointee;
                            if ( !modelQual.empty( ) ) {
                                outType = AppendSpaceIfNeeded( outType, modelQual );
                            }
                            outType = AppendSpaceIfNeeded( outType, isRef ? "&" : "*" );
                            if ( !prefixCv.empty( ) ) {
                                outType += " " + prefixCv;
                            }
                        }
                    }
                } else if ( ch == 'a' ) {
                    // Array: a<digits>$<elem-type>
                    Get( );
                    std::size_t const numStart { _pos };
                    while ( !AtEnd( ) && IsAsciiDigit( Peek( ) ) ) {
                        Get( );
                    }
                    std::string_view const dim { _input.substr( numStart, _pos - numStart ) };
                    ok = Consume( '$' );
                    std::string elemType;
                    if ( ok ) {
                        ok = _ParseType( elemType );
                    }
                    if ( ok ) {
                        outType = std::format( "{}[{}]", AppendSpaceIfNeeded( prefixCv, elemType ), dim );
                    }
                } else if ( ch == 'm' ) {
                    // Member pointer: m<class>$<member-type>
                    Get( );
                    std::string clsName;
                    std::string memType;
                    ok = _ParseLengthPrefixedClassName( clsName ) && Consume( '$' ) && _ParseType( memType );
                    if ( ok ) {
                        outType = std::format( "{} {}::*", memType, clsName );
                    }
                } else {
                    Get( );
                    std::string baseType;
                    switch ( ch ) {
                        case 'v': baseType = "void";        break;
                        case 'c': baseType = "char";        break;
                        case 's': baseType = "short";       break;
                        case 'i': baseType = "int";         break;
                        case 'l': baseType = "long";        break;
                        case 'f': baseType = "float";       break;
                        case 'd': baseType = "double";      break;
                        case 'g': baseType = "long double"; break;
                        case 'b': baseType = "bool";        break;
                        case 'j': baseType = "__int64";     break;
                        case 'o': baseType = "wchar_t";     break;
                        case 'e': baseType = "...";         break;
                        default:  ok = false;               break;
                    }
                    if ( ok ) {
                        outType = AppendSpaceIfNeeded( prefixCv, AppendSpaceIfNeeded( signQual, baseType ) );
                    }
                }

                LeaveRecursion( );
                return ok;
            }

            std::vector<std::string> _paramBackrefs;

        };

        //=====================================================================
        // 3. OpenWatcom C/C++ demangler (Watcom 10.x / 11.0 / OpenWatcom)
        //=====================================================================

        class OpenWatcomDemangler:
            private DemangleCursor
        {

        public:

            OpenWatcomDemangler( std::string_view const input, DemangleOptions const& options ) noexcept:
                DemangleCursor ( input, options )
            {
                /*empty*/
            }

            std::optional<std::string> Demangle( ) {
                if ( !ConsumePrefix( "W?" ) ) {
                    return std::nullopt;
                }

                SpecialNameKind specialKind { SpecialNameKind::None };
                std::string     baseName;
                if ( !_ParseBaseName( baseName, specialKind ) ) {
                    return std::nullopt;
                }

                // Parse innermost-to-outermost scope list: :ScopeName$
                std::vector<std::string> scopes;
                while ( !AtEnd( ) && ( Peek( ) == ':' ) ) {
                    Get( ); // consume ':'
                    std::size_t const start { _pos };
                    while ( !AtEnd( ) && ( Peek( ) != '$' ) ) {
                        Get( );
                    }
                    if ( ( _pos == start ) || !Consume( '$' ) ) {
                        return std::nullopt;
                    }
                    scopes.emplace_back( _input.substr( start, _pos - start - 1u ) );
                }

                if ( specialKind == SpecialNameKind::Constructor ) {
                    baseName = scopes.empty( ) ? std::string{ "ctor" } : scopes.front( );
                } else if ( specialKind == SpecialNameKind::Destructor ) {
                    baseName = scopes.empty( ) ? std::string{ "~dtor" } : ( "~" + scopes.front( ) );
                }

                std::string const qualifiedName { JoinQualifiedScopesReversed( scopes, baseName ) };

                if ( AtEnd( ) ) {
                    return qualifiedName;
                }

                // Collect leading function/member modifiers (n=near, f=far, g=far16, h=huge, x=const, y=volatile)
                std::size_t const savedPos { _pos };
                std::string       fnModel;
                std::string       memberCv;
                while ( !AtEnd( ) ) {
                    char const ch { Peek( ) };
                    if ( ch == 'n' ) {
                        Get( );
                    } else if ( ch == 'f' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            fnModel = "__far";
                        }
                    } else if ( ch == 'g' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            fnModel = "__far16";
                        }
                    } else if ( ch == 'h' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            fnModel = "__huge";
                        }
                    } else if ( ch == 'x' ) {
                        Get( );
                        memberCv = AppendSpaceIfNeeded( memberCv, "const" );
                    } else if ( ch == 'y' ) {
                        Get( );
                        memberCv = AppendSpaceIfNeeded( memberCv, "volatile" );
                    } else {
                        break;
                    }
                }

                // Function signature: (<params>)<return-type>
                if ( Consume( '(' ) ) {
                    std::vector<std::string> params;
                    while ( !AtEnd( ) && ( Peek( ) != ')' ) ) {
                        std::string paramType;
                        if ( !_ParseParamType( paramType ) ) {
                            return std::nullopt;
                        }
                        params.push_back( std::move( paramType ) );
                    }
                    if ( !Consume( ')' ) ) {
                        return std::nullopt;
                    }

                    std::string returnType;
                    if ( !AtEnd( ) && !Consume( '_' ) ) {
                        if ( !_ParseType( returnType ) ) {
                            return std::nullopt;
                        }
                    }

                    std::string displayName { qualifiedName };
                    if ( specialKind == SpecialNameKind::ConversionOperator ) {
                        displayName = qualifiedName + "operator " + returnType;
                        returnType.clear( );
                    }

                    std::string out;
                    if ( _options.includeReturnType && !returnType.empty( ) ) {
                        out += returnType + " ";
                    }
                    if ( !fnModel.empty( ) ) {
                        out += fnModel + " ";
                    }
                    out += displayName;
                    out += "(";
                    if ( params.empty( ) ) {
                        out += "void";
                    } else {
                        for ( std::size_t i { }; i < params.size( ); ++i ) {
                            if ( i > 0u ) {
                                out += ", ";
                            }
                            out += params[i];
                        }
                    }
                    out += ")";
                    if ( !memberCv.empty( ) ) {
                        out += " " + memberCv;
                    }
                    return out;
                }

                // Otherwise it is a data / static variable symbol: rewind modifiers and parse full type
                _pos = savedPos;
                std::string varType;
                if ( !_ParseType( varType ) ) {
                    return std::nullopt;
                }
                return AppendSpaceIfNeeded( varType, qualifiedName );
            }

        private:

            enum class SpecialNameKind:
                std::uint8_t
            {
                None,
                Constructor,
                Destructor,
                ConversionOperator,
                OtherSpecial
            };

            bool _ParseBaseName( std::string& outName, SpecialNameKind& outSpecialKind ) {
                outSpecialKind = SpecialNameKind::None;
                if ( AtEnd( ) ) {
                    return false;
                }

                // Special Watcom names start with '$' (e.g. W?$ct$:MyClass$n()_)
                if ( Consume( '$' ) ) {
                    std::size_t const start { _pos };
                    while ( !AtEnd( ) && ( Peek( ) != '$' ) ) {
                        Get( );
                    }
                    if ( ( _pos == start ) || !Consume( '$' ) ) {
                        return false;
                    }
                    std::string_view const code { _input.substr( start, _pos - start - 1u ) };
                    return _DecodeSpecialCode( code, outName, outSpecialKind );
                }

                std::size_t const start { _pos };
                while ( !AtEnd( ) && ( Peek( ) != '$' ) ) {
                    Get( );
                }
                if ( ( _pos == start ) || !Consume( '$' ) ) {
                    return false;
                }
                outName.assign( _input.substr( start, _pos - start - 1u ) );
                return true;
            }

            static bool _DecodeSpecialCode( std::string_view const code, std::string& outName, SpecialNameKind& outSpecialKind ) {
                outSpecialKind = SpecialNameKind::OtherSpecial;
                if ( code == "ct" ) { outSpecialKind = SpecialNameKind::Constructor;        return true; }
                if ( code == "dt" ) { outSpecialKind = SpecialNameKind::Destructor;         return true; }
                if ( code == "cv" ) { outSpecialKind = SpecialNameKind::ConversionOperator; return true; }

                struct OpEntry {
                    std::string_view code;
                    std::string_view name;
                };

                static constexpr std::array<OpEntry, 44> kOps {{
                    { "nw",  "operator new"      },
                    { "dl",  "operator delete"   },
                    { "na",  "operator new[]"    },
                    { "da",  "operator delete[]" },
                    { "as",  "operator="         },
                    { "eq",  "operator=="        },
                    { "ne",  "operator!="        },
                    { "lt",  "operator<"         },
                    { "le",  "operator<="        },
                    { "gt",  "operator>"         },
                    { "ge",  "operator>="        },
                    { "pl",  "operator+"         },
                    { "mi",  "operator-"         },
                    { "ml",  "operator*"         },
                    { "dv",  "operator/"         },
                    { "md",  "operator%"         },
                    { "rm",  "operator%"         },
                    { "an",  "operator&"         },
                    { "or",  "operator|"         },
                    { "er",  "operator^"         },
                    { "eo",  "operator^"         },
                    { "co",  "operator~"         },
                    { "nt",  "operator!"         },
                    { "aa",  "operator&&"        },
                    { "oo",  "operator||"        },
                    { "ls",  "operator<<"        },
                    { "rs",  "operator>>"        },
                    { "pp",  "operator++"        },
                    { "mm",  "operator--"        },
                    { "pe",  "operator+="        },
                    { "me",  "operator-="        },
                    { "te",  "operator*="        },
                    { "de",  "operator/="        },
                    { "re",  "operator%="        },
                    { "ae",  "operator&="        },
                    { "oe",  "operator|="        },
                    { "xe",  "operator^="        },
                    { "cm",  "operator,"         },
                    { "rf",  "operator->"        },
                    { "pm",  "operator->*"       },
                    { "cl",  "operator()"        },
                    { "ix",  "operator[]"        },
                    { "vt",  "`vftable'"         },
                    { "vb",  "`vbtable'"         }
                }};

                for ( auto const& entry : kOps ) {
                    if ( code == entry.code ) {
                        outName = std::string{ entry.name };
                        return true;
                    }
                }
                if ( code == "si" ) { outName = "__staticinit"; return true; }
                if ( code == "sd" ) { outName = "__staticdtor"; return true; }
                if ( code == "th" ) { outName = "`thunk'";      return true; }
                return false;
            }

            bool _ParseParamType( std::string& outParam ) {
                // 1-based parameter type backreference ('1'..'9')
                if ( ( Peek( ) >= '1' ) && ( Peek( ) <= '9' ) ) {
                    std::size_t const idx { static_cast<std::size_t>( Get( ) - '1' ) };
                    if ( idx >= _paramBackrefs.size( ) ) {
                        return false;
                    }
                    outParam = _paramBackrefs[idx];
                    return true;
                }

                if ( !_ParseType( outParam ) ) {
                    return false;
                }
                _paramBackrefs.push_back( outParam );
                return true;
            }

            bool _ParseScopedUserType( std::string& outName ) {
                // Watcom user-defined type in signatures: $<name>[:<scope>...]$
                std::size_t const start { _pos };
                while ( !AtEnd( ) && ( Peek( ) != '$' ) && ( Peek( ) != ':' ) ) {
                    Get( );
                }
                if ( _pos == start ) {
                    return false;
                }
                std::string leaf { _input.substr( start, _pos - start ) };
                std::vector<std::string> scopes;
                while ( Consume( ':' ) ) {
                    std::size_t const scStart { _pos };
                    while ( !AtEnd( ) && ( Peek( ) != '$' ) && ( Peek( ) != ':' ) ) {
                        Get( );
                    }
                    if ( _pos == scStart ) {
                        return false;
                    }
                    scopes.emplace_back( _input.substr( scStart, _pos - scStart ) );
                }
                if ( !Consume( '$' ) ) {
                    return false;
                }
                outName = JoinQualifiedScopesReversed( scopes, leaf );
                return true;
            }

            bool _ParseType( std::string& outType ) {
                if ( AtEnd( ) || !EnterRecursion( ) ) {
                    return false;
                }

                std::string cvQual;
                std::string signQual;
                std::string modelQual;

                while ( !AtEnd( ) ) {
                    char const ch { Peek( ) };
                    if ( ch == 'x' ) {
                        Get( );
                        cvQual = AppendSpaceIfNeeded( cvQual, "const" );
                    } else if ( ch == 'y' ) {
                        Get( );
                        cvQual = AppendSpaceIfNeeded( cvQual, "volatile" );
                    } else if ( ch == 'u' ) {
                        Get( );
                        signQual = "unsigned";
                    } else if ( ch == 'n' ) {
                        Get( );
                    } else if ( ch == 'f' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            modelQual = "__far";
                        }
                    } else if ( ch == 'g' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            modelQual = "__far16";
                        }
                    } else if ( ch == 'h' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            modelQual = "__huge";
                        }
                    } else {
                        break;
                    }
                }

                if ( AtEnd( ) ) {
                    LeaveRecursion( );
                    return false;
                }

                bool       ok { true };
                char const ch { Get( ) };

                if ( ( ch == 'p' ) || ( ch == 'r' ) ) {
                    bool const isRef { ch == 'r' };
                    // Optional pointer/reference distance qualifier immediately after p/r (pn, pf, ph, pg)
                    std::string ptrModel;
                    if ( Peek( ) == 'n' ) {
                        Get( );
                    } else if ( Peek( ) == 'f' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            ptrModel = "__far";
                        }
                    } else if ( Peek( ) == 'g' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            ptrModel = "__far16";
                        }
                    } else if ( Peek( ) == 'h' ) {
                        Get( );
                        if ( _options.includeMemoryModel ) {
                            ptrModel = "__huge";
                        }
                    }

                    // Function pointer: p[n|f](<params>)<ret>
                    if ( !isRef && Consume( '(' ) ) {
                        std::vector<std::string> fnParams;
                        while ( !AtEnd( ) && ( Peek( ) != ')' ) ) {
                            std::string pt;
                            if ( !_ParseType( pt ) ) {
                                ok = false;
                                break;
                            }
                            fnParams.push_back( std::move( pt ) );
                        }
                        if ( ok ) {
                            ok = Consume( ')' );
                        }
                        std::string retType;
                        if ( ok ) {
                            ok = _ParseType( retType );
                        }
                        if ( ok ) {
                            std::string pStr;
                            if ( fnParams.empty( ) ) {
                                pStr = "void";
                            } else {
                                for ( std::size_t i { }; i < fnParams.size( ); ++i ) {
                                    if ( i > 0u ) {
                                        pStr += ", ";
                                    }
                                    pStr += fnParams[i];
                                }
                            }
                            std::string const star { AppendSpaceIfNeeded( ptrModel, "*" ) };
                            outType = std::format( "{} ({})({})", retType, star, pStr );
                        }
                    } else {
                        std::string pointee;
                        ok = _ParseType( pointee );
                        if ( ok ) {
                            outType = pointee;
                            if ( !ptrModel.empty( ) ) {
                                outType = AppendSpaceIfNeeded( outType, ptrModel );
                            }
                            outType = AppendSpaceIfNeeded( outType, isRef ? "&" : "*" );
                            if ( !cvQual.empty( ) ) {
                                outType += " " + cvQual;
                            }
                        }
                    }
                } else if ( ch == 'm' ) {
                    // Pointer to member: m$<class>$<type>
                    std::string clsName;
                    std::string memType;
                    ok = Consume( '$' ) && _ParseScopedUserType( clsName ) && _ParseType( memType );
                    if ( ok ) {
                        outType = std::format( "{} {}::*", memType, clsName );
                    }
                } else if ( ch == '[' ) {
                    // Array: [<digits>]<elem-type>
                    std::size_t const dimStart { _pos };
                    while ( !AtEnd( ) && IsAsciiDigit( Peek( ) ) ) {
                        Get( );
                    }
                    std::string_view const dim { _input.substr( dimStart, _pos - dimStart ) };
                    ok = Consume( ']' );
                    std::string elemType;
                    if ( ok ) {
                        ok = _ParseType( elemType );
                    }
                    if ( ok ) {
                        outType = std::format( "{}[{}]", elemType, dim );
                    }
                } else if ( ch == '$' ) {
                    std::string userType;
                    ok = _ParseScopedUserType( userType );
                    if ( ok ) {
                        outType = AppendSpaceIfNeeded( cvQual, AppendSpaceIfNeeded( modelQual, userType ) );
                    }
                } else {
                    std::string baseType;
                    switch ( ch ) {
                        case 'a': baseType = "char";        break;
                        case 'b': baseType = "float";       break;
                        case 'c': baseType = "signed char"; break;
                        case 'd': baseType = "double";      break;
                        case 'e': baseType = "...";         break;
                        case 'i': baseType = "int";         break;
                        case 'j': baseType = "__int64";     break;
                        case 'l': baseType = "long";        break;
                        case 'o': baseType = "__segment";   break;
                        case 'q': baseType = "bool";        break;
                        case 's': baseType = "short";       break;
                        case 't': baseType = "long double"; break;
                        case 'v': baseType = "void";        break;
                        case 'w': baseType = "wchar_t";     break;
                        default:  ok = false;               break;
                    }
                    if ( ok ) {
                        outType = AppendSpaceIfNeeded(
                            cvQual,
                            AppendSpaceIfNeeded( signQual, AppendSpaceIfNeeded( modelQual, baseType ) )
                        );
                    }
                }

                LeaveRecursion( );
                return ok;
            }

            std::vector<std::string> _paramBackrefs;

        };

    } // anonymous namespace

    //=========================================================================
    // Public API implementation
    //=========================================================================

    DemangleStyle DetectManglingStyle( std::string_view const symbol ) noexcept {
        if ( symbol.size( ) < 2u ) {
            return DemangleStyle::None;
        }

        if ( symbol.starts_with( "W?" ) ) {
            return DemangleStyle::OpenWatcom;
        }

        if ( symbol.front( ) == '?' ) {
            return DemangleStyle::Msc;
        }

        if ( symbol.front( ) == '@' ) {
            // Distinguish MSC __fastcall (@Name@Bytes) from Borland C++ (@Name$q... / @Class@Member)
            std::size_t const secondAt { symbol.find( '@', 1u ) };
            if (
                ( secondAt != std::string_view::npos ) &&
                ( symbol.find( '$' ) == std::string_view::npos ) &&
                ( symbol.find( '@', secondAt + 1u ) == std::string_view::npos ) &&
                IsAllDigits( symbol.substr( secondAt + 1u ) )
            ) {
                return DemangleStyle::Msc;
            }
            if (
                ( symbol.find( '$' ) != std::string_view::npos ) ||
                ( secondAt != std::string_view::npos )
            ) {
                return DemangleStyle::Borland;
            }
            return DemangleStyle::None;
        }

        if ( symbol.front( ) == '_' ) {
            // MSC __stdcall (_Name@Bytes)
            std::size_t const atPos { symbol.find( '@', 1u ) };
            if (
                ( atPos != std::string_view::npos ) &&
                ( atPos > 1u ) &&
                ( symbol.find( '@', atPos + 1u ) == std::string_view::npos ) &&
                IsAllDigits( symbol.substr( atPos + 1u ) )
            ) {
                return DemangleStyle::Msc;
            }
        }

        return DemangleStyle::None;
    }

    DemangleResult DemangleSymbolDetailed( std::string_view const symbol, DemangleOptions const& options ) {
        if ( ( options.style == DemangleStyle::None ) || symbol.empty( ) ) {
            return DemangleResult{ std::string{ symbol }, DemangleStyle::None, false };
        }

        DemangleStyle const targetStyle { ( options.style == DemangleStyle::Auto ) ? DetectManglingStyle( symbol ) : options.style }; 

        std::optional<std::string> parsed;
        switch ( targetStyle ) {
            case DemangleStyle::Msc: {
                MscDemangler demangler( symbol, options );
                parsed = demangler.Demangle( );
                break;
            }

            case DemangleStyle::Borland: {
                BorlandDemangler demangler( symbol, options );
                parsed = demangler.Demangle( );
                break;
            }

            case DemangleStyle::OpenWatcom: {
                OpenWatcomDemangler demangler( symbol, options );
                parsed = demangler.Demangle( );
                break;
            }

            case DemangleStyle::None:
            case DemangleStyle::Auto:
                break;
        }

        if ( parsed.has_value( ) && !parsed->empty( ) ) {
            return DemangleResult { std::move( *parsed ), targetStyle, true };
        }

        return DemangleResult { std::string { symbol }, DemangleStyle::None, false };
    }

    std::string DemangleSymbol( std::string_view const symbol, DemangleStyle const style ) {
        if ( style == DemangleStyle::None ) {
            return std::string { symbol };
        }
        DemangleOptions opts { };
        opts.style = style;
        return DemangleSymbolDetailed( symbol, opts ).demangled;
    }

    std::string DemangleSymbol( std::string_view const symbol, DemangleOptions const& options ) {
        return DemangleSymbolDetailed( symbol, options ).demangled;
    }

    std::string_view ToString( DemangleStyle const style ) noexcept {
        switch ( style ) {
            case DemangleStyle::None:       return "none";
            case DemangleStyle::Auto:       return "auto";
            case DemangleStyle::Msc:        return "msc";
            case DemangleStyle::Borland:    return "bc";
            case DemangleStyle::OpenWatcom: return "ow";
        }
        return "none";
    }

    std::optional<DemangleStyle> ParseDemangleStyle( std::string_view const text ) noexcept {
        std::string const lower { ToLowerAscii( text ) };
        if (   lower == "none"                                                          ) { return DemangleStyle::None;       }
        if (   lower == "auto"                                                          ) { return DemangleStyle::Auto;       }
        if ( ( lower == "msc"  ) || ( lower == "msvc"    )                              ) { return DemangleStyle::Msc;        }
        if ( ( lower == "bc"   ) || ( lower == "borland" )                              ) { return DemangleStyle::Borland;    }
        if ( ( lower == "ow"   ) || ( lower == "watcom"  ) || ( lower == "openwatcom" ) ) { return DemangleStyle::OpenWatcom; }
        return std::nullopt;
    }

    DemangleArgResult ParseDemangleOption( std::string_view const arg, DemangleStyle& outStyle ) noexcept {
        if ( arg == "--demangle" ) {
            outStyle = DemangleStyle::Auto;
            return DemangleArgResult::Parsed;
        }
        constexpr std::string_view kPrefix { "--demangle=" };
        if ( !arg.starts_with( kPrefix ) ) {
            return DemangleArgResult::NotMatched;
        }
        auto const parsed { ParseDemangleStyle( arg.substr( kPrefix.size( ) ) ) };
        if ( !parsed.has_value( ) ) {
            return DemangleArgResult::InvalidValue;
        }
        outStyle = *parsed;
        return DemangleArgResult::Parsed;
    }

} // namespace Omf
