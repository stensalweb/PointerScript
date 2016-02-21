#include <stdlib.h>
#include <dlfcn.h>

#include "../parser/ast.h"
#include "../parser/common.h"
#include "include/error.h"
#include "include/conversion.h"
#include "include/memory.h"
#include "include/scope.h"

ptrs_var_t *ptrs_handle_body(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	struct ptrs_astlist *list = node->arg.astlist;

	while(list)
	{
		result = list->entry->handler(list->entry, result, scope);
		list = list->next;
	}
	return result;
}

ptrs_var_t *ptrs_handle_define(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	struct ptrs_ast_define stmt = node->arg.define;

	if(stmt.value != NULL)
		result = stmt.value->handler(stmt.value, result, scope);

	ptrs_scope_set(scope, stmt.name, ptrs_vardup(result));

	return result;
}

ptrs_var_t *ptrs_handle_import(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	ptrs_var_t valuev;
	ptrs_var_t *value;

	struct ptrs_ast_import import = node->arg.import;

	char from[128];
	if(import.from != NULL)
	{
		value = import.from->handler(import.from, &valuev, scope);
		ptrs_vartoa(value, from);
	}

	void *handle = dlopen(from, RTLD_LAZY);
	if(handle == NULL)
		ptrs_error(node, "%s", dlerror());

	struct ptrs_astlist *list = import.fields;
	while(list != NULL)
	{
		char name[32];
		value = list->entry->handler(list->entry, &valuev, scope);
		ptrs_vartoa(value, name);

		ptrs_var_t *func = ptrs_alloc();
		func->type = PTRS_TYPE_NATIVEFUNC;
		func->value.nativefunc = dlsym(handle, name);

		char *error = dlerror();
		if(error != NULL)
			ptrs_error(list->entry, "%s", error);

		ptrs_scope_set(scope, name, func);

		list = list->next;
	}

	dlclose(handle);
	return result;
}

ptrs_var_t *ptrs_handle_if(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	struct ptrs_ast_control stmt = node->arg.control;
	result = stmt.condition->handler(stmt.condition, result, scope);

	if(ptrs_vartob(result))
	{
		result = stmt.body->handler(stmt.body, result, scope);
	}

	return result;
}

ptrs_var_t *ptrs_handle_exprstatement(ptrs_ast_t *node, ptrs_var_t *result, ptrs_scope_t *scope)
{
	ptrs_ast_t *expr = node->arg.astval;
	result = expr->handler(expr, result, scope);
	return result;
}