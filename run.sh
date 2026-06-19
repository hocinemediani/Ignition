rm worker.c
wget http://147.127.121.93:8080/worker.c
gcc -Wall -Wextra worker.c -o worker
./worker