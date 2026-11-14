/*
 * Ir/Ir.h
 *
 * CPIR in-memory representation: node kinds, flags and tree building.
 */

#ifndef CXXE_IR_IR_H
#define CXXE_IR_IR_H

#include "Core/Common.h"
#include "Lexer/Token.h"

typedef enum {
    N_TRANSLATION_UNIT = 1,
    N_RAW,
    N_PREPROCESSOR,
    N_INCLUDE,
    N_DEFINE,
    N_UNDEF,
    N_IFDEF,
    N_IFNDEF,
    N_IF,
    N_ELIF,
    N_ELSE,
    N_ENDIF,
    N_PRAGMA,
    N_ERROR_DIRECTIVE,
    N_WARNING_DIRECTIVE,
    N_ATTRIBUTE,
    N_DECORATOR,
    N_DECORATOR_TEMPLATE,
    N_DECORATOR_TEMPLATE_ARG,
    N_CUSTOM_DECORATOR,
    N_DECORATOR_TARGET,
    N_NAMESPACE,
    N_NAMESPACE_ALIAS,
    N_USING_DIRECTIVE,
    N_USING_DECL,
    N_LINKAGE_SPEC,
    N_MODULE,
    N_IMPORT,
    N_EXPORT,
    N_CLASS,
    N_STRUCT,
    N_UNION,
    N_ENUM,
    N_ENUMERATOR,
    N_TEMPLATE,
    N_TEMPLATE_TYPE_PARAM,
    N_TEMPLATE_NON_TYPE_PARAM,
    N_TEMPLATE_TEMPLATE_PARAM,
    N_CONCEPT,
    N_REQUIRES,
    N_TYPEDEF,
    N_ALIAS,
    N_FRIEND,
    N_FIELD_DECL,
    N_PROPERTY,
    N_PROPERTY_GET,
    N_PROPERTY_SET,
    N_PROPERTY_ACCESS,
    N_VAR_DECL,
    N_VAR_ASSIGN,
    N_FUNCTION,
    N_METHOD,
    N_CONSTRUCTOR,
    N_DESTRUCTOR,
    N_OPERATOR_FUNCTION,
    N_PARAM_DECL,
    N_RETURN,
    N_IF_STMT,
    N_FOR_STMT,
    N_RANGE_FOR_STMT,
    N_WHILE_STMT,
    N_DO_STMT,
    N_SWITCH_STMT,
    N_MATCH_STMT,
    N_MATCH_CASE,
    N_CASE_STMT,
    N_DEFAULT_STMT,
    N_BREAK_STMT,
    N_CONTINUE_STMT,
    N_GOTO_STMT,
    N_LABEL_STMT,
    N_TRY_STMT,
    N_CATCH,
    N_THROW,
    N_CO_RETURN,
    N_CO_AWAIT,
    N_CO_YIELD,
    N_EXPR,
    N_FUNC_CALL,
    N_CALL_ARG,
    N_NAMED_ARG,
    N_MEMBER_CALL,
    N_GET_MEMBER,
    N_VAR_REF,
    N_MEMBER_ACCESS,
    N_INDEX_EXPR,
    N_BINARY_EXPR,
    N_UNARY_EXPR,
    N_TERNARY_EXPR,
    N_ASSIGN_EXPR,
    N_CAST_EXPR,
    N_NEW_EXPR,
    N_DELETE_EXPR,
    N_LAMBDA,
    N_THIS_EXPR,
    N_INIT_LIST,
    N_LITERAL,
    N_SIZEOF_EXPR,
    N_ALIGNOF_EXPR,
    N_TYPEID_EXPR,
    N_NOEXCEPT_EXPR,
    N_TYPE_TRAIT,
    N_UNKNOWN
} NodeKind;

#define NF_ACCESS_MASK       0x00000003u

#define NF_ACCESS_PRIVATE    0x00000000u

#define NF_ACCESS_PROTECTED  0x00000001u

#define NF_ACCESS_PUBLIC     0x00000002u

#define NF_DECORATOR_NATIVE  0x00000010u

#define NF_DECORATOR_REGISTER 0x00000020u

#define NF_DECORATOR_EXPOSED  0x00000040u

#define NF_REGISTERED         0x00000800u

#define NF_EXPOSED            0x00001000u

#define NF_GET_MEMBER_RUNTIME 0x00002000u

#define NF_DECORATOR_CUSTOM   0x00000080u

#define NF_DECORATOR_VARIADIC 0x00000100u

#define NF_EXTERNAL_DECL     0x00004000u

#define NF_INCLUDE_NEXT      0x00008000u
#define NF_PARAM_HAS_DEFAULT 0x00010000u /* N_PARAM_DECL: value holds the default expression */

typedef struct {
    NodeKind kind;
    uint32_t flags;
    uint64_t first_tok;
    uint64_t last_tok; /* inclusive */
    char *name;
    char *qualified;
    char *type;
    char *return_type;
    char *operator_spelling;
    char *value;
    char *resolved_name;
    uint64_t aux;
    int64_t parent;
    int64_t first_child;
    int64_t last_child;
    int64_t next_sibling;
    int64_t prev_sibling;
} IRNode;

typedef struct {
    uint8_t *source;
    size_t source_size;
    TokenVec tokens;
    IRNode *nodes;
    size_t node_count;
    size_t node_cap;
    char *path; /* source path for diagnostics (not serialized) */
} IR;

void ir_init(IR *ir);
/* Replaces dst's node tree by src's (src loses its nodes). */
void ir_replace_nodes(IR *dst, IR *src);
/* Diagnostics located at a token: "<path>:<line>:<col>: error: ...". */
void ir_die_at(const IR *ir, uint64_t tok, const char *fmt, ...);
void ir_warn_at(const IR *ir, uint64_t tok, const char *fmt, ...);
void ir_free(IR *ir);
int64_t ir_add_node(IR *ir, NodeKind kind, uint64_t first, uint64_t last, int64_t parent);
void ir_add_child(IR *ir, int64_t parent, int64_t child);
const char *node_kind_name(NodeKind k);

#endif /* CXXE_IR_IR_H */
