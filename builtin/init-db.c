/*
 * GIT - The information manager from hell
 *
 * Copyright (C) Linus Torvalds, 2005
 */
#include "cache.h"
#include "refs.h"
#include "builtin.h"
#include "exec_cmd.h"
#include "parse-options.h"

#ifndef DEFAULT_GIT_TEMPLATE_DIR
#define DEFAULT_GIT_TEMPLATE_DIR "/usr/share/git-core/templates"
#endif

#ifdef NO_TRUSTABLE_FILEMODE
#define TEST_FILEMODE 0
#else
#define TEST_FILEMODE 1
#endif

static int init_is_bare_repository = 0;
static int init_shared_repository = -1;
static const char *init_db_template_dir;
static const char *git_link;

static void safe_create_dir(const char *dir, int share)
{
	if (mkdir(dir, 0777) < 0) {
		if (errno != EEXIST) {
			perror(dir);
			exit(1);
		}
	}
	else if (share && adjust_shared_perm(dir))
		die(_("Could not make %s writable by group"), dir);
}

static void copy_templates_1(struct strbuf *path, struct strbuf *template,
			     DIR *dir)
{
	size_t path_baselen = path->len;
	size_t template_baselen = template->len;
	struct dirent *de;

	/* Note: if ".git/hooks" file exists in the repository being
	 * re-initialized, /etc/core-git/templates/hooks/update would
	 * cause "git init" to fail here.  I think this is sane but
	 * it means that the set of templates we ship by default, along
	 * with the way the namespace under .git/ is organized, should
	 * be really carefully chosen.
	 */
	safe_create_dir(path->buf, 1);
	while ((de = readdir(dir)) != NULL) {
		struct stat st_git, st_template;
		int exists = 0;

		strbuf_setlen(path, path_baselen);
		strbuf_setlen(template, template_baselen);

		if (de->d_name[0] == '.')
			continue;
		strbuf_addstr(path, de->d_name);
		strbuf_addstr(template, de->d_name);
		if (lstat(path->buf, &st_git)) {
			if (errno != ENOENT)
				die_errno(_("cannot stat '%s'"), path->buf);
		}
		else
			exists = 1;

		if (lstat(template->buf, &st_template))
			die_errno(_("cannot stat template '%s'"), template->buf);

		if (S_ISDIR(st_template.st_mode)) {
			DIR *subdir = opendir(template->buf);
			if (!subdir)
				die_errno(_("cannot opendir '%s'"), template->buf);
			strbuf_addch(path, '/');
			strbuf_addch(template, '/');
			copy_templates_1(path, template, subdir);
			closedir(subdir);
		}
		else if (exists)
			continue;
		else if (S_ISLNK(st_template.st_mode)) {
			struct strbuf lnk = STRBUF_INIT;
			if (strbuf_readlink(&lnk, template->buf, 0) < 0)
				die_errno(_("cannot readlink '%s'"), template->buf);
			if (symlink(lnk.buf, path->buf))
				die_errno(_("cannot symlink '%s' '%s'"),
					  lnk.buf, path->buf);
			strbuf_release(&lnk);
		}
		else if (S_ISREG(st_template.st_mode)) {
			if (copy_file(path->buf, template->buf, st_template.st_mode))
				die_errno(_("cannot copy '%s' to '%s'"),
					  template->buf, path->buf);
		}
		else
			error(_("ignoring template %s"), template->buf);
	}
}

static void copy_templates(const char *template_dir)
{
	struct strbuf path = STRBUF_INIT;
	struct strbuf template_path = STRBUF_INIT;
	size_t template_len;
	DIR *dir;
	char *to_free = NULL;

	if (!template_dir)
		template_dir = getenv(TEMPLATE_DIR_ENVIRONMENT);
	if (!template_dir)
		template_dir = init_db_template_dir;
	if (!template_dir)
		template_dir = to_free = system_path(DEFAULT_GIT_TEMPLATE_DIR);
	if (!template_dir[0]) {
		free(to_free);
		return;
	}

	strbuf_addstr(&template_path, template_dir);
	strbuf_complete(&template_path, '/');
	template_len = template_path.len;

	dir = opendir(template_path.buf);
	if (!dir) {
		warning(_("templates not found %s"), template_dir);
		goto free_return;
	}

	/* Make sure that template is from the correct vintage */
	strbuf_addstr(&template_path, "config");
	repository_format_version = 0;
	git_config_from_file(check_repository_format_version,
			     template_path.buf, NULL);
	strbuf_setlen(&template_path, template_len);

	if (repository_format_version &&
	    repository_format_version != GIT_REPO_VERSION) {
		warning(_("not copying templates of "
			"a wrong format version %d from '%s'"),
			repository_format_version,
			template_dir);
		goto close_free_return;
	}

	strbuf_addstr(&path, get_git_dir());
	strbuf_complete(&path, '/');
	copy_templates_1(&path, &template_path, dir);
close_free_return:
	closedir(dir);
free_return:
	free(to_free);
	strbuf_release(&path);
	strbuf_release(&template_path);
}

static int git_init_db_config(const char *k, const char *v, void *cb)
{
	if (!strcmp(k, "init.templatedir"))
		return git_config_pathname(&init_db_template_dir, k, v);

	return 0;
}

/*
 * If the git_dir is not directly inside the working tree, then git will not
 * find it by default, and we need to set the worktree explicitly.
 */
static int needs_work_tree_config(const char *git_dir, const char *work_tree)
{
	if (!strcmp(work_tree, "/") && !strcmp(git_dir, "/.git"))
		return 0;
	if (skip_prefix(git_dir, work_tree, &git_dir) &&
	    !strcmp(git_dir, "/.git"))
		return 0;
	return 1;
}

static int create_default_files(const char *template_path)
{
	struct stat st1;
	struct strbuf buf = STRBUF_INIT;
	char *path;
	char repo_version_string[10];
	char junk[2];
	int reinit;
	int filemode;

	/*
	 * Create .git/refs/{heads,tags}
	 */
	safe_create_dir(git_path_buf(&buf, "refs"), 1);
	safe_create_dir(git_path_buf(&buf, "refs/heads"), 1);
	safe_create_dir(git_path_buf(&buf, "refs/tags"), 1);

	/* Just look for `init.templatedir` */
	git_config(git_init_db_config, NULL);

	/* First copy the templates -- we might have the default
	 * config file there, in which case we would want to read
	 * from it after installing.
	 */
	copy_templates(template_path);

	git_config(git_default_config, NULL);
	is_bare_repository_cfg = init_is_bare_repository;

	/* reading existing config may have overwrote it */
	if (init_shared_repository != -1)
		shared_repository = init_shared_repository;

	/*
	 * We would have created the above under user's umask -- under
	 * shared-repository settings, we would need to fix them up.
	 */
	if (shared_repository) {
		adjust_shared_perm(get_git_dir());
		adjust_shared_perm(git_path_buf(&buf, "refs"));
		adjust_shared_perm(git_path_buf(&buf, "refs/heads"));
		adjust_shared_perm(git_path_buf(&buf, "refs/tags"));
	}

	/*
	 * Create the default symlink from ".git/HEAD" to the "master"
	 * branch, if it does not exist yet.
	 */
	path = git_path_buf(&buf, "HEAD");
	reinit = (!access(path, R_OK)
		  || readlink(path, junk, sizeof(junk)-1) != -1);
	if (!reinit) {
		if (create_symref("HEAD", "refs/heads/master", NULL) < 0)
			exit(1);
	}

	/* This forces creation of new config file */
	xsnprintf(repo_version_string, sizeof(repo_version_string),
		  "%d", GIT_REPO_VERSION);
	git_config_set("core.repositoryformatversion", repo_version_string);

	/* Check filemode trustability */
	path = git_path_buf(&buf, "config");
	filemode = TEST_FILEMODE;
	if (TEST_FILEMODE && !lstat(path, &st1)) {
		struct stat st2;
		filemode = (!chmod(path, st1.st_mode ^ S_IXUSR) &&
				!lstat(path, &st2) &&
				st1.st_mode != st2.st_mode &&
				!chmod(path, st1.st_mode));
		if (filemode && !reinit && (st1.st_mode & S_IXUSR))
			filemode = 0;
	}
	git_config_set("core.filemode", filemode ? "true" : "false");

	if (is_bare_repository())
		git_config_set("core.bare", "true");
	else {
		const char *work_tree = get_git_work_tree();
		git_config_set("core.bare", "false");
		/* allow template config file to override the default */
		if (log_all_ref_updates == -1)
		    git_config_set("core.logallrefupdates", "true");
		if (needs_work_tree_config(get_git_dir(), work_tree))
			git_config_set("core.worktree", work_tree);
	}

	if (!reinit) {
		/* Check if symlink is supported in the work tree */
		path = git_path_buf(&buf, "tXXXXXX");
		if (!close(xmkstemp(path)) &&
		    !unlink(path) &&
		    !symlink("testing", path) &&
		    !lstat(path, &st1) &&
		    S_ISLNK(st1.st_mode))
			unlink(path); /* good */
		else
			git_config_set("core.symlinks", "false");

		/* Check if the filesystem is case-insensitive */
		path = git_path_buf(&buf, "CoNfIg");
		if (!access(path, F_OK))
			git_config_set("core.ignorecase", "true");
		probe_utf8_pathname_composition();
	}

	strbuf_release(&buf);
	return reinit;
}

static void create_object_directory(void)
{
	struct strbuf path = STRBUF_INIT;
	size_t baselen;

	strbuf_addstr(&path, get_object_directory());
	baselen = path.len;

	safe_create_dir(path.buf, 1);

	strbuf_setlen(&path, baselen);
	strbuf_addstr(&path, "/pack");
	safe_create_dir(path.buf, 1);

	strbuf_setlen(&path, baselen);
	strbuf_addstr(&path, "/info");
	safe_create_dir(path.buf, 1);

	strbuf_release(&path);
}

int set_git_dir_init(const char *git_dir, const char *real_git_dir,
		     int exist_ok)
{
	if (real_git_dir) {
		struct stat st;

		if (!exist_ok && !stat(git_dir, &st))
			die(_("%s already exists"), git_dir);

		if (!exist_ok && !stat(real_git_dir, &st))
			die(_("%s already exists"), real_git_dir);

		/*
		 * make sure symlinks are resolved because we'll be
		 * moving the target repo later on in separate_git_dir()
		 */
		git_link = xstrdup(real_path(git_dir));
		set_git_dir(real_path(real_git_dir));
	}
	else {
		set_git_dir(real_path(git_dir));
		git_link = NULL;
	}
	return 0;
}

static void separate_git_dir(const char *git_dir)
{
	struct stat st;

	if (!stat(git_link, &st)) {
		const char *src;

		if (S_ISREG(st.st_mode))
			src = read_gitfile(git_link);
		else if (S_ISDIR(st.st_mode))
			src = git_link;
		else
			die(_("unable to handle file type %d"), (int)st.st_mode);

		if (rename(src, git_dir))
			die_errno(_("unable to move %s to %s"), src, git_dir);
	}

	write_file(git_link, "gitdir: %s", git_dir);
}

int init_db(const char *template_dir, unsigned int flags)
{
	int reinit;
	const char *git_dir = get_git_dir();

	if (git_link)
		separate_git_dir(git_dir);

	safe_create_dir(git_dir, 0);

	init_is_bare_repository = is_bare_repository();

	/* Check to see if the repository version is right.
	 * Note that a newly created repository does not have
	 * config file, so this will not fail.  What we are catching
	 * is an attempt to reinitialize new repository with an old tool.
	 */
	check_repository_format();

	reinit = create_default_files(template_dir);

	create_object_directory();

	if (shared_repository) {
		char buf[10];
		/* We do not spell "group" and such, so that
		 * the configuration can be read by older version
		 * of git. Note, we use octal numbers for new share modes,
		 * and compatibility values for PERM_GROUP and
		 * PERM_EVERYBODY.
		 */
		if (shared_repository < 0)
			/* force to the mode value */
			xsnprintf(buf, sizeof(buf), "0%o", -shared_repository);
		else if (shared_repository == PERM_GROUP)
			xsnprintf(buf, sizeof(buf), "%d", OLD_PERM_GROUP);
		else if (shared_repository == PERM_EVERYBODY)
			xsnprintf(buf, sizeof(buf), "%d", OLD_PERM_EVERYBODY);
		else
			die("BUG: invalid value for shared_repository");
		git_config_set("core.sharedrepository", buf);
		git_config_set("receive.denyNonFastforwards", "true");
	}

	if (!(flags & INIT_DB_QUIET)) {
		int len = strlen(git_dir);

		/* TRANSLATORS: The first '%s' is either "Reinitialized
		   existing" or "Initialized empty", the second " shared" or
		   "", and the last '%s%s' is the verbatim directory name. */
		printf(_("%s%s Git repository in %s%s\n"),
		       reinit ? _("Reinitialized existing") : _("Initialized empty"),
		       shared_repository ? _(" shared") : "",
		       git_dir, len && git_dir[len-1] != '/' ? "/" : "");
	}

	return 0;
}

static int guess_repository_type(const char *git_dir)
{
	const char *slash;
	char *cwd;
	int cwd_is_git_dir;

	/*
	 * "GIT_DIR=. git init" is always bare.
	 * "GIT_DIR=`pwd` git init" too.
	 */
	if (!strcmp(".", git_dir))
		return 1;
	cwd = xgetcwd();
	cwd_is_git_dir = !strcmp(git_dir, cwd);
	free(cwd);
	if (cwd_is_git_dir)
		return 1;
	/*
	 * "GIT_DIR=.git or GIT_DIR=something/.git is usually not.
	 */
	if (!strcmp(git_dir, ".git"))
		return 0;
	slash = strrchr(git_dir, '/');
	if (slash && !strcmp(slash, "/.git"))
		return 0;

	/*
	 * Otherwise it is often bare.  At this point
	 * we are just guessing.
	 */
	return 1;
}

static int shared_callback(const struct option *opt, const char *arg, int unset)
{
	*((int *) opt->value) = (arg) ? git_config_perm("arg", arg) : PERM_GROUP;
	return 0;
}

static const char *const init_db_usage[] = {
	N_("git init [-q | --quiet] [--bare] [--template=<template-directory>] [--shared[=<permissions>]] [<directory>]"),
	NULL
};

/*
 * If you want to, you can share the DB area with any number of branches.
 * That has advantages: you can save space by sharing all the SHA1 objects.
 * On the other hand, it might just make lookup slower and messier. You
 * be the judge.  The default case is to have one DB per managed directory.
 */
int cmd_init_db(int argc, const char **argv, const char *prefix)
{
	const char *git_dir;
	const char *real_git_dir = NULL;
	const char *work_tree;
	const char *template_dir = NULL;
	unsigned int flags = 0;
	const struct option init_db_options[] = {
		OPT_STRING(0, "template", &template_dir, N_("template-directory"),
				N_("directory from which templates will be used")),
		OPT_SET_INT(0, "bare", &is_bare_repository_cfg,
				N_("create a bare repository"), 1),
		{ OPTION_CALLBACK, 0, "shared", &init_shared_repository,
			N_("permissions"),
			N_("specify that the git repository is to be shared amongst several users"),
			PARSE_OPT_OPTARG | PARSE_OPT_NONEG, shared_callback, 0},
		OPT_BIT('q', "quiet", &flags, N_("be quiet"), INIT_DB_QUIET),
		OPT_STRING(0, "separate-git-dir", &real_git_dir, N_("gitdir"),
			   N_("separate git dir from working tree")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, init_db_options, init_db_usage, 0);

	if (real_git_dir && !is_absolute_path(real_git_dir))
		real_git_dir = xstrdup(real_path(real_git_dir));

	if (argc == 1) {
		int mkdir_tried = 0;
	retry:
		if (chdir(argv[0]) < 0) {
			if (!mkdir_tried) {
				int saved;
				/*
				 * At this point we haven't read any configuration,
				 * and we know shared_repository should always be 0;
				 * but just in case we play safe.
				 */
				saved = shared_repository;
				shared_repository = 0;
				switch (safe_create_leading_directories_const(argv[0])) {
				case SCLD_OK:
				case SCLD_PERMS:
					break;
				case SCLD_EXISTS:
					errno = EEXIST;
					/* fallthru */
				default:
					die_errno(_("cannot mkdir %s"), argv[0]);
					break;
				}
				shared_repository = saved;
				if (mkdir(argv[0], 0777) < 0)
					die_errno(_("cannot mkdir %s"), argv[0]);
				mkdir_tried = 1;
				goto retry;
			}
			die_errno(_("cannot chdir to %s"), argv[0]);
		}
	} else if (0 < argc) {
		usage(init_db_usage[0]);
	}
	if (is_bare_repository_cfg == 1) {
		char *cwd = xgetcwd();
		setenv(GIT_DIR_ENVIRONMENT, cwd, argc > 0);
		free(cwd);
	}

	if (init_shared_repository != -1)
		shared_repository = init_shared_repository;

	/*
	 * GIT_WORK_TREE makes sense only in conjunction with GIT_DIR
	 * without --bare.  Catch the error early.
	 */
	git_dir = getenv(GIT_DIR_ENVIRONMENT);
	work_tree = getenv(GIT_WORK_TREE_ENVIRONMENT);
	if ((!git_dir || is_bare_repository_cfg == 1) && work_tree)
		die(_("%s (or --work-tree=<directory>) not allowed without "
			  "specifying %s (or --git-dir=<directory>)"),
		    GIT_WORK_TREE_ENVIRONMENT,
		    GIT_DIR_ENVIRONMENT);

	/*
	 * Set up the default .git directory contents
	 */
	if (!git_dir)
		git_dir = DEFAULT_GIT_DIR_ENVIRONMENT;

	if (is_bare_repository_cfg < 0)
		is_bare_repository_cfg = guess_repository_type(git_dir);

	if (!is_bare_repository_cfg) {
		const char *git_dir_parent = strrchr(git_dir, '/');
		if (git_dir_parent) {
			char *rel = xstrndup(git_dir, git_dir_parent - git_dir);
			git_work_tree_cfg = xstrdup(real_path(rel));
			free(rel);
		}
		if (!git_work_tree_cfg)
			git_work_tree_cfg = xgetcwd();
		if (work_tree)
			set_git_work_tree(work_tree);
		else
			set_git_work_tree(git_work_tree_cfg);
		if (access(get_git_work_tree(), X_OK))
			die_errno (_("Cannot access work tree '%s'"),
				   get_git_work_tree());
	}
	else {
		if (work_tree)
			set_git_work_tree(work_tree);
	}

	set_git_dir_init(git_dir, real_git_dir, 1);

	return init_db(template_dir, flags);
}
