/*
 * Developer-only parsed-AST adapter.  It links against upstream mrustc's
 * archives and never participates in cmrustc's trusted/bootstrap build.
 */
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <cstring>

#include "ast/crate.hpp"
#include "ast/expr.hpp"
#include "include/main_bindings.hpp"
#include "target_version.hpp"

TargetVersion gTargetVersion = TargetVersion::Rustc1_90;

enum DumpMode { DUMP_EXACT, DUMP_SEMANTIC };
enum TypeFeature {
    TYPE_INFER = 0, TYPE_NEVER, TYPE_PATH, TYPE_REFERENCE, TYPE_POINTER,
    TYPE_TUPLE, TYPE_SLICE, TYPE_ARRAY, TYPE_FUNCTION, TYPE_OTHER,
    TYPE_FEATURE_COUNT
};
enum ExprFeature {
    EXPR_LITERAL = 0, EXPR_BLOCK, EXPR_CALL, EXPR_METHOD, EXPR_FIELD,
    EXPR_INDEX, EXPR_UNARY, EXPR_BINARY, EXPR_ASSIGN, EXPR_CAST, EXPR_RANGE,
    EXPR_RETURN, EXPR_BREAK, EXPR_CONTINUE, EXPR_IF, EXPR_MATCH, EXPR_LOOP,
    EXPR_WHILE, EXPR_FOR, EXPR_CLOSURE, EXPR_TUPLE, EXPR_ARRAY, EXPR_STRUCT,
    EXPR_LET, EXPR_FEATURE_COUNT
};
enum PatternFeature {
    PAT_WILDCARD = 0, PAT_BINDING, PAT_LITERAL, PAT_PATH, PAT_REFERENCE,
    PAT_TUPLE, PAT_STRUCT, PAT_SLICE, PAT_OR, PAT_RANGE, PAT_FEATURE_COUNT
};
enum ItemFeature {
    ITEM_FUNCTION = 0, ITEM_STRUCT, ITEM_ENUM, ITEM_TYPE, ITEM_CONST,
    ITEM_STATIC, ITEM_MODULE, ITEM_USE, ITEM_EXTERN_CRATE, ITEM_EXTERN_BLOCK,
    ITEM_TRAIT, ITEM_IMPL, ITEM_UNION, ITEM_FEATURE_COUNT
};

struct Features {
    unsigned long items[ITEM_FEATURE_COUNT];
    unsigned long types[TYPE_FEATURE_COUNT];
    unsigned long expressions[EXPR_FEATURE_COUNT];
    unsigned long patterns[PAT_FEATURE_COUNT];
    unsigned long generic_types;
    unsigned long generic_lifetimes;
    unsigned long generic_consts;
    unsigned long functions_const;
    unsigned long functions_async;
    unsigned long functions_unsafe;
    unsigned long functions_with_body;

    Features() { std::memset(this, 0, sizeof(*this)); }
};

static const char *const item_names[ITEM_FEATURE_COUNT] = {
    "function", "struct", "enum", "type", "const", "static", "module",
    "use", "extern-crate", "extern-block", "trait", "impl", "union"
};
static const char *const type_names[TYPE_FEATURE_COUNT] = {
    "infer", "never", "path", "reference", "pointer", "tuple", "slice",
    "array", "function", "other"
};
static const char *const expr_names[EXPR_FEATURE_COUNT] = {
    "literal", "block", "call", "method", "field", "index", "unary",
    "binary", "assign", "cast", "range", "return", "break", "continue",
    "if", "match", "loop", "while", "for", "closure", "tuple", "array",
    "struct", "let"
};
static const char *const pattern_names[PAT_FEATURE_COUNT] = {
    "wildcard", "binding", "literal", "path", "reference", "tuple",
    "struct", "slice", "or", "range"
};

static AST::Edition parse_edition(const char *text)
{
    const std::string value(text);
    if (value == "2015") return AST::Edition::Rust2015;
    if (value == "2018") return AST::Edition::Rust2018;
    if (value == "2021") return AST::Edition::Rust2021;
    if (value == "2024") return AST::Edition::Rust2024;
    throw std::runtime_error("unsupported edition: " + value);
}

static DumpMode parse_mode(const char *text)
{
    const std::string value(text);
    if (value == "exact") return DUMP_EXACT;
    if (value == "semantic") return DUMP_SEMANTIC;
    throw std::runtime_error("unsupported mode: " + value);
}

static const char *visibility_name(const AST::Visibility& visibility)
{
    switch (visibility.ty()) {
    case AST::Visibility::Ty::Private: return "private";
    case AST::Visibility::Ty::Pub: return "public";
    case AST::Visibility::Ty::Crate:
    case AST::Visibility::Ty::PubCrate: return "crate";
    case AST::Visibility::Ty::PubSelf: return "self";
    case AST::Visibility::Ty::PubSuper: return "super";
    case AST::Visibility::Ty::PubIn: return "restricted";
    }
    return "unknown";
}

static void print_path(const AST::Path& path)
{
    if (path.m_class.is_Local()) {
        std::cout << path.m_class.as_Local().name;
        return;
    }
    if (path.m_class.is_Absolute()) std::cout << "::";
    const auto& nodes = path.nodes();
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (index != 0) std::cout << "::";
        std::cout << nodes[index].name();
    }
}

static void count_type(const TypeRef& type, Features& features);

static void print_type(const TypeRef& type)
{
    const auto& data = type.m_data;
    if (data.is_None() || data.is_Any()) {
        std::cout << "_";
    } else if (data.is_Bang()) {
        std::cout << "!";
    } else if (data.is_Unit()) {
        std::cout << "tuple()";
    } else if (data.is_Primitive()) {
        std::cout << "path(" << coretype_name(data.as_Primitive().core_type)
                  << ")";
    } else if (data.is_Generic()) {
        std::cout << "path(" << data.as_Generic().name << ")";
    } else if (data.is_Path()) {
        std::cout << "path(";
        print_path(*data.as_Path());
        std::cout << ")";
    } else if (data.is_Borrow()) {
        const auto& inner = data.as_Borrow();
        std::cout << (inner.is_mut ? "ref(mut," : "ref(shared,");
        print_type(*inner.inner);
        std::cout << ")";
    } else if (data.is_Pointer()) {
        const auto& inner = data.as_Pointer();
        std::cout << (inner.is_mut ? "ptr(mut," : "ptr(const,");
        print_type(*inner.inner);
        std::cout << ")";
    } else if (data.is_Tuple()) {
        std::cout << "tuple(";
        const auto& types = data.as_Tuple().inner_types;
        for (std::size_t index = 0; index < types.size(); ++index) {
            if (index != 0) std::cout << ',';
            print_type(types[index]);
        }
        std::cout << ")";
    } else if (data.is_Slice()) {
        std::cout << "slice(";
        print_type(*data.as_Slice().inner);
        std::cout << ")";
    } else if (data.is_Array()) {
        std::cout << "array(";
        print_type(*data.as_Array().inner);
        std::cout << ")";
    } else if (data.is_Function()) {
        const auto& function = data.as_Function().info;
        std::cout << (function.is_unsafe ? "fn(unsafe;" : "fn(safe;");
        for (std::size_t index = 0; index < function.m_arg_types.size();
             ++index) {
            if (index != 0) std::cout << ',';
            print_type(function.m_arg_types[index]);
        }
        std::cout << "->";
        print_type(*function.m_rettype);
        std::cout << ")";
    } else if (data.is_TraitObject()) {
        std::cout << "trait-object";
    } else if (data.is_ErasedType()) {
        std::cout << "erased-type";
    } else {
        std::cout << "other";
    }
}

static void count_type(const TypeRef& type, Features& features)
{
    const auto& data = type.m_data;
    if (data.is_None() || data.is_Any()) {
        return;
    } else if (data.is_Bang()) {
        features.types[TYPE_NEVER] += 1;
    } else if (data.is_Primitive() || data.is_Generic() || data.is_Path()) {
        features.types[TYPE_PATH] += 1;
    } else if (data.is_Unit()) {
        return;
    } else if (data.is_Borrow()) {
        features.types[TYPE_REFERENCE] += 1;
        count_type(*data.as_Borrow().inner, features);
    } else if (data.is_Pointer()) {
        features.types[TYPE_POINTER] += 1;
        count_type(*data.as_Pointer().inner, features);
    } else if (data.is_Tuple()) {
        features.types[TYPE_TUPLE] += 1;
        for (const auto& inner : data.as_Tuple().inner_types)
            count_type(inner, features);
    } else if (data.is_Slice()) {
        features.types[TYPE_SLICE] += 1;
        count_type(*data.as_Slice().inner, features);
    } else if (data.is_Array()) {
        features.types[TYPE_ARRAY] += 1;
        count_type(*data.as_Array().inner, features);
    } else if (data.is_Function()) {
        features.types[TYPE_FUNCTION] += 1;
        for (const auto& argument : data.as_Function().info.m_arg_types)
            count_type(argument, features);
        count_type(*data.as_Function().info.m_rettype, features);
    } else {
        features.types[TYPE_OTHER] += 1;
    }
}

static void count_pattern(const AST::Pattern& pattern, Features& features);

static void count_pattern_value(const AST::Pattern::Value& value,
    Features& features)
{
    if (value.is_Named()) features.patterns[PAT_PATH] += 1;
    else features.patterns[PAT_LITERAL] += 1;
}

static void count_tuple_pattern(const AST::Pattern::TuplePat& tuple,
    Features& features)
{
    for (const auto& pattern : tuple.start) count_pattern(pattern, features);
    for (const auto& pattern : tuple.end) count_pattern(pattern, features);
}

static void count_pattern(const AST::Pattern& pattern, Features& features)
{
    const auto& data = pattern.data();
    bool has_binding = false;
    for (const auto& binding : pattern.bindings()) {
        if (binding.is_valid()) {
            features.patterns[PAT_BINDING] += 1;
            has_binding = true;
        }
    }
    if (data.is_MaybeBind()) {
        features.patterns[PAT_BINDING] += 1;
    } else if (data.is_Any()) {
        if (!has_binding) features.patterns[PAT_WILDCARD] += 1;
    } else if (data.is_Box()) {
        features.patterns[PAT_REFERENCE] += 1;
        count_pattern(*data.as_Box().sub, features);
    } else if (data.is_Ref()) {
        features.patterns[PAT_REFERENCE] += 1;
        count_pattern(*data.as_Ref().sub, features);
    } else if (data.is_Value() || data.is_ValueLeftInc()) {
        const AST::Pattern::Value *start;
        const AST::Pattern::Value *end;
        if (data.is_Value()) {
            start = &data.as_Value().start;
            end = &data.as_Value().end;
        } else {
            start = &data.as_ValueLeftInc().start;
            end = &data.as_ValueLeftInc().end;
        }
        if (!end->is_Invalid()) {
            features.patterns[PAT_RANGE] += 1;
            count_pattern_value(*start, features);
            count_pattern_value(*end, features);
        } else {
            count_pattern_value(*start, features);
        }
    } else if (data.is_Tuple()) {
        features.patterns[PAT_TUPLE] += 1;
        count_tuple_pattern(data.as_Tuple(), features);
    } else if (data.is_StructTuple()) {
        features.patterns[PAT_STRUCT] += 1;
        count_tuple_pattern(data.as_StructTuple().tup_pat, features);
    } else if (data.is_Struct()) {
        features.patterns[PAT_STRUCT] += 1;
        for (const auto& entry : data.as_Struct().sub_patterns)
            count_pattern(entry.pat, features);
    } else if (data.is_Slice()) {
        features.patterns[PAT_SLICE] += 1;
        for (const auto& child : data.as_Slice().sub_pats)
            count_pattern(child, features);
    } else if (data.is_SplitSlice()) {
        features.patterns[PAT_SLICE] += 1;
        for (const auto& child : data.as_SplitSlice().leading)
            count_pattern(child, features);
        if (data.as_SplitSlice().extra_bind.is_valid())
            features.patterns[PAT_BINDING] += 1;
        for (const auto& child : data.as_SplitSlice().trailing)
            count_pattern(child, features);
    } else if (data.is_Or()) {
        features.patterns[PAT_OR] += 1;
        for (const auto& child : data.as_Or()) count_pattern(child, features);
    }
}

static const char *pattern_kind_name(const AST::Pattern& pattern)
{
    const auto& data = pattern.data();
    if (data.is_Tuple()) return "tuple";
    if (data.is_StructTuple() || data.is_Struct()) return "struct";
    if (data.is_Slice() || data.is_SplitSlice()) return "slice";
    if (data.is_Or()) return "or";
    if (data.is_Ref() || data.is_Box()) return "reference";
    if (data.is_Value() || data.is_ValueLeftInc()) {
        const AST::Pattern::Value& start = data.is_Value() ?
            data.as_Value().start : data.as_ValueLeftInc().start;
        const AST::Pattern::Value& end = data.is_Value() ?
            data.as_Value().end : data.as_ValueLeftInc().end;
        if (!end.is_Invalid()) return "range";
        return start.is_Named() ? "path" : "literal";
    }
    if (data.is_MaybeBind() || !pattern.bindings().empty()) return "binding";
    if (data.is_Any()) return "wildcard";
    return "none";
}

class ExprFeatures : public AST::NodeVisitorDef {
    Features& features;
public:
    explicit ExprFeatures(Features& features): features(features) {}
    bool is_const() const override { return true; }

    void visit(AST::ExprNode_Block& node) override {
        features.expressions[EXPR_BLOCK] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Flow& node) override {
        switch (node.m_type) {
        case AST::ExprNode_Flow::RETURN:
        case AST::ExprNode_Flow::YIELD:
        case AST::ExprNode_Flow::YEET:
            features.expressions[EXPR_RETURN] += 1; break;
        case AST::ExprNode_Flow::BREAK:
            features.expressions[EXPR_BREAK] += 1; break;
        case AST::ExprNode_Flow::CONTINUE:
            features.expressions[EXPR_CONTINUE] += 1; break;
        }
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_LetBinding& node) override {
        features.expressions[EXPR_LET] += 1;
        count_pattern(node.m_pat, features);
        count_type(node.m_type, features);
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Assign& node) override {
        features.expressions[EXPR_ASSIGN] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_CallPath& node) override {
        features.expressions[EXPR_CALL] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_CallMethod& node) override {
        features.expressions[EXPR_METHOD] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_CallObject& node) override {
        features.expressions[EXPR_CALL] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Loop& node) override {
        features.expressions[EXPR_LOOP] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_For& node) override {
        features.expressions[EXPR_FOR] += 1;
        count_pattern(node.m_pattern, features);
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_While& node) override {
        features.expressions[EXPR_WHILE] += 1;
        if (node.m_conditions.size() > 1)
            features.expressions[EXPR_BINARY] += node.m_conditions.size() - 1;
        for (const auto& condition : node.m_conditions) {
            if (condition.opt_pat) {
                features.expressions[EXPR_LET] += 1;
                count_pattern(*condition.opt_pat, features);
            }
        }
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Match& node) override {
        features.expressions[EXPR_MATCH] += 1;
        for (const auto& arm : node.m_arms) {
            for (const auto& pattern : arm.m_patterns)
                count_pattern(pattern, features);
            if (arm.m_guard.size() > 1)
                features.expressions[EXPR_BINARY] += arm.m_guard.size() - 1;
            for (const auto& condition : arm.m_guard) {
                if (condition.opt_pat) {
                    features.expressions[EXPR_LET] += 1;
                    count_pattern(*condition.opt_pat, features);
                }
            }
        }
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_If& node) override {
        features.expressions[EXPR_IF] += 1;
        for (const auto& arm : node.m_arms) {
            if (arm.m_conditions.size() > 1)
                features.expressions[EXPR_BINARY] +=
                    arm.m_conditions.size() - 1;
            for (const auto& condition : arm.m_conditions) {
                if (condition.opt_pat) {
                    features.expressions[EXPR_LET] += 1;
                    count_pattern(*condition.opt_pat, features);
                }
            }
        }
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Integer& node) override {
        features.expressions[EXPR_LITERAL] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Float& node) override {
        features.expressions[EXPR_LITERAL] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Bool& node) override {
        features.expressions[EXPR_LITERAL] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_String& node) override {
        features.expressions[EXPR_LITERAL] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_ByteString& node) override {
        features.expressions[EXPR_LITERAL] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_CString& node) override {
        features.expressions[EXPR_LITERAL] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Closure& node) override {
        features.expressions[EXPR_CLOSURE] += 1;
        for (const auto& argument : node.m_args) {
            count_pattern(argument.first, features);
            count_type(argument.second, features);
        }
        count_type(node.m_return, features);
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_StructLiteral& node) override {
        features.expressions[EXPR_STRUCT] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Array& node) override {
        features.expressions[EXPR_ARRAY] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Tuple& node) override {
        features.expressions[EXPR_TUPLE] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Field& node) override {
        features.expressions[EXPR_FIELD] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Index& node) override {
        features.expressions[EXPR_INDEX] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Deref& node) override {
        features.expressions[EXPR_UNARY] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_Cast& node) override {
        features.expressions[EXPR_CAST] += 1;
        count_type(node.m_type, features);
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_BinOp& node) override {
        if (node.m_type == AST::ExprNode_BinOp::RANGE ||
            node.m_type == AST::ExprNode_BinOp::RANGE_INC)
            features.expressions[EXPR_RANGE] += 1;
        else
            features.expressions[EXPR_BINARY] += 1;
        AST::NodeVisitorDef::visit(node);
    }
    void visit(AST::ExprNode_UniOp& node) override {
        features.expressions[EXPR_UNARY] += 1;
        AST::NodeVisitorDef::visit(node);
    }
};

static void count_expr(const AST::Expr& expression, Features& features)
{
    if (!expression) return;
    ExprFeatures visitor(features);
    std::ostringstream trace;
    std::streambuf *normal_output = std::cout.rdbuf(trace.rdbuf());
    expression.visit_nodes(visitor);
    std::cout.rdbuf(normal_output);
}

static bool is_self_pattern(const AST::Pattern& pattern)
{
    for (const auto& binding : pattern.bindings())
        if (binding.is_valid() && binding.m_name.name == "self") return true;
    if (pattern.data().is_MaybeBind())
        return pattern.data().as_MaybeBind().name.name == "self";
    return false;
}

static void count_generics(const AST::GenericParams& generics,
    Features& features)
{
    for (const auto& parameter : generics.m_params) {
        if (parameter.is_Type()) features.generic_types += 1;
        else if (parameter.is_Lifetime()) features.generic_lifetimes += 1;
        else if (parameter.is_Value()) features.generic_consts += 1;
    }
}

static const char *field_form_name(const AST::StructData& data)
{
    if (data.is_Unit()) return "unit";
    if (data.is_Tuple()) return "tuple";
    return "named";
}

static void print_features(const Features& features, const char *prefix)
{
    for (unsigned int index = 0; index < EXPR_FEATURE_COUNT; ++index)
        if (features.expressions[index] != 0)
            std::cout << prefix << "\texpr." << expr_names[index] << "="
                      << features.expressions[index] << "\n";
    for (unsigned int index = 0; index < PAT_FEATURE_COUNT; ++index)
        if (features.patterns[index] != 0)
            std::cout << prefix << "\tpattern." << pattern_names[index]
                      << "=" << features.patterns[index] << "\n";
}

static void visit_named_item(const AST::Named<AST::Item>& named,
    unsigned int depth, DumpMode mode, Features& all);

static void visit_function(const AST::Function& function, unsigned int depth,
    DumpMode mode, Features& all)
{
    Features local;
    count_generics(function.params(), all);
    all.functions_const += function.is_const();
    all.functions_async += function.is_async();
    all.functions_unsafe += function.is_unsafe() &&
        (function.abi().empty() || function.abi() == "Rust");
    all.functions_with_body += function.code().is_valid();
    for (const auto& argument : function.args()) {
        const bool is_self = is_self_pattern(argument.pat);
        if (!is_self) {
            count_pattern(argument.pat, all);
            count_type(argument.ty, all);
        }
        if (mode == DUMP_EXACT) {
            std::cout << "param\t" << depth << "\tself=" << is_self << "\t"
                      << pattern_kind_name(argument.pat) << "\t";
            print_type(argument.ty);
            std::cout << "\n";
        }
    }
    count_type(function.rettype(), all);
    count_expr(function.code(), all);
    count_expr(function.code(), local);
    if (mode == DUMP_EXACT) {
        std::cout << "function\t" << depth
                  << "\tconst=" << function.is_const()
                  << "\tasync=" << function.is_async()
                  << "\tunsafe=" << function.is_unsafe()
                  << "\tabi=" << (function.abi().empty() ? "Rust" : function.abi())
                  << "\tparams=" << function.args().size()
                  << "\tbody=" << function.code().is_valid() << "\treturn=";
        print_type(function.rettype());
        std::cout << "\n";
        print_features(local, "body");
    }
}

static void visit_item_data(const AST::Item& item, const AST::Visibility& vis,
    const RcString& name, unsigned int depth, DumpMode mode, Features& all)
{
    ItemFeature feature;
    std::size_t generic_count = 0;
    if (item.is_Function()) {
        feature = ITEM_FUNCTION;
        generic_count = item.as_Function().params().m_params.size();
    } else if (item.is_Struct()) {
        feature = ITEM_STRUCT;
        generic_count = item.as_Struct().params().m_params.size();
    } else if (item.is_Union()) {
        feature = ITEM_UNION;
        generic_count = item.as_Union().params().m_params.size();
    } else if (item.is_Enum()) {
        feature = ITEM_ENUM;
        generic_count = item.as_Enum().params().m_params.size();
    } else if (item.is_Type()) {
        feature = ITEM_TYPE;
        generic_count = item.as_Type().params().m_params.size();
    } else if (item.is_Static()) {
        feature = item.as_Static().s_class() == AST::Static::CONST ?
            ITEM_CONST : ITEM_STATIC;
    } else if (item.is_Module()) feature = ITEM_MODULE;
    else if (item.is_Use()) feature = ITEM_USE;
    else if (item.is_Crate()) feature = ITEM_EXTERN_CRATE;
    else if (item.is_ExternBlock()) feature = ITEM_EXTERN_BLOCK;
    else if (item.is_Trait()) {
        feature = ITEM_TRAIT;
        generic_count = item.as_Trait().params().m_params.size();
    } else if (item.is_Impl() || item.is_NegImpl()) {
        feature = ITEM_IMPL;
        if (item.is_Impl())
            generic_count = item.as_Impl().def().params().m_params.size();
        else
            generic_count = item.as_NegImpl().params().m_params.size();
    } else {
        return;
    }
    all.items[feature] += 1;
    if (mode == DUMP_EXACT) {
        std::cout << "item\t" << depth << "\t" << item_names[feature]
                  << "\t" << visibility_name(vis) << "\t"
                  << (name == "" ? "_" : name.c_str())
                  << "\tgenerics=" << generic_count << "\n";
    }

    if (item.is_Function()) {
        visit_function(item.as_Function(), depth, mode, all);
    } else if (item.is_Struct()) {
        const auto& structure = item.as_Struct();
        count_generics(structure.params(), all);
        std::size_t field_count = structure.m_data.is_Unit() ? 0 :
            (structure.m_data.is_Tuple() ? structure.m_data.as_Tuple().ents.size() :
             structure.m_data.as_Struct().ents.size());
        if (mode == DUMP_EXACT)
            std::cout << "aggregate\t" << depth << "\t"
                      << field_form_name(structure.m_data) << "\tfields="
                      << field_count << "\n";
        if (structure.m_data.is_Tuple()) {
            for (const auto& field : structure.m_data.as_Tuple().ents) {
                count_type(field.m_type, all);
                if (mode == DUMP_EXACT) {
                    std::cout << "field\t" << depth << "\t"
                              << visibility_name(field.m_vis) << "\t_\t";
                    print_type(field.m_type); std::cout << "\n";
                }
            }
        } else if (structure.m_data.is_Struct()) {
            for (const auto& field : structure.m_data.as_Struct().ents) {
                count_type(field.m_type, all);
                if (mode == DUMP_EXACT) {
                    std::cout << "field\t" << depth << "\t"
                              << visibility_name(field.m_vis) << "\t"
                              << field.m_name << "\t";
                    print_type(field.m_type); std::cout << "\n";
                }
            }
        }
    } else if (item.is_Union()) {
        const auto& union_ = item.as_Union();
        count_generics(union_.params(), all);
        if (mode == DUMP_EXACT)
            std::cout << "aggregate\t" << depth
                      << "\tnamed\tfields=" << union_.m_variants.size()
                      << "\n";
        for (const auto& field : union_.m_variants) {
            count_type(field.m_type, all);
            if (mode == DUMP_EXACT) {
                std::cout << "field\t" << depth << "\t"
                          << visibility_name(field.m_vis) << "\t"
                          << field.m_name << "\t";
                print_type(field.m_type); std::cout << "\n";
            }
        }
    } else if (item.is_Enum()) {
        const auto& enumeration = item.as_Enum();
        count_generics(enumeration.params(), all);
        for (const auto& variant : enumeration.variants()) {
            const char *form = variant.m_data.is_Unit() ? "unit" :
                (variant.m_data.is_Tuple() ? "tuple" : "named");
            std::size_t field_count = variant.m_data.is_Unit() ? 0 :
                (variant.m_data.is_Tuple() ? variant.m_data.as_Tuple().m_items.size() :
                 variant.m_data.as_Struct().m_fields.size());
            if (mode == DUMP_EXACT)
                std::cout << "variant\t" << depth << "\t" << form << "\t"
                          << variant.m_name << "\tfields=" << field_count
                          << "\tdiscriminant="
                          << variant.m_discriminant_value.is_valid() << "\n";
            if (variant.m_data.is_Tuple())
                for (const auto& field : variant.m_data.as_Tuple().m_items)
                    count_type(field.m_type, all);
            if (variant.m_data.is_Struct())
                for (const auto& field : variant.m_data.as_Struct().m_fields)
                    count_type(field.m_type, all);
        }
    } else if (item.is_Type()) {
        count_generics(item.as_Type().params(), all);
        count_type(item.as_Type().type(), all);
        if (mode == DUMP_EXACT) {
            std::cout << "value\t" << depth
                      << "\tmutable=0\tinitialized=1\ttype=";
            print_type(item.as_Type().type()); std::cout << "\n";
        }
    } else if (item.is_Static()) {
        const auto& value = item.as_Static();
        count_type(value.type(), all);
        count_expr(value.value(), all);
        if (mode == DUMP_EXACT) {
            std::cout << "value\t" << depth << "\tmutable="
                      << (value.s_class() == AST::Static::MUT)
                      << "\tinitialized=" << value.value().is_valid()
                      << "\ttype=";
            print_type(value.type()); std::cout << "\n";
        }
    } else if (item.is_Module()) {
        const auto& module = item.as_Module();
        if (mode == DUMP_EXACT)
            std::cout << "module\t" << depth << "\tinline=1\titems="
                      << module.m_items.size() << "\n";
        for (const auto& child : module.m_items)
            visit_named_item(*child, depth + 1, mode, all);
    } else if (item.is_ExternBlock()) {
        for (const auto& child : item.as_ExternBlock().items())
            visit_named_item(child, depth + 1, mode, all);
    } else if (item.is_Trait()) {
        const auto& trait = item.as_Trait();
        count_generics(trait.params(), all);
        for (const auto& supertrait : trait.supertraits())
            if (supertrait.ent.path) all.types[TYPE_PATH] += 1;
        for (const auto& child : trait.items())
            visit_named_item(child, depth + 1, mode, all);
    } else if (item.is_Impl()) {
        const auto& implementation = item.as_Impl();
        count_generics(implementation.def().params(), all);
        count_type(implementation.def().type(), all);
        if (implementation.def().trait().ent.is_valid())
            all.types[TYPE_PATH] += 1;
        for (const auto& child : implementation.items())
            visit_item_data(*child.data, child.vis, child.name, depth + 1,
                mode, all);
    } else if (item.is_NegImpl()) {
        count_generics(item.as_NegImpl().params(), all);
        count_type(item.as_NegImpl().type(), all);
    }
}

static void visit_named_item(const AST::Named<AST::Item>& named,
    unsigned int depth, DumpMode mode, Features& all)
{
    visit_item_data(named.data, named.vis, named.name, depth, mode, all);
}

static void print_semantic(const Features& features)
{
    std::cout << "schema\tcmrustc-ast-facts-v1\n";
    for (unsigned int index = 0; index < ITEM_FEATURE_COUNT; ++index)
        std::cout << "item." << item_names[index] << "\t"
                  << features.items[index] << "\n";
    for (unsigned int index = 0; index < TYPE_FEATURE_COUNT; ++index)
        std::cout << "type." << type_names[index] << "\t"
                  << features.types[index] << "\n";
    for (unsigned int index = 0; index < EXPR_FEATURE_COUNT; ++index)
        std::cout << "expr." << expr_names[index] << "\t"
                  << features.expressions[index] << "\n";
    for (unsigned int index = 0; index < PAT_FEATURE_COUNT; ++index)
        std::cout << "pattern." << pattern_names[index] << "\t"
                  << features.patterns[index] << "\n";
    std::cout << "generic.type\t" << features.generic_types << "\n"
              << "generic.lifetime\t" << features.generic_lifetimes << "\n"
              << "generic.const\t" << features.generic_consts << "\n"
              << "function.const\t" << features.functions_const << "\n"
              << "function.async\t" << features.functions_async << "\n"
              << "function.unsafe\t" << features.functions_unsafe << "\n"
              << "function.body\t" << features.functions_with_body << "\n";
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " MODE EDITION FILE\n";
        return 2;
    }
    try {
        const DumpMode mode = parse_mode(argv[1]);
        std::ostringstream parser_trace;
        std::streambuf *normal_output = std::cout.rdbuf(parser_trace.rdbuf());
        AST::Crate crate = Parse_Crate(argv[3], parse_edition(argv[2]));
        std::cout.rdbuf(normal_output);
        Features features;
        if (mode == DUMP_EXACT)
            std::cout << "schema\tcmrustc-ast-facts-v1\n";
        for (const auto& item : crate.root_module().m_items)
            visit_named_item(*item, 0, mode, features);
        if (mode == DUMP_SEMANTIC) print_semantic(features);
    } catch (const std::exception& error) {
        std::cerr << "mrustc AST oracle: " << error.what() << "\n";
        return 1;
    } catch (const char *error) {
        std::cerr << "mrustc AST oracle: " << error << "\n";
        return 1;
    } catch (...) {
        std::cerr << "mrustc AST oracle: unknown exception\n";
        return 1;
    }
    return 0;
}
