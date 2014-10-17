#include "lcc.h"
#include "symbol.h"
#include "ir.h"

#include <stdlib.h>
#include <string.h>

typedef struct node {
    const char *text;
    struct token token;
    long value;
    struct node **children;
    size_t nc;
    size_t cap;
} node_t;

/* Tokenization interface and helper functions */
static struct token peek_value;
static int has_value;
static int eof;

static struct token
readtoken()
{
    struct token t;
    if (has_value) {
        has_value = 0;
        return peek_value;
    }
    eof = !get_token(&t);
    return t;
}

static enum token_type
peek()
{
    if (!has_value) {
        peek_value = readtoken();
        has_value = 1;
    }
    return peek_value.type;
}

static void
consume(enum token_type expected)
{
    struct token t = readtoken();
    if (t.type != expected) {
        error("Unexpected token %s, aborting\n", t.value);
        exit(1);
    }
}

/* Parse tree helper functions */
static struct node *
init_node(const char *name, size_t n)
{
    struct node *node = malloc(sizeof(node_t));
    node->text = name;
    node->value = 0;
    node->nc = 0;
    node->cap = n;
    if (n) node->children = malloc(sizeof(node_t *) * node->cap);
    else node->children = NULL;
    /* todo: add to free list */
    return node;
}

static void
addchild(node_t *node, node_t *child)
{
    if (node->nc == node->cap) {
        node->cap += 8;
        node->children = realloc(node->children, sizeof(node_t *) * node->cap);
    }
    node->children[node->nc] = child;
    node->nc++;
}

static typetree_t *
init_typetree(enum tree_type type)
{
    typetree_t *tree = calloc(1, sizeof(typetree_t));
    tree->type = type;
    return tree;
}

static node_t *declaration();
static typetree_t *declaration_specifiers();
static typetree_t *declarator(typetree_t *, const char **);
static typetree_t *pointer(typetree_t *);
static typetree_t *direct_declarator(typetree_t *, const char **);
static typetree_t *parameter_list(typetree_t *);
static node_t *block();
static node_t *statement();
static node_t *identifier();

/* expression nodes that are called in high level rules */
static symbol_t *expression();
static symbol_t *constant_expression();
static symbol_t *assignment_expression();

static void output_tree(int indent, struct node *tree);


/* External interface */
int
parse()
{
    peek();
    while (!eof) {
        node_t *node = declaration();
        if (node != NULL) {
            output_tree(0, node);
            puts("");
            return 1;
        }
        peek();
    }
    return 0;
}

/* Print parse tree in human readable format */
static void 
output_tree(int indent, struct node *tree)
{
    int i;
    if (tree == NULL) {
        printf("%*s(null)", indent, "");
        return;
    }
    printf("%*s(%s", indent, "", tree->text);
    if (!strcmp("integer", tree->text)) {
        printf(" %ld", tree->value);
    } else if (tree->token.value != NULL) {
        printf(" \"%s\"", tree->token.value);
    }
    if (tree->nc > 0) {
        printf("\n");
        for (i = 0; i < tree->nc; ++i) {
            output_tree(indent + 2, tree->children[i]);
            if (i < tree->nc - 1)
                printf("\n");
        }
    }
    printf(")");
}

/* Return either an initialized declaration, or a function definition.
 * Forward declarations are just registered in the symbol table. 
 */
static node_t *
declaration()
{
    node_t *node = NULL, *child = NULL;
    symbol_t *symbol;
    typetree_t *type, *base = declaration_specifiers();
    int i;

    do {
        const char *name = NULL;
        type = declarator(base, &name);
        symbol = sym_add(name, type);
        switch (peek()) {
            case ';':
                consume(';');
                return node;
            case '=':
                consume('=');
                if (node == NULL)
                    node = init_node("declaration", 0);
                child = init_node("assignment", 0);
                addchild(node, child);
                addchild(child, init_node("assignment-expression-dummy", 0));
                assignment_expression(); /* generate assignment ir */
                child->token.type = IDENTIFIER;
                child->token.value = name;
                if (peek() != ',') {
                    consume(';');
                    return node;
                }
                break;
            case '{':
                if (type->type != FUNCTION || node != NULL || symbol->depth > 0) {
                    error("Invalid function definition, aborting");
                    exit(1);
                }
                node = init_node("function-definition", 0);
                node->token.type = IDENTIFIER;
                node->token.value = name;
                mkblock(name);
                push_scope();
                for (i = 0; i < type->d.func.n_args; ++i) {
                    if (type->d.func.params[i] == NULL) {
                        error("Missing parameter name at position %d, aborting", i + 1);
                        exit(1);
                    }
                    sym_add(type->d.func.params[i], type->d.func.args[i]);
                }
                addchild(node, block());
                pop_scope();
                return node;
            default: 
                break;
        }
        consume(',');
    } while (1);
}

static typetree_t *
declaration_specifiers()
{
    int end = 0;
    typetree_t *type = init_typetree(BASIC);
    type->d.basic.qualifier = NONE_Q;
    type->d.basic.type = NONE_T;
    while (1) {
        switch (peek()) {
            case AUTO: case REGISTER: case STATIC: case EXTERN: case TYPEDEF:
                /* todo: something about storage class, maybe do it before this */
                break;
            case CHAR:
                type->d.basic.type = CHAR_T;
                break;
            case SHORT:
            case INT:
            case LONG:
            case SIGNED:
            case UNSIGNED:
                type->d.basic.type = INT64_T;
                break;
            case FLOAT:
            case DOUBLE:
                type->d.basic.type = DOUBLE_T;
                break;
            case VOID:
                type->d.basic.type = VOID_T;
                break;
            case CONST:
                type->d.ptr.qualifier |= CONST_Q;
                break;
            case VOLATILE:
                type->d.ptr.qualifier |= VOLATILE_Q;
                break;
            default:
                end = 1;
        }
        if (end) break;
        consume(peek());
    }
    if (type->d.basic.type == NONE_T) {
        error("Missing type specifier, aborting");
        exit(1);
    }
    return type;
}

static typetree_t *
declarator(typetree_t *base, const char **symbol)
{
    while (peek() == '*') {
        base = pointer(base);
    }
    return direct_declarator(base, symbol);
}

static typetree_t *
pointer(typetree_t *base)
{
    typetree_t *type = init_typetree(POINTER);
    type->d.ptr.to = base;
    base = type;
    consume('*');
    while (peek() == CONST || peek() == VOLATILE) {
        type->d.ptr.qualifier |= (readtoken().type == CONST) ? CONST_Q : VOLATILE_Q;
    }
    return type;
}

static long
get_symbol_constant_value(symbol_t *symbol, long *out)
{
    if (symbol->type->type == BASIC
        && symbol->type->d.basic.type == INT64_T
        && symbol->value != NULL) {
        *out = *(long *)symbol->value;
        return 1;
    }
    return 0;
}

/* Consume [s0][s1][s2]..[sn] in array declarations, returning type
 * <symbol> :: [s0] [s1] [s2] .. [sn] (base)
 */
static typetree_t *
direct_declarator_array(typetree_t *base)
{
    typetree_t *root;
    symbol_t *expr;
    long length;

    if (peek() != '[') {
        switch (base->type) {
            case BASIC:
                if (base->d.basic.type == CHAR_T) {
                    base->size = 1;
                    break;    
                }
            default:
                base->size = 8;
        }
        return base;
    }

    consume('[');
    if (peek() != ']') {
        expr = constant_expression();
        if (!get_symbol_constant_value(expr, &length)) {
            error("Array declaration must be a compile time constant, aborting");
            exit(1);
        }
        if (length < 1) {
            error("Invalid array size %ld, aborting");
            exit(1);
        }
    } else {
        /* special value for unspecified array size */
        length = 0;
    }
    consume(']');
    
    base = direct_declarator_array(base);
    root = init_typetree(ARRAY);

    root->d.arr.of = base;
    root->length = length;
    root->size = (base->type == ARRAY) ? base->size * base->length : base->size;
    return root;
}

static typetree_t *
direct_declarator(typetree_t *base, const char **symbol)
{
    typetree_t *type = base;
    switch (peek()) {
        case IDENTIFIER: 
            *symbol = readtoken().value;
            break;
        case '(':
            consume('(');
            type = declarator(base, symbol);
            consume(')');
            break;
        default: break;
    }
    /* left-recursive declarations like 'int foo[10][5];' */


    while (peek() == '[' || peek() == '(') {
        switch (peek()) {
            case '[':
                type = direct_declarator_array(base);
                /*type = init_typetree(ARRAY);
                type->d.arr.of = base;
                consume('[');
                if (peek() != ']') {
                    symbol_t *expr = constant_expression();
                    long size;
                    if (!get_symbol_constant_value(expr, &size)) {
                        error("Array declaration must be a compile time constant, aborting");
                        exit(1);
                    }
                    if (size < 1) {
                        error("Invalid array size %ld, aborting");
                        exit(1);
                    }
                    type->d.arr.size = size;
                }
                consume(']');*/
                break;
            case '(': {
                consume('(');
                type = parameter_list(base);
                consume(')');
                break;
            }
            default: break; /* impossible */
        }
        base = type;
    }
    return type;
}

/* FOLLOW(parameter-list) = { ')' }, peek to return empty list;
 * even though K&R require at least specifier: (void)
 * Set parameter-type-list = parameter-list, including the , ...
 */
static typetree_t *
parameter_list(typetree_t *base)
{
    typetree_t *type = init_typetree(FUNCTION), **args = NULL;
    const char **params = NULL;
    int nargs = 0;

    while (peek() != ')') {
        const char *symbol = NULL;
        typetree_t *decl = declaration_specifiers();
        decl = declarator(decl, &symbol);

        nargs++;
        args = realloc(args, sizeof(typetree_t *) * nargs);
        params = realloc(params, sizeof(char *) * nargs);
        args[nargs - 1] = decl;
        params[nargs - 1] = symbol;

        if (peek() != ',') break;
        consume(',');
        if (peek() == ')') {
            error("Trailing comma in parameter list, aborting");
            exit(1);
        }
        if (peek() == DOTS) {
            consume(DOTS); /* todo: add vararg type */
            break;
        }
    }
    
    type->d.func.ret = base;
    type->d.func.n_args = nargs;
    type->d.func.args = args;
    type->d.func.params = params;
    return type;
}

/* Treat statements and declarations equally, allowing declarations in between
 * statements as in modern C. Called compound-statement in K&R.
 */
static node_t *
block()
{
    node_t *node = init_node("block", 32), *child;
    consume('{');
    while (peek() != '}') {
        if (peek() == ';') {
            consume(';');
            continue;
        }
        child = statement();
        if (child != NULL)
            addchild(node, child);
    }
    consume('}');
    return node;
}

static node_t *
statement()
{
    node_t *node;
    enum token_type t = peek();
    switch (t) {
        case '{':
            push_scope();
            node = block();
            pop_scope();
            break;
        case IF:
        case SWITCH:
            node = init_node("selection-statement", 3);
            node->token = readtoken();
            consume('(');
            expression();
            /* addchild(node, expression()); */
            consume(')');
            addchild(node, statement());
            if (peek() == ELSE) {
                consume(ELSE);
                addchild(node, statement());
            }
            break;
        case WHILE:
        case DO:
        case FOR:
            node = init_node("iteration-statement", 4);
            node->token = readtoken();
            if (t == WHILE) {
                consume('(');
                expression(); /* no node */
                consume(')');
                addchild(node, statement());
            } else if (t == DO) {
                addchild(node, statement());
                consume(WHILE);
                consume('(');
                expression(); /* no node */
                consume(')');
            } else {
                consume('(');
                if (peek() != ';') expression(); /* no node */
                consume(';');
                if (peek() != ';') expression(); /* no node */
                consume(';');
                if (peek() != ')') expression(); /* no node */
                consume(')');
                addchild(node, statement());
            }
            break;
        case GOTO:
        case CONTINUE:
        case BREAK:
            node = init_node("jump-statement", 1);
            node->token = readtoken();
            if (t == GOTO && peek() == IDENTIFIER) {
                addchild(node, identifier());
            } else if (t == RETURN && peek() != ';') {
                expression(); /* no node */
            }
            consume(';');
            break;
        case RETURN:
            {
                symbol_t *val = NULL;
                consume(RETURN);
                if (peek() != ';') {
                    val = expression();
                }
                consume(';');
                mkir_ret(val);
            }
            break;
        case CASE:
        case DEFAULT:
            node = init_node("labeled-statement", 2);
            node->token = readtoken();
            if (peek() == ':') {
                consume(':');
                addchild(node, statement());
            } else {
                /*addchild(node, constant_expression()); */
                constant_expression();
                consume(':');
                addchild(node, statement());
            }
            break;
        case IDENTIFIER: /* also part of label statement, need 2 lookahead */
        case INTEGER: /* todo: any constant value */
        case STRING:
        case '(':
            node = init_node("expression", 0);
            expression(); /* no node */
            consume(';');
            break;
        default:
            node = declaration();
    }
    return node;
}

static node_t *
identifier()
{
    node_t *node = init_node("identifier", 0);
    struct token name = readtoken();
    symbol_t *symbol = sym_lookup(name.value);
    if (symbol == NULL) {
        error("Undefined symbol '%s', aborting", name.value);
        exit(0);
    }
    node->token = name;
    return node;
}

static symbol_t *conditional_expression();
static symbol_t *logical_expression();
static symbol_t *or_expression();
static symbol_t *and_expression();
static symbol_t *equality_expression();
static symbol_t *relational_expression();
static symbol_t *shift_expression();
static symbol_t *additive_expression();
static symbol_t *multiplicative_expression();
static symbol_t *cast_expression();
static symbol_t *postfix_expression();
static symbol_t *unary_expression();
static symbol_t *primary_expression();

static symbol_t *
expression()
{
    return assignment_expression();
}

static symbol_t *
assignment_expression()
{
    symbol_t *l = conditional_expression(), *r;
    if (peek() == '=') {
        /* todo: node must be unary-expression or lower (l-value) */
        consume('=');
        r = assignment_expression();
        mkir_assign(l, r);
    }
    return l;
}

static symbol_t *
constant_expression()
{
    symbol_t *node = conditional_expression();
    return node;
}

static symbol_t *
conditional_expression()
{
    symbol_t *node = logical_expression();
    if (peek() == '?') {
        consume('?');
        expression();
        consume(':');
        conditional_expression();
    }
    return node;
}

/* merge AND/OR */
static symbol_t *
logical_expression()
{
    symbol_t *l = or_expression();
    while (peek() == LOGICAL_OR || peek() == LOGICAL_AND) {
        struct token t = readtoken();
        symbol_t *x, *r = and_expression();
        typetree_t *type = init_type_basic(INT64_T); /* todo: check operands */
        x = sym_mktemp(type);
        mkir_arithmetic(x, l, r, (t.type == LOGICAL_AND) ? IR_OP_LOGICAL_AND : IR_OP_LOGICAL_OR);
        l = x;
    }
    return l;
}

/* merge OR/XOR */
static symbol_t *
or_expression()
{
    symbol_t *l = and_expression();
    while (peek() == '|' || peek() == '^') {
        struct token t = readtoken();
        symbol_t *x, *r = and_expression();
        typetree_t *type = init_type_basic(INT64_T); /* todo: check operands */
        x = sym_mktemp(type);
        mkir_arithmetic(x, l, r, (t.type == '|') ? IR_OP_BITWISE_OR : IR_OP_BITWISE_XOR);
        l = x;
    }
    return l;
}

static symbol_t *
and_expression()
{
    symbol_t *l = equality_expression();
    while (peek() == '&') {
        symbol_t *x, *r = and_expression();
        typetree_t *type = init_type_basic(INT64_T); /* todo: check operands */
        x = sym_mktemp(type);
        mkir_arithmetic(x, l, r, IR_OP_BITWISE_AND);
        l = x;
    }
    return l;
}

static symbol_t *
equality_expression()
{
    return relational_expression();
}

static symbol_t *
relational_expression()
{
    return shift_expression();
}

static symbol_t *
shift_expression()
{
    return additive_expression();
}

static symbol_t *
additive_expression()
{
    symbol_t *l = multiplicative_expression();
    while (peek() == '+' || peek() == '-') {
        struct token t = readtoken();
        symbol_t *x, *r = multiplicative_expression();
        typetree_t *type = type_combine(l->type, r->type);
        x = sym_mktemp(type);
        mkir_arithmetic(x, l, r, (t.type == '+') ? IR_OP_ADD : IR_OP_SUB);
        l = x;
    }
    return l;
}

static symbol_t *
multiplicative_expression()
{
    symbol_t *l = cast_expression();
    while (peek() == '*' || peek() == '/' || peek() == '%') {
        struct token t = readtoken();
        symbol_t *x, *r = cast_expression();
        typetree_t *type = type_combine(l->type, r->type);
        x = sym_mktemp(type);
        mkir_arithmetic(x, l, r, (t.type == '*') ? IR_OP_MUL : (t.type == '/') ? IR_OP_DIV : IR_OP_MOD);
        l = x;
    }
    return l;
}

static symbol_t *
cast_expression()
{
    return unary_expression();
}

static symbol_t *
unary_expression()
{
    return postfix_expression();
}


/* Parse and emit ir for general array indexing
 *  - From K&R: an array is not a variable, and cannot be assigned or modified.
 *    Referencing an array always converts the first rank to pointer type,
 *    e.g. int foo[3][2][1]; a = foo; assignment has the type int (*)[2][1].
 *  - Functions return and pass pointers to array. First index not necessary to
 *    specify in array (pointer) parameters: int (*foo(int arg[][3][2][1]))[3][2][1]
 */
static symbol_t *
postfix_expression_index_array(symbol_t *root)
{
    typetree_t *type = root->type; /* unwrap type for each indexing */
    symbol_t *expr, *skip, *tmp;
    symbol_t *idx;
    do {
        consume('[');
        expr = expression();
        consume(']');

        /*printf("Indexing into %d x %d\n", type->length, type->size); */
        skip = sym_mktemp_immediate(INT64_T, &type->size);
        tmp = sym_mktemp(init_type_basic(INT64_T));

        /* missing a + in here for multi-dim */
        mkir_arithmetic(tmp, expr, skip, IR_OP_MUL);

        type = type->d.arr.of;

    } while (peek() == '[');

    /* Partial dereferencing decays into pointer type */
    if (type->type == ARRAY) {
        typetree_t *ptr = init_typetree(POINTER);
        ptr->d.ptr.to = type->d.arr.of;
        type = ptr;

        idx = sym_mktemp(type);
        mkir_arithmetic(idx, root, tmp, IR_OP_ADD);
    } else {
        typetree_t *ptr = init_typetree(POINTER);
        symbol_t *t;
        ptr->d.ptr.to = type;
        t = sym_mktemp(ptr);

        mkir_arithmetic(t, root, tmp, IR_OP_ADD); /* idea: have mkir return symbol, and do necessary type conversion */

        idx = sym_mktemp(type);
        mkir_deref(idx, t);
    }

    root = idx;
    return root;
}


/* This rule is left recursive, build tree bottom up
 */
static symbol_t *
postfix_expression()
{
    symbol_t *root = primary_expression();

    if (peek() == '[') {
        root = postfix_expression_index_array(root);
    }

    /*while (peek() == '[' || peek() == '(' || peek() == '.') {
        node_t *parent = init_node("postfix-expression", 2);
        addchild(parent, root);
        switch (peek()) {
            case '[':
                consume('[');
                addchild(parent, expression());
                consume(']');
                break;
            case '(':
                 addchild(parent, argument_expression_list()); 
                consume('(');
                consume(')');
                break;
            case '.':
                parent->token = readtoken();
                addchild(parent, identifier());
                break;
            default:
                error("Unexpected token '%s', not a valid postfix expression", readtoken().value);
                exit(0);
        }
        root = parent;
    }*/
    return root;
}

static symbol_t *
primary_expression()
{
    symbol_t *symbol;
    struct token token = readtoken();
    switch (token.type) {
        case IDENTIFIER:
            symbol = sym_lookup(token.value);
            if (symbol == NULL) {
                error("Undefined symbol '%s', aborting", token.value);
                exit(0);
            }
            break;
        case INTEGER:
            symbol = sym_mkimmediate(token);
            break;
        case '(':
            symbol = expression();
            consume(')');
            break;
        default:
            error("Unexpected token '%s', not a valid primary expression", token.value);
            exit(0);
    }
    return symbol;
}
