#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ERROR_COORDINATE 800
#define PIXEL_THRESHOLD 30

struct node {
    char *key;
    int x;
    int y;
    int width;
    int height;
    int isSeen;
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

void insertNodeInternal(struct node *newNode, struct hashMap *map, struct node **hashTable);

void rehashTable(struct hashMap *map);

void updateNode(char *key, int oldX, int oldY, int x, int y, int width, int height, struct hashMap *map);

void createNode(char *key, int x, int y, int width, int height, struct hashMap *map);

int nodeExists(char *key, struct hashMap *map);

int getX(char *key, struct hashMap *map);

int getY(char *key, struct hashMap *map);

int getWidth(char *key, struct hashMap *map);

int getHeight(char *key, struct hashMap *map);

void printHashMap(struct hashMap *map);

void clearHashMap(struct hashMap *map);

int compareHashMap(struct hashMap *map1, struct hashMap *map2);

void deleteHashmap(struct hashMap *map);
