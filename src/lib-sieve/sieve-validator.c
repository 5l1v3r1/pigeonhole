#include "lib.h"
#include "mempool.h"
#include "hash.h"

#include "sieve-script.h"
#include "sieve-ast.h"
#include "sieve-commands.h"
#include "sieve-commands-private.h"
#include "sieve-validator.h"
#include "sieve-extensions.h"

#include "sieve-comparators.h"
#include "sieve-address-parts.h"

/* Context/Semantics checker implementation */

struct sieve_validator {
	pool_t pool;
	
	struct sieve_ast *ast;
	struct sieve_script *script;
	
	struct sieve_error_handler *ehandler;
	
	/* Registries */
	struct hash_table *commands;
	
	ARRAY_DEFINE(ext_contexts, void *);
};

/* Predeclared statics */

static void sieve_validator_register_core_commands(struct sieve_validator *validator);
static void sieve_validator_register_core_tests(struct sieve_validator *validator);

/* Error management */

void sieve_validator_warning
(struct sieve_validator *validator, struct sieve_ast_node *node, 
	const char *fmt, ...) 
{ 
	va_list args;
	
	va_start(args, fmt);
	sieve_ast_error(validator->ehandler, sieve_vwarning, node, fmt, args);
	va_end(args);
}
 
void sieve_validator_error
(struct sieve_validator *validator, struct sieve_ast_node *node, 
	const char *fmt, ...) 
{
	va_list args;
	
	va_start(args, fmt);
	sieve_ast_error(validator->ehandler, sieve_verror, node, fmt, args);
	va_end(args);
}

void sieve_validator_critical
(struct sieve_validator *validator, struct sieve_ast_node *node, 
	const char *fmt, ...) 
{
	va_list args;
	
	va_start(args, fmt);
	sieve_ast_error(validator->ehandler, sieve_vcritical, node, fmt, args);
	va_end(args);
}

/* Validator object */

struct sieve_validator *sieve_validator_create
	(struct sieve_ast *ast, struct sieve_error_handler *ehandler) 
{
	unsigned int i;
	pool_t pool;
	struct sieve_validator *validator;
	
	pool = pool_alloconly_create("sieve_validator", 4096);	
	validator = p_new(pool, struct sieve_validator, 1);
	validator->pool = pool;
	
	validator->ehandler = ehandler;
	
	validator->ast = ast;	
	validator->script = sieve_ast_script(ast);
	sieve_ast_ref(ast);

	/* Setup storage for extension contexts */		
	array_create(&validator->ext_contexts, pool, sizeof(void *), 
		sieve_extensions_get_count());
		
	/* Setup command registry */
	validator->commands = hash_create
		(pool, pool, 0, str_hash, (hash_cmp_callback_t *)strcmp);
	sieve_validator_register_core_commands(validator);
	sieve_validator_register_core_tests(validator);
	
	/* Pre-load core language features implemented as 'extensions' */
	for ( i = 0; i < sieve_preloaded_extensions_count; i++ ) {
		const struct sieve_extension *ext = sieve_preloaded_extensions[i];
		
		if ( ext->validator_load != NULL )
			(void)ext->validator_load(validator);		
	}
	
	return validator;
}

void sieve_validator_free(struct sieve_validator **validator) 
{
	hash_destroy(&(*validator)->commands);
	sieve_ast_unref(&(*validator)->ast);

	pool_unref(&(*validator)->pool);

	*validator = NULL;
}

inline pool_t sieve_validator_pool(struct sieve_validator *validator)
{
	return validator->pool;
}

inline struct sieve_error_handler *sieve_validator_error_handler
	(struct sieve_validator *validator)
{
	return validator->ehandler;
}

inline struct sieve_ast *sieve_validator_ast
	(struct sieve_validator *validator)
{
	return validator->ast;
}

inline struct sieve_script *sieve_validator_script
	(struct sieve_validator *validator)
{
	return validator->script;
}

/* Command registry */

struct sieve_tag_registration;

struct sieve_command_registration {
	const struct sieve_command *command;
	
	struct hash_table *tags;
	ARRAY_DEFINE(instanced_tags, struct sieve_tag_registration *); 
};

/* Dummy function */
static bool _cmd_unknown_validate
	(struct sieve_validator *validator ATTR_UNUSED, 
	struct sieve_command_context *cmd ATTR_UNUSED) 
{
	i_unreached();
	return FALSE;
}

static const struct sieve_command unknown_command = { 
	"", SCT_NONE, 0, 0, FALSE, FALSE , 
	NULL, NULL, _cmd_unknown_validate, NULL, NULL 
};

static void sieve_validator_register_core_tests(struct sieve_validator *validator) 
{
	unsigned int i;
	
	for ( i = 0; i < sieve_core_tests_count; i++ ) {
		sieve_validator_register_command(validator, sieve_core_tests[i]); 
	}
}

static void sieve_validator_register_core_commands(struct sieve_validator *validator) 
{
	unsigned int i;
	
	for ( i = 0; i < sieve_core_commands_count; i++ ) {
		sieve_validator_register_command(validator, sieve_core_commands[i]); 
	}
}

static struct sieve_command_registration *sieve_validator_find_command_registration
		(struct sieve_validator *validator, const char *command) 
{
  return 	(struct sieve_command_registration *) hash_lookup(validator->commands, command);
}

static struct sieve_command_registration *_sieve_validator_register_command
	(struct sieve_validator *validator, const struct sieve_command *command,
	const char *identifier) 
{
	struct sieve_command_registration *record = 
		p_new(validator->pool, struct sieve_command_registration, 1);
	record->command = command;
	record->tags = NULL;
	hash_insert(validator->commands, (void *) identifier, (void *) record);
		
	return record;
}

void sieve_validator_register_command
	(struct sieve_validator *validator, const struct sieve_command *command) 
{
	struct sieve_command_registration *cmd_reg =
		sieve_validator_find_command_registration(validator, command->identifier);
		
	if ( cmd_reg == NULL ) 
		cmd_reg = _sieve_validator_register_command
			(validator, command, command->identifier);
	else
		cmd_reg->command = command;
	
	if ( command->registered != NULL ) 
		command->registered(validator, cmd_reg);
}

static void sieve_validator_register_unknown_command
	(struct sieve_validator *validator, const char *command) 
{
	(void)_sieve_validator_register_command(validator, &unknown_command, command);		
}

static const struct sieve_command *
	sieve_validator_find_command(struct sieve_validator *validator, const char *command) 
{
  struct sieve_command_registration *record = 
  	sieve_validator_find_command_registration(validator, command);
  
  return ( record == NULL ? NULL : record->command );
}

/* Per-command tag/argument registry */

struct sieve_tag_registration {
	const struct sieve_argument *tag;
	
	int id_code;
};

static bool _unknown_tag_validate
	(struct sieve_validator *validator ATTR_UNUSED, 
	struct sieve_ast_argument **arg ATTR_UNUSED, 
	struct sieve_command_context *tst ATTR_UNUSED)
{
	i_unreached();
	return FALSE;
}

static const struct sieve_argument _unknown_tag = 
	{ "", NULL, _unknown_tag_validate, NULL, NULL };

static void _sieve_validator_register_tag
	(struct sieve_validator *validator, struct sieve_command_registration *cmd_reg, 
	const struct sieve_argument *tag, const char *identifier, int id_code) 
{
	struct sieve_tag_registration *reg;
	
	reg = p_new(validator->pool, struct sieve_tag_registration, 1);
	reg->tag = tag;
	reg->id_code = id_code;
	
	if ( cmd_reg->tags == NULL ) {
		cmd_reg->tags = hash_create
			(validator->pool, validator->pool, 0, str_hash, (hash_cmp_callback_t *)strcmp);
	}
	
	hash_insert(cmd_reg->tags, (void *) identifier, (void *) reg);
}

void sieve_validator_register_external_tag
(struct sieve_validator *validator, const struct sieve_argument *tag, 
	const char *command, int id_code) 
{
	struct sieve_command_registration *cmd_reg = 
		sieve_validator_find_command_registration(validator, command);
		
	if ( cmd_reg == NULL ) {
		cmd_reg = _sieve_validator_register_command(validator, NULL, command);
	}
	
	_sieve_validator_register_tag
		(validator, cmd_reg, tag, tag->identifier, id_code);
}

void sieve_validator_register_tag
	(struct sieve_validator *validator, struct sieve_command_registration *cmd_reg, 
	const struct sieve_argument *tag, int id_code) 
{
	if ( tag->is_instance_of == NULL )
		_sieve_validator_register_tag(validator, cmd_reg, tag, tag->identifier, id_code);
	else {
		struct sieve_tag_registration *reg = 
			p_new(validator->pool, struct sieve_tag_registration, 1);
		reg->tag = tag;
		reg->id_code = id_code;

		if ( !array_is_created(&cmd_reg->instanced_tags) ) 
				p_array_init(&cmd_reg->instanced_tags, validator->pool, 1);
				
		array_append(&cmd_reg->instanced_tags, &reg, 1);
	}
}

static void sieve_validator_register_unknown_tag
	(struct sieve_validator *validator, struct sieve_command_registration *cmd_reg, 
	const char *tag) 
{
	_sieve_validator_register_tag(validator, cmd_reg, &_unknown_tag, tag, 0);
}

static const struct sieve_argument *sieve_validator_find_tag
	(struct sieve_validator *validator, 
		struct sieve_command_registration *cmd_reg, 
		const char *tag, unsigned int *id_code) 
{
	unsigned int i;
	const struct sieve_tag_registration *reg;
	
	*id_code = 0;
	
	if ( cmd_reg->tags != NULL ) {
		reg = (const struct sieve_tag_registration *) 
			hash_lookup(cmd_reg->tags, tag);
	
		if ( reg != NULL ) {
			*id_code = reg->id_code;
  		return reg->tag; 
  	}
  }
  
  /* Not found so far, try the instanced tags */
  if ( array_is_created(&cmd_reg->instanced_tags) ) {
	  for ( i = 0; i < array_count(&cmd_reg->instanced_tags); i++ ) {
	  	struct sieve_tag_registration * const *reg = 
	  		array_idx(&cmd_reg->instanced_tags, i);
  	
	  	if ( (*reg)->tag != NULL && (*reg)->tag->is_instance_of(validator, tag) ) {
	  		*id_code = (*reg)->id_code;
	  		return (*reg)->tag;
	  	}
	  }
	}
	
	return NULL;
}

/* Extension support */

int sieve_validator_extension_load
	(struct sieve_validator *validator, struct sieve_command_context *cmd, 
		const char *ext_name) 
{
	const struct sieve_extension *ext;
	int ext_id = sieve_extension_get_by_name(ext_name, &ext); 
	
	if ( ext_id < 0 ) {
		sieve_command_validate_error(validator, cmd, 
			"unsupported sieve capability '%s'", ext_name);
		return -1;
	}

	if ( ext->validator_load != NULL && !ext->validator_load(validator) ) {
		sieve_command_validate_error(validator, cmd, 
			"failed to load sieve capability '%s'", ext->name);
		return -1;
	}
	
	return ext_id;
}

inline void sieve_validator_extension_set_context
	(struct sieve_validator *validator, int ext_id, void *context)
{
	array_idx_set(&validator->ext_contexts, (unsigned int) ext_id, &context);	
}

inline const void *sieve_validator_extension_get_context
	(struct sieve_validator *validator, int ext_id) 
{
	void * const *ctx;

	if  ( ext_id < 0 || ext_id >= (int) array_count(&validator->ext_contexts) )
		return NULL;
	
	ctx = array_idx(&validator->ext_contexts, (unsigned int) ext_id);		

	return *ctx;
}

/* Argument Validation API */

bool sieve_validate_positional_argument
	(struct sieve_validator *validator, struct sieve_command_context *cmd,
	struct sieve_ast_argument *arg, const char *arg_name, unsigned int arg_pos,
	enum sieve_ast_argument_type req_type)
{
	if ( sieve_ast_argument_type(arg) != req_type && 
		(sieve_ast_argument_type(arg) != SAAT_STRING || 
			req_type != SAAT_STRING_LIST) ) 
	{
		sieve_command_validate_error(validator, cmd, 
			"the %s %s expects %s as argument %d (%s), but %s was found", 
			cmd->command->identifier, sieve_command_type_name(cmd->command), 
			sieve_ast_argument_type_name(req_type),
			arg_pos, arg_name, sieve_ast_argument_name(arg));
		return FALSE; 
	}
	
	return TRUE;
}

void sieve_validator_argument_activate
	(struct sieve_validator *validator ATTR_UNUSED, struct sieve_ast_argument *arg)
{
	switch ( sieve_ast_argument_type(arg) ) {
	case SAAT_NUMBER:
		arg->argument = &number_argument;
		break;
	case SAAT_STRING:
		arg->argument = &string_argument;
		break;
	case SAAT_STRING_LIST:
		arg->argument = &string_list_argument;
		break;
	case SAAT_TAG:
		i_error("!!BUG!!: sieve_validator_argument_activate: cannot activate tagged argument.");
		break;
	default:
		break;
	}
}

bool sieve_validate_tag_parameter
	(struct sieve_validator *validator, struct sieve_command_context *cmd,
	struct sieve_ast_argument *tag, struct sieve_ast_argument *param,
	enum sieve_ast_argument_type req_type)
{
	if ( sieve_ast_argument_type(param) != req_type && 
		(sieve_ast_argument_type(param) != SAAT_STRING || 
			req_type != SAAT_STRING_LIST) ) 
	{
		sieve_command_validate_error(validator, cmd, 
			"the :%s tag for the %s %s requires %s as parameter, "
			"but %s was found", sieve_ast_argument_tag(tag), 
			cmd->command->identifier, sieve_command_type_name(cmd->command),
			sieve_ast_argument_type_name(req_type),	sieve_ast_argument_name(param));
		return FALSE;
	}
	sieve_validator_argument_activate(validator, param);
	param->arg_id_code = tag->arg_id_code;
	
	return TRUE;
}

/* Test validation API */

static bool sieve_validate_command_arguments
(struct sieve_validator *validator, struct sieve_command_context *cmd) 
{
	int arg_count = cmd->command->positional_arguments;
	int real_count = 0;
	struct sieve_ast_argument *arg;
	struct sieve_command_registration *cmd_reg = NULL;
	
	/* Validate any tags that might be present */\
	
	arg = sieve_ast_argument_first(cmd->ast_node);
	
	/* Get the command registration to get access to its tag registry */
	if ( sieve_ast_argument_type(arg) == SAAT_TAG ) {
		cmd_reg = sieve_validator_find_command_registration(validator, cmd->command->identifier);
		
		if ( cmd_reg == NULL ) {
			sieve_command_validate_error(
				validator, cmd, 
				"!!BUG!!: the '%s' %s seemed to be known before, "
				"but somehow its registration got lost",
				cmd->command->identifier, sieve_command_type_name(cmd->command)
			);
			i_error("BUG: the '%s' %s seemed to be known before, "
				"but somehow its registration got lost",
				cmd->command->identifier, sieve_command_type_name(cmd->command)
			);
			return FALSE; 
		}
	}
	
	/* Parse tagged and optional arguments */
	while ( sieve_ast_argument_type(arg) == SAAT_TAG ) {
		unsigned int id_code;
		struct sieve_ast_argument *tag_arg = arg;
		struct sieve_ast_argument *parg; 
		const struct sieve_argument *tag = 
			sieve_validator_find_tag
				(validator, cmd_reg, sieve_ast_argument_tag(arg), &id_code);
		
		if ( tag == NULL ) {
			sieve_command_validate_error(validator, cmd, 
				"unknown tagged argument ':%s' for the %s %s "
				"(reported only once at first occurence)",
				sieve_ast_argument_tag(arg), cmd->command->identifier, 
				sieve_command_type_name(cmd->command));
			sieve_validator_register_unknown_tag
				(validator, cmd_reg, sieve_ast_argument_tag(arg));
			return FALSE;					
		}
		
		/* Check whether previously tagged as unknown */
		if ( tag->identifier != NULL && *(tag->identifier) == '\0' ) 
			return FALSE;
		
		/* Assign the tagged argument type to the ast for later reference 
		 * (in generator) 
		 */
		arg->argument = tag;
		arg->arg_id_code = id_code;
		
		/* Call the validation function for the tag (if present)
		 *   Fail if the validation fails:
		 *     Let's not whine multiple times about a single command having multiple 
		 *     bad arguments...
		 */ 
		if ( tag->validate != NULL ) { 
			if ( !tag->validate(validator, &arg, cmd) ) 
				return FALSE;
		} else
			arg = sieve_ast_argument_next(arg);  
			
		/* Scan backwards for any duplicates */
		parg = sieve_ast_argument_prev(tag_arg);
		while ( parg != NULL ) {
			if ( parg->argument == tag ) {
				const char *tag_id = sieve_ast_argument_tag(tag_arg);
				const char *tag_desc =
					strcmp(tag->identifier, tag_id) != 0 ?
					t_strdup_printf("%s argument (:%s)", tag->identifier, tag_id) : 
					t_strdup_printf(":%s argument", tag->identifier); 
					 
				sieve_command_validate_error(validator, cmd, 
					"encountered duplicate %s for the %s %s",
					tag_desc, cmd->command->identifier, 
					sieve_command_type_name(cmd->command));
					
				return FALSE;	
			}
			
			parg = sieve_ast_argument_prev(parg);
		}
	} 
	
	/* Remaining arguments should be positional (tags are not allowed here) */
	cmd->first_positional = arg;
	
	while ( arg != NULL ) {
		if ( sieve_ast_argument_type(arg) == SAAT_TAG ) {
			sieve_command_validate_error(validator, cmd, 
				"encountered an unexpected tagged argument ':%s' "
				"while validating positional arguments for the %s %s",
				sieve_ast_argument_tag(arg), cmd->command->identifier, 
				sieve_command_type_name(cmd->command));
			return FALSE;
		}
		
		real_count++;
	 
		arg = sieve_ast_argument_next(arg);
	}
	
	/* Check the required count versus the real number of arguments */
	if ( arg_count >= 0 && real_count != arg_count ) {
		sieve_command_validate_error(validator, cmd, 
			"the %s %s requires %d positional argument(s), but %d is/are specified",
			cmd->command->identifier, sieve_command_type_name(cmd->command), 
			arg_count, real_count);
		return FALSE;
	}

	return TRUE;
}

static bool sieve_validate_arguments_context
(struct sieve_validator *validator, struct sieve_command_context *cmd)
{ 
	struct sieve_ast_argument *arg = 
		sieve_command_first_argument(cmd);
	
	while ( arg != NULL ) {
		const struct sieve_argument *argument = arg->argument;
		
		if ( argument != NULL && argument->validate_context != NULL ) { 
			if ( !argument->validate_context(validator, arg, cmd) ) 
				return FALSE;
		}
		
		arg = sieve_ast_argument_next(arg);
	}

	return TRUE;
}
 
/* Command Validation API */ 
                 
static bool sieve_validate_command_subtests
(struct sieve_validator *validator, struct sieve_command_context *cmd, 
	const unsigned int count) 
{
	switch ( count ) {
	
	case 0:
	 	if ( sieve_ast_test_count(cmd->ast_node) > 0 ) {
			sieve_command_validate_error
				( validator, cmd, "the %s %s accepts no sub-tests, but tests are specified anyway", 
					cmd->command->identifier, sieve_command_type_name(cmd->command) );
			return FALSE;
		}
		break;
	case 1:
		if ( sieve_ast_test_count(cmd->ast_node) == 0 ) {
			sieve_command_validate_error
				( validator, cmd, "the %s %s requires one sub-test, but none is specified", 
					cmd->command->identifier, sieve_command_type_name(cmd->command) );
			return FALSE;
		} else if ( sieve_ast_test_count(cmd->ast_node) > 1 || cmd->ast_node->test_list ) {
			sieve_command_validate_error
				( validator, cmd, "the %s %s requires one sub-test, but a list of tests is specified", 
					cmd->command->identifier, sieve_command_type_name(cmd->command) );
			return FALSE;
		}
		break;
		
	default:
		if ( sieve_ast_test_count(cmd->ast_node) == 0 ) {
			sieve_command_validate_error
				( validator, cmd, "the %s %s requires a list of sub-tests, but none is specified", 
					cmd->command->identifier, sieve_command_type_name(cmd->command) );
			return FALSE;
		} else if ( sieve_ast_test_count(cmd->ast_node) == 1 && !cmd->ast_node->test_list ) {
			sieve_command_validate_error
				( validator, cmd, "the %s %s requires a list of sub-tests, but a single test is specified", 
					cmd->command->identifier, sieve_command_type_name(cmd->command) );
			return FALSE;
		}
		break;		
	}

	return TRUE;
}

static bool sieve_validate_command_block
(struct sieve_validator *validator, struct sieve_command_context *cmd, 
	bool block_allowed, bool block_required) 
{
	i_assert( cmd->ast_node->type == SAT_COMMAND );
	
	if ( block_required ) {
		if ( !cmd->ast_node->block ) {
			sieve_command_validate_error
				( validator, cmd, 
					"the %s command requires a command block, but it is missing", 
					cmd->command->identifier );
			return FALSE;
		}
	} else if ( !block_allowed && cmd->ast_node->block ) {
		sieve_command_validate_error
				( validator, cmd, 
					"the %s command does not accept a command block, but one is specified anyway", 
					cmd->command->identifier );
		return FALSE;
	}
	
	return TRUE;
} 

/* AST Validation */

static bool sieve_validate_test_list
	(struct sieve_validator *validator, struct sieve_ast_node *test_list); 
static bool sieve_validate_block
	(struct sieve_validator *validator, struct sieve_ast_node *block);
static bool sieve_validate_command
	(struct sieve_validator *validator, struct sieve_ast_node *cmd_node);
	
static bool sieve_validate_command
	(struct sieve_validator *validator, struct sieve_ast_node *cmd_node) 
{
	enum sieve_ast_type ast_type = sieve_ast_node_type(cmd_node);
	bool result = TRUE;
	const struct sieve_command *command;
	
	i_assert( ast_type == SAT_TEST || ast_type == SAT_COMMAND );
	
	/* Verify the command specified by this node */
	
	command = sieve_validator_find_command(validator, cmd_node->identifier);
	
	if ( command != NULL ) {
		/* Identifier = "" when the command was previously marked as unknown */
		if ( *(command->identifier) != '\0' ) {
			if ( (command->type == SCT_COMMAND && ast_type == SAT_TEST) || 
				(command->type == SCT_TEST && ast_type == SAT_COMMAND) ) 
			{
				sieve_validator_error(
					validator, cmd_node, "attempted to use %s '%s' as %s", 
					sieve_command_type_name(command),	cmd_node->identifier,
					sieve_ast_type_name(ast_type));
			
			 	return FALSE;
			} 
			 
			struct sieve_command_context *ctx = 
				sieve_command_context_create(cmd_node, command); 
			cmd_node->context = ctx;
		
			/* If pre-validation fails, don't bother to validate further 
			 * as context might be missing and doing so is not very useful for 
			 * further error reporting anyway
			 */
			if ( command->pre_validate == NULL || 
				command->pre_validate(validator, ctx) ) {
		
				/* Check syntax */
				if ( 
					!sieve_validate_command_arguments(validator, ctx) ||
 					!sieve_validate_command_subtests
 						(validator, ctx, command->subtests) || 
 					(ast_type == SAT_COMMAND && !sieve_validate_command_block
 						(validator, ctx, command->block_allowed, 
 							command->block_required)) ) 
 				{
 					result = FALSE;
 				} else {
					/* Call command validation function if specified */
					if ( command->validate != NULL )
						result = command->validate(validator, ctx) && result;
				}
			} else
				result = FALSE;
				
			if ( result ) 
				result = sieve_validate_arguments_context(validator, ctx);
				
		} else 
			result = FALSE;
				
	} else {
		sieve_validator_error(
			validator, cmd_node, 
			"unknown %s '%s' (only reported once at first occurence)", 
			sieve_ast_type_name(ast_type), cmd_node->identifier);
			
		sieve_validator_register_unknown_command(validator, cmd_node->identifier);
		
		result = FALSE;
	}
	
	/* Descend further into the AST */
	
	result = sieve_validate_test_list(validator, cmd_node) && result;

	if ( ast_type == SAT_COMMAND )
		result = sieve_validate_block(validator, cmd_node) && result;
	
	return result;
}

static bool sieve_validate_test_list
	(struct sieve_validator *validator, struct sieve_ast_node *test_list) 
{
	bool result = TRUE;
	struct sieve_ast_node *test;

	test = sieve_ast_test_first(test_list);
	while ( test != NULL ) {	
		result = sieve_validate_command(validator, test) && result;	
		test = sieve_ast_test_next(test);
	}		
	
	return result;
}

static bool sieve_validate_block(struct sieve_validator *validator, struct sieve_ast_node *block) 
{
	bool result = TRUE;
	struct sieve_ast_node *command;

	T_FRAME(	
		command = sieve_ast_command_first(block);
		while ( command != NULL ) {	
			result = sieve_validate_command(validator, command) && result;	
			command = sieve_ast_command_next(command);
		}		
	);
	
	return result;
}

bool sieve_validator_run(struct sieve_validator *validator) {	
	return sieve_validate_block(validator, sieve_ast_root(validator->ast));
}



