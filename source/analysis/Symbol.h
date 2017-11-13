//------------------------------------------------------------------------------
// Symbol.h
// Symbols for semantic analysis.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "diagnostics/Diagnostics.h"
#include "numeric/SVInt.h"
#include "parsing/AllSyntax.h"
#include "text/SourceLocation.h"
#include "util/HashMap.h"

#include "ConstantValue.h"
#include "SemanticFacts.h"

namespace slang {

class Statement;
class StatementList;
class Expression;
class SyntaxTree;
class Symbol;
class ScopeSymbol;
class RootSymbol;
class TypeSymbol;
class WildcardImportSymbol;
class PackageSymbol;
class ParameterSymbol;
class SymbolFactory;

using SymbolList = span<const Symbol* const>;
using SymbolMap = std::unordered_map<string_view, const Symbol*>;

using Dimensions = span<ConstantRange const>;

enum class SymbolKind {
    Unknown,
    Root,
    DynamicScope,
    CompilationUnit,
    IntegralType,
    RealType,
    StringType,
    CHandleType,
    VoidType,
    EventType,
    EnumType,
    TypeAlias,
    Parameter,
    EnumValue,
    Module,
    Interface,
    Modport,
    ModuleInstance,
    InterfaceInstance,
    Package,
    ExplicitImport,
    ImplicitImport,
    WildcardImport,
    Program,
    Attribute,
    Genvar,
    IfGenerate,
    LoopGenerate,
    GenerateBlock,
    ProceduralBlock,
    SequentialBlock,
    Variable,
    Instance,
    FormalArgument,
    Subroutine
};

/// Specifies possible kinds of lookups that can be done.
enum class LookupKind {
    /// A direct lookup within the scope is performed, with no upward name referencing
    /// allowed. The lookup location is only used for error reporting, not qualifying
    /// which signals are accessible. Package imports are not considered.
    Direct,

    /// A lookup of a simple name, starting in the local scope. The lookup location is
    /// used to qualify accessible signals. Package imports are considered.
    Local,

    /// The lookup is for the first part of a scoped name. This first performs
    /// the equivalent of a Local lookup; if no symbol is found using that method,
    /// it will search for a package with the given name.
    Scoped,

    /// A lookup for a simple name that is part of a callable expression (task or function).
    /// This is similar to a Local lookup, with additional rules specific to callables.
    Callable,

    /// A lookup for a module, interface, or program definition. Similar to a Callable lookup,
    /// there are additional rules about where definitions can be found.
    Definition
};

/// Base class for all symbols (logical code constructs) such as modules, types,
/// functions, variables, etc.
class Symbol {
public:
    /// The type of symbol.
    SymbolKind kind;

    /// The name of the symbol; if the symbol does not have a name,
    /// this will be an empty string.
    string_view name;

    /// The declared location of the symbol in the source code, or an empty location
    /// if it was not explicitly declared in the source text. This is mainly used
    /// for reporting errors.
    SourceLocation location;

    /// The symbol that contains this symbol in the source text. All symbols have a containing
    /// symbol except for the design root, which has itself as the containing symbol. Keep that
    /// in mind when traversing the parent links.
    const ScopeSymbol* getParent() const { return parentScope; }

    /// Finds the first ancestor symbol of the given kind. If this symbol is already of
    /// the given kind, returns this symbol.
    const Symbol* findAncestor(SymbolKind searchKind) const;

    /// Gets the symbol for the root of the design.
    const RootSymbol& getRoot() const;

    SymbolFactory& getFactory() const;

    template<typename T>
    const T& as() const { return *static_cast<const T*>(this); }

    Symbol(const Symbol&) = delete;
    Symbol& operator=(const Symbol&) = delete;

protected:
    explicit Symbol(SymbolKind kind, string_view name = "", SourceLocation location = SourceLocation()) :
        kind(kind), name(name), location(location) {}

    Symbol(SymbolKind kind, const ScopeSymbol& containingSymbol, string_view name = "",
           SourceLocation location = SourceLocation()) :
        kind(kind), name(name), location(location),
        parentScope(&containingSymbol) {}

    Symbol(SymbolKind kind, Token token, const ScopeSymbol& containingSymbol) :
        kind(kind), name(token.valueText()), location(token.location()),
        parentScope(&containingSymbol) {}

    Diagnostic& addError(DiagCode code, SourceLocation location) const;

    // The following are helper classes for deferring binding of statements,
    // expressions, and types until they're ready to be accessed. This is
    // important both for correctness as well as for performance when not
    // doing a batch compilation.

    template<typename TDerived, typename TResult, typename TSource>
    struct Lazy {
        Lazy(const TResult* init) : cache(init) {}

        void set(const TResult* result) { cache = result; }
        void set(const TSource& source) { cache = &source; }

        const TResult* getOpt(const ScopeSymbol& scope) const {
            if (cache.index() == 0)
                return std::get<0>(cache);

            auto derived = static_cast<const TDerived*>(this);
            const TResult& result = derived->evaluate(scope, *std::get<1>(cache));
            cache = &result;
            return &result;
        }

        const TResult& get(const ScopeSymbol& scope) const { return *getOpt(scope); }

    private:
        mutable std::variant<const TResult*, const TSource*> cache;
    };

    struct LazyStatement : public Lazy<LazyStatement, Statement, StatementSyntax> {
        LazyStatement();
        const Statement& evaluate(const ScopeSymbol& scope, const StatementSyntax& syntax) const;
    };

    struct LazyStatementList : public Lazy<LazyStatementList, StatementList, SyntaxList<SyntaxNode>> {
        LazyStatementList();
        const StatementList& evaluate(const ScopeSymbol& scope, const SyntaxList<SyntaxNode>& list) const;
    };

    struct LazyConstant : public Lazy<LazyConstant, Expression, ExpressionSyntax> {
        LazyConstant();
        const Expression& evaluate(const ScopeSymbol& scope, const ExpressionSyntax& syntax) const;
    };

    struct LazyInitializer : public Lazy<LazyInitializer, Expression, ExpressionSyntax> {
        LazyInitializer();
        const Expression& evaluate(const ScopeSymbol& scope, const ExpressionSyntax& syntax) const;
    };

    struct LazyType : public Lazy<LazyType, TypeSymbol, DataTypeSyntax> {
        LazyType();
        const TypeSymbol& evaluate(const ScopeSymbol& scope, const DataTypeSyntax& syntax) const;
    };

private:
    const ScopeSymbol* parentScope = nullptr;
};

/// Base class for symbols that also act as scopes, which means they contain
/// child symbols that can be looked up by name.
class ScopeSymbol : public Symbol {
public:
    /// Looks up a symbol in the current scope. Returns null if no symbol is found.
    ///
    /// @param lookupLocation is used for reporting errors if the symbol is not found.
    ///        Additionally, depending on the `lookupKind` being used, it may be used
    ///        to qualify which local symbols are accessible.
    /// @param lookupKind specifies the kind of lookup to perform. This controls which
    ///        symbols are accessible, whether package imports are considered, and
    ///        whether parent scopes should be included.
    ///
    const Symbol* lookup(string_view searchName, SourceLocation lookupLocation, LookupKind lookupKind) const;

    /// Looks up a symbol in the current scope, expecting it to exist and be of the
    /// given type. If those conditions do not hold, this will assert.
    template<typename T>
    const T& lookup(string_view name, SourceLocation lookupLocation = SourceLocation(),
                    LookupKind lookupKind = LookupKind::Direct) const {
        const Symbol* sym = lookup(name, lookupLocation, lookupKind);
        ASSERT(sym);
        return sym->as<T>();
    }

    /// Gets a list of all of the members in the scope.
    SymbolList members() const;

    /// Gets a specific member at the given zero-based index, expecting it to be of the specified type.
    /// If the type does not match, this will assert.
    template<typename T>
    const T& member(uint32_t index) const { return members()[index]->as<T>(); }

    /// A helper method to evaluate a constant in the current scope.
    ConstantValue evaluateConstant(const ExpressionSyntax& expr) const;

    /// A helper method to evaluate a constant in the current scope and then
    /// convert it to the given destination type. If the conversion fails, the
    /// returned value will be marked bad.
    ConstantValue evaluateConstantAndConvert(const ExpressionSyntax& expr, const TypeSymbol& targetType,
                                             SourceLocation errorLocation) const;

    /// Overrides the members of the symbol to be the given list.
    /// Note that if the scope gets explicitly marked dirty and its
    /// members regenerated, this list will be lost.
    void setMembers(span<const Symbol* const> members);
    void setMember(const Symbol& member);

protected:
    using Symbol::Symbol;

    /// A simple wrapper around mutable buffers used to build up the
    /// list of members in a symbol.
    struct MemberBuilder {
        SmallHashMap<string_view, const Symbol*, 16> memberMap;
        SmallVectorSized<const Symbol*, 16> memberList;
        SmallVectorSized<const WildcardImportSymbol*, 8> wildcardImports;

        void add(const Symbol& symbol);
        void add(const SyntaxNode& node, const ScopeSymbol& parent);
    };

    /// Called to ensure that the list of members has been initialized.
    void ensureInit() const {
        // Most of the work of initialization is deferred to doInit() so that
        // this function will be easy inlined.
        if (!membersInitialized)
            doInit();
    }

    /// Called in a few rare cases to mark the symbol's members as dirty, which
    /// means they will be recomputed the next time they are requested.
    void markDirty() { membersInitialized = false; }

    /// Overriden by derived classes to fill in the list of members for the symbol.
    virtual void fillMembers(MemberBuilder&) const {}

private:
    void doInit() const;
    void copyMembers(MemberBuilder& builder) const;

    // For now, there is one hash table here for the normal members namespace. The other
    // namespaces are specific to certain symbol types so I don't want to have extra overhead
    // on every kind of scope symbol.
    mutable HashMapRef<string_view, const Symbol*> memberMap;

    // In addition to the hash, also maintain an ordered list of members for easier access.
    mutable span<const Symbol* const> memberList;

    // Also, maintain a separate list containing just wildcard imports.
    // Every time we fail to look up a symbol name, we'll fall back to looking at imports.
    mutable span<const WildcardImportSymbol* const> wildcardImports;

    mutable bool membersInitialized = false;
};

/// A scope that can be dynamically modified programmatically. Not used for batch compilation; intended
/// for tools and unit tests.
class DynamicScopeSymbol : public ScopeSymbol {
public:
    explicit DynamicScopeSymbol(const ScopeSymbol& parent);

    /// Adds a symbol to the scope.
    void addSymbol(const Symbol& symbol);

    /// Creates one or more symbols for the given syntax node and adds them to the scope.
    /// Also returns the set of created symbols.
    SymbolList createAndAddSymbols(const SyntaxNode& node);

private:
    std::vector<const Symbol*> members;
};

/// The root of a single compilation unit.
class CompilationUnitSymbol : public ScopeSymbol {
public:
    //CompilationUnitSymbol(const ScopeSymbol& parent, SymbolList members);
    CompilationUnitSymbol(const ScopeSymbol& parent);
};

/// A SystemVerilog package construct.
class PackageSymbol : public ScopeSymbol {
public:
    PackageSymbol(string_view name, const ScopeSymbol& parent);
};

/// Represents a module, interface, or program declaration.
class DefinitionSymbol : public Symbol {
public:
    const ModuleDeclarationSyntax& syntax;

    DefinitionSymbol(const ModuleDeclarationSyntax& decl, const ScopeSymbol& container);

    /// Small collection of info extracted from a parameter definition
    struct ParameterInfo {
        const ParameterDeclarationSyntax& paramDecl;
        const VariableDeclaratorSyntax& declarator;
        string_view name;
        SourceLocation location;
        ExpressionSyntax* initializer;
        bool local;
        bool bodyParam;
    };

    span<ParameterInfo> getDeclaredParams() const;

private:
    // Helper function used by getDeclaredParams to convert a single parameter declaration into
    // one or more ParameterInfo instances.
    bool getParamDecls(const ParameterDeclarationSyntax& syntax, SmallVector<ParameterInfo>& buffer,
                       HashMapBase<string_view, SourceLocation>& nameDupMap,
                       bool lastLocal, bool overrideLocal, bool bodyParam) const;

    mutable optional<span<ParameterInfo>> paramInfoCache;
};

/// Base class for module, interface, and program instance symbols.
class InstanceSymbol : public ScopeSymbol {
public:
    const DefinitionSymbol& definition;
    const HierarchicalInstanceSyntax* syntax;

    static void fromSyntax(const ScopeSymbol& parent, const HierarchyInstantiationSyntax& syntax,
                           SmallVector<const Symbol*>& results);

protected:
    InstanceSymbol(SymbolKind kind, const DefinitionSymbol& definition, const HierarchicalInstanceSyntax* syntax,
                   HashMapRef<string_view, const ExpressionSyntax*> parameters, const ScopeSymbol& parent);

    void fillMembers(MemberBuilder& builder) const override final;

    static SourceLocation getLocation(const DefinitionSymbol& definition, const HierarchicalInstanceSyntax* syntax);
    static string_view getName(const DefinitionSymbol& definition, const HierarchicalInstanceSyntax* syntax);

    HashMapRef<string_view, const ExpressionSyntax*> paramAssignments;
};

class ModuleInstanceSymbol : public InstanceSymbol {
public:
    ModuleInstanceSymbol(const DefinitionSymbol& definition, const ScopeSymbol& parent);
    ModuleInstanceSymbol(const DefinitionSymbol& definition, const HierarchicalInstanceSyntax* syntax,
                         HashMapRef<string_view, const ExpressionSyntax*> parameters, const ScopeSymbol& parent);
};

//class GenvarSymbol : public Symbol {
//public:
//    GenvarSymbol(string_view name, SourceLocation location, ) :
//        Symbol(SymbolKind::Genvar, nullptr, name, location) {}
//};

class SequentialBlockSymbol : public ScopeSymbol {
public:
    SequentialBlockSymbol(const ScopeSymbol& parent);

    static SequentialBlockSymbol& createImplicitBlock(const ForLoopStatementSyntax& forLoop, const ScopeSymbol& parent);

    const Statement& getBody() const { return body.get(*this); }
    void setBody(const Statement& stmt) { body.set(&stmt); }
    void setBody(const StatementSyntax& syntax) { body.set(syntax); }

private:
    LazyStatement body;
};

class ProceduralBlockSymbol : public ScopeSymbol {
public:
    ProceduralBlockKind procedureKind;

    ProceduralBlockSymbol(const ScopeSymbol& parent, ProceduralBlockKind procedureKind);

    const Statement& getBody() const { return body.get(*this); }
    void setBody(const Statement& stmt) { body.set(&stmt); }
    void setBody(const StatementSyntax& syntax) { body.set(syntax); }

private:
    LazyStatement body;
};

/// Represents a conditional if-generate construct; the results of evaluating
/// the condition become child members of this symbol.
class IfGenerateSymbol : public ScopeSymbol {
public:
    IfGenerateSymbol(const IfGenerateSyntax& syntax, const ScopeSymbol& parent);

private:
    void fillMembers(MemberBuilder& builder) const override final;

    const IfGenerateSyntax& syntax;
};

/// Represents a loop generate construct; the results of evaluating
/// the loop become child members of this symbol.
class LoopGenerateSymbol : public ScopeSymbol {
public:
    LoopGenerateSymbol(const LoopGenerateSyntax& syntax, const ScopeSymbol& parent);

private:
    void fillMembers(MemberBuilder& builder) const override final;

    const LoopGenerateSyntax& syntax;
};

/// Represents blocks that are instantiated by a loop generate or conditional
/// generate construct. These blocks can contain a bunch of members, or just
/// a single item. They can also contain an implicit parameter representing
/// the loop iteration value.
class GenerateBlockSymbol : public ScopeSymbol {
public:
    GenerateBlockSymbol(string_view name, SourceLocation location, const SyntaxNode& body, const ScopeSymbol& parent);
    GenerateBlockSymbol(string_view name, SourceLocation location, const SyntaxNode& body,
                        const ParameterSymbol& implicitParam, const ScopeSymbol& parent);

private:
    void fillMembers(MemberBuilder& builder) const override final;

    const SyntaxNode& body;
    const ParameterSymbol* implicitParam = nullptr;
};

/// Represents an explicit import from a package. This symbol type is
/// special in that it won't be returned from a lookup() call; instead
/// it will be unwrapped into the imported symbol.
class ExplicitImportSymbol : public Symbol {
public:
    string_view packageName;
    string_view importName;

    ExplicitImportSymbol(string_view packageName, string_view importName,
                         SourceLocation location, const ScopeSymbol& parent);

    const PackageSymbol* package() const;
    const Symbol* importedSymbol() const;

private:
    mutable const PackageSymbol* package_ = nullptr;
    mutable const Symbol* import = nullptr;
    mutable bool initialized = false;
};

/// Represents a symbol that has been implicitly imported into a scope via
/// a wildcard import. This symbol type is special in that it won't be
/// returned from a lookup() call; also it is created on demand during
/// lookups of other symbols.
class ImplicitImportSymbol : public Symbol {
public:
    ImplicitImportSymbol(const WildcardImportSymbol& wildcard, const Symbol& importedSymbol,
                         const ScopeSymbol& parent);

    const WildcardImportSymbol& wildcard() const { return wildcard_; }
    const Symbol* importedSymbol() const { return &import; }
    const PackageSymbol* package() const;

private:
    const WildcardImportSymbol& wildcard_;
    const Symbol& import;
};

/// Represents a wildcard import declaration. This symbol is special in
/// that it won't be returned by a lookup, and won't even be in the name
/// map of a symbol at all. Instead there is a sideband list used to
/// resolve names via wildcard.
class WildcardImportSymbol : public Symbol {
public:
    string_view packageName;

    WildcardImportSymbol(string_view packageName, SourceLocation location, const ScopeSymbol& parent);

    const PackageSymbol* package() const;
    const ImplicitImportSymbol* resolve(string_view lookupName, SourceLocation lookupLocation) const;

private:
    mutable const PackageSymbol* package_ = nullptr;
    mutable bool initialized = false;
};

class ParameterSymbol : public Symbol {
public:
    /// Creates a new parameter symbol with the given value.
    ParameterSymbol(string_view name, SourceLocation location, const TypeSymbol& type,
                    ConstantValue value, const ScopeSymbol& parent);

    /// Creates a new parameter symbol from the given syntax info.
    ParameterSymbol(string_view name, SourceLocation location, const DataTypeSyntax& typeSyntax,
                    const ExpressionSyntax* defaultInitializer, const ExpressionSyntax* assignedValue,
                    const ScopeSymbol* instanceScope, bool isLocalParam, bool isPortParam, const ScopeSymbol& parent);

    /// Indicates whether the parameter is a "localparam".
    bool isLocalParam() const { return isLocal; }

    /// Indicates whether the parameter was declared in the element's parameter port list.
    /// Otherwise, it was found declared in the design element itself as a member.
    bool isPortParam() const { return isPort; }

    /// Indicates whether the parameter was given a default value in its initializer.
    bool hasDefault() const { return defaultInitializer != nullptr; }

    // Methods for getting information about the default value for the parameter.
    // Parameters are not required to have a default so you must check if you care.
    const ConstantValue* defaultValue() const;
    const TypeSymbol* defaultType() const;

    const TypeSymbol& type() const;
    const ConstantValue& value() const;

private:
    void evaluate(const ExpressionSyntax* expr, const TypeSymbol*& determinedType,
                  ConstantValue*& determinedValue, const ScopeSymbol& scope) const;

    mutable const TypeSymbol* type_ = nullptr;
    mutable const TypeSymbol* defaultType_ = nullptr;
    mutable ConstantValue* value_;
    mutable ConstantValue* defaultValue_;

    const ScopeSymbol* instanceScope = nullptr;
    const DataTypeSyntax* typeSyntax = nullptr;
    const ExpressionSyntax* defaultInitializer = nullptr;
    const ExpressionSyntax* assignedValue = nullptr;

    bool isLocal = false;
    bool isPort = false;
};

/// Represents a variable declaration (which does not include nets).
class VariableSymbol : public Symbol {
public:
    VariableLifetime lifetime;
    bool isConst;

    VariableSymbol(string_view name, const ScopeSymbol& parent,
                   VariableLifetime lifetime = VariableLifetime::Automatic, bool isConst = false);

    /// Constructs all variable symbols specified by the given syntax node.
    static void fromSyntax(const ScopeSymbol& parent, const DataDeclarationSyntax& syntax,
                           SmallVector<const VariableSymbol*>& results);

    const TypeSymbol& getType() const { return type.get(*getParent()); }
    void setType(const TypeSymbol& t) { type.set(&t); }
    void setType(const DataTypeSyntax& syntax) { type.set(syntax); }

    const Expression* getInitializer() const { return initializer.getOpt(*getParent()); }
    void setInitializer(const Expression* expr) { initializer.set(expr); }
    void setInitializer(const ExpressionSyntax& syntax) { initializer.set(syntax); }

protected:
    VariableSymbol(SymbolKind childKind, string_view name, const ScopeSymbol& parent,
                   VariableLifetime lifetime = VariableLifetime::Automatic, bool isConst = false);

private:
    LazyType type;
    LazyInitializer initializer;
};

/// Represents a formal argument in subroutine (task or function).
class FormalArgumentSymbol : public VariableSymbol {
public:
    FormalArgumentDirection direction = FormalArgumentDirection::In;

    FormalArgumentSymbol(const ScopeSymbol& parent);

    FormalArgumentSymbol(string_view name, const ScopeSymbol& parent,
                         FormalArgumentDirection direction = FormalArgumentDirection::In);
};

/// Represents a subroutine (task or function).
class SubroutineSymbol : public ScopeSymbol {
public:
    VariableLifetime defaultLifetime = VariableLifetime::Automatic;
    SystemFunction systemFunctionKind = SystemFunction::Unknown;
    bool isTask = false;

    SubroutineSymbol(string_view name, VariableLifetime defaultLifetime, bool isTask, const ScopeSymbol& parent);
    SubroutineSymbol(string_view name, SystemFunction systemFunction, const ScopeSymbol& parent);

    static SubroutineSymbol& fromSyntax(SymbolFactory& factory, const FunctionDeclarationSyntax& syntax,
                                        const ScopeSymbol& parent);

    const StatementList& getBody() const { return body.get(*this); }
    void setBody(const StatementList& stmt) { body.set(&stmt); }
    void setBody(const SyntaxList<SyntaxNode>& syntax) { body.set(syntax); }

    const TypeSymbol& getReturnType() const { return returnType.get(*this); }
    void setReturnType(const TypeSymbol& type) { returnType.set(&type); }
    void setReturnType(const DataTypeSyntax& syntax) { returnType.set(syntax); }

    using ArgList = span<const FormalArgumentSymbol* const>;
    ArgList getArguments() const { return arguments; }
    void setArguments(ArgList args) { arguments = args; }

    bool isSystemFunction() const { return systemFunctionKind != SystemFunction::Unknown; }

private:
    LazyStatementList body;
    LazyType returnType;
    ArgList arguments;
};

}
