/***********************************************************************
 *
 * This file implements the functions for comparing lists.
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

/*
 * Scan two lists, hash them and prepare the datastructures needed in LCS.
 */
static int
HashLists(Tcl_Interp *interp,
	Tcl_Obj *list1Ptr, Tcl_Obj *list2Ptr,
	DiffOptions_T *optsPtr,
	Line_T *mPtr, Line_T *nPtr,
	P_T **PPtr, E_T **EPtr)
{
    V_T *V = NULL;
    E_T *E = NULL;
    P_T *P = NULL;
    Hash_T h, realh;
    Line_T j, m = 0, n = 0;
    int length1, length2, t;
    Line_T first, last;
    Tcl_Obj **elem1Ptrs, **elem2Ptrs;

    if (Tcl_ListObjGetElements(interp, list1Ptr, &length1, &elem1Ptrs) != TCL_OK) {
	return TCL_ERROR;
    }

    if (Tcl_ListObjGetElements(interp, list2Ptr, &length2, &elem2Ptrs) != TCL_OK) {
	return TCL_ERROR;
    }

    m = length1;
    n = length2;
    V = (V_T *) ckalloc((n + 1) * sizeof(V_T));

    /*
     * Read list 2 and calculate hashes for each element, to fill in
     * the V vector.
     */

    for (t = 1; t <= n; t++) {
        V[t].serial = t;

        Hash(elem2Ptrs[t-1], optsPtr, 0, &V[t].hash, &V[t].realhash);
    }

    /*
     * Sort the V vector on hash/serial to allow fast search.
     */

    qsort(&V[1], (unsigned long) n, sizeof(V_T), CompareV);

    /* Build E vector from V vector */
    E = BuildEVector(V, n);

    /*
     * Build P vector from list 1
     */

    P = (P_T *) ckalloc((m + 1) * sizeof(P_T));

    /* Read list and calculate hashes for each element */

    for (t = 1; t <= m; t++) {
        P[t].Eindex = 0;
        Hash(elem1Ptrs[t-1], optsPtr, 1, &h, &realh);
        P[t].hash = h;
        P[t].realhash = realh;

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
            P[t].Eindex = j;
        }
    }

    ckfree((char *) V);

    *mPtr = m;
    *nPtr = n;
    *PPtr = P;
    *EPtr = E;
    return TCL_OK;
}

/* Do the diff lists operation */
static int
CompareLists(
	Tcl_Interp *interp,
	Tcl_Obj *list1Ptr,
	Tcl_Obj *list2Ptr,
	DiffOptions_T *optsPtr,
	Tcl_Obj **resPtr)
{
    E_T *E;
    P_T *P;
    Line_T m, n, *J;
    int length1, length2;
    Tcl_Obj **elem1Ptrs, **elem2Ptrs;
    Line_T current1, current2;
    Line_T startBlock1, startBlock2;

    if (HashLists(interp, list1Ptr, list2Ptr, optsPtr, &m, &n, &P, &E)
        != TCL_OK) {
        return TCL_ERROR;
    }

    /* Handle the trivial case. */
    if (m == 0 || n == 0) {
	*resPtr = Tcl_NewListObj(0, NULL);
	if ((n > 0) || (m > 0)) {
	    AppendChunk(interp, *resPtr, optsPtr, 1, m, 1, n);
	}
	ckfree((char *) E);
	ckfree((char *) P);
	return TCL_OK;
    }

    J = LcsCore(interp, m, n, P, E, optsPtr);

    ckfree((char *) E);
    ckfree((char *) P);

    /*
     * Now we have a list of matching lines in J.  We need to go through
     * the lists and check that matching elements really are matching.
     */

    Tcl_ListObjGetElements(interp, list1Ptr, &length1, &elem1Ptrs);
    Tcl_ListObjGetElements(interp, list2Ptr, &length2, &elem2Ptrs);

    startBlock1 = startBlock2 = 1;
    current1 = current2 = 0;

    while (current1 < m || current2 < n) {
	/* Scan list 1 until next supposed match */
	while (current1 < m) {
	    current1++;
	    if (J[current1] != 0) break;
	}
	/* Scan list 2 until next supposed match */
	while (current2 < n) {
	    current2++;
	    if (J[current1] == current2) break;
	}
	/* Do they really match? */
	if (J[current1] != current2) continue;
	if (CompareObjects(elem1Ptrs[current1-1], elem2Ptrs[current2-1],
			optsPtr) != 0) {
	    /* Unmark since they don't match */
	    J[current1] = 0;
	}
    }

    /*
     * Now the J vector is valid, generate a list of
     * insert/delete/change operations.
     */

    *resPtr = BuildResultFromJ(interp, optsPtr, m, n, J);

    ckfree((char *) J);
    return TCL_OK;
}

int
DiffListsObjCmd(
    ClientData dummy,    	/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *CONST objv[])	/* Argument objects. */
{
    int index, t, result = TCL_OK;
    Tcl_Obj *resPtr, *list1Ptr, *list2Ptr;
    DiffOptions_T opts;
    static CONST char *options[] = {
	"-b", "-w", "-i", "-nocase",
        "-noempty", "-nodigit", (char *) NULL
    };
    enum options {
	OPT_B, OPT_W, OPT_I, OPT_NOCASE,
        OPT_NOEMPTY, OPT_NODIGIT
    };

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?opts? list1 list2");
	return TCL_ERROR;
    }

    InitDiffOptions_T(opts);

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
	}
    }
    NormaliseOpts(&opts);
    /*
     * Element indexes starts with 0, while LCS works from 1.
     * By starting the range from 0, the result is adjusted.
     */
    opts.rFrom1 = 0;
    opts.rFrom2 = 0;
    list1Ptr = objv[objc-2];
    list2Ptr = objv[objc-1];

    if (CompareLists(interp, list1Ptr, list2Ptr, &opts, &resPtr) != TCL_OK) {
        result = TCL_ERROR;
        goto cleanup;
    }
    Tcl_SetObjResult(interp, resPtr);

    cleanup:

    return result;
}
