/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <fcgi_stdio.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/wait.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "restconf_lib.h"


/*!
 */
int
notfound(FCGX_Request *r)
{
    char *path;

    clicon_debug(1, "%s", __FUNCTION__);
    path = FCGX_GetParam("DOCUMENT_URI", r->envp);
    FCGX_FPrintF(r->out, "Status: 404\r\n"); /* 404 not found */
    FCGX_FPrintF(r->out, "Content-Type: text/html\r\n\r\n");
    FCGX_FPrintF(r->out, "<h1>Not Found</h1>\n");
    FCGX_FPrintF(r->out, "The requested URL %s was not found on this server.\n",
		 path);
    return 0;
}

int
badrequest(FCGX_Request *r)
{
    char *path;

    clicon_debug(1, "%s", __FUNCTION__);
    path = FCGX_GetParam("DOCUMENT_URI", r->envp);
    FCGX_FPrintF(r->out, "Status: 400\r\n"); /* 400 bad request */
    FCGX_FPrintF(r->out, "Content-Type: text/html\r\n\r\n");
    FCGX_FPrintF(r->out, "<h1>Clixon Bad request/h1>\n");
    FCGX_FPrintF(r->out, "The requested URL %s or data is in some way badly formed.\n",
		 path);
    return 0;
}

int
conflict(FCGX_Request *r)
{
    clicon_debug(1, "%s", __FUNCTION__);
    FCGX_FPrintF(r->out, "Status: 409\r\n"); /* 409 Conflict */
    FCGX_FPrintF(r->out, "Content-Type: text/html\r\n\r\n");
    FCGX_FPrintF(r->out, "<h1>Data resource already exists</h1>\n");
    return 0;
}

/*! Specialization of clicon_debug with xml tree */
int 
clicon_debug_xml(int    dbglevel, 
		 char  *str,
		 cxobj *x)
{
    int   retval = -1;
    cbuf *cb;

    if ((cb = cbuf_new()) == NULL)
	goto done;
    if (clicon_xml2cbuf(cb, x, 0, 0) < 0)
	goto done;
    clicon_debug(1, "%s %s", str, cbuf_get(cb));
    retval = 0;
 done:
    if (cb!=NULL)
	cbuf_free(cb);
    return retval;
}

/*!
 * @param[in]  r        Fastcgi request handle
 */
static int
printparam(FCGX_Request *r, 
	   char         *e, 
	   int           dbgp)
{
    char *p = FCGX_GetParam(e, r->envp);

    if (dbgp)
	clicon_debug(1, "%s = '%s'", e, p?p:"");
    else
	FCGX_FPrintF(r->out, "%s = '%s'\n", e, p?p:"");
    return 0;
}

/*!
 * @param[in]  r        Fastcgi request handle
 */
int
test(FCGX_Request *r, 
     int           dbg)
{
    printparam(r, "QUERY_STRING", dbg);
    printparam(r, "REQUEST_METHOD", dbg);	
    printparam(r, "CONTENT_TYPE", dbg);	
    printparam(r, "CONTENT_LENGTH", dbg);	
    printparam(r, "SCRIPT_FILENAME", dbg);	
    printparam(r, "SCRIPT_NAME", dbg);	
    printparam(r, "REQUEST_URI", dbg);	
    printparam(r, "DOCUMENT_URI", dbg);	
    printparam(r, "DOCUMENT_ROOT", dbg);	
    printparam(r, "SERVER_PROTOCOL", dbg);	
    printparam(r, "GATEWAY_INTERFACE", dbg);
    printparam(r, "SERVER_SOFTWARE", dbg);
    printparam(r, "REMOTE_ADDR", dbg);
    printparam(r, "REMOTE_PORT", dbg);
    printparam(r, "SERVER_ADDR", dbg);
    printparam(r, "SERVER_PORT", dbg);
    printparam(r, "SERVER_NAME", dbg);
    printparam(r, "HTTP_COOKIE", dbg);
    printparam(r, "HTTPS", dbg);

    return 0;
}

/*!
 * @param[in]  r        Fastcgi request handle
 */
cbuf *
readdata(FCGX_Request *r)
{
    int   c;
    cbuf *cb;

    if ((cb = cbuf_new()) == NULL)
	return NULL;
    while ((c = FCGX_GetChar(r->in)) != -1)
	cprintf(cb, "%c", c);
    return cb;
}

typedef int (credentials_t)(clicon_handle h, FCGX_Request *r); 

static int nplugins = 0;
static plghndl_t *plugins = NULL;
static credentials_t *p_credentials = NULL; /* Credentials callback */

/*! Load all plugins you can find in CLICON_RESTCONF_DIR
 */
int 
restconf_plugin_load(clicon_handle h)
{
    int            retval = -1;
    char          *dir;
    int            ndp;
    struct dirent *dp = NULL;
    int            i;
    plghndl_t     *handle;
    char           filename[MAXPATHLEN];

    if ((dir = clicon_restconf_dir(h)) == NULL){
	clicon_err(OE_PLUGIN, 0, "clicon_restconf_dir not defined");
	goto quit;
    }
    /* Get plugin objects names from plugin directory */
    if((ndp = clicon_file_dirent(dir, &dp, "(.so)$", S_IFREG))<0)
	goto quit;

    /* Load all plugins */
    for (i = 0; i < ndp; i++) {
	snprintf(filename, MAXPATHLEN-1, "%s/%s", dir, dp[i].d_name);
	clicon_debug(1, "DEBUG: Loading plugin '%.*s' ...", 
		     (int)strlen(filename), filename);
	if ((handle = plugin_load(h, filename, RTLD_NOW)) == NULL)
	    goto quit;
	p_credentials    = dlsym(handle, "restconf_credentials");
	if ((plugins = realloc(plugins, (nplugins+1) * sizeof (*plugins))) == NULL) {
	    clicon_err(OE_UNIX, errno, "realloc");
	    goto quit;
	}
	plugins[nplugins++] = handle;
    }
    retval = 0;
quit:
    if (dp)
	free(dp);
    return retval;
}


/*! Unload all restconf plugins */
int
restconf_plugin_unload(clicon_handle h)
{
    int i;

    for (i = 0; i < nplugins; i++) 
	plugin_unload(h, plugins[i]);
    if (plugins){
	free(plugins);
	plugins = NULL;
    }
    nplugins = 0;
    return 0;
}

/*! Call plugin_start in all plugins
 */
int
restconf_plugin_start(clicon_handle h, 
		      int           argc, 
		      char        **argv)
{
    int i;
    plgstart_t *startfn;

    for (i = 0; i < nplugins; i++) {
	/* Call exit function is it exists */
	if ((startfn = dlsym(plugins[i], PLUGIN_START)) == NULL)
	    break;
	optind = 0;
	if (startfn(h, argc, argv) < 0) {
	    clicon_debug(1, "plugin_start() failed\n");
	    return -1;
	}
    }
    return 0;
}

int 
plugin_credentials(clicon_handle h,     
		   FCGX_Request *r,
		   int          *auth)
{
    int retval = -1;

    clicon_debug(1, "%s", __FUNCTION__);
    /* If no authentication callback then allow anything. Is this OK? */
    if (p_credentials == 0){
	*auth = 1;
	retval = 0;
	goto done;
    }
    if (p_credentials(h, r) < 0) 
	*auth = 0;
    else
	*auth = 1;
    retval = 0;
 done:
    return retval;
}
