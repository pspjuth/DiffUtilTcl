#include <tcl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "diffutil.h"

#define DEBUG 1

/* A type to hold hashing values */
typedef unsigned long Hash_T;

/* A type to hold line numbers */
typedef unsigned long Line_T;

/* Hold all options for diffing in a common struct */
#define STATIC_ALIGN 10
typedef struct {
    /* Ignore flags */
    int ignore;
    /* Range */
    Line_T rFrom1, rTo1, rFrom2, rTo2;
    /* Alignment */
    int alignLength;
    Line_T *align;
    Line_T staticAlign[STATIC_ALIGN];
} DiffOptions_T;

/* Flags in DiffOptions_T's ignore field */

#define IGNORE_ALL_SPACE 1
#define IGNORE_SPACE_CHANGE 2
#define IGNORE_CASE 4
#define IGNORE_KEYWORD 8

/* A type to implement the V vector in the LCS algorithm */
typedef struct V_T {
    Line_T serial;
    int chars;
    Hash_T hash;
} V_T;

/* A type to implement the E vector in the LCS algorithm */
typedef struct E_T {
    Line_T serial;
    int last;
} E_T;

/* A type to implement the Candidates in the LCS algorithm */
typedef struct Candidate_T {
    Line_T a, b;
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
    /* 
     * The next pointer is used for housekeeping.
     * By linking all allocated candidates it is easy to deallocate
     * them in the end.
     */
    struct Candidate_T *next;
} Candidate_T;


/* Check if an index pair fails to match due to alignment */
static int
CheckAlign(DiffOptions_T *optsPtr, Line_T i, Line_T j)
{
    int t;
    
    for (t = 0; t < optsPtr->alignLength; t += 2) {
        if (i <  optsPtr->align[t] && j <  optsPtr->align[t + 1]) return 0;
        if (i == optsPtr->align[t] && j == optsPtr->align[t + 1]) return 0;
        if (i <= optsPtr->align[t] || j <= optsPtr->align[t + 1]) return 1;
    }
    return 0;
}

/*
 * Get a string from a Tcl object and compute the hash value for it.
 */
static int
hash(Tcl_Obj *objPtr,         /* Input Object */
     DiffOptions_T *optsPtr,  /* Options      */
     Hash_T *res,             /* Hash value   */
     int *chars)              /* Number of chars read */
{
    Hash_T h = 0;
    int c, i, length, nc = 0;
    char *string;
    string = Tcl_GetStringFromObj(objPtr, &length);

    /* Use the fast way when no ignore flag is used. */
    if (optsPtr->ignore == 0) {
        for (i = 0; i < length; i++) {
            h += (h << 3) + string[i];
        }
        nc = length;
    } else {
        const int ignoreallspace = (optsPtr->ignore & IGNORE_ALL_SPACE);
        const int ignorespace    = (optsPtr->ignore & IGNORE_SPACE_CHANGE);
        const int ignorecase     = (optsPtr->ignore & IGNORE_CASE);
        /*const int ignorekey      = (optsPtr->ignore & IGNORE_KEYWORD);*/
        int inspace = 1;

        for (i = 0; i < length; i++) {
            c = string[i];
            if (c == '\n') break;
            if (isspace(c)) {
                if (ignoreallspace) continue;
                /* Any consecutive whitespace is regarded as a single space */
                if (ignorespace && inspace) continue;
                if (ignorespace) c = ' ';
                inspace = 1;
            } else {
                inspace = 0;
                if (ignorecase && isupper(c)) {
                    c = tolower(c);
                }
            }
            h += (h << 3) + c;
            nc++;
        }
    }
    *res = h;
    *chars = nc;
    return 0;
}

/* Compare two strings, ignoring things in the same way as hash does. */
static int
CompareObjects(Tcl_Obj *obj1Ptr,
               Tcl_Obj *obj2Ptr,
               DiffOptions_T *optsPtr)
{
    int c1, c2, i1, i2, length1, length2;
    char *string1, *string2;
    const int ignoreallspace = (optsPtr->ignore & IGNORE_ALL_SPACE);
    const int ignorespace    = (optsPtr->ignore & IGNORE_SPACE_CHANGE);
    const int ignorecase     = (optsPtr->ignore & IGNORE_CASE);
    /*const int ignorekey      = (optsPtr->ignore & IGNORE_KEYWORD);*/
    string1 = Tcl_GetStringFromObj(obj1Ptr, &length1);
    string2 = Tcl_GetStringFromObj(obj2Ptr, &length2);

    /* Use the fast way when no ignore flag is used. */
    if (optsPtr->ignore == 0) {
        return strcmp(string1, string2);
    }

    i1 = i2 = 0;
    while (i1 < length1 && i2 < length2) {
        c1 = string1[i1];
        if (isspace(c1)) {
            if (ignoreallspace || ignorespace) {
                /* Scan up to non-space */
                while (i1 < length1 && isspace(string1[i1])) i1++;
                if (ignorespace) {
                    i1--;
                    c1 = ' ';
                } else {
                    c1 = string1[i1];
                }
            }
        }
        if (ignorecase && isupper(c1)) {
            c1 = tolower(c1);
        }
        c2 = string2[i2];
        if (isspace(c2)) {
            if (ignoreallspace || ignorespace) {
                /* Scan up to non-space */
                while (i2 < length2 && isspace(string2[i2])) i2++;
                if (ignorespace) {
                    i2--;
                    c2 = ' ';
                } else {
                    c2 = string2[i2];
                }
            }
        }
        if (ignorecase && isupper(c2)) {
            c2 = tolower(c2);
        }
        if (i1 >= length1 && i2 <  length2) return -1;
        if (i1 < length1  && i2 >= length2) return  1;
        if (c1 < c2) return -1;
        if (c1 > c2) return -1;
        i1++;
        i2++;
    }
    return 0;
}

/* A compare function to qsort the V vector */
int
compareV(const void *a1, const void *a2) {
    V_T *v1 = (V_T *) a1;
    V_T *v2 = (V_T *) a2;
    if (v1->hash < v2->hash)
        return -1;
    else if (v1->hash > v2->hash)
        return 1;
    else if (v1->serial > v2->serial)
        /* Sort decreasing on the line number */
        return -1;
    else if (v1->serial < v2->serial)
        return 1;
    else
        return 0;
}

/* Create a new candidate */
static Candidate_T *
NewCandidate(Candidate_T **first, Line_T a, Line_T b,
             Candidate_T *prev, Candidate_T *peer) {
    Candidate_T *c = (Candidate_T *) ckalloc(sizeof(Candidate_T));
    c->a = a;
    c->b = b;
    c->prev = prev;
    c->peer = peer;
    c->next = *first;
    *first = c;
    return c;
}

/* Clean up all allocated candidates */
static void
FreeCandidates(Candidate_T **first) {
    Candidate_T *c = *first, *n;
    while (c != NULL) {
        n = c->next;
        ckfree((char *) c);
        c = n;
    }
    *first = NULL;
}

/*
 * This implements the merge function from the LCS algorithm
 */
static void
merge(Candidate_T **firstCandidate,
      Candidate_T **K,
      Line_T *k,
      Line_T i,
      E_T *E,
      Line_T p, 
      DiffOptions_T *optsPtr) {
    Candidate_T *c, *peer;
    Line_T r = *k, j, b1 = 0, b2 = 0;
    Line_T first, last, s = 0;

    /*printf("Merge: k = %ld  i = %ld  p = %ld\n", *k, i, p);*/
    
    while (1) {
        j = E[p].serial;
        /* Skip this candidate if alignment forbids it */
        if (optsPtr->alignLength > 0 && CheckAlign(optsPtr, i ,j)) {
            if (E[p].last) break;
            p++;
            continue;
        }

        /*printf("p = %ld  j = %ld  r = %ld  s= %ld  k = %ld\n", p, j, r, s, *k);*/
        /* Binary search */
        first = 0;
        last = r;
        while (first <= last) {
            /*printf("First %ld  Last %ld\n", first, last);*/
            s = (first + last) / 2;
            b1 = K[s]->b;
            b2 = K[s+1]->b;
            if ((b1 < j && b2 > j) || b1 == j) {
                break;
            }
            if (b2 == j) {
                s = s + 1;
                b1 = K[s]->b;
                break;
            }
            if (b2 < j) {
                first = s + 1;
            } else {
                if (s == 0) break;
                last = s - 1;
            }
        }
        /*printf("j = %ld  s = %ld  b1 = %ld  b2 = %ld\n", j, s, b1, b2);*/
        if (b1 < j && b2 > j) {
            peer = K[s+1];
            if (s >= *k) {
                /*printf("Set K%ld\n", *k+2);*/
                K[*k+2] = K[*k+1];
                (*k)++;
                peer = NULL;
            }
            /*printf("Set K%ld to (%ld,%ld)->(%ld,%ld)\n", r, c->a, c->b, c->prev == NULL ? 0: c->prev->a, c->prev == NULL ? 0: c->prev->b);*/
            K[s+1] = NewCandidate(firstCandidate, i, j, K[s], peer);
            r = s;
        } else if (b1 == j) {
            /* Search through s-1 candidates for a fitting one. */
            c = K[s-1];
            while (c != NULL) {
                if (c->a < i && c->b < j) break;
                c = c->peer;
            }
            K[s] = NewCandidate(firstCandidate, i, j, c, K[s]);
        }

        if (E[p].last) break;
        p++;
    }
}

/*
 * The core part of the LCS algorithm.
 * It is independent of data since it only works on hashes.
 *
 * Returns a ckalloc:ed array.
 * m - number of elements in first sequence
 * n - number of elements in second sequence
 * P - vector [0,m] of integers
 * E - vector [0,n]
 */
static Line_T *
LcsCore(Tcl_Interp *interp,
        Line_T m, Line_T n, Line_T *P, E_T *E,
        DiffOptions_T *optsPtr)
{
    Candidate_T **K, *c;
    Line_T i, k, *J;
    /* Keep track of all candidates to free them easily */
    Candidate_T *candidates = NULL;

    /* Find LCS */
    /*printf("Doing K\n");*/
    K = (Candidate_T **) ckalloc(sizeof(Candidate_T *) * ((m < n ? m : n) + 2));
    K[0] = NewCandidate(&candidates, 0, 0, NULL, NULL);
    K[1] = NewCandidate(&candidates, m + 1, n + 1, NULL, NULL);
    k = 0;

    for (i = 1; i <= m; i++) {
        if (P[i] != 0) {
            /*printf("Merge i %ld  Pi %ld\n", i , P[i]);*/
            merge(&candidates, K, &k, i, E, P[i], optsPtr);
        }
    }

    /* Debug, dump candidates to a variable */
#ifdef DEBUG
    {
        Tcl_DString ds;
        char buf[40];
        Candidate_T *peer;

        Tcl_DStringInit(&ds);
        for (i = k; i > 0; i--) {
            c = K[i];
            sprintf(buf, "K %ld %ld %ld 0 0 0  ",
                    (long) c->a, (long) c->b, (long) i);
            Tcl_DStringAppend(&ds, buf, -1);
        }
        c = candidates;
        while (c != NULL) {
            if (c->a <= 0 || c->a > m || c->b <= 0 || c->b > n) {
                c = c->next;
                continue;
            }
            sprintf(buf, "C %ld %ld ", (long) c->a, (long) c->b);
            Tcl_DStringAppend(&ds, buf, -1);
            peer = c->peer;
            if (peer != NULL) {
                sprintf(buf, "%ld %ld ", (long) peer->a, (long) peer->b);
            } else {
                sprintf(buf, "%ld %ld ", m + 1, n + 1);
            }
            Tcl_DStringAppend(&ds, buf, -1);
            if (c->prev != NULL) {
                sprintf(buf, "%ld %ld ", (long) c->prev->a, (long) c->prev->b);
            } else {
                sprintf(buf, "%d %d ", 0, 0);
            }
            Tcl_DStringAppend(&ds, buf, -1);
            
            c = c->next;
        }
    
        Tcl_SetVar(interp, "DiffUtil::Candidates", Tcl_DStringValue(&ds), TCL_GLOBAL_ONLY);
        Tcl_DStringFree(&ds);
    }
#endif /* DEBUG */
    
    /* Wrap up result */

    /*printf("Doing J\n");*/
    J = (Line_T *) ckalloc((m + 1) * sizeof(Line_T));
    for (i = 0; i <= m; i++) {
        J[i] = 0;
    }
    c = K[k];
    /* Are there more than one possible end point? */
    if (c->peer != NULL) {
        Candidate_T *bestc;
        Line_T score, score2, bests;
        /*
         * The best is the one where the distances to start or end of file
         * is the same in both files.
         */
        bestc = c;
        bests = 1000000000;
        while (c != NULL) {
            score  = labs(((long) m - (long) c->a) - ((long) n - (long) c->b));
            score2 = labs((long) c->a - (long) c->b);
            if (score2 < score) score = score2;
            if (score < bests) {
                bests = score;
                bestc = c;
            }
            c = c->peer;
        }
        c = bestc;
    }
    while (c != NULL) {
        if (c->a < 0 || c->a > m) printf("GURKA\n");
        J[c->a] = c->b;
        /* FIXA: check for alternative routes */
        c = c->prev;
    }

    /*printf("Clean up Candidates and K\n");*/
    FreeCandidates(&candidates);
    ckfree((char *) K);
    return J;
}

/*
 * Read two files, hash them and prepare the datastructures needed in LCS.
 */
static int
ReadAndHashFiles(Tcl_Interp *interp, char *name1, char *name2,
                 DiffOptions_T *optsPtr,
                 Line_T *mPtr, Line_T *nPtr,
                 Line_T **PPtr, E_T **EPtr)
{
    int result = TCL_OK;
    V_T *V = NULL;
    E_T *E = NULL;
    struct stat buf1, buf2;
    Line_T line, j, m = 0, n = 0, h, *P = NULL;
    Line_T allocedV, allocedP, first, last;
    int chars;
    Tcl_Channel ch;
    Tcl_Obj *linePtr;

    /* Stat files first to quickly see if they don't exist */
    if (stat(name1, &buf1) != 0) {
        /* FIXA: error message */
        Tcl_SetResult(interp, "bad file", TCL_STATIC);
        return TCL_ERROR;
    }
    if (stat(name2, &buf2) != 0) {
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

    /* Read file 2 and calculate hashes for each line */
    ch = Tcl_OpenFileChannel(interp, name2, "r", 0);
    if (ch == NULL) {
        result = TCL_ERROR;
        goto cleanup;
    }

    line = 1;
    if (optsPtr->rFrom2 > 1) {
        while (line < optsPtr->rFrom2) {
            /* Skip the first lines */
            Tcl_SetObjLength(linePtr, 0);
            if (Tcl_GetsObj(ch, linePtr) < 0) break;
            line++;
        }
    }

    n = 1;
    while (1) {
        V[n].serial = n;
        V[n].chars = 0;
        Tcl_SetObjLength(linePtr, 0);
        if (Tcl_GetsObj(ch, linePtr) < 0) {
            n--;
            break;
        }

        hash(linePtr, optsPtr, &V[n].hash, &V[n].chars);
        if (optsPtr->rTo2 > 0 && optsPtr->rTo2 <= line) break;

        n++;
        line++;
        if (n >= allocedV) {
            allocedV = allocedV * 3 / 2;
            V = (V_T *) ckrealloc((char *) V, allocedV * sizeof(V_T));
        }
    }
    Tcl_Close(interp, ch);

    /* If the last line is empty, disregard it */
    /*if (V[n].chars == 0) n--;*/

    /*for (j = 1; j <= n; j++) {
        printf("Line %ld  Hash %ld  Chars %ld\n", V[j].serial, V[j].hash, V[j].chars);
        }*/

    /* Sort V on hash/serial. */
    qsort(&V[1], (unsigned long) n, sizeof(V_T), compareV);

    /* Build E vector */
    E = (E_T *) ckalloc((n + 1) * sizeof(E_T));
    E[0].serial = 0;
    E[0].last = 1;
    for (j = 1; j <= n; j++) {
        E[j].serial = V[j].serial;
        if (j == n) {
            E[j].last = 1;
        } else {
            if (V[j].hash != V[j+1].hash) {
                E[j].last = 1;
            } else {
                E[j].last = 0;
            }
        }
    }

    /* Build P vector */

    /* Guess the number of lines in name1 for an inital allocation of P */
    allocedP = buf1.st_size / 40;
    /* If the guess is low, alloc some more to be safe. */
    if (allocedP < 10000) allocedP = 10000;
    P = (Line_T *) ckalloc(allocedP * sizeof(Line_T));

    /* Read file and calculate hashes for each line */
    ch = Tcl_OpenFileChannel(interp, name1, "r", 0);
    if (ch == NULL) {
        result = TCL_ERROR;
        goto cleanup;
    }

    line = 1;
    if (optsPtr->rFrom1 > 1) {
        while (line < optsPtr->rFrom1) {
            /* Skip the first lines */
            Tcl_SetObjLength(linePtr, 0);
            if (Tcl_GetsObj(ch, linePtr) < 0) break;
            line++;
        }
    }

    m = 1;
    while (1) {
        P[m] = 0;
        Tcl_SetObjLength(linePtr, 0);
        if (Tcl_GetsObj(ch, linePtr) < 0) {
            m--;
            break;
        }
        hash(linePtr, optsPtr, &h, &chars);

        /* Binary search for hash in V */
        first = 1;
        last = n;
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
            while (!E[j-1].last) j--;
            P[m] = j;
            /*printf("P %ld = %ld\n", m, j);*/
        }

        if (optsPtr->rTo1 > 0 && optsPtr->rTo1 <= line) break;

        m++;
        line++;
        if (m >= allocedP) {
            allocedP = allocedP * 3 / 2;
            P = (Line_T *) ckrealloc((char *) P,
                                   allocedP * sizeof(Line_T));
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

static Tcl_Obj *
NewChunk(Tcl_Interp *interp, DiffOptions_T *optsPtr,
         Line_T start1, Line_T n1, Line_T start2, Line_T n2) {
    Tcl_Obj *subPtr = Tcl_NewListObj(0, NULL);
    start1 += (optsPtr->rFrom1 - 1);
    start2 += (optsPtr->rFrom2 - 1);
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj((long) start1));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj((long) n1));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj((long) start2));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj((long) n2));
    return subPtr;
}

static int
CompareFiles(Tcl_Interp *interp, char *name1, char *name2,
             DiffOptions_T *optsPtr,
             Tcl_Obj **resPtr)
{
    E_T *E;
    Line_T m, n, *P, *J;
    Tcl_Obj *subPtr;

    /*printf("Doing ReadAndHash\n");*/
    if (ReadAndHashFiles(interp, name1, name2, optsPtr, &m, &n, &P, &E)
        != TCL_OK) {
        return TCL_ERROR;
    }
    /*printf("Doing LcsCore m = %ld, n = %ld\n", m, n);*/
    J = LcsCore(interp, m, n, P, E, optsPtr);
    /*printf("Done LcsCore\n");*/
    if (0) {
        int i;
        for (i = 0; i <= m; i++) {
            printf("J(%d)=%ld ", i, J[i]);
        }
        printf("\n");
    }

    ckfree((char *) E);
    ckfree((char *) P);

    /*
     * Now we have a list of matching lines in J.  We need to go through
     * the files and check that matching lines really are matching.
     * At the same time we generate a list of insert/delete/change opers.
     */

    *resPtr = Tcl_NewListObj(0, NULL);
    /* Take care of trivial cases first */
    if ((m == 0 && n > 0) || (m > 0 && n == 0)) {
        Tcl_ListObjAppendElement(interp, *resPtr,
                                 NewChunk(interp, optsPtr, 1, m, 1, n));
    } else if (m > 0 && n > 0) {
        Tcl_Channel ch1, ch2;
        Tcl_Obj *line1Ptr, *line2Ptr;
        Line_T current1, current2, n1, n2;
        Line_T startblock1, startblock2;

        /* Initialize objects to use as line buffers. */
        line1Ptr = Tcl_NewObj();
        Tcl_IncrRefCount(line1Ptr);
        Tcl_SetObjLength(line1Ptr, 1000);
        line2Ptr = Tcl_NewObj();
        Tcl_IncrRefCount(line2Ptr);
        Tcl_SetObjLength(line2Ptr, 1000);

        ch1 = Tcl_OpenFileChannel(interp, name1, "r", 0);
        ch2 = Tcl_OpenFileChannel(interp, name2, "r", 0);

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

        startblock1 = startblock2 = 1;
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
            /*printf("Compare %d (%ld) to %d\n", current1, J[current1],
              current2);*/
            if (J[current1] != current2) continue;
            if (CompareObjects(line1Ptr, line2Ptr, optsPtr) != 0) {
                /* No match, continue until next. */
                printf("NOT Match\n");
                continue;
            }

            n1 = current1 - startblock1;
            n2 = current2 - startblock2;
            if (n1 > 0 || n2 > 0) {
                subPtr = NewChunk(interp, optsPtr,
                                  startblock1, n1, startblock2, n2);
                Tcl_ListObjAppendElement(interp, *resPtr, subPtr);
            }
            startblock1 = current1 + 1;
            startblock2 = current2 + 1;
        }
        /* Scrape up the last */
        n1 = m - startblock1 + 1;
        n2 = n - startblock2 + 1;
        if (n1 > 0 || n2 > 0) {
            subPtr = NewChunk(interp, optsPtr,
                              startblock1, n1, startblock2, n2);
            Tcl_ListObjAppendElement(interp, *resPtr, subPtr);
        }
        Tcl_Close(interp, ch1);
        Tcl_Close(interp, ch2);
        Tcl_DecrRefCount(line1Ptr);
        Tcl_DecrRefCount(line2Ptr);
    }

    ckfree((char *) J);
    return TCL_OK;
}

/* Fill in the range option from a Tcl Value */
static int
SetOptsRange(Tcl_Interp *interp, Tcl_Obj *rangePtr, int first,
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
static int
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
    
    /* Sort the align pairs */
    if (optsPtr->alignLength > 2) {
        change = 1;
        while (change) {
            change = 0;
            for (i = 0; i < (optsPtr->alignLength - 2); i += 2) {
                if (optsPtr->align[i] > optsPtr->align[i + 2] ||
                    (optsPtr->align[i] == optsPtr->align[i + 2] &&
                     optsPtr->align[i+1] > optsPtr->align[i + 2])) {
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
static void
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


int
DiffFilesObjCmd(dummy, interp, objc, objv)
    ClientData dummy;    	/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int index, t, result = TCL_OK;
    Tcl_Obj *resPtr;
    char *file1, *file2;
    DiffOptions_T opts;
    static CONST char *options[] = {
	"-b", "-w", "-i", "-nocase", "-align", "-range", (char *) NULL
    };
    enum options {
	OPT_B, OPT_W, OPT_I, OPT_NOCASE, OPT_ALIGN, OPT_RANGE
    };

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
	return TCL_ERROR;
    }

    opts.ignore = 0;
    opts.rFrom1 = opts.rFrom2 = 1;
    opts.rTo1   = opts.rTo2   = 0;
    opts.alignLength = 0;

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
	}
    }
    NormaliseOpts(&opts);
    file1 = Tcl_GetString(objv[objc-2]);
    file2 = Tcl_GetString(objv[objc-1]);

    if (CompareFiles(interp, file1, file2, &opts, &resPtr) != TCL_OK) {
        result = TCL_ERROR;
        goto cleanup;
    }
    Tcl_SetObjResult(interp, resPtr);

    cleanup:
    if (opts.alignLength > STATIC_ALIGN) {
        ckfree((char *) opts.align);
    }

    return result;
}
