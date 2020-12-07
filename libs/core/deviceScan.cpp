/* The MIT License (MIT)
 *
 * Copyright (c) 2014-2018 David Medina and Tim Warburton
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 */

#include "mesh.hpp"

// was 512
#define SCAN_BLOCK_SIZE 1024


//template <class T>
dlong deviceScan_t::blockCount(const dlong entries){
  return ((entries+SCAN_BLOCK_SIZE-1)/SCAN_BLOCK_SIZE);  
}

void deviceScan_t::scan(const int    entries,
			occa::memory &o_list,
			occa::memory &o_tmp,
			dlong *h_tmp,
			occa::memory &o_scan) {

  dlong Nblocks = blockCount(entries);
  
  // 1. launch DEVICE block scan kernel
  blockShflScanKernel(entries, o_list, o_scan, o_tmp);

  // 2. copy offsets back to HOST
  o_tmp.copyTo(h_tmp);

  // 3. scan offsets on HOST
  for(int n=1;n<Nblocks;++n){
    h_tmp[n] += h_tmp[n-1];
  }
  
  // 4. copy scanned offsets back to DEVCE
  o_tmp.copyFrom(h_tmp);

  // 5. finalize scan
  finalizeScanKernel(entries, o_tmp, o_scan);
  
}

void  deviceScan_t::mallocTemps(occa::device &device, dlong entries, occa::memory &o_tmp, dlong **h_tmp){
  
  size_t sz =  sizeof(dlong)*blockCount(entries);
  o_tmp = device.malloc(sz);
  *h_tmp = (dlong*) malloc(sz);
}


deviceScan_t::deviceScan_t(occa::device &device, const char *entryType, const char *entryMap, occa::properties props){

  // Compile the kernel at run-time
  occa::settings()["kernel/verbose"] = true;

  occa::properties kernelInfo = props;
  
  kernelInfo["includes"] += entryType; // "entry.h";
  kernelInfo["includes"] += entryMap;  // "compareEntry.h";
  kernelInfo["defines/SCAN_BLOCK_SIZE"] = (int)SCAN_BLOCK_SIZE;
  
  blockShflScanKernel = device.buildKernel(LIBCORE_DIR "/okl/blockShflScan.okl",
				      "blockShflScan", kernelInfo);

  finalizeScanKernel = device.buildKernel(LIBCORE_DIR "/okl/blockShflScan.okl",
				      "finalizeScan", kernelInfo);
  

}

