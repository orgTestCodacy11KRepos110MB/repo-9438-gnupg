/* pkclist.c
 *	Copyright (c) 1997 by Werner Koch (dd9jn)
 *
 * This file is part of G10.
 *
 * G10 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * G10 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "options.h"
#include "packet.h"
#include "errors.h"
#include "keydb.h"
#include "memory.h"
#include "util.h"
#include "trustdb.h"
#include "ttyio.h"

static int
query_ownertrust( PKT_public_cert *pkc )
{
    char *p;
    size_t n;
    u32 keyid[2];

    keyid_from_pkc( pkc, keyid );
    tty_printf("No ownertrust specified for:\n"
	       "%4u%c/%08lX %s \"",
	      nbits_from_pkc( pkc ), pubkey_letter( pkc->pubkey_algo ),
	      (ulong)keyid[1], datestr_from_pkc( pkc ) );
    p = get_user_id( keyid, &n );
    tty_print_string( p, n ),
    m_free(p);
    tty_printf("\"\n\n"
"Please decide in how far do you trust this user to\n"
"correctly sign other users keys (looking at his passport,\n"
"checking the fingerprints from different sources ...)?\n\n"
" 1 = Don't know\n"
" 2 = I do NOT trust\n"
" 3 = I trust marginally\n"
" 4 = I trust fully\n"
" s = please show me more informations\n\n" );

    for(;;) {
	p = tty_get("Your decision? ");
	trim_spaces(p);
	tty_kill_prompt();
	if( *p && p[1] )
	    ;
	else if( *p == '?' ) {
	    tty_printf(
"It's up to you to assign a value here; this value will never be exported\n"
"to any 3rd party.  We need it to implement the web-of-trust; it has nothing\n"
"to do with the (implicitly created) web-of-certificates.\n");
	}
	else if( !p[1] && (*p >= '1' && *p <= '4') ) {
	    /* okay */
	    break;
	}
	else if( *p == 's' || *p == 'S' ) {
	    tty_printf("You will see a list of signators etc. here\n");
	}
	m_free(p); p = NULL;
    }
    m_free(p);
    return 0;
}



/****************
 * Check wether we can trust this pkc which has a trustlevel of TRUSTLEVEL
 * Returns: true if we trust.
 */
static int
do_we_trust( PKT_public_cert *pkc, int trustlevel )
{
    int rc;

    switch( trustlevel ) {
      case TRUST_UNKNOWN: /* No pubkey in trustDB: Insert and check again */
	rc = insert_trust_record( pkc );
	if( rc ) {
	    log_error("failed to insert it into the trustdb: %s\n",
						      g10_errstr(rc) );
	    return 0; /* no */
	}
	rc = check_trust( pkc, &trustlevel );
	if( rc )
	    log_fatal("trust check after insert failed: %s\n",
						      g10_errstr(rc) );
	if( trustlevel == TRUST_UNKNOWN || trustlevel == TRUST_EXPIRED )
	    BUG();
	return do_we_trust( pkc, trustlevel );

      case TRUST_EXPIRED:
	log_error("trust has expired: NOT yet implemented\n");
	return 0; /* no */

      case TRUST_UNDEFINED:
	if( opt.batch || opt.answer_no )
	    log_info("no info to calculate a trust probability\n");
	else {
	    query_ownertrust( pkc );
	}
	return 0; /* no */

      case TRUST_NEVER:
	log_info("We do NOT trust this key\n");
	return 0; /* no */

      case TRUST_MARGINAL:
	log_info("I'm not sure wether this keys really belongs to the owner\n"
		 "but I proceed anyway\n");
	return 1; /* yes */

      case TRUST_FULLY:
	log_info("This key probably belongs to the owner\n");
	return 1; /* yes */

      case TRUST_ULTIMATE:
	log_info("Our own key is always good.\n");
	return 1; /* yes */

      default: BUG();
    }


    /* Eventuell fragen falls der trustlevel nicht ausreichend ist */


    return 1; /* yes */
}



void
release_pkc_list( PKC_LIST pkc_list )
{
    PKC_LIST pkc_rover;

    for( ; pkc_list; pkc_list = pkc_rover ) {
	pkc_rover = pkc_list->next;
	free_public_cert( pkc_list->pkc );
	m_free( pkc_list );
    }
}

int
build_pkc_list( STRLIST remusr, PKC_LIST *ret_pkc_list )
{
    PKC_LIST pkc_list = NULL;
    int rc;

    if( !remusr ) { /* ask!!! */
	log_bug("ask for public key nyi\n");
    }
    else {
	for(; remusr; remusr = remusr->next ) {
	    PKT_public_cert *pkc;

	    pkc = m_alloc_clear( sizeof *pkc );
	    if( (rc = get_pubkey_byname( pkc, remusr->d )) ) {
		free_public_cert( pkc ); pkc = NULL;
		log_error("skipped '%s': %s\n", remusr->d, g10_errstr(rc) );
	    }
	    else if( !(rc=check_pubkey_algo(pkc->pubkey_algo)) ) {
		int trustlevel;

		rc = check_trust( pkc, &trustlevel );
		if( rc ) {
		    free_public_cert( pkc ); pkc = NULL;
		    log_error("error checking pkc of '%s': %s\n",
						      remusr->d, g10_errstr(rc) );
		}
		else if( do_we_trust( pkc, trustlevel ) ) {
		    /* note: do_we_trust may have changed the trustlevel */
		    PKC_LIST r;

		    r = m_alloc( sizeof *r );
		    r->pkc = pkc; pkc = NULL;
		    r->next = pkc_list;
		    r->mark = 0;
		    pkc_list = r;
		}
		else { /* we don't trust this pkc */
		    free_public_cert( pkc ); pkc = NULL;
		}
	    }
	    else {
		free_public_cert( pkc ); pkc = NULL;
		log_error("skipped '%s': %s\n", remusr->d, g10_errstr(rc) );
	    }
	}
    }


    if( !rc && !pkc_list ) {
	log_error("no valid addressees\n");
	rc = G10ERR_NO_USER_ID;
    }

    if( rc )
	release_pkc_list( pkc_list );
    else
	*ret_pkc_list = pkc_list;
    return rc;
}


