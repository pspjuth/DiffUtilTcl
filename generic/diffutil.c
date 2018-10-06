/***********************************************************************
 *
 * This is the C implementation of the DiffUtil package
 *
 * Copyright (c) 2004, 2010, Peter Spjuth
 *
 ***********************************************************************/

#include <tcl.h>
#include "diffutil.h"

#undef  TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT

typedef int (CompareFun) (CONST Tcl_UniChar *,
                          CONST Tcl_UniChar *,
                          unsigned long);

/* 'string first' for unicode string */
static int
UniCharFirst(ustring1, length1, ustring2, length2, nocase)
    Tcl_UniChar *ustring1;
    int length1;
    Tcl_UniChar *ustring2;
    int length2;
    int nocase;
{
    int match;
    CompareFun *cmp;
    Tcl_UniChar first1, c2;

    /*
     * We are searching string2 for the sequence string1.
     */
    
    match = -1;
    if (length1 < 0)
	length1 = Tcl_UniCharLen(ustring1);
    if (length2 < 0)
	length2 = Tcl_UniCharLen(ustring2);

    if (nocase) {
        cmp = Tcl_UniCharNcasecmp;
    } else {
        cmp = Tcl_UniCharNcmp;
    }

    if (length1 > 0) {
	register Tcl_UniChar *p, *end;
        if (nocase) {
            first1 = Tcl_UniCharToLower(*ustring1);
        } else {
            first1 = *ustring1;
        }
	end = ustring2 + length2 - length1 + 1;
	for (p = ustring2;  p < end;  p++) {
	    /*
	     * Scan forward to find the first character.
	     */
            if (nocase) {
                c2 = Tcl_UniCharToLower(*p);
            } else {
                c2 = *p;
            }
	    if ((c2 == first1) &&
                (cmp(ustring1, p, (unsigned long) length1) == 0)) {
		match = p - ustring2;
		break;
	    }
	}
    }

    return match;
}

/* Recursively look for common substrings in strings str1 and str2
 * res should point to list object where the result
 * will be appended. */
static void
CompareMidString(interp, obj1, obj2, res, wordparse, nocase)
    Tcl_Interp *interp;
    Tcl_Obj *obj1, *obj2;
    Tcl_Obj *res;
    int wordparse;
    int nocase;
{
    Tcl_UniChar *str1, *str2;
    int len1, len2, t, u, i, newt, newp1;
    int p1, p2, found1 = 0, found2 = 0, foundlen, minlen;
    Tcl_Obj *apa1, *apa2;

    str1 = Tcl_GetUnicodeFromObj(obj1, &len1);
    str2 = Tcl_GetUnicodeFromObj(obj2, &len2);

    /* Is str1 a substring of str2 ?*/
    if (len1 < len2) {
	if ((t = UniCharFirst(str1, len1, str2, len2, nocase)) != -1) {
	    Tcl_ListObjAppendElement(interp, res, Tcl_NewObj());
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewUnicodeObj(str2, t));
	    Tcl_ListObjAppendElement(interp, res, obj1);
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewUnicodeObj(str2 + t, len1));
	    Tcl_ListObjAppendElement(interp, res, Tcl_NewObj());
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewUnicodeObj(str2 + t + len1, -1));
	    return;
	}
    }

    /* Is str2 a substring of str1 ?*/
    if (len2 < len1) {
	if ((t = UniCharFirst(str2, len2, str1, len1, nocase)) != -1) {
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewUnicodeObj(str1, t));
	    Tcl_ListObjAppendElement(interp, res, Tcl_NewObj());
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewUnicodeObj(str1 + t, len2));
	    Tcl_ListObjAppendElement(interp, res, obj2);
	    Tcl_ListObjAppendElement(interp, res,
		    Tcl_NewUnicodeObj(str1 + t + len2, -1));
	    Tcl_ListObjAppendElement(interp, res, Tcl_NewObj());
	    return;
	}
    }

    /* Are they too short to be considered ? */
    if (len1 < 4 || len2 < 4) {
	Tcl_ListObjAppendElement(interp, res, obj1);
	Tcl_ListObjAppendElement(interp, res, obj2);
        return;
    }

    /* Find the longest string common to both strings */

    foundlen = -1;
    minlen = 2; /* The shortest common substring we detect is 3 chars*/

    for (t = 0, u = minlen; u < len1; t++, u++) {
        i = UniCharFirst(str1 + t, u - t + 1, str2, len2, nocase);
        if (i >= 0) {
            for (p1 = u + 1, p2 = i + minlen + 1; p1 < len1 && p2 < len2;
		 p1++, p2++) {
                if (nocase) {
                    if (Tcl_UniCharToLower(str1[p1]) !=
                        Tcl_UniCharToLower(str2[p2])) break;
                } else {
                    if (str1[p1] != str2[p2]) break;
                }
	    }
            if (wordparse) {
                newt = t;
                if ((t > 0 && !Tcl_UniCharIsSpace(str1[t-1])) ||
			(i > 0 && !Tcl_UniCharIsSpace(str2[i-1]))) {
                    for (; newt < p1; newt++) {
                        if (Tcl_UniCharIsSpace(str1[newt])) break;
                    }
                }

                newp1 = p1 - 1;
                if ((p1 < len1 && !Tcl_UniCharIsSpace(str1[p1])) ||
			(p2 < len2 && !Tcl_UniCharIsSpace(str2[p2]))) {
                    for (; newp1 > newt; newp1--) {
                        if (Tcl_UniCharIsSpace(str1[newp1])) break;
                    }
                }
                newp1++;

                if (newp1 - newt > minlen) {
                    foundlen = newp1 - newt;
                    found1 = newt;
                    found2 = i + newt - t;
                    minlen = foundlen;
                    u = t + minlen;
                }
            } else {
                foundlen = p1 - t;
                found1 = t;
                found2 = i;
                minlen = foundlen;
                u = t + minlen;
            }
        }
    }

    if (foundlen < 0) {
      /* No common string found */
	Tcl_ListObjAppendElement(interp, res, obj1);
	Tcl_ListObjAppendElement(interp, res, obj2);
        return;
    }

    /* Handle left part, recursively */
    apa1 = Tcl_NewUnicodeObj(str1, found1);
    apa2 = Tcl_NewUnicodeObj(str2, found2);
    Tcl_IncrRefCount(apa1);
    Tcl_IncrRefCount(apa2);
    CompareMidString(interp, apa1, apa2, res, wordparse, nocase);
    Tcl_DecrRefCount(apa1);
    Tcl_DecrRefCount(apa2);

    /* Handle middle (common) part*/
    Tcl_ListObjAppendElement(interp, res,
	    Tcl_NewUnicodeObj(str1 + found1, foundlen));
    Tcl_ListObjAppendElement(interp, res,
	    Tcl_NewUnicodeObj(str2 + found2, foundlen));
    
    /* Handle right part, recursively*/
    apa1 = Tcl_NewUnicodeObj(str1 + found1 + foundlen, -1);
    apa2 = Tcl_NewUnicodeObj(str2 + found2 + foundlen, -1);
    Tcl_IncrRefCount(apa1);
    Tcl_IncrRefCount(apa2);
    CompareMidString(interp, apa1, apa2, res, wordparse, nocase);
    Tcl_DecrRefCount(apa1);
    Tcl_DecrRefCount(apa2);
}

int
DiffStringsObjCmd(dummy, interp, objc, objv)
    ClientData dummy;    	/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int index, t, result = TCL_OK;
    int nocase = 0;
    int ignore = 0, wordparse = 0;
    int len1, len2;
    Tcl_UniChar *line1, *line2, *s1, *s2, *e1, *e2;
    /*char *line1, *line2, *s1, *s2, *e1, *e2, *prev, *prev2;*/
    Tcl_UniChar *word1, *word2;
    int wordflag;
    Tcl_Obj *res, *mid1, *mid2;
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
    
    for (t = 1; t < objc - 2; t++) {
	if (Tcl_GetIndexFromObj(interp, objv[t], options, "option", 0,
		&index) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch (index) {
          case OPT_NOCASE :
          case OPT_I :
            nocase = 1;
            break;
	  case OPT_B:
	    ignore = 1;
	    break;
	  case OPT_W:
	    ignore = 2;
	    break;
	  case OPT_WORDS:
	    wordparse = 1;
	    break;
	}
    }

    line1 = Tcl_GetUnicodeFromObj(objv[objc-2], &len1);
    line2 = Tcl_GetUnicodeFromObj(objv[objc-1], &len2);
    
    s1 = line1;
    s2 = line2;
    e1 = line1 + len1;
    e2 = line2 + len2;

    /* Skip whitespace in both ends*/
    if (ignore > 0) {
	while (s1 < e1 && Tcl_UniCharIsSpace(*s1)) s1++;
	while (s2 < e2 && Tcl_UniCharIsSpace(*s2)) s2++;
	while (e1 > s1 && Tcl_UniCharIsSpace(*(e1-1))) e1--;
	while (e2 > s2 && Tcl_UniCharIsSpace(*(e2-1))) e2--;
    }
    
    /* Skip matching chars in both ends
     * Forwards */
    word1 = s1; word2 = s2;
    wordflag = 0;
    while (s1 < e1 && s2 < e2) {
	if (wordflag) {
	    word1 = s1;
	    word2 = s2;
	}
        if (nocase) {
            if (Tcl_UniCharToLower(*s1) != Tcl_UniCharToLower(*s2)) break;
        } else {
            if (*s1 != *s2) break;
        }
	if (wordparse) {
	    if (!Tcl_UniCharIsSpace(*s1)) {
		wordflag = 0;
	    } else {
		wordflag = 1;
		word1 = s1;
		word2 = s2;
	    }
	}
	s1++;
	s2++;
    }
    if (wordparse && s1 < e1 && s2 < e2) {
	s1 = word1;
	s2 = word2;
    }
    /* Backwards*/
    word1 = e1; word2 = e2;
    wordflag = 0;
    while (e1 > s1 && e2 > s2) {
	if (wordflag) {
	    word1 = e1;
	    word2 = e2;
	}
        if (nocase) {
            if (Tcl_UniCharToLower(*(e1 - 1)) != Tcl_UniCharToLower(*(e2 - 1)))
                break;
        } else {
            if (*(e1 - 1) != *(e2 - 1)) break;
        }
	if (wordparse) {
	    if (!Tcl_UniCharIsSpace(*(e1 - 1))) {
		wordflag = 0;
	    } else {
		wordflag = 1;
		word1 = e1;
		word2 = e2;
	    }
	}
	e1--;
	e2--;
    }
    if (wordparse) {
	e1 = word1;
	e2 = word2;
    }

    res = Tcl_NewListObj(0, NULL);
    Tcl_IncrRefCount(res);
    Tcl_ListObjAppendElement(interp, res, Tcl_NewUnicodeObj(line1, s1-line1));
    Tcl_ListObjAppendElement(interp, res, Tcl_NewUnicodeObj(line2, s2-line2));

    if (e1 > s1 || e2 > s2) {
	mid1 = Tcl_NewUnicodeObj(s1, e1 - s1);
	mid2 = Tcl_NewUnicodeObj(s2, e2 - s2);
	Tcl_IncrRefCount(mid1);
	Tcl_IncrRefCount(mid2);
	CompareMidString(interp, mid1, mid2, res, wordparse, nocase);
	Tcl_DecrRefCount(mid1);
	Tcl_DecrRefCount(mid2);

	Tcl_ListObjAppendElement(interp, res, Tcl_NewUnicodeObj(e1, -1));
	Tcl_ListObjAppendElement(interp, res, Tcl_NewUnicodeObj(e2, -1));
    }

    Tcl_SetObjResult(interp, res);
    Tcl_DecrRefCount(res);
    return result;
}

#define TCOC(name, func) \
Tcl_CreateObjCommand(interp, name, func, (ClientData) NULL, \
                     (Tcl_CmdDeleteProc *) NULL)

EXTERN int
Diffutil_Init(Tcl_Interp *interp)
{
    if (Tcl_InitStubs(interp, "8.4", 0) == NULL) {
	return TCL_ERROR;
    }
    if (Tcl_PkgRequire(interp, "Tcl", "8.4", 0) == NULL) {
	return TCL_ERROR;
    }
    if (Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION) != TCL_OK) {
	return TCL_ERROR;
    }

    TCOC("DiffUtil::compareFiles", CompareFilesObjCmd);
    TCOC("DiffUtil::compareStreams", CompareStreamsObjCmd);
    TCOC("DiffUtil::diffFiles", DiffFilesObjCmd);
    TCOC("DiffUtil::diffLists", DiffListsObjCmd);
    TCOC("DiffUtil::diffStrings", DiffStringsObjCmd);
    TCOC("DiffUtil::diffStrings2", DiffStrings2ObjCmd);
    Tcl_SetVar(interp, "DiffUtil::version", PACKAGE_VERSION, TCL_GLOBAL_ONLY);
    Tcl_SetVar(interp, "DiffUtil::implementation", "c", TCL_GLOBAL_ONLY);

    return TCL_OK;
}

EXTERN int
Diffutil_SafeInit(Tcl_Interp *interp)
{
    return TCL_OK;
}
