#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "../parser/ast.h"
#include "../parser/common.h"
#include "include/error.h"
#include "include/conversion.h"
#include "include/stack.h"
#include "include/scope.h"
#include "include/object.h"
#include "include/run.h"

ptrs_var_t *ptrs_handle_body(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	struct ptrs_astlist *list = node->arg.astlist;
	ptrs_var_t *_result;

	while(list)
	{
		_result = list->entry->handler(list->entry, result, scope);

		if(scope->exit != 0)
		{
			if(scope->exit == 1)
				scope->exit = 0;
			return _result;
		}

		list = list->next;
	}
	return _result;
}

ptrs_var_t *ptrs_handle_define(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	struct ptrs_ast_define stmt = node->arg.define;

	if(stmt.value != NULL)
		result = stmt.value->handler(stmt.value, result, scope);
	else
		result->type = PTRS_TYPE_UNDEFINED;

	ptrs_scope_set(scope, stmt.name, result);

	return result;
}

ptrs_var_t *ptrs_handle_array(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	struct ptrs_ast_define stmt = node->arg.define;

	ptrs_var_t *val = ptrs_object_get(scope->current, stmt.name);
	if(val != NULL)
		ptrs_warn(node, "defining array multiple times in one scope");

	val = stmt.value->handler(stmt.value, result, scope);
	int size = ptrs_vartoi(val);

	if(size <= 0 || size > PTRS_STACK_SIZE)
		ptrs_error(node, "Trying to create array of size %d", size);

	if(size % sizeof(ptrs_var_t) == 0)
		result->type = PTRS_TYPE_POINTER;
	else
		result->type = PTRS_TYPE_NATIVE;

	result->value.nativeval = ptrs_alloc(size);
	ptrs_scope_set(scope, stmt.name, result);
	return result;
}

void importNative(const char *from, ptrs_ast_t *node, ptrs_scope_t *scope)
{
	ptrs_var_t valuev;
	ptrs_var_t *value;
	char namebuff[128];
	const char *name;
	const char *error;

	dlerror();

	void *handle = NULL;
	if(from != NULL)
	{
		handle = dlopen(from, RTLD_LAZY);

		error = dlerror();
		if(error != NULL)
			ptrs_error(node->arg.import.from, "%s", error);
	}

	struct ptrs_astlist *list = node->arg.import.fields;
	while(list != NULL)
	{
		value = list->entry->handler(list->entry, &valuev, scope);
		name = ptrs_vartoa(value, namebuff, 128);

		ptrs_var_t func;
		func.type = PTRS_TYPE_NATIVE;
		func.value.nativeval = dlsym(handle, name);

		error = dlerror();
		if(error != NULL)
			ptrs_error(list->entry, "%s", error);

		if(name == namebuff)
		{
			size_t len = strlen(name) + 1;
			char *_name = ptrs_alloc(len);
			memcpy(_name, name, len);
			name = _name;
		}

		ptrs_scope_set(scope, name, &func);

		list = list->next;
	}
}

void importScript(const char *from, ptrs_ast_t *node, ptrs_scope_t *scope)
{
	ptrs_var_t valuev;
	ptrs_var_t *value;
	char namebuff[128];
	const char *name;

	ptrs_scope_t *_scope = ptrs_alloc(sizeof(ptrs_scope_t));
	_scope->current = NULL;
	_scope->outer = NULL;
	_scope->exit = 0;

	ptrs_dofile(from, &valuev, _scope);

	struct ptrs_astlist *list = node->arg.import.fields;
	while(list != NULL)
	{
		value = list->entry->handler(list->entry, &valuev, scope);
		name = ptrs_vartoa(value, namebuff, 128);

		ptrs_var_t *val = ptrs_scope_get(_scope, name);
		if(val == NULL)
			ptrs_error(list->entry, "Script '%s' has no property '%s'", from, name);

		if(name == namebuff)
		{
			size_t len = strlen(name) + 1;
			char *_name = ptrs_alloc(len);
			memcpy(_name, name, len);
			name = _name;
		}

		ptrs_scope_set(scope, name, val);

		list = list->next;
	}
}

ptrs_var_t *ptrs_handle_import(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	char buff[128];
	const char *name;
	struct ptrs_ast_import import = node->arg.import;

	if(import.from != NULL)
	{
		ptrs_var_t *from = import.from->handler(import.from, result, scope);
		name = ptrs_vartoa(from, buff, 128);

		char *ending = strrchr(name, '.');
		if(strcmp(ending, ".ptrs") == 0)
			importScript(name, node, scope);
		else
			importNative(name, node, scope);
	}
	else
	{
		importNative(NULL, node, scope);
	}

	return result;
}

ptrs_var_t *ptrs_handle_return(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	ptrs_ast_t *ast = node->arg.astval;
	ptrs_var_t *val = ast->handler(ast, result, scope);

	if(val != result)
		memcpy(result, val, sizeof(ptrs_var_t));

	scope->exit = 3;
	return result;
}

ptrs_var_t *ptrs_handle_break(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	scope->exit = 2;
	return result;
}

ptrs_var_t *ptrs_handle_continue(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	scope->exit = 1;
	return result;
}

ptrs_var_t *ptrs_handle_function(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	struct ptrs_ast_function astfunc = node->arg.function;

	ptrs_function_t *func = ptrs_alloc(sizeof(ptrs_function_t));
	func->name = astfunc.name;
	func->argc = astfunc.argc;
	func->args = astfunc.args;
	func->body = astfunc.body;
	func->scope = scope;

	result->type = PTRS_TYPE_FUNCTION;
	result->value.funcval = func;
	result->meta.this = NULL;

	ptrs_scope_set(scope, astfunc.name, result);
	return result;
}

ptrs_var_t *ptrs_handle_struct(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	result->type = PTRS_TYPE_STRUCT;
	result->value.structval = &node->arg.structval;

	struct ptrs_structlist *curr = node->arg.structval.member;
	while(curr != NULL)
	{
		if(curr->function != NULL)
			curr->function->scope = scope;
		curr = curr->next;
	}

	ptrs_scope_set(scope, node->arg.structval.name, result);
	return result;
}

ptrs_var_t *ptrs_handle_if(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	struct ptrs_ast_ifelse stmt = node->arg.ifelse;
	result = stmt.condition->handler(stmt.condition, result, scope);

	if(ptrs_vartob(result))
		result = stmt.ifBody->handler(stmt.ifBody, result, scope);
	else if(stmt.elseBody != NULL)
		result = stmt.elseBody->handler(stmt.elseBody, result, scope);

	return result;
}

ptrs_var_t *ptrs_handle_while(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	ptrs_var_t conditionv;
	ptrs_var_t *condition;
	ptrs_var_t *_result = result;

	struct ptrs_ast_control stmt = node->arg.control;

	for(;;)
	{
		condition = stmt.condition->handler(stmt.condition, &conditionv, scope);
		if(!ptrs_vartob(condition))
			break;

		result = _result;
		result = stmt.body->handler(stmt.body, result, scope);

		if(scope->exit != 0)
		{
			if(scope->exit == 2)
				scope->exit = 0;
			return result;
		}
	}

	return result;
}

ptrs_var_t *ptrs_handle_dowhile(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	ptrs_var_t conditionv;
	ptrs_var_t *condition;
	ptrs_var_t *_result = result;

	struct ptrs_ast_control stmt = node->arg.control;

	do
	{
		result = _result;
		result = stmt.body->handler(stmt.body, result, scope);

		if(scope->exit != 0)
		{
			if(scope->exit == 2)
				scope->exit = 0;
			return result;
		}

		condition = stmt.condition->handler(stmt.condition, &conditionv, scope);
	} while(ptrs_vartob(condition));

	return result;
}

ptrs_var_t *ptrs_handle_for(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	ptrs_var_t conditionv;
	ptrs_var_t *condition;

	struct ptrs_ast_for stmt = node->arg.forstatement;

	stmt.init->handler(stmt.init, result, scope);

	for(;;)
	{
		condition = stmt.condition->handler(stmt.condition, &conditionv, scope);
		if(!ptrs_vartob(condition))
			break;

		stmt.body->handler(stmt.body, result, scope);

		if(scope->exit != 0)
		{
			if(scope->exit == 2)
				scope->exit = 0;
			return result;
		}

		stmt.step->handler(stmt.step, result, scope);
	}

	return result;
}

ptrs_var_t *ptrs_handle_exprstatement(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	ptrs_ast_t *expr = node->arg.astval;
	result = expr->handler(expr, result, scope);
	return result;
}
