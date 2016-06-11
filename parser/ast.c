#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>

#include "common.h"
#include INTERPRETER_INCLUDE
#include "ast.h"

#define talloc(type) malloc(sizeof(type))

struct symbollist
{
	unsigned offset;
	char *text;
	struct symbollist *next;
};
struct scopelist
{
	unsigned offset;
	struct symbollist *current;
	struct scopelist *outer;
};
typedef struct code
{
	const char *filename;
	char *src;
	char curr;
	int pos;
	struct scopelist *symbols;
} code_t;

static ptrs_ast_t *parseStmtList(code_t *code, char end);
static ptrs_ast_t *parseStatement(code_t *code);
static ptrs_ast_t *parseExpression(code_t *code);
static ptrs_ast_t *parseBinaryExpr(code_t *code, ptrs_ast_t *left, int minPrec);
static ptrs_ast_t *parseUnaryExpr(code_t *code);
static ptrs_ast_t *parseUnaryExtension(code_t *code, ptrs_ast_t *ast);
static struct ptrs_astlist *parseExpressionList(code_t *code, char end);

static ptrs_vartype_t readTypeName(code_t *code);
static const char *readOperator(code_t *code);
static char *readIdentifier(code_t *code);
static char *readString(code_t *code);
static char readEscapeSequence(code_t *code);
static int64_t readInt(code_t *code, int base);
static double readDouble(code_t *code);

static void addSymbol(code_t *code, char *symbol);
static ptrs_symbol_t getSymbol(code_t *code, char *symbol);

static bool lookahead(code_t *code, const char *str);
static void consume(code_t *code, const char *str);
static void consumec(code_t *code, char c);
static void consumecm(code_t *code, char c, const char *msg);
static void next(code_t *code);
static void rawnext(code_t *code);

static bool skipSpaces(code_t *code);
static bool skipComments(code_t *code);

static void unexpectedm(code_t *code, const char *expected, const char *msg);

#define unexpected(code, expected) \
	unexpectedm(code, expected, NULL)

ptrs_ast_t *ptrs_parse(char *src, const char *filename)
{
	code_t code;
	code.filename = filename;
	code.src = src;
	code.pos = -1;
	code.symbols = talloc(struct scopelist);
	code.symbols->offset = 0;
	code.symbols->current = NULL;
	code.symbols->outer = NULL;
	next(&code);

	return parseStmtList(&code, 0);
}

static ptrs_ast_t *parseStmtList(code_t *code, char end)
{
	ptrs_ast_t *elem = talloc(ptrs_ast_t);
	elem->handler = PTRS_HANDLE_BODY;
	if(code->curr == end || code->curr == 0)
	{
		elem->arg.astlist = NULL;
		return elem;
	}

	struct ptrs_astlist *curr = talloc(struct ptrs_astlist);
	elem->arg.astlist = curr;

	for(;;)
	{
		curr->entry = parseStatement(code);

		if(code->curr == end || code->curr == 0)
			break;

		struct ptrs_astlist *next = talloc(struct ptrs_astlist);
		curr->next = next;
		curr = next;
	}

	curr->next = NULL;
	return elem;
}

struct argdeflist
{
	char *name;
	ptrs_ast_t *value;
	struct argdeflist *next;
};
static char **parseArgumentDefinitionList(code_t *code, int *argc, ptrs_ast_t ***argv)
{
	*argc = 0;
	consumecm(code, '(', "Expected ( as the beginning of an argument definition");

	if(code->curr == ')')
	{
		next(code);
		if(argv != NULL)
			*argv = NULL;
		return NULL;
	}
	else
	{
		struct argdeflist first;
		struct argdeflist *list = &first;
		for(;;)
		{
			list->next = alloca(sizeof(struct argdeflist));
			list = list->next;

			list->name = readIdentifier(code);
			if(argv != NULL)
			{
				if(lookahead(code, "="))
					list->value = parseExpression(code);
				else
					list->value = NULL;
			}

			++*argc;

			if(code->curr == ')')
				break;
			consumecm(code, ',', "Expected , between two arguments");
		}

		char **args = malloc(sizeof(char *) * *argc);
		if(argv != NULL)
			*argv = malloc(sizeof(ptrs_ast_t *) * *argc);

		list = first.next;
		for(int i = 0; i < *argc; i++)
		{
			args[i] = list->name;
			if(argv != NULL)
				(*argv)[i] = list->value;

			list = list->next;
		}
		consumecm(code, ')', "Expected ) as the ending of an argument definition");

		return args;
	}
}

static ptrs_function_t *parseFunction(code_t *code, char *name)
{
	ptrs_function_t *func = talloc(ptrs_function_t);
	func->name = name;
	func->args = parseArgumentDefinitionList(code, &func->argc, &func->argv);

	consumec(code, '{');
	func->body = parseStmtList(code, '}');
	consumec(code, '}');

	return func;
}

static ptrs_ast_t *parseStatement(code_t *code)
{
	ptrs_ast_t *stmt = talloc(ptrs_ast_t);
	stmt->codepos = code->pos;
	stmt->code = code->src;
	stmt->file = code->filename;

	if(lookahead(code, "var"))
	{
		stmt->handler = PTRS_HANDLE_DEFINE;
		stmt->arg.define.name = readIdentifier(code);

		if(lookahead(code, "["))
		{
			stmt->handler = PTRS_HANDLE_ARRAY;
			stmt->arg.define.value = parseExpression(code);
			consumec(code, ']');
		}
		else if(lookahead(code, "{"))
		{
			stmt->handler = PTRS_HANDLE_VARARRAY;
			stmt->arg.define.value = parseExpression(code);
			consumec(code, '}');
		}
		else if(lookahead(code, "="))
		{
			stmt->arg.define.value = parseExpression(code);
		}
		else
		{
			stmt->arg.define.value = NULL;
		}

		consumec(code, ';');
	}
	else if(lookahead(code, "import"))
	{
		stmt->handler = PTRS_HANDLE_IMPORT;
		stmt->arg.import.fields = parseExpressionList(code, 0);

		if(lookahead(code, "from"))
			stmt->arg.import.from = parseExpression(code);
		else
			stmt->arg.import.from = NULL;

		consumec(code, ';');
	}
	else if(lookahead(code, "return"))
	{
		stmt->handler = PTRS_HANDLE_RETURN;
		stmt->arg.astval = parseExpression(code);
		consumec(code, ';');
	}
	else if(lookahead(code, "break"))
	{
		stmt->handler = PTRS_HANDLE_BREAK;
		consumec(code, ';');
	}
	else if(lookahead(code, "continue"))
	{
		stmt->handler = PTRS_HANDLE_CONTINUE;
		consumec(code, ';');
	}
	else if(lookahead(code, "delete"))
	{
		stmt->handler = PTRS_HANDLE_DELETE;
		stmt->arg.astval = parseExpression(code);
		consumec(code, ';');
	}
	else if(lookahead(code, "throw"))
	{
		stmt->handler = PTRS_HANDLE_THROW;
		stmt->arg.astlist = parseExpressionList(code, ';');
		consumec(code, ';');
	}
	else if(lookahead(code, "try"))
	{
		stmt->handler = PTRS_HANDLE_TRYCATCH;
		consumec(code, '{');
		stmt->arg.trycatch.tryBody = parseStmtList(code, '}');
		consumec(code, '}');

		if(lookahead(code, "catch"))
		{
			stmt->arg.trycatch.argv = parseArgumentDefinitionList(code, &stmt->arg.trycatch.argc, NULL);
			consumec(code, '{');
			stmt->arg.trycatch.catchBody = parseStmtList(code, '}');
			consumec(code, '}');
		}
		else
		{
			stmt->arg.trycatch.catchBody = NULL;
		}
	}
	else if(lookahead(code, "{"))
	{
		stmt->handler = PTRS_HANDLE_SCOPESTATEMENT;
		stmt->arg.astval = parseStmtList(code, '}');
		consumec(code, '}');
	}
	else if(lookahead(code, "function"))
	{
		stmt->handler = PTRS_HANDLE_FUNCTION;
		stmt->arg.function.name = readIdentifier(code);
		stmt->arg.function.args = parseArgumentDefinitionList(code,
				&stmt->arg.function.argc,
				&stmt->arg.function.argv);

		consumec(code, '{');
		stmt->arg.function.body = parseStmtList(code, '}');
		consumec(code, '}');
	}
	else if(lookahead(code, "struct"))
	{
		stmt->handler = PTRS_HANDLE_STRUCT;
		stmt->arg.structval.name = readIdentifier(code);
		stmt->arg.structval.constructor = NULL;
		stmt->arg.structval.overloads = NULL;
		stmt->arg.structval.size = 0;
		stmt->arg.structval.data = NULL;
		consumec(code, '{');

		struct ptrs_structlist *curr = NULL;
		while(code->curr != '}')
		{
			char *name = readIdentifier(code);
			if(strcmp(name, "constructor") == 0)
			{
				free(name);
				stmt->arg.structval.constructor = parseFunction(code, NULL);
				continue;
			}
			else if(strcmp(name, "operator") == 0)
			{
				free(name);

				struct ptrs_opoverload *overload = talloc(struct ptrs_opoverload);
				overload->op = readOperator(code);
				overload->handler = parseFunction(code, NULL);

				overload->next = stmt->arg.structval.overloads;
				stmt->arg.structval.overloads = overload;
				continue;
			}

			struct ptrs_structlist *next = talloc(struct ptrs_structlist);
			next->next = curr;
			curr = next;

			curr->name = name;

			if(code->curr == '(')
			{
				curr->type = PTRS_STRUCTMEMBER_FUNCTION;
				curr->value.function = parseFunction(code, name);
			}
			else if(code->curr == '[')
			{
				curr->type = PTRS_STRUCTMEMBER_ARRAY;
				consumec(code, '[');
				ptrs_ast_t *ast = parseExpression(code);
				consumec(code, ']');
				consumec(code, ';');

				if(ast->handler != PTRS_HANDLE_CONSTANT)
					PTRS_HANDLE_ASTERROR(ast, "Struct array member size must be a constant");

				curr->value.size = ast->arg.constval.value.intval;
				curr->offset = stmt->arg.structval.size;
				stmt->arg.structval.size += curr->value.size;
				free(ast);
			}
			else if(code->curr == '{')
			{
				curr->type = PTRS_STRUCTMEMBER_VARARRAY;
				consumec(code, '{');
				ptrs_ast_t *ast = parseExpression(code);
				consumec(code, '}');
				consumec(code, ';');

				if(ast->handler != PTRS_HANDLE_CONSTANT)
					PTRS_HANDLE_ASTERROR(ast, "Struct array member size must be a constant");

				curr->value.size = ast->arg.constval.value.intval * sizeof(ptrs_var_t);
				curr->offset = stmt->arg.structval.size;
				stmt->arg.structval.size += curr->value.size;
				free(ast);
			}
			else
			{
				curr->offset = stmt->arg.structval.size;
				stmt->arg.structval.size += sizeof(ptrs_var_t);

				if(lookahead(code, "="))
					curr->value.startval = parseExpression(code);
				else
					curr->value.startval = NULL;

				consumec(code, ';');
			}
		}
		stmt->arg.structval.member = curr;
		consumec(code, '}');
		consumec(code, ';');
	}
	else if(lookahead(code, "if"))
	{
		stmt->handler = PTRS_HANDLE_IF;

		consumec(code, '(');
		stmt->arg.ifelse.condition = parseExpression(code);
		consumec(code, ')');

		if(lookahead(code, "{"))
		{
			stmt->arg.ifelse.ifBody = parseStmtList(code, '}');
			consumec(code, '}');
		}
		else
		{
			stmt->arg.ifelse.ifBody = parseStatement(code);
		}

		stmt->arg.ifelse.elseBody = NULL;
		if(lookahead(code, "else"))
		{
			if(lookahead(code, "{"))
			{
				stmt->arg.ifelse.elseBody = parseStmtList(code, '}');
				consumec(code, '}');
			}
			else
			{
				stmt->arg.ifelse.elseBody = parseStatement(code);
			}
		}
	}
	else if(lookahead(code, "while"))
	{
		stmt->handler = PTRS_HANDLE_WHILE;

		consumec(code, '(');
		stmt->arg.control.condition = parseExpression(code);
		consumec(code, ')');

		if(lookahead(code, "{"))
		{
			stmt->arg.control.body = parseStmtList(code, '}');
			consumec(code, '}');
		}
		else
		{
			stmt->arg.control.body = parseStatement(code);
		}
	}
	else if(lookahead(code, "do"))
	{
		stmt->handler = PTRS_HANDLE_DOWHILE;

		if(lookahead(code, "{"))
		{
			stmt->arg.control.body = parseStmtList(code, '}');
			consumec(code, '}');
		}
		else
		{
			stmt->arg.control.body = parseStatement(code);
		}

		consume(code, "while");

		consumec(code, '(');
		stmt->arg.control.condition = parseExpression(code);
		consumec(code, ')');
		consumec(code, ';');
	}
	else if(lookahead(code, "for"))
	{
		consumec(code, '(');
		int pos = code->pos;

		if(lookahead(code, "var"))
			stmt->arg.forin.newvar = true;
		else
			stmt->arg.forin.newvar = false;

		if(isalpha(code->curr) || code->curr == '_')
		{
			stmt->arg.forin.varname = readIdentifier(code);

			if(lookahead(code, "in"))
			{
				stmt->arg.forin.value = parseExpression(code);
				stmt->handler = PTRS_HANDLE_FORIN;
				consumec(code, ')');

				if(lookahead(code, "{"))
				{
					stmt->arg.forin.body = parseStmtList(code, '}');
					consumec(code, '}');
				}
				else
				{
					stmt->arg.forin.body = parseStatement(code);
				}

				return stmt;
			}
		}

		code->pos = pos;
		code->curr = code->src[pos];
		stmt->handler = PTRS_HANDLE_FOR;

		stmt->arg.forstatement.init = parseStatement(code);
		stmt->arg.forstatement.condition = parseExpression(code);
		consumec(code, ';');
		stmt->arg.forstatement.step = parseExpression(code);
		consumec(code, ')');

		if(lookahead(code, "{"))
		{
			stmt->arg.forstatement.body = parseStmtList(code, '}');
			consumec(code, '}');
		}
		else
		{
			stmt->arg.forstatement.body = parseStatement(code);
		}
	}
	else
	{
		stmt->handler = PTRS_HANDLE_EXPRSTATEMENT;
		stmt->arg.astval = parseExpression(code);
		consumec(code, ';');
	}
	return stmt;
}

struct opinfo
{
	const char *op;
	int precendence;
	bool rightToLeft;
	ptrs_asthandler_t handler;
};

struct opinfo binaryOps[] = {
	// >> and << need to be before > and < or we will always lookahead greater / less
	{">>", 9, false, PTRS_HANDLE_OP_SHR}, //shift right
	{"<<", 9, false, PTRS_HANDLE_OP_SHL}, //shift left

	// == needs to be before = or we will always lookahead assign
	{"==", 8, false, PTRS_HANDLE_OP_EQUAL},
	{"!=", 8, false, PTRS_HANDLE_OP_INEQUAL},
	{"<=", 8, false, PTRS_HANDLE_OP_LESSEQUAL},
	{">=", 8, false, PTRS_HANDLE_OP_GREATEREQUAL},
	{"<", 8, false, PTRS_HANDLE_OP_LESS},
	{">", 8, false, PTRS_HANDLE_OP_GREATER},

	{"=", 1, true, PTRS_HANDLE_OP_ASSIGN},
	{"+=", 1, true, PTRS_HANDLE_OP_ADDASSIGN},
	{"-=", 1, true, PTRS_HANDLE_OP_SUBASSIGN},
	{"*=", 1, true, PTRS_HANDLE_OP_MULASSIGN},
	{"/=", 1, true, PTRS_HANDLE_OP_DIVASSIGN},
	{"%=", 1, true, PTRS_HANDLE_OP_MODASSIGN},
	{">>=", 1, true, PTRS_HANDLE_OP_SHRASSIGN},
	{"<<=", 1, true, PTRS_HANDLE_OP_SHLASSIGN},
	{"&=", 1, true, PTRS_HANDLE_OP_ANDASSIGN},
	{"^=", 1, true, PTRS_HANDLE_OP_XORASSIGN},
	{"|=", 1, true, PTRS_HANDLE_OP_ORASSIGN},

	{"?", 2, true, PTRS_HANDLE_OP_TERNARY},
	{":", -1, true, PTRS_HANDLE_OP_TERNARY},

	{"||", 3, false, PTRS_HANDLE_OP_LOGICOR},
	{"&&", 4, false, PTRS_HANDLE_OP_LOGICAND},

	{"|", 5, false, PTRS_HANDLE_OP_OR},
	{"^", 6, false, PTRS_HANDLE_OP_XOR},
	{"&", 7, false, PTRS_HANDLE_OP_AND},

	{"+", 10, false, PTRS_HANDLE_OP_ADD},
	{"-", 10, false, PTRS_HANDLE_OP_SUB},

	{"*", 11, false, PTRS_HANDLE_OP_MUL},
	{"/", 11, false, PTRS_HANDLE_OP_DIV},
	{"%", 11, false, PTRS_HANDLE_OP_MOD}
};
static int binaryOpCount = sizeof(binaryOps) / sizeof(struct opinfo);

struct opinfo prefixOps[] = {
	{"typeof", 12, true, PTRS_HANDLE_OP_TYPEOF},
	{"++", 12, true, PTRS_HANDLE_PREFIX_INC}, //prefixed ++
	{"--", 12, true, PTRS_HANDLE_PREFIX_DEC}, //prefixed --
	{"!", 12, true, PTRS_HANDLE_PREFIX_LOGICNOT}, //logical NOT
	{"~", 12, true, PTRS_HANDLE_PREFIX_NOT}, //bitwise NOT
	{"&", 12, true, PTRS_HANDLE_PREFIX_ADDRESS}, //adress of
	{"*", 12, true, PTRS_HANDLE_PREFIX_DEREFERENCE}, //dereference
	{"+", 12, true, PTRS_HANDLE_PREFIX_PLUS}, //unary +
	{"-", 12, true, PTRS_HANDLE_PREFIX_MINUS} //unary -
};
static int prefixOpCount = sizeof(prefixOps) / sizeof(struct opinfo);

struct opinfo suffixedOps[] = {
	{"++", 13, false, PTRS_HANDLE_SUFFIX_INC}, //suffixed ++
	{"--", 13, false, PTRS_HANDLE_SUFFIX_DEC} //suffixed --
};
static int suffixedOpCount = sizeof(suffixedOps) / sizeof(struct opinfo);

static ptrs_ast_t *parseExpression(code_t *code)
{
	return parseBinaryExpr(code, parseUnaryExpr(code), 0);
}

static struct opinfo *peekBinaryOp(code_t *code)
{
	int pos = code->pos;
	for(int i = 0; i < binaryOpCount; i++)
	{
		if(lookahead(code, binaryOps[i].op))
		{
			code->pos = pos;
			code->curr = code->src[pos];
			return &binaryOps[i];
		}
	}

	return NULL;
}
static ptrs_ast_t *parseBinaryExpr(code_t *code, ptrs_ast_t *left, int minPrec)
{
	struct opinfo *ahead = peekBinaryOp(code);
	while(ahead != NULL && ahead->precendence >= minPrec)
	{
		struct opinfo *op = ahead;
		int pos = code->pos;
		consume(code, ahead->op);
		ptrs_ast_t *right = parseUnaryExpr(code);
		ahead = peekBinaryOp(code);

		while(ahead != NULL && ahead->precendence > op->precendence)
		{
			right = parseBinaryExpr(code, right, ahead->precendence);
			ahead = peekBinaryOp(code);
		}

#ifndef PTRS_DISABLE_CONSTRESOLVE
		if(left->handler == PTRS_HANDLE_CONSTANT && right->handler == PTRS_HANDLE_CONSTANT)
		{
			ptrs_lastast = left;
			ptrs_var_t result;
			ptrs_ast_t node;
			node.handler = op->handler;
			node.arg.binary.left = left;
			node.arg.binary.right = right;

			node.handler(&node, &result, NULL);
			free(right);
			memcpy(&left->arg.constval, &result, sizeof(ptrs_var_t));
			continue;
		}
#endif
		if(op->handler == PTRS_HANDLE_OP_TERNARY)
		{
			ptrs_ast_t *ast = talloc(ptrs_ast_t);
			ast->handler = PTRS_HANDLE_OP_TERNARY;
			ast->arg.ternary.condition = left;
			ast->arg.ternary.trueVal = right;
			consumec(code, ':');
			ast->arg.ternary.falseVal = parseExpression(code);
			left = ast;
			continue;
		}

		ptrs_ast_t *_left = left;
		left = talloc(ptrs_ast_t);
		left->handler = op->handler;
		left->arg.binary.left = _left;
		left->arg.binary.right = right;
		left->codepos = pos;
		left->code = code->src;
		left->file = code->filename;
	}
	return left;
}

struct constinfo
{
	char *text;
	ptrs_vartype_t type;
	ptrs_val_t value;
};
struct constinfo constants[] = {
	{"true", PTRS_TYPE_INT, {.intval = true}},
	{"false", PTRS_TYPE_INT, {.intval = false}},
	{"NULL", PTRS_TYPE_NATIVE, {.nativeval = NULL}},
	{"null", PTRS_TYPE_POINTER, {.ptrval = NULL}},
	{"VARSIZE", PTRS_TYPE_INT, {.intval = sizeof(ptrs_var_t)}},
	{"undefined", PTRS_TYPE_UNDEFINED, {}}
};
int constantCount = sizeof(constants) / sizeof(struct constinfo);

static ptrs_ast_t *parseUnaryExpr(code_t *code)
{
	char curr = code->curr;
	int pos = code->pos;
	ptrs_ast_t *ast;

	for(int i = 0; i < prefixOpCount; i++)
	{
		if(lookahead(code, prefixOps[i].op))
		{
			ast = talloc(ptrs_ast_t);
			ast->arg.astval = parseUnaryExpr(code);
			ast->handler = prefixOps[i].handler;
			ast->codepos = pos;
			ast->code = code->src;
			ast->file = code->filename;
			return ast;
		}
	}

	for(int i = 0; i < constantCount; i++)
	{
		if(lookahead(code, constants[i].text))
		{
			ast = talloc(ptrs_ast_t);
			ast->arg.constval.type = constants[i].type;
			ast->arg.constval.value = constants[i].value;
			ast->handler = PTRS_HANDLE_CONSTANT;
			return ast;
		}
	}

	if(lookahead(code, "new"))
	{
		ptrs_ast_t *val = talloc(ptrs_ast_t);
		val->handler = PTRS_HANDLE_IDENTIFIER;
		val->codepos = code->pos;
		val->code = code->src;
		val->file = code->filename;
		val->arg.strval = readIdentifier(code);

		consumec(code, '(');
		ast = talloc(ptrs_ast_t);
		ast->handler = PTRS_HANDLE_NEW;
		ast->arg.call.value = val;
		ast->arg.call.arguments = parseExpressionList(code, ')');
		consumec(code, ')');
	}
	else if(lookahead(code, "type"))
	{
		consumec(code, '<');
		ptrs_vartype_t type = readTypeName(code);

		if(type > PTRS_TYPE_STRUCT)
			unexpectedm(code, NULL, "Syntax is type<TYPENAME>");

		ast = talloc(ptrs_ast_t);
		ast->handler = PTRS_HANDLE_CONSTANT;
		ast->arg.constval.type = PTRS_TYPE_INT;
		ast->arg.constval.value.intval = type;
		consumec(code, '>');
	}
	else if(lookahead(code, "cast"))
	{
		consumec(code, '<');
		ptrs_vartype_t type = readTypeName(code);

		if(type > PTRS_TYPE_STRUCT)
			unexpectedm(code, NULL, "Syntax is cast<TYPENAME>");

		ast = talloc(ptrs_ast_t);
		ast->handler = PTRS_HANDLE_CAST;
		ast->arg.cast.type = type;
		consumec(code, '>');

		ast->arg.cast.value = parseUnaryExpr(code);
	}
	else if(lookahead(code, "function"))
	{
		ast = talloc(ptrs_ast_t);
		ast->handler = PTRS_HANDLE_FUNCTION;
		ast->arg.function.name = NULL;
		ast->arg.function.args = parseArgumentDefinitionList(code,
				&ast->arg.function.argc,
				&ast->arg.function.argv);

		consumec(code, '{');
		ast->arg.function.body = parseStmtList(code, '}');
		consumec(code, '}');
	}
	else if(curr == '[')
	{
		next(code);
		ast = talloc(ptrs_ast_t);
		ast->handler = PTRS_HANDLE_ARRAYEXPR;
		ast->arg.astlist = parseExpressionList(code, ']');
		consumec(code, ']');
	}
	else if(curr == '{')
	{
		next(code);
		ast = talloc(ptrs_ast_t);
		ast->handler = PTRS_HANDLE_VARARRAYEXPR;
		ast->arg.astlist = parseExpressionList(code, '}');
		consumec(code, '}');
	}
	else if(isalpha(curr) || curr == '_')
	{
		ast = talloc(ptrs_ast_t);
		ast->arg.strval = readIdentifier(code);
		ast->handler = PTRS_HANDLE_IDENTIFIER;
	}
	else if(isdigit(curr) || curr == '.')
	{
		int startPos = code->pos;
		ast = talloc(ptrs_ast_t);
		ast->handler = PTRS_HANDLE_CONSTANT;

		ast->arg.constval.type = PTRS_TYPE_INT;
		ast->arg.constval.value.intval = readInt(code, 0);

		if(code->curr == '.' || code->curr == 'e')
		{
			code->pos = startPos;
			code->curr = code->src[code->pos];
			ast->arg.constval.type = PTRS_TYPE_FLOAT;
			ast->arg.constval.value.floatval = readDouble(code);
			lookahead(code, "f");
		}
		else if(lookahead(code, "f"))
		{
			ast->arg.constval.type = PTRS_TYPE_FLOAT;
			ast->arg.constval.value.floatval = ast->arg.constval.value.intval;
		}
	}
	else if(curr == '\'')
	{
		rawnext(code);
		ast = talloc(ptrs_ast_t);
		ast->handler = PTRS_HANDLE_CONSTANT;
		ast->arg.constval.type = PTRS_TYPE_INT;

		if(code->curr == '\\')
		{
			rawnext(code);
			ast->arg.constval.value.intval = readEscapeSequence(code);
		}
		else
		{
			ast->arg.constval.value.intval = code->curr;
		}
		rawnext(code);
		consumecm(code, '\'', "A char literal cannot contain multiple characters");
	}
	else if(curr == '"')
	{
		rawnext(code);
		ast = talloc(ptrs_ast_t);
		ast->arg.constval.type = PTRS_TYPE_NATIVE;
		ast->arg.constval.value.strval = readString(code);
		ast->arg.constval.meta.readOnly = true;
		ast->handler = PTRS_HANDLE_CONSTANT;
		consumec(code, '"');
	}
	else if(curr == '(')
	{
		int start = code->pos;
		consumec(code, '(');
		ast = parseExpression(code);

		if(ast->handler == PTRS_HANDLE_IDENTIFIER && (code->curr == ',' || (lookahead(code, ")") && lookahead(code, "->"))))
		{
			code->pos = start;
			code->curr = code->src[start];

			ast->handler = PTRS_HANDLE_FUNCTION;
			ast->arg.function.name = NULL;
			ast->arg.function.args = parseArgumentDefinitionList(code, &ast->arg.function.argc, NULL);

			consume(code, "->");
			if(lookahead(code, "{"))
			{
				ast->arg.function.body = parseStmtList(code, '}');
				consumec(code, '}');
			}
			else
			{
				ptrs_ast_t *retStmt = talloc(ptrs_ast_t);
				retStmt->handler = PTRS_HANDLE_RETURN;
				retStmt->arg.astval = parseExpression(code);
				ast->arg.function.body = retStmt;
			}
		}
		else
		{
			consumec(code, ')');
		}
	}
	else
	{
		return NULL;
	}

	ast->codepos = pos;
	ast->code = code->src;
	ast->file = code->filename;

	ptrs_ast_t *old;
	do {
		old = ast;
		ast = parseUnaryExtension(code, ast);
	} while(ast != old);

	return ast;
}

static ptrs_ast_t *parseUnaryExtension(code_t *code, ptrs_ast_t *ast)
{
	char curr = code->curr;
	if(curr == '.')
	{
		ptrs_ast_t *member = talloc(ptrs_ast_t);
		member->handler = PTRS_HANDLE_MEMBER;
		member->codepos = code->pos;
		member->code = code->src;
		member->file = code->filename;

		consumec(code, '.');
		member->arg.define.value = ast;
		member->arg.define.name = readIdentifier(code);

		ast = member;
	}
	else if(curr == '(')
	{
		ptrs_ast_t *call = talloc(ptrs_ast_t);
		call->handler = PTRS_HANDLE_CALL;
		call->codepos = code->pos;
		call->code = code->src;
		call->file = code->filename;
		consumec(code, '(');

		call->arg.call.value = ast;
		call->arg.call.arguments = parseExpressionList(code, ')');

		ast = call;
		consumec(code, ')');
	}
	else if(curr == '[')
	{
		ptrs_ast_t *indexExpr = talloc(ptrs_ast_t);
		indexExpr->handler = PTRS_HANDLE_INDEX;
		indexExpr->codepos = code->pos;
		indexExpr->code = code->src;
		indexExpr->file = code->filename;
		consumec(code, '[');

		indexExpr->arg.binary.left = ast;
		indexExpr->arg.binary.right = parseExpression(code);

		ast = indexExpr;
		consumec(code, ']');
	}
	else
	{
		int pos = code->pos;
		for(int i = 0; i < suffixedOpCount; i++)
		{
			if(lookahead(code, suffixedOps[i].op))
			{
				ptrs_ast_t *opAst = talloc(ptrs_ast_t);
				opAst->codepos = pos;
				opAst->code = code->src;
				opAst->file = code->filename;
				opAst->arg.astval = ast;
				opAst->handler = suffixedOps[i].handler;
				return opAst;
			}
		}
	}

	return ast;
}

static struct ptrs_astlist *parseExpressionList(code_t *code, char end)
{
	if(code->curr == end || code->curr == 0)
		return NULL;

	struct ptrs_astlist *curr = talloc(struct ptrs_astlist);
	struct ptrs_astlist *first = curr;

	for(;;)
	{
		curr->entry = parseExpression(code);

		if(!lookahead(code, ","))
			break;

		struct ptrs_astlist *next = talloc(struct ptrs_astlist);
		curr->next = next;
		curr = next;
	}

	curr->next = NULL;
	return first;
}

struct typeName
{
	const char *name;
	ptrs_vartype_t type;
};
struct typeName typeNames[] = {
	{"undefined", PTRS_TYPE_UNDEFINED},
	{"int", PTRS_TYPE_INT},
	{"float", PTRS_TYPE_FLOAT},
	{"native", PTRS_TYPE_NATIVE},
	{"pointer", PTRS_TYPE_POINTER},
	{"function", PTRS_TYPE_FUNCTION},
	{"struct", PTRS_TYPE_STRUCT}
};
static int typeNameCount = sizeof(typeNames) / sizeof(struct typeName);

static ptrs_vartype_t readTypeName(code_t *code)
{
	for(int i = 0; i < typeNameCount; i++)
	{
		if(lookahead(code, typeNames[i].name))
		{
			return typeNames[i].type;
		}
	}

	return PTRS_TYPE_STRUCT + 1;
}

struct opinfo specialOverloads[] = {
	{"()", 0, false, NULL}, //call
	{"[]", 0, false, NULL}, //index
	{".", 0, false, NULL}, //foo.bar
	{"cast", 0, false, NULL}, //cast
	{"delete", 0, false, NULL}, //delete
};
int specialOverloadsCount = sizeof(specialOverloads) / sizeof(struct opinfo);

static const char *readOperatorFrom(code_t *code, struct opinfo *ops, int opCount)
{
	for(int i = 0; i < opCount; i++)
	{
		if(lookahead(code, ops[i].op))
			return ops[i].op;
	}
	return NULL;
}
static const char *readOperator(code_t *code)
{
	const char *op;
	if((op = readOperatorFrom(code, prefixOps, prefixOpCount)) != NULL)
		return op;
	if((op = readOperatorFrom(code, binaryOps, binaryOpCount)) != NULL)
		return op;
	if((op = readOperatorFrom(code, specialOverloads, specialOverloadsCount)) != NULL)
		return op;

	unexpected(code, "Operator");
	return NULL; //doh
}

static char *readIdentifier(code_t *code)
{
	char val[128];
	int i = 0;

	while(isalpha(code->curr) || isdigit(code->curr) ||  code->curr == '_')
	{
		val[i] = code->curr;
		i++;
		rawnext(code);
	}

	if(i == 0)
		unexpected(code, "Identifier");

	skipSpaces(code);

	val[i] = 0;
	char *_val = malloc(i + 1);
	strcpy(_val, val);

	return _val;
}

static char *readString(code_t *code)
{
	char val[1024];
	int i = 0;

	while(code->curr != '"')
	{
		if(code->curr == '\\')
		{
			rawnext(code);
			val[i] = readEscapeSequence(code);
		}
		else
		{
			val[i] = code->curr;
		}
		i++;
		rawnext(code);
	}

	val[i] = 0;
	char *_val = malloc(i + 1);
	strcpy(_val, val);

	return _val;
}

static char readEscapeSequence(code_t *code)
{
	switch(code->curr)
	{
		case 'a':
			return '\a';
			break;
		case 'b':
			return '\b';
			break;
		case 'f':
			return '\f';
			break;
		case 'n':
			return '\n';
			break;
		case 'r':
			return '\r';
			break;
		case 't':
			return '\t';
			break;
		case 'v':
			return '\v';
			break;
		case '\\':
			return '\\';
			break;
		case '\'':
			return '\'';
			break;
		case '"':
			return '"';
			break;
		case '?':
			return '\?';
			break;
		case 'x':
			rawnext(code);
			return (char)readInt(code, 16);
			break;
		default:
			if(isdigit(code->curr))
				return (char)readInt(code, 8);
	}
	char msg[] = "Unknown escape sequence \\X";
	msg[25] = code->curr;
	unexpectedm(code, NULL, msg);
	return 0;
}

static int64_t readInt(code_t *code, int base)
{
	char *start = &code->src[code->pos];
	char *end;

	int64_t val = strtol(start, &end, base);
	code->pos += end - start - 1;
	next(code);

	return val;
}
static double readDouble(code_t *code)
{
	char *start = &code->src[code->pos];
	char *end;

	double val = strtod(start, &end);
	code->pos += end - start - 1;
	next(code);

	return val;
}

static void addSymbol(code_t *code, char *symbol)
{
	struct scopelist *curr = code->symbols;
	struct symbollist *entry = talloc(struct symbollist);

	entry->next = curr->current;
	entry->offset = curr->offset;
	curr->offset += sizeof(ptrs_var_t);
	curr->current = entry;
}

static ptrs_symbol_t getSymbol(code_t *code, char *text)
{
	ptrs_symbol_t symbol = {0, 0};
	struct scopelist *scope = code->symbols;
	while(scope != NULL)
	{
		struct symbollist *curr = scope->current;
		while(curr != NULL)
		{
			if(strcmp(curr->text, text) == 0)
			{
				symbol.offset = curr->offset;
				return symbol;
			}
			curr = curr->next;
		}

		scope = scope->outer;
		symbol.scope++;
	}

	char buff[128];
	sprintf(buff, "Unknown identifier %s", text);
	unexpectedm(code, NULL, buff);
	return symbol; //doh
}

static bool lookahead(code_t *code, const char *str)
{
	int start = code->pos;
	while(*str)
	{
		if(code->curr != *str)
		{
			code->pos = start;
			code->curr = code->src[start];
			return false;
		}
		str++;
		rawnext(code);
	}

	str--;
	if((isalpha(*str) || *str == '_') && (isalpha(code->curr) || code->curr == '_'))
	{
		code->pos = start;
		code->curr = code->src[start];
		return false;
	}

	code->pos--;
	code->curr = code->src[start];
	next(code);
	return true;
}

static void consume(code_t *code, const char *str)
{
	while(*str)
	{
		if(code->curr != *str)
			unexpected(code, str);
		str++;
		next(code);
	}
}
static void consumec(code_t *code, char c)
{
	if(code->curr != c)
	{
		char expected[] = {c, 0};
		unexpected(code, expected);
	}
	next(code);
}
static void consumecm(code_t *code, char c, const char *msg)
{
	if(code->curr != c)
		unexpectedm(code, NULL, msg);
	next(code);
}

static bool skipSpaces(code_t *code)
{
	int pos = code->pos;
	char curr = code->src[pos];
	if(isspace(curr))
	{
		while(isspace(curr))
		{
			pos++;
			curr = code->src[pos];
		}

		code->pos = pos;
		code->curr = curr;
		return true;
	}

	return false;
}
static bool skipComments(code_t *code)
{
	int pos = code->pos;
	char curr = code->src[pos];

	if(curr == '/' && code->src[pos + 1] == '/')
	{
		while(curr != '\n')
		{
			pos++;
			curr = code->src[pos];
		}

		code->pos = pos + 1;
		return true;
	}
	else if(curr == '/' && code->src[pos + 1] == '*')
	{
		while(curr != '*' || code->src[pos + 1] != '/')
		{
			pos++;
			curr = code->src[pos];
		}

		code->pos = pos + 2;
		return true;
	}

	return false;
}
static void next(code_t *code)
{
	code->pos++;
	while(skipSpaces(code) || skipComments(code));
	code->curr = code->src[code->pos];
}
static void rawnext(code_t *code)
{
	code->pos++;
	code->curr = code->src[code->pos];
}

static void unexpectedm(code_t *code, const char *expected, const char *msg)
{
	ptrs_ast_t node;
	node.codepos = code->pos;
	node.code = code->src;
	node.file = code->filename;

	char actual[16] = {0};
	char *curr = &code->src[code->pos];
	bool startIsAlnum = isalnum(curr[0]);
	bool startIsPunct = ispunct(curr[0]);

	for(int i = 0; i < 16; i++)
	{
		if(startIsAlnum && isalnum(curr[i]))
			actual[i] = curr[i];
		else if(startIsPunct && ispunct(curr[i]))
			actual[i] = curr[i];
		else
			break;
	}

	if(msg == NULL)
		PTRS_HANDLE_ASTERROR(&node, "Expected %s got '%s'", expected, actual);
	else if(expected == NULL)
		PTRS_HANDLE_ASTERROR(&node, "%s", msg);
	else
		PTRS_HANDLE_ASTERROR(&node, "%s - Expected %s got '%s'", msg, expected, actual);

}
