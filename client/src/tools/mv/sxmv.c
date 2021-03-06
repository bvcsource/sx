/*
 *  Copyright (C) 2012-2014 Skylable Ltd. <info-copyright@skylable.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *  Special exception for linking this software with OpenSSL:
 *
 *  In addition, as a special exception, Skylable Ltd. gives permission to
 *  link the code of this program with the OpenSSL library and distribute
 *  linked combinations including the two. You must obey the GNU General
 *  Public License in all respects for all of the code used other than
 *  OpenSSL. You may extend this exception to your version of the program,
 *  but you are not obligated to do so. If you do not wish to do so, delete
 *  this exception statement from your version.
 */

#include "default.h"
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#include "sx.h"
#include "cmdline.h"
#include "version.h"
#include "libsxclient/src/misc.h"
#include "bcrumbs.h"

struct gengetopt_args_info args;

static int is_sx(const char *p) {
    return strncmp(p, "sx://", 5) == 0 || strncmp(p, SXC_ALIAS_PREFIX, strlen(SXC_ALIAS_PREFIX)) == 0;
}

static sxc_client_t *sx = NULL;

static void sighandler(int signal)
{
    struct termios tcur;
    if(sx)
	sxc_shutdown(sx, signal);

    /* work around for ctrl+c during getpassword() in the aes filter */
    tcgetattr(0, &tcur);
    tcur.c_lflag |= ECHO;
    tcsetattr(0, TCSANOW, &tcur);

    fprintf(stderr, "Process interrupted\n");
    exit(1);
}

static sxc_file_t *sxfile_from_arg(sxc_cluster_t **cluster, const char *arg, int require_remote_path) {
    sxc_file_t *file;

    if(is_sx(arg)) {
	sxc_uri_t *uri = sxc_parse_uri(sx, arg);

	if(!uri) {
	    fprintf(stderr, "ERROR: Bad uri %s: %s\n", arg, sxc_geterrmsg(sx));
	    return NULL;
	}
	if(!uri->volume || (require_remote_path && !uri->path)) {
	    if(!uri->volume)
		fprintf(stderr, "ERROR: Bad path %s: Missing volume name\n", arg);
	    else
		fprintf(stderr, "ERROR: Bad path %s: Missing file path\n", arg);
	    sxc_free_uri(uri);
	    return NULL;
	}
        if(!*cluster || strcmp(sxc_cluster_get_sslname(*cluster), uri->host)) {
	    sxc_cluster_free(*cluster);
	    *cluster = sxc_cluster_load_and_update(sx, uri->host, uri->profile);
	}
	if(!*cluster) {
	    fprintf(stderr, "ERROR: Failed to load config for %s: %s\n", uri->host, sxc_geterrmsg(sx));
	    if(strstr(sxc_geterrmsg(sx), SXBC_TOOLS_CFG_ERR))
		fprintf(stderr, SXBC_TOOLS_CFG_MSG, uri->host, uri->profile ? uri->profile : "", uri->profile ? "@" : "", uri->host);
            else if(strstr(sxc_geterrmsg(sx), SXBC_TOOLS_CONN_ERR))
                fprintf(stderr, SXBC_TOOLS_CONN_MSG);
	    else if(strstr(sxc_geterrmsg(sx), SXBC_TOOLS_CERT_ERR))
		fprintf(stderr, SXBC_TOOLS_CERT_MSG, uri->profile ? uri->profile : "", uri->profile ? "@" : "", uri->host);
	    else if(strstr(sxc_geterrmsg(sx), SXBC_TOOLS_INVALIDPROF_ERR))
                fprintf(stderr, SXBC_TOOLS_INVALIDPROF_MSG);
	    sxc_free_uri(uri);
	    return NULL;
	}

	file = sxc_file_remote(*cluster, uri->volume, uri->path, NULL);
	sxc_free_uri(uri);
	if(!file) {
	    sxc_cluster_free(*cluster);
            *cluster = NULL;
        }
    } else {
	fprintf(stderr, "ERROR: '%s' is not a remote path\n", arg);
	return NULL;
    }

    if(!file) {
	fprintf(stderr, "ERROR: Failed to create file object: %s\n", sxc_geterrmsg(sx));
	return NULL;
    }

    return file;
}

static int cmp_clusters(sxc_cluster_t *c1, sxc_cluster_t *c2) {
    const char *c1_uuid, *c2_uuid;
    if(!c1 || !c2)
        return -1;
    c1_uuid = sxc_cluster_get_uuid(c1);
    c2_uuid = sxc_cluster_get_uuid(c2);
    if(!c1_uuid || !c2_uuid)
        return -1;
    return strcmp(c1_uuid, c2_uuid);
}

int main(int argc, char **argv) {
    int ret = 1, fail = 0;
    sxc_file_t *src_file = NULL, *dst_file = NULL;
    const char *fname;
    char *filter_dir;
    sxc_logger_t log;
    sxc_cluster_t *cluster1 = NULL, *cluster2 = NULL;
    sxc_file_list_t *lst = NULL;
    unsigned int i;

    if(cmdline_parser(argc, argv, &args))
	exit(1);

    if(args.version_given) {
	printf("%s %s\n", CMDLINE_PARSER_PACKAGE, SRC_VERSION);
	cmdline_parser_free(&args);
	exit(0);
    }

    if(args.inputs_num < 2) {
	cmdline_parser_print_help();
	printf("\n");
	fprintf(stderr, "ERROR: Wrong number of arguments\n");
	cmdline_parser_free(&args);
	exit(1);
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    if(!(sx = sxc_init(SRC_VERSION, sxc_default_logger(&log, argv[0]), sxc_input_fn, NULL))) {
	cmdline_parser_free(&args);
	return 1;
    }

    if(args.config_dir_given && sxc_set_confdir(sx, args.config_dir_arg)) {
        fprintf(stderr, "ERROR: Could not set configuration directory %s: %s\n", args.config_dir_arg, sxc_geterrmsg(sx));
        ret = 1;
        goto main_err;
    }

    sxc_set_debug(sx, args.debug_flag);

    if(args.filter_dir_given) {
	filter_dir = strdup(args.filter_dir_arg);
    } else {
	const char *pt = sxi_getenv("SX_FILTER_DIR");
	if(pt)
	    filter_dir = strdup(pt);
	else
	    filter_dir = strdup(SX_FILTER_DIR);
    }
    if(!filter_dir) {
	fprintf(stderr, "ERROR: Failed to set filter dir\n");
	cmdline_parser_free(&args);
        sxc_shutdown(sx, 0);
	return 1;
    }
    if(sxc_filter_loadall(sx, filter_dir)) {
	fprintf(stderr, "WARNING: Failed to load filters: %s\n", sxc_geterrmsg(sx));
	sxc_clearerr(sx);
    }
    free(filter_dir);

    fname = args.inputs[args.inputs_num - 1];
    if(!(dst_file = sxfile_from_arg(&cluster1, fname, 0)))
	goto main_err;

    if(args.inputs_num > 2 && sxc_file_require_dir(dst_file)) {
        fprintf(stderr, "ERROR: %s\n", sxc_geterrmsg(sx));
        goto main_err;
    }

    if(!args.replica_wait_flag)
	sxc_set_flush_policy(sx, SXC_FLUSH_NOWAIT);

    lst = sxc_file_list_new(sx, args.recursive_given, 0);
    for(i = 0;i < args.inputs_num-1; i++) {
        int fallback = 1;
        fname = args.inputs[i];
        if(!(src_file = sxfile_from_arg(&cluster2, fname, !args.recursive_flag)))
            goto main_err;

        if(!cmp_clusters(cluster1, cluster2) && !strcmp(sxc_file_get_volume(src_file), sxc_file_get_volume(dst_file))) {
            int r;
            /* Here we know that both src and dest share the same volume and belong to the same cluster. We can use mass rename facility. */
            if((r = sxc_mass_rename(cluster1, src_file, dst_file, args.recursive_given))) {
                if(r == -2) {
                    /* Mass operation requested on a volume with filename processing filter, falling back to sxc_copy + sxc_rm method */
                    sxc_clearerr(sx);
                } else {
                    fprintf(stderr, "ERROR: %s\n", sxc_geterrmsg(sx));
                    if(strstr(sxc_geterrmsg(sx), SXBC_TOOLS_NOTFOUND_ERR) && !args.recursive_given)
                        fprintf(stderr, SXBC_TOOLS_NOTFOUND_MSG, fname);
                    fail = 1;
                    sxc_file_free(src_file);
                    src_file = NULL;
                    break;
                }
                sxc_clearerr(sx);
            } else
                fallback = 0;

            if(!fallback)
                sxc_file_free(src_file);
        }

        if(fallback) {
            /* TODO: more than one input requires directory as target,
             * and do the filename appending if target *is* a directory */
            if(sxc_copy_single(src_file, dst_file, args.recursive_flag, 0, 0, NULL, 1)) {
                fprintf(stderr, "ERROR: %s\n", sxc_geterrmsg(sx));
                if(!args.recursive_flag && strstr(sxc_geterrmsg(sx), SXBC_TOOLS_NOTFOUND_ERR) && is_sx(fname) && fname[strlen(fname) - 1] == '/')
    		    fprintf(stderr, SXBC_TOOLS_NOTFOUND_MSG, fname);
	        if((cluster1 || cluster2) && strstr(sxc_geterrmsg(sx), SXBC_TOOLS_VOL_ERR))
		    fprintf(stderr, SXBC_TOOLS_VOL_MSG, "", "", cluster1 ? sxc_cluster_get_sslname(cluster1) : sxc_cluster_get_sslname(cluster2));
	        fail = 1;
	        sxc_file_free(src_file);
	        src_file = NULL;
	        break;
            }

            if(sxc_file_list_add(lst, src_file, 1)) {
	        fprintf(stderr, "ERROR: Cannot add file list entry '%s': %s\n", fname, sxc_geterrmsg(sx));
                goto main_err;
            }
        }
	src_file = NULL;
    }

    if(sxc_rm(lst, 0)) {
        fprintf(stderr, "ERROR: Failed to move file(s): %s\n", sxc_geterrmsg(sx));
	fail = 1;
    }

    ret = fail ? 1 : 0;

 main_err:

    sxc_file_list_free(lst);
    sxc_file_free(src_file);
    sxc_file_free(dst_file);

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    sxc_cluster_free(cluster1);
    sxc_cluster_free(cluster2);
    sxc_shutdown(sx, 0);
    cmdline_parser_free(&args);

    return ret;
}
