/* palmdev-prep.c: report on and generate paths to installed SDKs.

   Copyright 2001, 2002 John Marshall.

   This is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.  */

#include "config.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "libiberty.h"
#include "getopt.h"

#include "utils.h"


/* String arena used for the duration.  */
static struct string_store *store;


/* Returns true if STR starts with PREFIX, folding STR to lower case before
   comparing.  */
int
matches (const char *prefix, const char *str) {
  while (*prefix && tolower (*str) == *prefix)
    prefix++, str++;

  return *prefix == '\0';
  }


/* Returns the SDK key portion of a 'sdk-N' directory name (safely stored on
   the string store).  */
const char *
canonical_key (const char *name) {
  char *key, *dot;

  if (matches ("palmos", name))
    name += 6;
  if (matches ("sdk-", name))
    name += 4;

  key = insert_string (store, name);

  /* Canonicalise so that eg sdk-4.0 answers to "-palmos4".  */
  dot = strrchr (key, '.');
  if (dot && strcmp (dot, ".0") == 0)
    *dot = '\0';

  return key;
  }


/* A ROOT is a directory with "include" and/or "lib" subdirectories, etc,
   i.e., the base of a SDK directory tree; either really a Palm OS SDK
   (e.g., /opt/palmdev/sdk-3.5) or the generic SDK-neutral part of a
   PalmDev tree (e.g. /opt/palmdev).

   A series of analyze_palmdev_tree() calls creates two linked lists of
   roots:  GENERIC_ROOT_LIST is an ordered list of the generic ones, while
   SDK_ROOT_LIST contains the ones that are real SDKs and is really a
   table accessed with find().  The KEY field is only used by the latter.  */

enum sub_kind { include, lib };

struct root {
  struct root *next;
  const char *prefix;	/* Full path of the root directory.  */
  const char *sub[2];	/* Names of the headers & libraries subdirectories.  */
  const char *key;	/* Canonical SDK key, in the case of a SDK root.  */
  };

struct root *
make_root (const char *path_format, ...) {
  char path[FILENAME_MAX];
  va_list args;
  struct root *root = xmalloc (sizeof (struct root));

  va_start (args, path_format);
  vsprintf (path, path_format, args);
  va_end (args);

  root->next = NULL;
  root->prefix = insert_string (store, path);
  root->sub[include] = is_dir ("%s/include", path)? "include"
		     : is_dir ("%s/Incs", path)? "Incs"
		     : NULL;
  root->sub[lib] = is_dir ("%s/lib", path)? "lib"
		 : is_dir ("%s/GCC Libraries", path)? "GCC Libraries"
		 : NULL;

  return root;
  }

struct root *
find (struct root *list, const char *key) {
  if (key)
    for (; list; list = list->next)
      if (strcmp (key, list->key) == 0)
	return list;

  return NULL;
  }

void
free_root_list (struct root *list) {
  while (list) {
    struct root *next = list->next;

    /* All the strings in *LIST are allocated in the string_store, so need
       not be explicitly freed here.  */
    free (list);
    list = next;
    }
  }


struct root *generic_root_list, *sdk_root_list;

void
print_report (const char *name, const struct root *root,
	      const struct root *overriding_root, int headers_required) {
  printf ("  %-13s\t", name);

  if (overriding_root)
    printf ("UNUSED -- hidden by %s", overriding_root->prefix);
  else if (headers_required && ! root->sub[include])
    printf ("INVALID -- no headers");
  else {
    if (root->sub[include])
      printf ("headers in '%s', ", root->sub[include]);
    else
      printf ("no headers, ");

    if (root->sub[lib])
      printf ("libraries in '%s'", root->sub[lib]);
    else
      printf ("no libraries");
    }

  printf ("\n");
  }

void
analyze_palmdev_tree (const char *prefix, int report) {
  static struct root **last_generic_root = &generic_root_list;

  DIR *dir = opendir (prefix);

  if (dir) {
    struct dirent *e;
    struct root *root;
    int n = 0;

    if (report)
      printf ("Checking SDKs in %s\n", prefix);

    while ((e = readdir (dir)) != NULL)
      if (matches ("sdk-", e->d_name) &&
	  is_dir_dirent (e, "%s/%s", prefix, e->d_name)) {
	const char *key = canonical_key (e->d_name);
	struct root *overriding_sdk = find (sdk_root_list, key);

	if (overriding_sdk)
	  root = NULL;
	else
	  root = make_root ("%s/%s", prefix, e->d_name);

	n++;
	if (report)
	  print_report (e->d_name, root, overriding_sdk, 1);

	if (root) {
	  if (root->sub[include]) {
	    root->key = key;
	    root->next = sdk_root_list;
	    sdk_root_list = root;
	    }
	  else
	    free (root);
	  }
	}

    closedir (dir);

    if (report && n == 0)
      printf ("  (none)\n");

    root = make_root ("%s", prefix);
    if (root->sub[include] || root->sub[lib]) {
      if (report) {
	printf ("  and material in %s used regardless of SDK choice\n", prefix);
	print_report ("  (common)", root, NULL, 0);
	}

      *last_generic_root = make_root ("%s", prefix);
      last_generic_root = &((*last_generic_root)->next);
      }
    else
      free (root);

    if (report)
      printf ("\n");
    }
  }


const char *spec[2] = { "cpp", "link" };
const char *option[2] = { "-isystem ", "-L" };

static void
write_dirtree (FILE *f, struct root *root, const char *target,
	       enum sub_kind kind) {
  if (root->sub[kind]) {
    /* FIXME this will need generalising when we have multiple targets.  */
    const char *target_for_lib = target? "/m68k-palmos-coff" : "";
    TREE *tree = opentree (DIRS, "%s/%s%s", root->prefix, root->sub[kind],
			   target_for_lib);
    const char *dir;
    while ((dir = readtree (tree)) != NULL) {
      const char *s;
      fprintf (f, " %s", option[kind]);
      for (s = dir; *s; s++) {
	if (isspace (*s))
	  putc ('\\', f);
	putc (*s, f);
	}
      }

    closetree (tree);
    }
  }

static void
write_sdk_spec (FILE *f, struct root *sdk, const char *target,
		enum sub_kind kind) {
  fprintf (f, "*%s_sdk_%s:\n", spec[kind], sdk->key);
  write_dirtree (f, sdk, target, kind);
  fprintf (f, "\n\n");
  }

static void
write_main_spec (FILE *f, const char *target, const struct root *default_sdk,
		 enum sub_kind kind) {
  struct root *root, *sdk;

  fprintf (f, "*%s:\n+ %%{!palmos-none:", spec[kind]);

  for (root = generic_root_list; root; root = root->next)
    write_dirtree (f, root, target, kind);

  for (sdk = sdk_root_list; sdk; sdk = sdk->next) {
    fprintf (f, " %%{palmos%s:%%(%s_sdk_%s)}", sdk->key, spec[kind], sdk->key);
    if (strspn (sdk->key, "0123456789") == strlen (sdk->key))
      fprintf (f, " %%{palmos%s.0:%%(%s_sdk_%s)}", sdk->key, spec[kind],
	       sdk->key);
    }

  if (default_sdk)
    fprintf (f, " %%{!palmos*: %%(%s_sdk_%s)}", spec[kind], default_sdk->key);

  fprintf (f, "}\n\n");
  }

void
write_specs (FILE *f, const char *target, const struct root *default_sdk) {
  struct root *sdk;

  for (sdk = sdk_root_list; sdk; sdk = sdk->next) {
    write_sdk_spec (f, sdk, NULL, include);
    write_sdk_spec (f, sdk, target, lib);
    }

  write_main_spec (f, NULL, default_sdk, include);
  write_main_spec (f, target, default_sdk, lib);
  }


void
remove_file (int verbose, const char *fname) {
  struct stat st;

  if (stat (fname, &st) != 0)
    return;  /* Already non-existent.  */

  if (remove (fname) == 0) {
    if (verbose)
      printf ("Removed '%s'\n", fname);
    }
  else
    warning ("can't remove '%s': @P", fname);
  }


/* FIXME this will need to get cleverer when we have multiple targets.  */
const char *target_list[] = { TARGET_ALIAS, NULL };

char *
specfilename (const char *target) {
  static char fname[FILENAME_MAX];
  sprintf (fname, "%s/%s/specs", STANDARD_EXEC_PREFIX, target);
  return fname;
  }


void
usage () {
  printf ("Usage: %s [options] [directory...]\n", progname);
  printf ("Directories listed will be scanned in addition to %s\n",
	  PALMDEV_PREFIX);
  printf ("Options:\n");
  propt ("-d SDK, --default SDK", "Set default SDK");
  propt ("-r, --remove", "Remove all files installed by palmdev-prep");
  propt ("--dump-specs TARGET", "Write specs for TARGET to standard output");
  propt ("-q, --quiet, --silent", "Suppress display of installation analysis");
  propt ("-v, --verbose", "Display extra information about actions taken");
  }

static int show_help, show_version;

static const char shortopts[] = "d:rqv";

static struct option longopts[] = {
  { "default", required_argument, NULL, 'd' },
  { "remove", no_argument, NULL, 'r' },
  { "dump-specs", required_argument, NULL, 'D' },

  { "quiet", no_argument, NULL, 'q' },
  { "silent", no_argument, NULL, 'q' },
  { "verbose", no_argument, NULL, 'v' },

  { "help", no_argument, &show_help, 1 },
  { "version", no_argument, &show_version, 1 },
  { NULL, no_argument, NULL, 0 }
  };

int
main (int argc, char **argv) {
  const char *default_sdk_name = NULL;
  const char *dump_target = NULL;
  int removing = 0, report = 1, verbose = 0;
  int c;

  set_progname (argv[0]);

  while ((c = getopt_long (argc, argv, shortopts, longopts, NULL)) >= 0)
    switch (c) {
    case 'd':
      default_sdk_name = optarg;
      break;

    case 'D':
      dump_target = optarg;
      break;

    case 'r':
      removing = 1;
      break;

    case 'q':
      report = 0;
      break;

    case 'v':
      verbose = 1;
      break;

    case 0:
      /* 0 indicates an automatically handled long option: do nothing.  */
      break;

    default:
      nerrors++;
      show_help = 1;
      break;
      }

  if (show_version || show_help) {
    if (show_version)
      print_version ("palmdev-prep", "Jg");

    if (show_help)
      usage ();
    }
  else if (removing) {
    const char **target;

    for (target = target_list; *target; target++)
      remove_file (verbose, specfilename (*target));
    }
  else {
    struct root *default_sdk = NULL;

    store = new_string_store ();

    analyze_palmdev_tree (PALMDEV_PREFIX, report);
    for (; optind < argc; optind++)
      if (is_dir ("%s", argv[optind]))
	analyze_palmdev_tree (argv[optind], report);
      else
	warning ("can't open '%s': @P", argv[optind]);

    if (default_sdk_name) {
      default_sdk = find (sdk_root_list, canonical_key (default_sdk_name));
      if (default_sdk == NULL)
	warning ("SDK '%s' not found -- using highest found instead",
		 default_sdk_name);
      }

    if (default_sdk == NULL && sdk_root_list) {
      struct root *sdk;

      /* Find the SDK with the alphabetically highest key.  */
      default_sdk = sdk_root_list;
      for (sdk = sdk_root_list; sdk; sdk = sdk->next)
	if (strcmp (sdk->key, default_sdk->key) > 0)
	  default_sdk = sdk;

      if (report)
	printf ("When GCC is given no -palmos options, "
		"SDK '%s' will be used by default\n\n", default_sdk->key);
      }

    if (dump_target)
      write_specs (stdout, dump_target, default_sdk);
    else {
      const char **target;
      const char *message = "...done";

      if (report)
	printf ("Writing SDK details to target specs files...\n");

      for (target = target_list; *target; target++) {
	const char *fname = specfilename (*target);
	FILE *f = fopen (fname, "w");

	if (f) {
	  write_specs (f, *target, default_sdk);
	  fclose (f);

	  if (verbose)
	    printf ("Wrote %s specs to '%s'\n", *target, fname);
	  }
	else {
#ifdef EACCES
	  if (errno == EACCES)
	    message = "Permission to write spec files denied -- "
		      "try again as root";
#endif
	  error ("can't write to '%s': @P", fname);
	  }
	}

      if (report)
	printf ("%s\n", message);
      }

    free_root_list (generic_root_list);
    free_root_list (sdk_root_list);

    free_string_store (store);
    }

  return (nerrors == 0)? EXIT_SUCCESS : EXIT_FAILURE;
  }