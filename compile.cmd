gcc -Wall -Wextra -Wpedantic -Wshadow orchestrator/orchestrator.c -l"ws2_32" -o orchestrator/orchestrator
gcc -Wall -Wextra -Wpedantic -Wshadow client/client.c -l"ws2_32" -o client/client