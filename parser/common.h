#ifndef _PTRS_COMMON
#define _PTRS_COMMON

#include <stdint.h>

struct ptrs_var;

typedef enum type
{
	PTRS_TYPE_UNDEFINED,
	PTRS_TYPE_INT,
	PTRS_TYPE_FLOAT,
	PTRS_TYPE_RAW,
	PTRS_TYPE_STRING,
	PTRS_TYPE_POINTER,
	PTRS_TYPE_FUNCTION,
	PTRS_TYPE_NATIVEFUNC,
	PTRS_TYPE_OBJECT
} ptrs_vartype_t;

typedef struct object
{
	char *key;
	struct ptrs_var *value;
	struct object *next;
} ptrs_object_t;

typedef intptr_t (*ptrs_nativefunc_t)(intptr_t arg0, ...);

typedef union val
{
	int64_t intval;
	double floatval;
	signed char *strval;
	struct ptrs_var *ptrval;
	ptrs_nativefunc_t *nativefunc;
	ptrs_object_t *objval;
} ptrs_val_t;

struct ptrs_var
{
	ptrs_vartype_t type;
	ptrs_val_t value;
};
typedef struct ptrs_var ptrs_var_t;

#endif