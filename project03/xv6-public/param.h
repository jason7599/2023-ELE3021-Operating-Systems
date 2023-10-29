#define NPROC        64  // maximum number of processes
#define KSTACKSIZE 4096  // size of per-process kernel stack
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes

// ! 이거 키워야 하는거 같음. 근데 struct logheader 크기는 BSIZE보다 작아야해서 해보니 최대 126까지 키울 수 있다
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log 
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache

#define FSSIZE       50000    // size of file system in blocks // ~ 기존 1000
// * 이제 한 파일당 최대 2,113,674 블록 차지 가능.

