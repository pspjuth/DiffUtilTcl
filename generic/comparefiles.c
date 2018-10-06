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

typedef struct {
    int ignoreKey;
    int noCase;
    int binary;
} CmpOptions_T;

/* Helper to get a filled in CmpOptions_T */
#define InitCmpOptions_T(opts) {opts.ignoreKey = 0; opts.noCase = 0; opts.binary = 0;}

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
    char *string1, *string2;
    
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
    while (1) {
	charactersRead1 = Tcl_ReadChars(ch1, line1Ptr, 65536, 0);
	charactersRead2 = Tcl_ReadChars(ch2, line2Ptr, 65536, 0);
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
	if (cmpOptions->ignoreKey) {

	} else {
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
