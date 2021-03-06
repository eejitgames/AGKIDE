/*
*
*   Copyright (c) 2001-2002, Biswapesh Chattopadhyay
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License.
*
*/

#include <stdlib.h>
#include <string.h>
#include <glib-object.h>

#include "general.h"
#include "entry.h"
#include "parse.h"
#include "read.h"
#define LIBCTAGS_DEFINED
#include "tm_tag.h"
#include "tm_parser.h"


#define TAG_NEW(T)	((T) = g_slice_new0(TMTag))
#define TAG_FREE(T)	g_slice_free(TMTag, (T))


#ifdef DEBUG_TAG_REFS

static GHashTable *alive_tags = NULL;

static void foreach_tags_log(gpointer key, gpointer value, gpointer data)
{
	gsize *ref_count = data;
	const TMTag *tag = value;

	*ref_count += (gsize) tag->refcount;
	g_debug("Leaked TMTag (%d refs): %s", tag->refcount, tag->name);
}

static void log_refs_at_exit(void)
{
	gsize ref_count = 0;

	g_hash_table_foreach(alive_tags, foreach_tags_log, &ref_count);
	g_debug("TMTag references left at exit: %lu", ref_count);
}

static TMTag *log_tag_new(void)
{
	TMTag *tag;

	if (! alive_tags)
	{
		alive_tags = g_hash_table_new(g_direct_hash, g_direct_equal);
		atexit(log_refs_at_exit);
	}
	TAG_NEW(tag);
	g_hash_table_insert(alive_tags, tag, tag);

	return tag;
}

static void log_tag_free(TMTag *tag)
{
	g_return_if_fail(alive_tags != NULL);

	if (! g_hash_table_remove(alive_tags, tag)) {
		g_critical("Freeing invalid TMTag pointer %p", (void *) tag);
	} else {
		TAG_FREE(tag);
	}
}

#undef TAG_NEW
#undef TAG_FREE
#define TAG_NEW(T)	((T) = log_tag_new())
#define TAG_FREE(T)	log_tag_free(T)

#endif /* DEBUG_TAG_REFS */


/* Note: To preserve binary compatibility, it is very important
	that you only *append* to this list ! */
enum
{
	TA_NAME = 200,
	TA_LINE,
	TA_LOCAL,
	TA_POS, /* Obsolete */
	TA_TYPE,
	TA_ARGLIST,
	TA_SCOPE,
	TA_VARTYPE,
	TA_INHERITS,
	TA_TIME,
	TA_ACCESS,
	TA_IMPL,
	TA_LANG,
	TA_INACTIVE,
	TA_POINTER
};

static guint *s_sort_attrs = NULL;
static gboolean s_partial = FALSE;

static const char *s_tag_type_names[] = {
	"class", /* classes */
	"enum", /* enumeration names */
	"enumerator", /* enumerators (values inside an enumeration) */
	"externvar", /* external variable declarations */
	"field", /* fields */
	"function", /*  function definitions */
	"interface", /* interfaces */
	"macro", /* macro definitions */
	"member", /* class, struct, and union members */
	"method", /* methods */
	"namespace", /* namespaces */
	"package", /* packages */
	"prototype", /* function prototypes */
	"struct", /* structure names */
	"typedef", /* typedefs */
	"union", /* union names */
	"variable", /* variable definitions */
	"other" /* Other tag type (non C/C++/Java) */
};

static int s_tag_types[] = {
	tm_tag_class_t,
	tm_tag_enum_t,
	tm_tag_enumerator_t,
	tm_tag_externvar_t,
	tm_tag_field_t,
	tm_tag_function_t,
	tm_tag_interface_t,
	tm_tag_macro_t,
	tm_tag_member_t,
	tm_tag_method_t,
	tm_tag_namespace_t,
	tm_tag_package_t,
	tm_tag_prototype_t,
	tm_tag_struct_t,
	tm_tag_typedef_t,
	tm_tag_union_t,
	tm_tag_variable_t,
	tm_tag_other_t
};

GType tm_tag_get_type(void)
{
	static GType gtype = 0;
	if (G_UNLIKELY (gtype == 0))
	{
		gtype = g_boxed_type_register_static("TMTag", (GBoxedCopyFunc)tm_tag_ref,
											 (GBoxedFreeFunc)tm_tag_unref);
	}
	return gtype;
}

static int get_tag_type(const char *tag_name)
{
	unsigned int i;
	int cmp;
	g_return_val_if_fail(tag_name, 0);
	for (i=0; i < sizeof(s_tag_type_names)/sizeof(char *); ++i)
	{
		cmp = strcmp(tag_name, s_tag_type_names[i]);
		if (0 == cmp)
			return s_tag_types[i];
		else if (cmp < 0)
			break;
	}
	/* other is not checked above as it is last, not sorted alphabetically */
	if (strcmp(tag_name, "other") == 0)
		return tm_tag_other_t;
#ifdef TM_DEBUG
	fprintf(stderr, "Unknown tag type %s\n", tag_name);
#endif
	return tm_tag_undef_t;
}

static char get_tag_impl(const char *impl)
{
	if ((0 == strcmp("virtual", impl))
	 || (0 == strcmp("pure virtual", impl)))
		return TAG_IMPL_VIRTUAL;

#ifdef TM_DEBUG
		g_warning("Unknown implementation %s", impl);
#endif
	return TAG_IMPL_UNKNOWN;
}

static char get_tag_access(const char *access)
{
	if (0 == strcmp("public", access))
		return TAG_ACCESS_PUBLIC;
	else if (0 == strcmp("protected", access))
		return TAG_ACCESS_PROTECTED;
	else if (0 == strcmp("private", access))
		return TAG_ACCESS_PRIVATE;
	else if (0 == strcmp("friend", access))
		return TAG_ACCESS_FRIEND;
	else if (0 == strcmp("default", access))
		return TAG_ACCESS_DEFAULT;

#ifdef TM_DEBUG
	g_warning("Unknown access type %s", access);
#endif
	return TAG_ACCESS_UNKNOWN;
}

gboolean tm_tag_init(TMTag *tag, TMSourceFile *file, const tagEntryInfo *tag_entry)
{
	tag->refcount = 1;
	if (NULL == tag_entry)
	{
		/* This is a file tag */
		if (NULL == file)
			return FALSE;
		else
		{
			tag->name = g_strdup(file->work_object.file_name);
			tag->type = tm_tag_file_t;
			/* tag->atts.file.timestamp = file->work_object.analyze_time; */
			tag->atts.file.lang = file->lang;
			tag->atts.file.inactive = FALSE;
			return TRUE;
		}
	}
	else
	{
		/* This is a normal tag entry */
		if (NULL == tag_entry->name)
			return FALSE;
		tag->name = g_strdup(tag_entry->name);
		tag->type = get_tag_type(tag_entry->kindName);
		tag->atts.entry.local = tag_entry->isFileScope;
		tag->atts.entry.pointerOrder = 0;	/* backward compatibility (use var_type instead) */
		tag->atts.entry.line = tag_entry->lineNumber;
		if (NULL != tag_entry->extensionFields.arglist)
			tag->atts.entry.arglist = g_strdup(tag_entry->extensionFields.arglist);
		if ((NULL != tag_entry->extensionFields.scope[1]) &&
			(isalpha(tag_entry->extensionFields.scope[1][0]) ||
			 tag_entry->extensionFields.scope[1][0] == '_' ||
			 tag_entry->extensionFields.scope[1][0] == '$'))
			tag->atts.entry.scope = g_strdup(tag_entry->extensionFields.scope[1]);
		if (tag_entry->extensionFields.inheritance != NULL)
			tag->atts.entry.inheritance = g_strdup(tag_entry->extensionFields.inheritance);
		if (tag_entry->extensionFields.varType != NULL)
			tag->atts.entry.var_type = g_strdup(tag_entry->extensionFields.varType);
		if (tag_entry->extensionFields.access != NULL)
			tag->atts.entry.access = get_tag_access(tag_entry->extensionFields.access);
		if (tag_entry->extensionFields.implementation != NULL)
			tag->atts.entry.impl = get_tag_impl(tag_entry->extensionFields.implementation);
		if ((tm_tag_macro_t == tag->type) && (NULL != tag->atts.entry.arglist))
			tag->type = tm_tag_macro_with_arg_t;
		tag->atts.entry.file = file;
		return TRUE;
	}
}

TMTag *tm_tag_new(TMSourceFile *file, const tagEntryInfo *tag_entry)
{
	TMTag *tag;

	TAG_NEW(tag);
	if (FALSE == tm_tag_init(tag, file, tag_entry))
	{
		TAG_FREE(tag);
		return NULL;
	}
	return tag;
}

gboolean tm_tag_init_from_file(TMTag *tag, TMSourceFile *file, FILE *fp)
{
	guchar buf[BUFSIZ];
	guchar *start, *end;
	gboolean status;
	guchar changed_char = TA_NAME;

	tag->refcount = 1;
	if ((NULL == fgets((gchar*)buf, BUFSIZ, fp)) || ('\0' == *buf))
		return FALSE;
	for (start = end = buf, status = TRUE; (TRUE == status); start = end, ++ end)
	{
		while ((*end < TA_NAME) && (*end != '\0') && (*end != '\n'))
			++ end;
		if (('\0' == *end) || ('\n' == *end))
			status = FALSE;
		changed_char = *end;
		*end = '\0';
		if (NULL == tag->name)
		{
			if (!isprint(*start))
				return FALSE;
			else
				tag->name = g_strdup((gchar*)start);
		}
		else
		{
			switch (*start)
			{
				case TA_LINE:
					tag->atts.entry.line = atol((gchar*)start + 1);
					break;
				case TA_LOCAL:
					tag->atts.entry.local = atoi((gchar*)start + 1);
					break;
				case TA_TYPE:
					tag->type = (TMTagType) atoi((gchar*)start + 1);
					break;
				case TA_ARGLIST:
					tag->atts.entry.arglist = g_strdup((gchar*)start + 1);
					break;
				case TA_SCOPE:
					tag->atts.entry.scope = g_strdup((gchar*)start + 1);
					break;
				case TA_POINTER:
					tag->atts.entry.pointerOrder = atoi((gchar*)start + 1);
					break;
				case TA_VARTYPE:
					tag->atts.entry.var_type = g_strdup((gchar*)start + 1);
					break;
				case TA_INHERITS:
					tag->atts.entry.inheritance = g_strdup((gchar*)start + 1);
					break;
				case TA_TIME:
					if (tm_tag_file_t != tag->type)
					{
						g_warning("Got time attribute for non-file tag %s", tag->name);
						return FALSE;
					}
					else
						tag->atts.file.timestamp = atol((gchar*)start + 1);
					break;
				case TA_LANG:
					if (tm_tag_file_t != tag->type)
					{
						g_warning("Got lang attribute for non-file tag %s", tag->name);
						return FALSE;
					}
					else
						tag->atts.file.lang = atoi((gchar*)start + 1);
					break;
				case TA_INACTIVE:
					if (tm_tag_file_t != tag->type)
					{
						g_warning("Got inactive attribute for non-file tag %s", tag->name);
						return FALSE;
					}
					else
						tag->atts.file.inactive = (gboolean) atoi((gchar*)start + 1);
					break;
				case TA_ACCESS:
					tag->atts.entry.access = *(start + 1);
					break;
				case TA_IMPL:
					tag->atts.entry.impl = *(start + 1);
					break;
				default:
#ifdef GEANY_DEBUG
					g_warning("Unknown attribute %s", start + 1);
#endif
					break;
			}
		}
		*end = changed_char;
	}
	if (NULL == tag->name)
		return FALSE;
	if (tm_tag_file_t != tag->type)
		tag->atts.entry.file = file;
	return TRUE;
}

/* alternative parser for Pascal and LaTeX global tags files with the following format
 * tagname|return value|arglist|description\n */
gboolean tm_tag_init_from_file_alt(TMTag *tag, TMSourceFile *file, FILE *fp)
{
	guchar buf[BUFSIZ];
	guchar *start, *end;
	gboolean status;
	/*guchar changed_char = TA_NAME;*/

	tag->refcount = 1;
	if ((NULL == fgets((gchar*)buf, BUFSIZ, fp)) || ('\0' == *buf))
		return FALSE;
	{
		gchar **fields;
		guint field_len;
		for (start = end = buf, status = TRUE; (TRUE == status); start = end, ++ end)
		{
			while ((*end < TA_NAME) && (*end != '\0') && (*end != '\n'))
				++ end;
			if (('\0' == *end) || ('\n' == *end))
				status = FALSE;
			/*changed_char = *end;*/
			*end = '\0';
			if (NULL == tag->name && !isprint(*start))
					return FALSE;

			fields = g_strsplit((gchar*)start, "|", -1);
			field_len = g_strv_length(fields);

			if (field_len >= 1) tag->name = g_strdup(fields[0]);
			else tag->name = NULL;
			if (field_len >= 2 && fields[1] != NULL) tag->atts.entry.var_type = g_strdup(fields[1]);
			if (field_len >= 3 && fields[2] != NULL) tag->atts.entry.arglist = g_strdup(fields[2]);
			tag->type = tm_tag_prototype_t;
			g_strfreev(fields);
		}
	}

	if (NULL == tag->name)
		return FALSE;
	if (tm_tag_file_t != tag->type)
		tag->atts.entry.file = file;
	return TRUE;
}

/* Reads ctags format (http://ctags.sourceforge.net/FORMAT) */
gboolean tm_tag_init_from_file_ctags(TMTag *tag, TMSourceFile *file, FILE *fp)
{
	gchar buf[BUFSIZ];
	gchar *p, *tab;

	tag->refcount = 1;
	tag->type = tm_tag_function_t; /* default type is function if no kind is specified */
	do
	{
		if ((NULL == fgets(buf, BUFSIZ, fp)) || ('\0' == *buf))
			return FALSE;
	}
	while (strncmp(buf, "!_TAG_", 6) == 0); /* skip !_TAG_ lines */

	p = buf;

	/* tag name */
	if (! (tab = strchr(p, '\t')) || p == tab)
		return FALSE;
	tag->name = g_strndup(p, (gsize)(tab - p));
	p = tab + 1;

	/* tagfile, unused */
	if (! (tab = strchr(p, '\t')))
	{
		g_free(tag->name);
		tag->name = NULL;
		return FALSE;
	}
	p = tab + 1;
	/* Ex command, unused */
	if (*p == '/' || *p == '?')
	{
		gchar c = *p;
		for (++p; *p && *p != c; p++)
		{
			if (*p == '\\' && p[1])
				p++;
		}
	}
	else /* assume a line */
		tag->atts.entry.line = atol(p);
	tab = strstr(p, ";\"");
	/* read extension fields */
	if (tab)
	{
		p = tab + 2;
		while (*p && *p != '\n' && *p != '\r')
		{
			gchar *end;
			const gchar *key, *value = NULL;

			/* skip leading tabulations */
			while (*p && *p == '\t') p++;
			/* find the separator (:) and end (\t) */
			key = end = p;
			while (*end && *end != '\t' && *end != '\n' && *end != '\r')
			{
				if (*end == ':' && ! value)
				{
					*end = 0; /* terminate the key */
					value = end + 1;
				}
				end++;
			}
			/* move p paste the so we won't stop parsing by setting *end=0 below */
			p = *end ? end + 1 : end;
			*end = 0; /* terminate the value (or key if no value) */

			if (! value || 0 == strcmp(key, "kind")) /* tag kind */
			{
				const gchar *kind = value ? value : key;

				if (kind[0] && kind[1])
					tag->type = get_tag_type(kind);
				else
				{
					switch (*kind)
					{
						case 'c': tag->type = tm_tag_class_t; break;
						case 'd': tag->type = tm_tag_macro_t; break;
						case 'e': tag->type = tm_tag_enumerator_t; break;
						case 'F': tag->type = tm_tag_file_t; break;
						case 'f': tag->type = tm_tag_function_t; break;
						case 'g': tag->type = tm_tag_enum_t; break;
						case 'I': tag->type = tm_tag_class_t; break;
						case 'i': tag->type = tm_tag_interface_t; break;
						case 'l': tag->type = tm_tag_variable_t; break;
						case 'M': tag->type = tm_tag_macro_t; break;
						case 'm': tag->type = tm_tag_member_t; break;
						case 'n': tag->type = tm_tag_namespace_t; break;
						case 'P': tag->type = tm_tag_package_t; break;
						case 'p': tag->type = tm_tag_prototype_t; break;
						case 's': tag->type = tm_tag_struct_t; break;
						case 't': tag->type = tm_tag_typedef_t; break;
						case 'u': tag->type = tm_tag_union_t; break;
						case 'v': tag->type = tm_tag_variable_t; break;
						case 'x': tag->type = tm_tag_externvar_t; break;
						default:
#ifdef TM_DEBUG
							g_warning("Unknown tag kind %c", *kind);
#endif
							tag->type = tm_tag_other_t; break;
					}
				}
			}
			else if (0 == strcmp(key, "inherits")) /* comma-separated list of classes this class inherits from */
			{
				g_free(tag->atts.entry.inheritance);
				tag->atts.entry.inheritance = g_strdup(value);
			}
			else if (0 == strcmp(key, "implementation")) /* implementation limit */
				tag->atts.entry.impl = get_tag_impl(value);
			else if (0 == strcmp(key, "line")) /* line */
				tag->atts.entry.line = atol(value);
			else if (0 == strcmp(key, "access")) /* access */
				tag->atts.entry.access = get_tag_access(value);
			else if (0 == strcmp(key, "class") ||
					 0 == strcmp(key, "enum") ||
					 0 == strcmp(key, "function") ||
					 0 == strcmp(key, "struct") ||
					 0 == strcmp(key, "union")) /* Name of the class/enum/function/struct/union in which this tag is a member */
			{
				g_free(tag->atts.entry.scope);
				tag->atts.entry.scope = g_strdup(value);
			}
			else if (0 == strcmp(key, "file")) /* static (local) tag */
				tag->atts.entry.local = TRUE;
			else if (0 == strcmp(key, "signature")) /* arglist */
			{
				g_free(tag->atts.entry.arglist);
				tag->atts.entry.arglist = g_strdup(value);
			}
		}
	}

	if (tm_tag_file_t != tag->type)
		tag->atts.entry.file = file;
	return TRUE;
}

TMTag *tm_tag_new_from_file(TMSourceFile *file, FILE *fp, gint mode, TMFileFormat format)
{
	TMTag *tag;
	gboolean result = FALSE;

	TAG_NEW(tag);

	switch (format)
	{
		case TM_FILE_FORMAT_TAGMANAGER:
			result = tm_tag_init_from_file(tag, file, fp);
			break;
		case TM_FILE_FORMAT_PIPE:
			result = tm_tag_init_from_file_alt(tag, file, fp);
			break;
		case TM_FILE_FORMAT_CTAGS:
			result = tm_tag_init_from_file_ctags(tag, file, fp);
			break;
	}

	if (! result)
	{
		TAG_FREE(tag);
		return NULL;
	}
	tag->atts.file.lang = mode;
	return tag;
}

gboolean tm_tag_write(TMTag *tag, FILE *fp, guint attrs)
{
	fprintf(fp, "%s", tag->name);
	if (attrs & tm_tag_attr_type_t)
		fprintf(fp, "%c%d", TA_TYPE, tag->type);
	if (tag->type == tm_tag_file_t)
	{
		if (attrs & tm_tag_attr_time_t)
			fprintf(fp, "%c%ld", TA_TIME, tag->atts.file.timestamp);
		if (attrs & tm_tag_attr_lang_t)
			fprintf(fp, "%c%d", TA_LANG, tag->atts.file.lang);
		if ((attrs & tm_tag_attr_inactive_t) && tag->atts.file.inactive)
			fprintf(fp, "%c%d", TA_INACTIVE, tag->atts.file.inactive);
	}
	else
	{
		if ((attrs & tm_tag_attr_arglist_t) && (NULL != tag->atts.entry.arglist))
			fprintf(fp, "%c%s", TA_ARGLIST, tag->atts.entry.arglist);
		if (attrs & tm_tag_attr_line_t)
			fprintf(fp, "%c%ld", TA_LINE, tag->atts.entry.line);
		if (attrs & tm_tag_attr_local_t)
			fprintf(fp, "%c%d", TA_LOCAL, tag->atts.entry.local);
		if ((attrs & tm_tag_attr_scope_t) && (NULL != tag->atts.entry.scope))
			fprintf(fp, "%c%s", TA_SCOPE, tag->atts.entry.scope);
		if ((attrs & tm_tag_attr_inheritance_t) && (NULL != tag->atts.entry.inheritance))
			fprintf(fp, "%c%s", TA_INHERITS, tag->atts.entry.inheritance);
		if (attrs & tm_tag_attr_pointer_t)
			fprintf(fp, "%c%d", TA_POINTER, tag->atts.entry.pointerOrder);
		if ((attrs & tm_tag_attr_vartype_t) && (NULL != tag->atts.entry.var_type))
			fprintf(fp, "%c%s", TA_VARTYPE, tag->atts.entry.var_type);
		if ((attrs & tm_tag_attr_access_t) && (TAG_ACCESS_UNKNOWN != tag->atts.entry.access))
			fprintf(fp, "%c%c", TA_ACCESS, tag->atts.entry.access);
		if ((attrs & tm_tag_attr_impl_t) && (TAG_IMPL_UNKNOWN != tag->atts.entry.impl))
			fprintf(fp, "%c%c", TA_IMPL, tag->atts.entry.impl);
	}
	if (fprintf(fp, "\n"))
		return TRUE;
	else
		return FALSE;
}

static void tm_tag_destroy(TMTag *tag)
{
	g_free(tag->name);
	if (tm_tag_file_t != tag->type)
	{
		g_free(tag->atts.entry.arglist);
		g_free(tag->atts.entry.scope);
		g_free(tag->atts.entry.inheritance);
		g_free(tag->atts.entry.var_type);
	}
}

#if 0
void tm_tag_free(gpointer tag)
{
	tm_tag_unref(tag);
}
#endif

void tm_tag_unref(TMTag *tag)
{
	/* be NULL-proof because tm_tag_free() was NULL-proof and we indent to be a
	 * drop-in replacment of it */
	if (NULL != tag && g_atomic_int_dec_and_test(&tag->refcount))
	{
		tm_tag_destroy(tag);
		TAG_FREE(tag);
	}
}

TMTag *tm_tag_ref(TMTag *tag)
{
	g_atomic_int_inc(&tag->refcount);
	return tag;
}

int tm_str_cmp_case( const char* s1, const char* s2 )
{
	while( *s1 && *s2 )
	{
		char l1 = *s1;
		char l2 = *s2;
		if ( l1 >= 'A' && l1 <= 'Z' ) l1 = l1 - 'A' + 'a';
		if ( l2 >= 'A' && l2 <= 'Z' ) l2 = l2 - 'A' + 'a';

		if ( l1 < l2 ) return -1;
		else if ( l1 > l2 ) return 1;

		s1++;
		s2++;
	}

	char l1 = *s1;
	char l2 = *s2;
	if ( l1 >= 'A' && l1 <= 'Z' ) l1 = l1 - 'A' + 'a';
	if ( l2 >= 'A' && l2 <= 'Z' ) l2 = l2 - 'A' + 'a';

	if ( l1 < l2 ) return -1;
	else if ( l1 > l2 ) return 1;
	else return 0;
}

int tm_str_ncmp_case( const char* s1, const char* s2, int len )
{
	int count = 0;
	while( *s1 && *s2 && count < len )
	{
		char l1 = *s1;
		char l2 = *s2;
		if ( l1 >= 'A' && l1 <= 'Z' ) l1 = l1 - 'A' + 'a';
		if ( l2 >= 'A' && l2 <= 'Z' ) l2 = l2 - 'A' + 'a';

		if ( l1 < l2 ) return -1;
		else if ( l1 > l2 ) return 1;

		s1++;
		s2++;
		count++;
	}

	if ( count >= len ) return 0;

	char l1 = *s1;
	char l2 = *s2;
	if ( l1 >= 'A' && l1 <= 'Z' ) l1 = l1 - 'A' + 'a';
	if ( l2 >= 'A' && l2 <= 'Z' ) l2 = l2 - 'A' + 'a';

	if ( l1 < l2 ) return -1;
	else if ( l1 > l2 ) return 1;
	else return 0;
}

int tm_tag_compare(const void *ptr1, const void *ptr2)
{
	unsigned int *sort_attr;
	int returnval = 0;
	TMTag *t1 = *((TMTag **) ptr1);
	TMTag *t2 = *((TMTag **) ptr2);

	if ((NULL == t1) || (NULL == t2))
	{
		g_warning("Found NULL tag");
		return t2 - t1;
	}

	int ignoreCase = 1;
	
	char* s1 = g_strdup(t1->name ? t1->name : "");
	char* s2 = g_strdup(t2->name ? t2->name : "");

	// ignore any array size appended to the name
	char* bracket = strchr(s1,'[');
	if ( bracket ) 
	{
		*bracket = 0;
		bracket--;
		while( isspace(*bracket) && bracket > s1 )
		{
			*bracket = 0;
			bracket--;
		}
	}

	bracket = strchr(s2,'[');
	if ( bracket ) 
	{
		*bracket = 0;
		bracket--;
		while( isspace(*bracket) && bracket > s2 )
		{
			*bracket = 0;
			bracket--;
		}
	}

	if ( ignoreCase )
	{
		toLowerString(s1);
		toLowerString(s2);
	}

	if (NULL == s_sort_attrs)
	{
		if (s_partial)
			returnval = strncmp(s1, s2, strlen(s1));
		else
			returnval = strcmp(s1,s2);

		goto cleanup_and_return;
	}

	for (sort_attr = s_sort_attrs; *sort_attr != tm_tag_attr_none_t; ++ sort_attr)
	{
		switch (*sort_attr)
		{
			case tm_tag_attr_name_t:
				if (s_partial)
					returnval = strncmp(s1, s2, strlen(s1));
				else
					returnval = strcmp(s1,s2);
				
				if (0 != returnval) 
					goto cleanup_and_return;
				break;
			case tm_tag_attr_type_t:
				if (0 != (returnval = (t1->type - t2->type)))
					goto cleanup_and_return;
				break;
			case tm_tag_attr_file_t:
				if (0 != (returnval = (t1->atts.entry.file - t2->atts.entry.file)))
					goto cleanup_and_return;
				break;
			case tm_tag_attr_scope_t:
				if (0 != (returnval = strcmp(FALLBACK(t1->atts.entry.scope, ""), FALLBACK(t2->atts.entry.scope, ""))))
					goto cleanup_and_return;
				break;
			case tm_tag_attr_arglist_t:
				if (0 != (returnval = strcmp(FALLBACK(t1->atts.entry.arglist, ""), FALLBACK(t2->atts.entry.arglist, ""))))
				{
					int line_diff = (t1->atts.entry.line - t2->atts.entry.line);

					g_free(s1);
					g_free(s2);
					return line_diff ? line_diff : returnval;
				}
				break;
			case tm_tag_attr_vartype_t:
				if (0 != (returnval = strcmp(FALLBACK(t1->atts.entry.var_type, ""), FALLBACK(t2->atts.entry.var_type, ""))))
					goto cleanup_and_return;
				break;
			case tm_tag_attr_line_t:
				if (0 != (returnval = (t1->atts.entry.line - t2->atts.entry.line)))
					goto cleanup_and_return;
				break;
		}
	}

cleanup_and_return:
	g_free(s1);
	g_free(s2);
	return returnval;
}

gboolean tm_tags_prune(GPtrArray *tags_array)
{
	guint i, count;
	for (i=0, count = 0; i < tags_array->len; ++i)
	{
		if (NULL != tags_array->pdata[i])
			tags_array->pdata[count++] = tags_array->pdata[i];
	}
	tags_array->len = count;
	return TRUE;
}

gboolean tm_tags_dedup(GPtrArray *tags_array, TMTagAttrType *sort_attributes)
{
	guint i;

	if ((!tags_array) || (!tags_array->len))
		return TRUE;
	s_sort_attrs = sort_attributes;
	s_partial = FALSE;
	for (i = 1; i < tags_array->len; ++i)
	{
		if (0 == tm_tag_compare(&(tags_array->pdata[i - 1]), &(tags_array->pdata[i])))
		{
			tags_array->pdata[i-1] = NULL;
		}
	}
	tm_tags_prune(tags_array);
	return TRUE;
}

gboolean tm_tags_custom_dedup(GPtrArray *tags_array, TMTagCompareFunc compare_func)
{
	guint i;

	if ((!tags_array) || (!tags_array->len))
		return TRUE;
	for (i = 1; i < tags_array->len; ++i)
	{
		if (0 == compare_func(&(tags_array->pdata[i - 1]), &(tags_array->pdata[i])))
			tags_array->pdata[i-1] = NULL;
	}
	tm_tags_prune(tags_array);
	return TRUE;
}

/* Sorts newly-added tags and merges them in order with existing tags.
 * This is much faster than resorting the whole array.
 * Note: Having the caller append to the existing array should be faster
 * than creating a new array which would likely get resized more than once.
 * tags_array: array with new (perhaps unsorted) tags appended.
 * orig_len: number of existing tags. */
gboolean tm_tags_merge(GPtrArray *tags_array, gsize orig_len,
	TMTagAttrType *sort_attributes, gboolean dedup)
{
	gpointer *copy, *a, *b;
	gsize copy_len, i;

	if ((!tags_array) || (!tags_array->len) || orig_len >= tags_array->len)
		return TRUE;
	if (!orig_len)
		return tm_tags_sort(tags_array, sort_attributes, dedup);
	copy_len = tags_array->len - orig_len;
	copy = g_memdup(tags_array->pdata + orig_len, copy_len * sizeof(gpointer));
	s_sort_attrs = sort_attributes;
	s_partial = FALSE;
	/* enforce copy sorted with same attributes for merge */
	qsort(copy, copy_len, sizeof(gpointer), tm_tag_compare);
	a = tags_array->pdata + orig_len - 1;
	b = copy + copy_len - 1;
	for (i = tags_array->len - 1;; i--)
	{
		gint cmp = tm_tag_compare(a, b);

		tags_array->pdata[i] = (cmp >= 0) ? *a-- : *b--;
		if (a < tags_array->pdata)
		{
			/* include remainder of copy as well as current value of b */
			memcpy(tags_array->pdata, copy, ((b + 1) - copy) * sizeof(gpointer));
			break;
		}
		if (b < copy)
			break; /* remaining elements of 'a' are in place already */
		g_assert(i != 0);
	}
	s_sort_attrs = NULL;
	g_free(copy);
	if (dedup)
		tm_tags_dedup(tags_array, sort_attributes);
	return TRUE;
}

gboolean tm_tags_sort(GPtrArray *tags_array, TMTagAttrType *sort_attributes, gboolean dedup)
{
	if ((!tags_array) || (!tags_array->len))
		return TRUE;
	s_sort_attrs = sort_attributes;
	s_partial = FALSE;
	qsort(tags_array->pdata, tags_array->len, sizeof(gpointer), tm_tag_compare);
	s_sort_attrs = NULL;
	if (dedup)
		tm_tags_dedup(tags_array, sort_attributes);
	return TRUE;
}

gboolean tm_tags_custom_sort(GPtrArray *tags_array, TMTagCompareFunc compare_func, gboolean dedup)
{
	if ((!tags_array) || (!tags_array->len))
		return TRUE;
	qsort(tags_array->pdata, tags_array->len, sizeof(gpointer), compare_func);
	if (dedup)
		tm_tags_custom_dedup(tags_array, compare_func);
	return TRUE;
}

GPtrArray *tm_tags_extract(GPtrArray *tags_array, guint tag_types)
{
	GPtrArray *new_tags;
	guint i;
	if (NULL == tags_array)
		return NULL;
	new_tags = g_ptr_array_new();
	for (i=0; i < tags_array->len; ++i)
	{
		if (NULL != tags_array->pdata[i])
		{
			if (tag_types & (((TMTag *) tags_array->pdata[i])->type))
				g_ptr_array_add(new_tags, tags_array->pdata[i]);
		}
	}
	return new_tags;
}

void tm_tags_array_free(GPtrArray *tags_array, gboolean free_all)
{
	if (tags_array)
	{
		guint i;
		for (i = 0; i < tags_array->len; ++i)
			tm_tag_unref(tags_array->pdata[i]);
		if (free_all)
			g_ptr_array_free(tags_array, TRUE);
		else
			g_ptr_array_set_size(tags_array, 0);
	}
}

static TMTag **tags_search(const GPtrArray *tags_array, TMTag *tag, gboolean partial,
		gboolean tags_array_sorted)
{
	if (tags_array_sorted)
	{	/* fast binary search on sorted tags array */
		return (TMTag **) bsearch(&tag, tags_array->pdata, tags_array->len
		  , sizeof(gpointer), tm_tag_compare);
	}
	else
	{	/* the slow way: linear search (to make it a bit faster, search reverse assuming
		 * that the tag to search was added recently) */
		int i;
		TMTag **t;
		for (i = tags_array->len - 1; i >= 0; i--)
		{
			t = (TMTag **) &tags_array->pdata[i];
			if (0 == tm_tag_compare(&tag, t))
				return t;
		}
	}
	return NULL;
}

TMTag **tm_tags_find(const GPtrArray *tags_array, const char *name,
		gboolean partial, gboolean tags_array_sorted, int * tagCount)
{
	static TMTag *tag = NULL;
	TMTag **result;
	int tagMatches=0;

	if ((!tags_array) || (!tags_array->len))
		return NULL;

	if (NULL == tag)
		tag = g_new0(TMTag, 1);
	tag->name = (char *) name;
	s_sort_attrs = NULL;
	s_partial = partial;

	result = tags_search(tags_array, tag, partial, tags_array_sorted);
	/* There can be matches on both sides of result */
	if (result)
	{
		TMTag **last = (TMTag **) &tags_array->pdata[tags_array->len - 1];
		TMTag **adv;

		/* First look for any matches after result */
		adv = result;
		adv++;
		for (; adv <= last && *adv; ++ adv)
		{
			if (0 != tm_tag_compare(&tag, adv))
				break;
			++tagMatches;
		}
		/* Now look for matches from result and below */
		for (; result >= (TMTag **) tags_array->pdata; -- result)
		{
			if (0 != tm_tag_compare(&tag, (TMTag **) result))
				break;
			++tagMatches;
		}
		*tagCount=tagMatches;
		++ result;	/* Correct address for the last successful match */
	}
	s_partial = FALSE;
	return (TMTag **) result;
}

const char *tm_tag_type_name(const TMTag *tag)
{
	g_return_val_if_fail(tag, NULL);
	switch(tag->type)
	{
		case tm_tag_class_t: return "class";
		case tm_tag_enum_t: return "enum";
		case tm_tag_enumerator_t: return "enumval";
		case tm_tag_field_t: return "field";
		case tm_tag_function_t: return "function";
		case tm_tag_interface_t: return "interface";
		case tm_tag_member_t: return "member";
		case tm_tag_method_t: return "method";
		case tm_tag_namespace_t: return "namespace";
		case tm_tag_package_t: return "package";
		case tm_tag_prototype_t: return "prototype";
		case tm_tag_struct_t: return "struct";
		case tm_tag_typedef_t: return "typedef";
		case tm_tag_union_t: return "union";
		case tm_tag_variable_t: return "variable";
		case tm_tag_externvar_t: return "extern";
		case tm_tag_macro_t: return "define";
		case tm_tag_macro_with_arg_t: return "macro";
		case tm_tag_file_t: return "file";
		default: return NULL;
	}
	return NULL;
}

TMTagType tm_tag_name_type(const char* tag_name)
{
	g_return_val_if_fail(tag_name, tm_tag_undef_t);

	if (strcmp(tag_name, "class") == 0) return tm_tag_class_t;
	else if (strcmp(tag_name, "enum") == 0) return tm_tag_enum_t;
	else if (strcmp(tag_name, "enumval") == 0) return tm_tag_enumerator_t;
	else if (strcmp(tag_name, "field") == 0) return tm_tag_field_t;
	else if (strcmp(tag_name, "function") == 0) return tm_tag_function_t;
	else if (strcmp(tag_name, "interface") == 0) return tm_tag_interface_t;
	else if (strcmp(tag_name, "member") == 0) return tm_tag_member_t;
	else if (strcmp(tag_name, "method") == 0) return tm_tag_method_t;
	else if (strcmp(tag_name, "namespace") == 0) return tm_tag_namespace_t;
	else if (strcmp(tag_name, "package") == 0) return tm_tag_package_t;
	else if (strcmp(tag_name, "prototype") == 0) return tm_tag_prototype_t;
	else if (strcmp(tag_name, "struct") == 0) return tm_tag_struct_t;
	else if (strcmp(tag_name, "typedef") == 0) return tm_tag_typedef_t;
	else if (strcmp(tag_name, "union") == 0) return tm_tag_union_t;
	else if (strcmp(tag_name, "variable") == 0) return tm_tag_variable_t;
	else if (strcmp(tag_name, "extern") == 0) return tm_tag_externvar_t;
	else if (strcmp(tag_name, "define") == 0) return tm_tag_macro_t;
	else if (strcmp(tag_name, "macro") == 0) return tm_tag_macro_with_arg_t;
	else if (strcmp(tag_name, "file") == 0) return tm_tag_file_t;
	else return tm_tag_undef_t;
}

static const char *tm_tag_impl_name(TMTag *tag)
{
	g_return_val_if_fail(tag && (tm_tag_file_t != tag->type), NULL);
	if (TAG_IMPL_VIRTUAL == tag->atts.entry.impl)
		return "virtual";
	else
		return NULL;
}

static const char *tm_tag_access_name(TMTag *tag)
{
	g_return_val_if_fail(tag && (tm_tag_file_t != tag->type), NULL);
	if (TAG_ACCESS_PUBLIC == tag->atts.entry.access)
		return "public";
	else if (TAG_ACCESS_PROTECTED == tag->atts.entry.access)
		return "protected";
	else if (TAG_ACCESS_PRIVATE == tag->atts.entry.access)
		return "private";
	else
		return NULL;
}

void tm_tag_print(TMTag *tag, FILE *fp)
{
	const char *laccess, *impl, *type;
	if (!tag || !fp)
		return;
	if (tm_tag_file_t == tag->type)
	{
		fprintf(fp, "%s\n", tag->name);
		return;
	}
	laccess = tm_tag_access_name(tag);
	impl = tm_tag_impl_name(tag);
	type = tm_tag_type_name(tag);
	if (laccess)
		fprintf(fp, "%s ", laccess);
	if (impl)
		fprintf(fp, "%s ", impl);
	if (type)
		fprintf(fp, "%s ", type);
	if (tag->atts.entry.var_type)
		fprintf(fp, "%s ", tag->atts.entry.var_type);
	if (tag->atts.entry.scope)
		fprintf(fp, "%s::", tag->atts.entry.scope);
	fprintf(fp, "%s", tag->name);
	if (tag->atts.entry.arglist)
		fprintf(fp, "%s", tag->atts.entry.arglist);
	if (tag->atts.entry.inheritance)
		fprintf(fp, " : from %s", tag->atts.entry.inheritance);
	if ((tag->atts.entry.file) && (tag->atts.entry.line > 0))
		fprintf(fp, "[%s:%ld]", tag->atts.entry.file->work_object.file_name
		  , tag->atts.entry.line);
	fprintf(fp, "\n");
}

void tm_tags_array_print(GPtrArray *tags, FILE *fp)
{
	guint i;
	TMTag *tag;
	if (!(tags && (tags->len > 0) && fp))
		return;
	for (i = 0; i < tags->len; ++i)
	{
		tag = TM_TAG(tags->pdata[i]);
		tm_tag_print(tag, fp);
	}
}

gint tm_tag_scope_depth(const TMTag *t)
{
	gint depth;
	char *s;
	if(!(t && t->atts.entry.scope))
		return 0;
	for (s = t->atts.entry.scope, depth = 0; s; s = strstr(s, "::"))
	{
		++ depth;
		++ s;
	}
	return depth;
}
