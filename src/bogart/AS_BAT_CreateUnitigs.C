
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
 *  Modifications by:
 *
 *    Brian P. Walenz beginning on 2016-OCT-03
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "AS_BAT_ReadInfo.H"
#include "AS_BAT_OverlapCache.H"
#include "AS_BAT_BestOverlapGraph.H"
#include "AS_BAT_AssemblyGraph.H"
#include "AS_BAT_Logging.H"

#include "AS_BAT_Unitig.H"
#include "AS_BAT_TigVector.H"

#include "AS_BAT_PlaceReadUsingOverlaps.H"

#include "AS_BAT_CreateUnitigs.H"



//  Break on at a specific position.  In converting to unitigs, the position
//  is the end of a read with an intersection.
//
//  _bgn == true  -> reads that begin at after position are in the region
//  _end == false -> reads that end before position are in the region

class breakPointEnd {
public:
  breakPointEnd(uint32 tigID, uint32 pos, bool bgn) {
    _tigID    = tigID;
    _pos      = pos;
    _bgn      = bgn;
  };
  ~breakPointEnd() {
  };

  bool     operator<(breakPointEnd const &that) const {
    uint64  a =      _tigID;  a <<= 32;  a |=      _pos;  a <<= 1;  a |=      _bgn;  //  Because _tigID is 32-bit
    uint64  b = that._tigID;  b <<= 32;  b |= that._pos;  b <<= 1;  b |= that._bgn;

    return(a < b);
  };

  bool     operator==(breakPointEnd const &that) const {
    uint64  a =      _tigID;  a <<= 32;  a |=      _pos;  a <<= 1;  a |=      _bgn;  //  Because _tigID is 32-bit
    uint64  b = that._tigID;  b <<= 32;  b |= that._pos;  b <<= 1;  b |= that._bgn;

    return(a == b);
  };

  uint32  _tigID;
  uint32  _pos;
  bool    _bgn;
};







Unitig *
copyTig(TigVector    &tigs,
        Unitig       *oldtig) {
  Unitig  *newtig = tigs.newUnitig(false);

  newtig->_isUnassembled = oldtig->_isUnassembled;
  newtig->_isRepeat      = oldtig->_isRepeat;

  for (uint32 fi=0; fi<oldtig->ufpath.size(); fi++)
    newtig->addRead(oldtig->ufpath[fi], 0, false);

  return(newtig);
}





//  Split a tig based on read ends.

uint32
splitTig(TigVector                &tigs,
         Unitig                   *tig,
         vector<breakPointEnd>    &BP,
         Unitig                  **newTigs,
         int32                    *lowCoord,
         bool                      doMove) {

  writeLog("\n");
  writeLog("splitTig()-- processing tig %u\n", tig->id());

  //  The first call is with doMove = false.  This call just figures out how many new tigs are
  //  created.  We use nMoved to count if a new tig is made for a break point.

  uint32  *nMoved = NULL;

  if (doMove == false)
    allocateArray(nMoved, BP.size() + 2);

  //  The second call is with doMove = true.  This does the actual moving.

  if (doMove == true)
    for (uint32 tt=0; tt < BP.size() + 2; tt++) {
      newTigs[tt]  = NULL;
      lowCoord[tt] = INT32_MAX;
    }

  if (doMove == true)
    for (uint32 tt=0; tt < BP.size() - 1; tt++)
      writeLog("splitTig()-- piece %2u from %8u %c to %8u %c\n",
               tt,
               BP[tt  ]._pos, BP[tt  ]._bgn ? 't' : 'f',
               BP[tt+1]._pos, BP[tt+1]._bgn ? 't' : 'f');


  for (uint32 fi=0; fi<tig->ufpath.size(); fi++) {
    ufNode     &read   = tig->ufpath[fi];
    uint32      lo     = read.position.min();
    uint32      hi     = read.position.max();

    //writeLog("splitTig()-- processing read #%u ident %u pos %u-%u\n", fi, read.ident, lo, hi);

    //  Find the intervals the end points of the read fall into.  Suppose we're trying to place
    //  the long read.  It begins in piece 1 and ends in piece 6.
    //
    //
    //   [----1---][----3----]---4---[--5---]------6-----]   Piece and boundary condition
    //   ------
    //      --------------------------------------
    //        -----
    //             ------
    //                  ------
    //                               ----
    //                                  -----
    //                                          ----------
    //
    //  The long read can not go in piece 1, as it would span the end boundary.  Piece 2 is
    //  of size zero between pieces 1 and 3, and we can place the read there.  Or, we can place
    //  it in piece 6 (we prefer piece 6).

    uint32 bgnBP = UINT32_MAX;
    uint32 endBP = UINT32_MAX;
    uint32 finBP = UINT32_MAX;

    //  Find the pieces the end points are in.

    for (uint32 tt=0; tt < BP.size()-1; tt++) {
      uint32  p = BP[tt  ]._pos;   bool  pb = BP[tt  ]._bgn;
      uint32  n = BP[tt+1]._pos;   bool  nb = BP[tt+1]._bgn;

      if ((p <= lo) && (lo < n))      //  If bgn == true  -- p == lo is in this region
        bgnBP = tt;

      if ((p < hi) && (hi <= n))      //  If bgn == false -- hi == n is in this region
        endBP = tt;
    }

    //  If both pieces are the same, we're done.

    if (bgnBP == endBP) {
      finBP = bgnBP;
    }

    //  If the next BP is a bgn boundary, we can still place the read in this piece.  It'll extend
    //  off the end, but we don't care.

    else if (BP[bgnBP+1]._bgn == true) {
      finBP = bgnBP;
    }

    //  If not, the next boundary is an end point, and we cannot place the read in this piece.
    //  If the endBP piece doesn't have restrictions on the begin, we can place the read there.

    else if (BP[endBP]._bgn == false) {
      finBP = endBP;
    }

    //  Well, shucks.  No place to put the read.  Search for an unbounded region between bgnBP and
    //  endBP.  There must be one, because bgnBP ends with a bgn=false boundary, and endBP begins
    //  with a bgn=true boundary.  If there are no intermediate boundaries, we can place the read in
    //  the middle.  If there are intermediate boundaries, we'll still have some piece that is
    //  unbounded.

    else {
      for (finBP=bgnBP+1; finBP < endBP; finBP++) {
        if ((BP[finBP  ]._bgn == false) &&
            (BP[finBP+1]._bgn == true))
          break;
      }

      if (finBP == endBP)
        writeLog("splitTig()-- failed to place read %u %u-%u in a region.  found bgn=%u and end=%u\n",
                 read.ident, read.position.bgn, read.position.end, bgnBP, endBP);
      assert(finBP != endBP);
    }

    //  Make a new tig, if needed

    if ((doMove == true) && (newTigs[finBP] == NULL)) {
      writeLog("splitTig()-- new tig %u (id=%u) at read %u %u-%u\n", tigs.size(), finBP, read.ident, read.position.min(), read.position.max());
      lowCoord[finBP] = read.position.min();
      newTigs[finBP]  = tigs.newUnitig(false);
    }

    //  Now move the read, or account for moving it.

    if (doMove) {
      writeLog("splitTig()-- Move read %8u %8u-%-8u to piece %2u tig %6u\n",
               read.ident, read.position.bgn, read.position.end, finBP, newTigs[finBP]->id());
      newTigs[finBP]->addRead(read, -lowCoord[finBP], false);
    }
    else {
      //writeLog("splitTig()-- Move read %u %u-%u to piece %u (pos=%u)\n",
      //         read.ident, read.position.bgn, read.position.end, finBP, BP[finBP]._pos);
      nMoved[finBP]++;
    }
  }

  //  Return the number of tigs created.

  uint32  nTigsCreated = 0;

  if (doMove == false) {
    for (uint32 ii=0; ii<BP.size(); ii++)
      if (nMoved[ii] > 0)
        nTigsCreated++;

    delete [] nMoved;
  }

  return(nTigsCreated);
}



static
uint32
checkReadContained(overlapPlacement &op,
                   Unitig           *tgB) {

  for (uint32 ii=op.tigFidx; ii<=op.tigLidx; ii++) {
    if (isContained(op.verified, tgB->ufpath[ii].position))
      return(ii + 1);
  }

  return(0);
}



//  Decide which read, and which end, we're overlapping.  We know:
//
//    verified tells us the positions covered with overlaps and the orietation of the aligned read
//
//    isFirst and rdAfwd tell if the invading tig is flopping free to the left
//    or right of this location
//
//                                    break here
//                                    v
//    invaded tig     ----------------------------------------------
//                        ------------>
//                           ------->
//                             <------------------    (ignore these two container reads)
//                               <------------        (in reality, this wouldn't be split)
//                                 |  |
//                               (overlap)            (verified.isForward() == false)
//                                 |  |
//                                 <--------
//                                     -----------
//                                          -------------->
//
//  isLow is true if this coordinate is the start of the read placement
//
void
findEnd(overlapPlacement &op,
        bool              rdAfwd,
        bool              isFirst,
        bool             &isLow,
        int32            &coord) {

  if (((isFirst == true)  && (rdAfwd == true)  && (op.verified.isForward() == true))  ||
      ((isFirst == true)  && (rdAfwd == false) && (op.verified.isForward() == false)) ||
      ((isFirst == false) && (rdAfwd == false) && (op.verified.isForward() == true))  ||  //  rdAfwd is opposite what reality is,
      ((isFirst == false) && (rdAfwd == true)  && (op.verified.isForward() == false))) {  //  because we've flipped the tig outside here
    isLow = false;
    coord = INT32_MIN;
  } else {
    isLow = true;
    coord = INT32_MAX;
  }
}




static
uint32
checkRead(Unitig                    *tgA,
          ufNode                    *rdA,
          vector<overlapPlacement>  &rdAplacements,
          TigVector                 &contigs,
          vector<breakPointEnd>     &breakpoints,
          uint32                     minOverlap,
          uint32                     maxPlacements,
          bool                       isFirst) {
  bool   beVerbose = true;

  //  To support maxPlacements, we first find all the breaks as we've done forever, then simply
  //  ignore them if there are too many.

  vector<breakPointEnd>   breaks;

  for (uint32 pp=0; pp<rdAplacements.size(); pp++) {
    overlapPlacement  &op = rdAplacements[pp];
    Unitig            *tgB = contigs[op.tigID];

    bool    toUnassembled = false;
    bool    toSelf        = false;
    bool    expected5     = false;
    bool    expected3     = false;
    bool    tooSmall      = false;
    bool    isContained   = false;  //  Never used?
    bool    noOverlaps    = false;
    bool    notSimilar    = false;

    //  Silently ignore stuff to unassembled tigs.

    if (tgB->_isUnassembled == true) {
      toUnassembled = true;
      continue;
    }

    //  If we're overlapping with ourself, not a useful edge to be splitting on.

    if ((tgA->id() == tgB->id()) && (isOverlapping(op.verified, rdA->position))) {
      toSelf = true;
      if (beVerbose == false)
        continue;
    }

    //  If the overlap is on the end that is used in the tig, not a useful edge.
    //
    //           first == true        (tig)     first == false  (tig)
    //  is5      fwd   == true  ---------->     fwd   == false  <---------
    //  is3      fwd   == false <----------     fwd   == true   --------->

    bool   is5 = (isFirst == rdA->position.isForward()) ? true : false;

    if ((is5 == true) && (op.covered.bgn != 0)) {
      expected5 = true;
      if (beVerbose == false)
        continue;
    }

    if ((is5 == false) && (op.covered.end != RI->readLength(rdA->ident))) {
      expected3 = true;
      if (beVerbose == false)
        continue;
    }

    //  If too small, bail.

    if (op.verified.max() - op.verified.min() < minOverlap) {
      tooSmall = true;
      if (beVerbose == false)
        continue;
    }

    //  Sacn all the reads we supposedly overlap, checking for overlaps.  Save the one that is the
    //  lowest (is5 == true) or highest (is5 == false).  Also, compute an average erate for the
    //  overlaps to this read.

    uint32       ovlLen = 0;
    BAToverlap  *ovl    = OC->getOverlaps(rdA->ident, ovlLen);

    double       erate  = 0.0;
    uint32       erateN = 0;

    bool         isLow  = false;
    int32        coord  = 0;
    ufNode      *rdB    = NULL;

    //  DEBUG: If not to self, try to find the overlap.  Otherwise, this just adds useless clutter,
    //  the self edge is disqualifying enough.

    if (toSelf == false) {
      findEnd(op, rdA->position.isForward(), isFirst, isLow, coord);  //  Simple code, but lots of comments.

      writeLog("\n");
      writeLog("Scan reads from #%u to #%u for %s coordinate in verified region %u-%u\n",
               op.tigFidx, op.tigLidx,
               (isLow) ? "low" : "high",
               op.verified.min(), op.verified.max());

      for (uint32 ii=op.tigFidx; ii<=op.tigLidx; ii++) {
        for (uint32 oo=0; oo<ovlLen; oo++) {
          ufNode  *rdBii = &tgB->ufpath[ii];

          if (ovl[oo].b_iid != rdBii->ident)
            continue;

          writeLog("Test read #%6u ident %7u %9u-%9u against verified region %9u-%9u",
                   ii,
                   rdBii->ident, rdBii->position.min(), rdBii->position.max(),
                   op.verified.min(), op.verified.max());

          erate  += ovl[oo].erate();
          erateN += 1;

          //  Split on the higher coordinate.  If this is larger than the current coordinate AND still
          //  within the verified overlap range, reset the coordinate.  Allow only dovetail overlaps.

          if ((isLow == false) && (rdBii->position.max() < op.verified.max())) {
            writeLog(" - CANDIDATE hangs %7d %7d", ovl[oo].a_hang, ovl[oo].b_hang);

            if ((rdBii->position.max() > coord) && (rdBii->position.min() < op.verified.min()) /* && (ovl[oo].a_hang < 0) */) {
              writeLog(" - SAVED");
              rdB   = rdBii;
              coord = rdBii->position.max();
            }
          }

          //  Split on the lower coordinate.

          if ((isLow == true) && (rdBii->position.min() > op.verified.min())) {
            writeLog(" - CANDIDATE hangs %7d %7d", ovl[oo].a_hang, ovl[oo].b_hang);

            if ((rdBii->position.min() < coord) && (rdBii->position.max() > op.verified.max()) /* && (ovl[oo].b_hang > 0) */) {
              writeLog(" - SAVED");
              rdB   = rdBii;
              coord = rdBii->position.min();
            }
          }

          writeLog("\n");
        }
      }

      if (erateN > 0)
        erate /= erateN;

      //  Huh?  If didn't find any overlaps, give up without crashing (this hasn't ever been triggered).

      if (rdB == NULL) {
        writeLog("\n");
        writeLog("Failed to find appropriate intersecting read.\n");
        writeLog("\n");
        flushLog();

        noOverlaps = true;
        if (beVerbose == false)
          continue;
      } else {
        writeLog("Found appropriate intersecting read.\n");
      }
    }  //  End of toSelf DEBUG

    //  Finally, ignore it if the overlap isn't similar to everything else in the tig.  A
    //  complication here is we don't know what erate we have between tgA and tgB.  We approximate
    //  it by averaging all the overlaps from rdA to the reads it overlaps here.  Kind of expensive,
    //  too bad.

#define REPEAT_FRACTION   0.5

#warning deviationGraph hard coded
double deviationGraph = 6;

    double sim = tgB->overlapConsistentWithTig(deviationGraph, op.verified.min(), op.verified.max(), erate);

    if (sim < REPEAT_FRACTION) {
      notSimilar = true;
      if (beVerbose == false)
        continue;
    }

    //  if not useful, bail.  This only occurs here if beVerbose == true, otherwise, we shortcircuit in the tests above.

    if (toSelf || expected5 || expected3 || tooSmall || isContained || noOverlaps || notSimilar) {
      if (beVerbose)
        writeLog("createUnitigs()-- read %6u place %3d edgeTo tig %5u reads #%5u %9u-%9u verified %9d-%9d position %9d-%9d covered %7d-%7d%s%s%s%s%s%s%s\n",
                 rdA->ident, pp, op.tigID,
                 op.tigFidx, tgB->ufpath[op.tigFidx].ident, tgB->ufpath[op.tigLidx].ident,
                 op.verified.bgn, op.verified.end,
                 op.position.bgn, op.position.end,
                 op.covered.bgn,  op.covered.end,
                 (toSelf      == true) ? " SELF"         : "",
                 (expected5   == true) ? " EXPECTED_5'"  : "",
                 (expected3   == true) ? " EXPECTED_3'"  : "",
                 (tooSmall    == true) ? " TOO_SMALL"    : "",
                 (isContained == true) ? " IS_CONTAINED" : "",   //  Would be nice to report read it's contained in?
                 (noOverlaps  == true) ? " NO_OVERLAPS"  : "",
                 (notSimilar  == true) ? " NOT_SIMILAR"  : "");
      continue;
    }

    //  Otherwise, it's a useful edge.

    if (beVerbose)
      writeLog("createUnitigs()-- read %6u place %3d edgeTo tig %5u reads #%5u %9u-%9u verified %9d-%9d position %9d-%9d covered %7d-%7d BREAK at pos %8u read %6u isLow %u sim %.4f\n",
               rdA->ident, pp, op.tigID,
               op.tigFidx, tgB->ufpath[op.tigFidx].ident, tgB->ufpath[op.tigLidx].ident,
               op.verified.bgn, op.verified.end,
               op.position.bgn, op.position.end,
               op.covered.bgn,  op.covered.end,
               coord, rdB->ident, isLow, sim);

    breaks.push_back(breakPointEnd(op.tigID, coord, isLow));
  }

  if (breaks.size() == 0) {
    //  Do nothing.
  }

  else if (breaks.size() > maxPlacements) {
    writeLog("createUnitigs()-- discarding %u breakpoints.\n", breaks.size());
  }

  else if (breaks.size() <= maxPlacements) {
    writeLog("createUnitigs()-- saving %u breakpoints to master list.\n", breaks.size());

    //breakpoints.isert(breakpoints.end(), breaks.begin(), breaks.end());

    for (uint32 ii=0; ii<breaks.size(); ii++)
      breakpoints.push_back(breaks[ii]);
  }

  return(breaks.size());
}



void
stripNonBackboneFromStart(TigVector &unitigs, Unitig *tig, bool isFirst) {
  vector<ufNode>   ufpath;
  uint32           ii = 0;

  while (OG->isBackbone(tig->ufpath[ii].ident) == false) { //  Find the first backbone read,
    unitigs.registerRead(tig->ufpath[ii].ident);
    writeLog("WARNING: unitig %u %s read %8u %9u-%9u is not backbone, removing.\n",
             tig->id(),
             isFirst ? "first" : "last ",
             tig->ufpath[ii].ident,
             tig->ufpath[ii].position.bgn, tig->ufpath[ii].position.end);
    ii++;
  }

  while (ii < tig->ufpath.size()) {               //  and copy to a new vector.
    ufpath.push_back(tig->ufpath[ii]);
    ii++;
  }

  tig->ufpath.swap(ufpath);                       //  assign the new vector to the tig
  tig->cleanUp();                                 //  adjust zero, find new length
  tig->reverseComplement();                       //  rebuild the idx mappings, and reverse for the next phase
}



void
createUnitigs(TigVector             &contigs,
              TigVector             &unitigs,
              uint32                 minIntersectLen,
              uint32                 maxPlacements,
              vector<confusedEdge>  &confusedEdges,
              vector<tigLoc>        &unitigSource) {

  vector<breakPointEnd>   breaks;

  uint32                  nBreaksSentinel;
  uint32                  nBreaksConfused;
  uint32                  nBreaksIntersection;


  //  Give each tig a pair of bogus breakpoints at the ends, just to get it in the list.  If there
  //  are no break points, it won't be split.  These also serve as sentinels during splitting.

  writeLog("\n");
  writeLog("----------------------------------------\n");
  writeLog("Adding sentinel breaks at the ends of contigs.\n");

  for (uint32 ti=0; ti<contigs.size(); ti++) {
    Unitig    *tig = contigs[ti];

    if ((tig == NULL) ||
        (tig->_isUnassembled == true))
      continue;

    breaks.push_back(breakPointEnd(ti, 0,                true));    //  Add one at the start of the tig
    breaks.push_back(breakPointEnd(ti, tig->getLength(), false));   //  And one at the end
  }

  nBreaksSentinel = breaks.size();


  //  Add breaks for any confused edges detected during repeat detection.  We should, probably,
  //  remove duplicates, but they (should) cause no harm.

  writeLog("\n");
  writeLog("----------------------------------------\n");
  writeLog("Adding breaks at confused reads.\n");

  for (uint32 ii=0; ii<confusedEdges.size(); ii++) {
    uint32  aid   = confusedEdges[ii].aid;
    uint32  a3p   = confusedEdges[ii].a3p;

    uint32  tid   = contigs.inUnitig(aid);
    Unitig *tig   = contigs[tid];

    if ((tig == NULL) ||                 //  It won't be NULL, but we definitely don't want to
        (tig->_isUnassembled == true))   //  see unassembled crap here.  We don't care, and they'll crash.
      continue;

    uint32  tpp   = contigs.ufpathIdx(aid);  //  Not the Trans-Pacific Partnership, FYI.
    ufNode *rda   = &tig->ufpath[tpp];
    uint32  coord = 0;       //  Pick the coordinate and set isLow based on orientation
    bool    isLow = false;   //  and the end of the read that is confused.

    if (((rda->position.isForward() == true)  && (a3p == true)) ||
        ((rda->position.isForward() == false) && (a3p == false))) {
      coord = rda->position.max();
      isLow = false;
    }

    if (((rda->position.isForward() == true)  && (a3p == false)) ||
        ((rda->position.isForward() == false) && (a3p == true))) {
      coord = rda->position.min();
      isLow = true;
    }

    breakPointEnd  bp(tid, coord, isLow);

    if (breaks.back() == bp)
      continue;

    writeLog("createUnitigs()-- add break tig %u pos %u isLow %c\n", tid, coord, (isLow) ? 't' : 'f');

    breaks.push_back(bp);
  }

  nBreaksConfused = breaks.size();


  //  Check the reads at the end of every tig for intersections to other tigs.  If the read has a
  //  compatible overlap to the middle of some other tig, split the other tig into multiple unitigs.

  writeLog("\n");
  writeLog("----------------------------------------\n");
  writeLog("Finding contig-end to contig-middle intersections.\n");

  uint32          *numP = NULL;
  uint32           lenP = 0;
  uint32           maxP = 1024;

  allocateArray(numP, maxP);

  for (uint32 ti=0; ti<contigs.size(); ti++) {
    Unitig    *tig = contigs[ti];

    if ((tig == NULL) ||
        (tig->_isUnassembled == true))
      continue;

    //  Find break points in other tigs using the first and last reads.

    ufNode                   *fi = tig->firstRead();
    ufNode                   *li = tig->lastRead();
    vector<overlapPlacement>  fiPlacements;
    vector<overlapPlacement>  liPlacements;

    placeReadUsingOverlaps(contigs, NULL, fi->ident, fiPlacements, placeRead_all);
    placeReadUsingOverlaps(contigs, NULL, li->ident, liPlacements, placeRead_all);

    if (fiPlacements.size() + liPlacements.size() > 0)
      writeLog("\ncreateUnitigs()-- tig %u len %u first read %u with %lu placements - last read %u with %lu placements\n",
               ti, tig->getLength(),
               fi->ident, fiPlacements.size(),
               li->ident, liPlacements.size());

    uint32 npf = checkRead(tig, fi, fiPlacements, contigs, breaks, minIntersectLen, maxPlacements, true);
    uint32 npr = checkRead(tig, li, liPlacements, contigs, breaks, minIntersectLen, maxPlacements, false);

    lenP = max(lenP, npf);
    lenP = max(lenP, npr);

    resizeArray(numP, maxP, maxP, lenP+1, resizeArray_copyData | resizeArray_clearNew);

    numP[npf]++;
    numP[npr]++;
  }

  nBreaksIntersection = breaks.size();

  writeLog("\n");
  writeLog("Histogram of number of placements per contig end:\n");
  writeLog("numPlacements  numEnds\n");
  for (uint32 pp=0; pp<=lenP; pp++)
    writeLog("%13u  %7u\n", pp, numP[pp]);
  writeLog("\n");
  writeLog("----------------------------------------\n");
  writeLog("Found %u breakpoints (including duplicates).\n", breaks.size());
  writeLog("      %u from sentinels.\n",                     nBreaksSentinel);
  writeLog("      %u from confused edges.\n",                nBreaksConfused     - nBreaksSentinel);
  writeLog("      %u from intersections.\n",                 nBreaksIntersection - nBreaksConfused);
  writeLog("\n");
  writeLog("Splitting contigs into unitigs.\n");
  writeLog("\n");

  delete [] numP;

  //  The splitTigs function operates only on a single tig.  Sort the break points
  //  by tig id to find all the break points for each tig.

  sort(breaks.begin(), breaks.end());


  //  Allocate space for breaking tigs.  These are _vastly_ too big, but guaranteed.

  vector<breakPointEnd>  BP;

  Unitig **newTigs   = new Unitig * [breaks.size() + 2];  //  Plus two, because we add an extra
  int32   *lowCoord  = new int32    [breaks.size() + 2];  //  break at the start and end of each set.

  //  Walk through the breaks, making a new vector of breaks for each tig.

  uint32  ss = 0;
  uint32  ee = 0;

  while (ss < breaks.size()) {
    Unitig  *tig = contigs[breaks[ss]._tigID];

    //  Find the last break point for this tig.  (Technically, the one after the last, but...)

    while ((ee < breaks.size()) && (breaks[ss]._tigID == breaks[ee]._tigID))
      ee++;

    //  Make a new vector for those break points.

    BP.clear();

    for (uint32 bb=ss; bb<ee; bb++)
      if ((BP.size() == 0) ||
          (BP.back()._pos != breaks[bb]._pos) ||
          (BP.back()._bgn != breaks[bb]._bgn))
        BP.push_back(breaks[bb]);

    writeLog("\n");

    if (BP.size() > 2)
      writeLog("createUnitigs()-- contig %u found %u breakpoint%s\n",
               tig->id(), BP.size()-2, (BP.size()-2 != 1) ? "s" : "");

    //  Split the tig.  Copy it into the unitigs TigVector too.

    uint32  nTigs = splitTig(contigs, tig, BP, newTigs, lowCoord, false);

    if (nTigs > 1) {
      splitTig(unitigs, tig, BP, newTigs, lowCoord, true);
      writeLog("createUnitigs()-- contig %u was split into %u unitigs, %u through %u.\n",  //  Can't use newTigs, because
               tig->id(), nTigs, unitigs.size() - nTigs, unitigs.size() - 1);              //  there are holes in it
    }

    else {
      newTigs[0]  = copyTig(unitigs, tig);    // splitTig populates newTigs and lowCoord, used below.
      lowCoord[0] = 0;
      writeLog("createUnitigs()-- contig %u copied into unitig %u.\n", tig->id(), newTigs[0]->id());
    }

    //  Remember where these unitigs came from.

    unitigSource.resize(unitigs.size() + 1);

    for (uint32 tt=0; tt<nTigs; tt++) {
      if (newTigs[tt]) {
        uint32  id = newTigs[tt]->id();

        writeLog("createUnitigs()-- piece %3u -> tig %u from contig %u %u-%u\n",
                 tt, id, tig->id(), lowCoord[tt], lowCoord[tt] + newTigs[tt]->getLength());

        unitigSource[id].cID  = tig->id();
        unitigSource[id].cBgn = lowCoord[tt];
        unitigSource[id].cEnd = lowCoord[tt] + newTigs[tt]->getLength();
        unitigSource[id].uID  = id;
      }
    }

    //  Reset for the next iteration.

    ss = ee;
  }

  //  Remove non-backbone reads from the ends of unitigs.  These confound graph building because
  //  they can be missing overlaps.
  //
  //  If the last read in the tig is not a backbone read, we can remove it and all reads that come
  //  after it (because those reads are contained).

#if 1
  for (uint32 ti=0; ti<unitigs.size(); ti++) {
    Unitig    *tig = unitigs[ti];

    if ((tig == NULL) ||
        (tig->_isUnassembled == true))
      continue;

    //  First, check if we have any backbone reads.  If we have none, leave it as is.

    uint32  bbReads = 0;
    uint32  nbReads = 0;

    for (uint32 li=0; li<tig->ufpath.size(); li++) {
      if (OG->isBackbone(tig->ufpath[li].ident) == true)
        bbReads++;
      else
        nbReads++;
    }

    if (bbReads == 0)
      continue;

    //  Now remove non-backbone reads from the start of the tig.

    writeLog("unitig %u with %u reads, %u backbone and %u unplaced.\n",
             tig->id(), tig->ufpath.size(), bbReads, nbReads);

    stripNonBackboneFromStart(unitigs, tig, true);     //  Does reverse complement at very end
    stripNonBackboneFromStart(unitigs, tig, false);
  }
#endif

  //  Cleanup.

  delete [] newTigs;
  delete [] lowCoord;
}

