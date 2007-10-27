#include <stdio.h>

#include "lib.h"
#include "mempool.h"
#include "hash.h"

#include "sieve-common.h"
#include "sieve-extensions.h"
#include "sieve-commands-private.h"
#include "sieve-code.h"
#include "sieve-binary.h"

#include "sieve-generator.h"

/* Jump list */
void sieve_jumplist_init(struct sieve_jumplist *jlist)
{
	t_array_init(&jlist->jumps, 4);
}

void sieve_jumplist_add(struct sieve_jumplist *jlist, sieve_size_t jump) 
{
	array_append(&jlist->jumps, &jump, 1);
}

void sieve_jumplist_resolve(struct sieve_jumplist *jlist, struct sieve_generator *generator) 
{
	unsigned int i;
	
	for ( i = 0; i < array_count(&jlist->jumps); i++ ) {
		const sieve_size_t *jump = array_idx(&jlist->jumps, i);
	
		sieve_generator_resolve_offset(generator, *jump);
	}
	
	array_free(&jlist->jumps);
}

/* Extensions */

struct sieve_extension_registration {
	const struct sieve_extension *extension;
	unsigned int opcode;
};

/* Generator */

struct sieve_generator {
	pool_t pool;
	
	struct sieve_ast *ast;
	
	struct sieve_binary *binary;
	
	struct hash_table *extension_index; 
};

struct sieve_generator *sieve_generator_create(struct sieve_ast *ast) 
{
	pool_t pool;
	struct sieve_generator *generator;
	
	pool = pool_alloconly_create("sieve_generator", 4096);	
	generator = p_new(pool, struct sieve_generator, 1);
	generator->pool = pool;
	
	generator->ast = ast;	
	sieve_ast_ref(ast);

	generator->binary = sieve_binary_create_new();
	sieve_binary_ref(generator->binary);
	
	generator->extension_index = hash_create
		(pool, pool, 0, NULL, NULL);
	
	return generator;
}

void sieve_generator_free(struct sieve_generator *generator) 
{
	hash_destroy(&generator->extension_index);
	
	sieve_ast_unref(&generator->ast);
	sieve_binary_unref(&generator->binary);
	pool_unref(&(generator->pool));
}

/* Registration functions */

void sieve_generator_register_extension
	(struct sieve_generator *generator, const struct sieve_extension *extension) 
{
	struct sieve_extension_registration *reg;
	
	reg = p_new(generator->pool, struct sieve_extension_registration, 1);
	reg->extension = extension;
	reg->opcode = sieve_binary_link_extension(generator->binary, extension);
	
	hash_insert(generator->extension_index, (void *) extension, (void *) reg);
}

unsigned int sieve_generator_find_extension		
	(struct sieve_generator *generator, const struct sieve_extension *extension) 
{
  struct sieve_extension_registration *reg = 
    (struct sieve_extension_registration *) hash_lookup(generator->extension_index, extension);
    
  return reg->opcode;
}

/* Offset emission */

inline sieve_size_t sieve_generator_emit_offset(struct sieve_generator *generator, int offset) 
{
	return sieve_binary_emit_offset(generator->binary, offset);
}

inline void sieve_generator_resolve_offset(struct sieve_generator *generator, sieve_size_t address) 
{
	sieve_binary_resolve_offset(generator->binary, address);
}

/* Literal emission */

inline sieve_size_t sieve_generator_emit_byte(struct sieve_generator *generator, unsigned char btval)
{
  return sieve_binary_emit_byte(generator->binary, btval);
}


inline sieve_size_t sieve_generator_emit_integer(struct sieve_generator *generator, sieve_size_t integer)
{
  return sieve_binary_emit_integer(generator->binary, integer);
}

inline sieve_size_t sieve_generator_emit_string(struct sieve_generator *generator, const string_t *str)
{
  return sieve_binary_emit_string(generator->binary, str);
}

/* Emit operands */

sieve_size_t sieve_generator_emit_operand
	(struct sieve_generator *generator, int operand)
{
	unsigned char op = operand & SIEVE_OPERAND_CORE_MASK;
	
	return sieve_binary_emit_byte(generator->binary, op);
}

/* Emit opcodes */

sieve_size_t sieve_generator_emit_opcode
	(struct sieve_generator *generator, int opcode)
{
	unsigned char op = opcode & SIEVE_OPCODE_CORE_MASK;
	
	return sieve_binary_emit_byte(generator->binary, op);
}

sieve_size_t sieve_generator_emit_ext_opcode
	(struct sieve_generator *generator, const struct sieve_extension *extension)
{	
	unsigned char op = SIEVE_OPCODE_EXT_OFFSET + sieve_generator_find_extension(generator, extension);
	
	return sieve_binary_emit_byte(generator->binary, op);
}

/* Generator functions */

bool sieve_generate_arguments(struct sieve_generator *generator, 
	struct sieve_command_context *cmd, struct sieve_ast_argument **last_arg)
{
	struct sieve_ast_argument *arg = sieve_ast_argument_first(cmd->ast_node);
	
	/* Parse all arguments with assigned generator function */
	while ( arg != NULL && arg->argument != NULL) {
		const struct sieve_argument *argument = arg->argument;
		
		/* Call the generation function for the argument */ 
		if ( argument->generate != NULL ) { 
			if ( !argument->generate(generator, &arg, cmd) ) 
				return FALSE;
		} else break;
	}
	
	if ( last_arg != NULL )
		*last_arg = arg;
	
	return TRUE;
}

bool sieve_generate_test
	(struct sieve_generator *generator, struct sieve_ast_node *tst_node,
		struct sieve_jumplist *jlist, bool jump_true) 
{
	i_assert( tst_node->context != NULL && tst_node->context->command != NULL );

	if ( tst_node->context->command->control_generate != NULL ) {
		if ( tst_node->context->command->control_generate
			(generator, tst_node->context, jlist, jump_true) ) 
			return TRUE;
		
		return FALSE;
	}
	
	if ( tst_node->context->command->generate != NULL ) {

		if ( tst_node->context->command->generate(generator, tst_node->context) ) {
			
			if ( jump_true ) 
				sieve_generator_emit_opcode(generator, SIEVE_OPCODE_JMPTRUE);
			else
				sieve_generator_emit_opcode(generator, SIEVE_OPCODE_JMPFALSE);
			sieve_jumplist_add(jlist, sieve_generator_emit_offset(generator, 0));
						
			return TRUE;
		}	
		
		return FALSE;
	}
	
	return TRUE;
}

static bool sieve_generate_command(struct sieve_generator *generator, struct sieve_ast_node *cmd_node) 
{
	i_assert( cmd_node->context != NULL && cmd_node->context->command != NULL );

	if ( cmd_node->context->command->generate != NULL ) {
		return cmd_node->context->command->generate(generator, cmd_node->context);
	}
	
	return TRUE;		
}

bool sieve_generate_block(struct sieve_generator *generator, struct sieve_ast_node *block) 
{
	struct sieve_ast_node *command;

	t_push();	
	command = sieve_ast_command_first(block);
	while ( command != NULL ) {	
		sieve_generate_command(generator, command);	
		command = sieve_ast_command_next(command);
	}		
	t_pop();
	
	return TRUE;
}

struct sieve_binary *sieve_generator_run(struct sieve_generator *generator) {	
	if ( sieve_generate_block(generator, sieve_ast_root(generator->ast)) ) {
	 	return generator->binary;
	} 
	
	return NULL;
}


