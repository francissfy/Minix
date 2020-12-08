# build and test
clang -o test main.c

for i in 1 2 3 4 5 6 7 8;do
./test lottery &
done
wait