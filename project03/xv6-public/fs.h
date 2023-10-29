// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define NDIRECT 10 // * 크기 맞추려면 이거 해야됨. 기존 로직에는 문제 없을거같음 
#define NINDIRECT (BSIZE / sizeof(uint))  // ~ 128: single indirect
#define N2INDIRECT (NINDIRECT * NINDIRECT) // * double indirect: 128 * 128 이겠지? 128개의 single indirect block 중 하나, 그 안에서 128개 주소 중 하나니까
#define N3INDIRECT (N2INDIRECT * NINDIRECT) // * triple indirect: 128 ** 3. double indirect 128개에 대한 indirect block이 있는 셈?
#define MAXFILE (NDIRECT + NINDIRECT + N2INDIRECT + N3INDIRECT) // * 얘도 바꿔줘야 됨. fs.c랑 mkfs.c에서 bn 여기에 비교하니까.

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEV only)
  short minor;          // Minor device number (T_DEV only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  // uint addrs[NDIRECT+1];   // Data block addresses // ~ 기존: direct 12개 + single indirect 1개
  uint addrs[NDIRECT + 3]; // * direct * 12 + single + double + triple
};
// ! ! ! dinode 크기가 512의 약수가 되어야 함. 안 그러면 mkfs에서 assert fail
// ! 기존 크기는 8 + 4 + 4 * (12 + 1) = 12 + 52 = 64 = 2 ** 6 이었고
// ! 8 + 4 + 4 * (12 + 3) = 12 + 60 = 72 이라서 안되는거면..
// ! 그럼 BSIZE를 수정.. 하는건 좀 오바같고
// ! dinode 크기를 기존처럼 2**6로 하거나 아예 확 늘려서 2**7로 하면 되긴하겠는데
// ! 기존처럼 2**6 하려면 addrs 배열 크기는 건드리면 안되는데? 그럼 오히려 기존 NDIRECT 수를 줄여야하는데
// ! 2**7로 확 늘리는건 더욱 오바다 배열을 얼마나 키워야하는거야
// ! 그럼 NDIRECT 수를 줄여보는거로 해보자. 원래 없던 2개 추가할 거니까 2개 줄여서 10개로 줄여야 됨.
// ! NDIRECT가 기존 12였다가 이제 10이 되면 문제 될 상황을 생각해보자
// ! 일단 fs.c의 bmap 에서 bn이 NDIRECT보다 크면 bn - NDIRECT에 대해서 single indirect 주소 변환 했음.
// ! 아 그럼 결국 문제 없겠다 어차피 bn은 똑같이 순차적으로 생각하고 단계만 달라지는거니까 ㅇㅇ

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) (b/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent { // * directory entry?
  ushort inum;
  char name[DIRSIZ];
};

