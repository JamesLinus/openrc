/*
   librc-depend
   rc service dependency and ordering
   */

/*
 * Copyright 2007-2008 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "librc.h"

#define GENDEP          RC_LIBDIR "/sh/gendepends.sh"

#define RC_DEPCONFIG    RC_SVCDIR "/depconfig"

static const char *bootlevel = NULL;

static char *get_shell_value(char *string)
{
	char *p = string;
	char *e;

	if (! string)
		return NULL;

	if (*p == '\'')
		p++;

	e = p + strlen(p) - 1;
	if (*e == '\n')
		*e-- = 0;
	if (*e == '\'')
		*e-- = 0;

	if (*p != 0)
		return p;

	return NULL;
}

void rc_deptree_free(RC_DEPTREE *deptree)
{
	RC_DEPINFO *di;
	RC_DEPINFO *di2;
	RC_DEPTYPE *dt;
	RC_DEPTYPE *dt2;

	if (! deptree)
		return;

	di = STAILQ_FIRST(deptree);
	while (di)
	{
		di2 = STAILQ_NEXT(di, entries);
		dt = STAILQ_FIRST(&di->depends);
		while (dt)
		{
			dt2 = STAILQ_NEXT(dt, entries);
			rc_stringlist_free(dt->services);
			free(dt->type);
			free(dt);
			dt = dt2;
		}
		free(di->service);
		free(di);
		di = di2;
	}
	free(deptree);
}
librc_hidden_def(rc_deptree_free)

static RC_DEPINFO *get_depinfo(const RC_DEPTREE *deptree,
			       const char *service)
{
	RC_DEPINFO *di;

	STAILQ_FOREACH(di, deptree, entries)
		if (strcmp(di->service, service) == 0)
			return di;

	return NULL;
}

static RC_DEPTYPE *get_deptype(const RC_DEPINFO *depinfo,
			       const char *type)
{
	RC_DEPTYPE *dt;

	STAILQ_FOREACH(dt, &depinfo->depends, entries)
		if (strcmp(dt->type, type) == 0)
			return dt;

	return NULL;
}

RC_DEPTREE *rc_deptree_load(void)
{
	FILE *fp;
	RC_DEPTREE *deptree;
	RC_DEPINFO *depinfo = NULL;
	RC_DEPTYPE *deptype = NULL;
	char *line = NULL;
	size_t len = 0;
	char *type;
	char *p;
	char *e;
	int i;

	if (!(fp = fopen(RC_DEPTREE_CACHE, "r")))
		return NULL;

	deptree = xmalloc(sizeof(*deptree));
	STAILQ_INIT(deptree);

	while ((rc_getline(&line, &len, fp)))
	{
		p = line;
		e = strsep(&p, "_");
		if (! e || strcmp(e, "depinfo") != 0)
			continue;

		e = strsep (&p, "_");
		if (! e || sscanf(e, "%d", &i) != 1)
			continue;

		if (! (type = strsep(&p, "_=")))
			continue;

		if (strcmp(type, "service") == 0)
		{
			/* Sanity */
			e = get_shell_value(p);
			if (! e || *e == '\0')
				continue;

			depinfo = xmalloc(sizeof(*depinfo));
			STAILQ_INIT(&depinfo->depends);
			depinfo->service = xstrdup(e);
			STAILQ_INSERT_TAIL(deptree, depinfo, entries);
			deptype = NULL;
			continue;
		}

		e = strsep(&p, "=");
		if (! e || sscanf(e, "%d", &i) != 1)
			continue;

		/* Sanity */
		e = get_shell_value(p);
		if (! e || *e == '\0')
			continue;

		if (! deptype || strcmp(deptype->type, type) != 0) {
			deptype = xmalloc(sizeof(*deptype));
			deptype->services = rc_stringlist_new();
			deptype->type = xstrdup (type);
			STAILQ_INSERT_TAIL(&depinfo->depends, deptype, entries);
		}

		rc_stringlist_add(deptype->services, e);
	}
	fclose(fp);
	free(line);
	
	return deptree;
}
librc_hidden_def(rc_deptree_load)

static bool valid_service(const char *runlevel, const char *service)
{
	RC_SERVICE state = rc_service_state(service);

	return ((strcmp (runlevel, bootlevel) != 0 &&
		 rc_service_in_runlevel(service, bootlevel)) ||
		rc_service_in_runlevel(service, runlevel) ||
		state & RC_SERVICE_COLDPLUGGED ||
		state & RC_SERVICE_STARTED);
}

static bool get_provided1(const char *runlevel, RC_STRINGLIST *providers,
			  RC_DEPTYPE *deptype,
			  const char *level, bool coldplugged,
			  RC_SERVICE state)
{
	RC_STRING *service;
	RC_SERVICE st;
	bool retval = false;
	bool ok;
	const char *svc;

	TAILQ_FOREACH(service, deptype->services, entries) {
		ok = true;
		svc = service->value;
		st = rc_service_state(svc);

		if (level)
			ok = rc_service_in_runlevel(svc, level);
		else if (coldplugged)
			ok = (st & RC_SERVICE_COLDPLUGGED &&
			      ! rc_service_in_runlevel(svc, runlevel) &&
			      ! rc_service_in_runlevel(svc, bootlevel));

		if (! ok)
			continue;

		switch (state) {
			case RC_SERVICE_STARTED:
				ok = (st & RC_SERVICE_STARTED);
				break;
			case RC_SERVICE_INACTIVE:
			case RC_SERVICE_STARTING:
			case RC_SERVICE_STOPPING:
				ok = (st & RC_SERVICE_STARTING ||
				      st & RC_SERVICE_STOPPING ||
				      st & RC_SERVICE_INACTIVE);
				break;
			default:
				break;
		}

		if (! ok)
			continue;

		retval = true;
		rc_stringlist_add(providers, svc);
	}

	return retval;
}

/* Work out if a service is provided by another service.
   For example metalog provides logger.
   We need to be able to handle syslogd providing logger too.
   We do this by checking whats running, then what's starting/stopping,
   then what's run in the runlevels and finally alphabetical order.

   If there are any bugs in rc-depend, they will probably be here as
   provided dependancy can change depending on runlevel state.
   */
static RC_STRINGLIST *get_provided (const RC_DEPINFO *depinfo,
				    const char *runlevel, int options)
{
	RC_DEPTYPE *dt;
	RC_STRINGLIST *providers = rc_stringlist_new();
	RC_STRING *service;

	dt = get_deptype(depinfo, "providedby");
	if (! dt)
		return providers;

	/* If we are stopping then all depends are true, regardless of state.
	   This is especially true for net services as they could force a restart
	   of the local dns resolver which may depend on net. */
	if (options & RC_DEP_STOP)
	{
		TAILQ_FOREACH(service, dt->services, entries)
			rc_stringlist_add(providers, service->value);
		return providers;
	}

	/* If we're strict or startng, then only use what we have in our
	 * runlevel and bootlevel. If we starting then check cold-plugged too. */
	if (options & RC_DEP_STRICT || options & RC_DEP_START)
	{

		TAILQ_FOREACH(service, dt->services, entries)
			if (rc_service_in_runlevel(service->value, runlevel) ||
			    rc_service_in_runlevel(service->value, bootlevel) ||
			    (options & RC_DEP_START &&
			     rc_service_state(service->value) & RC_SERVICE_COLDPLUGGED))
				rc_stringlist_add(providers, service->value);

		if (TAILQ_FIRST(providers))
			return providers;
	}

	/* OK, we're not strict or there were no services in our runlevel.
	   This is now where the logic gets a little fuzzy :)
	   If there is >1 running service then we return NULL.
	   We do this so we don't hang around waiting for inactive services and
	   our need has already been satisfied as it's not strict.
	   We apply this to our runlevel, coldplugged services, then bootlevel
	   and finally any running.*/
#define DO \
	if (TAILQ_FIRST(providers)) { \
		if (TAILQ_NEXT(TAILQ_FIRST(providers), entries)) { \
			rc_stringlist_free(providers); \
			providers = rc_stringlist_new(); \
		} \
		return providers; \
	}

	/* Anything in the runlevel has to come first */
	if (get_provided1 (runlevel, providers, dt, runlevel, false, RC_SERVICE_STARTED))
	{ DO }
	if (get_provided1 (runlevel, providers, dt, runlevel, false, RC_SERVICE_STARTING))
		return providers;
	if (get_provided1 (runlevel, providers, dt, runlevel, false, RC_SERVICE_STOPPED))
		return providers;

	/* Check coldplugged services */
	if (get_provided1 (runlevel, providers, dt, NULL, true, RC_SERVICE_STARTED))
	{ DO }
	if (get_provided1 (runlevel, providers, dt, NULL, true, RC_SERVICE_STARTING))
		return providers;

	/* Check bootlevel if we're not in it */
	if (bootlevel && strcmp (runlevel, bootlevel) != 0)
	{
		if (get_provided1 (runlevel, providers, dt, bootlevel, false, RC_SERVICE_STARTED))
		{ DO }
		if (get_provided1 (runlevel, providers, dt, bootlevel, false, RC_SERVICE_STARTING))
			return providers;
	}

	/* Check coldplugged services */
	if (get_provided1 (runlevel, providers, dt, NULL, true, RC_SERVICE_STOPPED))
	{ DO }

	/* Check manually started */
	if (get_provided1 (runlevel, providers, dt, NULL, false, RC_SERVICE_STARTED))
	{ DO }
	if (get_provided1 (runlevel, providers, dt, NULL, false, RC_SERVICE_STARTING))
		return providers;

	/* Nothing started then. OK, lets get the stopped services */
	if (get_provided1 (runlevel, providers, dt, runlevel, false, RC_SERVICE_STOPPED))
		return providers;

	if (bootlevel && (strcmp (runlevel, bootlevel) != 0)
	    && (get_provided1 (runlevel, providers, dt, bootlevel, false, RC_SERVICE_STOPPED)))
		return providers;

	/* Still nothing? OK, list all services */
	TAILQ_FOREACH(service, dt->services, entries)
		rc_stringlist_add(providers, service->value);

	return providers;
}

static void visit_service(const RC_DEPTREE *deptree,
			  const RC_STRINGLIST *types,
			  RC_STRINGLIST **sorted,
			  RC_STRINGLIST *visited,
			  const RC_DEPINFO *depinfo,
			  const char *runlevel, int options)
{
	RC_STRING *type;
	RC_STRING *service;
	RC_DEPTYPE *dt;
	RC_DEPINFO *di;
	RC_STRINGLIST *provided;
	RC_STRING *p;
	const char *svcname;

	/* Check if we have already visited this service or not */
	TAILQ_FOREACH(type, visited, entries)
		if (strcmp(type->value, depinfo->service) == 0)
			return;

	/* Add ourselves as a visited service */
	rc_stringlist_add(visited, depinfo->service);

	TAILQ_FOREACH(type, types, entries)
	{
		if (!(dt = get_deptype(depinfo, type->value)))
			continue;

		TAILQ_FOREACH(service, dt->services, entries) {
			if (! options & RC_DEP_TRACE ||
			    strcmp(type->value, "iprovide") == 0)
			{
				if (! *sorted)
					*sorted = rc_stringlist_new();
				rc_stringlist_add(*sorted, service->value);
				continue;
			}

			if (!(di = get_depinfo(deptree, service->value)))
				continue;
			provided = get_provided(di, runlevel, options);
		
			if (TAILQ_FIRST(provided)) {
				TAILQ_FOREACH(p, provided, entries) {
					di = get_depinfo(deptree, p->value);
					if (di &&
					    (strcmp(type->value, "ineed") == 0 ||
					     strcmp(type->value, "needsme") == 0 ||
					     valid_service(runlevel, di->service)))
						visit_service(deptree, types, sorted, visited, di,
							      runlevel, options | RC_DEP_TRACE);
				}
			}
			else if (di &&
				 (strcmp(type->value, "ineed") == 0 ||
				  strcmp(type->value, "needsme") == 0 ||
				  valid_service(runlevel, service->value)))
				visit_service(deptree, types, sorted, visited, di,
					      runlevel, options | RC_DEP_TRACE);

			rc_stringlist_free(provided);
		}
	}

	/* Now visit the stuff we provide for */
	if (options & RC_DEP_TRACE &&
	    (dt = get_deptype(depinfo, "iprovide")))
	{
		TAILQ_FOREACH(service, dt->services, entries) {
			if (!(di = get_depinfo(deptree, service->value)))
				continue;

			provided = get_provided(di, runlevel, options);
			TAILQ_FOREACH(p, provided, entries)
				if (strcmp (p->value, depinfo->service) == 0) {
					//visit_service (deptree, types, sorted, visited, di,
					//	       runlevel, options | RC_DEP_TRACE);
					break;
				}
			rc_stringlist_free(provided);
		}
	}

	/* We've visited everything we need, so add ourselves unless we
	   are also the service calling us or we are provided by something */
	svcname = getenv("RC_SVCNAME");
	if (! svcname || strcmp(svcname, depinfo->service) != 0)
		if (! get_deptype(depinfo, "providedby")) {
			if (! *sorted)
				*sorted = rc_stringlist_new();
			rc_stringlist_add(*sorted, depinfo->service);
		}
}

RC_STRINGLIST *rc_deptree_depend(const RC_DEPTREE *deptree,
				 const char *service, const char *type)
{
	RC_DEPINFO *di;
	RC_DEPTYPE *dt;
	RC_STRINGLIST *svcs;
	RC_STRING *svc;

	if (!(di = get_depinfo(deptree, service)) ||
	    ! (dt = get_deptype(di, type)))
	{
		errno = ENOENT;
		return NULL;
	}

	/* For consistency, we copy the array */
	svcs = rc_stringlist_new();
	TAILQ_FOREACH(svc, dt->services, entries)
		rc_stringlist_add(svcs, svc->value);

	return svcs;
}
librc_hidden_def(rc_deptree_depend)

RC_STRINGLIST *rc_deptree_depends (const RC_DEPTREE *deptree,
				   const RC_STRINGLIST *types,
				   const RC_STRINGLIST *services,
				   const char *runlevel, int options)
{
	RC_STRINGLIST *sorted = NULL;
	RC_STRINGLIST *visited = rc_stringlist_new();
	RC_DEPINFO *di;
	const RC_STRING *service;

	bootlevel = getenv ("RC_BOOTLEVEL");
	if (! bootlevel)
		bootlevel = RC_LEVEL_BOOT;

	TAILQ_FOREACH(service, services, entries) {
		if (! (di = get_depinfo(deptree, service->value))) {
			errno = ENOENT;
			continue;
		}
		if (types)
			visit_service(deptree, types, &sorted, visited,
				      di, runlevel, options);
	}

	rc_stringlist_free(visited);
	return sorted;
}
librc_hidden_def(rc_deptree_depends)

RC_STRINGLIST *rc_deptree_order(const RC_DEPTREE *deptree,
				const char *runlevel, int options)
{
	RC_STRINGLIST *list;
	RC_STRINGLIST *list2;
	RC_STRINGLIST *types;
	RC_STRINGLIST *services;

	bootlevel = getenv ("RC_BOOTLEVEL");
	if (! bootlevel)
		bootlevel = RC_LEVEL_BOOT;

	/* When shutting down, list all running services */
	if (strcmp (runlevel, RC_LEVEL_SINGLE) == 0 ||
	    strcmp (runlevel, RC_LEVEL_SHUTDOWN) == 0 ||
	    strcmp (runlevel, RC_LEVEL_REBOOT) == 0)
	{
		list = rc_services_in_state(RC_SERVICE_STARTED);

		list2 = rc_services_in_state (RC_SERVICE_INACTIVE);
		if (list2) {
			if (list) {
				TAILQ_CONCAT(list, list2, entries);
				free(list2);
			} else
				list = list2;
		}

		list2 = rc_services_in_state (RC_SERVICE_STARTING);
		if (list2) {
			if (list) {
				TAILQ_CONCAT(list, list2, entries);
				free(list2);
			} else
				list = list2;
		}
		TAILQ_CONCAT(list, list2, entries);
	} else {
		list = rc_services_in_runlevel (runlevel);

		/* Add coldplugged services */
		list2 = rc_services_in_state (RC_SERVICE_COLDPLUGGED);
		TAILQ_CONCAT(list, list2, entries);
		free(list2);

		/* If we're not the boot runlevel then add that too */
		if (strcmp (runlevel, bootlevel) != 0) {
			list2 = rc_services_in_runlevel (bootlevel);
			TAILQ_CONCAT(list, list2, entries);
			free(list2);
		}
	}

	/* Now we have our lists, we need to pull in any dependencies
	   and order them */
	types = rc_stringlist_new();
	rc_stringlist_add(types, "ineed");
	rc_stringlist_add(types, "iuse");
	rc_stringlist_add(types, "iafter");

	services = rc_deptree_depends(deptree, types, list, runlevel,
				      RC_DEP_STRICT | RC_DEP_TRACE | options);
	rc_stringlist_free (list);
	rc_stringlist_free (types);

	return services;
}
librc_hidden_def(rc_deptree_order)

bool rc_newer_than(const char *source, const char *target)
{
	struct stat buf;
	time_t mtime;
	bool newer = true;
	DIR *dp;
	struct dirent *d;
	char path[PATH_MAX];
	int serrno = errno;

	/* We have to exist */
	if (stat(source, &buf) != 0)
		return false;
	mtime = buf.st_mtime;

	/* Of course we are newer than targets that don't exist
	   such as broken symlinks */
	if (stat(target, &buf) != 0)
		return true;

	if (mtime < buf.st_mtime)
		return false;

	/* If not a dir then reset errno */
	if (! (dp = opendir(target))) {
		errno = serrno;
		return true;
	}

	/* Check if we're newer than all the entries in the dir */
	while ((d = readdir(dp))) {
		if (d->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "%s/%s", target, d->d_name);
		newer = rc_newer_than(source, path);
		if (! newer)
			break;
	}
	closedir(dp);

	return newer;
}
librc_hidden_def(rc_newer_than)

typedef struct deppair
{
	const char *depend;
	const char *addto;
} DEPPAIR;

static const DEPPAIR deppairs[] = {
	{ "ineed",	"needsme" },
	{ "iuse",	"usesme" },
	{ "iafter",	"ibefore" },
	{ "ibefore",	"iafter" },
	{ "iprovide",	"providedby" },
	{ NULL, NULL }
};

static const char *const depdirs[] =
{
	RC_SVCDIR "/starting",
	RC_SVCDIR "/started",
	RC_SVCDIR "/stopping",
	RC_SVCDIR "/inactive",
	RC_SVCDIR "/wasinactive",
	RC_SVCDIR "/failed",
	RC_SVCDIR "/coldplugged",
	RC_SVCDIR "/daemons",
	RC_SVCDIR "/options",
	RC_SVCDIR "/exclusive",
	RC_SVCDIR "/scheduled",
	NULL
};

bool rc_deptree_update_needed(void)
{
	bool newer = false;
	RC_STRINGLIST *config;
	RC_STRING *s;
	int i;

	/* Create base directories if needed */
	for (i = 0; depdirs[i]; i++)
		if (mkdir(depdirs[i], 0755) != 0 && errno != EEXIST)
			fprintf(stderr, "mkdir `%s': %s\n", depdirs[i], strerror (errno));

	/* Quick test to see if anything we use has changed and we have
	 * data in our deptree */
	if (! existss(RC_DEPTREE_CACHE) ||
	    ! rc_newer_than(RC_DEPTREE_CACHE, RC_INITDIR) ||
	    ! rc_newer_than(RC_DEPTREE_CACHE, RC_CONFDIR) ||
#ifdef RC_PKG_INITDIR
	    ! rc_newer_than(RC_DEPTREE_CACHE, RC_PKG_INITDIR) ||
#endif
#ifdef RC_PKG_CONFDIR
	    ! rc_newer_than(RC_DEPTREE_CACHE, RC_PKG_CONFDIR) ||
#endif
#ifdef RC_LOCAL_INITDIR
	    ! rc_newer_than(RC_DEPTREE_CACHE, RC_LOCAL_INITDIR) ||
#endif
#ifdef RC_LOCAL_CONFDIR
	    ! rc_newer_than(RC_DEPTREE_CACHE, RC_LOCAL_CONFDIR) ||
#endif
	    ! rc_newer_than(RC_DEPTREE_CACHE, "/etc/rc.conf"))
		return true;

	/* Some init scripts dependencies change depending on config files
	 * outside of baselayout, like syslog-ng, so we check those too. */
	config = rc_config_list(RC_DEPCONFIG);
	if (config) {
		TAILQ_FOREACH(s, config, entries) {
			if (! rc_newer_than(RC_DEPTREE_CACHE, s->value)) {
				newer = true;
				break;
			}
		}
		rc_stringlist_free(config);
	}

	return newer;
}
librc_hidden_def(rc_deptree_update_needed)

/* This is a 5 phase operation
   Phase 1 is a shell script which loads each init script and config in turn
   and echos their dependency info to stdout
   Phase 2 takes that and populates a depinfo object with that data
   Phase 3 adds any provided services to the depinfo object
   Phase 4 scans that depinfo object and puts in backlinks
   Phase 5 saves the depinfo object to disk
   */
bool rc_deptree_update(void)
{
	FILE *fp;
	RC_DEPTREE *deptree;
	RC_DEPTREE *providers;
	RC_DEPINFO *depinfo = NULL;
	RC_DEPINFO *depinfo_np;
	RC_DEPINFO *di;
	RC_DEPTYPE *deptype = NULL;
	RC_DEPTYPE *dt;
	RC_DEPTYPE *dt_np;
	RC_STRINGLIST *config;
	RC_STRING *s;
	RC_STRING *s2;
	RC_DEPTYPE *provide;
	char *line = NULL;
	size_t len = 0;
	char *depend;
	char *depends;
	char *service;
	char *type;
	size_t i;
	size_t k;
	size_t l;
	int retval = true;
	const char *sys = rc_sys();
	char *nosys;

	/* Some init scripts need RC_LIBDIR to source stuff
	   Ideally we should be setting our full env instead */
	if (! getenv("RC_LIBDIR"))
		setenv("RC_LIBDIR", RC_LIBDIR, 0);

	/* Phase 1 - source all init scripts and print dependencies */
	if (! (fp = popen(GENDEP, "r")))
		return false;

	deptree = xmalloc(sizeof(*deptree));
	STAILQ_INIT(deptree);

	config = rc_stringlist_new();

	while ((rc_getline(&line, &len, fp)))
	{
		depends = line;
		service = strsep(&depends, " ");
		if (! service || ! *service)
			continue;

		type = strsep(&depends, " ");
		if (! depinfo || strcmp(depinfo->service, service) != 0) {
			deptype = NULL;
			depinfo = get_depinfo(deptree, service);
			if (! depinfo) {	
				depinfo = xmalloc(sizeof(*depinfo));
				STAILQ_INIT(&depinfo->depends);
				depinfo->service = xstrdup(service);
				STAILQ_INSERT_TAIL(deptree, depinfo, entries);
			}
		}
		
		/* We may not have any depends */
		if (! type || ! depends)
			continue;

		/* Get the type */
		if (strcmp(type, "config") != 0) {
			if (! deptype || strcmp (deptype->type, type) != 0)
				deptype = get_deptype(depinfo, type);
			if (! deptype) {
				deptype = xmalloc(sizeof(*deptype));
				deptype->type = xstrdup(type);
				deptype->services = rc_stringlist_new();
				STAILQ_INSERT_TAIL(&depinfo->depends, deptype, entries);
			}
		}

		/* Now add each depend to our type.
		   We do this individually so we handle multiple spaces gracefully */
		while ((depend = strsep(&depends, " ")))
		{
			if (depend[0] == 0)
				continue;

			if (strcmp(type, "config") == 0) {
				rc_stringlist_add(config, depend);
				continue;
			}

			/* .sh files are not init scripts */
			l = strlen(depend);
			if (l > 2 &&
			    depend[l - 3] == '.' &&
			    depend[l - 2] == 's' &&
			    depend[l - 1] == 'h')
				continue;
			
			/* Remove our dependency if instructed */
			if (depend[0] == '!') {
				rc_stringlist_delete(deptype->services, depend + 1);
				continue;
			}

			rc_stringlist_add(deptype->services, depend);

			/* We need to allow `after *; before local;` to work.
			 * Conversely, we need to allow 'before *; after modules' also */
			/* If we're before something, remove us from the after list */
			if (strcmp(type, "ibefore") == 0) {
				if ((dt = get_deptype(depinfo, "iafter")))
					rc_stringlist_delete(dt->services, depend);
			}
			/* If we're after something, remove us from the before list */
			if (strcmp (type, "iafter") == 0 ||
			    strcmp (type, "ineed") == 0 ||
			    strcmp (type, "iuse") == 0) {
				if ((dt = get_deptype(depinfo, "ibefore")))
					rc_stringlist_delete(dt->services, depend);
			}
		}
	}
	free(line);
	pclose(fp);

	/* Phase 2 - if we're a special system, remove services that don't
	 * work for them. This doesn't stop them from being run directly. */
	if (sys) {
		len = strlen(sys);
		nosys = xmalloc(len + 3);
		nosys[0] = 'n';
		nosys[1] = 'o';
		for (i = 0; i < len; i++)
			nosys[i + 2] = (char) tolower((int) sys[i]);
		nosys[i + 2] = '\0';

		STAILQ_FOREACH_SAFE(depinfo, deptree, entries, depinfo_np)
			if ((deptype = get_deptype(depinfo, "keyword")))
				TAILQ_FOREACH(s, deptype->services, entries)
					if (strcmp (s->value, nosys) == 0) {
						provide = get_deptype(depinfo, "iprovide");
						STAILQ_REMOVE(deptree, depinfo, rc_depinfo, entries);
						STAILQ_FOREACH(di, deptree, entries) {
							STAILQ_FOREACH_SAFE(dt, &di->depends, entries, dt_np) {
								rc_stringlist_delete(dt->services, depinfo->service);
								if (provide)
									TAILQ_FOREACH(s2, provide->services, entries)
										rc_stringlist_delete(dt->services, s2->value);
								if (! TAILQ_FIRST(dt->services)) {
									STAILQ_REMOVE(&di->depends, dt, rc_deptype, entries);
									free(dt->type);
									free(dt->services);
									free(dt);
								}
							}
						}
					}
		free (nosys);
	}

	/* Phase 3 - add our providers to the tree */
	providers = xmalloc(sizeof(*providers));
	STAILQ_INIT(providers);
	STAILQ_FOREACH(depinfo, deptree, entries)
		if ((deptype = get_deptype(depinfo, "iprovide")))
			TAILQ_FOREACH(s, deptype->services, entries) {
				STAILQ_FOREACH(di, providers, entries)
					if (strcmp(di->service, s->value) == 0)
						break;
				if (! di) {
					di = xmalloc(sizeof(*di));
					STAILQ_INIT(&di->depends);
					di->service = xstrdup(s->value);
					STAILQ_INSERT_TAIL(providers, di, entries);
				}
			}
	STAILQ_CONCAT(deptree, providers);
	free(providers);

	/* Phase 4 - backreference our depends */
	STAILQ_FOREACH(depinfo, deptree, entries)
		for (i = 0; deppairs[i].depend; i++) {
			deptype = get_deptype(depinfo, deppairs[i].depend);
			if (! deptype)
				continue;
			TAILQ_FOREACH(s, deptype->services, entries) {
				di = get_depinfo(deptree, s->value);
				if (! di) {
					if (strcmp (deptype->type, "ineed") == 0)
						fprintf (stderr,
							 "Service `%s' needs non"
							 " existant service `%s'\n",
							 depinfo->service, s->value);
					continue;
				}

				dt = get_deptype(di, deppairs[i].addto);
				if (! dt) {
					dt = xmalloc(sizeof(*dt));
					dt->type = xstrdup(deppairs[i].addto);
					dt->services = rc_stringlist_new();
					STAILQ_INSERT_TAIL(&di->depends, dt, entries);
				}
				rc_stringlist_add(dt->services, depinfo->service);
			}
		}

	/* Phase 5 - save to disk
	   Now that we're purely in C, do we need to keep a shell parseable file?
	   I think yes as then it stays human readable
	   This works and should be entirely shell parseable provided that depend
	   names don't have any non shell variable characters in
	   */
	if ((fp = fopen (RC_DEPTREE_CACHE, "w"))) {
		i = 0;
		STAILQ_FOREACH(depinfo, deptree, entries) {
			fprintf(fp, "depinfo_%zu_service='%s'\n",
				i, depinfo->service);
			STAILQ_FOREACH(deptype, &depinfo->depends, entries) {
				k = 0;
				TAILQ_FOREACH(s, deptype->services, entries) {
					fprintf(fp,
						"depinfo_%zu_%s_%zu='%s'\n",
						i, deptype->type, k, s->value);
					k++;
				}
			}
			i++;
		}
		fclose(fp);
	} else {
		fprintf(stderr, "fopen `%s': %s\n",
			RC_DEPTREE_CACHE, strerror(errno));
		retval = false;
	}

	/* Save our external config files to disk */
	if (TAILQ_FIRST(config)) {
		if ((fp = fopen(RC_DEPCONFIG, "w"))) {
			TAILQ_FOREACH(s, config, entries)
				fprintf (fp, "%s\n", s->value);
			fclose(fp);
		} else {
			fprintf(stderr, "fopen `%s': %s\n",
				RC_DEPCONFIG, strerror(errno));
			retval = false;
		}
		rc_stringlist_free (config);
	} else {
		unlink(RC_DEPCONFIG);
	}

	rc_deptree_free(deptree);

	return retval;
}
librc_hidden_def(rc_deptree_update)
