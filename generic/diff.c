/***********************************************************************
 *
 * This file implements the central LCS function for comparing things.
 *
 * Copyright (c) 2004, 2010-2012, Peter Spjuth
 *
 ***********************************************************************
 * References:
 *       J. W. Hunt and M. D. McIlroy, "An algorithm for differential
 *       file comparison," Comp. Sci. Tech. Rep. #41, Bell Telephone
 *       Laboratories (1976). Available on the Web at the second
 *       author's personal site: http://www.cs.dartmouth.edu/~doug/
 *
 ***********************************************************************/

#include <tcl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "diffutil.h"

/* A type for the parsing state */
typedef enum {
    IN_NONE, IN_SPACE, IN_NUMBER
} In_T;

#define max(a,b) ((a) > (b) ? (a) : (b))

/* A type to implement the Candidates in the LCS algorithm */
typedef struct Candidate_T {
    /* Line numbers in files */
    Line_T line1, line2;
    /* A score value to select between similar candidates */
    unsigned long score;
    /* Hash value for the line in the second file */
    Hash_T realhash;
    /* k-candidate */
    Line_T k;
#ifdef CANDIDATE_DEBUG
    Line_T wasK;
#endif
    /*
     * If this is a k-candidate, the prev pointer points to a
     * (k-1)-candidate that matches this one.
     */
    struct Candidate_T *prev;
    /*
     * If this is a k-candidate, the peer pointer points to another
     * k-candidate which is up/left of this one.
     */
    struct Candidate_T *peer;
} Candidate_T;

/*
 * Candidates are allocated in blocks to speed up handling and to
 * simplify freeing.
 */

/* Allocate in blocks of about 64k */
#define CANDIDATE_ALLOC ((65536-sizeof(int)-sizeof(struct CandidateAlloc_T *))\
                        /sizeof(Candidate_T))

typedef struct CandidateAlloc_T {
    int used;
#ifdef CANDIDATE_STATS
    int serial;
#endif
    struct CandidateAlloc_T *next;
    Candidate_T candidates[CANDIDATE_ALLOC];
} CandidateAlloc_T;

/* A dynamic list of lines */
#define LineListStaticAlloc_C 25
typedef struct LineInfo_T {
    Line_T line;
    Hash_T hash;
} LineInfo_T;
typedef struct LineList_T {
    LineInfo_T staticList[LineListStaticAlloc_C];
    LineInfo_T *Elems;
    unsigned long alloced, n;
} LineList_T;

static int       DiffOptsRegsub(Tcl_Interp *interp, Tcl_Obj *obj1Ptr,
                        Tcl_Obj *rePtr, Tcl_Obj *sub1Ptr,
                        Tcl_Obj **resultPtrPtr,
                        DiffOptions_T const *optsPtr);

static void
InitLineList(LineList_T *listPtr)
{
    listPtr->Elems = listPtr->staticList;
    listPtr->alloced = LineListStaticAlloc_C;
    listPtr->n = 0;
}

static void
AddToLineList(LineList_T *listPtr, Line_T Value,  Hash_T hash)
{
    if (listPtr->n >= listPtr->alloced) {
        /* Full, need to realloc */
        if (listPtr->Elems == listPtr->staticList) {
            listPtr->Elems = (LineInfo_T *)
                    ckalloc(sizeof(LineInfo_T) * listPtr->alloced * 2);
            memcpy(listPtr->Elems, listPtr->staticList,
                    LineListStaticAlloc_C * sizeof(LineInfo_T));
            listPtr->alloced *= 2;
        } else {
            listPtr->Elems = (LineInfo_T *)
                    ckrealloc((void *) listPtr->Elems,
                              sizeof(LineInfo_T) * listPtr->alloced * 2);
            listPtr->alloced *= 2;
        }
    }
    listPtr->Elems[listPtr->n].line = Value;
    listPtr->Elems[listPtr->n].hash = hash;
    listPtr->n++;
}

/*
 * A compare function to qsort a LineList.
 */
static int
CompareLine(const void *a1, const void *a2)
{
    const LineInfo_T *v1 = (LineInfo_T *) a1;
    const LineInfo_T *v2 = (LineInfo_T *) a2;
    if (v1->line < v2->line)
        return -1;
    else if (v1->line > v2->line)
        return 1;
    else
        return 0;
}

static void
SortLineList(LineList_T *listPtr)
{
    qsort(listPtr->Elems, listPtr->n, sizeof(LineInfo_T), CompareLine);
}

static void
ClearLineList(LineList_T *listPtr)
{
    listPtr->n = 0;
}

static void
FreeLineList(LineList_T *listPtr)
{
    if (listPtr->Elems != listPtr->staticList) {
        ckfree((void *) listPtr->Elems);
        listPtr->Elems = listPtr->staticList;
    }
    listPtr->alloced = LineListStaticAlloc_C;
    listPtr->n = 0;
}

/*
 * Check if an index pair fails to match due to alignment
 * Returns true if it fails.
 * This assumes the align list is sorted, as done by NormaliseOpts.
 */
static int
CheckAlign(const DiffOptions_T *optsPtr, Line_T i, Line_T j)
{
    int t;

    for (t = 0; t < optsPtr->alignLength; t += 2) {
        /* If both are below, it is ok since the list is sorted */
        if (i <  optsPtr->align[t] && j <  optsPtr->align[t + 1]) return 0;
        /* If aligned, it must be ok */
        if (i == optsPtr->align[t] && j == optsPtr->align[t + 1]) return 0;
        /* Fail if just one is below the align level */
        if (i <= optsPtr->align[t] || j <= optsPtr->align[t + 1]) return 1;
    }
    return 0;
}

/*
 * The hash algorithm is currently very simplistic and can probably
 * be replaced by something better without losing speed.
 * An empty line is assumed to have a hash value of 0.
 */
#define HASH_ADD(hash, character) hash += (hash << 7) + (character)

/*
 * Get a string from a Tcl object and compute the hash value for it.
 */
void
Hash(Tcl_Obj *objPtr,              /* Input Object */
     const DiffOptions_T *optsPtr, /* Options      */
     int left,                     /* Which side the string belongs to. */
     Hash_T *result,               /* Hash value   */
     Hash_T *real)                 /* Hash value when ignoring ignore */
{
    Hash_T hash;
    int i, length;
    char *string, *str;
    Tcl_UniChar c;
    Tcl_Obj *regsubPtr = left ?
            optsPtr->regsubLeftPtr : optsPtr->regsubRightPtr;

    Tcl_IncrRefCount(objPtr);
    if (regsubPtr != NULL) {
        int objc;
        Tcl_Obj **objv;
        Tcl_Obj *resultPtr = NULL;
        Tcl_ListObjGetElements(NULL, regsubPtr, &objc, &objv);
        for (i = 0; i < objc; i +=2) {
            /* Silently ignore errors from regsub */
            if (DiffOptsRegsub(NULL, objPtr, objv[i], objv[i+1], &resultPtr,
                               optsPtr) == TCL_OK) {
                Tcl_DecrRefCount(objPtr);
                objPtr = resultPtr;
            }
        }
    }
    string = Tcl_GetStringFromObj(objPtr, &length);

    /* Use the fast way when no ignore flag is used. */
    hash = 0;
    for (i = 0; i < length; i++) {
        HASH_ADD(hash, (unsigned char) string[i]);
    }
    *real = hash;
    if (optsPtr->ignore != 0) {
        const int ignoreAllSpace = (optsPtr->ignore & IGNORE_ALL_SPACE);
        const int ignoreSpace    = (optsPtr->ignore & IGNORE_SPACE_CHANGE);
        const int ignoreCase     = (optsPtr->ignore & IGNORE_CASE);
        const int ignoreNum      = (optsPtr->ignore & IGNORE_NUMBERS);
        /*
         * By starting in space, IGNORE_SPACE_CHANGE will ignore all
         * space in the beginning of a line.
         */
        In_T in = IN_SPACE;
        hash = 0;
        str = string;

        while (*str != 0) {
            str += Tcl_UtfToUniChar(str, &c);
            if (c == '\n') break;
            if (Tcl_UniCharIsSpace(c)) {
                if (ignoreAllSpace) continue;
                /* Any consecutive whitespace is regarded as a single space */
                if (ignoreSpace && in == IN_SPACE) continue;
                if (ignoreSpace)
                    c = ' ';
                in = IN_SPACE;
            } else if (ignoreNum && Tcl_UniCharIsDigit(c)) {
                if (in == IN_NUMBER) continue;
                /* A string of digits is replaced by a single 0 */
                c = '0';
                in = IN_NUMBER;
            } else {
                in = IN_NONE;
                if (ignoreCase) {
                    c = Tcl_UniCharToLower(c);
                }
            }
            HASH_ADD(hash, c);
        }
    }
    *result = hash;
    Tcl_DecrRefCount(objPtr);
    return;
}

/*
 * Compare two strings, ignoring things in the same way as hash does.
 * FIXA: Should be recoded to use Unicode functions.
 * Returns true if they differ.
 */
int
CompareObjects(Tcl_Obj *obj1Ptr,
               Tcl_Obj *obj2Ptr,
               const DiffOptions_T *optsPtr)
{
    int c1, c2, i1, i2, length1, length2, start;
    int i, result = 0;
    char *string1, *string2;
    const int ignoreAllSpace = (optsPtr->ignore & IGNORE_ALL_SPACE);
    const int ignoreSpace    = (optsPtr->ignore & IGNORE_SPACE_CHANGE);
    const int ignoreCase     = (optsPtr->ignore & IGNORE_CASE);
    const int ignoreNum      = (optsPtr->ignore & IGNORE_NUMBERS);

    Tcl_IncrRefCount(obj1Ptr);
    Tcl_IncrRefCount(obj2Ptr);
    if (optsPtr->regsubLeftPtr != NULL) {
        int objc;
        Tcl_Obj **objv;
        Tcl_Obj *resultPtr = NULL;

        Tcl_ListObjGetElements(NULL, optsPtr->regsubLeftPtr, &objc, &objv);

        for (i = 0; i < objc; i += 2) {
            /* Silently ignore errors from regsub */
            if (DiffOptsRegsub(NULL, obj1Ptr, objv[i], objv[i+1], &resultPtr,
                            optsPtr) == TCL_OK) {
                Tcl_DecrRefCount(obj1Ptr);
                obj1Ptr = resultPtr;
            }
        }
    }
    if (optsPtr->regsubRightPtr != NULL) {
        int objc;
        Tcl_Obj **objv;
        Tcl_Obj *resultPtr = NULL;

        Tcl_ListObjGetElements(NULL, optsPtr->regsubRightPtr, &objc, &objv);

        for (i = 0; i < objc; i += 2) {
            /* Silently ignore errors from regsub */
            if (DiffOptsRegsub(NULL, obj2Ptr, objv[i], objv[i+1], &resultPtr,
                            optsPtr) == TCL_OK) {
                Tcl_DecrRefCount(obj2Ptr);
                obj2Ptr = resultPtr;
            }
        }
    }
    string1 = Tcl_GetStringFromObj(obj1Ptr, &length1);
    string2 = Tcl_GetStringFromObj(obj2Ptr, &length2);

    /* Use the fast way when no ignore flag is used. */
    if (optsPtr->ignore == 0) {
        if (length1 != length2) {
            result = 1;
        } else {
            result = Tcl_UtfNcmp(string1, string2, length1);
        }
        goto cleanup;
    }

    i1 = i2 = 0;
    while (i1 < length1 && i2 < length2) {
        c1 = string1[i1];
        if (isspace(c1)) {
            if (ignoreAllSpace || ignoreSpace) {
                /* Scan up to non-space */
                start = i1;
                while (i1 < length1 && isspace(string1[i1])) i1++;
                if (ignoreAllSpace || start == 0) {
                    c1 = string1[i1];
                } else {
                    i1--;
                    c1 = ' ';
                }
            }
        }
        if (ignoreNum && isdigit(c1)) {
            /* Scan up to non-digit */
            while (i1 < length1 && isdigit(string1[i1])) i1++;
            i1--;
            c1 = '0';
        }
        if (ignoreCase && isupper(c1)) {
            c1 = tolower(c1);
        }

        c2 = string2[i2];
        if (isspace(c2)) {
            if (ignoreAllSpace || ignoreSpace) {
                /* Scan up to non-space */
                start = i2;
                while (i2 < length2 && isspace(string2[i2])) i2++;
                if (ignoreAllSpace || start == 0) {
                    c2 = string2[i2];
                } else {
                    i2--;
                    c2 = ' ';
                }
            }
        }
        if (ignoreNum && isdigit(c2)) {
            /* Scan up to non-digit */
            while (i2 < length2 && isdigit(string2[i2])) i2++;
            i2--;
            c2 = '0';
        }
        if (ignoreCase && isupper(c2)) {
            c2 = tolower(c2);
        }

        if (i1 >= length1 && i2 <  length2) { result = -1; goto cleanup; }
        if (i1 < length1  && i2 >= length2) { result =  1; goto cleanup; }
        if (c1 < c2) { result = -1; goto cleanup; }
        if (c1 > c2) { result =  1; goto cleanup; }
        i1++;
        i2++;
    }
    cleanup:
    Tcl_DecrRefCount(obj1Ptr);
    Tcl_DecrRefCount(obj2Ptr);
    return result;
}

/*
 * A compare function to qsort the V vector.
 * Sorts first on hash, then on serial number.
 */
int
CompareV(const void *a1, const void *a2)
{
    V_T const *v1 = (V_T *) a1;
    V_T const *v2 = (V_T *) a2;
    if (v1->hash < v2->hash)
        return -1;
    else if (v1->hash > v2->hash)
        return 1;
    else if (v1->serial < v2->serial)
        return -1;
    else if (v1->serial > v2->serial)
        return 1;
    else
        return 0;
}

/* Create a new candidate */
static Candidate_T *
NewCandidate(
    CandidateAlloc_T **first,
    Line_T a, Line_T b, Hash_T realhash,
    Candidate_T *prev, Candidate_T *peer)
{
    Candidate_T *cand;
    CandidateAlloc_T *candalloc;

    /* Allocate a new block if needed. */
    if (*first == NULL || (*first)->used >= CANDIDATE_ALLOC) {
        candalloc = (CandidateAlloc_T *) ckalloc(sizeof(CandidateAlloc_T));
        candalloc->used = 0;
#ifdef CANDIDATE_STATS
        if (*first != NULL) {
            candalloc->serial = (*first)->serial + 1;
        } else {
            candalloc->serial = 0;
        }
#endif
        candalloc->next = *first;
        *first = candalloc;
    } else {
        candalloc = *first;
    }
    /* Pick one from the block */
    cand = &candalloc->candidates[candalloc->used];
    candalloc->used++;

    cand->line1 = a;
    cand->line2 = b;
    cand->realhash = realhash;
    cand->prev = prev;
    cand->peer = peer;
    cand->score = 0;
#ifdef CANDIDATE_DEBUG
    cand->wasK = 0;
#endif
    if (prev == NULL) {
        cand->k = 0;
    } else {
        cand->k = prev->k + 1;
    }
    return cand;
}

/* Clean up all allocated candidates */
static void
FreeCandidates(CandidateAlloc_T **first) {
    CandidateAlloc_T *candalloc = *first, *next;
#ifdef CANDIDATE_STATS
    printf("Allocs %d * %d + %d = %d\n", (*first)->serial, CANDIDATE_ALLOC,
           (*first)->used, (*first)->serial * CANDIDATE_ALLOC+(*first)->used);
#endif
    while (candalloc != NULL) {
        next = candalloc->next;
        ckfree((char *) candalloc);
        candalloc = next;
    }
    *first = NULL;
}

#define ALLOW_SAME_COLUMN
#define SAME_COL_OPT
#define ALLOW_SAME_ROW
#define SAME_ROW_OPT
#define SAME_ROW_OPT2

/*
 * This implements the merge function from the LCS algorithm
 */
static void
merge(
    CandidateAlloc_T **firstCandidate,
    Candidate_T **K,
    Line_T *k,      /* Index to last used element in K */
    Line_T i,       /* Current index in file 1 */
    const P_T *P,   /* P vector */
    const E_T *E,   /* E vector */
    Line_T p,       /* Index in E of the file 2 class equivalent to line i */
    const DiffOptions_T *optsPtr,
    Line_T m,       /* Size of file 1 */
    Line_T n)       /* Size of file 2 */
{
    Candidate_T *c, *newc, *peer, *tmp;
    Line_T r, ck, j, b1 = 0, b2 = 0;
    Line_T first, last, s = 0;

    /*printf("Merge: k = %ld  i = %ld  p = %ld\n", *k, i, p);*/

    /*
     * Below, we deviate from Hunt/McIlroy's algorithm by allowing
     * extra candidates to get through.  These are candidates that can
     * not give longer sequences but might give "nicer" sequences
     * according to the scoring system we apply later.
     * Code that deviates are marked with "NonHM".
     */

    c = K[0];
    ck = 0; /* ck is where c is supposed to be stored. Following the
             * H/M algorithm ck will be equal to r.
             */
    r = 0;  /* r is the start of the search range */

    /*
     * The loop goes through all lines in file 2 that matches the current
     * line in file 1.
     * At the start, p points to the first in the equivalence class.
     */
    while (1) {
        j = E[p].serial; /* j is the current line from file 2 being checked */
        /* Skip this candidate if alignment forbids it */
        if (optsPtr->alignLength > 0 && CheckAlign(optsPtr, i ,j)) {
            if (E[p].last) break;
            p++;
            continue;
        }

        /*printf("p = %ld  j = %ld  r = %ld  s= %ld  k = %ld\n", p, j, r, s, *k);*/
        /*
         * Binary search in K from r to k.
         * K is ordered on its line2, and we want the place where j would fit.
         */
        first = r;
        last = *k;
        while (first <= last) {
            /*printf("First %ld  Last %ld\n", first, last);*/
            s = (first + last) / 2;
            b1 = K[s]->line2;
            b2 = K[s+1]->line2;
            if ((b1 < j && b2 > j) || b1 == j) {
                break;
            }
            if (b2 == j) {
                s = s + 1;
                b1 = K[s]->line2;
                break;
            }
            if (b2 < j) {
                first = s + 1;
            } else {
                if (s == 0) break;
                last = s - 1;
            }
        }

        /*
         * By now b1 is the line for K[s] and b2 is the line for K[s+1].
         * We know that, if possible, b1 <= j < b2.
         */

        /*printf("j = %ld  s = %ld  b1 = %ld  b2 = %ld\n", j, s, b1, b2);*/
        if (b1 < j && j < b2) {
            if (ck == s + 1 /*&& c->line1 == i*/) { /*NonHM*/
                /*
                 * If there already is a candidate for this level,
                 * create this candidate as a peer but do not update K.
                 */
                for (peer = c; peer->peer != NULL; peer = peer->peer) {
                    if (peer->peer->line1 != peer->line1) break;
                }
                newc = NewCandidate(firstCandidate, i, j, E[p].realhash,
                        c->prev, peer->peer);
                peer->peer = newc;
            } else {
                peer = K[s+1];
                if (s >= *k) {
                    /*printf("Set K%ld\n", *k+2);*/
                    K[*k+2] = K[*k+1];
                    (*k)++;
                    peer = NULL;
                }
                newc = NewCandidate(firstCandidate, i, j, E[p].realhash, K[s],
                        peer);
#ifdef CANDIDATE_DEBUG
                newc->wasK = s + 1;
#endif
                K[ck] = c;
                c = newc;
                ck = s + 1;
#ifdef ALLOW_SAME_COLUMN /*NonHM*/
#ifdef SAME_COL_OPT
                /*
                 * In the H/M algorithm, another k-candidate is not allowed
                 * in the same column as a previous.  This is since further
                 * k-candidates cannot give a longer sequence.  We want some
                 * more matches so we keep k as lower boundary in this column.
                 *
                 * If c is "optimally" placed, we can skip a lot by narrowing
                 * the search space for the next iteration by not allowing
                 * any more k-candidates in this column (Thus following H/M).
                 * c is optimal if it's next to its previous candidate,
                 * but not if it has a peer in the same column.
                 * And not if the previous line is empty.
                 * Not if this candidate is not exactly equal.
                 */

                if (c->prev != NULL                     &&
                    c->k > 1                            &&
                    c->prev->realhash != 0              &&
                    P[c->line1].realhash == c->realhash &&
                    (c->line1 - c->prev->line1) <= 1    &&
                    (c->line2 - c->prev->line2) <= 1    &&
                    (c->prev->peer == NULL ||
                     c->prev->peer->line1 < c->prev->line1)) {
                    /* Optimal */
                    r = s + 1;
                } else {
                    r = s;
                }
#else  /* SAME_COL_OPT */
                r = s;
#endif /* SAME_COL_OPT */
#else  /* ALLOW_SAME_COLUMN */
                r = s + 1;
#endif /* ALLOW_SAME_COLUMN */
            }
#ifdef ALLOW_SAME_ROW /*NonHM*/
        } else if (b1 == j) {
            /*
             * We have a new candidate on the same row as one of the
             * candidates in K.
             */
            if (ck == s /*&& c->line1 == i*/) {
                /*
                 * If there already is a candidate for this level, i.e. if
                 * there is a s-candidate below us and K[s] is about to be
                 * updated, create this candidate as a peer but do not update K.
                 */
                newc = NewCandidate(firstCandidate, i, j, E[p].realhash,
                        c->prev, c->peer);
                c->peer = newc;
            } else {
#ifdef SAME_ROW_OPT2
                /*
                 * If this candidate is not optimally placed and if K[s] is
                 * optimally placed, we skip this candidate.
                 * It is optimal if it's next to its previous candidate.
                 * Not if the previous candidate is an empty line.
                 * Not if this candidate is not exactly equal.
                 */
                register int ksoptimal =
                    (s > 1                                     &&
                     K[s]->prev != NULL                        &&
                     K[s]->prev->realhash != 0                 &&
                     P[K[s]->line1].realhash == K[s]->realhash &&
                     (K[s]->line1 - K[s]->prev->line1) <= 1    &&
                     (K[s]->line2 - K[s]->prev->line2) <= 1);
                if (!ksoptimal ||
                    ((i - K[s-1]->line1) <= 1 && (j - K[s-1]->line2) <= 1)) {
#endif /* SAME_ROW_OPT2 */
#ifdef SAME_ROW_OPT
                    if ((m - i) + s >= *k) {
#endif /* SAME_ROW_OPT */
                        /*
                         * Search through s-1 candidates for a fitting one
                         * to be "prev".
                         */
                        tmp = K[s-1];
                        while (tmp != NULL) {
                            if (tmp->line1 < i && tmp->line2 < j) break;
                            tmp = tmp->peer;
                        }
                        newc = NewCandidate(firstCandidate, i, j, E[p].realhash,
                                tmp, K[s]);
#ifdef CANDIDATE_DEBUG
                        newc->wasK = s;
#endif
                        r = s;
                        K[ck] = c;
                        ck = s;
                        c = newc;
#ifdef SAME_ROW_OPT
                    }
#endif /* SAME_ROW_OPT */
#ifdef SAME_ROW_OPT2
                }
#endif /* SAME_ROW_OPT2 */
            }
#endif /* ALLOW_SAME_ROW */
        }

        if (E[p].last) break;
        p++;
    }
    K[ck] = c;
}

/*
 * Give score to a candidate.
 */
#if defined(__hpux)||defined(_AIX)||defined(_WIN32)
#define inline
#endif
static inline void
ScoreCandidate(Candidate_T *c, const P_T *P)
{
    Candidate_T *prev, *bestc;
    long score, bestscore;

    bestscore = 1000000000;
    bestc = c->prev;

    for (prev = c->prev; prev != NULL; prev = prev->peer) {
        if (prev->line2 >= c->line2) break;
        score = prev->score;

        /* A jump increases score, unless the previous line is empty */
        if (c->k > 1 && prev->realhash != 0) {
            if ((c->line2 - prev->line2) > 1) score += 2;
            if ((c->line1 - prev->line1) > 1) score += 2;
            if ((c->line2 - prev->line2) > 1 &&
                (c->line1 - prev->line1) > 1) score--;
        }
        /*
         * By doing less than or equal we favor matches earlier
         * in the file.
         */
        if (score < bestscore ||
            (score == bestscore && bestc->line2 == prev->line2)) {
            /*printf("A %ld B %ld S %ld   Best A %ld B %ld S %ld\n",
                   prev->line1 , prev->line2, score,
                   bestc->line1, bestc->line2, bestscore);*/
            bestscore = score;
            bestc = prev;
        }
    }

    c->score = bestscore;
    /* If the lines differ, it's worse */
    if (P[c->line1].realhash != c->realhash) {
        c->score += 5;
    }
    /*
     * Redirect the prev pointer to the best score.
     * This means that the best path will follow the prev
     * pointers and will be easy to pick up in the end.
     */
    c->prev = bestc;
}

/*
 * Go through candidates and score them to select a good match.
 * A k-candidate's score includes the score of the (k-1)-candidate
 * below it.  Thus the score for a candidate is the score for the
 * entire chain below it.
 */
static void
ScoreCandidates(Line_T k, Candidate_T **K, const P_T *P)
{
    Line_T sp;
    Candidate_T *cand, *prev;
    Candidate_T **stack;
    int ready;

    /*
     * Do a depth-first search through the candidate tree.
     * This is currently not very efficienty implemented but this
     * stage takes a rather small amount of work compared with the
     * rest of the LCS algorithm.
     */

    /*
     * A score of 0 means the Score has not been calculated yet.
     * By setting the score to 1 in the lowest node, all scores
     * will be >= 1.
     */

    K[0]->score = 1;

    /* A candidate stack */
    if (k == 0) {
        return;
    }
    /* Calculate initial stack size needed */
    sp = 0;
    for (cand = K[k]; cand != NULL; cand = cand->peer) {
        sp++;
    }
    /* k*20 is a guess. I've seen k*13 be needed for tricky cases */
    int cStackSize = max(sp*2, k*20);
    stack = (Candidate_T **) ckalloc(cStackSize * sizeof(Candidate_T *));
    sp = 0;

    /* Start at the top, put all end points on the stack */
    for (cand = K[k]; cand != NULL; cand = cand->peer) {
        stack[sp++] = cand;
    }
    while (sp > 0) {
        cand = stack[sp - 1];
        /* Already scored? */
        if (cand->score != 0) {
            sp--;
            continue;
        }
        ready = 1;
        for (prev = cand->prev; prev != NULL; prev = prev->peer) {
            if (prev->line2 >= cand->line2) break;
            if (prev->score == 0) {
                stack[sp++] = prev;
                ready = 0;
                if (sp >= cStackSize) {
                    /* Out of stack, get more */
                    /*printf("Debug: Out of stack in ScoreCandidates k = %ld\n", k);*/
                    cStackSize *= 2;
                    stack = (Candidate_T **) ckrealloc((char *) stack,
                             cStackSize * sizeof(Candidate_T *));
                }
            }
        }
        if (ready) {
            /* All previous have a score, we can score this one */
            ScoreCandidate(cand, P);
            sp--;
        }
    }

    ckfree((char *) stack);
}

/*
 * Helper to process forbidden lines.
 * Checks if two Lines are possible to match.
 */
static int
IsLineMatch(
    const LineInfo_T *Elem1,
    const LineInfo_T *Elem2,
    const DiffOptions_T *optsPtr)

{
    Line_T line1 = Elem1->line;
    Line_T line2 = Elem2->line;
    Hash_T hash1 = Elem1->hash;
    Hash_T hash2 = Elem2->hash;
    if (hash1 == hash2 &&
        !CheckAlign(optsPtr, line1, line2)) {
        return 1;
    }
    return 0;
}

/*
 * Inner step of PostProcessForbidden.
 * A change block with forbidden lines in both sides has been found.
 * Look for matches that can be marked in the J vector.
 */
static void
PostProcessForbiddenBlock(
    Line_T * const J,        /* J vector */
    Line_T firstI,           /* First line in left change block */
    Line_T lastI,            /* First line in left change block */
    Line_T firstJ,           /* First line in right change block */
    Line_T lastJ,            /* First line in right change block */
    const LineList_T *iList, /* List of lines in left side (sorted) */
    const LineList_T *jList, /* List of lines in right side (sorted) */
    const DiffOptions_T *optsPtr)
{
    Line_T i, j;

    /*
     * Case: a single line to the left.
     * Scan right side for a match.
     */
    if (iList->n == 1) {
        for (j = 0; j < jList->n; j++) {
            Line_T line1 = iList->Elems[0].line;
            Line_T line2 = jList->Elems[j].line;
            if (IsLineMatch(&iList->Elems[0], &jList->Elems[j], optsPtr)) {
                J[line1] = line2;
                return;
            }
        }
        return;
    }

    /*
     * Case: a single line to the right.
     * Scan left side for a match.
     */
    if (jList->n == 1) {
        for (i = 0; i < iList->n; i++) {
            Line_T line1 = iList->Elems[i].line;
            Line_T line2 = jList->Elems[0].line;
            if (IsLineMatch(&iList->Elems[i], &jList->Elems[0], optsPtr)) {
                J[line1] = line2;
                return;
            }
        }
        return;
    }

    /*
     * Just do a raw sequential matching of forbidden lines.
     * This produces a reasonable, if non-optimal, result.
     * FIXA: Better algorithm here...
     * In principle this matching should be a rerun of LCS on the block, with
     * forbidden lines allowed. It might be reasonable to reuse the current P
     * and E vectors and call LcsCoreInner in some way.
     */ 
    for (j = 0; j < iList->n && j < jList->n; j++) {
        Line_T line1 = iList->Elems[j].line;
        Line_T line2 = jList->Elems[j].line;
        if (IsLineMatch(&iList->Elems[j], &jList->Elems[j], optsPtr)) {
            J[line1] = line2;
        }
    }
}
        
/*
 * We have ignored forbidden lines before which means that there
 * may be more lines that can be matched.
 */
static void
PostProcessForbidden(
    const Line_T m,          /* Size of file 1 */
    const Line_T n,          /* Size of file 2 */
    const P_T * const P,     /* P vector */
    const E_T * const E,     /* E vector */
    Line_T * const J,        /* J vector */
    const DiffOptions_T *optsPtr)
{
    /* lastLine tracks the last matching line, so the line after is the start
     * of a change block. */
    Line_T lastLine1 = 0, lastLine2 = 0;
    Line_T i, j, firstJ, lastJ;
    LineList_T iList, jList;

    InitLineList(&iList);
    InitLineList(&jList);

    for (i = 1; i <= (m + 1); i++) {
        if (i > m || J[i] != 0) {
            /* We are at the end or at a matching line, thus a change block
             * has ended. */
            if (iList.n > 0) {
                /*
                 * We have forbidden lines in the left part of this change
                 * block and thus must look in the right part too.
                 */

                /* Figure out the range in the right file */
                firstJ = lastLine2 + 1;
                lastJ = i > m ? n : J[i] - 1;
                
                for (j = 1; j <= n; j++) {
                    if (E[j].serial >= firstJ && E[j].serial <= lastJ) {
                        /* Line is within range. Is it forbidden? */
                        if (E[j].forbidden) {
                            AddToLineList(&jList, E[j].serial, E[j].hash);
                        }
                    }
                }

                if (jList.n > 0) {
                    /*
                     * We have forbidden lines in both parts of this change
                     * block. Sort J list and deal with it.
                     */

                    SortLineList(&jList);
                    PostProcessForbiddenBlock(J, lastLine1 + 1, i - 1,
                                              firstJ, lastJ,
                                              &iList, &jList, optsPtr);

                    /*printf("Handled forbidden. L %ld-%ld (%ld) R %ld-%ld (%ld)\n", lastLine1 + 1, i-1, iList.n, lastLine2 + 1, lastJ, jList.n);*/
                }
            }
            lastLine1 = i;
            lastLine2 = J[i];
            ClearLineList(&iList);
            ClearLineList(&jList);
            continue;
        }
        if (P[i].forbidden) {
            /* Make a list of all forbidden lines in this change block. */
            AddToLineList(&iList, i, P[i].hash);
            continue;
        }
    }
    
    FreeLineList(&iList);
    FreeLineList(&jList);
    /*
     * This could be left to block matching if that is improved
     * to handle equal lines within change blocks.
     */
}

/*
 * Mark a line in the left side (P vector) as forbidden.
 * Also mark the corresponding equivalence class on the right side (E vector).
 */
static void
ForbidP(Line_T i, P_T *P, E_T *E)
{
    Line_T j;
    P[i].forbidden = 1;
    j = P[i].Eindex;
    while (!E[j].forbidden) {
        E[j].forbidden = 1;
        if (E[j].last) break;
        j++;
    }
}


/*
 * The core part of the LCS algorithm.
 * It is independent of data since it only works on hashes.
 *
 * This is the inner part of it, which do not meddle with forbidden lines.
 * It respects them, but do not add or clean up any forbidden lines.
 * 
 * Returns the J vector as a ckalloc:ed array.
 */
Line_T *
LcsCoreInner(
    Tcl_Interp *interp,
    Line_T m,      /* number of elements in first sequence */
    Line_T n,      /* number of elements in second sequence */
    const P_T *P,  /* The P vector [0,m] corresponds to lines in "file 1" */
    const E_T *E,  /* The E vector [0,n] corresponds to lines in "file 2" */
    const DiffOptions_T *optsPtr,
    int *anyForbidden) /* Out parameter, was any forbidden lines skipped? */
{
    Candidate_T **K, *c;
    Line_T i, k, *J;
    /* Keep track of all candidates to free them easily */
    CandidateAlloc_T *candidates = NULL;
    
    *anyForbidden = 0;

    /*printf("Doing K\n"); */

    /* Initialise K candidate vector */
    K = (Candidate_T **) ckalloc(sizeof(Candidate_T *) * ((m < n ? m : n) + 2));

    /* k is the last meaningful element of K */
    K[0] = NewCandidate(&candidates, 0, 0, 0, NULL, NULL);
    k = 0;

    /* Add a fence outside the used range of K */
    K[1] = NewCandidate(&candidates, m + 1, n + 1, 0, NULL, NULL);

    /*
     * For each line in file 1, if it matches any line in file 2,
     * merge it into the set of candidates.
     */

    for (i = 1; i <= m; i++) {
        if (P[i].Eindex != 0) {
            if (P[i].forbidden) {
                *anyForbidden = 1;
            } else {
                /*printf("Merge i %ld  Pi %ld\n", i , P[i]);*/
                merge(&candidates, K, &k, i, P, E, P[i].Eindex, optsPtr, m, n);
            }
        }
    }

    /*printf("Doing Score k = %ld\n", k); */
    ScoreCandidates(k, K, P);

    /* Debug, dump candidates to a variable */
#ifdef CANDIDATE_DEBUG
    {
        Tcl_DString ds;
        char buf[40];
        Candidate_T *peer;
        CandidateAlloc_T *ca;

        Tcl_DStringInit(&ds);
        for (i = k; i > 0; i--) {
            c = K[i];
            sprintf(buf, "K %ld %ld %ld 0 0 0 0 0  ",
                    (long) c->line1, (long) c->line2, (long) i);
            Tcl_DStringAppend(&ds, buf, -1);
        }

        ca = candidates;
        while (ca != NULL) {
            for (i = 0; i < ca->used; i++) {
                c = &ca->candidates[i];
                if (c->line1 <= 0 || c->line1 > m ||
                    c->line2 <= 0 || c->line2 > n) {
                    continue;
                }
                sprintf(buf, "C %ld %ld %ld %ld ",
                        (long) c->line1, (long) c->line2, (long) c->score,
                        (long) c->wasK);
                Tcl_DStringAppend(&ds, buf, -1);
                peer = c->peer;
                if (peer != NULL) {
                    sprintf(buf, "%ld %ld ", (long) peer->line1,
                            (long) peer->line2);
                } else {
                    sprintf(buf, "%ld %ld ", m + 1, n + 1);
                }
                Tcl_DStringAppend(&ds, buf, -1);
                if (c->prev != NULL) {
                    sprintf(buf, "%ld %ld ", (long) c->prev->line1,
                            (long) c->prev->line2);
                } else {
                    sprintf(buf, "%d %d ", 0, 0);
                }
                Tcl_DStringAppend(&ds, buf, -1);
            }
            ca = ca->next;
        }

        Tcl_SetVar(interp, "DiffUtil::Candidates", Tcl_DStringValue(&ds), TCL_GLOBAL_ONLY);
        Tcl_DStringFree(&ds);
    }
#endif /* CANDIDATE_DEBUG */

    /* Wrap up result */

    /*printf("Doing J\n");*/
    J = (Line_T *) ckalloc((m + 1) * sizeof(Line_T));
    for (i = 0; i <= m; i++) {
        J[i] = 0;
    }

    /*
     * K[k] lists the possible end points of length k, i.e. the longest
     * common subsequences.  If there is more than one, check which is
     * best.
     */

    c = K[k];
    if (c->peer != NULL) {
        Candidate_T *bestc;
        Line_T primscore, secscore, score2, bestps, bestss;
        /*
         * Check the candidates' score first. if they are equal, use a
         * secondary score where the best is the one where the distances
         * to start or end of file is the same in both files.
         */
        bestc = c;
        bestps = 1000000000;
        bestss = 1000000000;
        while (c != NULL) {
            primscore = c->score;
            secscore  = labs(((long) m - (long) c->line1) -
                             ((long) n - (long) c->line2));
            score2 = labs((long) c->line1 - (long) c->line2);
            if (score2 < secscore) secscore = score2;
            if (P[c->line1].realhash != c->realhash) {
                /* Worse score if lines differ */
                secscore += 100;
            }
            if (primscore < bestps ||
                (primscore == bestps && secscore < bestss)) {
                bestps = primscore;
                bestss = secscore;
                bestc = c;
            }
            c = c->peer;
        }
        c = bestc;
    }

    /*
     * Traverse the chain from the selected K[k] candidate and fill
     * in the resulting J vector.
     */

    while (c != NULL) {
        /* Sanity check */
        if (c->line1 < 0 || c->line1 > m) {
            Tcl_Panic("Bad line number when constructing J vector");
        }
        J[c->line1] = c->line2;
        c = c->prev;
    }

    /*printf("Clean up Candidates and K\n");*/
    FreeCandidates(&candidates);
    ckfree((char *) K);

    return J;
}

/*
 * The core part of the LCS algorithm.
 * It is independent of data since it only works on hashes.
 *
 * Returns the J vector as a ckalloc:ed array.
 * J is a [0,m] vector, i.e. it has one element per line in "file 1".
 * If J[i] is non-zero, line i in "file 1" matches line J[i] in "file 2".
 *
 * m - number of elements in first sequence
 * n - number of elements in second sequence
 * P - The P vector [0,m] corresponds to lines in "file 1"
 * E - The E vector [0,n] corresponds to lines in "file 2"
 */
Line_T *
LcsCore(
    Tcl_Interp *interp,
    Line_T m, Line_T n,
    P_T *P, E_T *E,
    const DiffOptions_T *optsPtr)
{
    Line_T i, *J;
    int anyForbidden;
    
    for (i = 1; i <= m; i++) {
        if (P[i].Eindex != 0) {
            if (optsPtr->noempty && P[i].hash == 0) {
                /* Uphold the -noempty rule by forbidding those connections */
                ForbidP(i, P, E);
            }
            if (E[P[i].Eindex].count > optsPtr->pivot) {
                /* Experiment to forbid large equivalence classes */
                ForbidP(i, P, E);
            }
        }
    }

    J = LcsCoreInner(interp, m, n, P, E, optsPtr, &anyForbidden);

    if (anyForbidden) {
        /*
         * We have ignored forbidden lines before which means that there
         * may be more lines that can be matched.
         */

        PostProcessForbidden(m, n, P, E, J, optsPtr);
    }
    return J;
}

/*
 * Build E vector from V vector.
 *
 * Returns the ckalloc:ed E vector.
 */
E_T *
BuildEVector(const V_T *V, Line_T n)
{
    Line_T j, first;
    E_T *E;

    E = (E_T *) ckalloc((n + 1) * sizeof(E_T));
    E[0].serial = 0;
    E[0].last = 1;
    E[0].count = 0;
    E[0].forbidden = 1;
    first = 1;
    for (j = 1; j <= n; j++) {
        E[j].serial   = V[j].serial;
        E[j].hash     = V[j].hash;
        E[j].realhash = V[j].realhash;
        E[j].forbidden = 0;
        E[j].count = 0;
        E[first].count++;

        if (j == n) {
            E[j].last = 1;
        } else {
            if (V[j].hash != V[j+1].hash) {
                E[j].last = 1;
                first = j + 1;
            } else {
                E[j].last = 0;
            }
        }
    }
    return E;
}

/* Binary search for hash in V */
Line_T
BSearchVVector(const V_T *V, Line_T n, Hash_T h)
{
    Line_T first = 1;
    Line_T last = n;
    Line_T j = 1;
    while (first <= last) {
        j = (first + last) / 2;
        if (V[j].hash == h) break;
        if (V[j].hash < h) {
            first = j + 1;
        } else {
            last = j - 1;
        }
    }
    return j;
}

/* Allocate the type of chunk that is the result of the diff functions */
Tcl_Obj *
NewChunk(Tcl_Interp *interp, const DiffOptions_T *optsPtr,
         Line_T start1, Line_T n1, Line_T start2, Line_T n2)
{
    Tcl_Obj *subPtr = Tcl_NewListObj(0, NULL);
    start1 += (optsPtr->rFrom1 - 1);
    start2 += (optsPtr->rFrom2 - 1);
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj((long) start1));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj((long) n1));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj((long) start2));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj((long) n2));
    return subPtr;
}

/* Add a chunk to a result list */
void
AppendChunk(
    Tcl_Interp *interp, Tcl_Obj *listPtr,
    const DiffOptions_T *optsPtr,
    Line_T start1, Line_T n1,
    Line_T start2, Line_T n2)
{
    int t;

    /* If an alignment happens within a changed chunk, it should be split */
    for (t = 0; t < optsPtr->alignLength; t += 2) {
        int lMatch = start1 <= optsPtr->align[t] &&
                optsPtr->align[t] < (start1 + n1);
        int rMatch = start2 <= optsPtr->align[t + 1] &&
                optsPtr->align[t + 1] < (start2 + n2);
        if (lMatch && rMatch) {
            /* This aligned pair is within the chunk */
            int preN1 = optsPtr->align[t]     - start1;
            int preN2 = optsPtr->align[t + 1] - start2;
            if (preN1 > 0 || preN2 > 0) {
                /* Chunk before the align */
                Tcl_ListObjAppendElement(interp, listPtr,
                        NewChunk(interp, optsPtr,
                                start1, preN1, start2, preN2));
            }
            /* Chunk for the aligned rows */
            Tcl_ListObjAppendElement(interp, listPtr,
                    NewChunk(interp, optsPtr,
                            optsPtr->align[t], 1, optsPtr->align[t + 1], 1));
            /* Adjust block to contain the remains */
            start1 = optsPtr->align[t]     + 1;
            start2 = optsPtr->align[t + 1] + 1;
            n1 -= preN1 + 1;
            n2 -= preN2 + 1;
        }
    }
    /* Alignment could leave us with an empty chunk */
    if (n1 > 0 || n2 > 0) {
        Tcl_ListObjAppendElement(interp, listPtr,
                NewChunk(interp, optsPtr, start1, n1, start2, n2));
    }
}

/*
 * Given a valid J vector,
 * generate a list of insert/delete/change operations.
 */

static Tcl_Obj *
BuildResultFromJDiffStyle(
    Tcl_Interp *interp, const DiffOptions_T *optsPtr,
    Line_T m, Line_T n, const Line_T *J)
{
    Tcl_Obj *resPtr;
    Line_T current1, current2, n1, n2;
    Line_T startBlock1, startBlock2;

    resPtr = Tcl_NewListObj(0, NULL);
    startBlock1 = startBlock2 = 1;
    current1 = current2 = 0;

    /* If any side is empty, there is nothing to scan */
    if (m > 0 && n > 0) {
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
            if (J[current1] != current2) continue;

            n1 = current1 - startBlock1;
            n2 = current2 - startBlock2;
            if (n1 > 0 || n2 > 0) {
                AppendChunk(interp, resPtr, optsPtr,
                            startBlock1, n1, startBlock2, n2);
            }
            startBlock1 = current1 + 1;
            startBlock2 = current2 + 1;
        }
    }
    /* Scrape up the last */
    n1 = m - startBlock1 + 1;
    n2 = n - startBlock2 + 1;
    if (n1 > 0 || n2 > 0) {
        AppendChunk(interp, resPtr, optsPtr,
                startBlock1, n1, startBlock2, n2);
    }
    return resPtr;
}

static Tcl_Obj *
BuildResultFromJMatchStyle(
    Tcl_Interp *interp, const DiffOptions_T *optsPtr,
    Line_T m, Line_T n, const Line_T *J)
{
    Tcl_Obj *resPtr, *leftPtr, *rightPtr;
    Line_T current1, current2;

    resPtr = Tcl_NewListObj(0, NULL);
    leftPtr = Tcl_NewListObj(0, NULL);
    rightPtr = Tcl_NewListObj(0, NULL);

    Tcl_ListObjAppendElement(interp, resPtr, leftPtr);
    Tcl_ListObjAppendElement(interp, resPtr, rightPtr);

    current1 = current2 = 0;

    while (current1 < m && current2 < n) {
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
        if (J[current1] != current2) continue;

        Tcl_ListObjAppendElement(interp, leftPtr,
                Tcl_NewLongObj(current1 + (optsPtr->rFrom1 - 1)));
        Tcl_ListObjAppendElement(interp, rightPtr,
                Tcl_NewLongObj(current2 + (optsPtr->rFrom2 - 1)));
    }
    return resPtr;
}

Tcl_Obj *
BuildResultFromJ(
    Tcl_Interp *interp, const DiffOptions_T *optsPtr,
    Line_T m, Line_T n, const Line_T *J)
{
    if (optsPtr->resultStyle == Result_Diff) {
        return BuildResultFromJDiffStyle(interp, optsPtr, m, n, J);
    }
    return BuildResultFromJMatchStyle(interp, optsPtr, m, n, J);
}

/* Fill in the range option from a Tcl Value */
int
SetOptsRange(
    Tcl_Interp *interp, Tcl_Obj *rangePtr, int first,
    DiffOptions_T *optsPtr)
{
    int listLen, i;
    long values[4];
    Tcl_Obj **elemPtrs;
    if (Tcl_ListObjGetElements(interp, rangePtr, &listLen, &elemPtrs)
        != TCL_OK) {
        return TCL_ERROR;
    }

    if (listLen != 4 && listLen != 0) {
        Tcl_SetResult(interp, "bad range", TCL_STATIC);
        return TCL_ERROR;
    }
    if (listLen == 0) {
        optsPtr->rFrom1 = optsPtr->rFrom2 = 1;
        optsPtr->rTo1   = optsPtr->rTo2   = 0;
        return TCL_OK;
    }

    for (i = 0; i < 4; i++) {
        if (Tcl_GetLongFromObj(interp, elemPtrs[i], &values[i]) != TCL_OK) {
            return TCL_ERROR;
        }
        values[i] -= (first - 1);
        if (values[i] < 1) {
            Tcl_SetResult(interp, "bad range", TCL_STATIC);
            return TCL_ERROR;
        }
    }

    optsPtr->rFrom1 = values[0];
    optsPtr->rTo1   = values[1];
    optsPtr->rFrom2 = values[2];
    optsPtr->rTo2   = values[3];
    return TCL_OK;
}

/* Fill in the align option from a Tcl Value */
int
SetOptsAlign(Tcl_Interp *interp,
             Tcl_Obj *alignPtr,
             int first,
             DiffOptions_T *optsPtr)
{
    int listLen, i, change;
    long value;
    Line_T tmp;
    Tcl_Obj **elemPtrs;
    if (Tcl_ListObjGetElements(interp, alignPtr, &listLen, &elemPtrs)
        != TCL_OK) {
        return TCL_ERROR;
    }

    if ((listLen % 2) != 0) {
        /* FIXA error message */
        Tcl_SetResult(interp, "bad align", TCL_STATIC);
        return TCL_ERROR;
    }

    if (optsPtr->alignLength > STATIC_ALIGN) {
        ckfree((char *) optsPtr->align);
    }
    if (listLen <= STATIC_ALIGN) {
        optsPtr->align = optsPtr->staticAlign;
    } else {
        optsPtr->align = (Line_T *) ckalloc(sizeof(Line_T) * listLen);
    }
    optsPtr->alignLength = listLen;

    for (i = 0; i < listLen; i++) {
        if (Tcl_GetLongFromObj(interp, elemPtrs[i], &value) != TCL_OK) {
            return TCL_ERROR;
        }
        value -= (first - 1);
        if (value < 1) {
            Tcl_SetResult(interp, "bad align", TCL_STATIC);
            return TCL_ERROR;
        }
        optsPtr->align[i] = value;
    }

    /* Sort the align pairs (bubble sort) */
    if (optsPtr->alignLength > 2) {
        change = 1;
        while (change) {
            change = 0;
            for (i = 0; i < (optsPtr->alignLength - 2); i += 2) {
                if (optsPtr->align[i] > optsPtr->align[i + 2] ||
                    (optsPtr->align[i] == optsPtr->align[i + 2] &&
                     optsPtr->align[i+1] > optsPtr->align[i + 2])) {
                    /* Swap */
                    tmp                   = optsPtr->align[i];
                    optsPtr->align[i]     = optsPtr->align[i + 2];
                    optsPtr->align[i + 2] = tmp;
                    tmp                   = optsPtr->align[i + 1];
                    optsPtr->align[i + 1] = optsPtr->align[i + 3];
                    optsPtr->align[i + 3] = tmp;
                    change = 1;
                }
            }
        }
    }

    return TCL_OK;
}

/* Tidy up a DiffOptions structure before it is used */
void
NormaliseOpts(DiffOptions_T *optsPtr)
{
    int i;
    Line_T prev1, prev2;

    /*
     * If there is both a range and align, move the alignment
     * to index from 1 withing the range.
     */

    if (optsPtr->rFrom1 > 1) {
        for (i = 0; i < optsPtr->alignLength; i += 2) {
            if (optsPtr->align[i] >= optsPtr->rFrom1) {
                optsPtr->align[i] -= (optsPtr->rFrom1 - 1);
            } else {
                optsPtr->align[i] = 0;
            }
        }
    }
    if (optsPtr->rFrom2 > 1) {
        for (i = 1; i < optsPtr->alignLength; i += 2) {
            if (optsPtr->align[i] >= optsPtr->rFrom2) {
                optsPtr->align[i] -= (optsPtr->rFrom2 - 1);
            } else {
                optsPtr->align[i] = 0;
            }
        }
    }

    /*
     * Check for contradictions in align
     */
    prev1 = prev2 = 0;
    for (i = 0; i < optsPtr->alignLength; i += 2) {
        /*
         * Ignore contradicting by making them duplicate the previous.
         */
        if (optsPtr->align[i] <= prev1 || optsPtr->align[i + 1] <= prev2) {
            optsPtr->align[i]     = prev1;
            optsPtr->align[i + 1] = prev2;
        }
        prev1 = optsPtr->align[i];
        prev2 = optsPtr->align[i + 1];
    }
}

/*
 * Do a regsub on an object.
 * Mostly copied from Tcl_RegsubObjCmd.
 */
static int
DiffOptsRegsub(
    Tcl_Interp *interp,           /* Current interpreter. */
    Tcl_Obj *obj1Ptr,             /* Input object. */
    Tcl_Obj *rePtr,               /* Regexp object. */
    Tcl_Obj *sub1Ptr,             /* Substitution. */
    Tcl_Obj **resultPtrPtr,       /* Store result, if successful. */ 
    const DiffOptions_T *optsPtr) /* Options. */ 
{
    int idx, result, cflags, all, wlen, wsublen, numMatches, offset;
    int start, end, subStart, subEnd, match;
    Tcl_RegExp regExpr;
    Tcl_RegExpInfo info;
    Tcl_Obj *resultPtr, *objPtr, *subPtr;
    Tcl_UniChar ch, *wsrc, *wfirstChar, *wstring, *wsubspec, *wend;

    cflags = TCL_REG_ADVANCED;
    all = 1;
    offset = 0;
    resultPtr = NULL;
    if (optsPtr->ignore & IGNORE_CASE) {
        cflags |= TCL_REG_NOCASE;
    }

    if ((strpbrk(Tcl_GetString(sub1Ptr), "&\\") == NULL)
            && (strpbrk(Tcl_GetString(rePtr), "*+?{}()[].\\|^$") == NULL)) {
        /*
         * This is a simple one pair string map situation. We make use of a
         * slightly modified version of the one pair STR_MAP code.
         */

        int slen, nocase;
        int (*strCmpFn)(const Tcl_UniChar*,const Tcl_UniChar*,unsigned long);
        Tcl_UniChar *p, wsrclc;

        numMatches = 0;
        nocase = (cflags & TCL_REG_NOCASE);
        strCmpFn = nocase ? Tcl_UniCharNcasecmp : Tcl_UniCharNcmp;

        wsrc = Tcl_GetUnicodeFromObj(rePtr, &slen);
        wstring = Tcl_GetUnicodeFromObj(obj1Ptr, &wlen);
        wsubspec = Tcl_GetUnicodeFromObj(sub1Ptr, &wsublen);
        wend = wstring + wlen - (slen ? slen - 1 : 0);
        result = TCL_OK;

        if (slen == 0) {
            /*
             * regsub behavior for "" matches between each character. 'string
             * map' skips the "" case.
             */

            if (wstring < wend) {
                resultPtr = Tcl_NewUnicodeObj(wstring, 0);
                Tcl_IncrRefCount(resultPtr);
                for (; wstring < wend; wstring++) {
                    Tcl_AppendUnicodeToObj(resultPtr, wsubspec, wsublen);
                    Tcl_AppendUnicodeToObj(resultPtr, wstring, 1);
                    numMatches++;
                }
                wlen = 0;
            }
        } else {
            wsrclc = Tcl_UniCharToLower(*wsrc);
            for (p = wfirstChar = wstring; wstring < wend; wstring++) {
                if ((*wstring == *wsrc ||
                        (nocase && Tcl_UniCharToLower(*wstring)==wsrclc)) &&
                        (slen==1 || (strCmpFn(wstring, wsrc,
                                (unsigned long) slen) == 0))) {
                    if (numMatches == 0) {
                        resultPtr = Tcl_NewUnicodeObj(wstring, 0);
                        Tcl_IncrRefCount(resultPtr);
                    }
                    if (p != wstring) {
                        Tcl_AppendUnicodeToObj(resultPtr, p, wstring - p);
                        p = wstring + slen;
                    } else {
                        p += slen;
                    }
                    wstring = p - 1;

                    Tcl_AppendUnicodeToObj(resultPtr, wsubspec, wsublen);
                    numMatches++;
                }
            }
            if (numMatches) {
                wlen    = wfirstChar + wlen - p;
                wstring = p;
            }
        }
        objPtr = NULL;
        subPtr = NULL;
        goto regsubDone;
    }

    regExpr = Tcl_GetRegExpFromObj(interp, rePtr, cflags);
    if (regExpr == NULL) {
        return TCL_ERROR;
    }

    /*
     * Make sure to avoid problems where the objects are shared. This can
     * cause RegExpObj <> UnicodeObj shimmering that causes data corruption.
     * [Bug #461322]
     */

    if (obj1Ptr == rePtr) {
        objPtr = Tcl_DuplicateObj(obj1Ptr);
    } else {
        objPtr = obj1Ptr;
    }
    wstring = Tcl_GetUnicodeFromObj(objPtr, &wlen);
    if (sub1Ptr == rePtr) {
        subPtr = Tcl_DuplicateObj(sub1Ptr);
    } else {
        subPtr = sub1Ptr;
    }
    wsubspec = Tcl_GetUnicodeFromObj(subPtr, &wsublen);

    result = TCL_OK;

    /*
     * The following loop is to handle multiple matches within the same source
     * string; each iteration handles one match and its corresponding
     * substitution. If "-all" hasn't been specified then the loop body only
     * gets executed once. We must use 'offset <= wlen' in particular for the
     * case where the regexp pattern can match the empty string - this is
     * useful when doing, say, 'regsub -- ^ $str ...' when $str might be
     * empty.
     */

    numMatches = 0;
    for ( ; offset <= wlen; ) {

        /*
         * The flags argument is set if string is part of a larger string, so
         * that "^" won't match.
         */

        match = Tcl_RegExpExecObj(interp, regExpr, objPtr, offset,
                10 /* matches */, ((offset > 0 &&
                (wstring[offset-1] != (Tcl_UniChar)'\n'))
                ? TCL_REG_NOTBOL : 0));

        if (match < 0) {
            result = TCL_ERROR;
            goto done;
        }
        if (match == 0) {
            break;
        }
        if (numMatches == 0) {
            resultPtr = Tcl_NewUnicodeObj(wstring, 0);
            Tcl_IncrRefCount(resultPtr);
            if (offset > 0) {
                /*
                 * Copy the initial portion of the string in if an offset was
                 * specified.
                 */

                Tcl_AppendUnicodeToObj(resultPtr, wstring, offset);
            }
        }
        numMatches++;

        /*
         * Copy the portion of the source string before the match to the
         * result variable.
         */

        Tcl_RegExpGetInfo(regExpr, &info);
        start = info.matches[0].start;
        end = info.matches[0].end;
        Tcl_AppendUnicodeToObj(resultPtr, wstring + offset, start);

        /*
         * Append the subSpec argument to the variable, making appropriate
         * substitutions. This code is a bit hairy because of the backslash
         * conventions and because the code saves up ranges of characters in
         * subSpec to reduce the number of calls to Tcl_SetVar.
         */

        wsrc = wfirstChar = wsubspec;
        wend = wsubspec + wsublen;
        for (ch = *wsrc; wsrc != wend; wsrc++, ch = *wsrc) {
            if (ch == '&') {
                idx = 0;
            } else if (ch == '\\') {
                ch = wsrc[1];
                if ((ch >= '0') && (ch <= '9')) {
                    idx = ch - '0';
                } else if ((ch == '\\') || (ch == '&')) {
                    *wsrc = ch;
                    Tcl_AppendUnicodeToObj(resultPtr, wfirstChar,
                            wsrc - wfirstChar + 1);
                    *wsrc = '\\';
                    wfirstChar = wsrc + 2;
                    wsrc++;
                    continue;
                } else {
                    continue;
                }
            } else {
                continue;
            }

            if (wfirstChar != wsrc) {
                Tcl_AppendUnicodeToObj(resultPtr, wfirstChar,
                        wsrc - wfirstChar);
            }

            if (idx <= info.nsubs) {
                subStart = info.matches[idx].start;
                subEnd = info.matches[idx].end;
                if ((subStart >= 0) && (subEnd >= 0)) {
                    Tcl_AppendUnicodeToObj(resultPtr,
                            wstring + offset + subStart, subEnd - subStart);
                }
            }

            if (*wsrc == '\\') {
                wsrc++;
            }
            wfirstChar = wsrc + 1;
        }

        if (wfirstChar != wsrc) {
            Tcl_AppendUnicodeToObj(resultPtr, wfirstChar, wsrc - wfirstChar);
        }

        if (end == 0) {
            /*
             * Always consume at least one character of the input string in
             * order to prevent infinite loops.
             */

            if (offset < wlen) {
                Tcl_AppendUnicodeToObj(resultPtr, wstring + offset, 1);
            }
            offset++;
        } else {
            offset += end;
            if (start == end) {
                /*
                 * We matched an empty string, which means we must go forward
                 * one more step so we don't match again at the same spot.
                 */

                if (offset < wlen) {
                    Tcl_AppendUnicodeToObj(resultPtr, wstring + offset, 1);
                }
                offset++;
            }
        }
        if (!all) {
            break;
        }
    }

    /*
     * Copy the portion of the source string after the last match to the
     * result variable.
     */

  regsubDone:
    if (numMatches == 0) {
        /*
         * On zero matches, just ignore the offset, since it shouldn't matter
         * to us in this case, and the user may have skewed it.
         */

        resultPtr = obj1Ptr;
        Tcl_IncrRefCount(resultPtr);
    } else if (offset < wlen) {
        Tcl_AppendUnicodeToObj(resultPtr, wstring + offset, wlen - offset);
    }
    *resultPtrPtr = resultPtr;
    Tcl_IncrRefCount(resultPtr);

  done:
    if (objPtr && (obj1Ptr == rePtr)) {
        Tcl_DecrRefCount(objPtr);
    }
    if (subPtr && (sub1Ptr == rePtr)) {
        Tcl_DecrRefCount(subPtr);
    }
    if (resultPtr) {
        Tcl_DecrRefCount(resultPtr);
    }
    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
