/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "lib-signals.h"
#include "ioloop.h"
#include "master-service.h"
#include "mail-user.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-storage-settings.h"
#include "mail-storage-service.h"
#include "doveadm.h"
#include "doveadm-mail.h"

#include <stdio.h>

ARRAY_TYPE(doveadm_mail_cmd) doveadm_mail_cmds;

static int killed_signo = 0;

static void cmd_purge(struct mail_user *user, const char *args[] ATTR_UNUSED)
{
	struct mail_namespace *ns;

	for (ns = user->namespaces; ns != NULL; ns = ns->next) {
		if (ns->type != NAMESPACE_PRIVATE || ns->alias_for != NULL)
			continue;

		if (mail_storage_purge(ns->storage) < 0) {
			i_error("Purging namespace '%s' failed: %s", ns->prefix,
				mail_storage_get_last_error(ns->storage, NULL));
		}
	}
}

static struct mailbox *
mailbox_find_and_open(struct mail_user *user, const char *mailbox)
{
	struct mail_namespace *ns;
	struct mailbox *box;
	const char *orig_mailbox = mailbox;

	ns = mail_namespace_find(user->namespaces, &mailbox);
	if (ns == NULL)
		i_fatal("Can't find namespace for mailbox %s", mailbox);

	box = mailbox_alloc(ns->list, mailbox, NULL, MAILBOX_FLAG_KEEP_RECENT |
			    MAILBOX_FLAG_IGNORE_ACLS);
	if (mailbox_open(box) < 0) {
		i_fatal("Opening mailbox %s failed: %s", orig_mailbox,
			mail_storage_get_last_error(mailbox_get_storage(box),
						    NULL));
	}
	return box;
}

static void cmd_force_resync(struct mail_user *user, const char *args[])
{
	const char *mailbox = args[0];
	struct mail_storage *storage;
	struct mailbox *box;

	if (mailbox == NULL)
		usage();

	box = mailbox_find_and_open(user, mailbox);
	storage = mailbox_get_storage(box);
	if (mailbox_sync(box, MAILBOX_SYNC_FLAG_FORCE_RESYNC |
			 MAILBOX_SYNC_FLAG_FIX_INCONSISTENT, 0, NULL) < 0) {
		i_fatal("Forcing a resync on mailbox %s failed: %s", mailbox,
			mail_storage_get_last_error(storage, NULL));
	}
	mailbox_close(&box);
}

static void
doveadm_mail_single_user(doveadm_mail_command_t *cmd, const char *username,
			 enum mail_storage_service_flags service_flags,
			 const char *args[])
{
	struct mail_storage_service_ctx *storage_service;
	struct mail_storage_service_user *service_user;
	struct mail_storage_service_input input;
	struct mail_user *mail_user;
	const char *error;

	if (username == NULL)
		i_fatal("USER environment is missing and -u option not used");

	memset(&input, 0, sizeof(input));
	input.username = username;

	storage_service = mail_storage_service_init(master_service, NULL,
						    service_flags);
	if (mail_storage_service_lookup_next(storage_service, &input,
					     &service_user, &mail_user,
					     &error) <= 0)
		i_fatal("%s", error);
	cmd(mail_user, args);
	mail_user_unref(&mail_user);
	mail_storage_service_user_free(&service_user);
	mail_storage_service_deinit(&storage_service);
}

static int
doveadm_mail_next_user(doveadm_mail_command_t *cmd,
		       struct mail_storage_service_ctx *storage_service,
		       const struct mail_storage_service_input *input,
		       const char *args[])
{
	struct mail_storage_service_user *service_user;
	struct mail_user *mail_user;
	const char *error;
	int ret;

	i_set_failure_prefix(t_strdup_printf("doveadm(%s): ", input->username));
	ret = mail_storage_service_lookup(storage_service, input,
					  &service_user, &error);
	if (ret <= 0) {
		if (ret == 0) {
			i_info("User no longer exists, skipping");
			return 0;
		} else {
			i_error("User lookup failed: %s", error);
			return -1;
		}
	}
	if (mail_storage_service_next(storage_service, service_user,
				      &mail_user, &error) < 0) {
		i_error("User init failed: %s", error);
		mail_storage_service_user_free(&service_user);
		return -1;
	}
	cmd(mail_user, args);
	mail_storage_service_user_free(&service_user);
	mail_user_unref(&mail_user);
	return 0;
}

static void sig_die(const siginfo_t *si, void *context ATTR_UNUSED)
{
	killed_signo = si->si_signo;
}

static void
doveadm_mail_all_users(doveadm_mail_command_t *cmd,
		       enum mail_storage_service_flags service_flags,
		       const char *args[])
{
	struct mail_storage_service_input input;
	struct mail_storage_service_ctx *storage_service;
	unsigned int user_idx, user_count, interval, n;
	const char *user;
	int ret;

	service_flags |= MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;

	memset(&input, 0, sizeof(input));
	input.service = "doveadm";

	storage_service = mail_storage_service_init(master_service, NULL,
						    service_flags);

        lib_signals_set_handler(SIGINT, FALSE, sig_die, NULL);
	lib_signals_set_handler(SIGTERM, FALSE, sig_die, NULL);

	user_count = mail_storage_service_all_init(storage_service);
	n = user_count / 10000;
	for (interval = 10; n > 0 && interval < 1000; interval *= 10)
		n /= 10;
	
	user_idx = 0;
	while ((ret = mail_storage_service_all_next(storage_service,
						    &user)) > 0) {
		input.username = user;
		T_BEGIN {
			ret = doveadm_mail_next_user(cmd, storage_service,
						     &input, args);
		} T_END;
		if (ret < 0)
			break;
		if ((service_flags & MAIL_STORAGE_SERVICE_FLAG_DEBUG) != 0) {
			if (++user_idx % interval == 0) {
				printf("\r%d / %d", user_idx, user_count);
				fflush(stdout);
			}
		}
		if (killed_signo != 0) {
			i_warning("Killed with signal %d", killed_signo);
			ret = -1;
			break;
		}
	}
	if ((service_flags & MAIL_STORAGE_SERVICE_FLAG_DEBUG) != 0)
		printf("\n");
	i_set_failure_prefix("doveadm: ");
	if (ret < 0)
		i_error("Failed to iterate through some users");
	mail_storage_service_deinit(&storage_service);
}

static void
doveadm_mail_cmd(doveadm_mail_command_t *cmd, int argc, char *argv[])
{
	enum mail_storage_service_flags service_flags = 0;
	const char *username;
	bool all_users = FALSE;
	int c;

	while ((c = getopt(argc, argv, "av")) > 0) {
		switch (c) {
		case 'a':
			all_users = TRUE;
			break;
		case 'v':
			service_flags |= MAIL_STORAGE_SERVICE_FLAG_DEBUG;
			break;
		default:
			usage();
		}
	}
	if (!all_users) {
		if (optind == argc)
			usage();
		service_flags |= MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;
		username = argv[optind++];
		doveadm_mail_single_user(cmd, username, service_flags,
					 (const char **)argv + optind);
	} else {
		service_flags |= MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP;
		doveadm_mail_all_users(cmd, service_flags,
				       (const char **)argv + optind);
	}
}

bool doveadm_mail_try_run(const char *cmd_name, int argc, char *argv[])
{
	const struct doveadm_mail_cmd *cmd;

	array_foreach(&doveadm_mail_cmds, cmd) {
		if (strcmp(cmd->name, cmd_name) == 0) {
			doveadm_mail_cmd(cmd->cmd, argc, argv);
			return TRUE;
		}
	}
	return FALSE;
}

void doveadm_mail_register_cmd(const struct doveadm_mail_cmd *cmd)
{
	/* for now we'll just assume that cmd will be permanently in memory */
	array_append(&doveadm_mail_cmds, cmd, 1);
}

void doveadm_mail_usage(void)
{
	const struct doveadm_mail_cmd *cmd;

	array_foreach(&doveadm_mail_cmds, cmd) {
		fprintf(stderr, USAGE_CMDNAME_FMT" <user>|-a", cmd->name);
		if (cmd->usage_args != NULL)
			fprintf(stderr, " %s", cmd->usage_args);
		fputc('\n', stderr);
	}
}

static struct doveadm_mail_cmd mail_commands[] = {
	{ cmd_purge, "purge", NULL },
	{ cmd_force_resync, "force-resync", "<mailbox>" }
};

void doveadm_mail_init(void)
{
	unsigned int i;

	i_array_init(&doveadm_mail_cmds, 32);
	for (i = 0; i < N_ELEMENTS(mail_commands); i++)
		doveadm_mail_register_cmd(&mail_commands[i]);
}

void doveadm_mail_deinit(void)
{
	array_free(&doveadm_mail_cmds);
}
