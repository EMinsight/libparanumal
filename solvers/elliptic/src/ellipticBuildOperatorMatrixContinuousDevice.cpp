/*

  The MIT License (MIT)

  Copyright (c) 2017 Tim Warburton, Noel Chalmers, Jesse Chan, Ali Karakus

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

*/

#include "elliptic.hpp"
#include "mesh/meshDefines2D.h"
#include "mesh/meshDefines3D.h"

#define nonZero_t parAlmond::parCOO::nonZero_t

int compareColumns(const void *a, const void *b){

  nonZero_t *ea = (nonZero_t*) a;
  nonZero_t *eb = (nonZero_t*) b;

  if(ea->col < eb->col) return -1;
  if(ea->col > eb->col) return +1;

  return 0;
}

typedef struct{
  hlong gnum;
  int   lnum;
}globalNode_t;

int compareGlobalNodes(const void *a, const void *b){

  globalNode_t *ea = (globalNode_t*) a;
  globalNode_t *eb = (globalNode_t*) b;

  if(ea->gnum < eb->gnum) return -1;
  if(ea->gnum > eb->gnum) return +1;

  return 0;
}


static void compressMatrixMultiDevice(platform_t &platform, mesh_t &mesh, ogs_t *ogsMasked, 
				      occa::memory &o_AL,
				      dlong nnzLocal,
				      dlong *AsendCounts,
				      hlong BIG_NUM,
				      deviceSort_t &sorter,
				      deviceScan_t &scanner,
				      occa::memory &o_A,
				      dlong &Annz,
				      parAlmond::parCOO& A){

  // 1. copy to host
  nonZero_t *h_AL = (nonZero_t*) calloc(nnzLocal, sizeof(nonZero_t));
  o_AL.copyTo(h_AL);
  o_AL.free(); // release this memory - it will likely bite us later
  
  // 2. number of degrees of freedom on this rank (after gathering)
  hlong Ngather = ogsMasked->Ngather;

  // 3. every gathered degree of freedom has its own global id
  A.globalRowStarts = (hlong*) calloc(mesh.size+1,sizeof(hlong));
  A.globalColStarts = (hlong*) calloc(mesh.size+1,sizeof(hlong));
  MPI_Allgather(&Ngather, 1, MPI_HLONG, A.globalRowStarts+1, 1, MPI_HLONG, mesh.comm);
  for(int r=0;r<mesh.size;++r) {
    A.globalRowStarts[r+1] = A.globalRowStarts[r]+A.globalRowStarts[r+1];
    A.globalColStarts[r+1] = A.globalRowStarts[r+1];
  }

  int *ArecvCounts  = (int*) calloc(mesh.size+1, sizeof(int));
  int *AsendOffsets = (int*) calloc(mesh.size+1, sizeof(int));
  int *ArecvOffsets = (int*) calloc(mesh.size+1, sizeof(int));

  // 4. find how many nodes to expect (should use sparse version)
  MPI_Alltoall(AsendCounts, 1, MPI_INT, ArecvCounts, 1, MPI_INT, mesh.comm);

  // 5. find send and recv offsets for gather
  A.nnz = 0;
  for(int r=0;r<mesh.size;++r){
    AsendOffsets[r+1] = AsendOffsets[r] + AsendCounts[r];
    ArecvOffsets[r+1] = ArecvOffsets[r] + ArecvCounts[r];
    A.nnz += ArecvCounts[r];    
  }
  
  nonZero_t *entriesIn = (nonZero_t*) calloc(A.nnz+1, sizeof(nonZero_t));

  // 6. determine number to receive
  MPI_Alltoallv(h_AL,      AsendCounts, AsendOffsets, parAlmond::MPI_NONZERO_T,
		entriesIn, ArecvCounts, ArecvOffsets, parAlmond::MPI_NONZERO_T,
		mesh.comm);


  if(1){
    
    double tic2 = MPI_Wtime();
    
    // find location of block starts
    hlong start = A.globalRowStarts[mesh.rank];
    dlong *h_countConns = (dlong*) calloc(Ngather,sizeof(dlong));
    
    // each partial row gets Np entries
    for(dlong n=0;n<A.nnz/mesh.Np;++n){
      hlong row = entriesIn[n*mesh.Np].row - start;
      for(dlong m=0;m<mesh.Np;++m){
	row = mymin(row, entriesIn[n*mesh.Np+m].row - start);
      }
      if(row<Ngather){
	++(h_countConns[row]); // assumes blocks of Np
      }
    }
    
    dlong maxCountNodes = 0;
    dlong *h_starts = (dlong*) calloc(Ngather+1,sizeof(dlong));
    for(dlong n=1;n<=Ngather;++n){
      h_starts[n] = h_starts[n-1] + h_countConns[n-1];
    }
    for(dlong n=0;n<Ngather;++n){
      maxCountNodes = mymax(maxCountNodes, h_countConns[n]);
    }
    
    dlong *h_colMap = (dlong*) calloc((A.nnz)/mesh.Np, sizeof(dlong));
    dlong *h_countOffsets = (dlong*) calloc(Ngather, sizeof(dlong));
    for(dlong n=0;n<(A.nnz/mesh.Np);++n){
      hlong row = entriesIn[n*mesh.Np].row - start;
      for(dlong m=0;m<mesh.Np;++m){
	row = mymin(row, entriesIn[n*mesh.Np+m].row - start);
      }
      if(row<Ngather){
	dlong id = h_starts[row] +  (h_countOffsets[row]++);
	h_colMap[id] = n;
      }
    }
    double tic3 = MPI_Wtime();
    printf("HOST assembly set up %f\n", tic3-tic2);
    
    nonZero_t *h_AL2 = (nonZero_t*) calloc(nnzLocal, sizeof(nonZero_t));
    nonZero_t *h_tmp = (nonZero_t*) calloc(mesh.Np*maxCountNodes, sizeof(nonZero_t));
    int *offsets = (int*) calloc(maxCountNodes, sizeof(int));
    
    double tic0 = MPI_Wtime();
    
    dlong cnt = 0;
    // test run for segmented sort
    for(dlong n=0;n<Ngather;++n){
      dlong sn = h_starts[n];
      dlong sp = h_starts[n+1];

      dlong cnt1 = 0;
      for(dlong s=sn;s<sp;++s){
	dlong id = mesh.Np*h_colMap[s];
	memcpy(h_tmp+cnt1, entriesIn+id, mesh.Np*sizeof(nonZero_t));
	cnt1 += mesh.Np;
      }

#if 0
      for(dlong m=0;m<cnt1;++m){
	if(!(m%mesh.Np)) printf("---\n");
	printf("h_tmp[%d] = (%lld,%lld,%e)\n",m, h_tmp[m].row, h_tmp[m].col, h_tmp[m].val);
      }
#endif 
      // merge row chunks
      int Nchunks = sp-sn;
      memset(offsets, 0, Nchunks*sizeof(int));
      
      int merged = 0;
      int written = 0;
      while(merged<cnt1){

	// find next entry with smallest column from Nchunks lists
	nonZero_t minEnt;
	minEnt.col = BIG_NUM;
	int minc = -1;
	for(int c=0;c<Nchunks;++c){

	  // skip BIG_NUM entries
	  while(offsets[c]<mesh.Np && h_tmp[offsets[c]+c*mesh.Np].row == BIG_NUM){
	    ++offsets[c];
	    ++merged;
	  }
	  // is this the smallest next entry ?
	  if(offsets[c]<mesh.Np){
	    if(h_tmp[offsets[c]+c*mesh.Np].col < minEnt.col){
	      minEnt = h_tmp[offsets[c]+c*mesh.Np];
	      minc = c;
	    }
	  }
	}

	if(merged<cnt1  && minc==-1) printf("lost some entries: merged=%d, cnt1=%d\n", merged, cnt1);
	
	if(merged==cnt1 || minc==-1) break; // forward wind
	
	++offsets[minc];

	// is this a duplicate of last written entry ?
	if(written && minEnt.col==h_AL2[cnt-1].col){
	  h_AL2[cnt-1].val += minEnt.val;
	  ++merged;
	}	
	else{
	  // or a new entry
	  h_AL2[cnt] = minEnt;
	  ++merged;
	  ++cnt;
	  written = 1;
	}
      }
    }
    Annz = cnt;
    double tic1 = MPI_Wtime();
    printf("HOST assembly = %g\n", tic1-tic0);
    
    o_A = platform.device.malloc(Annz*sizeof(nonZero_t), h_AL2);

    free(h_AL2);
    free(h_tmp);
    free(offsets);

  }
  else{
   // 7. make sure at least one fake entry is in local matrix
   nonZero_t dummyEnt;
   dummyEnt.row = BIG_NUM;
   dummyEnt.col = BIG_NUM;
   dummyEnt.val = 0;
   
   entriesIn[A.nnz] = dummyEnt;
   ++A.nnz;
   
   // 8. load onto device and sort
   occa::memory o_AL3 = platform.device.malloc(A.nnz*sizeof(nonZero_t), entriesIn);
   sorter.sort(A.nnz, o_AL3);
   
   // 10. free up host storage
   free(entriesIn);
   
   // 11. assemble matrix (gather duplicates and drop fakes)
   int includeLast = 0; 
   Annz = scanner.segmentedReduction(platform, A.nnz, sizeof(nonZero_t), includeLast, o_AL3, o_A);
   // release buffers
   o_AL3.free();
   
  }

//  free(AsendCounts);
  free(ArecvCounts);
  free(AsendOffsets);
  free(ArecvOffsets);
  free(h_AL);

}


void elliptic_t::BuildOperatorMatrixContinuousDevice(occa::kernel &buildMatrixKernel, occa::memory &o_maskedGlobalNumbering, hlong BIG_NUM,
						     deviceSort_t &sorter, deviceScan_t &scanner, parAlmond::parCOO& A, occa::memory &o_A, dlong &Annz) {


  // build nodeMap [ map local node to local node - sorted so that maps to outgoing rank ]
  
  // find ranges on each rank
  hlong Ngather = ogsMasked->Ngather;
  hlong *globalRowStarts = (hlong*) calloc(mesh.size+1,sizeof(hlong));
  MPI_Allgather(&Ngather, 1, MPI_HLONG, globalRowStarts+1, 1, MPI_HLONG, mesh.comm);
  for(int r=0;r<mesh.size;++r) {
    globalRowStarts[r+1] = globalRowStarts[r]+globalRowStarts[r+1];
  }

  dlong Nlocal = mesh.Np*mesh.Nelements;
  dlong *h_nodeMap = (dlong*) calloc(Nlocal, sizeof(dlong));
  hlong *h_sendNodeMap = (hlong*) calloc(Nlocal, sizeof(hlong));

  memset(h_nodeMap, -1, Nlocal*sizeof(dlong));
  
  dlong cnt = 0;
  dlong *AsendCounts = (dlong*) calloc(mesh.size+1, sizeof(dlong));

  for(int r=0;r<mesh.size;++r){ // yuck
    hlong start = globalRowStarts[r];
    hlong end = globalRowStarts[r+1];
    dlong cntr = 0;
    for(dlong n=0;n<Nlocal;++n){
      hlong id = maskedGlobalNumbering[n];
      if(start<=id && id<end){
	h_nodeMap[n] = cnt;
	h_sendNodeMap[cnt] = id;
	++cnt;
	++cntr;
      }
    }
    AsendCounts[r] = cntr*mesh.Np; // note scaling by mesh.Np
  }
  
  // copy to device
  occa::memory o_nodeMap = platform.device.malloc(Nlocal*sizeof(dlong), h_nodeMap);
  
  // for each element build map to shuffle output
  globalNode_t *gnodes = (globalNode_t*) calloc(mesh.Np, sizeof(globalNode_t));
  dlong *outNodeMap = (dlong*) calloc(mesh.Np*mesh.Nelements, sizeof(dlong));
  for(dlong e=0;e<mesh.Nelements;++e){
    for(int n=0;n<mesh.Np;++n){
      hlong id = maskedGlobalNumbering[e*mesh.Np+n];
      gnodes[n].gnum = (id<0) ? BIG_NUM:id;
      gnodes[n].lnum = n;
    }
    qsort(gnodes, mesh.Np, sizeof(globalNode_t), compareGlobalNodes);
    for(int n=0;n<mesh.Np;++n){
      //      printf("gnodes[%d] = %lld,%d\n", n, gnodes[n].gnum, gnodes[n].lnum);
      outNodeMap[e*mesh.Np+n] = gnodes[n].lnum;
    }
  }
  
  occa::memory o_outNodeMap = platform.device.malloc(mesh.Nelements*mesh.Np*sizeof(dlong), outNodeMap);
  
  // count number of entries
  dlong nnzLocal = cnt*mesh.Np;
  occa::memory o_AL =  platform.malloc(nnzLocal*sizeof(nonZero_t));
  
  switch(mesh.elementType){
  case TRIANGLES:
    buildMatrixKernel(mesh.Nelements, o_maskedGlobalNumbering, o_nodeMap, o_outNodeMap,
		      mesh.o_S, mesh.o_MM, mesh.o_ggeo, 
		      lambda, o_AL);

    break;
  case QUADRILATERALS:
    buildMatrixKernel(mesh.Nelements, o_maskedGlobalNumbering, o_nodeMap, o_outNodeMap,
		      mesh.o_D, mesh.o_ggeo, 
		      lambda, o_AL);

    break;
  case TETRAHEDRA:
    buildMatrixKernel(mesh.Nelements, o_maskedGlobalNumbering, o_nodeMap, o_outNodeMap,
		      mesh.o_S, mesh.o_MM, mesh.o_ggeo, 
		      lambda, o_AL);
    break;
  case HEXAHEDRA:
    buildMatrixKernel(mesh.Nelements, o_maskedGlobalNumbering, o_nodeMap, o_outNodeMap,
		      mesh.o_D,  mesh.o_ggeo,
		      lambda, o_AL);
    break;
  }

  // assemble on device + MPI
  // [ warning - destroys o_AL ]
  // output is to host A
  compressMatrixMultiDevice(platform, mesh, ogsMasked, o_AL, nnzLocal, AsendCounts, BIG_NUM, sorter, scanner, o_A, Annz, A);

  free(AsendCounts);
  
}
