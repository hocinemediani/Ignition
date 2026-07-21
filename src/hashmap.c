#include "hashmap.h"

/** Initialise une hash map et la renvoie avec comme capacité initiale
 * `initialCapacity`.
 * @param initialCapacity La capacité initiale de la hash map souhaitée
 * @return Un pointeur vers la structure hash map créée
*/
struct hashMap *initializeHashMap(int initialCapacity) {
    struct hashMap *map = malloc(sizeof(struct hashMap));
    map->size = 0;
    map->capacity = initialCapacity;
    map->hashTable = malloc(map->capacity * sizeof(struct node *));
    memset(map->hashTable, 0, map->capacity * sizeof(struct node *));
    return map;
}


/** Renvoie la valeur de la clef hashée pour une clef donnée `key`.
 * Le calcul du hash se fait en additionnant les représentations ASCII
 * des caractères, modulo la capacité de `map`.
 * @param key La clef dont on calcule le hash
 * @param map La hash map dans laquelle résidera le noeud
 * @return La valeur du hash de la clef
*/
int getHashValue(char *key, struct hashMap *map) {
    int hashValue = 0;
    for (int i = 0; i < (int) strlen(key); i++) {
        hashValue += key[i];
    }
    return hashValue & (map->capacity - 1);
}


/** Insère un noeud dans la structure `map`.
 * @param newNode Le noeud à insérer
 * @param map La hash map dans laquelle insérer le noeud
*/
void insertNode(struct node *newNode, struct hashMap *map) {
    /* Si la hash map commence à être saturée, on la rehash. */
    if ((float) map->size / map->capacity > 0.75) {
        rehashTable(map);
    }

    int hashValue = getHashValue(newNode->key, map);

    /* Si l'emplacement dans la hashtable est libre, on insère le noeud directement. */
    struct node *toExplore = map->hashTable[hashValue];
    if (toExplore == NULL) {
        map->hashTable[hashValue] = newNode;
        goto end_add_node;
    }

    /* Exploration de la hash map pour trouver le premier emplacement libre. */
    struct node *previous;
    do {
        previous = toExplore;
        toExplore = toExplore->next;
    } while (toExplore != NULL);

    if (previous != newNode) previous->next = newNode;

    end_add_node:
    map->size++;
}


/** Insère un noeud dans la structure `map`. Utilisée par rehashTable.
 * @param newNode Le noeud à insérer
 * @param map La hash map dans laquelle insérer le noeud
 * @param hashTable la hashTable correspondante
*/
void insertNodeInternal(struct node *newNode, struct hashMap *map, struct node **hashTable) {
    int hashValue = getHashValue(newNode->key, map);

    /* Si l'emplacement dans la hashtable est libre, on insère le noeud directement. */
    struct node *toExplore = hashTable[hashValue];
    if (toExplore == NULL) {
        hashTable[hashValue] = newNode;
        return;
    }

    /* Exploration de la hash map pour trouver le premier emplacement libre. */
    struct node *previous;
    do {
        if (strcmp(toExplore->key, newNode->key) == 0) {
            toExplore->x = newNode->x;
            toExplore->y = newNode->y;
            return;
        }
        previous = toExplore;
        toExplore = toExplore->next;
    } while (toExplore != NULL);

    if (previous != newNode) previous->next = newNode;
}


/** Rehash la hash map afin d'augmenter la capacité et de ré-agencer
 * les noeuds aux bons endroit.
 * @param map La hash map à rehash
*/
void rehashTable(struct hashMap *map) {
    printf("\nRehashage nécessaire, utilisation actuelle : %d/%d\n", map->size, map->capacity);

    /* Allocation de la nouvelle hashTable (la structure de hash map reste la même). */
    struct node **newHashTable = malloc(map->capacity * 2 * sizeof(struct node *));
    memset(newHashTable, 0, map->capacity * 2 * sizeof(struct node *));

    map->capacity = map->capacity * 2;

    /* Exploration de la hash map puis intégration des anciens noeuds dans la nouvelle table. */
    for (int i = 0; i < map->capacity / 2; i++) {
        struct node *toExplore = map->hashTable[i];

        while (toExplore != NULL) {
            struct node *nextNode = toExplore->next;
            toExplore->next = NULL;
            insertNodeInternal(toExplore, map, newHashTable);
            toExplore = nextNode;
        }
    }
    
    free(map->hashTable);
    map->hashTable = newHashTable;
    printf("Rehashage terminé avec succès, nouvelle utilisation : %d/%d\n", map->size, map->capacity);
}


/** Mets à jour un noeud de clef `key` avec les nouvelles valeurs `x`, `y`, `width`, `height`.
 * Le noeud peut-être dans une structure chainée, il est distingué par sa clef et ses paramètres
 * actuels, `oldX`, `oldY`.
 * @param key La clef du noeud à mettre à jour
 * @param oldX La valeur actuelle `x` du noeud à mettre à jour
 * @param oldY La valeur actuelle `y` du noeud à mettre à jour
 * @param x La nouvelle valeur `x` du noeud à mettre à jour
 * @param y La nouvelle valeur `y` du noeud à mettre à jour
 * @param width La nouvelle valeur `width` du noeud à mettre à jour
 * @param height La nouvelle valeur `height` du noeud à mettre à jour
 * @param map La hash map à laquelle apartient le noeud à mettre à jour
*/
void updateNode(char *key, int oldX, int oldY, int x, int y, int width, int height, struct hashMap *map) {
    int hashValue = getHashValue(key, map);

    /* Si le noeud n'existe pas, on le créé. */
    if (map->hashTable[hashValue] == NULL) {
        createNode(key, x, y, width, height, map);
    } else {
        struct node *toExplore = map->hashTable[hashValue];

        while (strcmp(toExplore->key, key) != 0 || toExplore->x != oldX || toExplore->y != oldY) {
            toExplore = toExplore->next;
            if (toExplore == NULL) return;
        }

        toExplore->x = x;
        toExplore->y = y;
        toExplore->width = width;
        toExplore->height = height;
    }
}


/** Créé un noeud et l'insère dans la hash map `map`.
 * @param key La clef du noeud à mettre à jour
 * @param x La valeur `x` du noeud
 * @param y La valeur `y` du noeud
 * @param width La valeur `width` du noeud
 * @param height La valeur `height` du noeud
 * @param map La hash map dans laquelle insérer le noeud
*/
void createNode(char *key, int x, int y, int width, int height, struct hashMap *map) {
    struct node *newNode = malloc(sizeof(struct node));
    newNode->key = strdup(key);
    newNode->x = x;
    newNode->y = y;
    newNode->next = NULL;
    newNode->isSeen = 0;
    newNode->width = width;
    newNode->height = height;
    insertNode(newNode, map);
}


/** Vérifie l'existence d'un noeud dans une hash map donnée.
 * @param key La clef du noeud à vérifier
 * @param map La hash map dans laquelle résiderait le noeud
 * @return 1 si le noeud est trouvé, 0 sinon
 */
int nodeExists(char *key, struct hashMap *map) {
    int hashValue = getHashValue(key, map);
    struct node *toExplore = map->hashTable[hashValue];

    if (toExplore == NULL) return 0;

    while (strcmp(toExplore->key, key) != 0) {
        toExplore = toExplore->next;
        if (toExplore == NULL) return 0;
    }

    return 1;
}


/** Renvoie la valeur `x` d'un noeud de clef `key` dans une hash map donnée.
 * @param key La clef du noeud
 * @param map La hash map dans laquelle réside le noeud
 * @return `x` si le noeud est trouvé, `ERROR_COORDINATE` sinon
 */
int getX(char *key, struct hashMap *map) {
    int hashValue = getHashValue(key, map);
    struct node *toExplore = map->hashTable[hashValue];

    if (toExplore == NULL) return ERROR_COORDINATE;

    while (strcmp(toExplore->key, key) != 0) {
        toExplore = toExplore->next;
        if (toExplore == NULL) return ERROR_COORDINATE;
    }

    return toExplore->x;
}


/** Renvoie la valeur `y` d'un noeud de clef `key` dans une hash map donnée.
 * @param key La clef du noeud
 * @param map La hash map dans laquelle réside le noeud
 * @return `y` si le noeud est trouvé, `ERROR_COORDINATE` sinon
 */
int getY(char *key, struct hashMap *map) {
    int hashValue = getHashValue(key, map);
    struct node *toExplore = map->hashTable[hashValue];

    if (toExplore == NULL) return ERROR_COORDINATE;

    while (strcmp(toExplore->key, key) != 0) {
        toExplore = toExplore->next;
        if (toExplore == NULL) return ERROR_COORDINATE;
    }

    return toExplore->y;
}


/** Renvoie la valeur `width` d'un noeud de clef `key` dans une hash map donnée.
 * @param key La clef du noeud
 * @param map La hash map dans laquelle réside le noeud
 * @return `width` si le noeud est trouvé, `ERROR_COORDINATE` sinon
 */
int getWidth(char *key, struct hashMap *map) {
    int hashValue = getHashValue(key, map);
    struct node *toExplore = map->hashTable[hashValue];

    if (toExplore == NULL) return ERROR_COORDINATE;

    while (strcmp(toExplore->key, key) != 0) {
        toExplore = toExplore->next;
        if (toExplore == NULL) return ERROR_COORDINATE;
    }

    return toExplore->width;
}


/** Renvoie la valeur `height` d'un noeud de clef `key` dans une hash map donnée.
 * @param key La clef du noeud
 * @param map La hash map dans laquelle réside le noeud
 * @return `height` si le noeud est trouvé, `ERROR_COORDINATE` sinon
 */
int getHeight(char *key, struct hashMap *map) {
    int hashValue = getHashValue(key, map);
    struct node *toExplore = map->hashTable[hashValue];

    if (toExplore == NULL) return ERROR_COORDINATE;

    while (strcmp(toExplore->key, key) != 0) {
        toExplore = toExplore->next;
        if (toExplore == NULL) return ERROR_COORDINATE;
    }

    return toExplore->height;
}


/** Affiche le contenu de la hash map `map` dans le terminal.
 * @param map La hash map à afficher
 */
void printHashMap(struct hashMap *map) {
    printf("\n");
    printf("Utilisation de la hashmap : %d/%d\n", map->size, map->capacity);
    for (int i = 0; i < map->capacity; i++) {
        struct node *toExplore = map->hashTable[i];
        while (toExplore != NULL) {
            printf("{Hash: %d, Key: %s, X: %d, Y: %d}\n", getHashValue(toExplore->key, map), toExplore->key, toExplore->x, toExplore->y);
            toExplore = toExplore->next;
        } 
    }
}


/** Vide le contenu de la hash map `map`.
 * @param map La hash map à purger
 */
void clearHashMap(struct hashMap *map) {
    for (int i = 0; i < map->capacity; i++) {
        struct node *toExplore = map->hashTable[i];
        while (toExplore != NULL) {
            struct node *nextNode = toExplore->next;
            free(toExplore);
            toExplore = nextNode;
        }
        map->hashTable[i] = NULL;
    }
    map->size = 0;

}


/** Compare 2 hash map entre elles.
 * @param map1 La première hash map
 * @param map2 La seconde hash map
 * @return 0 si les hash map sont identiques, -1 sinon
 */
int compareHashMap(struct hashMap *map1, struct hashMap *map2) {
    if (map1->size != map2->size) return -1;
    if (map1->capacity != map2->capacity) return -1;

    for (int i = 0; i < map1->capacity; i++) {
        struct node *toExplore1 = map1->hashTable[i];
        struct node *toExplore2 = map2->hashTable[i];

        while (toExplore1 != NULL && toExplore2 != NULL) {
            /* Si les clefs sont différentes pour un même noeud dans l'arborescence. */
            if (strcmp(toExplore1->key, toExplore2->key) != 0) return -1;
            /* L'égalité des coordonnées n'est pas stricte pour prendre en compte la nature
             * des boîtes de détection de YOLO qui sont bruitées. */
            if (abs(toExplore1->x - toExplore2->x) > PIXEL_THRESHOLD) return -1;
            if (abs(toExplore1->y - toExplore2->y) > PIXEL_THRESHOLD) return -1;
            toExplore1 = toExplore1->next;
            toExplore2 = toExplore2->next;
        }

        if (toExplore1 == NULL && toExplore2 != NULL) return -1;
        if (toExplore1 != NULL && toExplore2 == NULL) return -1;
    }

    return 0;
}


/** Supprime la hash map `map`.
 * @param map La hash map à supprimer
 */
void deleteHashmap(struct hashMap *map) {
    for (int i = 0; i < map->capacity; i++) {
        struct node *toExplore = map->hashTable[i];
        while (toExplore != NULL) {
            struct node *nextNode = toExplore->next;
            free(toExplore);
            toExplore = nextNode;
        } 
    }
    free(map->hashTable);
    printf("\nHashmap libérée avec succès.");
}
