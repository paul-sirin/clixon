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

 *
 *  Code for handling netconf rpc messages according to RFC 4741 and RFC 5277
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <assert.h>
#include <grp.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_netconf.h"
#include "netconf_lib.h"
#include "netconf_filter.h"
#include "netconf_plugin.h"
#include "netconf_rpc.h"

/*
 * <rpc [attributes]> 
    <!- - tag elements in a request from a client application - -> 
    </rpc> 
 */

/*! Get configuration
 * @param[in]  h       Clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] cb      Output xml stream. For reply
 * @param[out] cb_err  Error xml stream. For error reply
 * @param[out] xret    Return XML, error or OK
 * @note filter type subtree and xpath is supported, but xpath is preferred, and
 *              better performance and tested. Please use xpath.
 *
 *     <get-config> 
 *	 <source> 
 *	   <candidate/> | <running/> 
 *	 </source> 
 *     </get-config> 
 *
 *     <get-config> 
 *	 <source> 
 *	   <candidate/> | <running/> 
 *	 </source> 
 *	 <filter type="subtree"> 
 *	     <configuration> 
 *		 <!- - tag elements for each configuration element to return - -> 
 *	     </configuration> 
 *	 </filter> 
 *     </get-config> 
 *
 *  Example:
 *    <rpc><get-config><source><running /></source>
 *      <filter type="xpath" select="//SenderTwampIpv4"/>
 *    </get-config></rpc>]]>]]>
 * Variants of the functions where x-axis is the variants of the <filter> clause
 * and y-axis is whether a <filter><configuration> or <filter select=""> is present.
 *                  | no filter | filter subnet | filter xpath |
 * -----------------+-----------+---------------+--------------+
 * no config        |           |               |              |
 * -----------------+-----------+---------------+--------------+
 * config/select    |     -     |               |              |
 * -----------------+-----------+---------------+--------------+
 * Example requests of each:
 * no filter + no config
     <rpc><get-config><source><candidate/></source></get-config></rpc>]]>]]>
 * filter subnet + no config:
     <rpc><get-config><source><candidate/></source><filter/></get-config></rpc>]]>]]>
 * filter xpath + select all:
     <rpc><get-config><source><candidate/></source><filter type="xpath" select="/"/></get-config></rpc>]]>]]>
 * filter subtree + config:
     <rpc><get-config><source><candidate/></source><filter type="subtree"><configuration><interfaces><interface><ipv4><enabled/></ipv4></interface></interfaces></configuration></filter></get-config></rpc>]]>]]>
 * filter xpath + select:
     <rpc><get-config><source><candidate/></source><filter type="xpath" select="/interfaces/interface/ipv4"/></get-config></rpc>]]>]]>
 */
static int
netconf_get_config(clicon_handle h, 
		   cxobj        *xn, 
		   cxobj       **xret)
{
     cxobj      *xfilter; /* filter */
     int         retval = -1;
     char       *source;
     char       *ftype = NULL;
     cxobj      *xfilterconf; 
     cxobj      *xconf;

     if ((source = netconf_get_target(xn, "source")) == NULL){
	 clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			  "<error-tag>missing-element</error-tag>"
			  "<error-type>protocol</error-type>"
			  "<error-severity>error</error-severity>"
			  "<error-info><bad-element>source</bad-element></error-info>"
			  "</rpc-error></rpc-reply>");
	 goto ok;
     }
     /* ie <filter>...</filter> */
     if ((xfilter = xpath_first(xn, "filter")) != NULL) 
	 ftype = xml_find_value(xfilter, "type");
     if (ftype == NULL || strcmp(ftype, "xpath")==0){
	 if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	     goto done;	
     }
     else if (strcmp(ftype, "subtree")==0){
	 /* Default rfc filter is subtree. I prefer xpath and use it internally.
	    Get whole subtree and then filter aftwerwards. This is suboptimal.
	    Therefore please use xpath.
	  */
	 if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	     goto done;	
	 if (xfilter &&
	     (xfilterconf = xpath_first(xfilter, "//configuration"))!= NULL &&
	     (xconf = xpath_first(*xret, "/rpc-reply/data/configuration")) != NULL){
	     /* xml_filter removes parts of xml tree not matching */
	     if ((strcmp(xml_name(xfilterconf), xml_name(xconf))!=0) ||
		 xml_filter(xfilterconf, xconf) < 0){
		     clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
				      "<error-tag>operation-failed</error-tag>"
				      "<error-type>applicatio</error-type>"
				      "<error-severity>error</error-severity>"
				      "<error-info>filtering</error-info>"
				      "</rpc-error></rpc-reply>");
	     }
	 }
     }
     else{
	 clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			  "<error-tag>operation-failed</error-tag>"
			  "<error-type>applicatio</error-type>"
			  "<error-severity>error</error-severity>"
			  "<error-message>filter type not supported</error-message>"
			  "<error-info>type</error-info>"
			  "</rpc-error></rpc-reply>");
     }
 ok: /* netconf error is not fatal */
    retval = 0;
 done:
    return retval;
}

/*! Get options from netconf edit-config
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] op      Operation type, eg merge,replace,...
 * @param[out] testopt test option, eg set, test
 * @param[out] erropt  Error option, eg stop-on-error
 * @retval    -1       Fatal Error
 * @retval     0       parameter error, xret returns error
 * @retval     1       OK, op, testopt and erropt set
 * @example
 *  <edit-config>
 *     <config>...</config>
 *     <default-operation>(merge | none | replace)</default-operation> 
 *     <error-option>(stop-on-error | continue-on-error )</error-option> 
 *     <test-option>(set | test-then-set | test-only)</test-option> 
 *  </edit-config>
 */
static int
get_edit_opts(cxobj               *xn,
	      enum test_option    *testopt,
	      enum error_option   *erropt,
	      cxobj              **xret)
{
    int    retval = -1;
    cxobj *x;
    char  *optstr;
    
    if ((x = xpath_first(xn, "test-option")) != NULL){
	if ((optstr = xml_body(x)) != NULL){
	    if (strcmp(optstr, "test-then-set") == 0)
		*testopt = TEST_THEN_SET;
	    else
	    if (strcmp(optstr, "set") == 0)
		*testopt = SET;
	    else
	    if (strcmp(optstr, "test-only") == 0)
		*testopt = TEST_ONLY;
	    else
		goto parerr;
	}
    }
    if ((x = xpath_first(xn, "error-option")) != NULL){
	if ((optstr = xml_body(x)) != NULL){
	    if (strcmp(optstr, "stop-on-error") == 0)
		*erropt = STOP_ON_ERROR;
	    else
	    if (strcmp(optstr, "continue-on-error") == 0)
		*erropt = CONTINUE_ON_ERROR;
	    else
		goto parerr;
	}
    }
    retval = 1; /* hunky dory */
    return retval;
 parerr: /* parameter error, xret set */
    clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
		     "<error-tag>invalid-value</error-tag>"
		     "<error-type>protocol</error-type>"
		     "<error-severity>error</error-severity>"
		     "</rpc-error></rpc-reply>");
    return 0;
}

/*! Netconf edit configuration
  Write the change on a tmp file, then load that into candidate configuration.
    <edit-config> 
        <target> 
            <candidate/> 
        </target> 
    
    <!- - EITHER - -> 

        <config> 
            <configuration> 
                <!- - tag elements representing the data to incorporate - -> 
            </configuration> 
        </config> 

    <!- - OR - -> 
    
        <config-text> 
            <configuration-text> 
                <!- - tag elements inline configuration data in text format - -> 
            </configuration-text> 
        </config-text> 

    <!- - OR - -> 

        <url> 
            <!- - location specifier for file containing data - -> 
        </url> 
    
        <default-operation>(merge | none | replace)</default-operation> 
        <error-option>(stop-on-error | continue-on-error )</error-option> 
        <test-option>(set | test-then-set | test-only)</test-option> 
    <edit-config> 

CLIXON addition:
    <filter type="restconf" select="/data/profile=a" />

 *
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 * @retval     0       OK, xret points to valid return, either ok or rpc-error
 * @retval    -1       Error
 * only 'config' supported
 * error-option: only stop-on-error supported
 * test-option:  not supported
 *
 * @note erropt, testopt only supports default
 */
static int
netconf_edit_config(clicon_handle h,
		    cxobj        *xn, 
		    cxobj       **xret)
{
    int                 retval = -1;
    int                 optret;
    enum operation_type operation = OP_MERGE;
    enum test_option    testopt = TEST_THEN_SET;/* only supports this */
    enum error_option   erropt = STOP_ON_ERROR; /* only supports this */
    cxobj              *xc;       /* config */
    cxobj              *x;
    cxobj              *xfilter;
    char               *ftype = NULL;
    cxobj              *xcc;      /* child of config */
    char               *target;  /* db */
    cbuf               *cbxml = NULL;

    /* must have target, and it should be candidate */
    if ((target = netconf_get_target(xn, "target")) == NULL ||
	strcmp(target, "candidate")){
	clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			 "<error-tag>missing-element</error-tag>"
			 "<error-type>protocol</error-type>"
			 "<error-severity>error</error-severity>"
			 "<error-info><bad-element>target</bad-element></error-info>"
			 "</rpc-error></rpc-reply>");
	goto ok;
    }
    /* CLICON addition, eg <filter type="restconf" select=<api-path> /> */
    if ((xfilter = xpath_first(xn, "filter")) != NULL) {
	if ((ftype = xml_find_value(xfilter, "type")) != NULL)
	    if (strcmp(ftype,"restconf")){
		clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
				 "<error-tag>invalid-value</error-tag>"
				 "<error-type>protocol</error-type>"
				 "<error-severity>error</error-severity>"
				 "</rpc-error></rpc-reply>");
		goto ok;
	    }
    }
    if ((x = xpath_first(xn, "default-operation")) != NULL){
	if (xml_operation(xml_body(x), &operation) < 0){
	    clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			     "<error-tag>invalid-value</error-tag>"
			     "<error-type>protocol</error-type>"
			     "<error-severity>error</error-severity>"
			     "</rpc-error></rpc-reply>");
	    goto ok;
	}
    }
    if ((optret = get_edit_opts(xn, &testopt, &erropt, xret)) < 0)
	goto done;
    if (optret == 0) /* error in opt parameters */
	goto ok;
    /* not supported opts */
    if (testopt!=TEST_THEN_SET || erropt!=STOP_ON_ERROR){
	clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			 "<error-tag>operation-not-supported</error-tag>"
			 "<error-type>protocol</error-type>"
			 "<error-severity>error</error-severity>"
			 "</rpc-error></rpc-reply>");
	goto ok;
    }    

    /* operation is OP_REPLACE, OP_MERGE, or OP_NONE pass all to backend */
    if ((xc  = xpath_first(xn, "config")) != NULL){
#if 0
	/* application-specific code registers 'config' */
	if ((ret = netconf_plugin_callbacks(h, xc, xret)) < 0){
	    goto ok;
	}
#endif
	if ((cbxml = cbuf_new()) == NULL)
	    goto done;
	if ((xcc = xml_child_i(xc, 0)) != NULL)
	    if (clicon_xml2cbuf(cbxml, xcc, 0, 0) < 0)
		goto done;
	if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	     goto done;	
    }
 ok:
    retval = 0;
 done:
    if (cbxml)
	cbuf_free(cbxml);
    return retval;
}

/*! Netconf copy configuration
    <copy-config> 
        <target> 
            <candidate/> 
        </target> 
        <source> 
            <url> 
                <!- - location specifier for file containing the new configuration - -> 
            </url> 
        </source> 
    <copy-config> 
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
static int
netconf_copy_config(clicon_handle h,
		    cxobj        *xn, 
		    cxobj       **xret)	    
{
    int       retval = -1;
    char     *source;
    char     *target; /* filenames */

    if ((source = netconf_get_target(xn, "source")) == NULL){
	clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			 "<error-tag>missing-element</error-tag>"
			 "<error-type>protocol</error-type>"
			 "<error-severity>error</error-severity>"
			 "<error-info><bad-element>source</bad-element></error-info>"
			 "</rpc-error></rpc-reply>");
	goto ok;
    }
    if ((target = netconf_get_target(xn, "target")) == NULL){
	clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			 "<error-tag>missing-element</error-tag>"
			 "<error-type>protocol</error-type>"
			 "<error-severity>error</error-severity>"
			 "<error-info><bad-element>target</bad-element></error-info>"
			 "</rpc-error></rpc-reply>");
	goto ok;
    }
    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	goto done;	
 ok:
    retval = 0;
  done:
    return retval;
}

/*! Delete configuration
  <delete-config> 
        <target> 
            <candidate/> 
        </target> 
    </delete-config> 
    Delete a configuration datastore.  The <running>
    configuration datastore cannot be deleted.
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
static int
netconf_delete_config(clicon_handle h,
		      cxobj        *xn, 
		      cxobj       **xret)
{
    char              *target; /* filenames */
    int                retval = -1;

    if ((target = netconf_get_target(xn, "target")) == NULL ||
	strcmp(target, "running")==0){
	clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			 "<error-tag>missing-element</error-tag>"
			 "<error-type>protocol</error-type>"
			 "<error-severity>error</error-severity>"
			 "<error-info><bad-element>target</bad-element></error-info>"
			 "</rpc-error></rpc-reply>");
	goto ok;
    }
    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	goto done;	
 ok:
    retval = 0;
  done:
    return retval;
}


/*! Lock a database
    <lock> 
        <target> 
            <candidate/> 
        </target> 
    </lock> 
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
static int
netconf_lock(clicon_handle h,
	     cxobj        *xn, 
	     cxobj       **xret)
{
    int      retval = -1;
    char    *target;

    if ((target = netconf_get_target(xn, "target")) == NULL){
	 clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			  "<error-tag>missing-element</error-tag>"
			  "<error-type>protocol</error-type>"
			  "<error-severity>error</error-severity>"
			  "<error-info><bad-element>target</bad-element></error-info>"
			  "</rpc-error></rpc-reply>");
	goto ok;
    }
    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	goto done;
 ok:
    retval = 0;
  done:
    return retval;
}

/*! Unlock a database
   <unlock> 
        <target> 
            <candidate/> 
        </target> 
    </unlock> 
    XXX
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
static int
netconf_unlock(clicon_handle h, 
	       cxobj        *xn, 
	       cxobj       **xret)
{
    return netconf_lock(h, xn, xret);
}

/*! Close a (user) session
    <close-session/> 
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
*/
static int
netconf_close_session(clicon_handle h,
		      cxobj        *xn, 
		      cxobj       **xret)
{
    int retval = -1;

    cc_closed++;
    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Kill other user sessions
  <kill-session> 
        <session-id>PID</session-id> 
  </kill-session> 
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
static int
netconf_kill_session(clicon_handle h,
		     cxobj        *xn, 
		     cxobj       **xret)
{
    int    retval=-1;
    cxobj *xs;

    if ((xs = xpath_first(xn, "//session-id")) == NULL){
	 clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			  "<error-tag>missing-element</error-tag>"
			  "<error-type>protocol</error-type>"
			  "<error-severity>error</error-severity>"
			  "<error-info><bad-element>session-id</bad-element></error-info>"
			  "</rpc-error></rpc-reply>");
	 goto ok;
    }
    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	goto done;
 ok:
    retval = 0;
 done:
    return retval;
}
/*! Check the semantic consistency of candidate
    <validate/> 
    :validate
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
static int
netconf_validate(clicon_handle h, 
		 cxobj        *xn, 
		 cxobj       **xret)
{
    int    retval = -1;
    char  *target;

    if ((target = netconf_get_target(xn, "source")) == NULL){
	 clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
			  "<error-tag>missing-element</error-tag>"
			  "<error-type>protocol</error-type>"
			  "<error-severity>error</error-severity>"
			  "<error-info><bad-element>target</bad-element></error-info>"
			  "</rpc-error></rpc-reply>");
	 goto ok;
    }
    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	goto done;
 ok:
    retval = 0;
  done:
    return retval;
}

/*! Commit candidate -> running
    <commit/> 
    :candidate
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
static int
netconf_commit(clicon_handle h,
	       cxobj        *xn, 
	       cxobj       **xret)
{
    int      retval = -1;

    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Discard all changes in candidate / revert to running
    <discard-changes/> 
    :candidate
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
static int
netconf_discard_changes(clicon_handle h,
			cxobj        *xn, 
			cxobj       **xret)
{
    int     retval = -1;

    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, NULL) < 0)
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Called when a notification has happened on backend
 * and this session has registered for that event.
 * Filter it and forward it.
   <notification>
      <eventTime>2007-07-08T00:01:00Z</eventTime>
      <event xmlns="http://example.com/event/1.0">
         <eventClass>fault</eventClass>
         <reportingEntity>
             <card>Ethernet0</card>
         </reportingEntity>
         <severity>major</severity>
      </event>
   </notification>
 */
static int
netconf_notification_cb(int   s, 
			void *arg)
{
    cxobj             *xfilter = (cxobj *)arg; 
    char              *selector;
    struct clicon_msg *reply = NULL;
    int                eof;
    char              *event = NULL;
    int                retval = -1;
    cbuf              *cb;
    cxobj             *xe = NULL; /* event xml */
    cxobj             *xt = NULL; /* top xml */

    if (0){
	fprintf(stderr, "%s\n", __FUNCTION__); /* debug */
	xml_print(stderr, xfilter); /* debug */
    }
    /* get msg (this is the reason this function is called) */
    if (clicon_msg_rcv(s, &reply, &eof) < 0)
	goto done;
    /* handle close from remote end: this will exit the client */
    if (eof){
	clicon_err(OE_PROTO, ESHUTDOWN, "%s: Socket unexpected close", __FUNCTION__);
	close(s);
	errno = ESHUTDOWN;
	event_unreg_fd(s, netconf_notification_cb);
	if (xfilter)
	    xml_free(xfilter);
	goto done;
    }
    if (clicon_msg_decode(reply, &xt) < 0) 
	goto done;
    if ((xe = xpath_first(xt, "//event")) != NULL)
	event = xml_body(xe);
    
    /* parse event */
    if (0){ /* XXX CLICON events are not xml */
	    /* find and apply filter */
	if ((selector = xml_find_value(xfilter, "select")) == NULL)
	    goto done;
	if (xpath_first(xe, selector) == NULL) {
	    fprintf(stderr, "%s no match\n", __FUNCTION__); /* debug */
	}
    }
    /* create netconf message */
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "%s: cbuf_new", __FUNCTION__);
	goto done;
    }
    add_preamble(cb); /* Make it well-formed netconf xml */
    cprintf(cb, "<notification><event>%s</event></notification>", event); 
    add_postamble(cb);
    /* Send it to listening client on stdout */
    if (netconf_output(1, cb, "notification") < 0){
	cbuf_free(cb);
	goto done;
    }
    fflush(stdout);
    cbuf_free(cb);
    retval = 0;
  done:
    if (xt != NULL)
	xml_free(xt);
    if (reply)
	free(reply);
    return retval;
}

/*

    <create-subscription> 
       <stream>RESULT</stream> # If not present, events in the default NETCONF stream will be sent.
       <filter>XPATH-EXPR<(filter>
       <startTime/> # only for replay (NYI)
       <stopTime/>  # only for replay (NYI)
    </create-subscription> 
    Dont support replay
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK

 */
static int
netconf_create_subscription(clicon_handle h, 
			    cxobj        *xn, 
			    cxobj       **xret)
{
    int              retval = -1;
    cxobj           *xfilter; 
    int              s;
    char            *ftype;

    if ((xfilter = xpath_first(xn, "//filter")) != NULL){
	if ((ftype = xml_find_value(xfilter, "type")) != NULL){
	    if (strcmp(ftype, "xpath") != 0){
		clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
				 "<error-tag>operation-failed</error-tag>"
				 "<error-type>application</error-type>"
				 "<error-severity>error</error-severity>"
				 "<error-message>only xpath filter type supported</error-message>"
				 "<error-info>type</error-info>"
				 "</rpc-error></rpc-reply>");
		goto ok;
	    }
	}
    }
    if (clicon_rpc_netconf_xml(h, xml_parent(xn), xret, &s) < 0)
	goto done;
    if (event_reg_fd(s, 
		     netconf_notification_cb, 
		     xfilter?xml_dup(xfilter):NULL,
		     "notification socket") < 0)
	goto done;
 ok:
    retval = 0;
  done:
    return retval;
}

/*! The central netconf rpc dispatcher. Look at first tag and dispach to sub-functions.
 * Call plugin handler if tag not found. If not handled by any handler, return
 * error.
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] xret    Return XML, error or OK
 */
int
netconf_rpc_dispatch(clicon_handle h,
		     cxobj        *xn, 
		     cxobj       **xret)
{
    cxobj      *xe;
    int         ret = 0;
    
    xe = NULL;
    while ((xe = xml_child_each(xn, xe, CX_ELMNT)) != NULL) {
	if (strcmp(xml_name(xe), "get-config") == 0)
	    return netconf_get_config(h, xe, xret);
	else if (strcmp(xml_name(xe), "edit-config") == 0)
	    return netconf_edit_config(h, xe, xret);
        else if (strcmp(xml_name(xe), "copy-config") == 0)
	    return netconf_copy_config(h, xe, xret);
	else if (strcmp(xml_name(xe), "delete-config") == 0)
	    return netconf_delete_config(h, xe, xret);
	else if (strcmp(xml_name(xe), "lock") == 0) 
	    return netconf_lock(h, xe, xret);
	else if (strcmp(xml_name(xe), "unlock") == 0)
	    return netconf_unlock(h, xe, xret);
	else if (strcmp(xml_name(xe), "close-session") == 0)
	    return netconf_close_session(h, xe, xret);
	else if (strcmp(xml_name(xe), "kill-session") == 0) 
	    return netconf_kill_session(h, xe, xret);
	/* Validate capability :validate */
	else if (strcmp(xml_name(xe), "validate") == 0)
	   return netconf_validate(h, xe, xret);
	/* Candidate configuration capability :candidate */
	else if (strcmp(xml_name(xe), "commit") == 0)
	    return netconf_commit(h, xe, xret);
	else if (strcmp(xml_name(xe), "discard-changes") == 0)
	    return netconf_discard_changes(h, xe, xret);
	/* RFC 5277 :notification */
	else if (strcmp(xml_name(xe), "create-subscription") == 0)
	    return netconf_create_subscription(h, xe, xret);
	/* Others */
	else{
	    if ((ret = netconf_plugin_callbacks(h, xe, xret)) < 0)
	       return -1;
	   if (ret == 0){ /* not handled by callback */
		clicon_xml_parse(xret, "<rpc-reply><rpc-error>"
				 "<error-tag>operation-failed</error-tag>"
				 "<error-type>rpc</error-type>"
				 "<error-severity>error</error-severity>"
				 "<error-message>%s</error-message>"
				 "<error-info>Not recognized</error-info>"
				 "</rpc-error></rpc-reply>", xml_name(xe));
		return 0;
	   }
	   
       }
   }
    return ret;
}
