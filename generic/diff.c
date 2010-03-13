/***********************************************************************
 *
 * This file implements the central LCS function for comparing things,
 * and the diffFiles command.
 *
 * Copyright (c) 2004, Peter Spjuth
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

/*
 * Check if an index pair fails to match due to alignment
 */
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
 * The hash algorithm is currently very simplistic and can probably
 * be replaced by something better without losing speed.
 * An empty line is assumed to have a hash value of 0.
 */
#define HASH_ADD(hash, character) hash += (hash << 7) + (character)

/*
 * Get a string from a Tcl object and compute the hash value for it.
 */
void
Hash(Tcl_Obj *objPtr,         /* Input Object */
     DiffOptions_T *optsPtr,  /* Options      */
     Hash_T *result,          /* Hash value   */
     Hash_T *real)            /* Hash value when ignoring ignore */
{
    Hash_T hash;
    int i, length;
    char *string, *str;
    Tcl_UniChar c;

    string = Tcl_GetStringFromObj(objPtr, &length);

    /* Use the fast way when no ignore flag is used. */
    hash = 0;
    for (i = 0; i < length; i++) {
	HASH_ADD(hash, string[i]);
    }
    *real = hash;
    if (optsPtr->ignore != 0) {
        const int ignoreallspace = (optsPtr->ignore & IGNORE_ALL_SPACE);
        const int ignorespace    = (optsPtr->ignore & IGNORE_SPACE_CHANGE);
        const int ignorecase     = (optsPtr->ignore & IGNORE_CASE);
        const int ignorenum      = (optsPtr->ignore & IGNORE_NUMBERS);
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
                if (ignoreallspace) continue;
                /* Any consecutive whitespace is regarded as a single space */
                if (ignorespace && in == IN_SPACE) continue;
                if (ignorespace)
		    c = ' ';
                in = IN_SPACE;
            } else if (ignorenum && Tcl_UniCharIsDigit(c)) {
                if (in == IN_NUMBER) continue;
                /* A string of digits is replaced by a single 0 */
                c = '0';
                in = IN_NUMBER;
            } else {
                in = IN_NONE;
                if (ignorecase) {
                    c = Tcl_UniCharToLower(c);
                }
            }
	    HASH_ADD(hash, c);
        }
    }
    *result = hash;
    return;
}

/*
 * Compare two strings, ignoring things in the same way as hash does.
 * Should be recoded to use Unicode functions.
 */
int
CompareObjects(Tcl_Obj *obj1Ptr,
               Tcl_Obj *obj2Ptr,
               DiffOptions_T *optsPtr)
{
    int c1, c2, i1, i2, length1, length2, start;
    char *string1, *string2;
    const int ignoreallspace = (optsPtr->ignore & IGNORE_ALL_SPACE);
    const int ignorespace    = (optsPtr->ignore & IGNORE_SPACE_CHANGE);
    const int ignorecase     = (optsPtr->ignore & IGNORE_CASE);
    const int ignorenum      = (optsPtr->ignore & IGNORE_NUMBERS);

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
                start = i1;
                while (i1 < length1 && isspace(string1[i1])) i1++;
                if (ignoreallspace || start == 0) {
                    c1 = string1[i1];
                } else {
                    i1--;
                    c1 = ' ';
                }
            }
        }
        if (ignorenum && isdigit(c1)) {
            /* Scan up to non-digit */
            while (i1 < length1 && isdigit(string1[i1])) i1++;
            i1--;
            c1 = '0';
        }
        if (ignorecase && isupper(c1)) {
            c1 = tolower(c1);
        }

        c2 = string2[i2];
        if (isspace(c2)) {
            if (ignoreallspace || ignorespace) {
                /* Scan up to non-space */
                start = i2;
                while (i2 < length2 && isspace(string2[i2])) i2++;
                if (ignoreallspace || start == 0) {
                    c2 = string2[i2];
                } else {
                    i2--;
                    c2 = ' ';
                }
            }
        }
        if (ignorenum && isdigit(c2)) {
            /* Scan up to non-digit */
            while (i2 < length2 && isdigit(string2[i2])) i2++;
            i2--;
            c2 = '0';
        }
        if (ignorecase && isupper(c2)) {
            c2 = tolower(c2);
        }

        if (i1 >= length1 && i2 <  length2) return -1;
        if (i1 < length1  && i2 >= length2) return  1;
        if (c1 < c2) return -1;
        if (c1 > c2) return  1;
        i1++;
        i2++;
    }
    return 0;
}

/*
 * A compare function to qsort the V vector.
 * Sorts first on hash, then on serial number.
 */
int
CompareV(const void *a1, const void *a2) {
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

/* Create a new candidate */
static Candidate_T *
NewCandidate(CandidateAlloc_T **first,
	     Line_T a, Line_T b, Hash_T realhash,
             Candidate_T *prev, Candidate_T *peer) {
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
merge(CandidateAlloc_T **firstCandidate,
      Candidate_T **K,
      Line_T *k,      /* Index to last used element in K */
      Line_T i,       /* Current index in file 1 */
      P_T *P,         /* P vector */
      E_T *E,         /* E vector */
      Line_T p,       /* Index in E of the file 2 class equivalent to line i */
      DiffOptions_T *optsPtr,
      Line_T m,       /* Size of file 1 */
      Line_T n        /* Size of file 2 */
	)
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
static inline void
ScoreCandidate(Candidate_T *c, P_T *P)
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
ScoreCandidates(Line_T k, Candidate_T **K, P_T *P)
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
    stack = (Candidate_T **) ckalloc((k * 3) * sizeof(Candidate_T *));
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
            }
        }
        if (ready) {
            /* All previous have a score, we can score this one */
            ScoreCandidate(cand, P);
            sp--;
        }
        if (sp > (k * 2)) {
            /* Out of stack, bad */
            printf("Debug: Out of stack in ScoreCandidates\n");
            break;
        }
    }

    ckfree((char *) stack);
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
LcsCore(Tcl_Interp *interp,
        Line_T m, Line_T n,
	P_T *P, E_T *E,
        DiffOptions_T *optsPtr)
{
    Candidate_T **K, *c;
    Line_T i, k, *J;
    const int allowempty = !optsPtr->noempty;
    /* Keep track of all candidates to free them easily */
    CandidateAlloc_T *candidates = NULL;

    //printf("Doing K\n");

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
        if (P[i].Eindex != 0 && (allowempty || P[i].hash != 0)) {
            /*printf("Merge i %ld  Pi %ld\n", i , P[i]);*/
            merge(&candidates, K, &k, i, P, E, P[i].Eindex, optsPtr, m, n);
        }
    }

    //printf("Doing Score k = %ld\n", k);
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

    if (!allowempty) {
        /*
         * We have ignored empty lines before which means that there
         * may be empty lines that can be matched.
         */

	Line_T lastline1 = 0, lastline2 = 0;
	Line_T cntempty1 = 0, emptyI = 0;
	Line_T cntempty2 = 0, emptyJ = 0;
	Line_T j, firstJ, lastJ;

	for (i = 1; i <= (m + 1); i++) {
	    if (i > m || J[i] != 0) {
		if (cntempty1 > 0) {
		    /*
		     * We have empty lines in the left part of this change
		     * block.
		     */
		    firstJ = lastline2 + 1;
		    lastJ = i > m ? n : J[i];
		    cntempty2 = 0;

		    for (j = 1; j <= m; j++) {
			if (E[j].serial >= firstJ && E[j].serial <= lastJ &&
				E[j].hash == 0) {
			    cntempty2++;
			    /* Remember the first one */
			    if (emptyJ == 0) {
				emptyJ = E[j].serial;
			    }
			}
		    }

		    if (cntempty2 > 0) {
			/*
			 * We have empty lines in both parts of this change
			 * block. What to do with it?
			 * FIXA
			 * Match the first ones, to handle a simple case
			 */
			J[emptyI] = emptyJ;
		    }
		}
		lastline1 = i;
		lastline2 = J[i];
		cntempty1 = 0;
		cntempty2 = 0;
		emptyI = 0;
		emptyJ = 0;
		continue;
	    }
	    if (P[i].hash == 0) {
		cntempty1++;
		/* Remember the first one */
		if (emptyI == 0) {
		    emptyI = i;
		}
		continue;
	    }
	}

        /*
         * This could be left to block matching if that is improved
         * to handle equal lines within change blocks.
         */
    }

    return J;
}

/*
 * Build E vector from V vector.
 *
 * Returns the ckalloc:ed E vector.
 */
E_T *
BuildEVector(V_T *V, Line_T n)
{
    Line_T j, first;
    E_T *E;

    E = (E_T *) ckalloc((n + 1) * sizeof(E_T));
    E[0].serial = 0;
    E[0].last = 1;
    E[0].count = 0;
    first = 1;
    for (j = 1; j <= n; j++) {
        E[j].serial   = V[j].serial;
        E[j].hash     = V[j].hash;
        E[j].realhash = V[j].realhash;
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
BSearchVVector(V_T *V, Line_T n, Hash_T h)
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

/* Fill in the range option from a Tcl Value */
int
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
