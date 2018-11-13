/* SYSIFCOPT(*IFSIO) TERASPACE(*YES *TSIFC) STGMDL(*SNGLVL)      */
/* SYSIFCOPT(*IFSIO) TERASPACE(*YES *TSIFC) STGMDL(*SNGLVL)      */
/* COMPILEOPT('OUTPUT(*PRINT) OPTION(*EXPMAC *SHOWINC)')         */
/* Program . . . : JXM002                                        */
/* Design  . . . : Niels Liisberg                                */
/* Function  . . : SQL database I/O                              */
/*                                                               */
/*  SQLCLI documentation:


https://www.ibm.com/support/knowledgecenter/ssw_ibm_i_73/cli/rzadphdapi.htm?lang=da
                                                                */
/* By     Date       PTF     Description                         */
/* NL     21.10.2006         New program                         */
/* NL     16.10.2014         Added to JSONXML library            */
/* ------------------------------------------------------------- */

#include <unistd.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <decimal.h>
#include <stdarg.h>
#include <ctype.h>
#include "ostypes.h"
#include "varchar.h"
#include "xlate.h"
#include "strutil.h"
#include "minmax.h"
#include "parms.h"
#include "noxdb.h"
#include "mem001.h"
#include "timestamp.h"

// Globlas: TODO !!! remove to make code reintrant
__thread extern UCHAR jxMessage[512];
__thread extern BOOL  jxError;

// Unit Globals:
static PJXSQLCONNECT pConnection = NULL;
static BOOL keepConnection = false;
__thread static SQLINTEGER sqlCode = 0;

static enum  {
   UNCONNECTED,
   AUTOCONNECTED,
   HOSTED
} connectionMode;

typedef _Packed struct _SQLCHUNK {
   SQLINTEGER actLen;
   SQLINTEGER chunkLen;
   SQLINTEGER offset;
   PUCHAR value;
} SQLCHUNK, *PSQLCHUNK;

void          jx_sqlDisconnect (void);
PJXSQLCONNECT jx_sqlNewConnection(void);
void jx_traceOpen (PJXSQLCONNECT pCon);
void jx_traceInsert (PJXSQL pSQL, PUCHAR stmt , PUCHAR sqlState);

/* ------------------------------------------------------------- */
SQLINTEGER jx_sqlCode(void)
{
	return sqlCode;
}
/* ------------------------------------------------------------- */
static SQLINTEGER getSqlCode(SQLHSTMT hStmt)
{
	SQLCHAR        buffer[SQL_MAX_MESSAGE_LENGTH + 1];
	SQLCHAR        sqlstate[SQL_SQLSTATE_SIZE + 1];
	SQLSMALLINT    length;

	SQLError(SQL_NULL_HENV, SQL_NULL_HDBC, hStmt,
		sqlstate,
		&sqlCode,
		buffer,
		SQL_MAX_MESSAGE_LENGTH + 1,
		&length
	);
	return sqlCode;
}
/* ------------------------------------------------------------- */
static int check_error (PJXSQL pSQL)
{
	SQLSMALLINT length;
	ULONG l;
	int rc;

	SQLHANDLE     handle;
	SQLSMALLINT   hType = SQL_HANDLE_ENV;

	SQLHENV       henv  = SQL_NULL_HENV;
	SQLHDBC       hdbc  = SQL_NULL_HDBC;
	SQLHSTMT      hstmt = pSQL && pSQL->pstmt? pSQL->pstmt->hstmt : SQL_NULL_HSTMT;
	UCHAR         sqlState[5];
	UCHAR         sqlMsgDta[SQL_MAX_MESSAGE_LENGTH + 1];
	PUCHAR        psqlState  = sqlState;
	PLONG         psqlCode   = &sqlCode;
	PUCHAR        psqlMsgDta = sqlMsgDta;

	if (pSQL && pSQL->pstmt) {
		hType  =  SQL_HANDLE_STMT;
		handle =  pSQL->pstmt->hstmt;
	} else if ( pConnection) {
		if (pConnection->hdbc) {
			hType  =  SQL_HANDLE_DBC;
			handle =  pConnection->hdbc;
		} else {
			hType  =  SQL_HANDLE_ENV;
			handle =  pConnection->henv;
		}
		psqlState  = pConnection->sqlState;
		psqlCode   = &pConnection->sqlCode;
		psqlMsgDta = pConnection->sqlMsgDta;
	}

	length = 0;
	rc = SQLGetDiagRec(hType , handle, 1, psqlState, psqlCode, psqlMsgDta,  sizeof(sqlMsgDta), &length);
	sprintf( jxMessage , "%-5.5s %-*.*s" , psqlState, length, length, psqlMsgDta);
	jx_sqlClose (&pSQL); // Free the data
	jxError = true;

	return;
}
/* ------------------------------------------------------------- */
static PJXSQLCONNECT jx_sqlNewConnection(void )
{
	// static SQLHSTMT      hstmt = 0 ;
	// SQLINTEGEREGER    len;
	// UCHAR Label [256];
	// LGL  err = ON;
	// LONG rows =0;

	// PJXSQLCONNECT pConnection;  I hate this - bu need to go global for now !!
	LONG          attrParm;
	PUCHAR        server = "*LOCAL";
	int rc;
	PSQLOPTIONS po;

	pConnection = memAlloc(sizeof(JXSQLCONNECT));
	memset(pConnection , 0 , sizeof(JXSQLCONNECT));
	pConnection->sqlTrace.handle = -1;
	pConnection->pCd = XlateXdOpen (13488, 0);
	po = &pConnection->options;
	po->upperCaseColName = OFF;
	po->autoParseContent = ON;
	po->DecimalPoint     = '.';
	po->hexSort          = OFF;
	po->sqlNaming        = OFF;
	po->DateSep          = '-';
	po->DateFmt          = 'y';
	po->TimeSep          = ':';
	po->TimeFmt          = 'H';

	// allocate an environment handle
	rc = SQLAllocEnv (&pConnection->henv);
	if (rc != SQL_SUCCESS ) {
		check_error (NULL);
		jx_sqlDisconnect ();
		return NULL; // we have an error
	}

	// Note - this is invers: Default to IBMi naming
	attrParm = pConnection->options.sqlNaming == ON ? SQL_FALSE : SQL_TRUE;
	rc = SQLSetEnvAttr  (pConnection->henv, SQL_ATTR_SYS_NAMING, &attrParm  , 0);
	/* Dont test since the activations groupe might be reclaimed, and a new "session" is on..
	if (rc != SQL_SUCCESS ) {
		check_error (NULL);
		jx_sqlDisconnect ();
		return NULL; // we have an error
	}
	... */

	/* TODO !!! always use UTF-8 */
	attrParm = SQL_TRUE;
	rc = SQLSetEnvAttr  (pConnection->henv, SQL_ATTR_UTF8 , &attrParm  , 0);
	if (rc != SQL_SUCCESS ) {
		jx_sqlDisconnect ();
		return NULL; // we have an error
	}


	attrParm = SQL_TRUE;
	rc = SQLSetEnvAttr  (pConnection->henv, SQL_ATTR_JOB_SORT_SEQUENCE , &attrParm  , 0);
	if (rc != SQL_SUCCESS ) {
		jx_sqlDisconnect ();
		return NULL; // we have an error
	}

	rc = SQLAllocConnect (pConnection->henv, &pConnection->hdbc);  // allocate a connection handle
	if (rc != SQL_SUCCESS ) {
		check_error (NULL);
		jx_sqlDisconnect ();
		return NULL; // we have an error
	}

	attrParm = SQL_TXN_NO_COMMIT; // does not work with BLOBS
	// attrParm = SQL_TXN_READ_UNCOMMITTED; // does not work for updates !!! can noet bes pr- statemnet
	rc = SQLSetConnectAttr (pConnection->hdbc, SQL_ATTR_COMMIT , &attrParm  , 0);
	if (rc != SQL_SUCCESS ) {
		check_error (NULL);
		jx_sqlDisconnect ();
		return NULL; // we have an error
	}

	rc = SQLConnect (pConnection->hdbc, server , SQL_NTS, NULL, SQL_NTS, NULL, SQL_NTS);
	if (rc != SQL_SUCCESS ) {
		check_error (NULL);
		jx_sqlDisconnect ();
		return NULL; // we have an error
	}

	// If required, open the trace table
	jx_traceOpen (pConnection);

	return pConnection; // we are ok

}
/* ------------------------------------------------------------- */
static PJXSQLCONNECT jx_getCurrentConnection(void)
{

	if (pConnection == NULL) {
		pConnection = jx_sqlNewConnection ();
	}
	return pConnection;
}
/* ------------------------------------------------------------- */
static int sqlEscape (PUCHAR out  , PUCHAR in)
{
    PUCHAR p = out;
    UCHAR  fnyt = '\'';
    int len;

    *p++ = fnyt;
    for (;*in; in++) {
		if (*in == fnyt) {
			*p++ = fnyt;
		}
		*p++ = *in;
   	}
	*p++ = fnyt;
	*p   = '\0';
	len = p - out;
	return len;
}
/* ------------------------------------------------------------- */
static int insertMarkerValue (PUCHAR buf , PUCHAR marker, PJXNODE parms)
{
	int len =0;
	PJXNODE pNode;
	PUCHAR value;

	pNode = jx_GetNode   ( parms , marker);
	if (pNode) {
		value = jx_GetValuePtr  ( pNode , "" , NULL );
		if (pNode->isLiteral) {
			strcpy(buf, value);
			len = strlen(buf);
		} else {
			len = sqlEscape (buf , value);
		}
	}
	return len;
}
/* ------------------------------------------------------------- */
int jx_sqlExecDirectTrace(PJXSQL pSQL , int hstmt, PUCHAR sqlstmt)
{

	int rc, rc2;
	SQLSMALLINT   length   = 0;
	LONG          lrc;
	PUCHAR        psqlState  = "";
	PJXTRACE      pTrc = &pConnection->sqlTrace;

	sqlCode = 0; // TODO re-entrant !!
	memset ( pConnection->sqlState , ' ' , 5);
	ts_nowstr(pTrc->tsStart); // TODO !!! not form global
	rc = SQLExecDirect( hstmt, sqlstmt, SQL_NTS);
	if (rc != SQL_SUCCESS) {
		rc2= SQLGetDiagRec(SQL_HANDLE_STMT,hstmt,1,pConnection->sqlState,&sqlCode, pTrc->text,sizeof(pTrc->text), &length);
		sprintf( jxMessage , "%-5.5s %0.*s" , pConnection->sqlState , length, pTrc->text);
		jx_sqlClose (&pSQL); // Free the data
	}
	pTrc->text [length] = '\0';
	ts_nowstr(pTrc->tsEnd); // TODO !!! not form global
	jx_traceInsert ( pSQL , sqlstmt , pConnection->sqlState);
	return rc; // we have an error
}
/* ------------------------------------------------------------- */
PUCHAR strFormat (PUCHAR out, PUCHAR in , PJXNODE parms)
{
	PUCHAR p, pMarker, res = out;
	PJXNODE pParms = parms;
	int markerLen ;
	UCHAR marker [64];
	UCHAR dollar = 0x67;


	if (parms == NULL) {
		strcpy(out , in);
		return out;
	}

	if (OFF == jx_isNode (parms)) {
	pParms =  jx_ParseString((PUCHAR) parms, NULL);
	}

	while (*in) {
		if (*in == dollar) {
			pMarker = ++in;
			for (;isalnum(*in) || *in=='.' || *in=='/' || *in=='_' ; in++);
			markerLen = in - pMarker ;
			if (markerLen > 0) {
				substr(marker , pMarker , markerLen );
				out += insertMarkerValue (out , marker, pParms );
			}
		} else {
			*(out++) = *(in++);
		}
	}
	*(out++) =  '\0';

	if (OFF == jx_isNode (parms)) {
		jx_Close (&pParms);
	}

	return res;
}
/* ------------------------------------------------------------- */
static PJXSQL jx_sqlNewStatement(PJXNODE pSqlParms, BOOL exec, BOOL scroll)
{
	PJXSQL pSQL;
	PJXSQLCONNECT pc;
	PJXSQLSTMT pStmt;
	SHORT i;
	int rc;
	LONG   attrParm;

	sqlCode  = 0;
	pSQL = memAllocClear(sizeof(JXSQL));
	pSQL->rowcount = -1;

	// build or get the connection
	pc = jx_getCurrentConnection();
	if (pc == NULL) return NULL;

	pSQL->pstmt        = memAlloc(sizeof(JXSQLSTMT));
	pSQL->pstmt->hstmt = 0;
	pSQL->pstmt->exec  = exec;

	// allocate  and initialize with defaults
	rc = SQLAllocStmt(pConnection->hdbc, &pSQL->pstmt->hstmt);
	if (rc != SQL_SUCCESS ) {
		check_error (pSQL);
		return NULL; // we have an error
	}

	if (exec) {
		// This need to allow update
		attrParm = SQL_INSENSITIVE;
		rc = SQLSetStmtAttr  (pSQL->pstmt->hstmt, SQL_ATTR_CURSOR_SENSITIVITY , &attrParm  , 0);
		if (rc != SQL_SUCCESS ) {
			check_error (pSQL);
			return NULL; // we have an error
		}

	} else {
		if (scroll) {
			attrParm = SQL_TRUE;
			rc = SQLSetStmtAttr  (pSQL->pstmt->hstmt, SQL_ATTR_CURSOR_SCROLLABLE , &attrParm  , 0);
			if (rc != SQL_SUCCESS ) {
				check_error (pSQL);
				return NULL; // we have an error
			}
		}

		attrParm = SQL_TRUE;
		rc = SQLSetStmtAttr  (pSQL->pstmt->hstmt, SQL_ATTR_EXTENDED_COL_INFO , &attrParm  , 0);
		if (rc != SQL_SUCCESS ) {
			check_error (pSQL);
			return NULL; // we have an error
		}


		attrParm = SQL_CONCUR_READ_ONLY;
		rc = SQLSetStmtAttr  (pSQL->pstmt->hstmt, SQL_ATTR_CONCURRENCY , &attrParm  , 0);
		if (rc != SQL_SUCCESS ) {
			check_error (pSQL);
			return NULL; // we have an error
		}
	}
	return pSQL;
}
/* ------------------------------------------------------------- */
/* ------------------------------------------------------------- */
PJXSQL jx_sqlOpen(PUCHAR sqlstmt , PJXNODE pSqlParmsP, BOOL scroll)
{

	UCHAR sqlTempStmt[32766];
	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	PJXNODE pSqlParms  =  (pParms->OpDescList->NbrOfParms >= 2 ) ? pSqlParmsP : NULL;
	LONG   attrParm;
	LONG   i;
	//   PJXSQL pSQL = jx_sqlNewStatement (pParms->OpDescList->NbrOfParms >= 2 ? pSqlParms  :NULL);
	PJXSQL pSQL;
	SQLINTEGER len, descLen, isTrue;
	int rc;

	if (pParms->OpDescList->NbrOfParms <= 2 ) scroll = true;
	jxError = false; // Assume OK

	pSQL = jx_sqlNewStatement (NULL, false, scroll);
	if  ( pSQL == NULL) return NULL;

	if ( pConnection->options.hexSort == ON ) {
		LONG attrParm = SQL_FALSE ;
		rc = SQLSetEnvAttr  (pConnection->henv, SQL_ATTR_JOB_SORT_SEQUENCE , &attrParm  , 0);
	}

	// build the final sql statement
	strFormat(sqlTempStmt , sqlstmt , pSqlParms);

	//// huxi !! need uncomitted read for blob fields
	// and IBMi does not support statement attribute to set the pr statement. :/
	// so we simply append the "ur" uncommited read options
	strcat ( sqlTempStmt , " with ur");
	pSQL->sqlstmt = strdup(sqlTempStmt);

	rc = jx_sqlExecDirectTrace(pSQL , pSQL->pstmt->hstmt, pSQL->sqlstmt);
	if (rc != SQL_SUCCESS ) {
		// is checked in abowe    check_error (pSQL);
		return NULL; // we have an error
	}

	// Number of rows? .. No does not work :(
	/*
	SQLGetDiagField(SQL_HANDLE_STMT,pSQL->pstmt->hstmt, 0 ,SQL_DIAG_ROW_COUNT,&pSQL->rowcount,0, NULL);
	*/

	/*
	// Row count is only affected row in a "delete" or "update" ..TODO find a solution for select
	rc = SQLRowCount (pSQL->pstmt->hstmt, &pSQL->rowcount);
	if (rc != SQL_SUCCESS ) {
		check_error (pSQL);
		return NULL; // we have an error
	}
	*/

	rc = SQLNumResultCols (pSQL->pstmt->hstmt, &pSQL->nresultcols);
	if (rc != SQL_SUCCESS ) {
		check_error (pSQL);
		return NULL; // we have an error
	}
	pSQL->cols = memAlloc (pSQL->nresultcols * sizeof(JXCOL));

	for (i = 0; i < pSQL->nresultcols; i++) {

		PJXCOL pCol = &pSQL->cols[i];

		SQLDescribeCol (pSQL->pstmt->hstmt, i+1, pCol->colname, sizeof (pCol->colname),
			&pCol->colnamelen, &pCol->coltype, &pCol->collen, &pCol->scale, &pCol->nullable);

		pCol->colname[pCol->colnamelen] = '\0';

		// If all uppsercase ( not given name by .. AS "newName") the lowercase
		if (OFF == jx_IsTrue (pConnection->pOptions ,"uppercasecolname")) {
			UCHAR temp [256];
			str2upper  (temp , pCol->colname);
			if (strcmp (temp , pCol->colname) == 0) {
				str2lower  (pCol->colname , pCol->colname);
			}
		}

		// is it an ID column ? get the label, if no label then use the column name
		isTrue = SQL_FALSE;
		rc = SQLColAttributes (pSQL->pstmt->hstmt,i+1,SQL_DESC_AUTO_INCREMENT, NULL, 0, NULL ,&isTrue);
		pCol->isId = isTrue == SQL_TRUE;

		// get the label, if no label then use the column name
		// NOTE in ver 5.4 this only return the 10 first chars ...
		rc = SQLColAttributes (pSQL->pstmt->hstmt,i+1,SQL_DESC_LABEL, pCol->header, 127,&len,&descLen);

		// No headers, if none provided
		if (rc != SQL_SUCCESS ) {
			strcpy(pCol->header ,  pCol->colname);
		} else {
			pCol->header[len] =  '\0';
		}

		// get display length for column
		SQLColAttributes (pSQL->pstmt->hstmt, i+1, SQL_DESC_PRECISION, NULL, 0,NULL, &pCol->displaysize);

		// set column length to max of display length, and column name
		//   length.  Plus one byte for null terminator
		// collen[i] = max(displaysize, collen[i]);
		// collen[i] = max(collen[i], strlen((char *) colname) ) + 1;

		// printf ("%-*.*s", collen[i], collen[i], colname);

		// allocate memory to bind column

		// bind columns to program vars, converting all types to CHAR
		//*SQLBindCol (hstmt, i+1, SQL_C_CHAR, data[i], collen[i], &outlen[i]);

		// NOTE: Timestamp returns 6!! first time for displaysize  -
		if ( pCol->displaysize <  pCol->collen) {
			pCol->displaysize = pCol->collen;
		}

		switch( pCol->coltype) {
			case SQL_BLOB:
			case SQL_CLOB:
	//           pCol->collen = pCol->displaysize * 2;
	//           pCol->data = (SQLCHAR *) malloc (pCol->collen);
			pCol->collen = 1048576;  // 1MEGABYTES
	//            pCol->data = (SQLCHAR *) malloc (pCol->collen);  // 1MEGABYTES
	//            rc = SQLBindCol (pSQL->pstmt->hstmt, i+1, SQL_C_BINARY , pCol->data, pCol->collen, &pCol->outlen);
			break;

			case SQL_WCHAR:
			case SQL_WVARCHAR:
			case SQL_GRAPHIC:
			case SQL_VARGRAPHIC:
				pCol->collen = pCol->displaysize * 2;
	//          pCol->data = (SQLCHAR *) malloc (pCol->collen);
	//          rc = SQLBindCol (pSQL->pstmt->hstmt, i+1, pCol->coltype, pCol->data, pCol->collen, &pCol->outlen);
			break;

			// TODO !! FIX - SQLCLI resturn 6 first time for timestamp...
			/*
			case SQL_TIMESTAMP:
			pCol->collen = 27; // and zero termination
			pCol->data = (SQLCHAR *) malloc (pCol->collen);
			rc = SQLBindCol (pSQL->pstmt->hstmt, i+1, SQL_C_CHAR, pCol->data, pCol->collen, &pCol->outlen);
			break;
			*/

			default:
				pCol->collen = pCol->displaysize + 3; // + . and , and zero term
	//          pCol->data = (SQLCHAR *) malloc (pCol->collen);
	//          rc = SQLBindCol (pSQL->pstmt->hstmt, i+1, SQL_C_CHAR, pCol->data, pCol->collen, &pCol->outlen);
			break;
		}
		if (pCol->coltype >= SQL_NUMERIC && pCol->coltype <= SQL_DOUBLE) {
			pCol->nodeType = JX_LITERAL;
		} else {
			pCol->nodeType = JX_VALUE;
		}
	}

	return pSQL;

}
/* ------------------------------------------------------------- */
PJXNODE jx_sqlFormatRow  (PJXSQL pSQL)
{
	int i;
	PJXNODE pRow;
	SQLINTEGER buflen, datatype;

	if ( pSQL->rc == SQL_SUCCESS
	||   pSQL->rc == SQL_SUCCESS_WITH_INFO ) {
		jxError = false;

		pRow = jx_NewObject(NULL);

		for (i = 0; i < pSQL->nresultcols; i++) {

			PJXCOL pCol = &pSQL->cols[i];
			UCHAR buf [1048576]; // one meg

			// TODO - Work arround !!! first get the length - if null, the dont try the get data
			// If it has a pointer value, the API will fail..
			// For now BLOB and CLOB does not support "not null with default"
			// buflen =0;
			// SQLGetCol (pSQL->pstmt->hstmt, i+1, SQL_BLOB, NULL , 0, &buflen);
			switch (pCol->coltype) {
			case SQL_BLOB:
			case SQL_CLOB:
			case SQL_WCHAR:
			case SQL_WVARCHAR:
			case SQL_GRAPHIC:
			case SQL_VARGRAPHIC:
				SQLGetCol (pSQL->pstmt->hstmt, i+1, pCol->coltype, buf , sizeof(buf), &buflen);
				break;
			default:
				SQLGetCol (pSQL->pstmt->hstmt, i+1, SQL_CHAR, buf , sizeof(buf), &buflen);
			}


			// Null data is the same for all types
			if (buflen  ==  SQL_NULL_DATA) {
				jx_NodeAdd (pRow , RL_LAST_CHILD, pCol->colname , NULL,  JX_LITERAL );
			} else {

				buf[buflen] = '\0';

				switch( pCol->coltype) {
					case SQL_WCHAR:
					case SQL_WVARCHAR:
					case SQL_GRAPHIC:
					case SQL_VARGRAPHIC: {
						UCHAR temp [32768];
						PUCHAR pInBuf = buf;
						size_t OutLen;
						size_t inbytesleft;

						if (pCol->coltype ==  SQL_WVARCHAR) {
						inbytesleft = (* (PSHORT) pInBuf) * 2; // Peak the length, and unicode uses two bytes
						pInBuf += 2; // skip len
						} else {
							for  (inbytesleft = pCol->collen; inbytesleft > 0 ; inbytesleft -= 2) {
							if ( * (PSHORT) (pInBuf + inbytesleft - 2) > 0x0020) break;
							}
						}
						OutLen = XlateXdBuf  (pConnection->pCd, temp , pInBuf, inbytesleft);
						temp[OutLen] = '\0';

						jx_NodeAdd (pRow , RL_LAST_CHILD, pCol->colname , temp,  pCol->nodeType );

						break;
					}

					case SQL_NUMERIC:
					case SQL_DECIMAL:
					case SQL_FLOAT:
					case SQL_REAL:
					case SQL_DOUBLE: {
						PUCHAR p = buf;
						int len;
						// skip leading blanks
						for (;*p == ' '; p++);

						len = strTrimLen(p);
						p[len] = '\0';

						// Have to fix .00 numeric as 0.00
						// and -.8  as -0.8
						if (*p == '.') {
							memmove (p+1 , p, len+1);
							*p = '0';
						} else if (0==memcmp(p, "-.",2)){
							memmove (p+1 , p, len+1);
							*(p+1) = '0';

						} 

						jx_NodeAdd (pRow , RL_LAST_CHILD, pCol->colname , p,  pCol->nodeType );
						break ;
					}

					default: {
						PUCHAR p = buf;
						int len;

						if (pCol->coltype != SQL_BLOB
						&&  pCol->coltype != SQL_CLOB) {
							len = strTrimLen(p);
							p[len] = '\0';
						}

						// trigger new parsing of JSON-objects in columns:
						// Predicts json data i columns
						if (pConnection->options.autoParseContent == ON) {
							if (*p == BRABEG || *p == CURBEG) {
							PJXNODE pNode = jx_ParseString(p, NULL);
							if (pNode) {
								jx_NodeRename(pNode, pCol->colname);
								jx_NodeAddChildTail (pRow, pNode);
								break;
							}
							}
						}

						jx_NodeAdd (pRow , RL_LAST_CHILD, pCol->colname , p,  pCol->nodeType );
						break;
					}
				}
			}
		}
		return pRow; // Found

	} else {
		sqlCode = getSqlCode(pSQL->pstmt->hstmt);
		if (pSQL->rc != SQL_NO_DATA_FOUND ) {
			check_error (pSQL);
		}
	}

	return NULL; // not found
}
/* ------------------------------------------------------------- */
PJXNODE jx_sqlFetchRelative (PJXSQL pSQL, LONG fromRow)
{
	int rc;
	if (pSQL == NULL) return (NULL);

	pSQL->rc = SQLFetchScroll (pSQL->pstmt->hstmt, SQL_FETCH_RELATIVE , (fromRow < 1) ? 1: fromRow);
	return jx_sqlFormatRow(pSQL);
}
/* ------------------------------------------------------------- */
PJXNODE jx_sqlFetchNext (PJXSQL pSQL)
{
	int rc;
	if (pSQL == NULL) return (NULL);

	// List next row from the result set
	pSQL->rc = SQLFetch (pSQL->pstmt->hstmt);
	return jx_sqlFormatRow(pSQL);
}
/* ------------------------------------------------------------- */
PJXNODE jx_sqlFetchFirst (PJXSQL pSQL, LONG fromRow)
{
	int rc;
	if (pSQL == NULL) return (NULL);

	if (fromRow > 1) {
		pSQL->rc = SQLFetchScroll (pSQL->pstmt->hstmt, SQL_FETCH_RELATIVE , fromRow);
	} else {
		pSQL->rc = SQLFetch (pSQL->pstmt->hstmt);
	}
	return jx_sqlFormatRow(pSQL);
}
/* ------------------------------------------------------------- */
void jx_sqlClose (PJXSQL * ppSQL)
{
	int i;
	int rc;
	PJXSQL pSQL = * ppSQL;

	if (pConnection->options.hexSort == ON ) {
		LONG attrParm = SQL_TRUE ;
		rc = SQLSetEnvAttr (pConnection->henv, SQL_ATTR_JOB_SORT_SEQUENCE , &attrParm  , 0);
		pConnection->options.hexSort = OFF;
	}

	// Do we have an active statement ...
	if (pSQL) {

		memFree(&pSQL->cols);
		pSQL->nresultcols = 0; // !! Done

		if (pSQL->pstmt) {
			if (! pSQL->pstmt->exec) {
				// Clean up for read cursor statements:
				rc = SQLCloseCursor(pSQL->pstmt->hstmt);
			}
			rc = SQLFreeHandle (SQL_HANDLE_STMT, pSQL->pstmt->hstmt);
			memFree (&pSQL->pstmt);
		}

		if (pSQL->sqlstmt) {
			free  (pSQL->sqlstmt);
			pSQL->sqlstmt = null;
		}
		memFree(ppSQL);  // Note!! memFree takes the address of the pointer, and nulls it afterwards
		* ppSQL  = NULL;
	}

}
/* ------------------------------------------------------------- */
void jx_sqlKeepConnection (BOOL keep)
{
   keepConnection = keep;
}
/* ------------------------------------------------------------- */
void jx_sqlDisconnect (void)
{

	int rc;

	// if (keepConnection )  return;

	if (pConnection == NULL) return;

	XlateXdClose(pConnection->pCd) ;

	if (pConnection->sqlTrace.handle != -1) {
		rc = SQLFreeHandle (SQL_HANDLE_STMT, pConnection->sqlTrace.handle);
		pConnection->sqlTrace.handle = -1;
	}

	// disconnect from database
	if (pConnection->hdbc != -1) {
		SQLDisconnect  (pConnection->hdbc);
		SQLFreeConnect (pConnection->hdbc);
		pConnection->hdbc = -1;
	}

	// free environment handle
	if (pConnection->henv != -1) {
		SQLFreeEnv (pConnection->henv);
		pConnection->henv = -1;
	}

	jx_Close(&pConnection->pOptions);
	memFree (&pConnection);

}
/* ------------------------------------------------------------- */
PJXNODE jx_buildMetaFields ( PJXSQL pSQL )
{
	int rc;
	LONG    attrParm;
	PJXNODE pFields;
	int i;

	if (pSQL == NULL) return(NULL);

	pFields  = jx_NewArray(NULL);

	/*****  need to be done before the cursor is closed !!

	attrParm = SQL_TRUE;
	rc = SQLSetStmtAttr  (pSQL->pstmt->hstmt, SQL_ATTR_EXTENDED_COL_INFO , &attrParm  , 0);
	if (rc != SQL_SUCCESS ) {
		check_error (pSQL);
		return NULL; // we have an error
	}
	******/

	for (i = 1; i <= pSQL->nresultcols; i++) {
		PJXNODE pField  = jx_NewObject (NULL);
		PJXCOL  pCol     = &pSQL->cols[i-1];
		PUCHAR  type = "string";
		UCHAR   temp [256];
		SQLINTEGER templen;
		SQLINTEGER descNo;

		// Add name
		jx_NodeAdd (pField  , RL_LAST_CHILD, "name" , pCol->colname,  VALUE );

		// Add type
		switch( pCol->coltype) {
			case SQL_BLOB:        type = "blob"           ; break;
			case SQL_CLOB:        type = "clob"           ; break;
			case SQL_CHAR:        type = "char"           ; break;
			case SQL_VARCHAR:     type = "varchar"        ; break;
			case SQL_DATETIME:    type = "datetime"       ; break;
			case SQL_DATE:        type = "date"           ; break;
			case SQL_TIME:        type = "time"           ; break;
			case SQL_TIMESTAMP:   type = "timestamp"      ; break;
			case SQL_WCHAR:       type = "wchar"          ; break;
			case SQL_WVARCHAR:    type = "wvarchar"       ; break;
			case SQL_GRAPHIC:     type = "graphic"        ; break;
			case SQL_VARGRAPHIC:  type = "vargraphic"     ; break;
			default: {
			if (pCol->coltype >= SQL_NUMERIC && pCol->coltype <= SQL_DOUBLE ) {
				if (pCol->scale > 0) {
					type = "dec"     ;
				} else {
					type = "int"     ;
				}
			} else {
				sprintf(temp ,"unknown%d" , pCol->coltype);
				type = temp;
			}
			}
		}
		jx_NodeAdd (pField  , RL_LAST_CHILD, "datatype" , type,  VALUE );

		sprintf(temp , "%d" ,  pCol->coltype);
		jx_NodeAdd (pField  , RL_LAST_CHILD, "sqltype" , temp ,  LITERAL);

		// Add size
		sprintf(temp , "%d" , pCol->displaysize);
		jx_NodeAdd (pField  , RL_LAST_CHILD, "size"     , temp,  LITERAL  );

		// Add decimal precission
		if  (pCol->coltype >= SQL_NUMERIC && pCol->coltype <= SQL_DOUBLE
		&&   pCol->scale > 0) {
			sprintf(temp , "%d" , pCol->scale);
			jx_NodeAdd (pField  , RL_LAST_CHILD, "prec"     , temp,  LITERAL  );
		}

		jx_NodeAdd (pField  , RL_LAST_CHILD, "header" , pCol->header, VALUE  );

		// Push to array
		jx_ArrayPush (pFields , pField, FALSE);
	}

	// jx_Debug ("Fields:", pFields);
	return  pFields;

}
/* ------------------------------------------------------------- */
LONG jx_sqlNumberOfRows(PUCHAR sqlstmt)
{

	LONG    rowCount, para = 0;
	PJXNODE pRow;
	PUCHAR p, w, lastSelect, orderby, from, withur;
	UCHAR  str2 [32766];

	// replace the coloumn list with a count(*) - TODO !! This will not work on a "with" statment

	// Find the last select ( there can be more when using "with")

	p = sqlstmt;
	w = stristr(p, "with ");
	p = stristr(p , "select ");

	if (w == NULL || w > p) {
		lastSelect = p;
	} else {
		for(;;) {
			if (w == NULL) break;
			switch (*w) {
				case '(':
					para++;
					break;
				case ')':
					para--;
					if (para == 0) {
						for (w++; *w == ' '; w++);
						if ( *w != ',') {
						lastSelect = stristr(w , "select ");
						goto outer;
						}
					}
					break;
			}
		w++;
		}
	}
	outer:
	if (lastSelect == NULL) return 0;

	// We need to replace all columns between "select" and  "from"  with the count(*)
	from  = stristr(lastSelect , " from ");
	if (from == NULL) return 0;

	// remove order by - if any
	orderby = stristr(from  , " order ");
	if (orderby) {
		*orderby = '\0';
	}

	// remove "with ur" - if any
	withur = stristr(from  , " with ur");
	if (withur) {
		*withur = '\0';
	}

	// rebuild the select statement as a "select count(*) from ..."
	substr (str2 , sqlstmt , lastSelect - sqlstmt); // if a "With" exists then grab that
	strcat (str2 ,"select count(*) as counter" );
	strcat (str2 , from );

	// Get that only row
	pRow = jx_sqlResultRow(str2, NULL);

	rowCount = atoi(jx_GetValuePtr(pRow, "counter", NULL));

	jx_NodeDelete (pRow);

	return rowCount;
}
/* ------------------------------------------------------------- */

/***********
LONG jx_sqlNumberOfRowsDiag(PUCHAR sqlstmt)
{

   LONG    rowCount, para = 0;
   PJXNODE pRow;
   PUCHAR p, w, lastSelect, orderby, from, withur;
   UCHAR  str2 [32766];

   strcat (str2 , "GET DIAGNOSTICS :v = DB2_NUMBER_ROWS" );

   // Get that only row
   pRow = jx_sqlResultRow(str2, NULL);

   rowCount = atoi(jx_GetValuePtr(pRow, "counter", NULL));

   jx_NodeDelete (pRow);

   return rowCount;
}
*/
/* ------------------------------------------------------------- */
void jx_sqlUpperCaseNames(PJXSQL pSQL)
{
	int i;
	for (i = 0; i < pSQL->nresultcols; i++) {
		PJXCOL pCol = &pSQL->cols[i];
		str2upper (pCol->colname , pCol->colname);
	}
}
/* ------------------------------------------------------------- */
LONG jx_sqlColumns (PJXSQL pSQL)
{
   if (pSQL == NULL) return -1;
   return (pSQL->nresultcols);
}
/* ------------------------------------------------------------- */
LONG jx_sqlRows (PJXSQL pSQL)
{
	if (pSQL == NULL) return -1;
	if (pSQL->rowcount == -1) {
		pSQL->rowcount = jx_sqlNumberOfRows(pSQL->sqlstmt);
	}

	return (pSQL->rowcount);
}
/* ------------------------------------------------------------- */
PJXNODE jx_sqlResultSet( PUCHAR sqlstmt, LONG startP, LONG limitP, LONG formatP , PJXNODE pSqlParmsP  )
{
	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	LONG    start     = (pParms->OpDescList->NbrOfParms >= 2) ? startP     : 1;  // From first row
	LONG    limit     = (pParms->OpDescList->NbrOfParms >= 3) ? limitP     : -1; // All row
	LONG    format    = (pParms->OpDescList->NbrOfParms >= 4) ? formatP    : 0;  // Arrray only
	PJXNODE pSqlParms = (pParms->OpDescList->NbrOfParms >= 5) ? pSqlParmsP : NULL;
	PJXNODE pRows     = jx_NewArray(NULL);
	PJXNODE pRow      ;
	PJXNODE pResult;
	PJXSQL  pSQL;
	LONG    i, rc;
	LONG rowCount=0;
	SHORT strLen=0;

	start = start < 1 ? 1 : start;

	pSQL = jx_sqlOpen(sqlstmt , pSqlParms, start > 1);
	if ( pSQL == NULL) {
		return NULL;
	}

	if (format & (JX_UPPERCASE)) {
		jx_sqlUpperCaseNames(pSQL);
	}

	pRow  = jx_sqlFetchFirst (pSQL, start);
	for (rowCount = 1; pRow && (rowCount <=limit || limit == -1); rowCount ++) {
		jx_ArrayPush (pRows , pRow, FALSE);
		pRow  = jx_sqlFetchNext (pSQL);
	}

	// need a object as return value
	if (format & (JX_META | JX_FIELDS | JX_TOTALROWS | JX_APROXIMATE_TOTALROWS)) {
		PJXNODE pMeta;
		pResult  = jx_NewObject(NULL);
		pMeta    = jx_NewObject(NULL);
		jx_SetValueByName(pResult  , "success" , "true" , LITERAL);
		jx_SetValueByName(pResult , "root"    , "rows" , VALUE);
		if (format & JX_FIELDS ) {
			PJXNODE pFields = jx_buildMetaFields (pSQL);
			jx_NodeMoveInto(pMeta , "fields" , pFields);
		}
		if (format & (JX_TOTALROWS | JX_APROXIMATE_TOTALROWS)) {
			jx_SetValueByName(pMeta , "totalProperty"   , "totalRows" , VALUE);

			if (format & JX_APROXIMATE_TOTALROWS ) {
				if (pRow) { // Yet more rows to come
					pSQL->rowcount = start + limit;
				} else {
					pSQL->rowcount = start + rowCount - 2; // "start" and "rowCount" count has both 1 as option base)
				}
			} else {
				pSQL->rowcount = jx_sqlNumberOfRows(sqlstmt);
			}
			jx_SetIntByName(pResult , "totalRows" , pSQL->rowcount );

			//SQLGetDiagField(SQL_HANDLE_STMT,
			//    pSQL->pstmt->hstmt,0,DB2_NUMBER_ROWS,&rowCount,SQL_INTEGER,&strLen);


			// SQL_INTEGER
			//rc=SQLGetDiagField(SQL_HANDLE_STMT,pSQL->pstmt->hstmt, 0 ,SQL_DIAG_ROW_COUNT,&rowCount,SQL_INTEGER,&strLen);
			// pSQL->rowcount = rowCount;

			jx_SetIntByName(pResult , "totalRows" , pSQL->rowcount );
		}

		jx_NodeMoveInto (pResult , "metaData" , pMeta);
		jx_NodeMoveInto (pResult , "rows"     , pRows);

	} else {

		// simply return the array of rows
		pResult = pRows;
	}

	jx_sqlClose (&pSQL);
	return pResult;

}
/* ------------------------------------------------------------- */
PJXNODE jx_sqlResultRowAt ( PUCHAR sqlstmt, LONG startP, PJXNODE pSqlParmsP )
{

	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	LONG    start     = (pParms->OpDescList->NbrOfParms >= 2) ? startP     : 1;  // From first row
	PJXNODE pSqlParms = (pParms->OpDescList->NbrOfParms >= 3) ? pSqlParmsP : NULL;
	PJXNODE pRow;
	PJXSQL  pSQL;

	pSQL = jx_sqlOpen(sqlstmt , pSqlParms, start > 1);
	pRow  = jx_sqlFetchFirst (pSQL, start);
	jx_sqlClose (&pSQL);
	return pRow;

}
/* ------------------------------------------------------------- */
PJXNODE jx_sqlGetMeta (PUCHAR sqlstmt)
{
	int i;
	PJXSQL  pSQL  = jx_sqlOpen(sqlstmt , NULL, false);
	PJXNODE pMeta = jx_buildMetaFields ( pSQL );
	jx_sqlClose (&pSQL);
	return pMeta;
}
/* ------------------------------------------------------------- */
PJXNODE jx_sqlResultRow ( PUCHAR sqlstmt, PJXNODE pSqlParmsP )
{

	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	PJXNODE pSqlParms = (pParms->OpDescList->NbrOfParms >= 2) ? pSqlParmsP : NULL;
	PJXNODE pRow;
	PJXSQL  pSQL;

	pSQL = jx_sqlOpen(sqlstmt , pSqlParms, false);
	pRow  = jx_sqlFetchNext (pSQL);
	jx_sqlClose (&pSQL);
	return pRow;

}
/* ------------------------------------------------------------- */
LGL jx_sqlExec(PUCHAR sqlstmt , PJXNODE pSqlParms)
{

	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	LONG   attrParm;
	LONG   i;
	int rc;
	//   PJXSQL pSQL = jx_sqlNewStatement (pParms->OpDescList->NbrOfParms >= 2 ? pSqlParms  :NULL);
	PJXSQL pSQL = jx_sqlNewStatement (NULL, true, false);

	// run  the  statement in "sqlstr"
	if (pParms->OpDescList->NbrOfParms >= 2) {
		UCHAR sqlTempStmt[32766];
		strFormat(sqlTempStmt , sqlstmt , pSqlParms);
		rc = jx_sqlExecDirectTrace(pSQL , pSQL->pstmt->hstmt, sqlTempStmt);
	} else {
		rc = jx_sqlExecDirectTrace(pSQL , pSQL->pstmt->hstmt, sqlstmt);
	}
	jx_sqlClose (&pSQL);
	return rc == SQL_SUCCESS ? OFF: ON;
}
/* ------------------------------------------------------------- */
/* .........
int getColType(SQLHSTMT hstmt , SHORT col )
{
   JXCOL Col;

   int rc = SQLDescribeCol (
      hstmt,
      col,
      Col.colname,
      sizeof (Col.colname),
      &Col.colnamelen,
      &Col.coltype,
      &Col.collen,
      &Col.scale,
      &Col.nullable
   );

   return (Col.coltype);
}
....... */
/* ------------------------------------------------------------- */
/* does this work ?  */
/* ------------------------------------------------------------- */
static BOOL isIdColumn(SQLHSTMT hstmt , int colno)
{
	int rc;
	SQLINTEGER isTrue;

	// is it an ID column ? get the label, if no label then use the column name
	isTrue = SQL_FALSE;
	rc = SQLColAttributes (hstmt, colno,SQL_DESC_AUTO_INCREMENT, NULL, 0, NULL ,&isTrue);
	// rc = SQLColAttributes (hstmt, colno,SQL_DESC_UPDATABLE, NULL, 0, NULL ,&isTrue);
	return (isTrue == SQL_TRUE);
}
/* ------------------------------------------------------------- */
/* This is required for this wird reason:                        */
/* If you update a row containing a CLOB or BLOB with            */
/* data over 32K it will fail and create a SQL trap error        */
/* if you also are updating another column with a bound          */
/* blank value.                                                  */
/*                                                               */
/* Therfor you will see tha blanks and nulls are updated         */
/* and inserted by SQL constants and not by bound markers        */
/* ------------------------------------------------------------- */
static BOOL  nodeisnull(PJXNODE pNode)
{
	PUCHAR val;

	if (pNode == NULL) return true;

	// has always a content ...
	if (pNode->type == ARRAY ||  pNode->type == OBJECT) {
		// .. so this will fail
		// pNode =  jx_GetNodeChild (pNode);
		// if (pNode != NULL) return false;
		return false;
	}

	val = jx_GetNodeValuePtr (pNode , NULL);
	return (val == null);
}
/* ------------------------------------------------------------- */
/* This is required for this wird reason:                        */
/* If you update a row containing a CLOB or BLOB with            */
/* data over 32K it will fail and create a SQL trap error        */
/* if you also are updating another column with a bound          */
/* blank value.                                                  */
/*                                                               */
/* Therfor you will see tha blanks and nulls are updated         */
/* and inserted by SQL constants and not by bound markers        */
/* ------------------------------------------------------------- */
static BOOL  nodeisblank(PJXNODE pNode)
{
	PUCHAR val;

	if (pNode == NULL) return true;

	// has always a content ...
	if (pNode->type == ARRAY ||  pNode->type == OBJECT) {
		// .. so this will fail
		// pNode =  jx_GetNodeChild (pNode);
		// if (pNode != NULL) return false;
		return false;
	}

	if (pNode->isLiteral) return false;
	val = jx_GetNodeValuePtr (pNode , null);
	if (val == null) return false;
	if (*val == 0) return true;
	return false;
}
/* ------------------------------------------------------------- */
static void buildUpdate (SQLHSTMT hstmt, SQLHSTMT hMetastmt,
   PUCHAR sqlStmt, PUCHAR table, PJXNODE pSqlParms , PUCHAR where)
{
	PUCHAR  stmt = sqlStmt;
	PUCHAR  comma = "";
	PJXNODE pNode;
	PUCHAR  name;
	int     colno;
	UCHAR   temp [128];

	stmt += sprintf (stmt , "update %s set " , table);

	pNode    =  jx_GetNodeChild (pSqlParms);
	for ( colno=1; pNode; colno++) {
		if (! isIdColumn(hMetastmt, colno)) {
			name  = jx_GetNodeNamePtr   (pNode);
			str2upper (temp  , name);   // Needed for national charse in columns names i.e.: BELØB
			if  (nodeisnull(pNode)) {
				stmt += sprintf (stmt , "%s%s=NULL" , comma , temp);
			} else if  (nodeisblank(pNode)) {
				stmt += sprintf (stmt , "%s%s=default" , comma , temp);    // because timesstamp / date can be set as ''
			} else {
				stmt += sprintf (stmt , "%s%s=?"  , comma , temp);
			}
			comma = ",";
		}
		pNode = jx_GetNodeNext(pNode);
	}

	stmt += sprintf (stmt , " %s " , where);
}
/* ------------------------------------------------------------- */
static void buildInsert  (SQLHSTMT hstmt, SQLHSTMT hMetaStmt,
   PUCHAR sqlStmt, PUCHAR table, PJXNODE pSqlParms , PUCHAR where)
{
	PUCHAR  stmt = sqlStmt;
	PUCHAR  comma = "";
	PJXNODE pNode;
	PUCHAR  name;
	PUCHAR  value;
	int     i,colno;
	UCHAR   markers[4096] ;
	UCHAR   temp [128];
	PUCHAR  pMarker = markers;

	stmt += sprintf (stmt , "insert into  %s (" , table);

	pNode = jx_GetNodeChild (pSqlParms);
	for ( colno=1; pNode; colno++) {
		if (! isIdColumn(hMetaStmt, colno)) {
			if (!nodeisnull(pNode)) {
				name     = jx_GetNodeNamePtr   (pNode);
				str2upper (temp  , name);   // Needed for national charse in columns names i.e.: BELØB
				stmt    += sprintf (stmt , "%s%s" , comma , temp);
				if  (nodeisblank(pNode)) {
					pMarker+= sprintf (pMarker , "%sdefault" , comma);    // because timesstamp / date can be set as ''
				} else {
					pMarker+= sprintf (pMarker , "%s?" , comma);
				}
				comma = ",";
			}
		}
		pNode = jx_GetNodeNext(pNode);
	}

	stmt += sprintf (stmt , ") values( ");
	stmt += sprintf (stmt , markers);
	stmt += sprintf (stmt , ")") ;
}
/* ------------------------------------------------------------- */
void createTracetable(PJXSQLCONNECT pCon)
{
	PJXTRACE pTrc = &pCon->sqlTrace;
	UCHAR  t [512];
	PUCHAR s = "CREATE            TABLE %s/sqlTrace (       "
		"   STSTART TIMESTAMP NOT NULL WITH DEFAULT,        "
		"   STEND TIMESTAMP NOT NULL WITH DEFAULT,          "
		"   STSQLSTATE CHAR(5) NOT NULL WITH DEFAULT,         "
		"   STTEXT VARCHAR ( 256) NOT NULL WITH DEFAULT,      "
		"   STJOB  VARCHAR ( 30) NOT NULL WITH DEFAULT,       "
		"   STTRID BIGINT NOT NULL WITH DEFAULT,              "
		"   STSQLSTMT VARCHAR ( 8192) NOT NULL WITH DEFAULT)  ";
	sprintf(t , s , pTrc->lib);
	pTrc->doTrace =  OFF; // So we don't end up in a recusive death spiral
	jx_sqlExec(t , NULL);
	pTrc->doTrace =  ON;
}
/* ------------------------------------------------------------- */
void jx_traceOpen (PJXSQLCONNECT pCon)
{
	PJXTRACE pTrc = &pCon->sqlTrace;
	int rc;
	PUCHAR insertStmt = "insert into sqltrace (STSTART,STEND,STSQLSTATE,STTEXT,STJOB,STTRID,STSQLSTMT) "
						"values (?,?,?,?,?,?,?)";

	JXM902 ( pTrc->lib , &pTrc->doTrace , pTrc->job);
	if (pTrc->doTrace == OFF) return;
	createTracetable(pCon);
	rc = SQLAllocStmt(pCon->hdbc, &pTrc->handle);
	rc = SQLPrepare(pTrc->handle , insertStmt, SQL_NTS);

	rc = SQLBindParameter(pTrc->handle,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_TIMESTAMP,26 ,0,pTrc->tsStart,0,NULL);
	rc = SQLBindParameter(pTrc->handle,2,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_TIMESTAMP,26 ,0,pTrc->tsEnd  ,0,NULL);
	// rc = SQLBindParameter(pTrc->handle,3,SQL_PARAM_INPUT,SQL_C_LONG,SQL_INTEGER  , 9 ,0,pTrc->sqlcode,0,NULL);
	rc = SQLBindParameter(pTrc->handle,4,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR  ,256,0,pTrc->text   ,0,NULL);
	rc = SQLBindParameter(pTrc->handle,5,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR  ,28 ,0,pTrc->job    ,0,NULL);
	rc = SQLBindParameter(pTrc->handle,6,SQL_PARAM_INPUT,SQL_C_BIGINT,SQL_BIGINT ,18 ,0,&pTrc->trid  ,0,NULL);
	// rc = SQLBindParameter(pTrc->handle,7,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR ,8192,0,pTrc->sqlstmt,0,NULL);
}
/* ------------------------------------------------------------- */
void jx_traceSetId (INT64 trid)
{
	PJXSQLCONNECT pc = jx_getCurrentConnection();
	PJXTRACE pTrc = &pc->sqlTrace;
	pTrc->trid = trid;
}
/* ------------------------------------------------------------- */
void jx_traceInsert (PJXSQL pSQL, PUCHAR stmt , PUCHAR sqlState)
{
	int rc;
	PJXTRACE pTrc = &pConnection->sqlTrace; // !!! TODO not from global !!!
	if (pTrc->doTrace == OFF) return;
	rc = SQLBindParameter(pTrc->handle,3,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_CHAR    ,   5,0,sqlState,0,NULL);
	rc = SQLBindParameter(pTrc->handle,7,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR ,8192,0,stmt,0,NULL);

	// insert row
	rc = SQLExecute(pTrc->handle);

}
/* ------------------------------------------------------------- */
SHORT  doInsertOrUpdate(
	PJXSQL pSQL,
	PJXSQL pSQLmeta,
	PUCHAR sqlTempStmt,
	PUCHAR table ,
	PJXNODE pRow,
	PUCHAR where ,
	BOOL   update
)
{
	LONG   attrParm;
	LONG   colno;
	LONG   i;
	PUCHAR valArr[64];
	SHORT  valArrIx= 0;
	SQLINTEGER sql_nts;
	SQLINTEGER bindColNo;

	PJXNODE pNode;
	PUCHAR comma = "";
	PUCHAR name, value;

	SQLSMALLINT   length;
	SQLRETURN     rc;
	PUCHAR        sqlNullPtr = NULL;

	if (pSQL == NULL || pSQL->pstmt == NULL) return -1;

	// Now we have the colume definitions - now build the update statement:
	if (update) {
		buildUpdate (pSQL->pstmt->hstmt, pSQLmeta->pstmt->hstmt, sqlTempStmt , table, pRow , where);
	} else {
		buildInsert (pSQL->pstmt->hstmt, pSQLmeta->pstmt->hstmt, sqlTempStmt , table, pRow , where);
	}

	// prepare the statement that will do the update
	rc = SQLPrepare(pSQL->pstmt->hstmt , sqlTempStmt, SQL_NTS);
	if (rc  != SQL_SUCCESS ) {
		if (-109 == getSqlCode(pSQL->pstmt->hstmt)) {
			return rc; // we  have an error - so try with next...
		}
		check_error (pSQL);
		return rc; // we have an error, and die
	}


	// Take the description from the "select" and use it on the "update"
	pNode    =  jx_GetNodeChild (pRow);
	bindColNo =0;
	for(colno =1; pNode ; colno ++) {
		int realLength;
		JXCOL Col;
		BOOL isId = isIdColumn(pSQLmeta->pstmt->hstmt, colno);

		if (!isId && !nodeisnull(pNode) && !nodeisblank(pNode)) {

			bindColNo ++; // Only columns with data ( not null nor blank) need to be bound

			memset (&Col , 0 , sizeof(JXCOL));
			rc = SQLDescribeCol (
				pSQLmeta->pstmt->hstmt,
				colno,    // The meta "cursor" contaians all columns
				Col.colname,
				sizeof (Col.colname),
				&Col.colnamelen,
				&Col.coltype,
				&Col.collen,
				&Col.scale,
				&Col.nullable
			);

			if (rc != SQL_SUCCESS ) {
				check_error (pSQL);
				return rc; // we have an error
			}


			if (pNode->type == ARRAY ||  pNode->type == OBJECT) {
				value = valArr[valArrIx++] = memAlloc(Col.collen);
				realLength = jx_AsJsonTextMem (pNode , value,  Col.collen );
				value [realLength] = '\0';
			} else {
				value = jx_GetNodeValuePtr  (pNode , NULL);
				realLength = strlen(value);
			}

			// Long data > 32K will be chopped into chunks for update.
			if (realLength > 32000) {
				// Set parameters based on total data to send.
				SQLINTEGER lbytes = realLength;
				SQLINTEGER cbTextSize = SQL_DATA_AT_EXEC;

				// Bind the parameter marker.
				rc  = SQLBindParameter (
					pSQL->pstmt->hstmt, // hstmt
					bindColNo,
					SQL_PARAM_INPUT,  // fParamType
					SQL_C_CHAR,       // fCType
					Col.coltype,      // FSqlType
					lbytes,           // cbColDef
					0,                // ibScale
					pNode,            // rgbValue - store the complete node. Here SQL RPC are very flexible - any pointer
					0,                // cbValueMax
					&cbTextSize       // pcbValue
				);
				if ( rc  != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rc != SQL_NEED_DATA ) {
					check_error (pSQL);
					return rc; // we have an error
				}

			} else {

				// length  !!! 1234,56 gives 6 digits                                              //GIT
				SQLINTEGER colLen = Col.coltype  == SQL_TIMESTAMP ? Col.collen : realLength + Col.scale;  //GIT
				sql_nts = SQL_NTS;

				rc = SQLBindParameter(
					pSQL->pstmt->hstmt,
					bindColNo,
					SQL_PARAM_INPUT,
					SQL_C_CHAR,
					Col.coltype,
					colLen,       // column len - Take care: timestamp need real length of colum. Numbers need string le//GIT
					Col.scale,    // presition
					value,
					0,
					&sql_nts // NULL terminated string -(pointer to length variable)
				);
			}

			if (rc != SQL_SUCCESS ) {
				check_error (pSQL);
				return rc; // we have an error
			}
		}
		pNode = jx_GetNodeNext(pNode);
	}

	// run  the  statement in "sqlstr"
	rc = SQLExecute( pSQL->pstmt->hstmt);

	if (rc == SQL_NEED_DATA) {
		// Check to see if NEED_DATA; if yes, use SQLPutData.
		rc  = SQLParamData(pSQL->pstmt->hstmt, &pNode);
		while (rc == SQL_NEED_DATA) {
			LONG    cbBatch = 32000; // Dont use real 32K it will be to large a buffer
			PUCHAR  value = jx_GetNodeValuePtr (pNode , NULL);
			LONG    lbytes = strlen(value);

			while (lbytes > cbBatch) {
				rc = SQLPutData(pSQL->pstmt->hstmt, value , cbBatch);
				lbytes -= cbBatch;
				value  += cbBatch;
			}

			// Put final batch.
			rc = SQLPutData(pSQL->pstmt->hstmt, value, lbytes);

			// Setup next column
			rc = SQLParamData(pSQL->pstmt->hstmt, &pNode  );
		}
	}

	for(i=0;i<valArrIx; i++) {
		memFree(&valArr[i]);
	}

	if (rc != SQL_SUCCESS && rc != SQL_NO_DATA_FOUND) {
		check_error (pSQL);
		return rc; // we have an error
	}

	return rc;

}

/* ------------------------------------------------------------- */
static PJXSQL buildMetaStmt (PUCHAR table, PJXNODE pRow)
{
	UCHAR     sqlTempStmt[32766];
	PUCHAR    stmt = sqlTempStmt;
	PUCHAR    name;
	UCHAR     temp  [256];
	PJXNODE   pNode;
	PUCHAR    comma = "";
	SQLRETURN rc;
	PJXSQL    pSQLmeta = jx_sqlNewStatement (NULL, false, false);

	stmt += sprintf (stmt , "select ");

	comma = "";
	pNode    =  jx_GetNodeChild (pRow);
	while (pNode) {
		name  = jx_GetNodeNamePtr   (pNode);
		str2upper (temp  , name);   // Needed for national charse in columns names i.e.: BELØB
		stmt += sprintf (stmt , "%s%s" , comma , temp);
		comma = ",";
		pNode = jx_GetNodeNext(pNode);
	}

	stmt += sprintf (stmt , " from %s where 1=0 with ur" , table);

	// prepare the statement that provides the columns
	rc = SQLPrepare(pSQLmeta->pstmt->hstmt , sqlTempStmt, SQL_NTS);
	if (rc != SQL_SUCCESS ) {
		check_error (pSQLmeta);
		jx_sqlClose (&pSQLmeta); // Free the data
		return NULL;
	}
	return  pSQLmeta;
}
/* ------------------------------------------------------------- */
LGL jx_sqlUpdateOrInsert (BOOL update, PUCHAR table  , PJXNODE pRowP , PUCHAR whereP, PJXNODE pSqlParms)
{
	LONG   attrParm;
	LONG   i;
	PUCHAR valArr[64];
	SHORT  valArrIx= 0;

	UCHAR sqlTempStmt[32766];
	UCHAR where [4096];
	UCHAR temp  [256];
	PUCHAR stmt = sqlTempStmt;
	PJXNODE pNode;
	PUCHAR comma = "";
	PUCHAR name, value;

	SQLSMALLINT   length;
	SQLRETURN     rc;
	PJXSQL        pSQL     = jx_sqlNewStatement (NULL, true, false);
	PJXSQL        pSQLmeta;
	SQLCHUNK      sqlChunk[32];
	SHORT         sqlChunkIx =0;
	PUCHAR        sqlNullPtr = NULL;
	PJXNODE       pRow = jx_ParseString((PUCHAR) pRowP, NULL);
	LGL err = ON; // assume error


	// First get the columen types - by now we use a select to mimic that
	pSQLmeta = buildMetaStmt ( table, pRow);
	if (pSQLmeta == NULL) {
		goto cleanup;
	}

	strFormat(where , whereP , pSqlParms);
	rc = doInsertOrUpdate(pSQL, pSQLmeta, sqlTempStmt, table,pRow, where , update);

	// So far we suceeded: But with data ?
	err = (rc == SQL_SUCCESS) ? OFF:ON;

	if (err == OFF) {
		sqlCode =0;
	} else if ( pSQL && pSQL->pstmt) {
		getSqlCode (pSQL->pstmt->hstmt);
	}

	// Now we are done with the select statement:
cleanup:
	if (pRowP != pRow) jx_NodeDelete (pRow);
	jx_sqlClose (&pSQLmeta); // Free the data
	jx_sqlClose (&pSQL);
	return err;
}
/* ------------------------------------------------------------- */
LGL jx_sqlUpdate (PUCHAR table  , PJXNODE pRow , PUCHAR whereP, PJXNODE pSqlParmsP  )
{
	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	PUCHAR  where     = (pParms->OpDescList->NbrOfParms >= 3) ? whereP : "";
	PJXNODE pSqlParms = (pParms->OpDescList->NbrOfParms >= 4) ? pSqlParmsP : NULL;
	UCHAR  whereStr [1024];
	str2upper(table , table);
	for(; *where == ' ' ; where++); // skip leading blanks
	if (*where > ' ' && ! BeginsWith(where, "where")) {
		sprintf (whereStr , "where %s" , where);
		where = whereStr;
	}
	return jx_sqlUpdateOrInsert  (true , table  , pRow , where, pSqlParms);
}
/* ------------------------------------------------------------- */
LGL jx_sqlInsert (PUCHAR table  , PJXNODE pRow , PUCHAR whereP, PJXNODE pSqlParmsP  )
{
	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	PUCHAR  where    =  (pParms->OpDescList->NbrOfParms >= 3) ? whereP : "";
	PJXNODE pSqlParms = (pParms->OpDescList->NbrOfParms >= 4) ? pSqlParmsP : NULL;
	str2upper(table , table);
	return jx_sqlUpdateOrInsert  (false , table  , pRow , where , pSqlParms);
}
/* ------------------------------------------------------------- */
LGL jx_sqlUpsert (PUCHAR table  , PJXNODE pRow , PUCHAR whereP, PJXNODE pSqlParmsP  )
{
	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	PUCHAR  where     = (pParms->OpDescList->NbrOfParms >= 3) ? whereP : "";
	PJXNODE pSqlParms = (pParms->OpDescList->NbrOfParms >= 4) ? pSqlParmsP : NULL;
	LGL err;
	// First update - if not found the insert
	err = jx_sqlUpdate  ( table  , pRow , where, pSqlParms);
	if (err == ON && jx_sqlCode() == 100) {
		err = jx_sqlInsert (table  , pRow , where, pSqlParms);
	}
	return err;
}
/* -------------------------------------------------------------------
 * Provide options to a pSQL environment - If NULL then use the default
 * ------------------------------------------------------------------- */
LONG jx_sqlGetInsertId (void)
{
	LONG    id;
	PJXNODE pRow;
	// PUCHAR  sqlStmt = "values IDENTITY_VAL_LOCAL() as id ";
	// PUCHAR  sqlStmt = "values IDENTITY_VAL_LOCAL() into :id";
	PUCHAR  sqlStmt = "Select IDENTITY_VAL_LOCAL() as id from sysibm/sysdummy1";

	// Get that only row
	pRow = jx_sqlResultRow(sqlStmt, NULL);

	id = atoi(jx_GetValuePtr(pRow, "id", NULL));

	jx_NodeDelete (pRow);

	return id ;
}
/* -------------------------------------------------------------------
 * Provide options to a pSQL environment - If NULL the use the default
 * ------------------------------------------------------------------- */
void jx_sqlSetOptions (PJXNODE pOptionsP)
{

	PJXSQLCONNECT pc = jx_getCurrentConnection();
	PSQLOPTIONS po = &pConnection->options;
	PJXNODE pNode;

	// Delete previous settings, if we did that parsing
	if (pConnection->pOptionsCleanup) {
		jx_Close(&pConnection->pOptions);
	}

	// .. and set the new setting
	pConnection->pOptionsCleanup = false;
	if (ON == jx_isNode(pOptionsP)) {
		pConnection->pOptions = pOptionsP;
	} else if (pOptionsP != NULL) {
		pConnection->pOptions = jx_ParseString ((PUCHAR) pOptionsP , NULL);
		pConnection->pOptionsCleanup = true;
	}

	pNode    =  jx_GetNodeChild (pConnection->pOptions);
	while (pNode) {
		int rc = SQL_SUCCESS;
		PUCHAR name, value;
		LONG attrParm;
		name  = jx_GetNodeNamePtr   (pNode);
		value = jx_GetNodeValuePtr  (pNode , NULL);

		// Is header overriden by userprogram ?
		if (BeginsWith(name , "upperCaseColName")) {
			po->upperCaseColName = *value == 't'? ON:OFF; // for true
		}
		else if (BeginsWith(name , "autoParseContent")) {
			po->autoParseContent = *value == 't' ? ON:OFF; // for true
		}
		else if (BeginsWith(name , "DecimalPoint")) {
			po->DecimalPoint = *value;
		}
		else if (BeginsWith(name , "sqlNaming")) {
			po->sqlNaming = *value == 't' ? ON:OFF; // for true
			attrParm = po->sqlNaming == OFF; // sysname is invers of SQL naming :(
			rc = SQLSetConnectAttr     (pConnection->hdbc , SQL_ATTR_DBC_SYS_NAMING, &attrParm  , 0);
		}
		// NOTE !! hexSort can only be set at environlevel - befor connect time !!!
		// else if (BeginsWith(name , "hexSort")) {
		//   po->hexSort = *value == 't' ? ON:OFF; // for true
		//}
		if (rc  != SQL_SUCCESS ) {
			check_error (NULL);
			return ; // we have an error
		}

		/* more to come....
			po->DateSep;
			po->DateFmt;
			po->TimeSep;
			po->TimeFmt;
			po->DecimalPoint;
		*/
		pNode = jx_GetNodeNext(pNode);
	}
}
/* ------------------------------------------------------------- */
PJXSQLCONNECT jx_sqlConnect(PJXNODE pOptionsP)
{
	PNPMPARMLISTADDRP pParms = _NPMPARMLISTADDR();
	PJXNODE  pOptions = pParms->OpDescList->NbrOfParms >= 1 ? pOptionsP : NULL;
	JXSQL tempSQL;
	PJXSQLCONNECT pc;

	connectionMode = HOSTED;
	// memset(&tempSQL , 0 , sizeof(tempSQL));
	// jx_BuildEnv(&tempSQL);
	pc = jx_getCurrentConnection ();

	return pc;
}

