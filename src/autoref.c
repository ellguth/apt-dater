/* apt-dater - terminal-based remote package update manager
 *
 * Authors:
 *   Andre Ellguth <ellguth@ibh.de>
 *   Thomas Liske <liske@ibh.de>
 *
 * Copyright Holder:
 *   2008-2012 (C) IBH IT-Service GmbH [http://www.ibh.de/apt-dater/]
 *
 * License:
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this package; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "apt-dater.h"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef FEAT_AUTOREF

/**
* We record installed/updatable packages on the hosts. When a host has been
* refreshed, we look at each package and put it in the following tree (this
* will be used later to detect if some hosts are outdated running the same
* distri):
*
* [ht_distris]
*   |
*   +--[Debian/4.0/etch]
*   |    +--[package1]
*   |    |    +--(version1)->(host1, host2, host3)
*   |    |    +--(version2)->(host5, host6, ...)
*   |    |    |
*   |    |   ...
*   |    +--[package2]
*   |    |
*   |   ...
*   +--[CentOS/4.7/Final]
*   |
*  ...
*
* [ ]: GHashTable
* ( ): GList
*
**/

#include <glib.h>

typedef struct _distri {
#ifndef NDEBUG
 etype _type;
#endif
 gchar     *lsb_distributor;
 gchar     *lsb_release;
 gchar     *lsb_codename;
 gchar     *uname_kernel;
 gchar     *uname_machine;
} Distri;

typedef struct _version {
#ifndef NDEBUG
 etype _type;
#endif
 gchar *version;
 time_t ts_first;
 GList *nodes;
} Version;

static GHashTable *ht_distris = NULL;

/* ====================[ begin: stuff taken from libdpkg.a ]==================== */
struct versionrevision {
  unsigned long epoch;
  const char *version;
  const char *revision;
};

static inline int cisdigit(int c) {
  return (c>='0') && (c<='9');
}

static int cisalpha(int c) {
  return ((c>='a') && (c<='z')) || ((c>='A') && (c<='Z'));
}

/* assume ascii; warning: evaluates x multiple times! */
#define order(x) ((x) == '~' ? -1 \
		: cisdigit((x)) ? 0 \
		: !(x) ? 0 \
		: cisalpha((x)) ? (x) \
		: (x) + 256)

static int verrevcmp(const char *val, const char *ref) {
  if (!val) val= "";
  if (!ref) ref= "";

  while (*val || *ref) {
    int first_diff= 0;

    while ( (*val && !cisdigit(*val)) || (*ref && !cisdigit(*ref)) ) {
      int vc= order(*val), rc= order(*ref);
      if (vc != rc) return vc - rc;
      val++; ref++;
    }

    while ( *val == '0' ) val++;
    while ( *ref == '0' ) ref++;
    while (cisdigit(*val) && cisdigit(*ref)) {
      if (!first_diff) first_diff= *val - *ref;
      val++; ref++;
    }
    if (cisdigit(*val)) return 1;
    if (cisdigit(*ref)) return -1;
    if (first_diff) return first_diff;
  }
  return 0;
}
/* =====================[ end: stuff taken from libdpkg.a ]===================== */


/* Creates a string from LSB fields used for the hashtable. */
static inline gchar *distri2str(const Distri *d, gchar *buf, const gint bsize) {
    snprintf(buf, bsize-1, "%s|%s|%s|%s|%s", d->lsb_distributor, d->lsb_release,
	     d->lsb_codename, d->uname_kernel, d->uname_machine);

    return buf;
}

/* Compares to distris. */
static gboolean distri_equal(gconstpointer a, gconstpointer b) {
    gchar da[0x1ff];
    gchar db[0x1ff];

    return g_str_equal(distri2str(a, da, sizeof(da)), distri2str(b, db, sizeof(db)));
}

/* Hash a distri. */
static guint distri_hash(gconstpointer key) {
    Distri *distri = (Distri *)key;
    gchar b[0x1ff];

    ASSERT_TYPE(distri, T_DISTRI);

    return g_str_hash(distri2str(distri, b, sizeof(b)));
}

static gint cmp_vers(gconstpointer a, gconstpointer b) {
    ASSERT_TYPE((Version *)a, T_VERSION);
    ASSERT_TYPE((Version *)b, T_VERSION);

    return -verrevcmp(((Version *)a)->version, ((Version *)b)->version);
}

static inline int test_pkg(const PkgNode *pkg) {
    return (pkg->flag & (HOST_STATUS_PKGEXTRA | HOST_STATUS_PKGBROKEN)) == 0;
}

/* Add PkgNode to package hashtable */
static void add_pkg(gpointer data, gpointer user_data) {
    PkgNode *pkg = (PkgNode *)data;
    GHashTable *ht_packages = (GHashTable *)(((gpointer *)user_data)[0]);
    HostNode *node = (HostNode *)(((gpointer *)user_data)[1]);

    ASSERT_TYPE(pkg, T_PKGNODE);
    ASSERT_TYPE(node, T_HOSTNODE);

    if (!test_pkg(pkg))
	return;

    gchar *v;
    if(((pkg->flag & HOST_STATUS_PKGUPDATE) ||
        (pkg->flag & HOST_STATUS_PKGKEPTBACK)) &&
        (pkg->data))
     v = pkg->data;
    else
     v = pkg->version;

    if(!v)
     return;

    /* Create packages hashtable if needed. */
    GList *l_versions = g_hash_table_lookup(ht_packages, pkg->package);

    Version _v;
#ifndef NDEBUG
    _v._type = T_VERSION;
#endif
    _v.version = v;

    /* Create version entry if needed. */
    Version *vers = NULL;
    GList *l_v = g_list_find_custom(l_versions, &_v, cmp_vers);
    if (l_v) {
	vers = (Version *)l_v->data;
	ASSERT_TYPE(vers, T_VERSION);
    }

    if(!vers) {
	vers = g_malloc(sizeof(Version));
#ifndef NDEBUG
	vers->_type = T_VERSION;
#endif
	vers->version = strdup(v);
	vers->ts_first = node->last_upd;
	vers->nodes = NULL;

	l_versions = g_list_prepend(l_versions, vers);
	g_hash_table_insert(ht_packages, g_strdup(pkg->package), l_versions);
    }
    else if (vers->ts_first > node->last_upd) {
	vers->ts_first = node->last_upd;
    }

    vers->nodes = g_list_prepend(vers->nodes, node);
}

/* Test if host should be observed by autoref. */
static inline int test_hostnode(const HostNode *node) {
    return (node->lsb_distributor &&
	    node->lsb_release &&
	    node->lsb_codename &&
	    node->uname_kernel &&
	    node->uname_machine);
}

/* Fetch package data from HostNode and feed it into the hashtables. */
void autoref_add_host_info(HostNode *node) {
    Distri distri;

    if(!test_hostnode(node))
	return;

    if (!ht_distris)
	ht_distris = g_hash_table_new(distri_hash, distri_equal);

#ifdef NDEBUG
#define ASSIGN_DIST(d, n, f) \
    (d).lsb_distributor = f((n)->lsb_distributor); \
    (d).lsb_release = f((n)->lsb_release); \
    (d).lsb_codename = f((n)->lsb_codename); \
    (d).uname_kernel = f((n)->uname_kernel); \
    (d).uname_machine = f((n)->uname_machine);
#else
#define ASSIGN_DIST(d, n, f) \
    (d)._type = T_DISTRI; \
    (d).lsb_distributor = f((n)->lsb_distributor); \
    (d).lsb_release = f((n)->lsb_release); \
    (d).lsb_codename = f((n)->lsb_codename); \
    (d).uname_kernel = f((n)->uname_kernel); \
    (d).uname_machine = f((n)->uname_machine);
#endif

    ASSIGN_DIST(distri, node, );

    /* Create distri hashtable if needed. */
    GHashTable *ht_packages = g_hash_table_lookup(ht_distris, &distri);
    if(!ht_packages) {
	ht_packages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	Distri *ndistri = g_malloc(sizeof(Distri));
#ifndef NDEBUG
	ndistri->_type = T_DISTRI;
#endif
	ASSIGN_DIST(*ndistri, node, g_strdup);

	g_hash_table_insert(ht_distris, ndistri, ht_packages);
    }

    /* Traverse package list and count installed versions. */
    gpointer data[2];
    data[0] = ht_packages;
    data[1] = node;
    g_list_foreach(node->packages, add_pkg, data);
}

/* Remove PkgNode from package hashtable */
static void rem_pkg(gpointer data, gpointer user_data) {
    PkgNode *pkg = (PkgNode *)data;
    GHashTable *ht_packages = (GHashTable *)(((gpointer *)user_data)[0]);
    HostNode *node = (HostNode *)(((gpointer *)user_data)[1]);

    ASSERT_TYPE(pkg, T_PKGNODE);
    ASSERT_TYPE(node, T_HOSTNODE);

    if (!test_pkg(pkg))
	return;

    /* Lookup packages hashtable. */
    GList *l_versions = g_hash_table_lookup(ht_packages, pkg->package);
    if(!l_versions)
	return;

    /* Lookup version list. */
    gchar *v = NULL;
    if(((pkg->flag & HOST_STATUS_PKGUPDATE) ||
        (pkg->flag & HOST_STATUS_PKGKEPTBACK)) &&
        (pkg->data))
     v = pkg->data;
    else
     v = pkg->version;

    Version _v;
#ifndef NDEBUG
    _v._type = T_VERSION;
#endif
    _v.version = v;

    Version *vers = NULL;
    GList *l_v = g_list_find_custom(l_versions, &_v, cmp_vers);
    if (l_v) {
	vers = (Version *)l_v->data;
	ASSERT_TYPE(vers, T_VERSION);
    }

    if(!vers)
	return;

    vers->nodes = g_list_remove(vers->nodes, node);
}

/* Remove package data of HostNode from the hashtable leaves. */
void autoref_rem_host_info(HostNode *node) {
    Distri distri;

    if(!test_hostnode(node))
	return;

    if (!ht_distris)
	return;

    ASSIGN_DIST(distri, node, );

    /* Lookup distri hashtable. */
    GHashTable *ht_packages = g_hash_table_lookup(ht_distris, &distri);
    if(!ht_packages)
	return;

    /* Traverse package list and count installed versions. */
    gpointer data[2];
    data[0] = ht_packages;
    data[1] = node;
    g_list_foreach(node->packages, rem_pkg, data);
}

/* Put a HostNode into refresh. */
static void trigger_refresh(gpointer data, gpointer user_data) {
    HostNode *node = (HostNode *)data;

    node->category = C_REFRESH_REQUIRED;
}

/* Record list of nodes which needs to be refreshed. */
GList *refresh_nodes = NULL;

/**
 * Add node to refresh list if:
 *  - last refresh is older than the time this version was
 *    seen by apt-dater first
 *  - the node is allowed to refresh
 **/
static void check_refresh(gpointer data, gpointer user_data) {
    HostNode *node = (HostNode *)data;
    int *ts_first = (int *)user_data;

    ASSERT_TYPE(node, T_HOSTNODE);

    if(node && (node->last_upd < *ts_first) &&
       ((node->forbid & HOST_FORBID_REFRESH) == 0) &&
       (g_list_find(refresh_nodes, node) == NULL))
	refresh_nodes = g_list_prepend(refresh_nodes, node);
}

/* Trigger refresh for any node which has older version data. */
static void add_refresh(gpointer value, gpointer user_data) {
    Version *version = (Version *)value;

    ASSERT_TYPE(version, T_VERSION);

    g_list_foreach(version->nodes, check_refresh, user_data);
}

/* Check if refresh is needed for each package. */
static void trigger_package(gpointer key, gpointer value, gpointer user_data) {
    GList *l_versions = (GList *)value;
    GHashTable *ht_packages = (GHashTable *)user_data;

    if(l_versions == NULL)
	return;

    l_versions = g_list_sort(l_versions, cmp_vers);
    if (value != l_versions)
	g_hash_table_insert(ht_packages, g_strdup(key), l_versions);

    Version *newest = (Version *)(l_versions->data);
    ASSERT_TYPE(newest, T_VERSION);

    GList *l_oldvers = g_list_next(l_versions);

    /* Only one version known => nothing todo. */
    if(l_oldvers == NULL)
	return;

    /* Any host, which has last_upd < version->ts_first needs to be refreshed. */
    g_list_foreach(l_oldvers, add_refresh, &( newest -> ts_first ));
}

/* Start refresh stuff for each distri. */
static void trigger_distri(gpointer key, gpointer value, gpointer user_data) {
    GHashTable *ht_packages = (GHashTable *)value;

    g_hash_table_foreach(ht_packages, trigger_package, ht_packages);
}

/**
* This function is called by refreshStats when no
* host remains in IN_REFRESH state.
**/
guint autoref_trigger_auto() {
    if (ht_distris) {
	g_hash_table_foreach(ht_distris, trigger_distri, NULL);

	guint ret = g_list_length(refresh_nodes);
	if (ret) {
	    /* Refresh now! */
	    g_list_foreach(refresh_nodes, trigger_refresh, NULL);
	    g_list_free(refresh_nodes);
	    refresh_nodes = NULL;
	    return ret;
	}
    }
    
    return 0;
}
#endif
