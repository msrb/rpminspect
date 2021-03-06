/*
 * Copyright (C) 2019-2020  Red Hat, Inc.
 * Author(s):  David Cantrell <dcantrell@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sys/queue.h>

#include "rpminspect.h"

/* Global variables */
static string_list_t *source = NULL;
static severity_t sev;
static waiverauth_t waiver;
static const char *remedy = NULL;

/*
 * Get the SOURCE tag from the RPM header and read in all of
 * the values from that tag and put them in the 'source' list.
 *
 * False is returned if the package lacks any Source entries.
 */
static bool init_source(const rpmfile_entry_t *file)
{
    rpmtd td = NULL;
    rpmFlags flags = HEADERGET_MINMEM | HEADERGET_EXT | HEADERGET_ARGV;
    string_entry_t *entry = NULL;
    const char *val = NULL;

    assert(file != NULL);

    /* get the SOURCE tag */
    td = rpmtdNew();

    if (!headerGet(file->rpm_header, RPMTAG_SOURCE, td, flags)) {
        /* source packages that lack Source files are allowed */
        return false;
    }

    /* walk the SOURCE tag and cram everything in to a list */
    source = calloc(1, sizeof(*source));
    assert(source != NULL);
    TAILQ_INIT(source);

    while ((val = rpmtdNextString(td)) != NULL) {
        entry = calloc(1, sizeof(*entry));
        assert(entry != NULL);
        entry->data = strdup(val);
        TAILQ_INSERT_TAIL(source, entry, items);
    }

    rpmtdFree(td);
    return true;
}

/* Returns true if this file is a Source file */
static bool is_source(const rpmfile_entry_t *file)
{
    bool ret = false;
    char *shortname = NULL;
    string_entry_t *entry = NULL;

    assert(file != NULL);

    /* Initialize the source list once for the run */
    if (source == NULL) {
        if (!init_source(file)) {
            /* source package lacks any Source archives */
            return false;
        }
    }

    /* The RPM header stores basenames */
    shortname = rindex(file->fullpath, '/') + 1;

    /* See if this file is a Source file */
    TAILQ_FOREACH(entry, source, items) {
        if (!strcmp(shortname, entry->data)) {
            return true;
        }
    }

    return ret;
}

/* Main driver for the 'upstream' inspection. */
static bool upstream_driver(struct rpminspect *ri, rpmfile_entry_t *file)
{
    bool result = true;
    char *before_sum = NULL;
    char *after_sum = NULL;
    char *diff_output = NULL;
    char *diff_head = NULL;
    char *shortname = NULL;
    char *msg = NULL;
    int exitcode;

    /* If we are not looking at a Source file, bail. */
    if (!is_source(file)) {
        return true;
    }

    /* Compare digests of source archive */
    shortname = basename(file->fullpath);

    /* Report what was found */
    if (file->peer_file == NULL) {
        xasprintf(&msg, _("New upstream source file `%s` appeared"), shortname)
        add_result(ri, sev, waiver, HEADER_UPSTREAM, msg, NULL, remedy);
        result = false;
    } else {
        /* compare checksums to see if the upstream sources changed */
        before_sum = checksum(file->peer_file);
        after_sum = checksum(file);

        if (strcmp(before_sum, after_sum)) {
            /* capture 'diff -u' output for text files */
            if (is_text_file(file->peer_file) && is_text_file(file)) {
                diff_head = diff_output = run_cmd(&exitcode, DIFF_CMD, "-u", file->peer_file->fullpath, file->fullpath, NULL);

                /* skip the two leading lines */
                if (strprefix(diff_head, "--- ")) {
                    diff_head = index(diff_head, '\n') + 1;
                }

                if (strprefix(diff_head, "+++ ")) {
                    diff_head = index(diff_head, '\n') + 1;
                }
            }

            /* report the changed file */
            xasprintf(&msg, _("Upstream source file `%s` changed content"), shortname);
            add_result(ri, sev, waiver, HEADER_UPSTREAM, msg, diff_head, remedy);
            result = false;

            /* clean up */
            free(diff_output);
        }
    }

    free(msg);
    return result;
}

/*
 * Main driver for the 'upstream' inspection.
 */
bool inspect_upstream(struct rpminspect *ri)
{
    bool result = true;
    const char *bv = NULL;
    const char *av = NULL;
    rpmpeer_entry_t *peer = NULL;
    rpmfile_entry_t *file = NULL;
    char *msg = NULL;

    assert(ri != NULL);

    /* Initialize before and after versions */
    TAILQ_FOREACH(peer, ri->peers, items) {
        if (bv && av) {
            break;
        }

        if (!headerIsSource(peer->after_hdr)) {
            continue;
        }

        TAILQ_FOREACH(file, peer->after_files, items) {
            if (strsuffix(file->localpath, SPEC_FILENAME_EXTENSION)) {
                bv = headerGetString(file->peer_file->rpm_header, RPMTAG_VERSION);
                av = headerGetString(file->rpm_header, RPMTAG_VERSION);
                break;
            }
        }
    }

    assert(bv != NULL);
    assert(av != NULL);

    /* Set result type based on version difference */
    if (strcmp(bv, av)) {
        /* versions changed */
        sev = RESULT_INFO;
        waiver = NOT_WAIVABLE;
        remedy = NULL;
    } else {
        /* versions are the same, likely maintenance */
        sev = RESULT_VERIFY;
        waiver = WAIVABLE_BY_ANYONE;
        remedy = REMEDY_UPSTREAM;
    }

    /* Run the main inspection */
    TAILQ_FOREACH(peer, ri->peers, items) {
        /* Only look at the files in SRPMs */
        if (!headerIsSource(peer->after_hdr)) {
            continue;
        }

        /* On the off chance the SRPM is empty, just ignore */
        if (peer->after_files == NULL || TAILQ_EMPTY(peer->after_files)) {
            continue;
        }

        /* Iterate over the SRPM files */
        TAILQ_FOREACH(file, peer->after_files, items) {
            if (!upstream_driver(ri, file)) {
                result = false;
            }
        }

        /* Report any removed source files from the SRPM */
        if (peer->before_files) {
            TAILQ_FOREACH(file, peer->before_files, items) {
                if (file->peer_file == NULL) {
                    xasprintf(&msg, _("Source RPM member `%s` removed"), basename(file->fullpath));
                    add_result(ri, sev, waiver, HEADER_UPSTREAM, msg, NULL, remedy);
                    result = false;
                    free(msg);
                }
            }
        }
    }

    /* Sound the everything-is-ok alarm if everything is, in fact, ok */
    if (result) {
        add_result(ri, RESULT_OK, NOT_WAIVABLE, HEADER_UPSTREAM, NULL, NULL, NULL);
    }

    /* Our static list of SourceN: spec file members, dump it */
    list_free(source, free);

    return result;
}
