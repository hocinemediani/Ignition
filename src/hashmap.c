#include "hashmap.h"

struct hashMap *initializeHashMap(int initialCapacity) {
    struct hashMap *map = malloc(sizeof(struct hashMap));
    map->size = 0;
    map->capacity = initialCapacity;
    map->hashTable = malloc(map->capacity * sizeof(struct node *));
    memset(map->hashTable, 0, map->capacity * sizeof(struct node *));
    return map;
}


int getHashValue(char *key, struct hashMap *map) {
    int hashValue = 0;
    for (int i = 0; i < (int) strlen(key); i++) {
        hashValue += key[i];
    }
    return hashValue & (map->capacity - 1);
}


void insertNode(struct node *newNode, struct hashMap *map) {
    struct hashMap *correctHashMap = map;
    if ((float) map->size / map->capacity > 0.75) {
        correctHashMap = rehashTable(map);
    }

    int hashValue = getHashValue(newNode->key, map);

    struct node *toExplore = correctHashMap->hashTable[hashValue];
    if (toExplore == NULL) {
        correctHashMap->hashTable[hashValue] = newNode;
        goto end_add_node;
    }

    struct node *previous;
    do {
        if (strcmp(toExplore->key, newNode->key) == 0) {
            toExplore->value = newNode->value;
            return;
        }
        previous = toExplore;
        toExplore = toExplore->next;
    } while (toExplore != NULL);

    if (previous != newNode) previous->next = newNode;

    end_add_node:
    map->size++;
}


struct hashMap *rehashTable(struct hashMap *map) {
    printf("\nRehashage nécessaire, utilisation actuelle : %d/%d\n", map->size, map->capacity);

    struct hashMap *newHashMap = initializeHashMap(map->capacity * 2);

    for (int i = 0; i < map->capacity / 2; i++) {
        struct node *toExplore = map->hashTable[i];

        while (toExplore != NULL) {
            struct node *nextNode = toExplore->next;
            toExplore->next = NULL;
            insertNode(toExplore, newHashMap);
            toExplore = nextNode;
        }
    }
    
    free(map->hashTable);
    printf("Rehashage terminé avec succès, nouvelle utilisation : %d/%d\n", newHashMap->size, newHashMap->capacity);
    return newHashMap;
}


void updateNode(char *key, int value, struct hashMap *map) {
    int hashValue = getHashValue(key, map);
    if (map->hashTable[hashValue] == NULL) {
        createNode(key, value, map);
    } else {
        struct node *toExplore = map->hashTable[hashValue];

        while (strcmp(toExplore->key, key) != 0) {
            toExplore = toExplore->next;
            if (toExplore == NULL) return;
        }

        toExplore->value += value;
    }
}


void createNode(char *key, int value, struct hashMap *map) {
    struct node *newNode = malloc(sizeof(struct node));
    newNode->key = strdup(key);
    newNode->value = value;
    newNode->next = NULL;
    insertNode(newNode, map);
}


int getValue(char *key, struct hashMap *map) {
    int hashValue = getHashValue(key, map);
    struct node *toExplore = map->hashTable[hashValue];

    if (toExplore == NULL) return -1;

    while (strcmp(toExplore->key, key) != 0) {
        toExplore = toExplore->next;
        if (toExplore == NULL) return -1;
    }

    return toExplore->value;
}


void printHashMap(struct hashMap *map) {
    printf("\n");
    printf("Utilisation de la hashmap : %d/%d\n", map->size, map->capacity);
    for (int i = 0; i < map->capacity; i++) {
        struct node *toExplore = map->hashTable[i];
        while (toExplore != NULL) {
            printf("{Hash: %d, Key: %s, Value: %d}\n", getHashValue(toExplore->key, map), toExplore->key, toExplore->value);
            toExplore = toExplore->next;
        } 
    }
}


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


int compareHashMap(struct hashMap *map1, struct hashMap *map2) {
    if (map1->size != map2->size) return -1;
    if (map1->capacity != map2->capacity) return -1;

    for (int i = 0; i < map1->capacity; i++) {
        struct node *toExplore1 = map1->hashTable[i];
        struct node *toExplore2 = map2->hashTable[i];

        while (toExplore1 != NULL && toExplore2 != NULL) {
            if (strcmp(toExplore1->key, toExplore2->key) != 0) return -1;
            if (toExplore1->value != toExplore2->value) return -1;
            toExplore1 = toExplore1->next;
            toExplore2 = toExplore2->next;
        }

        if (toExplore1 == NULL && toExplore2 != NULL) return -1;
        if (toExplore1 != NULL && toExplore2 == NULL) return -1;
    }

    return 0;
}


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
