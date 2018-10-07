/***********************************************************************
 *
 * This file implements the function for check files for equality
 *
 * Copyright (c) 2010, Peter Spjuth
 *
 ***********************************************************************/

#include <tcl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "diffutil.h"

/* Visual C++ does not define S_ISDIR */
#ifndef S_ISDIR
#define S_ISDIR(mode_) (((mode_) & _S_IFMT) == _S_IFDIR)
#endif

const int BlockRead_C = 65536;

typedef struct {
    int ignoreKey;
    int noCase;
    int binary;
} CmpOptions_T;

/* Helper to get a filled in CmpOptions_T */
#define InitCmpOptions_T(opts) {opts.ignoreKey = 0; opts.noCase = 0; opts.binary = 0;}

/* This is called when a dollar is encountered during ignore-keyword.
   If return is equal res1/2 says how far we covered and considered equal.
   This do not bother with encoding since the chars we are interested in
   are the same in ascii/binary/utf8. */
static int
ScanKey(
    const char *s1,
    const char *s2,
    const char *e1,
    const char *e2,
    const char **res1,
    const char **res2)
{
    *res1 = s1;
    *res2 = s2;
    /* Scan word chars until : or $ ends the keyword.
       They must be equal up to that point. */
    while (s1 < e1 && s2 < e2) {
	if ((*s1 == ':' || *s1 == '$') && (*s2 == ':' || *s2 == '$')) {
	    /* The keyword part has ended on both sides */

	    /* To be a bit conservative and not confuse keywords with e.g.
	       Tcl namespace variables we only acknowledge these forms:
	       keyword$
	       keyword:$
	       keyword: .*$
	       keyword:: .*$
	    */
	    if (*s1 == ':') {
		s1++;
		if (s1 + 1 >= e1) {
		    return 1;
		}
		/* May be a double colon */
		if (*s1 == ':') {
		    s1++;
		}
		/* Colon must be followed by space or $ */
		if (*s1 != ' ' && *s1 != '$') {
		    return 1;
		}
	    }
	    if (*s2 == ':') {
		s2++;
		if (s2 + 1 >= e2) {
		    return 1;
		}
		if (*s2 == ':') {
		    s2++;
		}
		if (*s2 != ' ' && *s2 != '$') {
		    return 1;
		}
	    }
	    break;
	}
	if (*s1 != *s2) {
	    /* They are not equal keywords */
	    return 0;
	}
	/* Only standard ascii word chars count */
	if ((*s1 >= 'a' && *s1 <= 'z') || (*s1 >= 'A' && *s1 <= 'Z')) {
	    s1++;
	    s2++;
	} else {
	    /* This did not count as a keyword but is sofar equal. */
	    *res1 = s1;
	    *res2 = s2;
	    return 1;
	}
    }
    /* Skip all until $ */
    while (s1 < e1) {
	if (*s1 == '$') {
	    break;
	}
	s1++;
    }
    while (s2 < e2) {
	if (*s2 == '$') {
	    break;
	}
	s2++;
    }
    /* At this point s1/2 should point to the dollar ending the keyword. */
    if (s1 == e1 || s2 == e2) {
	/* We reached the end of string without finishing the keyword.
	   If a potential keyword is at the end we don't care. */
	return 1;
    }
    /* Strings are equal up to this point. Skip the last dollar as well. */
    *res1 = s1 + 1;
    *res2 = s2 + 1;
    return 1;
}

/* Compare two strings, ignoring keywords.
   If return is true, they are equal up to the point of res1/2. */
static int
CompareNoKey(
    const char *s1,		/* UTF string to compare to s2. */
    const char *s2,		/* UTF string s2 is compared to. */
    unsigned long len1,		/* Number of bytes to compare. */
    unsigned long len2,
    CmpOptions_T *cmpOptions,
    const char **res1,
    const char **res2)
{
    Tcl_UniChar ch1 = 0, ch2 = 0;
    const char *end1 = s1 + len1;
    const char *end2 = s2 + len2;
    const char *scan1, *scan2;

    while (s1 < end1 && s2 < end2) {
	if (cmpOptions->binary) {
	    ch1 = *(s1++);
	    ch2 = *(s2++);
	} else {
	    s1 += Tcl_UtfToUniChar(s1, &ch1);
	    s2 += Tcl_UtfToUniChar(s2, &ch2);
	}
	if (ch1 != ch2) {
	    if (cmpOptions->noCase) {
		ch1 = Tcl_UniCharToLower(ch1);
		ch2 = Tcl_UniCharToLower(ch2);
		if (ch1 != ch2) {
		    return 0;
		}
	    } else {
		return 0;
	    }
	}
	if (ch1 == '$') {
	    if (!ScanKey(s1, s2, end1, end2, &scan1, &scan2)) {
		/* If ScanKey said not equal, we trust it unless we use
		   less strict rules */
		if (!cmpOptions->noCase) {
		    return 0;
		}
	    }
	    s1 = scan1;
	    s2 = scan2;
	}
    }
    *res1 = s1;
    *res2 = s2;
    return 1;
}

static int
CompareStreams(
    Tcl_Channel ch1,
    Tcl_Channel ch2,
    CmpOptions_T *cmpOptions)
{
    int equal;
    Tcl_Obj *line1Ptr, *line2Ptr;
    int charactersRead1, charactersRead2;
    int length1, length2;
    int firstblock;
    const char *string1, *string2;
    
    /* Initialize an object to use as line buffer. */
    line1Ptr = Tcl_NewObj();
    line2Ptr = Tcl_NewObj();
    Tcl_IncrRefCount(line1Ptr);
    Tcl_IncrRefCount(line2Ptr);
    if (cmpOptions->binary) {
	Tcl_SetByteArrayLength(line1Ptr, 70000);
	Tcl_SetByteArrayLength(line2Ptr, 70000);
    } else {
	Tcl_SetObjLength(line1Ptr, 70000);
	Tcl_SetObjLength(line2Ptr, 70000);
    }

    equal = 1;
    firstblock = 1;
    while (equal) {
	charactersRead1 = Tcl_ReadChars(ch1, line1Ptr, BlockRead_C, 0);
	charactersRead2 = Tcl_ReadChars(ch2, line2Ptr, BlockRead_C, 0);
	if (charactersRead1 <= 0 && charactersRead2 <= 0) {
	    /* Nothing more to check */
	    break;
	}
	if (charactersRead1 <= 0 || charactersRead2 <= 0) {
	    /* One of the files is ended */
	    equal = 0;
	    break;
	}
	if (charactersRead1 != charactersRead2  && !cmpOptions->ignoreKey) {
	    /* Different size */
	    equal = 0;
	    break;
	}
	if (cmpOptions->binary) {
	    string1 = (char *) Tcl_GetByteArrayFromObj(line1Ptr, &length1);
	    string2 = (char *) Tcl_GetByteArrayFromObj(line2Ptr, &length2);
	} else {
	    string1 = Tcl_GetStringFromObj(line1Ptr, &length1);
	    string2 = Tcl_GetStringFromObj(line2Ptr, &length2);
	}
	/* Limit ignoreKey to first block to simplify.
	   No need to handle keywords crossing block boundary and a bit
	   better performance on large files. */
	if (firstblock && cmpOptions->ignoreKey) {
	    const char *res1, *res2;
	    unsigned long rem1, rem2;
	    int eq = CompareNoKey(string1, string2, length1, length2,
				  cmpOptions, &res1, &res2);
	    if (!eq) {
		equal = 0;
		break;
	    }
	    firstblock = 0;
	    /* Did we consume everything? */
	    if (res1 >= string1 + length1 && res2 >= string2 + length2) {
		continue;
	    }
	    /* Compensate for any change in length. */
	    rem1 = length1 - (res1 - string1);
	    rem2 = length2 - (res2 - string2);
	    /* Only one side should have anything left. */
	    if (rem1 > 0 && rem2 > 0) { Tcl_Panic("PANIC"); }
	    if (rem1 > 0) {
		unsigned long chars1;
		/* Adjust side 1 */
		string1 = res1;
		length1 = rem1;
		chars1 = Tcl_NumUtfChars(string1, length1);
		/* Read extra characters from side 2 */
		charactersRead2 = Tcl_ReadChars(ch2, line2Ptr, chars1, 0);
		if (charactersRead2 <= 0) {
		    equal = 0;
		    break;
		}
		if (cmpOptions->binary) {
		    string2 = (char *) Tcl_GetByteArrayFromObj(line2Ptr, &length2);
		} else {
		    string2 = Tcl_GetStringFromObj(line2Ptr, &length2);
		}
	    } else { /* rem2 > 0 */
		unsigned long chars2;
		/* Adjust side 2 */
		string2 = res2;
		length2 = rem2;
		chars2 = Tcl_NumUtfChars(string2, length2);
		/* Read extra characters from side 1 */
		charactersRead1 = Tcl_ReadChars(ch1, line1Ptr, chars2, 0);
		if (charactersRead1 <= 0) {
		    equal = 0;
		    break;
		}
		if (cmpOptions->binary) {
		    string1 = (char *) Tcl_GetByteArrayFromObj(line1Ptr, &length1);
		} else {
		    string1 = Tcl_GetStringFromObj(line1Ptr, &length1);
		}
	    }
	}
	if (length1 != length2) {
	    equal = 0;
	    break;
	}
	if (cmpOptions->binary) {
	    if (strncmp(string1, string2, length1) != 0) {
		equal = 0;
		break;
	    }
	} else if (cmpOptions->noCase) {
	    if (Tcl_UtfNcasecmp(string1, string2, length1) != 0) {
		equal = 0;
		break;
	    }
	} else {
	    if (Tcl_UtfNcmp(string1, string2, length1) != 0) {
		equal = 0;
		break;
	    }
	}
    }

    Tcl_DecrRefCount(line1Ptr);
    Tcl_DecrRefCount(line2Ptr);
    return equal;
}

	
int
CompareFilesObjCmd(
    ClientData dummy,    	/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *CONST objv[])	/* Argument objects. */
{
    int index, t, result = TCL_OK;
    Tcl_Obj *file1Ptr, *file2Ptr;
    int equal = 0;
    CmpOptions_T cmpOptions;
    Tcl_Obj *encodingPtr = NULL;
    Tcl_Obj *translationPtr = NULL;
    Tcl_StatBuf *statBuf;
    Tcl_Channel ch1 = NULL, ch2 = NULL;
    Tcl_WideUInt size1, size2;
    unsigned mode1, mode2;

    static CONST char *options[] = {
	"-nocase", "-ignorekey", "-encoding",
        "-translation", (char *) NULL
    };
    enum options {
	OPT_NOCASE, OPT_IGNOREKEY, OPT_ENCODING,
        OPT_TRANSLATION
    };

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
	return TCL_ERROR;
    }
    InitCmpOptions_T(cmpOptions);

    for (t = 1; t < objc - 2; t++) {
	if (Tcl_GetIndexFromObj(interp, objv[t], options, "option", 0,
		&index) != TCL_OK) {
            result = TCL_ERROR;
            goto cleanup;
	}
	switch (index) {
	  case OPT_NOCASE:
	      cmpOptions.noCase = 1;
	      break;
	  case OPT_IGNOREKEY:
	      cmpOptions.ignoreKey = 1;
	      break;
	  case OPT_ENCODING:
	      t++;
	      if (t >= objc - 2) {
		  Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
		  result = TCL_ERROR;
		  goto cleanup;
	      }
	      encodingPtr = objv[t];
	      Tcl_IncrRefCount(objv[t]);
	      break;
	  case OPT_TRANSLATION:
	      t++;
	      if (t >= objc - 2) {
		  Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
		  result = TCL_ERROR;
		  goto cleanup;
	      }
	      translationPtr = objv[t];
	      Tcl_IncrRefCount(objv[t]);
	      break;
	}
    }
    file1Ptr = objv[objc-2];
    file2Ptr = objv[objc-1];

    if (translationPtr != NULL) {
	char *valueName = Tcl_GetString(translationPtr);
	if (strcmp(valueName, "binary") == 0) {
	    cmpOptions.binary = 1;
	}
    }

    statBuf = Tcl_AllocStatBuf();

    /* Stat files first to quickly see if they don't exist */
    if (Tcl_FSStat(file1Ptr, statBuf) != 0) {
        /* FIXA: error message */
        Tcl_SetResult(interp, "bad file", TCL_STATIC);
        ckfree((char *) statBuf);
	result = TCL_ERROR;
	goto cleanup;
    }
    size1 = Tcl_GetSizeFromStat(statBuf);
    mode1 = Tcl_GetModeFromStat(statBuf);
    if (Tcl_FSStat(file2Ptr, statBuf) != 0) {
        /* FIXA: error message */
        Tcl_SetResult(interp, "bad file", TCL_STATIC);
        ckfree((char *) statBuf);
	result = TCL_ERROR;
	goto cleanup;
    }
    size2 = Tcl_GetSizeFromStat(statBuf);
    mode2 = Tcl_GetModeFromStat(statBuf);
    ckfree((char *) statBuf);

    if (S_ISDIR(mode1) || S_ISDIR(mode2)) {
	equal = 0;
	goto done;
    }
    /* On binary comparison, different size means different */
    if (cmpOptions.binary && !cmpOptions.ignoreKey && size1 != size2) {
	equal = 0;
	goto done;
    }
    
    ch1 = Tcl_FSOpenFileChannel(interp, file1Ptr, "r", 0);
    if (ch1 == NULL) {
        result = TCL_ERROR;
        goto cleanup;
    }
    ch2 = Tcl_FSOpenFileChannel(interp, file2Ptr, "r", 0);
    if (ch2 == NULL) {
        result = TCL_ERROR;
        goto cleanup;
    }
    if (encodingPtr != NULL) {
	char *valueName = Tcl_GetString(encodingPtr);
	if (Tcl_SetChannelOption(interp, ch1, "-encoding", valueName)
		!= TCL_OK) {
	    result = TCL_ERROR;
	    goto cleanup;
	}
	if (Tcl_SetChannelOption(interp, ch2, "-encoding", valueName)
		!= TCL_OK) {
	    result = TCL_ERROR;
	    goto cleanup;
	}
    }
    if (translationPtr != NULL) {
	char *valueName = Tcl_GetString(translationPtr);
	if (Tcl_SetChannelOption(interp, ch1, "-translation", valueName)
		!= TCL_OK) {
	    result = TCL_ERROR;
	    goto cleanup;
	}
	if (Tcl_SetChannelOption(interp, ch2, "-translation", valueName)
		!= TCL_OK) {
	    result = TCL_ERROR;
	    goto cleanup;
	}
    }
    equal = CompareStreams(ch1, ch2, &cmpOptions);

    done:
    Tcl_SetObjResult(interp, Tcl_NewIntObj(equal));

    cleanup:

    if (ch1 != NULL) {
	Tcl_Close(interp, ch1);
    }
    if (ch2 != NULL) {
	Tcl_Close(interp, ch2);
    }
    if (encodingPtr != NULL) {
	Tcl_DecrRefCount(encodingPtr);
    }
    if (translationPtr != NULL) {
	Tcl_DecrRefCount(translationPtr);
    }

    return result;
}

int
CompareStreamsObjCmd(
    ClientData dummy,    	/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *CONST objv[])	/* Argument objects. */
{
    int index, t, result = TCL_OK;
    int equal;
    CmpOptions_T cmpOptions;
    Tcl_Channel ch1, ch2;

    static CONST char *options[] = {
	"-nocase", "-ignorekey", "-binary",
        (char *) NULL
    };
    enum options {
	OPT_NOCASE, OPT_IGNOREKEY, OPT_BINARY
    };

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?opts? ch1 ch2");
	return TCL_ERROR;
    }
    InitCmpOptions_T(cmpOptions);

    for (t = 1; t < objc - 2; t++) {
	if (Tcl_GetIndexFromObj(interp, objv[t], options, "option", 0,
		&index) != TCL_OK) {
            result = TCL_ERROR;
            goto cleanup;
	}
	switch (index) {
	  case OPT_NOCASE:
	      cmpOptions.noCase = 1;
	      break;
	  case OPT_IGNOREKEY:
	      cmpOptions.ignoreKey = 1;
	      break;
	  case OPT_BINARY:
	      cmpOptions.binary = 1;
	      break;
	}
    }

    ch1 = Tcl_GetChannel(interp, Tcl_GetString(objv[objc-2]), NULL);
    if (ch1 == NULL) {
	result = TCL_ERROR;
	goto cleanup;
    }
    ch2 = Tcl_GetChannel(interp, Tcl_GetString(objv[objc-1]), NULL);
    if (ch2 == NULL) {
	result = TCL_ERROR;
	goto cleanup;
    }

    equal = CompareStreams(ch1, ch2, &cmpOptions);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(equal));

    cleanup:
    return result;
}
