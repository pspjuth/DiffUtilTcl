/***********************************************************************
 *
 * This file implements the functions for comparing strings.
 *
 * Copyright (c) 2004, Peter Spjuth
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
 * Prepare the datastructures needed in LCS for two strings.
 */
static void
PrepareStringsLcs(Tcl_Interp *interp,
                  char *str1, int len1,
                  char *str2, int len2,
                  DiffOptions_T *optsPtr,
                  Line_T *mPtr, Line_T *nPtr,
                  P_T **PPtr, E_T **EPtr)
{
    V_T *V = NULL;
    E_T *E = NULL;
    P_T *P = NULL;
    Hash_T h;
    Line_T j, m = 0, n = 0;
    char *str;
    Tcl_UniChar c, realc;

    /* Allocate V vector for string 2 */
    V = (V_T *) ckalloc((len2 + 1) * sizeof(V_T));

    /* Go through string 2 and fill in hashes for each char */

    n = 0;
    str = str2;
    while (*str != 0) {
        str += Tcl_UtfToUniChar(str, &realc);
        n++;
        V[n].serial = n;
        /* FIXA -ignore */
        if (optsPtr->ignore & IGNORE_CASE) {
            c = Tcl_UniCharToLower(realc);
        } else {
            c = realc;
        }
        V[n].hash = c;
        V[n].realhash = realc;
    }

    /* Sort V on hash/serial. */
    SortV(V, n, optsPtr);

    /* Build E vector */
    E = BuildEVector(V, n, optsPtr);

    /* Build P vector */

    /* Allocate P vector for string 1 */
    P = (P_T *) ckalloc((len1 + 1) * sizeof(P_T));

    m = 0;
    str = str1;
    while (*str != 0) {
        str += Tcl_UtfToUniChar(str, &realc);
        /* FIXA -ignore */
        if (optsPtr->ignore & IGNORE_CASE) {
            c = Tcl_UniCharToLower(realc);
        } else {
            c = realc;
        }
        m++;
        P[m].Eindex = 0;
        P[m].forbidden = 0;
        P[m].hash = c;
        P[m].realhash = realc;
        h = c;

        /* Binary search for hash in V */
        j = BSearchVVector(V, n, h, optsPtr);
        if (V[j].hash == h) {
            while (!E[j-1].last) j--;
            P[m].Eindex = j;
            /*printf("P %ld = %ld\n", m, j);*/
        }
    }

    /* Clean up */
    ckfree((char *) V);

    *mPtr = m;
    *nPtr = n;
    *PPtr = P;
    *EPtr = E;
}

/*
 * Split a string into a list where each element is good as a chunk
 * for comparison.
 * E.g. if ws is ignored, any stretch of whitespace becomes one element.
 * If nothing is ignored, it will be one element per character.
 */
Tcl_Obj *
SplitString(Tcl_Obj *strPtr,
            DiffOptions_T *optsPtr)
{
    int state = 0;
    int length, chsize, isSpace;
    char *string, *str, *startWord, *endWord;
    Tcl_UniChar c;
    Tcl_Obj *resPtr;
    int igSpace = (optsPtr->ignore & (IGNORE_SPACE_CHANGE | IGNORE_ALL_SPACE));
    int word = optsPtr->wordparse;

    resPtr = Tcl_NewListObj(0, NULL);
    Tcl_IncrRefCount(resPtr);
    
    string = Tcl_GetStringFromObj(strPtr, &length);
    str = string;
    startWord = str;
    while (*str != 0) {
        endWord = str;
        chsize = Tcl_UtfToUniChar(str, &c);
        isSpace = Tcl_UniCharIsSpace(c);
        str += chsize;
        if (state == 0) {
            if (igSpace && isSpace) {
                state = 1;
                continue;
            }
            if (word && !isSpace) {
                state = 2;
                continue;
            }
        }
        if (state == 1 && isSpace) {
            continue;
        }
        if (state == 2 && !isSpace) {
            continue;
        }
        if (state == 0) {
            /* The just seen char is part of the chunk */
            endWord = str;
        }
        Tcl_ListObjAppendElement(
            NULL, resPtr,
            Tcl_NewStringObj(startWord, endWord-startWord));
        startWord = endWord;
        str = endWord;
        state = 0;
    }
    if (startWord < str) {
        /* Last chunk */
        Tcl_ListObjAppendElement(
            NULL, resPtr,
            Tcl_NewStringObj(startWord, str-startWord));
    }
    return resPtr;
}

/*
 * The main string LCS routine returns the J vector.
 * J is ckalloc:ed and need to be freed by the caller.
 */
static void
CompareStrings1(Tcl_Interp *interp,
	Tcl_Obj *str1Ptr, Tcl_Obj *str2Ptr,
	DiffOptions_T *optsPtr,
	Line_T **resPtr, Line_T *mPtr, Line_T *nPtr)
{
    E_T *E;
    P_T *P;
    Line_T m, n, *J, *newJ;
    int i, j, length1, length2, chsize1, chsize2, len1, len2;
    char *string1, *string2, *str1, *str2;
    int skip1start = 0, skip2start = 0;
    Tcl_UniChar c1, c2;
    const int nocase = optsPtr->ignore & IGNORE_CASE;

    /*
     * Start by detecting leading and trailing equalities to lessen
     * the load on the LCS algorithm
     */
    string1 = Tcl_GetStringFromObj(str1Ptr, &length1);
    string2 = Tcl_GetStringFromObj(str2Ptr, &length2);
    str1 = string1;
    str2 = string2;
    if (optsPtr->ignore & (IGNORE_SPACE_CHANGE | IGNORE_ALL_SPACE)) {
        /* Skip all leading white-space */
        while (*str1 != 0) {
            chsize1 = Tcl_UtfToUniChar(str1, &c1);
            if (!Tcl_UniCharIsSpace(c1)) break;
            str1 += chsize1;
            skip1start++;
        }
        while (*str2 != 0) {
            chsize2 = Tcl_UtfToUniChar(str2, &c2);
            if (!Tcl_UniCharIsSpace(c2)) break;
            str2 += chsize2;
            skip2start++;
        }
	/*printf("Ignore %d %d %d\n", optsPtr->ignore, skip1start, skip2start); */
    }
    /* Skip leading equalities */
    while (*str1 != 0 && *str2 != 0) {
        chsize1 = Tcl_UtfToUniChar(str1, &c1);
        chsize2 = Tcl_UtfToUniChar(str2, &c2);
        if (c1 != c2 &&
            (!nocase ||
             Tcl_UniCharToLower(c1) != Tcl_UniCharToLower(c2))) break;
        str1 += chsize1;
        str2 += chsize2;
        skip1start++;
        skip2start++;
    }
    len1 = length1 - (str1 - string1);
    len2 = length2 - (str2 - string2);

    if (len1 == 0 || len2 == 0) {
	/*
	 * The trivial case of nothing left.
	 * Just fill in an empty J vector.
	 */

	m = len1;
	n = len2;
	J = (Line_T *) ckalloc((m + 1) * sizeof(Line_T));
	for (i = 0; i <= m; i++) {
	    J[i] = 0;
	}
    } else {
	/*printf("Doing ReadAndHash\n");*/
	PrepareStringsLcs(interp, str1, len1, str2, len2,
		optsPtr, &m, &n, &P, &E);

	/*printf("Doing LcsCore m = %ld, n = %ld for '%s' '%s'\n", m, n, string1, string2); */
	J = LcsCore(interp, m, n, P, E, optsPtr);
	/*printf("Done LcsCore\n");*/

	ckfree((char *) E);
	ckfree((char *) P);
    }

    if (skip1start > 0) {
        /* Need to reallocate J to fill in the leading places */
        newJ = (Line_T *) ckalloc((m + skip1start + 1) * sizeof(Line_T));
        newJ[0] = 0;
        for (i = 1; i <= (skip1start - skip2start); i++) {
            newJ[i] = 0;
        }
        j = skip2start - skip1start + 1;
        if (j < 1) j = 1;
        for (; i <= skip1start; i++, j++) {
            newJ[i] = j;
        }
        for (; i <= (m + skip1start); i++) {
            newJ[i] = J[i-skip1start];
            if (newJ[i] > 0) {
                newJ[i] += skip2start;
            }
        }
        ckfree((char *) J);
        J = newJ;
        m += skip1start;
    } else if (skip2start > 0) {
        /* Adjust line numbers in J */
        for (i = 1; i <= m; i++) {
            if (J[i] > 0) {
                J[i] += skip2start;
            }
        }
    }
    n += skip2start;

    *mPtr = m;
    *nPtr = n;
    *resPtr = J;
}

/*
 * Make a string by joining a subrange of a list.
 */
static Tcl_Obj *
JoinRange(Tcl_Obj **elemPtrs, int from, int to)
{
    Tcl_Obj *resPtr;
    int i;

    resPtr = Tcl_NewObj();
    for (i = from; i <= to; i++) {
	Tcl_AppendObjToObj(resPtr, elemPtrs[i]);
    }
    return resPtr;
}

/*
 * Compare Strings through list chunks.
 */
static void
CompareStringsL(Tcl_Interp *interp,
                Tcl_Obj *str1Ptr, Tcl_Obj *str2Ptr,
                DiffOptions_T *optsPtr,
                Tcl_Obj **resPtr)
{
    Tcl_Obj *list1Ptr, *list2Ptr, *listResPtr;
    int chunk, from1, from2, to1, to2;
    int ch1, ch2, ch3, ch4;
    int length1, length2, lengthr, lengthc;
    Tcl_Obj **elem1Ptrs, **elem2Ptrs, **elemRPtrs, **elemCPtrs;

    /* Do it chunk-wise through list diff */
    list1Ptr = SplitString(str1Ptr, optsPtr);
    list2Ptr = SplitString(str2Ptr, optsPtr);
    optsPtr->resultStyle = Result_Diff;
    optsPtr->firstIndex = 0;
    CompareLists(interp, list1Ptr, list2Ptr, optsPtr, &listResPtr);
    /* Go through the chunks */
    *resPtr = Tcl_NewListObj(0, NULL);
    Tcl_ListObjGetElements(NULL, list1Ptr, &length1, &elem1Ptrs);
    Tcl_ListObjGetElements(NULL, list2Ptr, &length2, &elem2Ptrs);
    /*printf("Split l1 %d l2 %d\n", length1, length2);
    printf("L1 %s\n", Tcl_GetString(list1Ptr));
    printf("L2 %s\n", Tcl_GetString(list2Ptr));*/

    Tcl_ListObjGetElements(NULL, listResPtr, &lengthr, &elemRPtrs);
    from1 = from2 = 0;
    for (chunk = 0; chunk < lengthr; chunk++) {
        Tcl_ListObjGetElements(NULL, elemRPtrs[chunk], &lengthc, &elemCPtrs);
        Tcl_GetIntFromObj(interp, elemCPtrs[0], &ch1);
        Tcl_GetIntFromObj(interp, elemCPtrs[1], &ch2);
        Tcl_GetIntFromObj(interp, elemCPtrs[2], &ch3);
        Tcl_GetIntFromObj(interp, elemCPtrs[3], &ch4);
        /*printf("Chunk %d %d %d %d\n", ch1, ch2, ch3, ch4);*/

        /* Pre chunk */
        to1 = ch1 - 1;
        to2 = ch3 - 1;
        Tcl_ListObjAppendElement(interp, *resPtr,
                                 JoinRange(elem1Ptrs, from1, to1));
        Tcl_ListObjAppendElement(interp, *resPtr,
                                 JoinRange(elem2Ptrs, from2, to2));
        /* Chunk */
        from1 = ch1;
        from2 = ch3;
        to1 = ch1 + ch2 - 1;
        to2 = ch3 + ch4 - 1;
        Tcl_ListObjAppendElement(interp, *resPtr,
                                 JoinRange(elem1Ptrs, from1, to1));
        Tcl_ListObjAppendElement(interp, *resPtr,
                                 JoinRange(elem2Ptrs, from2, to2));
        from1 = to1+1;
        from2 = to2+1;
    }
    /* Post last chunk */
    to1 = length1 - 1;
    to2 = length2 - 1;
    Tcl_ListObjAppendElement(interp, *resPtr,
                             JoinRange(elem1Ptrs, from1, to1));
    Tcl_ListObjAppendElement(interp, *resPtr,
                             JoinRange(elem2Ptrs, from2, to2));

    Tcl_DecrRefCount(list1Ptr);
    Tcl_DecrRefCount(list2Ptr);
}

/*
 * String LCS routine.
 * returns a list of substrings from alternating strings
 * str1sub1 str2sub1 str1sub2 str2sub2...
 * str1sub* concatenated gives string1
 * str2sub* concatenated gives string2
 * str1subN and str2subN are equal when N is odd, not equal when N is even
 */
static void
CompareStrings3(Tcl_Interp *interp,
               Tcl_Obj *str1Ptr, Tcl_Obj *str2Ptr,
               DiffOptions_T *optsPtr,
               Tcl_Obj **resPtr)
{
    Line_T m, n, *J;
    Tcl_Obj *subPtr, *emptyPtr;
    Tcl_UniChar c1, c2, c3;
    int len1, len2;
    int current1, current2;
    int startblock1, startblock2;
    int startchange1, startchange2;

    len1 = Tcl_GetCharLength(str1Ptr);
    len2 = Tcl_GetCharLength(str2Ptr);

    /* Take care of trivial cases first */
    if (len1 == 0 || len2 == 0) {
        *resPtr = Tcl_NewListObj(0, NULL);
        emptyPtr = Tcl_NewObj();
        Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
        Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
        if (len1 > 0 || len2 > 0) {
            Tcl_ListObjAppendElement(interp, *resPtr, str1Ptr);
            Tcl_ListObjAppendElement(interp, *resPtr, str2Ptr);
            Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
            Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
        }
        return;
    }
    
    if ((optsPtr->ignore & (IGNORE_SPACE_CHANGE | IGNORE_ALL_SPACE)) ||
        optsPtr->wordparse) {
        /* Do it chunk-wise through list diff */
        CompareStringsL(interp, str1Ptr, str2Ptr, optsPtr, resPtr);
        return;
    }

    /* Char by char will work */
    CompareStrings1(interp, str1Ptr, str2Ptr, optsPtr, &J, &m, &n);

    /*
     * Now we have a list of matching chars in J.
     * Generate a list of substrings.
     */

    *resPtr = Tcl_NewListObj(0, NULL);
    emptyPtr = Tcl_NewObj();
    Tcl_IncrRefCount(emptyPtr);

    /* All indexes in startblock etc. starts at 1 for the first char. */
    startblock1 = startblock2 = 1;
    current1 = current2 = 1;

    while (1) {
        /* Do equal chars first, so scan until a mismatch */
        while (current1 <= m || current2 <= n) {
            if (J[current1] == 0) break;
            if (J[current1] != current2) break;
            current1++;
            current2++;
        }

        /*
         * Finished?
         * Since the result should always end with an equal pair
         * this is the only exit point from the loop.
         */
        if (current1 > m && current2 > n) {
            if (current1 == startblock1) {
                /* Nothing equal */
                Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
                Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
            } else {
                subPtr = Tcl_GetRange(str1Ptr, startblock1 - 1, current1 - 2);
                Tcl_ListObjAppendElement(interp, *resPtr, subPtr);
                subPtr = Tcl_GetRange(str2Ptr, startblock2 - 1, current2 - 2);
                Tcl_ListObjAppendElement(interp, *resPtr, subPtr);
            }
            break;
        }

        /* Do change block */
        startchange1 = current1;
        startchange2 = current2;

        scanChangeBlock:

        /* Scan string 1 until next match */
        while (current1 <= m) {
            if (J[current1] != 0) break;
            current1++;
        }
        /* Scan string 2 until next match */
        if (current1 <= m) {
            current2 = J[current1];
        } else {
            current2 = n+1;
        }

        if (optsPtr->wordparse) {
            /* FIXA adjust if wordparse */
            /* Adjust start of change */
            if (current1 == startchange1) {
                /* Block 1 is empty, handle the other one */
                while (startchange2 > startblock2) {
                    c1 = Tcl_GetUniChar(str2Ptr, startchange2 - 1 - 1);
                    c2 = Tcl_GetUniChar(str2Ptr, startchange2 - 1);
                    c3 = Tcl_GetUniChar(str2Ptr, current2 - 1 - 1);
                    if (Tcl_UniCharIsSpace(c1) || Tcl_UniCharIsSpace(c2))
                        break;
                    startchange1--;
                    startchange2--;
                    /*
                     * If the one before the change is equal to the one in
                     * the end, we move the block.
                     */
                    if (c1 == c3) {
                        current1--;
                        current2--;
                    }
                }
            } else if (current2 == startchange2) {
                /* Block 2 is empty, handle the other one */
                while (startchange1 > startblock1) {
                    c1 = Tcl_GetUniChar(str1Ptr, startchange1 - 1 - 1);
                    c2 = Tcl_GetUniChar(str1Ptr, startchange1 - 1);
                    c3 = Tcl_GetUniChar(str1Ptr, current1 - 1 - 1);
                    if (Tcl_UniCharIsSpace(c1) || Tcl_UniCharIsSpace(c2))
                        break;
                    startchange1--;
                    startchange2--;
                    /*
                     * If the one before the change is equal to the one in
                     * the end, we move the block.
                     */
                    if (c1 == c3) {
                        current1--;
                        current2--;
                    }
                }
            } else {
                /*printf("1: %d %d %d  2: %d %d %d\n",
                       startblock1, startchange1, current1,
                       startblock2, startchange2, current2);*/
                while (startchange1 > startblock1) {
                    c1 = Tcl_GetUniChar(str1Ptr, startchange1 - 1 - 1);
                    if (Tcl_UniCharIsSpace(c1)) break;
                    startchange1--;
                    startchange2--;
                }
            }
            /* Adjust end of change */
            while (current1 <= m && current2 <= n) {
                if (J[current1] == 0 || J[current1] != current2) {
                    /*
                     * We encountered a difference before any space could
                     * be found.  Go back to scanning the change block.
                     */
                    goto scanChangeBlock;
                }
                c1 = Tcl_GetUniChar(str1Ptr, current1 - 1);
                if (Tcl_UniCharIsSpace(c1)) break;
                current1++;
                current2++;
            }
        } /* if wordparse */

        /* Add the equals to the result */
        if (startchange1 == startblock1) {
            /* Nothing equal */
            Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
            Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
        } else {
            subPtr = Tcl_GetRange(str1Ptr, startblock1 - 1, startchange1 - 2);
            Tcl_ListObjAppendElement(interp, *resPtr, subPtr);
            subPtr = Tcl_GetRange(str2Ptr, startblock2 - 1, startchange2 - 2);
            Tcl_ListObjAppendElement(interp, *resPtr, subPtr);
        }
        /* Add the changes to the result */
        if (current1 <= startchange1) {
            Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
        } else {
            subPtr = Tcl_GetRange(str1Ptr, startchange1 - 1, current1 - 2);
            Tcl_ListObjAppendElement(interp, *resPtr, subPtr);
        }
        if (current2 <= startchange2) {
            Tcl_ListObjAppendElement(interp, *resPtr, emptyPtr);
        } else {
            subPtr = Tcl_GetRange(str2Ptr, startchange2 - 1, current2 - 2);
            Tcl_ListObjAppendElement(interp, *resPtr, subPtr);
        }

        startblock1 = current1;
        startblock2 = current2;
    }

    Tcl_DecrRefCount(emptyPtr);
    ckfree((char *) J);
}

int
DiffStrings2ObjCmd(dummy, interp, objc, objv)
    ClientData dummy;    	/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int index, t, result = TCL_OK;
    Tcl_Obj *res;
    DiffOptions_T opts;

    static CONST char *options[] = {
	"-nocase", "-i", "-b", "-w", "-words", (char *) NULL
    };
    enum options {
	OPT_NOCASE, OPT_I, OPT_B, OPT_W, OPT_WORDS
    };

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?opts? line1 line2");
	return TCL_ERROR;
    }

    InitDiffOptions_T(opts);

    for (t = 1; t < objc - 2; t++) {
	if (Tcl_GetIndexFromObj(interp, objv[t], options, "option", 0,
		&index) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch (index) {
          case OPT_NOCASE :
          case OPT_I :
            opts.ignore |= IGNORE_CASE;
            break;
	  case OPT_B:
            opts.ignore |= IGNORE_SPACE_CHANGE;
	    break;
	  case OPT_W:
            opts.ignore |= IGNORE_ALL_SPACE;
	    break;
	  case OPT_WORDS:
	    opts.wordparse = 1;
	    break;
	}
    }

    CompareStrings3(interp, objv[objc-2], objv[objc-1], &opts, &res);

    Tcl_SetObjResult(interp, res);
    return result;
}
