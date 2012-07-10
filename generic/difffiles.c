/***********************************************************************
 *
 * This file implements the functions for comparing files.
 *
 * Copyright (c) 2004, 2010, Peter Spjuth
 *
 ***********************************************************************/

#include <tcl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "diffutil.h"

typedef struct {
    Tcl_Obj *encodingPtr;
    Tcl_Obj *translationPtr;
} FileOptions_T;

/* Helper to get a filled in FileOptions_T */
#define InitFileOptions_T(opts) {opts.encodingPtr = NULL; opts.translationPtr = NULL;}

/*
 * Open a file for reading and configure the channel.
 */
static Tcl_Channel
OpenReadChannel(Tcl_Interp *interp,
                Tcl_Obj *namePtr,
                FileOptions_T *fileOptsPtr)
{
    Tcl_Channel ch;
    ch = Tcl_FSOpenFileChannel(interp, namePtr, "r", 0);
    if (ch == NULL) {
        return NULL;
    }
    if (fileOptsPtr->translationPtr != NULL) {
	char *valueName = Tcl_GetString(fileOptsPtr->translationPtr);
	if (Tcl_SetChannelOption(interp, ch, "-translation", valueName)
		!= TCL_OK) {
            Tcl_Close(interp, ch);
            return NULL;
	}
    }
    /*
     * Apply encoding after translation since -translation binary resets
     * encoding. If a user sets both this is likely what they want.
     */
    if (fileOptsPtr->encodingPtr != NULL) {
	char *valueName = Tcl_GetString(fileOptsPtr->encodingPtr);
	if (Tcl_SetChannelOption(interp, ch, "-encoding", valueName)
		!= TCL_OK) {
            Tcl_Close(interp, ch);
            return NULL;
	}
    }
    return ch;
}

/*
 * Read two files, hash them and prepare the datastructures needed in LCS.
 */
static int
ReadAndHashFiles(Tcl_Interp *interp,
	         Tcl_Obj *name1Ptr, Tcl_Obj *name2Ptr,
                 DiffOptions_T *optsPtr,
                 FileOptions_T *fileOptsPtr,
                 Line_T *mPtr, Line_T *nPtr,
                 P_T **PPtr, E_T **EPtr)
{
    int result = TCL_OK;
    V_T *V = NULL;
    E_T *E = NULL;
    P_T *P = NULL;
    Tcl_StatBuf buf1, buf2;
    Hash_T h, realh;
    Line_T line, j, m = 0, n = 0;
    Line_T allocedV, allocedP, first, last;
    Tcl_Channel ch;
    Tcl_Obj *linePtr;

    /* Stat files first to quickly see if they don't exist */
    if (Tcl_FSStat(name1Ptr, &buf1) != 0) {
        /* FIXA: error message */
        Tcl_SetResult(interp, "bad file", TCL_STATIC);
        return TCL_ERROR;
    }
    if (Tcl_FSStat(name2Ptr, &buf2) != 0) {
        /* FIXA: error message */
        Tcl_SetResult(interp, "bad file", TCL_STATIC);
        return TCL_ERROR;
    }

    /* Initialize an object to use as line buffer. */
    linePtr = Tcl_NewObj();
    Tcl_IncrRefCount(linePtr);
    Tcl_SetObjLength(linePtr, 1000);
    Tcl_SetObjLength(linePtr, 0);

    /* Guess the number of lines in name2 for an inital allocation of V */
    allocedV = buf2.st_size / 40;
    /* If the guess is low, alloc some more to be safe. */
    if (allocedV < 5000) allocedV = 5000;
    V = (V_T *) ckalloc(allocedV * sizeof(V_T));

    /*
     * Read file 2 and calculate hashes for each line, to fill in
     * the V vector.
     */

    ch = OpenReadChannel(interp, name2Ptr, fileOptsPtr);
    if (ch == NULL) {
        result = TCL_ERROR;
        goto cleanup;
    }

    /* Skip the first lines if there is a range set. */
    line = 1;
    while (line < optsPtr->rFrom2) {
	Tcl_SetObjLength(linePtr, 0);
	if (Tcl_GetsObj(ch, linePtr) < 0) break;
	line++;
    }

    n = 1;
    while (1) {
        V[n].serial = n;
        Tcl_SetObjLength(linePtr, 0);
        if (Tcl_GetsObj(ch, linePtr) < 0) {
            n--;
            break;
        }

        Hash(linePtr, optsPtr, 0, &V[n].hash, &V[n].realhash);
        if (optsPtr->rTo2 > 0 && optsPtr->rTo2 <= line) break;

        n++;
        line++;
	/* Reallocate if more room is needed */
        if (n >= allocedV) {
            allocedV = allocedV * 3 / 2;
            V = (V_T *) ckrealloc((char *) V, allocedV * sizeof(V_T));
        }
    }
    Tcl_Close(interp, ch);

    /*
     * Sort the V vector on hash/serial to allow fast search.
     */

    qsort(&V[1], (unsigned long) n, sizeof(V_T), CompareV);

    /* Build E vector from V vector */
    E = BuildEVector(V, n);

    /*
     * Build P vector from file 1
     */

    /* Guess the number of lines in name1 for an inital allocation of P */
    allocedP = buf1.st_size / 40;
    /* If the guess is low, alloc some more to be safe. */
    if (allocedP < 10000) allocedP = 10000;
    P = (P_T *) ckalloc(allocedP * sizeof(P_T));

    /* Read file and calculate hashes for each line */
    ch = OpenReadChannel(interp, name1Ptr, fileOptsPtr);
    if (ch == NULL) {
        result = TCL_ERROR;
        goto cleanup;
    }

    /* Skip the first lines if there is a range set. */
    line = 1;
    if (optsPtr->rFrom1 > 1) {
        while (line < optsPtr->rFrom1) {
            Tcl_SetObjLength(linePtr, 0);
            if (Tcl_GetsObj(ch, linePtr) < 0) break;
            line++;
        }
    }

    m = 1;
    while (1) {
        P[m].Eindex = 0;
        P[m].forbidden = 0;
        Tcl_SetObjLength(linePtr, 0);
        if (Tcl_GetsObj(ch, linePtr) < 0) {
            m--;
            break;
        }
        Hash(linePtr, optsPtr, 1, &h, &realh);
        P[m].hash = h;
        P[m].realhash = realh;

        /* Binary search for hash in V */
        first = 1;
        last = n;
	j = 1;
        while (first <= last) {
            j = (first + last) / 2;
            if (V[j].hash == h) break;
            if (V[j].hash < h) {
                first = j + 1;
            } else {
                last = j - 1;
            }
        }
        if (V[j].hash == h) {
	    /* Search back to the first in the class */
            while (!E[j-1].last) j--;
            P[m].Eindex = j;
            /*printf("P %ld = %ld\n", m, j);*/
        }

	/* Abort if the limited range has been filled */
        if (optsPtr->rTo1 > 0 && optsPtr->rTo1 <= line) break;

        m++;
        line++;
	/* Realloc if necessary */
        if (m >= allocedP) {
            allocedP = allocedP * 3 / 2;
            P = (P_T *) ckrealloc((char *) P,
                                  allocedP * sizeof(P_T));
        }
    }
    Tcl_Close(interp, ch);

    /* Clean up */
    cleanup:
    ckfree((char *) V);
    Tcl_DecrRefCount(linePtr);

    if (result != TCL_OK) {
        if (P != NULL) ckfree((char *) P);
        if (E != NULL) ckfree((char *) E);
        P = NULL; E = NULL;
    }
    *mPtr = m;
    *nPtr = n;
    *PPtr = P;
    *EPtr = E;
    return result;
}

/* Do the diff files operation */
static int
CompareFiles(
	Tcl_Interp *interp,
	Tcl_Obj *name1Ptr,
	Tcl_Obj *name2Ptr,
	DiffOptions_T *optsPtr,
	FileOptions_T *fileOptsPtr,
	Tcl_Obj **resPtr)
{
    E_T *E;
    P_T *P;
    Line_T m, n, *J;
    Tcl_Channel ch1, ch2;
    Tcl_Obj *line1Ptr, *line2Ptr;
    Line_T current1, current2;
    /*Line_T startBlock1, startBlock2;*/

    /*printf("Doing ReadAndHash\n"); */
    if (ReadAndHashFiles(interp, name1Ptr, name2Ptr, optsPtr, fileOptsPtr,
		    &m, &n, &P, &E) != TCL_OK) {
        return TCL_ERROR;
    }

    /* Handle the trivial case. */
    if (m == 0 || n == 0) {
        *resPtr = BuildResultFromJ(interp, optsPtr, m, n, NULL);
	ckfree((char *) E);
	ckfree((char *) P);
	return TCL_OK;
    }

    J = LcsCore(interp, m, n, P, E, optsPtr);

    ckfree((char *) E);
    ckfree((char *) P);

    /*
     * Now we have a list of matching lines in J.  We need to go through
     * the files and check that matching lines really are matching.
     */

    /* Initialize objects to use as line buffers. */
    line1Ptr = Tcl_NewObj();
    Tcl_IncrRefCount(line1Ptr);
    Tcl_SetObjLength(line1Ptr, 1000);
    line2Ptr = Tcl_NewObj();
    Tcl_IncrRefCount(line2Ptr);
    Tcl_SetObjLength(line2Ptr, 1000);

    ch1 = Tcl_FSOpenFileChannel(interp, name1Ptr, "r", 0);
    ch2 = Tcl_FSOpenFileChannel(interp, name2Ptr, "r", 0);
    if (fileOptsPtr->encodingPtr != NULL) {
	char *valueName = Tcl_GetString(fileOptsPtr->encodingPtr);
	Tcl_SetChannelOption(interp, ch1, "-encoding", valueName);
	Tcl_SetChannelOption(interp, ch2, "-encoding", valueName);
    }
    if (fileOptsPtr->translationPtr != NULL) {
	char *valueName = Tcl_GetString(fileOptsPtr->translationPtr);
	Tcl_SetChannelOption(interp, ch1, "-translation", valueName);
	Tcl_SetChannelOption(interp, ch2, "-translation", valueName);
    }

    /* Skip start if there is a range */
    if (optsPtr->rFrom1 > 1) {
	int line = 1;
	while (line < optsPtr->rFrom1) {
	    /* Skip the first lines */
	    Tcl_SetObjLength(line1Ptr, 0);
	    if (Tcl_GetsObj(ch1, line1Ptr) < 0) break;
	    line++;
	}
    }
    if (optsPtr->rFrom2 > 1) {
	int line = 1;
	while (line < optsPtr->rFrom2) {
	    /* Skip the first lines */
	    Tcl_SetObjLength(line2Ptr, 0);
	    if (Tcl_GetsObj(ch2, line2Ptr) < 0) break;
	    line++;
	}
    }

    /*startBlock1 = startBlock2 = 1;*/
    current1 = current2 = 0;

    while (current1 < m || current2 < n) {
	/* Scan file 1 until next supposed match */
	while (current1 < m) {
	    current1++;
	    Tcl_SetObjLength(line1Ptr, 0);
	    Tcl_GetsObj(ch1, line1Ptr);
	    if (J[current1] != 0) break;
	}
	/* Scan file 2 until next supposed match */
	while (current2 < n) {
	    current2++;
	    Tcl_SetObjLength(line2Ptr, 0);
	    Tcl_GetsObj(ch2, line2Ptr);
	    if (J[current1] == current2) break;
	}
	/* Do they really match? */
	/*printf("Compare %d (%ld) to %d\n", current1, J[current1], */
	/*  current2); */
	if (J[current1] != current2) continue;
	if (CompareObjects(line1Ptr, line2Ptr, optsPtr) != 0) {
	    /* Unmark since they don't match */
	    J[current1] = 0;
            /*printf("Unmarking unmatched %ld vs %ld\n", current1, current2);*/
	}
    }

    Tcl_Close(interp, ch1);
    Tcl_Close(interp, ch2);
    Tcl_DecrRefCount(line1Ptr);
    Tcl_DecrRefCount(line2Ptr);

    /*
     * Now the J vector is valid, generate a list of
     * insert/delete/change operations.
     */

    *resPtr = BuildResultFromJ(interp, optsPtr, m, n, J);

    ckfree((char *) J);
    return TCL_OK;
}

int
DiffFilesObjCmd(
    ClientData dummy,    	/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *CONST objv[])	/* Argument objects. */
{
    int index, resultStyle, t, result = TCL_OK;
    Tcl_Obj *resPtr, *file1Ptr, *file2Ptr;
    DiffOptions_T opts;
    FileOptions_T fileOpts;
    static CONST char *options[] = {
	"-b", "-w", "-i", "-nocase", "-align", "-encoding", "-range",
        "-noempty", "-nodigit", "-pivot", "-regsub", "-regsubleft",
	"-regsubright", "-result", "-translation", (char *) NULL
    };
    enum options {
	OPT_B, OPT_W, OPT_I, OPT_NOCASE, OPT_ALIGN, OPT_ENCODING, OPT_RANGE,
        OPT_NOEMPTY, OPT_NODIGIT, OPT_PIVOT, OPT_REGSUB, OPT_REGSUBLEFT,
	OPT_REGSUBRIGHT, OPT_RESULT, OPT_TRANSLATION
    };
    static CONST char *resultOptions[] = {
	"diff", "match", (char *) NULL
    };

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
	return TCL_ERROR;
    }

    InitDiffOptions_T(opts);
    InitFileOptions_T(fileOpts);

    for (t = 1; t < objc - 2; t++) {
	if (Tcl_GetIndexFromObj(interp, objv[t], options, "option", 0,
		&index) != TCL_OK) {
            result = TCL_ERROR;
            goto cleanup;
	}
	switch (index) {
	  case OPT_NOCASE:
	  case OPT_I:
            opts.ignore |= IGNORE_CASE;
	    break;
	  case OPT_B:
            opts.ignore |= IGNORE_SPACE_CHANGE;
	    break;
	  case OPT_W:
            opts.ignore |= IGNORE_ALL_SPACE;
	    break;
	  case OPT_NODIGIT:
            opts.ignore |= IGNORE_NUMBERS;
	    break;
	  case OPT_NOEMPTY:
            opts.noempty = 1;
            break;
          case OPT_PIVOT:
            t++;
            if (t >= objc - 2) {
                Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
                result = TCL_ERROR;
                goto cleanup;
            }
            if (Tcl_GetIntFromObj(interp, objv[t], &opts.pivot) != TCL_OK) {
                result = TCL_ERROR;
                goto cleanup;
            }
            if (opts.pivot < 1) {
                Tcl_SetResult(interp, "Pivot must be at least 1", TCL_STATIC);
                result = TCL_ERROR;
                goto cleanup;
            }
            break;
          case OPT_REGSUB:
          case OPT_REGSUBLEFT:
          case OPT_REGSUBRIGHT:
            t++;
            if (t >= (objc - 2)) {
                /* FIXA error message */
                Tcl_SetResult(interp, "missing value", TCL_STATIC);
                result = TCL_ERROR;
                goto cleanup;
            }
	    if (index != OPT_REGSUBRIGHT) {
		if (opts.regsubLeftPtr == NULL) {
		    opts.regsubLeftPtr = Tcl_NewListObj(0, NULL);
		    Tcl_IncrRefCount(opts.regsubLeftPtr);
		}
		if (Tcl_ListObjAppendList(interp, opts.regsubLeftPtr, objv[t])
			!= TCL_OK) {
		    result = TCL_ERROR;
		    goto cleanup;
		}
	    }
	    if (index != OPT_REGSUBLEFT) {
		if (opts.regsubRightPtr == NULL) {
		    opts.regsubRightPtr = Tcl_NewListObj(0, NULL);
		    Tcl_IncrRefCount(opts.regsubRightPtr);
		}
		if (Tcl_ListObjAppendList(interp, opts.regsubRightPtr, objv[t])
			!= TCL_OK) {
		    result = TCL_ERROR;
		    goto cleanup;
		}
	    }
	    break;
	  case OPT_RANGE:
            t++;
            if (t >= (objc - 2)) {
                /* FIXA error message */
                Tcl_SetResult(interp, "missing value", TCL_STATIC);
                result = TCL_ERROR;
                goto cleanup;
            }
            if (SetOptsRange(interp, objv[t], 1, &opts) != TCL_OK) {
                result = TCL_ERROR;
                goto cleanup;
            }
            break;
	  case OPT_ALIGN:
            t++;
            if (t >= (objc - 2)) {
                /* FIXA error message */
                Tcl_SetResult(interp, "missing value", TCL_STATIC);
                result = TCL_ERROR;
                goto cleanup;
            }
            if (SetOptsAlign(interp, objv[t], 1, &opts) != TCL_OK) {
                result = TCL_ERROR;
                goto cleanup;
            }
            break;
	  case OPT_RESULT:
	      t++;
	      if (t >= objc - 2) {
		  Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
		  result = TCL_ERROR;
		  goto cleanup;
	      }
	      if (Tcl_GetIndexFromObj(interp, objv[t], resultOptions,
			      "result style", 0, &resultStyle) != TCL_OK) {
		  result = TCL_ERROR;
		  goto cleanup;
	      }
	      opts.resultStyle = resultStyle;
	      break;
	  case OPT_ENCODING:
	      t++;
	      if (t >= objc - 2) {
		  Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
		  result = TCL_ERROR;
		  goto cleanup;
	      }
	      fileOpts.encodingPtr = objv[t];
	      Tcl_IncrRefCount(objv[t]);
	      break;
	  case OPT_TRANSLATION:
	      t++;
	      if (t >= objc - 2) {
		  Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
		  result = TCL_ERROR;
		  goto cleanup;
	      }
	      fileOpts.translationPtr = objv[t];
	      Tcl_IncrRefCount(objv[t]);
	      break;
	}
    }
    NormaliseOpts(&opts);
    file1Ptr = objv[objc-2];
    file2Ptr = objv[objc-1];

    if (CompareFiles(interp, file1Ptr, file2Ptr, &opts, &fileOpts, &resPtr)
	    != TCL_OK) {
        result = TCL_ERROR;
        goto cleanup;
    }
    Tcl_SetObjResult(interp, resPtr);

    cleanup:
    if (opts.regsubLeftPtr != NULL) {
	Tcl_DecrRefCount(opts.regsubLeftPtr);
    }
    if (opts.regsubRightPtr != NULL) {
	Tcl_DecrRefCount(opts.regsubRightPtr);
    }
    if (fileOpts.encodingPtr != NULL) {
	Tcl_DecrRefCount(fileOpts.encodingPtr);
    }
    if (fileOpts.translationPtr != NULL) {
	Tcl_DecrRefCount(fileOpts.translationPtr);
    }
    if (opts.alignLength > STATIC_ALIGN) {
        ckfree((char *) opts.align);
    }

    return result;
}
