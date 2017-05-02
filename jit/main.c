#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include "../parser/ast.h"
#include "../parser/common.h"
#include "include/run.h"
#include "include/error.h"

static bool handleSignals = true;
static bool interactive = false;
static int dumpOps = -1;
extern size_t ptrs_arraymax;
extern bool ptrs_zeroMemory;
extern int ptrs_asmSize;

static struct option options[] = {
	{"array-max", required_argument, 0, 1},
	{"no-sig", no_argument, 0, 2},
	{"asmdump", no_argument, 0, 3},
	{"jitdump", no_argument, 0, 4},
	{"error", required_argument, 0, 5},
	{"help", no_argument, 0, 6},
	{0, 0, 0, 0}
};

static int parseOptions(int argc, char **argv)
{
	for(;;)
	{
		int ret = getopt_long_only(argc, argv, "+", options, NULL);

		switch(ret)
		{
			case -1:
				return optind;
			case 1:
				ptrs_arraymax = strtoul(optarg, NULL, 0);
				break;
			case 2:
				handleSignals = false;
				break;
			case 3:
				if(dumpOps == JIT_DEBUG_OPS)
					dumpOps = JIT_DEBUG_COMBINED;
				else
					dumpOps = JIT_DEBUG_CODE;
				break;
			case 4:
				if(dumpOps == JIT_DEBUG_CODE)
					dumpOps = JIT_DEBUG_COMBINED;
				else
					dumpOps = JIT_DEBUG_OPS;
				break;
				ptrs_errorfile = fopen(optarg, "w");
				if(ptrs_errorfile == NULL)
				{
					fprintf(stderr, "Could not open %s\n", optarg);
					exit(EXIT_FAILURE);
				}
				break;
			case 5:
				printf("Usage: ptrs [options ...] <file> [script options ...]\n"
					"Valid Options:\n"
						"\t--help               Show this information\n"
						"\t--array-max <size>   Set maximal allowed array size to 'size' bytes. Default: 0x%X\n"
						"\t--error <file>       Set where error messages are written to. Default: /dev/stderr\n"
						"\t--no-sig             Do not listen to signals.\n"
					"Source code can be found at https://github.com/M4GNV5/PointerScript\n", UINT32_MAX);
				exit(EXIT_SUCCESS);
			default:
				fprintf(stderr, "Try '--help' for more information.\n");
				exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char **argv)
{
	ptrs_errorfile = stderr;
	int i = parseOptions(argc, argv);

	if(i == argc)
	{
		fprintf(stderr, "No input file specified\n");
		return EXIT_FAILURE;
	}
	char *file = argv[i++];

	if(handleSignals)
		ptrs_handle_signals();

	int len = argc - i;
	ptrs_var_t arguments[len];
	for(int j = 0; j < len; j++)
	{
		arguments[j].value.strval = argv[i++];
		arguments[j].meta.type = PTRS_TYPE_NATIVE;
		arguments[j].meta.array.readOnly = false;
	}

	//TODO pass the arguments to the main function
	ptrs_result_t *result = ptrs_compilefile(file);

	if(dumpOps != -1)
		jit_dump_ops(result->jit, dumpOps);
	else
		result->code();

	return EXIT_SUCCESS;
}