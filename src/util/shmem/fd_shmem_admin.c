#if FD_HAS_THREADS && FD_HAS_X86 /* THREADS implies HOSTED */
#define _GNU_SOURCE
#endif

#include "fd_shmem_private.h"

#if FD_HAS_HOSTED && FD_HAS_X86

/* Note: There is an error in system headers (on RHEL8 at least) such
   that linux/mempolicy.h must be before numaif.h */

#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <numa.h>
#include <linux/mempolicy.h>
#include <numaif.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/mman.h>

#if FD_HAS_THREADS
pthread_mutex_t fd_shmem_private_lock[1] = { PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP };
#endif

char  fd_shmem_private_base[ FD_SHMEM_PRIVATE_BASE_MAX ]; /* ""  at thread group start, initialized at boot */
ulong fd_shmem_private_base_len;                          /* 0UL at ",                  initialized at boot */

/* NUMA TOPOLOGY APIS *************************************************/

static ulong  fd_shmem_private_numa_cnt;                      /* 0UL at thread group start, initialized at boot */
static ulong  fd_shmem_private_cpu_cnt;                       /* " */
static ushort fd_shmem_private_numa_idx[ FD_SHMEM_CPU_MAX  ]; /* " */
static ushort fd_shmem_private_cpu_idx [ FD_SHMEM_NUMA_MAX ]; /* " */

ulong fd_shmem_numa_cnt( void ) { return fd_shmem_private_numa_cnt; }
ulong fd_shmem_cpu_cnt ( void ) { return fd_shmem_private_cpu_cnt;  }

ulong
fd_shmem_numa_idx( ulong cpu_idx ) {
  if( FD_UNLIKELY( cpu_idx>=fd_shmem_private_cpu_cnt ) ) return ULONG_MAX;
  return (ulong)fd_shmem_private_numa_idx[ cpu_idx ];
}

ulong
fd_shmem_cpu_idx( ulong numa_idx ) {
  if( FD_UNLIKELY( numa_idx>=fd_shmem_private_numa_cnt ) ) return ULONG_MAX;
  return (ulong)fd_shmem_private_cpu_idx[ numa_idx ];
}

int
fd_shmem_numa_validate( void const * mem,
                        ulong        page_sz,
                        ulong        page_cnt,
                        ulong        cpu_idx ) {
  if( FD_UNLIKELY( !mem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return EINVAL;
  }

  if( FD_UNLIKELY( !fd_shmem_is_page_sz( page_sz ) ) ) {
    FD_LOG_WARNING(( "bad page_sz (%lu)", page_sz ));
    return EINVAL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)mem, page_sz ) ) ) {
    FD_LOG_WARNING(( "misaligned mem" ));
    return EINVAL;
  }

  if( FD_UNLIKELY( !((1UL<=page_cnt) & (page_cnt<=(((ulong)LONG_MAX)/page_sz))) ) ) {
    FD_LOG_WARNING(( "bad page_cnt (%lu)", page_cnt ));
    return EINVAL;
  }

  if( FD_UNLIKELY( !(cpu_idx<fd_shmem_cpu_cnt()) ) ) {
    FD_LOG_WARNING(( "bad cpu_idx (%lu)", cpu_idx ));
    return EINVAL;
  }

  ulong numa_idx = fd_shmem_numa_idx( cpu_idx );

  ulong   page = (ulong)mem;
  int     batch_status[ 512 ];
  void *  batch_page  [ 512 ];
  ulong   batch_cnt = 0UL;
  while( page_cnt ) {
    batch_page[ batch_cnt++ ] = (void *)page;
    page += page_sz;
    page_cnt--;
    if( FD_UNLIKELY( ((batch_cnt==512UL) | (!page_cnt) ) ) ) {
      if( FD_UNLIKELY( move_pages( 0, batch_cnt, batch_page, NULL, batch_status, 0 ) ) ) {
        FD_LOG_WARNING(( "move_pages query failed (%i-%s)", errno, strerror( errno ) ));
        return errno;
      }
      for( ulong batch_idx=0UL; batch_idx<batch_cnt; batch_idx++ ) {
        if( FD_UNLIKELY( batch_status[batch_idx]<0 ) ) {
          int err = -batch_status[batch_idx];
          FD_LOG_WARNING(( "page status failed (%i-%s)", err, strerror( err ) ));
          return err;
        }
        if( FD_UNLIKELY( batch_status[batch_idx]!=(int)numa_idx ) ) {
          FD_LOG_WARNING(( "page allocated to numa %i instead of numa %lu", batch_status[batch_idx], numa_idx ));
          return EFAULT;
        }
      }
      batch_cnt = 0UL;
    }
  }

  return 0;
}

/* SHMEM REGION CREATION AND DESTRUCTION ******************************/

int
fd_shmem_create( char const * name,
                 ulong        page_sz,
                 ulong        page_cnt,
                 ulong        cpu_idx,
                 ulong        mode ) {

  /* Check input args */

  if( FD_UNLIKELY( !fd_shmem_name_len( name ) ) ) { FD_LOG_WARNING(( "bad name (%s)", name ? name : "NULL" )); return EINVAL; }

  if( FD_UNLIKELY( !fd_shmem_is_page_sz( page_sz ) ) ) { FD_LOG_WARNING(( "bad page_sz (%lu)", page_sz )); return EINVAL; }

  if( FD_UNLIKELY( !((1UL<=page_cnt) & (page_cnt<=(((ulong)LONG_MAX)/page_sz))) ) ) {
    FD_LOG_WARNING(( "bad page_cnt (%lu)", page_cnt ));
    return EINVAL;
  }

  if( FD_UNLIKELY( !(cpu_idx<fd_shmem_cpu_cnt()) ) ) { FD_LOG_WARNING(( "bad cpu_idx (%lu)", cpu_idx )); return EINVAL; }

  if( FD_UNLIKELY( mode!=(ulong)(mode_t)mode ) ) { FD_LOG_WARNING(( "bad mode (0%03lo)", mode )); return EINVAL; }

  ulong sz       = page_cnt*page_sz;
  ulong numa_idx = fd_shmem_numa_idx( cpu_idx );

  /* We use the FD_SHMEM_LOCK in create just to be safe given some
     thread safety ambiguities in the documentation for some of the
     below APIs. */

  FD_SHMEM_LOCK;

  int err;
# define ERROR( cleanup ) do { err = errno; goto cleanup; } while(0)

  int    orig_mempolicy;
  ulong  orig_nodemask[ (FD_SHMEM_NUMA_MAX+63UL)/64UL ];
  ulong  nodemask[ (FD_SHMEM_NUMA_MAX+63UL)/64UL ];
  char   path[ FD_SHMEM_PRIVATE_PATH_BUF_MAX ];
  int    fd;
  void * shmem;

  /* Save this thread's numa node mempolicy and then set it to bind
     newly created memory to the numa idx corresponding to logical cpu
     cpu_idx.  This should force page allocation to be on the desired
     numa node even if triggered preemptively in the ftruncate / mmap
     because the user thread group has configured things like
     mlockall(MCL_FUTURE).  Theoretically, the mbind below should do it
     without this but the Linux kernel tends to view requests to move
     pages between numa nodes after allocation as for entertainment
     purposes only. */

  if( FD_UNLIKELY( get_mempolicy( &orig_mempolicy, orig_nodemask, FD_SHMEM_NUMA_MAX, NULL, 0UL ) ) ) {
    FD_LOG_WARNING(( "get_mempolicy failed (%i-%s)", errno, strerror( errno ) ));
    ERROR( done );
  }

  fd_memset( nodemask, 0, 8UL*((FD_SHMEM_NUMA_MAX+63UL)/64UL) );
  nodemask[ numa_idx >> 6 ] = 1UL << (numa_idx & 63UL);
  if( FD_UNLIKELY( set_mempolicy( MPOL_BIND | MPOL_F_STATIC_NODES, nodemask, FD_SHMEM_NUMA_MAX ) ) ) {
    FD_LOG_WARNING(( "set_mempolicy failed (%i-%s)", errno, strerror( errno ) ));
    ERROR( done );
  }

  /* Create the region */

  fd = open( fd_shmem_private_path( name, page_sz, path ), O_RDWR | O_CREAT | O_EXCL, (mode_t)mode );
  if( FD_UNLIKELY( fd==-1 ) ) {
    FD_LOG_WARNING(( "open(\"%s\",O_RDWR|O_CREAT|O_EXCL,0%03lo) failed (%i-%s)", path, mode, errno, strerror( errno ) ));
    ERROR( restore );
  }

  /* Size the region */

  if( FD_UNLIKELY( ftruncate( fd, (off_t)sz ) ) ) {
    FD_LOG_WARNING(( "ftruncate(\"%s\",%lu KiB) failed (%i-%s)", path, sz>>10, errno, strerror( errno ) ));
    ERROR( close );
  }

  /* Map the region into our address space. */

  shmem = mmap( NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0);
  if( FD_UNLIKELY( shmem==MAP_FAILED ) ) {
    FD_LOG_WARNING(( "mmap(NULL,%lu KiB,PROT_READ|PROT_WRITE,MAP_SHARED,\"%s\",0) failed (%i-%s)",
                     sz>>10, path, errno, strerror( errno ) ));
    ERROR( close );
  }

  /* Validate the mapping */

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)shmem, page_sz ) ) ) {
    FD_LOG_WARNING(( "misaligned memory mapping for \"%s\"\n\t"
                     "This thread group's hugetlbfs mount path (--shmem-path / FD_SHMEM_PATH):\n\t"
                     "\t%s\n\t"
                     "has probably been corrupted and needs to be redone.\n\t"
                     "See 'bin/fd_shmem_cfg help' for more information.",
                     path, fd_shmem_private_base ));
    errno = EFAULT; /* ENOMEM is arguable */
    ERROR( unmap );
  }

  /* If a mempolicy has been set and the numa_idx node does not have
     sufficient pages to back the mapping, touching the memory will
     trigger a a SIGBUS when it touches the first part of the mapping
     for which there are no pages.  Unfortunately, mmap will only error
     if there are insufficient pages across all NUMA nodes (even if
     using mlockall( MCL_FUTURE ) or passing MAP_POPULATE), so we need
     to check that the mapping can be backed without handling signals.

     So we mlock the region to force the region to be backed by pages
     now.  The region should be backed by page_sz pages (thanks to the
     hugetlbfs configuration) and should be on the correct NUMA node
     (thanks to the mempolicy above).  Specifically, mlock will error
     with ENOMEM if there were insufficient pages available.  mlock
     guarantees that if it succeeds, the mapping has been fully backed
     by pages and these pages will remain resident in DRAM at least
     until the mapping is closed.  We can then proceed as usual without
     the risk of meeting SIGBUS or its friends. */

  if( FD_UNLIKELY( fd_mlock( shmem, sz ) ) ) {
    FD_LOG_WARNING(( "mlock(\"%s\",%lu KiB) failed (%i-%s)", path, sz>>10, errno, strerror( errno ) ));
    ERROR( unmap );
  }

  /* At this point all pages should be allocated on the right NUMA node
     and resident in DRAM.  But in the spirit of not trusting Linux to
     get this right robustly, we continue with touching pages from
     cpu_idx. */

  /* FIXME: NUMA TOUCH HERE (ALSO WOULD A LOCAL TOUCH WORK GIVEN THE
     MEMPOLICY DONE ABOVE?) */

  /* mbind the memory region to this numa node to nominally stay put
     after we unmap it. */

  /* Just in case set_mempolicy clobbered it */
  fd_memset( nodemask, 0, 8UL*((FD_SHMEM_NUMA_MAX+63UL)/64UL) );
  nodemask[ numa_idx >> 6 ] = 1UL << (numa_idx & 63UL);
  if( FD_UNLIKELY( mbind( shmem, sz, MPOL_BIND, nodemask, FD_SHMEM_NUMA_MAX, MPOL_MF_MOVE | MPOL_MF_STRICT ) ) ) {
    FD_LOG_WARNING(( "mbind(\"%s\",%lu KiB,MPOL_BIND,1UL<<%lu,MPOL_MF_MOVE|MPOL_MF_STRICT) failed (%i-%s)",
                     path, sz>>10, numa_idx, errno, strerror( errno ) ));
    ERROR( unmap );
  }

  /* And since the mbind still often will ignore requests, we double
     check that the pages are in the right place. */

  err = fd_shmem_numa_validate( shmem, page_sz, page_cnt, cpu_idx ); /* logs details */
  if( FD_UNLIKELY( err ) )
    FD_LOG_WARNING(( "mmap(NULL,%lu KiB,PROT_READ|PROT_WRITE,MAP_SHARED,\"%s\",0) numa binding failed (%i-%s)",
                     sz>>10, path, err, strerror( err ) ));

# undef ERROR

unmap:
  if( FD_UNLIKELY( munmap( shmem, sz ) ) )
    FD_LOG_WARNING(( "munmap(\"%s\",%lu KiB) failed (%i-%s); attempting to continue", path, sz>>10, errno, strerror( errno ) ));

close:
  if( FD_UNLIKELY( err ) && FD_UNLIKELY( unlink( path ) ) )
    FD_LOG_WARNING(( "unlink(\"%s\") failed (%i-%s)", path, errno, strerror( errno ) )); /* Don't log "attempting ..." */
  if( FD_UNLIKELY( close( fd ) ) )
    FD_LOG_WARNING(( "close(\"%s\") failed (%i-%s); attempting to continue", path, errno, strerror( errno ) ));

restore:
  if( FD_UNLIKELY( set_mempolicy( orig_mempolicy, orig_nodemask, FD_SHMEM_NUMA_MAX ) ) )
    FD_LOG_WARNING(( "set_mempolicy failed (%i-%s); attempting to continue", errno, strerror( errno ) ));

done:
  FD_SHMEM_UNLOCK;
  return err;
}

int
fd_shmem_unlink( char const * name,
                 ulong        page_sz ) {
  char path[ FD_SHMEM_PRIVATE_PATH_BUF_MAX ];

  /* Check input args */

  if( FD_UNLIKELY( !fd_shmem_name_len( name ) ) ) { FD_LOG_WARNING(( "bad name (%s)", name ? name : "NULL" )); return EINVAL; }

  if( FD_UNLIKELY( !fd_shmem_is_page_sz( page_sz ) ) ) { FD_LOG_WARNING(( "bad page_sz (%lu)", page_sz )); return EINVAL; }

  /* Unlink the name */

  if( FD_UNLIKELY( unlink( fd_shmem_private_path( name, page_sz, path ) ) ) ) {
    FD_LOG_WARNING(( "unlink(\"%s\") failed (%i-%s)", path, errno, strerror( errno ) ));
    return errno;
  }

  return 0;
}

int
fd_shmem_info( char const *      name,
               ulong             page_sz,
               fd_shmem_info_t * opt_info ) {

  if( FD_UNLIKELY( !fd_shmem_name_len( name ) ) ) { FD_LOG_WARNING(( "bad name (%s)", name ? name : "NULL" )); return EINVAL; }

  if( !page_sz ) {
    if( !fd_shmem_info( name, FD_SHMEM_GIGANTIC_PAGE_SZ, opt_info ) ) return 0;
    if( !fd_shmem_info( name, FD_SHMEM_HUGE_PAGE_SZ,     opt_info ) ) return 0;
    if( !fd_shmem_info( name, FD_SHMEM_NORMAL_PAGE_SZ,   opt_info ) ) return 0;
    return ENOENT;
  }

  if( FD_UNLIKELY( !fd_shmem_is_page_sz( page_sz ) ) ) { FD_LOG_WARNING(( "bad page_sz (%lu)", page_sz )); return EINVAL; }

  char path[ FD_SHMEM_PRIVATE_PATH_BUF_MAX ];
  int  fd = open( fd_shmem_private_path( name, page_sz, path ), O_RDONLY, (mode_t)0 );
  if( FD_UNLIKELY( fd==-1 ) ) return errno; /* no logging here as this might be an existence check */

  struct stat stat[1];
  if( FD_UNLIKELY( fstat( fd, stat ) ) ) {
    FD_LOG_WARNING(( "fstat failed (%i-%s)", errno, strerror( errno ) ));
    int err = errno;
    if( FD_UNLIKELY( close( fd ) ) )
      FD_LOG_WARNING(( "close(\"%s\") failed (%i-%s); attempting to continue", path, errno, strerror( errno ) ));
    return err;
  }

  ulong sz = (ulong)stat->st_size;
  if( FD_UNLIKELY( !fd_ulong_is_aligned( sz, page_sz ) ) ) {
    FD_LOG_WARNING(( "\"%s\" size (%lu) not a page size (%lu) multiple\n\t"
                     "This thread group's hugetlbfs mount path (--shmem-path / FD_SHMEM_PATH):\n\t"
                     "\t%s\n\t"
                     "has probably been corrupted and needs to be redone.\n\t"
                     "See 'bin/fd_shmem_cfg help' for more information.",
                     path, sz, page_sz, fd_shmem_private_base ));
    if( FD_UNLIKELY( close( fd ) ) )
      FD_LOG_WARNING(( "close(\"%s\") failed (%i-%s); attempting to continue", path, errno, strerror( errno ) ));
    return EFAULT;
  }
  ulong page_cnt = sz / page_sz;

  if( FD_UNLIKELY( close( fd ) ) )
    FD_LOG_WARNING(( "close(\"%s\") failed (%i-%s); attempting to continue", path, errno, strerror( errno ) ));

  if( opt_info ) {
    opt_info->page_sz  = page_sz;
    opt_info->page_cnt = page_cnt;
  }
  return 0;
}

/* RAW PAGE ALLOCATION APIS *******************************************/

void *
fd_shmem_acquire( ulong page_sz,
                  ulong page_cnt,
                  ulong cpu_idx ) {

  if( FD_UNLIKELY( !fd_shmem_is_page_sz( page_sz ) ) ) {
    FD_LOG_WARNING(( "bad page_sz (%lu)", page_sz ));
    return NULL;
  }

  if( FD_UNLIKELY( !((1UL<=page_cnt) & (page_cnt<=(((ulong)LONG_MAX)/page_sz))) ) ) {
    FD_LOG_WARNING(( "bad page_cnt (%lu)", page_cnt ));
    return NULL;
  }

  if( FD_UNLIKELY( !(cpu_idx<fd_shmem_cpu_cnt()) ) ) {
    FD_LOG_WARNING(( "bad cpu_idx (%lu)", cpu_idx ));
    return NULL;
  }

  ulong sz       = page_cnt*page_sz;
  ulong numa_idx = fd_shmem_numa_idx( cpu_idx );

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if( page_sz==FD_SHMEM_HUGE_PAGE_SZ     ) flags |= MAP_HUGETLB | MAP_HUGE_2MB;
  if( page_sz==FD_SHMEM_GIGANTIC_PAGE_SZ ) flags |= MAP_HUGETLB | MAP_HUGE_1GB;

  /* See fd_shmem_create for details on the locking, mempolicy
     and what not tricks */

  FD_SHMEM_LOCK;

  int err;
# define ERROR( cleanup ) do { err = errno; goto cleanup; } while(0)

  int    orig_mempolicy;
  ulong  orig_nodemask[ (FD_SHMEM_NUMA_MAX+63UL)/64UL ];
  ulong  nodemask[ (FD_SHMEM_NUMA_MAX+63UL)/64UL ];
  void * mem = NULL;

  if( FD_UNLIKELY( get_mempolicy( &orig_mempolicy, orig_nodemask, FD_SHMEM_NUMA_MAX, NULL, 0UL ) ) ) {
    FD_LOG_WARNING(( "get_mempolicy failed (%i-%s)", errno, strerror( errno ) ));
    ERROR( done );
  }

  fd_memset( nodemask, 0, 8UL*((FD_SHMEM_NUMA_MAX+63UL)/64UL) );
  nodemask[ numa_idx >> 6 ] = 1UL << (numa_idx & 63UL);
  if( FD_UNLIKELY( set_mempolicy( MPOL_BIND | MPOL_F_STATIC_NODES, nodemask, FD_SHMEM_NUMA_MAX ) ) ) {
    FD_LOG_WARNING(( "set_mempolicy failed (%i-%s)", errno, strerror( errno ) ));
    ERROR( done );
  }

  mem = mmap( NULL, sz, PROT_READ | PROT_WRITE, flags, -1, (off_t)0);
  if( FD_UNLIKELY( mem==MAP_FAILED ) ) {
    FD_LOG_WARNING(( "mmap(NULL,%lu KiB,PROT_READ|PROT_WRITE,%x,-1,0) failed (%i-%s)", sz>>10, flags, errno, strerror( errno ) ));
    ERROR( restore );
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)mem, page_sz ) ) ) {
    FD_LOG_WARNING(( "mmap(NULL,%lu KiB,PROT_READ|PROT_WRITE,%x,-1,0) misaligned", sz>>10, flags ));
    errno = EFAULT; /* ENOMEM is arguable */
    ERROR( unmap );
  }

  if( FD_UNLIKELY( fd_mlock( mem, sz ) ) ) {
    FD_LOG_WARNING(( "mlock(anon,%lu KiB) failed (%i-%s)", sz>>10, errno, strerror( errno ) ));
    ERROR( unmap );
  }

  /* FIXME: NUMA TOUCH HERE (ALSO WOULD A LOCAL TOUCH WORK GIVEN THE
     MEMPOLICY DONE ABOVE?) */

  /* Just in case set_mempolicy clobbered it */
  fd_memset( nodemask, 0, 8UL*((FD_SHMEM_NUMA_MAX+63UL)/64UL) );
  nodemask[ numa_idx >> 6 ] = 1UL << (numa_idx & 63UL);
  if( FD_UNLIKELY( mbind( mem, sz, MPOL_BIND, nodemask, FD_SHMEM_NUMA_MAX, MPOL_MF_MOVE | MPOL_MF_STRICT ) ) ) {
    FD_LOG_WARNING(( "mbind(anon,%lu KiB,MPOL_BIND,1UL<<%lu,MPOL_MF_MOVE|MPOL_MF_STRICT) failed (%i-%s)",
                     sz>>10, numa_idx, errno, strerror( errno ) ));
    ERROR( unmap );
  }

  err = fd_shmem_numa_validate( mem, page_sz, page_cnt, numa_idx ); /* logs details */
  if( FD_UNLIKELY( err ) )
    FD_LOG_WARNING(( "mmap(NULL,%lu KiB,PROT_READ|PROT_WRITE,%x,-1,0) numa binding failed (%i-%s)",
                     sz>>10, flags, err, strerror( err ) ));

# undef ERROR

unmap:
  if( FD_UNLIKELY( err ) && FD_UNLIKELY( munmap( mem, sz ) ) )
    FD_LOG_WARNING(( "munmap(anon,%lu KiB) failed (%i-%s); attempting to continue", sz>>10, errno, strerror( errno ) ));

restore:
  if( FD_UNLIKELY( set_mempolicy( orig_mempolicy, orig_nodemask, FD_SHMEM_NUMA_MAX ) ) )
    FD_LOG_WARNING(( "set_mempolicy failed (%i-%s); attempting to continue", errno, strerror( errno ) ));

done:
  FD_SHMEM_UNLOCK;
  return err ? NULL : mem;
}

void
fd_shmem_release( void * mem,
                  ulong  page_sz,
                  ulong  page_cnt ) {
  if( FD_UNLIKELY( !mem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return;
  }

  if( FD_UNLIKELY( !fd_shmem_is_page_sz( page_sz ) ) ) {
    FD_LOG_WARNING(( "bad page_sz (%lu)", page_sz ));
    return;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)mem, page_sz ) ) ) {
    FD_LOG_WARNING(( "misaligned mem" ));
    return;
  }

  if( FD_UNLIKELY( !((1UL<=page_cnt) & (page_cnt<=(((ulong)LONG_MAX)/page_sz))) ) ) {
    FD_LOG_WARNING(( "bad page_cnt (%lu)", page_cnt ));
    return;
  }

  ulong sz = page_sz*page_cnt;

  if( FD_UNLIKELY( munmap( mem, sz ) ) )
    FD_LOG_WARNING(( "munmap(anon,%lu KiB) failed (%i-%s); attempting to continue", sz>>10, errno, strerror( errno ) ));
}

/* SHMEM PARSING APIS *************************************************/

ulong
fd_shmem_name_len( char const * name ) {
  if( FD_UNLIKELY( !name ) ) return 0UL; /* NULL name */

  ulong len = 0UL;
  while( FD_LIKELY( len<FD_SHMEM_NAME_MAX ) ) {
    char c = name[len];
    if( FD_UNLIKELY( !c ) ) break;
    if( FD_UNLIKELY( !( (!!isalnum( c )) | ((len>0UL) & ((c=='_') | (c=='-') | (c=='.'))) ) ) ) return 0UL; /* Bad character */
    len++;
  }

  if( FD_UNLIKELY( !len                   ) ) return 0UL; /* Name too short (empty string) */
  if( FD_UNLIKELY( len>=FD_SHMEM_NAME_MAX ) ) return 0UL; /* Name too long */
  return len;
}

int
fd_cstr_to_shmem_lg_page_sz( char const * cstr ) {
  if( !cstr ) return FD_SHMEM_UNKNOWN_LG_PAGE_SZ;

  if( !fd_cstr_casecmp( cstr, "normal"   ) ) return FD_SHMEM_NORMAL_LG_PAGE_SZ;
  if( !fd_cstr_casecmp( cstr, "huge"     ) ) return FD_SHMEM_HUGE_LG_PAGE_SZ;
  if( !fd_cstr_casecmp( cstr, "gigantic" ) ) return FD_SHMEM_GIGANTIC_LG_PAGE_SZ;

  int i = fd_cstr_to_int( cstr );
  if( i==FD_SHMEM_NORMAL_LG_PAGE_SZ   ) return FD_SHMEM_NORMAL_LG_PAGE_SZ;
  if( i==FD_SHMEM_HUGE_LG_PAGE_SZ     ) return FD_SHMEM_HUGE_LG_PAGE_SZ;
  if( i==FD_SHMEM_GIGANTIC_LG_PAGE_SZ ) return FD_SHMEM_GIGANTIC_LG_PAGE_SZ;

  return FD_SHMEM_UNKNOWN_LG_PAGE_SZ;
}

char const *
fd_shmem_lg_page_sz_to_cstr( int lg_page_sz ) {
  switch( lg_page_sz ) {
  case FD_SHMEM_NORMAL_LG_PAGE_SZ:   return "normal";
  case FD_SHMEM_HUGE_LG_PAGE_SZ:     return "huge";
  case FD_SHMEM_GIGANTIC_LG_PAGE_SZ: return "gigantic";
  default:                           break;
  }
  return "unknown";
}

ulong
fd_cstr_to_shmem_page_sz( char const * cstr ) {
  if( !cstr ) return FD_SHMEM_UNKNOWN_PAGE_SZ;

  if( !fd_cstr_casecmp( cstr, "normal"   ) ) return FD_SHMEM_NORMAL_PAGE_SZ;
  if( !fd_cstr_casecmp( cstr, "huge"     ) ) return FD_SHMEM_HUGE_PAGE_SZ;
  if( !fd_cstr_casecmp( cstr, "gigantic" ) ) return FD_SHMEM_GIGANTIC_PAGE_SZ;

  ulong u = fd_cstr_to_ulong( cstr );
  if( u==FD_SHMEM_NORMAL_PAGE_SZ   ) return FD_SHMEM_NORMAL_PAGE_SZ;
  if( u==FD_SHMEM_HUGE_PAGE_SZ     ) return FD_SHMEM_HUGE_PAGE_SZ;
  if( u==FD_SHMEM_GIGANTIC_PAGE_SZ ) return FD_SHMEM_GIGANTIC_PAGE_SZ;

  return FD_SHMEM_UNKNOWN_PAGE_SZ;
}

char const *
fd_shmem_page_sz_to_cstr( ulong page_sz ) {
  switch( page_sz ) {
  case FD_SHMEM_NORMAL_PAGE_SZ:   return "normal";
  case FD_SHMEM_HUGE_PAGE_SZ:     return "huge";
  case FD_SHMEM_GIGANTIC_PAGE_SZ: return "gigantic";
  default:                        break;
  }
  return "unknown";
}

/* BOOT/HALT APIs *****************************************************/

void
fd_shmem_private_boot( int *    pargc,
                       char *** pargv ) {
  FD_LOG_INFO(( "fd_shmem: booting" ));

  /* Determine the numa topology this thread group's host */

  if( FD_UNLIKELY( numa_available()==-1 ) ) FD_LOG_ERR(( "fd_shmem: numa available failed" ));

  int numa_cnt = numa_num_configured_nodes();
  if( FD_UNLIKELY( !(1<=numa_cnt && numa_cnt<=(int)FD_SHMEM_NUMA_MAX) ) )
    FD_LOG_ERR(( "fd_shmem: unexpected numa_cnt %i (expected in [1,%lu])", numa_cnt, FD_SHMEM_NUMA_MAX ));
  fd_shmem_private_numa_cnt = (ulong)numa_cnt;

  int cpu_cnt = numa_num_configured_cpus();
  if( FD_UNLIKELY( !(1<=cpu_cnt && cpu_cnt<=(int)FD_SHMEM_CPU_MAX) ) )
    FD_LOG_ERR(( "fd_shmem: unexpected cpu_cnt %i (expected in [1,%lu])", cpu_cnt, FD_SHMEM_CPU_MAX ));
  fd_shmem_private_cpu_cnt = (ulong)cpu_cnt;

  for( int cpu_idx=cpu_cnt-1; cpu_idx>=0; cpu_idx-- ) {
    int numa_idx = numa_node_of_cpu( cpu_idx );
    if( FD_UNLIKELY( !((0<=numa_idx) & (numa_idx<(int)FD_SHMEM_NUMA_MAX)) ) )
      FD_LOG_ERR(( "fd_shmem: unexpected numa idx (%i) for cpu idx %i (%i-%s)", numa_idx, cpu_idx, errno, strerror( errno ) ));
    fd_shmem_private_numa_idx[ cpu_idx  ] = (ushort)numa_idx;
    fd_shmem_private_cpu_idx [ numa_idx ] = (ushort)cpu_idx;
  }

  /* Determine the shared memory domain for this thread group */

  char const * shmem_base = fd_env_strip_cmdline_cstr( pargc, pargv, "--shmem-path", "FD_SHMEM_PATH", "/mnt/.fd" );

  ulong len = strlen( shmem_base );
  while( (len>1UL) && (shmem_base[len-1UL]=='/') ) len--; /* lop off any trailing slashes */
  if( FD_UNLIKELY( !len ) ) FD_LOG_ERR(( "Too short --shmem-base" ));
  if( FD_UNLIKELY( len>=FD_SHMEM_PRIVATE_BASE_MAX ) ) FD_LOG_ERR(( "Too long --shmem-base" ));
  fd_memcpy( fd_shmem_private_base, shmem_base, len );
  fd_shmem_private_base[len] = '\0';
  fd_shmem_private_base_len = (ulong)len;

  /* At this point, shared memory is online */

  FD_LOG_INFO(( "fd_shmem: --shmem-path %s", fd_shmem_private_base ));
  FD_LOG_INFO(( "fd_shmem: boot success" ));
}

void
fd_shmem_private_halt( void ) {
  FD_LOG_INFO(( "fd_shmem: halting" ));

  /* At this point, shared memory is offline */

  fd_shmem_private_numa_cnt = 0;
  fd_shmem_private_cpu_cnt  = 0;
  fd_memset( fd_shmem_private_numa_idx, 0, FD_SHMEM_CPU_MAX );

  fd_shmem_private_base[0] = '\0';
  fd_shmem_private_base_len = 0UL;

  FD_LOG_INFO(( "fd_shmem: halt success" ));
}

#else /* unhosted or not x86 */

void
fd_shmem_private_boot( int *    pargc,
                       char *** pargv ) {
  FD_LOG_INFO(( "fd_shmem: booting" ));

  /* Strip the command line even though ignored to make environemnt
     parsing identical to downstream regardless of platform. */

  (void)fd_env_strip_cmdline_cstr( pargc, pargv, "--shmem-path", "FD_SHMEM_PATH", "/mnt/.fd" );

  FD_LOG_INFO(( "fd_shmem: --shmem-path (ignored)" ));
  FD_LOG_INFO(( "fd_shmem: boot success" ));
}

void
fd_shmem_private_halt( void ) {
  FD_LOG_INFO(( "fd_shmem: halting" ));
  FD_LOG_INFO(( "fd_shmem: halt success" ));
}

#endif

