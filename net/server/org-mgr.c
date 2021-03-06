/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include "ccnet-db.h"
#include "org-mgr.h"

#include "log.h"

struct _CcnetOrgManagerPriv
{
    CcnetDB	*db;
};

static int open_db (CcnetOrgManager *manager);
static int check_db_table (CcnetDB *db);

CcnetOrgManager* ccnet_org_manager_new (CcnetSession *session)
{
    CcnetOrgManager *manager = g_new0 (CcnetOrgManager, 1);

    manager->session = session;
    manager->priv = g_new0 (CcnetOrgManagerPriv, 1);

    return manager;
}

int
ccnet_org_manager_prepare (CcnetOrgManager *manager)
{
    return open_db (manager);
}

static CcnetDB *
open_sqlite_db (CcnetOrgManager *manager)
{
    CcnetDB *db = NULL;
    char *db_dir;
    char *db_path;

    db_dir = g_build_filename (manager->session->config_dir, "OrgMgr", NULL);
    if (checkdir_with_mkdir(db_dir) < 0) {
        ccnet_error ("Cannot open db dir %s: %s\n", db_dir,
                     strerror(errno));
        g_free (db_dir);
        return NULL;
    }
    g_free (db_dir);

    db_path = g_build_filename (manager->session->config_dir, "OrgMgr",
                                "orgmgr.db", NULL);
    db = ccnet_db_new_sqlite (db_path);

    g_free (db_path);

    return db;
}

static int
open_db (CcnetOrgManager *manager)
{
    CcnetDB *db = NULL;

    switch (ccnet_db_type(manager->session->db)) {
    case CCNET_DB_TYPE_SQLITE:
        db = open_sqlite_db (manager);
        break;
    case CCNET_DB_TYPE_PGSQL:
    case CCNET_DB_TYPE_MYSQL:
        db = manager->session->db;
        break;
    }

    if (!db)
        return -1;
    
    manager->priv->db = db;
    return check_db_table (db);
}

void ccnet_org_manager_start (CcnetOrgManager *manager)
{
}

/* -------- Group Database Management ---------------- */

static int check_db_table (CcnetDB *db)
{
    char *sql;

    int db_type = ccnet_db_type (db);
    if (db_type == CCNET_DB_TYPE_MYSQL) {
        sql = "CREATE TABLE IF NOT EXISTS Organization (org_id INTEGER"
            " PRIMARY KEY AUTO_INCREMENT, org_name VARCHAR(255),"
            " url_prefix VARCHAR(255), creator VARCHAR(255), ctime BIGINT,"
            " UNIQUE INDEX (url_prefix))"
            "ENGINE=INNODB";
        if (ccnet_db_query (db, sql) < 0)
            return -1;
        
        sql = "CREATE TABLE IF NOT EXISTS OrgUser (org_id INTEGER, "
            "email VARCHAR(255), is_staff BOOL NOT NULL, "
            "INDEX (email), PRIMARY KEY (org_id, email))"
            "ENGINE=INNODB";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE TABLE IF NOT EXISTS OrgGroup (org_id INTEGER, "
            "group_id INTEGER, INDEX (group_id), "
            "PRIMARY KEY (org_id, group_id))"
            "ENGINE=INNODB";
        if (ccnet_db_query (db, sql) < 0)
            return -1;
        
    } else if (db_type == CCNET_DB_TYPE_SQLITE) {
        sql = "CREATE TABLE IF NOT EXISTS Organization (org_id INTEGER"
            " PRIMARY KEY AUTOINCREMENT, org_name VARCHAR(255),"
            " url_prefix VARCHAR(255), "
            " creator VARCHAR(255), ctime BIGINT)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE UNIQUE INDEX IF NOT EXISTS url_prefix_indx on "
            "Organization (url_prefix)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;
        
        sql = "CREATE TABLE IF NOT EXISTS OrgUser (org_id INTEGER, "
            "email TEXT, is_staff bool NOT NULL)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE INDEX IF NOT EXISTS email_indx on "
            "OrgUser (email)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE UNIQUE INDEX IF NOT EXISTS orgid_email_indx on "
            "OrgUser (org_id, email)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE TABLE IF NOT EXISTS OrgGroup (org_id INTEGER, "
            "group_id INTEGER)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE INDEX IF NOT EXISTS groupid_indx on OrgGroup (group_id)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE UNIQUE INDEX IF NOT EXISTS org_group_indx on "
            "OrgGroup (org_id, group_id)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;
    } else if (db_type == CCNET_DB_TYPE_PGSQL) {
        sql = "CREATE TABLE IF NOT EXISTS Organization (org_id SERIAL"
            " PRIMARY KEY, org_name VARCHAR(255),"
            " url_prefix VARCHAR(255), creator VARCHAR(255), ctime BIGINT,"
            " UNIQUE (url_prefix))";
        if (ccnet_db_query (db, sql) < 0)
            return -1;
        
        sql = "CREATE TABLE IF NOT EXISTS OrgUser (org_id INTEGER, "
            "email VARCHAR(255), is_staff INTEGER NOT NULL, "
            "UNIQUE (org_id, email))";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        if (!pgsql_index_exists (db, "orguser_email_idx")) {
            sql = "CREATE INDEX orguser_email_idx ON OrgUser (email)";
            if (ccnet_db_query (db, sql) < 0)
                return -1;
        }

        sql = "CREATE TABLE IF NOT EXISTS OrgGroup (org_id INTEGER, "
            "group_id INTEGER, "
            "UNIQUE (org_id, group_id))";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        if (!pgsql_index_exists (db, "orggroup_groupid_idx")) {
            sql = "CREATE INDEX orggroup_groupid_idx ON OrgGroup (group_id)";
            if (ccnet_db_query (db, sql) < 0)
                return -1;
        }
    }

    return 0;
}

int ccnet_org_manager_create_org (CcnetOrgManager *mgr,
                                  const char *org_name,
                                  const char *url_prefix,
                                  const char *creator,
                                  GError **error)
{
    CcnetDB *db = mgr->priv->db;
    gint64 now = get_current_time();
    int rc;

    rc = ccnet_db_statement_query (db,
                                   "INSERT INTO Organization(org_name, url_prefix,"
                                   " creator, ctime) VALUES (?, ?, ?, ?)",
                                   4, "string", org_name, "string", url_prefix,
                                   "string", creator, "int64", now);
    
    if (rc < 0) {
        g_set_error (error, CCNET_DOMAIN, 0, "Failed to create organization");
        return -1;
    }

    int org_id = ccnet_db_statement_get_int (db,
                                             "SELECT org_id FROM Organization WHERE "
                                             "url_prefix = ?", 1, "string", url_prefix);
    if (org_id < 0) {
        g_set_error (error, CCNET_DOMAIN, 0, "Failed to create organization");
        return -1;
    }

    rc = ccnet_db_statement_query (db, "INSERT INTO OrgUser values (?, ?, ?)",
                                   3, "int", org_id, "string", creator, "int", 1);
    if (rc < 0) {
        ccnet_db_statement_query (db, "DELETE FROM Organization WHERE org_id=?",
                                  1, "int", org_id);
        g_set_error (error, CCNET_DOMAIN, 0, "Failed to create organization");
        return -1;
    }
    
    return org_id;
}

int
ccnet_org_manager_remove_org (CcnetOrgManager *mgr,
                              int org_id,
                              GError **error)
{
    CcnetDB *db = mgr->priv->db;

    ccnet_db_statement_query (db, "DELETE FROM Organization WHERE org_id = ?",
                              1, "int", org_id);

    ccnet_db_statement_query (db, "DELETE FROM OrgUser WHERE org_id = %d",
                              1, "int", org_id);

    ccnet_db_statement_query (db, "DELETE FROM OrgGroup WHERE org_id = %d",
                              1, "int", org_id);

    return 0;
}


static gboolean
get_all_orgs_cb (CcnetDBRow *row, void *data)
{
    GList **p_list = data;
    CcnetOrganization *org = NULL;
    int org_id;
    const char *org_name;
    const char *url_prefix;
    const char *creator;
    gint64 ctime;

    org_id = ccnet_db_row_get_column_int (row, 0);
    org_name = ccnet_db_row_get_column_text (row, 1);
    url_prefix = ccnet_db_row_get_column_text (row, 2);
    creator = ccnet_db_row_get_column_text (row, 3);
    ctime = ccnet_db_row_get_column_int64 (row, 4);

    org = g_object_new (CCNET_TYPE_ORGANIZATION,
                        "org_id", org_id,
                        "org_name", org_name,
                        "url_prefix", url_prefix,
                        "creator", creator,
                        "ctime", ctime,
                        NULL);

    *p_list = g_list_prepend (*p_list, org);

    return TRUE;
}

GList *
ccnet_org_manager_get_all_orgs (CcnetOrgManager *mgr,
                                int start,
                                int limit)
{
    CcnetDB *db = mgr->priv->db;
    char *sql;
    GList *ret = NULL;
    int rc;

    if (start == -1 && limit == -1) {
        sql = "SELECT * FROM Organization ORDER BY org_id";
        rc = ccnet_db_statement_foreach_row (db, sql, get_all_orgs_cb, &ret, 0);
    } else {
        sql = "SELECT * FROM Organization ORDER BY org_id LIMIT ? OFFSET ?";
        rc = ccnet_db_statement_foreach_row (db, sql, get_all_orgs_cb, &ret,
                                             2, "int", limit, "int", start);
    }

    if (rc < 0)
        return NULL;

    return g_list_reverse (ret);
}

static gboolean
get_org_cb (CcnetDBRow *row, void *data)
{
    CcnetOrganization **p_org = data;
    int org_id;
    const char *org_name;
    const char *url_prefix;
    const char *creator;
    gint64 ctime;

    org_id = ccnet_db_row_get_column_int (row, 0);    
    org_name = ccnet_db_row_get_column_text (row, 1);
    url_prefix = ccnet_db_row_get_column_text (row, 2);
    creator = ccnet_db_row_get_column_text (row, 3);
    ctime = ccnet_db_row_get_column_int64 (row, 4);

    *p_org = g_object_new (CCNET_TYPE_ORGANIZATION,
                           "org_id", org_id,
                           "org_name", org_name,
                           "url_prefix", url_prefix,
                           "creator", creator,
                           "ctime", ctime,
                           NULL);
    return FALSE;
}

CcnetOrganization *
ccnet_org_manager_get_org_by_url_prefix (CcnetOrgManager *mgr,
                                         const char *url_prefix,
                                         GError **error)
{
    CcnetDB *db = mgr->priv->db;
    char *sql;
    CcnetOrganization *org = NULL;

    sql = "SELECT org_id, org_name, url_prefix, creator,"
        " ctime FROM Organization WHERE url_prefix = ?";

    if (ccnet_db_statement_foreach_row (db, sql, get_org_cb, &org,
                                        1, "string", url_prefix) < 0) {
        return NULL;
    }

    return org;
}

CcnetOrganization *
ccnet_org_manager_get_org_by_id (CcnetOrgManager *mgr,
                                 int org_id,
                                 GError **error)
{
    CcnetDB *db = mgr->priv->db;
    char *sql;
    CcnetOrganization *org = NULL;

    sql = "SELECT org_id, org_name, url_prefix, creator,"
        " ctime FROM Organization WHERE org_id = ?";

    if (ccnet_db_statement_foreach_row (db, sql, get_org_cb, &org,
                                        1, "int", org_id) < 0) {
        return NULL;
    }

    return org;
}

int
ccnet_org_manager_add_org_user (CcnetOrgManager *mgr,
                                int org_id,
                                const char *email,
                                int is_staff,
                                GError **error)
{
    CcnetDB *db = mgr->priv->db;

    return ccnet_db_statement_query (db, "INSERT INTO OrgUser values (?, ?, ?)",
                                     3, "int", org_id, "string", email,
                                     "int", is_staff);
}

int
ccnet_org_manager_remove_org_user (CcnetOrgManager *mgr,
                                   int org_id,
                                   const char *email,
                                   GError **error)
{
    CcnetDB *db = mgr->priv->db;

    return ccnet_db_statement_query (db, "DELETE FROM OrgUser WHERE org_id=? AND "
                                     "email=?", 2, "int", org_id, "string", email);
}

static gboolean
get_orgs_by_user_cb (CcnetDBRow *row, void *data)
{
    GList **p_list = (GList **)data;
    CcnetOrganization *org = NULL;
    int org_id;
    const char *email;
    int is_staff;
    const char *org_name;
    const char *url_prefix;
    const char *creator;
    gint64 ctime;

    org_id = ccnet_db_row_get_column_int (row, 0);
    email = (char *) ccnet_db_row_get_column_text (row, 1);
    is_staff = ccnet_db_row_get_column_int (row, 2);
    org_name = (char *) ccnet_db_row_get_column_text (row, 3);
    url_prefix = (char *) ccnet_db_row_get_column_text (row, 4);
    creator = (char *) ccnet_db_row_get_column_text (row, 5);
    ctime = ccnet_db_row_get_column_int64 (row, 6);
    
    org = g_object_new (CCNET_TYPE_ORGANIZATION,
                        "org_id", org_id,
                        "email", email,
                        "is_staff", is_staff,
                        "org_name", org_name,
                        "url_prefix", url_prefix,
                        "creator", creator,
                        "ctime", ctime,
                        NULL);
    *p_list = g_list_prepend (*p_list, org);
        
    return TRUE;
}

GList *
ccnet_org_manager_get_orgs_by_user (CcnetOrgManager *mgr,
                                   const char *email,
                                   GError **error)
{
    CcnetDB *db = mgr->priv->db;
    char *sql;
    GList *ret = NULL;

    sql = "SELECT t1.org_id, email, is_staff, org_name,"
        " url_prefix, creator, ctime FROM OrgUser t1, Organization t2"
        " WHERE t1.org_id = t2.org_id AND email = ?";

    if (ccnet_db_statement_foreach_row (db, sql, get_orgs_by_user_cb, &ret,
                                        1, "string", email) < 0) {
        g_list_free (ret);
        return NULL;
    }

    return g_list_reverse (ret);
}

static gboolean
get_org_emailusers (CcnetDBRow *row, void *data)
{
    GList **list = (GList **)data;
    const char *email = (char *) ccnet_db_row_get_column_text (row, 0);

    *list = g_list_prepend (*list, g_strdup (email));
    return TRUE;
}

GList *
ccnet_org_manager_get_org_emailusers (CcnetOrgManager *mgr,
                                      const char *url_prefix,
                                      int start, int limit)
{
    CcnetDB *db = mgr->priv->db;
    char *sql;
    GList *ret = NULL;
    int rc;

    if (start == -1 && limit == -1) {
        sql = "SELECT email FROM OrgUser WHERE org_id ="
            " (SELECT org_id FROM Organization WHERE url_prefix = ?)"
            " ORDER BY email";
        rc = ccnet_db_statement_foreach_row (db, sql, get_org_emailusers, &ret,
                                             1, "string", url_prefix);
    } else {
        sql = "SELECT email FROM OrgUser WHERE org_id ="
            " (SELECT org_id FROM Organization WHERE url_prefix = ?)"
            " ORDER BY email LIMIT ? OFFSET ?";
        rc = ccnet_db_statement_foreach_row (db, sql, get_org_emailusers, &ret,
                                             3, "string", url_prefix,
                                             "int", limit, "int", start);
    }

    if (rc < 0)
        return NULL;

    return g_list_reverse (ret);
}

int
ccnet_org_manager_add_org_group (CcnetOrgManager *mgr,
                                 int org_id,
                                 int group_id,
                                 GError **error)
{
    CcnetDB *db = mgr->priv->db;

    return ccnet_db_statement_query (db, "INSERT INTO OrgGroup VALUES (?, ?)",
                                     2, "int", org_id, "int", group_id);
}

int
ccnet_org_manager_remove_org_group (CcnetOrgManager *mgr,
                                    int org_id,
                                    int group_id,
                                    GError **error)
{
    CcnetDB *db = mgr->priv->db;

    return ccnet_db_statement_query (db, "DELETE FROM OrgGroup WHERE org_id=?"
                                     " AND group_id=?",
                                     2, "int", org_id, "string", group_id);
}

int
ccnet_org_manager_is_org_group (CcnetOrgManager *mgr,
                                int group_id,
                                GError **error)
{
    CcnetDB *db = mgr->priv->db;

    return ccnet_db_statement_exists (db, "SELECT group_id FROM OrgGroup "
                                      "WHERE group_id = ?", 1, "int", group_id);
}

int
ccnet_org_manager_get_org_id_by_group (CcnetOrgManager *mgr,
                                       int group_id,
                                       GError **error)
{
    CcnetDB *db = mgr->priv->db;
    char *sql;

    sql = "SELECT org_id FROM OrgGroup WHERE group_id = ?";
    return ccnet_db_statement_get_int (db, sql, 1, "int", group_id);
}

static gboolean
get_org_groups (CcnetDBRow *row, void *data)
{
    GList **plist = data;

    int group_id = ccnet_db_row_get_column_int (row, 0);

    *plist = g_list_prepend (*plist, (gpointer)(long)group_id);

    return TRUE;
}

GList *
ccnet_org_manager_get_org_groups (CcnetOrgManager *mgr,
                                  int org_id,
                                  int start,
                                  int limit)
{
    CcnetDB *db = mgr->priv->db;
    GList *ret = NULL;
    int rc;

    if (limit == -1) {
        rc = ccnet_db_statement_foreach_row (db,
                                             "SELECT group_id FROM OrgGroup WHERE "
                                             "org_id = ?",
                                             get_org_groups, &ret,
                                             1, "int", org_id);
    } else {
        rc = ccnet_db_statement_foreach_row (db,
                                             "SELECT group_id FROM OrgGroup WHERE "
                                             "org_id = ? LIMIT ?, ?",
                                             get_org_groups, &ret,
                                             3, "int", org_id, "int", start,
                                             "int", limit);
    }
    
    if (rc < 0) {
        g_list_free (ret);
        return NULL;
    }

    return g_list_reverse (ret);
}

int
ccnet_org_manager_org_user_exists (CcnetOrgManager *mgr,
                                   int org_id,
                                   const char *email,
                                   GError **error)
{
    CcnetDB *db = mgr->priv->db;

    return ccnet_db_statement_exists (db, "SELECT org_id FROM OrgUser WHERE "
                                      "org_id = ? AND email = ?",
                                      2, "int", org_id, "string", email);
}

char *
ccnet_org_manager_get_url_prefix_by_org_id (CcnetOrgManager *mgr,
                                            int org_id,
                                            GError **error)
{
    CcnetDB *db = mgr->priv->db;
    char *sql;

    sql = "SELECT url_prefix FROM Organization WHERE org_id = ?";

    return ccnet_db_statement_get_string (db, sql, 1, "int", org_id);
}

int
ccnet_org_manager_is_org_staff (CcnetOrgManager *mgr,
                                int org_id,
                                const char *email,
                                GError **error)
{
    CcnetDB *db = mgr->priv->db;
    char *sql;

    sql = "SELECT is_staff FROM OrgUser WHERE org_id=? AND email=?";

    return ccnet_db_statement_get_int (db, sql, 2, "int", org_id, "string", email);
}

int
ccnet_org_manager_set_org_staff (CcnetOrgManager *mgr,
                                 int org_id,
                                 const char *email,
                                 GError **error)
{
    CcnetDB *db = mgr->priv->db;

    return ccnet_db_statement_query (db, "UPDATE OrgUser SET is_staff = 1 "
                                     "WHERE org_id=? AND email=?", 2,
                                     "int", org_id, "string", email);
}

int
ccnet_org_manager_unset_org_staff (CcnetOrgManager *mgr,
                                   int org_id,
                                   const char *email,
                                   GError **error)
{
    CcnetDB *db = mgr->priv->db;

    return ccnet_db_statement_query (db, "UPDATE OrgUser SET is_staff = 0 "
                                     "WHERE org_id=? AND email=?", 2,
                                     "int", org_id, "string", email);
}
