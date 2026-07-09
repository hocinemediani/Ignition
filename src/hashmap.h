#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct node {
    char *key;
    int value;
    struct node *next;
};

struct hashMap {
    int size;
    int capacity;
    struct node **hashTable;
};

struct hashMap *initializeHashMap(int initialCapacity);

int getHashValue(char *key, struct hashMap *map);

void insertNode(struct node *newNode, struct hashMap *map);

struct hashMap *rehashTable(struct hashMap *map);

void updateNode(char *key, int value, struct hashMap *map);

void createNode(char *key, int value, struct hashMap *map);

int getValue(char *key, struct hashMap *map);

void printHashMap(struct hashMap *map);

void clearHashMap(struct hashMap *map);

int compareHashMap(struct hashMap *map1, struct hashMap *map2);

void deleteHashmap(struct hashMap *map);
