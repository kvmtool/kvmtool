#ifndef __PARSE_OPTIONS_H__
#define __PARSE_OPTIONS_H__

#include <inttypes.h>
#include <kvm/util.h>

enum parse_opt_type {
	/* special types */
	OPTION_END,
	OPTION_ARGUMENT,
	OPTION_GROUP,
	/* options with no arguments */
	OPTION_BIT,
	OPTION_BOOLEAN,
	OPTION_INCR,
	OPTION_SET_UINT,
	OPTION_SET_PTR,
	/* options with arguments (usually) */
	OPTION_STRING,
	OPTION_INTEGER,
	OPTION_LONG,
	OPTION_CALLBACK,
	OPTION_U64,
	OPTION_UINTEGER,
};

enum parse_opt_flags {
	PARSE_OPT_KEEP_DASHDASH = 1,
	PARSE_OPT_STOP_AT_NON_OPTION = 2,
	PARSE_OPT_KEEP_ARGV0 = 4,
	PARSE_OPT_KEEP_UNKNOWN = 8,
	PARSE_OPT_NO_INTERNAL_HELP = 16,
};

enum parse_opt_option_flags {
	PARSE_OPT_OPTARG  = 1,
	PARSE_OPT_NOARG   = 2,
	PARSE_OPT_NONEG   = 4,
	PARSE_OPT_HIDDEN  = 8,
	PARSE_OPT_LASTARG_DEFAULT = 16,
};

struct option;
typedef int parse_opt_cb(const struct option *, const char *arg, int unset);
/*
 * `type`::
 *   holds the type of the option, you must have an OPTION_END last in your
 *   array.
 *
 * `short_name`::
 *   the character to use as a short option name, '\0' if none.
 *
 * `long_name`::
 *   the long option name, without the leading dashes, NULL if none.
 *
 * `value`::
 *   stores pointers to the values to be filled.
 *
 * `argh`::
 *   token to explain the kind of argument this option wants. Keep it
 *   homogenous across the repository.
 *
 * `help`::
 *   the short help associated to what the option does.
 *   Must never be NULL (except for OPTION_END).
 *   OPTION_GROUP uses this pointer to store the group header.
 *
 * `flags`::
 *   mask of parse_opt_option_flags.
 *   PARSE_OPT_OPTARG: says that the argument is optionnal (not for BOOLEANs)
 *   PARSE_OPT_NOARG: says that this option takes no argument, for CALLBACKs
 *   PARSE_OPT_NONEG: says that this option cannot be negated
 *   PARSE_OPT_HIDDEN this option is skipped in the default usage, showed in
 *                    the long one.
 *
 * `callback`::
 *   pointer to the callback to use for OPTION_CALLBACK.
 *
 * `defval`::
 *   default value to fill (*->value) with for PARSE_OPT_OPTARG.
 *   OPTION_{BIT,SET_UINT,SET_PTR} store the {mask,integer,pointer} to put in
 *   the value when met.
 *   CALLBACKS can use it like they want.
 */
struct option {
	enum parse_opt_type type;
	int short_name;
	const char *long_name;
	void *value;
	const char *argh;
	const char *help;
	void *ptr;

	int flags;
	parse_opt_cb *callback;
	intptr_t defval;
};

#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int:-!!(e); }))
#define check_vtype(v, type) \
	(BUILD_BUG_ON_ZERO(!__builtin_types_compatible_p(typeof(v), type)) + v)

#define OPT_INTEGER(s, l, v, h)             \
{                                           \
	.type = OPTION_INTEGER,             \
	.short_name = (s),                  \
	.long_name = (l),                   \
	.value = check_vtype(v, int *),     \
	.help = (h)                         \
}

#define OPT_U64(s, l, v, h)                 \
{                                           \
	.type = OPTION_U64,                 \
	.short_name = (s),                  \
	.long_name = (l),                   \
	.value = check_vtype(v, u64 *),     \
	.help = (h)                         \
}

#define OPT_STRING(s, l, v, a, h)           \
{                                           \
	.type = OPTION_STRING,              \
	.short_name = (s),                  \
	.long_name = (l),                   \
	.value = check_vtype(v, const char **), (a), \
	.help = (h)                         \
}

#define OPT_BOOLEAN(s, l, v, h)             \
{                                           \
	.type = OPTION_BOOLEAN,             \
	.short_name = (s),                  \
	.long_name = (l),                   \
	.value = check_vtype(v, bool *),    \
	.help = (h)                         \
}

#define OPT_INCR(s, l, v, h)                \
{                                           \
	.type = OPTION_INCR,	            \
	.short_name = (s),                  \
	.long_name = (l),                   \
	.value = check_vtype(v, int *),     \
	.help = (h)                         \
}

#define OPT_GROUP(h)                        \
{                                           \
	.type = OPTION_GROUP,               \
	.help = (h)                         \
}

#define OPT_CALLBACK(s, l, v, a, h, f, p)   \
{					    \
	.type = OPTION_CALLBACK,	    \
	.short_name = (s),		    \
	.long_name = (l),		    \
	.value = (v),			    \
	(a),				    \
	.help = (h),			    \
	.callback = (f),		    \
	.ptr = (p),			    \
}

#define OPT_CALLBACK_NOOPT(s, l, v, a, h, f, p) \
{					    \
	.type = OPTION_CALLBACK,	    \
	.short_name = (s),		    \
	.long_name = (l),		    \
	.value = (v),			    \
	(a),				    \
	.help = (h),			    \
	.callback = (f),		    \
	.flags = PARSE_OPT_NOARG,	    \
	.ptr = (p),			    \
}

#define OPT_CALLBACK_DEFAULT(s, l, v, a, h, f, d, p) \
{					    \
	.type = OPTION_CALLBACK,	    \
	.short_name = (s),		    \
	.long_name = (l),		    \
	.value = (v), (a),		    \
	.help = (h),			    \
	.callback = (f),		    \
	.defval = (intptr_t)d,		    \
	.flags = PARSE_OPT_LASTARG_DEFAULT, \
	.ptr = (p)			    \
}

#define OPT_END() { .type = OPTION_END }

enum {
	PARSE_OPT_HELP = -1,
	PARSE_OPT_DONE,
	PARSE_OPT_UNKNOWN,
};

/*
 * It's okay for the caller to consume argv/argc in the usual way.
 * Other fields of that structure are private to parse-options and should not
 * be modified in any way.
 **/
struct parse_opt_ctx_t {
	const char **argv;
	const char **out;
	int argc, cpidx;
	const char *opt;
	int flags;
};

/* global functions */
void usage_with_options(const char * const *usagestr,
		const struct option *opts) NORETURN;
int parse_options(int argc, const char **argv, const struct option *options,
		const char * const usagestr[], int flags);
#endif
