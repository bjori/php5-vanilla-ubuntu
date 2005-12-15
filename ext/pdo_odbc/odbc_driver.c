/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2005 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Wez Furlong <wez@php.net>                                    |
  +----------------------------------------------------------------------+
*/

/* $Id: odbc_driver.c,v 1.27.2.1 2005/09/26 21:37:33 wez Exp $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_odbc.h"
#include "php_pdo_odbc_int.h"
#include "zend_exceptions.h"

static int pdo_odbc_fetch_error_func(pdo_dbh_t *dbh, pdo_stmt_t *stmt, zval *info TSRMLS_DC)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	pdo_odbc_errinfo *einfo = &H->einfo;
	pdo_odbc_stmt *S = NULL;
	char *message = NULL;

	if (stmt) {
		S = (pdo_odbc_stmt*)stmt->driver_data;
		einfo = &S->einfo;
	}

	spprintf(&message, 0, "%s (%s[%ld] at %s:%d)",
				einfo->last_err_msg,
				einfo->what, einfo->last_error,
				einfo->file, einfo->line);

	add_next_index_long(info, einfo->last_error);
	add_next_index_string(info, message, 0);
	add_next_index_string(info, einfo->last_state, 1);

	return 1;
}


void pdo_odbc_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, PDO_ODBC_HSTMT statement, char *what, const char *file, int line TSRMLS_DC) /* {{{ */
{
	RETCODE rc;
	SWORD	errmsgsize = 0;
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle*)dbh->driver_data;
	pdo_odbc_errinfo *einfo = &H->einfo;
	pdo_odbc_stmt *S = NULL;
	pdo_error_type *pdo_err = &dbh->error_code;

	if (stmt) {
		S = (pdo_odbc_stmt*)stmt->driver_data;

		einfo = &S->einfo;
		pdo_err = &stmt->error_code;
	}

	if (statement == SQL_NULL_HSTMT && S) {
		statement = S->stmt;
	}
	
	rc = SQLError(H->env, H->dbc, statement, einfo->last_state, &einfo->last_error,
			einfo->last_err_msg, sizeof(einfo->last_err_msg)-1, &errmsgsize);

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		errmsgsize = 0;
	}

	einfo->last_err_msg[errmsgsize] = '\0';
	einfo->file = file;
	einfo->line = line;
	einfo->what = what;

	strcpy(*pdo_err, einfo->last_state);
/* printf("@@ SQLSTATE[%s] %s\n", *pdo_err, einfo->last_err_msg); */
	if (!dbh->methods) {
		zend_throw_exception_ex(php_pdo_get_exception(), 0 TSRMLS_CC, "SQLSTATE[%s] %s: %d %s",
				*pdo_err, what, einfo->last_error, einfo->last_err_msg);
	}
}
/* }}} */

static int odbc_handle_closer(pdo_dbh_t *dbh TSRMLS_DC)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle*)dbh->driver_data;

	if (H->dbc != SQL_NULL_HANDLE) {
		SQLEndTran(SQL_HANDLE_DBC, H->dbc, SQL_ROLLBACK);
		SQLDisconnect(H->dbc);
		SQLFreeHandle(SQL_HANDLE_DBC, H->dbc);
		H->dbc = NULL;
	}
	SQLFreeHandle(SQL_HANDLE_ENV, H->env);
	H->env = NULL;
	pefree(H, dbh->is_persistent);
	dbh->driver_data = NULL;

	return 0;
}

static int odbc_handle_preparer(pdo_dbh_t *dbh, const char *sql, long sql_len, pdo_stmt_t *stmt, zval *driver_options TSRMLS_DC)
{
	RETCODE rc;
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	pdo_odbc_stmt *S = ecalloc(1, sizeof(*S));
	enum pdo_cursor_type cursor_type = PDO_CURSOR_FWDONLY;
	int ret;
	char *nsql = NULL;
	int nsql_len = 0;

	S->H = H;

	/* before we prepare, we need to peek at the query; if it uses named parameters,
	 * we want PDO to rewrite them for us */
	stmt->supports_placeholders = PDO_PLACEHOLDER_POSITIONAL;
	ret = pdo_parse_params(stmt, (char*)sql, sql_len, &nsql, &nsql_len TSRMLS_CC);
	
	if (ret == 1) {
		/* query was re-written */
		sql = nsql;
	} else if (ret == -1) {
		/* couldn't grok it */
		strcpy(dbh->error_code, stmt->error_code);
		efree(S);
		return 0;
	}
	
	rc = SQLAllocHandle(SQL_HANDLE_STMT, H->dbc, &S->stmt);

	if (rc == SQL_INVALID_HANDLE || rc == SQL_ERROR) {
		efree(S);
		if (nsql) {
			efree(nsql);
		}
		pdo_odbc_drv_error("SQLAllocStmt");
		return 0;
	}

	cursor_type = pdo_attr_lval(driver_options, PDO_ATTR_CURSOR, PDO_CURSOR_FWDONLY TSRMLS_CC);
	if (cursor_type != PDO_CURSOR_FWDONLY) {
		rc = SQLSetStmtAttr(S->stmt, SQL_ATTR_CURSOR_SCROLLABLE, (void*)SQL_SCROLLABLE, 0);
		if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
			pdo_odbc_stmt_error("SQLSetStmtAttr: SQL_ATTR_CURSOR_SCROLLABLE");
			SQLFreeHandle(SQL_HANDLE_STMT, S->stmt);
			if (nsql) {
				efree(nsql);
			}
			return 0;
		}
	}
	
	rc = SQLPrepare(S->stmt, (char*)sql, SQL_NTS);
	if (nsql) {
		efree(nsql);
	}

	if (rc != SQL_SUCCESS) {
		pdo_odbc_stmt_error("SQLPrepare");
	}

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		SQLFreeHandle(SQL_HANDLE_STMT, S->stmt);
		return 0;
	}

	stmt->driver_data = S;
	stmt->methods = &odbc_stmt_methods;
	
	return 1;
}

static long odbc_handle_doer(pdo_dbh_t *dbh, const char *sql, long sql_len TSRMLS_DC)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	RETCODE rc;
	long row_count = -1;
	PDO_ODBC_HSTMT	stmt;
	
	rc = SQLAllocHandle(SQL_HANDLE_STMT, H->dbc, &stmt);
	if (rc != SQL_SUCCESS) {
		pdo_odbc_drv_error("SQLAllocHandle: STMT");
		return -1;
	}

	rc = SQLExecDirect(stmt, (char *)sql, sql_len);

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_doer_error("SQLExecDirect");
		goto out;
	}

	rc = SQLRowCount(stmt, &row_count);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_doer_error("SQLRowCount");
		goto out;
	}
	if (row_count == -1) {
		row_count = 0;
	}
out:
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	return row_count;
}

static int odbc_handle_quoter(pdo_dbh_t *dbh, const char *unquoted, int unquotedlen, char **quoted, int *quotedlen, enum pdo_param_type param_type  TSRMLS_DC)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	/* TODO: figure it out */
	return 0;
}

static int odbc_handle_begin(pdo_dbh_t *dbh TSRMLS_DC)
{
	if (dbh->auto_commit) {
		/* we need to disable auto-commit now, to be able to initiate a transaction */
		RETCODE rc;
		pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;

		rc = SQLSetConnectAttr(H->dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, SQL_IS_INTEGER);
		if (rc != SQL_SUCCESS) {
			pdo_odbc_drv_error("SQLSetConnectAttr AUTOCOMMIT = OFF");
			return 0;
		}
	}
	return 1;
}

static int odbc_handle_commit(pdo_dbh_t *dbh TSRMLS_DC)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	RETCODE rc;

	rc = SQLEndTran(SQL_HANDLE_DBC, H->dbc, SQL_COMMIT);

	if (rc != SQL_SUCCESS) {
		pdo_odbc_drv_error("SQLEndTran: Commit");

		if (rc != SQL_SUCCESS_WITH_INFO) {
			return 0;
		}
	}

	if (dbh->auto_commit) {
		/* turn auto-commit back on again */
		rc = SQLSetConnectAttr(H->dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_ON, SQL_IS_INTEGER);
		if (rc != SQL_SUCCESS) {
			pdo_odbc_drv_error("SQLSetConnectAttr AUTOCOMMIT = ON");
			return 0;
		}
	}
	return 1;
}

static int odbc_handle_rollback(pdo_dbh_t *dbh TSRMLS_DC)
{
	pdo_odbc_db_handle *H = (pdo_odbc_db_handle *)dbh->driver_data;
	RETCODE rc;

	rc = SQLEndTran(SQL_HANDLE_DBC, H->dbc, SQL_ROLLBACK);

	if (rc != SQL_SUCCESS) {
		pdo_odbc_drv_error("SQLEndTran: Rollback");

		if (rc != SQL_SUCCESS_WITH_INFO) {
			return 0;
		}
	}
	if (dbh->auto_commit && H->dbc) {
		/* turn auto-commit back on again */
		rc = SQLSetConnectAttr(H->dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_ON, SQL_IS_INTEGER);
		if (rc != SQL_SUCCESS) {
			pdo_odbc_drv_error("SQLSetConnectAttr AUTOCOMMIT = ON");
			return 0;
		}
	}

	return 1;
}

static int odbc_handle_get_attr(pdo_dbh_t *dbh, long attr, zval *val TSRMLS_DC)
{
	switch (attr) {
		case PDO_ATTR_CLIENT_VERSION:
			ZVAL_STRING(val, "ODBC-" PDO_ODBC_TYPE, 1);
			return 1;

		case PDO_ATTR_SERVER_VERSION:
		case PDO_ATTR_PREFETCH:
		case PDO_ATTR_TIMEOUT:
		case PDO_ATTR_SERVER_INFO:
		case PDO_ATTR_CONNECTION_STATUS:
			break;

	}
	return 0;
}

static struct pdo_dbh_methods odbc_methods = {
	odbc_handle_closer,
	odbc_handle_preparer,
	odbc_handle_doer,
	odbc_handle_quoter,
	odbc_handle_begin,
	odbc_handle_commit,
	odbc_handle_rollback,
	NULL, 	/* set attr */
	NULL,	/* last id */
	pdo_odbc_fetch_error_func,
	odbc_handle_get_attr,	/* get attr */
	NULL,	/* check_liveness */
};

static int pdo_odbc_handle_factory(pdo_dbh_t *dbh, zval *driver_options TSRMLS_DC) /* {{{ */
{
	pdo_odbc_db_handle *H;
	RETCODE rc;
	int use_direct = 0;
	SQLUINTEGER cursor_lib;

	H = pecalloc(1, sizeof(*H), dbh->is_persistent);

	dbh->driver_data = H;
	
	SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &H->env);
	rc = SQLSetEnvAttr(H->env, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_drv_error("SQLSetEnvAttr: ODBC3");
		goto fail;
	}

#ifdef SQL_ATTR_CONNECTION_POOLING
	if (pdo_odbc_pool_on != SQL_CP_OFF) {
		rc = SQLSetEnvAttr(H->env, SQL_ATTR_CP_MATCH, (void*)pdo_odbc_pool_mode, 0);
		if (rc != SQL_SUCCESS) {
			pdo_odbc_drv_error("SQLSetEnvAttr: SQL_ATTR_CP_MATCH");
			goto fail;
		}
	}
#endif
	
	rc = SQLAllocHandle(SQL_HANDLE_DBC, H->env, &H->dbc);
	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_drv_error("SQLAllocHandle (DBC)");
		goto fail;
	}

	rc = SQLSetConnectAttr(H->dbc, SQL_ATTR_AUTOCOMMIT,
		(SQLPOINTER)(dbh->auto_commit ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF), SQL_IS_INTEGER);
	if (rc != SQL_SUCCESS) {
		pdo_odbc_drv_error("SQLSetConnectAttr AUTOCOMMIT");
		goto fail;
	}

	/* set up the cursor library, if needed, or if configured explicitly */
	cursor_lib = pdo_attr_lval(driver_options, PDO_ODBC_ATTR_USE_CURSOR_LIBRARY, SQL_CUR_USE_IF_NEEDED TSRMLS_CC);
	rc = SQLSetConnectAttr(H->dbc, SQL_ODBC_CURSORS, (void*)cursor_lib, SQL_IS_INTEGER);
	if (rc != SQL_SUCCESS && cursor_lib != SQL_CUR_USE_IF_NEEDED) {
		pdo_odbc_drv_error("SQLSetConnectAttr SQL_ODBC_CURSORS");
		goto fail;
	}

	if (strchr(dbh->data_source, ';')) {
		char dsnbuf[1024];
		short dsnbuflen;

		use_direct = 1;

		/* Force UID and PWD to be set in the DSN */
		if (dbh->username && *dbh->username && !strstr(dbh->data_source, "uid")
				&& !strstr(dbh->data_source, "UID")) {
			char *dsn = pemalloc(strlen(dbh->data_source) + strlen(dbh->username) + strlen(dbh->password) + sizeof(";UID=;PWD="), dbh->is_persistent);
			sprintf(dsn, "%s;UID=%s;PWD=%s", dbh->data_source, dbh->username, dbh->password);
			pefree((char*)dbh->data_source, dbh->is_persistent);
			dbh->data_source = dsn;
		}

		rc = SQLDriverConnect(H->dbc, NULL, (char*)dbh->data_source, strlen(dbh->data_source),
				dsnbuf, sizeof(dsnbuf)-1, &dsnbuflen, SQL_DRIVER_NOPROMPT);
	}
	if (!use_direct) {
		rc = SQLConnect(H->dbc, (char*)dbh->data_source, SQL_NTS, dbh->username, SQL_NTS, dbh->password, SQL_NTS);
	}

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_drv_error(use_direct ? "SQLDriverConnect" : "SQLConnect");
		goto fail;
	}

	/* TODO: if we want to play nicely, we should check to see if the driver really supports ODBC v3 or not */

	dbh->methods = &odbc_methods;
	dbh->alloc_own_columns = 1;
	
	return 1;

fail:
	dbh->methods = &odbc_methods;
	return 0;
}
/* }}} */

pdo_driver_t pdo_odbc_driver = {
	PDO_DRIVER_HEADER(odbc),
	pdo_odbc_handle_factory
};

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */