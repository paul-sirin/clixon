/*
 *
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <errno.h>
#include <syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#include "clixon_backend_commit.h"
#include "backend_failsafe.h"

/*! Reset running and start in failsafe mode. If no failsafe then quit.
  Typically done when startup status is not OK so

failsafe      ----------------------+
                            reset    \ commit
running                   ----|-------+---------------> RUNNING FAILSAFE
                           \
tmp                         |---------------------->
 */
int
load_failsafe(clicon_handle h, char *phase)
{
    int   retval = -1;
    int   ret;
    char *db = "failsafe";
    cbuf *cbret = NULL;

    phase = phase == NULL ? "(unknown)" : phase;

    if ((cbret = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    if ((ret = xmldb_exists(h, db)) < 0)
        goto done;
    if (ret == 0){ /* No it does not exist, fail */
        clicon_err(OE_DB, 0, "%s failed and no Failsafe database found, exiting", phase);
        goto done;
    }
    /* Copy original running to tmp as backup (restore if error) */
    if (xmldb_copy(h, "running", "tmp") < 0)
        goto done;
    if (xmldb_db_reset(h, "running") < 0)
        goto done;
    ret = candidate_commit(h, db, cbret);
    if (ret != 1)
        if (xmldb_copy(h, "tmp", "running") < 0)
            goto done;
    if (ret < 0)
        goto done;
    if (ret == 0){
        clicon_err(OE_DB, 0, "%s failed, Failsafe database validation failed %s", phase, cbuf_get(cbret));
        goto done;
    }
    clicon_log(LOG_NOTICE, "%s failed, Failsafe database loaded ", phase);
    retval = 0;
    done:
    if (cbret)
        cbuf_free(cbret);
    return retval;
}
