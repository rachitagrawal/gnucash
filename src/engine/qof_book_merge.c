/*********************************************************************
 * qof_book_merge.c -- api for QoFBook merge with collision handling *
 * Copyright (C) 2004-2005 Neil Williams <linux@codehelp.co.uk>      *
 *                                                                   *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
 *                                                                   *
 * This program is distributed in the hope that it will be useful,   *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the     *
 * GNU General Public License for more details.                      *
 *                                                                   *
 * You should have received a copy of the GNU General Public License *
 * along with this program; if not, contact:                         *
 *                                                                   *
 * Free Software Foundation           Voice:  +1-617-542-5942        *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652        *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                    *
 *                                                                   *
 ********************************************************************/

#include "qof_book_merge.h"
#include "qofid-p.h"
static short module = MOD_IMPORT; 

/* private rule iteration struct */
struct qof_book_mergeRuleIterate {
	qof_book_mergeRuleForeachCB   fcn;
	qof_book_mergeData *data;
	qof_book_mergeRule *rule;
	GList *ruleList;
	guint remainder;
};

/* Make string type parameters 3 times more
	important in the match than default types.
	i.e. even if two other parameters differ, 
	a string match will still provide a better target
	than when other types match and the string does not.
*/
#define DEFAULT_MERGE_WEIGHT	1
#define QOF_STRING_WEIGHT		3
#define QOF_DATE_STRING_LENGTH	31
#define QOF_UTC_DATE_FORMAT		"%Y-%m-%dT%H:%M:%SZ"

/* ================================================================ */
/* API functions. */

qof_book_mergeData*
qof_book_mergeInit( QofBook *importBook, QofBook *targetBook) 
{
	qof_book_mergeData *mergeData;
	qof_book_mergeRule *currentRule;
	GList *check;

	g_return_val_if_fail((importBook != NULL)&&(targetBook != NULL), NULL);
	mergeData = g_new(qof_book_mergeData, 1);
	mergeData->abort = FALSE;
	mergeData->mergeList = NULL;
	mergeData->targetList = NULL;
	mergeData->mergeBook = importBook;
	mergeData->targetBook = targetBook;
	mergeData->mergeObjectParams = NULL;
	mergeData->orphan_list = NULL;
	mergeData->target_table = g_hash_table_new( g_direct_hash, qof_book_merge_rule_cmp);
	currentRule = g_new(qof_book_mergeRule, 1);
	mergeData->currentRule = currentRule;
	qof_object_foreach_type(qof_book_mergeForeachType, mergeData);
	g_return_val_if_fail(mergeData->mergeObjectParams, NULL);
	if(mergeData->orphan_list != NULL) {
		qof_book_merge_match_orphans(mergeData);
	}
	
	check = g_list_copy(mergeData->mergeList);
	while(check != NULL) {
		currentRule = check->data;
		if(currentRule->mergeResult == MERGE_INVALID) {
			mergeData->abort = TRUE;
			return(NULL);
		}
		check = g_list_next(check);
	}
	g_list_free(check);
	return mergeData;
}

void
qof_book_merge_abort (qof_book_mergeData *mergeData)
{
	qof_book_mergeRule *currentRule;
	
	g_return_if_fail(mergeData != NULL);
	while(mergeData->mergeList != NULL) {
		currentRule = mergeData->mergeList->data;
		g_slist_free(currentRule->linkedEntList);
		g_slist_free(currentRule->mergeParam);
		g_free(mergeData->mergeList->data);
	if(currentRule) {
		g_slist_free(currentRule->linkedEntList);
		g_slist_free(currentRule->mergeParam);
		g_free(currentRule);
	}
		mergeData->mergeList = g_list_next(mergeData->mergeList);
	}
	while(mergeData->targetList != NULL) {
		g_free(mergeData->targetList->data);
		mergeData->targetList = g_slist_next(mergeData->targetList);
	}
	g_list_free(mergeData->mergeList);
	g_slist_free(mergeData->mergeObjectParams);
	g_slist_free(mergeData->targetList);
	if(mergeData->orphan_list != NULL) { g_slist_free(mergeData->orphan_list); }
	g_hash_table_destroy(mergeData->target_table);
	g_free(mergeData);
}

/*  Q: This could be a general usage function:
	qof_param_as_string(QofParam*, QofEntity*);
	Useful? Worth transferring to qofclass.c?
	Need to fix the KVP->string. How?

	The QOF_TYPE_DATE output format from
	qof_book_merge_param_as_string has been changed to QSF_XSD_TIME,
	a UTC formatted timestring: 2005-01-01T10:55:23Z
	If you change QOF_UTC_DATE_FORMAT, change 
	backend/file/qsf-xml.c : qsf_entity_foreach to
	reformat to QSF_XSD_TIME or the QSF XML will
	FAIL the schema validation and QSF exports will become invalid.

	The QOF_TYPE_BOOLEAN is lowercase for the same reason.
*/
char*
qof_book_merge_param_as_string(QofParam *qtparam, QofEntity *qtEnt)
{
	gchar 		*param_string, param_date[QOF_DATE_STRING_LENGTH];
	char 		param_sa[GUID_ENCODING_LENGTH + 1];
	KvpFrame 	*param_kvp;
	QofType 	paramType;
	const GUID *param_guid;
	time_t 		param_t;
	gnc_numeric param_numeric, 	(*numeric_getter)	(QofEntity*, QofParam*);
	Timespec 	param_ts, 		(*date_getter)		(QofEntity*, QofParam*);
	double 		param_double, 	(*double_getter)	(QofEntity*, QofParam*);
	gboolean 	param_boolean, 	(*boolean_getter)	(QofEntity*, QofParam*);
	gint32 		param_i32, 		(*int32_getter)		(QofEntity*, QofParam*);
	gint64 		param_i64, 		(*int64_getter)		(QofEntity*, QofParam*);
	
	param_string = NULL;
	paramType = qtparam->param_type;
	if(safe_strcmp(paramType, QOF_TYPE_STRING) == 0)  { 
			param_string = g_strdup(qtparam->param_getfcn(qtEnt,qtparam));
			if(param_string == NULL) { param_string = ""; }
			return param_string;
		}
		if(safe_strcmp(paramType, QOF_TYPE_DATE) == 0) { 
			date_getter = (Timespec (*)(QofEntity*, QofParam*))qtparam->param_getfcn;
			param_ts = date_getter(qtEnt, qtparam);
			param_t = timespecToTime_t(param_ts);
			strftime(param_date, QOF_DATE_STRING_LENGTH, QOF_UTC_DATE_FORMAT, gmtime(&param_t));
			param_string = g_strdup(param_date);
			return param_string;
		}
		if((safe_strcmp(paramType, QOF_TYPE_NUMERIC) == 0)  ||
		(safe_strcmp(paramType, QOF_TYPE_DEBCRED) == 0)) { 
			numeric_getter = (gnc_numeric (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			param_numeric = numeric_getter(qtEnt,qtparam);
			param_string = g_strdup(gnc_numeric_to_string(param_numeric));
			return param_string;
		}
		if(safe_strcmp(paramType, QOF_TYPE_GUID) == 0) { 
			param_guid = qtparam->param_getfcn(qtEnt,qtparam);
			guid_to_string_buff(param_guid, param_sa);
			param_string = g_strdup(param_sa);
			return param_string;
		}
		if(safe_strcmp(paramType, QOF_TYPE_INT32) == 0) { 
			int32_getter = (gint32 (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			param_i32 = int32_getter(qtEnt, qtparam);
			param_string = g_strdup_printf("%u", param_i32);
			return param_string;
		}
		if(safe_strcmp(paramType, QOF_TYPE_INT64) == 0) { 
			int64_getter = (gint64 (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			param_i64 = int64_getter(qtEnt, qtparam);
			param_string = g_strdup_printf("%llu", param_i64);
			return param_string;
		}
		if(safe_strcmp(paramType, QOF_TYPE_DOUBLE) == 0) { 
			double_getter = (double (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			param_double = double_getter(qtEnt, qtparam);
			param_string = g_strdup_printf("%f", param_double);
			return param_string;
		}
		if(safe_strcmp(paramType, QOF_TYPE_BOOLEAN) == 0){ 
			boolean_getter = (gboolean (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			param_boolean = boolean_getter(qtEnt, qtparam);
			/* Boolean values need to be lowercase for QSF validation. */
			if(param_boolean == TRUE) { param_string = g_strdup("true"); }
			else { param_string = g_strdup("false"); }
			return param_string;
		}
		/* "kvp" */
		/* FIXME: how can this be a string??? */
		if(safe_strcmp(paramType, QOF_TYPE_KVP) == 0) { 
			param_kvp = kvp_frame_copy(qtparam->param_getfcn(qtEnt,qtparam));

			return param_string;
		}
		if(safe_strcmp(paramType, QOF_TYPE_CHAR) == 0) { 
			param_string = g_strdup(qtparam->param_getfcn(qtEnt,qtparam));
			return param_string;
		}
	return NULL;
}

qof_book_mergeData*
qof_book_mergeUpdateResult(qof_book_mergeData *mergeData,
						qof_book_mergeResult tag)
{
	qof_book_mergeRule *resolved;
	
	g_return_val_if_fail((mergeData != NULL), NULL);
	g_return_val_if_fail((tag > 0), NULL);
	g_return_val_if_fail((tag != MERGE_REPORT), NULL);
	resolved = mergeData->currentRule;
	g_return_val_if_fail((resolved != NULL), NULL);
	if((resolved->mergeAbsolute == TRUE)&&(tag == MERGE_DUPLICATE)) 	
	{ 
		tag = MERGE_ABSOLUTE; 
	}
	if((resolved->mergeAbsolute == TRUE)&&(tag == MERGE_NEW))
	{
		tag = MERGE_UPDATE; 
	}
	if((resolved->mergeAbsolute == FALSE)&&	(tag == MERGE_ABSOLUTE)) 	
	{ 
		tag = MERGE_DUPLICATE; 
	}
	if((resolved->mergeResult == MERGE_NEW)&&(tag == MERGE_UPDATE)) 
	{ 
		tag = MERGE_NEW; 
	}
	if(resolved->updated == FALSE) { resolved->mergeResult = tag;	}
	resolved->updated = TRUE;
	if(tag >= MERGE_INVALID) { 
		mergeData->abort = TRUE;
		mergeData->currentRule = resolved;
		return NULL; 
	}
	mergeData->currentRule = resolved;
	return mergeData;
}

int
qof_book_mergeCommit( qof_book_mergeData *mergeData )
{
	qof_book_mergeRule *currentRule;
	GList *check;
	
	g_return_val_if_fail(mergeData != NULL, -1);
	g_return_val_if_fail(mergeData->mergeList != NULL, -1);
	g_return_val_if_fail(mergeData->targetBook != NULL, -1);
	if(mergeData->abort == TRUE) return -1;
	check = g_list_copy(mergeData->mergeList);
	g_return_val_if_fail(check != NULL, -1);
	while(check != NULL) {
		currentRule = check->data;
		if(currentRule->mergeResult == MERGE_INVALID) {
			qof_book_merge_abort(mergeData);
			return(-2);
		}
		if(currentRule->mergeResult == MERGE_REPORT) {
			g_list_free(check);
			return 1;
		}
		check = g_list_next(check);
	}
	qof_book_mergeCommitForeach( qof_book_mergeCommitRuleLoop, MERGE_NEW, mergeData);
	qof_book_mergeCommitForeach( qof_book_mergeCommitRuleLoop, MERGE_UPDATE, mergeData);
	/* Placeholder for QofObject merge_helper_cb - all objects and all parameters set */
	while(mergeData->mergeList != NULL) {
		currentRule = mergeData->mergeList->data;
		g_slist_free(currentRule->mergeParam);
		g_slist_free(currentRule->linkedEntList);
		mergeData->mergeList = g_list_next(mergeData->mergeList);
	}
	while(mergeData->targetList != NULL) {
		g_free(mergeData->targetList->data);
		mergeData->targetList = g_slist_next(mergeData->targetList);
	}
	g_list_free(mergeData->mergeList);
	g_slist_free(mergeData->mergeObjectParams);
	g_slist_free(mergeData->targetList);
	if(mergeData->orphan_list != NULL) { g_slist_free(mergeData->orphan_list); }
	g_hash_table_destroy(mergeData->target_table);
	g_free(mergeData);
	return 0;
}

/* End of API functions. Internal code follows. */
/* ==================================================================== */

void qof_book_mergeRuleForeach( qof_book_mergeData *mergeData, 
								qof_book_mergeRuleForeachCB cb, 
								qof_book_mergeResult mergeResult )
{
	struct qof_book_mergeRuleIterate iter;
	qof_book_mergeRule *currentRule;
	GList *matching_rules;

	g_return_if_fail(cb != NULL);
	g_return_if_fail(mergeData != NULL);
	currentRule = mergeData->currentRule;
	g_return_if_fail(mergeResult > 0);
	g_return_if_fail(mergeResult != MERGE_INVALID);
	g_return_if_fail(mergeData->abort == FALSE);
	iter.fcn = cb;
	iter.data = mergeData;
	matching_rules = NULL;
	iter.ruleList = g_list_copy(mergeData->mergeList);
	while(iter.ruleList!=NULL) {
		currentRule = iter.ruleList->data;
		if(currentRule->mergeResult == mergeResult) {
			matching_rules = g_list_prepend(matching_rules, currentRule);
		}
		iter.ruleList = g_list_next(iter.ruleList);
	}
	iter.remainder = g_list_length(matching_rules);
	g_list_foreach (matching_rules, qof_book_mergeRuleCB, &iter);
	g_list_free(matching_rules);
}

qof_book_mergeRule*
qof_book_mergeUpdateRule(qof_book_mergeRule *currentRule, gboolean match, gint weight)
{
	gboolean absolute;

	absolute = currentRule->mergeAbsolute;
	if(absolute && match && currentRule->mergeResult == MERGE_UNDEF) {
			currentRule->mergeResult = MERGE_ABSOLUTE;
	}
	if(absolute && !match) { currentRule->mergeResult = MERGE_UPDATE; }
	if(!absolute && match &&currentRule->mergeResult == MERGE_UNDEF) {
			currentRule->mergeResult = MERGE_DUPLICATE;
	}
	if(!absolute && !match) {
		currentRule->difference += weight;
		if(currentRule->mergeResult == MERGE_DUPLICATE) {
			currentRule->mergeResult = MERGE_REPORT;
		}
	}
	return currentRule;
}

int 
qof_book_mergeCompare( qof_book_mergeData *mergeData ) 
{
	qof_book_mergeRule *currentRule;
	gchar 			*stringImport, *stringTarget, *charImport, *charTarget;
	QofEntity	 	*mergeEnt, *targetEnt, *referenceEnt;
	const GUID 		*guidImport, *guidTarget;
	QofParam 		*qtparam;
	KvpFrame 		*kvpImport, *kvpTarget;
	QofIdType 		mergeParamName;
	QofType 		mergeType;
	GSList 			*paramList;
	gboolean	 	absolute, mergeError, knowntype, mergeMatch, booleanImport, booleanTarget,
													(*boolean_getter)	(QofEntity*, QofParam*);
	Timespec 		tsImport, tsTarget, 			(*date_getter)		(QofEntity*, QofParam*);
	gnc_numeric 	numericImport, numericTarget, 	(*numeric_getter)	(QofEntity*, QofParam*);
	double 			doubleImport, doubleTarget, 	(*double_getter)	(QofEntity*, QofParam*);
	gint32 			i32Import, i32Target, 			(*int32_getter)		(QofEntity*, QofParam*);
	gint64 			i64Import, i64Target, 			(*int64_getter)		(QofEntity*, QofParam*);

	g_return_val_if_fail((mergeData != NULL), -1);
	currentRule = mergeData->currentRule;
	g_return_val_if_fail((currentRule != NULL), -1);
	absolute = currentRule->mergeAbsolute;
	mergeEnt = currentRule->importEnt;
	targetEnt = currentRule->targetEnt;
	paramList = currentRule->mergeParam;
	currentRule->difference = 0;
	currentRule->mergeResult = MERGE_UNDEF;
	currentRule->linkedEntList = NULL;
	g_return_val_if_fail((targetEnt)||(mergeEnt)||(paramList), -1);
	
	kvpImport = kvp_frame_new();
	kvpTarget = kvp_frame_new();
	mergeError = FALSE;
	while(paramList != NULL) {
		mergeMatch = FALSE;
		knowntype = FALSE;
		qtparam = paramList->data;
		mergeParamName = qtparam->param_name;

		g_return_val_if_fail(mergeParamName != NULL, -1);
		mergeType = qtparam->param_type;
		if(safe_strcmp(mergeType, QOF_TYPE_STRING) == 0)  { 
			stringImport = qtparam->param_getfcn(mergeEnt,qtparam);
			stringTarget = qtparam->param_getfcn(targetEnt,qtparam);
			/* very strict string matches may need to be relaxed. */
			if(stringImport == NULL) { stringImport = ""; }
			if(stringTarget == NULL) { stringTarget = ""; }
			if(safe_strcmp(stringImport,stringTarget) == 0) { mergeMatch = TRUE; }
			/* Give special weight to a string match */
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, QOF_STRING_WEIGHT);
			stringImport = stringTarget = NULL;
			knowntype= TRUE;
		}
		if(safe_strcmp(mergeType, QOF_TYPE_DATE) == 0) {
			date_getter = (Timespec (*)(QofEntity*, QofParam*))qtparam->param_getfcn;
			tsImport = date_getter(mergeEnt, qtparam);
			tsTarget = date_getter(targetEnt, qtparam);
			if(timespec_cmp(&tsImport, &tsTarget) == 0) { mergeMatch = TRUE; }
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT);
			knowntype= TRUE;
		}
		if((safe_strcmp(mergeType, QOF_TYPE_NUMERIC) == 0)  ||
		(safe_strcmp(mergeType, QOF_TYPE_DEBCRED) == 0)) { 
			numeric_getter = (gnc_numeric (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			numericImport = numeric_getter(mergeEnt,qtparam);
			numericTarget = numeric_getter(targetEnt,qtparam);
			if(gnc_numeric_compare (numericImport, numericTarget) == 0) { mergeMatch = TRUE; }
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT);
			knowntype= TRUE;
		}
		if(safe_strcmp(mergeType, QOF_TYPE_GUID) == 0) { 
			guidImport = qtparam->param_getfcn(mergeEnt,qtparam);
			guidTarget = qtparam->param_getfcn(targetEnt,qtparam);
			if(guid_compare(guidImport, guidTarget) == 0) { mergeMatch = TRUE; }
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT);
			knowntype= TRUE;
		}
		if(safe_strcmp(mergeType, QOF_TYPE_INT32) == 0) { 
			int32_getter = (gint32 (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			i32Import = int32_getter(mergeEnt, qtparam);
			i32Target = int32_getter(targetEnt, qtparam);
			if(i32Target == i32Import) { mergeMatch = TRUE; }
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT);
			knowntype= TRUE;
		}
		if(safe_strcmp(mergeType, QOF_TYPE_INT64) == 0) { 
			int64_getter = (gint64 (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			i64Import = int64_getter(mergeEnt, qtparam);
			i64Target = int64_getter(targetEnt, qtparam);
			if(i64Target == i64Import) { mergeMatch = TRUE; }
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT); 
			knowntype= TRUE;
		}
		if(safe_strcmp(mergeType, QOF_TYPE_DOUBLE) == 0) { 
			double_getter = (double (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			doubleImport = double_getter(mergeEnt, qtparam);
			doubleTarget = double_getter(mergeEnt, qtparam);
			if(doubleImport == doubleTarget) { mergeMatch = TRUE; }
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT); 
			knowntype= TRUE;
		}
		if(safe_strcmp(mergeType, QOF_TYPE_BOOLEAN) == 0){ 
			boolean_getter = (gboolean (*)(QofEntity*, QofParam*)) qtparam->param_getfcn;
			booleanImport = boolean_getter(mergeEnt, qtparam);
			booleanTarget = boolean_getter(targetEnt, qtparam);
			if(booleanImport != FALSE && booleanImport != TRUE) { booleanImport = FALSE; }
			if(booleanTarget != FALSE && booleanTarget != TRUE) { booleanTarget = FALSE; }
			if(booleanImport == booleanTarget) { mergeMatch = TRUE; }
			currentRule	= qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT);
			knowntype= TRUE;
		}
		if(safe_strcmp(mergeType, QOF_TYPE_KVP) == 0) { 
			kvpImport = kvp_frame_copy(qtparam->param_getfcn(mergeEnt,qtparam));
			kvpTarget = kvp_frame_copy(qtparam->param_getfcn(targetEnt,qtparam));
			if(kvp_frame_compare(kvpImport, kvpTarget) == 0) { mergeMatch = TRUE; }
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT); 
			knowntype= TRUE;
		}
		if(safe_strcmp(mergeType, QOF_TYPE_CHAR) == 0) { 
			charImport = qtparam->param_getfcn(mergeEnt,qtparam);
			charTarget = qtparam->param_getfcn(targetEnt,qtparam);
			if(charImport == charTarget) { mergeMatch = TRUE; }
			currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT); 
			knowntype= TRUE;
		}
		/* No object should have QofSetterFunc defined for the book, but just to be safe, do nothing. */
		if(safe_strcmp(mergeType, QOF_ID_BOOK) == 0) { knowntype= TRUE;	}
		/* deal with custom type parameters : 
		 using references to other registered QOF objects. */
		if(knowntype == FALSE) {
			referenceEnt = qtparam->param_getfcn(mergeEnt, qtparam);
			if((referenceEnt != NULL)
				&&(safe_strcmp(referenceEnt->e_type, mergeType) == 0)) {
				currentRule->linkedEntList = g_slist_prepend(currentRule->linkedEntList, referenceEnt);
					/* Compare the mergeEnt reference with targetEnt reference */
					if(referenceEnt == qtparam->param_getfcn(targetEnt, qtparam)) { mergeMatch = TRUE; }
					currentRule = qof_book_mergeUpdateRule(currentRule, mergeMatch, DEFAULT_MERGE_WEIGHT);
			}
		}
	paramList = g_slist_next(paramList);
	}
	mergeData->currentRule = currentRule;
	g_free(kvpImport);
	g_free(kvpTarget);
	return 0;
}

void
qof_book_mergeCommitForeach (
			qof_book_mergeRuleForeachCB cb, 
			qof_book_mergeResult mergeResult,
			qof_book_mergeData *mergeData)
{
	struct qof_book_mergeRuleIterate iter;
	qof_book_mergeRule *currentRule;
	GList *subList;

	g_return_if_fail(cb != NULL);
	g_return_if_fail(mergeData != NULL);
	currentRule = mergeData->currentRule;
	g_return_if_fail(currentRule != NULL);
	g_return_if_fail(mergeResult > 0);
	g_return_if_fail((mergeResult != MERGE_INVALID)||(mergeResult != MERGE_UNDEF)||(mergeResult != MERGE_REPORT));

	iter.fcn = cb;
	subList = NULL;
	iter.ruleList = g_list_copy(mergeData->mergeList);
	while(iter.ruleList!=NULL) {
		currentRule = iter.ruleList->data;
		if(currentRule->mergeResult == mergeResult) {
			subList = g_list_prepend(subList, currentRule);
		}
		iter.ruleList = g_list_next(iter.ruleList);
	}
	iter.remainder = g_list_length(subList);
	iter.data = mergeData;
	g_list_foreach (subList, qof_book_mergeCommitForeachCB, &iter);
}

void qof_book_mergeCommitForeachCB(gpointer rule, gpointer arg)
{
	struct qof_book_mergeRuleIterate *iter;
	
	g_return_if_fail(arg != NULL);
	iter = (struct qof_book_mergeRuleIterate*)arg;
	g_return_if_fail(iter->data != NULL);
	iter->fcn (iter->data, (qof_book_mergeRule*)rule, iter->remainder);
	iter->remainder--;
}

gboolean
qof_book_merge_rule_cmp(gconstpointer a, gconstpointer b)
{
	qof_book_mergeRule *ra = (qof_book_mergeRule *) a;
	qof_book_mergeRule *rb = (qof_book_mergeRule *) b;
	
	if (ra->difference == rb->difference) { return TRUE; }
	else return FALSE;
}

static void
qof_book_merge_orphan_check(double difference, qof_book_mergeRule *mergeRule, qof_book_mergeData *mergeData)
{
	/* Called when difference is lower than previous
		Lookup target to find previous match
		and re-assign mergeEnt to orphan_list */
	qof_book_mergeRule *rule;
	
	g_return_if_fail(mergeRule != NULL);
	g_return_if_fail(mergeData != NULL);
	if(g_hash_table_size(mergeData->target_table) == 0) { return; }
	rule = (qof_book_mergeRule*)g_hash_table_lookup(mergeData->target_table, mergeRule->targetEnt);
	/* If NULL, no match was found. */
	if(rule == NULL) { return; }
	/* Only orphan if this is a better match than already exists. */
	if(difference >= rule->difference) { return; }
	rule->targetEnt = NULL;
	rule->mergeResult = MERGE_UNDEF;
	mergeData->orphan_list = g_slist_append(mergeData->orphan_list, rule);
}

void
qof_book_merge_match_orphans(qof_book_mergeData *mergeData)
{
	GSList *orphans, *targets;
	qof_book_mergeRule *rule, *currentRule;
	QofEntity *best_matchEnt;
	double difference;

	g_return_if_fail(mergeData != NULL);
	currentRule = mergeData->currentRule;
	g_return_if_fail(currentRule != NULL);
	/* This routine does NOT copy the orphan list, it
		is used recursively until empty. */
	orphans = mergeData->orphan_list;
	targets = g_slist_copy(mergeData->targetList);
	while(orphans != NULL) {
		rule = orphans->data;
		g_return_if_fail(rule != NULL);
		difference = g_slist_length(mergeData->mergeObjectParams);
		if(rule->targetEnt == NULL) {
			rule->mergeResult = MERGE_NEW;
			rule->difference = 0;
			mergeData->mergeList = g_list_prepend(mergeData->mergeList,rule);
			orphans = g_slist_next(orphans);
			continue;
		}
		mergeData->currentRule = rule;
		g_return_if_fail(qof_book_mergeCompare(mergeData) != -1);
		if(difference > mergeData->currentRule->difference) {
			best_matchEnt = currentRule->targetEnt;
			difference = currentRule->difference;
			rule = currentRule;
			mergeData->mergeList = g_list_prepend(mergeData->mergeList,rule);
			qof_book_merge_orphan_check(difference, rule, mergeData);
		}
		orphans = g_slist_next(orphans);
	}
	g_slist_free(mergeData->orphan_list);
	g_slist_free(targets);
}

void 
qof_book_mergeForeach ( QofEntity* mergeEnt, gpointer user_data) 
{
	qof_book_mergeRule *mergeRule, *currentRule;
	qof_book_mergeData *mergeData;
	QofEntity *targetEnt, *best_matchEnt;
	GUID *g;
	double difference;
	GSList *c;
	
	g_return_if_fail(user_data != NULL);
	mergeData = (qof_book_mergeData*)user_data;
	g_return_if_fail(mergeEnt != NULL);
	currentRule = mergeData->currentRule;
	g_return_if_fail(currentRule != NULL);
	g = guid_malloc();
	*g = mergeEnt->guid;
	mergeRule = g_new(qof_book_mergeRule,1);
	mergeRule->importEnt = 		mergeEnt;
	mergeRule->difference = 	difference = 0;
	mergeRule->mergeAbsolute = 	FALSE;
	mergeRule->mergeResult = 	MERGE_UNDEF;
	mergeRule->updated = 		FALSE;
	mergeRule->mergeType = 		mergeEnt->e_type;
	mergeRule->mergeLabel = 	qof_object_get_type_label(mergeEnt->e_type);
	mergeRule->mergeParam = 	g_slist_copy(mergeData->mergeObjectParams);
	mergeRule->linkedEntList =	NULL;
	mergeData->currentRule = mergeRule;
	targetEnt = best_matchEnt = NULL;
	targetEnt = qof_collection_lookup_entity (
		qof_book_get_collection (mergeData->targetBook, mergeEnt->e_type), g);
	if( targetEnt != NULL) { 
		mergeRule->mergeAbsolute = TRUE;
		mergeRule->targetEnt = targetEnt;
		g_return_if_fail(qof_book_mergeCompare(mergeData) != -1);
		mergeRule->linkedEntList = g_slist_copy(currentRule->linkedEntList);
		mergeData->mergeList = g_list_prepend(mergeData->mergeList,mergeRule);
		return;
	}
	/* no absolute match exists */
	g_slist_free(mergeData->targetList);
	mergeData->targetList = NULL;
	qof_object_foreach_type(qof_book_mergeForeachTypeTarget, mergeData);
	if(g_slist_length(mergeData->targetList) == 0) {
		mergeRule->mergeResult = MERGE_NEW;
	}
	difference = g_slist_length(mergeRule->mergeParam);
	c = g_slist_copy(mergeData->targetList);
	while(c != NULL) {
		mergeRule->targetEnt = c->data;
		currentRule = mergeRule;
		/* compare two entities and sum the differences */
		g_return_if_fail(qof_book_mergeCompare(mergeData) != -1);
		if(mergeRule->difference == 0) {
			/* check if this is a better match than one already assigned */
			best_matchEnt = mergeRule->targetEnt;
			mergeRule->mergeResult = MERGE_DUPLICATE;
			difference = 0;
			mergeRule->linkedEntList = g_slist_copy(currentRule->linkedEntList);
			g_slist_free(c);
			guid_free(g);
			/* exact match, return */
			return;
		}
		if(difference > mergeRule->difference) {
			/* The chosen targetEnt determines the parenting of any child object */
			/* check if this is a better match than one already assigned */
			best_matchEnt = mergeRule->targetEnt;
			difference = mergeRule->difference;
			/* Use match to lookup the previous entity that matched this targetEnt (if any)
				and remove targetEnt from the rule for that mergeEnt.
				Add the previous mergeEnt to orphan_list.
			*/			
			qof_book_merge_orphan_check(difference, mergeRule, mergeData);
		}
		c = g_slist_next(c);
	}
	g_slist_free(c);
	if(best_matchEnt != NULL ) {
		mergeRule->targetEnt = best_matchEnt;
		mergeRule->difference = difference;
		/* Set this entity in the target_table in case a better match can be made
			with the next mergeEnt. */
		g_hash_table_insert(mergeData->target_table, mergeRule->targetEnt, mergeRule);
		/* compare again with the best partial match */
		g_return_if_fail(qof_book_mergeCompare(mergeData) != -1);
		mergeRule->linkedEntList = g_slist_copy(currentRule->linkedEntList);
	}
	else {
		mergeRule->targetEnt = NULL;
		mergeRule->difference = 0;
		mergeRule->mergeResult = MERGE_NEW;
		mergeRule->linkedEntList = g_slist_copy(currentRule->linkedEntList);
	}
	mergeData->mergeList = g_list_prepend(mergeData->mergeList,mergeRule);
	guid_free(g);
	/* return to qof_book_mergeInit */
}

void qof_book_mergeForeachTarget (QofEntity* targetEnt, gpointer user_data)
{
	qof_book_mergeData *mergeData;
	
	g_return_if_fail(user_data != NULL);
	mergeData = (qof_book_mergeData*)user_data;
	g_return_if_fail(targetEnt != NULL);
		mergeData->targetList = g_slist_prepend(mergeData->targetList,targetEnt);
}

void 
qof_book_mergeForeachTypeTarget ( QofObject* merge_obj, gpointer user_data) 
{
	qof_book_mergeData *mergeData;
	qof_book_mergeRule *currentRule;
	
	g_return_if_fail(user_data != NULL);
	mergeData = (qof_book_mergeData*)user_data;
	currentRule = mergeData->currentRule;
	g_return_if_fail(currentRule != NULL);
	g_return_if_fail(merge_obj != NULL);
	if(safe_strcmp(merge_obj->e_type, currentRule->importEnt->e_type) == 0) {
		qof_object_foreach(currentRule->importEnt->e_type, mergeData->targetBook, 
			qof_book_mergeForeachTarget, user_data);
	}
}

void 
qof_book_mergeForeachType ( QofObject* merge_obj, gpointer user_data) 
{
	qof_book_mergeData *mergeData;
	
	g_return_if_fail(user_data != NULL);
	mergeData = (qof_book_mergeData*)user_data;
	g_return_if_fail((merge_obj != NULL));
	/* Skip unsupported objects */
	if((merge_obj->create == NULL)||(merge_obj->foreach == NULL)){
		DEBUG (" merge_obj QOF support failed %s", merge_obj->e_type);
		return;
	}

	if(mergeData->mergeObjectParams != NULL) g_slist_free(mergeData->mergeObjectParams);
	mergeData->mergeObjectParams = NULL;
	qof_class_param_foreach(merge_obj->e_type, qof_book_mergeForeachParam , mergeData);
	qof_object_foreach(merge_obj->e_type, mergeData->mergeBook, qof_book_mergeForeach, mergeData);
}

void 
qof_book_mergeForeachParam( QofParam* param, gpointer user_data) 
{
	qof_book_mergeData *mergeData;
	
	g_return_if_fail(user_data != NULL);
	mergeData = (qof_book_mergeData*)user_data;
	g_return_if_fail(param != NULL);
	if((param->param_getfcn != NULL)&&(param->param_setfcn != NULL)) {
		mergeData->mergeObjectParams = g_slist_append(mergeData->mergeObjectParams, param);
	}
}

void
qof_book_mergeRuleCB(gpointer rule, gpointer arg)
{
	struct qof_book_mergeRuleIterate *iter;
	qof_book_mergeData *mergeData;

	g_return_if_fail(arg != NULL);
	iter = (struct qof_book_mergeRuleIterate*)arg;
	mergeData = iter->data;
	g_return_if_fail(mergeData != NULL);
	g_return_if_fail(mergeData->abort == FALSE);
	iter->fcn (mergeData, (qof_book_mergeRule*)rule, iter->remainder);
	iter->data = mergeData;
	iter->remainder--;
}

static QofEntity*
qof_book_mergeLocateReference( QofEntity *ent, qof_book_mergeData *mergeData)
{
	GList *all_rules;
	qof_book_mergeRule *rule;
	QofEntity *referenceEnt;

	/* locates the rule referring to this import entity */
	if(!ent) { return NULL; }
	g_return_val_if_fail((mergeData != NULL), NULL);
	all_rules = NULL;
	referenceEnt = NULL;
	all_rules = g_list_copy(mergeData->mergeList);
	while(all_rules != NULL) {
		rule = all_rules->data;
		if(rule->importEnt == ent) { referenceEnt = rule->targetEnt; }
		all_rules = g_list_next(all_rules);
	}
	return referenceEnt;
}

void qof_book_mergeCommitRuleLoop(
						qof_book_mergeData *mergeData,
						qof_book_mergeRule *rule, 
						guint remainder) 
{ 
	QofInstance 	*inst;
	gboolean		registered_type;
	QofEntity 		*referenceEnt;
	GSList 			*linkage;
	/* cm_ prefix used for variables that hold the data to commit */
	QofParam 		*cm_param;
	gchar 			*cm_string, *cm_char;
	const GUID 		*cm_guid;
	KvpFrame 		*cm_kvp;
	/* function pointers and variables for parameter getters that don't use pointers normally */
	gnc_numeric 	cm_numeric, (*numeric_getter)	(QofEntity*, QofParam*);
	double 			cm_double, 	(*double_getter)	(QofEntity*, QofParam*);
	gboolean 		cm_boolean, (*boolean_getter)	(QofEntity*, QofParam*);
	gint32 			cm_i32, 	(*int32_getter)		(QofEntity*, QofParam*);
	gint64 			cm_i64, 	(*int64_getter)		(QofEntity*, QofParam*);
	Timespec 		cm_date, 	(*date_getter)		(QofEntity*, QofParam*);
	/* function pointers to the parameter setters */
	void	(*string_setter)	(QofEntity*, const char*);
	void	(*date_setter)		(QofEntity*, Timespec);
	void	(*numeric_setter)	(QofEntity*, gnc_numeric);
	void	(*guid_setter)		(QofEntity*, const GUID*);
	void	(*double_setter)	(QofEntity*, double);
	void	(*boolean_setter)	(QofEntity*, gboolean);
	void	(*i32_setter)		(QofEntity*, gint32);
	void	(*i64_setter)		(QofEntity*, gint64);
	void	(*char_setter)		(QofEntity*, char*);
	void	(*kvp_frame_setter)	(QofEntity*, KvpFrame*);
	void	(*reference_setter)	(QofEntity*, QofEntity*);

	g_return_if_fail(rule != NULL);
	g_return_if_fail(mergeData != NULL);
	g_return_if_fail(mergeData->targetBook != NULL);
	g_return_if_fail((rule->mergeResult != MERGE_NEW)||(rule->mergeResult != MERGE_UPDATE));
	/* create a new object for MERGE_NEW */
	/* The new object takes the GUID from the import to retain an absolute match */
	if(rule->mergeResult == MERGE_NEW) {
		inst = (QofInstance*)qof_object_new_instance(rule->importEnt->e_type, mergeData->targetBook);
		g_return_if_fail(inst != NULL);
		rule->targetEnt = &inst->entity;
		qof_entity_set_guid(rule->targetEnt, qof_entity_get_guid(rule->importEnt));
	}
	/* currentRule->targetEnt is now set,
		1. by an absolute GUID match or
		2. by best_matchEnt and difference or
		3. by MERGE_NEW.
	*/
	while(rule->mergeParam != NULL) {
		registered_type = FALSE;
		g_return_if_fail(rule->mergeParam->data);		
		cm_param = rule->mergeParam->data;
		rule->mergeType = cm_param->param_type;
		if(safe_strcmp(rule->mergeType, QOF_TYPE_STRING) == 0)  { 
			cm_string = cm_param->param_getfcn(rule->importEnt, cm_param);
			string_setter = (void(*)(QofEntity*, const char*))cm_param->param_setfcn;
			if(string_setter != NULL) {	string_setter(rule->targetEnt, cm_string); }
			registered_type = TRUE;
		}
		if(safe_strcmp(rule->mergeType, QOF_TYPE_DATE) == 0) { 
			date_getter = (Timespec (*)(QofEntity*, QofParam*))cm_param->param_getfcn;
			cm_date = date_getter(rule->importEnt, cm_param);
			date_setter = (void(*)(QofEntity*, Timespec))cm_param->param_setfcn;
			if(date_setter != NULL) { date_setter(rule->targetEnt, cm_date); }
			registered_type = TRUE;
		}
		if((safe_strcmp(rule->mergeType, QOF_TYPE_NUMERIC) == 0)  ||
		(safe_strcmp(rule->mergeType, QOF_TYPE_DEBCRED) == 0)) { 
			numeric_getter = (gnc_numeric (*)(QofEntity*, QofParam*))cm_param->param_getfcn;
			cm_numeric = numeric_getter(rule->importEnt, cm_param);
			numeric_setter = (void(*)(QofEntity*, gnc_numeric))cm_param->param_setfcn;
			if(numeric_setter != NULL) { numeric_setter(rule->targetEnt, cm_numeric); }
			registered_type = TRUE;
		}
		if(safe_strcmp(rule->mergeType, QOF_TYPE_GUID) == 0) { 
			cm_guid = cm_param->param_getfcn(rule->importEnt, cm_param);
			guid_setter = (void(*)(QofEntity*, const GUID*))cm_param->param_setfcn;
			if(guid_setter != NULL) { guid_setter(rule->targetEnt, cm_guid); }
			registered_type = TRUE;
		}
		if(safe_strcmp(rule->mergeType, QOF_TYPE_INT32) == 0) { 
			int32_getter = (gint32 (*)(QofEntity*, QofParam*)) cm_param->param_getfcn;
			cm_i32 = int32_getter(rule->importEnt, cm_param);
			i32_setter = (void(*)(QofEntity*, gint32))cm_param->param_setfcn;
			if(i32_setter != NULL) { i32_setter(rule->targetEnt, cm_i32); }
			registered_type = TRUE;
		}
		if(safe_strcmp(rule->mergeType, QOF_TYPE_INT64) == 0) { 
			int64_getter = (gint64 (*)(QofEntity*, QofParam*)) cm_param->param_getfcn;
			cm_i64 = int64_getter(rule->importEnt, cm_param);
			i64_setter = (void(*)(QofEntity*, gint64))cm_param->param_setfcn;
			if(i64_setter != NULL) { i64_setter(rule->targetEnt, cm_i64); }
			registered_type = TRUE;
		}
		if(safe_strcmp(rule->mergeType, QOF_TYPE_DOUBLE) == 0) { 
			double_getter = (double (*)(QofEntity*, QofParam*)) cm_param->param_getfcn;
			cm_double = double_getter(rule->importEnt, cm_param);
			double_setter = (void(*)(QofEntity*, double))cm_param->param_setfcn;
			if(double_setter != NULL) { double_setter(rule->targetEnt, cm_double); }
			registered_type = TRUE;
		}
		if(safe_strcmp(rule->mergeType, QOF_TYPE_BOOLEAN) == 0){ 
			boolean_getter = (gboolean (*)(QofEntity*, QofParam*)) cm_param->param_getfcn;
			cm_boolean = boolean_getter(rule->importEnt, cm_param);
			boolean_setter = (void(*)(QofEntity*, gboolean))cm_param->param_setfcn;
			if(boolean_setter != NULL) { boolean_setter(rule->targetEnt, cm_boolean); }
			registered_type = TRUE;
		}
		if(safe_strcmp(rule->mergeType, QOF_TYPE_KVP) == 0) { 
			cm_kvp = kvp_frame_copy(cm_param->param_getfcn(rule->importEnt,cm_param));
			kvp_frame_setter = (void(*)(QofEntity*, KvpFrame*))cm_param->param_setfcn;
			if(kvp_frame_setter != NULL) { kvp_frame_setter(rule->targetEnt, cm_kvp); }
			registered_type = TRUE;
		}
		if(safe_strcmp(rule->mergeType, QOF_TYPE_CHAR) == 0) { 
			cm_char = cm_param->param_getfcn(rule->importEnt,cm_param);
			char_setter = (void(*)(QofEntity*, char*))cm_param->param_setfcn;
			if(char_setter != NULL) { char_setter(rule->targetEnt, cm_char); }
			registered_type = TRUE;
		}
		if(registered_type == FALSE) {
			linkage = g_slist_copy(rule->linkedEntList);
			referenceEnt = NULL;
//			currentRule = NULL;
			reference_setter = (void(*)(QofEntity*, QofEntity*))cm_param->param_setfcn;
			if((linkage == NULL)&&(rule->mergeResult == MERGE_NEW)) {
				referenceEnt = cm_param->param_getfcn(rule->importEnt, cm_param);
				reference_setter(rule->targetEnt, qof_book_mergeLocateReference(referenceEnt, mergeData));
			}
			while(linkage != NULL) {
				referenceEnt = linkage->data;
				if((referenceEnt)
					&&(referenceEnt->e_type)
					&&(safe_strcmp(referenceEnt->e_type, rule->mergeType) == 0)) {
					/* The function behind reference_setter must create objects for any non-QOF references */
					reference_setter(rule->targetEnt, qof_book_mergeLocateReference(referenceEnt, mergeData));
				}
				linkage = g_slist_next(linkage);
			}
		}
		rule->mergeParam = g_slist_next(rule->mergeParam);
	}
}
