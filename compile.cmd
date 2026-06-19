set sortie=%~n1".exe"
gcc -Wall -Wextra -Wpedantic -Wshadow %1 -l"ws2_32" -o %sortie%