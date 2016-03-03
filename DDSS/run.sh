#It is crucial to launch MPI processes consecutively within a node!
#Environment variable QF_PROCS_PER_NODE must be set appropriately!
export QFORCE_PATH=/ccs/home/div/src/DDSS           #mandatory
export QF_NUM_PROCS=4                               #mandatory
export QF_PROCS_PER_NODE=1                          #mandatory
export QF_CORES_PER_PROC=8                          #mandatory
export QF_GPUS_PER_PROCESS=1                        #optional (Nvidia GPU)
export QF_MICS_PER_PROCESS=0                        #optional (Intel Xeon Phi)
export QF_AMDS_PER_PROCESS=0                        #optional (AMD GPU)
export OMP_NUM_THREADS=8                            #mandatory
export MKL_NUM_THREADS=$OMP_NUM_THREADS             #optional (CPU only: Intel MKL)
export OMP_MAX_ACTIVE_LEVELS=3                      #mandatory
export OMP_THREAD_LIMIT=64                          #optional
export OMP_DYNAMIC=FALSE                            #mandatory
export OMP_NESTED=TRUE                              #mandatory
export OMP_WAIT_POLICY=PASSIVE                      #optional (idle thread behavior)
#export KMP_AFFINITY=compact                        #optional (Intel CPU only)
export MIC_PREFIX=MIC                               #mandatory when using MIC
export MIC_ENV_PREFIX=MIC                           #mandatory when using MIC
export MIC_OMP_PREFIX=MIC                           #mandatory when using MIC
export MIC_OMP_NUM_THREADS=236                      #mandatory when using MIC
export MIC_MKL_NUM_THREADS=$MIC_OMP_NUM_THREADS     #mandatory when using MIC (Intel MIC MKL)
export MIC_KMP_PLACE_THREADS="59c,4t"               #optional (MIC only)
export MIC_KMP_AFFINITY="granularity=fine,compact"  #optional (MIC only)
export MIC_USE_2MB_BUFFERS=64K                      #optional (MIC only)
export MKL_MIC_ENABLE=0                             #optional (MIC only: MKL MIC auto-offloading)
export OFFLOAD_REPORT=2                             #optional (MIC only)
export CRAY_OMP_CHECK_AFFINITY=TRUE                 #optional (CRAY only: shows thread placement)

export MPICH_NEMESIS_ASYNC_PROGRESS="SC"
export MPICH_MAX_THREAD_SAFETY=multiple
export MPICH_RMA_OVER_DMAPP=1
export MPICH_ALLOC_MEM_HUGE_PAGES=1
export MPICH_ALLOC_MEM_HUGEPG_SZ=2M
#export MPICH_ENV_DISPLAY=1
export MPICH_GNI_MALLOC_FALLBACK=enabled
export MPICH_GNI_MEM_DEBUG_FNAME=MPICH.memdebug
#export MPICH_RANK_REORDER_DISPLAY=1

rm *.tmp *.log *.out *.x
cp $QFORCE_PATH/ddss.x ./
aprun -n $QF_NUM_PROCS -N $QF_PROCS_PER_NODE -d $QF_CORES_PER_PROC -cc 0,2,4,6,8,10,12,14 -r 8 ./ddss.x #> qforce.log
#mpiexec -n $QF_NUM_PROCS -npernode $QF_PROCS_PER_NODE ./ddss.x
#nvprof --log-file nv_profile.log --print-gpu-trace ./qforce.v13.01.x # &> qforce.log &
#nvprof --log-file nv_profile.log --print-gpu-trace --metrics branch_efficiency,gld_efficiency,gst_efficiency ./qforce.v13.01.x # &> qforce.log &
#gprof ./qforce.v13.01.x gmon.out > profile.log