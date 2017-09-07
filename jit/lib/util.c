#include <assert.h>
#include <jit/jit.h>

#include "../../parser/common.h"
#include "../../parser/ast.h"
#include "../include/run.h"
#include "../include/conversion.h"

void ptrs_initScope(ptrs_scope_t *scope)
{
	scope->continueLabel = jit_label_undefined;
	scope->breakLabel = jit_label_undefined;
	scope->firstAssertion = NULL;
	scope->lastAssertion = NULL;
	scope->indexSize = NULL;
}

jit_type_t ptrs_jit_getVarType()
{
	static jit_type_t vartype = NULL;
	if(vartype == NULL)
	{
		jit_type_t fields[] = {
			jit_type_long,
			jit_type_ulong,
		};

		vartype = jit_type_create_struct(fields, 2, 0);
	}

	return vartype;
}

ptrs_jit_var_t ptrs_jit_valToVar(jit_function_t func, jit_value_t val)
{
	assert(jit_value_get_type(val) == ptrs_jit_getVarType());
	jit_value_set_addressable(val);

	jit_value_t addr = jit_insn_address_of(func, val);

	ptrs_jit_var_t ret = {
		.val = jit_insn_load_relative(func, addr, 0, jit_type_long),
		.meta = jit_insn_load_relative(func, addr, sizeof(ptrs_val_t), jit_type_ulong),
		.constType = -1,
	};

	return ret;
}

jit_value_t ptrs_jit_varToVal(jit_function_t func, ptrs_jit_var_t var)
{
	jit_value_t val = jit_value_create(func, ptrs_jit_getVarType());
	jit_value_set_addressable(val);

	jit_value_t addr = jit_insn_address_of(func, val);

	jit_insn_store_relative(func, addr, 0, var.val);
	jit_insn_store_relative(func, addr, sizeof(ptrs_val_t), var.meta);

	return val;
}

ptrs_val_t ptrs_jit_value_getValConstant(jit_value_t val)
{
	jit_long _constVal = jit_value_get_long_constant(val);
	return *(ptrs_val_t *)&_constVal;
}

ptrs_meta_t ptrs_jit_value_getMetaConstant(jit_value_t meta)
{
	jit_ulong _constMeta = jit_value_get_long_constant(meta);
	return *(ptrs_meta_t *)&_constMeta;
}

jit_value_t ptrs_jit_reinterpretCast(jit_function_t func, jit_value_t val, jit_type_t newType)
{
	if(jit_value_is_constant(val))
	{
		jit_constant_t constVal = jit_value_get_constant(val);
		jit_type_t type = jit_type_normalize(jit_type_promote_int(newType));

		if(type == jit_type_int || type == jit_type_uint)
			return jit_value_create_nint_constant(func, newType, constVal.un.int_value);
		else if(type == jit_type_long || type == jit_type_ulong)
			return jit_value_create_long_constant(func, newType, constVal.un.long_value);
		else if(type == jit_type_float64)
			return jit_value_create_float64_constant(func, newType, constVal.un.float64_value);
	}

	if(jit_value_is_addressable(val))
	{
		return jit_insn_load_relative(func, jit_insn_address_of(func, val), 0, newType);
	}
	else
	{

		jit_value_t ret = jit_value_create(func, newType);
		jit_value_set_addressable(ret);
		jit_insn_store_relative(func, jit_insn_address_of(func, ret), 0, val);
		return ret;
	}
}
