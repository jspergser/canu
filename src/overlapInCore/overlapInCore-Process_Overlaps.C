
/******************************************************************************
 *
 *  This file is part of canu, a software program that assembles whole-genome
 *  sequencing reads into contigs.
 *
 *  This software is based on:
 *    'Celera Assembler' (http://wgs-assembler.sourceforge.net)
 *    the 'kmer package' (http://kmer.sourceforge.net)
 *  both originally distributed by Applera Corporation under the GNU General
 *  Public License, version 2.
 *
 *  Canu branched from Celera Assembler at its revision 4587.
 *  Canu branched from the kmer project at its revision 1994.
 *
 *  This file is derived from:
 *
 *    src/AS_OVL/AS_OVL_overlap_common.h
 *    src/AS_OVM/overlapInCore-Process_Overlaps.C
 *
 *  Modifications by:
 *
 *    Michael Schatz on 2004-SEP-23
 *      are Copyright 2004 The Institute for Genomics Research, and
 *      are subject to the GNU General Public License version 2
 *
 *    Jason Miller on 2005-MAR-22
 *      are Copyright 2005 The Institute for Genomics Research, and
 *      are subject to the GNU General Public License version 2
 *
 *    Brian P. Walenz from 2005-JUN-16 to 2013-AUG-01
 *      are Copyright 2005-2011,2013 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Eli Venter from 2005-JUL-15 to 2007-NOV-20
 *      are Copyright 2005,2007 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Aaron Halpern from 2006-MAR-27 to 2006-AUG-21
 *      are Copyright 2006 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Art Delcher on 2007-FEB-13
 *      are Copyright 2007 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Sergey Koren from 2007-AUG-27 to 2009-JAN-16
 *      are Copyright 2007,2009 J. Craig Venter Institute, and
 *      are subject to the GNU General Public License version 2
 *
 *    Sergey Koren from 2011-MAR-08 to 2015-AUG-21
 *      are Copyright 2011,2015 Battelle National Biodefense Institute, and
 *      are subject to the BSD 3-Clause License
 *
 *    Brian P. Walenz from 2014-DEC-15 to 2015-AUG-25
 *      are Copyright 2014-2015 Battelle National Biodefense Institute, and
 *      are subject to the BSD 3-Clause License
 *
 *    Brian P. Walenz beginning on 2015-NOV-23
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *    Sergey Koren beginning on 2016-JUN-08
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "overlapInCore.H"
#include "sequence.H"

//  Find and output all overlaps between strings in store and those in the global hash table.
//  This is the entry point for each compute thread.

void *
Process_Overlaps(void *ptr){
  Work_Area_t  *WA = (Work_Area_t *)ptr;

  uint32        seqptrLen = 0;
  uint32        seqptrMax = AS_MAX_READLEN + 1;
  char         *seqptr    = new char [seqptrMax];
  char         *bases     = new char [AS_MAX_READLEN + 1];

  while (WA->bgnID < G.endRefID) {
    WA->overlapsLen                = 0;

    WA->Total_Overlaps             = 0;
    WA->Contained_Overlap_Ct       = 0;
    WA->Dovetail_Overlap_Ct        = 0;

    WA->Kmer_Hits_Without_Olap_Ct  = 0;
    WA->Kmer_Hits_With_Olap_Ct     = 0;
    WA->Kmer_Hits_Skipped_Ct       = 0;
    WA->Multi_Overlap_Ct           = 0;

    fprintf(stderr, "Thread %02u processes reads " F_U32 "-" F_U32 "\n",
            WA->thread_id, WA->bgnID, WA->endID);

    for (uint32 fi=WA->bgnID; fi<=WA->endID; fi++) {
      uint32  libID   = WA->readStore->sqStore_getLibraryIDForRead(fi);
      uint32  readLen = WA->readCache->sqCache_getLength(fi);

      //  Load sequence/quality data
      //  Duplicated in Build_Hash_Index()

      if ((libID < G.minLibToRef) ||
          (libID > G.maxLibToRef))
        continue;

      if (readLen < G.Min_Olap_Len)
        continue;

      WA->readCache->sqCache_getSequence(fi, seqptr, seqptrLen, seqptrMax);

      for (uint32 i=0; i<readLen; i++)
        bases[i] = tolower(seqptr[i]);

      bases[readLen] = 0;

      assert(strlen(bases) == readLen);

      //  Generate overlaps.

      Find_Overlaps(bases, readLen, fi, FORWARD, WA);

      reverseComplementSequence(bases, readLen);

      Find_Overlaps(bases, readLen, fi, REVERSE, WA);
    }

    //  Write out this block of overlaps, no need to keep them in core!
    //  While we have a mutex, also find the next block of things to process.

    fprintf(stderr, "Thread %02u writes    reads " F_U32 "-" F_U32 " (" F_U64 " overlaps " F_U64 "/" F_U64 "/" F_U64 " kmer hits with/without overlap/skipped)\n",
            WA->thread_id, WA->bgnID, WA->endID,
            WA->overlapsLen,
            WA->Kmer_Hits_With_Olap_Ct, WA->Kmer_Hits_Without_Olap_Ct, WA->Kmer_Hits_Skipped_Ct);

    //  Flush any remaining overlaps and update statistics.

#pragma omp critical
    {
      for (int zz=0; zz<WA->overlapsLen; zz++)
        Out_BOF->writeOverlap(WA->overlaps + zz);

      WA->overlapsLen = 0;

      Total_Overlaps            += WA->Total_Overlaps;
      Contained_Overlap_Ct      += WA->Contained_Overlap_Ct;
      Dovetail_Overlap_Ct       += WA->Dovetail_Overlap_Ct;

      Kmer_Hits_Without_Olap_Ct += WA->Kmer_Hits_Without_Olap_Ct;
      Kmer_Hits_With_Olap_Ct    += WA->Kmer_Hits_With_Olap_Ct;
      Kmer_Hits_Skipped_Ct      += WA->Kmer_Hits_Skipped_Ct;
      Multi_Overlap_Ct          += WA->Multi_Overlap_Ct;

      WA->bgnID = G.curRefID;
      WA->endID = G.curRefID + G.perThread - 1;

      if (WA->endID > G.endRefID)
        WA->endID = G.endRefID;

      G.curRefID = WA->endID + 1;
    }
  }

  delete [] bases;
  delete [] seqptr;

  return(ptr);
}


