#!/bin/bash
#### GAP Benchmarks

########Twitter
#bfs
# valgrind --tool=exp-bbv --interval-size=1000000000 --bb-out-file=bfs-twitter-bb.out ../gapbs/bfs -f ../gapbs/benchmark/graphs/twitter.sg -n64 > ../gapbs/benchmark/out/bfs-twitter.out

#pr
# valgrind --tool=exp-bbv --interval-size=1000000000 --bb-out-file=pr-twitter-bb.out ../gapbs/pr -f ../gapbs/benchmark/graphs/twitter.sg -i1000 -t1e-4 -n16 > ../gapbs/benchmark/out/pr-twitter.out

#cc
# valgrind --tool=exp-bbv --interval-size=1000000000 --bb-out-file=./simpoints/cc-twitter-bb.out ../gapbs/cc -f ../gapbs/benchmark/graphs/twitter.sg -n16 > ../gapbs/benchmark/out/cc-twitter.out

#bc
# valgrind --tool=exp-bbv --interval-size=1000000000 --bb-out-file=./simpoints/bc-twitter-bb.out ../gapbs/bc -f ../gapbs/benchmark/graphs/twitter.sg -i4 -n16 > ../gapbs/benchmark/out/bc-twitter.out

########Web
#bfs
# valgrind --tool=exp-bbv --interval-size=1000000000 --bb-out-file=./simpoints/bfs-web-bb.out ../gapbs/bfs -f ../gapbs/benchmark/graphs/web.sg -n64 > ../gapbs/benchmark/out/bfs-web.out

#pr
# valgrind --tool=exp-bbv --interval-size=1000000000 --bb-out-file=./simpoints/pr-web-bb.out ../gapbs/pr -f ../gapbs/benchmark/graphs/web.sg -i1000 -t1e-4 -n16 > ../gapbs/benchmark/out/pr-web.out

#cc
# valgrind --tool=exp-bbv --interval-size=1000000000 --bb-out-file=./simpoints/cc-web-bb.out ../gapbs/cc -f ../gapbs/benchmark/graphs/web.sg -n16 > ../gapbs/benchmark/out/cc-web.out

#bc
valgrind --tool=exp-bbv --interval-size=1000000000 --bb-out-file=./simpoints/bc-web-bb.out ../gapbs/bc -f ../gapbs/benchmark/graphs/web.sg -i4 -n16 > ../gapbs/benchmark/out/bc-web.out
