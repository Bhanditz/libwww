/*								     HTFormat.c
**	MANAGE DIFFERENT FILE FORMATS
**
**	(c) COPYRIGHT MIT 1995.
**	Please first read the full copyright statement in the file COPYRIGH.
**
** Bugs:
**	Assumes the incoming stream is ASCII, rather than a local file
**	format, and so ALWAYS converts from ASCII on non-ASCII machines.
**	Therefore, non-ASCII machines can't read local files.
**
** HISTORY:
**	8 Jul 94  FM	Insulate free() from _free structure element.
**	8 Nov 94  HFN	Changed a lot to make reentrant
*/

/* Library Include files */
#include "tcp.h"
#include "HTUtils.h"
#include "HTString.h"
#include "HTTCP.h"
#include "HTFWrite.h"
#include "HTGuess.h"
#include "HTNetMan.h"
#include "HTError.h"
#include "HTReqMan.h"
#include "HTFormat.h"					 /* Implemented here */

#define NO_VALUE_FOUND	-1e30		 /* Stream Stack Value if none found */

PRIVATE HTList * HTConversions = NULL;
PRIVATE HTList * HTCharsets = NULL;
PRIVATE HTList * HTEncodings = NULL;
PRIVATE HTList * HTLanguages = NULL;

PRIVATE double HTMaxSecs = 1e10;		/* No effective limit */

struct _HTStream {
    CONST HTStreamClass *	isa;
};

/* ------------------------------------------------------------------------- */
/* 	  ACCEPT LISTS OF CONVERSIONS, ENCODINGS, LANGUAGE, AND CHARSET	     */
/* ------------------------------------------------------------------------- */

/*
**	For all `accept lists' there is a local list and a global list. The
**	local list is a part of the request structure and the global list is
**	internal to the HTFormat module. The global lists can be used when
**	specifying accept lists for ALL requests and the local list can be 
**	used to add specific accept headers to the request.
*/


/*	Define a presentation system command for a content-type
**	-------------------------------------------------------
** INPUT:
**	conversions:	The list of conveters and presenters
**	representation:	the MIME-style format name
**	command:	the MAILCAP-style command template
**	quality:	A degradation faction [0..1]
**	maxbytes:	A limit on the length acceptable as input (0 infinite)
**	maxsecs:	A limit on the time user will wait (0 for infinity)
*/
PUBLIC void HTPresentation_add (HTList *	conversions,
				CONST char *	representation,
				CONST char *	command,
				CONST char *	test_command,
				double		quality,
				double		secs, 
				double		secs_per_byte)
{
    HTPresentation * pres = (HTPresentation *)calloc(1,sizeof(HTPresentation));
    if (pres == NULL) outofmem(__FILE__, "HTSetPresentation");
    
    pres->rep = HTAtom_for(representation);
    pres->rep_out = WWW_PRESENT;		/* Fixed for now ... :-) */
    pres->converter = HTSaveAndExecute;		/* Fixed for now ...     */
    pres->quality = quality;
    pres->secs = secs;
    pres->secs_per_byte = secs_per_byte;
    pres->rep = HTAtom_for(representation);
    pres->command = NULL;
    StrAllocCopy(pres->command, command);
    pres->test_command = NULL;
    StrAllocCopy(pres->test_command, test_command);
    HTList_addObject(conversions, pres);
}

PUBLIC void HTPresentation_deleteAll (HTList * list)
{
    if (list) {
	HTList *cur = list;
	HTPresentation *pres;
	while ((pres = (HTPresentation*) HTList_nextObject(cur))) {
	    FREE(pres->command);
	    free(pres);
	}
	HTList_delete(list);
    }
}

/*	Define a built-in function for a content-type
**	---------------------------------------------
*/
PUBLIC void HTConversion_add (HTList *		conversions,
			      CONST char *	representation_in,
			      CONST char *	representation_out,
			      HTConverter *	converter,
			      double		quality,
			      double		secs, 
			      double		secs_per_byte)
{
    HTPresentation * pres = (HTPresentation *)calloc(1,sizeof(HTPresentation));
    if (pres == NULL) outofmem(__FILE__, "HTSetPresentation");
    
    pres->rep = HTAtom_for(representation_in);
    pres->rep_out = HTAtom_for(representation_out);
    pres->converter = converter;
    pres->command = NULL;		/* Fixed */
    pres->test_command = NULL;
    pres->quality = quality;
    pres->secs = secs;
    pres->secs_per_byte = secs_per_byte;
    HTList_addObject(conversions, pres);
}

PUBLIC void HTConversion_deleteAll (HTList * list)
{
    HTPresentation_deleteAll(list);
}

PUBLIC void HTEncoding_add (HTList * 		list,
			    CONST char *	enc,
			    double		quality)
{
    HTAcceptNode * node;
    if (!list || !enc || !*enc) {
	if (WWWTRACE)
	    fprintf(TDEST, "Encodings... Bad argument\n");
	return;
    }
    node = (HTAcceptNode*)calloc(1, sizeof(HTAcceptNode));
    if (!node) outofmem(__FILE__, "HTAcceptEncoding");
    HTList_addObject(list, (void*)node);

    node->atom = HTAtom_for(enc);
    node->quality = quality;
}

PUBLIC void HTEncoding_deleteAll (HTList * list)
{
    if (list) {
	HTList *cur = list;
	HTAcceptNode *pres;
	while ((pres = (HTAcceptNode *) HTList_nextObject(cur))) {
	    free(pres);
	}
	HTList_delete(list);
    }
}

PUBLIC void HTLanguage_add (HTList *		list,
			    CONST char *	lang,
			    double		quality)
{
    HTAcceptNode * node;
    if (!list || !lang || !*lang)  {
	if (WWWTRACE)
	    fprintf(TDEST, "Languages... Bad argument\n");
	return;
    }
    node = (HTAcceptNode*)calloc(1, sizeof(HTAcceptNode));
    if (!node) outofmem(__FILE__, "HTAcceptLanguage");

    HTList_addObject(list, (void*)node);
    node->atom = HTAtom_for(lang);
    node->quality = quality;
}

PUBLIC void HTLanguage_deleteAll (HTList * list)
{
    HTEncoding_deleteAll(list);
}

PUBLIC void HTCharset_add (HTList *		list,
			   CONST char *		charset,
			   double		quality)
{
    HTAcceptNode * node;
    if (!list || !charset || !*charset)  {
	if (WWWTRACE)
	    fprintf(TDEST, "Charset..... Bad argument\n");
	return;
    }
    node = (HTAcceptNode*)calloc(1, sizeof(HTAcceptNode));
    if (!node) outofmem(__FILE__, "HTAcceptCharsetuage");

    HTList_addObject(list, (void*)node);
    node->atom = HTAtom_for(charset);
    node->quality = quality;
}

PUBLIC void HTCharset_deleteAll (HTList * list)
{
    HTEncoding_deleteAll(list);
}

/* ------------------------------------------------------------------------- */
/* 			GLOBAL LIST OF CONVERTERS ETC.			     */
/* ------------------------------------------------------------------------- */

/*
**	Global Accept Format Types Conversions
**	list can be NULL
*/
PUBLIC void HTFormat_setConversion (HTList * list)
{
    HTConversions = list;
}

PUBLIC HTList * HTFormat_conversion (void)
{
    return HTConversions;
}

/*
**	Global Accept Encodings
**	list can be NULL
*/
PUBLIC void HTFormat_setEncoding (HTList *list)
{
    HTEncodings = list;
}

PUBLIC HTList * HTFormat_encoding (void)
{
    return HTEncodings;
}

/*
**	Global Accept Languages
**	list can be NULL
*/
PUBLIC void HTFormat_setLanguage (HTList *list)
{
    HTLanguages = list;
}

PUBLIC HTList * HTFormat_language (void)
{
    return HTLanguages;
}

/*
**	Global Accept Charsets
**	list can be NULL
*/
PUBLIC void HTFormat_setCharset (HTList *list)
{
    HTCharsets = list;
}

PUBLIC HTList * HTFormat_charset (void)
{
    return HTCharsets;
}

/*
**	Convenience function to clean up
*/
PUBLIC void HTFormat_deleteAll (void)
{
    if (HTConversions) {
	HTConversion_deleteAll(HTConversions);
	HTConversions = NULL;
    }
    if (HTLanguages) {
	HTLanguage_deleteAll(HTLanguages);
	HTLanguages = NULL;
    }
    if (HTEncodings) {
	HTEncoding_deleteAll(HTEncodings);
	HTEncodings = NULL;
    }
    if (HTCharsets) {
	HTCharset_deleteAll(HTCharsets);
	HTCharsets = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* 				FORMAT NEGOTIATION			     */
/* ------------------------------------------------------------------------- */

PRIVATE BOOL better_match ARGS2(HTFormat, f,
				HTFormat, g)
{
    CONST char *p, *q;

    if (f && g  &&  (p = HTAtom_name(f))  &&  (q = HTAtom_name(g))) {
	int i,j;
	for(i=0 ; *p; p++) if (*p == '*') i++;
	for(j=0 ; *q; q++) if (*q == '*') j++;
	if (i < j) return YES;
    }
    return NO;
}


PRIVATE BOOL wild_match ARGS2(HTAtom *,	tmplate,
			      HTAtom *,	actual)
{
    char *t, *a, *st, *sa;
    BOOL match = NO;

    if (tmplate && actual && (t = HTAtom_name(tmplate))) {
	if (!strcmp(t, "*"))
	    return YES;

	if (strchr(t, '*') &&
	    (a = HTAtom_name(actual)) &&
	    (st = strchr(t, '/')) && (sa = strchr(a,'/'))) {

	    *sa = 0;
	    *st = 0;

	    if ((*(st-1)=='*' &&
		 (*(st+1)=='*' || !strcasecomp(st+1, sa+1))) ||
		(*(st+1)=='*' && !strcasecomp(t,a)))
		match = YES;

	    *sa = '/';
	    *st = '/';
	}    
    }
    return match;
}

/*
 * Added by takada@seraph.ntt.jp (94/04/08)
 */
PRIVATE BOOL lang_match ARGS2(HTAtom *,	tmplate,
			      HTAtom *,	actual)
{
    char *t, *a, *st, *sa;
    BOOL match = NO;

    if (tmplate && actual &&
	(t = HTAtom_name(tmplate)) && (a = HTAtom_name(actual))) {
	st = strchr(t, '_');
	sa = strchr(a, '_');
	if ((st != NULL) && (sa != NULL)) {
	    if (!strcasecomp(t, a))
	      match = YES;
	    else
	      match = NO;
	}
	else {
	    if (st != NULL) *st = 0;
	    if (sa != NULL) *sa = 0;
	    if (!strcasecomp(t, a))
	      match = YES;
	    else
	      match = NO;
	    if (st != NULL) *st = '_';
	    if (sa != NULL) *sa = '_';
	}
    }
    return match;
}
/* end of addition */



PRIVATE double type_value ARGS2(HTAtom *,	content_type,
			       HTList *,	accepted)
{
    HTList * cur = accepted;
    HTPresentation * pres;
    HTPresentation * wild = NULL;

    if (!content_type || !accepted) return -1;

    while ((pres = (HTPresentation*)HTList_nextObject(cur))) {
	if (pres->rep == content_type)
	    return pres->quality;
	else if (wild_match(pres->rep, content_type))
	    wild = pres;
    }
    if (wild) return wild->quality;
    else return -1;
}


PRIVATE double lang_value ARGS2(HTAtom *,	language,
			       HTList *,	accepted)
{
    HTList * cur = accepted;
    HTAcceptNode * node;
    HTAcceptNode * wild = NULL;

    if (!language || !accepted || HTList_isEmpty(accepted)) {
	return 0.1;
    }

    while ((node = (HTAcceptNode*)HTList_nextObject(cur))) {
	if (node->atom == language) {
	    return node->quality;
	}
	/*
	 * patch by takada@seraph.ntt.jp (94/04/08)
	 * the original line was
	 * else if (wild_match(node->atom, language)) {
	 * and the new line is
	 */
	else if (lang_match(node->atom, language)) {
	    wild = node;
	}
    }

    if (wild) {
	return wild->quality;
    }
    else {
	return 0.1;
    }
}


PRIVATE double encoding_value ARGS2(HTAtom *,	encoding,
				   HTList *,	accepted)
{
    HTList * cur = accepted;
    HTAcceptNode * node;
    HTAcceptNode * wild = NULL;
    char * e;

    if (!encoding || !accepted || HTList_isEmpty(accepted))
	return 1;

    e = HTAtom_name(encoding);
    if (!strcmp(e, "7bit") || !strcmp(e, "8bit") || !strcmp(e, "binary"))
	return 1;

    while ((node = (HTAcceptNode*)HTList_nextObject(cur))) {
	if (node->atom == encoding)
	    return node->quality;
	else if (wild_match(node->atom, encoding))
	    wild = node;
    }
    if (wild) return wild->quality;
    else return 1;
}


PUBLIC BOOL HTRank ARGS4(HTList *, possibilities,
			 HTList *, accepted_content_types,
			 HTList *, accepted_languages,
			 HTList *, accepted_encodings)
{
    int accepted_cnt = 0;
    HTList * accepted;
    HTList * sorted;
    HTList * cur;
    HTContentDescription * d;

    if (!possibilities) return NO;

    accepted = HTList_new();
    cur = possibilities;
    while ((d = (HTContentDescription*)HTList_nextObject(cur))) {
	double tv = type_value(d->content_type, accepted_content_types);
	double lv = lang_value(d->content_language, accepted_languages);
	double ev = encoding_value(d->content_encoding, accepted_encodings);

	if (tv > 0) {
	    d->quality *= tv * lv * ev;
	    HTList_addObject(accepted, d);
	    accepted_cnt++;
	}
	else {
	    if (d->filename) free(d->filename);
	    free(d);
	}
    }

    if (PROT_TRACE) fprintf(TDEST, "Ranking.....\n");
    if (PROT_TRACE) fprintf(TDEST,
	   "\nRANK QUALITY CONTENT-TYPE         LANGUAGE ENCODING    FILE\n");

    sorted = HTList_new();
    while (accepted_cnt-- > 0) {
	HTContentDescription * worst = NULL;
	cur = accepted;
	while ((d = (HTContentDescription*)HTList_nextObject(cur))) {
	    if (!worst || d->quality < worst->quality)
		worst = d;
	}
	if (worst) {
	    if (PROT_TRACE)
		fprintf(TDEST, "%d.   %.4f  %-20.20s %-8.8s %-10.10s %s\n",
			accepted_cnt+1,
			worst->quality,
			(worst->content_type
		         ? HTAtom_name(worst->content_type)      : "-"),
			(worst->content_language
		         ? HTAtom_name(worst->content_language)  :"-"),
			(worst->content_encoding
		         ? HTAtom_name(worst->content_encoding)  :"-"),
			(worst->filename
		         ? worst->filename                       :"-"));
	    HTList_removeObject(accepted, (void*)worst);
	    HTList_addObject(sorted, (void*)worst);
	}
    }
    if (PROT_TRACE) fprintf(TDEST, "\n");
    HTList_delete(accepted);
    HTList_delete(possibilities->next);
    possibilities->next = sorted->next;
    sorted->next = NULL;
    HTList_delete(sorted);

    if (!HTList_isEmpty(possibilities)) return YES;
    else return NO;
}


/*		Create a filter stack
**		---------------------
**
**	If a wildcard match is made, a temporary HTPresentation
**	structure is made to hold the destination format while the
**	new stack is generated. This is just to pass the out format to
**	MIME so far.  Storing the format of a stream in the stream might
**	be a lot neater.
**
**	The star/star format is special, in that if you can take
**	that you can take anything.
*/
PUBLIC HTStream * HTStreamStack ARGS5(HTFormat,		rep_in,
				      HTFormat,		rep_out,
				      HTStream *,	output_stream,
				      HTRequest *,	request,
				      BOOL,		guess)
{
    HTList * conversion[2];
    int which_list;
    double best_quality = -1e30;		/* Pretty bad! */
    HTPresentation *pres, *best_match=NULL;
    
    if (guess && rep_in == WWW_UNKNOWN) {
	if (STREAM_TRACE)fprintf(TDEST,"StreamStack. Using guessing stream\n");
	return HTGuess_new(request, NULL, rep_in, rep_out, output_stream);
    }
    if (rep_out == WWW_SOURCE || rep_out == rep_in) {
	if (STREAM_TRACE)
	    fprintf(TDEST,"StreamStack. Identical in/out format: %s\n",
		    HTAtom_name(rep_in));
	return output_stream ? output_stream : HTBlackHole();
    }
    if (STREAM_TRACE) {
	fprintf(TDEST, "StreamStack. Constructing stream stack for %s to %s\n",
		HTAtom_name(rep_in), HTAtom_name(rep_out));
    }

    conversion[0] = request->conversions;
    conversion[1] = HTConversions;

    for(which_list = 0; which_list<2; which_list++) {
	HTList * cur = conversion[which_list];
	
	while ((pres = (HTPresentation*)HTList_nextObject(cur))) {
	    if ((pres->rep==rep_in || wild_match(pres->rep, rep_in)) &&
		(pres->rep_out==rep_out || wild_match(pres->rep_out,rep_out))){
		if (!best_match || better_match(pres->rep, best_match->rep) ||
		    (!better_match(best_match->rep, pres->rep) &&
		     pres->quality > best_quality)) {
#ifdef GOT_SYSTEM
		    int result=0;
		    if (pres->test_command) {
			result = system(pres->test_command);
			if (STREAM_TRACE) 
			    fprintf(TDEST, "StreamStack. system(%s) returns %d\n", pres->test_command, result);
		    }
		    if (!result) {
			best_match = pres;
			best_quality = pres->quality;
		    }
#else
		    best_match = pres;
		    best_quality = pres->quality;
#endif /* GOT_SYSTEM */
		}
	    }
	}
    }
    if (best_match)
	return (*best_match->converter)(request, best_match->command,
					rep_in, rep_out, output_stream);
    if (STREAM_TRACE)
	fprintf(TDEST, "StreamStack. No match found, dumping to local file\n");
    return HTSaveLocally(request, NULL, rep_in, rep_out, output_stream);
}
	

/*		Find the cost of a filter stack
**		-------------------------------
**
**	Must return the cost of the same stack which StreamStack would set up.
**
** On entry,
**	length	The size of the data to be converted
*/
PUBLIC double HTStackValue ARGS5(
	HTList *,		theseConversions,
	HTFormat,		rep_in,
	HTFormat,		rep_out,
	double,			initial_value,
	long int,		length)
{
    int which_list;
    HTList* conversion[2];
    
    if (STREAM_TRACE) {
	fprintf(TDEST, "StackValue.. Evaluating stream stack for %s worth %.3f to %s\n",
		HTAtom_name(rep_in),	initial_value,
		HTAtom_name(rep_out));
    }
    if (rep_out == WWW_SOURCE ||
    	rep_out == rep_in) return 0.0;

    conversion[0] = theseConversions;
    conversion[1] = HTConversions;
    
    for(which_list = 0; which_list<2; which_list++)
     if (conversion[which_list]) {
        HTList * cur = conversion[which_list];
	HTPresentation * pres;
	while ((pres = (HTPresentation*)HTList_nextObject(cur))) {
	    if (pres->rep == rep_in &&
		(pres->rep_out == rep_out || wild_match(pres->rep_out, rep_out))) {
	        double value = initial_value * pres->quality;
		if (HTMaxSecs != 0.0)
		    value = value - (length*pres->secs_per_byte + pres->secs)
			                 /HTMaxSecs;
		return value;
	    }
	}
    }
    return NO_VALUE_FOUND;		/* Really bad */
}


