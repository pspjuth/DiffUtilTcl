#include <tcl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "diffutil.h"

#define IGNORE_ALL_SPACE 1
#define IGNORE_SPACE_CHANGE 2
#define IGNORE_CASE 4
#define IGNORE_KEYWORD 8

typedef unsigned long Hash_T;

typedef struct V_T {
    long serial;
    long chars;
    Hash_T hash;
} V_T;

typedef struct E_T {
    long serial;
    int last;
} E_T;

typedef struct Candidate_T {
    long a, b;
    struct Candidate_T *prev;
    struct Candidate_T *next;
} Candidate_T;

/*
 * Get a string from a Tcl object and compute the hash value for it.
 */
static int
hash(Tcl_Obj *objPtr,      /* Input Object */
     int ignore,           /* Ignore flags */
     Hash_T *res,          /* Hash value   */
     long *chars)          /* Number of chars read */
{
    Hash_T h = 0;
    int c, i, length;
    long nc = 0;
    char *string;
    string = Tcl_GetStringFromObj(objPtr, &length);

    /* Use the fast way when no ignore flag is used. */
    if (ignore == 0) {
        for (i = 0; i < length; i++) {
            h += (h << 3) + string[i];
        }
        nc = length;
    } else {
        const int ignoreallspace = (ignore & IGNORE_ALL_SPACE);
        const int ignorespace    = (ignore & IGNORE_SPACE_CHANGE);
        const int ignorecase     = (ignore & IGNORE_CASE);
        /*const int ignorekey      = (ignore & IGNORE_KEYWORD);*/
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
               int ignore)           /* Ignore flags */
{
    int c1, c2, i1, i2, length1, length2;
    char *string1, *string2;
    const int ignoreallspace = (ignore & IGNORE_ALL_SPACE);
    const int ignorespace    = (ignore & IGNORE_SPACE_CHANGE);
    const int ignorecase     = (ignore & IGNORE_CASE);
    /*const int ignorekey      = (ignore & IGNORE_KEYWORD);*/
    string1 = Tcl_GetStringFromObj(obj1Ptr, &length1);
    string2 = Tcl_GetStringFromObj(obj2Ptr, &length2);

    /* Use the fast way when no ignore flag is used. */
    if (ignore == 0) {
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
    else if (v1->serial < v2->serial)
        return -1;
    else if (v1->serial > v2->serial)
        return 1;
    else
        return 0;
}

/* Keep track of all candidates to free them easily */
static Candidate_T *candidates = NULL;

static Candidate_T *
NewCandidate(long a, long b, Candidate_T *prev) {
    Candidate_T *c = (Candidate_T *) ckalloc(sizeof(Candidate_T));
    c->a = a;
    c->b = b;
    c->prev = prev;
    c->next = candidates;
    candidates = c;
    return c;
}

static void
FreeCandidates(void) {
    Candidate_T *c = candidates, *n;
    while (c != NULL) {
        n = c->next;
        ckfree((char *) c);
        c = n;
    }
    candidates = NULL;
}

static void
merge(Candidate_T **K,
      long *k,
      long i,
      E_T *E,
      long p) {
    Candidate_T *c = K[0], *newc;
    long r = 0, j, b1 = 0, b2 = 0;
    long first, last, s = 0;

    /*printf("Merge: k = %ld  i = %ld  p = %ld\n", *k, i, p);*/

    while (1) {
        j = E[p].serial;
        /*printf("p = %ld  j = %ld  r = %ld  s= %ld  k = %ld\n", p, j, r, s, *k);*/
        /* Binary search */
        first = r;
        last = *k;
        while (first <= last) {
            /*printf("First %ld  Last %ld\n", first, last);*/
            s = (first + last) / 2;
            b1 = K[s]->b;
            b2 = K[s+1]->b;
            if (b1 < j && b2 > j) {
                break;
            }
            if (b2 < j) {
                first = s + 1;
            } else {
                last = s - 1;
            }
        }
        if (b1 < j && b2 > j) {
            /*printf("Set K%ld to (%ld,%ld)->(%ld,%ld)\n", r, c->a, c->b, c->prev == NULL ? 0: c->prev->a, c->prev == NULL ? 0: c->prev->b);*/
            newc = NewCandidate(i, j, K[s]);
            K[r] = c;
            r = s + 1;
            c = newc;

            /*printf("r = %ld  s= %ld  k = %ld\n", r, s, *k);*/
            if (s >= *k) {
                /*printf("Set K%ld\n", *k+2);*/
                K[*k+2] = K[*k+1];
                (*k)++;
                break;
            }
        }
        
        if (E[p].last) break;
        p++;
    }
    /*printf("Set K%ld to (%ld,%ld)->(%ld,%ld)\n", r, c->a, c->b, c->prev == NULL ? 0: c->prev->a, c->prev == NULL ? 0: c->prev->b);*/
    K[r] = c;
}

/*
 * The core part of the LCS algorithm.
 * Returns a ckalloc:ed array.
 * m - number of elements in first sequence
 * n - number of elements in second sequence
 * P - vector [0,m] of integers
 * E - vector [0,n]
 */
static long *
LcsCore(long m, long n, long *P, E_T *E)
{
    Candidate_T **K, *c;
    long i, k, *J;

    /* Find LCS */
    /*printf("Doing K\n");*/
    K = (Candidate_T **) ckalloc(sizeof(Candidate_T *) * ((m < n ? m : n) + 2));
    K[0] = NewCandidate(0, 0, NULL);
    K[1] = NewCandidate(m + 1, n + 1, NULL);
    k = 0;

    for (i = 1; i <= m; i++) {
        if (P[i] != 0) {
            /*printf("Merge i %ld  Pi %ld\n", i , P[i]);*/
            merge(K, &k, i, E, P[i]);
        }
    }
    
    /* Wrap up result */
    
    /*printf("Doing J\n");*/
    J = (long *) ckalloc((m + 1) * sizeof(long));
    for (i = 0; i <= m; i++) {
        J[i] = 0;
    }
    c = K[k];
    while (c != NULL) {
        if (c->a < 0 || c->a > m) printf("GURKA\n");
        J[c->a] = c->b;
        c = c->prev;
    }

    /*printf("Clean up Candidates and K\n");*/
    FreeCandidates();
    ckfree((char *) K);
    return J;
}

/*
 * Read two files, hash them and prepare the datastructures needed in LCS.
 */
static void
ReadAndHashFiles(Tcl_Interp *interp, char *name1, char *name2, int ignore,
                 long *mPtr, long *nPtr,
                 long **PPtr, E_T **EPtr)
{
    V_T *V;
    E_T *E;
    struct stat buf;
    long j, m, n, h, *P;
    long allocedV, allocedP, first, last, chars;
    Tcl_Channel ch;
    Tcl_Obj *linePtr;

    m = 0; /* FIXA ta bort */

    /* Initialize an object to use as line buffer. */
    linePtr = Tcl_NewObj();
    Tcl_IncrRefCount(linePtr);
    Tcl_SetObjLength(linePtr, 1000);
    Tcl_SetObjLength(linePtr, 0);

    /* Guess the number of lines in name2 for an inital allocation of V */
    stat(name2, &buf);
    allocedV = buf.st_size / 40;
    /* If the guess is low, alloc some more to be safe. */
    if (allocedV < 5000) allocedV = 5000;
    V = (V_T *) ckalloc(allocedV * sizeof(V_T));

    /* Read file 2 and calculate hashes for each line */
    ch = Tcl_OpenFileChannel(interp, name2, "r", 0);

    n = 1;
    while (1) {
        V[n].serial = n;
        V[n].chars = 0;
        Tcl_SetObjLength(linePtr, 0);
        if (Tcl_GetsObj(ch, linePtr) < 0) {
            n--;
            break;
        }

        hash(linePtr, ignore, &V[n].hash, &V[n].chars);
        n++;
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
    stat(name1, &buf);
    allocedP = buf.st_size / 40;
    /* If the guess is low, alloc some more to be safe. */
    if (allocedP < 10000) allocedP = 10000;
    P = (long *) ckalloc(allocedP * sizeof(long));

    /* Read file and calculate hashes for each line */
    ch = Tcl_OpenFileChannel(interp, name1, "r", 0);
    m = 1;
    while (1) {
        P[m] = 0;
        Tcl_SetObjLength(linePtr, 0);
        if (Tcl_GetsObj(ch, linePtr) < 0) {
            m--;
            break;
        }
        hash(linePtr, ignore, &h, &chars);

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
        
        m++;
        if (m >= allocedP) {
            allocedP = allocedP * 3 / 2;
            P = (long *) ckrealloc((char *) P,
                                   allocedP * sizeof(long));
        }
    }
    Tcl_Close(interp, ch);

    /* Clean up */
    ckfree((char *) V);
    Tcl_DecrRefCount(linePtr);

    *mPtr = m;
    *nPtr = n;
    *PPtr = P;
    *EPtr = E;
}

static Tcl_Obj * 
NewChunk(Tcl_Interp *interp, int start1, int n1, int start2, int n2) {
    Tcl_Obj *subPtr = Tcl_NewListObj(0, NULL);
    if (n1 == 0) {
        /* Added */
        Tcl_ListObjAppendElement(interp, subPtr,
                                 Tcl_NewStringObj("a", 1));
    } else if (n2 == 0) {
        /* Deleted */
        Tcl_ListObjAppendElement(interp, subPtr,
                                 Tcl_NewStringObj("d", 1));
    } else {
        /* Changed */
        Tcl_ListObjAppendElement(interp, subPtr,
                                 Tcl_NewStringObj("c", 1));
    }
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj(start1));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj(n1));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj(start2));
    Tcl_ListObjAppendElement(interp, subPtr, Tcl_NewLongObj(n2));
    return subPtr;
}

static Tcl_Obj *
CompareFiles(Tcl_Interp *interp, char *name1, char *name2, int ignore)
{
    E_T *E;
    long m, n, *P, *J;
    Tcl_Obj *resPtr, *subPtr;

    /*printf("Doing ReadAndHash\n");*/
    ReadAndHashFiles(interp, name1, name2, ignore, &m, &n, &P, &E);
    /*printf("Doing LcsCore m = %ld, n = %ld\n", m, n);*/
    J = LcsCore(m, n, P, E);
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

    resPtr = Tcl_NewListObj(0, NULL);
    /* Take care of trivial cases first */
    if ((m == 0 && n > 0) || (m > 0 && n == 0)) {
        Tcl_ListObjAppendElement(interp, resPtr, NewChunk(interp, 1, m, 1, n));
    } else if (m > 0 && n > 0) {
        Tcl_Channel ch1, ch2;
        Tcl_Obj *line1Ptr, *line2Ptr;
        int current1, current2, n1, n2;
        int startblock1, startblock2;

        /* Initialize objects to use as line buffers. */
        line1Ptr = Tcl_NewObj();
        Tcl_IncrRefCount(line1Ptr);
        Tcl_SetObjLength(line1Ptr, 1000);
        line2Ptr = Tcl_NewObj();
        Tcl_IncrRefCount(line2Ptr);
        Tcl_SetObjLength(line2Ptr, 1000);

        ch1 = Tcl_OpenFileChannel(interp, name1, "r", 0);
        ch2 = Tcl_OpenFileChannel(interp, name2, "r", 0);
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
            if (CompareObjects(line1Ptr, line2Ptr, ignore) != 0) {
                /* No match, continue until next. */
                printf("NOT Match\n");
                continue;
            }
            
            n1 = current1 - startblock1;
            n2 = current2 - startblock2;
            if (n1 > 0 || n2 > 0) {
                subPtr = NewChunk(interp, startblock1, n1, startblock2, n2);
                Tcl_ListObjAppendElement(interp, resPtr, subPtr);
            }
            startblock1 = current1 + 1;
            startblock2 = current2 + 1;
        }
        /* Scrape up the last */
        n1 = m - startblock1 + 1;
        n2 = n - startblock2 + 1;
        if (n1 > 0 || n2 > 0) {
            subPtr = NewChunk(interp, startblock1, n1, startblock2, n2);
            Tcl_ListObjAppendElement(interp, resPtr, subPtr);
        }
        Tcl_Close(interp, ch1);
        Tcl_Close(interp, ch2);
        Tcl_DecrRefCount(line1Ptr);
        Tcl_DecrRefCount(line2Ptr);
    }

    ckfree((char *) J);
    return resPtr;
}

int
DiffFilesObjCmd(dummy, interp, objc, objv)
    ClientData dummy;    	/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int index, t, ignore;
    Tcl_Obj *resPtr;
    char *file1, *file2;
    static CONST char *options[] = {
	"-b", "-w", "-i", "-nocase", (char *) NULL
    };
    enum options {
	OPT_B, OPT_W, OPT_I, OPT_NOCASE
    };	  

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?opts? file1 file2");
	return TCL_ERROR;
    }

    ignore = 0;
    for (t = 1; t < objc - 2; t++) {
	if (Tcl_GetIndexFromObj(interp, objv[t], options, "option", 0,
		&index) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch (index) {
	  case OPT_NOCASE:
	  case OPT_I:
            ignore |= IGNORE_CASE;
	    break;
	  case OPT_B:
            ignore |= IGNORE_SPACE_CHANGE;
	    break;
	  case OPT_W:
            ignore |= IGNORE_ALL_SPACE;
	    break;
	}
    }
    file1 = Tcl_GetString(objv[objc-2]);
    file2 = Tcl_GetString(objv[objc-1]);

    resPtr = CompareFiles(interp, file1, file2, ignore);
    Tcl_SetObjResult(interp, resPtr);

    return TCL_OK;
}
