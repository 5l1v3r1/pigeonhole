/* Copyright (c) 2002-2010 Dovecot Sieve authors, see the included COPYING file
 */

#include "lib.h"
#include "settings-parser.h"
#include "service-settings.h"
#include "login-settings.h"
#include "managesieve-login-settings.h"

#include <stddef.h>

struct service_settings managesieve_login_settings_service_settings = {
	.name = "managesieve-login",
	.protocol = "managesieve",
	.type = "login",
	.executable = "managesieve-login",
	.user = "dovecot",
	.group = "",
	.privileged_group = "",
	.extra_groups = "",
	.chroot = "login",

	.drop_priv_before_exec = FALSE,

	.process_min_avail = 0,
	.process_limit = 0,
	.client_limit = 0,
	.service_count = 1,
	.vsz_limit = 64,

	.unix_listeners = ARRAY_INIT,
	.fifo_listeners = ARRAY_INIT,
	.inet_listeners = ARRAY_INIT
};

#undef DEF
#define DEF(type, name) \
	{ type, #name, offsetof(struct managesieve_login_settings, name), NULL }

static const struct setting_define managesieve_login_setting_defines[] = {
	DEF(SET_STR, managesieve_implementation_string),
	DEF(SET_STR, managesieve_sieve_capability),

	SETTING_DEFINE_LIST_END
};

static const struct managesieve_login_settings managesieve_login_default_settings = {
	.managesieve_implementation_string = PACKAGE_NAME,
	.managesieve_sieve_capability = ""
};

static const struct setting_parser_info *managesieve_login_setting_dependencies[] = {
	&login_setting_parser_info,
	NULL
};

static const struct setting_parser_info managesieve_login_setting_parser_info = {
	.module_name = "managesieve-login",
	.defines = managesieve_login_setting_defines,
	.defaults = &managesieve_login_default_settings,
	
	.type_offset = (size_t)-1,
	.struct_size = sizeof(struct managesieve_login_settings),

	.parent_offset = (size_t)-1,
	.parent = NULL,

	.check_func = NULL,
	.dependencies = managesieve_login_setting_dependencies
};

const struct setting_parser_info *managesieve_login_settings_set_roots[] = {
	&login_setting_parser_info,
	&managesieve_login_setting_parser_info,
	NULL
};
