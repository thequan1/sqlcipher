/*
** 2004 May 26
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains code use to manipulate "Mem" structure.  A "Mem"
** stores a single value in the VDBE.  Mem is an opaque structure visible
** only within the VDBE.  Interface routines refer to a Mem using the
** name sqlite_value
*/
#include "sqliteInt.h"
#include "vdbeInt.h"

#ifdef SQLITE_DEBUG
/*
** Check invariants on a Mem object.
**
** This routine is intended for use inside of assert() statements, like
** this:    assert( sqlite3VdbeCheckMemInvariants(pMem) );
*/
int sqlite3VdbeCheckMemInvariants(Mem *p){
  /* The MEM_Dyn bit is set if and only if Mem.xDel is a non-NULL destructor
  ** function for Mem.z 
  */
  assert( (p->flags & MEM_Dyn)==0 || p->xDel!=0 );
  assert( (p->flags & MEM_Dyn)!=0 || p->xDel==0 );

  /* If p holds a string or blob, the Mem.z must point to exactly
  ** one of the following:
  **
  **   (1) Memory in Mem.zMalloc and managed by the Mem object
  **   (2) Memory to be freed using Mem.xDel
  **   (3) An ephermal string or blob
  **   (4) A static string or blob
  */
  if( (p->flags & (MEM_Str|MEM_Blob)) && p->z!=0 ){
    assert( 
      ((p->z==p->zMalloc)? 1 : 0) +
      ((p->flags&MEM_Dyn)!=0 ? 1 : 0) +
      ((p->flags&MEM_Ephem)!=0 ? 1 : 0) +
      ((p->flags&MEM_Static)!=0 ? 1 : 0) == 1
    );
  }

  return 1;
}
#endif


/*
** If pMem is an object with a valid string representation, this routine
** ensures the internal encoding for the string representation is
** 'desiredEnc', one of SQLITE_UTF8, SQLITE_UTF16LE or SQLITE_UTF16BE.
**
** If pMem is not a string object, or the encoding of the string
** representation is already stored using the requested encoding, then this
** routine is a no-op.
**
** SQLITE_OK is returned if the conversion is successful (or not required).
** SQLITE_NOMEM may be returned if a malloc() fails during conversion
** between formats.
*/
int sqlite3VdbeChangeEncoding(Mem *pMem, int desiredEnc){
#ifndef SQLITE_OMIT_UTF16
  int rc;
#endif
  assert( (pMem->flags&MEM_RowSet)==0 );
  assert( desiredEnc==SQLITE_UTF8 || desiredEnc==SQLITE_UTF16LE
           || desiredEnc==SQLITE_UTF16BE );
  if( !(pMem->flags&MEM_Str) || pMem->enc==desiredEnc ){
    return SQLITE_OK;
  }
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
#ifdef SQLITE_OMIT_UTF16
  return SQLITE_ERROR;
#else

  /* MemTranslate() may return SQLITE_OK or SQLITE_NOMEM. If NOMEM is returned,
  ** then the encoding of the value may not have changed.
  */
  rc = sqlite3VdbeMemTranslate(pMem, (u8)desiredEnc);
  assert(rc==SQLITE_OK    || rc==SQLITE_NOMEM);
  assert(rc==SQLITE_OK    || pMem->enc!=desiredEnc);
  assert(rc==SQLITE_NOMEM || pMem->enc==desiredEnc);
  return rc;
#endif
}

/*
** Make sure pMem->z points to a writable allocation of at least 
** min(n,32) bytes.
**
** If the bPreserve argument is true, then copy of the content of
** pMem->z into the new allocation.  pMem must be either a string or
** blob if bPreserve is true.  If bPreserve is false, any prior content
** in pMem->z is discarded.
*/
int sqlite3VdbeMemGrow(Mem *pMem, int n, int bPreserve){
  assert( sqlite3VdbeCheckMemInvariants(pMem) );
  assert( (pMem->flags&MEM_RowSet)==0 );

  /* If the bPreserve flag is set to true, then the memory cell must already
  ** contain a valid string or blob value.  */
  assert( bPreserve==0 || pMem->flags&(MEM_Blob|MEM_Str) );
  testcase( bPreserve && pMem->z==0 );

  if( pMem->zMalloc==0 || sqlite3DbMallocSize(pMem->db, pMem->zMalloc)<n ){
    if( n<32 ) n = 32;
    if( bPreserve && pMem->z==pMem->zMalloc ){
      pMem->z = pMem->zMalloc = sqlite3DbReallocOrFree(pMem->db, pMem->z, n);
      bPreserve = 0;
    }else{
      sqlite3DbFree(pMem->db, pMem->zMalloc);
      pMem->zMalloc = sqlite3DbMallocRaw(pMem->db, n);
    }
    if( pMem->zMalloc==0 ){
      VdbeMemRelease(pMem);
      pMem->z = 0;
      pMem->flags = MEM_Null;  
      return SQLITE_NOMEM;
    }
  }

  if( pMem->z && bPreserve && pMem->z!=pMem->zMalloc ){
    memcpy(pMem->zMalloc, pMem->z, pMem->n);
  }
  if( (pMem->flags&MEM_Dyn)!=0 ){
    assert( pMem->xDel!=0 && pMem->xDel!=SQLITE_DYNAMIC );
    pMem->xDel((void *)(pMem->z));
  }

  pMem->z = pMem->zMalloc;
  pMem->flags &= ~(MEM_Dyn|MEM_Ephem|MEM_Static);
  pMem->xDel = 0;
  return SQLITE_OK;
}

/*
** Make the given Mem object MEM_Dyn.  In other words, make it so
** that any TEXT or BLOB content is stored in memory obtained from
** malloc().  In this way, we know that the memory is safe to be
** overwritten or altered.
**
** Return SQLITE_OK on success or SQLITE_NOMEM if malloc fails.
*/
int sqlite3VdbeMemMakeWriteable(Mem *pMem){
  int f;
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags&MEM_RowSet)==0 );
  ExpandBlob(pMem);
  f = pMem->flags;
  if( (f&(MEM_Str|MEM_Blob)) && pMem->z!=pMem->zMalloc ){
    if( sqlite3VdbeMemGrow(pMem, pMem->n + 2, 1) ){
      return SQLITE_NOMEM;
    }
    pMem->z[pMem->n] = 0;
    pMem->z[pMem->n+1] = 0;
    pMem->flags |= MEM_Term;
#ifdef SQLITE_DEBUG
    pMem->pScopyFrom = 0;
#endif
  }

  return SQLITE_OK;
}

/*
** If the given Mem* has a zero-filled tail, turn it into an ordinary
** blob stored in dynamically allocated space.
*/
#ifndef SQLITE_OMIT_INCRBLOB
int sqlite3VdbeMemExpandBlob(Mem *pMem){
  if( pMem->flags & MEM_Zero ){
    int nByte;
    assert( pMem->flags&MEM_Blob );
    assert( (pMem->flags&MEM_RowSet)==0 );
    assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );

    /* Set nByte to the number of bytes required to store the expanded blob. */
    nByte = pMem->n + pMem->u.nZero;
    if( nByte<=0 ){
      nByte = 1;
    }
    if( sqlite3VdbeMemGrow(pMem, nByte, 1) ){
      return SQLITE_NOMEM;
    }

    memset(&pMem->z[pMem->n], 0, pMem->u.nZero);
    pMem->n += pMem->u.nZero;
    pMem->flags &= ~(MEM_Zero|MEM_Term);
  }
  return SQLITE_OK;
}
#endif


/*
** Make sure the given Mem is \u0000 terminated.
*/
int sqlite3VdbeMemNulTerminate(Mem *pMem){
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  if( (pMem->flags & MEM_Term)!=0 || (pMem->flags & MEM_Str)==0 ){
    return SQLITE_OK;   /* Nothing to do */
  }
  if( sqlite3VdbeMemGrow(pMem, pMem->n+2, 1) ){
    return SQLITE_NOMEM;
  }
  pMem->z[pMem->n] = 0;
  pMem->z[pMem->n+1] = 0;
  pMem->flags |= MEM_Term;
  return SQLITE_OK;
}

/*
** Add MEM_Str to the set of representations for the given Mem.  Numbers
** are converted using sqlite3_snprintf().  Converting a BLOB to a string
** is a no-op.
**
** Existing representations MEM_Int and MEM_Real are *not* invalidated.
**
** A MEM_Null value will never be passed to this function. This function is
** used for converting values to text for returning to the user (i.e. via
** sqlite3_value_text()), or for ensuring that values to be used as btree
** keys are strings. In the former case a NULL pointer is returned the
** user and the later is an internal programming error.
*/
int sqlite3VdbeMemStringify(Mem *pMem, int enc){
  int rc = SQLITE_OK;
  int fg = pMem->flags;
  const int nByte = 32;

  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( !(fg&MEM_Zero) );
  assert( !(fg&(MEM_Str|MEM_Blob)) );
  assert( fg&(MEM_Int|MEM_Real) );
  assert( (pMem->flags&MEM_RowSet)==0 );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );


  if( sqlite3VdbeMemGrow(pMem, nByte, 0) ){
    return SQLITE_NOMEM;
  }

  /* For a Real or Integer, use sqlite3_mprintf() to produce the UTF-8
  ** string representation of the value. Then, if the required encoding
  ** is UTF-16le or UTF-16be do a translation.
  ** 
  ** FIX ME: It would be better if sqlite3_snprintf() could do UTF-16.
  */
  if( fg & MEM_Int ){
    sqlite3_snprintf(nByte, pMem->z, "%lld", pMem->u.i);
  }else{
    assert( fg & MEM_Real );
    sqlite3_snprintf(nByte, pMem->z, "%!.15g", pMem->r);
  }
  pMem->n = sqlite3Strlen30(pMem->z);
  pMem->enc = SQLITE_UTF8;
  pMem->flags |= MEM_Str|MEM_Term;
  sqlite3VdbeChangeEncoding(pMem, enc);
  return rc;
}

/*
** Memory cell pMem contains the context of an aggregate function.
** This routine calls the finalize method for that function.  The
** result of the aggregate is stored back into pMem.
**
** Return SQLITE_ERROR if the finalizer reports an error.  SQLITE_OK
** otherwise.
*/
int sqlite3VdbeMemFinalize(Mem *pMem, FuncDef *pFunc){
  int rc = SQLITE_OK;
  if( ALWAYS(pFunc && pFunc->xFinalize) ){
    sqlite3_context ctx;
    assert( (pMem->flags & MEM_Null)!=0 || pFunc==pMem->u.pDef );
    assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
    memset(&ctx, 0, sizeof(ctx));
    ctx.s.flags = MEM_Null;
    ctx.s.db = pMem->db;
    ctx.pMem = pMem;
    ctx.pFunc = pFunc;
    pFunc->xFinalize(&ctx); /* IMP: R-24505-23230 */
    assert( 0==(pMem->flags&MEM_Dyn) && !pMem->xDel );
    sqlite3DbFree(pMem->db, pMem->zMalloc);
    memcpy(pMem, &ctx.s, sizeof(ctx.s));
    rc = ctx.isError;
  }
  return rc;
}

/*
** If the memory cell contains a string value that must be freed by
** invoking an external callback, free it now. Calling this function
** does not free any Mem.zMalloc buffer.
*/
void sqlite3VdbeMemReleaseExternal(Mem *p){
  assert( p->db==0 || sqlite3_mutex_held(p->db->mutex) );
  if( p->flags&MEM_Agg ){
    sqlite3VdbeMemFinalize(p, p->u.pDef);
    assert( (p->flags & MEM_Agg)==0 );
    sqlite3VdbeMemRelease(p);
  }else if( p->flags&MEM_Dyn ){
    assert( (p->flags&MEM_RowSet)==0 );
    assert( p->xDel!=SQLITE_DYNAMIC && p->xDel!=0 );
    p->xDel((void *)p->z);
    p->xDel = 0;
  }else if( p->flags&MEM_RowSet ){
    sqlite3RowSetClear(p->u.pRowSet);
  }else if( p->flags&MEM_Frame ){
    sqlite3VdbeMemSetNull(p);
  }
}

/*
** Release any memory held by the Mem. This may leave the Mem in an
** inconsistent state, for example with (Mem.z==0) and
** (Mem.flags==MEM_Str).
*/
void sqlite3VdbeMemRelease(Mem *p){
  assert( sqlite3VdbeCheckMemInvariants(p) );
  VdbeMemRelease(p);
  if( p->zMalloc ){
    sqlite3DbFree(p->db, p->zMalloc);
    p->zMalloc = 0;
  }
  p->z = 0;
  assert( p->xDel==0 );  /* Zeroed by VdbeMemRelease() above */
}

/*
** Convert a 64-bit IEEE double into a 64-bit signed integer.
** If the double is out of range of a 64-bit signed integer then
** return the closest available 64-bit signed integer.
*/
static i64 doubleToInt64(double r){
#ifdef SQLITE_OMIT_FLOATING_POINT
  /* When floating-point is omitted, double and int64 are the same thing */
  return r;
#else
  /*
  ** Many compilers we encounter do not define constants for the
  ** minimum and maximum 64-bit integers, or they define them
  ** inconsistently.  And many do not understand the "LL" notation.
  ** So we define our own static constants here using nothing
  ** larger than a 32-bit integer constant.
  */
  static const i64 maxInt = LARGEST_INT64;
  static const i64 minInt = SMALLEST_INT64;

  if( r<=(double)minInt ){
    return minInt;
  }else if( r>=(double)maxInt ){
    return maxInt;
  }else{
    return (i64)r;
  }
#endif
}

/*
** Return some kind of integer value which is the best we can do
** at representing the value that *pMem describes as an integer.
** If pMem is an integer, then the value is exact.  If pMem is
** a floating-point then the value returned is the integer part.
** If pMem is a string or blob, then we make an attempt to convert
** it into a integer and return that.  If pMem represents an
** an SQL-NULL value, return 0.
**
** If pMem represents a string value, its encoding might be changed.
*/
i64 sqlite3VdbeIntValue(Mem *pMem){
  int flags;
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );
  flags = pMem->flags;
  if( flags & MEM_Int ){
    return pMem->u.i;
  }else if( flags & MEM_Real ){
    return doubleToInt64(pMem->r);
  }else if( flags & (MEM_Str|MEM_Blob) ){
    i64 value = 0;
    assert( pMem->z || pMem->n==0 );
    testcase( pMem->z==0 );
    sqlite3Atoi64(pMem->z, &value, pMem->n, pMem->enc);
    return value;
  }else{
    return 0;
  }
}

/*
** Return the best representation of pMem that we can get into a
** double.  If pMem is already a double or an integer, return its
** value.  If it is a string or blob, try to convert it to a double.
** If it is a NULL, return 0.0.
*/
double sqlite3VdbeRealValue(Mem *pMem){
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );
  if( pMem->flags & MEM_Real ){
    return pMem->r;
  }else if( pMem->flags & MEM_Int ){
    return (double)pMem->u.i;
  }else if( pMem->flags & (MEM_Str|MEM_Blob) ){
    /* (double)0 In case of SQLITE_OMIT_FLOATING_POINT... */
    double val = (double)0;
    sqlite3AtoF(pMem->z, &val, pMem->n, pMem->enc);
    return val;
  }else{
    /* (double)0 In case of SQLITE_OMIT_FLOATING_POINT... */
    return (double)0;
  }
}

/*
** The MEM structure is already a MEM_Real.  Try to also make it a
** MEM_Int if we can.
*/
void sqlite3VdbeIntegerAffinity(Mem *pMem){
  assert( pMem->flags & MEM_Real );
  assert( (pMem->flags & MEM_RowSet)==0 );
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->u.i = doubleToInt64(pMem->r);

  /* Only mark the value as an integer if
  **
  **    (1) the round-trip conversion real->int->real is a no-op, and
  **    (2) The integer is neither the largest nor the smallest
  **        possible integer (ticket #3922)
  **
  ** The second and third terms in the following conditional enforces
  ** the second condition under the assumption that addition overflow causes
  ** values to wrap around.
  */
  if( pMem->r==(double)pMem->u.i
   && pMem->u.i>SMALLEST_INT64
   && pMem->u.i<LARGEST_INT64
  ){
    pMem->flags |= MEM_Int;
  }
}

/*
** Convert pMem to type integer.  Invalidate any prior representations.
*/
int sqlite3VdbeMemIntegerify(Mem *pMem){
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags & MEM_RowSet)==0 );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->u.i = sqlite3VdbeIntValue(pMem);
  MemSetTypeFlag(pMem, MEM_Int);
  return SQLITE_OK;
}

/*
** Convert pMem so that it is of type MEM_Real.
** Invalidate any prior representations.
*/
int sqlite3VdbeMemRealify(Mem *pMem){
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->r = sqlite3VdbeRealValue(pMem);
  MemSetTypeFlag(pMem, MEM_Real);
  return SQLITE_OK;
}

/*
** Convert pMem so that it has types MEM_Real or MEM_Int or both.
** Invalidate any prior representations.
**
** Every effort is made to force the conversion, even if the input
** is a string that does not look completely like a number.  Convert
** as much of the string as we can and ignore the rest.
*/
int sqlite3VdbeMemNumerify(Mem *pMem){
  if( (pMem->flags & (MEM_Int|MEM_Real|MEM_Null))==0 ){
    assert( (pMem->flags & (MEM_Blob|MEM_Str))!=0 );
    assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
    if( 0==sqlite3Atoi64(pMem->z, &pMem->u.i, pMem->n, pMem->enc) ){
      MemSetTypeFlag(pMem, MEM_Int);
    }else{
      pMem->r = sqlite3VdbeRealValue(pMem);
      MemSetTypeFlag(pMem, MEM_Real);
      sqlite3VdbeIntegerAffinity(pMem);
    }
  }
  assert( (pMem->flags & (MEM_Int|MEM_Real|MEM_Null))!=0 );
  pMem->flags &= ~(MEM_Str|MEM_Blob);
  return SQLITE_OK;
}

/*
** Delete any previous value and set the value stored in *pMem to NULL.
*/
void sqlite3VdbeMemSetNull(Mem *pMem){
  if( pMem->flags & MEM_Frame ){
    VdbeFrame *pFrame = pMem->u.pFrame;
    pFrame->pParent = pFrame->v->pDelFrame;
    pFrame->v->pDelFrame = pFrame;
  }
  if( pMem->flags & MEM_RowSet ){
    sqlite3RowSetClear(pMem->u.pRowSet);
  }
  MemSetTypeFlag(pMem, MEM_Null);
}
void sqlite3ValueSetNull(sqlite3_value *p){
  sqlite3VdbeMemSetNull((Mem*)p); 
}

/*
** Delete any previous value and set the value to be a BLOB of length
** n containing all zeros.
*/
void sqlite3VdbeMemSetZeroBlob(Mem *pMem, int n){
  sqlite3VdbeMemRelease(pMem);
  pMem->flags = MEM_Blob|MEM_Zero;
  pMem->n = 0;
  if( n<0 ) n = 0;
  pMem->u.nZero = n;
  pMem->enc = SQLITE_UTF8;

#ifdef SQLITE_OMIT_INCRBLOB
  sqlite3VdbeMemGrow(pMem, n, 0);
  if( pMem->z ){
    pMem->n = n;
    memset(pMem->z, 0, n);
  }
#endif
}

/*
** Delete any previous value and set the value stored in *pMem to val,
** manifest type INTEGER.
*/
void sqlite3VdbeMemSetInt64(Mem *pMem, i64 val){
  sqlite3VdbeMemRelease(pMem);
  pMem->u.i = val;
  pMem->flags = MEM_Int;
}

#ifndef SQLITE_OMIT_FLOATING_POINT
/*
** Delete any previous value and set the value stored in *pMem to val,
** manifest type REAL.
*/
void sqlite3VdbeMemSetDouble(Mem *pMem, double val){
  if( sqlite3IsNaN(val) ){
    sqlite3VdbeMemSetNull(pMem);
  }else{
    sqlite3VdbeMemRelease(pMem);
    pMem->r = val;
    pMem->flags = MEM_Real;
  }
}
#endif

/*
** Delete any previous value and set the value of pMem to be an
** empty boolean index.
*/
void sqlite3VdbeMemSetRowSet(Mem *pMem){
  sqlite3 *db = pMem->db;
  assert( db!=0 );
  assert( (pMem->flags & MEM_RowSet)==0 );
  sqlite3VdbeMemRelease(pMem);
  pMem->zMalloc = sqlite3DbMallocRaw(db, 64);
  if( db->mallocFailed ){
    pMem->flags = MEM_Null;
  }else{
    assert( pMem->zMalloc );
    pMem->u.pRowSet = sqlite3RowSetInit(db, pMem->zMalloc, 
                                       sqlite3DbMallocSize(db, pMem->zMalloc));
    assert( pMem->u.pRowSet!=0 );
    pMem->flags = MEM_RowSet;
  }
}

/*
** Return true if the Mem object contains a TEXT or BLOB that is
** too large - whose size exceeds SQLITE_MAX_LENGTH.
*/
int sqlite3VdbeMemTooBig(Mem *p){
  assert( p->db!=0 );
  if( p->flags & (MEM_Str|MEM_Blob) ){
    int n = p->n;
    if( p->flags & MEM_Zero ){
      n += p->u.nZero;
    }
    return n>p->db->aLimit[SQLITE_LIMIT_LENGTH];
  }
  return 0; 
}

#ifdef SQLITE_DEBUG
/*
** This routine prepares a memory cell for modication by breaking
** its link to a shallow copy and by marking any current shallow
** copies of this cell as invalid.
**
** This is used for testing and debugging only - to make sure shallow
** copies are not misused.
*/
void sqlite3VdbeMemAboutToChange(Vdbe *pVdbe, Mem *pMem){
  int i;
  Mem *pX;
  for(i=1, pX=&pVdbe->aMem[1]; i<=pVdbe->nMem; i++, pX++){
    if( pX->pScopyFrom==pMem ){
      pX->flags |= MEM_Undefined;
      pX->pScopyFrom = 0;
    }
  }
  pMem->pScopyFrom = 0;
}
#endif /* SQLITE_DEBUG */

/*
** Size of struct Mem not including the Mem.zMalloc member.
*/
#define MEMCELLSIZE offsetof(Mem,zMalloc)

/*
** Make an shallow copy of pFrom into pTo.  Prior contents of
** pTo are freed.  The pFrom->z field is not duplicated.  If
** pFrom->z is used, then pTo->z points to the same thing as pFrom->z
** and flags gets srcType (either MEM_Ephem or MEM_Static).
*/
void sqlite3VdbeMemShallowCopy(Mem *pTo, const Mem *pFrom, int srcType){
  assert( (pFrom->flags & MEM_RowSet)==0 );
  VdbeMemRelease(pTo);
  memcpy(pTo, pFrom, MEMCELLSIZE);
  pTo->xDel = 0;
  if( (pFrom->flags&MEM_Static)==0 ){
    pTo->flags &= ~(MEM_Dyn|MEM_Static|MEM_Ephem);
    assert( srcType==MEM_Ephem || srcType==MEM_Static );
    pTo->flags |= srcType;
  }
}

/*
** Make a full copy of pFrom into pTo.  Prior contents of pTo are
** freed before the copy is made.
*/
int sqlite3VdbeMemCopy(Mem *pTo, const Mem *pFrom){
  int rc = SQLITE_OK;

  assert( (pFrom->flags & MEM_RowSet)==0 );
  VdbeMemRelease(pTo);
  memcpy(pTo, pFrom, MEMCELLSIZE);
  pTo->flags &= ~MEM_Dyn;
  pTo->xDel = 0;

  if( pTo->flags&(MEM_Str|MEM_Blob) ){
    if( 0==(pFrom->flags&MEM_Static) ){
      pTo->flags |= MEM_Ephem;
      rc = sqlite3VdbeMemMakeWriteable(pTo);
    }
  }

  return rc;
}

/*
** Transfer the contents of pFrom to pTo. Any existing value in pTo is
** freed. If pFrom contains ephemeral data, a copy is made.
**
** pFrom contains an SQL NULL when this routine returns.
*/
void sqlite3VdbeMemMove(Mem *pTo, Mem *pFrom){
  assert( pFrom->db==0 || sqlite3_mutex_held(pFrom->db->mutex) );
  assert( pTo->db==0 || sqlite3_mutex_held(pTo->db->mutex) );
  assert( pFrom->db==0 || pTo->db==0 || pFrom->db==pTo->db );

  sqlite3VdbeMemRelease(pTo);
  memcpy(pTo, pFrom, sizeof(Mem));
  pFrom->flags = MEM_Null;
  pFrom->xDel = 0;
  pFrom->zMalloc = 0;
}

/*
** Change the value of a Mem to be a string or a BLOB.
**
** The memory management strategy depends on the value of the xDel
** parameter. If the value passed is SQLITE_TRANSIENT, then the 
** string is copied into a (possibly existing) buffer managed by the 
** Mem structure. Otherwise, any existing buffer is freed and the
** pointer copied.
**
** If the string is too large (if it exceeds the SQLITE_LIMIT_LENGTH
** size limit) then no memory allocation occurs.  If the string can be
** stored without allocating memory, then it is.  If a memory allocation
** is required to store the string, then value of pMem is unchanged.  In
** either case, SQLITE_TOOBIG is returned.
*/
int sqlite3VdbeMemSetStr(
  Mem *pMem,          /* Memory cell to set to string value */
  const char *z,      /* String pointer */
  int n,              /* Bytes in string, or negative */
  u8 enc,             /* Encoding of z.  0 for BLOBs */
  void (*xDel)(void*) /* Destructor function */
){
  int nByte = n;      /* New value for pMem->n */
  int iLimit;         /* Maximum allowed string or blob size */
  u16 flags = 0;      /* New value for pMem->flags */

  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags & MEM_RowSet)==0 );

  /* If z is a NULL pointer, set pMem to contain an SQL NULL. */
  if( !z ){
    sqlite3VdbeMemSetNull(pMem);
    return SQLITE_OK;
  }

  if( pMem->db ){
    iLimit = pMem->db->aLimit[SQLITE_LIMIT_LENGTH];
  }else{
    iLimit = SQLITE_MAX_LENGTH;
  }
  flags = (enc==0?MEM_Blob:MEM_Str);
  if( nByte<0 ){
    assert( enc!=0 );
    if( enc==SQLITE_UTF8 ){
      for(nByte=0; nByte<=iLimit && z[nByte]; nByte++){}
    }else{
      for(nByte=0; nByte<=iLimit && (z[nByte] | z[nByte+1]); nByte+=2){}
    }
    flags |= MEM_Term;
  }

  /* The following block sets the new values of Mem.z and Mem.xDel. It
  ** also sets a flag in local variable "flags" to indicate the memory
  ** management (one of MEM_Dyn or MEM_Static).
  */
  if( xDel==SQLITE_TRANSIENT ){
    int nAlloc = nByte;
    if( flags&MEM_Term ){
      nAlloc += (enc==SQLITE_UTF8?1:2);
    }
    if( nByte>iLimit ){
      return SQLITE_TOOBIG;
    }
    if( sqlite3VdbeMemGrow(pMem, nAlloc, 0) ){
      return SQLITE_NOMEM;
    }
    memcpy(pMem->z, z, nAlloc);
  }else if( xDel==SQLITE_DYNAMIC ){
    sqlite3VdbeMemRelease(pMem);
    pMem->zMalloc = pMem->z = (char *)z;
    pMem->xDel = 0;
  }else{
    sqlite3VdbeMemRelease(pMem);
    pMem->z = (char *)z;
    pMem->xDel = xDel;
    flags |= ((xDel==SQLITE_STATIC)?MEM_Static:MEM_Dyn);
  }

  pMem->n = nByte;
  pMem->flags = flags;
  pMem->enc = (enc==0 ? SQLITE_UTF8 : enc);

#ifndef SQLITE_OMIT_UTF16
  if( pMem->enc!=SQLITE_UTF8 && sqlite3VdbeMemHandleBom(pMem) ){
    return SQLITE_NOMEM;
  }
#endif

  if( nByte>iLimit ){
    return SQLITE_TOOBIG;
  }

  return SQLITE_OK;
}

/*
** Move data out of a btree key or data field and into a Mem structure.
** The data or key is taken from the entry that pCur is currently pointing
** to.  offset and amt determine what portion of the data or key to retrieve.
** key is true to get the key or false to get data.  The result is written
** into the pMem element.
**
** The pMem structure is assumed to be uninitialized.  Any prior content
** is overwritten without being freed.
**
** If this routine fails for any reason (malloc returns NULL or unable
** to read from the disk) then the pMem is left in an inconsistent state.
*/
int sqlite3VdbeMemFromBtree(
  BtCursor *pCur,   /* Cursor pointing at record to retrieve. */
  u32 offset,       /* Offset from the start of data to return bytes from. */
  u32 amt,          /* Number of bytes to return. */
  int key,          /* If true, retrieve from the btree key, not data. */
  Mem *pMem         /* OUT: Return data in this Mem structure. */
){
  char *zData;        /* Data from the btree layer */
  u32 available = 0;  /* Number of bytes available on the local btree page */
  int rc = SQLITE_OK; /* Return code */

  assert( sqlite3BtreeCursorIsValid(pCur) );

  /* Note: the calls to BtreeKeyFetch() and DataFetch() below assert() 
  ** that both the BtShared and database handle mutexes are held. */
  assert( (pMem->flags & MEM_RowSet)==0 );
  if( key ){
    zData = (char *)sqlite3BtreeKeyFetch(pCur, &available);
  }else{
    zData = (char *)sqlite3BtreeDataFetch(pCur, &available);
  }
  assert( zData!=0 );

  if( offset+amt<=available ){
    sqlite3VdbeMemRelease(pMem);
    pMem->z = &zData[offset];
    pMem->flags = MEM_Blob|MEM_Ephem;
    pMem->n = (int)amt;
  }else if( SQLITE_OK==(rc = sqlite3VdbeMemGrow(pMem, amt+2, 0)) ){
    if( key ){
      rc = sqlite3BtreeKey(pCur, offset, amt, pMem->z);
    }else{
      rc = sqlite3BtreeData(pCur, offset, amt, pMem->z);
    }
    if( rc==SQLITE_OK ){
      pMem->z[amt] = 0;
      pMem->z[amt+1] = 0;
      pMem->flags = MEM_Blob|MEM_Term;
      pMem->n = (int)amt;
    }else{
      sqlite3VdbeMemRelease(pMem);
    }
  }

  return rc;
}

/* This function is only available internally, it is not part of the
** external API. It works in a similar way to sqlite3_value_text(),
** except the data returned is in the encoding specified by the second
** parameter, which must be one of SQLITE_UTF16BE, SQLITE_UTF16LE or
** SQLITE_UTF8.
**
** (2006-02-16:)  The enc value can be or-ed with SQLITE_UTF16_ALIGNED.
** If that is the case, then the result must be aligned on an even byte
** boundary.
*/
const void *sqlite3ValueText(sqlite3_value* pVal, u8 enc){
  if( !pVal ) return 0;

  assert( pVal->db==0 || sqlite3_mutex_held(pVal->db->mutex) );
  assert( (enc&3)==(enc&~SQLITE_UTF16_ALIGNED) );
  assert( (pVal->flags & MEM_RowSet)==0 );

  if( pVal->flags&MEM_Null ){
    return 0;
  }
  assert( (MEM_Blob>>3) == MEM_Str );
  pVal->flags |= (pVal->flags & MEM_Blob)>>3;
  ExpandBlob(pVal);
  if( pVal->flags&MEM_Str ){
    sqlite3VdbeChangeEncoding(pVal, enc & ~SQLITE_UTF16_ALIGNED);
    if( (enc & SQLITE_UTF16_ALIGNED)!=0 && 1==(1&SQLITE_PTR_TO_INT(pVal->z)) ){
      assert( (pVal->flags & (MEM_Ephem|MEM_Static))!=0 );
      if( sqlite3VdbeMemMakeWriteable(pVal)!=SQLITE_OK ){
        return 0;
      }
    }
    sqlite3VdbeMemNulTerminate(pVal); /* IMP: R-31275-44060 */
  }else{
    assert( (pVal->flags&MEM_Blob)==0 );
    sqlite3VdbeMemStringify(pVal, enc);
    assert( 0==(1&SQLITE_PTR_TO_INT(pVal->z)) );
  }
  assert(pVal->enc==(enc & ~SQLITE_UTF16_ALIGNED) || pVal->db==0
              || pVal->db->mallocFailed );
  if( pVal->enc==(enc & ~SQLITE_UTF16_ALIGNED) ){
    return pVal->z;
  }else{
    return 0;
  }
}

/*
** Create a new sqlite3_value object.
*/
sqlite3_value *sqlite3ValueNew(sqlite3 *db){
  Mem *p = sqlite3DbMallocZero(db, sizeof(*p));
  if( p ){
    p->flags = MEM_Null;
    p->db = db;
  }
  return p;
}

/*
** Context object passed by sqlite3Stat4ProbeSetValue() through to 
** valueNew(). See comments above valueNew() for details.
*/
struct ValueNewStat4Ctx {
  Parse *pParse;
  Index *pIdx;
  UnpackedRecord **ppRec;
  int iVal;
};

/*
** Allocate and return a pointer to a new sqlite3_value object. If
** the second argument to this function is NULL, the object is allocated
** by calling sqlite3ValueNew().
**
** Otherwise, if the second argument is non-zero, then this function is 
** being called indirectly by sqlite3Stat4ProbeSetValue(). If it has not
** already been allocated, allocate the UnpackedRecord structure that 
** that function will return to its caller here. Then return a pointer 
** an sqlite3_value within the UnpackedRecord.a[] array.
*/
static sqlite3_value *valueNew(sqlite3 *db, struct ValueNewStat4Ctx *p){
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  if( p ){
    UnpackedRecord *pRec = p->ppRec[0];

    if( pRec==0 ){
      Index *pIdx = p->pIdx;      /* Index being probed */
      int nByte;                  /* Bytes of space to allocate */
      int i;                      /* Counter variable */
      int nCol = pIdx->nColumn;   /* Number of index columns including rowid */
  
      nByte = sizeof(Mem) * nCol + ROUND8(sizeof(UnpackedRecord));
      pRec = (UnpackedRecord*)sqlite3DbMallocZero(db, nByte);
      if( pRec ){
        pRec->pKeyInfo = sqlite3KeyInfoOfIndex(p->pParse, pIdx);
        if( pRec->pKeyInfo ){
          assert( pRec->pKeyInfo->nField+pRec->pKeyInfo->nXField==nCol );
          assert( pRec->pKeyInfo->enc==ENC(db) );
          pRec->aMem = (Mem *)((u8*)pRec + ROUND8(sizeof(UnpackedRecord)));
          for(i=0; i<nCol; i++){
            pRec->aMem[i].flags = MEM_Null;
            pRec->aMem[i].db = db;
          }
        }else{
          sqlite3DbFree(db, pRec);
          pRec = 0;
        }
      }
      if( pRec==0 ) return 0;
      p->ppRec[0] = pRec;
    }
  
    pRec->nField = p->iVal+1;
    return &pRec->aMem[p->iVal];
  }
#else
  UNUSED_PARAMETER(p);
#endif /* defined(SQLITE_ENABLE_STAT3_OR_STAT4) */
  return sqlite3ValueNew(db);
}

/*
** Extract a value from the supplied expression in the manner described
** above sqlite3ValueFromExpr(). Allocate the sqlite3_value object
** using valueNew().
**
** If pCtx is NULL and an error occurs after the sqlite3_value object
** has been allocated, it is freed before returning. Or, if pCtx is not
** NULL, it is assumed that the caller will free any allocated object
** in all cases.
*/
static int valueFromExpr(
  sqlite3 *db,                    /* The database connection */
  Expr *pExpr,                    /* The expression to evaluate */
  u8 enc,                         /* Encoding to use */
  u8 affinity,                    /* Affinity to use */
  sqlite3_value **ppVal,          /* Write the new value here */
  struct ValueNewStat4Ctx *pCtx   /* Second argument for valueNew() */
){
  int op;
  char *zVal = 0;
  sqlite3_value *pVal = 0;
  int negInt = 1;
  const char *zNeg = "";
  int rc = SQLITE_OK;

  if( !pExpr ){
    *ppVal = 0;
    return SQLITE_OK;
  }
  op = pExpr->op;
  if( NEVER(op==TK_REGISTER) ) op = pExpr->op2;

  /* Handle negative integers in a single step.  This is needed in the
  ** case when the value is -9223372036854775808.
  */
  if( op==TK_UMINUS
   && (pExpr->pLeft->op==TK_INTEGER || pExpr->pLeft->op==TK_FLOAT) ){
    pExpr = pExpr->pLeft;
    op = pExpr->op;
    negInt = -1;
    zNeg = "-";
  }

  if( op==TK_STRING || op==TK_FLOAT || op==TK_INTEGER ){
    pVal = valueNew(db, pCtx);
    if( pVal==0 ) goto no_mem;
    if( ExprHasProperty(pExpr, EP_IntValue) ){
      sqlite3VdbeMemSetInt64(pVal, (i64)pExpr->u.iValue*negInt);
    }else{
      zVal = sqlite3MPrintf(db, "%s%s", zNeg, pExpr->u.zToken);
      if( zVal==0 ) goto no_mem;
      sqlite3ValueSetStr(pVal, -1, zVal, SQLITE_UTF8, SQLITE_DYNAMIC);
    }
    if( (op==TK_INTEGER || op==TK_FLOAT ) && affinity==SQLITE_AFF_NONE ){
      sqlite3ValueApplyAffinity(pVal, SQLITE_AFF_NUMERIC, SQLITE_UTF8);
    }else{
      sqlite3ValueApplyAffinity(pVal, affinity, SQLITE_UTF8);
    }
    if( pVal->flags & (MEM_Int|MEM_Real) ) pVal->flags &= ~MEM_Str;
    if( enc!=SQLITE_UTF8 ){
      rc = sqlite3VdbeChangeEncoding(pVal, enc);
    }
  }else if( op==TK_UMINUS ) {
    /* This branch happens for multiple negative signs.  Ex: -(-5) */
    if( SQLITE_OK==sqlite3ValueFromExpr(db,pExpr->pLeft,enc,affinity,&pVal) 
     && pVal!=0
    ){
      sqlite3VdbeMemNumerify(pVal);
      if( pVal->u.i==SMALLEST_INT64 ){
        pVal->flags &= ~MEM_Int;
        pVal->flags |= MEM_Real;
        pVal->r = (double)SMALLEST_INT64;
      }else{
        pVal->u.i = -pVal->u.i;
      }
      pVal->r = -pVal->r;
      sqlite3ValueApplyAffinity(pVal, affinity, enc);
    }
  }else if( op==TK_NULL ){
    pVal = valueNew(db, pCtx);
    if( pVal==0 ) goto no_mem;
  }
#ifndef SQLITE_OMIT_BLOB_LITERAL
  else if( op==TK_BLOB ){
    int nVal;
    assert( pExpr->u.zToken[0]=='x' || pExpr->u.zToken[0]=='X' );
    assert( pExpr->u.zToken[1]=='\'' );
    pVal = valueNew(db, pCtx);
    if( !pVal ) goto no_mem;
    zVal = &pExpr->u.zToken[2];
    nVal = sqlite3Strlen30(zVal)-1;
    assert( zVal[nVal]=='\'' );
    sqlite3VdbeMemSetStr(pVal, sqlite3HexToBlob(db, zVal, nVal), nVal/2,
                         0, SQLITE_DYNAMIC);
  }
#endif

  *ppVal = pVal;
  return rc;

no_mem:
  db->mallocFailed = 1;
  sqlite3DbFree(db, zVal);
  assert( *ppVal==0 );
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  if( pCtx==0 ) sqlite3ValueFree(pVal);
#else
  assert( pCtx==0 ); sqlite3ValueFree(pVal);
#endif
  return SQLITE_NOMEM;
}

/*
** Create a new sqlite3_value object, containing the value of pExpr.
**
** This only works for very simple expressions that consist of one constant
** token (i.e. "5", "5.1", "'a string'"). If the expression can
** be converted directly into a value, then the value is allocated and
** a pointer written to *ppVal. The caller is responsible for deallocating
** the value by passing it to sqlite3ValueFree() later on. If the expression
** cannot be converted to a value, then *ppVal is set to NULL.
*/
int sqlite3ValueFromExpr(
  sqlite3 *db,              /* The database connection */
  Expr *pExpr,              /* The expression to evaluate */
  u8 enc,                   /* Encoding to use */
  u8 affinity,              /* Affinity to use */
  sqlite3_value **ppVal     /* Write the new value here */
){
  return valueFromExpr(db, pExpr, enc, affinity, ppVal, 0);
}

#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
/*
** The implementation of the sqlite_record() function. This function accepts
** a single argument of any type. The return value is a formatted database 
** record (a blob) containing the argument value.
**
** This is used to convert the value stored in the 'sample' column of the
** sqlite_stat3 table to the record format SQLite uses internally.
*/
static void recordFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  const int file_format = 1;
  int iSerial;                    /* Serial type */
  int nSerial;                    /* Bytes of space for iSerial as varint */
  int nVal;                       /* Bytes of space required for argv[0] */
  int nRet;
  sqlite3 *db;
  u8 *aRet;

  UNUSED_PARAMETER( argc );
  iSerial = sqlite3VdbeSerialType(argv[0], file_format);
  nSerial = sqlite3VarintLen(iSerial);
  nVal = sqlite3VdbeSerialTypeLen(iSerial);
  db = sqlite3_context_db_handle(context);

  nRet = 1 + nSerial + nVal;
  aRet = sqlite3DbMallocRaw(db, nRet);
  if( aRet==0 ){
    sqlite3_result_error_nomem(context);
  }else{
    aRet[0] = nSerial+1;
    sqlite3PutVarint(&aRet[1], iSerial);
    sqlite3VdbeSerialPut(&aRet[1+nSerial], argv[0], iSerial);
    sqlite3_result_blob(context, aRet, nRet, SQLITE_TRANSIENT);
    sqlite3DbFree(db, aRet);
  }
}

/*
** Register built-in functions used to help read ANALYZE data.
*/
void sqlite3AnalyzeFunctions(void){
  static SQLITE_WSD FuncDef aAnalyzeTableFuncs[] = {
    FUNCTION(sqlite_record,   1, 0, 0, recordFunc),
  };
  int i;
  FuncDefHash *pHash = &GLOBAL(FuncDefHash, sqlite3GlobalFunctions);
  FuncDef *aFunc = (FuncDef*)&GLOBAL(FuncDef, aAnalyzeTableFuncs);
  for(i=0; i<ArraySize(aAnalyzeTableFuncs); i++){
    sqlite3FuncDefInsert(pHash, &aFunc[i]);
  }
}

/*
** Attempt to extract a value from pExpr and use it to construct *ppVal.
**
** If pAlloc is not NULL, then an UnpackedRecord object is created for
** pAlloc if one does not exist and the new value is added to the
** UnpackedRecord object.
**
** A value is extracted in the following cases:
**
**  * (pExpr==0). In this case the value is assumed to be an SQL NULL,
**
**  * The expression is a bound variable, and this is a reprepare, or
**
**  * The expression is a literal value.
**
** On success, *ppVal is made to point to the extracted value.  The caller
** is responsible for ensuring that the value is eventually freed.
*/
static int stat4ValueFromExpr(
  Parse *pParse,                  /* Parse context */
  Expr *pExpr,                    /* The expression to extract a value from */
  u8 affinity,                    /* Affinity to use */
  struct ValueNewStat4Ctx *pAlloc,/* How to allocate space.  Or NULL */
  sqlite3_value **ppVal           /* OUT: New value object (or NULL) */
){
  int rc = SQLITE_OK;
  sqlite3_value *pVal = 0;
  sqlite3 *db = pParse->db;

  /* Skip over any TK_COLLATE nodes */
  pExpr = sqlite3ExprSkipCollate(pExpr);

  if( !pExpr ){
    pVal = valueNew(db, pAlloc);
    if( pVal ){
      sqlite3VdbeMemSetNull((Mem*)pVal);
    }
  }else if( pExpr->op==TK_VARIABLE
        || NEVER(pExpr->op==TK_REGISTER && pExpr->op2==TK_VARIABLE)
  ){
    Vdbe *v;
    int iBindVar = pExpr->iColumn;
    sqlite3VdbeSetVarmask(pParse->pVdbe, iBindVar);
    if( (v = pParse->pReprepare)!=0 ){
      pVal = valueNew(db, pAlloc);
      if( pVal ){
        rc = sqlite3VdbeMemCopy((Mem*)pVal, &v->aVar[iBindVar-1]);
        if( rc==SQLITE_OK ){
          sqlite3ValueApplyAffinity(pVal, affinity, ENC(db));
        }
        pVal->db = pParse->db;
      }
    }
  }else{
    rc = valueFromExpr(db, pExpr, ENC(db), affinity, &pVal, pAlloc);
  }

  assert( pVal==0 || pVal->db==db );
  *ppVal = pVal;
  return rc;
}

/*
** This function is used to allocate and populate UnpackedRecord 
** structures intended to be compared against sample index keys stored 
** in the sqlite_stat4 table.
**
** A single call to this function attempts to populates field iVal (leftmost 
** is 0 etc.) of the unpacked record with a value extracted from expression
** pExpr. Extraction of values is possible if:
**
**  * (pExpr==0). In this case the value is assumed to be an SQL NULL,
**
**  * The expression is a bound variable, and this is a reprepare, or
**
**  * The sqlite3ValueFromExpr() function is able to extract a value 
**    from the expression (i.e. the expression is a literal value).
**
** If a value can be extracted, the affinity passed as the 5th argument
** is applied to it before it is copied into the UnpackedRecord. Output
** parameter *pbOk is set to true if a value is extracted, or false 
** otherwise.
**
** When this function is called, *ppRec must either point to an object
** allocated by an earlier call to this function, or must be NULL. If it
** is NULL and a value can be successfully extracted, a new UnpackedRecord
** is allocated (and *ppRec set to point to it) before returning.
**
** Unless an error is encountered, SQLITE_OK is returned. It is not an
** error if a value cannot be extracted from pExpr. If an error does
** occur, an SQLite error code is returned.
*/
int sqlite3Stat4ProbeSetValue(
  Parse *pParse,                  /* Parse context */
  Index *pIdx,                    /* Index being probed */
  UnpackedRecord **ppRec,         /* IN/OUT: Probe record */
  Expr *pExpr,                    /* The expression to extract a value from */
  u8 affinity,                    /* Affinity to use */
  int iVal,                       /* Array element to populate */
  int *pbOk                       /* OUT: True if value was extracted */
){
  int rc;
  sqlite3_value *pVal = 0;
  struct ValueNewStat4Ctx alloc;

  alloc.pParse = pParse;
  alloc.pIdx = pIdx;
  alloc.ppRec = ppRec;
  alloc.iVal = iVal;

  rc = stat4ValueFromExpr(pParse, pExpr, affinity, &alloc, &pVal);
  assert( pVal==0 || pVal->db==pParse->db );
  *pbOk = (pVal!=0);
  return rc;
}

/*
** Attempt to extract a value from expression pExpr using the methods
** as described for sqlite3Stat4ProbeSetValue() above. 
**
** If successful, set *ppVal to point to a new value object and return 
** SQLITE_OK. If no value can be extracted, but no other error occurs
** (e.g. OOM), return SQLITE_OK and set *ppVal to NULL. Or, if an error
** does occur, return an SQLite error code. The final value of *ppVal
** is undefined in this case.
*/
int sqlite3Stat4ValueFromExpr(
  Parse *pParse,                  /* Parse context */
  Expr *pExpr,                    /* The expression to extract a value from */
  u8 affinity,                    /* Affinity to use */
  sqlite3_value **ppVal           /* OUT: New value object (or NULL) */
){
  return stat4ValueFromExpr(pParse, pExpr, affinity, 0, ppVal);
}

/*
** Extract the iCol-th column from the nRec-byte record in pRec.  Write
** the column value into *ppVal.  If *ppVal is initially NULL then a new
** sqlite3_value object is allocated.
**
** If *ppVal is initially NULL then the caller is responsible for 
** ensuring that the value written into *ppVal is eventually freed.
*/
int sqlite3Stat4Column(
  sqlite3 *db,                    /* Database handle */
  const void *pRec,               /* Pointer to buffer containing record */
  int nRec,                       /* Size of buffer pRec in bytes */
  int iCol,                       /* Column to extract */
  sqlite3_value **ppVal           /* OUT: Extracted value */
){
  u32 t;                          /* a column type code */
  int nHdr;                       /* Size of the header in the record */
  int iHdr;                       /* Next unread header byte */
  int iField;                     /* Next unread data byte */
  int szField;                    /* Size of the current data field */
  int i;                          /* Column index */
  u8 *a = (u8*)pRec;              /* Typecast byte array */
  Mem *pMem = *ppVal;             /* Write result into this Mem object */

  assert( iCol>0 );
  iHdr = getVarint32(a, nHdr);
  if( nHdr>nRec || iHdr>=nHdr ) return SQLITE_CORRUPT_BKPT;
  iField = nHdr;
  for(i=0; i<=iCol; i++){
    iHdr += getVarint32(&a[iHdr], t);
    testcase( iHdr==nHdr );
    testcase( iHdr==nHdr+1 );
    if( iHdr>nHdr ) return SQLITE_CORRUPT_BKPT;
    szField = sqlite3VdbeSerialTypeLen(t);
    iField += szField;
  }
  testcase( iField==nRec );
  testcase( iField==nRec+1 );
  if( iField>nRec ) return SQLITE_CORRUPT_BKPT;
  if( pMem==0 ){
    pMem = *ppVal = sqlite3ValueNew(db);
    if( pMem==0 ) return SQLITE_NOMEM;
  }
  sqlite3VdbeSerialGet(&a[iField-szField], t, pMem);
  pMem->enc = ENC(db);
  return SQLITE_OK;
}

/*
** Unless it is NULL, the argument must be an UnpackedRecord object returned
** by an earlier call to sqlite3Stat4ProbeSetValue(). This call deletes
** the object.
*/
void sqlite3Stat4ProbeFree(UnpackedRecord *pRec){
  if( pRec ){
    int i;
    int nCol = pRec->pKeyInfo->nField+pRec->pKeyInfo->nXField;
    Mem *aMem = pRec->aMem;
    sqlite3 *db = aMem[0].db;
    for(i=0; i<nCol; i++){
      sqlite3DbFree(db, aMem[i].zMalloc);
    }
    sqlite3KeyInfoUnref(pRec->pKeyInfo);
    sqlite3DbFree(db, pRec);
  }
}
#endif /* ifdef SQLITE_ENABLE_STAT4 */

/*
** Change the string value of an sqlite3_value object
*/
void sqlite3ValueSetStr(
  sqlite3_value *v,     /* Value to be set */
  int n,                /* Length of string z */
  const void *z,        /* Text of the new string */
  u8 enc,               /* Encoding to use */
  void (*xDel)(void*)   /* Destructor for the string */
){
  if( v ) sqlite3VdbeMemSetStr((Mem *)v, z, n, enc, xDel);
}

/*
** Free an sqlite3_value object
*/
void sqlite3ValueFree(sqlite3_value *v){
  if( !v ) return;
  sqlite3VdbeMemRelease((Mem *)v);
  sqlite3DbFree(((Mem*)v)->db, v);
}

/*
** Return the number of bytes in the sqlite3_value object assuming
** that it uses the encoding "enc"
*/
int sqlite3ValueBytes(sqlite3_value *pVal, u8 enc){
  Mem *p = (Mem*)pVal;
  if( (p->flags & MEM_Blob)!=0 || sqlite3ValueText(pVal, enc) ){
    if( p->flags & MEM_Zero ){
      return p->n + p->u.nZero;
    }else{
      return p->n;
    }
  }
  return 0;
}
