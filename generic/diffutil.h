/***********************************************************************
 *
 * This is the C implementation of the DiffUtil package
 *
 * Copyright (c) 2004, 2010, Peter Spjuth
 *
 ***********************************************************************/

/*#define CANDIDATE_DEBUG*/
/*#define CANDIDATE_STATS*/

/* A type to hold hashing values */
typedef unsigned long Hash_T;

/* A type to hold line numbers */
typedef unsigned long Line_T;

/* A type for selecting result style of diff functions */
typedef enum {
    Result_Diff, Result_Match
} Result_T;

/* Hold all options for diffing in a common struct */
#define STATIC_ALIGN 10
typedef struct {
    /* Ignore flags */
    int ignore;
    /* Let empty lines be considered different in the LCS algorithm. */
    int noempty;
    /* How many equal elements does it take before it is disregarded? */
    int pivot;
    /* Show full words in changes */
    int wordparse;
    /* Range */
    Line_T rFrom1, rTo1, rFrom2, rTo2;
    /* Regsub */
    Tcl_Obj *regsubLeftPtr;
    Tcl_Obj *regsubRightPtr;
    /* Result Style */
    Result_T resultStyle;
    /* Alignment */
    int alignLength;
    Line_T *align;
    Line_T staticAlign[STATIC_ALIGN];
} DiffOptions_T;

/* Helper to get a filled in DiffOptions_T */
#define InitDiffOptions_T(opts) {opts.ignore = 0; opts.noempty = 0; opts.pivot = 100; opts.wordparse = 0; opts.rFrom1 = 1; opts.rTo1 = 0; opts.rFrom2 = 1; opts.rTo2 = 0; opts.regsubLeftPtr = NULL; opts.regsubRightPtr = NULL; opts.resultStyle = Result_Diff; opts.alignLength = 0; opts.align = opts.staticAlign;}
 
/* Flags in DiffOptions_T's ignore field */

#define IGNORE_ALL_SPACE 1
#define IGNORE_SPACE_CHANGE 2
#define IGNORE_CASE 4
#define IGNORE_NUMBERS 8

/*
 * A type to implement the V vector in the LCS algorithm.
 * For each line (element) in "file 2", this struct holds
 * the line number (serial) and the hash values for the line.
 *
 * The hashes are split in hash and realhash. The former is calculated
 * taking account ignore options and is used for matching lines.
 * The latter hashes the full line, to give the algorithm the
 * possibility to prefer exact matches.
 */
typedef struct {
    Line_T serial;
    Hash_T hash;
    Hash_T realhash;
} V_T;

/*
 * A type to implement the E vector in the LCS algorithm.
 * The E vector mirrors the sorted V vector and holds equivalence
 * classes of lines in "file 2".
 */
typedef struct {
    Line_T serial;
    int last;        /* True on the last element of each class */
    int count;       /* On the first in each class, keeps the number
		      * of lines in the class. Otherwise zero. */
    Hash_T hash;     /* Keep the hash for reference */
    Hash_T realhash; /* Keep the realhash for reference */
    int    forbidden; /* True if this element cannot match initially. */
} E_T;

/*
 * A type to implement the P vector in the LCS algorithm.
 *
 * This reflects each line in "file 1" and points to the equivalent
 * class in the E vector.
 * A zero means there is no matching line in "file 2".
 */
typedef struct {
    Line_T Eindex;   /* First element in equivalent class in E vector */
    Hash_T hash;     /* Keep the hash for reference */
    Hash_T realhash; /* Keep the realhash for reference */
    int    forbidden; /* True if this element cannot match initially. */
} P_T;


extern void      AppendChunk(Tcl_Interp *interp, Tcl_Obj *listPtr,
			DiffOptions_T *optsPtr,	Line_T start1, Line_T n1,
			Line_T start2, Line_T n2);
extern E_T *     BuildEVector(V_T *V, Line_T n);
extern Tcl_Obj * BuildResultFromJ(Tcl_Interp *interp, DiffOptions_T *optsPtr,
			Line_T m, Line_T n, Line_T *J);
extern int       CompareObjects(Tcl_Obj *obj1Ptr, Tcl_Obj *obj2Ptr,
			DiffOptions_T *optsPtr);
extern int       CompareV(const void *a1, const void *a2);
extern void      Hash(Tcl_Obj *objPtr, DiffOptions_T *optsPtr, int left,
			Hash_T *result, Hash_T *real);
extern Line_T *  LcsCore(Tcl_Interp *interp, Line_T m, Line_T n, P_T *P,
			E_T *E, DiffOptions_T *optsPtr);
extern Tcl_Obj * NewChunk(Tcl_Interp *interp, DiffOptions_T *optsPtr,
			Line_T start1, Line_T n1, Line_T start2, Line_T n2);
extern void      NormaliseOpts(DiffOptions_T *optsPtr);
extern int       SetOptsRange(Tcl_Interp *interp, Tcl_Obj *rangePtr, int first,
			DiffOptions_T *optsPtr);
extern int       SetOptsAlign(Tcl_Interp *interp, Tcl_Obj *alignPtr, int first,
			DiffOptions_T *optsPtr);


extern int
CompareFilesObjCmd(ClientData dummy,
                Tcl_Interp *interp,
                int objc,
                Tcl_Obj *CONST objv[]);

extern int
DiffFilesObjCmd(ClientData dummy,
                Tcl_Interp *interp,
                int objc,
                Tcl_Obj *CONST objv[]);

extern int
DiffListsObjCmd(ClientData dummy,
                Tcl_Interp *interp,
                int objc,
                Tcl_Obj *CONST objv[]);

extern int
DiffStringsObjCmd(ClientData dummy,
                  Tcl_Interp *interp,
                  int objc,
                  Tcl_Obj *CONST objv[]);

extern int
DiffStrings2ObjCmd(ClientData dummy,
                   Tcl_Interp *interp,
                   int objc,
                   Tcl_Obj *CONST objv[]);
