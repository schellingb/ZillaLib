/*
** SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008) 
** Copyright (C) [dates of first publication] Silicon Graphics, Inc.
** All Rights Reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
** of the Software, and to permit persons to whom the Software is furnished to do so,
** subject to the following conditions:
** 
** The above copyright notice including the dates of first publication and either this
** permission notice or a reference to http://oss.sgi.com/projects/FreeB/ shall be
** included in all copies or substantial portions of the Software. 
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
** INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
** PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL SILICON GRAPHICS, INC.
** BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
** OR OTHER DEALINGS IN THE SOFTWARE.
** 
** Except as contained in this notice, the name of Silicon Graphics, Inc. shall not
** be used in advertising or otherwise to promote the sale, use or other dealings in
** this Software without prior written authorization from Silicon Graphics, Inc.
*/
/*
** Author: Eric Veach, July 1994.
*/
/*
** Libtess2 - Version 1.0.1 - https://github.com/memononen/libtess2 (Bucketalloc, etc)
** Author: Mikko Mononen, July 2009.
*/

#include "tesselator.h"

#include <setjmp.h>
#include <assert.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define TRUE 1
#define FALSE 0
#define INIT_SIZE 32

struct BucketAlloc *createBucketAlloc( TESSalloc* alloc, const char *name,
									  unsigned int itemSize, unsigned int bucketSize );
void *bucketAlloc( struct BucketAlloc *ba);
void bucketFree( struct BucketAlloc *ba, void *ptr );
void deleteBucketAlloc( struct BucketAlloc *ba );

typedef struct TESSmesh TESSmesh; 
typedef struct TESSvertex TESSvertex;
typedef struct TESSface TESSface;
typedef struct TESShalfEdge TESShalfEdge;
typedef struct ActiveRegion ActiveRegion;

/* The mesh structure is similar in spirit, notation, and operations
* to the "quad-edge" structure (see L. Guibas and J. Stolfi, Primitives
* for the manipulation of general subdivisions and the computation of
* Voronoi diagrams, ACM Transactions on Graphics, 4(2):74-123, April 1985).
* For a simplified description, see the course notes for CS348a,
* "Mathematical Foundations of Computer Graphics", available at the
* Stanford bookstore (and taught during the fall quarter).
* The implementation also borrows a tiny subset of the graph-based approach
* use in Mantyla's Geometric Work Bench (see M. Mantyla, An Introduction
* to Sold Modeling, Computer Science Press, Rockville, Maryland, 1988).
*
* The fundamental data structure is the "half-edge".  Two half-edges
* go together to make an edge, but they point in opposite directions.
* Each half-edge has a pointer to its mate (the "symmetric" half-edge Sym),
* its origin vertex (Org), the face on its left side (Lface), and the
* adjacent half-edges in the CCW direction around the origin vertex
* (Onext) and around the left face (Lnext).  There is also a "next"
* pointer for the global edge list (see below).
*
* The notation used for mesh navigation:
*  Sym   = the mate of a half-edge (same edge, but opposite direction)
*  Onext = edge CCW around origin vertex (keep same origin)
*  Dnext = edge CCW around destination vertex (keep same dest)
*  Lnext = edge CCW around left face (dest becomes new origin)
*  Rnext = edge CCW around right face (origin becomes new dest)
*
* "prev" means to substitute CW for CCW in the definitions above.
*
* The mesh keeps global lists of all vertices, faces, and edges,
* stored as doubly-linked circular lists with a dummy header node.
* The mesh stores pointers to these dummy headers (vHead, fHead, eHead).
*
* The circular edge list is special; since half-edges always occur
* in pairs (e and e->Sym), each half-edge stores a pointer in only
* one direction.  Starting at eHead and following the e->next pointers
* will visit each *edge* once (ie. e or e->Sym, but not both).
* e->Sym stores a pointer in the opposite direction, thus it is
* always true that e->Sym->next->Sym->next == e.
*
* Each vertex has a pointer to next and previous vertices in the
* circular list, and a pointer to a half-edge with this vertex as
* the origin (NULL if this is the dummy header).  There is also a
* field "data" for client data.
*
* Each face has a pointer to the next and previous faces in the
* circular list, and a pointer to a half-edge with this face as
* the left face (NULL if this is the dummy header).  There is also
* a field "data" for client data.
*
* Note that what we call a "face" is really a loop; faces may consist
* of more than one loop (ie. not simply connected), but there is no
* record of this in the data structure.  The mesh may consist of
* several disconnected regions, so it may not be possible to visit
* the entire mesh by starting at a half-edge and traversing the edge
* structure.
*
* The mesh does NOT support isolated vertices; a vertex is deleted along
* with its last edge.  Similarly when two faces are merged, one of the
* faces is deleted (see tessMeshDelete below).  For mesh operations,
* all face (loop) and vertex pointers must not be NULL.  However, once
* mesh manipulation is finished, TESSmeshZapFace can be used to delete
* faces of the mesh, one at a time.  All external faces can be "zapped"
* before the mesh is returned to the client; then a NULL face indicates
* a region which is not part of the output polygon.
*/

struct TESSvertex {
	TESSvertex *next;      /* next vertex (never NULL) */
	TESSvertex *prev;      /* previous vertex (never NULL) */
	TESShalfEdge *anEdge;    /* a half-edge with this origin */

	/* Internal data (keep hidden) */
	TESSreal coords[3];  /* vertex location in 3D */
	TESSreal s, t;       /* projection onto the sweep plane */
	int pqHandle;   /* to allow deletion from priority queue */
	TESSindex n;			/* to allow identify unique vertices */
	TESSindex idx;			/* to allow map result to original verts */
};

struct TESSface {
	TESSface *next;      /* next face (never NULL) */
	TESSface *prev;      /* previous face (never NULL) */
	TESShalfEdge *anEdge;    /* a half edge with this left face */

	/* Internal data (keep hidden) */
	TESSface *trail;     /* "stack" for conversion to strips */
	TESSindex n;		/* to allow identiy unique faces */
	char marked;     /* flag for conversion to strips */
	char inside;     /* this face is in the polygon interior */
};

struct TESShalfEdge {
	TESShalfEdge *next;      /* doubly-linked list (prev==Sym->next) */
	TESShalfEdge *Sym;       /* same edge, opposite direction */
	TESShalfEdge *Onext;     /* next edge CCW around origin */
	TESShalfEdge *Lnext;     /* next edge CCW around left face */
	TESSvertex *Org;       /* origin vertex (Overtex too long) */
	TESSface *Lface;     /* left face */

	/* Internal data (keep hidden) */
	ActiveRegion *activeRegion;  /* a region with this upper edge (sweep.c) */
	int winding;    /* change in winding number when crossing
						  from the right face to the left face */
};

#define Rface   Sym->Lface
#define Dst Sym->Org

#define Oprev   Sym->Lnext
#define Lprev   Onext->Sym
#define Dprev   Lnext->Sym
#define Rprev   Sym->Onext
#define Dnext   Rprev->Sym  /* 3 pointers */
#define Rnext   Oprev->Sym  /* 3 pointers */


struct TESSmesh {
	TESSvertex vHead;      /* dummy header for vertex list */
	TESSface fHead;      /* dummy header for face list */
	TESShalfEdge eHead;      /* dummy header for edge list */
	TESShalfEdge eHeadSym;   /* and its symmetric counterpart */

	struct BucketAlloc* edgeBucket;
	struct BucketAlloc* vertexBucket;
	struct BucketAlloc* faceBucket;
};

/* The mesh operations below have three motivations: completeness,
* convenience, and efficiency.  The basic mesh operations are MakeEdge,
* Splice, and Delete.  All the other edge operations can be implemented
* in terms of these.  The other operations are provided for convenience
* and/or efficiency.
*
* When a face is split or a vertex is added, they are inserted into the
* global list *before* the existing vertex or face (ie. e->Org or e->Lface).
* This makes it easier to process all vertices or faces in the global lists
* without worrying about processing the same data twice.  As a convenience,
* when a face is split, the "inside" flag is copied from the old face.
* Other internal data (v->data, v->activeRegion, f->data, f->marked,
* f->trail, e->winding) is set to zero.
*
* ********************** Basic Edge Operations **************************
*
* tessMeshMakeEdge( mesh ) creates one edge, two vertices, and a loop.
* The loop (face) consists of the two new half-edges.
*
* tessMeshSplice( eOrg, eDst ) is the basic operation for changing the
* mesh connectivity and topology.  It changes the mesh so that
*  eOrg->Onext <- OLD( eDst->Onext )
*  eDst->Onext <- OLD( eOrg->Onext )
* where OLD(...) means the value before the meshSplice operation.
*
* This can have two effects on the vertex structure:
*  - if eOrg->Org != eDst->Org, the two vertices are merged together
*  - if eOrg->Org == eDst->Org, the origin is split into two vertices
* In both cases, eDst->Org is changed and eOrg->Org is untouched.
*
* Similarly (and independently) for the face structure,
*  - if eOrg->Lface == eDst->Lface, one loop is split into two
*  - if eOrg->Lface != eDst->Lface, two distinct loops are joined into one
* In both cases, eDst->Lface is changed and eOrg->Lface is unaffected.
*
* tessMeshDelete( eDel ) removes the edge eDel.  There are several cases:
* if (eDel->Lface != eDel->Rface), we join two loops into one; the loop
* eDel->Lface is deleted.  Otherwise, we are splitting one loop into two;
* the newly created loop will contain eDel->Dst.  If the deletion of eDel
* would create isolated vertices, those are deleted as well.
*
* ********************** Other Edge Operations **************************
*
* tessMeshAddEdgeVertex( eOrg ) creates a new edge eNew such that
* eNew == eOrg->Lnext, and eNew->Dst is a newly created vertex.
* eOrg and eNew will have the same left face.
*
* tessMeshSplitEdge( eOrg ) splits eOrg into two edges eOrg and eNew,
* such that eNew == eOrg->Lnext.  The new vertex is eOrg->Dst == eNew->Org.
* eOrg and eNew will have the same left face.
*
* tessMeshConnect( eOrg, eDst ) creates a new edge from eOrg->Dst
* to eDst->Org, and returns the corresponding half-edge eNew.
* If eOrg->Lface == eDst->Lface, this splits one loop into two,
* and the newly created loop is eNew->Lface.  Otherwise, two disjoint
* loops are merged into one, and the loop eDst->Lface is destroyed.
*
* ************************ Other Operations *****************************
*
* tessMeshNewMesh() creates a new mesh with no edges, no vertices,
* and no loops (what we usually call a "face").
*
* tessMeshUnion( mesh1, mesh2 ) forms the union of all structures in
* both meshes, and returns the new mesh (the old meshes are destroyed).
*
* tessMeshDeleteMesh( mesh ) will free all storage for any valid mesh.
*
* tessMeshZapFace( fZap ) destroys a face and removes it from the
* global face list.  All edges of fZap will have a NULL pointer as their
* left face.  Any edges which also have a NULL pointer as their right face
* are deleted entirely (along with any isolated vertices this produces).
* An entire mesh can be deleted by zapping its faces, one at a time,
* in any order.  Zapped faces cannot be used in further mesh operations!
*
* tessMeshCheckMesh( mesh ) checks a mesh for self-consistency.
*/

TESShalfEdge *tessMeshMakeEdge( TESSmesh *mesh );
int tessMeshSplice( TESSmesh *mesh, TESShalfEdge *eOrg, TESShalfEdge *eDst );
int tessMeshDelete( TESSmesh *mesh, TESShalfEdge *eDel );

TESShalfEdge *tessMeshAddEdgeVertex( TESSmesh *mesh, TESShalfEdge *eOrg );
TESShalfEdge *tessMeshSplitEdge( TESSmesh *mesh, TESShalfEdge *eOrg );
TESShalfEdge *tessMeshConnect( TESSmesh *mesh, TESShalfEdge *eOrg, TESShalfEdge *eDst );

TESSmesh *tessMeshNewMesh( TESSalloc* alloc );
TESSmesh *tessMeshUnion( TESSalloc* alloc, TESSmesh *mesh1, TESSmesh *mesh2 );
int tessMeshMergeConvexFaces( TESSmesh *mesh, int maxVertsPerFace );
void tessMeshDeleteMesh( TESSalloc* alloc, TESSmesh *mesh );
void tessMeshZapFace( TESSmesh *mesh, TESSface *fZap );

#ifdef NDEBUG
#define tessMeshCheckMesh( mesh )
#else
void tessMeshCheckMesh( TESSmesh *mesh );
#endif

typedef void *DictKey;
typedef struct Dict Dict;
typedef struct DictNode DictNode;

Dict *dictNewDict( TESSalloc* alloc, void *frame, int (*leq)(void *frame, DictKey key1, DictKey key2) );

void dictDeleteDict( TESSalloc* alloc, Dict *dict );

/* Search returns the node with the smallest key greater than or equal
* to the given key.  If there is no such key, returns a node whose
* key is NULL.  Similarly, Succ(Max(d)) has a NULL key, etc.
*/
DictNode *dictSearch( Dict *dict, DictKey key );
DictNode *dictInsertBefore( Dict *dict, DictNode *node, DictKey key );
void dictDelete( Dict *dict, DictNode *node );

#define dictKey(n)	((n)->key)
#define dictSucc(n)	((n)->next)
#define dictPred(n)	((n)->prev)
#define dictMin(d)	((d)->head.next)
#define dictMax(d)	((d)->head.prev)
#define dictInsert(d,k) (dictInsertBefore((d),&(d)->head,(k)))


/*** Private data structures ***/

struct DictNode {
	DictKey	key;
	DictNode *next;
	DictNode *prev;
};

struct Dict {
	DictNode head;
	void *frame;
	struct BucketAlloc *nodePool;
	int (*leq)(void *frame, DictKey key1, DictKey key2);
};

typedef void *PQkey;
typedef int PQhandle;
typedef struct PriorityQHeap PriorityQHeap;

#define INV_HANDLE 0x0fffffff

typedef struct { PQhandle handle; } PQnode;
typedef struct { PQkey key; PQhandle node; } PQhandleElem;

struct PriorityQHeap {

	PQnode *nodes;
	PQhandleElem *handles;
	int size, max;
	PQhandle freeList;
	int initialized;

	int (*leq)(PQkey key1, PQkey key2);
};

typedef struct PriorityQ PriorityQ;

struct PriorityQ {
	PriorityQHeap *heap;

	PQkey *keys;
	PQkey **order;
	PQhandle size, max;
	int initialized;

	int (*leq)(PQkey key1, PQkey key2);
};

PriorityQ *pqNewPriorityQ( TESSalloc* alloc, int size, int (*leq)(PQkey key1, PQkey key2) );
void pqDeletePriorityQ( TESSalloc* alloc, PriorityQ *pq );

int pqInit( TESSalloc* alloc, PriorityQ *pq );
PQhandle pqInsert( TESSalloc* alloc, PriorityQ *pq, PQkey key );
PQkey pqExtractMin( PriorityQ *pq );
void pqDelete( PriorityQ *pq, PQhandle handle );

PQkey pqMinimum( PriorityQ *pq );
int pqIsEmpty( PriorityQ *pq );

//typedef struct TESStesselator TESStesselator;

struct TESStesselator {

	/*** state needed for collecting the input data ***/
	TESSmesh	*mesh;		/* stores the input contours, and eventually
						the tessellation itself */
	int outOfMemory;

	/*** state needed for projecting onto the sweep plane ***/

	TESSreal normal[3];	/* user-specified normal (if provided) */
	TESSreal sUnit[3];	/* unit vector in s-direction (debugging) */
	TESSreal tUnit[3];	/* unit vector in t-direction (debugging) */

	TESSreal bmin[2];
	TESSreal bmax[2];

	/*** state needed for the line sweep ***/
	int	windingRule;	/* rule for determining polygon interior */

	Dict *dict;		/* edge dictionary for sweep line */
	PriorityQ *pq;		/* priority queue of vertex events */
	TESSvertex *event;		/* current sweep event being processed */

	struct BucketAlloc* regionPool;

	TESSindex vertexIndexCounter;
	
	TESSreal *vertices;
	TESSindex *vertexIndices;
	int vertexCount;
	TESSindex *elements;
	int elementCount;

	TESSalloc alloc;
	
	jmp_buf env;			/* place to jump to when memAllocs fail */
};

//-------------------------------------------------------------------------------------------------------------------------------------------------

#ifdef NO_BRANCH_CONDITIONS
/* MIPS architecture has special instructions to evaluate boolean
* conditions -- more efficient than branching, IF you can get the
* compiler to generate the right instructions (SGI compiler doesn't)
*/
#define VertEq(u,v)	(((u)->s == (v)->s) & ((u)->t == (v)->t))
#define VertLeq(u,v)	(((u)->s < (v)->s) | \
	((u)->s == (v)->s & (u)->t <= (v)->t))
#else
#define VertEq(u,v) ((u)->s == (v)->s && (u)->t == (v)->t)
#define VertLeq(u,v) (((u)->s < (v)->s) || ((u)->s == (v)->s && (u)->t <= (v)->t))
#endif

#define EdgeEval(u,v,w)	tesedgeEval(u,v,w)
#define EdgeSign(u,v,w)	tesedgeSign(u,v,w)

/* Versions of VertLeq, EdgeSign, EdgeEval with s and t transposed. */

#define TransLeq(u,v) (((u)->t < (v)->t) || ((u)->t == (v)->t && (u)->s <= (v)->s))
#define TransEval(u,v,w) testransEval(u,v,w)
#define TransSign(u,v,w) testransSign(u,v,w)


#define EdgeGoesLeft(e) VertLeq( (e)->Dst, (e)->Org )
#define EdgeGoesRight(e) VertLeq( (e)->Org, (e)->Dst )

#define ABS(x) ((x) < 0 ? -(x) : (x))
#define VertL1dist(u,v) (ABS(u->s - v->s) + ABS(u->t - v->t))

#define VertCCW(u,v,w) tesvertCCW(u,v,w)

int tesvertLeq( TESSvertex *u, TESSvertex *v );
TESSreal	tesedgeEval( TESSvertex *u, TESSvertex *v, TESSvertex *w );
TESSreal	tesedgeSign( TESSvertex *u, TESSvertex *v, TESSvertex *w );
TESSreal	testransEval( TESSvertex *u, TESSvertex *v, TESSvertex *w );
TESSreal	testransSign( TESSvertex *u, TESSvertex *v, TESSvertex *w );
int tesvertCCW( TESSvertex *u, TESSvertex *v, TESSvertex *w );
void tesedgeIntersect( TESSvertex *o1, TESSvertex *d1, TESSvertex *o2, TESSvertex *d2, TESSvertex *v );

//-------------------------------------------------------------------------------------------------------------------------------------------------

int tesvertLeq( TESSvertex *u, TESSvertex *v )
{
	/* Returns TRUE if u is lexicographically <= v. */

	return VertLeq( u, v );
}

TESSreal tesedgeEval( TESSvertex *u, TESSvertex *v, TESSvertex *w )
{
	/* Given three vertices u,v,w such that VertLeq(u,v) && VertLeq(v,w),
	* evaluates the t-coord of the edge uw at the s-coord of the vertex v.
	* Returns v->t - (uw)(v->s), ie. the signed distance from uw to v.
	* If uw is vertical (and thus passes thru v), the result is zero.
	*
	* The calculation is extremely accurate and stable, even when v
	* is very close to u or w.  In particular if we set v->t = 0 and
	* let r be the negated result (this evaluates (uw)(v->s)), then
	* r is guaranteed to satisfy MIN(u->t,w->t) <= r <= MAX(u->t,w->t).
	*/
	TESSreal gapL, gapR;

	assert( VertLeq( u, v ) && VertLeq( v, w ));

	gapL = v->s - u->s;
	gapR = w->s - v->s;

	if( gapL + gapR > 0 ) {
		if( gapL < gapR ) {
			return (v->t - u->t) + (u->t - w->t) * (gapL / (gapL + gapR));
		} else {
			return (v->t - w->t) + (w->t - u->t) * (gapR / (gapL + gapR));
		}
	}
	/* vertical line */
	return 0;
}

TESSreal tesedgeSign( TESSvertex *u, TESSvertex *v, TESSvertex *w )
{
	/* Returns a number whose sign matches EdgeEval(u,v,w) but which
	* is cheaper to evaluate.  Returns > 0, == 0 , or < 0
	* as v is above, on, or below the edge uw.
	*/
	TESSreal gapL, gapR;

	assert( VertLeq( u, v ) && VertLeq( v, w ));

	gapL = v->s - u->s;
	gapR = w->s - v->s;

	if( gapL + gapR > 0 ) {
		return (v->t - w->t) * gapL + (v->t - u->t) * gapR;
	}
	/* vertical line */
	return 0;
}


/***********************************************************************
* Define versions of EdgeSign, EdgeEval with s and t transposed.
*/

TESSreal testransEval( TESSvertex *u, TESSvertex *v, TESSvertex *w )
{
	/* Given three vertices u,v,w such that TransLeq(u,v) && TransLeq(v,w),
	* evaluates the t-coord of the edge uw at the s-coord of the vertex v.
	* Returns v->s - (uw)(v->t), ie. the signed distance from uw to v.
	* If uw is vertical (and thus passes thru v), the result is zero.
	*
	* The calculation is extremely accurate and stable, even when v
	* is very close to u or w.  In particular if we set v->s = 0 and
	* let r be the negated result (this evaluates (uw)(v->t)), then
	* r is guaranteed to satisfy MIN(u->s,w->s) <= r <= MAX(u->s,w->s).
	*/
	TESSreal gapL, gapR;

	assert( TransLeq( u, v ) && TransLeq( v, w ));

	gapL = v->t - u->t;
	gapR = w->t - v->t;

	if( gapL + gapR > 0 ) {
		if( gapL < gapR ) {
			return (v->s - u->s) + (u->s - w->s) * (gapL / (gapL + gapR));
		} else {
			return (v->s - w->s) + (w->s - u->s) * (gapR / (gapL + gapR));
		}
	}
	/* vertical line */
	return 0;
}

TESSreal testransSign( TESSvertex *u, TESSvertex *v, TESSvertex *w )
{
	/* Returns a number whose sign matches TransEval(u,v,w) but which
	* is cheaper to evaluate.  Returns > 0, == 0 , or < 0
	* as v is above, on, or below the edge uw.
	*/
	TESSreal gapL, gapR;

	assert( TransLeq( u, v ) && TransLeq( v, w ));

	gapL = v->t - u->t;
	gapR = w->t - v->t;

	if( gapL + gapR > 0 ) {
		return (v->s - w->s) * gapL + (v->s - u->s) * gapR;
	}
	/* vertical line */
	return 0;
}


int tesvertCCW( TESSvertex *u, TESSvertex *v, TESSvertex *w )
{
	/* For almost-degenerate situations, the results are not reliable.
	* Unless the floating-point arithmetic can be performed without
	* rounding errors, *any* implementation will give incorrect results
	* on some degenerate inputs, so the client must have some way to
	* handle this situation.
	*/
	return (u->s*(v->t - w->t) + v->s*(w->t - u->t) + w->s*(u->t - v->t)) >= 0;
}

/* Given parameters a,x,b,y returns the value (b*x+a*y)/(a+b),
* or (x+y)/2 if a==b==0.  It requires that a,b >= 0, and enforces
* this in the rare case that one argument is slightly negative.
* The implementation is extremely stable numerically.
* In particular it guarantees that the result r satisfies
* MIN(x,y) <= r <= MAX(x,y), and the results are very accurate
* even when a and b differ greatly in magnitude.
*/
#define RealInterpolate(a,x,b,y)			\
	(a = (a < 0) ? 0 : a, b = (b < 0) ? 0 : b,		\
	((a <= b) ? ((b == 0) ? ((x+y) / 2)			\
	: (x + (y-x) * (a/(a+b))))	\
	: (y + (x-y) * (b/(a+b)))))

#ifndef FOR_TRITE_TEST_PROGRAM
#define Interpolate(a,x,b,y)	RealInterpolate(a,x,b,y)
#else

/* Claim: the ONLY property the sweep algorithm relies on is that
* MIN(x,y) <= r <= MAX(x,y).  This is a nasty way to test that.
*/
#include <stdlib.h>
extern int RandomInterpolate;

double Interpolate( double a, double x, double b, double y)
{
	printf("*********************%d\n",RandomInterpolate);
	if( RandomInterpolate ) {
		a = 1.2 * drand48() - 0.1;
		a = (a < 0) ? 0 : ((a > 1) ? 1 : a);
		b = 1.0 - a;
	}
	return RealInterpolate(a,x,b,y);
}

#endif

#define Swap(a,b)	if (1) { TESSvertex *t = a; a = b; b = t; } else

void tesedgeIntersect( TESSvertex *o1, TESSvertex *d1,
					  TESSvertex *o2, TESSvertex *d2,
					  TESSvertex *v )
					  /* Given edges (o1,d1) and (o2,d2), compute their point of intersection.
					  * The computed point is guaranteed to lie in the intersection of the
					  * bounding rectangles defined by each edge.
					  */
{
	TESSreal z1, z2;

	/* This is certainly not the most efficient way to find the intersection
	* of two line segments, but it is very numerically stable.
	*
	* Strategy: find the two middle vertices in the VertLeq ordering,
	* and interpolate the intersection s-value from these.  Then repeat
	* using the TransLeq ordering to find the intersection t-value.
	*/

	if( ! VertLeq( o1, d1 )) { Swap( o1, d1 ); }
	if( ! VertLeq( o2, d2 )) { Swap( o2, d2 ); }
	if( ! VertLeq( o1, o2 )) { Swap( o1, o2 ); Swap( d1, d2 ); }

	if( ! VertLeq( o2, d1 )) {
		/* Technically, no intersection -- do our best */
		v->s = (o2->s + d1->s) / 2;
	} else if( VertLeq( d1, d2 )) {
		/* Interpolate between o2 and d1 */
		z1 = EdgeEval( o1, o2, d1 );
		z2 = EdgeEval( o2, d1, d2 );
		if( z1+z2 < 0 ) { z1 = -z1; z2 = -z2; }
		v->s = Interpolate( z1, o2->s, z2, d1->s );
	} else {
		/* Interpolate between o2 and d2 */
		z1 = EdgeSign( o1, o2, d1 );
		z2 = -EdgeSign( o1, d2, d1 );
		if( z1+z2 < 0 ) { z1 = -z1; z2 = -z2; }
		v->s = Interpolate( z1, o2->s, z2, d2->s );
	}

	/* Now repeat the process for t */

	if( ! TransLeq( o1, d1 )) { Swap( o1, d1 ); }
	if( ! TransLeq( o2, d2 )) { Swap( o2, d2 ); }
	if( ! TransLeq( o1, o2 )) { Swap( o1, o2 ); Swap( d1, d2 ); }

	if( ! TransLeq( o2, d1 )) {
		/* Technically, no intersection -- do our best */
		v->t = (o2->t + d1->t) / 2;
	} else if( TransLeq( d1, d2 )) {
		/* Interpolate between o2 and d1 */
		z1 = TransEval( o1, o2, d1 );
		z2 = TransEval( o2, d1, d2 );
		if( z1+z2 < 0 ) { z1 = -z1; z2 = -z2; }
		v->t = Interpolate( z1, o2->t, z2, d1->t );
	} else {
		/* Interpolate between o2 and d2 */
		z1 = TransSign( o1, o2, d1 );
		z2 = -TransSign( o1, d2, d1 );
		if( z1+z2 < 0 ) { z1 = -z1; z2 = -z2; }
		v->t = Interpolate( z1, o2->t, z2, d2->t );
	}
}

//-------------------------------------------------------------------------------------------------------------------------------------------------

/* tessComputeInterior( tess ) computes the planar arrangement specified
* by the given contours, and further subdivides this arrangement
* into regions.  Each region is marked "inside" if it belongs
* to the polygon, according to the rule given by tess->windingRule.
* Each interior region is guaranteed be monotone.
*/
int tessComputeInterior( TESStesselator *tess );


/* The following is here *only* for access by debugging routines */

/* For each pair of adjacent edges crossing the sweep line, there is
* an ActiveRegion to represent the region between them.  The active
* regions are kept in sorted order in a dynamic dictionary.  As the
* sweep line crosses each vertex, we update the affected regions.
*/

struct ActiveRegion {
	TESShalfEdge *eUp;		/* upper edge, directed right to left */
	DictNode *nodeUp;	/* dictionary node corresponding to eUp */
	int windingNumber;	/* used to determine which regions are
							* inside the polygon */
	int inside;		/* is this region inside the polygon? */
	int sentinel;	/* marks fake edges at t = +/-infinity */
	int dirty;		/* marks regions where the upper or lower
					* edge has changed, but we haven't checked
					* whether they intersect yet */
	int fixUpperEdge;	/* marks temporary edges introduced when
						* we process a "right vertex" (one without
						* any edges leaving to the right) */
};

#define RegionBelow(r) ((ActiveRegion *) dictKey(dictPred((r)->nodeUp)))
#define RegionAbove(r) ((ActiveRegion *) dictKey(dictSucc((r)->nodeUp)))

//-------------------------------------------------------------------------------------------------------------------------------------------------

#ifdef FOR_TRITE_TEST_PROGRAM
extern void DebugEvent( TESStesselator *tess );
#else
#define DebugEvent( tess )
#endif

/*
* Invariants for the Edge Dictionary.
* - each pair of adjacent edges e2=Succ(e1) satisfies EdgeLeq(e1,e2)
*   at any valid location of the sweep event
* - if EdgeLeq(e2,e1) as well (at any valid sweep event), then e1 and e2
*   share a common endpoint
* - for each e, e->Dst has been processed, but not e->Org
* - each edge e satisfies VertLeq(e->Dst,event) && VertLeq(event,e->Org)
*   where "event" is the current sweep line event.
* - no edge e has zero length
*
* Invariants for the Mesh (the processed portion).
* - the portion of the mesh left of the sweep line is a planar graph,
*   ie. there is *some* way to embed it in the plane
* - no processed edge has zero length
* - no two processed vertices have identical coordinates
* - each "inside" region is monotone, ie. can be broken into two chains
*   of monotonically increasing vertices according to VertLeq(v1,v2)
*   - a non-invariant: these chains may intersect (very slightly)
*
* Invariants for the Sweep.
* - if none of the edges incident to the event vertex have an activeRegion
*   (ie. none of these edges are in the edge dictionary), then the vertex
*   has only right-going edges.
* - if an edge is marked "fixUpperEdge" (it is a temporary edge introduced
*   by ConnectRightVertex), then it is the only right-going edge from
*   its associated vertex.  (This says that these edges exist only
*   when it is necessary.)
*/

#define MAX(x,y)	((x) >= (y) ? (x) : (y))
#define MIN(x,y)	((x) <= (y) ? (x) : (y))

/* When we merge two edges into one, we need to compute the combined
* winding of the new edge.
*/
#define AddWinding(eDst,eSrc)	(eDst->winding += eSrc->winding, \
	eDst->Sym->winding += eSrc->Sym->winding)

static void SweepEvent( TESStesselator *tess, TESSvertex *vEvent );
static void WalkDirtyRegions( TESStesselator *tess, ActiveRegion *regUp );
static int CheckForRightSplice( TESStesselator *tess, ActiveRegion *regUp );

static int EdgeLeq( TESStesselator *tess, ActiveRegion *reg1, ActiveRegion *reg2 )
/*
* Both edges must be directed from right to left (this is the canonical
* direction for the upper edge of each region).
*
* The strategy is to evaluate a "t" value for each edge at the
* current sweep line position, given by tess->event.  The calculations
* are designed to be very stable, but of course they are not perfect.
*
* Special case: if both edge destinations are at the sweep event,
* we sort the edges by slope (they would otherwise compare equally).
*/
{
	TESSvertex *event = tess->event;
	TESShalfEdge *e1, *e2;
	TESSreal t1, t2;

	e1 = reg1->eUp;
	e2 = reg2->eUp;

	if( e1->Dst == event ) {
		if( e2->Dst == event ) {
			/* Two edges right of the sweep line which meet at the sweep event.
			* Sort them by slope.
			*/
			if( VertLeq( e1->Org, e2->Org )) {
				return EdgeSign( e2->Dst, e1->Org, e2->Org ) <= 0;
			}
			return EdgeSign( e1->Dst, e2->Org, e1->Org ) >= 0;
		}
		return EdgeSign( e2->Dst, event, e2->Org ) <= 0;
	}
	if( e2->Dst == event ) {
		return EdgeSign( e1->Dst, event, e1->Org ) >= 0;
	}

	/* General case - compute signed distance *from* e1, e2 to event */
	t1 = EdgeEval( e1->Dst, event, e1->Org );
	t2 = EdgeEval( e2->Dst, event, e2->Org );
	return (t1 >= t2);
}


static void DeleteRegion( TESStesselator *tess, ActiveRegion *reg )
{
	if( reg->fixUpperEdge ) {
		/* It was created with zero winding number, so it better be
		* deleted with zero winding number (ie. it better not get merged
		* with a real edge).
		*/
		assert( reg->eUp->winding == 0 );
	}
	reg->eUp->activeRegion = NULL;
	dictDelete( tess->dict, reg->nodeUp );
	bucketFree( tess->regionPool, reg );
}


static int FixUpperEdge( TESStesselator *tess, ActiveRegion *reg, TESShalfEdge *newEdge )
/*
* Replace an upper edge which needs fixing (see ConnectRightVertex).
*/
{
	assert( reg->fixUpperEdge );
	if ( !tessMeshDelete( tess->mesh, reg->eUp ) ) return 0;
	reg->fixUpperEdge = FALSE;
	reg->eUp = newEdge;
	newEdge->activeRegion = reg;

	return 1; 
}

static ActiveRegion *TopLeftRegion( TESStesselator *tess, ActiveRegion *reg )
{
	TESSvertex *org = reg->eUp->Org;
	TESShalfEdge *e;

	/* Find the region above the uppermost edge with the same origin */
	do {
		reg = RegionAbove( reg );
	} while( reg->eUp->Org == org );

	/* If the edge above was a temporary edge introduced by ConnectRightVertex,
	* now is the time to fix it.
	*/
	if( reg->fixUpperEdge ) {
		e = tessMeshConnect( tess->mesh, RegionBelow(reg)->eUp->Sym, reg->eUp->Lnext );
		if (e == NULL) return NULL;
		if ( !FixUpperEdge( tess, reg, e ) ) return NULL;
		reg = RegionAbove( reg );
	}
	return reg;
}

static ActiveRegion *TopRightRegion( ActiveRegion *reg )
{
	TESSvertex *dst = reg->eUp->Dst;

	/* Find the region above the uppermost edge with the same destination */
	do {
		reg = RegionAbove( reg );
	} while( reg->eUp->Dst == dst );
	return reg;
}

static ActiveRegion *AddRegionBelow( TESStesselator *tess,
									ActiveRegion *regAbove,
									TESShalfEdge *eNewUp )
/*
* Add a new active region to the sweep line, *somewhere* below "regAbove"
* (according to where the new edge belongs in the sweep-line dictionary).
* The upper edge of the new region will be "eNewUp".
* Winding number and "inside" flag are not updated.
*/
{
	ActiveRegion *regNew = (ActiveRegion *)bucketAlloc( tess->regionPool );
	if (regNew == NULL) longjmp(tess->env,1);

	regNew->eUp = eNewUp;
	regNew->nodeUp = dictInsertBefore( tess->dict, regAbove->nodeUp, regNew );
	if (regNew->nodeUp == NULL) longjmp(tess->env,1);
	regNew->fixUpperEdge = FALSE;
	regNew->sentinel = FALSE;
	regNew->dirty = FALSE;

	eNewUp->activeRegion = regNew;
	return regNew;
}

static int IsWindingInside( TESStesselator *tess, int n )
{
	switch( tess->windingRule ) {
		case TESS_WINDING_ODD:
			return (n & 1);
		case TESS_WINDING_NONZERO:
			return (n != 0);
		case TESS_WINDING_POSITIVE:
			return (n > 0);
		case TESS_WINDING_NEGATIVE:
			return (n < 0);
		case TESS_WINDING_ABS_GEQ_TWO:
			return (n >= 2) || (n <= -2);
	}
	/*LINTED*/
	assert( FALSE );
	/*NOTREACHED*/

	return( FALSE );
}


static void ComputeWinding( TESStesselator *tess, ActiveRegion *reg )
{
	reg->windingNumber = RegionAbove(reg)->windingNumber + reg->eUp->winding;
	reg->inside = IsWindingInside( tess, reg->windingNumber );
}


static void FinishRegion( TESStesselator *tess, ActiveRegion *reg )
/*
* Delete a region from the sweep line.  This happens when the upper
* and lower chains of a region meet (at a vertex on the sweep line).
* The "inside" flag is copied to the appropriate mesh face (we could
* not do this before -- since the structure of the mesh is always
* changing, this face may not have even existed until now).
*/
{
	TESShalfEdge *e = reg->eUp;
	TESSface *f = e->Lface;

	f->inside = reg->inside;
	f->anEdge = e;   /* optimization for tessMeshTessellateMonoRegion() */
	DeleteRegion( tess, reg );
}


static TESShalfEdge *FinishLeftRegions( TESStesselator *tess,
									  ActiveRegion *regFirst, ActiveRegion *regLast )
/*
* We are given a vertex with one or more left-going edges.  All affected
* edges should be in the edge dictionary.  Starting at regFirst->eUp,
* we walk down deleting all regions where both edges have the same
* origin vOrg.  At the same time we copy the "inside" flag from the
* active region to the face, since at this point each face will belong
* to at most one region (this was not necessarily true until this point
* in the sweep).  The walk stops at the region above regLast; if regLast
* is NULL we walk as far as possible.  At the same time we relink the
* mesh if necessary, so that the ordering of edges around vOrg is the
* same as in the dictionary.
*/
{
	ActiveRegion *reg, *regPrev;
	TESShalfEdge *e, *ePrev;

	regPrev = regFirst;
	ePrev = regFirst->eUp;
	while( regPrev != regLast ) {
		regPrev->fixUpperEdge = FALSE;	/* placement was OK */
		reg = RegionBelow( regPrev );
		e = reg->eUp;
		if( e->Org != ePrev->Org ) {
			if( ! reg->fixUpperEdge ) {
				/* Remove the last left-going edge.  Even though there are no further
				* edges in the dictionary with this origin, there may be further
				* such edges in the mesh (if we are adding left edges to a vertex
				* that has already been processed).  Thus it is important to call
				* FinishRegion rather than just DeleteRegion.
				*/
				FinishRegion( tess, regPrev );
				break;
			}
			/* If the edge below was a temporary edge introduced by
			* ConnectRightVertex, now is the time to fix it.
			*/
			e = tessMeshConnect( tess->mesh, ePrev->Lprev, e->Sym );
			if (e == NULL) longjmp(tess->env,1);
			if ( !FixUpperEdge( tess, reg, e ) ) longjmp(tess->env,1);
		}

		/* Relink edges so that ePrev->Onext == e */
		if( ePrev->Onext != e ) {
			if ( !tessMeshSplice( tess->mesh, e->Oprev, e ) ) longjmp(tess->env,1);
			if ( !tessMeshSplice( tess->mesh, ePrev, e ) ) longjmp(tess->env,1);
		}
		FinishRegion( tess, regPrev );	/* may change reg->eUp */
		ePrev = reg->eUp;
		regPrev = reg;
	}
	return ePrev;
}


static void AddRightEdges( TESStesselator *tess, ActiveRegion *regUp,
						  TESShalfEdge *eFirst, TESShalfEdge *eLast, TESShalfEdge *eTopLeft,
						  int cleanUp )
/*
* Purpose: insert right-going edges into the edge dictionary, and update
* winding numbers and mesh connectivity appropriately.  All right-going
* edges share a common origin vOrg.  Edges are inserted CCW starting at
* eFirst; the last edge inserted is eLast->Oprev.  If vOrg has any
* left-going edges already processed, then eTopLeft must be the edge
* such that an imaginary upward vertical segment from vOrg would be
* contained between eTopLeft->Oprev and eTopLeft; otherwise eTopLeft
* should be NULL.
*/
{
	ActiveRegion *reg, *regPrev;
	TESShalfEdge *e, *ePrev;
	int firstTime = TRUE;

	/* Insert the new right-going edges in the dictionary */
	e = eFirst;
	do {
		assert( VertLeq( e->Org, e->Dst ));
		AddRegionBelow( tess, regUp, e->Sym );
		e = e->Onext;
	} while ( e != eLast );

	/* Walk *all* right-going edges from e->Org, in the dictionary order,
	* updating the winding numbers of each region, and re-linking the mesh
	* edges to match the dictionary ordering (if necessary).
	*/
	if( eTopLeft == NULL ) {
		eTopLeft = RegionBelow( regUp )->eUp->Rprev;
	}
	regPrev = regUp;
	ePrev = eTopLeft;
	for( ;; ) {
		reg = RegionBelow( regPrev );
		e = reg->eUp->Sym;
		if( e->Org != ePrev->Org ) break;

		if( e->Onext != ePrev ) {
			/* Unlink e from its current position, and relink below ePrev */
			if ( !tessMeshSplice( tess->mesh, e->Oprev, e ) ) longjmp(tess->env,1);
			if ( !tessMeshSplice( tess->mesh, ePrev->Oprev, e ) ) longjmp(tess->env,1);
		}
		/* Compute the winding number and "inside" flag for the new regions */
		reg->windingNumber = regPrev->windingNumber - e->winding;
		reg->inside = IsWindingInside( tess, reg->windingNumber );

		/* Check for two outgoing edges with same slope -- process these
		* before any intersection tests (see example in tessComputeInterior).
		*/
		regPrev->dirty = TRUE;
		if( ! firstTime && CheckForRightSplice( tess, regPrev )) {
			AddWinding( e, ePrev );
			DeleteRegion( tess, regPrev );
			if ( !tessMeshDelete( tess->mesh, ePrev ) ) longjmp(tess->env,1);
		}
		firstTime = FALSE;
		regPrev = reg;
		ePrev = e;
	}
	regPrev->dirty = TRUE;
	assert( regPrev->windingNumber - e->winding == reg->windingNumber );

	if( cleanUp ) {
		/* Check for intersections between newly adjacent edges. */
		WalkDirtyRegions( tess, regPrev );
	}
}


static void SpliceMergeVertices( TESStesselator *tess, TESShalfEdge *e1,
								TESShalfEdge *e2 )
/*
* Two vertices with idential coordinates are combined into one.
* e1->Org is kept, while e2->Org is discarded.
*/
{
	if ( !tessMeshSplice( tess->mesh, e1, e2 ) ) longjmp(tess->env,1); 
}

static void VertexWeights( TESSvertex *isect, TESSvertex *org, TESSvertex *dst,
						  TESSreal *weights )
/*
* Find some weights which describe how the intersection vertex is
* a linear combination of "org" and "dest".  Each of the two edges
* which generated "isect" is allocated 50% of the weight; each edge
* splits the weight between its org and dst according to the
* relative distance to "isect".
*/
{
	TESSreal t1 = VertL1dist( org, isect );
	TESSreal t2 = VertL1dist( dst, isect );

	weights[0] = (TESSreal)0.5 * t2 / (t1 + t2);
	weights[1] = (TESSreal)0.5 * t1 / (t1 + t2);
	isect->coords[0] += weights[0]*org->coords[0] + weights[1]*dst->coords[0];
	isect->coords[1] += weights[0]*org->coords[1] + weights[1]*dst->coords[1];
	isect->coords[2] += weights[0]*org->coords[2] + weights[1]*dst->coords[2];
}


static void GetIntersectData( TESStesselator *tess, TESSvertex *isect,
							 TESSvertex *orgUp, TESSvertex *dstUp,
							 TESSvertex *orgLo, TESSvertex *dstLo )
 /*
 * We've computed a new intersection point, now we need a "data" pointer
 * from the user so that we can refer to this new vertex in the
 * rendering callbacks.
 */
{
	TESSreal weights[4];
	TESS_NOTUSED( tess );

	isect->coords[0] = isect->coords[1] = isect->coords[2] = 0;
	isect->idx = TESS_UNDEF;
	VertexWeights( isect, orgUp, dstUp, &weights[0] );
	VertexWeights( isect, orgLo, dstLo, &weights[2] );
}

static int CheckForRightSplice( TESStesselator *tess, ActiveRegion *regUp )
/*
* Check the upper and lower edge of "regUp", to make sure that the
* eUp->Org is above eLo, or eLo->Org is below eUp (depending on which
* origin is leftmost).
*
* The main purpose is to splice right-going edges with the same
* dest vertex and nearly identical slopes (ie. we can't distinguish
* the slopes numerically).  However the splicing can also help us
* to recover from numerical errors.  For example, suppose at one
* point we checked eUp and eLo, and decided that eUp->Org is barely
* above eLo.  Then later, we split eLo into two edges (eg. from
* a splice operation like this one).  This can change the result of
* our test so that now eUp->Org is incident to eLo, or barely below it.
* We must correct this condition to maintain the dictionary invariants.
*
* One possibility is to check these edges for intersection again
* (ie. CheckForIntersect).  This is what we do if possible.  However
* CheckForIntersect requires that tess->event lies between eUp and eLo,
* so that it has something to fall back on when the intersection
* calculation gives us an unusable answer.  So, for those cases where
* we can't check for intersection, this routine fixes the problem
* by just splicing the offending vertex into the other edge.
* This is a guaranteed solution, no matter how degenerate things get.
* Basically this is a combinatorial solution to a numerical problem.
*/
{
	ActiveRegion *regLo = RegionBelow(regUp);
	TESShalfEdge *eUp = regUp->eUp;
	TESShalfEdge *eLo = regLo->eUp;

	if( VertLeq( eUp->Org, eLo->Org )) {
		if( EdgeSign( eLo->Dst, eUp->Org, eLo->Org ) > 0 ) return FALSE;

		/* eUp->Org appears to be below eLo */
		if( ! VertEq( eUp->Org, eLo->Org )) {
			/* Splice eUp->Org into eLo */
			if ( tessMeshSplitEdge( tess->mesh, eLo->Sym ) == NULL) longjmp(tess->env,1);
			if ( !tessMeshSplice( tess->mesh, eUp, eLo->Oprev ) ) longjmp(tess->env,1);
			regUp->dirty = regLo->dirty = TRUE;

		} else if( eUp->Org != eLo->Org ) {
			/* merge the two vertices, discarding eUp->Org */
			pqDelete( tess->pq, eUp->Org->pqHandle );
			SpliceMergeVertices( tess, eLo->Oprev, eUp );
		}
	} else {
		if( EdgeSign( eUp->Dst, eLo->Org, eUp->Org ) < 0 ) return FALSE;

		/* eLo->Org appears to be above eUp, so splice eLo->Org into eUp */
		RegionAbove(regUp)->dirty = regUp->dirty = TRUE;
		if (tessMeshSplitEdge( tess->mesh, eUp->Sym ) == NULL) longjmp(tess->env,1);
		if ( !tessMeshSplice( tess->mesh, eLo->Oprev, eUp ) ) longjmp(tess->env,1);
	}
	return TRUE;
}

static int CheckForLeftSplice( TESStesselator *tess, ActiveRegion *regUp )
/*
* Check the upper and lower edge of "regUp", to make sure that the
* eUp->Dst is above eLo, or eLo->Dst is below eUp (depending on which
* destination is rightmost).
*
* Theoretically, this should always be true.  However, splitting an edge
* into two pieces can change the results of previous tests.  For example,
* suppose at one point we checked eUp and eLo, and decided that eUp->Dst
* is barely above eLo.  Then later, we split eLo into two edges (eg. from
* a splice operation like this one).  This can change the result of
* the test so that now eUp->Dst is incident to eLo, or barely below it.
* We must correct this condition to maintain the dictionary invariants
* (otherwise new edges might get inserted in the wrong place in the
* dictionary, and bad stuff will happen).
*
* We fix the problem by just splicing the offending vertex into the
* other edge.
*/
{
	ActiveRegion *regLo = RegionBelow(regUp);
	TESShalfEdge *eUp = regUp->eUp;
	TESShalfEdge *eLo = regLo->eUp;
	TESShalfEdge *e;

	assert( ! VertEq( eUp->Dst, eLo->Dst ));

	if( VertLeq( eUp->Dst, eLo->Dst )) {
		if( EdgeSign( eUp->Dst, eLo->Dst, eUp->Org ) < 0 ) return FALSE;

		/* eLo->Dst is above eUp, so splice eLo->Dst into eUp */
		RegionAbove(regUp)->dirty = regUp->dirty = TRUE;
		e = tessMeshSplitEdge( tess->mesh, eUp );
		if (e == NULL) longjmp(tess->env,1);
		if ( !tessMeshSplice( tess->mesh, eLo->Sym, e ) ) longjmp(tess->env,1);
		e->Lface->inside = regUp->inside;
	} else {
		if( EdgeSign( eLo->Dst, eUp->Dst, eLo->Org ) > 0 ) return FALSE;

		/* eUp->Dst is below eLo, so splice eUp->Dst into eLo */
		regUp->dirty = regLo->dirty = TRUE;
		e = tessMeshSplitEdge( tess->mesh, eLo );
		if (e == NULL) longjmp(tess->env,1);    
		if ( !tessMeshSplice( tess->mesh, eUp->Lnext, eLo->Sym ) ) longjmp(tess->env,1);
		e->Rface->inside = regUp->inside;
	}
	return TRUE;
}


static int CheckForIntersect( TESStesselator *tess, ActiveRegion *regUp )
/*
* Check the upper and lower edges of the given region to see if
* they intersect.  If so, create the intersection and add it
* to the data structures.
*
* Returns TRUE if adding the new intersection resulted in a recursive
* call to AddRightEdges(); in this case all "dirty" regions have been
* checked for intersections, and possibly regUp has been deleted.
*/
{
	ActiveRegion *regLo = RegionBelow(regUp);
	TESShalfEdge *eUp = regUp->eUp;
	TESShalfEdge *eLo = regLo->eUp;
	TESSvertex *orgUp = eUp->Org;
	TESSvertex *orgLo = eLo->Org;
	TESSvertex *dstUp = eUp->Dst;
	TESSvertex *dstLo = eLo->Dst;
	TESSreal tMinUp, tMaxLo;
	TESSvertex isect, *orgMin;
	TESShalfEdge *e;

	assert( ! VertEq( dstLo, dstUp ));
	assert( EdgeSign( dstUp, tess->event, orgUp ) <= 0 );
	assert( EdgeSign( dstLo, tess->event, orgLo ) >= 0 );
	assert( orgUp != tess->event && orgLo != tess->event );
	assert( ! regUp->fixUpperEdge && ! regLo->fixUpperEdge );

	if( orgUp == orgLo ) return FALSE;	/* right endpoints are the same */

	tMinUp = MIN( orgUp->t, dstUp->t );
	tMaxLo = MAX( orgLo->t, dstLo->t );
	if( tMinUp > tMaxLo ) return FALSE;	/* t ranges do not overlap */

	if( VertLeq( orgUp, orgLo )) {
		if( EdgeSign( dstLo, orgUp, orgLo ) > 0 ) return FALSE;
	} else {
		if( EdgeSign( dstUp, orgLo, orgUp ) < 0 ) return FALSE;
	}

	/* At this point the edges intersect, at least marginally */
	DebugEvent( tess );

	tesedgeIntersect( dstUp, orgUp, dstLo, orgLo, &isect );
	/* The following properties are guaranteed: */
	assert( MIN( orgUp->t, dstUp->t ) <= isect.t );
	assert( isect.t <= MAX( orgLo->t, dstLo->t ));
	assert( MIN( dstLo->s, dstUp->s ) <= isect.s );
	assert( isect.s <= MAX( orgLo->s, orgUp->s ));

	if( VertLeq( &isect, tess->event )) {
		/* The intersection point lies slightly to the left of the sweep line,
		* so move it until it''s slightly to the right of the sweep line.
		* (If we had perfect numerical precision, this would never happen
		* in the first place).  The easiest and safest thing to do is
		* replace the intersection by tess->event.
		*/
		isect.s = tess->event->s;
		isect.t = tess->event->t;
	}
	/* Similarly, if the computed intersection lies to the right of the
	* rightmost origin (which should rarely happen), it can cause
	* unbelievable inefficiency on sufficiently degenerate inputs.
	* (If you have the test program, try running test54.d with the
	* "X zoom" option turned on).
	*/
	orgMin = VertLeq( orgUp, orgLo ) ? orgUp : orgLo;
	if( VertLeq( orgMin, &isect )) {
		isect.s = orgMin->s;
		isect.t = orgMin->t;
	}

	if( VertEq( &isect, orgUp ) || VertEq( &isect, orgLo )) {
		/* Easy case -- intersection at one of the right endpoints */
		(void) CheckForRightSplice( tess, regUp );
		return FALSE;
	}

	if(    (! VertEq( dstUp, tess->event )
		&& EdgeSign( dstUp, tess->event, &isect ) >= 0)
		|| (! VertEq( dstLo, tess->event )
		&& EdgeSign( dstLo, tess->event, &isect ) <= 0 ))
	{
		/* Very unusual -- the new upper or lower edge would pass on the
		* wrong side of the sweep event, or through it.  This can happen
		* due to very small numerical errors in the intersection calculation.
		*/
		if( dstLo == tess->event ) {
			/* Splice dstLo into eUp, and process the new region(s) */
			if (tessMeshSplitEdge( tess->mesh, eUp->Sym ) == NULL) longjmp(tess->env,1);
			if ( !tessMeshSplice( tess->mesh, eLo->Sym, eUp ) ) longjmp(tess->env,1);
			regUp = TopLeftRegion( tess, regUp );
			if (regUp == NULL) longjmp(tess->env,1);
			eUp = RegionBelow(regUp)->eUp;
			FinishLeftRegions( tess, RegionBelow(regUp), regLo );
			AddRightEdges( tess, regUp, eUp->Oprev, eUp, eUp, TRUE );
			return TRUE;
		}
		if( dstUp == tess->event ) {
			/* Splice dstUp into eLo, and process the new region(s) */
			if (tessMeshSplitEdge( tess->mesh, eLo->Sym ) == NULL) longjmp(tess->env,1);
			if ( !tessMeshSplice( tess->mesh, eUp->Lnext, eLo->Oprev ) ) longjmp(tess->env,1); 
			regLo = regUp;
			regUp = TopRightRegion( regUp );
			e = RegionBelow(regUp)->eUp->Rprev;
			regLo->eUp = eLo->Oprev;
			eLo = FinishLeftRegions( tess, regLo, NULL );
			AddRightEdges( tess, regUp, eLo->Onext, eUp->Rprev, e, TRUE );
			return TRUE;
		}
		/* Special case: called from ConnectRightVertex.  If either
		* edge passes on the wrong side of tess->event, split it
		* (and wait for ConnectRightVertex to splice it appropriately).
		*/
		if( EdgeSign( dstUp, tess->event, &isect ) >= 0 ) {
			RegionAbove(regUp)->dirty = regUp->dirty = TRUE;
			if (tessMeshSplitEdge( tess->mesh, eUp->Sym ) == NULL) longjmp(tess->env,1);
			eUp->Org->s = tess->event->s;
			eUp->Org->t = tess->event->t;
		}
		if( EdgeSign( dstLo, tess->event, &isect ) <= 0 ) {
			regUp->dirty = regLo->dirty = TRUE;
			if (tessMeshSplitEdge( tess->mesh, eLo->Sym ) == NULL) longjmp(tess->env,1);
			eLo->Org->s = tess->event->s;
			eLo->Org->t = tess->event->t;
		}
		/* leave the rest for ConnectRightVertex */
		return FALSE;
	}

	/* General case -- split both edges, splice into new vertex.
	* When we do the splice operation, the order of the arguments is
	* arbitrary as far as correctness goes.  However, when the operation
	* creates a new face, the work done is proportional to the size of
	* the new face.  We expect the faces in the processed part of
	* the mesh (ie. eUp->Lface) to be smaller than the faces in the
	* unprocessed original contours (which will be eLo->Oprev->Lface).
	*/
	if (tessMeshSplitEdge( tess->mesh, eUp->Sym ) == NULL) longjmp(tess->env,1);
	if (tessMeshSplitEdge( tess->mesh, eLo->Sym ) == NULL) longjmp(tess->env,1);
	if ( !tessMeshSplice( tess->mesh, eLo->Oprev, eUp ) ) longjmp(tess->env,1);
	eUp->Org->s = isect.s;
	eUp->Org->t = isect.t;
	eUp->Org->pqHandle = pqInsert( &tess->alloc, tess->pq, eUp->Org );
	if (eUp->Org->pqHandle == INV_HANDLE) {
		pqDeletePriorityQ( &tess->alloc, tess->pq );
		tess->pq = NULL;
		longjmp(tess->env,1);
	}
	GetIntersectData( tess, eUp->Org, orgUp, dstUp, orgLo, dstLo );
	RegionAbove(regUp)->dirty = regUp->dirty = regLo->dirty = TRUE;
	return FALSE;
}

static void WalkDirtyRegions( TESStesselator *tess, ActiveRegion *regUp )
/*
* When the upper or lower edge of any region changes, the region is
* marked "dirty".  This routine walks through all the dirty regions
* and makes sure that the dictionary invariants are satisfied
* (see the comments at the beginning of this file).  Of course
* new dirty regions can be created as we make changes to restore
* the invariants.
*/
{
	ActiveRegion *regLo = RegionBelow(regUp);
	TESShalfEdge *eUp, *eLo;

	for( ;; ) {
		/* Find the lowest dirty region (we walk from the bottom up). */
		while( regLo->dirty ) {
			regUp = regLo;
			regLo = RegionBelow(regLo);
		}
		if( ! regUp->dirty ) {
			regLo = regUp;
			regUp = RegionAbove( regUp );
			if( regUp == NULL || ! regUp->dirty ) {
				/* We've walked all the dirty regions */
				return;
			}
		}
		regUp->dirty = FALSE;
		eUp = regUp->eUp;
		eLo = regLo->eUp;

		if( eUp->Dst != eLo->Dst ) {
			/* Check that the edge ordering is obeyed at the Dst vertices. */
			if( CheckForLeftSplice( tess, regUp )) {

				/* If the upper or lower edge was marked fixUpperEdge, then
				* we no longer need it (since these edges are needed only for
				* vertices which otherwise have no right-going edges).
				*/
				if( regLo->fixUpperEdge ) {
					DeleteRegion( tess, regLo );
					if ( !tessMeshDelete( tess->mesh, eLo ) ) longjmp(tess->env,1);
					regLo = RegionBelow( regUp );
					eLo = regLo->eUp;
				} else if( regUp->fixUpperEdge ) {
					DeleteRegion( tess, regUp );
					if ( !tessMeshDelete( tess->mesh, eUp ) ) longjmp(tess->env,1);
					regUp = RegionAbove( regLo );
					eUp = regUp->eUp;
				}
			}
		}
		if( eUp->Org != eLo->Org ) {
			if(    eUp->Dst != eLo->Dst
				&& ! regUp->fixUpperEdge && ! regLo->fixUpperEdge
				&& (eUp->Dst == tess->event || eLo->Dst == tess->event) )
			{
				/* When all else fails in CheckForIntersect(), it uses tess->event
				* as the intersection location.  To make this possible, it requires
				* that tess->event lie between the upper and lower edges, and also
				* that neither of these is marked fixUpperEdge (since in the worst
				* case it might splice one of these edges into tess->event, and
				* violate the invariant that fixable edges are the only right-going
				* edge from their associated vertex).
				*/
				if( CheckForIntersect( tess, regUp )) {
					/* WalkDirtyRegions() was called recursively; we're done */
					return;
				}
			} else {
				/* Even though we can't use CheckForIntersect(), the Org vertices
				* may violate the dictionary edge ordering.  Check and correct this.
				*/
				(void) CheckForRightSplice( tess, regUp );
			}
		}
		if( eUp->Org == eLo->Org && eUp->Dst == eLo->Dst ) {
			/* A degenerate loop consisting of only two edges -- delete it. */
			AddWinding( eLo, eUp );
			DeleteRegion( tess, regUp );
			if ( !tessMeshDelete( tess->mesh, eUp ) ) longjmp(tess->env,1);
			regUp = RegionAbove( regLo );
		}
	}
}


static void ConnectRightVertex( TESStesselator *tess, ActiveRegion *regUp,
							   TESShalfEdge *eBottomLeft )
/*
* Purpose: connect a "right" vertex vEvent (one where all edges go left)
* to the unprocessed portion of the mesh.  Since there are no right-going
* edges, two regions (one above vEvent and one below) are being merged
* into one.  "regUp" is the upper of these two regions.
*
* There are two reasons for doing this (adding a right-going edge):
*  - if the two regions being merged are "inside", we must add an edge
*    to keep them separated (the combined region would not be monotone).
*  - in any case, we must leave some record of vEvent in the dictionary,
*    so that we can merge vEvent with features that we have not seen yet.
*    For example, maybe there is a vertical edge which passes just to
*    the right of vEvent; we would like to splice vEvent into this edge.
*
* However, we don't want to connect vEvent to just any vertex.  We don''t
* want the new edge to cross any other edges; otherwise we will create
* intersection vertices even when the input data had no self-intersections.
* (This is a bad thing; if the user's input data has no intersections,
* we don't want to generate any false intersections ourselves.)
*
* Our eventual goal is to connect vEvent to the leftmost unprocessed
* vertex of the combined region (the union of regUp and regLo).
* But because of unseen vertices with all right-going edges, and also
* new vertices which may be created by edge intersections, we don''t
* know where that leftmost unprocessed vertex is.  In the meantime, we
* connect vEvent to the closest vertex of either chain, and mark the region
* as "fixUpperEdge".  This flag says to delete and reconnect this edge
* to the next processed vertex on the boundary of the combined region.
* Quite possibly the vertex we connected to will turn out to be the
* closest one, in which case we won''t need to make any changes.
*/
{
	TESShalfEdge *eNew;
	TESShalfEdge *eTopLeft = eBottomLeft->Onext;
	ActiveRegion *regLo = RegionBelow(regUp);
	TESShalfEdge *eUp = regUp->eUp;
	TESShalfEdge *eLo = regLo->eUp;
	int degenerate = FALSE;

	if( eUp->Dst != eLo->Dst ) {
		(void) CheckForIntersect( tess, regUp );
	}

	/* Possible new degeneracies: upper or lower edge of regUp may pass
	* through vEvent, or may coincide with new intersection vertex
	*/
	if( VertEq( eUp->Org, tess->event )) {
		if ( !tessMeshSplice( tess->mesh, eTopLeft->Oprev, eUp ) ) longjmp(tess->env,1);
		regUp = TopLeftRegion( tess, regUp );
		if (regUp == NULL) longjmp(tess->env,1);
		eTopLeft = RegionBelow( regUp )->eUp;
		FinishLeftRegions( tess, RegionBelow(regUp), regLo );
		degenerate = TRUE;
	}
	if( VertEq( eLo->Org, tess->event )) {
		if ( !tessMeshSplice( tess->mesh, eBottomLeft, eLo->Oprev ) ) longjmp(tess->env,1);
		eBottomLeft = FinishLeftRegions( tess, regLo, NULL );
		degenerate = TRUE;
	}
	if( degenerate ) {
		AddRightEdges( tess, regUp, eBottomLeft->Onext, eTopLeft, eTopLeft, TRUE );
		return;
	}

	/* Non-degenerate situation -- need to add a temporary, fixable edge.
	* Connect to the closer of eLo->Org, eUp->Org.
	*/
	if( VertLeq( eLo->Org, eUp->Org )) {
		eNew = eLo->Oprev;
	} else {
		eNew = eUp;
	}
	eNew = tessMeshConnect( tess->mesh, eBottomLeft->Lprev, eNew );
	if (eNew == NULL) longjmp(tess->env,1);

	/* Prevent cleanup, otherwise eNew might disappear before we've even
	* had a chance to mark it as a temporary edge.
	*/
	AddRightEdges( tess, regUp, eNew, eNew->Onext, eNew->Onext, FALSE );
	eNew->Sym->activeRegion->fixUpperEdge = TRUE;
	WalkDirtyRegions( tess, regUp );
}

/* Because vertices at exactly the same location are merged together
* before we process the sweep event, some degenerate cases can't occur.
* However if someone eventually makes the modifications required to
* merge features which are close together, the cases below marked
* TOLERANCE_NONZERO will be useful.  They were debugged before the
* code to merge identical vertices in the main loop was added.
*/
#define TOLERANCE_NONZERO	FALSE

static void ConnectLeftDegenerate( TESStesselator *tess,
								  ActiveRegion *regUp, TESSvertex *vEvent )
/*
* The event vertex lies exacty on an already-processed edge or vertex.
* Adding the new vertex involves splicing it into the already-processed
* part of the mesh.
*/
{
	TESShalfEdge *e, *eTopLeft, *eTopRight, *eLast;
	ActiveRegion *reg;

	e = regUp->eUp;
	if( VertEq( e->Org, vEvent )) {
		/* e->Org is an unprocessed vertex - just combine them, and wait
		* for e->Org to be pulled from the queue
		*/
		assert( TOLERANCE_NONZERO );
		SpliceMergeVertices( tess, e, vEvent->anEdge );
		return;
	}

	if( ! VertEq( e->Dst, vEvent )) {
		/* General case -- splice vEvent into edge e which passes through it */
		if (tessMeshSplitEdge( tess->mesh, e->Sym ) == NULL) longjmp(tess->env,1);
		if( regUp->fixUpperEdge ) {
			/* This edge was fixable -- delete unused portion of original edge */
			if ( !tessMeshDelete( tess->mesh, e->Onext ) ) longjmp(tess->env,1);
			regUp->fixUpperEdge = FALSE;
		}
		if ( !tessMeshSplice( tess->mesh, vEvent->anEdge, e ) ) longjmp(tess->env,1);
		SweepEvent( tess, vEvent );	/* recurse */
		return;
	}

	/* vEvent coincides with e->Dst, which has already been processed.
	* Splice in the additional right-going edges.
	*/
	assert( TOLERANCE_NONZERO );
	regUp = TopRightRegion( regUp );
	reg = RegionBelow( regUp );
	eTopRight = reg->eUp->Sym;
	eTopLeft = eLast = eTopRight->Onext;
	if( reg->fixUpperEdge ) {
		/* Here e->Dst has only a single fixable edge going right.
		* We can delete it since now we have some real right-going edges.
		*/
		assert( eTopLeft != eTopRight );   /* there are some left edges too */
		DeleteRegion( tess, reg );
		if ( !tessMeshDelete( tess->mesh, eTopRight ) ) longjmp(tess->env,1);
		eTopRight = eTopLeft->Oprev;
	}
	if ( !tessMeshSplice( tess->mesh, vEvent->anEdge, eTopRight ) ) longjmp(tess->env,1);
	if( ! EdgeGoesLeft( eTopLeft )) {
		/* e->Dst had no left-going edges -- indicate this to AddRightEdges() */
		eTopLeft = NULL;
	}
	AddRightEdges( tess, regUp, eTopRight->Onext, eLast, eTopLeft, TRUE );
}


static void ConnectLeftVertex( TESStesselator *tess, TESSvertex *vEvent )
/*
* Purpose: connect a "left" vertex (one where both edges go right)
* to the processed portion of the mesh.  Let R be the active region
* containing vEvent, and let U and L be the upper and lower edge
* chains of R.  There are two possibilities:
*
* - the normal case: split R into two regions, by connecting vEvent to
*   the rightmost vertex of U or L lying to the left of the sweep line
*
* - the degenerate case: if vEvent is close enough to U or L, we
*   merge vEvent into that edge chain.  The subcases are:
*	- merging with the rightmost vertex of U or L
*	- merging with the active edge of U or L
*	- merging with an already-processed portion of U or L
*/
{
	ActiveRegion *regUp, *regLo, *reg;
	TESShalfEdge *eUp, *eLo, *eNew;
	ActiveRegion tmp;

	/* assert( vEvent->anEdge->Onext->Onext == vEvent->anEdge ); */

	/* Get a pointer to the active region containing vEvent */
	tmp.eUp = vEvent->anEdge->Sym;
	/* __GL_DICTLISTKEY */ /* tessDictListSearch */
	regUp = (ActiveRegion *)dictKey( dictSearch( tess->dict, &tmp ));
	regLo = RegionBelow( regUp );
	if( !regLo ) {
		// This may happen if the input polygon is coplanar.
		return;
	}
	eUp = regUp->eUp;
	eLo = regLo->eUp;

	/* Try merging with U or L first */
	if( EdgeSign( eUp->Dst, vEvent, eUp->Org ) == 0 ) {
		ConnectLeftDegenerate( tess, regUp, vEvent );
		return;
	}

	/* Connect vEvent to rightmost processed vertex of either chain.
	* e->Dst is the vertex that we will connect to vEvent.
	*/
	reg = VertLeq( eLo->Dst, eUp->Dst ) ? regUp : regLo;

	if( regUp->inside || reg->fixUpperEdge) {
		if( reg == regUp ) {
			eNew = tessMeshConnect( tess->mesh, vEvent->anEdge->Sym, eUp->Lnext );
			if (eNew == NULL) longjmp(tess->env,1);
		} else {
			TESShalfEdge *tempHalfEdge= tessMeshConnect( tess->mesh, eLo->Dnext, vEvent->anEdge);
			if (tempHalfEdge == NULL) longjmp(tess->env,1);

			eNew = tempHalfEdge->Sym;
		}
		if( reg->fixUpperEdge ) {
			if ( !FixUpperEdge( tess, reg, eNew ) ) longjmp(tess->env,1);
		} else {
			ComputeWinding( tess, AddRegionBelow( tess, regUp, eNew ));
		}
		SweepEvent( tess, vEvent );
	} else {
		/* The new vertex is in a region which does not belong to the polygon.
		* We don''t need to connect this vertex to the rest of the mesh.
		*/
		AddRightEdges( tess, regUp, vEvent->anEdge, vEvent->anEdge, NULL, TRUE );
	}
}


static void SweepEvent( TESStesselator *tess, TESSvertex *vEvent )
/*
* Does everything necessary when the sweep line crosses a vertex.
* Updates the mesh and the edge dictionary.
*/
{
	ActiveRegion *regUp, *reg;
	TESShalfEdge *e, *eTopLeft, *eBottomLeft;

	tess->event = vEvent;		/* for access in EdgeLeq() */
	DebugEvent( tess );

	/* Check if this vertex is the right endpoint of an edge that is
	* already in the dictionary.  In this case we don't need to waste
	* time searching for the location to insert new edges.
	*/
	e = vEvent->anEdge;
	while( e->activeRegion == NULL ) {
		e = e->Onext;
		if( e == vEvent->anEdge ) {
			/* All edges go right -- not incident to any processed edges */
			ConnectLeftVertex( tess, vEvent );
			return;
		}
	}

	/* Processing consists of two phases: first we "finish" all the
	* active regions where both the upper and lower edges terminate
	* at vEvent (ie. vEvent is closing off these regions).
	* We mark these faces "inside" or "outside" the polygon according
	* to their winding number, and delete the edges from the dictionary.
	* This takes care of all the left-going edges from vEvent.
	*/
	regUp = TopLeftRegion( tess, e->activeRegion );
	if (regUp == NULL) longjmp(tess->env,1);
	reg = RegionBelow( regUp );
	eTopLeft = reg->eUp;
	eBottomLeft = FinishLeftRegions( tess, reg, NULL );

	/* Next we process all the right-going edges from vEvent.  This
	* involves adding the edges to the dictionary, and creating the
	* associated "active regions" which record information about the
	* regions between adjacent dictionary edges.
	*/
	if( eBottomLeft->Onext == eTopLeft ) {
		/* No right-going edges -- add a temporary "fixable" edge */
		ConnectRightVertex( tess, regUp, eBottomLeft );
	} else {
		AddRightEdges( tess, regUp, eBottomLeft->Onext, eTopLeft, eTopLeft, TRUE );
	}
}


/* Make the sentinel coordinates big enough that they will never be
* merged with real input features.
*/

static void AddSentinel( TESStesselator *tess, TESSreal smin, TESSreal smax, TESSreal t )
/*
* We add two sentinel edges above and below all other edges,
* to avoid special cases at the top and bottom.
*/
{
	TESShalfEdge *e;
	ActiveRegion *reg = (ActiveRegion *)bucketAlloc( tess->regionPool );
	if (reg == NULL) longjmp(tess->env,1);

	e = tessMeshMakeEdge( tess->mesh );
	if (e == NULL) longjmp(tess->env,1);

	e->Org->s = smax;
	e->Org->t = t;
	e->Dst->s = smin;
	e->Dst->t = t;
	tess->event = e->Dst;		/* initialize it */

	reg->eUp = e;
	reg->windingNumber = 0;
	reg->inside = FALSE;
	reg->fixUpperEdge = FALSE;
	reg->sentinel = TRUE;
	reg->dirty = FALSE;
	reg->nodeUp = dictInsert( tess->dict, reg );
	if (reg->nodeUp == NULL) longjmp(tess->env,1);
}


static void InitEdgeDict( TESStesselator *tess )
/*
* We maintain an ordering of edge intersections with the sweep line.
* This order is maintained in a dynamic dictionary.
*/
{
	TESSreal w, h;
	TESSreal smin, smax, tmin, tmax;

	tess->dict = dictNewDict( &tess->alloc, tess, (int (*)(void *, DictKey, DictKey)) EdgeLeq );
	if (tess->dict == NULL) longjmp(tess->env,1);

	w = (tess->bmax[0] - tess->bmin[0]);
	h = (tess->bmax[1] - tess->bmin[1]);

        /* If the bbox is empty, ensure that sentinels are not coincident by
           slightly enlarging it. */
	smin = tess->bmin[0] - (w > 0 ? w : (TESSreal)0.01f);
        smax = tess->bmax[0] + (w > 0 ? w : (TESSreal)0.01f);
        tmin = tess->bmin[1] - (h > 0 ? h : (TESSreal)0.01f);
        tmax = tess->bmax[1] + (h > 0 ? h : (TESSreal)0.01f);

	AddSentinel( tess, smin, smax, tmin );
	AddSentinel( tess, smin, smax, tmax );
}


static void DoneEdgeDict( TESStesselator *tess )
{
	ActiveRegion *reg;
	#ifndef NDEBUG
	int fixedEdges = 0;
	#endif

	while( (reg = (ActiveRegion *)dictKey( dictMin( tess->dict ))) != NULL ) {
		/*
		* At the end of all processing, the dictionary should contain
		* only the two sentinel edges, plus at most one "fixable" edge
		* created by ConnectRightVertex().
		*/
		if( ! reg->sentinel ) {
			assert( reg->fixUpperEdge );
			#ifndef NDEBUG
			assert( ++fixedEdges == 1 );
			#endif
		}
		assert( reg->windingNumber == 0 );
		DeleteRegion( tess, reg );
		/*    tessMeshDelete( reg->eUp );*/
	}
	dictDeleteDict( &tess->alloc, tess->dict );
}


static void RemoveDegenerateEdges( TESStesselator *tess )
/*
* Remove zero-length edges, and contours with fewer than 3 vertices.
*/
{
	TESShalfEdge *e, *eNext, *eLnext;
	TESShalfEdge *eHead = &tess->mesh->eHead;

	/*LINTED*/
	for( e = eHead->next; e != eHead; e = eNext ) {
		eNext = e->next;
		eLnext = e->Lnext;

		if( VertEq( e->Org, e->Dst ) && e->Lnext->Lnext != e ) {
			/* Zero-length edge, contour has at least 3 edges */

			SpliceMergeVertices( tess, eLnext, e );	/* deletes e->Org */
			if ( !tessMeshDelete( tess->mesh, e ) ) longjmp(tess->env,1); /* e is a self-loop */
			e = eLnext;
			eLnext = e->Lnext;
		}
		if( eLnext->Lnext == e ) {
			/* Degenerate contour (one or two edges) */

			if( eLnext != e ) {
				if( eLnext == eNext || eLnext == eNext->Sym ) { eNext = eNext->next; }
				if ( !tessMeshDelete( tess->mesh, eLnext ) ) longjmp(tess->env,1);
			}
			if( e == eNext || e == eNext->Sym ) { eNext = eNext->next; }
			if ( !tessMeshDelete( tess->mesh, e ) ) longjmp(tess->env,1);
		}
	}
}

static int InitPriorityQ( TESStesselator *tess )
/*
* Insert all vertices into the priority queue which determines the
* order in which vertices cross the sweep line.
*/
{
	PriorityQ *pq;
	TESSvertex *v, *vHead;
	int vertexCount = 0;
	
	vHead = &tess->mesh->vHead;
	for( v = vHead->next; v != vHead; v = v->next ) {
		vertexCount++;
	}
	/* Make sure there is enough space for sentinels. */
	vertexCount += MAX( 8, tess->alloc.extraVertices );
	
	pq = tess->pq = pqNewPriorityQ( &tess->alloc, vertexCount, (int (*)(PQkey, PQkey)) tesvertLeq );
	if (pq == NULL) return 0;

	vHead = &tess->mesh->vHead;
	for( v = vHead->next; v != vHead; v = v->next ) {
		v->pqHandle = pqInsert( &tess->alloc, pq, v );
		if (v->pqHandle == INV_HANDLE)
			break;
	}
	if (v != vHead || !pqInit( &tess->alloc, pq ) ) {
		pqDeletePriorityQ( &tess->alloc, tess->pq );
		tess->pq = NULL;
		return 0;
	}

	return 1;
}


static void DonePriorityQ( TESStesselator *tess )
{
	pqDeletePriorityQ( &tess->alloc, tess->pq );
}


static int RemoveDegenerateFaces( TESStesselator *tess, TESSmesh *mesh )
/*
* Delete any degenerate faces with only two edges.  WalkDirtyRegions()
* will catch almost all of these, but it won't catch degenerate faces
* produced by splice operations on already-processed edges.
* The two places this can happen are in FinishLeftRegions(), when
* we splice in a "temporary" edge produced by ConnectRightVertex(),
* and in CheckForLeftSplice(), where we splice already-processed
* edges to ensure that our dictionary invariants are not violated
* by numerical errors.
*
* In both these cases it is *very* dangerous to delete the offending
* edge at the time, since one of the routines further up the stack
* will sometimes be keeping a pointer to that edge.
*/
{
	TESSface *f, *fNext;
	TESShalfEdge *e;

	/*LINTED*/
	for( f = mesh->fHead.next; f != &mesh->fHead; f = fNext ) {
		fNext = f->next;
		e = f->anEdge;
		assert( e->Lnext != e );

		if( e->Lnext->Lnext == e ) {
			/* A face with only two edges */
			AddWinding( e->Onext, e );
			if ( !tessMeshDelete( tess->mesh, e ) ) return 0;
		}
	}
	return 1;
}

int tessComputeInterior( TESStesselator *tess )
/*
* tessComputeInterior( tess ) computes the planar arrangement specified
* by the given contours, and further subdivides this arrangement
* into regions.  Each region is marked "inside" if it belongs
* to the polygon, according to the rule given by tess->windingRule.
* Each interior region is guaranteed be monotone.
*/
{
	TESSvertex *v, *vNext;

	/* Each vertex defines an event for our sweep line.  Start by inserting
	* all the vertices in a priority queue.  Events are processed in
	* lexicographic order, ie.
	*
	*	e1 < e2  iff  e1.x < e2.x || (e1.x == e2.x && e1.y < e2.y)
	*/
	RemoveDegenerateEdges( tess );
	if ( !InitPriorityQ( tess ) ) return 0; /* if error */
	InitEdgeDict( tess );

	while( (v = (TESSvertex *)pqExtractMin( tess->pq )) != NULL ) {
		for( ;; ) {
			vNext = (TESSvertex *)pqMinimum( tess->pq );
			if( vNext == NULL || ! VertEq( vNext, v )) break;

			/* Merge together all vertices at exactly the same location.
			* This is more efficient than processing them one at a time,
			* simplifies the code (see ConnectLeftDegenerate), and is also
			* important for correct handling of certain degenerate cases.
			* For example, suppose there are two identical edges A and B
			* that belong to different contours (so without this code they would
			* be processed by separate sweep events).  Suppose another edge C
			* crosses A and B from above.  When A is processed, we split it
			* at its intersection point with C.  However this also splits C,
			* so when we insert B we may compute a slightly different
			* intersection point.  This might leave two edges with a small
			* gap between them.  This kind of error is especially obvious
			* when using boundary extraction (TESS_BOUNDARY_ONLY).
			*/
			vNext = (TESSvertex *)pqExtractMin( tess->pq );
			SpliceMergeVertices( tess, v->anEdge, vNext->anEdge );
		}
		SweepEvent( tess, v );
	}

	/* Set tess->event for debugging purposes */
	tess->event = ((ActiveRegion *) dictKey( dictMin( tess->dict )))->eUp->Org;
	DebugEvent( tess );
	DoneEdgeDict( tess );
	DonePriorityQ( tess );

	if ( !RemoveDegenerateFaces( tess, tess->mesh ) ) return 0;
	tessMeshCheckMesh( tess->mesh );

	return 1;
}

//-------------------------------------------------------------------------------------------------------------------------------------------------

/* really tessDictListNewDict */
Dict *dictNewDict( TESSalloc* alloc, void *frame, int (*leq)(void *frame, DictKey key1, DictKey key2) )
{
	Dict *dict = (Dict *)alloc->memalloc( alloc->userData, sizeof( Dict ));
	DictNode *head;

	if (dict == NULL) return NULL;

	head = &dict->head;

	head->key = NULL;
	head->next = head;
	head->prev = head;

	dict->frame = frame;
	dict->leq = leq;

	if (alloc->dictNodeBucketSize < 16)
		alloc->dictNodeBucketSize = 16;
	if (alloc->dictNodeBucketSize > 4096)
		alloc->dictNodeBucketSize = 4096;
	dict->nodePool = createBucketAlloc( alloc, "Dict", sizeof(DictNode), alloc->dictNodeBucketSize );

	return dict;
}

/* really tessDictListDeleteDict */
void dictDeleteDict( TESSalloc* alloc, Dict *dict )
{
	deleteBucketAlloc( dict->nodePool );
	alloc->memfree( alloc->userData, dict );
}

/* really tessDictListInsertBefore */
DictNode *dictInsertBefore( Dict *dict, DictNode *node, DictKey key )
{
	DictNode *newNode;

	do {
		node = node->prev;
	} while( node->key != NULL && ! (*dict->leq)(dict->frame, node->key, key));

	newNode = (DictNode *)bucketAlloc( dict->nodePool );
	if (newNode == NULL) return NULL;

	newNode->key = key;
	newNode->next = node->next;
	node->next->prev = newNode;
	newNode->prev = node;
	node->next = newNode;

	return newNode;
}

/* really tessDictListDelete */
void dictDelete( Dict *dict, DictNode *node ) /*ARGSUSED*/
{
	node->next->prev = node->prev;
	node->prev->next = node->next;
	bucketFree( dict->nodePool, node );
}

/* really tessDictListSearch */
DictNode *dictSearch( Dict *dict, DictKey key )
{
	DictNode *node = &dict->head;

	do {
		node = node->next;
	} while( node->key != NULL && ! (*dict->leq)(dict->frame, key, node->key));

	return node;
}

//-------------------------------------------------------------------------------------------------------------------------------------------------

/************************ Utility Routines ************************/

/* Allocate and free half-edges in pairs for efficiency.
* The *only* place that should use this fact is allocation/free.
*/
typedef struct { TESShalfEdge e, eSym; } EdgePair;

/* MakeEdge creates a new pair of half-edges which form their own loop.
* No vertex or face structures are allocated, but these must be assigned
* before the current edge operation is completed.
*/
static TESShalfEdge *MakeEdge( TESSmesh* mesh, TESShalfEdge *eNext )
{
	TESShalfEdge *e;
	TESShalfEdge *eSym;
	TESShalfEdge *ePrev;
	EdgePair *pair = (EdgePair *)bucketAlloc( mesh->edgeBucket );
	if (pair == NULL) return NULL;

	e = &pair->e;
	eSym = &pair->eSym;

	/* Make sure eNext points to the first edge of the edge pair */
	if( eNext->Sym < eNext ) { eNext = eNext->Sym; }

	/* Insert in circular doubly-linked list before eNext.
	* Note that the prev pointer is stored in Sym->next.
	*/
	ePrev = eNext->Sym->next;
	eSym->next = ePrev;
	ePrev->Sym->next = e;
	e->next = eNext;
	eNext->Sym->next = eSym;

	e->Sym = eSym;
	e->Onext = e;
	e->Lnext = eSym;
	e->Org = NULL;
	e->Lface = NULL;
	e->winding = 0;
	e->activeRegion = NULL;

	eSym->Sym = e;
	eSym->Onext = eSym;
	eSym->Lnext = e;
	eSym->Org = NULL;
	eSym->Lface = NULL;
	eSym->winding = 0;
	eSym->activeRegion = NULL;

	return e;
}

/* Splice( a, b ) is best described by the Guibas/Stolfi paper or the
* CS348a notes (see mesh.h).  Basically it modifies the mesh so that
* a->Onext and b->Onext are exchanged.  This can have various effects
* depending on whether a and b belong to different face or vertex rings.
* For more explanation see tessMeshSplice() below.
*/
static void Splice( TESShalfEdge *a, TESShalfEdge *b )
{
	TESShalfEdge *aOnext = a->Onext;
	TESShalfEdge *bOnext = b->Onext;

	aOnext->Sym->Lnext = b;
	bOnext->Sym->Lnext = a;
	a->Onext = bOnext;
	b->Onext = aOnext;
}

/* MakeVertex( newVertex, eOrig, vNext ) attaches a new vertex and makes it the
* origin of all edges in the vertex loop to which eOrig belongs. "vNext" gives
* a place to insert the new vertex in the global vertex list.  We insert
* the new vertex *before* vNext so that algorithms which walk the vertex
* list will not see the newly created vertices.
*/
static void MakeVertex( TESSvertex *newVertex, 
					   TESShalfEdge *eOrig, TESSvertex *vNext )
{
	TESShalfEdge *e;
	TESSvertex *vPrev;
	TESSvertex *vNew = newVertex;

	assert(vNew != NULL);

	/* insert in circular doubly-linked list before vNext */
	vPrev = vNext->prev;
	vNew->prev = vPrev;
	vPrev->next = vNew;
	vNew->next = vNext;
	vNext->prev = vNew;

	vNew->anEdge = eOrig;
	/* leave coords, s, t undefined */

	/* fix other edges on this vertex loop */
	e = eOrig;
	do {
		e->Org = vNew;
		e = e->Onext;
	} while( e != eOrig );
}

/* MakeFace( newFace, eOrig, fNext ) attaches a new face and makes it the left
* face of all edges in the face loop to which eOrig belongs.  "fNext" gives
* a place to insert the new face in the global face list.  We insert
* the new face *before* fNext so that algorithms which walk the face
* list will not see the newly created faces.
*/
static void MakeFace( TESSface *newFace, TESShalfEdge *eOrig, TESSface *fNext )
{
	TESShalfEdge *e;
	TESSface *fPrev;
	TESSface *fNew = newFace;

	assert(fNew != NULL); 

	/* insert in circular doubly-linked list before fNext */
	fPrev = fNext->prev;
	fNew->prev = fPrev;
	fPrev->next = fNew;
	fNew->next = fNext;
	fNext->prev = fNew;

	fNew->anEdge = eOrig;
	fNew->trail = NULL;
	fNew->marked = FALSE;

	/* The new face is marked "inside" if the old one was.  This is a
	* convenience for the common case where a face has been split in two.
	*/
	fNew->inside = fNext->inside;

	/* fix other edges on this face loop */
	e = eOrig;
	do {
		e->Lface = fNew;
		e = e->Lnext;
	} while( e != eOrig );
}

/* KillEdge( eDel ) destroys an edge (the half-edges eDel and eDel->Sym),
* and removes from the global edge list.
*/
static void KillEdge( TESSmesh *mesh, TESShalfEdge *eDel )
{
	TESShalfEdge *ePrev, *eNext;

	/* Half-edges are allocated in pairs, see EdgePair above */
	if( eDel->Sym < eDel ) { eDel = eDel->Sym; }

	/* delete from circular doubly-linked list */
	eNext = eDel->next;
	ePrev = eDel->Sym->next;
	eNext->Sym->next = ePrev;
	ePrev->Sym->next = eNext;

	bucketFree( mesh->edgeBucket, eDel );
}


/* KillVertex( vDel ) destroys a vertex and removes it from the global
* vertex list.  It updates the vertex loop to point to a given new vertex.
*/
static void KillVertex( TESSmesh *mesh, TESSvertex *vDel, TESSvertex *newOrg )
{
	TESShalfEdge *e, *eStart = vDel->anEdge;
	TESSvertex *vPrev, *vNext;

	/* change the origin of all affected edges */
	e = eStart;
	do {
		e->Org = newOrg;
		e = e->Onext;
	} while( e != eStart );

	/* delete from circular doubly-linked list */
	vPrev = vDel->prev;
	vNext = vDel->next;
	vNext->prev = vPrev;
	vPrev->next = vNext;

	bucketFree( mesh->vertexBucket, vDel );
}

/* KillFace( fDel ) destroys a face and removes it from the global face
* list.  It updates the face loop to point to a given new face.
*/
static void KillFace( TESSmesh *mesh, TESSface *fDel, TESSface *newLface )
{
	TESShalfEdge *e, *eStart = fDel->anEdge;
	TESSface *fPrev, *fNext;

	/* change the left face of all affected edges */
	e = eStart;
	do {
		e->Lface = newLface;
		e = e->Lnext;
	} while( e != eStart );

	/* delete from circular doubly-linked list */
	fPrev = fDel->prev;
	fNext = fDel->next;
	fNext->prev = fPrev;
	fPrev->next = fNext;

	bucketFree( mesh->faceBucket, fDel );
}


/****************** Basic Edge Operations **********************/

/* tessMeshMakeEdge creates one edge, two vertices, and a loop (face).
* The loop consists of the two new half-edges.
*/
TESShalfEdge *tessMeshMakeEdge( TESSmesh *mesh )
{
	TESSvertex *newVertex1 = (TESSvertex*)bucketAlloc(mesh->vertexBucket);
	TESSvertex *newVertex2 = (TESSvertex*)bucketAlloc(mesh->vertexBucket);
	TESSface *newFace = (TESSface*)bucketAlloc(mesh->faceBucket);
	TESShalfEdge *e;

	/* if any one is null then all get freed */
	if (newVertex1 == NULL || newVertex2 == NULL || newFace == NULL) {
		if (newVertex1 != NULL) bucketFree( mesh->vertexBucket, newVertex1 );
		if (newVertex2 != NULL) bucketFree( mesh->vertexBucket, newVertex2 );
		if (newFace != NULL) bucketFree( mesh->faceBucket, newFace );     
		return NULL;
	} 

	e = MakeEdge( mesh, &mesh->eHead );
	if (e == NULL) return NULL;

	MakeVertex( newVertex1, e, &mesh->vHead );
	MakeVertex( newVertex2, e->Sym, &mesh->vHead );
	MakeFace( newFace, e, &mesh->fHead );
	return e;
}


/* tessMeshSplice( eOrg, eDst ) is the basic operation for changing the
* mesh connectivity and topology.  It changes the mesh so that
*	eOrg->Onext <- OLD( eDst->Onext )
*	eDst->Onext <- OLD( eOrg->Onext )
* where OLD(...) means the value before the meshSplice operation.
*
* This can have two effects on the vertex structure:
*  - if eOrg->Org != eDst->Org, the two vertices are merged together
*  - if eOrg->Org == eDst->Org, the origin is split into two vertices
* In both cases, eDst->Org is changed and eOrg->Org is untouched.
*
* Similarly (and independently) for the face structure,
*  - if eOrg->Lface == eDst->Lface, one loop is split into two
*  - if eOrg->Lface != eDst->Lface, two distinct loops are joined into one
* In both cases, eDst->Lface is changed and eOrg->Lface is unaffected.
*
* Some special cases:
* If eDst == eOrg, the operation has no effect.
* If eDst == eOrg->Lnext, the new face will have a single edge.
* If eDst == eOrg->Lprev, the old face will have a single edge.
* If eDst == eOrg->Onext, the new vertex will have a single edge.
* If eDst == eOrg->Oprev, the old vertex will have a single edge.
*/
int tessMeshSplice( TESSmesh* mesh, TESShalfEdge *eOrg, TESShalfEdge *eDst )
{
	int joiningLoops = FALSE;
	int joiningVertices = FALSE;

	if( eOrg == eDst ) return 1;

	if( eDst->Org != eOrg->Org ) {
		/* We are merging two disjoint vertices -- destroy eDst->Org */
		joiningVertices = TRUE;
		KillVertex( mesh, eDst->Org, eOrg->Org );
	}
	if( eDst->Lface != eOrg->Lface ) {
		/* We are connecting two disjoint loops -- destroy eDst->Lface */
		joiningLoops = TRUE;
		KillFace( mesh, eDst->Lface, eOrg->Lface );
	}

	/* Change the edge structure */
	Splice( eDst, eOrg );

	if( ! joiningVertices ) {
		TESSvertex *newVertex = (TESSvertex*)bucketAlloc( mesh->vertexBucket );
		if (newVertex == NULL) return 0;

		/* We split one vertex into two -- the new vertex is eDst->Org.
		* Make sure the old vertex points to a valid half-edge.
		*/
		MakeVertex( newVertex, eDst, eOrg->Org );
		eOrg->Org->anEdge = eOrg;
	}
	if( ! joiningLoops ) {
		TESSface *newFace = (TESSface*)bucketAlloc( mesh->faceBucket );  
		if (newFace == NULL) return 0;

		/* We split one loop into two -- the new loop is eDst->Lface.
		* Make sure the old face points to a valid half-edge.
		*/
		MakeFace( newFace, eDst, eOrg->Lface );
		eOrg->Lface->anEdge = eOrg;
	}

	return 1;
}


/* tessMeshDelete( eDel ) removes the edge eDel.  There are several cases:
* if (eDel->Lface != eDel->Rface), we join two loops into one; the loop
* eDel->Lface is deleted.  Otherwise, we are splitting one loop into two;
* the newly created loop will contain eDel->Dst.  If the deletion of eDel
* would create isolated vertices, those are deleted as well.
*
* This function could be implemented as two calls to tessMeshSplice
* plus a few calls to memFree, but this would allocate and delete
* unnecessary vertices and faces.
*/
int tessMeshDelete( TESSmesh *mesh, TESShalfEdge *eDel )
{
	TESShalfEdge *eDelSym = eDel->Sym;
	int joiningLoops = FALSE;

	/* First step: disconnect the origin vertex eDel->Org.  We make all
	* changes to get a consistent mesh in this "intermediate" state.
	*/
	if( eDel->Lface != eDel->Rface ) {
		/* We are joining two loops into one -- remove the left face */
		joiningLoops = TRUE;
		KillFace( mesh, eDel->Lface, eDel->Rface );
	}

	if( eDel->Onext == eDel ) {
		KillVertex( mesh, eDel->Org, NULL );
	} else {
		/* Make sure that eDel->Org and eDel->Rface point to valid half-edges */
		eDel->Rface->anEdge = eDel->Oprev;
		eDel->Org->anEdge = eDel->Onext;

		Splice( eDel, eDel->Oprev );
		if( ! joiningLoops ) {
			TESSface *newFace= (TESSface*)bucketAlloc( mesh->faceBucket );
			if (newFace == NULL) return 0; 

			/* We are splitting one loop into two -- create a new loop for eDel. */
			MakeFace( newFace, eDel, eDel->Lface );
		}
	}

	/* Claim: the mesh is now in a consistent state, except that eDel->Org
	* may have been deleted.  Now we disconnect eDel->Dst.
	*/
	if( eDelSym->Onext == eDelSym ) {
		KillVertex( mesh, eDelSym->Org, NULL );
		KillFace( mesh, eDelSym->Lface, NULL );
	} else {
		/* Make sure that eDel->Dst and eDel->Lface point to valid half-edges */
		eDel->Lface->anEdge = eDelSym->Oprev;
		eDelSym->Org->anEdge = eDelSym->Onext;
		Splice( eDelSym, eDelSym->Oprev );
	}

	/* Any isolated vertices or faces have already been freed. */
	KillEdge( mesh, eDel );

	return 1;
}


/******************** Other Edge Operations **********************/

/* All these routines can be implemented with the basic edge
* operations above.  They are provided for convenience and efficiency.
*/


/* tessMeshAddEdgeVertex( eOrg ) creates a new edge eNew such that
* eNew == eOrg->Lnext, and eNew->Dst is a newly created vertex.
* eOrg and eNew will have the same left face.
*/
TESShalfEdge *tessMeshAddEdgeVertex( TESSmesh *mesh, TESShalfEdge *eOrg )
{
	TESShalfEdge *eNewSym;
	TESShalfEdge *eNew = MakeEdge( mesh, eOrg );
	if (eNew == NULL) return NULL;

	eNewSym = eNew->Sym;

	/* Connect the new edge appropriately */
	Splice( eNew, eOrg->Lnext );

	/* Set the vertex and face information */
	eNew->Org = eOrg->Dst;
	{
		TESSvertex *newVertex= (TESSvertex*)bucketAlloc( mesh->vertexBucket );
		if (newVertex == NULL) return NULL;

		MakeVertex( newVertex, eNewSym, eNew->Org );
	}
	eNew->Lface = eNewSym->Lface = eOrg->Lface;

	return eNew;
}


/* tessMeshSplitEdge( eOrg ) splits eOrg into two edges eOrg and eNew,
* such that eNew == eOrg->Lnext.  The new vertex is eOrg->Dst == eNew->Org.
* eOrg and eNew will have the same left face.
*/
TESShalfEdge *tessMeshSplitEdge( TESSmesh *mesh, TESShalfEdge *eOrg )
{
	TESShalfEdge *eNew;
	TESShalfEdge *tempHalfEdge= tessMeshAddEdgeVertex( mesh, eOrg );
	if (tempHalfEdge == NULL) return NULL;

	eNew = tempHalfEdge->Sym;

	/* Disconnect eOrg from eOrg->Dst and connect it to eNew->Org */
	Splice( eOrg->Sym, eOrg->Sym->Oprev );
	Splice( eOrg->Sym, eNew );

	/* Set the vertex and face information */
	eOrg->Dst = eNew->Org;
	eNew->Dst->anEdge = eNew->Sym;	/* may have pointed to eOrg->Sym */
	eNew->Rface = eOrg->Rface;
	eNew->winding = eOrg->winding;	/* copy old winding information */
	eNew->Sym->winding = eOrg->Sym->winding;

	return eNew;
}


/* tessMeshConnect( eOrg, eDst ) creates a new edge from eOrg->Dst
* to eDst->Org, and returns the corresponding half-edge eNew.
* If eOrg->Lface == eDst->Lface, this splits one loop into two,
* and the newly created loop is eNew->Lface.  Otherwise, two disjoint
* loops are merged into one, and the loop eDst->Lface is destroyed.
*
* If (eOrg == eDst), the new face will have only two edges.
* If (eOrg->Lnext == eDst), the old face is reduced to a single edge.
* If (eOrg->Lnext->Lnext == eDst), the old face is reduced to two edges.
*/
TESShalfEdge *tessMeshConnect( TESSmesh *mesh, TESShalfEdge *eOrg, TESShalfEdge *eDst )
{
	TESShalfEdge *eNewSym;
	int joiningLoops = FALSE;  
	TESShalfEdge *eNew = MakeEdge( mesh, eOrg );
	if (eNew == NULL) return NULL;

	eNewSym = eNew->Sym;

	if( eDst->Lface != eOrg->Lface ) {
		/* We are connecting two disjoint loops -- destroy eDst->Lface */
		joiningLoops = TRUE;
		KillFace( mesh, eDst->Lface, eOrg->Lface );
	}

	/* Connect the new edge appropriately */
	Splice( eNew, eOrg->Lnext );
	Splice( eNewSym, eDst );

	/* Set the vertex and face information */
	eNew->Org = eOrg->Dst;
	eNewSym->Org = eDst->Org;
	eNew->Lface = eNewSym->Lface = eOrg->Lface;

	/* Make sure the old face points to a valid half-edge */
	eOrg->Lface->anEdge = eNewSym;

	if( ! joiningLoops ) {
		TESSface *newFace= (TESSface*)bucketAlloc( mesh->faceBucket );
		if (newFace == NULL) return NULL;

		/* We split one loop into two -- the new loop is eNew->Lface */
		MakeFace( newFace, eNew, eOrg->Lface );
	}
	return eNew;
}


/******************** Other Operations **********************/

/* tessMeshZapFace( fZap ) destroys a face and removes it from the
* global face list.  All edges of fZap will have a NULL pointer as their
* left face.  Any edges which also have a NULL pointer as their right face
* are deleted entirely (along with any isolated vertices this produces).
* An entire mesh can be deleted by zapping its faces, one at a time,
* in any order.  Zapped faces cannot be used in further mesh operations!
*/
void tessMeshZapFace( TESSmesh *mesh, TESSface *fZap )
{
	TESShalfEdge *eStart = fZap->anEdge;
	TESShalfEdge *e, *eNext, *eSym;
	TESSface *fPrev, *fNext;

	/* walk around face, deleting edges whose right face is also NULL */
	eNext = eStart->Lnext;
	do {
		e = eNext;
		eNext = e->Lnext;

		e->Lface = NULL;
		if( e->Rface == NULL ) {
			/* delete the edge -- see TESSmeshDelete above */

			if( e->Onext == e ) {
				KillVertex( mesh, e->Org, NULL );
			} else {
				/* Make sure that e->Org points to a valid half-edge */
				e->Org->anEdge = e->Onext;
				Splice( e, e->Oprev );
			}
			eSym = e->Sym;
			if( eSym->Onext == eSym ) {
				KillVertex( mesh, eSym->Org, NULL );
			} else {
				/* Make sure that eSym->Org points to a valid half-edge */
				eSym->Org->anEdge = eSym->Onext;
				Splice( eSym, eSym->Oprev );
			}
			KillEdge( mesh, e );
		}
	} while( e != eStart );

	/* delete from circular doubly-linked list */
	fPrev = fZap->prev;
	fNext = fZap->next;
	fNext->prev = fPrev;
	fPrev->next = fNext;

	bucketFree( mesh->faceBucket, fZap );
}


/* tessMeshNewMesh() creates a new mesh with no edges, no vertices,
* and no loops (what we usually call a "face").
*/
TESSmesh *tessMeshNewMesh( TESSalloc* alloc )
{
	TESSvertex *v;
	TESSface *f;
	TESShalfEdge *e;
	TESShalfEdge *eSym;
	TESSmesh *mesh = (TESSmesh *)alloc->memalloc( alloc->userData, sizeof( TESSmesh ));
	if (mesh == NULL) {
		return NULL;
	}
	
	if (alloc->meshEdgeBucketSize < 16)
		alloc->meshEdgeBucketSize = 16;
	if (alloc->meshEdgeBucketSize > 4096)
		alloc->meshEdgeBucketSize = 4096;
	
	if (alloc->meshVertexBucketSize < 16)
		alloc->meshVertexBucketSize = 16;
	if (alloc->meshVertexBucketSize > 4096)
		alloc->meshVertexBucketSize = 4096;
	
	if (alloc->meshFaceBucketSize < 16)
		alloc->meshFaceBucketSize = 16;
	if (alloc->meshFaceBucketSize > 4096)
		alloc->meshFaceBucketSize = 4096;

	mesh->edgeBucket = createBucketAlloc( alloc, "Mesh Edges", sizeof(EdgePair), alloc->meshEdgeBucketSize );
	mesh->vertexBucket = createBucketAlloc( alloc, "Mesh Vertices", sizeof(TESSvertex), alloc->meshVertexBucketSize );
	mesh->faceBucket = createBucketAlloc( alloc, "Mesh Faces", sizeof(TESSface), alloc->meshFaceBucketSize );

	v = &mesh->vHead;
	f = &mesh->fHead;
	e = &mesh->eHead;
	eSym = &mesh->eHeadSym;

	v->next = v->prev = v;
	v->anEdge = NULL;

	f->next = f->prev = f;
	f->anEdge = NULL;
	f->trail = NULL;
	f->marked = FALSE;
	f->inside = FALSE;

	e->next = e;
	e->Sym = eSym;
	e->Onext = NULL;
	e->Lnext = NULL;
	e->Org = NULL;
	e->Lface = NULL;
	e->winding = 0;
	e->activeRegion = NULL;

	eSym->next = eSym;
	eSym->Sym = e;
	eSym->Onext = NULL;
	eSym->Lnext = NULL;
	eSym->Org = NULL;
	eSym->Lface = NULL;
	eSym->winding = 0;
	eSym->activeRegion = NULL;

	return mesh;
}


/* tessMeshUnion( mesh1, mesh2 ) forms the union of all structures in
* both meshes, and returns the new mesh (the old meshes are destroyed).
*/
TESSmesh *tessMeshUnion( TESSalloc* alloc, TESSmesh *mesh1, TESSmesh *mesh2 )
{
	TESSface *f1 = &mesh1->fHead;
	TESSvertex *v1 = &mesh1->vHead;
	TESShalfEdge *e1 = &mesh1->eHead;
	TESSface *f2 = &mesh2->fHead;
	TESSvertex *v2 = &mesh2->vHead;
	TESShalfEdge *e2 = &mesh2->eHead;

	/* Add the faces, vertices, and edges of mesh2 to those of mesh1 */
	if( f2->next != f2 ) {
		f1->prev->next = f2->next;
		f2->next->prev = f1->prev;
		f2->prev->next = f1;
		f1->prev = f2->prev;
	}

	if( v2->next != v2 ) {
		v1->prev->next = v2->next;
		v2->next->prev = v1->prev;
		v2->prev->next = v1;
		v1->prev = v2->prev;
	}

	if( e2->next != e2 ) {
		e1->Sym->next->Sym->next = e2->next;
		e2->next->Sym->next = e1->Sym->next;
		e2->Sym->next->Sym->next = e1;
		e1->Sym->next = e2->Sym->next;
	}

	alloc->memfree( alloc->userData, mesh2 );
	return mesh1;
}


static int CountFaceVerts( TESSface *f )
{
	TESShalfEdge *eCur = f->anEdge;
	int n = 0;
	do
	{
		n++;
		eCur = eCur->Lnext;
	}
	while (eCur != f->anEdge);
	return n;
}

int tessMeshMergeConvexFaces( TESSmesh *mesh, int maxVertsPerFace )
{
	TESSface *f;
	TESShalfEdge *eCur, *eNext, *eSym;
	TESSvertex *vStart;
	int curNv, symNv;
	
	for( f = mesh->fHead.next; f != &mesh->fHead; f = f->next )
	{
		// Skip faces which are outside the result.
		if( !f->inside )
			continue;

		eCur = f->anEdge;
		vStart = eCur->Org;
			
		while (1)
		{
			eNext = eCur->Lnext;
			eSym = eCur->Sym;

			// Try to merge if the neighbour face is valid.
			if( eSym && eSym->Lface && eSym->Lface->inside )
			{
				// Try to merge the neighbour faces if the resulting polygons
				// does not exceed maximum number of vertices.
				curNv = CountFaceVerts( f );
				symNv = CountFaceVerts( eSym->Lface );
				if( (curNv+symNv-2) <= maxVertsPerFace )
				{
					// Merge if the resulting poly is convex.
					if( VertCCW( eCur->Lprev->Org, eCur->Org, eSym->Lnext->Lnext->Org ) &&
						VertCCW( eSym->Lprev->Org, eSym->Org, eCur->Lnext->Lnext->Org ) )
					{
						eNext = eSym->Lnext;
						if( !tessMeshDelete( mesh, eSym ) )
							return 0;
						eCur = 0;
					}
				}
			}
			
			if( eCur && eCur->Lnext->Org == vStart )
				break;
				
			// Continue to next edge.
			eCur = eNext;
		}
	}
	
	return 1;
}


#ifdef DELETE_BY_ZAPPING

/* tessMeshDeleteMesh( mesh ) will free all storage for any valid mesh.
*/
void tessMeshDeleteMesh( TESSalloc* alloc, TESSmesh *mesh )
{
	TESSface *fHead = &mesh->fHead;

	while( fHead->next != fHead ) {
		tessMeshZapFace( fHead->next );
	}
	assert( mesh->vHead.next == &mesh->vHead );

	alloc->memfree( alloc->userData, mesh );
}

#else

/* tessMeshDeleteMesh( mesh ) will free all storage for any valid mesh.
*/
void tessMeshDeleteMesh( TESSalloc* alloc, TESSmesh *mesh )
{
	deleteBucketAlloc(mesh->edgeBucket);
	deleteBucketAlloc(mesh->vertexBucket);
	deleteBucketAlloc(mesh->faceBucket);

	alloc->memfree( alloc->userData, mesh );
}

#endif

#ifndef NDEBUG

/* tessMeshCheckMesh( mesh ) checks a mesh for self-consistency.
*/
void tessMeshCheckMesh( TESSmesh *mesh )
{
	TESSface *fHead = &mesh->fHead;
	TESSvertex *vHead = &mesh->vHead;
	TESShalfEdge *eHead = &mesh->eHead;
	TESSface *f, *fPrev;
	TESSvertex *v, *vPrev;
	TESShalfEdge *e, *ePrev;

	fPrev = fHead;
	for( fPrev = fHead ; (f = fPrev->next) != fHead; fPrev = f) {
		assert( f->prev == fPrev );
		e = f->anEdge;
		do {
			assert( e->Sym != e );
			assert( e->Sym->Sym == e );
			assert( e->Lnext->Onext->Sym == e );
			assert( e->Onext->Sym->Lnext == e );
			assert( e->Lface == f );
			e = e->Lnext;
		} while( e != f->anEdge );
	}
	assert( f->prev == fPrev && f->anEdge == NULL );

	vPrev = vHead;
	for( vPrev = vHead ; (v = vPrev->next) != vHead; vPrev = v) {
		assert( v->prev == vPrev );
		e = v->anEdge;
		do {
			assert( e->Sym != e );
			assert( e->Sym->Sym == e );
			assert( e->Lnext->Onext->Sym == e );
			assert( e->Onext->Sym->Lnext == e );
			assert( e->Org == v );
			e = e->Onext;
		} while( e != v->anEdge );
	}
	assert( v->prev == vPrev && v->anEdge == NULL );

	ePrev = eHead;
	for( ePrev = eHead ; (e = ePrev->next) != eHead; ePrev = e) {
		assert( e->Sym->next == ePrev->Sym );
		assert( e->Sym != e );
		assert( e->Sym->Sym == e );
		assert( e->Org != NULL );
		assert( e->Dst != NULL );
		assert( e->Lnext->Onext->Sym == e );
		assert( e->Onext->Sym->Lnext == e );
	}
	assert( e->Sym->next == ePrev->Sym
		&& e->Sym == &mesh->eHeadSym
		&& e->Sym->Sym == e
		&& e->Org == NULL && e->Dst == NULL
		&& e->Lface == NULL && e->Rface == NULL );
}

#endif

//-------------------------------------------------------------------------------------------------------------------------------------------------

#ifdef FOR_TRITE_TEST_PROGRAM
#define LEQ(x,y)	(*pq->leq)(x,y)
#else
/* Violates modularity, but a little faster */
#define LEQ(x,y)	VertLeq((TESSvertex *)x, (TESSvertex *)y)
#endif


/* Include all the code for the regular heap-based queue here. */

/* The basic operations are insertion of a new key (pqInsert),
* and examination/extraction of a key whose value is minimum
* (pqMinimum/pqExtractMin).  Deletion is also allowed (pqDelete);
* for this purpose pqInsert returns a "handle" which is supplied
* as the argument.
*
* An initial heap may be created efficiently by calling pqInsert
* repeatedly, then calling pqInit.  In any case pqInit must be called
* before any operations other than pqInsert are used.
*
* If the heap is empty, pqMinimum/pqExtractMin will return a NULL key.
* This may also be tested with pqIsEmpty.
*/


/* Since we support deletion the data structure is a little more
* complicated than an ordinary heap.  "nodes" is the heap itself;
* active nodes are stored in the range 1..pq->size.  When the
* heap exceeds its allocated size (pq->max), its size doubles.
* The children of node i are nodes 2i and 2i+1.
*
* Each node stores an index into an array "handles".  Each handle
* stores a key, plus a pointer back to the node which currently
* represents that key (ie. nodes[handles[i].node].handle == i).
*/


#define pqHeapMinimum(pq)	((pq)->handles[(pq)->nodes[1].handle].key)
#define pqHeapIsEmpty(pq)	((pq)->size == 0)



/* really pqHeapNewPriorityQHeap */
PriorityQHeap *pqHeapNewPriorityQ( TESSalloc* alloc, int size, int (*leq)(PQkey key1, PQkey key2) )
{
	PriorityQHeap *pq = (PriorityQHeap *)alloc->memalloc( alloc->userData, sizeof( PriorityQHeap ));
	if (pq == NULL) return NULL;

	pq->size = 0;
	pq->max = size;
	pq->nodes = (PQnode *)alloc->memalloc( alloc->userData, (size + 1) * sizeof(pq->nodes[0]) );
	if (pq->nodes == NULL) {
		alloc->memfree( alloc->userData, pq );
		return NULL;
	}

	pq->handles = (PQhandleElem *)alloc->memalloc( alloc->userData, (size + 1) * sizeof(pq->handles[0]) );
	if (pq->handles == NULL) {
		alloc->memfree( alloc->userData, pq->nodes );
		alloc->memfree( alloc->userData, pq );
		return NULL;
	}

	pq->initialized = FALSE;
	pq->freeList = 0;
	pq->leq = leq;

	pq->nodes[1].handle = 1;	/* so that Minimum() returns NULL */
	pq->handles[1].key = NULL;
	return pq;
}

/* really pqHeapDeletePriorityQHeap */
void pqHeapDeletePriorityQ( TESSalloc* alloc, PriorityQHeap *pq )
{
	alloc->memfree( alloc->userData, pq->handles );
	alloc->memfree( alloc->userData, pq->nodes );
	alloc->memfree( alloc->userData, pq );
}


static void FloatDown( PriorityQHeap *pq, int curr )
{
	PQnode *n = pq->nodes;
	PQhandleElem *h = pq->handles;
	PQhandle hCurr, hChild;
	int child;

	hCurr = n[curr].handle;
	for( ;; ) {
		child = curr << 1;
		if( child < pq->size && LEQ( h[n[child+1].handle].key,
			h[n[child].handle].key )) {
				++child;
		}

		assert(child <= pq->max);

		hChild = n[child].handle;
		if( child > pq->size || LEQ( h[hCurr].key, h[hChild].key )) {
			n[curr].handle = hCurr;
			h[hCurr].node = curr;
			break;
		}
		n[curr].handle = hChild;
		h[hChild].node = curr;
		curr = child;
	}
}


static void FloatUp( PriorityQHeap *pq, int curr )
{
	PQnode *n = pq->nodes;
	PQhandleElem *h = pq->handles;
	PQhandle hCurr, hParent;
	int parent;

	hCurr = n[curr].handle;
	for( ;; ) {
		parent = curr >> 1;
		hParent = n[parent].handle;
		if( parent == 0 || LEQ( h[hParent].key, h[hCurr].key )) {
			n[curr].handle = hCurr;
			h[hCurr].node = curr;
			break;
		}
		n[curr].handle = hParent;
		h[hParent].node = curr;
		curr = parent;
	}
}

/* really pqHeapInit */
void pqHeapInit( PriorityQHeap *pq )
{
	int i;

	/* This method of building a heap is O(n), rather than O(n lg n). */

	for( i = pq->size; i >= 1; --i ) {
		FloatDown( pq, i );
	}
	pq->initialized = TRUE;
}

/* really pqHeapInsert */
/* returns INV_HANDLE iff out of memory */
PQhandle pqHeapInsert( TESSalloc* alloc, PriorityQHeap *pq, PQkey keyNew )
{
	int curr;
	PQhandle free;

	curr = ++ pq->size;
	if( (curr*2) > pq->max ) {
		if (!alloc->memrealloc)
		{
			return INV_HANDLE;
		}
		else
		{
			PQnode *saveNodes= pq->nodes;
			PQhandleElem *saveHandles= pq->handles;

			// If the heap overflows, double its size.
			pq->max <<= 1;
			pq->nodes = (PQnode *)alloc->memrealloc( alloc->userData, pq->nodes, 
				(size_t)((pq->max + 1) * sizeof( pq->nodes[0] )));
			if (pq->nodes == NULL) {
				pq->nodes = saveNodes;	// restore ptr to free upon return 
				return INV_HANDLE;
			}
			pq->handles = (PQhandleElem *)alloc->memrealloc( alloc->userData, pq->handles,
				(size_t) ((pq->max + 1) * sizeof( pq->handles[0] )));
			if (pq->handles == NULL) {
				pq->handles = saveHandles; // restore ptr to free upon return 
				return INV_HANDLE;
			}
		}
	}

	if( pq->freeList == 0 ) {
		free = curr;
	} else {
		free = pq->freeList;
		pq->freeList = pq->handles[free].node;
	}

	pq->nodes[curr].handle = free;
	pq->handles[free].node = curr;
	pq->handles[free].key = keyNew;

	if( pq->initialized ) {
		FloatUp( pq, curr );
	}
	assert(free != INV_HANDLE);
	return free;
}

/* really pqHeapExtractMin */
PQkey pqHeapExtractMin( PriorityQHeap *pq )
{
	PQnode *n = pq->nodes;
	PQhandleElem *h = pq->handles;
	PQhandle hMin = n[1].handle;
	PQkey min = h[hMin].key;

	if( pq->size > 0 ) {
		n[1].handle = n[pq->size].handle;
		h[n[1].handle].node = 1;

		h[hMin].key = NULL;
		h[hMin].node = pq->freeList;
		pq->freeList = hMin;

		if( -- pq->size > 0 ) {
			FloatDown( pq, 1 );
		}
	}
	return min;
}

/* really pqHeapDelete */
void pqHeapDelete( PriorityQHeap *pq, PQhandle hCurr )
{
	PQnode *n = pq->nodes;
	PQhandleElem *h = pq->handles;
	int curr;

	assert( hCurr >= 1 && hCurr <= pq->max && h[hCurr].key != NULL );

	curr = h[hCurr].node;
	n[curr].handle = n[pq->size].handle;
	h[n[curr].handle].node = curr;

	if( curr <= -- pq->size ) {
		if( curr <= 1 || LEQ( h[n[curr>>1].handle].key, h[n[curr].handle].key )) {
			FloatDown( pq, curr );
		} else {
			FloatUp( pq, curr );
		}
	}
	h[hCurr].key = NULL;
	h[hCurr].node = pq->freeList;
	pq->freeList = hCurr;
}



/* Now redefine all the function names to map to their "Sort" versions. */

/* really tessPqSortNewPriorityQ */
PriorityQ *pqNewPriorityQ( TESSalloc* alloc, int size, int (*leq)(PQkey key1, PQkey key2) )
{
	PriorityQ *pq = (PriorityQ *)alloc->memalloc( alloc->userData, sizeof( PriorityQ ));
	if (pq == NULL) return NULL;

	pq->heap = pqHeapNewPriorityQ( alloc, size, leq );
	if (pq->heap == NULL) {
		alloc->memfree( alloc->userData, pq );
		return NULL;
	}

//	pq->keys = (PQkey *)memAlloc( INIT_SIZE * sizeof(pq->keys[0]) );
	pq->keys = (PQkey *)alloc->memalloc( alloc->userData, size * sizeof(pq->keys[0]) );
	if (pq->keys == NULL) {
		pqHeapDeletePriorityQ( alloc, pq->heap );
		alloc->memfree( alloc->userData, pq );
		return NULL;
	}

	pq->size = 0;
	pq->max = size; //INIT_SIZE;
	pq->initialized = FALSE;
	pq->leq = leq;
	
	return pq;
}

/* really tessPqSortDeletePriorityQ */
void pqDeletePriorityQ( TESSalloc* alloc, PriorityQ *pq )
{
	assert(pq != NULL); 
	if (pq->heap != NULL) pqHeapDeletePriorityQ( alloc, pq->heap );
	if (pq->order != NULL) alloc->memfree( alloc->userData, pq->order );
	if (pq->keys != NULL) alloc->memfree( alloc->userData, pq->keys );
	alloc->memfree( alloc->userData, pq );
}


#define LT(x,y)     (! LEQ(y,x))
#define GT(x,y)     (! LEQ(x,y))
#define SwapPQ(a,b)   if(1){PQkey *tmp = *a; *a = *b; *b = tmp;}else

/* really tessPqSortInit */
int pqInit( TESSalloc* alloc, PriorityQ *pq )
{
	PQkey **p, **r, **i, **j, *piv;
	struct { PQkey **p, **r; } Stack[50], *top = Stack;
	unsigned int seed = 2016473283;

	/* Create an array of indirect pointers to the keys, so that we
	* the handles we have returned are still valid.
	*/
	/*
	pq->order = (PQkey **)memAlloc( (size_t)
	(pq->size * sizeof(pq->order[0])) );
	*/
	pq->order = (PQkey **)alloc->memalloc( alloc->userData,
										  (size_t)((pq->size+1) * sizeof(pq->order[0])) );
	/* the previous line is a patch to compensate for the fact that IBM */
	/* machines return a null on a malloc of zero bytes (unlike SGI),   */
	/* so we have to put in this defense to guard against a memory      */
	/* fault four lines down. from fossum@austin.ibm.com.               */
	if (pq->order == NULL) return 0;

	p = pq->order;
	r = p + pq->size - 1;
	for( piv = pq->keys, i = p; i <= r; ++piv, ++i ) {
		*i = piv;
	}

	/* Sort the indirect pointers in descending order,
	* using randomized Quicksort
	*/
	top->p = p; top->r = r; ++top;
	while( --top >= Stack ) {
		p = top->p;
		r = top->r;
		while( r > p + 10 ) {
			seed = seed * 1539415821 + 1;
			i = p + seed % (r - p + 1);
			piv = *i;
			*i = *p;
			*p = piv;
			i = p - 1;
			j = r + 1;
			do {
				do { ++i; } while( GT( **i, *piv ));
				do { --j; } while( LT( **j, *piv ));
				SwapPQ( i, j );
			} while( i < j );
			SwapPQ( i, j ); /* Undo last swap */
			if( i - p < r - j ) {
				top->p = j+1; top->r = r; ++top;
				r = i-1;
			} else {
				top->p = p; top->r = i-1; ++top;
				p = j+1;
			}
		}
		/* Insertion sort small lists */
		for( i = p+1; i <= r; ++i ) {
			piv = *i;
			for( j = i; j > p && LT( **(j-1), *piv ); --j ) {
				*j = *(j-1);
			}
			*j = piv;
		}
	}
	pq->max = pq->size;
	pq->initialized = TRUE;
	pqHeapInit( pq->heap );  /* always succeeds */

#ifndef NDEBUG
	p = pq->order;
	r = p + pq->size - 1;
	for( i = p; i < r; ++i ) {
		assert( LEQ( **(i+1), **i ));
	}
#endif

	return 1;
}

/* really tessPqSortInsert */
/* returns INV_HANDLE iff out of memory */ 
PQhandle pqInsert( TESSalloc* alloc, PriorityQ *pq, PQkey keyNew )
{
	int curr;

	if( pq->initialized ) {
		return pqHeapInsert( alloc, pq->heap, keyNew );
	}
	curr = pq->size;
	if( ++ pq->size >= pq->max ) {
		if (!alloc->memrealloc)
		{
			return INV_HANDLE;
		}
		else
		{
			PQkey *saveKey= pq->keys;
			// If the heap overflows, double its size.
			pq->max <<= 1;
			pq->keys = (PQkey *)alloc->memrealloc( alloc->userData, pq->keys, 
				(size_t)(pq->max * sizeof( pq->keys[0] )));
			if (pq->keys == NULL) { 
				pq->keys = saveKey;  // restore ptr to free upon return 
				return INV_HANDLE;
			}
		}
	}
	assert(curr != INV_HANDLE); 
	pq->keys[curr] = keyNew;

	/* Negative handles index the sorted array. */
	return -(curr+1);
}

/* really tessPqSortExtractMin */
PQkey pqExtractMin( PriorityQ *pq )
{
	PQkey sortMin, heapMin;

	if( pq->size == 0 ) {
		return pqHeapExtractMin( pq->heap );
	}
	sortMin = *(pq->order[pq->size-1]);
	if( ! pqHeapIsEmpty( pq->heap )) {
		heapMin = pqHeapMinimum( pq->heap );
		if( LEQ( heapMin, sortMin )) {
			return pqHeapExtractMin( pq->heap );
		}
	}
	do {
		-- pq->size;
	} while( pq->size > 0 && *(pq->order[pq->size-1]) == NULL );
	return sortMin;
}

/* really tessPqSortMinimum */
PQkey pqMinimum( PriorityQ *pq )
{
	PQkey sortMin, heapMin;

	if( pq->size == 0 ) {
		return pqHeapMinimum( pq->heap );
	}
	sortMin = *(pq->order[pq->size-1]);
	if( ! pqHeapIsEmpty( pq->heap )) {
		heapMin = pqHeapMinimum( pq->heap );
		if( LEQ( heapMin, sortMin )) {
			return heapMin;
		}
	}
	return sortMin;
}

/* really tessPqSortIsEmpty */
int pqIsEmpty( PriorityQ *pq )
{
	return (pq->size == 0) && pqHeapIsEmpty( pq->heap );
}

/* really tessPqSortDelete */
void pqDelete( PriorityQ *pq, PQhandle curr )
{
	if( curr >= 0 ) {
		pqHeapDelete( pq->heap, curr );
		return;
	}
	curr = -(curr+1);
	assert( curr < pq->max && pq->keys[curr] != NULL );

	pq->keys[curr] = NULL;
	while( pq->size > 0 && *(pq->order[pq->size-1]) == NULL ) {
		-- pq->size;
	}
}

//-------------------------------------------------------------------------------------------------------------------------------------------------

#define Dot(u,v)	(u[0]*v[0] + u[1]*v[1] + u[2]*v[2])

#if defined(FOR_TRITE_TEST_PROGRAM) || defined(TRUE_PROJECT)
static void Normalize( TESSreal v[3] )
{
	TESSreal len = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];

	assert( len > 0 );
	len = sqrtf( len );
	v[0] /= len;
	v[1] /= len;
	v[2] /= len;
}
#endif

#define ABS(x)	((x) < 0 ? -(x) : (x))

static int LongAxis( TESSreal v[3] )
{
	int i = 0;

	if( ABS(v[1]) > ABS(v[0]) ) { i = 1; }
	if( ABS(v[2]) > ABS(v[i]) ) { i = 2; }
	return i;
}

static int ShortAxis( TESSreal v[3] )
{
	int i = 0;

	if( ABS(v[1]) < ABS(v[0]) ) { i = 1; }
	if( ABS(v[2]) < ABS(v[i]) ) { i = 2; }
	return i;
}

static void ComputeNormal( TESStesselator *tess, TESSreal norm[3] )
{
	TESSvertex *v, *v1, *v2;
	TESSreal c, tLen2, maxLen2;
	TESSreal maxVal[3], minVal[3], d1[3], d2[3], tNorm[3];
	TESSvertex *maxVert[3], *minVert[3];
	TESSvertex *vHead = &tess->mesh->vHead;
	int i;

	v = vHead->next;
	for( i = 0; i < 3; ++i ) {
		c = v->coords[i];
		minVal[i] = c;
		minVert[i] = v;
		maxVal[i] = c;
		maxVert[i] = v;
	}

	for( v = vHead->next; v != vHead; v = v->next ) {
		for( i = 0; i < 3; ++i ) {
			c = v->coords[i];
			if( c < minVal[i] ) { minVal[i] = c; minVert[i] = v; }
			if( c > maxVal[i] ) { maxVal[i] = c; maxVert[i] = v; }
		}
	}

	/* Find two vertices separated by at least 1/sqrt(3) of the maximum
	* distance between any two vertices
	*/
	i = 0;
	if( maxVal[1] - minVal[1] > maxVal[0] - minVal[0] ) { i = 1; }
	if( maxVal[2] - minVal[2] > maxVal[i] - minVal[i] ) { i = 2; }
	if( minVal[i] >= maxVal[i] ) {
		/* All vertices are the same -- normal doesn't matter */
		norm[0] = 0; norm[1] = 0; norm[2] = 1;
		return;
	}

	/* Look for a third vertex which forms the triangle with maximum area
	* (Length of normal == twice the triangle area)
	*/
	maxLen2 = 0;
	v1 = minVert[i];
	v2 = maxVert[i];
	d1[0] = v1->coords[0] - v2->coords[0];
	d1[1] = v1->coords[1] - v2->coords[1];
	d1[2] = v1->coords[2] - v2->coords[2];
	for( v = vHead->next; v != vHead; v = v->next ) {
		d2[0] = v->coords[0] - v2->coords[0];
		d2[1] = v->coords[1] - v2->coords[1];
		d2[2] = v->coords[2] - v2->coords[2];
		tNorm[0] = d1[1]*d2[2] - d1[2]*d2[1];
		tNorm[1] = d1[2]*d2[0] - d1[0]*d2[2];
		tNorm[2] = d1[0]*d2[1] - d1[1]*d2[0];
		tLen2 = tNorm[0]*tNorm[0] + tNorm[1]*tNorm[1] + tNorm[2]*tNorm[2];
		if( tLen2 > maxLen2 ) {
			maxLen2 = tLen2;
			norm[0] = tNorm[0];
			norm[1] = tNorm[1];
			norm[2] = tNorm[2];
		}
	}

	if( maxLen2 <= 0 ) {
		/* All points lie on a single line -- any decent normal will do */
		norm[0] = norm[1] = norm[2] = 0;
		norm[ShortAxis(d1)] = 1;
	}
}


static void CheckOrientation( TESStesselator *tess )
{
	TESSreal area;
	TESSface *f, *fHead = &tess->mesh->fHead;
	TESSvertex *v, *vHead = &tess->mesh->vHead;
	TESShalfEdge *e;

	/* When we compute the normal automatically, we choose the orientation
	* so that the the sum of the signed areas of all contours is non-negative.
	*/
	area = 0;
	for( f = fHead->next; f != fHead; f = f->next ) {
		e = f->anEdge;
		if( e->winding <= 0 ) continue;
		do {
			area += (e->Org->s - e->Dst->s) * (e->Org->t + e->Dst->t);
			e = e->Lnext;
		} while( e != f->anEdge );
	}
	if( area < 0 ) {
		/* Reverse the orientation by flipping all the t-coordinates */
		for( v = vHead->next; v != vHead; v = v->next ) {
			v->t = - v->t;
		}
		tess->tUnit[0] = - tess->tUnit[0];
		tess->tUnit[1] = - tess->tUnit[1];
		tess->tUnit[2] = - tess->tUnit[2];
	}
}

#ifdef FOR_TRITE_TEST_PROGRAM
#include <stdlib.h>
extern int RandomSweep;
#define S_UNIT_X	(RandomSweep ? (2*drand48()-1) : 1.0)
#define S_UNIT_Y	(RandomSweep ? (2*drand48()-1) : 0.0)
#else
#if defined(SLANTED_SWEEP) 
/* The "feature merging" is not intended to be complete.  There are
* special cases where edges are nearly parallel to the sweep line
* which are not implemented.  The algorithm should still behave
* robustly (ie. produce a reasonable tesselation) in the presence
* of such edges, however it may miss features which could have been
* merged.  We could minimize this effect by choosing the sweep line
* direction to be something unusual (ie. not parallel to one of the
* coordinate axes).
*/
#define S_UNIT_X	(TESSreal)0.50941539564955385	/* Pre-normalized */
#define S_UNIT_Y	(TESSreal)0.86052074622010633
#else
#define S_UNIT_X	(TESSreal)1.0
#define S_UNIT_Y	(TESSreal)0.0
#endif
#endif

/* Determine the polygon normal and project vertices onto the plane
* of the polygon.
*/
void tessProjectPolygon( TESStesselator *tess )
{
	TESSvertex *v, *vHead = &tess->mesh->vHead;
	TESSreal norm[3];
	TESSreal *sUnit, *tUnit;
	int i, first, computedNormal = FALSE;

	norm[0] = tess->normal[0];
	norm[1] = tess->normal[1];
	norm[2] = tess->normal[2];
	if( norm[0] == 0 && norm[1] == 0 && norm[2] == 0 ) {
		ComputeNormal( tess, norm );
		computedNormal = TRUE;
	}
	sUnit = tess->sUnit;
	tUnit = tess->tUnit;
	i = LongAxis( norm );

#if defined(FOR_TRITE_TEST_PROGRAM) || defined(TRUE_PROJECT)
	/* Choose the initial sUnit vector to be approximately perpendicular
	* to the normal.
	*/
	Normalize( norm );

	sUnit[i] = 0;
	sUnit[(i+1)%3] = S_UNIT_X;
	sUnit[(i+2)%3] = S_UNIT_Y;

	/* Now make it exactly perpendicular */
	w = Dot( sUnit, norm );
	sUnit[0] -= w * norm[0];
	sUnit[1] -= w * norm[1];
	sUnit[2] -= w * norm[2];
	Normalize( sUnit );

	/* Choose tUnit so that (sUnit,tUnit,norm) form a right-handed frame */
	tUnit[0] = norm[1]*sUnit[2] - norm[2]*sUnit[1];
	tUnit[1] = norm[2]*sUnit[0] - norm[0]*sUnit[2];
	tUnit[2] = norm[0]*sUnit[1] - norm[1]*sUnit[0];
	Normalize( tUnit );
#else
	/* Project perpendicular to a coordinate axis -- better numerically */
	sUnit[i] = 0;
	sUnit[(i+1)%3] = S_UNIT_X;
	sUnit[(i+2)%3] = S_UNIT_Y;

	tUnit[i] = 0;
	tUnit[(i+1)%3] = (norm[i] > 0) ? -S_UNIT_Y : S_UNIT_Y;
	tUnit[(i+2)%3] = (norm[i] > 0) ? S_UNIT_X : -S_UNIT_X;
#endif

	/* Project the vertices onto the sweep plane */
	for( v = vHead->next; v != vHead; v = v->next )
	{
		v->s = Dot( v->coords, sUnit );
		v->t = Dot( v->coords, tUnit );
	}
	if( computedNormal ) {
		CheckOrientation( tess );
	}

	/* Compute ST bounds. */
	first = 1;
	for( v = vHead->next; v != vHead; v = v->next )
	{
		if (first)
		{
			tess->bmin[0] = tess->bmax[0] = v->s;
			tess->bmin[1] = tess->bmax[1] = v->t;
			first = 0;
		}
		else
		{
			if (v->s < tess->bmin[0]) tess->bmin[0] = v->s;
			if (v->s > tess->bmax[0]) tess->bmax[0] = v->s;
			if (v->t < tess->bmin[1]) tess->bmin[1] = v->t;
			if (v->t > tess->bmax[1]) tess->bmax[1] = v->t;
		}
	}
}

#define AddWinding(eDst,eSrc)	(eDst->winding += eSrc->winding, \
	eDst->Sym->winding += eSrc->Sym->winding)

/* tessMeshTessellateMonoRegion( face ) tessellates a monotone region
* (what else would it do??)  The region must consist of a single
* loop of half-edges (see mesh.h) oriented CCW.  "Monotone" in this
* case means that any vertical line intersects the interior of the
* region in a single interval.  
*
* Tessellation consists of adding interior edges (actually pairs of
* half-edges), to split the region into non-overlapping triangles.
*
* The basic idea is explained in Preparata and Shamos (which I don''t
* have handy right now), although their implementation is more
* complicated than this one.  The are two edge chains, an upper chain
* and a lower chain.  We process all vertices from both chains in order,
* from right to left.
*
* The algorithm ensures that the following invariant holds after each
* vertex is processed: the untessellated region consists of two
* chains, where one chain (say the upper) is a single edge, and
* the other chain is concave.  The left vertex of the single edge
* is always to the left of all vertices in the concave chain.
*
* Each step consists of adding the rightmost unprocessed vertex to one
* of the two chains, and forming a fan of triangles from the rightmost
* of two chain endpoints.  Determining whether we can add each triangle
* to the fan is a simple orientation test.  By making the fan as large
* as possible, we restore the invariant (check it yourself).
*/
int tessMeshTessellateMonoRegion( TESSmesh *mesh, TESSface *face )
{
	TESShalfEdge *up, *lo;

	/* All edges are oriented CCW around the boundary of the region.
	* First, find the half-edge whose origin vertex is rightmost.
	* Since the sweep goes from left to right, face->anEdge should
	* be close to the edge we want.
	*/
	up = face->anEdge;
	assert( up->Lnext != up && up->Lnext->Lnext != up );

	for( ; VertLeq( up->Dst, up->Org ); up = up->Lprev )
		;
	for( ; VertLeq( up->Org, up->Dst ); up = up->Lnext )
		;
	lo = up->Lprev;

	while( up->Lnext != lo ) {
		if( VertLeq( up->Dst, lo->Org )) {
			/* up->Dst is on the left.  It is safe to form triangles from lo->Org.
			* The EdgeGoesLeft test guarantees progress even when some triangles
			* are CW, given that the upper and lower chains are truly monotone.
			*/
			while( lo->Lnext != up && (EdgeGoesLeft( lo->Lnext )
				|| EdgeSign( lo->Org, lo->Dst, lo->Lnext->Dst ) <= 0 )) {
					TESShalfEdge *tempHalfEdge= tessMeshConnect( mesh, lo->Lnext, lo );
					if (tempHalfEdge == NULL) return 0;
					lo = tempHalfEdge->Sym;
			}
			lo = lo->Lprev;
		} else {
			/* lo->Org is on the left.  We can make CCW triangles from up->Dst. */
			while( lo->Lnext != up && (EdgeGoesRight( up->Lprev )
				|| EdgeSign( up->Dst, up->Org, up->Lprev->Org ) >= 0 )) {
					TESShalfEdge *tempHalfEdge= tessMeshConnect( mesh, up, up->Lprev );
					if (tempHalfEdge == NULL) return 0;
					up = tempHalfEdge->Sym;
			}
			up = up->Lnext;
		}
	}

	/* Now lo->Org == up->Dst == the leftmost vertex.  The remaining region
	* can be tessellated in a fan from this leftmost vertex.
	*/
	assert( lo->Lnext != up );
	while( lo->Lnext->Lnext != up ) {
		TESShalfEdge *tempHalfEdge= tessMeshConnect( mesh, lo->Lnext, lo );
		if (tempHalfEdge == NULL) return 0;
		lo = tempHalfEdge->Sym;
	}

	return 1;
}


/* tessMeshTessellateInterior( mesh ) tessellates each region of
* the mesh which is marked "inside" the polygon.  Each such region
* must be monotone.
*/
int tessMeshTessellateInterior( TESSmesh *mesh )
{
	TESSface *f, *next;

	/*LINTED*/
	for( f = mesh->fHead.next; f != &mesh->fHead; f = next ) {
		/* Make sure we don''t try to tessellate the new triangles. */
		next = f->next;
		if( f->inside ) {
			if ( !tessMeshTessellateMonoRegion( mesh, f ) ) return 0;
		}
	}

	return 1;
}


/* tessMeshDiscardExterior( mesh ) zaps (ie. sets to NULL) all faces
* which are not marked "inside" the polygon.  Since further mesh operations
* on NULL faces are not allowed, the main purpose is to clean up the
* mesh so that exterior loops are not represented in the data structure.
*/
void tessMeshDiscardExterior( TESSmesh *mesh )
{
	TESSface *f, *next;

	/*LINTED*/
	for( f = mesh->fHead.next; f != &mesh->fHead; f = next ) {
		/* Since f will be destroyed, save its next pointer. */
		next = f->next;
		if( ! f->inside ) {
			tessMeshZapFace( mesh, f );
		}
	}
}

/* tessMeshSetWindingNumber( mesh, value, keepOnlyBoundary ) resets the
* winding numbers on all edges so that regions marked "inside" the
* polygon have a winding number of "value", and regions outside
* have a winding number of 0.
*
* If keepOnlyBoundary is TRUE, it also deletes all edges which do not
* separate an interior region from an exterior one.
*/
int tessMeshSetWindingNumber( TESSmesh *mesh, int value,
							 int keepOnlyBoundary )
{
	TESShalfEdge *e, *eNext;

	for( e = mesh->eHead.next; e != &mesh->eHead; e = eNext ) {
		eNext = e->next;
		if( e->Rface->inside != e->Lface->inside ) {

			/* This is a boundary edge (one side is interior, one is exterior). */
			e->winding = (e->Lface->inside) ? value : -value;
		} else {

			/* Both regions are interior, or both are exterior. */
			if( ! keepOnlyBoundary ) {
				e->winding = 0;
			} else {
				if ( !tessMeshDelete( mesh, e ) ) return 0;
			}
		}
	}
	return 1;
}

void* heapAlloc( void* userData, size_t size )
{
	TESS_NOTUSED( userData );
	return malloc( size );
}

void* heapRealloc( void *userData, void* ptr, size_t size )
{
	TESS_NOTUSED( userData );
	return realloc( ptr, size );
}

void heapFree( void* userData, void* ptr )
{
	TESS_NOTUSED( userData );
	free( ptr );
}

static TESSalloc defaulAlloc =
{
	heapAlloc,
	heapRealloc,
	heapFree,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
};

TESStesselator* tessNewTess( TESSalloc* alloc )
{
	TESStesselator* tess;

	if (alloc == NULL)
		alloc = &defaulAlloc;
	
	/* Only initialize fields which can be changed by the api.  Other fields
	* are initialized where they are used.
	*/

	tess = (TESStesselator *)alloc->memalloc( alloc->userData, sizeof( TESStesselator ));
	if ( tess == NULL ) {
		return 0;          /* out of memory */
	}
	tess->alloc = *alloc;
	/* Check and set defaults. */
	if (tess->alloc.meshEdgeBucketSize == 0)
		tess->alloc.meshEdgeBucketSize = 512;
	if (tess->alloc.meshVertexBucketSize == 0)
		tess->alloc.meshVertexBucketSize = 512;
	if (tess->alloc.meshFaceBucketSize == 0)
		tess->alloc.meshFaceBucketSize = 256;
	if (tess->alloc.dictNodeBucketSize == 0)
		tess->alloc.dictNodeBucketSize = 512;
	if (tess->alloc.regionBucketSize == 0)
		tess->alloc.regionBucketSize = 256;
	
	tess->normal[0] = 0;
	tess->normal[1] = 0;
	tess->normal[2] = 0;

	tess->bmin[0] = 0;
	tess->bmin[1] = 0;
	tess->bmax[0] = 0;
	tess->bmax[1] = 0;

	tess->windingRule = TESS_WINDING_ODD;

	if (tess->alloc.regionBucketSize < 16)
		tess->alloc.regionBucketSize = 16;
	if (tess->alloc.regionBucketSize > 4096)
		tess->alloc.regionBucketSize = 4096;
	tess->regionPool = createBucketAlloc( &tess->alloc, "Regions",
										 sizeof(ActiveRegion), tess->alloc.regionBucketSize );

	// Initialize to begin polygon.
	tess->mesh = NULL;

	tess->outOfMemory = 0;
	tess->vertexIndexCounter = 0;
	
	tess->vertices = 0;
	tess->vertexIndices = 0;
	tess->vertexCount = 0;
	tess->elements = 0;
	tess->elementCount = 0;

	return tess;
}

void tessDeleteTess( TESStesselator *tess )
{
	
	struct TESSalloc alloc = tess->alloc;
	
	deleteBucketAlloc( tess->regionPool );

	if( tess->mesh != NULL ) {
		tessMeshDeleteMesh( &alloc, tess->mesh );
		tess->mesh = NULL;
	}
	if (tess->vertices != NULL) {
		alloc.memfree( alloc.userData, tess->vertices );
		tess->vertices = 0;
	}
	if (tess->vertexIndices != NULL) {
		alloc.memfree( alloc.userData, tess->vertexIndices );
		tess->vertexIndices = 0;
	}
	if (tess->elements != NULL) {
		alloc.memfree( alloc.userData, tess->elements );
		tess->elements = 0;
	}

	alloc.memfree( alloc.userData, tess );
}


static TESSindex GetNeighbourFace(TESShalfEdge* edge)
{
	if (!edge->Rface)
		return TESS_UNDEF;
	if (!edge->Rface->inside)
		return TESS_UNDEF;
	return edge->Rface->n;
}

void OutputPolymesh( TESStesselator *tess, TESSmesh *mesh, int elementType, int polySize, int vertexSize )
{
	TESSvertex* v = 0;
	TESSface* f = 0;
	TESShalfEdge* edge = 0;
	int maxFaceCount = 0;
	int maxVertexCount = 0;
	int faceVerts, i;
	TESSindex *elements = 0;
	TESSreal *vert;

	// Assume that the input data is triangles now.
	// Try to merge as many polygons as possible
	if (polySize > 3)
	{
		if (!tessMeshMergeConvexFaces( mesh, polySize ))
		{
			tess->outOfMemory = 1;
			return;
		}
	}

	// Mark unused
	for ( v = mesh->vHead.next; v != &mesh->vHead; v = v->next )
		v->n = TESS_UNDEF;

	// Create unique IDs for all vertices and faces.
	for ( f = mesh->fHead.next; f != &mesh->fHead; f = f->next )
	{
		f->n = TESS_UNDEF;
		if( !f->inside ) continue;

		edge = f->anEdge;
		faceVerts = 0;
		do
		{
			v = edge->Org;
			if ( v->n == TESS_UNDEF )
			{
				v->n = maxVertexCount;
				maxVertexCount++;
			}
			faceVerts++;
			edge = edge->Lnext;
		}
		while (edge != f->anEdge);
		
		assert( faceVerts <= polySize );

		f->n = maxFaceCount;
		++maxFaceCount;
	}

	tess->elementCount = maxFaceCount;
	if (elementType == TESS_CONNECTED_POLYGONS)
		maxFaceCount *= 2;
	tess->elements = (TESSindex*)tess->alloc.memalloc( tess->alloc.userData,
													  sizeof(TESSindex) * maxFaceCount * polySize );
	if (!tess->elements)
	{
		tess->outOfMemory = 1;
		return;
	}
	
	tess->vertexCount = maxVertexCount;
	tess->vertices = (TESSreal*)tess->alloc.memalloc( tess->alloc.userData,
													 sizeof(TESSreal) * tess->vertexCount * vertexSize );
	if (!tess->vertices)
	{
		tess->outOfMemory = 1;
		return;
	}

	tess->vertexIndices = (TESSindex*)tess->alloc.memalloc( tess->alloc.userData,
														    sizeof(TESSindex) * tess->vertexCount );
	if (!tess->vertexIndices)
	{
		tess->outOfMemory = 1;
		return;
	}
	
	// Output vertices.
	for ( v = mesh->vHead.next; v != &mesh->vHead; v = v->next )
	{
		if ( v->n != TESS_UNDEF )
		{
			// Store coordinate
			vert = &tess->vertices[v->n*vertexSize];
			vert[0] = v->coords[0];
			vert[1] = v->coords[1];
			if ( vertexSize > 2 )
				vert[2] = v->coords[2];
			// Store vertex index.
			tess->vertexIndices[v->n] = v->idx;
		}
	}

	// Output indices.
	elements = tess->elements;
	for ( f = mesh->fHead.next; f != &mesh->fHead; f = f->next )
	{
		if ( !f->inside ) continue;
		
		// Store polygon
		edge = f->anEdge;
		faceVerts = 0;
		do
		{
			v = edge->Org;
			*elements++ = v->n;
			faceVerts++;
			edge = edge->Lnext;
		}
		while (edge != f->anEdge);
		// Fill unused.
		for (i = faceVerts; i < polySize; ++i)
			*elements++ = TESS_UNDEF;

		// Store polygon connectivity
		if ( elementType == TESS_CONNECTED_POLYGONS )
		{
			edge = f->anEdge;
			do
			{
				*elements++ = GetNeighbourFace( edge );
				edge = edge->Lnext;
			}
			while (edge != f->anEdge);
			// Fill unused.
			for (i = faceVerts; i < polySize; ++i)
				*elements++ = TESS_UNDEF;
		}
	}
}

void OutputContours( TESStesselator *tess, TESSmesh *mesh, int vertexSize )
{
	TESSface *f = 0;
	TESShalfEdge *edge = 0;
	TESShalfEdge *start = 0;
	TESSreal *verts = 0;
	TESSindex *elements = 0;
	TESSindex *vertInds = 0;
	int startVert = 0;
	int vertCount = 0;

	tess->vertexCount = 0;
	tess->elementCount = 0;

	for ( f = mesh->fHead.next; f != &mesh->fHead; f = f->next )
	{
		if ( !f->inside ) continue;

		start = edge = f->anEdge;
		do
		{
			++tess->vertexCount;
			edge = edge->Lnext;
		}
		while ( edge != start );

		++tess->elementCount;
	}

	tess->elements = (TESSindex*)tess->alloc.memalloc( tess->alloc.userData,
													  sizeof(TESSindex) * tess->elementCount * 2 );
	if (!tess->elements)
	{
		tess->outOfMemory = 1;
		return;
	}
	
	tess->vertices = (TESSreal*)tess->alloc.memalloc( tess->alloc.userData,
													  sizeof(TESSreal) * tess->vertexCount * vertexSize );
	if (!tess->vertices)
	{
		tess->outOfMemory = 1;
		return;
	}

	tess->vertexIndices = (TESSindex*)tess->alloc.memalloc( tess->alloc.userData,
														    sizeof(TESSindex) * tess->vertexCount );
	if (!tess->vertexIndices)
	{
		tess->outOfMemory = 1;
		return;
	}
	
	verts = tess->vertices;
	elements = tess->elements;
	vertInds = tess->vertexIndices;

	startVert = 0;

	for ( f = mesh->fHead.next; f != &mesh->fHead; f = f->next )
	{
		if ( !f->inside ) continue;

		vertCount = 0;
		start = edge = f->anEdge;
		do
		{
			*verts++ = edge->Org->coords[0];
			*verts++ = edge->Org->coords[1];
			if ( vertexSize > 2 )
				*verts++ = edge->Org->coords[2];
			*vertInds++ = edge->Org->idx;
			++vertCount;
			edge = edge->Lnext;
		}
		while ( edge != start );

		elements[0] = startVert;
		elements[1] = vertCount;
		elements += 2;

		startVert += vertCount;
	}
}

void tessAddContour( TESStesselator *tess, int size, const void* vertices,
					int stride, int numVertices )
{
	const unsigned char *src = (const unsigned char*)vertices;
	TESShalfEdge *e;
	int i;

	if ( tess->mesh == NULL )
	  	tess->mesh = tessMeshNewMesh( &tess->alloc );
 	if ( tess->mesh == NULL ) {
		tess->outOfMemory = 1;
		return;
	}

	if ( size < 2 )
		size = 2;
	if ( size > 3 )
		size = 3;

	e = NULL;

	for( i = 0; i < numVertices; ++i )
	{
		const TESSreal* coords = (const TESSreal*)src;
		src += stride;

		if( e == NULL ) {
			/* Make a self-loop (one vertex, one edge). */
			e = tessMeshMakeEdge( tess->mesh );
			if ( e == NULL ) {
				tess->outOfMemory = 1;
				return;
			}
			if ( !tessMeshSplice( tess->mesh, e, e->Sym ) ) {
				tess->outOfMemory = 1;
				return;
			}
		} else {
			/* Create a new vertex and edge which immediately follow e
			* in the ordering around the left face.
			*/
			if ( tessMeshSplitEdge( tess->mesh, e ) == NULL ) {
				tess->outOfMemory = 1;
				return;
			}
			e = e->Lnext;
		}

		/* The new vertex is now e->Org. */
		e->Org->coords[0] = coords[0];
		e->Org->coords[1] = coords[1];
		if ( size > 2 )
			e->Org->coords[2] = coords[2];
		else
			e->Org->coords[2] = 0;
		/* Store the insertion number so that the vertex can be later recognized. */
		e->Org->idx = tess->vertexIndexCounter++;

		/* The winding of an edge says how the winding number changes as we
		* cross from the edge''s right face to its left face.  We add the
		* vertices in such an order that a CCW contour will add +1 to
		* the winding number of the region inside the contour.
		*/
		e->winding = 1;
		e->Sym->winding = -1;
	}
}

int tessTesselate( TESStesselator *tess, int windingRule, int elementType,
				  int polySize, int vertexSize, const TESSreal* normal )
{
	TESSmesh *mesh;
	int rc = 1;

	if (tess->vertices != NULL) {
		tess->alloc.memfree( tess->alloc.userData, tess->vertices );
		tess->vertices = 0;
	}
	if (tess->elements != NULL) {
		tess->alloc.memfree( tess->alloc.userData, tess->elements );
		tess->elements = 0;
	}
	if (tess->vertexIndices != NULL) {
		tess->alloc.memfree( tess->alloc.userData, tess->vertexIndices );
		tess->vertexIndices = 0;
	}

	tess->vertexIndexCounter = 0;
	
	if (normal)
	{
		tess->normal[0] = normal[0];
		tess->normal[1] = normal[1];
		tess->normal[2] = normal[2];
	}

	tess->windingRule = windingRule;

	if (vertexSize < 2)
		vertexSize = 2;
	if (vertexSize > 3)
		vertexSize = 3;

	if (setjmp(tess->env) != 0) { 
		/* come back here if out of memory */
		return 0;
	}

	if (!tess->mesh)
	{
		return 0;
	}

	/* Determine the polygon normal and project vertices onto the plane
	* of the polygon.
	*/
	tessProjectPolygon( tess );

	/* tessComputeInterior( tess ) computes the planar arrangement specified
	* by the given contours, and further subdivides this arrangement
	* into regions.  Each region is marked "inside" if it belongs
	* to the polygon, according to the rule given by tess->windingRule.
	* Each interior region is guaranteed be monotone.
	*/
	if ( !tessComputeInterior( tess ) ) {
		longjmp(tess->env,1);  /* could've used a label */
	}

	mesh = tess->mesh;

	/* If the user wants only the boundary contours, we throw away all edges
	* except those which separate the interior from the exterior.
	* Otherwise we tessellate all the regions marked "inside".
	*/
	if (elementType == TESS_BOUNDARY_CONTOURS) {
		rc = tessMeshSetWindingNumber( mesh, 1, TRUE );
	} else {
		rc = tessMeshTessellateInterior( mesh ); 
	}
	if (rc == 0) longjmp(tess->env,1);  /* could've used a label */

	tessMeshCheckMesh( mesh );

	if (elementType == TESS_BOUNDARY_CONTOURS) {
		OutputContours( tess, mesh, vertexSize );     /* output contours */
	}
	else
	{
		OutputPolymesh( tess, mesh, elementType, polySize, vertexSize );     /* output polygons */
	}

	tessMeshDeleteMesh( &tess->alloc, mesh );
	tess->mesh = NULL;

	if (tess->outOfMemory)
		return 0;
	return 1;
}

int tessGetVertexCount( TESStesselator *tess )
{
	return tess->vertexCount;
}

const TESSreal* tessGetVertices( TESStesselator *tess )
{
	return tess->vertices;
}

const TESSindex* tessGetVertexIndices( TESStesselator *tess )
{
	return tess->vertexIndices;
}

int tessGetElementCount( TESStesselator *tess )
{
	return tess->elementCount;
}

const int* tessGetElements( TESStesselator *tess )
{
	return tess->elements;
}

//-------------------------------------------------------------------------------------------------------------------------------------------------

//#define CHECK_BOUNDS

typedef struct BucketAlloc BucketAlloc;
typedef struct Bucket Bucket;

struct Bucket
{
	Bucket *next;
};

struct BucketAlloc
{
	void *freelist;
	Bucket *buckets;
	unsigned int itemSize;
	unsigned int bucketSize;
	const char *name;
	TESSalloc* alloc;
};

static int CreateBucket( struct BucketAlloc* ba )
{
	unsigned int size;
	Bucket* bucket;
	void* freelist;
	unsigned char* head;
	unsigned char* it;

	// Allocate memory for the bucket
	size = sizeof(Bucket) + ba->itemSize * ba->bucketSize;
	bucket = (Bucket*)ba->alloc->memalloc( ba->alloc->userData, size );
	if ( !bucket )
		return 0;
	bucket->next = 0;

	// Add the bucket into the list of buckets.
	bucket->next = ba->buckets;
	ba->buckets = bucket;

	// Add new items to the free list.
	freelist = ba->freelist;
	head = (unsigned char*)bucket + sizeof(Bucket);
	it = head + ba->itemSize * ba->bucketSize;
	do
	{
		it -= ba->itemSize;
		// Store pointer to next free item.
		*((void**)it) = freelist;
		// Pointer to next location containing a free item.
		freelist = (void*)it;
	}
	while ( it != head );
	// Update pointer to next location containing a free item.
	ba->freelist = (void*)it;

	return 1;
}

static void *NextFreeItem( struct BucketAlloc *ba )
{
	return *(void**)ba->freelist;
}

struct BucketAlloc* createBucketAlloc( TESSalloc* alloc, const char* name,
									  unsigned int itemSize, unsigned int bucketSize )
{
	BucketAlloc* ba = (BucketAlloc*)alloc->memalloc( alloc->userData, sizeof(BucketAlloc) );

	ba->alloc = alloc;
	ba->name = name;
	ba->itemSize = itemSize;
	if ( ba->itemSize < sizeof(void*) )
		ba->itemSize = sizeof(void*);
	ba->bucketSize = bucketSize;
	ba->freelist = 0;
	ba->buckets = 0;

	if ( !CreateBucket( ba ) )
	{
		alloc->memfree( alloc->userData, ba );
		return 0;
	}

	return ba;
}

void* bucketAlloc( struct BucketAlloc *ba )
{
	void *it;

	// If running out of memory, allocate new bucket and update the freelist.
	if ( !ba->freelist || !NextFreeItem( ba ) )
	{
		if ( !CreateBucket( ba ) )
			return 0;
	}

	// Pop item from in front of the free list.
	it = ba->freelist;
	ba->freelist = NextFreeItem( ba );

	return it;
}

void bucketFree( struct BucketAlloc *ba, void *ptr )
{
#ifdef CHECK_BOUNDS
	int inBounds = 0;
	Bucket *bucket;

	// Check that the pointer is allocated with this allocator.
	bucket = ba->buckets;
	while ( bucket )
	{
		void *bucketMin = (void*)((unsigned char*)bucket + sizeof(Bucket));
		void *bucketMax = (void*)((unsigned char*)bucket + sizeof(Bucket) + ba->itemSize * ba->bucketSize);
		if ( ptr >= bucketMin && ptr < bucketMax )
		{
			inBounds = 1;
			break;
		}
		bucket = bucket->next;			
	}		

	if ( inBounds )
	{
		// Add the node in front of the free list.
		*(void**)ptr = ba->freelist;
		ba->freelist = ptr;
	}
	else
	{
		printf("ERROR! pointer 0x%p does not belong to allocator '%s'\n", ba->name);
	}
#else
	// Add the node in front of the free list.
	*(void**)ptr = ba->freelist;
	ba->freelist = ptr;
#endif
}

void deleteBucketAlloc( struct BucketAlloc *ba )
{
	TESSalloc* alloc = ba->alloc;
	Bucket *bucket = ba->buckets;
	Bucket *next;
	while ( bucket )
	{
		next = bucket->next;
		alloc->memfree( alloc->userData, bucket );
		bucket = next;
	}		
	ba->freelist = 0;
	ba->buckets = 0;
	alloc->memfree( alloc->userData, ba );
}

//-------------------------------------------------------------------------------------------------------------------------------------------------
