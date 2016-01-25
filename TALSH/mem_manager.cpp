/** Explicit memory management for the accelerator-enabled
implementation of the tensor algebra library TAL-SH:
CP-TAL (TAL for CPU), NV-TAL (TAL for NVidia GPU),
XP-TAL (TAL for Intel Xeon Phi), AM-TAL (TAL for AMD GPU).
REVISION: 2016/01/24
Copyright (C) 2015 Dmitry I. Lyakh (email: quant4me@gmail.com)
Copyright (C) 2015 Oak Ridge National Laboratory (UT-Battelle)

This source file is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
-------------------------------------------------------------------------------
OPTIONS (PREPROCESSOR):
 # -DNO_GPU: disables GPU usage.
 # -DNO_MIC: disables Intel MIC usage (future).
 # -DNO_AMD: disables AMD GPU usage (future).
FOR DEVELOPERS ONLY:
 # So far each argument buffer entry is occupied as a whole,
   making it impossible to track the actual amount of memory
   requested by the application. This needs to be fixed.
**/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef NO_GPU
#include <cuda.h>
#include <cuda_runtime.h>
#endif

#include "tensor_algebra.h"

#define GPU_MEM_PART_USED 90         //percentage of free GPU global memory to be actually allocated for GPU argument buffers
#define MEM_ALIGN GPU_CACHE_LINE_LEN //memory alignment (in bytes) for argument buffers
//Host argument buffer structure:
#define BLCK_BUF_DEPTH_HOST 7        //number of distinct tensor block buffer levels on Host
#define BLCK_BUF_TOP_HOST 3          //number of argument buffer entries of the largest size (level 0) on Host: multiple of 3
#define BLCK_BUF_BRANCH_HOST 2       //branching factor for each subsequent buffer level on Host
//GPU argument buffer structure (the total number of entries must be less of equal to MAX_GPU_ARGS):
#define BLCK_BUF_DEPTH_GPU 4         //number of distinct tensor block buffer levels on GPU
#define BLCK_BUF_TOP_GPU 3           //number of argument buffer entries of the largest size (level 0) on GPU: multiple of 3
#define BLCK_BUF_BRANCH_GPU 2        //branching factor for each subsequent buffer level on GPU

static int VERBOSE=1; //verbosity (for errors)
static int DEBUG=0;   //debugging

//DERIVED TYPES:
// Argument buffer configuration:
typedef struct{
 int buf_top;    //amount of top-level blocks (of the largest size)
 int buf_depth;  //number of levels
 int buf_branch; //branching factor for each subsequent level
} ab_conf_t;

//MODULE DATA:
// Buffer memory management:
static int bufs_ready=0; //status of the Host and GPU argument buffers
static void *arg_buf_host; //base address of the argument buffer in Host memory (page-locked)
static void *arg_buf_gpu[MAX_GPUS_PER_NODE]; //base addresses of argument buffers in GPUs Global memories
static size_t arg_buf_host_size=0; //total size of the Host argument buffer in bytes
static size_t arg_buf_gpu_size[MAX_GPUS_PER_NODE]; //total sizes of each GPU argument buffer in bytes
static int max_args_host=0; //max number of arguments (those of the lowest size level) which can reside in Host buffer
static int max_args_gpu[MAX_GPUS_PER_NODE]; //max number of arguments (those of the lowest size level) which can reside in a GPU buffer: will be overtaken by MAX_GPU_ARGS
static size_t blck_sizes_host[BLCK_BUF_DEPTH_HOST]; //distinct tensor block buffered sizes (in bytes) on Host
static size_t blck_sizes_gpu[MAX_GPUS_PER_NODE][BLCK_BUF_DEPTH_GPU]; //distinct tensor block buffered sizes (in bytes) on GPUs
static int const_args_link[MAX_GPUS_PER_NODE][MAX_GPU_ARGS]; //linked list of free entries in constant memory banks for each GPU
static int const_args_ffe[MAX_GPUS_PER_NODE]; //FFE of the const_args_link[] for each GPU
static size_t *abh_occ=NULL; //occupation status for each buffer entry in Host argument buffer (*arg_buf_host)
static size_t *abg_occ[MAX_GPUS_PER_NODE]; //occupation status for each buffer entry in GPU argument buffers(*arg_buf_gpu)
static size_t abh_occ_size=0; //total number of entries in the multi-level Host argument buffer occupancy table
static size_t abg_occ_size[MAX_GPUS_PER_NODE]; //total numbers of entries in the multi-level GPUs argument buffer occupancy tables
// Buffer memory status:
static int num_args_host=0; //number of occupied entries in the Host argument buffer
static int num_args_gpu[MAX_GPUS_PER_NODE]={0}; //number of occupied entries in each GPU argument buffer
static size_t occ_size_host=0; //total size (bytes) of all occupied entries in the Host argument buffer
static size_t occ_size_gpu[MAX_GPUS_PER_NODE]={0}; //total size (bytes) of all occupied entries in each GPU buffer
static size_t args_size_host=0; //total size (bytes) of all arguments in the Host argument buffer !`Not used now
static size_t args_size_gpu[MAX_GPUS_PER_NODE]={0}; //total size (bytes) of all arguments in each GPU buffer !`Not used now

//LOCAL (PRIVATE) FUNCTION PROTOTYPES:
static int const_args_link_init(int gpu_beg, int gpu_end);
static int ab_get_2d_pos(ab_conf_t ab_conf, int entry_num, int *level, int *offset);
static int ab_get_1d_pos(ab_conf_t ab_conf, int level, int offset);
static int ab_get_parent(ab_conf_t ab_conf, int level, int offset);
static int ab_get_1st_child(ab_conf_t ab_conf, int level, int offset);
static size_t ab_get_offset(ab_conf_t ab_conf, int level, int offset, const size_t *blck_sizes);
static int get_buf_entry(ab_conf_t ab_conf, size_t bsize, void *arg_buf_ptr, size_t *ab_occ, size_t ab_occ_size,
                         const size_t *blck_sizes, char **entry_ptr, int *entry_num);
static int free_buf_entry(ab_conf_t ab_conf, size_t *ab_occ, size_t ab_occ_size, const size_t *blck_sizes, int entry_num);
//------------------------------------------------------------------------------------------------------------------------

//FUNCTION DEFINITIONS:
static int ab_get_2d_pos(ab_conf_t ab_conf, int entry_num, int *level, int *offset)
/** Given an argument buffer entry number, this function returns the
corresponding buffer level and offset within that level **/
{
 int i,j,k,m;
 if(entry_num >= 0){
  m=ab_conf.buf_top; k=0; j=m;
  for(i=0;i<ab_conf.buf_depth;i++){
   if(entry_num<j){*level=i; *offset=entry_num-k; return 0;};
   m*=ab_conf.buf_branch; k=j; j=k+m;
  }
  return 1; //entry number is out of range
 }else{
  return 2;
 }
}

static int ab_get_1d_pos(ab_conf_t ab_conf, int level, int offset)
/** Given a buffer level and offset within it,
this function returns the plain buffer entry number **/
{
 int i,j,k;
 if(level >= 0 && level < ab_conf.buf_depth && offset >= 0){
  if(level == 0) return offset; j=ab_conf.buf_top; k=ab_conf.buf_top;
  for(i=1;i<ab_conf.buf_depth;i++){k*=ab_conf.buf_branch; if(i==level) break; j+=k;}
  if(offset < k){return j+offset;}else{return -1;}
 }else{
  return -2; //invalid buffer level
 }
}

static int ab_get_parent(ab_conf_t ab_conf, int level, int offset)
/** This function returns the offset of the parent of a given buffer entry {level, offset} **/
{
 if(level >= 0 && level < ab_conf.buf_depth && offset >= 0 && ab_conf.buf_branch > 0){
  return offset/ab_conf.buf_branch;
 }else{
  return -1;
 }
}

static int ab_get_1st_child(ab_conf_t ab_conf, int level, int offset)
{
/** This function returns the offset of the 1st child for a given buffer entry {level, offset} **/
 if(level >= 0 && level < ab_conf.buf_depth && offset >= 0 && ab_conf.buf_branch > 0){
  return offset*ab_conf.buf_branch;
 }else{
  return -1;
 }
}

static size_t ab_get_offset(ab_conf_t ab_conf, int level, int offset, const size_t *blck_sizes)
/** This function returns a byte offset in the argument buffer space
corresponding to a given buffer entry {level, offset}.
Note that the base address of the argument buffer must be added a posteriori!
No arguments bounds check here! **/
{
 int i,j;
 size_t ab_offset=0;
 ab_offset=offset*blck_sizes[level]; j=offset;
 for(i=level;i>0;i--){
  j=ab_get_parent(ab_conf,i,j);
  ab_offset+=(blck_sizes[i-1]%ab_conf.buf_branch)*j;
 }
 return ab_offset;
}

int arg_buf_allocate(size_t *arg_buf_size, int *arg_max, int gpu_beg, int gpu_end)
/** This function initializes all argument buffers on the Host and GPUs in the range [gpu_beg..gpu_end].
INPUT:
 # arg_buf_size - requested size of the page-locked Host argument buffer in bytes;
 # [gpu_beg..gpu_end] - range of GPUs assigned to the current MPI process;
OUTPUT:
 # arg_buf_size - actual size of the allocated page-locked Host argument buffer in bytes;
 # arg_max - max number of arguments the Host buffer can contain (those of the lowest size level).
**/
{
 size_t hsize,total,mem_alloc_dec;
 int i,j,err_code;
#ifndef NO_GPU
 cudaError_t err=cudaSuccess;
#endif

 if(bufs_ready != 0) return 1; //buffers are already allocated
 *arg_max=0; abh_occ=NULL; abh_occ_size=0; max_args_host=0; arg_buf_host_size=0;
 for(i=0;i<MAX_GPUS_PER_NODE;i++){abg_occ[i]=NULL; abg_occ_size[i]=0; max_args_gpu[i]=0; arg_buf_gpu_size[i]=0;}
//Allocate the Host argument buffer:
 mem_alloc_dec=MEM_ALIGN*BLCK_BUF_TOP_HOST; for(i=1;i<BLCK_BUF_DEPTH_HOST;i++) mem_alloc_dec*=BLCK_BUF_BRANCH_HOST;
 hsize=*arg_buf_size; hsize-=hsize%mem_alloc_dec; err_code=1;
 while(hsize > mem_alloc_dec){
#ifndef NO_GPU
  err=cudaHostAlloc(&arg_buf_host,hsize,cudaHostAllocPortable);
  if(err != cudaSuccess){
   hsize-=mem_alloc_dec;
  }else{
   *arg_buf_size=hsize; arg_buf_host_size=hsize; err_code=0;
   if(DEBUG) printf("\n#DEBUG(c_proc_bufs.cu:arg_buf_allocate): Pinned Host argument buffer address/size: %p %lld\n",arg_buf_host,(long long)hsize); //debug
   break;
  }
#else
  arg_buf_host=malloc(hsize);
  if(arg_buf_host == NULL){
   hsize-=mem_alloc_dec;
  }else{
   *arg_buf_size=hsize; arg_buf_host_size=hsize; err_code=0;
   if(DEBUG) printf("\n#DEBUG(c_proc_bufs.cu:arg_buf_allocate): Host buffer address/size: %p %lld\n",arg_buf_host,(long long)hsize); //debug
   break;
  }
#endif
 }
 if(err_code == 0){
//Set buffered block sizes hierarchy (buffer levels) for the Host argument buffer:
  hsize=BLCK_BUF_TOP_HOST; max_args_host=BLCK_BUF_TOP_HOST; blck_sizes_host[0]=arg_buf_host_size/BLCK_BUF_TOP_HOST;
  for(i=1;i<BLCK_BUF_DEPTH_HOST;i++){
   blck_sizes_host[i]=blck_sizes_host[i-1]/BLCK_BUF_BRANCH_HOST; max_args_host*=BLCK_BUF_BRANCH_HOST;
   hsize+=max_args_host;
  }
  *arg_max=max_args_host;
//Initialize the Host argument buffer occupancy tables:
  abh_occ=(size_t*)malloc(hsize*sizeof(size_t)); if(abh_occ == NULL) return 2; //Host buffer occupancy table
  abh_occ_size=hsize;
  for(hsize=0;hsize<abh_occ_size;hsize++){abh_occ[hsize]=0;} //initialize zero occupancy for each buffer entry
  num_args_host=0; occ_size_host=0; args_size_host=0; //clear Host memory statistics
#ifndef NO_GPU
//Allocate GPUs buffers, if needed:
  if(gpu_beg >= 0 && gpu_end >= gpu_beg){ //GPU exist for this MPI process
   err=cudaGetDeviceCount(&i); if(err != cudaSuccess) return 3;
   if(gpu_end < MAX_GPUS_PER_NODE && gpu_end < i){
    err_code=init_gpus(gpu_beg,gpu_end); if(err_code < 0) return 4;
// Constant memory banks for all GPUs:
    err_code=const_args_link_init(gpu_beg,gpu_end); if(err_code != 0) return 5;
// Global memory banks for each GPU:
    mem_alloc_dec=MEM_ALIGN*BLCK_BUF_TOP_GPU; for(i=1;i<BLCK_BUF_DEPTH_GPU;i++) mem_alloc_dec*=BLCK_BUF_BRANCH_GPU;
    for(i=gpu_beg;i<=gpu_end;i++){
     if(gpu_is_mine(i) != 0){ //Initialize only my GPUs
      err=cudaSetDevice(i); if(err != cudaSuccess) return 6;
      err=cudaMemGetInfo(&hsize,&total); if(err != cudaSuccess) return 7;
      hsize=(size_t)(float(hsize)/100.0f*float(GPU_MEM_PART_USED)); hsize-=hsize%mem_alloc_dec; err_code=1;
      while(hsize > mem_alloc_dec){
       err=cudaMalloc(&arg_buf_gpu[i],hsize);
       if(err != cudaSuccess){
        hsize-=mem_alloc_dec;
       }else{
        arg_buf_gpu_size[i]=hsize; err_code=0;
        if(DEBUG) printf("\n#DEBUG(c_proc_bufs.cu:arg_buf_allocate): GPU#%d argument buffer address/size: %p %lld\n",i,arg_buf_gpu[i],(long long)hsize); //debug
        break;
       }
      }
      if(err_code == 0){
// Set buffered block sizes hierarchy (buffer levels) for each GPU argument buffer:
       hsize=BLCK_BUF_TOP_GPU; max_args_gpu[i]=BLCK_BUF_TOP_GPU; blck_sizes_gpu[i][0]=arg_buf_gpu_size[i]/BLCK_BUF_TOP_GPU;
       for(j=1;j<BLCK_BUF_DEPTH_GPU;j++){
        blck_sizes_gpu[i][j]=blck_sizes_gpu[i][j-1]/BLCK_BUF_BRANCH_GPU; max_args_gpu[i]*=BLCK_BUF_BRANCH_GPU;
        hsize+=max_args_gpu[i];
       }
       if(max_args_gpu[i] > MAX_GPU_ARGS) return 8; //Increase MAX_GPU_ARGS and recompile
// Initialize each GPU argument buffer occupancy tables:
       abg_occ[i]=(size_t*)malloc(hsize*sizeof(size_t)); if(abg_occ[i] == NULL) return 9; //GPU#i buffer occupancy table
       abg_occ_size[i]=hsize;
       for(hsize=0;hsize<abg_occ_size[i];hsize++){abg_occ[i][hsize]=0;} //initialize each buffer entry to zero occupancy
       num_args_gpu[i]=0; occ_size_gpu[i]=0; args_size_gpu[i]=0; //clear GPU memory statistics
      }else{
       return 10;
      }
     }
    }
   }else{
    return 11;
   }
  }
#endif
 }else{
  return 12;
 }
 bufs_ready=1; //mark the Host and GPU argument buffers as ready
 return 0;
}

int arg_buf_deallocate(int gpu_beg, int gpu_end)
/** This function deallocates all argument buffers on the Host and GPUs in the range [gpu_beg..gpu_end] **/
{
 int i,err_code;
#ifndef NO_GPU
 cudaError_t err=cudaSuccess;
#endif

 if(bufs_ready == 0) return -1; //buffers are not allocated
 err_code=0;
 if(abh_occ != NULL) free(abh_occ); abh_occ=NULL; abh_occ_size=0; max_args_host=0;
 for(i=0;i<MAX_GPUS_PER_NODE;i++){
  if(abg_occ[i] != NULL) free(abg_occ[i]); abg_occ[i]=NULL; abg_occ_size[i]=0; max_args_gpu[i]=0;
 }
 arg_buf_host_size=0; num_args_host=0; occ_size_host=0; args_size_host=0; //clear Host memory statistics
#ifndef NO_GPU
 err=cudaFreeHost(arg_buf_host);
 if(err != cudaSuccess){
  if(VERBOSE) printf("\n#ERROR(c_proc_bufs.cu:arg_buf_deallocate): Host argument buffer deallocation failed!");
  err_code=1;
 }
 if(gpu_beg >= 0 && gpu_end >= gpu_beg){
  for(i=gpu_beg;i<=gpu_end;i++){
   if(i < MAX_GPUS_PER_NODE){
    if(gpu_is_mine(i) != 0){
     err=cudaSetDevice(i); if(err == cudaSuccess){
      arg_buf_gpu_size[i]=0; num_args_gpu[i]=0; occ_size_gpu[i]=0; args_size_gpu[i]=0; //clear GPU memory statistics
      err=cudaFree(arg_buf_gpu[i]);
      if(err != cudaSuccess){
       if(VERBOSE) printf("\n#ERROR(c_proc_bufs.cu:arg_buf_deallocate): GPU# %d argument buffer deallocation failed!",i);
       err_code++;
      }
     }else{
      if(VERBOSE) printf("\n#ERROR(c_proc_bufs.cu:arg_buf_deallocate): Unable to set GPU# %d!",i);
      err_code++;
     }
    }
   }else{
    err_code++;
   }
  }
  i=free_gpus(gpu_beg,gpu_end); if(i != 0) err_code+=1000;
 }
#else
 free(arg_buf_host); arg_buf_host=NULL;
#endif
 bufs_ready=0;
 return err_code;
}

int arg_buf_clean_host()
/** Returns zero if all entries of the Host argument buffer are free.
The first buffer entry, which is not free, will cause positive return status.
Negative return status means that an error occurred. **/
{
 if(bufs_ready == 0) return -1; //memory buffers are not initialized
 for(size_t i=0;i<abh_occ_size;i++){if(abh_occ[i] != 0) return (int)(i+1);}
 return 0;
}

#ifndef NO_GPU
int arg_buf_clean_gpu(int gpu_num)
/** Returns zero if all entries of the GPU#gpu_num argument buffer are free.
The first buffer entry, which is not free, will cause positive return status.
Negative return status means that an error occurred. **/
{
 if(bufs_ready == 0) return -1; //memory buffers are not initialized
 if(gpu_num >= 0 && gpu_num < MAX_GPUS_PER_NODE){
  if(gpu_is_mine(gpu_num) != 0){
   for(size_t i=0;i<abg_occ_size[gpu_num];i++){if(abg_occ[gpu_num][i] != 0) return (int)(i+1);}
  }else{
   return -2;
  }
 }else{
  return -3; //invalid GPU number
 }
 return 0;
}
#endif

int get_blck_buf_sizes_host(size_t *blck_sizes)
/** This function returns the registered block (buffered) sizes for each level of the Host argument buffer.
Negative return status means that an error occurred. **/
{
 if(bufs_ready == 0) return -1;
 for(int i=0;i<BLCK_BUF_DEPTH_HOST;i++){blck_sizes[i]=blck_sizes_host[i];}
 return BLCK_BUF_DEPTH_HOST; //depth of the argument buffer
}

#ifndef NO_GPU
int get_blck_buf_sizes_gpu(int gpu_num, size_t *blck_sizes)
/** This function returns the registered block (buffered) sizes for each level of the GPU#gpu_num argument buffer.
Negative return status means that an error occurred. **/
{
 if(bufs_ready == 0) return -1;
 if(gpu_num >= 0 && gpu_num < MAX_GPUS_PER_NODE){
  if(gpu_is_mine(gpu_num) != 0){
   for(int i=0;i<BLCK_BUF_DEPTH_GPU;i++){blck_sizes[i]=blck_sizes_gpu[gpu_num][i];}
  }else{
   return -2;
  }
 }else{
  return -3;
 }
 return BLCK_BUF_DEPTH_GPU; //depth of the argument buffer
}
#endif

static int get_buf_entry(ab_conf_t ab_conf, size_t bsize, void *arg_buf_ptr, size_t *ab_occ, size_t ab_occ_size,
                         const size_t *blck_sizes, char **entry_ptr, int *entry_num)
/** This function finds an appropriate argument buffer entry in any given argument buffer **/
{
 int i,j,k,l,m,n;
// if(DEBUG) printf("\n#DEBUG(c_proc_bufs.cu:get_buf_entry): %lu %lu\n",bsize,blck_sizes[0]); //debug
 *entry_ptr=NULL; *entry_num=-1;
 n=0; j=0; i=0; l=0; //l is a base offset within level i
 while(i<ab_conf.buf_depth){ //argument buffer level
  if(i > 0){k=ab_conf.buf_branch;}else{k=ab_conf.buf_top;};
  j=l%k; l-=j; j+=n;
  while(j<k){ //(l+j) is an offset within level i
   m=ab_get_1d_pos(ab_conf,i,l+j); if(m < 0 || m >= ab_occ_size) return 1; //m is an absolute offset in an occupancy table
//   if(DEBUG) printf("\n#DEBUG(c_proc_bufs.cu:get_buf_entry): Current level/offset/sizes: %d %d %d \n",i,l+j,blck_sizes[i]); //debug
   if(bsize <= blck_sizes[i]-ab_occ[m]){ //there is a good chance to find a free entry along this path
    if(i == ab_conf.buf_depth-1 && ab_occ[m] == 0){
     *entry_num=m; *entry_ptr=&(((char*)arg_buf_ptr)[ab_get_offset(ab_conf,i,l+j,blck_sizes)]); //entry found
     break;
    }else{
     if(blck_sizes[i+1] < bsize && ab_occ[m] == 0){
      *entry_num=m; *entry_ptr=&(((char*)arg_buf_ptr)[ab_get_offset(ab_conf,i,l+j,blck_sizes)]); //entry found
      break;
     }else{
      if(i < ab_conf.buf_depth-1){if(blck_sizes[i+1] >= bsize) break;} //initiate passing to the next level
     }
    }
   }
   j++; //horizontal shift
  } //enddo j
  if(*entry_num >= 0) break; //entry found
  if(j < k){ //proceed to the next level
   l=ab_get_1st_child(ab_conf,i,l+j); if(l < 0 || l >= ab_occ_size) return 2; i++; n=0; //go to the next level
  }else{ //back to the upper level
   if(i > 0){
    l=ab_get_parent(ab_conf,i,l); if(l < 0 || l >= ab_occ_size) return 3; i--; n=1; //return to the previous level
   }else{
    break;
   }
  }
 } //enddo i
 if(*entry_num >= 0 && *entry_num < ab_occ_size){
  k=blck_sizes[i]; ab_occ[m]=k;
  while(i>0){ //modify occupancy of the upper-level parental entries
   l=ab_get_parent(ab_conf,i,l); i--; m=ab_get_1d_pos(ab_conf,i,l); if(m < 0 || m >= ab_occ_size) return 4;
   ab_occ[m]+=k;
  }
 }else{ //no appropriate entry found: not an error
  if(bsize > blck_sizes[0]){
   return DEVICE_UNABLE; //device memory buffer can never provide such a big chunk
  }else{
   return TRY_LATER; //device memory buffer currently cannot provide the requested memory chunk due to occupation
  }
 }
 return 0;
}

static int free_buf_entry(ab_conf_t ab_conf, size_t *ab_occ, size_t ab_occ_size, const size_t *blck_sizes, int entry_num)
/** This function releases an argument buffer entry in any given argument buffer **/
{
 int i,j,k,m;
 k=ab_get_2d_pos(ab_conf,entry_num,&i,&j); if(k != 0) return 1;
 if(ab_occ[entry_num] == blck_sizes[i]){ //buffer entries are always occupied as a whole
  k=blck_sizes[i]; ab_occ[entry_num]=0;
  while(i>0){ //modify occupancy of the upper-level parental entries
   j=ab_get_parent(ab_conf,i,j); i--; m=ab_get_1d_pos(ab_conf,i,j); if(m < 0 || m >= ab_occ_size) return 2;
   ab_occ[m]-=k;
  }
 }else{
  return 3;
 }
 return 0;
}

int get_buf_entry_host(size_t bsize, char **entry_ptr, int *entry_num)
/** This function returns a pointer to a free argument buffer space in the Host argument buffer.
INPUT:
 # bsize - requested size of a tensor block (in bytes);
OUTPUT:
 # entry_ptr - pointer to a free space in the argument buffer where the tensor block or packet can be put;
 # entry_num - entry number corresponding to the free space assigned to the tensor block or packet;
RETURN STATUS:
 # 0 - success (*entry_num>=0, *entry_ptr!=NULL);
 # TRY_LATER - the argument buffer currently does not have enough space left;
 # DEVICE_UNABLE - the argument buffer can never satisfy this request;
 # Other - an error occurred.
**/
{
 int i,j,err_code;
 ab_conf_t ab_conf;

 if(bufs_ready == 0) return -1;
 err_code=0;
 ab_conf.buf_top=BLCK_BUF_TOP_HOST; ab_conf.buf_depth=BLCK_BUF_DEPTH_HOST; ab_conf.buf_branch=BLCK_BUF_BRANCH_HOST;
 err_code=get_buf_entry(ab_conf,bsize,arg_buf_host,abh_occ,abh_occ_size,blck_sizes_host,entry_ptr,entry_num);
// if(err_code == 0 && DEBUG != 0) printf("\n#DEBUG(c_proc_bufs.cu:get_buf_entry_host): Entry allocated: %d %p\n",*entry_num,*entry_ptr); //debug
 if(err_code == 0){
  err_code=ab_get_2d_pos(ab_conf,*entry_num,&i,&j);
  if(err_code == 0){num_args_host++; occ_size_host+=blck_sizes_host[i]; args_size_host+=bsize;}
 }
 return err_code;
}

int free_buf_entry_host(int entry_num)
/** This function releases a Host argument buffer entry.
INPUT:
 # entry_num - argument buffer entry number.
**/
{
 int i,j,err_code;
 ab_conf_t ab_conf;

 if(bufs_ready == 0) return -1;
 err_code=0;
 ab_conf.buf_top=BLCK_BUF_TOP_HOST; ab_conf.buf_depth=BLCK_BUF_DEPTH_HOST; ab_conf.buf_branch=BLCK_BUF_BRANCH_HOST;
 err_code=free_buf_entry(ab_conf,abh_occ,abh_occ_size,blck_sizes_host,entry_num);
// if(err_code == 0 && DEBUG != 0) printf("\n#DEBUG(c_proc_bufs.cu:free_buf_entry_host): Entry deallocated: %d\n",entry_num); //debug
 if(err_code == 0){
  err_code=ab_get_2d_pos(ab_conf,entry_num,&i,&j);
  if(err_code == 0){num_args_host--; occ_size_host-=blck_sizes_host[i]; args_size_host=0;} //`args_size_host is not used (ignore it)
 }
 return err_code;
}

#ifndef NO_GPU
int get_buf_entry_gpu(int gpu_num, size_t bsize, char **entry_ptr, int *entry_num)
/** This function returns a pointer to a free argument buffer space in the GPU#gpu_num argument buffer.
INPUT:
 # gpu_num - GPU number;
 # bsize - requested size of a tensor block (in bytes);
OUTPUT:
 # entry_ptr - pointer to a free space in the argument buffer where the tensor block elements can be put;
 # entry_num - entry number corresponding to the free space assigned to the tensor block elements.
RETURN STATUS:
 # 0 - success (*entry_num>=0, *entry_ptr!=NULL);
 # TRY_LATER - the argument buffer currently does not have enough space left;
 # DEVICE_UNABLE - the argument buffer can never satisfy this request;
 # Other - an error occurred.
**/
{
 int i,j,err_code;
 ab_conf_t ab_conf;

 if(bufs_ready == 0) return -1;
 err_code=0;
 if(gpu_num >= 0 && gpu_num < MAX_GPUS_PER_NODE){
  if(gpu_is_mine(gpu_num) != 0){
   ab_conf.buf_top=BLCK_BUF_TOP_GPU; ab_conf.buf_depth=BLCK_BUF_DEPTH_GPU; ab_conf.buf_branch=BLCK_BUF_BRANCH_GPU;
   err_code=get_buf_entry(ab_conf,bsize,arg_buf_gpu[gpu_num],abg_occ[gpu_num],abg_occ_size[gpu_num],&blck_sizes_gpu[gpu_num][0],entry_ptr,entry_num);
// if(err_code == 0 && DEBUG != 0) printf("\n#DEBUG(c_proc_bufs.cu:get_buf_entry_gpu): Entry allocated: %d %d %p\n",gpu_num,*entry_num,*entry_ptr); //debug
   if(err_code == 0){
    err_code=ab_get_2d_pos(ab_conf,*entry_num,&i,&j);
    if(err_code == 0){num_args_gpu[gpu_num]++; occ_size_gpu[gpu_num]+=blck_sizes_gpu[gpu_num][i]; args_size_gpu[gpu_num]+=bsize;}
   }
  }else{
   err_code=-2;
  }
 }else{
  err_code=-3;
 }
 return err_code;
}

int free_buf_entry_gpu(int gpu_num, int entry_num)
/** This function releases a GPU#gpu_num argument buffer entry.
INPUT:
 # gpu_num - GPU number;
 # entry_num - argument buffer entry number.
**/
{
 int i,j,err_code;
 ab_conf_t ab_conf;

 if(bufs_ready == 0) return -1;
 err_code=0;
 if(gpu_num >= 0 && gpu_num < MAX_GPUS_PER_NODE){
  if(gpu_is_mine(gpu_num) != 0){
   ab_conf.buf_top=BLCK_BUF_TOP_GPU; ab_conf.buf_depth=BLCK_BUF_DEPTH_GPU; ab_conf.buf_branch=BLCK_BUF_BRANCH_GPU;
   err_code=free_buf_entry(ab_conf,abg_occ[gpu_num],abg_occ_size[gpu_num],&blck_sizes_gpu[gpu_num][0],entry_num);
// if(err_code == 0 && DEBUG != 0) printf("\n#DEBUG(c_proc_bufs.cu:free_buf_entry_gpu): Entry deallocated: %d %d\n",gpu_num,entry_num); //debug
   if(err_code == 0){
    err_code=ab_get_2d_pos(ab_conf,entry_num,&i,&j);
    if(err_code == 0){num_args_gpu[gpu_num]--; occ_size_gpu[gpu_num]-=blck_sizes_gpu[gpu_num][i]; args_size_gpu[gpu_num]=0;} //`args_size_gpu is not used here (ignore it)
   }
  }else{
   err_code=-2;
  }
 }else{
  err_code=-3;
 }
 return err_code;
}

static int const_args_link_init(int gpu_beg, int gpu_end)
/** This function initializes the linked list const_args_link[]
for GPU constant memory buffers (for each GPU in the range [gpu_beg..gpu_end]) **/
{
 if(gpu_beg >= 0 && gpu_end >= gpu_beg){
  for(int gpu_num=gpu_beg;gpu_num<=gpu_end;gpu_num++){
   if(gpu_num < MAX_GPUS_PER_NODE){
    const_args_ffe[gpu_num]=0; //first free entry for each GPU
    for(int i=0;i<MAX_GPU_ARGS;i++) const_args_link[gpu_num][i]=i+1; //linked list of free entries for each GPU
   }else{
    return 1;
   }
  }
 }
 return 0;
}

int const_args_entry_get(int gpu_num, int *entry_num)
/** This function returns the number of a free const_args[] entry for GPU#gpu_num.
TRY_LATER return status means that currently all entries are busy. **/
{
 *entry_num=-1; if(bufs_ready == 0) return -1;
 if(gpu_num >= 0 && gpu_num < MAX_GPUS_PER_NODE){
  if(gpu_is_mine(gpu_num) != 0){
   if(const_args_ffe[gpu_num] >= 0 && const_args_ffe[gpu_num] < MAX_GPU_ARGS){ //free entry exists
    *entry_num=const_args_ffe[gpu_num];
    const_args_ffe[gpu_num]=const_args_link[gpu_num][const_args_ffe[gpu_num]];
   }else{ //no free entry is currently available
    return TRY_LATER;
   }
  }else{
   return -2;
  }
 }else{
  return -3;
 }
 return 0;
}

int const_args_entry_free(int gpu_num, int entry_num)
/** This function frees an entry of const_args[] for GPU#gpu_num **/
{
 if(bufs_ready == 0) return -1;
 if(gpu_num >= 0 && gpu_num < MAX_GPUS_PER_NODE){
  if(gpu_is_mine(gpu_num) != 0){
   if(entry_num >= 0 && entry_num < MAX_GPU_ARGS){ //valid entry number
    if(const_args_ffe[gpu_num] < MAX_GPU_ARGS && const_args_ffe[gpu_num] >= 0){
     const_args_link[gpu_num][entry_num]=const_args_ffe[gpu_num];
    }
    const_args_ffe[gpu_num]=entry_num;
   }else{ //invalid entry number
    return 1;
   }
  }else{
   return -2;
  }
 }else{
  return -3;
 }
 return 0;
}
#endif

int mem_free_left(int dev_id, size_t * free_mem) //returns free buffer space in bytes
{
 int i,devk;
 *free_mem=0;
 if(bufs_ready == 0) return -1;
 i=decode_device_id(dev_id,&devk);
 if(i >= 0){
  switch(devk){
   case DEV_HOST:
    *free_mem=arg_buf_host_size-occ_size_host;
    break;
#ifndef NO_GPU
   case DEV_NVIDIA_GPU:
    *free_mem=arg_buf_gpu_size[i]-occ_size_gpu[i];
    break;
#endif
#ifndef NO_MIC
   case DEV_INTEL_MIC: //`Future
    break;
#endif
#ifndef NO_AMD
   case DEV_AMD_GPU: //`Future
    break;
#endif
   default:
    return -3; //unknown device kind
  }
 }else{
  return -2; //invalid device id
 }
 return 0;
}

int mem_print_stats(int dev_id) //print memory statistics for Device <dev_id>
{
 int i,devk;
 if(bufs_ready == 0) return -1;
 i=decode_device_id(dev_id,&devk);
 if(i >= 0){
  switch(devk){
   case DEV_HOST:
    printf("\nTAL-SH: Host argument buffer usage state:\n");
    printf(" Total buffer size (bytes)       : %lu\n",arg_buf_host_size);
    printf(" Total number of entries         : %d\n",max_args_host);
    printf(" Number of occupied entries      : %d\n",num_args_host);
    printf(" Size of occupied entries (bytes): %lu\n",occ_size_host);
//  printf(" Size of all arguments (bytes)   : %lu\n",args_size_host);
    break;
#ifndef NO_GPU
   case DEV_NVIDIA_GPU:
    if(gpu_is_mine(i) != GPU_OFF){
     printf("\nTAL-SH: GPU #%d argument buffer usage state:\n",i);
     printf(" Total buffer size (bytes)       : %lu\n",arg_buf_gpu_size[i]);
     printf(" Total number of entries         : %d\n",max_args_gpu[i]);
     printf(" Number of occupied entries      : %d\n",num_args_gpu[i]);
     printf(" Size of occupied entries (bytes): %lu\n",occ_size_gpu[i]);
//   printf(" Size of all arguments (bytes)   : %lu\n",args_size_gpu[i]);
    }else{
     printf("\nTAL-SH: GPU #%d is OFF (no memory statistics).\n",i);
    }
    break;
#endif
#ifndef NO_MIC
   case DEV_INTEL_MIC: //`Future
    break;
#endif
#ifndef NO_AMD
   case DEV_AMD_GPU: //`Future
    break;
#endif
   default:
    return -3; //unknown device kind
  }
 }else{
  return -2; //invalid device id
 }
 return 0;
}

int host_mem_alloc_pin(void **host_ptr, size_t tsize){
#ifndef NO_GPU
 cudaError_t err=cudaHostAlloc(host_ptr,tsize,cudaHostAllocPortable);
 if(err != cudaSuccess) return 1;
#else
 *host_ptr=(void*)malloc(tsize);
 if(*host_ptr == NULL) return 1;
#endif
 return 0;
}

int host_mem_free_pin(void *host_ptr){
#ifndef NO_GPU
 cudaError_t err=cudaFreeHost(host_ptr);
 if(err != cudaSuccess) return 1;
#else
 free(host_ptr); host_ptr = NULL;
#endif
 return 0;
}

int host_mem_register(void *host_ptr, size_t tsize){
#ifndef NO_GPU
 cudaError_t err=cudaHostRegister(host_ptr,tsize,cudaHostAllocPortable);
 if(err != cudaSuccess) return 1;
 return 0;
#else
 return 0; //`Cannot register the Host memory without CUDA
#endif
}

int host_mem_unregister(void *host_ptr){
#ifndef NO_GPU
 cudaError_t err=cudaHostUnregister(host_ptr);
 if(err != cudaSuccess) return 1;
 return 0;
#else
 return 0; //`Cannot register the Host memory without CUDA
#endif
}

#ifndef NO_GPU
int gpu_mem_alloc(void **dev_ptr, size_t tsize, int gpu_id)
/** Allocates global memory on a specific GPU (or current GPU by default).
    A return status NOT_CLEAN is not critical. **/
{
 int i;
 cudaError_t err;
 i=-1;
 if(gpu_id >= 0 && gpu_id < MAX_GPUS_PER_NODE){
  err=cudaGetDevice(&i); if(err != cudaSuccess) return 1;
  err=cudaSetDevice(gpu_id); if(err != cudaSuccess){err=cudaSetDevice(i); return 2;}
 }
 err=cudaMalloc(dev_ptr,tsize); if(err != cudaSuccess){if(i >= 0) err=cudaSetDevice(i); return 3;}
 if(i >= 0){err=cudaSetDevice(i); if(err != cudaSuccess) return NOT_CLEAN;}
 return 0;
}

int gpu_mem_free(void *dev_ptr, int gpu_id)
/** Frees global memory on a specific GPU (or current GPU by default).
    A return status NOT_CLEAN is not critical. **/
{
 int i;
 cudaError_t err;
 i=-1;
 if(gpu_id >= 0 && gpu_id < MAX_GPUS_PER_NODE){
  err=cudaGetDevice(&i); if(err != cudaSuccess) return 1;
  err=cudaSetDevice(gpu_id); if(err != cudaSuccess){err=cudaSetDevice(i); return 2;}
 }
 err=cudaFree(dev_ptr); if(err != cudaSuccess){if(i >= 0) err=cudaSetDevice(i); return 3;}
 if(i >= 0){err=cudaSetDevice(i); if(err != cudaSuccess) return NOT_CLEAN;}
 return 0;
}
#endif