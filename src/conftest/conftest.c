/*
 * Copyright (C) 2010 Martin Willi
 * Copyright (C) 2010 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <dlfcn.h>
#include <libgen.h>

#include "conftest.h"
#include "config.h"
#include "hooks/hook.h"

#include <threading/thread.h>

/**
 * Conftest globals struct
 */
conftest_t *conftest;

/**
 * Print usage information
 */
static void usage(FILE *out)
{
	fprintf(out, "Usage:\n");
	fprintf(out, "  --help           show usage information\n");
	fprintf(out, "  --version        show conftest version\n");
	fprintf(out, "  --suite <file>   global testsuite configuration "
									 "(default: ./suite.conf)\n");
	fprintf(out, "  --test <file>    test specific configuration\n");
}

/**
 * Handle SIGSEGV/SIGILL signals raised by threads
 */
static void segv_handler(int signal)
{
	fprintf(stderr, "thread %u received %d\n", thread_current_id(), signal);
	abort();
}

/**
 * Load suite and test specific configurations
 */
static bool load_configs(char *suite_file, char *test_file)
{
	if (!test_file)
	{
		fprintf(stderr, "Missing test configuration file.\n");
		return FALSE;
	}
	if (access(suite_file, R_OK) != 0)
	{
		fprintf(stderr, "Reading suite configuration file '%s' failed: %s.\n",
				suite_file, strerror(errno));
		return FALSE;
	}
	if (access(test_file, R_OK) != 0)
	{
		fprintf(stderr, "Reading test configuration file '%s' failed: %s.\n",
				test_file, strerror(errno));
		return FALSE;
	}
	conftest->suite = settings_create(suite_file);
	conftest->test = settings_create(test_file);
	suite_file = dirname(suite_file);
	test_file = dirname(test_file);
	conftest->suite_dir = strdup(suite_file);
	conftest->test_dir = strdup(test_file);
	return TRUE;
}

/**
 * Load trusted/untrusted certificates
 */
static bool load_trusted_cert(settings_t *settings, bool trusted)
{
	enumerator_t *enumerator;
	char *key, *value;

	enumerator = settings->create_key_value_enumerator(settings,
								trusted ? "certs.trusted" : "certs.untrusted");
	while (enumerator->enumerate(enumerator, &key, &value))
	{
		certificate_t *cert = NULL;

		if (strcaseeq(key, "x509"))
		{
			cert = lib->creds->create(lib->creds, CRED_CERTIFICATE,
							CERT_X509, BUILD_FROM_FILE, value, BUILD_END);
		}
		else if (strcaseeq(key, "crl"))
		{
			cert = lib->creds->create(lib->creds, CRED_CERTIFICATE,
							CERT_X509_CRL, BUILD_FROM_FILE, value, BUILD_END);
		}
		else
		{
			fprintf(stderr, "certificate type '%s' not supported\n", key);
			enumerator->destroy(enumerator);
			return FALSE;
		}
		if (!cert)
		{
			fprintf(stderr, "loading %strusted certificate '%s' from '%s' "
					"failed\n", trusted ? "" : "un", key, value);
			enumerator->destroy(enumerator);
			return FALSE;
		}
		conftest->creds->add_cert(conftest->creds, trusted, cert);
	}
	enumerator->destroy(enumerator);
	return TRUE;
}

/**
 * Load certificates from the confiuguration file
 */
static bool load_certs(settings_t *settings, char *dir)
{
	char wd[PATH_MAX];

	if (getcwd(wd, sizeof(wd)) == NULL)
	{
		fprintf(stderr, "getting cwd failed: %s\n", strerror(errno));
		return FALSE;
	}
	if (chdir(dir) != 0)
	{
		fprintf(stderr, "opening directory '%s' failed: %s\n",
				dir, strerror(errno));
		return FALSE;
	}

	if (!load_trusted_cert(settings, TRUE) ||
		!load_trusted_cert(settings, FALSE))
	{
		return FALSE;
	}

	if (chdir(wd) != 0)
	{
		fprintf(stderr, "opening directory '%s' failed: %s\n",
				wd, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

/**
 * Load private keys from the confiuguration file
 */
static bool load_keys(settings_t *settings, char *dir)
{
	enumerator_t *enumerator;
	char *type, *value, wd[PATH_MAX];
	private_key_t *key;
	key_type_t key_type;

	if (getcwd(wd, sizeof(wd)) == NULL)
	{
		fprintf(stderr, "getting cwd failed: %s\n", strerror(errno));
		return FALSE;
	}
	if (chdir(dir) != 0)
	{
		fprintf(stderr, "opening directory '%s' failed: %s\n",
				dir, strerror(errno));
		return FALSE;
	}

	enumerator = settings->create_key_value_enumerator(settings, "keys");
	while (enumerator->enumerate(enumerator, &type, &value))
	{
		if (strcaseeq(type, "ecdsa"))
		{
			key_type = KEY_ECDSA;
		}
		else if (strcaseeq(type, "rsa"))
		{
			key_type = KEY_RSA;
		}
		else
		{
			fprintf(stderr, "unkown key type: '%s'\n", type);
			enumerator->destroy(enumerator);
			return FALSE;
		}
		key = lib->creds->create(lib->creds, CRED_PRIVATE_KEY, key_type,
								 BUILD_FROM_FILE, value, BUILD_END);
		if (!key)
		{
			fprintf(stderr, "loading %s key from '%s' failed\n", type, value);
			enumerator->destroy(enumerator);
			return FALSE;
		}
		conftest->creds->add_key(conftest->creds, key);
	}
	enumerator->destroy(enumerator);

	if (chdir(wd) != 0)
	{
		fprintf(stderr, "opening directory '%s' failed: %s\n",
				wd, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

/**
 * Load configured hooks
 */
static bool load_hooks()
{
	enumerator_t *enumerator;
	char *name, *pos, buf[64];
	hook_t *(*create)(char*);
	hook_t *hook;

	enumerator = conftest->test->create_section_enumerator(conftest->test,
														   "hooks");
	while (enumerator->enumerate(enumerator, &name))
	{
		pos = strchr(name, '-');
		if (pos)
		{
			snprintf(buf, sizeof(buf), "%.*s_hook_create", pos - name, name);
		}
		else
		{
			snprintf(buf, sizeof(buf), "%s_hook_create", name);
		}
		create = dlsym(RTLD_DEFAULT, buf);
		if (create)
		{
			hook = create(name);
			if (hook)
			{
				conftest->hooks->insert_last(conftest->hooks, hook);
				charon->bus->add_listener(charon->bus, &hook->listener);
			}
		}
		else
		{
			fprintf(stderr, "dlsym() for hook '%s' failed: %s\n", name, dlerror());
			enumerator->destroy(enumerator);
			return FALSE;
		}
	}
	enumerator->destroy(enumerator);
	return TRUE;
}

/**
 * atexit() cleanup handler
 */
static void cleanup()
{
	hook_t *hook;

	DESTROY_IF(conftest->suite);
	DESTROY_IF(conftest->test);
	lib->credmgr->remove_set(lib->credmgr, &conftest->creds->set);
	conftest->creds->destroy(conftest->creds);
	DESTROY_IF(conftest->actions);
	while (conftest->hooks->remove_last(conftest->hooks,
										(void**)&hook) == SUCCESS)
	{
		charon->bus->remove_listener(charon->bus, &hook->listener);
		hook->destroy(hook);
	}
	conftest->hooks->destroy(conftest->hooks);
	if (conftest->config)
	{
		if (charon->backends)
		{
			charon->backends->remove_backend(charon->backends,
											 &conftest->config->backend);
		}
		conftest->config->destroy(conftest->config);
	}
	free(conftest->suite_dir);
	free(conftest->test_dir);
	free(conftest);
	libcharon_deinit();
	libhydra_deinit();
	library_deinit();
}

/**
 * Main function, starts the conftest daemon.
 */
int main(int argc, char *argv[])
{
	struct sigaction action;
	int status = 0;
	sigset_t set;
	int sig;
	char *suite_file = "suite.conf", *test_file = NULL;
	file_logger_t *logger;

	if (!library_init(NULL))
	{
		library_deinit();
		return SS_RC_LIBSTRONGSWAN_INTEGRITY;
	}
	if (!libhydra_init("conftest"))
	{
		libhydra_deinit();
		library_deinit();
		return SS_RC_INITIALIZATION_FAILED;
	}
	if (!libcharon_init())
	{
		libcharon_deinit();
		libhydra_deinit();
		library_deinit();
		return SS_RC_INITIALIZATION_FAILED;
	}

	INIT(conftest,
		.creds = mem_cred_create(),
	);
	logger = file_logger_create(stdout, NULL, FALSE);
	logger->set_level(logger, DBG_ANY, LEVEL_CTRL);
	charon->bus->add_listener(charon->bus, &logger->listener);
	charon->file_loggers->insert_last(charon->file_loggers, logger);

	lib->credmgr->add_set(lib->credmgr, &conftest->creds->set);
	conftest->hooks = linked_list_create();
	conftest->config = config_create();

	atexit(cleanup);

	while (TRUE)
	{
		struct option long_opts[] = {
			{ "help", no_argument, NULL, 'h' },
			{ "version", no_argument, NULL, 'v' },
			{ "suite", required_argument, NULL, 's' },
			{ "test", required_argument, NULL, 't' },
			{ 0,0,0,0 }
		};
		switch (getopt_long(argc, argv, "", long_opts, NULL))
		{
			case EOF:
				break;
			case 'h':
				usage(stdout);
				return 0;
			case 'v':
				printf("strongSwan %s conftest\n", VERSION);
				return 0;
			case 's':
				suite_file = optarg;
				continue;
			case 't':
				test_file = optarg;
				continue;
			default:
				usage(stderr);
				return 1;
		}
		break;
	}

	if (!load_configs(suite_file, test_file))
	{
		return 1;
	}
	if (!lib->plugins->load(lib->plugins, NULL,
			conftest->test->get_str(conftest->test, "preload", "")))
	{
		return 1;
	}
	if (!charon->initialize(charon))
	{
		return 1;
	}
	if (!load_certs(conftest->suite, conftest->suite_dir) ||
		!load_certs(conftest->test, conftest->test_dir))
	{
		return 1;
	}
	if (!load_keys(conftest->suite, conftest->suite_dir) ||
		!load_keys(conftest->test, conftest->test_dir))
	{
		return 1;
	}
	if (!load_hooks())
	{
		return 1;
	}
	charon->backends->add_backend(charon->backends, &conftest->config->backend);
	conftest->config->load(conftest->config, conftest->test);
	conftest->config->load(conftest->config, conftest->suite);
	conftest->actions = actions_create();

	/* set up thread specific handlers */
	action.sa_handler = segv_handler;
	action.sa_flags = 0;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGINT);
	sigaddset(&action.sa_mask, SIGTERM);
	sigaddset(&action.sa_mask, SIGHUP);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);
	pthread_sigmask(SIG_SETMASK, &action.sa_mask, NULL);

	/* start thread pool */
	charon->start(charon);

	/* handle SIGINT/SIGTERM in main thread */
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_BLOCK, &set, NULL);

	while (sigwait(&set, &sig) == 0)
	{
		switch (sig)
		{
			case SIGINT:
			case SIGTERM:
				fprintf(stderr, "\nshutting down...\n");
				break;
			default:
				continue;
		}
		break;
	}
	return status;
}