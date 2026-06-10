#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>

/* Instructions préprocesseur pour différencier les architectures linux de windows qui ont des imports différents. */
#ifdef _WIN32
    #include <io.h>
#else
    #include <sys/ioctl.h>
#endif


/** Méthode helper pour initialiser les matrices avec
 * des coefficients aléatoires.
 * @param matrixSize La taille des matrices
 */
void initMatrices(int matrixSize, int *matrixA, int *matrixB) {
    for (int i = 0; i < matrixSize; i++) {
        for (int j = 0; j < matrixSize; j++) {
            matrixA[i * matrixSize + j] = rand();
            matrixB[i * matrixSize + j] = rand();
        }
    }
}


/** Méthode helper pour afficher une matrice à la console.
 * @param matrix La matrice a afficher
 * @param size La taille de la matrice
*/
void printMatrix(int* matrix, int size) {
    for (int k = 0; k < size * size; k++) {
        printf("%d\t", matrix[k]);
        if (k != 0 && (k % size) == (size - 1)) {
            printf("\n\n");
        }
    }
}


/** Fonction helper permettant de calculer le produit C = AxB et de
 * mettre le résultat dans C.
 * @param matrixSize La taille des matrices
 * @param matrixA La matrice A
 * @param matrixB La matrice B
 * @param matrixC La matrice C à calculer
 */
__global__
void computeMatrices(int matrixSize, int *matrixA, int *matrixB, int *matrixC) {
    /* Numéro du thread actuel. */
    int index = blockIdx.x * blockDim.x + threadIdx.x;
    int totalCoefficients = matrixSize * matrixSize;

    if (index >= totalCoefficients) {
        return;
    }

    /* A quelle ligne de la matrice le coefficient de ce thread se trouve t'il. */
    int row = index / matrixSize;
    /* A quelle colonne de la matrice le coefficient de ce thread se trouve t'il. */
    int col = index % matrixSize;

    int coefficient = 0;

    /* A i et j fixé, on parcours la ligne de A et la colonne de B. */
    for (int k = 0; k < matrixSize; k++) {
        coefficient += matrixA[row * matrixSize + k] * matrixB[k * matrixSize + col];
    }

    matrixC[index] = coefficient;
}


int main(void) {
    /* Initialisation des variables. */
    int matrixSize = 12;
    int *matrixA;
    int *matrixB;
    int *matrixC;

    cudaMallocManaged(&matrixA, matrixSize * matrixSize * sizeof(int));
    cudaMallocManaged(&matrixB, matrixSize * matrixSize * sizeof(int));

    initMatrices(matrixSize, matrixA, matrixB);

    cudaMallocManaged(&matrixC, matrixSize * matrixSize * sizeof(int));
    memset(matrixC, 0, matrixSize * matrixSize * sizeof(int));

    int blockSize = 256;
    int numBlocks = (matrixSize * matrixSize + blockSize - 1) / blockSize;
        
    /* On copie en VRAM les données utiles. */
    cudaMemPrefetchAsync(matrixA, matrixSize * matrixSize * sizeof(int), 0, 0);
    cudaMemPrefetchAsync(matrixB, matrixSize * matrixSize * sizeof(int), 0, 0);
    cudaMemPrefetchAsync(matrixC, matrixSize * matrixSize * sizeof(int), 0, 0);

    computeMatrices<<<numBlocks, blockSize>>>(matrixSize, matrixA, matrixB, matrixC);

    /* On attends que le GPU ait fini les tâches assignées. */
    cudaDeviceSynchronize();

    printMatrix(matrixC, matrixSize);
}