#include "../fd_ballet.h"
#include "fd_sha512_test_vector.c"

#ifdef HAS_CAVP_TEST_VECTORS
#include "cavp/sha512_short.inc"
#include "cavp/sha512_long.inc"
#endif

FD_STATIC_ASSERT( FD_SHA512_ALIGN    ==128UL, unit_test );
FD_STATIC_ASSERT( FD_SHA512_FOOTPRINT==256UL, unit_test );

FD_STATIC_ASSERT( FD_SHA512_ALIGN    ==alignof(fd_sha512_t), unit_test );
FD_STATIC_ASSERT( FD_SHA512_FOOTPRINT==sizeof (fd_sha512_t), unit_test );

FD_STATIC_ASSERT( FD_SHA512_LG_HASH_SZ==6, unit_test );
FD_STATIC_ASSERT( FD_SHA512_HASH_SZ==64UL, unit_test );

static void
test_sha512_vectors( fd_sha512_test_vector_t const * vec,
                     fd_sha512_t *                   sha,
                     fd_rng_t *                      rng ) {
  uchar hash[ 64 ] __attribute__((aligned(64)));

  for( ; vec->msg; vec++ ) {
    char const *  msg      = vec->msg;
    ulong         sz       = vec->sz;
    uchar const * expected = vec->hash;

    /* test single shot hashing */

    FD_TEST( fd_sha512_init( sha )==sha );
    FD_TEST( fd_sha512_append( sha, msg, sz )==sha );
    FD_TEST( fd_sha512_fini( sha, hash )==hash );
    if( FD_UNLIKELY( memcmp( hash, expected, 64UL ) ) )
      FD_LOG_ERR(( "FAIL (sz %lu)"
                   "\n\tGot"
                   "\n\t\t" FD_LOG_HEX16_FMT "  " FD_LOG_HEX16_FMT
                   "\n\t\t" FD_LOG_HEX16_FMT "  " FD_LOG_HEX16_FMT
                   "\n\tExpected"
                   "\n\t\t" FD_LOG_HEX16_FMT "  " FD_LOG_HEX16_FMT
                   "\n\t\t" FD_LOG_HEX16_FMT "  " FD_LOG_HEX16_FMT, sz,
                   FD_LOG_HEX16_FMT_ARGS(     hash    ), FD_LOG_HEX16_FMT_ARGS(     hash+16 ),
                   FD_LOG_HEX16_FMT_ARGS(     hash+32 ), FD_LOG_HEX16_FMT_ARGS(     hash+48 ),
                   FD_LOG_HEX16_FMT_ARGS( expected    ), FD_LOG_HEX16_FMT_ARGS( expected+16 ),
                   FD_LOG_HEX16_FMT_ARGS( expected+32 ), FD_LOG_HEX16_FMT_ARGS( expected+48 ) ));

    /* test incremental hashing */

    memset( hash, 0, 64UL );
    FD_TEST( fd_sha512_init( sha )==sha );

    char const * nxt = msg;
    ulong        rem = sz;
    while( rem ) {
      ulong nxt_sz = fd_ulong_min( rem, fd_rng_ulong_roll( rng, sz+1UL ) );
      FD_TEST( fd_sha512_append( sha, nxt, nxt_sz )==sha );
      nxt += nxt_sz;
      rem -= nxt_sz;
      if( fd_rng_uint( rng ) & 1UL ) FD_TEST( fd_sha512_append( sha, NULL, 0UL )==sha ); /* test zero append too */
    }

    FD_TEST( fd_sha512_fini( sha, hash )==hash );

    if( FD_UNLIKELY( memcmp( hash, expected, 64UL ) ) )
      FD_LOG_ERR(( "FAIL (sz %lu)"
                   "\n\tGot"
                   "\n\t\t" FD_LOG_HEX16_FMT "  " FD_LOG_HEX16_FMT
                   "\n\t\t" FD_LOG_HEX16_FMT "  " FD_LOG_HEX16_FMT
                   "\n\tExpected"
                   "\n\t\t" FD_LOG_HEX16_FMT "  " FD_LOG_HEX16_FMT
                   "\n\t\t" FD_LOG_HEX16_FMT "  " FD_LOG_HEX16_FMT, sz,
                   FD_LOG_HEX16_FMT_ARGS(     hash    ), FD_LOG_HEX16_FMT_ARGS(     hash+16 ),
                   FD_LOG_HEX16_FMT_ARGS(     hash+32 ), FD_LOG_HEX16_FMT_ARGS(     hash+48 ),
                   FD_LOG_HEX16_FMT_ARGS( expected    ), FD_LOG_HEX16_FMT_ARGS( expected+16 ),
                   FD_LOG_HEX16_FMT_ARGS( expected+32 ), FD_LOG_HEX16_FMT_ARGS( expected+48 ) ));
  }
}

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  fd_rng_t _rng[1]; fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, 0U, 0UL ) );

  FD_TEST( fd_sha512_align    ()==FD_SHA512_ALIGN     );
  FD_TEST( fd_sha512_footprint()==FD_SHA512_FOOTPRINT );

  fd_sha512_t   mem[1];
  void *        obj = fd_sha512_new ( mem ); FD_TEST( obj );
  fd_sha512_t * sha = fd_sha512_join( obj ); FD_TEST( sha );

  /* Test random vectors */
  test_sha512_vectors( fd_sha512_test_vector, sha, rng );
  FD_LOG_NOTICE(( "OK: Random vectors" ));

# ifdef HAS_CAVP_TEST_VECTORS
  /* Test NIST CAVP message fixtures */
  test_sha512_vectors( cavp_sha512_short, sha, rng );
  FD_LOG_NOTICE(( "OK: CAVP SHA512ShortMsg.rsp" ));
  test_sha512_vectors( cavp_sha512_long,  sha, rng );
  FD_LOG_NOTICE(( "OK: CAVP SHA512LongMsg.rsp" ));
# endif

  /* do a quick benchmark of sha-512 to UDP payloads of MTU Ethernet
     packets on UDP/IP4/VLAN/Ethernet */

# define SZ (1472UL)
  uchar buf[ SZ ] __attribute__((aligned(64)));
  for( ulong b=0UL; b<SZ; b++ ) buf[b] = fd_rng_uchar( rng );

  uchar hash[ 64 ] __attribute__((aligned(64)));

  /* warmup */
  ulong iter = 10000UL;
  long dt = fd_log_wallclock();
  for( ulong rem=iter; rem; rem-- ) fd_sha512_fini( fd_sha512_append( fd_sha512_init( sha ), buf, SZ ), hash );
  dt = fd_log_wallclock() - dt;

  /* for real */
  iter = 100000UL;
  dt = fd_log_wallclock();
  for( ulong rem=iter; rem; rem-- ) fd_sha512_fini( fd_sha512_append( fd_sha512_init( sha ), buf, SZ ), hash );
  dt = fd_log_wallclock() - dt;

  FD_LOG_NOTICE(( "~%.3f Gbps Ethernet equiv throughput per core", (double)(((float)(8UL*(70UL+SZ)*iter))/((float)dt)) ));
# undef SZ

  /* clean up */

  FD_TEST( fd_sha512_leave ( sha )==obj );
  FD_TEST( fd_sha512_delete( obj )==mem );
  fd_rng_delete( fd_rng_leave( rng ) );
  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}

