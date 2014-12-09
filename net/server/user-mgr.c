/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <sys/stat.h>
#include <dirent.h>

#include "ccnet-db.h"
#include "timer.h"
#include "utils.h"


#include "peer.h"
#include "session.h"
#include "peer-mgr.h"
#include "user-mgr.h"

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>

#ifdef HAVE_LDAP
  #ifndef WIN32
    #define LDAP_DEPRECATED 1
    #include <ldap.h>
  #else
    #include <winldap.h>
  #endif
#endif

#define DEBUG_FLAG  CCNET_DEBUG_PEER
#include "log.h"

#define DEFAULT_SAVING_INTERVAL_MSEC 30000


G_DEFINE_TYPE (CcnetUserManager, ccnet_user_manager, G_TYPE_OBJECT);


#define GET_PRIV(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), CCNET_TYPE_USER_MANAGER, CcnetUserManagerPriv))


static int open_db (CcnetUserManager *manager);

#ifdef HAVE_LDAP
static int try_load_ldap_settings (CcnetUserManager *manager);
#endif

struct CcnetUserManagerPriv {
    CcnetDB    *db;
    int         max_users;
    int         cur_users;
};


static void
ccnet_user_manager_class_init (CcnetUserManagerClass *klass)
{

    g_type_class_add_private (klass, sizeof (CcnetUserManagerPriv));
}

static void
ccnet_user_manager_init (CcnetUserManager *manager)
{
    manager->priv = GET_PRIV(manager);
}

CcnetUserManager*
ccnet_user_manager_new (CcnetSession *session)
{
    CcnetUserManager* manager;

    manager = g_object_new (CCNET_TYPE_USER_MANAGER, NULL);
    manager->session = session;
    manager->user_hash = g_hash_table_new (g_str_hash, g_str_equal);

    return manager;
}

#define DEFAULT_PASSWD_HASH_ITER 10000

int
ccnet_user_manager_prepare (CcnetUserManager *manager)
{
    int ret;

#ifdef HAVE_LDAP
    if (try_load_ldap_settings (manager) < 0)
        return -1;
#endif

    int iter = g_key_file_get_integer (manager->session->keyf,
                                       "USER", "PASSWORD_HASH_ITERATIONS",
                                       NULL);
    if (iter <= 0)
        iter = DEFAULT_PASSWD_HASH_ITER;
    manager->passwd_hash_iter = iter;

    manager->userdb_path = g_build_filename (manager->session->config_dir,
                                             "user-db", NULL);
    ret = open_db(manager);
    if (ret < 0)
        return ret;

    manager->priv->cur_users = ccnet_user_manager_count_emailusers (manager);
    if (manager->priv->max_users != 0
        && manager->priv->cur_users > manager->priv->max_users) {
        ccnet_warning ("The number of users exceeds limit, max %d, current %d\n",
                       manager->priv->max_users, manager->priv->cur_users);
        return -1;
    }
    return 0;
}

void
ccnet_user_manager_free (CcnetUserManager *manager)
{
    g_object_unref (manager);
}

void
ccnet_user_manager_start (CcnetUserManager *manager)
{

}

void ccnet_user_manager_on_exit (CcnetUserManager *manager)
{
}

void
ccnet_user_manager_set_max_users (CcnetUserManager *manager, gint64 max_users)
{
    manager->priv->max_users = max_users;
}

/* -------- LDAP related --------- */

#ifdef HAVE_LDAP


static int try_load_ldap_settings (CcnetUserManager *manager)
{
    GKeyFile *config = manager->session->keyf;

    manager->ldap_host = g_key_file_get_string (config, "LDAP", "HOST", NULL);
    if (!manager->ldap_host)
        return 0;

    manager->use_ldap = TRUE;

#ifdef WIN32
    manager->use_ssl = g_key_file_get_boolean (config, "LDAP", "USE_SSL", NULL);
#endif

    char *base_list = g_key_file_get_string (config, "LDAP", "BASE", NULL);
    if (!base_list) {
        ccnet_warning ("LDAP: BASE not found in config file.\n");
        return -1;
    }
    manager->base_list = g_strsplit (base_list, ";", -1);

    manager->filter = g_key_file_get_string (config, "LDAP", "FILTER", NULL);

    manager->user_dn = g_key_file_get_string (config, "LDAP", "USER_DN", NULL);
    if (manager->user_dn) {
        manager->password = g_key_file_get_string (config, "LDAP", "PASSWORD", NULL);
        if (!manager->password) {
            ccnet_warning ("LDAP: PASSWORD not found in config file.\n");
            return -1;
        }
    }
    /* Use anonymous if user_dn is not set. */

    manager->login_attr = g_key_file_get_string (config, "LDAP", "LOGIN_ATTR", NULL);
    if (!manager->login_attr)
        manager->login_attr = g_strdup("mail");

    return 0;
}

static LDAP *ldap_init_and_bind (const char *host,
#ifdef WIN32
                                 gboolean use_ssl,
#endif
                                 const char *user_dn,
                                 const char *password)
{
    LDAP *ld;
    int res;
    int desired_version = LDAP_VERSION3;

#ifndef WIN32
    res = ldap_initialize (&ld, host);
    if (res != LDAP_SUCCESS) {
        ccnet_warning ("ldap_initialize failed: %s.\n", ldap_err2string(res));
        return NULL;
    }
#else
    char *host_copy = g_strdup (host);
    if (!use_ssl)
        ld = ldap_init (host_copy, LDAP_PORT);
    else
        ld = ldap_sslinit (host_copy, LDAP_SSL_PORT, 1);
    g_free (host_copy);
    if (!ld) {
        ccnet_warning ("ldap_init failed: %ul.\n", LdapGetLastError());
        return NULL;
    }
#endif

    /* set the LDAP version to be 3 */
    res = ldap_set_option (ld, LDAP_OPT_PROTOCOL_VERSION, &desired_version);
    if (res != LDAP_OPT_SUCCESS) {
        ccnet_warning ("ldap_set_option failed: %s.\n", ldap_err2string(res));
        return NULL;
    }

    if (user_dn) {
#ifndef WIN32
        res = ldap_bind_s (ld, user_dn, password, LDAP_AUTH_SIMPLE);
#else
        char *dn_copy = g_strdup(user_dn);
        char *password_copy = g_strdup(password);
        res = ldap_bind_s (ld, dn_copy, password_copy, LDAP_AUTH_SIMPLE);
        g_free (dn_copy);
        g_free (password_copy);
#endif
        if (res != LDAP_SUCCESS ) {
            ccnet_warning ("ldap_bind failed: %s.\n", ldap_err2string(res));
            ldap_unbind_s (ld);
            return NULL;
        }
    }

    return ld;
}

static int ldap_verify_user_password (CcnetUserManager *manager,
                                      const char *uid,
                                      const char *password)
{
    LDAP *ld = NULL;
    int res;
    GString *filter;
    char *filter_str = NULL;
    char *attrs[2];
    LDAPMessage *msg = NULL, *entry;
    char *dn = NULL;
    int ret = 0;

    /* First search for the DN with the given uid. */

    ld = ldap_init_and_bind (manager->ldap_host,
#ifdef WIN32
                             manager->use_ssl,
#endif
                             manager->user_dn,
                             manager->password);
    if (!ld)
        return -1;

    filter = g_string_new (NULL);
    if (!manager->filter)
        g_string_printf (filter, "(%s=%s)", manager->login_attr, uid);
    else
        g_string_printf (filter, "(&(%s=%s) (%s))",
                         manager->login_attr, uid, manager->filter);
    filter_str = g_string_free (filter, FALSE);

    attrs[0] = manager->login_attr;
    attrs[1] = NULL;

    char **base;
    for (base = manager->base_list; *base; base++) {
        res = ldap_search_s (ld, *base, LDAP_SCOPE_SUBTREE,
                             filter_str, attrs, 0, &msg);
        if (res != LDAP_SUCCESS) {
            ccnet_warning ("ldap_search failed: %s.\n", ldap_err2string(res));
            ret = -1;
            ldap_msgfree (msg);
            goto out;
        }

        entry = ldap_first_entry (ld, msg);
        if (entry) {
            dn = ldap_get_dn (ld, entry);
            ldap_msgfree (msg);
            break;
        }

        ldap_msgfree (msg);
    }

    if (!dn) {
        ccnet_warning ("Can't find user %s in LDAP.\n", uid);
        ret = -1;
        goto out;
    }

    /* Then bind the DN with password. */

    ldap_unbind_s (ld);

    ld = ldap_init_and_bind (manager->ldap_host,
#ifdef WIN32
                             manager->use_ssl,
#endif
                             dn, password);
    if (!ld) {
        ccnet_warning ("Password check for %s failed.\n", uid);
        ret = -1;
    }

out:
    ldap_memfree (dn);
    g_free (filter_str);
    if (ld) ldap_unbind_s (ld);
    return ret;
}

/*
 * @uid: user's uid, list all users if * is passed in.
 */
static GList *ldap_list_users (CcnetUserManager *manager, const char *uid,
                               int start, int limit)
{
    LDAP *ld = NULL;
    GList *ret = NULL;
    int res;
    GString *filter;
    char *filter_str;
    char *attrs[2];
    LDAPMessage *msg = NULL, *entry;

    ld = ldap_init_and_bind (manager->ldap_host,
#ifdef WIN32
                             manager->use_ssl,
#endif
                             manager->user_dn,
                             manager->password);
    if (!ld)
        return NULL;

    filter = g_string_new (NULL);
    if (!manager->filter)
        g_string_printf (filter, "(%s=%s)", manager->login_attr, uid);
    else
        g_string_printf (filter, "(&(%s=%s) (%s))",
                         manager->login_attr, uid, manager->filter);
    filter_str = g_string_free (filter, FALSE);

    attrs[0] = manager->login_attr;
    attrs[1] = NULL;

    int i = 0;
    if (start == -1)
        start = 0;

    char **base;
    for (base = manager->base_list; *base; ++base) {
        res = ldap_search_s (ld, *base, LDAP_SCOPE_SUBTREE,
                             filter_str, attrs, 0, &msg);
        if (res != LDAP_SUCCESS) {
            ccnet_warning ("ldap_search failed: %s.\n", ldap_err2string(res));
            ret = NULL;
            ldap_msgfree (msg);
            goto out;
        }

        for (entry = ldap_first_entry (ld, msg);
             entry != NULL;
             entry = ldap_next_entry (ld, entry), ++i) {
            char *attr;
            char **vals;
            BerElement *ber;
            CcnetEmailUser *user;

            if (i < start)
                continue;
            if (limit >= 0 && i >= start + limit) {
                ldap_msgfree (msg);
                goto out;
            }

            attr = ldap_first_attribute (ld, entry, &ber);
            vals = ldap_get_values (ld, entry, attr);

            char *email_l = g_ascii_strdown (vals[0], -1);
            user = g_object_new (CCNET_TYPE_EMAIL_USER,
                                 "id", 0,
                                 "email", email_l,
                                 "is_staff", FALSE,
                                 "is_active", TRUE,
                                 "ctime", (gint64)0,
                                 "source", "LDAP",
                                 NULL);
            g_free (email_l);
            ret = g_list_prepend (ret, user);

            ldap_memfree (attr);
            ldap_value_free (vals);
            ber_free (ber, 0);
        }

        ldap_msgfree (msg);
    }

out:
    g_free (filter_str);
    if (ld) ldap_unbind_s (ld);
    return ret;
}

/*
 * @uid: user's uid, list all users if * is passed in.
 */
static int ldap_count_users (CcnetUserManager *manager, const char *uid)
{
    LDAP *ld = NULL;
    int res;
    GString *filter;
    char *filter_str;
    char *attrs[2];
    LDAPMessage *msg = NULL;

    ld = ldap_init_and_bind (manager->ldap_host,
#ifdef WIN32
                             manager->use_ssl,
#endif
                             manager->user_dn,
                             manager->password);
    if (!ld)
        return -1;

    filter = g_string_new (NULL);
    if (!manager->filter)
        g_string_printf (filter, "(%s=%s)", manager->login_attr, uid);
    else
        g_string_printf (filter, "(&(%s=%s) (%s))",
                         manager->login_attr, uid, manager->filter);
    filter_str = g_string_free (filter, FALSE);

    attrs[0] = manager->login_attr;
    attrs[1] = NULL;

    char **base;
    int count = 0;
    for (base = manager->base_list; *base; ++base) {
        res = ldap_search_s (ld, *base, LDAP_SCOPE_SUBTREE,
                             filter_str, attrs, 0, &msg);
        if (res != LDAP_SUCCESS) {
            ccnet_warning ("ldap_search failed: %s.\n", ldap_err2string(res));
            ldap_msgfree (msg);
            count = -1;
            goto out;
        }

        count += ldap_count_entries (ld, msg);
        ldap_msgfree (msg);
    }

out:
    g_free (filter_str);
    if (ld) ldap_unbind_s (ld);
    return count;
}

#endif  /* HAVE_LDAP */

/* -------- DB Operations -------- */

static int check_db_table (CcnetDB *db)
{
    char *sql;

    int db_type = ccnet_db_type (db);
    if (db_type == CCNET_DB_TYPE_MYSQL) {
        sql = "CREATE TABLE IF NOT EXISTS EmailUser ("
            "id INTEGER NOT NULL PRIMARY KEY AUTO_INCREMENT, "
            "email VARCHAR(255), passwd VARCHAR(256), "
            "is_staff BOOL NOT NULL, is_active BOOL NOT NULL, "
            "ctime BIGINT, UNIQUE INDEX (email))"
            "ENGINE=INNODB";
        if (ccnet_db_query (db, sql) < 0)
            return -1;
        sql = "CREATE TABLE IF NOT EXISTS Binding (email VARCHAR(255), peer_id CHAR(41),"
            "UNIQUE INDEX (peer_id), INDEX (email(20)))"
            "ENGINE=INNODB";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE TABLE IF NOT EXISTS UserRole ("
          "id INTEGER NOT NULL PRIMARY KEY AUTO_INCREMENT, "
          "email VARCHAR(255), role VARCHAR(255), UNIQUE INDEX (email)) "
          "ENGINE=INNODB";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

    } else if (db_type == CCNET_DB_TYPE_SQLITE) {
        sql = "CREATE TABLE IF NOT EXISTS EmailUser ("
            "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,"
            "email TEXT, passwd TEXT, is_staff bool NOT NULL, "
            "is_active bool NOT NULL, ctime INTEGER)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE UNIQUE INDEX IF NOT EXISTS email_index on EmailUser (email)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE TABLE IF NOT EXISTS Binding (email TEXT, peer_id TEXT)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE INDEX IF NOT EXISTS email_index on Binding (email)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE UNIQUE INDEX IF NOT EXISTS peer_index on Binding (peer_id)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE TABLE IF NOT EXISTS UserRole (email TEXT, role TEXT)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE INDEX IF NOT EXISTS userrole_email_index on UserRole (email)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE UNIQUE INDEX IF NOT EXISTS userrole_userrole_index on UserRole (email, role)";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

    } else if (db_type == CCNET_DB_TYPE_PGSQL) {
        sql = "CREATE TABLE IF NOT EXISTS EmailUser ("
            "id SERIAL PRIMARY KEY, "
            "email VARCHAR(255), passwd VARCHAR(256), "
            "is_staff INTEGER NOT NULL, is_active INTEGER NOT NULL, "
            "ctime BIGINT, UNIQUE (email))";
        if (ccnet_db_query (db, sql) < 0)
            return -1;
        sql = "CREATE TABLE IF NOT EXISTS Binding (email VARCHAR(255), peer_id CHAR(41),"
            "UNIQUE (peer_id))";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        sql = "CREATE TABLE IF NOT EXISTS UserRole (email VARCHAR(255), "
          " role VARCHAR(255), UNIQUE (email, role))";
        if (ccnet_db_query (db, sql) < 0)
            return -1;

        if (!pgsql_index_exists (db, "userrole_email_idx")) {
            sql = "CREATE INDEX userrole_email_idx ON UserRole (email)";
            if (ccnet_db_query (db, sql) < 0)
                return -1;
        }
    }

    return 0;
}


static CcnetDB *
open_sqlite_db (CcnetUserManager *manager)
{
    CcnetDB *db = NULL;
    char *db_dir;
    char *db_path;

    db_dir = g_build_filename (manager->session->config_dir, "PeerMgr", NULL);
    if (checkdir_with_mkdir(db_dir) < 0) {
        ccnet_error ("Cannot open db dir %s: %s\n", db_dir,
                     strerror(errno));
        return NULL;
    }
    g_free (db_dir);

    db_path = g_build_filename (manager->session->config_dir, "PeerMgr",
                                "usermgr.db", NULL);
    db = ccnet_db_new_sqlite (db_path);
    g_free (db_path);

    return db;
}

static int
open_db (CcnetUserManager *manager)
{
    CcnetDB *db = NULL;

    switch (ccnet_db_type(manager->session->db)) {
    /* To be compatible with the db file layout of 0.9.1 version,
     * we don't use conf-dir/ccnet.db for user and peer info, but
     * user conf-dir/PeerMgr/peermgr.db and conf-dir/PeerMgr/usermgr.db instead.
     */
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


/* -------- EmailUser Management -------- */

/* truly random sequece read from /dev/urandom. */
static unsigned char salt[8] = { 0xdb, 0x91, 0x45, 0xc3, 0x06, 0xc7, 0xcc, 0x26 };

static void
hash_password (const char *passwd, char *hashed_passwd)
{
    unsigned char sha1[20];
    SHA_CTX s;

    SHA1_Init (&s);
    SHA1_Update (&s, passwd, strlen(passwd));
    SHA1_Final (sha1, &s);
    rawdata_to_hex (sha1, hashed_passwd, 20);
}

static void
hash_password_salted (const char *passwd, char *hashed_passwd)
{
    unsigned char sha[SHA256_DIGEST_LENGTH];
    SHA256_CTX s;

    SHA256_Init (&s);
    SHA256_Update (&s, passwd, strlen(passwd));
    SHA256_Update (&s, salt, sizeof(salt));
    SHA256_Final (sha, &s);
    rawdata_to_hex (sha, hashed_passwd, SHA256_DIGEST_LENGTH);
}

static void
hash_password_pbkdf2_sha256 (const char *passwd,
                             int iterations,
                             char **db_passwd)
{
    guint8 sha[SHA256_DIGEST_LENGTH];
    guint8 salt[SHA256_DIGEST_LENGTH];
    char hashed_passwd[SHA256_DIGEST_LENGTH*2+1];
    char salt_str[SHA256_DIGEST_LENGTH*2+1];

    if (!RAND_bytes (salt, sizeof(salt))) {
        ccnet_warning ("Failed to generate salt "
                       "with RAND_bytes(), use RAND_pseudo_bytes().\n");
        RAND_pseudo_bytes (salt, sizeof(salt));
    }

    PKCS5_PBKDF2_HMAC (passwd, strlen(passwd),
                       salt, sizeof(salt),
                       iterations,
                       EVP_sha256(),
                       sizeof(sha), sha);

    rawdata_to_hex (sha, hashed_passwd, SHA256_DIGEST_LENGTH);

    rawdata_to_hex (salt, salt_str, SHA256_DIGEST_LENGTH);

    /* Encode password hash related information into one string, similar to Django. */
    GString *buf = g_string_new (NULL);
    g_string_printf (buf, "PBKDF2SHA256$%d$%s$%s",
                     iterations, salt_str, hashed_passwd);
    *db_passwd = g_string_free (buf, FALSE);
}

static gboolean
validate_passwd_pbkdf2_sha256 (const char *passwd, const char *db_passwd)
{
    char **tokens;
    char *salt_str, *hash;
    int iter;
    guint8 sha[SHA256_DIGEST_LENGTH];
    guint8 salt[SHA256_DIGEST_LENGTH];
    char hashed_passwd[SHA256_DIGEST_LENGTH*2+1];

    tokens = g_strsplit (db_passwd, "$", -1);
    if (!tokens || g_strv_length (tokens) != 4) {
        ccnet_warning ("Invalide db passwd format %s.\n", db_passwd);
        return FALSE;
    }

    iter = atoi (tokens[1]);
    salt_str = tokens[2];
    hash = tokens[3];

    hex_to_rawdata (salt_str, salt, SHA256_DIGEST_LENGTH);

    PKCS5_PBKDF2_HMAC (passwd, strlen(passwd),
                       salt, sizeof(salt),
                       iter,
                       EVP_sha256(),
                       sizeof(sha), sha);
    rawdata_to_hex (sha, hashed_passwd, SHA256_DIGEST_LENGTH);

    gboolean ret = (strcmp (hash, hashed_passwd) == 0);

    g_strfreev (tokens);
    return ret;
}

static gboolean
validate_passwd (const char *passwd, const char *stored_passwd,
                 gboolean *need_upgrade)
{
    char hashed_passwd[SHA256_DIGEST_LENGTH * 2 + 1];
    int hash_len = strlen(stored_passwd);

    *need_upgrade = FALSE;

    if (hash_len == SHA256_DIGEST_LENGTH * 2) {
        hash_password_salted (passwd, hashed_passwd);
        *need_upgrade = TRUE;
    } else if (hash_len == SHA_DIGEST_LENGTH * 2) {
        hash_password (passwd, hashed_passwd);
        *need_upgrade = TRUE;
    } else {
        return validate_passwd_pbkdf2_sha256 (passwd, stored_passwd);
    }

    if (strcmp (hashed_passwd, stored_passwd) == 0)
        return TRUE;
    else
        return FALSE;
}

static int
update_user_passwd (CcnetUserManager *manager,
                    const char *email, const char *passwd)
{
    CcnetDB *db = manager->priv->db;
    char *db_passwd = NULL;
    int ret;

    hash_password_pbkdf2_sha256 (passwd, manager->passwd_hash_iter,
                                 &db_passwd);

    /* convert email to lower case for case insensitive lookup. */
    char *email_down = g_ascii_strdown (email, strlen(email));

    ret = ccnet_db_statement_query (db,
                                    "UPDATE EmailUser SET passwd=? WHERE email=?",
                                    2, "string", db_passwd, "string", email_down);

    g_free (db_passwd);
    g_free (email_down);

    if (ret < 0)
        return ret;

    return 0;
}

int
ccnet_user_manager_add_emailuser (CcnetUserManager *manager,
                                  const char *email,
                                  const char *passwd,
                                  int is_staff, int is_active)
{
    CcnetDB *db = manager->priv->db;
    gint64 now = get_current_time();
    char *db_passwd = NULL;
    int ret;

    if (manager->priv->max_users &&
        manager->priv->cur_users >= manager->priv->max_users) {
        ccnet_warning ("User number exceeds limit. Users %d, limit %d.\n",
                       manager->priv->cur_users, manager->priv->max_users);
        return -1;
    }

    hash_password_pbkdf2_sha256 (passwd, manager->passwd_hash_iter,
                                 &db_passwd);

    /* convert email to lower case for case insensitive lookup. */
    char *email_down = g_ascii_strdown (email, strlen(email));

    ret = ccnet_db_statement_query (db,
                                    "INSERT INTO EmailUser(email, passwd, is_staff, "
                                    "is_active, ctime) VALUES (?, ?, ?, ?, ?)",
                                    5, "string", email_down, "string", db_passwd,
                                    "int", is_staff, "int", is_active, "int64", now);

    g_free (db_passwd);
    g_free (email_down);

    if (ret < 0)
        return ret;

    manager->priv->cur_users ++;
    return 0;
}

int
ccnet_user_manager_remove_emailuser (CcnetUserManager *manager,
                                     const char *email)
{
    CcnetDB *db = manager->priv->db;
    int ret;

    ccnet_db_statement_query (db,
                              "DELETE FROM UserRole WHERE email=?",
                              1, "string", email);

    ret = ccnet_db_statement_query (db,
                                    "DELETE FROM EmailUser WHERE email=?",
                                    1, "string", email);

    if (ret < 0)
        return ret;

    manager->priv->cur_users --;
    return 0;
}

static gboolean
get_password (CcnetDBRow *row, void *data)
{
    char **p_passwd = data;

    *p_passwd = g_strdup(ccnet_db_row_get_column_text (row, 0));
    return FALSE;
}

int
ccnet_user_manager_validate_emailuser (CcnetUserManager *manager,
                                       const char *email,
                                       const char *passwd)
{
    CcnetDB *db = manager->priv->db;
    char *sql;
    char *email_down;
    char *stored_passwd = NULL;
    gboolean need_upgrade = FALSE;

#ifdef HAVE_LDAP
    if (manager->use_ldap) {
        if (ldap_verify_user_password (manager, email, passwd) == 0)
            return 0;
    }
#endif

    sql = "SELECT passwd FROM EmailUser WHERE email=?";
    if (ccnet_db_statement_foreach_row (db, sql,
                                        get_password, &stored_passwd,
                                        1, "string", email) > 0) {
        if (validate_passwd (passwd, stored_passwd, &need_upgrade)) {
            if (need_upgrade)
                update_user_passwd (manager, email, passwd);
            g_free (stored_passwd);
            return 0;
        } else {
            g_free (stored_passwd);
            return -1;
        }
    }

    email_down = g_ascii_strdown (email, strlen(email));
    if (ccnet_db_statement_foreach_row (db, sql,
                                        get_password, &stored_passwd,
                                        1, "string", email_down) > 0) {
        g_free (email_down);
        if (validate_passwd (passwd, stored_passwd, &need_upgrade)) {
            if (need_upgrade)
                update_user_passwd (manager, email, passwd);
            g_free (stored_passwd);
            return 0;
        } else {
            g_free (stored_passwd);
            return -1;
        }
    }
    g_free (email_down);

    return -1;
}

static gboolean
get_emailuser_cb (CcnetDBRow *row, void *data)
{
    CcnetEmailUser **p_emailuser = data;

    int id = ccnet_db_row_get_column_int (row, 0);
    const char *email = (const char *)ccnet_db_row_get_column_text (row, 1);
    int is_staff = ccnet_db_row_get_column_int (row, 2);
    int is_active = ccnet_db_row_get_column_int (row, 3);
    gint64 ctime = ccnet_db_row_get_column_int64 (row, 4);

    char *email_l = g_ascii_strdown (email, -1);
    *p_emailuser = g_object_new (CCNET_TYPE_EMAIL_USER,
                                 "id", id,
                                 "email", email_l,
                                 "is_staff", is_staff,
                                 "is_active", is_active,
                                 "ctime", ctime,
                                 "source", "DB",
                                 NULL);
    g_free (email_l);

    return FALSE;
}

static char*
ccnet_user_manager_get_role_emailuser (CcnetUserManager *manager,
                                     const char* email);

CcnetEmailUser*
ccnet_user_manager_get_emailuser (CcnetUserManager *manager,
                                  const char *email)
{
    CcnetDB *db = manager->priv->db;
    char *sql;
    CcnetEmailUser *emailuser = NULL;
    char *email_down;

    sql = "SELECT id, email, is_staff, is_active, ctime"
        " FROM EmailUser WHERE email=?";
    if (ccnet_db_statement_foreach_row (db, sql, get_emailuser_cb, &emailuser,
                                        1, "string", email) > 0) {
        char *role = ccnet_user_manager_get_role_emailuser (manager, email);
        if (role) {
            g_object_set (emailuser, "role", role, NULL);
            g_free (role);
        }
        return emailuser;
    }

    email_down = g_ascii_strdown (email, strlen(email));
    if (ccnet_db_statement_foreach_row (db, sql, get_emailuser_cb, &emailuser,
                                        1, "string", email_down) > 0) {
        char *role = ccnet_user_manager_get_role_emailuser(manager, email_down);
        if (role) {
            g_object_set (emailuser, "role", role, NULL);
            g_free (role);
        }
        g_free (email_down);
        return emailuser;
    }
    g_free (email_down);

#ifdef HAVE_LDAP
    if (manager->use_ldap) {
        GList *users, *ptr;

        users = ldap_list_users (manager, email, -1, -1);
        if (!users)
            return NULL;
        emailuser = users->data;

        /* Free all except the first user. */
        for (ptr = users->next; ptr; ptr = ptr->next)
            g_object_unref (ptr->data);
        g_list_free (users);

        char *role = ccnet_user_manager_get_role_emailuser(manager, email);
        if (role) {
            g_object_set (emailuser, "role", role, NULL);
            g_free (role);
        }
        return emailuser;
    }
#endif

    return NULL;
}

CcnetEmailUser*
ccnet_user_manager_get_emailuser_by_id (CcnetUserManager *manager, int id)
{
    CcnetDB *db = manager->priv->db;
    char *sql;
    CcnetEmailUser *emailuser = NULL;

    sql = "SELECT id, email, is_staff, is_active, ctime"
        " FROM EmailUser WHERE id=?";
    if (ccnet_db_statement_foreach_row (db, sql, get_emailuser_cb, &emailuser,
                                        1, "int", id) < 0)
        return NULL;

    return emailuser;
}

static gboolean
get_emailusers_cb (CcnetDBRow *row, void *data)
{
    GList **plist = data;
    CcnetEmailUser *emailuser;

    int id = ccnet_db_row_get_column_int (row, 0);
    const char *email = (const char *)ccnet_db_row_get_column_text (row, 1);
    int is_staff = ccnet_db_row_get_column_int (row, 2);
    int is_active = ccnet_db_row_get_column_int (row, 3);
    gint64 ctime = ccnet_db_row_get_column_int64 (row, 4);
    const char *role = (const char *)ccnet_db_row_get_column_text (row, 5);

    char *email_l = g_ascii_strdown (email, -1);
    emailuser = g_object_new (CCNET_TYPE_EMAIL_USER,
                              "id", id,
                              "email", email_l,
                              "is_staff", is_staff,
                              "is_active", is_active,
                              "ctime", ctime,
                              "role", role ? role : "",
                              "source", "DB", 
                              NULL);
    g_free (email_l);

    *plist = g_list_prepend (*plist, emailuser);

    return TRUE;
}

GList*
ccnet_user_manager_get_emailusers (CcnetUserManager *manager,
                                   const char *source,
                                   int start, int limit)
{
    CcnetDB *db = manager->priv->db;
    GList *ret = NULL;

#ifdef HAVE_LDAP
    if (manager->use_ldap && g_strcmp0 (source, "LDAP") == 0) {
        GList *users;
        users = ldap_list_users (manager, "*", start, limit);
        return g_list_reverse (users);
    }
#endif

    if (g_strcmp0 (source, "DB") != 0)
        return NULL;

    int rc;
    if (start == -1 && limit == -1)
        rc = ccnet_db_statement_foreach_row (db,
                                             "SELECT t1.id, t1.email, "
                                             "t1.is_staff, t1.is_active, t1.ctime, "
                                             "t2.role FROM EmailUser AS t1 "
                                             "LEFT JOIN UserRole AS t2 "
                                             "ON t1.email = t2.email ",
                                             get_emailusers_cb, &ret,
                                             0);
    else
        rc = ccnet_db_statement_foreach_row (db,
                                             "SELECT t1.id, t1.email, "
                                             "t1.is_staff, t1.is_active, t1.ctime, "
                                             "t2.role FROM EmailUser AS t1 "
                                             "LEFT JOIN UserRole AS t2 "
                                             "ON t1.email = t2.email "
                                             "ORDER BY t1.id LIMIT ? OFFSET ?",
                                             get_emailusers_cb, &ret,
                                             2, "int", limit, "int", start);

    if (rc < 0) {
        while (ret != NULL) {
            g_object_unref (ret->data);
            ret = g_list_delete_link (ret, ret);
        }
        return NULL;
    }

    return g_list_reverse (ret);
}

static char *
db_pattern_to_ldap_pattern (const char *db_pattern)
{
    char *ldap_patt = g_strdup(db_pattern);
    char *ptr;

    for (ptr = ldap_patt; *ptr != 0; ++ptr) {
        if (*ptr == '%')
            *ptr = '*';
    }

    return ldap_patt;
}

GList*
ccnet_user_manager_search_emailusers (CcnetUserManager *manager,
                                      const char *email_patt,
                                      int start, int limit)
{
    CcnetDB *db = manager->priv->db;
    GList *ret = NULL;

#ifdef HAVE_LDAP
    if (manager->use_ldap) {
        char *ldap_patt = db_pattern_to_ldap_pattern (email_patt);
        ret = ldap_list_users (manager, ldap_patt, -1, -1);
        g_free (ldap_patt);
    }
#endif

    int rc;
    if (start == -1 && limit == -1)
        rc = ccnet_db_statement_foreach_row (db,
                                             "SELECT t1.id, t1.email, "
                                             "t1.is_staff, t1.is_active, t1.ctime, "
                                             "t2.role FROM EmailUser AS t1 "
                                             "LEFT JOIN UserRole AS t2 "
                                             "ON t1.email = t2.email "
                                             "WHERE t1.Email LIKE ? "
                                             "ORDER BY t1.id",
                                             get_emailusers_cb, &ret,
                                             1, "string", email_patt);
    else
        rc = ccnet_db_statement_foreach_row (db,
                                             "SELECT t1.id, t1.email, "
                                             "t1.is_staff, t1.is_active, t1.ctime, "
                                             "t2.role FROM EmailUser AS t1 "
                                             "LEFT JOIN UserRole AS t2 "
                                             "ON t1.email = t2.email "
                                             "WHERE t1.Email LIKE ? "
                                             "ORDER BY t1.id LIMIT ? OFFSET ?",
                                             get_emailusers_cb, &ret,
                                             3, "string", email_patt,
                                             "int", limit, "int", start);
    
    if (rc < 0) {
        while (ret != NULL) {
            g_object_unref (ret->data);
            ret = g_list_delete_link (ret, ret);
        }
        return NULL;
    }

    return g_list_reverse (ret);
}


gint64
ccnet_user_manager_count_emailusers (CcnetUserManager *manager)
{
    CcnetDB* db = manager->priv->db;
    char sql[512];
    gint64 count = 0, ret;

#ifdef HAVE_LDAP
    if (manager->use_ldap) {
        gint64 ret = ldap_count_users (manager, "*");
        if (ret < 0)
            return -1;
        count += (gint64) ret;
    }
#endif

    snprintf (sql, 512, "SELECT COUNT(*) FROM EmailUser");

    ret = ccnet_db_get_int64 (db, sql);
    if (ret < 0)
        return -1;
    count += ret;
    return count;
}

#if 0
GList*
ccnet_user_manager_filter_emailusers_by_emails(CcnetUserManager *manager,
                                               const char *emails)
{
    CcnetDB *db = manager->priv->db;
    char *copy = g_strdup (emails), *saveptr;
    GList *ret = NULL;

#ifdef HAVE_LDAP
    if (manager->use_ldap)
        return NULL;            /* todo */
#endif

    GString *sql = g_string_new(NULL);

    g_string_append (sql, "SELECT * FROM EmailUser WHERE Email IN (");
    char *name = strtok_r (copy, ", ", &saveptr);
    while (name != NULL) {
        g_string_append_printf (sql, "'%s',", name);
        name = strtok_r (NULL, ", ", &saveptr);
    }
    g_string_erase (sql, sql->len-1, 1); /* remove last "," */
    g_string_append (sql, ")");

    if (ccnet_db_foreach_selected_row (db, sql->str, get_emailusers_cb,
        &ret) < 0) {
        while (ret != NULL) {
            g_object_unref (ret->data);
            ret = g_list_delete_link (ret, ret);
        }
        return NULL;
    }

    g_free (copy);
    g_string_free (sql, TRUE);
    
    return g_list_reverse (ret);
}
#endif

int
ccnet_user_manager_update_emailuser (CcnetUserManager *manager,
                                     int id, const char* passwd,
                                     int is_staff, int is_active)
{
    CcnetDB* db = manager->priv->db;
    char *db_passwd = NULL;

    if (g_strcmp0 (passwd, "!") == 0) {
        /* Don't update passwd if it starts with '!' */
        return ccnet_db_statement_query  (db, "UPDATE EmailUser SET is_staff=?, "
                                          "is_active=? WHERE id=?",
                                          3, "int", is_staff, "int", is_active,
                                          "int", id);
    } else {
        hash_password_pbkdf2_sha256 (passwd, manager->passwd_hash_iter, &db_passwd);

        return ccnet_db_statement_query (db, "UPDATE EmailUser SET passwd=?, "
                                         "is_staff=?, is_active=? WHERE id=?",
                                         4, "string", db_passwd, "int", is_staff,
                                         "int", is_active, "int", id);
    }
}

static gboolean
get_role_emailuser_cb (CcnetDBRow *row, void *data)
{
    *((char **)data) = g_strdup (ccnet_db_row_get_column_text (row, 0));

    return FALSE;
}

static char*
ccnet_user_manager_get_role_emailuser (CcnetUserManager *manager,
                                     const char* email)
{

    CcnetDB *db = manager->priv->db;
    const char *sql;
    char* role;

    sql = "SELECT role FROM UserRole WHERE email=?";
    if (ccnet_db_statement_foreach_row (db, sql, get_role_emailuser_cb, &role,
                                        1, "string", email) > 0)
        return role;

    return NULL;
}

int
ccnet_user_manager_update_role_emailuser (CcnetUserManager *manager,
                                     const char* email, const char* role)
{
    CcnetDB* db = manager->priv->db;
    char *old_role = ccnet_user_manager_get_role_emailuser (manager, email);
    if (old_role) {
        g_free (old_role);
        return ccnet_db_statement_query (db, "UPDATE UserRole SET role=? "
                                         "WHERE email=?",
                                         2, "string", role, "string", email);
    } else
        return ccnet_db_statement_query (db, "INSERT INTO UserRole(role, email)"
                                         " VALUES (?, ?)",
                                         2, "string", role, "string", email);
}

GList*
ccnet_user_manager_get_superusers(CcnetUserManager *manager)
{
    CcnetDB* db = manager->priv->db;
    GList *ret = NULL;
    char sql[512];

    snprintf (sql, 512,
              "SELECT t1.id, t1.email, "
              "t1.is_staff, t1.is_active, t1.ctime, "
              "t2.role FROM EmailUser AS t1 "
              "LEFT JOIN UserRole AS t2 "
              "ON t1.email = t2.email "
              "WHERE is_staff = 1;");

    if (ccnet_db_foreach_selected_row (db, sql, get_emailusers_cb, &ret) < 0) {
        while (ret != NULL) {
            g_object_unref (ret->data);
            ret = g_list_delete_link (ret, ret);
        }
        return NULL;
    }

    return g_list_reverse (ret);
}
