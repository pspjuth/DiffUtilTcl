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

int
CompareFilesObjCmd(
    ClientData dummy,    	/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *CONST objv[])	/* Argument objects. */
{
    int index, t, result = TCL_OK;
    Tcl_Obj *file1Ptr, *file2Ptr;
    int ignoreKey = 0, noCase = 0, binary = 0, equal = 0;
    Tcl_Obj *encodingPtr = NULL;
    Tcl_Obj *translationPtr = NULL;
    Tcl_StatBuf *buf1, *buf2;
    Tcl_Channel ch1 = NULL, ch2 = NULL;
    Tcl_Obj *line1Ptr, *line2Ptr;
    Tcl_WideUInt size1, size2;
    unsigned mode1, mode2;
    int charactersRead1, charactersRead2;
    int length1, length2;
    char *string1, *string2;

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

    for (t = 1; t < objc - 2; t++) {
	if (Tcl_GetIndexFromObj(interp, objv[t], options, "option", 0,
		&index) != TCL_OK) {
            result = TCL_ERROR;
            goto cleanup;
	}
	switch (index) {
	  case OPT_NOCASE:
	      noCase = 1;
	      break;
	  case OPT_IGNOREKEY:
	      ignoreKey = 1;
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
	    binary = 1;
	}
    }

    /* Stat files first to quickly see if they don't exist */
    buf1 = Tcl_AllocStatBuf();
    buf2 = Tcl_AllocStatBuf();
    if (Tcl_FSStat(file1Ptr, buf1) != 0) {
        /* FIXA: error message */
        Tcl_SetResult(interp, "bad file", TCL_STATIC);
	result = TCL_ERROR;
	goto cleanup;
    }
    if (Tcl_FSStat(file2Ptr, buf2) != 0) {
        /* FIXA: error message */
        Tcl_SetResult(interp, "bad file", TCL_STATIC);
	result = TCL_ERROR;
	goto cleanup;
    }

#if 1    
    size1 = (Tcl_WideUInt) buf1->st_size;
    size2 = (Tcl_WideUInt) buf2->st_size;
    mode1 = (unsigned) buf1->st_mode;
    mode2 = (unsigned) buf2->st_mode;
#else
    /* These were added in 8.6, use them and demand 8.6 one day. */
    size1 = Tcl_GetSizeFromStat(buf1);
    size2 = Tcl_GetSizeFromStat(buf2);
    mode1 = Tcl_GetModeFromStat(buf1);
    mode2 = Tcl_GetModeFromStat(buf2);
#endif
    ckfree(buf1);
    ckfree(buf2);

    if (S_ISDIR(mode1) || S_ISDIR(mode2)) {
	equal = 0;
	goto done;
    }
    /* On binary comparison, different size means different */
    if (binary && !ignoreKey && size1 != size2) {
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

    /* Initialize an object to use as line buffer. */
    line1Ptr = Tcl_NewObj();
    line2Ptr = Tcl_NewObj();
    Tcl_IncrRefCount(line1Ptr);
    Tcl_IncrRefCount(line2Ptr);
    if (binary) {
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
	if (charactersRead1 != charactersRead2  && !ignoreKey) {
	    /* Different size */
	    equal = 0;
	    break;
	}
	if (binary) {
	    string1 = (char *) Tcl_GetByteArrayFromObj(line1Ptr, &length1);
	    string2 = (char *) Tcl_GetByteArrayFromObj(line2Ptr, &length2);
	} else {
	    string1 = Tcl_GetStringFromObj(line1Ptr, &length1);
	    string2 = Tcl_GetStringFromObj(line2Ptr, &length2);
	}
	if (ignoreKey) {

	} else {
	    if (length1 != length2) {
		equal = 0;
		break;
	    }
	    if (binary) {
		if (strncmp(string1, string2, length1) != 0) {
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
